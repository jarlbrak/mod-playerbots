#include "doctest.h"
#include "Cooldown/BotCooldownMap.h"
#include <chrono>
#include <thread>

using namespace std::chrono;

TEST_CASE("BotCooldownMap unseen bot is eligible") {
    BotCooldownMap m;
    CHECK(m.Eligible(123) == true);
}

TEST_CASE("BotCooldownMap Set blocks until expiry") {
    BotCooldownMap m;
    auto until = steady_clock::now() + milliseconds(200);
    m.Set(42, until);
    CHECK(m.Eligible(42) == false);
    std::this_thread::sleep_for(milliseconds(250));
    CHECK(m.Eligible(42) == true);
}

TEST_CASE("BotCooldownMap bots are independent") {
    BotCooldownMap m;
    m.Set(1, steady_clock::now() + seconds(60));
    CHECK(m.Eligible(1) == false);
    CHECK(m.Eligible(2) == true);
}

TEST_CASE("BotCooldownMap Set overwrites earlier value") {
    BotCooldownMap m;
    m.Set(1, steady_clock::now() + seconds(60));
    m.Set(1, steady_clock::now() - seconds(1));  // already in the past
    CHECK(m.Eligible(1) == true);
}

TEST_CASE("BotCooldownMap Clear resets everything") {
    BotCooldownMap m;
    m.Set(1, steady_clock::now() + seconds(60));
    m.Set(2, steady_clock::now() + seconds(60));
    m.Clear();
    CHECK(m.Eligible(1) == true);
    CHECK(m.Eligible(2) == true);
}

TEST_CASE("BotCooldownMap returns time_until_eligible") {
    BotCooldownMap m;
    auto until = steady_clock::now() + milliseconds(500);
    m.Set(7, until);
    auto remaining = m.RemainingMs(7);
    CHECK(remaining.has_value());
    CHECK(*remaining > 100);
    CHECK(*remaining < 600);
}
