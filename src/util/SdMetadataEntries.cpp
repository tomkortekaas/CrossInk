#include "SdMetadataEntries.h"

#include <cctype>

namespace SdMetadataEntries {
namespace {
bool equalsIgnoreCase(std::string_view a, std::string_view b) {
  if (a.length() != b.length()) return false;
  for (size_t i = 0; i < a.length(); ++i) {
    if (tolower(static_cast<unsigned char>(a[i])) != tolower(static_cast<unsigned char>(b[i]))) {
      return false;
    }
  }
  return true;
}
}  // namespace

bool isMacOS(std::string_view filename) {
  return filename.rfind("._", 0) == 0 || filename == ".DS_Store" || filename == ".Spotlight-V100" ||
         filename == ".Trashes" || filename == ".fseventsd";
}

bool isWindows(std::string_view filename) {
  return equalsIgnoreCase(filename, "System Volume Information") || equalsIgnoreCase(filename, "$RECYCLE.BIN") ||
         equalsIgnoreCase(filename, "desktop.ini") || equalsIgnoreCase(filename, "Thumbs.db") ||
         equalsIgnoreCase(filename, "IndexerVolumeGuid") || equalsIgnoreCase(filename, "WPSettings.dat");
}
}  // namespace SdMetadataEntries
