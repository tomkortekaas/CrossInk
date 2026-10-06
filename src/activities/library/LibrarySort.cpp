#include "LibrarySort.h"

#include <algorithm>
#include <climits>

namespace LibrarySort {
namespace {
unsigned char fold(const char c) {
  const auto u = static_cast<unsigned char>(c);
  return (u >= 'A' && u <= 'Z') ? static_cast<unsigned char>(u - 'A' + 'a') : u;
}

bool titleLess(const LibraryEntry& a, const LibraryEntry& b) {
  const int byTitle = compareText(a.title, b.title);
  if (byTitle != 0) return byTitle < 0;
  return a.path < b.path;
}

int groupRank(const LibraryBookStatus status) {
  switch (status) {
    case LibraryBookStatus::InProgress:
      return 0;
    case LibraryBookStatus::New:
      return 1;
    case LibraryBookStatus::Finished:
      return 2;
  }
  return 1;
}

int recentOrder(const LibraryEntry& e) { return e.recentRank < 0 ? INT_MAX : e.recentRank; }
}  // namespace

std::string titleFromPath(const std::string_view path) {
  const size_t slash = path.find_last_of('/');
  std::string_view name = slash == std::string_view::npos ? path : path.substr(slash + 1);
  const size_t dot = name.find_last_of('.');
  if (dot != std::string_view::npos && dot > 0) name = name.substr(0, dot);
  return std::string(name);
}

LibraryBookStatus deriveStatus(const bool inRecents, const bool completed) {
  if (completed) return LibraryBookStatus::Finished;
  return inRecents ? LibraryBookStatus::InProgress : LibraryBookStatus::New;
}

int compareText(const std::string_view a, const std::string_view b) {
  const size_t n = std::min(a.size(), b.size());
  for (size_t i = 0; i < n; ++i) {
    const unsigned char ca = fold(a[i]);
    const unsigned char cb = fold(b[i]);
    if (ca != cb) return ca < cb ? -1 : 1;
  }
  if (a.size() == b.size()) return 0;
  return a.size() < b.size() ? -1 : 1;
}

void sort(std::vector<LibraryEntry>& entries, const LibrarySortMode mode) {
  switch (mode) {
    case LibrarySortMode::InProgressFirst:
      std::stable_sort(entries.begin(), entries.end(), [](const LibraryEntry& a, const LibraryEntry& b) {
        const int ga = groupRank(a.status);
        const int gb = groupRank(b.status);
        if (ga != gb) return ga < gb;
        if (a.status == LibraryBookStatus::InProgress && recentOrder(a) != recentOrder(b)) {
          return recentOrder(a) < recentOrder(b);
        }
        return titleLess(a, b);
      });
      return;
    case LibrarySortMode::RecentlyAdded:
      std::stable_sort(entries.begin(), entries.end(), [](const LibraryEntry& a, const LibraryEntry& b) {
        if (a.addedSeq != b.addedSeq) return a.addedSeq > b.addedSeq;
        return titleLess(a, b);
      });
      return;
    case LibrarySortMode::Title:
      std::stable_sort(entries.begin(), entries.end(), titleLess);
      return;
    case LibrarySortMode::Author:
      std::stable_sort(entries.begin(), entries.end(), [](const LibraryEntry& a, const LibraryEntry& b) {
        if (a.author.empty() != b.author.empty()) return b.author.empty();
        const int byAuthor = compareText(a.author, b.author);
        if (byAuthor != 0) return byAuthor < 0;
        return titleLess(a, b);
      });
      return;
  }
}
}  // namespace LibrarySort
