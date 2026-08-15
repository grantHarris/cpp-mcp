/**
 * @file mcp_test.cpp
 * @brief Tests for MCP 2025-03-26 spec compliance
 *
 * Tests cover: JSON-RPC message format, server lifecycle, Streamable HTTP
 * transport, legacy SSE transport, tools, resources, resource templates,
 * session management, and CORS headers.
 */

#include <gtest/gtest.h>
#include <future>
#include "mcp_message.h"
#include "mcp_server.h"
#include "mcp_tool.h"
#include "mcp_sse_client.h"
#include "httplib.h"

using namespace mcp;
using json = nlohmann::ordered_json;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static int next_port() {
    static int port = 9100;
    return port++;
}

// POST a JSON-RPC request to the Streamable HTTP endpoint and return the
// parsed JSON response body.
static json mcp_post(httplib::Client& cli, const std::string& path,
                     const json& body, const std::string& session_id = "",
                     const httplib::Headers& extra_headers = {}) {
    httplib::Headers headers;
    headers.emplace("Content-Type", "application/json");
    headers.emplace("Accept", "application/json, text/event-stream");
    if (!session_id.empty()) {
        headers.emplace("Mcp-Session-Id", session_id);
    }
    for (const auto& header : extra_headers) {
        headers.emplace(header.first, header.second);
    }
    auto res = cli.Post(path, headers, body.dump(), "application/json");
    if (!res) return json{{"_http_error", true}};
    json out;
    out["_status"] = res->status;
    if (!res->body.empty()) {
        try {
            out["_body"] = json::parse(res->body);
        } catch (...) {
            out["_body_raw"] = res->body;
        }
    }
    // Capture Mcp-Session-Id header if present
    if (res->has_header("Mcp-Session-Id")) {
        out["_session_id"] = res->get_header_value("Mcp-Session-Id");
    }
    return out;
}

// Initialize via Streamable HTTP and return {session_id, response_json}
static std::pair<std::string, json> mcp_initialize(httplib::Client& cli,
                                                    const std::string& path = "/mcp",
                                                    const httplib::Headers& extra_headers = {}) {
    json init_req = {
        {"jsonrpc", "2.0"},
        {"id", 1},
        {"method", "initialize"},
        {"params", {
            {"protocolVersion", MCP_VERSION},
            {"clientInfo", {{"name", "TestClient"}, {"version", "1.0.0"}}},
            {"capabilities", json::object()}
        }}
    };
    auto res = mcp_post(cli, path, init_req, "", extra_headers);
    std::string sid = res.value("_session_id", "");
    json body = res.value("_body", json::object());

    // Send initialized notification
    if (!sid.empty()) {
        json notif = {
            {"jsonrpc", "2.0"},
            {"method", "notifications/initialized"}
        };
        mcp_post(cli, path, notif, sid, extra_headers);
    }
    return {sid, body};
}

// ===========================================================================
// Message Format Tests (pure unit tests, no server needed)
// ===========================================================================

TEST(MessageFormat, RequestRoundTrip) {
    auto req = request::create("test/method", {{"key", "value"}});
    json j = req.to_json();

    EXPECT_EQ(j["jsonrpc"], "2.0");
    EXPECT_TRUE(j.contains("id"));
    EXPECT_EQ(j["method"], "test/method");
    EXPECT_EQ(j["params"]["key"], "value");
}

TEST(MessageFormat, NotificationOmitsId) {
    auto notif = request::create_notification("initialized");
    json j = notif.to_json();

    EXPECT_FALSE(j.contains("id"));
    EXPECT_TRUE(notif.is_notification());
    EXPECT_EQ(j["method"], "notifications/initialized");
}

TEST(MessageFormat, SuccessResponse) {
    auto res = response::create_success(42, {{"ok", true}});
    json j = res.to_json();

    EXPECT_EQ(j["id"], 42);
    EXPECT_TRUE(j.contains("result"));
    EXPECT_FALSE(j.contains("error"));
}

TEST(MessageFormat, ErrorResponse) {
    auto res = response::create_error(1, error_code::invalid_params,
                                       "bad params", {{"field", "x"}});
    json j = res.to_json();

    EXPECT_EQ(j["error"]["code"], static_cast<int>(error_code::invalid_params));
    EXPECT_EQ(j["error"]["message"], "bad params");
    EXPECT_EQ(j["error"]["data"]["field"], "x");
    EXPECT_FALSE(j.contains("result"));
}

// Spec: notifications MUST NOT include id — from_json must handle absent id
TEST(MessageFormat, FromJsonNotificationWithoutId) {
    json j = {{"jsonrpc", "2.0"}, {"method", "notifications/progress"},
              {"params", {{"token", "abc"}}}};
    auto req = request::from_json(j);
    EXPECT_TRUE(req.is_notification());
    EXPECT_EQ(req.method, "notifications/progress");
}

// from_json with null id should also produce a notification
TEST(MessageFormat, FromJsonNotificationWithNullId) {
    json j = {{"jsonrpc", "2.0"}, {"id", nullptr}, {"method", "notifications/test"}};
    auto req = request::from_json(j);
    EXPECT_TRUE(req.is_notification());
}

// from_json with minimal JSON (missing optional fields)
TEST(MessageFormat, FromJsonMinimal) {
    json j = {{"method", "ping"}};
    auto req = request::from_json(j);
    EXPECT_EQ(req.method, "ping");
    EXPECT_EQ(req.jsonrpc, "2.0");
    EXPECT_TRUE(req.params.empty());
}

// response::from_json with only result (no error key)
TEST(MessageFormat, ResponseFromJsonNoError) {
    json j = {{"jsonrpc", "2.0"}, {"id", 1}, {"result", {{"ok", true}}}};
    auto res = response::from_json(j);
    EXPECT_FALSE(res.is_error());
    EXPECT_EQ(res.result["ok"], true);
}

// response::from_json with only error (no result key)
TEST(MessageFormat, ResponseFromJsonNoResult) {
    json j = {{"jsonrpc", "2.0"}, {"id", 1},
              {"error", {{"code", -32600}, {"message", "Invalid Request"}}}};
    auto res = response::from_json(j);
    EXPECT_TRUE(res.is_error());
    EXPECT_TRUE(res.result.empty());
}

// ===========================================================================
// Tool Builder Tests
// ===========================================================================

TEST(ProtocolVersion, ConstantsExposed) {
    EXPECT_STREQ(mcp::LATEST_MCP_VERSION, "2025-11-25");
    EXPECT_TRUE(mcp::is_supported_version("2025-11-25"));
    EXPECT_TRUE(mcp::is_supported_version("2025-06-18"));
    EXPECT_TRUE(mcp::is_supported_version("2025-03-26"));
    EXPECT_FALSE(mcp::is_supported_version("2024-11-05"));
    EXPECT_FALSE(mcp::is_supported_version(""));
}

TEST(ToolBuilder, BasicTool) {
    auto t = tool_builder("echo")
        .with_description("Echoes input back")
        .with_string_param("text", "Text to echo")
        .build();

    EXPECT_EQ(t.name, "echo");
    EXPECT_EQ(t.description, "Echoes input back");
    json schema = t.parameters_schema;
    EXPECT_EQ(schema["type"], "object");
    EXPECT_TRUE(schema["properties"].contains("text"));
    EXPECT_EQ(schema["required"][0], "text");
}

TEST(ToolBuilder, OptionalParam) {
    auto t = tool_builder("search")
        .with_description("Search")
        .with_string_param("query", "Search query", true)
        .with_number_param("limit", "Max results", false)
        .build();

    json required = t.parameters_schema["required"];
    EXPECT_EQ(required.size(), 1);
    EXPECT_EQ(required[0], "query");
}

TEST(ThreadPool, ZeroThreadsFallsBackToOneWorker) {
    thread_pool pool(0);
    auto result = pool.enqueue([] { return 42; });
    ASSERT_EQ(result.wait_for(std::chrono::seconds(1)), std::future_status::ready);
    EXPECT_EQ(result.get(), 42);
}

// ===========================================================================
// Server fixture — starts a server with Streamable HTTP on a unique port
// ===========================================================================

