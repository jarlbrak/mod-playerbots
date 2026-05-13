#include "LlmAgentManager.h"
#include "Client/LlmHttpClient.h"
#include "Schemas/Goal.h"

#include <chrono>
#include <filesystem>
#include <sstream>
#include <utility>

namespace {

uint64_t ms_since(std::chrono::steady_clock::time_point t0) {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - t0).count());
}

uint64_t epoch_ms() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
}

std::string truncate(const std::string& s, size_t max) {
    return s.size() <= max ? s : s.substr(0, max) + "...[truncated]";
}

std::string goal_kind_to_string(GoalKind k) {
    switch (k) {
        case GoalKind::Idle:         return "idle";
        case GoalKind::GoGrind:      return "go_grind";
        case GoalKind::GoCamp:       return "go_camp";
        case GoalKind::WanderNpc:    return "wander_npc";
        case GoalKind::WanderRandom: return "wander_random";
        case GoalKind::DoQuest:      return "do_quest";
        case GoalKind::TravelFlight: return "travel_flight";
        case GoalKind::Rest:         return "rest";
        case GoalKind::OutdoorPvp:   return "outdoor_pvp";
    }
    return "unknown";
}

nlohmann::json parsed_goal_to_json(const ParsedGoal& g) {
    return {
        {"goal", goal_kind_to_string(g.goal)},
        {"reasoning", g.reasoning},
        {"ttl_minutes", g.ttl_minutes},
    };
}

}  // namespace

LlmAgentManager& LlmAgentManager::Instance() {
    static LlmAgentManager instance;
    return instance;
}

void LlmAgentManager::Start(LlmAgentConfig cfg) {
    Shutdown();  // safe even if never started

    cfg_ = std::move(cfg);
    if (!cfg_.Enabled) return;

    // Open JSONL (parent dir created if missing).
    if (!cfg_.JsonlPath.empty()) {
        auto p = std::filesystem::path(cfg_.JsonlPath);
        if (p.has_parent_path()) {
            std::error_code ec;
            std::filesystem::create_directories(p.parent_path(), ec);
        }
        jsonl_.open(cfg_.JsonlPath, std::ios::app);
    }

    running_.store(true);
    workers_.reserve(cfg_.WorkerThreads);
    for (uint32_t i = 0; i < cfg_.WorkerThreads; ++i)
        workers_.emplace_back(&LlmAgentManager::WorkerLoop, this);
}

void LlmAgentManager::Shutdown() {
    if (!running_.exchange(false) && workers_.empty()) return;
    queue_cv_.notify_all();
    for (auto& t : workers_) if (t.joinable()) t.join();
    workers_.clear();
    if (jsonl_.is_open()) jsonl_.close();
}

bool LlmAgentManager::IsInFlight(uint64_t bot_guid) const {
    std::lock_guard<std::mutex> g(inflight_mu_);
    return inflight_.count(bot_guid) > 0;
}

bool LlmAgentManager::HasPendingResults(uint64_t bot_guid) const {
    std::lock_guard<std::mutex> g(results_mu_);
    auto it = results_.find(bot_guid);
    return it != results_.end() && !it->second.empty();
}

bool LlmAgentManager::Enqueue(LlmRequest request) {
    {
        std::lock_guard<std::mutex> g(inflight_mu_);
        if (inflight_.count(request.bot_guid)) return false;
        inflight_.insert(request.bot_guid);
    }
    request.ts_enqueued = std::chrono::steady_clock::now();
    {
        std::lock_guard<std::mutex> g(queue_mu_);
        queue_.push_back(std::move(request));
    }
    queue_cv_.notify_one();
    return true;
}

std::vector<LlmResult> LlmAgentManager::DrainResults(uint64_t bot_guid) {
    std::vector<LlmResult> out;
    std::lock_guard<std::mutex> g(results_mu_);
    auto it = results_.find(bot_guid);
    if (it == results_.end()) return out;
    while (!it->second.empty()) {
        out.push_back(std::move(it->second.top()));
        it->second.pop();
    }
    results_.erase(it);
    return out;
}

