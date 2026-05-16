#include "doctest.h"
#include "EventBuffer/RecentEventBuffer.h"

TEST_CASE("RecentEventBuffer empty bot returns empty vector") {
    RecentEventBuffer b;
    b.Configure(20);
    auto v = b.Snapshot(1);
    CHECK(v.empty());
}

TEST_CASE("RecentEventBuffer preserves insertion order") {
    RecentEventBuffer b;
    b.Configure(20);
    b.Push(1, "a");
    b.Push(1, "b");
    b.Push(1, "c");
    auto v = b.Snapshot(1);
    REQUIRE(v.size() == 3);
    CHECK(v[0] == "a");
    CHECK(v[1] == "b");
    CHECK(v[2] == "c");
}

TEST_CASE("RecentEventBuffer drops oldest when capacity exceeded") {
    RecentEventBuffer b;
    b.Configure(3);
    b.Push(1, "a");
    b.Push(1, "b");
    b.Push(1, "c");
    b.Push(1, "d");
    auto v = b.Snapshot(1);
    REQUIRE(v.size() == 3);
    CHECK(v[0] == "b");
    CHECK(v[1] == "c");
    CHECK(v[2] == "d");
}

TEST_CASE("RecentEventBuffer is per-bot") {
    RecentEventBuffer b;
    b.Configure(10);
    b.Push(1, "alpha");
    b.Push(2, "beta");
    auto v1 = b.Snapshot(1);
    auto v2 = b.Snapshot(2);
    REQUIRE(v1.size() == 1);
    REQUIRE(v2.size() == 1);
    CHECK(v1[0] == "alpha");
    CHECK(v2[0] == "beta");
}

TEST_CASE("RecentEventBuffer Clear removes bot's history") {
    RecentEventBuffer b;
    b.Configure(10);
    b.Push(1, "alpha");
    b.Clear(1);
    CHECK(b.Snapshot(1).empty());
}

TEST_CASE("RecentEventBuffer Configure 0 disables") {
    RecentEventBuffer b;
    b.Configure(0);
    b.Push(1, "alpha");
    CHECK(b.Snapshot(1).empty());
}