class ServerTest : public ::testing::Test {
protected:
    void SetUp() override {
        port_ = next_port();
        server::configuration conf;
        conf.host = "127.0.0.1";
        conf.port = port_;
        conf.name = "TestServer";
        conf.version = "1.0.0";

        srv_ = std::make_unique<server>(conf);

        json caps = {
            {"tools", {{"listChanged", true}}},
            {"resources", {{"subscribe", true}}}
        };
        srv_->set_capabilities(caps);

        // Register a simple echo tool
        auto echo = tool_builder("echo")
            .with_description("Echo")
            .with_string_param("text", "text")
            .build();
        srv_->register_tool(echo, [](const json& args, const std::string&) -> json {
            return json::array({{{"type", "text"}, {"text", args.value("text", "")}}});
        });

        // Register a resource
        auto res = std::make_shared<text_resource>("test://hello", "hello",
                                                    "text/plain", "A test resource");
        res->set_text("Hello, world!");
        srv_->register_resource("test://hello", res);

        // Register a resource template
        srv_->register_resource_template(
            "test://items/{id}", "item", "application/json", "Item by ID",
            [](const std::string& uri, const std::map<std::string, std::string>& params,
               const std::string&) -> json {
                return {{"uri", uri}, {"mimeType", "application/json"},
                        {"text", "{\"id\":\"" + params.at("id") + "\"}"}};
            });

        // Register a prompt
        prompt greet_prompt;
        greet_prompt.name = "greet";
        greet_prompt.description = "Generate a greeting";
        greet_prompt.arguments = {{"name", "Name to greet", true}};
        srv_->register_prompt(greet_prompt, [](const json& args, const std::string&) -> json {
            std::string name = args.value("name", "world");
            return {
                {"description", "A greeting"},
                {"messages", json::array({
                    {{"role", "user"}, {"content", {{"type", "text"}, {"text", "Say hello to " + name}}}}
                })}
            };
        });

        srv_->start(false);
        // Give the server a moment to bind
        std::this_thread::sleep_for(std::chrono::milliseconds(50));

        cli_ = std::make_unique<httplib::Client>("127.0.0.1", port_);
        cli_->set_connection_timeout(2);
        cli_->set_read_timeout(5);
    }

    void TearDown() override {
        cli_.reset();
        if (srv_) srv_->stop();
        srv_.reset();
    }

    int port_;
    std::unique_ptr<server> srv_;
    std::unique_ptr<httplib::Client> cli_;
};

// ===========================================================================
// Streamable HTTP Transport Tests
// ===========================================================================

TEST_F(ServerTest, InitializeReturnsSessionId) {
    auto [sid, body] = mcp_initialize(*cli_);
    EXPECT_FALSE(sid.empty());
    EXPECT_EQ(body["result"]["protocolVersion"], MCP_VERSION);
    EXPECT_EQ(body["result"]["serverInfo"]["name"], "TestServer");
}

TEST_F(ServerTest, InitializeUnsupportedVersionFallsBackToLatest) {
    json req = {
        {"jsonrpc", "2.0"}, {"id", 1}, {"method", "initialize"},
        {"params", {
            {"protocolVersion", "1999-01-01"},
            {"clientInfo", {{"name", "OldClient"}, {"version", "0.1"}}},
            {"capabilities", json::object()}
        }}
    };
    auto res = mcp_post(*cli_, "/mcp", req);
    // Unknown version → server falls back to its latest supported.
    EXPECT_EQ(res["_body"]["result"]["protocolVersion"], LATEST_MCP_VERSION);
}

TEST_F(ServerTest, InitializeNegotiatesClientRequestedVersion) {
    // 2025-06-18 is in our supported set; server should echo it back.
    json req = {
        {"jsonrpc", "2.0"}, {"id", 1}, {"method", "initialize"},
        {"params", {
            {"protocolVersion", "2025-06-18"},
            {"clientInfo", {{"name", "Mid"}, {"version", "1.0"}}},
            {"capabilities", json::object()}
        }}
    };
    auto res = mcp_post(*cli_, "/mcp", req);
    EXPECT_EQ(res["_body"]["result"]["protocolVersion"], "2025-06-18");
}

TEST_F(ServerTest, InitializeStoresNegotiatedVersionPerSession) {
    json req = {
        {"jsonrpc", "2.0"}, {"id", 1}, {"method", "initialize"},
        {"params", {
            {"protocolVersion", "2025-03-26"},
            {"clientInfo", {{"name", "Old"}, {"version", "1.0"}}},
            {"capabilities", json::object()}
        }}
    };
    auto res = mcp_post(*cli_, "/mcp", req);
    std::string sid = res.value("_session_id", "");
    ASSERT_FALSE(sid.empty());
    EXPECT_EQ(srv_->session_protocol_version(sid), "2025-03-26");
}

TEST_F(ServerTest, PingAfterInitialize) {
    auto [sid, _] = mcp_initialize(*cli_);
    json ping = {{"jsonrpc", "2.0"}, {"id", 2}, {"method", "ping"}};
    auto res = mcp_post(*cli_, "/mcp", ping, sid);
    EXPECT_EQ(res["_status"], 200);
    EXPECT_EQ(res["_body"]["result"], json::object());
}

TEST_F(ServerTest, RejectMissingSessionId) {
    // Non-initialize request without Mcp-Session-Id should get 400
    json req = {{"jsonrpc", "2.0"}, {"id", 1}, {"method", "ping"}};
    auto res = mcp_post(*cli_, "/mcp", req);
    EXPECT_EQ(res["_status"], 400);
}

TEST_F(ServerTest, RejectInvalidSessionId) {
    json req = {{"jsonrpc", "2.0"}, {"id", 1}, {"method", "ping"}};
    auto res = mcp_post(*cli_, "/mcp", req, "nonexistent-session");
    EXPECT_EQ(res["_status"], 404);
}

TEST_F(ServerTest, RejectReInitialize) {
    auto [sid, _] = mcp_initialize(*cli_);
    // Try to initialize again on the same session
    json init_req = {
        {"jsonrpc", "2.0"}, {"id", 99}, {"method", "initialize"},
        {"params", {
            {"protocolVersion", MCP_VERSION},
            {"clientInfo", {{"name", "Dup"}, {"version", "1.0"}}},
            {"capabilities", json::object()}
        }}
    };
    auto res = mcp_post(*cli_, "/mcp", init_req, sid);
    EXPECT_EQ(res["_status"], 400);
}

TEST_F(ServerTest, DeleteSession) {
    auto [sid, _] = mcp_initialize(*cli_);
    ASSERT_FALSE(sid.empty());

    httplib::Headers headers;
    headers.emplace("Mcp-Session-Id", sid);
    auto res = cli_->Delete("/mcp", headers);
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 200);

    // Subsequent request should fail with 404
    json req = {{"jsonrpc", "2.0"}, {"id", 1}, {"method", "ping"}};
    auto res2 = mcp_post(*cli_, "/mcp", req, sid);
    EXPECT_EQ(res2["_status"], 404);
}

TEST_F(ServerTest, NotificationReturns202) {
    auto [sid, _] = mcp_initialize(*cli_);
    json notif = {{"jsonrpc", "2.0"}, {"method", "notifications/test"}};
    auto res = mcp_post(*cli_, "/mcp", notif, sid);
    EXPECT_EQ(res["_status"], 202);
}

// MCP-Protocol-Version header (spec 2025-06-18). Server must emit it on
// every response and validate it on post-init Streamable HTTP requests.
TEST_F(ServerTest, InitializeResponseSendsProtocolVersionHeader) {
    json init_req = {
        {"jsonrpc", "2.0"}, {"id", 1}, {"method", "initialize"},
        {"params", {
            {"protocolVersion", "2025-11-25"},
            {"clientInfo", {{"name", "T"}, {"version", "1"}}},
            {"capabilities", json::object()}
        }}
    };
    httplib::Headers h = {
        {"Content-Type", "application/json"},
        {"Accept", "application/json, text/event-stream"}
    };
    auto res = cli_->Post("/mcp", h, init_req.dump(), "application/json");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 200);
    EXPECT_EQ(res->get_header_value("MCP-Protocol-Version"), "2025-11-25");
}

TEST_F(ServerTest, RejectsMismatchedProtocolVersionHeader) {
    auto [sid, _] = mcp_initialize(*cli_);  // negotiates LATEST_MCP_VERSION
    httplib::Headers h = {
        {"Content-Type", "application/json"},
        {"Accept", "application/json, text/event-stream"},
        {"Mcp-Session-Id", sid},
        {"MCP-Protocol-Version", "2025-03-26"}  // mismatch
    };
    json ping = {{"jsonrpc", "2.0"}, {"id", 99}, {"method", "ping"}};
    auto res = cli_->Post("/mcp", h, ping.dump(), "application/json");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 400);
}

TEST_F(ServerTest, RejectsUnknownProtocolVersionHeader) {
    auto [sid, _] = mcp_initialize(*cli_);
    httplib::Headers h = {
        {"Content-Type", "application/json"},
        {"Accept", "application/json, text/event-stream"},
        {"Mcp-Session-Id", sid},
        {"MCP-Protocol-Version", "1999-01-01"}
    };
    json ping = {{"jsonrpc", "2.0"}, {"id", 99}, {"method", "ping"}};
    auto res = cli_->Post("/mcp", h, ping.dump(), "application/json");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 400);
}

TEST_F(ServerTest, AcceptsMissingProtocolVersionHeader) {
    // Spec compat: missing header implies 2025-03-26. We allow it through.
    auto [sid, _] = mcp_initialize(*cli_);
    json ping = {{"jsonrpc", "2.0"}, {"id", 99}, {"method", "ping"}};
    auto res = mcp_post(*cli_, "/mcp", ping, sid);  // helper omits the header
    EXPECT_EQ(res["_status"], 200);
}

