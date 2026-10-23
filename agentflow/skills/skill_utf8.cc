#include "agentflow/skills/skill_utf8.h"

#include <cstddef>

namespace agentflow::skills {

bool IsValidUtf8(std::string_view text) {
  for (size_t i = 0; i < text.size();) {
    const auto lead = static_cast<unsigned char>(text[i]);
    if (lead < 0x80) {
      ++i;
      continue;
    }
    size_t trailing = 0;
    unsigned int point = 0;
    unsigned int minimum = 0;
    if (lead >= 0xC2 && lead <= 0xDF) {
      trailing = 1; point = lead & 0x1F; minimum = 0x80;
    } else if (lead >= 0xE0 && lead <= 0xEF) {
      trailing = 2; point = lead & 0x0F; minimum = 0x800;
    } else if (lead >= 0xF0 && lead <= 0xF4) {
      trailing = 3; point = lead & 0x07; minimum = 0x10000;
    } else {
      return false;
    }
    if (trailing > text.size() - i - 1) return false;
    for (size_t k = 1; k <= trailing; ++k) {
      const auto byte = static_cast<unsigned char>(text[i + k]);
      if ((byte & 0xC0) != 0x80) return false;
      point = (point << 6) | (byte & 0x3F);
    }
    if (point < minimum || point > 0x10FFFF ||
        (point >= 0xD800 && point <= 0xDFFF)) return false;
    i += trailing + 1;
  }
  return true;
}

}  // namespace agentflow::skills
