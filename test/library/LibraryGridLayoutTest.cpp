#include <cassert>

#include "activities/library/LibraryGridLayout.h"

int main() {
  // X3 portrait: 528 x 792 with the 40 px button-hint bar.
  const auto x3 = LibraryGrid::compute(528, 792, 40);
  assert(x3.coverWidth == 220 && x3.coverHeight == 330);
  assert(x3.covers[0].x == 36 && x3.covers[0].y == 8);
  assert(x3.covers[1].x == 272 && x3.covers[1].y == 8);
  assert(x3.covers[2].x == 36 && x3.covers[2].y == 372);
  assert(x3.covers[3].x == 272 && x3.covers[3].y == 372);
  assert(x3.bars[0].x == 36 && x3.bars[0].y == 338 && x3.bars[0].w == 220 && x3.bars[0].h == 22);
  assert(x3.bars[3].y == 702);
  assert(x3.footer.x == 36 && x3.footer.w == 456);
  assert(x3.footer.y == 724 && x3.footer.y + x3.footer.h == 752);  // ends exactly at the hint bar

  // X4 portrait is width-limited.
  const auto x4 = LibraryGrid::compute(480, 800, 40);
  assert(x4.coverWidth == 216 && x4.coverHeight == 324);
  assert(x4.covers[0].x == 16);
  assert(x4.footer.y + x4.footer.h <= 760);

  // Degenerate screens must not produce negative sizes.
  const auto tiny = LibraryGrid::compute(100, 100, 40);
  assert(tiny.coverWidth == 0 && tiny.coverHeight == 0);
  return 0;
}
