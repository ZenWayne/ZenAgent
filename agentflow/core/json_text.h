// agentflow/core/json_text.h
#ifndef AGENTFLOW_CORE_JSON_TEXT_H_
#define AGENTFLOW_CORE_JSON_TEXT_H_

#include <string>

#include <nlohmann/json.hpp>

namespace agentflow {

// Serializes a payload that may embed text from a third party's error message.
// nlohmann's parse_error::what() can split a multi-byte UTF-8 sequence, and
// strict dump() throws on the stray bytes, hiding the original error. Invalid
// bytes are replaced instead, so the rest of the payload stays readable.
inline std::string DumpJsonLenient(const nlohmann::json& j) {
  return j.dump(/*indent=*/-1, /*indent_char=*/' ', /*ensure_ascii=*/false,
                nlohmann::json::error_handler_t::replace);
}

}  // namespace agentflow

#endif  // AGENTFLOW_CORE_JSON_TEXT_H_