// Origin allowlist with HTTP 403 (spec 2025-11-25).
namespace {
struct OriginServer {
    std::unique_ptr<server> srv;
    std::unique_ptr<httplib::Client> cli;
    int port;

    explicit OriginServer(std::vector<std::string> allowlist) {
        port = next_port();
        server::configuration conf;
        conf.host = "127.0.0.1";
        conf.port = port;
        conf.allowed_origins = std::move(allowlist);
        srv = std::make_unique<server>(conf);
        srv->start(false);
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        cli = std::make_unique<httplib::Client>("127.0.0.1", port);
        cli->set_connection_timeout(2);
        cli->set_read_timeout(5);
    }
    ~OriginServer() {
        cli.reset();
        if (srv) srv->stop();
    }
};

httplib::Result post_init_with_origin(httplib::Client& c, const std::string& origin) {
    json init = {{"jsonrpc", "2.0"}, {"id", 1}, {"method", "initialize"},
                 {"params", {{"protocolVersion", LATEST_MCP_VERSION},
                             {"clientInfo", {{"name", "t"}, {"version", "0"}}},
                             {"capabilities", json::object()}}}};
    httplib::Headers h = {
        {"Content-Type", "application/json"},
        {"Accept", "application/json, text/event-stream"},
        {"Origin", origin}
    };
    return c.Post("/mcp", h, init.dump(), "application/json");
}
}  // namespace

TEST(OriginAllowlist, RejectsDisallowedOrigin) {
    OriginServer s({"http://localhost:3000"});
    auto resp = post_init_with_origin(*s.cli, "http://evil.example.com");
    ASSERT_TRUE(resp);
    EXPECT_EQ(resp->status, 403);
}

TEST(OriginAllowlist, AllowsListedOrigin) {
    OriginServer s({"http://localhost:3000"});
    auto resp = post_init_with_origin(*s.cli, "http://localhost:3000");
    ASSERT_TRUE(resp);
    EXPECT_EQ(resp->status, 200);
    EXPECT_EQ(resp->get_header_value("Access-Control-Allow-Origin"),
              "http://localhost:3000");
    EXPECT_EQ(resp->get_header_value("Vary"), "Origin");
}

TEST(OriginAllowlist, EmptyAllowlistAllowsAllOrigins) {
    OriginServer s({});
    auto resp = post_init_with_origin(*s.cli, "http://anywhere.example.com");
    ASSERT_TRUE(resp);
    EXPECT_EQ(resp->status, 200);
    EXPECT_EQ(resp->get_header_value("Access-Control-Allow-Origin"), "*");
}

TEST(OriginAllowlist, MissingOriginIsAllowed) {
    OriginServer s({"http://localhost:3000"});
    json init = {{"jsonrpc", "2.0"}, {"id", 1}, {"method", "initialize"},
                 {"params", {{"protocolVersion", LATEST_MCP_VERSION},
                             {"clientInfo", {{"name", "t"}, {"version", "0"}}},
                             {"capabilities", json::object()}}}};
    httplib::Headers h = {
        {"Content-Type", "application/json"},
        {"Accept", "application/json, text/event-stream"}
    };
    auto resp = s.cli->Post("/mcp", h, init.dump(), "application/json");
    ASSERT_TRUE(resp);
    EXPECT_EQ(resp->status, 200);
}

TEST(OriginAllowlist, RejectsDisallowedPreflightOrigin) {
    OriginServer s({"http://localhost:3000"});
    httplib::Headers h = {
        {"Origin", "http://evil.example.com"},
        {"Access-Control-Request-Method", "POST"},
        {"Access-Control-Request-Headers", "Authorization, Content-Type"}
    };
    auto resp = s.cli->Options("/mcp", h);
    ASSERT_TRUE(resp);
    EXPECT_EQ(resp->status, 403);
    EXPECT_FALSE(resp->has_header("Access-Control-Allow-Origin"));
}

TEST(OriginAllowlist, AllowsListedPreflightOrigin) {
    OriginServer s({"http://localhost:3000"});
    httplib::Headers h = {
        {"Origin", "http://localhost:3000"},
        {"Access-Control-Request-Method", "POST"},
        {"Access-Control-Request-Headers", "Authorization, Content-Type"}
    };
    auto resp = s.cli->Options("/mcp", h);
    ASSERT_TRUE(resp);
    EXPECT_EQ(resp->status, 204);
    EXPECT_EQ(resp->get_header_value("Access-Control-Allow-Origin"),
              "http://localhost:3000");
    EXPECT_NE(resp->get_header_value("Access-Control-Allow-Headers").find("Authorization"),
              std::string::npos);
}

namespace {
struct AuthServer {
    std::unique_ptr<server> srv;
    std::unique_ptr<httplib::Client> cli;
    int port;

    explicit AuthServer(auth_handler handler,
                        std::vector<std::string> allowed_origins = {},
                        std::string resource_metadata_url = {}) {
        port = next_port();
        server::configuration conf;
        conf.host = "127.0.0.1";
        conf.port = port;
        conf.name = "AuthServer";
        conf.allowed_origins = std::move(allowed_origins);
        conf.auth_resource_metadata_url = std::move(resource_metadata_url);
        srv = std::make_unique<server>(conf);
        srv->set_auth_handler(std::move(handler));
        srv->start(false);
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        cli = std::make_unique<httplib::Client>("127.0.0.1", port);
        cli->set_connection_timeout(2);
        cli->set_read_timeout(5);
    }

    ~AuthServer() {
        cli.reset();
        if (srv) srv->stop();
    }
};
}  // namespace

TEST(Authentication, InitializeRequiresBearerToken) {
    AuthServer s([](const std::string& token, const std::string&) {
        return token == "good";
    });
    json init = {{"jsonrpc", "2.0"}, {"id", 1}, {"method", "initialize"},
                 {"params", {{"protocolVersion", LATEST_MCP_VERSION},
                             {"clientInfo", {{"name", "t"}, {"version", "0"}}},
                             {"capabilities", json::object()}}}};
    httplib::Headers h = {
        {"Content-Type", "application/json"},
        {"Accept", "application/json, text/event-stream"}
    };
    auto resp = s.cli->Post("/mcp", h, init.dump(), "application/json");
    ASSERT_TRUE(resp);
    EXPECT_EQ(resp->status, 401);
    EXPECT_EQ(resp->get_header_value("WWW-Authenticate"), "Bearer");
}

TEST(Authentication, ValidBearerTokenAuthorizesSessionRequests) {
    AuthServer s([](const std::string& token, const std::string&) {
        return token == "good";
    });
    httplib::Headers auth = {{"Authorization", "Bearer good"}};
    auto [sid, body] = mcp_initialize(*s.cli, "/mcp", auth);
    ASSERT_FALSE(sid.empty());
    EXPECT_EQ(body["result"]["serverInfo"]["name"], "AuthServer");

    json ping = {{"jsonrpc", "2.0"}, {"id", 2}, {"method", "ping"}};
    auto no_auth = mcp_post(*s.cli, "/mcp", ping, sid);
    EXPECT_EQ(no_auth["_status"], 401);

    auto bad_auth = mcp_post(*s.cli, "/mcp", ping, sid,
                             {{"Authorization", "Bearer bad"}});
    EXPECT_EQ(bad_auth["_status"], 401);

    auto ok = mcp_post(*s.cli, "/mcp", ping, sid, auth);
    EXPECT_EQ(ok["_status"], 200);
    EXPECT_EQ(ok["_body"]["result"], json::object());
}

TEST(Authentication, RefreshedBearerTokenAuthorizesExistingSession) {
    AuthServer s([](const std::string& token, const std::string&) {
        return token == "good" || token == "other";
    });
    auto [sid, _] = mcp_initialize(*s.cli, "/mcp",
                                   {{"Authorization", "Bearer good"}});
    ASSERT_FALSE(sid.empty());

    json ping = {{"jsonrpc", "2.0"}, {"id", 2}, {"method", "ping"}};
    auto switched = mcp_post(*s.cli, "/mcp", ping, sid,
                             {{"Authorization", "Bearer other"}});
    EXPECT_EQ(switched["_status"], 200);

    auto original = mcp_post(*s.cli, "/mcp", ping, sid,
                             {{"Authorization", "Bearer good"}});
    EXPECT_EQ(original["_status"], 200);
}

TEST(Authentication, LegacyFailureIncludesCorsHeaders) {
    AuthServer s([](const std::string& token, const std::string&) {
        return token == "good";
    }, {"https://app.example.com"});

    httplib::Headers headers = {
        {"Origin", "https://app.example.com"},
        {"Content-Type", "application/json"}
    };
    auto response = s.cli->Post("/message?session_id=missing", headers, "{}",
                                "application/json");
    ASSERT_TRUE(response);
    EXPECT_EQ(response->status, 401);
    EXPECT_EQ(response->get_header_value("Access-Control-Allow-Origin"),
              "https://app.example.com");
    EXPECT_EQ(response->get_header_value("WWW-Authenticate"), "Bearer");
}

