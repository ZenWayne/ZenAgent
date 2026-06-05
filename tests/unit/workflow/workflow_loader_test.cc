#include "agentflow/workflow/workflow_loader.h"

#include <functional>
#include <map>
#include <string>
#include <string_view>

#include <gtest/gtest.h>
#include <absl/strings/escaping.h>
#include <absl/strings/match.h>
#include <asio/io_context.hpp>
#include <google/protobuf/descriptor.h>
#include <google/protobuf/descriptor.pb.h>
#include <nlohmann/json.hpp>

#include "agentflow/core/state.h"
#include "agentflow/tools/tool_registry.h"
#include "agentflow/workflow/hmac_sha256.h"
#include "test_messages.pb.h"

namespace agentflow::workflow {
namespace {

constexpr char kMinimalJson[] = R"({
  "schema_version": 1,
  "name": "test_wf",
  "version": "v1",
  "state": {
    "kind": "dynamic_json",
    "fields": { "user_query": {"type":"string"} }
  },
  "agents": {
    "chat": {
      "system_prompt": "be brief",
      "model": {"max_output_tokens": 128, "constrained_tool_calls": false},
      "tools": []
    }
  },
  "main": "chat"
})";

TEST(WorkflowLoaderTest, LoadMinimalJson) {
  asio::io_context io;
  ToolRegistry host_tools(io);
  auto wf_or = WorkflowLoader::Load(kMinimalJson, host_tools);
  ASSERT_TRUE(wf_or.ok()) << wf_or.status();
  EXPECT_EQ((*wf_or)->name(), "test_wf");
  EXPECT_EQ((*wf_or)->version(), "v1");
}

TEST(WorkflowLoaderTest, RejectMissingMain) {
  std::string bad = R"({
    "schema_version": 1,
    "name": "x", "version": "v1",
    "state": {"kind":"dynamic_json","fields":{}},
    "agents": {"a": {"system_prompt":"", "model":{}, "tools":[]}},
    "main": "ghost"
  })";
  asio::io_context io;
  ToolRegistry host_tools(io);
  auto wf_or = WorkflowLoader::Load(bad, host_tools);
  EXPECT_FALSE(wf_or.ok());
  EXPECT_TRUE(absl::StrContains(wf_or.status().message(), "main") ||
              absl::StrContains(wf_or.status().message(), "ghost"));
}

TEST(WorkflowLoaderTest, RejectUnknownToolReference) {
  std::string bad = R"({
    "schema_version": 1,
    "name":"x","version":"v1",
    "state":{"kind":"dynamic_json","fields":{}},
    "agents":{"a":{"system_prompt":"","model":{},"tools":["does_not_exist"]}},
    "main":"a"
  })";
  asio::io_context io;
  ToolRegistry host_tools(io);
  auto wf_or = WorkflowLoader::Load(bad, host_tools);
  EXPECT_FALSE(wf_or.ok());
  EXPECT_TRUE(absl::StrContains(wf_or.status().message(), "does_not_exist"));
}

TEST(WorkflowLoaderTest, RejectFutureSchemaVersion) {
  std::string bad = R"({
    "schema_version": 999,
    "name":"x","version":"v1",
    "state":{"kind":"dynamic_json","fields":{}},
    "agents":{"a":{"system_prompt":"","model":{},"tools":[]}},
    "main":"a"
  })";
  asio::io_context io;
  ToolRegistry host_tools(io);
  EXPECT_FALSE(WorkflowLoader::Load(bad, host_tools).ok());
}

TEST(WorkflowLoaderTest, RejectMalformedJson) {
  asio::io_context io;
  ToolRegistry host_tools(io);
  EXPECT_FALSE(WorkflowLoader::Load("{not json", host_tools).ok());
}

TEST(WorkflowLoaderTest, RejectTemplateReferencingUnknownStateField) {
  std::string bad = R"({
    "schema_version": 1,
    "name":"x","version":"v1",
    "state":{"kind":"dynamic_json","fields":{"only_field":{"type":"string"}}},
    "agents":{
      "a":{"system_prompt":"","model":{},"tools":[],
           "delegates":{"agents":["b"],"max_depth":2,
                         "input_template":{"u":"{{state.does_not_exist}}"}}},
      "b":{"system_prompt":"","model":{},"tools":[]}
    },
    "main":"a"
  })";
  asio::io_context io;
  ToolRegistry host_tools(io);
  auto wf_or = WorkflowLoader::Load(bad, host_tools);
  EXPECT_FALSE(wf_or.ok());
  EXPECT_TRUE(absl::StrContains(wf_or.status().message(), "does_not_exist"));
}

TEST(WorkflowLoaderTest, RequireSignedRejectsUnsigned) {
  asio::io_context io;
  ToolRegistry host_tools(io);
  WorkflowLoader::Options opts;
  opts.require_signed = true;
  auto wf_or = WorkflowLoader::Load(kMinimalJson, host_tools, opts);
  EXPECT_FALSE(wf_or.ok());
  EXPECT_TRUE(absl::StrContains(wf_or.status().message(), "signature"));
}

