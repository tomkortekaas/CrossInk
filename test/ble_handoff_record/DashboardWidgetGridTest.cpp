#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <string>

#include "DashboardWidgetGrid.h"

namespace {

template <size_t N>
void setField(std::array<uint8_t, N>& bytes, uint8_t& length, const char* value) {
  length = static_cast<uint8_t>(std::strlen(value));
  std::copy_n(reinterpret_cast<const uint8_t*>(value), length, bytes.begin());
}

dashboard::Widget kpiWidget(uint8_t column, uint8_t row, const char* label, const char* value,
                            uint8_t columnSpan = 1, uint8_t rowSpan = 1) {
  dashboard::Widget widget{};
  widget.type = dashboard::WidgetType::Kpi;
  widget.column = column;
  widget.row = row;
  widget.columnSpan = columnSpan;
  widget.rowSpan = rowSpan;
  setField(widget.kpi.labelBytes, widget.kpi.labelLength, label);
  setField(widget.kpi.valueBytes, widget.kpi.valueLength, value);
  return widget;
}

dashboard::Widget listWidget(uint8_t column, uint8_t row, const char* heading, uint8_t columnSpan = 4,
                             uint8_t rowSpan = 4) {
  dashboard::Widget widget{};
  widget.type = dashboard::WidgetType::List;
  widget.column = column;
  widget.row = row;
  widget.columnSpan = columnSpan;
  widget.rowSpan = rowSpan;
  setField(widget.list.headingBytes, widget.list.headingLength, heading);
  auto& row0 = widget.list.rows[0];
  setField(row0.timeBytes, row0.timeLength, "09:00");
  setField(row0.labelBytes, row0.labelLength, "Stand-up");
  auto& row1 = widget.list.rows[1];
  setField(row1.timeBytes, row1.timeLength, "14:00");
  setField(row1.labelBytes, row1.labelLength, "Tandarts");
  widget.list.rowCount = 2;
  return widget;
}

// Three widgets at distinct, non-overlapping explicit positions: two 1x1 KPIs
// side by side on the top row, a full-width list filling the rows below.
dashboard::WidgetGridPackage validPackage() {
  dashboard::WidgetGridPackage package{};
  package.packageId = 9;
  package.generatedAt = 1000;
  package.validUntil = 2000;
  package.widgets[0] = kpiWidget(/*column=*/0, /*row=*/0, "Stappen", "8421");
  package.widgets[1] = kpiWidget(/*column=*/1, /*row=*/0, "BPM", "72");
  package.widgets[2] = listWidget(/*column=*/0, /*row=*/1, "AGENDA");
  package.widgetCount = 3;
  return package;
}

}  // namespace

TEST(DashboardWidgetGrid, EncodesAndRoundTripsMixedWidgets) {
  dashboard::PackageBytes bytes{};
  size_t length = 0;
  ASSERT_EQ(dashboard::encodeWidgetGridPackage(validPackage(), bytes, length), dashboard::Status::Ok);
  EXPECT_EQ(bytes[5], dashboard::TEMPLATE_WIDGET_GRID);
  EXPECT_EQ(bytes[28], dashboard::GRID_COLUMNS);

  dashboard::WidgetGridPackage decoded{};
  ASSERT_EQ(dashboard::decodeWidgetGridPackage(bytes.data(), length, decoded), dashboard::Status::Ok);
  EXPECT_EQ(decoded.packageId, 9U);
  ASSERT_EQ(decoded.widgetCount, 3U);

  EXPECT_EQ(decoded.widgets[0].type, dashboard::WidgetType::Kpi);
  EXPECT_EQ(decoded.widgets[0].column, 0U);
  EXPECT_EQ(decoded.widgets[0].row, 0U);
  EXPECT_EQ(decoded.widgets[0].columnSpan, 1U);
  EXPECT_TRUE(std::equal(decoded.widgets[0].kpi.labelBytes.begin(),
                        decoded.widgets[0].kpi.labelBytes.begin() + decoded.widgets[0].kpi.labelLength,
                        reinterpret_cast<const uint8_t*>("Stappen")));
  EXPECT_TRUE(std::equal(decoded.widgets[0].kpi.valueBytes.begin(),
                        decoded.widgets[0].kpi.valueBytes.begin() + decoded.widgets[0].kpi.valueLength,
                        reinterpret_cast<const uint8_t*>("8421")));

  EXPECT_EQ(decoded.widgets[2].type, dashboard::WidgetType::List);
  EXPECT_EQ(decoded.widgets[2].column, 0U);
  EXPECT_EQ(decoded.widgets[2].row, 1U);
  EXPECT_EQ(decoded.widgets[2].columnSpan, 4U);
  ASSERT_EQ(decoded.widgets[2].list.rowCount, 2U);
  EXPECT_TRUE(std::equal(decoded.widgets[2].list.rows[1].timeBytes.begin(),
                        decoded.widgets[2].list.rows[1].timeBytes.begin() + 5,
                        reinterpret_cast<const uint8_t*>("14:00")));
}