TEST(Authentication, DetailedHandlerReturnsScopeChallenge) {
    int port = next_port();
    server::configuration conf;
    conf.host = "127.0.0.1";
    conf.port = port;
    conf.auth_resource_metadata_url =
        "https://mcp.example.com/.well-known/oauth-protected-resource";
    server srv(conf);
    srv.set_detailed_auth_handler([](const std::string&, const std::string&) {
        return auth_result::insufficient_scope("files:read");
    });
    srv.start(false);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    httplib::Client cli("127.0.0.1", port);
    json init = {{"jsonrpc", "2.0"}, {"id", 1}, {"method", "initialize"},
                 {"params", {{"protocolVersion", LATEST_MCP_VERSION},
                             {"clientInfo", {{"name", "t"}, {"version", "0"}}},
                             {"capabilities", json::object()}}}};
    httplib::Headers headers = {{"Authorization", "Bearer narrow"}};
    auto response = cli.Post("/mcp", headers, init.dump(), "application/json");
    ASSERT_TRUE(response);
    EXPECT_EQ(response->status, 403);
    const auto challenge = response->get_header_value("WWW-Authenticate");
    EXPECT_NE(challenge.find("error=\"insufficient_scope\""), std::string::npos);
    EXPECT_NE(challenge.find("scope=\"files:read\""), std::string::npos);
    EXPECT_NE(challenge.find("resource_metadata=\"https://mcp.example.com/"),
              std::string::npos);

    srv.stop();
}

TEST(RequestLimits, RejectsOversizedRequestBody) {
    int port = next_port();
    server::configuration conf;
    conf.host = "127.0.0.1";
    conf.port = port;
    conf.max_request_body_size = 64;
    server srv(conf);
    srv.start(false);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    httplib::Client cli("127.0.0.1", port);
    cli.set_connection_timeout(2);
    cli.set_read_timeout(2);

    std::string body(256, 'x');
    auto resp = cli.Post("/mcp", body, "application/json");
    ASSERT_TRUE(resp);
    EXPECT_EQ(resp->status, 413);

    srv.stop();
}

TEST(SessionTimeout, ActivePostOnlySessionStaysAliveInBlockingMode) {
    int port = next_port();
    server::configuration conf;
    conf.host = "127.0.0.1";
    conf.port = port;
    conf.session_timeout = 1;
    server srv(conf);
    std::thread server_thread([&] { srv.start(true); });
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    httplib::Client cli("127.0.0.1", port);
    cli.set_connection_timeout(2);
    cli.set_read_timeout(2);
    auto [sid, _] = mcp_initialize(cli);

    int last_status = 0;
    if (sid.empty()) {
        ADD_FAILURE() << "Failed to initialize blocking-mode server";
    } else {
        for (int i = 0; i < 8; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(300));
            json ping = {{"jsonrpc", "2.0"}, {"id", i + 2}, {"method", "ping"}};
            auto response = mcp_post(cli, "/mcp", ping, sid);
            last_status = response.value("_status", 0);
            EXPECT_EQ(last_status, 200);
        }
    }

    srv.stop();
    server_thread.join();
    EXPECT_EQ(last_status, 200);
}

// Spec 2025-06-18 removed JSON-RPC batching. Servers MUST reject array bodies.
TEST_F(ServerTest, BatchRejected) {
    auto [sid, _] = mcp_initialize(*cli_);
    json batch = json::array({
        {{"jsonrpc", "2.0"}, {"id", 10}, {"method", "ping"}},
        {{"jsonrpc", "2.0"}, {"id", 11}, {"method", "ping"}}
    });
    auto res = mcp_post(*cli_, "/mcp", batch, sid);
    EXPECT_EQ(res["_status"], 400);
    EXPECT_EQ(res["_body"]["error"]["code"].get<int>(),
              static_cast<int>(error_code::invalid_request));
}

TEST_F(ServerTest, BatchedInitializeRejected) {
    json batch = json::array({
        {{"jsonrpc", "2.0"}, {"id", 1}, {"method", "initialize"},
         {"params", {{"protocolVersion", MCP_VERSION},
                     {"clientInfo", {{"name", "Bad"}, {"version", "1.0"}}},
                     {"capabilities", json::object()}}}},
        {{"jsonrpc", "2.0"}, {"id", 2}, {"method", "ping"}}
    });
    auto res = mcp_post(*cli_, "/mcp", batch);
    EXPECT_EQ(res["_status"], 400);
}

TEST_F(ServerTest, MalformedJsonRpcReturnsProtocolError) {
    auto [sid, _] = mcp_initialize(*cli_);
    json bad = {{"jsonrpc", "2.0"}, {"id", 77}, {"method", 42},
                {"params", json::object()}};
    auto res = mcp_post(*cli_, "/mcp", bad, sid);
    EXPECT_EQ(res["_status"], 400);
    EXPECT_EQ(res["_body"]["id"], 77);
    EXPECT_EQ(res["_body"]["error"]["code"].get<int>(),
              static_cast<int>(error_code::invalid_request));
}

// ===========================================================================
// Tools via Streamable HTTP
// ===========================================================================

TEST_F(ServerTest, ToolsList) {
    auto [sid, _] = mcp_initialize(*cli_);
    json req = {{"jsonrpc", "2.0"}, {"id", 3}, {"method", "tools/list"},
                {"params", json::object()}};
    auto res = mcp_post(*cli_, "/mcp", req, sid);
    auto tools = res["_body"]["result"]["tools"];
    ASSERT_EQ(tools.size(), 1);
    EXPECT_EQ(tools[0]["name"], "echo");
}

TEST_F(ServerTest, ToolsListNoCursorWhenAllFit) {
    auto [sid, _] = mcp_initialize(*cli_);
    json req = {{"jsonrpc", "2.0"}, {"id", 50}, {"method", "tools/list"},
                {"params", json::object()}};
    auto res = mcp_post(*cli_, "/mcp", req, sid);
    auto result = res["_body"]["result"];
    EXPECT_TRUE(result.contains("tools"));
    // With only 1 tool (< page size 100), no nextCursor should be present
    EXPECT_FALSE(result.contains("nextCursor"));
}

TEST_F(ServerTest, ToolsListWithCursor) {
    auto [sid, _] = mcp_initialize(*cli_);
    // Request with cursor "0" — should return same as no cursor
    json req = {{"jsonrpc", "2.0"}, {"id", 51}, {"method", "tools/list"},
                {"params", {{"cursor", "0"}}}};
    auto res = mcp_post(*cli_, "/mcp", req, sid);
    auto tools = res["_body"]["result"]["tools"];
    EXPECT_EQ(tools.size(), 1);
}

TEST_F(ServerTest, ResourcesListNoCursor) {
    auto [sid, _] = mcp_initialize(*cli_);
    json req = {{"jsonrpc", "2.0"}, {"id", 52}, {"method", "resources/list"},
                {"params", json::object()}};
    auto res = mcp_post(*cli_, "/mcp", req, sid);
    auto result = res["_body"]["result"];
    EXPECT_TRUE(result.contains("resources"));
    EXPECT_FALSE(result.contains("nextCursor"));
}

TEST_F(ServerTest, TemplatesListNoCursor) {
    auto [sid, _] = mcp_initialize(*cli_);
    json req = {{"jsonrpc", "2.0"}, {"id", 53}, {"method", "resources/templates/list"},
                {"params", json::object()}};
    auto res = mcp_post(*cli_, "/mcp", req, sid);
    auto result = res["_body"]["result"];
    EXPECT_TRUE(result.contains("resourceTemplates"));
    EXPECT_FALSE(result.contains("nextCursor"));
}

TEST_F(ServerTest, ToolCall) {
    auto [sid, _] = mcp_initialize(*cli_);
    json req = {{"jsonrpc", "2.0"}, {"id", 4}, {"method", "tools/call"},
                {"params", {{"name", "echo"}, {"arguments", {{"text", "hello"}}}}}};
    auto res = mcp_post(*cli_, "/mcp", req, sid);
    auto content = res["_body"]["result"]["content"];
    ASSERT_FALSE(content.empty());
    EXPECT_EQ(content[0]["text"], "hello");
}

// Spec 2025-11-25 (SEP-1303): tools/call validation errors are returned as
// CallToolResult with isError:true, not as JSON-RPC -32602 protocol errors,
// so the model can self-correct on the next attempt.

TEST_F(ServerTest, ToolCallUnknownReturnsToolError) {
    auto [sid, _] = mcp_initialize(*cli_);
    json req = {{"jsonrpc", "2.0"}, {"id", 5}, {"method", "tools/call"},
                {"params", {{"name", "nonexistent"}}}};
    auto res = mcp_post(*cli_, "/mcp", req, sid);
    ASSERT_EQ(res["_status"], 200);
    ASSERT_TRUE(res["_body"].contains("result"));
    EXPECT_FALSE(res["_body"].contains("error"));
    EXPECT_EQ(res["_body"]["result"]["isError"], true);
    EXPECT_EQ(res["_body"]["result"]["content"][0]["type"], "text");
}

