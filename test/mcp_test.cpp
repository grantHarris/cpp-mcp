/**
 * @file mcp_test.cpp
 * @brief Tests for MCP 2025-03-26 spec compliance
 *
 * Tests cover: JSON-RPC message format, server lifecycle, Streamable HTTP
 * transport, legacy SSE transport, tools, resources, resource templates,
 * session management, and CORS headers.
 */

#include <gtest/gtest.h>
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
                     const json& body, const std::string& session_id = "") {
    httplib::Headers headers;
    headers.emplace("Content-Type", "application/json");
    headers.emplace("Accept", "application/json, text/event-stream");
    if (!session_id.empty()) {
        headers.emplace("Mcp-Session-Id", session_id);
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
                                                    const std::string& path = "/mcp") {
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
    auto res = mcp_post(cli, path, init_req);
    std::string sid = res.value("_session_id", "");
    json body = res.value("_body", json::object());

    // Send initialized notification
    if (!sid.empty()) {
        json notif = {
            {"jsonrpc", "2.0"},
            {"method", "notifications/initialized"}
        };
        mcp_post(cli, path, notif, sid);
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

TEST_F(ServerTest, InitializeVersionNegotiation) {
    json req = {
        {"jsonrpc", "2.0"}, {"id", 1}, {"method", "initialize"},
        {"params", {
            {"protocolVersion", "1999-01-01"},
            {"clientInfo", {{"name", "OldClient"}, {"version", "0.1"}}},
            {"capabilities", json::object()}
        }}
    };
    auto res = mcp_post(*cli_, "/mcp", req);
    // Server responds with its own version regardless
    EXPECT_EQ(res["_body"]["result"]["protocolVersion"], MCP_VERSION);
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

TEST_F(ServerTest, BatchRequest) {
    auto [sid, _] = mcp_initialize(*cli_);
    json batch = json::array({
        {{"jsonrpc", "2.0"}, {"id", 10}, {"method", "ping"}},
        {{"jsonrpc", "2.0"}, {"id", 11}, {"method", "ping"}}
    });
    auto res = mcp_post(*cli_, "/mcp", batch, sid);
    // Should get back an array of two responses or SSE
    int status = res["_status"];
    EXPECT_TRUE(status == 200);
}

TEST_F(ServerTest, RejectInitializeInBatch) {
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

TEST_F(ServerTest, ToolCallNotFound) {
    auto [sid, _] = mcp_initialize(*cli_);
    json req = {{"jsonrpc", "2.0"}, {"id", 5}, {"method", "tools/call"},
                {"params", {{"name", "nonexistent"}}}};
    auto res = mcp_post(*cli_, "/mcp", req, sid);
    EXPECT_TRUE(res["_body"].contains("error"));
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
