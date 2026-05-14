#include "doctest.h"
#include "EventBuffer/InteractionEventBuffer.h"

TEST_CASE("InteractionEventBuffer empty bot has no pending events") {
    InteractionEventBuffer b;
    CHECK(b.HasPending(123) == false);
    auto p = b.SnapshotFor(123);
    CHECK(p.pending_invites.empty());
    CHECK(p.recent_whispers.empty());
    CHECK(p.recent_group_joins.empty());
}

TEST_CASE("InteractionEventBuffer PushWhisper makes HasPending true") {
    InteractionEventBuffer b;
    b.PushWhisper(123, "RealPlayerBob", 999, "hi there", 1000);
    CHECK(b.HasPending(123) == true);
    auto p = b.SnapshotFor(123);
    REQUIRE(p.recent_whispers.size() == 1);
    CHECK(p.recent_whispers[0].from_name == "RealPlayerBob");
    CHECK(p.recent_whispers[0].text == "hi there");
    CHECK(p.recent_whispers[0].ts == 1000);
}

TEST_CASE("InteractionEventBuffer PushInvite + PushJoin both flag pending") {
    InteractionEventBuffer b;
    b.PushInvite(1, "Alice", 100, 5000);
    CHECK(b.HasPending(1));
    b.PushJoin(2, "Bob", 101, 5000);
    CHECK(b.HasPending(2));
}

TEST_CASE("InteractionEventBuffer ExpireOlderThan drops stale whispers") {
    InteractionEventBuffer b;
    b.PushWhisper(1, "A", 100, "old",   1000);
    b.PushWhisper(1, "B", 101, "fresh", 2000);
    // cutoff = now - window = 3000 - 1500 = 1500; old(1000) < 1500 drops, fresh(2000) >= 1500 survives
    b.ExpireOlderThan(1, /*now=*/3000, /*window_seconds=*/1500);
    auto p = b.SnapshotFor(1);
    REQUIRE(p.recent_whispers.size() == 1);
    CHECK(p.recent_whispers[0].text == "fresh");
}

TEST_CASE("InteractionEventBuffer is per-bot") {
    InteractionEventBuffer b;
    b.PushWhisper(1, "X", 1, "a", 1);
    b.PushWhisper(2, "Y", 2, "b", 1);
    CHECK(b.SnapshotFor(1).recent_whispers.size() == 1);
    CHECK(b.SnapshotFor(2).recent_whispers.size() == 1);
    CHECK(b.SnapshotFor(3).recent_whispers.empty());
}

TEST_CASE("InteractionEventBuffer Clear removes a bot's events") {
    InteractionEventBuffer b;
    b.PushWhisper(1, "X", 1, "a", 1);
    b.PushInvite(1, "Y", 2, 1);
    b.Clear(1);
    CHECK(b.HasPending(1) == false);
}
