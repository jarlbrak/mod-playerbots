#ifndef _PLAYERBOT_LLMAGENT_HTTP_CLIENT_H
#define _PLAYERBOT_LLMAGENT_HTTP_CLIENT_H

#include <chrono>
#include <optional>
#include <string>

struct RawResponse {
    int         status = 0;
    std::string body;
    std::string error;  // populated when status != 200
};

class LlmHttpClient {
  public:
    explicit LlmHttpClient(std::string endpoint);  // e.g. "http://127.0.0.1:8080"

    std::optional<RawResponse> PostChatCompletion(
        const std::string& body_json,
        std::chrono::milliseconds timeout);

  private:
    std::string endpoint_;  // host[:port], no path, no scheme stripped at construction
};

#endif  // _PLAYERBOT_LLMAGENT_HTTP_CLIENT_H
