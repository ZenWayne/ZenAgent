// agentflow/core/state.cc
#include "agentflow/core/state.h"

#include <string>
#include <vector>

#include <google/protobuf/descriptor.h>
#include <google/protobuf/dynamic_message.h>
#include <google/protobuf/io/coded_stream.h>
#include <google/protobuf/io/zero_copy_stream_impl_lite.h>

#include "absl/status/status.h"

namespace agentflow {

absl::Status ParseBoundedIntoMessage(google::protobuf::Message& msg,
                                     std::string_view bytes, int max_depth,
                                     int max_bytes) {
  // Reject early if the input already exceeds the byte budget. This also keeps
  // the static_cast<int> below well-defined: a >2 GiB string would otherwise
  // truncate to a negative size.
  if (bytes.size() > static_cast<size_t>(max_bytes)) {
    return absl::InvalidArgumentError(
        "ParseBoundedIntoMessage: input exceeds max_bytes");
  }
  google::protobuf::io::ArrayInputStream array_in(
      bytes.data(), static_cast<int>(bytes.size()));
  google::protobuf::io::CodedInputStream coded_in(&array_in);
  coded_in.SetRecursionLimit(max_depth);
  coded_in.SetTotalBytesLimit(max_bytes);
  msg.Clear();
  if (!msg.ParseFromCodedStream(&coded_in) ||
      !coded_in.ConsumedEntireMessage()) {
    return absl::InvalidArgumentError(
        "ParseBoundedIntoMessage: malformed message or limit exceeded");
  }
  return absl::OkStatus();
}

namespace {

using ProtoArm = std::unique_ptr<google::protobuf::Message>;
using JsonArm = std::unique_ptr<nlohmann::ordered_json>;

// Initialize a JSON value from a fields-decl entry per Spec §3+§6.
nlohmann::ordered_json InitFromSpec(const nlohmann::ordered_json& spec) {
  if (spec.is_object() && spec.contains("default")) {
    return spec.at("default");
  }
  if (spec.is_object() && spec.contains("type") && spec["type"].is_string()) {
    const std::string t = spec["type"].get<std::string>();
    if (t == "string") return std::string{};
    if (t == "integer") return 0;
    if (t == "number") return 0.0;
    if (t == "boolean") return false;
    if (t == "array") return nlohmann::ordered_json::array();
    if (t == "object") return nlohmann::ordered_json::object();
  }
  return nullptr;
}

std::vector<std::string> SplitPath(std::string_view path) {
  std::vector<std::string> parts;
  std::string current;
  for (char c : path) {
    if (c == '.') {
      parts.push_back(std::move(current));
      current.clear();
    } else {
      current.push_back(c);
    }
  }
  parts.push_back(std::move(current));
  return parts;
}

}  // namespace

State State::FromJson(const nlohmann::ordered_json& fields_decl) {
  State s;
  auto json_ptr = std::make_unique<nlohmann::ordered_json>(
      nlohmann::ordered_json::object());
  if (fields_decl.is_object()) {
    for (auto it = fields_decl.begin(); it != fields_decl.end(); ++it) {
      (*json_ptr)[it.key()] = InitFromSpec(it.value());
    }
  }
  s.backing_ = std::move(json_ptr);
  return s;
}

State::DynamicEnv::~DynamicEnv() = default;

State::Kind State::kind() const noexcept {
  if (std::holds_alternative<JsonArm>(backing_)) return Kind::Json;
  return pool_ ? Kind::ProtoDynamic : Kind::Proto;
}

State State::FromDynamicProto(
    std::shared_ptr<google::protobuf::DescriptorPool> pool,
    std::string_view message_type) {
  State s;
  if (!pool) return s;
  const auto* desc =
      pool->FindMessageTypeByName(std::string(message_type));
  if (!desc) return s;

  // Bundle the pool and a factory bound to it; both must outlive any Message
  // instance derived from that factory (the Message holds non-owning
  // pointers to factory-owned vtable data and pool-owned descriptors).
  auto env = std::make_shared<DynamicEnv>();
  env->pool = std::move(pool);
  env->factory =
      std::make_unique<google::protobuf::DynamicMessageFactory>(env->pool.get());
  const auto* prototype = env->factory->GetPrototype(desc);
  if (!prototype) return s;
  std::unique_ptr<google::protobuf::Message> instance(prototype->New());
  s.backing_ = std::move(instance);
  s.pool_ = std::move(env);
  return s;
}

// Bounded parse into this State's proto arm. Only valid for proto-backed
// states (tier 1 / tier 3); JSON-backed or empty states return FailedPrecondition.
absl::Status State::ParseFromStringBounded(std::string_view data, int max_depth,
                                           int max_bytes) {
  auto* p = std::get_if<ProtoArm>(&backing_);
  if (!p || !*p) {
    return absl::FailedPreconditionError(
        "State::ParseFromStringBounded: no proto message");
  }
  return ParseBoundedIntoMessage(**p, data, max_depth, max_bytes);
}

bool State::IsEmpty() const noexcept {
  if (const auto* p = std::get_if<ProtoArm>(&backing_)) {
    return *p == nullptr;
  }
  if (const auto* j = std::get_if<JsonArm>(&backing_)) {
    return *j == nullptr;
  }
  return true;
}

const google::protobuf::Message* State::UnsafeMessage() const {
  if (const auto* p = std::get_if<ProtoArm>(&backing_)) {
    return p->get();
  }
  return nullptr;
}

std::string State::SerializeAsString() const {
  if (const auto* p = std::get_if<ProtoArm>(&backing_)) {
    if (!*p) return {};
    std::string out;
    (*p)->SerializeToString(&out);
    return out;
  }
  if (const auto* j = std::get_if<JsonArm>(&backing_)) {
    if (!*j) return {};
    return (*j)->dump();
  }
  return {};
}

bool State::ParseFromString(std::string_view data) {
  if (auto* p = std::get_if<ProtoArm>(&backing_)) {
    if (!*p) return false;
    return (*p)->ParseFromArray(data.data(), static_cast<int>(data.size()));
  }
  if (auto* j = std::get_if<JsonArm>(&backing_)) {
    if (!*j) return false;
    try {
      **j = nlohmann::ordered_json::parse(data);
    } catch (const nlohmann::json::exception&) {
      return false;
    }
    return true;
  }
  return false;
}

State State::Clone() const {
  State out;
  if (const auto* p = std::get_if<ProtoArm>(&backing_)) {
    if (*p) {
      ProtoArm copy((*p)->New());
      copy->CopyFrom(**p);
      out.backing_ = std::move(copy);
    }
    out.pool_ = pool_;  // keepalive carries forward (no-op for tier 1)
    return out;
  }
  if (const auto* j = std::get_if<JsonArm>(&backing_)) {
    if (*j) {
      out.backing_ = std::make_unique<nlohmann::ordered_json>(**j);
    }
    return out;
  }
  return out;
}

// ---------------------------------------------------------------------------
// Free-function helpers.
// ---------------------------------------------------------------------------

namespace {

// Proto-arm read: treat the (possibly dotted) path as a single TYPE_STRING
// top-level field name. Matches the lifted node logic from agent_node.cc.
std::string ReadProtoField(const google::protobuf::Message& msg,
                           std::string_view path) {
  const auto* refl = msg.GetReflection();
  const auto* desc =
      msg.GetDescriptor()->FindFieldByName(std::string(path));
  if (!desc) return {};
  if (desc->type() == google::protobuf::FieldDescriptor::TYPE_STRING) {
    return refl->GetString(msg, desc);
  }
  return {};
}

void WriteProtoField(google::protobuf::Message* msg, std::string_view path,
                     std::string_view value) {
  if (!msg) return;
  const auto* refl = msg->GetReflection();
  const auto* desc =
      msg->GetDescriptor()->FindFieldByName(std::string(path));
  if (!desc) return;
  if (desc->type() == google::protobuf::FieldDescriptor::TYPE_STRING) {
    refl->SetString(msg, desc, std::string(value));
  }
}

std::string ReadJsonField(const nlohmann::ordered_json& root,
                          std::string_view path) {
  auto parts = SplitPath(path);
  const nlohmann::ordered_json* cur = &root;
  for (size_t i = 0; i < parts.size(); ++i) {
    if (!cur->is_object()) return {};
    if (!cur->contains(parts[i])) return {};
    cur = &cur->at(parts[i]);
  }
  if (cur->is_string()) return cur->get<std::string>();
  return cur->dump();
}

void WriteJsonField(nlohmann::ordered_json& root, std::string_view path,
                    std::string_view value) {
  auto parts = SplitPath(path);
  if (parts.empty()) return;
  nlohmann::ordered_json* cur = &root;
  for (size_t i = 0; i + 1 < parts.size(); ++i) {
    if (!cur->is_object()) {
      *cur = nlohmann::ordered_json::object();
    }
    if (!cur->contains(parts[i]) || !(*cur)[parts[i]].is_object()) {
      (*cur)[parts[i]] = nlohmann::ordered_json::object();
    }
    cur = &(*cur)[parts[i]];
  }
  if (!cur->is_object()) {
    *cur = nlohmann::ordered_json::object();
  }
  (*cur)[parts.back()] = std::string(value);
}

}  // namespace

std::string ReadStringField(const State& s, std::string_view path) {
  if (const auto* p = std::get_if<ProtoArm>(&s.backing_)) {
    if (!*p) return {};
    return ReadProtoField(**p, path);
  }
  if (const auto* j = std::get_if<JsonArm>(&s.backing_)) {
    if (!*j) return {};
    return ReadJsonField(**j, path);
  }
  return {};
}

void WriteStringField(State& s, std::string_view path,
                      std::string_view value) {
  if (auto* p = std::get_if<ProtoArm>(&s.backing_)) {
    if (!*p) return;
    WriteProtoField(p->get(), path, value);
    return;
  }
  if (auto* j = std::get_if<JsonArm>(&s.backing_)) {
    if (!*j) {
      *j = std::make_unique<nlohmann::ordered_json>(
          nlohmann::ordered_json::object());
    }
    WriteJsonField(**j, path, value);
    return;
  }
}

const nlohmann::ordered_json* AsJson(const State& s) {
  if (const auto* j = std::get_if<JsonArm>(&s.backing_)) {
    return j->get();
  }
  return nullptr;
}

nlohmann::ordered_json* MutableJson(State& s) {
  if (auto* j = std::get_if<JsonArm>(&s.backing_)) {
    return j->get();
  }
  return nullptr;
}

}  // namespace agentflow
