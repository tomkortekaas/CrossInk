#include <cassert>
#include <cstdint>
#include <string>
#include <vector>
#include "util/PreviousBookShortcut.h"

int main() {
  using Result = PreviousBookGesture::Result;
  PreviousBookGesture gesture;
  auto event = [&](bool enabled, bool pressed, bool held, bool released, bool other, uint32_t now) {
    return gesture.update(enabled, pressed, held, released, other, now);
  };
  assert(event(true, true, true, false, false, 0) == Result::None);
  assert(event(true, false, true, false, false, 800) == Result::None);
  assert(event(true, false, false, true, false, 900) == Result::SwitchBook);
  assert(event(true, false, false, true, false, 901) == Result::None);
  assert(event(true, true, true, false, false, 1000) == Result::None);
  assert(event(true, false, false, true, false, 1100) == Result::PageTurn);
  event(true, true, true, false, false, 1200);
  assert(event(true, false, false, true, false, 1899) == Result::PageTurn);
  event(true, true, true, false, false, 2000);
  assert(event(true, false, false, true, false, 2700) == Result::SwitchBook);
  // A key already held on entry must not switch the newly loaded book.
  assert(event(true, false, true, false, false, 2000) == Result::None);
  assert(event(true, false, false, true, false, 3000) == Result::None);
  event(true, true, true, false, false, 4000);
  event(true, false, true, false, true, 4100);
  assert(event(true, false, false, true, false, 5000) == Result::None);
  event(true, true, true, false, false, 6000);
  event(false, false, true, false, false, 6100);
  assert(event(true, false, false, true, false, 7000) == Result::None);
  event(true, true, true, false, false, UINT32_MAX - 300);
  assert(event(true, false, false, true, false, 500) == Result::SwitchBook);
  // Lost release edge must not leave an armed shortcut behind.
  event(true, true, true, false, false, 8000);
  event(true, false, false, false, false, 8100);
  assert(event(true, false, false, true, false, 9000) == Result::None);

  struct Book { std::string path; };
  std::vector<Book> books{{"A.epub"}, {"missing.epub"}, {"B.epub"}, {"C.epub"}};
  auto valid = [](const std::string& path) { return path != "missing.epub"; };
  assert(previousBookPath(books, std::string("A.epub"), valid) == "B.epub");
  books = {{"B.epub"}, {"A.epub"}, {"C.epub"}};
  assert(previousBookPath(books, std::string("B.epub"), valid) == "A.epub");
  books = {{"A.epub"}, {"A.epub"}, {"missing.epub"}};
  assert(previousBookPath(books, std::string("A.epub"), valid).empty());
  books.clear();
  assert(previousBookPath(books, std::string("A.epub"), valid).empty());
}
