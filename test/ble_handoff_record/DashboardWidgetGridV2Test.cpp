#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <cstring>

#include "DashboardWidgetGridV2.h"

namespace {

dashboard::v2::WidgetV2 groupWidget(dashboard::v2::WidgetGridPackageV2& package, uint8_t column, uint8_t row,
                                    uint8_t columnSpan, uint8_t rowSpan, uint8_t shape) {
  dashboard::v2::WidgetV2 widget{};
  widget.type = dashboard::v2::WidgetType::Group;
  widget.column = column;
  widget.row = row;
  widget.columnSpan = columnSpan;
  widget.rowSpan = rowSpan;
  widget.groupIndex = package.groupCount;
  dashboard::v2::GroupContent& group = package.groups[package.groupCount++];
  group.shape = shape;
  group.itemCount = 1;
  group.items[0].fill = 50;
  const char* label = "Thuisaccu";
  group.items[0].labelLength = static_cast<uint8_t>(std::strlen(label));
  std::copy_n(reinterpret_cast<const uint8_t*>(label), group.items[0].labelLength,
              group.items[0].labelBytes.begin());
  const char* value = "19%";
  group.items[0].valueLength = static_cast<uint8_t>(std::strlen(value));
  std::copy_n(reinterpret_cast<const uint8_t*>(value), group.items[0].valueLength,
              group.items[0].valueBytes.begin());
  return widget;
}

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

TEST(WidgetGridV2Validate, AcceptsFullWidthWidget) {
  dashboard::v2::WidgetGridPackageV2 package{};
  package.widgets[0] = groupWidget(package, 0, 11, 12, 1, dashboard::v2::GROUP_SHAPE_STRIP);
  package.widgetCount = 1;
  EXPECT_EQ(dashboard::v2::validateWidgetV2(package, package.widgets[0]), dashboard::Status::Ok);
}

TEST(WidgetGridV2Validate, RejectsColumnPastGrid) {
  dashboard::v2::WidgetGridPackageV2 package{};
  package.widgets[0] = groupWidget(package, 12, 0, 1, 1, dashboard::v2::GROUP_SHAPE_ARC);
  package.widgetCount = 1;
  EXPECT_NE(dashboard::v2::validateWidgetV2(package, package.widgets[0]), dashboard::Status::Ok);
}

TEST(WidgetGridV2Validate, RejectsSpanPastRightEdge) {
  dashboard::v2::WidgetGridPackageV2 package{};
  package.widgets[0] = groupWidget(package, 10, 0, 3, 1, dashboard::v2::GROUP_SHAPE_ARC);
  package.widgetCount = 1;
  EXPECT_NE(dashboard::v2::validateWidgetV2(package, package.widgets[0]), dashboard::Status::Ok);
}

TEST(WidgetGridV2Validate, RejectsSpanPastBottomEdge) {
  dashboard::v2::WidgetGridPackageV2 package{};
  package.widgets[0] = groupWidget(package, 0, 10, 1, 3, dashboard::v2::GROUP_SHAPE_ARC);
  package.widgetCount = 1;
  EXPECT_NE(dashboard::v2::validateWidgetV2(package, package.widgets[0]), dashboard::Status::Ok);
}

TEST(WidgetGridV2Validate, RejectsUnknownShape) {
  dashboard::v2::WidgetGridPackageV2 package{};
  package.widgets[0] = groupWidget(package, 0, 0, 1, 1, dashboard::v2::MAX_GROUP_SHAPE + 1);
  package.widgetCount = 1;
  EXPECT_NE(dashboard::v2::validateWidgetV2(package, package.widgets[0]), dashboard::Status::Ok);
}

// 101 is geen geldig percentage; FILL_NONE wel, want dat betekent "geen meter".
TEST(WidgetGridV2Validate, RejectsFillAboveHundredButAllowsFillNone) {
  dashboard::v2::WidgetGridPackageV2 package{};
  package.widgets[0] = groupWidget(package, 0, 0, 1, 1, dashboard::v2::GROUP_SHAPE_ARC);
  package.widgetCount = 1;
  package.groups[0].items[0].fill = 101;
  EXPECT_NE(dashboard::v2::validateWidgetV2(package, package.widgets[0]), dashboard::Status::Ok);
  package.groups[0].items[0].fill = dashboard::v2::FILL_NONE;
  EXPECT_EQ(dashboard::v2::validateWidgetV2(package, package.widgets[0]), dashboard::Status::Ok);
}

TEST(WidgetGridV2Validate, RejectsEmptyGroup) {
  dashboard::v2::WidgetGridPackageV2 package{};
  package.widgets[0] = groupWidget(package, 0, 0, 1, 1, dashboard::v2::GROUP_SHAPE_ARC);
  package.widgetCount = 1;
  package.groups[0].itemCount = 0;
  EXPECT_NE(dashboard::v2::validateWidgetV2(package, package.widgets[0]), dashboard::Status::Ok);
}

TEST(WidgetGridV2Validate, RejectsTooManyItems) {
  dashboard::v2::WidgetGridPackageV2 package{};
  package.widgets[0] = groupWidget(package, 0, 0, 1, 1, dashboard::v2::GROUP_SHAPE_ARC);
  package.widgetCount = 1;
  package.groups[0].itemCount = static_cast<uint8_t>(dashboard::v2::MAX_GROUP_ITEMS + 1);
  EXPECT_NE(dashboard::v2::validateWidgetV2(package, package.widgets[0]), dashboard::Status::Ok);
}

dashboard::v2::WidgetGridPackageV2 minimalPackage() {
  dashboard::v2::WidgetGridPackageV2 package{};
  package.packageId = 7;
  package.generatedAt = 1000;
  package.validUntil = 2000;
  return package;
}

TEST(WidgetGridV2Encode, LengthFieldMatchesActualLength) {
  dashboard::v2::WidgetGridPackageV2 package = minimalPackage();
  package.widgets[0] = groupWidget(package, 0, 0, 12, 1, dashboard::v2::GROUP_SHAPE_STRIP);
  package.widgetCount = 1;

  dashboard::PackageBytes bytes{};
  size_t length = 0;
  ASSERT_EQ(dashboard::v2::encodeWidgetGridPackageV2(package, bytes, length), dashboard::Status::Ok);

  const uint16_t declared = static_cast<uint16_t>(bytes[6]) | (static_cast<uint16_t>(bytes[7]) << 8);
  EXPECT_EQ(declared, length);
  EXPECT_EQ(bytes[5], dashboard::v2::TEMPLATE_WIDGET_GRID_V2);
  EXPECT_EQ(bytes[28], dashboard::v2::GRID_COLUMNS);
}

TEST(WidgetGridV2Encode, RejectsOverlappingWidgets) {
  dashboard::v2::WidgetGridPackageV2 package = minimalPackage();
  package.widgets[0] = groupWidget(package, 0, 0, 6, 2, dashboard::v2::GROUP_SHAPE_ARC);
  package.widgets[1] = groupWidget(package, 3, 1, 6, 2, dashboard::v2::GROUP_SHAPE_ARC);
  package.widgetCount = 2;

  dashboard::PackageBytes bytes{};
  size_t length = 0;
  EXPECT_NE(dashboard::v2::encodeWidgetGridPackageV2(package, bytes, length), dashboard::Status::Ok);
}

TEST(WidgetGridV2Encode, RejectsMissingTimestamps) {
  dashboard::v2::WidgetGridPackageV2 package = minimalPackage();
  package.generatedAt = 0;
  package.widgets[0] = groupWidget(package, 0, 0, 1, 1, dashboard::v2::GROUP_SHAPE_ARC);
  package.widgetCount = 1;

  dashboard::PackageBytes bytes{};
  size_t length = 0;
  EXPECT_NE(dashboard::v2::encodeWidgetGridPackageV2(package, bytes, length), dashboard::Status::Ok);
}

}  // namespace