void LlmAgentManager::WorkerLoop() {
    while (running_.load()) {
        LlmRequest req;
        {
            std::unique_lock<std::mutex> lk(queue_mu_);
            queue_cv_.wait(lk, [&]{ return !running_.load() || !queue_.empty(); });
            if (!running_.load() && queue_.empty()) return;
            if (queue_.empty()) continue;
            req = std::move(queue_.front());
            queue_.pop_front();
        }
        HandleRequest(std::move(req));
    }
}

void LlmAgentManager::HandleRequest(LlmRequest req) {
    auto t_dispatch = std::chrono::steady_clock::now();
    uint64_t queue_wait_ms = ms_since(req.ts_enqueued);

    LlmHttpClient client(cfg_.Endpoint);
    auto raw = client.PostChatCompletion(
        req.body_json,
        std::chrono::milliseconds(cfg_.RequestTimeoutMs));
    uint64_t inference_ms = ms_since(t_dispatch);

    LlmResult result;
    result.bot_guid = req.bot_guid;
    result.bot_name = req.bot_name;
    result.queue_wait_ms = queue_wait_ms;
    result.inference_ms = inference_ms;
    result.total_latency_ms = queue_wait_ms + inference_ms;

    if (!raw.has_value()) {
        result.parsed_status = "transport_error";
        result.validator_error = "connect/timeout";
    } else if (raw->status < 200 || raw->status >= 300) {
        result.parsed_status = "http_error";
        result.raw_response = truncate(raw->body, 4096);
        result.validator_error = std::to_string(raw->status) + " " + raw->error;
    } else {
        // Extract choices[0].message.content
        std::string content;
        try {
            auto env = nlohmann::json::parse(raw->body);
            content = env.at("choices").at(0).at("message").at("content").get<std::string>();
        } catch (const std::exception& e) {
            result.parsed_status = "schema_error";
            result.raw_response = truncate(raw->body, 4096);
            result.validator_error = std::string{"envelope: "} + e.what();
        }
        if (result.parsed_status.empty()) {
            result.raw_response = truncate(content, 4096);
            auto parsed = ParseAndValidate(content);
            if (std::holds_alternative<ParsedGoal>(parsed)) {
                result.parsed_status = "ok";
                result.parsed_goal = parsed_goal_to_json(std::get<ParsedGoal>(parsed));
            } else {
                result.parsed_status = "schema_error";
                result.validator_error = std::get<ParseError>(parsed).message;
            }
        }
    }

    // Append JSONL line.
    nlohmann::json record;
    record["ts_completed_ms_epoch"] = epoch_ms();
    record["bot_guid"] = req.bot_guid;
    record["bot_name"] = req.bot_name;
    record["queue_wait_ms"] = result.queue_wait_ms;
    record["inference_ms"] = result.inference_ms;
    record["total_latency_ms"] = result.total_latency_ms;
    record["digest"] = req.digest_json;
    record["raw_response"] = result.raw_response;
    record["parsed_status"] = result.parsed_status;
    record["parsed_goal"] = result.parsed_goal.is_null() ? nlohmann::json(nullptr) : result.parsed_goal;
    record["validator_error"] = result.validator_error.empty() ? nlohmann::json(nullptr) : nlohmann::json(result.validator_error);

    AppendJsonl(record.dump());

    // Push to result stack.
    {
        std::lock_guard<std::mutex> g(results_mu_);
        results_[req.bot_guid].push(std::move(result));
    }
    // Clear in-flight.
    {
        std::lock_guard<std::mutex> g(inflight_mu_);
        inflight_.erase(req.bot_guid);
    }
}

void LlmAgentManager::AppendJsonl(const std::string& line) {
    std::lock_guard<std::mutex> g(jsonl_mu_);
    if (!jsonl_.is_open()) return;
    jsonl_ << line << '\n';
    jsonl_.flush();
}
