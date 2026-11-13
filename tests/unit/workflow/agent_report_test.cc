#include "agentflow/workflow/agent_report.h"

#include <gtest/gtest.h>

#include <string>

namespace agentflow::workflow {
namespace {

AgentReport Sample() {
  AgentReport report;
  report.event_id = "agent-report:child:async-1";
  report.from_session_id = "delegate:child";
  report.from_agent = "worker-agent";
  report.from_depth = 1;
  report.to_session_id = "session:parent";
  report.to_agent = "lead-agent";
  report.to_depth = 0;
  report.goal = "prepare the next artifact";
  report.outcome = "completed";
  report.summary = "The child finished the requested step.";
  report.hop = 1;
  report.created_at = "2026-11-08T00:00:00Z";
  return report;
}

std::string ValidJson() {
  return R"({"type":"agent_report","event_id":"agent-report:child:async-1",)"
         R"("from":{"session_id":"delegate:child","agent_name":"worker-agent","delegate_depth":1},)"
         R"("to":{"session_id":"session:parent","agent_name":"lead-agent","delegate_depth":0},)"
         R"("goal":"prepare the next artifact","outcome":"completed",)"
         R"("summary":"done","hop":1,"created_at":"2026-11-08T00:00:00Z"})";
}

TEST(AgentReportTest, RoundTripsGenericEnvelope) {
  const AgentReport report = Sample();
  const std::string encoded = EncodeAgentReport(report);
  ASSERT_FALSE(encoded.empty());
  const auto decoded = DecodeAgentReport(encoded);
  ASSERT_TRUE(decoded.has_value());
  EXPECT_EQ(decoded->event_id, report.event_id);
  EXPECT_EQ(decoded->from_session_id, report.from_session_id);
  EXPECT_EQ(decoded->from_agent, report.from_agent);
  EXPECT_EQ(decoded->from_depth, report.from_depth);
  EXPECT_EQ(decoded->to_session_id, report.to_session_id);
  EXPECT_EQ(decoded->to_agent, report.to_agent);
  EXPECT_EQ(decoded->to_depth, report.to_depth);
  EXPECT_EQ(decoded->goal, report.goal);
  EXPECT_EQ(decoded->outcome, report.outcome);
  EXPECT_EQ(decoded->summary, report.summary);
  EXPECT_EQ(decoded->hop, report.hop);
  EXPECT_EQ(decoded->created_at, report.created_at);
}

TEST(AgentReportTest, TruncatesSummaryWithoutSplittingUtf8) {
  AgentReport report = Sample();
  report.summary = "ab";
  report.summary += "\xE4\xB8\xAD";  // U+4E2D, three UTF-8 bytes.
  const std::string encoded = EncodeAgentReport(report, 4);
  ASSERT_FALSE(encoded.empty());
  const auto decoded = DecodeAgentReport(encoded);
  ASSERT_TRUE(decoded.has_value());
  EXPECT_EQ(decoded->summary, "ab");
}

TEST(AgentReportTest, TruncatesLongSummaryToConfiguredLimit) {
  AgentReport report = Sample();
  report.summary.assign(9000, 'x');
  const std::string encoded = EncodeAgentReport(report, 2000);
  ASSERT_FALSE(encoded.empty());
  const auto decoded = DecodeAgentReport(encoded);
  ASSERT_TRUE(decoded.has_value());
  EXPECT_EQ(decoded->summary.size(), 2000u);
}

TEST(AgentReportTest, RejectsUnknownOutcomeAndMissingType) {
  EXPECT_TRUE(EncodeAgentReport([] {
                AgentReport report = Sample();
                report.outcome = "running";
                return report;
              }()).empty());
  EXPECT_FALSE(DecodeAgentReport(R"({"event_id":"x","from":{},"to":{},"outcome":"completed","hop":1})")
                   .has_value());
}

TEST(AgentReportTest, RejectsOutOfBoundsIdentity) {
  AgentReport report = Sample();
  report.event_id.assign(kAgentReportMaxEventIdBytes + 1, 'x');
  EXPECT_TRUE(EncodeAgentReport(report).empty());

  report = Sample();
  report.hop = 0;
  EXPECT_TRUE(EncodeAgentReport(report).empty());
}

TEST(AgentReportTest, DecodesValidJsonWithoutUnknownFields) {
  const auto decoded = DecodeAgentReport(ValidJson());
  ASSERT_TRUE(decoded.has_value());
  EXPECT_EQ(decoded->outcome, "completed");
  EXPECT_EQ(decoded->summary, "done");
}

TEST(AgentReportTest, RejectsInvalidHopAndDepth) {
  std::string json = ValidJson();
  json.replace(json.find("\"hop\":1"), 7, "\"hop\":0");
  EXPECT_FALSE(DecodeAgentReport(json).has_value());

  json = ValidJson();
  json.replace(json.find("\"delegate_depth\":1"), 18,
               "\"delegate_depth\":99");
  EXPECT_FALSE(DecodeAgentReport(json).has_value());
}

}  // namespace
}  // namespace agentflow::workflow
