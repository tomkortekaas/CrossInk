#include <gtest/gtest.h>

#include "DashboardV3Layout.h"

namespace {

TEST(DashboardV3Layout, PortraitCanvasMatchesApprovedBandsAndColumns) {
  const auto layout = dashboard::v3::computeDashboardV3Layout(528, 792, {});

  EXPECT_EQ(layout.header, (dashboard::v3::Rect{0, 0, 528, 77}));
  EXPECT_EQ(layout.rain, (dashboard::v3::Rect{0, 77, 528, 117}));
  EXPECT_EQ(layout.traffic, (dashboard::v3::Rect{0, 194, 528, 77}));
  EXPECT_EQ(layout.body, (dashboard::v3::Rect{0, 271, 528, 459}));
  EXPECT_EQ(layout.footer, (dashboard::v3::Rect{0, 730, 528, 62}));
  EXPECT_EQ(layout.bodyLeft, (dashboard::v3::Rect{0, 271, 270, 459}));
  EXPECT_EQ(layout.bodyRight, (dashboard::v3::Rect{270, 271, 258, 459}));
}

TEST(DashboardV3Layout, SafeInsetsKeepEveryBandInsideTheUsableCanvas) {
  const dashboard::v3::Insets safe{9, 15, 9, 9};
  const auto layout = dashboard::v3::computeDashboardV3Layout(528, 792, safe);

  EXPECT_EQ(layout.header.x, 9);
  EXPECT_EQ(layout.header.y, 15);
  EXPECT_EQ(layout.header.width, 510);
  EXPECT_EQ(layout.footer.y + layout.footer.height, 783);
  EXPECT_EQ(layout.bodyLeft.width + layout.bodyRight.width, 510);
  EXPECT_EQ(layout.bodyRight.x, layout.bodyLeft.x + layout.bodyLeft.width);
}

TEST(DashboardV3Layout, InvalidInsetsProduceAnEmptyLayout) {
  const auto layout = dashboard::v3::computeDashboardV3Layout(20, 20, {11, 0, 10, 0});
  EXPECT_EQ(layout.header.width, 0);
  EXPECT_EQ(layout.footer.height, 0);
}

}  // namespace
