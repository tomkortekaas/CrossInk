#include "DashboardGridLayout.h"

#include <algorithm>

namespace dashboard {

void computeGridLayout(const WidgetGridPackage& package, const int canvasWidth, const int canvasHeight,
                       std::array<WidgetRect, MAX_WIDGETS>& rectsOut) {
  const int colWidth = canvasWidth / GRID_COLUMNS;
  const int rowUnitHeight = canvasHeight / MAX_ROW_SPAN;

  int column = 0;
  int rowTop = 0;
  uint8_t rowMaxSpan = 0;
  for (uint8_t index = 0; index < package.widgetCount; ++index) {
    const Widget& widget = package.widgets[index];
    if (column + widget.columnSpan > GRID_COLUMNS) {
      rowTop += rowMaxSpan * rowUnitHeight;
      column = 0;
      rowMaxSpan = 0;
    }
    rectsOut[index] = WidgetRect{column * colWidth, rowTop, widget.columnSpan * colWidth,
                                 widget.rowSpan * rowUnitHeight};
    column += widget.columnSpan;
    rowMaxSpan = std::max(rowMaxSpan, widget.rowSpan);
  }
}

}  // namespace dashboard
