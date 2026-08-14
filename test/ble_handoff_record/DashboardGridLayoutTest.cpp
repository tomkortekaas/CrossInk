#include <gtest/gtest.h>

#include "DashboardGridLayout.h"

namespace {

dashboard::Widget widget(uint8_t columnSpan, uint8_t rowSpan) {
  dashboard::Widget w{};
  w.columnSpan = columnSpan;
  w.rowSpan = rowSpan;
  return w;
}

}  // namespace

TEST(DashboardGridLayout, SingleFullWidthWidgetFillsTopRow) {
  dashboard::WidgetGridPackage package{};
  package.widgets[0] = widget(4, 1);
  package.widgetCount = 1;

  std::array<dashboard::WidgetRect, dashboard::MAX_WIDGETS> rects{};
  dashboard::computeGridLayout(package, 480, 600, rects);

  EXPECT_EQ(rects[0].x, 0);
  EXPECT_EQ(rects[0].y, 0);
  EXPECT_EQ(rects[0].width, 480);
  EXPECT_EQ(rects[0].height, 100);  // 600 / MAX_ROW_SPAN(6) = 100
}

TEST(DashboardGridLayout, TwoHalfWidthWidgetsSitSideBySide) {
  dashboard::WidgetGridPackage package{};
  package.widgets[0] = widget(2, 1);
  package.widgets[1] = widget(2, 1);
  package.widgetCount = 2;

  std::array<dashboard::WidgetRect, dashboard::MAX_WIDGETS> rects{};
  dashboard::computeGridLayout(package, 480, 600, rects);

  EXPECT_EQ(rects[0].x, 0);
  EXPECT_EQ(rects[1].x, 240);
  EXPECT_EQ(rects[0].y, rects[1].y);
}

TEST(DashboardGridLayout, FourSingleColumnWidgetsFillOneRowThenFifthWraps) {
  dashboard::WidgetGridPackage package{};
  for (int i = 0; i < 5; ++i) package.widgets[i] = widget(1, 1);
  package.widgetCount = 5;

  std::array<dashboard::WidgetRect, dashboard::MAX_WIDGETS> rects{};
  dashboard::computeGridLayout(package, 480, 600, rects);

  EXPECT_EQ(rects[0].x, 0);
  EXPECT_EQ(rects[1].x, 120);
  EXPECT_EQ(rects[2].x, 240);
  EXPECT_EQ(rects[3].x, 360);
  EXPECT_EQ(rects[0].y, 0);
  EXPECT_EQ(rects[4].x, 0);
  EXPECT_EQ(rects[4].y, 100);
}

TEST(DashboardGridLayout, NextShelfClearsTheTallestWidgetInThePreviousRow) {
  dashboard::WidgetGridPackage package{};
  package.widgets[0] = widget(2, 2);  // tall widget, left half
  package.widgets[1] = widget(2, 1);  // short widget, right half, same row
  package.widgets[2] = widget(4, 1);  // wraps after the row; must clear the tall widget
  package.widgetCount = 3;

  std::array<dashboard::WidgetRect, dashboard::MAX_WIDGETS> rects{};
  dashboard::computeGridLayout(package, 480, 600, rects);

  EXPECT_EQ(rects[0].y, 0);
  EXPECT_EQ(rects[0].height, 200);  // 2 row units
  EXPECT_EQ(rects[1].y, 0);
  EXPECT_EQ(rects[1].height, 100);  // 1 row unit
  EXPECT_EQ(rects[2].y, 200);       // clears widget 0's height, not widget 1's
}

TEST(DashboardGridLayout, ZeroWidgetsProducesNoLayout) {
  dashboard::WidgetGridPackage package{};
  package.widgetCount = 0;
  std::array<dashboard::WidgetRect, dashboard::MAX_WIDGETS> rects{};
  EXPECT_NO_THROW(dashboard::computeGridLayout(package, 480, 600, rects));
}
