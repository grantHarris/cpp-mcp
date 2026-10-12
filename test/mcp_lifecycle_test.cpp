#include "mcp_server.h"
#include "mcp_sse_client.h"
#include <fstream>
#include <gtest/gtest.h>
#ifndef _WIN32
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace mcp {
// Inspect retained ownership without adding a public diagnostic API.
struct server_test_access {
    static std::vector<std::shared_future<void>> heartbeat_completions(server &srv) {
        std::lock_guard<std::mutex> lock(srv.mutex_);
        std::vector<std::shared_future<void>> out;
        for (const auto &entry : srv.sse_threads_)
            out.push_back(entry.second.finished);
        return out;
    }
    static size_t heartbeat_count(server &srv) {
        std::lock_guard<std::mutex> lock(srv.mutex_);
        return srv.sse_threads_.size();
    }
    static std::shared_ptr<event_dispatcher> dispatcher(server &srv, const std::string &sid) {
        std::lock_guard<std::mutex> lock(srv.mutex_);
        return srv.session_dispatchers_.at(sid);
    }
    static bool wait_for_stop_completion(server &srv) {
        std::unique_lock<std::mutex> lock(srv.stop_mutex_);
        return srv.stop_cv_.wait_for(lock, std::chrono::seconds(3),
                                     [&] { return !srv.stop_in_progress_; });
    }
};
} // namespace mcp

