#include "LanguageRegistry.h"

#include <algorithm>
#include <array>

#include "HyphenationCommon.h"
#include "generated/hyph-en.trie.h"

// Only English hyphenation is compiled in for this fork to save flash; the
// other generated tries under generated/ are kept on disk (regeneratable via
// scripts/update_hyphenation.sh) but intentionally not #included here. A
// language with no registered hyphenator simply gets no hyphenation, the
// same fallback already used for any language never covered by this list.
namespace {

// English hyphenation patterns (3/3 minimum prefix/suffix length)
LanguageHyphenator englishHyphenator(en_patterns, isLatinLetter, toLowerLatin, 3, 3);

using EntryArray = std::array<LanguageEntry, 1>;

const EntryArray& entries() {
  static const EntryArray kEntries = {{{"english", "en", &englishHyphenator}}};
  return kEntries;
}

}  // namespace

const LanguageHyphenator* getLanguageHyphenatorForPrimaryTag(const std::string& primaryTag) {
  const auto& allEntries = entries();
  const auto it = std::find_if(allEntries.begin(), allEntries.end(),
                               [&primaryTag](const LanguageEntry& entry) { return primaryTag == entry.primaryTag; });
  return (it != allEntries.end()) ? it->hyphenator : nullptr;
}

LanguageEntryView getLanguageEntries() {
  const auto& allEntries = entries();
  return LanguageEntryView{allEntries.data(), allEntries.size()};
}