TEST(DashboardWidgetGrid, AllowsZeroWidgetsAsEmptyDashboard) {
  auto package = validPackage();
  package.widgetCount = 0;
  dashboard::PackageBytes bytes{};
  size_t length = 0;
  ASSERT_EQ(dashboard::encodeWidgetGridPackage(package, bytes, length), dashboard::Status::Ok);
  dashboard::WidgetGridPackage decoded{};
  ASSERT_EQ(dashboard::decodeWidgetGridPackage(bytes.data(), length, decoded), dashboard::Status::Ok);
  EXPECT_EQ(decoded.widgetCount, 0U);
}

TEST(DashboardWidgetGrid, RejectsTooManyWidgets) {
  auto package = validPackage();
  package.widgetCount = dashboard::MAX_WIDGETS + 1;
  dashboard::PackageBytes bytes{};
  size_t length = 0;
  EXPECT_EQ(dashboard::encodeWidgetGridPackage(package, bytes, length), dashboard::Status::InvalidLength);
}

TEST(DashboardWidgetGrid, RejectsSpanOutsideGrid) {
  auto package = validPackage();
  package.widgets[0].columnSpan = dashboard::GRID_COLUMNS + 1;
  dashboard::PackageBytes bytes{};
  size_t length = 0;
  EXPECT_EQ(dashboard::encodeWidgetGridPackage(package, bytes, length), dashboard::Status::InvalidLength);

  package = validPackage();
  package.widgets[0].columnSpan = 0;
  EXPECT_EQ(dashboard::encodeWidgetGridPackage(package, bytes, length), dashboard::Status::InvalidLength);

  package = validPackage();
  package.widgets[0].rowSpan = dashboard::MAX_ROW_SPAN + 1;
  EXPECT_EQ(dashboard::encodeWidgetGridPackage(package, bytes, length), dashboard::Status::InvalidLength);
}

TEST(DashboardWidgetGrid, RejectsPositionOutsideGrid) {
  auto package = validPackage();
  package.widgets[0].column = dashboard::GRID_COLUMNS;  // column alone already out of bounds
  dashboard::PackageBytes bytes{};
  size_t length = 0;
  EXPECT_EQ(dashboard::encodeWidgetGridPackage(package, bytes, length), dashboard::Status::InvalidLength);

  package = validPackage();
  package.widgets[1].column = 3;
  package.widgets[1].columnSpan = 2;  // in-bounds column, but column + span overflows GRID_COLUMNS
  EXPECT_EQ(dashboard::encodeWidgetGridPackage(package, bytes, length), dashboard::Status::InvalidLength);

  package = validPackage();
  package.widgets[2].row = dashboard::MAX_ROW_SPAN;
  EXPECT_EQ(dashboard::encodeWidgetGridPackage(package, bytes, length), dashboard::Status::InvalidLength);
}

TEST(DashboardWidgetGrid, RejectsOverlappingWidgets) {
  auto package = validPackage();
  // Moves widget 1 on top of widget 0's cell.
  package.widgets[1].column = 0;
  package.widgets[1].row = 0;
  dashboard::PackageBytes bytes{};
  size_t length = 0;
  EXPECT_EQ(dashboard::encodeWidgetGridPackage(package, bytes, length), dashboard::Status::InvalidArgument);
}

TEST(DashboardWidgetGrid, AllowsAdjacentNonOverlappingWidgets) {
  auto package = validPackage();
  dashboard::PackageBytes bytes{};
  size_t length = 0;
  // validPackage() itself is already a non-overlapping arrangement.
  EXPECT_EQ(dashboard::encodeWidgetGridPackage(package, bytes, length), dashboard::Status::Ok);
}

TEST(DashboardWidgetGrid, RejectsEmptyKpiLabelOrValue) {
  auto package = validPackage();
  package.widgets[0].kpi.labelLength = 0;
  dashboard::PackageBytes bytes{};
  size_t length = 0;
  EXPECT_EQ(dashboard::encodeWidgetGridPackage(package, bytes, length), dashboard::Status::InvalidLength);

  package = validPackage();
  package.widgets[0].kpi.valueLength = 0;
  EXPECT_EQ(dashboard::encodeWidgetGridPackage(package, bytes, length), dashboard::Status::InvalidLength);
}

TEST(DashboardWidgetGrid, AllowsEmptyListHeadingButRequiresRowText) {
  auto package = validPackage();
  package.widgets[2].list.headingLength = 0;
  dashboard::PackageBytes bytes{};
  size_t length = 0;
  EXPECT_EQ(dashboard::encodeWidgetGridPackage(package, bytes, length), dashboard::Status::Ok);

  package = validPackage();
  package.widgets[2].list.rows[0].timeLength = 0;
  EXPECT_EQ(dashboard::encodeWidgetGridPackage(package, bytes, length), dashboard::Status::InvalidLength);
}

