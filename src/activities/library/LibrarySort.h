#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

enum class LibraryBookStatus : uint8_t { New = 0, InProgress = 1, Finished = 2 };

// Values are persisted in CrossPointSettings::librarySortMode; never renumber.
enum class LibrarySortMode : uint8_t { InProgressFirst = 0, RecentlyAdded = 1, Title = 2, Author = 3 };
constexpr uint8_t kLibrarySortModeCount = 4;

struct LibraryEntry {
  std::string path;
  std::string title;   // filename stem until metadataLoaded
  std::string author;  // empty when unknown
  uint32_t addedSeq = 0;  // "first seen" generation; higher is newer
  LibraryBookStatus status = LibraryBookStatus::New;
  bool metadataLoaded = false;  // title/author were read from the book itself
  bool coverMissing = false;    // book has no cover image; do not retry thumbnail generation
  int16_t recentRank = -1;      // runtime only: index in RECENT_BOOKS, -1 when absent (not persisted)
};

namespace LibrarySort {
// "/books/Foo Bar.epub" -> "Foo Bar"
std::string titleFromPath(std::string_view path);
LibraryBookStatus deriveStatus(bool inRecents, bool completed);
// ASCII case-insensitive; bytes >= 0x80 compare raw. Returns <0, 0 or >0.
int compareText(std::string_view a, std::string_view b);
void sort(std::vector<LibraryEntry>& entries, LibrarySortMode mode);
}  // namespace LibrarySort
