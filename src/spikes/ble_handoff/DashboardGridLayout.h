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

// Computes each widget's pixel rectangle directly from its explicit
// column/row/columnSpan/rowSpan: column/row are measured in units of
// canvasWidth / GRID_COLUMNS and canvasHeight / MAX_ROW_SPAN respectively, so
// a widget at column 0 spanning GRID_COLUMNS columns fills the full canvas
// width. Placement is free-form - the caller (the iPhone composer) is
// responsible for not producing overlapping widgets; this function does not
// detect or resolve overlap, it only maps position/span to pixels. Pure
// integer geometry, no rendering dependency, so it is host-testable
// independent of GfxRenderer.
void computeGridLayout(const WidgetGridPackage& package, int canvasWidth, int canvasHeight,
                       std::array<WidgetRect, MAX_WIDGETS>& rectsOut);

}  // namespace dashboard
