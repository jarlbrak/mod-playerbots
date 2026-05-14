#include "doctest.h"
#include "Tools/ToolValidators.h"
#include "Tools/InteractionContext.h"
#include "Tools/ToolCatalog.h"

namespace {

InteractionContext make_ctx() {
    InteractionContext c;
    c.bot_level = 37;
    c.map_id = 0;
    c.map_min_x = -10000; c.map_max_x = 10000;
    c.map_min_y = -10000; c.map_max_y = 10000;
    c.pending_invites = {{"RealPlayerBob", 999, 1000}};
    c.quest_log.push_back({502, /*status=incomplete*/0, /*obj_count*/3});
    c.quest_log.push_back({488, /*status=complete*/1, /*obj_count*/1});
    c.in_group = false;
    c.nearby_creatures = {{1001, "Marshal Dughan", "humanoid", /*in_range_10y*/true,
                            /*is_quest_giver_for*/502u,  /*is_turn_in_for*/0u,
                            /*is_vendor*/false}};
    c.nearby_creatures.push_back({1002, "Innkeeper Anne", "humanoid", true,
                                   0u, 488u, true});  // turn-in for 488, vendor
    return c;
}

}  // namespace

// accept_party_invite
TEST_CASE("Validate accept_party_invite happy") {
    auto r = Validate(AcceptPartyInviteCall{"RealPlayerBob"}, make_ctx());
    CHECK(r.accepted);
}
TEST_CASE("Validate accept_party_invite rejects unknown sender") {
    auto r = Validate(AcceptPartyInviteCall{"NoSuchPlayer"}, make_ctx());
    CHECK_FALSE(r.accepted);
    CHECK(r.reject_reason == "rejected_invite_from_unknown");
}
TEST_CASE("Validate accept_party_invite rejects when no pending invites") {
    InteractionContext c = make_ctx();
    c.pending_invites.clear();
    auto r = Validate(AcceptPartyInviteCall{"RealPlayerBob"}, c);
    CHECK_FALSE(r.accepted);
    CHECK(r.reject_reason == "rejected_no_pending_invite");
}

// leave_party
TEST_CASE("Validate leave_party rejects when not in group") {
    auto r = Validate(LeavePartyCall{}, make_ctx());
    CHECK_FALSE(r.accepted);
    CHECK(r.reject_reason == "rejected_not_in_group");
}
TEST_CASE("Validate leave_party happy when in group") {
    InteractionContext c = make_ctx();
    c.in_group = true;
    auto r = Validate(LeavePartyCall{}, c);
    CHECK(r.accepted);
}

// accept_quest
TEST_CASE("Validate accept_quest happy") {
    InteractionContext c = make_ctx();
    // Marshal Dughan gives quest 502; remove 502 from log so it's not already there
    c.quest_log.clear();
    c.quest_log.push_back({488, 1, 1});
    auto r = Validate(AcceptQuestCall{502, "Marshal Dughan"}, c);
    CHECK(r.accepted);
}
TEST_CASE("Validate accept_quest rejects unknown NPC") {
    auto r = Validate(AcceptQuestCall{502, "Mystery NPC"}, make_ctx());
    CHECK_FALSE(r.accepted);
    CHECK(r.reject_reason == "rejected_npc_not_in_range");
}
TEST_CASE("Validate accept_quest rejects when NPC is not quest giver for that id") {
    InteractionContext c = make_ctx();
    c.nearby_creatures[0].is_quest_giver_for = 999u;  // different quest
    c.quest_log.clear();  // remove 502 so we hit the npc check, not the already-in-log check
    auto r = Validate(AcceptQuestCall{502, "Marshal Dughan"}, c);
    CHECK_FALSE(r.accepted);
    CHECK(r.reject_reason == "rejected_npc_not_quest_giver");
}
TEST_CASE("Validate accept_quest rejects when already in log") {
    auto r = Validate(AcceptQuestCall{502, "Marshal Dughan"}, make_ctx());
    CHECK_FALSE(r.accepted);
    CHECK(r.reject_reason == "rejected_quest_already_in_log");
}

// turn_in_quest
TEST_CASE("Validate turn_in_quest happy") {
    auto r = Validate(TurnInQuestCall{488, "Innkeeper Anne"}, make_ctx());
    CHECK(r.accepted);
}
TEST_CASE("Validate turn_in_quest rejects not-in-log") {
    auto r = Validate(TurnInQuestCall{999, "Innkeeper Anne"}, make_ctx());
    CHECK_FALSE(r.accepted);
    CHECK(r.reject_reason == "rejected_quest_not_in_log");
}
TEST_CASE("Validate turn_in_quest rejects not-complete") {
    auto r = Validate(TurnInQuestCall{502, "Marshal Dughan"}, make_ctx());  // 502 is incomplete
    CHECK_FALSE(r.accepted);
    CHECK(r.reject_reason == "rejected_quest_not_complete");
}
TEST_CASE("Validate turn_in_quest rejects NPC not turn-in target") {
    auto r = Validate(TurnInQuestCall{488, "Marshal Dughan"}, make_ctx());  // Marshal doesn't take 488
    CHECK_FALSE(r.accepted);
    CHECK(r.reject_reason == "rejected_npc_not_turn_in_target");
}

// vendor_junk
TEST_CASE("Validate vendor_junk happy") {
    auto r = Validate(VendorJunkCall{"Innkeeper Anne"}, make_ctx());
    CHECK(r.accepted);
}
TEST_CASE("Validate vendor_junk rejects non-vendor") {
    auto r = Validate(VendorJunkCall{"Marshal Dughan"}, make_ctx());
    CHECK_FALSE(r.accepted);
    CHECK(r.reject_reason == "rejected_npc_not_vendor");
}
TEST_CASE("Validate vendor_junk rejects unknown NPC") {
    auto r = Validate(VendorJunkCall{"NoSuchNPC"}, make_ctx());
    CHECK_FALSE(r.accepted);
    CHECK(r.reject_reason == "rejected_npc_not_in_range");
}

// memory.remember
TEST_CASE("Validate memory.remember happy") {
    auto r = Validate(MemoryRememberCall{"text", {"x"}, 0.5, {}}, make_ctx());
    CHECK(r.accepted);
}
TEST_CASE("Validate memory.remember rejects text-too-long") {
    MemoryRememberCall c;
    c.text = std::string(600, 'x');
    c.salience = 0.5;
    auto r = Validate(c, make_ctx());
    CHECK_FALSE(r.accepted);
    CHECK(r.reject_reason == "rejected_text_too_long");
}
TEST_CASE("Validate memory.remember rejects salience out of range") {
    auto r = Validate(MemoryRememberCall{"x", {}, 1.5, {}}, make_ctx());
    CHECK_FALSE(r.accepted);
    CHECK(r.reject_reason == "rejected_salience_out_of_range");
}

// set_goal — reuses Phase 2 validator, smoke-check one case
TEST_CASE("Validate set_goal Idle is always accepted") {
    ParsedGoal g{GoalKind::Idle, IdleParams{}, "x", 5};
    auto r = Validate(SetGoalCall{g}, make_ctx());
    CHECK(r.accepted);
}
