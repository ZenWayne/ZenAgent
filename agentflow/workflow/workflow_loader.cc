#include "agentflow/workflow/workflow_loader.h"

#include <fstream>
#include <functional>
#include <map>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

#include <absl/status/status.h>
#include <absl/strings/escaping.h>
#include <absl/strings/str_cat.h>
#include <google/protobuf/descriptor.h>
#include <google/protobuf/descriptor.pb.h>
#include <nlohmann/json.hpp>
#include <openssl/base64.h>
#include <openssl/digest.h>
#include <openssl/hmac.h>

#include "agentflow/workflow/template_engine.h"
#include "workflow_spec.pb.h"

namespace agentflow::workflow {
namespace {

using ::nlohmann::ordered_json;

// ---- JSON → proto mapping ------------------------------------------------

absl::Status ParseStateSpec(const ordered_json& j,
                             proto::WorkflowSpec::StateSpec* out) {
  if (!j.is_object()) {
    return absl::InvalidArgumentError("state must be an object");
  }
  if (auto it = j.find("kind"); it != j.end() && it->is_string()) {
    out->set_kind(it->get<std::string>());
  }
  if (auto it = j.find("message_type"); it != j.end() && it->is_string()) {
    out->set_message_type(it->get<std::string>());
  }
  if (auto it = j.find("descriptor_set_path");
      it != j.end() && it->is_string()) {
    out->set_descriptor_set_path(it->get<std::string>());
  }
  if (auto it = j.find("descriptor_set_b64");
      it != j.end() && it->is_string()) {
    out->set_descriptor_set_b64(it->get<std::string>());
  }
  if (auto it = j.find("fields"); it != j.end()) {
    if (!it->is_object()) {
      return absl::InvalidArgumentError("state.fields must be an object");
    }
    auto& fields_map = *out->mutable_fields();
    for (auto field_it = it->begin(); field_it != it->end(); ++field_it) {
      const std::string& fname = field_it.key();
      const ordered_json& fdecl = field_it.value();
      if (!fdecl.is_object()) {
        return absl::InvalidArgumentError(
            absl::StrCat("state.fields[", fname, "] must be an object"));
      }
      proto::WorkflowSpec::StateFieldDecl decl;
      if (auto t = fdecl.find("type"); t != fdecl.end() && t->is_string()) {
        decl.set_type(t->get<std::string>());
      }
      if (auto d = fdecl.find("default"); d != fdecl.end()) {
        decl.set_default_value_json(d->dump());
      }
      fields_map[fname] = std::move(decl);
    }
  }
  return absl::OkStatus();
}

absl::Status ParseDelegateSpec(const ordered_json& j,
                                proto::WorkflowSpec::DelegateSpec* out) {
  if (!j.is_object()) {
    return absl::InvalidArgumentError("delegates must be an object");
  }
  if (auto it = j.find("agents"); it != j.end()) {
    if (!it->is_array()) {
      return absl::InvalidArgumentError("delegates.agents must be an array");
    }
    for (const auto& a : *it) {
      if (!a.is_string()) {
        return absl::InvalidArgumentError(
            "delegates.agents entries must be strings");
      }
      out->add_agents(a.get<std::string>());
    }
  }
  if (auto it = j.find("max_depth"); it != j.end() && it->is_number_integer()) {
    out->set_max_depth(it->get<uint32_t>());
  }
  if (auto it = j.find("parallel"); it != j.end() && it->is_boolean()) {
    out->set_parallel(it->get<bool>());
  }
  if (auto it = j.find("input_template"); it != j.end()) {
    if (!it->is_object()) {
      return absl::InvalidArgumentError(
          "delegates.input_template must be an object");
    }
    auto& m = *out->mutable_input_template();
    for (auto k = it->begin(); k != it->end(); ++k) {
      if (!k.value().is_string()) {
        return absl::InvalidArgumentError(absl::StrCat(
            "delegates.input_template[", k.key(), "] must be a string"));
      }
      m[k.key()] = k.value().get<std::string>();
    }
  }
  if (auto it = j.find("goal_template");
      it != j.end() && it->is_string()) {
    out->set_goal_template(it->get<std::string>());
  }
  if (auto it = j.find("output_extract");
      it != j.end() && it->is_string()) {
    out->set_output_extract(it->get<std::string>());
  }
  return absl::OkStatus();
}

absl::Status ParseAgentDef(const std::string& agent_name,
                            const ordered_json& j,
                            proto::WorkflowSpec::AgentDef* out) {
  if (!j.is_object()) {
    return absl::InvalidArgumentError(
        absl::StrCat("agent '", agent_name, "' must be an object"));
  }
  if (auto it = j.find("system_prompt");
      it != j.end() && it->is_string()) {
    out->set_system_prompt(it->get<std::string>());
  }
  if (auto it = j.find("model"); it != j.end()) {
    if (!it->is_object()) {
      return absl::InvalidArgumentError(
          absl::StrCat("agent '", agent_name, "'.model must be an object"));
    }
    auto* m = out->mutable_model();
    if (auto t = it->find("max_output_tokens");
        t != it->end() && t->is_number_integer()) {
      m->set_max_output_tokens(t->get<int32_t>());
    }
    if (auto t = it->find("constrained_tool_calls");
        t != it->end() && t->is_boolean()) {
      m->set_constrained_tool_calls(t->get<bool>());
    }
  }
  if (auto it = j.find("tools"); it != j.end()) {
    if (!it->is_array()) {
      return absl::InvalidArgumentError(
          absl::StrCat("agent '", agent_name, "'.tools must be an array"));
    }
    for (const auto& t : *it) {
      if (!t.is_string()) {
        return absl::InvalidArgumentError(absl::StrCat(
            "agent '", agent_name, "'.tools entries must be strings"));
      }
      out->add_tools(t.get<std::string>());
    }
  }
  if (auto it = j.find("delegates"); it != j.end()) {
    if (auto s = ParseDelegateSpec(*it, out->mutable_delegates()); !s.ok()) {
      return absl::InvalidArgumentError(
          absl::StrCat("agent '", agent_name, "': ", s.message()));
    }
  }
  return absl::OkStatus();
}

absl::Status ParseSigning(const ordered_json& j,
                           proto::WorkflowSpec::Signing* out) {
  if (!j.is_object()) {
    return absl::InvalidArgumentError("signing must be an object");
  }
  if (auto it = j.find("algo"); it != j.end() && it->is_string()) {
    out->set_algo(it->get<std::string>());
  }
  if (auto it = j.find("key_id"); it != j.end() && it->is_string()) {
    out->set_key_id(it->get<std::string>());
  }
  if (auto it = j.find("signature"); it != j.end() && it->is_string()) {
    out->set_signature(it->get<std::string>());
  }
  return absl::OkStatus();
}

absl::Status ParseRoot(const ordered_json& root, proto::WorkflowSpec* spec) {
  if (!root.is_object()) {
    return absl::InvalidArgumentError("workflow root must be a JSON object");
  }
  if (auto it = root.find("schema_version");
      it != root.end() && it->is_number_integer()) {
    spec->set_schema_version(it->get<uint32_t>());
  }
  if (auto it = root.find("name"); it != root.end() && it->is_string()) {
    spec->set_name(it->get<std::string>());
  }
  if (auto it = root.find("version"); it != root.end() && it->is_string()) {
    spec->set_version(it->get<std::string>());
  }
  if (auto it = root.find("main"); it != root.end() && it->is_string()) {
    spec->set_main(it->get<std::string>());
  }
  if (auto it = root.find("state"); it != root.end()) {
    if (auto s = ParseStateSpec(*it, spec->mutable_state()); !s.ok()) return s;
  }
  if (auto it = root.find("agents"); it != root.end()) {
    if (!it->is_object()) {
      return absl::InvalidArgumentError("agents must be an object");
    }
    auto& agents_map = *spec->mutable_agents();
    for (auto a = it->begin(); a != it->end(); ++a) {
      proto::WorkflowSpec::AgentDef def;
      if (auto s = ParseAgentDef(a.key(), a.value(), &def); !s.ok()) return s;
      agents_map[a.key()] = std::move(def);
    }
  }
  if (auto it = root.find("signing"); it != root.end()) {
    if (auto s = ParseSigning(*it, spec->mutable_signing()); !s.ok()) return s;
  }
  return absl::OkStatus();
}

// ---- Validation passes ---------------------------------------------------

constexpr size_t kMaxAgents = 32;
constexpr size_t kMaxToolsPerAgent = 64;
constexpr size_t kMaxDelegateAgents = 16;
constexpr uint32_t kMaxDelegateDepth = 8;

absl::Status CheckResourceLimits(const proto::WorkflowSpec& spec,
                                  size_t json_bytes, size_t max_json_bytes) {
  if (json_bytes > max_json_bytes) {
    return absl::ResourceExhaustedError(absl::StrCat(
        "workflow json size ", json_bytes, " > limit ", max_json_bytes));
  }
  if (spec.agents().size() > kMaxAgents) {
    return absl::ResourceExhaustedError(absl::StrCat(
        "agent count ", spec.agents().size(), " > limit ", kMaxAgents));
  }
  for (const auto& [name, def] : spec.agents()) {
    if (static_cast<size_t>(def.tools_size()) > kMaxToolsPerAgent) {
      return absl::ResourceExhaustedError(absl::StrCat(
          "agent '", name, "' has ", def.tools_size(),
          " tools (limit ", kMaxToolsPerAgent, ")"));
    }
    if (def.has_delegates()) {
      if (static_cast<size_t>(def.delegates().agents_size()) >
          kMaxDelegateAgents) {
        return absl::ResourceExhaustedError(absl::StrCat(
            "agent '", name, "' delegates ", def.delegates().agents_size(),
            " agents (limit ", kMaxDelegateAgents, ")"));
      }
      if (def.delegates().max_depth() > kMaxDelegateDepth) {
        return absl::ResourceExhaustedError(absl::StrCat(
            "agent '", name, "' delegate max_depth ",
            def.delegates().max_depth(), " > limit ", kMaxDelegateDepth));
      }
    }
  }
  return absl::OkStatus();
}

absl::Status CheckReferences(const proto::WorkflowSpec& spec,
                              const ToolRegistry& host_tools) {
  if (spec.main().empty()) {
    return absl::InvalidArgumentError("main is required");
  }
  if (spec.agents().find(spec.main()) == spec.agents().end()) {
    return absl::InvalidArgumentError(absl::StrCat(
        "main '", spec.main(), "' is not a declared agent"));
  }
  for (const auto& [agent_name, def] : spec.agents()) {
    for (const auto& t : def.tools()) {
      if (t == "delegate") {
        return absl::InvalidArgumentError(absl::StrCat(
            "agent '", agent_name,
            "' references reserved tool name 'delegate' "
            "(auto-registered by the runtime)"));
      }
      if (!host_tools.Has(t)) {
        return absl::InvalidArgumentError(absl::StrCat(
            "agent '", agent_name, "' references unknown tool '", t, "'"));
      }
    }
    if (def.has_delegates()) {
      for (const auto& target : def.delegates().agents()) {
        if (spec.agents().find(target) == spec.agents().end()) {
          return absl::InvalidArgumentError(absl::StrCat(
              "agent '", agent_name,
              "' delegates to unknown agent '", target, "'"));
        }
      }
    }
  }
  return absl::OkStatus();
}

absl::Status CheckOneTemplate(const std::string& agent_name,
                                const std::string& location,
                                const std::string& source,
                                const std::unordered_set<std::string>&
                                    state_field_names) {
  auto parsed = TemplateString::Parse(source);
  if (!parsed.ok()) {
    return absl::InvalidArgumentError(absl::StrCat(
        "agent '", agent_name, "' ", location, ": ",
        parsed.status().message()));
  }
  for (const auto& parts : parsed->paths()) {
    if (parts.empty()) continue;
    if (parts[0] != "state") continue;  // runtime-resolved heads checked later.
    if (parts.size() < 2) {
      return absl::InvalidArgumentError(absl::StrCat(
          "agent '", agent_name, "' ", location,
          ": template '{{state}}' missing field name"));
    }
    if (state_field_names.find(parts[1]) == state_field_names.end()) {
      return absl::InvalidArgumentError(absl::StrCat(
          "agent '", agent_name, "' ", location, ": references unknown state field '",
          parts[1], "'"));
    }
  }
  return absl::OkStatus();
}

absl::Status CheckTemplates(const proto::WorkflowSpec& spec) {
  std::unordered_set<std::string> state_field_names;
  for (const auto& [k, _] : spec.state().fields()) state_field_names.insert(k);
  for (const auto& [agent_name, def] : spec.agents()) {
    if (!def.has_delegates()) continue;
    const auto& d = def.delegates();
    if (!d.goal_template().empty()) {
      if (auto s = CheckOneTemplate(agent_name, "delegates.goal_template",
                                      d.goal_template(), state_field_names);
          !s.ok())
        return s;
    }
    for (const auto& [key, tmpl] : d.input_template()) {
      if (auto s = CheckOneTemplate(
              agent_name, absl::StrCat("delegates.input_template[", key, "]"),
              tmpl, state_field_names);
          !s.ok())
        return s;
    }
  }
  return absl::OkStatus();
}

absl::Status CheckAcyclic(const proto::WorkflowSpec& spec) {
  // Iterative DFS with white/gray/black coloring rooted at every agent.
  enum Color : uint8_t { kWhite = 0, kGray, kBlack };
  std::unordered_map<std::string, Color> color;
  for (const auto& [name, _] : spec.agents()) color[name] = kWhite;
  for (const auto& [start, _] : spec.agents()) {
    if (color[start] != kWhite) continue;
    // Stack of (node, child_index).
    std::vector<std::pair<std::string, int>> stack;
    stack.emplace_back(start, 0);
    color[start] = kGray;
    while (!stack.empty()) {
      auto& [cur, idx] = stack.back();
      const auto& def = spec.agents().at(cur);
      const bool has_dels = def.has_delegates();
      const int n_children = has_dels ? def.delegates().agents_size() : 0;
      if (idx < n_children) {
        const std::string child = def.delegates().agents(idx);
        ++idx;
        auto it = color.find(child);
        if (it == color.end()) continue;  // unknown — caught by CheckReferences
        if (it->second == kGray) {
          return absl::InvalidArgumentError(absl::StrCat(
              "delegation cycle detected through agent '", child, "'"));
        }
        if (it->second == kWhite) {
          it->second = kGray;
          stack.emplace_back(child, 0);
        }
      } else {
        color[cur] = kBlack;
        stack.pop_back();
      }
    }
  }
  return absl::OkStatus();
}

// Canonical JSON: recursively sort object keys, strip top-level "signing".
// Used as the signed payload — the signature is computed over this canonical
// form, not over the author's original bytes, so reformatting/reordering of
// the author's JSON doesn't invalidate the signature.
std::string CanonicalForm(const nlohmann::ordered_json& root_in) {
  std::function<nlohmann::json(const nlohmann::ordered_json&, bool)> canon =
      [&](const nlohmann::ordered_json& v, bool is_root) -> nlohmann::json {
    if (v.is_object()) {
      std::map<std::string, nlohmann::json> sorted;
      for (auto it = v.begin(); it != v.end(); ++it) {
        if (is_root && it.key() == "signing") continue;
        sorted[it.key()] = canon(it.value(), false);
      }
      nlohmann::json out = nlohmann::json::object();
      for (auto& [k, val] : sorted) out[k] = std::move(val);
      return out;
    }
    if (v.is_array()) {
      nlohmann::json out = nlohmann::json::array();
      for (const auto& el : v) out.push_back(canon(el, false));
      return out;
    }
    return v;
  };
  return canon(root_in, /*is_root=*/true).dump();
}

// Returns base64-encoded HMAC-SHA256(data, key). Backed by boringssl —
// signing isn't security-critical for control-plane workflows but using a
// maintained crypto runtime beats vendoring one.
std::string HmacSha256Base64(std::string_view key, std::string_view data) {
  uint8_t mac[EVP_MAX_MD_SIZE];
  unsigned int mac_len = 0;
  HMAC(EVP_sha256(),
       key.data(), key.size(),
       reinterpret_cast<const uint8_t*>(data.data()), data.size(),
       mac, &mac_len);
  if (mac_len == 0) return {};
  std::string out;
  out.resize(((mac_len + 2) / 3) * 4 + 1);  // +1 for the NUL EVP_EncodeBlock writes
  int written = EVP_EncodeBlock(
      reinterpret_cast<uint8_t*>(out.data()), mac, mac_len);
  out.resize(static_cast<size_t>(written));
  return out;
}

absl::Status VerifySignature(const std::string& canonical,
                              const proto::WorkflowSpec& spec,
                              KeyResolver& resolver) {
  const auto& sig = spec.signing();
  if (sig.algo() != "HMAC-SHA256") {
    return absl::InvalidArgumentError(
        absl::StrCat("unsupported signing algo '", sig.algo(), "'"));
  }
  auto key_or = resolver.Resolve(sig.key_id());
  if (!key_or.ok()) return key_or.status();
  std::string expected = HmacSha256Base64(*key_or, canonical);
  // Compare as raw strings. Constant-time isn't strictly needed for control-
  // plane workflows but a length-first guard is cheap.
  std::string_view supplied(reinterpret_cast<const char*>(sig.signature().data()),
                            sig.signature().size());
  if (expected.size() != supplied.size()) {
    return absl::InvalidArgumentError("signature_invalid");
  }
  unsigned diff = 0;
  for (size_t i = 0; i < expected.size(); ++i) diff |= expected[i] ^ supplied[i];
  if (diff != 0) return absl::InvalidArgumentError("signature_invalid");
  return absl::OkStatus();
}

}  // namespace

absl::StatusOr<std::shared_ptr<Workflow>> WorkflowLoader::Load(
    std::string_view json_text,
    const ToolRegistry& host_tools,
    const Options& opts) {
  if (json_text.size() > opts.max_json_bytes) {
    return absl::ResourceExhaustedError(absl::StrCat(
        "workflow json size ", json_text.size(),
        " > limit ", opts.max_json_bytes));
  }
  auto root = ordered_json::parse(json_text, nullptr, /*allow_exceptions=*/false);
  if (root.is_discarded()) {
    return absl::InvalidArgumentError("malformed json");
  }

  proto::WorkflowSpec spec;
  if (auto s = ParseRoot(root, &spec); !s.ok()) return s;

  // Forward-leniency: treat absent/zero schema_version as the current min.
  if (spec.schema_version() == 0) spec.set_schema_version(1);
  if (spec.schema_version() > kCurrentWorkflowSchemaVersion) {
    return absl::FailedPreconditionError(absl::StrCat(
        "schema_version ", spec.schema_version(),
        " > supported ", kCurrentWorkflowSchemaVersion));
  }

  if (auto s = CheckResourceLimits(spec, json_text.size(), opts.max_json_bytes);
      !s.ok())
    return s;
  if (auto s = CheckReferences(spec, host_tools); !s.ok()) return s;
  if (auto s = CheckAcyclic(spec); !s.ok()) return s;
  if (auto s = CheckTemplates(spec); !s.ok()) return s;

  // Signing verification. The signature is computed over a canonical JSON
  // form (sorted keys, no whitespace, top-level "signing" stripped) — this
  // decouples the verifier from author-side formatting.
  //
  // Three cases:
  //   1. has_signing && opts.key_resolver  → verify, must match.
  //   2. has_signing && !opts.key_resolver → permissive (signature ignored).
  //      This is the dev-host escape hatch; production hosts must wire a
  //      resolver and (typically) set require_signed=true.
  //   3. !has_signing && opts.require_signed → reject.
  const bool has_signing = spec.has_signing() &&
                           (!spec.signing().algo().empty() ||
                            !spec.signing().signature().empty());
  if (has_signing) {
    if (opts.key_resolver != nullptr) {
      const std::string canon = CanonicalForm(root);
      if (auto s = VerifySignature(canon, spec, *opts.key_resolver); !s.ok()) {
        return s;
      }
    }
    // else: permissive — accept without verification. A future change may
    // emit SIGNED_WORKFLOW_UNVERIFIED via opts.trace.
  } else if (opts.require_signed) {
    return absl::FailedPreconditionError("signature_required");
  }

  // Tier-3 (proto_dynamic) state: build a DescriptorPool from the supplied
  // FileDescriptorSet (preferred: descriptor_set_b64 inline; fallback:
  // descriptor_set_path on disk). The pool is held on the Workflow so any
  // State produced by NewEmptyState() remains valid for the workflow's
  // lifetime. Reflection (ReadStringField/WriteStringField) on the resulting
  // Message works unchanged.
  std::shared_ptr<google::protobuf::DescriptorPool> state_pool;
  if (spec.state().kind() == "proto_dynamic") {
    std::string desc_bytes;
    if (!spec.state().descriptor_set_b64().empty()) {
      if (!absl::Base64Unescape(spec.state().descriptor_set_b64(),
                                  &desc_bytes)) {
        return absl::InvalidArgumentError("descriptor_set_b64 decode failed");
      }
    } else if (!spec.state().descriptor_set_path().empty()) {
      std::ifstream in(spec.state().descriptor_set_path(), std::ios::binary);
      if (!in) {
        return absl::NotFoundError(absl::StrCat(
            "descriptor_set_path not readable: ",
            spec.state().descriptor_set_path()));
      }
      std::stringstream ss;
      ss << in.rdbuf();
      desc_bytes = ss.str();
    } else {
      return absl::InvalidArgumentError(
          "proto_dynamic requires descriptor_set_path or descriptor_set_b64");
    }
    google::protobuf::FileDescriptorSet fds;
    if (!fds.ParseFromString(desc_bytes)) {
      return absl::InvalidArgumentError("descriptor_set parse failed");
    }
    state_pool = std::make_shared<google::protobuf::DescriptorPool>();
    for (const auto& fdp : fds.file()) {
      if (state_pool->BuildFile(fdp) == nullptr) {
        return absl::InvalidArgumentError(
            absl::StrCat("BuildFile failed for ", fdp.name()));
      }
    }
    if (spec.state().message_type().empty()) {
      return absl::InvalidArgumentError(
          "proto_dynamic requires message_type");
    }
    if (!state_pool->FindMessageTypeByName(spec.state().message_type())) {
      return absl::InvalidArgumentError(absl::StrCat(
          "descriptor for ", spec.state().message_type(),
          " not found in supplied set"));
    }
  }

  auto workflow = std::make_shared<Workflow>(std::move(spec));
  if (state_pool) workflow->SetStatePool(std::move(state_pool));
  return workflow;
}

absl::StatusOr<std::shared_ptr<Workflow>> WorkflowLoader::LoadFromFile(
    const std::string& path,
    const ToolRegistry& host_tools,
    const Options& opts) {
  std::ifstream in(path);
  if (!in) {
    return absl::NotFoundError(absl::StrCat("cannot open workflow file: ", path));
  }
  std::ostringstream buf;
  buf << in.rdbuf();
  return Load(buf.str(), host_tools, opts);
}

}  // namespace agentflow::workflow
