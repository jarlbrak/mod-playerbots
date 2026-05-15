#include "LlmAgentManager.h"
#include "Client/LlmHttpClient.h"
#include "Schemas/Goal.h"

#ifndef LLMAGENT_UNIT_TESTS
#include "Log.h"
#endif

#include <chrono>
#include <ctime>
#include <filesystem>
#include <optional>
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
#ifndef LLMAGENT_UNIT_TESTS
    LOG_INFO("server.loading",
             "[LlmAgent] Start: Enabled={} JsonlPath='{}' WorkerThreads={} Tier3.Enabled={} SamplePct={}",
             cfg_.Enabled ? 1 : 0, cfg_.JsonlPath, cfg_.WorkerThreads,
             cfg_.Tier3_Enabled ? 1 : 0, cfg_.SamplePct);
#endif
    // Phase 2 component config
    selector_.Configure(cfg_.SamplePct, cfg_.SocialOptIn);
    events_.Configure(cfg_.EventLogSize);

    memory_client_ = std::make_unique<MemoryHttpClient>(
        cfg_.MemorySidecar_Endpoint,
        std::chrono::milliseconds(cfg_.MemorySidecar_RequestTimeoutMs));

#ifndef LLMAGENT_UNIT_TESTS
    persona_ = std::make_unique<PersonaCache>(
        [this](uint64_t guid) -> std::optional<std::string> {
            return memory_client_->GetPersonality(guid);
        },
        static_cast<int64_t>(cfg_.Tier3_PersonaCacheTtlSeconds),
        []{ return static_cast<int64_t>(time(nullptr)); });
#endif

    if (!cfg_.Enabled) {
#ifndef LLMAGENT_UNIT_TESTS
        LOG_INFO("server.loading", "[LlmAgent] Start: skipped — Enabled=0");
#endif
        return;
    }

    // Open JSONL (parent dir created if missing).
    if (!cfg_.JsonlPath.empty()) {
        auto p = std::filesystem::path(cfg_.JsonlPath);
        if (p.has_parent_path()) {
            std::error_code ec;
            std::filesystem::create_directories(p.parent_path(), ec);
        }
        jsonl_.open(cfg_.JsonlPath, std::ios::app);
#ifndef LLMAGENT_UNIT_TESTS
        LOG_INFO("server.loading", "[LlmAgent] Start: jsonl_open='{}' is_open={}",
                 cfg_.JsonlPath, jsonl_.is_open() ? 1 : 0);
#endif
    }

    running_.store(true);
    workers_.reserve(cfg_.WorkerThreads);
    for (uint32_t i = 0; i < cfg_.WorkerThreads; ++i)
        workers_.emplace_back(&LlmAgentManager::WorkerLoop, this);
#ifndef LLMAGENT_UNIT_TESTS
    LOG_INFO("server.loading", "[LlmAgent] Start: ready — workers={}", workers_.size());
#endif
}

void LlmAgentManager::Shutdown() {
    if (!running_.exchange(false) && workers_.empty()) return;
    queue_cv_.notify_all();
    for (auto& t : workers_) if (t.joinable()) t.join();
    workers_.clear();
    counters_.DumpToLog();
    selector_.Clear();
    cooldowns_.Clear();
    t2_cooldowns_.Clear();
    events_.ClearAll();
    interactions_.ClearAll();
    whispers_.ClearAll();
    t3_cooldowns_.Clear();
#ifndef LLMAGENT_UNIT_TESTS
    if (persona_) persona_->ClearAll();
#endif
    if (jsonl_.is_open()) jsonl_.close();
}

bool LlmAgentManager::IsInFlight(uint64_t bot_guid, uint32_t tier) const {
    std::lock_guard<std::mutex> g(inflight_mu_);
    auto it = inflight_.find(bot_guid);
    return it != inflight_.end() && it->second.count(tier) > 0;
}

bool LlmAgentManager::HasPendingResults(uint64_t bot_guid, uint32_t tier) const {
    std::lock_guard<std::mutex> g(results_mu_);
    auto it = results_.find(bot_guid);
    if (it == results_.end()) return false;
    auto it2 = it->second.find(tier);
    return it2 != it->second.end() && !it2->second.empty();
}

bool LlmAgentManager::Enqueue(LlmRequest request) {
    {
        std::lock_guard<std::mutex> g(inflight_mu_);
        auto& tiers = inflight_[request.bot_guid];
        if (tiers.count(request.tier)) return false;
        tiers.insert(request.tier);
    }
    counters_.IncEnqueued();
    request.ts_enqueued = std::chrono::steady_clock::now();
    {
        std::lock_guard<std::mutex> g(queue_mu_);
        queue_.push_back(std::move(request));
    }
    queue_cv_.notify_one();
    return true;
}

std::vector<LlmResult> LlmAgentManager::DrainResults(uint64_t bot_guid, uint32_t tier) {
    std::vector<LlmResult> out;
    std::lock_guard<std::mutex> g(results_mu_);
    auto it = results_.find(bot_guid);
    if (it == results_.end()) return out;
    auto it2 = it->second.find(tier);
    if (it2 == it->second.end()) return out;
    while (!it2->second.empty()) {
        out.push_back(std::move(it2->second.top()));
        it2->second.pop();
    }
    it->second.erase(it2);
    if (it->second.empty()) results_.erase(it);
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
            // Tier 1 (goal replan): worker validates against Phase 1 goal schema.
            // Tier 2 (interactive): worker leaves the raw response unparsed —
            //   LlmInteractAction calls ParseToolCalls on the array of {name,
            //   arguments} elements. T2 success/schema_error is recorded there.
            if (req.tier == 1) {
                auto parsed = ParseAndValidate(content);
                if (std::holds_alternative<ParsedGoal>(parsed)) {
                    result.parsed_status = "ok";
                    result.parsed_goal = parsed_goal_to_json(std::get<ParsedGoal>(parsed));
                } else {
                    result.parsed_status = "schema_error";
                    result.validator_error = std::get<ParseError>(parsed).message;
                }
            } else {
                result.parsed_status = "ok";  // tool-call shape verified downstream
            }
        }
    }

    counters_.IncStatus(result.parsed_status);

    // Append JSONL line.
    nlohmann::json record;
    record["ts_completed_ms_epoch"] = epoch_ms();
    record["tier"] = req.tier;
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

    // Propagate tier so consumers can filter by it.
    result.tier = req.tier;
    // Push to result stack.
    {
        std::lock_guard<std::mutex> g(results_mu_);
        results_[req.bot_guid][req.tier].push(std::move(result));
    }
    // Clear in-flight.
    {
        std::lock_guard<std::mutex> g(inflight_mu_);
        auto it = inflight_.find(req.bot_guid);
        if (it != inflight_.end()) {
            it->second.erase(req.tier);
            if (it->second.empty()) inflight_.erase(it);
        }
    }
}

void LlmAgentManager::AppendJsonl(const std::string& line) {
    std::lock_guard<std::mutex> g(jsonl_mu_);
    if (!jsonl_.is_open()) return;
    jsonl_ << line << '\n';
    jsonl_.flush();
}
