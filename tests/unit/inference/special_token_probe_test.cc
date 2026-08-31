// tests/unit/inference/special_token_probe_test.cc
#include "agentflow/inference/special_token_probe.h"

#include <map>
#include <string>
#include <vector>

#include "gtest/gtest.h"

namespace agentflow {
namespace {

// Fake tokenizer. Registration is the tokenizer's editorial choice: a string
// either maps to a registered added-token id, or splits into ordinary pieces
// (never a single registered id). The detokenized TEXT of a token id may
// differ from its syntactic candidate name — e.g. <escape> maps to id 7 whose
// detokenized text is <|"|>. That asymmetry is exactly what the probe must
// resolve (it strips the DETOKENIZED form).
class FakeTokenizer {
 public:
  // Registers `text` as a single added token; `detokenized_text` is what the
  // tokenizer emits for that id.
  void AddToken(std::string text, int id, std::string detokenized_text) {
    tokenize_[text] = id;
    detokenize_[id] = std::move(detokenized_text);
  }

  TokenizerProbe Probe() {
    auto* self = this;
    TokenizerProbe p;
    p.tokenize = [self](std::string_view text)
        -> std::optional<std::vector<int>> {
      auto it = self->tokenize_.find(std::string(text));
      if (it != self->tokenize_.end()) return std::vector<int>{it->second};
      // Unregistered: split into per-character ids (never a single
      // registered id) so candidates are recognized as non-special.
      std::vector<int> ids;
      for (char c : text) ids.push_back(9000 + static_cast<int>(c));
      return ids;
    };
    p.detokenize = [self](const std::vector<int>& ids)
        -> std::optional<std::string> {
      if (ids.size() != 1) return std::nullopt;
      auto it = self->detokenize_.find(ids[0]);
      if (it == self->detokenize_.end()) return std::nullopt;
      return it->second;
    };
    return p;
  }

