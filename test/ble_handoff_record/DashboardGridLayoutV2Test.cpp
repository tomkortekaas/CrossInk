#include <gtest/gtest.h>

#include <array>

#include "DashboardGridLayoutV2.h"

namespace {

TEST(GridLayoutV2, CellIsFortyTwoBySixtyFour) {
  dashboard::v2::WidgetGridPackageV2 package{};
  package.widgetCount = 1;
  package.widgets[0].column = 0;
  package.widgets[0].row = 0;
  package.widgets[0].columnSpan = 1;
  package.widgets[0].rowSpan = 1;
  std::array<dashboard::v2::WidgetRectV2, dashboard::v2::MAX_WIDGETS> rects{};
  dashboard::v2::computeGridLayoutV2(package, 510, 768, rects, 9, 15);
  EXPECT_EQ(rects[0].x, 9);
  EXPECT_EQ(rects[0].y, 15);
  EXPECT_EQ(rects[0].width, 42);
  EXPECT_EQ(rects[0].height, 64);
}

TEST(GridLayoutV2, SpansMultiplyTheCell) {
  dashboard::v2::WidgetGridPackageV2 package{};
  package.widgetCount = 1;
  package.widgets[0].column = 3;
  package.widgets[0].row = 2;
  package.widgets[0].columnSpan = 6;
  package.widgets[0].rowSpan = 4;
  std::array<dashboard::v2::WidgetRectV2, dashboard::v2::MAX_WIDGETS> rects{};
  dashboard::v2::computeGridLayoutV2(package, 510, 768, rects, 9, 15);
  EXPECT_EQ(rects[0].x, 9 + 3 * 42);
  EXPECT_EQ(rects[0].y, 15 + 2 * 64);
  EXPECT_EQ(rects[0].width, 6 * 42);
  EXPECT_EQ(rects[0].height, 4 * 64);
}

// De volle breedte laat 6 px onbenut door de deling; dat is bekend en bewust.
TEST(GridLayoutV2, FullWidthLeavesTheRoundingRemainder) {
  dashboard::v2::WidgetGridPackageV2 package{};
  package.widgetCount = 1;
  package.widgets[0].columnSpan = 12;
  package.widgets[0].rowSpan = 12;
  std::array<dashboard::v2::WidgetRectV2, dashboard::v2::MAX_WIDGETS> rects{};
  dashboard::v2::computeGridLayoutV2(package, 510, 768, rects, 9, 15);
  EXPECT_EQ(rects[0].width, 504);
  EXPECT_EQ(rects[0].height, 768);
}

}  // namespace
