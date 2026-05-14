#include "doctest.h"
#include "Tools/ToolCatalog.h"
#include "Vendor/nlohmann_json.hpp"
#include <algorithm>

TEST_CASE("kToolsJsonSchema is a non-empty JSON array") {
    auto j = nlohmann::json::parse(kToolsJsonSchema);
    REQUIRE(j.is_array());
    CHECK(j.size() == 7);
}

TEST_CASE("kToolsJsonSchema contains expected tool names") {
    auto j = nlohmann::json::parse(kToolsJsonSchema);
    std::vector<std::string> names;
    for (const auto& t : j) names.push_back(t["function"]["name"].get<std::string>());
    CHECK(std::find(names.begin(), names.end(), "accept_party_invite") != names.end());
    CHECK(std::find(names.begin(), names.end(), "leave_party")         != names.end());
    CHECK(std::find(names.begin(), names.end(), "accept_quest")        != names.end());
    CHECK(std::find(names.begin(), names.end(), "turn_in_quest")       != names.end());
    CHECK(std::find(names.begin(), names.end(), "set_goal")            != names.end());
    CHECK(std::find(names.begin(), names.end(), "vendor_junk")         != names.end());
    CHECK(std::find(names.begin(), names.end(), "memory.remember")     != names.end());
}

TEST_CASE("kToolsJsonSchema each tool has function.parameters.type==object") {
    auto j = nlohmann::json::parse(kToolsJsonSchema);
    for (const auto& t : j) {
        REQUIRE(t.contains("function"));
        REQUIRE(t["function"].contains("parameters"));
        CHECK(t["function"]["parameters"].value("type", std::string{}) == "object");
    }
}

TEST_CASE("accept_party_invite requires 'from'") {
    auto j = nlohmann::json::parse(kToolsJsonSchema);
    for (const auto& t : j) {
        if (t["function"]["name"] == "accept_party_invite") {
            auto req = t["function"]["parameters"].value("required", nlohmann::json::array());
            bool found = false;
            for (const auto& r : req) if (r == "from") found = true;
            CHECK(found);
            return;
        }
    }
    FAIL("accept_party_invite missing from catalog");
}

TEST_CASE("leave_party has no required params") {
    auto j = nlohmann::json::parse(kToolsJsonSchema);
    for (const auto& t : j) {
        if (t["function"]["name"] == "leave_party") {
            auto req = t["function"]["parameters"].value("required", nlohmann::json::array());
            CHECK(req.empty());
            return;
        }
    }
    FAIL("leave_party missing from catalog");
}

TEST_CASE("memory.remember has text/entities/salience required") {
    auto j = nlohmann::json::parse(kToolsJsonSchema);
    for (const auto& t : j) {
        if (t["function"]["name"] == "memory.remember") {
            auto req = t["function"]["parameters"].value("required", nlohmann::json::array());
            bool ftext=false, fent=false, fsal=false;
            for (const auto& r : req) {
                if (r == "text") ftext = true;
                if (r == "entities") fent = true;
                if (r == "salience") fsal = true;
            }
            CHECK(ftext); CHECK(fent); CHECK(fsal);
            return;
        }
    }
    FAIL("memory.remember missing from catalog");
}

TEST_CASE("kT3OutputSchema parses and constrains utterance + side_effects") {
    auto j = nlohmann::json::parse(kT3OutputSchema);
    REQUIRE(j["type"] == "object");
    auto req = j["required"];
    bool has_utt = false, has_sfx = false;
    for (const auto& r : req) {
        if (r == "utterance")    has_utt = true;
        if (r == "side_effects") has_sfx = true;
    }
    CHECK(has_utt);
    CHECK(has_sfx);
    CHECK(j["properties"]["utterance"]["maxLength"] == 200);
    CHECK(j["properties"]["side_effects"]["maxItems"] == 3);
}
