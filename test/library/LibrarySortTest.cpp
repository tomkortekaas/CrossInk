#include <cassert>
#include <string>
#include <vector>

#include "activities/library/LibrarySort.h"

namespace {
LibraryEntry entry(const char* path, const char* title, const char* author, uint32_t seq, LibraryBookStatus status,
                   int16_t rank = -1) {
  LibraryEntry e;
  e.path = path;
  e.title = title;
  e.author = author;
  e.addedSeq = seq;
  e.status = status;
  e.recentRank = rank;
  return e;
}

std::vector<std::string> paths(const std::vector<LibraryEntry>& entries) {
  std::vector<std::string> out;
  for (const auto& e : entries) out.push_back(e.path);
  return out;
}
}  // namespace

int main() {
  using S = LibraryBookStatus;

  assert(LibrarySort::titleFromPath("/books/Foo Bar.epub") == "Foo Bar");
  assert(LibrarySort::titleFromPath("Plain") == "Plain");
  assert(LibrarySort::titleFromPath("/a/.hidden") == ".hidden");
  assert(LibrarySort::titleFromPath("/x/archive.tar.gz") == "archive.tar");

  assert(LibrarySort::deriveStatus(true, false) == S::InProgress);
  assert(LibrarySort::deriveStatus(true, true) == S::Finished);
  assert(LibrarySort::deriveStatus(false, false) == S::New);
  assert(LibrarySort::deriveStatus(false, true) == S::Finished);

  assert(LibrarySort::compareText("abc", "ABD") < 0);
  assert(LibrarySort::compareText("Zeta", "alpha") > 0);
  assert(LibrarySort::compareText("same", "SAME") == 0);
  assert(LibrarySort::compareText("ab", "abc") < 0);

  // In progress (recent order; unranked after ranked) -> new by title -> finished by title.
  std::vector<LibraryEntry> books = {
      entry("/a", "Beta", "", 1, S::New),           entry("/b", "Alpha", "", 1, S::Finished),
      entry("/c", "Zulu", "", 1, S::InProgress, 1), entry("/d", "Yankee", "", 1, S::InProgress, 0),
      entry("/e", "Able", "", 1, S::InProgress),    entry("/f", "alpha", "", 1, S::New),
  };
  LibrarySort::sort(books, LibrarySortMode::InProgressFirst);
  assert((paths(books) == std::vector<std::string>{"/d", "/c", "/e", "/f", "/a", "/b"}));

  // Recently added: highest sequence first, title breaks ties.
  books = {entry("/1", "B", "", 1, S::New), entry("/3", "Z", "", 3, S::New), entry("/2", "A", "", 2, S::New),
           entry("/4", "C", "", 3, S::New)};
  LibrarySort::sort(books, LibrarySortMode::RecentlyAdded);
  assert((paths(books) == std::vector<std::string>{"/4", "/3", "/2", "/1"}));

  // Title, then path for identical titles.
  books = {entry("/y", "same", "", 1, S::New), entry("/x", "Same", "", 1, S::New), entry("/w", "Again", "", 1, S::New)};
  LibrarySort::sort(books, LibrarySortMode::Title);
  assert((paths(books) == std::vector<std::string>{"/w", "/x", "/y"}));

  // Author: case-insensitive, books without author last.
  books = {entry("/1", "T", "Eco", 1, S::New), entry("/2", "T", "", 1, S::New), entry("/3", "T", "austen", 1, S::New)};
  LibrarySort::sort(books, LibrarySortMode::Author);
  assert((paths(books) == std::vector<std::string>{"/3", "/1", "/2"}));
  return 0;
}
