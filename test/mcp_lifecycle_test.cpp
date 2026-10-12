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
