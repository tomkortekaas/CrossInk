#include <gtest/gtest.h>

#include "DashboardGridLayout.h"

namespace {

dashboard::Widget widget(uint8_t column, uint8_t row, uint8_t columnSpan, uint8_t rowSpan) {
  dashboard::Widget w{};
  w.column = column;
  w.row = row;
  w.columnSpan = columnSpan;
  w.rowSpan = rowSpan;
  return w;
}

}  // namespace

TEST(DashboardGridLayout, PlacesWidgetAtItsExplicitColumnAndRow) {
  dashboard::WidgetGridPackage package{};
  package.widgets[0] = widget(/*column=*/1, /*row=*/2, /*columnSpan=*/2, /*rowSpan=*/1);
  package.widgetCount = 1;

  std::array<dashboard::WidgetRect, dashboard::MAX_WIDGETS> rects{};
  dashboard::computeGridLayout(package, 480, 600, rects);

  // colWidth = 480 / 4 = 120, rowUnitHeight = 600 / 6 = 100
  EXPECT_EQ(rects[0].x, 120);
  EXPECT_EQ(rects[0].y, 200);
  EXPECT_EQ(rects[0].width, 240);
  EXPECT_EQ(rects[0].height, 100);
}

TEST(DashboardGridLayout, FullWidthFullHeightWidgetFillsTheCanvas) {
  dashboard::WidgetGridPackage package{};
  package.widgets[0] = widget(0, 0, dashboard::GRID_COLUMNS, dashboard::MAX_ROW_SPAN);
  package.widgetCount = 1;

  std::array<dashboard::WidgetRect, dashboard::MAX_WIDGETS> rects{};
  dashboard::computeGridLayout(package, 480, 600, rects);

  EXPECT_EQ(rects[0].x, 0);
  EXPECT_EQ(rects[0].y, 0);
  EXPECT_EQ(rects[0].width, 480);
  EXPECT_EQ(rects[0].height, 600);
}

// Placement is free-form: each widget's rectangle comes only from its own
// column/row/columnSpan/rowSpan, never from where it sits in the widgets
// array or from any other widget's placement.
TEST(DashboardGridLayout, EachWidgetsRectDependsOnlyOnItsOwnPositionNotArrayOrder) {
  dashboard::WidgetGridPackage package{};
  // Deliberately out of "reading order": the bottom-right widget is first.
  package.widgets[0] = widget(/*column=*/2, /*row=*/4, /*columnSpan=*/2, /*rowSpan=*/2);
  package.widgets[1] = widget(/*column=*/0, /*row=*/0, /*columnSpan=*/1, /*rowSpan=*/1);
  package.widgetCount = 2;

  std::array<dashboard::WidgetRect, dashboard::MAX_WIDGETS> rects{};
  dashboard::computeGridLayout(package, 480, 600, rects);

  // colWidth = 120, rowUnitHeight = 100
  EXPECT_EQ(rects[0].x, 240);
  EXPECT_EQ(rects[0].y, 400);
  EXPECT_EQ(rects[1].x, 0);
  EXPECT_EQ(rects[1].y, 0);
}

TEST(DashboardGridLayout, ZeroWidgetsProducesNoLayout) {
  dashboard::WidgetGridPackage package{};
  package.widgetCount = 0;
  std::array<dashboard::WidgetRect, dashboard::MAX_WIDGETS> rects{};
  EXPECT_NO_THROW(dashboard::computeGridLayout(package, 480, 600, rects));
}
