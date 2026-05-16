#include "doctest.h"
#include "LlmAgentManager.h"
#include "Vendor/httplib.h"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <thread>

namespace {

struct StubServer {
    httplib::Server svr;
    std::thread th;
    int port = 0;
    std::atomic<int> hit_count{0};
    std::chrono::milliseconds sleep{0};

    StubServer() {
        port = svr.bind_to_any_port("127.0.0.1");
        REQUIRE(port > 0);
        svr.Post("/v1/chat/completions", [this](const httplib::Request&, httplib::Response& res) {
            hit_count.fetch_add(1);
            if (sleep.count() > 0) std::this_thread::sleep_for(sleep);
            res.set_content(R"({"choices":[{"message":{"content":"{\"goal\":\"rest\",\"params\":{},\"reasoning\":\"x\",\"ttl_minutes\":5}"}}]})", "application/json");
        });
        th = std::thread([this]{ svr.listen_after_bind(); });
        for (int i = 0; i < 50 && !svr.is_running(); ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    ~StubServer() { svr.stop(); if (th.joinable()) th.join(); }
    std::string base_url() const { return "http://127.0.0.1:" + std::to_string(port); }
};

LlmAgentConfig test_cfg(const std::string& url, const std::string& jsonl_path) {
    LlmAgentConfig c;
    c.Enabled = true;
    c.Endpoint = url;
    c.WorkerThreads = 4;
    c.RequestTimeoutMs = 2000;
    c.JsonlPath = jsonl_path;
    c.SystemPrompt = "test";
    return c;
}

}  // namespace

TEST_CASE("LlmAgentManager processes 100 requests with 4 workers") {
    StubServer srv;
    auto tmpdir = std::filesystem::temp_directory_path() / "llmagent_test_t1";
    std::filesystem::create_directories(tmpdir);
    auto jsonl = (tmpdir / "out.jsonl").string();
    if (std::filesystem::exists(jsonl)) std::filesystem::remove(jsonl);

    LlmAgentManager mgr;
    mgr.Start(test_cfg(srv.base_url(), jsonl));

    for (int i = 0; i < 100; ++i) {
        LlmRequest req;
        req.bot_guid = static_cast<uint64_t>(i + 1);
        req.bot_name = "bot" + std::to_string(i);
        req.body_json = R"({"model":"test","messages":[]})";
        req.digest_json = nlohmann::json::object();
        mgr.Enqueue(std::move(req));
    }

    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (srv.hit_count.load() < 100 && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(20));

    mgr.Shutdown();
    CHECK(srv.hit_count.load() == 100);
}

TEST_CASE("LlmAgentManager in-flight cap rejects second enqueue for same bot") {
    StubServer srv;
    srv.sleep = std::chrono::milliseconds(300);  // slow enough to overlap
    auto tmpdir = std::filesystem::temp_directory_path() / "llmagent_test_t2";
    std::filesystem::create_directories(tmpdir);
    auto jsonl = (tmpdir / "out.jsonl").string();

    LlmAgentManager mgr;
    mgr.Start(test_cfg(srv.base_url(), jsonl));

    LlmRequest req1;
    req1.bot_guid = 42;
    req1.bot_name = "Grimblade";
    req1.body_json = "{}";
    req1.digest_json = nlohmann::json::object();
    CHECK(mgr.Enqueue(req1) == true);

    LlmRequest req2 = req1;
    CHECK(mgr.Enqueue(std::move(req2)) == false);  // already in-flight

    std::this_thread::sleep_for(std::chrono::milliseconds(600));
    CHECK(srv.hit_count.load() == 1);

    mgr.Shutdown();
}

TEST_CASE("LlmAgentManager DrainResults is per-bot and clears stack") {
    StubServer srv;
    auto tmpdir = std::filesystem::temp_directory_path() / "llmagent_test_t3";
    std::filesystem::create_directories(tmpdir);
    auto jsonl = (tmpdir / "out.jsonl").string();

    LlmAgentManager mgr;
    mgr.Start(test_cfg(srv.base_url(), jsonl));

    for (int i = 0; i < 5; ++i) {
        LlmRequest req;
        req.bot_guid = 100 + i;
        req.bot_name = "bot" + std::to_string(i);
        req.body_json = "{}";
        req.digest_json = nlohmann::json::object();
        mgr.Enqueue(std::move(req));
    }

    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (srv.hit_count.load() < 5 && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(20));

    for (int i = 0; i < 5; ++i) {
        auto results = mgr.DrainResults(100 + i);
        CHECK(results.size() == 1);
        CHECK(results[0].bot_guid == static_cast<uint64_t>(100 + i));
    }
    auto empty = mgr.DrainResults(999);
    CHECK(empty.empty());

    mgr.Shutdown();
}

TEST_CASE("LlmAgentManager writes one JSONL line per response") {
    StubServer srv;
    auto tmpdir = std::filesystem::temp_directory_path() / "llmagent_test_t4";
    std::filesystem::create_directories(tmpdir);
    auto jsonl = (tmpdir / "out.jsonl").string();
    if (std::filesystem::exists(jsonl)) std::filesystem::remove(jsonl);

    LlmAgentManager mgr;
    mgr.Start(test_cfg(srv.base_url(), jsonl));

    for (int i = 0; i < 10; ++i) {
        LlmRequest req;
        req.bot_guid = 200 + i;
        req.bot_name = "n";
        req.body_json = "{}";
        req.digest_json = nlohmann::json::object();
        mgr.Enqueue(std::move(req));
    }

    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (srv.hit_count.load() < 10 && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(20));

    mgr.Shutdown();

    std::ifstream f(jsonl);
    int lines = 0;
    std::string line;
    while (std::getline(f, line)) {
        ++lines;
        auto j = nlohmann::json::parse(line);
        CHECK(j.contains("bot_guid"));
        CHECK(j.contains("inference_ms"));
        CHECK(j.contains("parsed_status"));
    }
    CHECK(lines == 10);
}

