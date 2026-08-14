#pragma once

#include <array>
#include <cstdint>

#include "DashboardWidgetGrid.h"

namespace dashboard {

struct WidgetRect {
  int x = 0;
  int y = 0;
  int width = 0;
  int height = 0;
};

// Computes each widget's pixel rectangle by flowing left-to-right/top-to-
// bottom through GRID_COLUMNS columns spanning canvasWidth, wrapping to a new
// row (shelf) whenever a widget's columnSpan does not fit what remains of the
// current row. A row's height is the tallest rowSpan placed in it, so the
// next shelf always clears every widget above it. A widget's rowSpan is
// measured in units of canvasHeight / MAX_ROW_SPAN, so a widget spanning
// MAX_ROW_SPAN rows fills the full canvas height. Pure integer geometry, no
// rendering dependency, so it is host-testable independent of GfxRenderer.
void computeGridLayout(const WidgetGridPackage& package, int canvasWidth, int canvasHeight,
                       std::array<WidgetRect, MAX_WIDGETS>& rectsOut);

}  // namespace dashboard
