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

dashboard::Widget kpiWidget(const char* label, const char* value, uint8_t columnSpan = 1, uint8_t rowSpan = 1) {
  dashboard::Widget widget{};
  widget.type = dashboard::WidgetType::Kpi;
  widget.columnSpan = columnSpan;
  widget.rowSpan = rowSpan;
  setField(widget.kpi.labelBytes, widget.kpi.labelLength, label);
  setField(widget.kpi.valueBytes, widget.kpi.valueLength, value);
  return widget;
}

dashboard::Widget listWidget(const char* heading, uint8_t columnSpan = 4, uint8_t rowSpan = 4) {
  dashboard::Widget widget{};
  widget.type = dashboard::WidgetType::List;
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

dashboard::WidgetGridPackage validPackage() {
  dashboard::WidgetGridPackage package{};
  package.packageId = 9;
  package.generatedAt = 1000;
  package.validUntil = 2000;
  package.widgets[0] = kpiWidget("Stappen", "8421");
  package.widgets[1] = kpiWidget("BPM", "72");
  package.widgets[2] = listWidget("AGENDA");
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
  EXPECT_EQ(decoded.widgets[0].columnSpan, 1U);
  EXPECT_TRUE(std::equal(decoded.widgets[0].kpi.labelBytes.begin(),
                        decoded.widgets[0].kpi.labelBytes.begin() + decoded.widgets[0].kpi.labelLength,
                        reinterpret_cast<const uint8_t*>("Stappen")));
  EXPECT_TRUE(std::equal(decoded.widgets[0].kpi.valueBytes.begin(),
                        decoded.widgets[0].kpi.valueBytes.begin() + decoded.widgets[0].kpi.valueLength,
                        reinterpret_cast<const uint8_t*>("8421")));

  EXPECT_EQ(decoded.widgets[2].type, dashboard::WidgetType::List);
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
  for (auto& widget : package.widgets) {
    widget = listWidget("Vandaag");
    for (auto& row : widget.list.rows) {
      setField(row.timeBytes, row.timeLength, "00:00-23:59");
      row.labelLength = dashboard::MAX_LIST_ROW_LABEL_SIZE;
      std::fill_n(row.labelBytes.begin(), row.labelLength, 'a');
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