TEST_F(ServerTest, ToolCallMissingNameReturnsToolError) {
    auto [sid, _] = mcp_initialize(*cli_);
    json req = {{"jsonrpc", "2.0"}, {"id", 5}, {"method", "tools/call"},
                {"params", json::object()}};
    auto res = mcp_post(*cli_, "/mcp", req, sid);
    ASSERT_EQ(res["_status"], 200);
    ASSERT_TRUE(res["_body"].contains("result"));
    EXPECT_EQ(res["_body"]["result"]["isError"], true);
}

TEST_F(ServerTest, ToolCallBadJsonStringArgsReturnsToolError) {
    auto [sid, _] = mcp_initialize(*cli_);
    json req = {{"jsonrpc", "2.0"}, {"id", 5}, {"method", "tools/call"},
                {"params", {{"name", "echo"}, {"arguments", "{not valid json"}}}};
    auto res = mcp_post(*cli_, "/mcp", req, sid);
    ASSERT_EQ(res["_status"], 200);
    ASSERT_TRUE(res["_body"].contains("result"));
    EXPECT_EQ(res["_body"]["result"]["isError"], true);
}

TEST_F(ServerTest, ToolCallMissingRequiredSchemaParamReturnsToolError) {
    auto [sid, _] = mcp_initialize(*cli_);
    json req = {{"jsonrpc", "2.0"}, {"id", 5}, {"method", "tools/call"},
                {"params", {{"name", "echo"}, {"arguments", json::object()}}}};
    auto res = mcp_post(*cli_, "/mcp", req, sid);
    ASSERT_EQ(res["_status"], 200);
    ASSERT_TRUE(res["_body"].contains("result"));
    EXPECT_EQ(res["_body"]["result"]["isError"], true);
    EXPECT_NE(res["_body"]["result"]["content"][0]["text"].get<std::string>().find("text"),
              std::string::npos);
}

TEST_F(ServerTest, ToolCallWrongSchemaTypeReturnsToolError) {
    auto [sid, _] = mcp_initialize(*cli_);
    json req = {{"jsonrpc", "2.0"}, {"id", 5}, {"method", "tools/call"},
                {"params", {{"name", "echo"}, {"arguments", {{"text", 42}}}}}};
    auto res = mcp_post(*cli_, "/mcp", req, sid);
    ASSERT_EQ(res["_status"], 200);
    ASSERT_TRUE(res["_body"].contains("result"));
    EXPECT_EQ(res["_body"]["result"]["isError"], true);
    EXPECT_NE(res["_body"]["result"]["content"][0]["text"].get<std::string>().find("string"),
              std::string::npos);
}

TEST_F(ServerTest, ToolCallEnforcesCommonJsonSchemaConstraints) {
    tool constrained;
    constrained.name = "constrained";
    constrained.description = "Constrained input";
    constrained.parameters_schema = {
        {"type", "object"},
        {"properties", {
            {"mode", {{"type", "string"}, {"enum", {"safe", "fast"}}}},
            {"count", {{"type", "integer"}, {"minimum", 1}, {"maximum", 3}}},
            {"code", {{"type", "string"}, {"pattern", "^[A-Z]{2}$"}}}
        }},
        {"required", {"mode", "count", "code"}},
        {"additionalProperties", false}
    };

    std::atomic<int> calls{0};
    srv_->register_tool(constrained,
        [&calls](const json&, const std::string&) -> json {
            ++calls;
            return json::array({{{"type", "text"}, {"text", "ok"}}});
        });

    auto [sid, _] = mcp_initialize(*cli_);
    auto call = [&](const json& arguments, int id) {
        json request = {{"jsonrpc", "2.0"}, {"id", id}, {"method", "tools/call"},
                        {"params", {{"name", "constrained"},
                                    {"arguments", arguments}}}};
        return mcp_post(*cli_, "/mcp", request, sid);
    };

    auto bad_enum = call({{"mode", "admin"}, {"count", 2}, {"code", "AB"}}, 60);
    EXPECT_TRUE(bad_enum["_body"]["result"]["isError"]);

    auto bad_range = call({{"mode", "safe"}, {"count", 9}, {"code", "AB"}}, 61);
    EXPECT_TRUE(bad_range["_body"]["result"]["isError"]);

    auto bad_pattern = call({{"mode", "safe"}, {"count", 2}, {"code", "bad"}}, 62);
    EXPECT_TRUE(bad_pattern["_body"]["result"]["isError"]);

    auto extra = call({{"mode", "safe"}, {"count", 2}, {"code", "AB"},
                       {"unexpected", true}}, 63);
    EXPECT_TRUE(extra["_body"]["result"]["isError"]);
    EXPECT_EQ(calls.load(), 0);

    auto valid = call({{"mode", "safe"}, {"count", 2}, {"code", "AB"}}, 64);
    EXPECT_FALSE(valid["_body"]["result"]["isError"]);
    EXPECT_EQ(calls.load(), 1);
}

TEST_F(ServerTest, NullArgumentsTreatedAsEmptyObject) {
    tool no_args;
    no_args.name = "no_args";
    no_args.description = "Takes no arguments";
    no_args.parameters_schema = {{"type", "object"}};

    std::atomic<int> calls{0};
    srv_->register_tool(no_args,
        [&calls](const json&, const std::string&) -> json {
            ++calls;
            return json::array({{{"type", "text"}, {"text", "ok"}}});
        });

    auto [sid, _] = mcp_initialize(*cli_);

    // "arguments" is optional, so an explicit null means the same as omitting
    // the key. Clients send it that way routinely for no-argument tools - our
    // own sse_client does, since a default constructed json is null - and
    // validating null against {"type":"object"} would reject every such call.
    json explicit_null = {{"jsonrpc", "2.0"}, {"id", 70}, {"method", "tools/call"},
                          {"params", {{"name", "no_args"}, {"arguments", nullptr}}}};
    auto with_null = mcp_post(*cli_, "/mcp", explicit_null, sid);
    EXPECT_FALSE(with_null["_body"]["result"]["isError"]);

    json omitted = {{"jsonrpc", "2.0"}, {"id", 71}, {"method", "tools/call"},
                    {"params", {{"name", "no_args"}}}};
    auto without = mcp_post(*cli_, "/mcp", omitted, sid);
    EXPECT_FALSE(without["_body"]["result"]["isError"]);

    EXPECT_EQ(calls.load(), 2);
}

TEST_F(ServerTest, UnsupportedToolSchemaKeywordFailsClosed) {
    tool referenced;
    referenced.name = "referenced";
    referenced.description = "Unsupported schema";
    referenced.parameters_schema = {
        {"type", "object"},
        {"properties", {{"value", {{"$ref", "#/$defs/value"}}}}},
        {"$defs", {{"value", {{"type", "string"}}}}}
    };

    std::atomic<bool> called{false};
    srv_->register_tool(referenced,
        [&called](const json&, const std::string&) -> json {
            called = true;
            return json::array();
        });

    auto [sid, _] = mcp_initialize(*cli_);
    json request = {{"jsonrpc", "2.0"}, {"id", 65}, {"method", "tools/call"},
                    {"params", {{"name", "referenced"},
                                {"arguments", {{"value", "x"}}}}}};
    auto response = mcp_post(*cli_, "/mcp", request, sid);
    EXPECT_TRUE(response["_body"]["result"]["isError"]);
    EXPECT_NE(response["_body"]["result"]["content"][0]["text"]
                  .get<std::string>().find("unsupported"),
              std::string::npos);
    EXPECT_FALSE(called.load());
}

TEST_F(ServerTest, InternalMcpExceptionDetailsAreSanitized) {
    srv_->register_method("secret/fail",
        [](const json&, const std::string&) -> json {
            throw mcp_exception(error_code::internal_error,
                                "database password is hunter2");
        });

    auto secret_tool = tool_builder("secret_tool").with_description("fails").build();
    srv_->register_tool(secret_tool,
        [](const json&, const std::string&) -> json {
            throw mcp_exception(error_code::internal_error,
                                "private tool implementation detail");
        });

    auto [sid, _] = mcp_initialize(*cli_);
    json method_request = {{"jsonrpc", "2.0"}, {"id", 66},
                           {"method", "secret/fail"}};
    auto method_response = mcp_post(*cli_, "/mcp", method_request, sid);
    EXPECT_EQ(method_response["_body"]["error"]["message"], "Internal error");

    json tool_request = {{"jsonrpc", "2.0"}, {"id", 67}, {"method", "tools/call"},
                         {"params", {{"name", "secret_tool"},
                                     {"arguments", json::object()}}}};
    auto tool_response = mcp_post(*cli_, "/mcp", tool_request, sid);
    EXPECT_EQ(tool_response["_body"]["result"]["content"][0]["text"],
              "Tool execution failed");
}

// ===========================================================================
// Resources via Streamable HTTP
// ===========================================================================

