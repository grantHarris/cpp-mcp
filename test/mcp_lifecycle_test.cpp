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
    static json process(server &srv, const request &req, const std::string &sid) {
        return srv.process_request(req, sid);
    }
    static size_t log_level_entries(server &srv) {
        std::lock_guard<std::mutex> lock(srv.mutex_);
        return srv.session_log_levels_.size();
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
        conf.threadpool_size = 2;
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
    // Calls that block until their handler returns need their own connection.
    std::future<httplib::Result> post_async(const std::string &path, const std::string &sid,
                                            const json &body) {
        return std::async(std::launch::async, [this, path, sid, body] {
            httplib::Client client(conf.host, conf.port);
            client.set_read_timeout(10);
            return client.Post(path, {{"Mcp-Session-Id", sid}}, body.dump(),
                               "application/json");
        });
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

// A size bound can arrive as a signed literal, a parsed unsigned value, or an
// integral float such as Python's json.dumps(64.0). All mean the same limit.
TEST_F(LifecycleTest, SchemaSizeLimitsAcceptEveryIntegerEncoding) {
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
        const auto schema_with = [&](const json &bound) {
            json schema = {{"type", "object"}};
            if (test.root_bound) {
                schema[test.keyword] = bound;
            } else {
                schema["properties"] = {{"x", {{"type", test.value_type},
                                              {test.keyword, bound}}}};
            }
            return schema;
        };
        const std::vector<std::pair<std::string, json>> encodings = {
            {"literal", schema_with(test.limit)},
            {"parsed", json::parse(schema_with(test.limit).dump())},
            {"float", schema_with(static_cast<double>(test.limit))},
        };
        for (const auto &[encoding, schema] : encodings) {
            SCOPED_TRACE(std::string(test.keyword) + " " + encoding);
            tool entry;
            entry.name = std::string(test.keyword) + "-" + encoding;
            entry.description = "size constraint regression";
            entry.parameters_schema = schema;
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

// stop() must not return while an accepted legacy callback is running: a host
// frees what its callbacks use as soon as stop() returns. Requests still queued
// are dropped rather than run with their side effects after shutdown began.
TEST_F(LifecycleTest, StopWaitsForRunningLegacyCallbacksAndDropsQueuedOnes) {
    // One pool worker, so the notification queues behind the held method.
    srv->stop();
    conf.threadpool_size = 1;
    srv = std::make_unique<server>(conf);
    ASSERT_TRUE(srv->start(false));
    const auto sid = initialized_session();
    ASSERT_FALSE(sid.empty());
    struct shared_state {
        std::promise<void> entered, release, hook_ran, stop_returned;
        std::atomic<bool> stop_returned_while_held{false};
        std::atomic<bool> notification_ran{false};
    };
    auto state = std::make_shared<shared_state>();
    auto entered = state->entered.get_future();
    auto hook_ran = state->hook_ran.get_future();
    auto release = state->release.get_future().share();
    auto stop_returned = state->stop_returned.get_future().share();
    srv->register_method("review/hold", [state, release, stop_returned](const json &,
                                                                        const std::string &) {
        state->entered.set_value();
        release.wait();
        // Always a timeout when stop() is correct: it cannot return while
        // this callback is still running.
        state->stop_returned_while_held =
            stop_returned.wait_for(std::chrono::milliseconds(200)) == std::future_status::ready;
        return json::object();
    });
    srv->register_notification("notifications/review", [state](const json &, const std::string &) {
        state->notification_ran = true;
    });
    srv->register_session_cleanup("review",
                                   [state](const std::string &) { state->hook_ran.set_value(); });
    std::future<void> stopped;
    struct release_on_exit {
        std::shared_ptr<shared_state> state;
        ~release_on_exit() {
            try {
                state->release.set_value();
            } catch (const std::future_error &) {
            }
        }
    } guard{state};

    auto hold = cli->Post("/message?session_id=" + sid, rpc_request("review/hold").dump(),
                          "application/json");
    ASSERT_TRUE(hold);
    ASSERT_EQ(202, hold->status);
    ASSERT_EQ(std::future_status::ready, entered.wait_for(std::chrono::seconds(3)));
    auto queued = cli->Post("/message?session_id=" + sid,
                            rpc_request("notifications/review", json::object(), nullptr).dump(),
                            "application/json");
    ASSERT_TRUE(queued);
    ASSERT_EQ(202, queued->status);
    stopped = std::async(std::launch::async, [this, state] {
        srv->stop();
        state->stop_returned.set_value();
    });
    // Cleanup hooks run before stop() waits, so a hook can cancel callbacks.
    EXPECT_EQ(std::future_status::ready, hook_ran.wait_for(std::chrono::seconds(3)));
    state->release.set_value();
    ASSERT_EQ(std::future_status::ready, stopped.wait_for(std::chrono::seconds(3)));
    EXPECT_FALSE(state->stop_returned_while_held);
    EXPECT_FALSE(state->notification_ran);
    EXPECT_TRUE(srv->get_sessions().empty());
}

// A handler may wait for its session's cleanup hook to cancel it. stop() must
// run that hook before it waits for the handler, or neither ever finishes.
TEST_F(LifecycleTest, StopRunsCleanupHooksBeforeWaitingForCallbacks) {
    const auto sid = initialized_session();
    ASSERT_FALSE(sid.empty());
    struct shared_state {
        std::promise<void> entered, cancel;
        std::atomic<bool> cancelled{false};
    };
    auto state = std::make_shared<shared_state>();
    auto entered = state->entered.get_future();
    auto cancel = state->cancel.get_future().share();
    srv->register_method("review/wait", [state, cancel](const json &, const std::string &) {
        state->entered.set_value();
        state->cancelled =
            cancel.wait_for(std::chrono::seconds(5)) == std::future_status::ready;
        return json::object();
    });
    srv->register_session_cleanup("cancel", [state](const std::string &) {
        try {
            state->cancel.set_value();
        } catch (const std::future_error &) {
        }
    });
    auto call = post_async("/mcp", sid, rpc_request("review/wait"));
    ASSERT_EQ(std::future_status::ready, entered.wait_for(std::chrono::seconds(3)));
    auto stopped = std::async(std::launch::async, [this] { srv->stop(); });
    EXPECT_EQ(std::future_status::ready, stopped.wait_for(std::chrono::seconds(3)));
    EXPECT_TRUE(state->cancelled);
}

// Once stop() closes the listener no notifications/cancelled can arrive, so
// is_cancelled() must report the shutdown itself to handlers that poll it.
TEST_F(LifecycleTest, StopCancelsCallbacksThatPollIsCancelled) {
    const auto sid = initialized_session();
    ASSERT_FALSE(sid.empty());
    struct shared_state {
        std::promise<void> entered;
        std::atomic<bool> cancelled{false};
    };
    auto state = std::make_shared<shared_state>();
    auto entered = state->entered.get_future();
    server *raw = srv.get();
    srv->register_method("review/poll", [state, raw](const json &, const std::string &session) {
        state->entered.set_value();
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (!state->cancelled && std::chrono::steady_clock::now() < deadline) {
            state->cancelled = raw->is_cancelled(2, session);
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        return json::object();
    });
    auto accepted = cli->Post("/message?session_id=" + sid,
                              rpc_request("review/poll", json::object(), 2).dump(),
                              "application/json");
    ASSERT_TRUE(accepted);
    ASSERT_EQ(202, accepted->status);
    ASSERT_EQ(std::future_status::ready, entered.wait_for(std::chrono::seconds(3)));
    auto stopped = std::async(std::launch::async, [this] { srv->stop(); });
    EXPECT_EQ(std::future_status::ready, stopped.wait_for(std::chrono::seconds(3)));
    EXPECT_TRUE(state->cancelled);
}

// start(true) serves from the caller's thread. A stop() from another thread
// must still wait for HTTP callbacks that are running.
TEST_F(LifecycleTest, BlockingStopWaitsForRunningHttpCallbacks) {
    srv->stop();
    srv = std::make_unique<server>(conf);
    struct shared_state {
        std::promise<void> entered, release;
    };
    auto state = std::make_shared<shared_state>();
    auto entered = state->entered.get_future();
    auto release = state->release.get_future().share();
    srv->register_method("review/hold", [state, release](const json &, const std::string &) {
        state->entered.set_value();
        release.wait();
        return json::object();
    });
    auto serving = std::async(std::launch::async, [this] { return srv->start(true); });
    std::future<void> stopped;
    std::future<httplib::Result> call;
    struct stop_on_exit {
        server &srv;
        std::shared_ptr<shared_state> state;
        ~stop_on_exit() {
            try {
                state->release.set_value();
            } catch (const std::future_error &) {
            }
            srv.stop();
        }
    } guard{*srv, state};

    // Nothing signals when a blocking start() is accepting, so retry.
    std::string sid;
    for (int attempt = 0; attempt < 100 && sid.empty(); ++attempt) {
        sid = initialized_session();
        if (sid.empty()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
    }
    ASSERT_FALSE(sid.empty());
    call = post_async("/mcp", sid, rpc_request("review/hold"));
    ASSERT_EQ(std::future_status::ready, entered.wait_for(std::chrono::seconds(3)));
    stopped = std::async(std::launch::async, [this] { srv->stop(); });
    EXPECT_EQ(std::future_status::timeout, stopped.wait_for(std::chrono::milliseconds(300)));
    state->release.set_value();
    ASSERT_EQ(std::future_status::ready, stopped.wait_for(std::chrono::seconds(3)));
    ASSERT_EQ(std::future_status::ready, serving.wait_for(std::chrono::seconds(3)));
    EXPECT_TRUE(serving.get());
}

TEST_F(LifecycleTest, CleanupHookCanRequestStopFromHttpWorker) {
    const auto sid = initialized_session();
    ASSERT_FALSE(sid.empty());
    auto returned = std::make_shared<std::promise<void>>();
    auto returned_future = returned->get_future();
    srv->register_session_cleanup("stop-from-hook", [this, returned](const std::string &) {
        srv->stop();
        try {
            returned->set_value();
        } catch (const std::future_error &) {
        }
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
    auto returned = std::make_shared<std::promise<bool>>();
    auto returned_future = returned->get_future();
    srv->register_method("review/stop", [this, returned](const json &, const std::string &) {
        srv->stop();
        // The deferred shutdown is still running, but the server is already
        // no longer running from the caller's point of view.
        returned->set_value(srv->is_running());
        return json::object();
    });
    auto accepted = cli->Post("/message?session_id=" + sid,
                              rpc_request("review/stop").dump(), "application/json");
    ASSERT_TRUE(accepted);
    EXPECT_EQ(202, accepted->status);
    ASSERT_EQ(std::future_status::ready, returned_future.wait_for(std::chrono::seconds(3)));
    EXPECT_FALSE(returned_future.get());
    ASSERT_TRUE(server_test_access::wait_for_stop_completion(*srv));
    EXPECT_TRUE(srv->get_sessions().empty());
    ASSERT_TRUE(srv->start(false));
    EXPECT_FALSE(initialized_session().empty());
    srv->stop();
    EXPECT_FALSE(srv->is_running());
    EXPECT_TRUE(srv->get_sessions().empty());
}

// A session whose only traffic is an open GET stream is in use. With the 30 s
// keepalive and 30 s timeout defaults, the reaper closed it whenever a tick
// fell between the keepalive deadline and the stream's next activity update.
TEST_F(LifecycleTest, OpenGetStreamKeepsIdleSessionAlive) {
    srv->stop();
    conf.session_timeout = 1;
    srv = std::make_unique<server>(conf);
    ASSERT_TRUE(srv->start(false));
    auto reaped = std::make_shared<std::promise<void>>();
    auto reaped_future = reaped->get_future().share();
    srv->register_session_cleanup("reaped", [reaped](const std::string &) {
        try {
            reaped->set_value();
        } catch (const std::future_error &) {
        }
    });
    const auto sid = initialized_session();
    ASSERT_FALSE(sid.empty());
    httplib::Client stream(conf.host, conf.port);
    stream.set_read_timeout(60);
    auto get = std::async(std::launch::async, [&] {
        return stream.Get("/mcp", {{"Mcp-Session-Id", sid}, {"Accept", "text/event-stream"}},
                          [](const char *, size_t) { return true; });
    });
    // Far longer than the 1 s timeout, far shorter than the 30 s keepalive.
    EXPECT_EQ(std::future_status::timeout, reaped_future.wait_for(std::chrono::seconds(3)));
    stream.stop();
    get.wait();
    // With no stream the session is idle again, and it must still expire.
    EXPECT_EQ(std::future_status::ready, reaped_future.wait_for(std::chrono::seconds(5)));
}

// Cancellations are kept only for running requests, and only while they run,
// so a stream of cancels for made-up ids cannot grow the server's memory.
TEST_F(LifecycleTest, CancelMarksOnlyRunningRequests) {
    const auto sid = initialized_session();
    ASSERT_FALSE(sid.empty());
    struct shared_state {
        std::promise<void> entered, release;
        std::atomic<bool> cancelled{false};
    };
    auto state = std::make_shared<shared_state>();
    auto entered = state->entered.get_future();
    auto release = state->release.get_future().share();
    server *raw = srv.get();
    srv->register_method("review/hold", [state, release, raw](const json &,
                                                              const std::string &session) {
        state->entered.set_value();
        release.wait();
        state->cancelled = raw->is_cancelled(7, session);
        return json::object();
    });
    const auto cancel = [&](int id) {
        auto res = cli->Post("/mcp", {{"Mcp-Session-Id", sid}},
                             rpc_request("notifications/cancelled", {{"requestId", id}}, nullptr)
                                 .dump(),
                             "application/json");
        return res ? res->status : 0;
    };
    for (int id = 100; id < 200; ++id) {
        ASSERT_EQ(202, cancel(id));
    }
    EXPECT_FALSE(srv->is_cancelled(150, sid));

    auto call = post_async("/mcp", sid, rpc_request("review/hold", json::object(), 7));
    ASSERT_EQ(std::future_status::ready, entered.wait_for(std::chrono::seconds(3)));
    EXPECT_FALSE(srv->is_cancelled(7, sid));
    EXPECT_EQ(202, cancel(7));
    EXPECT_TRUE(srv->is_cancelled(7, sid));
    state->release.set_value();
    ASSERT_EQ(std::future_status::ready, call.wait_for(std::chrono::seconds(3)));
    EXPECT_TRUE(state->cancelled);
    EXPECT_FALSE(srv->is_cancelled(7, sid)); // Finished requests are forgotten.
}

// A logging/setLevel that lands after its session closed must not recreate
// per-session state that nothing will ever remove.
TEST_F(LifecycleTest, SetLevelForClosedSessionLeavesNoState) {
    const auto before = server_test_access::log_level_entries(*srv);
    request req;
    req.jsonrpc = "2.0";
    req.id = 1;
    req.method = "logging/setLevel";
    req.params = {{"level", "debug"}};
    server_test_access::process(*srv, req, "closed-session");
    EXPECT_EQ(before, server_test_access::log_level_entries(*srv));
}

// Reads a legacy /sse stream on its own thread.
struct legacy_stream {
    httplib::Client client;
    std::mutex m;
    std::condition_variable cv;
    std::string received;
    bool done = false;
    std::thread reader;

    legacy_stream(const std::string &host, int port) : client(host, port) {
        client.set_read_timeout(10);
        reader = std::thread([this] {
            client.Get("/sse", [this](const char *data, size_t n) {
                std::lock_guard<std::mutex> lock(m);
                received.append(data, n);
                cv.notify_all();
                return !done;
            });
            std::lock_guard<std::mutex> lock(m);
            done = true;
            cv.notify_all();
        });
    }
    ~legacy_stream() {
        {
            std::lock_guard<std::mutex> lock(m);
            done = true;
        }
        client.stop();
        reader.join();
    }
    bool wait_for(const std::string &text) {
        std::unique_lock<std::mutex> lock(m);
        return cv.wait_for(lock, std::chrono::seconds(3),
                           [&] { return received.find(text) != std::string::npos; });
    }
    std::string endpoint() {
        if (!wait_for("session_id=")) {
            return {};
        }
        std::lock_guard<std::mutex> lock(m);
        const auto start = received.find("/message?");
        return received.substr(start, received.find('\r', start) - start);
    }
};

// Handler results with invalid UTF-8 must still reach the client. Serializing
// them threw: Streamable HTTP answered 500 and legacy SSE never answered.
TEST_F(LifecycleTest, InvalidUtf8InResultIsReplacedNotDropped) {
    srv->register_method("review/bytes", [](const json &, const std::string &) {
        return json{{"text", std::string("bad \xff byte")}};
    });
    const auto sid = initialized_session();
    ASSERT_FALSE(sid.empty());
    auto res = cli->Post("/mcp", {{"Mcp-Session-Id", sid}},
                         rpc_request("review/bytes", json::object(), 5).dump(), "application/json");
    ASSERT_TRUE(res);
    EXPECT_EQ(200, res->status);
    EXPECT_NE(std::string::npos, res->body.find("bad \xEF\xBF\xBD byte"));

    legacy_stream sse(conf.host, conf.port);
    const auto endpoint = sse.endpoint();
    ASSERT_FALSE(endpoint.empty());
    const auto post = [&](const json &body) {
        auto r = cli->Post(endpoint, body.dump(), "application/json");
        return r ? r->status : 0;
    };
    ASSERT_EQ(202, post(initialize_request()));
    ASSERT_TRUE(sse.wait_for("\"id\":1"));
    ASSERT_EQ(202, post(rpc_request("notifications/initialized", json::object(), nullptr)));
    ASSERT_EQ(202, post(rpc_request("review/bytes", json::object(), 6)));
    EXPECT_TRUE(sse.wait_for("\"id\":6"));
}

void destroy_server_from_its_own_callback() {
    server::configuration conf;
    conf.host = "127.0.0.1";
    conf.port = 9590;
    conf.session_timeout = 0;
    auto *srv = new server(conf);
    srv->register_method("review/destroy", [srv](const json &, const std::string &) {
        delete srv;
        return json::object();
    });
    if (!srv->start(false)) {
        return;
    }
    httplib::Client cli(conf.host, conf.port);
    auto init = cli.Post("/mcp", initialize_request().dump(), "application/json");
    if (!init) {
        return;
    }
    const httplib::Headers session = {
        {"Mcp-Session-Id", init->get_header_value("Mcp-Session-Id")}};
    cli.Post("/mcp", session,
             rpc_request("notifications/initialized", json::object(), nullptr).dump(),
             "application/json");
    cli.Post("/mcp", session, rpc_request("review/destroy").dump(), "application/json");
}

// Destruction joins every server thread, so it cannot run on one of them.
// Abort with the reason rather than free state those threads still use.
TEST(LifecycleDeathTest, DestroyingServerFromItsOwnCallbackAborts) {
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    EXPECT_DEATH(destroy_server_from_its_own_callback(), "destroyed from one of its own threads");
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
