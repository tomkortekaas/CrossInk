#pragma once

// Pure geometry for the 2x2 library page. No renderer dependency so it is host-testable.
namespace LibraryGrid {
constexpr int kBooksPerPage = 4;
constexpr int kColumns = 2;
constexpr int kTopMargin = 8;
constexpr int kMinSideMargin = 16;
constexpr int kColumnGap = 16;
constexpr int kRowGap = 12;
constexpr int kBarRowHeight = 22;  // progress bar + percentage under each cover
constexpr int kFooterHeight = 28;  // selected book: title and time left

struct Rect {
  int x = 0;
  int y = 0;
  int w = 0;
  int h = 0;
};

struct Layout {
  int coverWidth = 0;
  int coverHeight = 0;
  Rect covers[kBooksPerPage];
  Rect bars[kBooksPerPage];
  Rect footer;
};

// Largest 2:3 covers that fit two rows above the footer and the button-hint bar.
Layout compute(int screenWidth, int screenHeight, int buttonHintsHeight);
}  // namespace LibraryGrid
