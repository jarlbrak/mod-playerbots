#include "Client/MemoryHttpClient.h"
#include "Vendor/httplib.h"
#include "Vendor/nlohmann_json.hpp"

#include <chrono>
#include <string>

namespace {

constexpr int64_t kStickyDownMs = 30000;

struct ParsedEndpoint {
    std::string host;
    int port = 80;
};

ParsedEndpoint parse_endpoint(const std::string& url) {
    std::string s = url;
    constexpr const char* kPrefix = "http://";
    if (s.rfind(kPrefix, 0) == 0) s = s.substr(std::char_traits<char>::length(kPrefix));
    ParsedEndpoint out;
    auto colon = s.find(':');
    if (colon == std::string::npos) {
        out.host = s;
    } else {
        out.host = s.substr(0, colon);
        try { out.port = std::stoi(s.substr(colon + 1)); } catch (...) { out.port = 80; }
    }
    return out;
}

int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch()).count();
}

}  // namespace

MemoryHttpClient::MemoryHttpClient(std::string endpoint, std::chrono::milliseconds timeout)
    : endpoint_(std::move(endpoint)), timeout_(timeout) {}

bool MemoryHttpClient::ShortCircuitBecauseDown() const {
    int64_t until = down_until_ms_.load();
    return until > 0 && now_ms() < until;
}

void MemoryHttpClient::MarkDown() {
    down_until_ms_.store(now_ms() + kStickyDownMs);
}

void MemoryHttpClient::MarkUp() {
    down_until_ms_.store(0);
}

bool MemoryHttpClient::Available() const {
    return !ShortCircuitBecauseDown();
}

std::vector<std::string> MemoryHttpClient::RecallAbout(
    uint64_t bot_guid, const std::string& entity, uint32_t max_hops, uint32_t top_k)
{
    if (ShortCircuitBecauseDown()) return {};

    nlohmann::json body = {
        {"bot_id",   std::to_string(bot_guid)},
        {"entity",   entity},
        {"max_hops", max_hops},
        {"top_k",    top_k},
    };
    auto ep = parse_endpoint(endpoint_);
    httplib::Client cli(ep.host, ep.port);
    cli.set_connection_timeout(timeout_);
    cli.set_read_timeout(timeout_);

    auto res = cli.Post("/memory/recall_about", body.dump(), "application/json");
    if (!res) { MarkDown(); return {}; }
    if (res->status >= 200 && res->status < 300) {
        MarkUp();
        try {
            auto j = nlohmann::json::parse(res->body);
            std::vector<std::string> hints;
            for (const auto& h : j.value("hints", nlohmann::json::array())) {
                hints.push_back(h.get<std::string>());
            }
            return hints;
        } catch (...) { return {}; }
    }
    return {};
}

bool MemoryHttpClient::Remember(uint64_t bot_guid, const std::string& text,
                                 const std::vector<std::string>& entities,
                                 double salience,
                                 const std::vector<std::tuple<std::string, std::string, std::string>>& relations)
{
    if (ShortCircuitBecauseDown()) return false;

    nlohmann::json rels = nlohmann::json::array();
    for (const auto& [src, rel, dst] : relations) {
        rels.push_back({{"src", src}, {"rel", rel}, {"dst", dst}});
    }
    nlohmann::json body = {
        {"bot_id",    std::to_string(bot_guid)},
        {"text",      text},
        {"entities",  entities},
        {"salience",  salience},
        {"relations", rels},
    };

    auto ep = parse_endpoint(endpoint_);
    httplib::Client cli(ep.host, ep.port);
    cli.set_connection_timeout(timeout_);
    cli.set_read_timeout(timeout_);

    auto res = cli.Post("/memory/remember", body.dump(), "application/json");
    if (!res) { MarkDown(); return false; }
    if (res->status >= 200 && res->status < 300) { MarkUp(); return true; }
    return false;
}

std::optional<std::string> MemoryHttpClient::GetPersonality(uint64_t bot_guid) {
    if (ShortCircuitBecauseDown()) return std::nullopt;

    nlohmann::json body = {{"bot_id", std::to_string(bot_guid)}};
    auto ep = parse_endpoint(endpoint_);
    httplib::Client cli(ep.host, ep.port);
    cli.set_connection_timeout(timeout_);
    cli.set_read_timeout(timeout_);

    auto res = cli.Post("/memory/personality/get", body.dump(), "application/json");
    if (!res) { MarkDown(); return std::nullopt; }
    if (res->status == 404) { MarkUp(); return std::nullopt; }
    if (res->status >= 200 && res->status < 300) {
        MarkUp();
        try {
            auto j = nlohmann::json::parse(res->body);
            return j.value("persona", std::string{});
        } catch (...) { return std::nullopt; }
    }
    return std::nullopt;
}

bool MemoryHttpClient::SetPersonality(uint64_t bot_guid, const std::string& persona) {
    if (ShortCircuitBecauseDown()) return false;

    nlohmann::json body = {
        {"bot_id",  std::to_string(bot_guid)},
        {"persona", persona},
    };
    auto ep = parse_endpoint(endpoint_);
    httplib::Client cli(ep.host, ep.port);
    cli.set_connection_timeout(timeout_);
    cli.set_read_timeout(timeout_);

    auto res = cli.Post("/memory/personality/set", body.dump(), "application/json");
    if (!res) { MarkDown(); return false; }
    if (res->status >= 200 && res->status < 300) { MarkUp(); return true; }
    return false;
}
