#include "DashboardGridLayout.h"

namespace dashboard {

void computeGridLayout(const WidgetGridPackage& package, const int canvasWidth, const int canvasHeight,
                       std::array<WidgetRect, MAX_WIDGETS>& rectsOut) {
  const int colWidth = canvasWidth / GRID_COLUMNS;
  const int rowUnitHeight = canvasHeight / MAX_ROW_SPAN;

  for (uint8_t index = 0; index < package.widgetCount; ++index) {
    const Widget& widget = package.widgets[index];
    rectsOut[index] = WidgetRect{widget.column * colWidth, widget.row * rowUnitHeight,
                                 widget.columnSpan * colWidth, widget.rowSpan * rowUnitHeight};
  }
}

}  // namespace dashboard