TEST(DashboardWidgetGrid, RejectsOversizedPackage) {
  auto package = validPackage();
  for (uint8_t index = 0; index < dashboard::MAX_WIDGETS; ++index) {
    // Stack every widget in its own row so none overlap; MAX_WIDGETS(8) <= MAX_ROW_SPAN(6) is false,
    // so wrap into two columns of up to MAX_ROW_SPAN rows each.
    const uint8_t column = static_cast<uint8_t>((index / dashboard::MAX_ROW_SPAN) * 2);
    const uint8_t row = static_cast<uint8_t>(index % dashboard::MAX_ROW_SPAN);
    auto& widget = package.widgets[index];
    widget = listWidget(column, row, "Vandaag", /*columnSpan=*/2, /*rowSpan=*/1);
    for (auto& listRow : widget.list.rows) {
      setField(listRow.timeBytes, listRow.timeLength, "00:00-23:59");
      listRow.labelLength = dashboard::MAX_LIST_ROW_LABEL_SIZE;
      std::fill_n(listRow.labelBytes.begin(), listRow.labelLength, 'a');
    }
    widget.list.rowCount = dashboard::MAX_LIST_ROWS;
  }
  package.widgetCount = dashboard::MAX_WIDGETS;
  dashboard::PackageBytes bytes{};
  size_t length = 0;
  EXPECT_EQ(dashboard::encodeWidgetGridPackage(package, bytes, length), dashboard::Status::InvalidLength);
}

TEST(DashboardWidgetGrid, RejectsInvalidUtf8) {
  auto package = validPackage();
  package.widgets[0].kpi.labelBytes[0] = 0xFF;
  package.widgets[0].kpi.labelLength = 1;
  dashboard::PackageBytes bytes{};
  size_t length = 0;
  EXPECT_EQ(dashboard::encodeWidgetGridPackage(package, bytes, length), dashboard::Status::InvalidUtf8);
}

TEST(DashboardWidgetGrid, DecodeRejectsCorruptionAndWrongTemplate) {
  dashboard::PackageBytes bytes{};
  size_t length = 0;
  ASSERT_EQ(dashboard::encodeWidgetGridPackage(validPackage(), bytes, length), dashboard::Status::Ok);
  bytes[length - 1] ^= 1;
  dashboard::WidgetGridPackage decoded{};
  EXPECT_EQ(dashboard::decodeWidgetGridPackage(bytes.data(), length, decoded), dashboard::Status::InvalidCrc);

  ASSERT_EQ(dashboard::encodeWidgetGridPackage(validPackage(), bytes, length), dashboard::Status::Ok);
  bytes[5] = dashboard::TEMPLATE_AGENDA;
  EXPECT_EQ(dashboard::decodeWidgetGridPackage(bytes.data(), length, decoded), dashboard::Status::UnsupportedTemplate);
}

TEST(DashboardWidgetGrid, RejectsInvalidTimestamp) {
  auto package = validPackage();
  package.validUntil = package.generatedAt - 1;
  dashboard::PackageBytes bytes{};
  size_t length = 0;
  EXPECT_EQ(dashboard::encodeWidgetGridPackage(package, bytes, length), dashboard::Status::InvalidTimestamp);
}

// peekPackageHeader is the only validation the persistence layer runs, and it
// is documented as template-agnostic. Its MIN_PACKAGE_SIZE, however, is
// TEMPLATE_AGENDA's 32-byte fixed header plus the CRC. A widget grid's content
// starts at byte 30, so a valid grid package carrying no widgets is 34 bytes
// and used to be rejected as InvalidSize by peek while decodeWidgetGridPackage
// accepted it - meaning such a package could never be persisted.
TEST(DashboardWidgetGrid, PeekAcceptsWidgetGridPackageWithoutWidgets) {
  dashboard::WidgetGridPackage package{};
  package.packageId = 5;
  package.generatedAt = 500;
  package.validUntil = 1500;
  package.widgetCount = 0;

  dashboard::PackageBytes bytes{};
  size_t length = 0;
  ASSERT_EQ(dashboard::encodeWidgetGridPackage(package, bytes, length), dashboard::Status::Ok);
  ASSERT_EQ(length, 34u);

  dashboard::PackageHeader header{};
  EXPECT_EQ(dashboard::peekPackageHeader(bytes.data(), length, header), dashboard::Status::Ok);
  EXPECT_EQ(header.packageId, 5u);
  EXPECT_EQ(header.templateId, dashboard::TEMPLATE_WIDGET_GRID);
}
