#include "Client/LlmHttpClient.h"
#include "Vendor/httplib.h"

#include <stdexcept>

namespace {

struct ParsedEndpoint {
    std::string host;
    int port = 80;
};

ParsedEndpoint parse_endpoint(const std::string& url) {
    // Accepts "http://host[:port]"; strips scheme; defaults port to 80.
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

}  // namespace

LlmHttpClient::LlmHttpClient(std::string endpoint) : endpoint_(std::move(endpoint)) {}

std::optional<RawResponse> LlmHttpClient::PostChatCompletion(
    const std::string& body_json,
    std::chrono::milliseconds timeout)
{
    ParsedEndpoint ep = parse_endpoint(endpoint_);
    httplib::Client cli(ep.host, ep.port);
    cli.set_connection_timeout(timeout);
    cli.set_read_timeout(timeout);
    cli.set_write_timeout(timeout);

    auto res = cli.Post("/v1/chat/completions", body_json, "application/json");
    if (!res) return std::nullopt;  // transport or timeout

    RawResponse out;
    out.status = res->status;
    out.body = res->body;
    if (res->status < 200 || res->status >= 300) out.error = res->reason;
    return out;
}