namespace {
using namespace mcp;

json initialize_request() {
    return {{"jsonrpc", "2.0"},
            {"id", 1},
            {"method", "initialize"},
            {"params",
             {{"protocolVersion", MCP_VERSION},
              {"capabilities", json::object()},
              {"clientInfo", {{"name", "LifecycleTest"}, {"version", "1"}}}}}};
}

json rpc_request(const std::string &method, const json &params = json::object(),
                 const json &id = 1) {
    json out = {{"jsonrpc", "2.0"}, {"method", method}, {"params", params}};
    if (!id.is_null()) out["id"] = id;
    return out;
}

class LifecycleTest : public ::testing::Test {
  protected:
    void SetUp() override {
        static int next_port = 9500;
        conf.host = "127.0.0.1";
        conf.port = next_port++;
        conf.max_sessions = 2;
        conf.session_timeout = 0;
        conf.threadpool_size = 1;
        conf.http_thread_pool_size = 4;
        srv = std::make_unique<server>(conf);
        ASSERT_TRUE(srv->start(false));
        cli = std::make_unique<httplib::Client>(conf.host, conf.port);
        cli->set_read_timeout(3);
    }
    void TearDown() override { srv->stop(); }
    httplib::Result initialize(const json &req = initialize_request()) {
        return cli->Post("/mcp", req.dump(), "application/json");
    }
    std::string initialized_session() {
        auto response = initialize();
        if (!response || response->status != 200 || !response->has_header("Mcp-Session-Id")) {
            return {};
        }
        const auto sid = response->get_header_value("Mcp-Session-Id");
        const auto initialized = rpc_request("notifications/initialized", json::object(), nullptr);
        auto notice = cli->Post("/mcp", {{"Mcp-Session-Id", sid}}, initialized.dump(),
                                "application/json");
        return notice && notice->status == 202 ? sid : std::string{};
    }
    server::configuration conf;
    std::mutex cleanup_mutex;
    std::condition_variable cleanup_cv;
    std::vector<std::string> cleaned;
    std::unique_ptr<server> srv;
    std::unique_ptr<httplib::Client> cli;
};

TEST_F(LifecycleTest, InvalidInitializeDoesNotConsumeSessionCapacity) {
    for (int i = 0; i < 6; ++i) {
        auto req = initialize_request();
        if (i % 2 == 0)
            req["params"].erase("protocolVersion");
        else
            req["params"]["clientInfo"]["name"] = 42;
        auto res = initialize(req);
        ASSERT_TRUE(res);
        EXPECT_EQ(200, res->status);
        EXPECT_EQ(-32602, json::parse(res->body)["error"]["code"]);
        EXPECT_FALSE(res->has_header("Mcp-Session-Id"));
        EXPECT_TRUE(srv->get_sessions().empty());
    }
    auto first = initialize();
    auto second = initialize();
    ASSERT_TRUE(first);
    ASSERT_TRUE(second);
    EXPECT_TRUE(first->has_header("Mcp-Session-Id"));
    EXPECT_TRUE(second->has_header("Mcp-Session-Id"));
    auto overflow = initialize();
    ASSERT_TRUE(overflow);
    EXPECT_EQ(503, overflow->status);
}

TEST_F(LifecycleTest, ThrowingOpenHookReleasesInitializeReservation) {
    srv->register_session_open(
        "test", [](const std::string &) { throw std::runtime_error("host hook failed"); });
    for (int i = 0; i < 4; ++i) {
        auto res = initialize();
        ASSERT_TRUE(res);
        EXPECT_EQ(500, res->status);
        EXPECT_FALSE(res->has_header("Mcp-Session-Id"));
        EXPECT_TRUE(srv->get_sessions().empty());
    }
    srv->register_session_open("test", {});
    auto valid = initialize();
    ASSERT_TRUE(valid);
    EXPECT_EQ(200, valid->status);
    EXPECT_TRUE(valid->has_header("Mcp-Session-Id"));
}

TEST_F(LifecycleTest, StopCleansEachSessionExactlyOnceAndCanRestart) {
    srv->register_session_cleanup("registration-key", [this](const std::string &id) {
        // Both calls must be safe while stop invokes host cleanup.
        (void)srv->get_sessions();
        cleaned.push_back(id);
        if (!srv->is_running())
            srv->stop();
    });
    auto a = initialize();
    auto b = initialize();
    ASSERT_TRUE(a);
    ASSERT_TRUE(b);
    const auto sid_a = a->get_header_value("Mcp-Session-Id");
    const auto sid_b = b->get_header_value("Mcp-Session-Id");
    auto deleted = cli->Delete("/mcp", {{"Mcp-Session-Id", sid_a}});
    ASSERT_TRUE(deleted);
    EXPECT_EQ(200, deleted->status);
    srv->stop();
    srv->stop();
    EXPECT_EQ((std::vector<std::string>{sid_a, sid_b}), cleaned);
    EXPECT_TRUE(srv->get_sessions().empty());
    EXPECT_TRUE(srv->session_protocol_version(sid_b).empty());
    ASSERT_TRUE(srv->start(false));
    auto c = initialize();
    ASSERT_TRUE(c);
    ASSERT_EQ(200, c->status);
    srv->stop();
    ASSERT_EQ(3u, cleaned.size());
    EXPECT_EQ(c->get_header_value("Mcp-Session-Id"), cleaned.back());
    EXPECT_TRUE(srv->get_sessions().empty());
}

TEST_F(LifecycleTest, ReconnectingSseClientsDoNotAccumulateFinishedThreads) {
    srv->register_session_cleanup("test", [this](const std::string &id) {
        std::lock_guard<std::mutex> lock(cleanup_mutex);
        cleaned.push_back(id);
        cleanup_cv.notify_all();
    });
    for (size_t i = 0; i < 8; ++i) {
        std::vector<std::shared_future<void>> completions;
        {
            sse_client client("http://127.0.0.1:" + std::to_string(conf.port));
            client.set_timeout(2);
            ASSERT_TRUE(client.initialize("reconnecting", "1"));
            EXPECT_LE(server_test_access::heartbeat_count(*srv), 1u);
            completions = server_test_access::heartbeat_completions(*srv);
        }
        {
            std::unique_lock<std::mutex> lock(cleanup_mutex);
            ASSERT_TRUE(cleanup_cv.wait_for(lock, std::chrono::seconds(3),
                                            [&] { return cleaned.size() == i + 1; }));
        }
        for (auto &done : completions) {
            ASSERT_EQ(std::future_status::ready, done.wait_for(std::chrono::seconds(3)));
        }
    }
    srv->stop();
    EXPECT_EQ(0u, server_test_access::heartbeat_count(*srv));
    EXPECT_EQ(8u, cleaned.size());
}

TEST_F(LifecycleTest, DisconnectingStreamableGetKeepsSessionAndPushChannelUsable) {
    const auto sid = initialized_session();
    ASSERT_FALSE(sid.empty());
    auto dispatcher = server_test_access::dispatcher(*srv, sid);
    const httplib::Headers headers = {{"Mcp-Session-Id", sid}, {"Accept", "text/event-stream"}};

    std::string first_chunk;
    auto first = cli->Get("/mcp", headers, [&](const char *data, size_t n) {
        first_chunk.append(data, n);
        return false; // Cancel this GET without terminating the MCP session.
    });
    EXPECT_EQ(":\n\n", first_chunk);
    EXPECT_FALSE(dispatcher->wait_for_close(std::chrono::seconds(2)));
    ASSERT_TRUE(dispatcher->send_event("event: message\ndata: reconnected\n\n"));

    httplib::Client reconnect(conf.host, conf.port);
    reconnect.set_read_timeout(3);
    std::string received;
    auto second = reconnect.Get("/mcp", headers, [&](const char *data, size_t n) {
        received.append(data, n);
        return received.find("reconnected") == std::string::npos;
    });
    EXPECT_NE(std::string::npos, received.find("reconnected"));
    EXPECT_FALSE(dispatcher->is_closed());
    const auto sessions = srv->get_sessions();
    ASSERT_EQ(1u, sessions.size());
    EXPECT_EQ(sid, sessions.front().session_id);
    auto ping = cli->Post("/mcp", {{"Mcp-Session-Id", sid}},
                          rpc_request("ping").dump(), "application/json");
    ASSERT_TRUE(ping);
    EXPECT_EQ(200, ping->status);
}

TEST_F(LifecycleTest, SignedLiteralSchemaSizeLimitsMatchParsedSchemas) {
    struct case_data {
        const char *keyword;
        const char *value_type;
        int limit;
        json invalid;
        json valid;
        bool root_bound = false;
    };
    const std::vector<case_data> cases = {
        {"minLength", "string", 3, {{"x", "a"}}, {{"x", "abc"}}},
        {"maxLength", "string", 1, {{"x", "abc"}}, {{"x", "a"}}},
        {"minItems", "array", 3, {{"x", json::array({1})}},
         {{"x", json::array({1, 2, 3})}}},
        {"maxItems", "array", 1, {{"x", json::array({1, 2, 3})}},
         {{"x", json::array({1})}}},
        {"minProperties", "object", 2, {{"x", 1}}, {{"x", 1}, {"y", 2}}, true},
        {"maxProperties", "object", 1, {{"x", 1}, {"y", 2}}, {{"x", 1}}, true},
    };
    const auto sid = initialized_session();
    ASSERT_FALSE(sid.empty());
    int id = 20;
    for (const auto &test : cases) {
        json schema = {{"type", "object"}};
        if (test.root_bound) {
            schema[test.keyword] = test.limit;
        } else {
            schema["properties"] = {{"x", {{"type", test.value_type},
                                          {test.keyword, test.limit}}}};
        }
        for (bool parsed : {false, true}) {
            SCOPED_TRACE(std::string(test.keyword) + (parsed ? " parsed" : " literal"));
            tool entry;
            entry.name = std::string(test.keyword) + (parsed ? "-parsed" : "-literal");
            entry.description = "size constraint regression";
            entry.parameters_schema = parsed ? json::parse(schema.dump()) : schema;
            int calls = 0;
            srv->register_tool(entry, [&](const json &, const std::string &) -> json {
                ++calls;
                return json::array();
            });
            const auto call = [&](const json &args) {
                auto response = cli->Post("/mcp", {{"Mcp-Session-Id", sid}},
                    rpc_request("tools/call", {{"name", entry.name}, {"arguments", args}}, id++).dump(),
                    "application/json");
                if (!response || response->status != 200) return json::object();
                return json::parse(response->body);
            };
            auto bad = call(test.invalid);
            ASSERT_TRUE(bad.contains("result"));
            EXPECT_TRUE(bad["result"].value("isError", false));
            EXPECT_EQ(0, calls);
            auto good = call(test.valid);
            ASSERT_TRUE(good.contains("result"));
            EXPECT_FALSE(good["result"].value("isError", true));
            EXPECT_EQ(1, calls);
        }
    }
}

TEST_F(LifecycleTest, StopWaitsForAcceptedLegacyCallbacksBeforeCleanup) {
    const auto sid = initialized_session();
    ASSERT_FALSE(sid.empty());
    std::promise<void> entered, release;
    auto entered_future = entered.get_future();
    auto release_future = release.get_future().share();
    std::atomic<bool> notification_ran{false};
    std::atomic<bool> cleanup_after_notification{false};
    srv->register_method("review/hold", [&](const json &, const std::string &) -> json {
        entered.set_value();
        release_future.wait();
        return json::object();
    });
    srv->register_notification("notifications/review", [&](const json &, const std::string &) {
        notification_ran = true;
    });
    srv->register_session_cleanup("review", [&](const std::string &) {
        cleanup_after_notification = notification_ran.load();
    });
    auto hold = cli->Post("/message?session_id=" + sid,
                          rpc_request("review/hold").dump(), "application/json");
    ASSERT_TRUE(hold);
    ASSERT_EQ(202, hold->status);
    ASSERT_EQ(std::future_status::ready, entered_future.wait_for(std::chrono::seconds(3)));
    auto queued = cli->Post("/message?session_id=" + sid,
                            rpc_request("notifications/review", json::object(), nullptr).dump(),
                            "application/json");
    ASSERT_TRUE(queued);
    ASSERT_EQ(202, queued->status);
    auto stopped = std::async(std::launch::async, [&] { srv->stop(); });
    const auto premature = stopped.wait_for(std::chrono::milliseconds(0));
    release.set_value();
    ASSERT_EQ(std::future_status::ready, stopped.wait_for(std::chrono::seconds(3)));
    stopped.get();
    EXPECT_EQ(std::future_status::timeout, premature);
    EXPECT_TRUE(notification_ran);
    EXPECT_TRUE(cleanup_after_notification);
    EXPECT_TRUE(srv->get_sessions().empty());
}

TEST_F(LifecycleTest, CleanupHookCanRequestStopFromHttpWorker) {
    const auto sid = initialized_session();
    ASSERT_FALSE(sid.empty());
    std::promise<void> returned;
    auto returned_future = returned.get_future();
    srv->register_session_cleanup("stop-from-hook", [&](const std::string &) {
        srv->stop();
        returned.set_value();
    });
    auto deleted = cli->Delete("/mcp", {{"Mcp-Session-Id", sid}});
    ASSERT_TRUE(deleted);
    EXPECT_EQ(200, deleted->status);
    ASSERT_EQ(std::future_status::ready, returned_future.wait_for(std::chrono::seconds(3)));
    srv->stop(); // Outside a callback this also waits for deferred shutdown.
    EXPECT_FALSE(srv->is_running());
}

TEST_F(LifecycleTest, LegacyMethodCanRequestStopFromPoolWorker) {
    const auto sid = initialized_session();
    ASSERT_FALSE(sid.empty());
    std::promise<void> returned;
    auto returned_future = returned.get_future();
    srv->register_method("review/stop", [&](const json &, const std::string &) -> json {
        srv->stop();
        returned.set_value();
        return json::object();
    });
    auto accepted = cli->Post("/message?session_id=" + sid,
                              rpc_request("review/stop").dump(), "application/json");
    ASSERT_TRUE(accepted);
    EXPECT_EQ(202, accepted->status);
    ASSERT_EQ(std::future_status::ready, returned_future.wait_for(std::chrono::seconds(3)));
    ASSERT_TRUE(server_test_access::wait_for_stop_completion(*srv));
    EXPECT_TRUE(srv->get_sessions().empty());
    ASSERT_TRUE(srv->start(false));
    EXPECT_FALSE(initialized_session().empty());
    srv->stop();
    EXPECT_FALSE(srv->is_running());
    EXPECT_TRUE(srv->get_sessions().empty());
}

#ifndef _WIN32
TEST(FileResourceTimestamp, DetectsSameSecondEditAndUnchangedFile) {
    char path[] = "/tmp/cpp-mcp-mtime-XXXXXX";
    const int fd = mkstemp(path);
    ASSERT_GE(fd, 0);
    close(fd);
    struct remove_file {
        const char *path;
        ~remove_file() { unlink(path); }
    } cleanup{path};
    {
        std::ofstream out(path);
        out << "before";
    }
    timespec times[2] = {{1712345678, 100000000}, {1712345678, 100000000}};
    ASSERT_EQ(0, utimensat(AT_FDCWD, path, times, 0));
    file_resource resource(path);
    EXPECT_EQ("before", resource.read()["text"]);
    EXPECT_FALSE(resource.is_modified());
    {
        std::ofstream out(path);
        out << "after";
    }
    times[0].tv_nsec = times[1].tv_nsec = 200000000;
    ASSERT_EQ(0, utimensat(AT_FDCWD, path, times, 0));
    EXPECT_TRUE(resource.is_modified());
    EXPECT_EQ("after", resource.read()["text"]);
    EXPECT_FALSE(resource.is_modified());
}
#endif
} // namespace