TEST_F(ServerTest, ResourcesList) {
    auto [sid, _] = mcp_initialize(*cli_);
    json req = {{"jsonrpc", "2.0"}, {"id", 6}, {"method", "resources/list"},
                {"params", json::object()}};
    auto res = mcp_post(*cli_, "/mcp", req, sid);
    auto resources = res["_body"]["result"]["resources"];
    ASSERT_GE(resources.size(), 1);
    EXPECT_EQ(resources[0]["uri"], "test://hello");
}

TEST_F(ServerTest, ResourceRead) {
    auto [sid, _] = mcp_initialize(*cli_);
    json req = {{"jsonrpc", "2.0"}, {"id", 7}, {"method", "resources/read"},
                {"params", {{"uri", "test://hello"}}}};
    auto res = mcp_post(*cli_, "/mcp", req, sid);
    auto contents = res["_body"]["result"]["contents"];
    ASSERT_FALSE(contents.empty());
    EXPECT_EQ(contents[0]["text"], "Hello, world!");
}

TEST_F(ServerTest, ResourceSubscribeAndUnsubscribe) {
    auto [sid, _] = mcp_initialize(*cli_);
    json sub = {{"jsonrpc", "2.0"}, {"id", 20}, {"method", "resources/subscribe"},
                {"params", {{"uri", "test://hello"}}}};
    auto res1 = mcp_post(*cli_, "/mcp", sub, sid);
    EXPECT_EQ(res1["_status"], 200);

    json unsub = {{"jsonrpc", "2.0"}, {"id", 21}, {"method", "resources/unsubscribe"},
                  {"params", {{"uri", "test://hello"}}}};
    auto res2 = mcp_post(*cli_, "/mcp", unsub, sid);
    EXPECT_EQ(res2["_status"], 200);
}

TEST_F(ServerTest, ResourceTemplateRead) {
    auto [sid, _] = mcp_initialize(*cli_);
    json req = {{"jsonrpc", "2.0"}, {"id", 8}, {"method", "resources/read"},
                {"params", {{"uri", "test://items/42"}}}};
    auto res = mcp_post(*cli_, "/mcp", req, sid);
    auto contents = res["_body"]["result"]["contents"];
    ASSERT_FALSE(contents.empty());
    EXPECT_EQ(contents[0]["text"], "{\"id\":\"42\"}");
}

TEST_F(ServerTest, ResourceTemplateList) {
    auto [sid, _] = mcp_initialize(*cli_);
    json req = {{"jsonrpc", "2.0"}, {"id", 9}, {"method", "resources/templates/list"},
                {"params", json::object()}};
    auto res = mcp_post(*cli_, "/mcp", req, sid);
    auto templates = res["_body"]["result"]["resourceTemplates"];
    ASSERT_GE(templates.size(), 1);
    EXPECT_EQ(templates[0]["uriTemplate"], "test://items/{id}");
}

// ===========================================================================
// Logging Protocol
// ===========================================================================

TEST_F(ServerTest, LoggingSetLevel) {
    auto [sid, _] = mcp_initialize(*cli_);
    json req = {{"jsonrpc", "2.0"}, {"id", 30}, {"method", "logging/setLevel"},
                {"params", {{"level", "debug"}}}};
    auto res = mcp_post(*cli_, "/mcp", req, sid);
    EXPECT_EQ(res["_status"], 200);
    EXPECT_EQ(res["_body"]["result"], json::object());
}

TEST_F(ServerTest, BroadcastLog) {
    auto [sid, _] = mcp_initialize(*cli_);
    // Set level to debug so all messages pass
    json req = {{"jsonrpc", "2.0"}, {"id", 31}, {"method", "logging/setLevel"},
                {"params", {{"level", "debug"}}}};
    mcp_post(*cli_, "/mcp", req, sid);

    // broadcast_log shouldn't crash
    EXPECT_NO_THROW(srv_->broadcast_log("info", "test", "Hello from test"));
}

// ===========================================================================
// Prompts via Streamable HTTP
// ===========================================================================

TEST_F(ServerTest, PromptsList) {
    auto [sid, _] = mcp_initialize(*cli_);
    json req = {{"jsonrpc", "2.0"}, {"id", 40}, {"method", "prompts/list"},
                {"params", json::object()}};
    auto res = mcp_post(*cli_, "/mcp", req, sid);
    auto prompts = res["_body"]["result"]["prompts"];
    ASSERT_EQ(prompts.size(), 1);
    EXPECT_EQ(prompts[0]["name"], "greet");
    EXPECT_TRUE(prompts[0].contains("arguments"));
}

TEST_F(ServerTest, PromptsGet) {
    auto [sid, _] = mcp_initialize(*cli_);
    json req = {{"jsonrpc", "2.0"}, {"id", 41}, {"method", "prompts/get"},
                {"params", {{"name", "greet"}, {"arguments", {{"name", "Alice"}}}}}};
    auto res = mcp_post(*cli_, "/mcp", req, sid);
    auto messages = res["_body"]["result"]["messages"];
    ASSERT_FALSE(messages.empty());
    EXPECT_EQ(messages[0]["role"], "user");
}

TEST_F(ServerTest, PromptsGetNotFound) {
    auto [sid, _] = mcp_initialize(*cli_);
    json req = {{"jsonrpc", "2.0"}, {"id", 42}, {"method", "prompts/get"},
                {"params", {{"name", "nonexistent"}}}};
    auto res = mcp_post(*cli_, "/mcp", req, sid);
    EXPECT_TRUE(res["_body"].contains("error"));
}

// ===========================================================================
// CORS Headers
// ===========================================================================

TEST_F(ServerTest, CorsPreflightHeaders) {
    auto res = cli_->Options("/mcp");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 204);
    EXPECT_TRUE(res->has_header("Access-Control-Allow-Origin"));
    EXPECT_TRUE(res->has_header("Access-Control-Allow-Methods"));
    EXPECT_TRUE(res->has_header("Access-Control-Allow-Headers"));
    EXPECT_TRUE(res->has_header("Access-Control-Expose-Headers"));

    std::string expose = res->get_header_value("Access-Control-Expose-Headers");
    EXPECT_NE(expose.find("Mcp-Session-Id"), std::string::npos);
}

TEST_F(ServerTest, StreamableHttpGetSendsInitialBytesImmediately) {
    auto [sid, _] = mcp_initialize(*cli_);
    ASSERT_FALSE(sid.empty());

    std::atomic<bool> got_data{false};
    auto sse_cli = std::make_unique<httplib::Client>("127.0.0.1", port_);
    sse_cli->set_read_timeout(2);

    httplib::Headers headers;
    headers.emplace("Accept", "text/event-stream");
    headers.emplace("Mcp-Session-Id", sid);

    std::thread t([&] {
        sse_cli->Get("/mcp", headers, [&](const char* data, size_t len) {
            if (len > 0) {
                got_data.store(true);
            }
            return false; // close after the initial SSE bytes
        });
    });
    t.join();

    EXPECT_TRUE(got_data.load());
}

// ===========================================================================
// Legacy SSE Transport
// ===========================================================================

TEST_F(ServerTest, SseEndpointReturnsEventStream) {
    // Verify the SSE endpoint accepts connections and sends the endpoint event.
    std::atomic<bool> got_data{false};
    auto sse_cli = std::make_unique<httplib::Client>("127.0.0.1", port_);
    sse_cli->set_read_timeout(2);
    std::thread t([&] {
        sse_cli->Get("/sse", [&](const char* data, size_t len) {
            if (len > 0) got_data.store(true);
            return false; // close after first chunk
        });
    });
    t.join();
    EXPECT_TRUE(got_data.load());
}

TEST(LegacySseTransport, CanBeDisabled) {
    int port = next_port();
    server::configuration conf;
    conf.host = "127.0.0.1";
    conf.port = port;
    conf.enable_legacy_sse_transport = false;
    server srv(conf);
    srv.start(false);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    httplib::Client cli("127.0.0.1", port);
    cli.set_connection_timeout(2);
    cli.set_read_timeout(2);

    auto sse = cli.Get("/sse");
    ASSERT_TRUE(sse);
    EXPECT_EQ(sse->status, 404);

    auto message = cli.Post("/message?session_id=missing", "{}", "application/json");
    ASSERT_TRUE(message);
    EXPECT_EQ(message->status, 404);

    srv.stop();
}

// ===========================================================================
// Progress & Cancellation
// ===========================================================================

TEST_F(ServerTest, CancellationTracking) {
    auto [sid, _] = mcp_initialize(*cli_);

    // Send a cancellation notification for request ID 42
    json cancel = {{"jsonrpc", "2.0"}, {"method", "notifications/cancelled"},
                   {"params", {{"requestId", 42}, {"reason", "User abort"}}}};
    auto res = mcp_post(*cli_, "/mcp", cancel, sid);
    EXPECT_EQ(res["_status"], 202);

    // Server should now report that request 42 is cancelled
    EXPECT_TRUE(srv_->is_cancelled(42, sid));
    EXPECT_FALSE(srv_->is_cancelled(99, sid));
}

TEST_F(ServerTest, SendProgress) {
    auto [sid, _] = mcp_initialize(*cli_);
    // Just verify send_progress doesn't crash — actual delivery requires SSE stream
    EXPECT_NO_THROW(srv_->send_progress(sid, "token-abc", 50, 100, "Half done"));
}

