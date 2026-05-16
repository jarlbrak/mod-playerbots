#include "doctest.h"
#include "Selector/BotSelector.h"
#include <atomic>
#include <thread>
#include <vector>

TEST_CASE("BotSelector SamplePct=0 hits nothing") {
    BotSelector s;
    s.Configure(0, true);
    int hits = 0;
    for (uint64_t i = 1; i <= 1000; ++i) {
        if (s.IsLlmBot(i)) ++hits;
    }
    CHECK(hits == 0);
}

TEST_CASE("BotSelector SamplePct=100 hits everything") {
    BotSelector s;
    s.Configure(100, true);
    int hits = 0;
    for (uint64_t i = 1; i <= 1000; ++i) {
        if (s.IsLlmBot(i)) ++hits;
    }
    CHECK(hits == 1000);
}

TEST_CASE("BotSelector SamplePct=10 chi-square within tolerance") {
    BotSelector s;
    s.Configure(10, true);
    int hits = 0;
    constexpr int N = 10000;
    for (uint64_t i = 1; i <= N; ++i) {
        if (s.IsLlmBot(i)) ++hits;
    }
    // Expected ~1000 hits. 4-sigma tolerance ~= 120.
    CHECK(hits >= 880);
    CHECK(hits <= 1120);
}

TEST_CASE("BotSelector opt-in adds bots regardless of sample") {
    BotSelector s;
    s.Configure(0, true);
    s.OptInBot(42);
    CHECK(s.IsLlmBot(42) == true);
    CHECK(s.IsLlmBot(43) == false);
}

TEST_CASE("BotSelector opt-in is idempotent") {
    BotSelector s;
    s.Configure(0, true);
    s.OptInBot(7);
    s.OptInBot(7);
    s.OptInBot(7);
    CHECK(s.IsLlmBot(7) == true);
}

TEST_CASE("BotSelector Clear empties the opt-in set") {
    BotSelector s;
    s.Configure(0, true);
    s.OptInBot(100);
    s.OptInBot(101);
    s.Clear();
    CHECK(s.IsLlmBot(100) == false);
    CHECK(s.IsLlmBot(101) == false);
}

TEST_CASE("BotSelector OptInBot is thread-safe") {
    BotSelector s;
    s.Configure(0, true);
    constexpr int N = 4;
    constexpr int PER = 1000;
    std::vector<std::thread> threads;
    for (int t = 0; t < N; ++t) {
        threads.emplace_back([&, t]{
            for (int i = 0; i < PER; ++i) {
                s.OptInBot(static_cast<uint64_t>(t * PER + i + 1));
            }
        });
    }
    for (auto& th : threads) th.join();
    int count = 0;
    for (uint64_t i = 1; i <= N * PER; ++i) {
        if (s.IsLlmBot(i)) ++count;
    }
    CHECK(count == N * PER);
}

TEST_CASE("BotSelector SocialOptIn=false ignores opted-in bots") {
    BotSelector s;
    s.Configure(0, false);
    s.OptInBot(42);
    CHECK(s.IsLlmBot(42) == false);
}