TEST_CASE("LlmAgentManager records transport_error on bad endpoint") {
    auto tmpdir = std::filesystem::temp_directory_path() / "llmagent_test_t5";
    std::filesystem::create_directories(tmpdir);
    auto jsonl = (tmpdir / "out.jsonl").string();
    if (std::filesystem::exists(jsonl)) std::filesystem::remove(jsonl);

    LlmAgentManager mgr;
    mgr.Start(test_cfg("http://127.0.0.1:1", jsonl));  // unreachable

    LlmRequest req;
    req.bot_guid = 1;
    req.bot_name = "n";
    req.body_json = "{}";
    req.digest_json = nlohmann::json::object();
    mgr.Enqueue(std::move(req));

    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    auto results = mgr.DrainResults(1);
    REQUIRE(results.size() == 1);
    CHECK(results[0].parsed_status == "transport_error");

    mgr.Shutdown();
}

TEST_CASE("LlmAgentManager Enqueue prioritizes tier-3 ahead of tier-1") {
    // Phase 5.1: chat (T3) is user-facing; replan (T1) is background.
    // With 1 worker and a slow stub, T3 should jump in front of any
    // pending T1 in the queue.
    StubServer srv;
    srv.sleep = std::chrono::milliseconds(150);
    auto tmpdir = std::filesystem::temp_directory_path() / "llmagent_test_prio";
    std::filesystem::create_directories(tmpdir);
    auto jsonl = (tmpdir / "out.jsonl").string();
    if (std::filesystem::exists(jsonl)) std::filesystem::remove(jsonl);

    LlmAgentConfig cfg = test_cfg(srv.base_url(), jsonl);
    cfg.WorkerThreads = 1;  // serialize dispatch
    LlmAgentManager mgr;
    mgr.Start(cfg);

    auto mk = [](uint64_t g, uint32_t tier) {
        LlmRequest r;
        r.bot_guid = g;
        r.bot_name = std::to_string(g);
        r.body_json = "{}";
        r.digest_json = nlohmann::json::object();
        r.tier = tier;
        return r;
    };

    // T1 bot=1 enqueued first → worker picks it up immediately (queue empty).
    REQUIRE(mgr.Enqueue(mk(1, 1)));
    // Give the worker time to dequeue and start sleeping in the stub.
    std::this_thread::sleep_for(std::chrono::milliseconds(30));

    // Now enqueue T1 bot=2, T3 bot=3, T1 bot=4 in that order.
    // Without priority: queue = [2, 3, 4] → completion order = [1,2,3,4]
    // With priority (tier-3 jumps to front): queue = [3, 2, 4] → [1,3,2,4]
    REQUIRE(mgr.Enqueue(mk(2, 1)));
    REQUIRE(mgr.Enqueue(mk(3, 3)));
    REQUIRE(mgr.Enqueue(mk(4, 1)));

    // Wait for all 4 to be processed.
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (srv.hit_count.load() < 4 && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    REQUIRE(srv.hit_count.load() == 4);
    mgr.Shutdown();

    // Read JSONL and extract bot_guids in completion order.
    std::ifstream f(jsonl);
    std::vector<uint64_t> order;
    std::string line;
    while (std::getline(f, line)) {
        auto j = nlohmann::json::parse(line);
        order.push_back(j.at("bot_guid").get<uint64_t>());
    }
    REQUIRE(order.size() == 4);
    CHECK(order[0] == 1);  // T1 bot=1 — already in-flight before others enqueued
    CHECK(order[1] == 3);  // T3 jumps queue
    CHECK(order[2] == 2);  // then remaining T1s in FIFO
    CHECK(order[3] == 4);
}

TEST_CASE("LlmAgentManager Shutdown is idempotent") {
    LlmAgentManager mgr;
    LlmAgentConfig cfg;
    cfg.Enabled = false;
    mgr.Start(cfg);
    mgr.Shutdown();
    mgr.Shutdown();  // second call must be a no-op
    CHECK(true);     // reaching here proves no crash
}

TEST_CASE("LlmAgentManager Cooldowns().Set blocks Eligible()") {
    LlmAgentManager mgr;
    LlmAgentConfig cfg;
    cfg.Enabled = false;
    mgr.Start(cfg);
    mgr.Cooldowns().Set(42, std::chrono::steady_clock::now() + std::chrono::seconds(60));
    CHECK(mgr.Cooldowns().Eligible(42) == false);
    CHECK(mgr.Cooldowns().Eligible(43) == true);
    mgr.Shutdown();
}

TEST_CASE("LlmAgentManager Selector().Configure + OptInBot interplay") {
    LlmAgentManager mgr;
    LlmAgentConfig cfg;
    cfg.Enabled = false;
    cfg.SamplePct = 0;
    cfg.SocialOptIn = true;
    mgr.Start(cfg);
    CHECK(mgr.Selector().IsLlmBot(100) == false);
    mgr.Selector().OptInBot(100);
    CHECK(mgr.Selector().IsLlmBot(100) == true);
    mgr.Shutdown();
}
