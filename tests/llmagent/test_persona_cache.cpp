#include "doctest.h"
#include "Chat/PersonaCache.h"

#include <optional>

namespace {
struct FakeClock {
    int64_t now = 1000;
    int64_t operator()() const { return now; }
};
}  // namespace

TEST_CASE("PersonaCache miss fetches once") {
    int fetches = 0;
    FakeClock clock;
    PersonaCache cache(
        [&](uint64_t /*guid*/) -> std::optional<std::string> {
            ++fetches;
            return std::string{"persona text"};
        },
        /*ttl_seconds*/600,
        [&]{ return clock.now; });

    CHECK(cache.Get(42) == "persona text");
    CHECK(fetches == 1);
}

TEST_CASE("PersonaCache hit avoids refetch within TTL") {
    int fetches = 0;
    FakeClock clock;
    PersonaCache cache(
        [&](uint64_t) -> std::optional<std::string> {
            ++fetches;
            return std::string{"v1"};
        },
        /*ttl*/600,
        [&]{ return clock.now; });

    CHECK(cache.Get(42) == "v1");
    clock.now += 500;
    CHECK(cache.Get(42) == "v1");
    CHECK(fetches == 1);
}

TEST_CASE("PersonaCache TTL expiry triggers refetch") {
    int fetches = 0;
    std::string current = "v1";
    FakeClock clock;
    PersonaCache cache(
        [&](uint64_t) -> std::optional<std::string> {
            ++fetches;
            return current;
        },
        /*ttl*/600,
        [&]{ return clock.now; });

    CHECK(cache.Get(42) == "v1");
    clock.now += 601;
    current = "v2";
    CHECK(cache.Get(42) == "v2");
    CHECK(fetches == 2);
}

TEST_CASE("PersonaCache fetcher returning nullopt yields empty string and caches it") {
    int fetches = 0;
    FakeClock clock;
    PersonaCache cache(
        [&](uint64_t) -> std::optional<std::string> {
            ++fetches;
            return std::nullopt;
        },
        /*ttl*/600,
        [&]{ return clock.now; });

    CHECK(cache.Get(42).empty());
    CHECK(cache.Get(42).empty());
    CHECK(fetches == 1);
}
