#ifndef AGENTFLOW_INFERENCE_OPENAI_TOKEN_USAGE_H_
#define AGENTFLOW_INFERENCE_OPENAI_TOKEN_USAGE_H_

#include <limits>
#include <optional>

#include <nlohmann/json.hpp>

namespace agentflow::openai {

// Selected by the host's provider configuration, never inferred from a model.
enum class UsageProvider { kOpenAi, kGoogle, kDeepSeek };

inline bool ValidTokenCount(const nlohmann::json& count) {
  if (count.is_number_unsigned())
    return count.get<unsigned long long>() <=
        static_cast<unsigned long long>(std::numeric_limits<long long>::max());
  return count.is_number_integer() && count.get<long long>() >= 0;
}

inline std::optional<long long> OutputTokenCount(
    const nlohmann::json& usage, UsageProvider provider) {
  if (!usage.contains("completion_tokens") ||
      !ValidTokenCount(usage["completion_tokens"])) return std::nullopt;

  const auto completion = usage["completion_tokens"].get<long long>();
  switch (provider) {
    case UsageProvider::kGoogle: {
      // Vertex's completion count excludes thinking tokens.
      if (!usage.contains("completion_tokens_details") ||
          !usage["completion_tokens_details"].is_object()) return completion;
      const auto& details = usage["completion_tokens_details"];
      if (!details.contains("reasoning_tokens") ||
          !ValidTokenCount(details["reasoning_tokens"])) return completion;
      const auto thinking = details["reasoning_tokens"].get<long long>();
      if (thinking > std::numeric_limits<long long>::max() - completion)
        return completion;
      return completion + thinking;
    }
    case UsageProvider::kDeepSeek:
      // DeepSeek's completion count already includes reasoning.
      return completion;
    case UsageProvider::kOpenAi:
      return completion;
  }
  return completion;
}

}  // namespace agentflow::openai
#endif  // AGENTFLOW_INFERENCE_OPENAI_TOKEN_USAGE_H_
