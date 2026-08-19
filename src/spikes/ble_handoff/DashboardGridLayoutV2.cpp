#include "DashboardGridLayoutV2.h"

namespace dashboard {
namespace v2 {

void computeGridLayoutV2(const WidgetGridPackageV2& package, const int canvasWidth, const int canvasHeight,
                         std::array<WidgetRectV2, MAX_WIDGETS>& rectsOut, const int originX,
                         const int originY) {
  const int colWidth = canvasWidth / GRID_COLUMNS;
  const int rowHeight = canvasHeight / MAX_ROW_SPAN;
  for (uint8_t index = 0; index < package.widgetCount; ++index) {
    const WidgetV2& widget = package.widgets[index];
    rectsOut[index] = WidgetRectV2{originX + widget.column * colWidth, originY + widget.row * rowHeight,
                                   widget.columnSpan * colWidth, widget.rowSpan * rowHeight};
  }
}

}  // namespace v2
}  // namespace dashboard