 private:
  std::map<std::string, int> tokenize_;
  std::map<int, std::string> detokenize_;
};

TEST(SpecialTokenProbeTest, GemmaFamilyResolvesEscapeToDetokenizedQuote) {
  FakeTokenizer tok;
  tok.AddToken("<start_of_turn>", 1, "<start_of_turn>");  // family probe
  // The quote fence: syntactic name <escape>, registered id 7, detokenized
  // text is the quote token <|"|> — the actual leak string.
  tok.AddToken("<escape>", 7, "<|\"|>");
  tok.AddToken("<end_of_turn>", 2, "<end_of_turn>");

  auto tokens = ProbeModelSpecialTokens(tok.Probe());

  bool has_quote = false;
  for (const auto& s : tokens.strip) {
    if (s == "<|\"|>") has_quote = true;  // detokenized form, NOT "<escape>"
    EXPECT_NE(s, "<escape>");             // never strip the syntactic name
  }
  EXPECT_TRUE(has_quote);
  EXPECT_NE(tokens.strip.size(), 0u);
}

TEST(SpecialTokenProbeTest, GemmaFamilyVerifiesEachCandidateAgainstTokenizer) {
  FakeTokenizer tok;
  tok.AddToken("<start_of_turn>", 1, "<start_of_turn>");
  tok.AddToken("<start_function_call>", 21, "<start_function_call>");
  // "call" is present in the template text but NOT a registered special.
  auto tokens = ProbeModelSpecialTokens(tok.Probe());
  bool has_fence = false;
  for (const auto& s : tokens.strip) {
    if (s == "<start_function_call>") has_fence = true;
    EXPECT_NE(s, "call");
  }
  EXPECT_TRUE(has_fence);
}

TEST(SpecialTokenProbeTest, QwenFamilyThroughImStartProbe) {
  FakeTokenizer tok;
  // id 1 detokenizes to something non-gemma (none are registered here).
  tok.AddToken("<|im_start|>", 5, "<|im_start|>");
  tok.AddToken("<|im_end|>", 6, "<|im_end|>");
  tok.AddToken("<tool_call>", 7, "<tool_call>");
  auto tokens = ProbeModelSpecialTokens(tok.Probe());
  ASSERT_EQ(tokens.strip.size(), 3u);
  EXPECT_EQ(tokens.strip[0], "<|im_start|>");
  EXPECT_EQ(tokens.strip[1], "<|im_end|>");
  EXPECT_EQ(tokens.strip[2], "<tool_call>");
}

TEST(SpecialTokenProbeTest, UnknownFamilyYieldsEmptySet) {
  FakeTokenizer tok;
  tok.AddToken("hello", 1, "hello");  // id 1 not <start_of_turn>
  // <|im_start|> unregistered -> per-char pieces -> not a single token.
  auto tokens = ProbeModelSpecialTokens(tok.Probe());
  EXPECT_TRUE(tokens.empty());
}

TEST(SpecialTokenProbeTest, FamilyRegistryYieldsFamilyStrings) {
  // Pre-upgrade path: no tokenizer probe available -> the host-declared
  // family registry supplies the known leak strings.
  auto gemma = FamilySpecialTokens(ModelFamily::kGemma);
  ASSERT_FALSE(gemma.empty());
  bool has_quote = false;
  for (const auto& s : gemma.strip) {
    if (s == "<|\"|>") has_quote = true;
  }
  EXPECT_TRUE(has_quote);  // the observed gemma4 leak string is in the set

  auto qwen = FamilySpecialTokens(ModelFamily::kQwen);
  ASSERT_FALSE(qwen.empty());

  auto unknown = FamilySpecialTokens(ModelFamily::kUnknown);
  EXPECT_TRUE(unknown.empty());
}

TEST(SpecialTokenProbeTest, ResolvePrefersProbeOverDeclaration) {
  FakeTokenizer tok;
  tok.AddToken("<start_of_turn>", 1, "<start_of_turn>");
  tok.AddToken("<escape>", 7, "<|\"|>");  // detokenized to the quote token

  // Declared family says qwen, but the probe (tokenizer) knows better and is
  // checked first: the resolved set carries the gemma quote token.
  auto probe = tok.Probe();
  auto resolved = ResolveSpecialTokens(&probe, ModelFamily::kQwen);
  bool has_quote = false;
  for (const auto& s : resolved.strip) {
    if (s == "<|\"|>") has_quote = true;
  }
  EXPECT_TRUE(has_quote);
  EXPECT_NE(resolved.strip.size(), 0u);
}

TEST(SpecialTokenProbeTest, ResolveFallsBackToDeclarationWhenProbeUnknown) {
  // Probe present but the tokenizer is "generic" (empty result) -> declared
  // family registry takes over.
  FakeTokenizer tok;
  tok.AddToken("hello", 1, "hello");
  auto probe = tok.Probe();
  auto resolved = ResolveSpecialTokens(&probe, ModelFamily::kGemma);
  bool has_quote = false;
  for (const auto& s : resolved.strip) {
    if (s == "<|\"|>") has_quote = true;
  }
  EXPECT_TRUE(has_quote);
}

TEST(SpecialTokenProbeTest, ResolveWithoutProbeUsesDeclaration) {
  // Pre-upgrade wiring: probe == nullptr -> registry only.
  auto resolved = ResolveSpecialTokens(nullptr, ModelFamily::kGemma);
  bool has_quote = false;
  for (const auto& s : resolved.strip) {
    if (s == "<|\"|>") has_quote = true;
  }
  EXPECT_TRUE(has_quote);

  auto none = ResolveSpecialTokens(nullptr, ModelFamily::kUnknown);
  EXPECT_TRUE(none.empty());
}

TEST(SpecialTokenProbeTest, UnregisteredCandidatesAreSkipped) {
  FakeTokenizer tok;
  tok.AddToken("<start_of_turn>", 1, "<start_of_turn>");  // gemma family
  tok.AddToken("<escape>", 7, "<|\"|>");
  // <start_function_call> NOT registered -> skipped.
  auto tokens = ProbeModelSpecialTokens(tok.Probe());
  // Both registered specials kept (quote fence + turn marker); the
  // unregistered fence candidate is absent.
  bool has_quote = false, has_turn = false, has_fence = false;
  for (const auto& s : tokens.strip) {
    if (s == "<|\"|>") has_quote = true;
    if (s == "<start_of_turn>") has_turn = true;
    if (s == "<start_function_call>") has_fence = true;
  }
  EXPECT_TRUE(has_quote);
  EXPECT_TRUE(has_turn);
  EXPECT_FALSE(has_fence);
}

}  // namespace
}  // namespace agentflow
