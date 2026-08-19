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

// Het globale style-byte en het per-widget style-woord gebruiken dezelfde
// bitposities als template 3; de telefoon-app codeert die al zo en de firmware
// mag daar niet van afwijken.
TEST(WidgetGridV2Style, BorderLevelShiftMatchesTemplateThree) {
  EXPECT_EQ(dashboard::v2::GLOBAL_STYLE_BORDER_LEVEL_SHIFT,
            dashboard::GLOBAL_STYLE_BORDER_LEVEL_SHIFT);
}

TEST(WidgetGridV2Style, GlobalBorderLevelExtractsLowBits) {
  EXPECT_EQ(dashboard::v2::globalBorderLevel(0x00), 0);
  EXPECT_EQ(dashboard::v2::globalBorderLevel(0x05), 5);
  EXPECT_EQ(dashboard::v2::globalBorderLevel(0x07), 7);
}

TEST(WidgetGridV2Style, WidgetSizeRungUsesTemplateThreeShift) {
  EXPECT_EQ(dashboard::v2::WIDGET_STYLE_SIZE_RUNG_SHIFT,
            dashboard::WIDGET_STYLE_SIZE_RUNG_SHIFT);
}

TEST(WidgetGridV2Style, WidgetSizeRungExtractsTwoBits) {
  EXPECT_EQ(dashboard::v2::widgetSizeRung(0x0000), 0);
  EXPECT_EQ(dashboard::v2::widgetSizeRung(0x0080), 1);
  EXPECT_EQ(dashboard::v2::widgetSizeRung(0x0100), 2);
  EXPECT_EQ(dashboard::v2::widgetSizeRung(0x0380), 3);
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
  EXPECT_EQ(bytes[31], dashboard::v2::GRID_COLUMNS);
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

TEST(WidgetGridV2Encode, RejectsBorderLevelAboveMax) {
  dashboard::v2::WidgetGridPackageV2 package = minimalPackage();
  package.style = dashboard::MAX_BORDER_LEVEL + 1;
  package.widgets[0] = groupWidget(package, 0, 0, 1, 1, dashboard::v2::GROUP_SHAPE_ARC);
  package.widgetCount = 1;

  dashboard::PackageBytes bytes{};
  size_t length = 0;
  EXPECT_EQ(dashboard::v2::encodeWidgetGridPackageV2(package, bytes, length),
            dashboard::Status::InvalidArgument);
}

void rewriteCrcV2(dashboard::PackageBytes& bytes, const size_t length) {
  const uint32_t crc = dashboard::crc32(bytes.data(), length - dashboard::CRC_SIZE);
  for (size_t index = 0; index < dashboard::CRC_SIZE; ++index) {
    bytes[length - dashboard::CRC_SIZE + index] = static_cast<uint8_t>(crc >> (index * 8U));
  }
}

// De belangrijkste test van dit plan: alles wat een groep draagt moet de
// draad overleven. Loopt de Swift-encoder ooit uit de pas, dan is dit de test
// die het als eerste laat zien.
TEST(WidgetGridV2Decode, GroupRoundTripsEveryField) {
  dashboard::v2::WidgetGridPackageV2 package = minimalPackage();
  package.widgets[0] = groupWidget(package, 0, 1, 12, 3, dashboard::v2::GROUP_SHAPE_ARC);
  package.widgetCount = 1;
  dashboard::v2::GroupContent& group = package.groups[0];
  const char* heading = "Accu's";
  group.headingLength = static_cast<uint8_t>(std::strlen(heading));
  std::copy_n(reinterpret_cast<const uint8_t*>(heading), group.headingLength, group.headingBytes.begin());
  group.itemCount = 2;
  group.items[1].fill = dashboard::v2::FILL_NONE;
  const char* detail = "410 km";
  group.items[1].detailLength = static_cast<uint8_t>(std::strlen(detail));
  std::copy_n(reinterpret_cast<const uint8_t*>(detail), group.items[1].detailLength,
              group.items[1].detailBytes.begin());

  dashboard::PackageBytes bytes{};
  size_t length = 0;
  ASSERT_EQ(dashboard::v2::encodeWidgetGridPackageV2(package, bytes, length), dashboard::Status::Ok);

  dashboard::v2::WidgetGridPackageV2 decoded{};
  ASSERT_EQ(dashboard::v2::decodeWidgetGridPackageV2(bytes.data(), length, decoded), dashboard::Status::Ok);

  ASSERT_EQ(decoded.widgetCount, 1);
  ASSERT_EQ(decoded.groupCount, 1);
  EXPECT_EQ(decoded.widgets[0].type, dashboard::v2::WidgetType::Group);
  EXPECT_EQ(decoded.widgets[0].columnSpan, 12);
  EXPECT_EQ(decoded.widgets[0].rowSpan, 3);
  const dashboard::v2::GroupContent& out = decoded.groups[0];
  EXPECT_EQ(out.shape, dashboard::v2::GROUP_SHAPE_ARC);
  EXPECT_EQ(out.itemCount, 2);
  EXPECT_EQ(out.headingLength, std::strlen("Accu's"));
  EXPECT_EQ(out.items[0].fill, 50);
  EXPECT_EQ(out.items[1].fill, dashboard::v2::FILL_NONE);
  EXPECT_EQ(out.items[1].detailLength, std::strlen("410 km"));
}

TEST(WidgetGridV2Decode, RejectsTemplateThree) {
  dashboard::v2::WidgetGridPackageV2 package = minimalPackage();
  package.widgets[0] = groupWidget(package, 0, 0, 1, 1, dashboard::v2::GROUP_SHAPE_ARC);
  package.widgetCount = 1;
  dashboard::PackageBytes bytes{};
  size_t length = 0;
  ASSERT_EQ(dashboard::v2::encodeWidgetGridPackageV2(package, bytes, length), dashboard::Status::Ok);
  bytes[5] = 3;
  rewriteCrcV2(bytes, length);

  dashboard::v2::WidgetGridPackageV2 decoded{};
  EXPECT_EQ(dashboard::v2::decodeWidgetGridPackageV2(bytes.data(), length, decoded),
            dashboard::Status::UnsupportedTemplate);
}

TEST(WidgetGridV2Decode, RejectsWrongGridColumns) {
  dashboard::v2::WidgetGridPackageV2 package = minimalPackage();
  package.widgets[0] = groupWidget(package, 0, 0, 1, 1, dashboard::v2::GROUP_SHAPE_ARC);
  package.widgetCount = 1;
  dashboard::PackageBytes bytes{};
  size_t length = 0;
  ASSERT_EQ(dashboard::v2::encodeWidgetGridPackageV2(package, bytes, length), dashboard::Status::Ok);
  bytes[31] = 4;
  rewriteCrcV2(bytes, length);

  dashboard::v2::WidgetGridPackageV2 decoded{};
  EXPECT_NE(dashboard::v2::decodeWidgetGridPackageV2(bytes.data(), length, decoded), dashboard::Status::Ok);
}

TEST(WidgetGridV2Decode, RejectsBadCrc) {
  dashboard::v2::WidgetGridPackageV2 package = minimalPackage();
  package.widgets[0] = groupWidget(package, 0, 0, 1, 1, dashboard::v2::GROUP_SHAPE_ARC);
  package.widgetCount = 1;
  dashboard::PackageBytes bytes{};
  size_t length = 0;
  ASSERT_EQ(dashboard::v2::encodeWidgetGridPackageV2(package, bytes, length), dashboard::Status::Ok);
  bytes[length - 1] ^= 0xFF;

  dashboard::v2::WidgetGridPackageV2 decoded{};
  EXPECT_EQ(dashboard::v2::decodeWidgetGridPackageV2(bytes.data(), length, decoded), dashboard::Status::InvalidCrc);
}

TEST(WidgetGridV2Decode, RejectsBorderLevelAboveMax) {
  dashboard::v2::WidgetGridPackageV2 package = minimalPackage();
  package.widgets[0] = groupWidget(package, 0, 0, 1, 1, dashboard::v2::GROUP_SHAPE_ARC);
  package.widgetCount = 1;
  dashboard::PackageBytes bytes{};
  size_t length = 0;
  ASSERT_EQ(dashboard::v2::encodeWidgetGridPackageV2(package, bytes, length), dashboard::Status::Ok);
  bytes[33] = dashboard::MAX_BORDER_LEVEL + 1;
  rewriteCrcV2(bytes, length);

  dashboard::v2::WidgetGridPackageV2 decoded{};
  EXPECT_EQ(dashboard::v2::decodeWidgetGridPackageV2(bytes.data(), length, decoded),
            dashboard::Status::InvalidArgument);
}

TEST(WidgetGridV2Decode, RejectsUnknownWidgetType) {
  dashboard::v2::WidgetGridPackageV2 package = minimalPackage();
  package.widgets[0] = groupWidget(package, 0, 0, 1, 1, dashboard::v2::GROUP_SHAPE_ARC);
  package.widgetCount = 1;
  dashboard::PackageBytes bytes{};
  size_t length = 0;
  ASSERT_EQ(dashboard::v2::encodeWidgetGridPackageV2(package, bytes, length), dashboard::Status::Ok);
  bytes[34] = 9;  // eerste byte van het eerste widget is het type
  rewriteCrcV2(bytes, length);

  dashboard::v2::WidgetGridPackageV2 decoded{};
  EXPECT_NE(dashboard::v2::decodeWidgetGridPackageV2(bytes.data(), length, decoded), dashboard::Status::Ok);
}

// Een groep met één item, geen kop, label "X3", waarde "50%", geen detail.
// Handmatig uitgerekend: widget begint op 34, groepskop op 34+7=41,
// items beginnen op 41+3=44 (kop is leeg), itemvelden 44..47, dan de tekst.
TEST(WidgetGridV2Bytes, GroupLayoutIsAtTheExpectedOffsets) {
  dashboard::v2::WidgetGridPackageV2 package = minimalPackage();
  dashboard::v2::WidgetV2 widget{};
  widget.type = dashboard::v2::WidgetType::Group;
  widget.column = 2;
  widget.row = 3;
  widget.columnSpan = 4;
  widget.rowSpan = 5;
  widget.groupIndex = 0;
  dashboard::v2::GroupContent& group = package.groups[0];
  group.shape = dashboard::v2::GROUP_SHAPE_BAR;
  group.itemCount = 1;
  group.items[0].fill = 79;
  group.items[0].labelLength = 2;
  group.items[0].labelBytes[0] = 'X';
  group.items[0].labelBytes[1] = '3';
  group.items[0].valueLength = 3;
  group.items[0].valueBytes[0] = '5';
  group.items[0].valueBytes[1] = '0';
  group.items[0].valueBytes[2] = '%';
  package.groupCount = 1;
  package.widgets[0] = widget;
  package.widgetCount = 1;

  dashboard::PackageBytes bytes{};
  size_t length = 0;
  ASSERT_EQ(dashboard::v2::encodeWidgetGridPackageV2(package, bytes, length), dashboard::Status::Ok);

  EXPECT_EQ(bytes[34], static_cast<uint8_t>(dashboard::v2::WidgetType::Group));
  EXPECT_EQ(bytes[35], 2);   // column
  EXPECT_EQ(bytes[36], 3);   // row
  EXPECT_EQ(bytes[37], 4);   // columnSpan
  EXPECT_EQ(bytes[38], 5);   // rowSpan
  EXPECT_EQ(bytes[41], dashboard::v2::GROUP_SHAPE_BAR);
  EXPECT_EQ(bytes[42], 0);   // headingLength
  EXPECT_EQ(bytes[43], 1);   // itemCount
  EXPECT_EQ(bytes[44], 2);   // labelLength
  EXPECT_EQ(bytes[45], 3);   // valueLength
  EXPECT_EQ(bytes[46], 0);   // detailLength
  EXPECT_EQ(bytes[47], 79);  // fill
  EXPECT_EQ(bytes[48], 'X');
  EXPECT_EQ(bytes[49], '3');
  EXPECT_EQ(bytes[50], '5');
  EXPECT_EQ(bytes[51], '0');
  EXPECT_EQ(bytes[52], '%');
  // 34 kop + 7 widget + 3 groepskop + 4 itemkop + 5 tekst + 4 CRC = 57
  EXPECT_EQ(length, 57u);
}

// De vulbyte staat vóór de tekst, niet erachter. Zonder deze test schuift een
// verkeerde volgorde ongemerkt door naar de Swift-encoder.
TEST(WidgetGridV2Bytes, FillPrecedesTheTextBytes) {
  dashboard::v2::WidgetGridPackageV2 package = minimalPackage();
  package.widgets[0] = groupWidget(package, 0, 0, 1, 1, dashboard::v2::GROUP_SHAPE_ARC);
  package.widgetCount = 1;
  package.groups[0].items[0].fill = 42;

  dashboard::PackageBytes bytes{};
  size_t length = 0;
  ASSERT_EQ(dashboard::v2::encodeWidgetGridPackageV2(package, bytes, length), dashboard::Status::Ok);

  // groepskop op 41 (shape, headingLength, itemCount), itemkop op 44.
  EXPECT_EQ(bytes[47], 42);
  EXPECT_EQ(bytes[48], 'T');  // eerste teken van "Thuisaccu"
}

}  // namespace
