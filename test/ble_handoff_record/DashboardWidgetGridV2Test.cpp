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

TEST(WidgetGridV2Group, ShapesAreNamed) {
  EXPECT_EQ(dashboard::v2::GROUP_SHAPE_ARC, 0);
  EXPECT_EQ(dashboard::v2::GROUP_SHAPE_BAR, 1);
  EXPECT_EQ(dashboard::v2::GROUP_SHAPE_STRIP, 2);
  EXPECT_EQ(dashboard::v2::MAX_GROUP_SHAPE, 2);
}

// Een enkele meter is dit widget met itemCount 1 en geen kop; een cluster is
// hetzelfde widget met meer items. Daarom moet 1 een geldige itemCount zijn.
TEST(WidgetGridV2Group, SingleItemIsValid) {
  dashboard::v2::GroupContent group{};
  group.itemCount = 1;
  EXPECT_LE(group.itemCount, dashboard::v2::MAX_GROUP_ITEMS);
}

// FILL_NONE onderscheidt "geen meter" (strip) van "meter op nul" (lege accu).
// Zonder dat verschil tekent een lege thuisaccu hetzelfde als een tekstitem.
TEST(WidgetGridV2Group, FillNoneIsDistinctFromZero) {
  EXPECT_NE(dashboard::v2::FILL_NONE, 0);
  EXPECT_GT(dashboard::v2::FILL_NONE, dashboard::v2::MAX_FILL);
}

TEST(WidgetGridV2Group, WidgetTypeGroupIsFour) {
  EXPECT_EQ(static_cast<uint8_t>(dashboard::v2::WidgetType::Group), 4);
}

TEST(WidgetGridV2Package, DefaultsAreSchemaTwoTemplateFour) {
  dashboard::v2::WidgetGridPackageV2 package{};
  EXPECT_EQ(package.schema, dashboard::SCHEMA_V2);
  EXPECT_EQ(package.templateId, dashboard::v2::TEMPLATE_WIDGET_GRID_V2);
}

// listIndex, dateField en groupIndex delen één byte omdat een widget nooit
// twee van de drie tegelijk is. Zou dat uit elkaar getrokken worden, dan groeit
// WidgetV2 en daarmee de hele array.
TEST(WidgetGridV2Package, IndexFieldsShareOneByte) {
  dashboard::v2::WidgetV2 widget{};
  widget.groupIndex = 3;
  EXPECT_EQ(widget.listIndex, 3);
  widget.listIndex = 5;
  EXPECT_EQ(widget.groupIndex, 5);
}

// De gedecodeerde package staat static (permanent DRAM), niet op de stack.
// Deze grens bewaakt dat het formaat niet ongemerkt opzwelt.
TEST(WidgetGridV2Package, FitsTheMemoryBudget) {
  EXPECT_LE(sizeof(dashboard::v2::WidgetGridPackageV2), 5120u);
}

}  // namespace
