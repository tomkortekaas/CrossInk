#include "LibraryGridLayout.h"

#include <algorithm>

namespace LibraryGrid {
Layout compute(const int screenWidth, const int screenHeight, const int buttonHintsHeight) {
  Layout layout;
  const int verticalBudget =
      screenHeight - buttonHintsHeight - kTopMargin - kColumns * kBarRowHeight - kRowGap - kFooterHeight;
  const int heightLimited = std::max(0, verticalBudget / 2);
  const int widthLimited = std::max(0, (screenWidth - 2 * kMinSideMargin - kColumnGap) / kColumns);

  int coverWidth = heightLimited * 2 / 3;
  int coverHeight = heightLimited;
  if (coverWidth > widthLimited) {
    coverWidth = widthLimited;
    coverHeight = widthLimited * 3 / 2;
  }
  layout.coverWidth = coverWidth;
  layout.coverHeight = coverHeight;

  const int sideMargin = (screenWidth - kColumns * coverWidth - kColumnGap) / 2;
  for (int i = 0; i < kBooksPerPage; ++i) {
    const int col = i % kColumns;
    const int row = i / kColumns;
    const int x = sideMargin + col * (coverWidth + kColumnGap);
    const int y = kTopMargin + row * (coverHeight + kBarRowHeight + kRowGap);
    layout.covers[i] = {x, y, coverWidth, coverHeight};
    layout.bars[i] = {x, y + coverHeight, coverWidth, kBarRowHeight};
  }
  const int footerY = kTopMargin + 2 * (coverHeight + kBarRowHeight) + kRowGap;
  layout.footer = {sideMargin, footerY, kColumns * coverWidth + kColumnGap, kFooterHeight};
  return layout;
}
}  // namespace LibraryGrid