// ===========================================================================
// SSE Client Integration (uses sse_client class)
// NOTE: sse_client teardown has a known segfault in thread cleanup.
// These tests are disabled by default until the SSE client is fixed.
// Run with: --gtest_also_run_disabled_tests
// ===========================================================================

class SseClientTest : public ::testing::Test {
protected:
    void SetUp() override {
        port_ = next_port();
        server::configuration conf;
        conf.host = "127.0.0.1";
        conf.port = port_;
        conf.name = "SseTestServer";
        conf.version = "1.0.0";

        srv_ = std::make_unique<server>(conf);
        srv_->set_capabilities({{"tools", {{"listChanged", true}}}});

        auto t = tool_builder("greet")
            .with_description("Greet")
            .with_string_param("name", "Name")
            .build();
        srv_->register_tool(t, [](const json& args, const std::string&) -> json {
            return json::array({{{"type", "text"},
                                 {"text", "Hi " + args.value("name", "world")}}});
        });

        srv_->start(false);
        std::this_thread::sleep_for(std::chrono::milliseconds(50));

        std::string url = "http://127.0.0.1:" + std::to_string(port_);
        client_ = std::make_unique<sse_client>(url);
    }

    void TearDown() override {
        client_.reset();
        if (srv_) srv_->stop();
        srv_.reset();
    }

    int port_;
    std::unique_ptr<server> srv_;
    std::unique_ptr<sse_client> client_;
};

TEST_F(SseClientTest, DISABLED_InitializeAndPing) {
    ASSERT_TRUE(client_->initialize("TestClient", "1.0.0"));
    EXPECT_TRUE(client_->ping());
}

TEST_F(SseClientTest, DISABLED_GetTools) {
    ASSERT_TRUE(client_->initialize("TestClient", "1.0.0"));
    auto tools = client_->get_tools();
    ASSERT_EQ(tools.size(), 1);
    EXPECT_EQ(tools[0].name, "greet");
}

TEST_F(SseClientTest, DISABLED_CallTool) {
    ASSERT_TRUE(client_->initialize("TestClient", "1.0.0"));
    json result = client_->call_tool("greet", {{"name", "Alice"}});
    EXPECT_EQ(result["content"][0]["text"], "Hi Alice");
}

TEST_F(SseClientTest, DISABLED_ServerCapabilities) {
    ASSERT_TRUE(client_->initialize("TestClient", "1.0.0"));
    json caps = client_->get_server_capabilities();
    EXPECT_TRUE(caps.contains("tools"));
}

// ===========================================================================
// Session Limits
// ===========================================================================

TEST_F(ServerTest, SessionLimitEnforced) {
    // Default MCP_MAX_SESSIONS is 10. Fill them up via Streamable HTTP.
    std::vector<std::string> sessions;
    for (int i = 0; i < 10; i++) {
        auto [sid, body] = mcp_initialize(*cli_);
        if (!sid.empty()) sessions.push_back(sid);
    }
    ASSERT_EQ(sessions.size(), 10);

    // 11th should be rejected with 503
    json init_req = {
        {"jsonrpc", "2.0"}, {"id", 1}, {"method", "initialize"},
        {"params", {
            {"protocolVersion", MCP_VERSION},
            {"clientInfo", {{"name", "Overflow"}, {"version", "1.0"}}},
            {"capabilities", json::object()}
        }}
    };
    auto res = mcp_post(*cli_, "/mcp", init_req);
    EXPECT_EQ(res["_status"], 503);

    // Clean up: delete sessions so other tests aren't affected
    for (const auto& sid : sessions) {
        httplib::Headers headers;
        headers.emplace("Mcp-Session-Id", sid);
        cli_->Delete("/mcp", headers);
    }
}

// ===========================================================================
// Method Not Found
// ===========================================================================

TEST_F(ServerTest, MethodNotFound) {
    auto [sid, _] = mcp_initialize(*cli_);
    json req = {{"jsonrpc", "2.0"}, {"id", 99}, {"method", "nonexistent/method"},
                {"params", json::object()}};
    auto res = mcp_post(*cli_, "/mcp", req, sid);
    EXPECT_TRUE(res["_body"].contains("error"));
    EXPECT_EQ(res["_body"]["error"]["code"],
              static_cast<int>(error_code::method_not_found));
}

// ===========================================================================
// Broadcast Notification
// ===========================================================================

TEST_F(ServerTest, BroadcastNotification) {
    auto [sid, _] = mcp_initialize(*cli_);
    ASSERT_FALSE(sid.empty());

    auto sessions = srv_->get_active_sessions();
    EXPECT_GE(sessions.size(), 1);

    // Just verify it doesn't crash — actual delivery requires SSE stream
    auto notif = request::create_notification("test_event", {{"data", "hello"}});
    EXPECT_NO_THROW(srv_->broadcast_notification(notif));
}

// ===========================================================================
// resource_manager: callbacks must not run under the manager's mutex,
// so reentrant subscribe/unsubscribe/notify don't self-deadlock.
// ===========================================================================

// Helper: run `fn` on a background thread. If it doesn't finish within
// `timeout`, detach it (leaking the thread) and return false. Used to test
// for deadlocks without hanging the whole suite. NOTE: detached deadlocked
// threads hold whatever locks they were stuck on, so subsequent tests in
// the same process that touch the same locks may be affected. Acceptable
// here because these regression tests verify the fix is in place; if they
// fail, the suite is already in a degraded state and needs developer
// attention regardless.
static bool run_with_timeout(std::function<void()> fn,
                             std::chrono::milliseconds timeout) {
    auto done = std::make_shared<std::atomic<bool>>(false);
    std::thread t([fn = std::move(fn), done]() {
        fn();
        done->store(true);
    });
    auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!done->load() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    if (done->load()) {
        t.join();
        return true;
    }
    t.detach();
    return false;
}

TEST(ResourceManagerReentrancy, CallbackCanUnsubscribeWithoutDeadlocking) {
    // Regression: notify_resource_changed() used to invoke subscriber
    // callbacks while holding g_resource_manager_mutex. Any callback that
    // touched the manager (subscribe/unsubscribe/list_resources/get_resource)
    // would re-lock the same non-recursive mutex and hard-deadlock.
    auto& mgr = resource_manager::instance();

    auto res = std::make_shared<text_resource>(
        "test://reentry-unsub", "reentry-unsub", "text/plain");
    res->set_text("ok");
    mgr.register_resource(res);

    int sub_id = -1;
    std::atomic<bool> called{false};
    sub_id = mgr.subscribe(
        "test://reentry-unsub",
        [&](const std::string&) {
            called.store(true);
            mgr.unsubscribe(sub_id);  // would deadlock without the fix
        });

    bool finished = run_with_timeout(
        [&]() { mgr.notify_resource_changed("test://reentry-unsub"); },
        std::chrono::seconds(2));

    ASSERT_TRUE(finished)
        << "notify_resource_changed deadlocks when callback re-enters manager";
    EXPECT_TRUE(called.load());
    mgr.unregister_resource("test://reentry-unsub");
}

TEST(ResourceManagerReentrancy, CallbackCanListResourcesWithoutDeadlocking) {
    auto& mgr = resource_manager::instance();
    auto res = std::make_shared<text_resource>(
        "test://reentry-list", "reentry-list", "text/plain");
    res->set_text("ok");
    mgr.register_resource(res);

    std::atomic<bool> called{false};
    int sub_id = mgr.subscribe(
        "test://reentry-list",
        [&](const std::string&) {
            called.store(true);
            (void)mgr.list_resources();  // would deadlock without the fix
        });

    bool finished = run_with_timeout(
        [&]() { mgr.notify_resource_changed("test://reentry-list"); },
        std::chrono::seconds(2));

    ASSERT_TRUE(finished)
        << "notify_resource_changed deadlocks when callback calls list_resources";
    EXPECT_TRUE(called.load());

    mgr.unsubscribe(sub_id);
    mgr.unregister_resource("test://reentry-list");
}

// ===========================================================================
// event_dispatcher: queued events are delivered in order
// ===========================================================================
//
// Regression for the lost-event race fixed by the queue-based dispatcher.
// The pre-fix dispatcher had a single std::string slot plus an id_/cid_
// equality predicate: two send_event() calls between consume cycles would
// (a) overwrite the first message in the slot, and (b) skip cid_ past the
// consumer's snapshot, stranding wait_event() until keepalive timeout.
// Practical impact: large/slow tool responses (e.g. 80KB add_stage payload)
// raced with the 5-second heartbeat thread and never reached the client —
// the SSE channel just kept emitting heartbeats forever.

