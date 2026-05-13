#include "doctest.h"
#include "Client/LlmHttpClient.h"
#include "Vendor/httplib.h"
#include <thread>
#include <atomic>
#include <chrono>

namespace {

struct StubServer {
    httplib::Server svr;
    std::thread th;
    int port = 0;

    StubServer() {
        port = svr.bind_to_any_port("127.0.0.1");
        REQUIRE(port > 0);
        th = std::thread([this]{ svr.listen_after_bind(); });
        // tiny wait for the server thread to be ready
        for (int i = 0; i < 50 && !svr.is_running(); ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    ~StubServer() {
        svr.stop();
        if (th.joinable()) th.join();
    }
    std::string base_url() const { return "http://127.0.0.1:" + std::to_string(port); }
};

}  // namespace

TEST_CASE("LlmHttpClient returns 200 body unchanged") {
    StubServer srv;
    srv.svr.Post("/v1/chat/completions", [](const httplib::Request& req, httplib::Response& res) {
        res.set_content(R"({"choices":[{"message":{"content":"ok"}}]})", "application/json");
    });

    LlmHttpClient client(srv.base_url());
    auto result = client.PostChatCompletion(R"({"x":1})", std::chrono::milliseconds(2000));
    REQUIRE(result.has_value());
    CHECK(result->status == 200);
    CHECK(result->body.find("choices") != std::string::npos);
    CHECK(result->error.empty());
}

TEST_CASE("LlmHttpClient surfaces HTTP non-2xx") {
    StubServer srv;
    srv.svr.Post("/v1/chat/completions", [](const httplib::Request& req, httplib::Response& res) {
        res.status = 500;
        res.set_content("boom", "text/plain");
    });

    LlmHttpClient client(srv.base_url());
    auto result = client.PostChatCompletion(R"({"x":1})", std::chrono::milliseconds(2000));
    REQUIRE(result.has_value());
    CHECK(result->status == 500);
    CHECK(result->body == "boom");
}

TEST_CASE("LlmHttpClient returns nullopt on connect error") {
    // Port 1 is reserved / unbound; expect connection refused.
    LlmHttpClient client("http://127.0.0.1:1");
    auto result = client.PostChatCompletion(R"({"x":1})", std::chrono::milliseconds(500));
    CHECK(!result.has_value());
}

TEST_CASE("LlmHttpClient honors read timeout") {
    StubServer srv;
    srv.svr.Post("/v1/chat/completions", [](const httplib::Request& req, httplib::Response& res) {
        std::this_thread::sleep_for(std::chrono::milliseconds(800));
        res.set_content("late", "text/plain");
    });

    LlmHttpClient client(srv.base_url());
    auto t0 = std::chrono::steady_clock::now();
    auto result = client.PostChatCompletion(R"({"x":1})", std::chrono::milliseconds(200));
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                       std::chrono::steady_clock::now() - t0).count();
    CHECK(!result.has_value());
    CHECK(elapsed < 600);  // timeout took effect well before the 800ms response
}

TEST_CASE("LlmHttpClient posts body unchanged") {
    StubServer srv;
    std::atomic<bool> got_body{false};
    std::string received;
    srv.svr.Post("/v1/chat/completions", [&](const httplib::Request& req, httplib::Response& res) {
        received = req.body;
        got_body.store(true);
        res.set_content(R"({"ok":true})", "application/json");
    });

    LlmHttpClient client(srv.base_url());
    const std::string body = R"({"model":"qwen","messages":[]})";
    auto result = client.PostChatCompletion(body, std::chrono::milliseconds(2000));
    REQUIRE(result.has_value());
    CHECK(got_body.load());
    CHECK(received == body);
}
