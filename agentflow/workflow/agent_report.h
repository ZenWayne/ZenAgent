#ifndef AGENTFLOW_WORKFLOW_AGENT_REPORT_H_
#define AGENTFLOW_WORKFLOW_AGENT_REPORT_H_

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include <nlohmann/json.hpp>

namespace agentflow::workflow {

// Generic parent-bound report produced by a delegated session after an
// automatic (event-driven) turn finishes. This is deliberately free of any
// application/tool names: the service routes it; the application decides what
// the parent should say about it.
struct AgentReport {
  std::string event_id;
  std::string from_session_id;
  std::string from_agent;
  int from_depth = 0;
  std::string to_session_id;
  std::string to_agent;
  int to_depth = 0;
  std::string goal;
  std::string outcome;  // "completed" or "failed"
  std::string summary;
  int hop = 1;
  std::string created_at;
};

inline constexpr std::size_t kAgentReportMaxEventIdBytes = 128;
inline constexpr std::size_t kAgentReportMaxSessionIdBytes = 256;
inline constexpr std::size_t kAgentReportMaxAgentNameBytes = 128;
inline constexpr std::size_t kAgentReportMaxGoalBytes = 4096;
inline constexpr std::size_t kAgentReportMaxSummaryBytes = 8192;
inline constexpr int kAgentReportMaxHop = 16;
inline constexpr std::string_view kAgentReportType = "agent_report";

inline bool IsValidAgentReportOutcome(std::string_view outcome) {
  return outcome == "completed" || outcome == "failed";
}

namespace agent_report_internal {

inline std::size_t CutUtf8(std::string_view text, std::size_t max_bytes) {
  if (text.size() <= max_bytes) return text.size();
  std::size_t cut = max_bytes;
  // Never cut in the middle of a UTF-8 sequence. Back up from a continuation
  // byte to the lead byte that starts it, then cut before that code point.
  while (cut > 0 &&
         (static_cast<unsigned char>(text[cut]) & 0xC0) == 0x80) {
    --cut;
  }
  return cut;
}

inline std::string Bounded(std::string_view text, std::size_t max_bytes) {
  return std::string(text.substr(0, CutUtf8(text, max_bytes)));
}

inline bool ReadString(const nlohmann::json& object, const char* key,
                       std::string* out, std::size_t max_bytes,
                       bool required) {
  const auto it = object.find(key);
  if (it == object.end()) return !required;
  if (!it->is_string()) return false;
  const std::string& value = it->get_ref<const std::string&>();
  if (value.size() > max_bytes) return false;
  if (required && value.empty()) return false;
  *out = value;
  return true;
}

inline bool ReadDepth(const nlohmann::json& object, const char* key,
                      int* out) {
  const auto it = object.find(key);
  if (it == object.end()) {
    *out = 0;
    return true;
  }
  if (!it->is_number_integer()) return false;
  const auto value = it->get<int64_t>();
  if (value < 0 || value > kAgentReportMaxHop) return false;
  *out = static_cast<int>(value);
  return true;
}

}  // namespace agent_report_internal

// Encodes a report. Returns an empty string when the envelope is invalid;
// callers must treat that as a bug in the producer, not as a transport error.
inline std::string EncodeAgentReport(
    const AgentReport& report,
    std::size_t max_summary_bytes = kAgentReportMaxSummaryBytes) {
  if (report.event_id.empty() ||
      report.event_id.size() > kAgentReportMaxEventIdBytes ||
      report.from_session_id.empty() ||
      report.from_session_id.size() > kAgentReportMaxSessionIdBytes ||
      report.from_agent.empty() ||
      report.from_agent.size() > kAgentReportMaxAgentNameBytes ||
      report.to_session_id.empty() ||
      report.to_session_id.size() > kAgentReportMaxSessionIdBytes ||
      report.to_agent.empty() ||
      report.to_agent.size() > kAgentReportMaxAgentNameBytes ||
      !IsValidAgentReportOutcome(report.outcome) ||
      report.hop < 1 || report.hop > kAgentReportMaxHop ||
      report.from_depth < 0 || report.from_depth > kAgentReportMaxHop ||
      report.to_depth < 0 || report.to_depth > kAgentReportMaxHop) {
    return {};
  }
  nlohmann::ordered_json from = {
      {"session_id", report.from_session_id},
      {"agent_name", report.from_agent},
      {"delegate_depth", report.from_depth},
  };
  nlohmann::ordered_json to = {
      {"session_id", report.to_session_id},
      {"agent_name", report.to_agent},
      {"delegate_depth", report.to_depth},
  };
  const std::size_t summary_limit =
      max_summary_bytes < kAgentReportMaxSummaryBytes ? max_summary_bytes
                                                      : kAgentReportMaxSummaryBytes;
  nlohmann::ordered_json json = {
      {"type", std::string(kAgentReportType)},
      {"event_id", report.event_id},
      {"from", std::move(from)},
      {"to", std::move(to)},
      {"goal", agent_report_internal::Bounded(
                   report.goal, kAgentReportMaxGoalBytes)},
      {"outcome", report.outcome},
      {"summary", agent_report_internal::Bounded(report.summary, summary_limit)},
      {"hop", report.hop},
      {"created_at", report.created_at},
  };
  return json.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
}

inline std::optional<AgentReport> DecodeAgentReport(
    std::string_view json_text) {
  const auto parsed = nlohmann::json::parse(json_text, nullptr, false);
  if (parsed.is_discarded() || !parsed.is_object()) return std::nullopt;
  const auto type = parsed.find("type");
  if (type == parsed.end() || !type->is_string() ||
      type->get_ref<const std::string&>() != std::string(kAgentReportType)) {
    return std::nullopt;
  }
  const auto from_it = parsed.find("from");
  const auto to_it = parsed.find("to");
  if (from_it == parsed.end() || !from_it->is_object() ||
      to_it == parsed.end() || !to_it->is_object()) {
    return std::nullopt;
  }
  AgentReport report;
  if (!agent_report_internal::ReadString(
          parsed, "event_id", &report.event_id,
          kAgentReportMaxEventIdBytes, true) ||
      !agent_report_internal::ReadString(
          parsed, "goal", &report.goal, kAgentReportMaxGoalBytes, false) ||
      !agent_report_internal::ReadString(
          parsed, "outcome", &report.outcome,
          kAgentReportMaxAgentNameBytes, true) ||
      !agent_report_internal::ReadString(
          parsed, "summary", &report.summary,
          kAgentReportMaxSummaryBytes, false) ||
      !agent_report_internal::ReadString(
          parsed, "created_at", &report.created_at,
          kAgentReportMaxSessionIdBytes, false) ||
      !agent_report_internal::ReadString(
          *from_it, "session_id", &report.from_session_id,
          kAgentReportMaxSessionIdBytes, true) ||
      !agent_report_internal::ReadString(
          *from_it, "agent_name", &report.from_agent,
          kAgentReportMaxAgentNameBytes, true) ||
      !agent_report_internal::ReadString(
          *to_it, "session_id", &report.to_session_id,
          kAgentReportMaxSessionIdBytes, true) ||
      !agent_report_internal::ReadString(
          *to_it, "agent_name", &report.to_agent,
          kAgentReportMaxAgentNameBytes, true) ||
      !agent_report_internal::ReadDepth(*from_it, "delegate_depth",
                                        &report.from_depth) ||
      !agent_report_internal::ReadDepth(*to_it, "delegate_depth",
                                        &report.to_depth)) {
    return std::nullopt;
  }
  if (!IsValidAgentReportOutcome(report.outcome)) return std::nullopt;
  const auto hop_it = parsed.find("hop");
  if (hop_it == parsed.end() || !hop_it->is_number_integer()) {
    return std::nullopt;
  }
  const auto hop = hop_it->get<int64_t>();
  if (hop < 1 || hop > kAgentReportMaxHop) return std::nullopt;
  report.hop = static_cast<int>(hop);
  return report;
}

}  // namespace agentflow::workflow

#endif  // AGENTFLOW_WORKFLOW_AGENT_REPORT_H_