TEST(EventDispatcher, DeliversAllQueuedEventsInOrder) {
    event_dispatcher dispatcher;

    // Enqueue three events before any consumer runs. Pre-fix this would
    // leave only the third in the slot and strand wait_event waiting for
    // a stale id snapshot.
    EXPECT_TRUE(dispatcher.send_event("event-1"));
    EXPECT_TRUE(dispatcher.send_event("event-2"));
    EXPECT_TRUE(dispatcher.send_event("event-3"));

    std::string out;
    httplib::DataSink sink;
    sink.write = [&out](const char* data, size_t len) {
        out.append(data, len);
        return true;
    };
    sink.is_writable = [] { return true; };
    sink.done = [] {};
    sink.done_with_trailer = [](const httplib::Headers&) {};

    EXPECT_TRUE(dispatcher.wait_event(&sink, std::chrono::milliseconds(50)));
    EXPECT_EQ(out, "event-1");
    out.clear();

    EXPECT_TRUE(dispatcher.wait_event(&sink, std::chrono::milliseconds(50)));
    EXPECT_EQ(out, "event-2");
    out.clear();

    EXPECT_TRUE(dispatcher.wait_event(&sink, std::chrono::milliseconds(50)));
    EXPECT_EQ(out, "event-3");
}

TEST(EventDispatcher, ConcurrentSendersDoNotLoseEvents) {
    // Reproduces the heartbeat-vs-tool-response race. Two threads emit
    // distinct messages while a single consumer drains them. Pre-fix the
    // strict cid_ == id predicate stranded the consumer when the second
    // sender ran between the consumer's wakeup and lock acquisition;
    // here we expect EVERY message from both senders to reach the sink.
    event_dispatcher dispatcher;

    constexpr int N = 50;
    std::atomic<int> heartbeats_sent{0};
    std::atomic<int> responses_sent{0};

    std::thread hb([&] {
        for (int i = 0; i < N; ++i) {
            if (dispatcher.send_event("hb-" + std::to_string(i))) {
                ++heartbeats_sent;
            }
            std::this_thread::sleep_for(std::chrono::microseconds(100));
        }
    });
    std::thread resp([&] {
        for (int i = 0; i < N; ++i) {
            if (dispatcher.send_event("resp-" + std::to_string(i))) {
                ++responses_sent;
            }
            std::this_thread::sleep_for(std::chrono::microseconds(150));
        }
    });

    hb.join();
    resp.join();

    std::string out;
    httplib::DataSink sink;
    sink.write = [&out](const char* data, size_t len) {
        out.append(data, len);
        return true;
    };
    sink.is_writable = [] { return true; };
    sink.done = [] {};
    sink.done_with_trailer = [](const httplib::Headers&) {};

    int consumed = 0;
    while (consumed < heartbeats_sent + responses_sent) {
        std::string before = out;
        if (!dispatcher.wait_event(&sink, std::chrono::milliseconds(50))) {
            break;
        }
        if (out.size() > before.size()) {
            ++consumed;
        }
    }

    EXPECT_EQ(heartbeats_sent.load(), N);
    EXPECT_EQ(responses_sent.load(), N);
    EXPECT_EQ(consumed, 2 * N);

    // Every message body must appear exactly once in the sink output.
    for (int i = 0; i < N; ++i) {
        EXPECT_NE(out.find("hb-" + std::to_string(i)), std::string::npos)
            << "lost hb-" << i;
        EXPECT_NE(out.find("resp-" + std::to_string(i)), std::string::npos)
            << "lost resp-" << i;
    }
}

TEST(EventDispatcher, OverflowReturnsFalseRatherThanBlocking) {
    // The bounded queue caps memory growth if a consumer stalls. Past the
    // cap, send_event returns false so the caller can detect a wedged
    // connection rather than silently dropping or growing unbounded.
    event_dispatcher dispatcher;

    size_t sent = 0;
    while (dispatcher.send_event("x") && sent < event_dispatcher::MAX_QUEUED_EVENTS + 10) {
        ++sent;
    }
    EXPECT_EQ(sent, event_dispatcher::MAX_QUEUED_EVENTS);

    // After overflow, further sends still fail until the consumer drains.
    EXPECT_FALSE(dispatcher.send_event("y"));

    std::string out;
    httplib::DataSink sink;
    sink.write = [&out](const char* data, size_t len) {
        out.append(data, len);
        return true;
    };
    sink.is_writable = [] { return true; };
    sink.done = [] {};
    sink.done_with_trailer = [](const httplib::Headers&) {};
    EXPECT_TRUE(dispatcher.wait_event(&sink, std::chrono::milliseconds(50)));

    // One slot freed — next send succeeds.
    EXPECT_TRUE(dispatcher.send_event("y"));
}

// clientInfo is the only identity a client volunteers, and it arrives once at
// initialize. Discarding it leaves a server operator unable to tell their own
// tooling apart from a client they did not start -- which matters most when the
// transport has no authentication.
TEST_F(ServerTest, InitializeRetainsClientIdentity) {
    auto [sid, _] = mcp_initialize(*cli_);

    auto sessions = srv_->get_sessions();
    ASSERT_EQ(1u, sessions.size());
    EXPECT_EQ(sid, sessions[0].session_id);
    EXPECT_FALSE(sessions[0].client_name.empty()) << "clientInfo.name was discarded";
    EXPECT_FALSE(sessions[0].protocol_version.empty());
    EXPECT_GT(sessions[0].connected_at_unix, 0);
}

// A host tracking who is attached has register_session_cleanup for the way out
// but nothing for the way in, leaving it to poll get_sessions() to notice a
// connection. The handler must fire after the session is recorded, or a host
// that responds by reading get_sessions() would not find the client it was just
// told about.
TEST_F(ServerTest, SessionOpenHandlerFiresAfterTheSessionIsRecorded) {
    std::mutex seen_mutex;
    std::vector<std::string> opened;
    std::vector<size_t> visible_session_counts;

    srv_->register_session_open("test", [&](const std::string& session_id) {
        std::lock_guard<std::mutex> lock(seen_mutex);
        opened.push_back(session_id);
        visible_session_counts.push_back(srv_->get_sessions().size());
    });

    auto [sid, _] = mcp_initialize(*cli_);

    std::lock_guard<std::mutex> lock(seen_mutex);
    ASSERT_EQ(1u, opened.size()) << "handler should fire exactly once per session";
    EXPECT_EQ(sid, opened[0]);
    ASSERT_EQ(1u, visible_session_counts.size());
    EXPECT_EQ(1u, visible_session_counts[0])
        << "get_sessions() must already include the session being announced";
}

// Re-registering under the same key replaces, matching register_session_cleanup.
// Without this a host that restarts its bookkeeping would stack duplicate
// handlers and emit one event per registration.
TEST_F(ServerTest, SessionOpenHandlerKeyReplacesRatherThanAccumulates) {
    std::mutex seen_mutex;
    int first_calls = 0;
    int second_calls = 0;

    srv_->register_session_open("test", [&](const std::string&) {
        std::lock_guard<std::mutex> lock(seen_mutex);
        ++first_calls;
    });
    srv_->register_session_open("test", [&](const std::string&) {
        std::lock_guard<std::mutex> lock(seen_mutex);
        ++second_calls;
    });

    mcp_initialize(*cli_);

    std::lock_guard<std::mutex> lock(seen_mutex);
    EXPECT_EQ(0, first_calls) << "replaced handler should not still fire";
    EXPECT_EQ(1, second_calls);
}

TEST_F(ServerTest, ToolCallsAreCountedPerSession) {
    tool counted;
    counted.name = "counted";
    counted.description = "Counts";
    counted.parameters_schema = {{"type", "object"}};
    srv_->register_tool(counted, [](const json&, const std::string&) -> json {
        return json::array({{{"type", "text"}, {"text", "ok"}}});
    });

    auto [sid, _] = mcp_initialize(*cli_);
    for (int i = 0; i < 3; ++i) {
        json request = {{"jsonrpc", "2.0"}, {"id", 80 + i}, {"method", "tools/call"},
                        {"params", {{"name", "counted"}, {"arguments", json::object()}}}};
        mcp_post(*cli_, "/mcp", request, sid);
    }

    auto sessions = srv_->get_sessions();
    ASSERT_EQ(1u, sessions.size());
    EXPECT_EQ(3u, sessions[0].tool_call_count);
    EXPECT_GT(sessions[0].last_seen_unix, 0);
}

// Sessions are tracked independently, so a status view can distinguish two
// clients rather than collapsing them.
//
// Not covered here: forgetting a session on disconnect. close_session is
// private and the timeout path is too slow for a unit test; the erase sits
// beside the other per-session cleanup, which is the same path those already
// exercise.
TEST_F(ServerTest, SessionsAreTrackedIndependently) {
    auto [sid_a, _a] = mcp_initialize(*cli_);

    httplib::Client second("localhost", port_);
    auto [sid_b, _b] = mcp_initialize(second);

    ASSERT_NE(sid_a, sid_b);
    auto sessions = srv_->get_sessions();
    ASSERT_EQ(2u, sessions.size());

    std::set<std::string> ids;
    for (const auto& s : sessions) {
        ids.insert(s.session_id);
    }
    EXPECT_EQ(1u, ids.count(sid_a));
    EXPECT_EQ(1u, ids.count(sid_b));
}