TEST(WorkflowLoaderTest, AcceptValidSignature) {
  // Minimal spec; we compute the canonical form, sign it, embed the b64 sig,
  // then load with a KeyResolver that returns the same key. Avoids the
  // brittleness of a hand-precomputed HMAC literal.
  //
  // CanonicalForm strips the "signing" block, so we compute against the
  // unsigned skeleton, then splice "signing" back in.
  std::string skeleton = R"({
    "schema_version":1,"name":"x","version":"v1",
    "state":{"kind":"dynamic_json","fields":{}},
    "agents":{"a":{"system_prompt":"","model":{},"tools":[]}},
    "main":"a"
  })";
  nlohmann::ordered_json parsed = nlohmann::ordered_json::parse(skeleton);
  // Canonical form (sorted keys, no whitespace, signing stripped):
  std::function<nlohmann::json(const nlohmann::ordered_json&,bool)> canon =
      [&](const nlohmann::ordered_json& v, bool root)->nlohmann::json{
    if (v.is_object()) {
      std::map<std::string,nlohmann::json> s;
      for (auto it=v.begin(); it!=v.end(); ++it){
        if (root && it.key()=="signing") continue;
        s[it.key()] = canon(it.value(), false);
      }
      nlohmann::json o = nlohmann::json::object();
      for (auto& [k,val]:s) o[k]=std::move(val);
      return o;
    }
    if (v.is_array()) {
      nlohmann::json o = nlohmann::json::array();
      for (const auto& el : v) o.push_back(canon(el,false));
      return o;
    }
    return v;
  };
  std::string canonical = canon(parsed, true).dump();
  std::string sig = HmacSha256Base64("supersecret", canonical);

  nlohmann::ordered_json signed_json = parsed;
  nlohmann::ordered_json sblock;
  sblock["algo"] = "HMAC-SHA256";
  sblock["key_id"] = "k";
  sblock["signature"] = sig;
  signed_json["signing"] = sblock;

  class FixedResolver : public KeyResolver {
   public:
    absl::StatusOr<std::string> Resolve(std::string_view) override {
      return std::string("supersecret");
    }
  } resolver;

  asio::io_context io;
  ToolRegistry host_tools(io);
  WorkflowLoader::Options opts;
  opts.key_resolver = &resolver;
  auto wf_or = WorkflowLoader::Load(signed_json.dump(), host_tools, opts);
  EXPECT_TRUE(wf_or.ok()) << wf_or.status();
}

TEST(WorkflowLoaderTest, BadSignatureRejected) {
  nlohmann::ordered_json j = nlohmann::ordered_json::parse(R"({
    "schema_version":1,"name":"x","version":"v1",
    "state":{"kind":"dynamic_json","fields":{}},
    "agents":{"a":{"system_prompt":"","model":{},"tools":[]}},
    "main":"a",
    "signing":{"algo":"HMAC-SHA256","key_id":"k","signature":"AAAA"}
  })");
  class FixedResolver : public KeyResolver {
   public:
    absl::StatusOr<std::string> Resolve(std::string_view) override {
      return std::string("supersecret");
    }
  } resolver;
  asio::io_context io;
  ToolRegistry host_tools(io);
  WorkflowLoader::Options opts;
  opts.key_resolver = &resolver;
  auto wf_or = WorkflowLoader::Load(j.dump(), host_tools, opts);
  EXPECT_FALSE(wf_or.ok());
  EXPECT_TRUE(absl::StrContains(wf_or.status().message(), "signature"));
}

// ---- Tier-3 (proto_dynamic) -------------------------------------------------

TEST(WorkflowLoaderTest, Tier3LoadsAndProducesProtoDynamicState) {
  google::protobuf::FileDescriptorProto fdp;
  agentflow::test::TestState::descriptor()->file()->CopyTo(&fdp);
  google::protobuf::FileDescriptorSet fds;
  *fds.add_file() = fdp;
  std::string desc_bytes;
  fds.SerializeToString(&desc_bytes);
  std::string b64;
  absl::Base64Escape(desc_bytes, &b64);

  std::string json = std::string(R"({
    "schema_version":1,"name":"x","version":"v1",
    "state":{"kind":"proto_dynamic",
              "message_type":"agentflow.test.TestState",
              "descriptor_set_b64":")") + b64 + std::string(R"("},
    "agents":{"a":{"system_prompt":"","model":{},"tools":[]}},
    "main":"a"
  })");
  asio::io_context io;
  ToolRegistry host_tools(io);
  auto wf_or = WorkflowLoader::Load(json, host_tools);
  ASSERT_TRUE(wf_or.ok()) << wf_or.status();
  ::agentflow::State s = (*wf_or)->NewEmptyState();
  EXPECT_EQ(s.kind(), ::agentflow::State::Kind::ProtoDynamic);
}

TEST(WorkflowLoaderTest, Tier3RejectsMissingDescriptorSet) {
  std::string json = R"({
    "schema_version":1,"name":"x","version":"v1",
    "state":{"kind":"proto_dynamic","message_type":"agentflow.test.TestState"},
    "agents":{"a":{"system_prompt":"","model":{},"tools":[]}},
    "main":"a"
  })";
  asio::io_context io;
  ToolRegistry host_tools(io);
  auto wf_or = WorkflowLoader::Load(json, host_tools);
  EXPECT_FALSE(wf_or.ok());
}

TEST(WorkflowLoaderTest, Tier3RejectsBadDescriptorSet) {
  std::string json = R"({
    "schema_version":1,"name":"x","version":"v1",
    "state":{"kind":"proto_dynamic","message_type":"agentflow.test.TestState",
              "descriptor_set_b64":"!!!not_base64!!!"},
    "agents":{"a":{"system_prompt":"","model":{},"tools":[]}},
    "main":"a"
  })";
  asio::io_context io;
  ToolRegistry host_tools(io);
  auto wf_or = WorkflowLoader::Load(json, host_tools);
  EXPECT_FALSE(wf_or.ok());
}

}  // namespace
}  // namespace agentflow::workflow
