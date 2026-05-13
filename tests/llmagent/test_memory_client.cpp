#include "doctest.h"
#include "Client/MemoryHttpClient.h"
#include "Vendor/httplib.h"
#include "Vendor/nlohmann_json.hpp"

#include <atomic>
#include <chrono>
#include <thread>

namespace {

struct StubMem {
    httplib::Server svr;
    std::thread th;
    int port = 0;
    std::atomic<int> recall_about_calls{0};
    std::atomic<int> remember_calls{0};
    std::atomic<int> persona_get_calls{0};

    StubMem() {
        port = svr.bind_to_any_port("127.0.0.1");
        REQUIRE(port > 0);

        svr.Post("/memory/recall_about", [this](const httplib::Request&, httplib::Response& res) {
            recall_about_calls.fetch_add(1);
            res.set_content(R"({"hints":["a hint","b hint"]})", "application/json");
        });
        svr.Post("/memory/remember", [this](const httplib::Request&, httplib::Response& res) {
            remember_calls.fetch_add(1);
            res.set_content(R"({"memory_id":"m_abc","evicted":0})", "application/json");
        });
        svr.Post("/memory/personality/get", [this](const httplib::Request& req, httplib::Response& res) {
            persona_get_calls.fetch_add(1);
            auto j = nlohmann::json::parse(req.body);
            if (j.value("bot_id", std::string{}) == "missing") {
                res.status = 404;
                res.set_content(R"({"detail":"no persona"})", "application/json");
            } else {
                res.set_content(R"({"persona":"orc warrior"})", "application/json");
            }
        });
        svr.Post("/memory/personality/set", [](const httplib::Request&, httplib::Response& res) {
            res.set_content(R"({"ok":true})", "application/json");
        });

        th = std::thread([this]{ svr.listen_after_bind(); });
        for (int i = 0; i < 50 && !svr.is_running(); ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    ~StubMem() { svr.stop(); if (th.joinable()) th.join(); }
    std::string base_url() const { return "http://127.0.0.1:" + std::to_string(port); }
};

}  // namespace

TEST_CASE("MemoryHttpClient RecallAbout parses hints array") {
    StubMem s;
    MemoryHttpClient c(s.base_url(), std::chrono::milliseconds(2000));
    auto hints = c.RecallAbout(123, "Tarren Mill");
    CHECK(hints.size() == 2);
    CHECK(hints[0] == "a hint");
    CHECK(s.recall_about_calls.load() == 1);
}

TEST_CASE("MemoryHttpClient Remember posts and returns true on 200") {
    StubMem s;
    MemoryHttpClient c(s.base_url(), std::chrono::milliseconds(2000));
    bool ok = c.Remember(123, "killed Murloc", {"Murloc"}, 0.1);
    CHECK(ok == true);
    CHECK(s.remember_calls.load() == 1);
}

TEST_CASE("MemoryHttpClient GetPersonality returns value on 200") {
    StubMem s;
    MemoryHttpClient c(s.base_url(), std::chrono::milliseconds(2000));
    auto p = c.GetPersonality(123);
    REQUIRE(p.has_value());
    CHECK(*p == "orc warrior");
}

TEST_CASE("MemoryHttpClient SetPersonality returns true on 200") {
    StubMem s;
    MemoryHttpClient c(s.base_url(), std::chrono::milliseconds(2000));
    bool ok = c.SetPersonality(123, "test persona");
    CHECK(ok == true);
}

TEST_CASE("MemoryHttpClient Available is true when stub responds") {
    StubMem s;
    MemoryHttpClient c(s.base_url(), std::chrono::milliseconds(2000));
    c.RecallAbout(1, "x");
    CHECK(c.Available() == true);
}

TEST_CASE("MemoryHttpClient transient error sets sticky-down for 30s window") {
    MemoryHttpClient c("http://127.0.0.1:1", std::chrono::milliseconds(200));
    auto hints = c.RecallAbout(1, "x");
    CHECK(hints.empty());
    CHECK(c.Available() == false);

    auto t0 = std::chrono::steady_clock::now();
    c.RecallAbout(1, "y");
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                       std::chrono::steady_clock::now() - t0).count();
    CHECK(elapsed < 50);
}
