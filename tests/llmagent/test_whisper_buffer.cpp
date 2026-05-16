#include "doctest.h"
#include "Chat/WhisperBuffer.h"

#include <thread>
#include <vector>

TEST_CASE("WhisperBuffer push then snapshot returns newest first") {
    WhisperBuffer buf;
    buf.Push(1, 2, WhisperEntry::Incoming, "hello",       1000);
    buf.Push(1, 2, WhisperEntry::Outgoing, "hi there",    1001);
    buf.Push(1, 2, WhisperEntry::Incoming, "want group?", 1002);

    auto snap = buf.SnapshotFor(1, 2, /*now*/1100, /*window*/3600, /*max_n*/10);
    REQUIRE(snap.size() == 3);
    CHECK(snap[0].text == "want group?");
    CHECK(snap[0].direction == WhisperEntry::Incoming);
    CHECK(snap[2].text == "hello");
}

TEST_CASE("WhisperBuffer SnapshotFor respects max_n") {
    WhisperBuffer buf;
    for (int i = 0; i < 10; ++i)
        buf.Push(1, 2, WhisperEntry::Incoming, "msg" + std::to_string(i), 1000 + i);

    auto snap = buf.SnapshotFor(1, 2, /*now*/2000, /*window*/3600, /*max_n*/3);
    REQUIRE(snap.size() == 3);
    CHECK(snap[0].text == "msg9");
    CHECK(snap[2].text == "msg7");
}

TEST_CASE("WhisperBuffer SnapshotFor skips entries older than window") {
    WhisperBuffer buf;
    buf.Push(1, 2, WhisperEntry::Incoming, "old",   1000);
    buf.Push(1, 2, WhisperEntry::Incoming, "fresh", 2000);

    auto snap = buf.SnapshotFor(1, 2, /*now*/2100, /*window*/200, /*max_n*/10);
    REQUIRE(snap.size() == 1);
    CHECK(snap[0].text == "fresh");
}

TEST_CASE("WhisperBuffer ExpireOlderThan drops stale globally") {
    WhisperBuffer buf;
    buf.Push(1, 2, WhisperEntry::Incoming, "very old",  500);
    buf.Push(1, 3, WhisperEntry::Incoming, "also old",  600);
    buf.Push(1, 2, WhisperEntry::Incoming, "fresh",     2000);

    buf.ExpireOlderThan(/*now*/2100, /*window*/200);
    CHECK(buf.SnapshotFor(1, 2, 2100, 3600, 10).size() == 1);
    CHECK(buf.SnapshotFor(1, 3, 2100, 3600, 10).empty());
}

TEST_CASE("WhisperBuffer Clear targets a single pair") {
    WhisperBuffer buf;
    buf.Push(1, 2, WhisperEntry::Incoming, "x", 1000);
    buf.Push(1, 3, WhisperEntry::Incoming, "y", 1000);

    buf.Clear(1, 2);
    CHECK(buf.SnapshotFor(1, 2, 2000, 3600, 10).empty());
    CHECK(buf.SnapshotFor(1, 3, 2000, 3600, 10).size() == 1);
}

TEST_CASE("WhisperBuffer ClearAll empties everything") {
    WhisperBuffer buf;
    buf.Push(1, 2, WhisperEntry::Incoming, "a", 1000);
    buf.Push(4, 5, WhisperEntry::Incoming, "b", 1000);

    buf.ClearAll();
    CHECK(buf.SnapshotFor(1, 2, 2000, 3600, 10).empty());
    CHECK(buf.SnapshotFor(4, 5, 2000, 3600, 10).empty());
}

TEST_CASE("WhisperBuffer concurrent pushes are race-free") {
    WhisperBuffer buf;
    constexpr int N = 4;
    constexpr int PER = 500;
    std::vector<std::thread> threads;
    for (int t = 0; t < N; ++t) {
        threads.emplace_back([&, t]{
            for (int i = 0; i < PER; ++i)
                buf.Push(1, 2, WhisperEntry::Incoming,
                         "msg-" + std::to_string(t) + "-" + std::to_string(i),
                         1000 + i);
        });
    }
    for (auto& th : threads) th.join();
    auto snap = buf.SnapshotFor(1, 2, /*now*/3000, /*window*/3600, /*max_n*/100000);
    CHECK(snap.size() == static_cast<size_t>(N * PER));
}
