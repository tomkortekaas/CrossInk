#include <gtest/gtest.h>

#include <cstdint>

#include "DashboardWidgetGridV2.h"

namespace {

TEST(WidgetGridV2Constants, GridIsTwelveByTwelve) {
  EXPECT_EQ(dashboard::v2::GRID_COLUMNS, 12);
  EXPECT_EQ(dashboard::v2::MAX_ROW_SPAN, 12);
}

TEST(WidgetGridV2Constants, TemplateIdIsFour) {
  EXPECT_EQ(dashboard::v2::TEMPLATE_WIDGET_GRID_V2, 4);
}

// MAX_WIDGETS mag niet het product van kolommen en rijen zijn: dat zou 144
// worden en de widget-array onbetaalbaar maken.
TEST(WidgetGridV2Constants, MaxWidgetsIsNotTheProduct) {
  EXPECT_EQ(dashboard::v2::MAX_WIDGETS, 16u);
  EXPECT_LT(dashboard::v2::MAX_WIDGETS,
            static_cast<size_t>(dashboard::v2::GRID_COLUMNS) * dashboard::v2::MAX_ROW_SPAN);
}

}  // namespace
