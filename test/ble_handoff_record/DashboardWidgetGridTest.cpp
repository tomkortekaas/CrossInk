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

void rewriteCrc(dashboard::PackageBytes& bytes, const size_t length) {
  const uint32_t crc = dashboard::crc32(bytes.data(), length - dashboard::CRC_SIZE);
  for (size_t index = 0; index < dashboard::CRC_SIZE; ++index) {
    bytes[length - dashboard::CRC_SIZE + index] = static_cast<uint8_t>(crc >> (index * 8U));
  }
}

dashboard::Widget kpiWidget(uint8_t column, uint8_t row, const char* label, const char* value, uint8_t columnSpan = 1,
                            uint8_t rowSpan = 1) {
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

// List content lives in the package, so building a list widget also claims one
// of its MAX_LIST_WIDGETS content slots.
dashboard::Widget listWidget(dashboard::WidgetGridPackage& package, uint8_t column, uint8_t row, const char* heading,
                             uint8_t columnSpan = 4, uint8_t rowSpan = 4) {
  dashboard::Widget widget{};
  widget.type = dashboard::WidgetType::List;
  widget.column = column;
  widget.row = row;
  widget.columnSpan = columnSpan;
  widget.rowSpan = rowSpan;
  widget.listIndex = package.listCount;
  dashboard::ListContent& list = package.lists[package.listCount++];
  setField(list.headingBytes, list.headingLength, heading);
  auto& row0 = list.rows[0];
  setField(row0.timeBytes, row0.timeLength, "09:00");
  setField(row0.labelBytes, row0.labelLength, "Stand-up");
  auto& row1 = list.rows[1];
  setField(row1.timeBytes, row1.timeLength, "14:00");
  setField(row1.labelBytes, row1.labelLength, "Tandarts");
  list.rowCount = 2;
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
  package.widgets[2] = listWidget(package, /*column=*/0, /*row=*/1, "AGENDA");
  package.widgetCount = 3;
  return package;
}

dashboard::Widget dateWidget(uint8_t column, uint8_t row, dashboard::DateField field, uint8_t columnSpan = 1,
                             uint8_t rowSpan = 1) {
  dashboard::Widget widget{};
  widget.type = dashboard::WidgetType::Date;
  widget.column = column;
  widget.row = row;
  widget.columnSpan = columnSpan;
  widget.rowSpan = rowSpan;
  widget.dateField = field;
  return widget;
}

}  // namespace

TEST(DashboardWidgetGrid, EncodesSchemaV2WithGlobalStyleAndContentAtOffset31) {
  auto package = validPackage();
  dashboard::PackageBytes bytes{};
  size_t length = 0;
  ASSERT_EQ(dashboard::encodeWidgetGridPackage(package, bytes, length), dashboard::Status::Ok);
  EXPECT_EQ(bytes[4], 2);   // TEMPLATE_WIDGET_GRID moved to schema 2
  EXPECT_EQ(bytes[30], 0);  // global style byte defaults to zero
  EXPECT_EQ(bytes[31], static_cast<uint8_t>(dashboard::WidgetType::Kpi));
}

TEST(DashboardWidgetGrid, RoundTripsGlobalAndPerWidgetStyle) {
  auto package = validPackage();
  package.style = static_cast<uint8_t>(2U | (1U << dashboard::GLOBAL_STYLE_DENSITY_SHIFT) |
                                       (1U << dashboard::GLOBAL_STYLE_LIST_DIVIDERS_SHIFT));
  package.widgets[0].style = dashboard::makeWidgetStyle(/*iconId=*/64, /*sizeRung=*/3, /*emphasis=*/2);
  package.widgets[1].style = dashboard::makeWidgetStyle(/*iconId=*/1, /*sizeRung=*/0, /*emphasis=*/0);
  package.widgets[2].style = dashboard::makeWidgetStyle(/*iconId=*/0, /*sizeRung=*/2, /*emphasis=*/3);

  dashboard::PackageBytes bytes{};
  size_t length = 0;
  ASSERT_EQ(dashboard::encodeWidgetGridPackage(package, bytes, length), dashboard::Status::Ok);

  dashboard::WidgetGridPackage decoded{};
  ASSERT_EQ(dashboard::decodeWidgetGridPackage(bytes.data(), length, decoded), dashboard::Status::Ok);
  EXPECT_EQ(decoded.style, package.style);
  EXPECT_EQ(dashboard::widgetIconId(decoded.widgets[0].style), 64U);
  EXPECT_EQ(dashboard::widgetSizeRung(decoded.widgets[0].style), 3U);
  EXPECT_EQ(dashboard::widgetEmphasis(decoded.widgets[0].style), 2U);
  EXPECT_EQ(decoded.widgets[0].style, dashboard::makeWidgetStyle(64, 3, 2));
  EXPECT_EQ(decoded.widgets[1].style, dashboard::makeWidgetStyle(1, 0, 0));
  EXPECT_EQ(decoded.widgets[2].style, dashboard::makeWidgetStyle(0, 2, 3));
}

TEST(DashboardWidgetGrid, RejectsReservedBitsInGlobalStyle) {
  auto package = validPackage();
  package.style = dashboard::GLOBAL_STYLE_RESERVED_MASK;  // 0xC0
  dashboard::PackageBytes bytes{};
  size_t length = 0;
  EXPECT_EQ(dashboard::encodeWidgetGridPackage(package, bytes, length), dashboard::Status::InvalidArgument);

  package.style = 0;
  ASSERT_EQ(dashboard::encodeWidgetGridPackage(package, bytes, length), dashboard::Status::Ok);
  bytes[30] |= 0x80;
  rewriteCrc(bytes, length);
  dashboard::WidgetGridPackage decoded{};
  EXPECT_EQ(dashboard::decodeWidgetGridPackage(bytes.data(), length, decoded), dashboard::Status::InvalidArgument);
}

TEST(DashboardWidgetGrid, RejectsReservedBitsInWidgetStyle) {
  auto package = validPackage();
  package.widgets[0].style = dashboard::WIDGET_STYLE_RESERVED_MASK;  // 0xF800
  dashboard::PackageBytes bytes{};
  size_t length = 0;
  EXPECT_EQ(dashboard::encodeWidgetGridPackage(package, bytes, length), dashboard::Status::InvalidArgument);

  package.widgets[0].style = 0;
  ASSERT_EQ(dashboard::encodeWidgetGridPackage(package, bytes, length), dashboard::Status::Ok);
  // Widget 0's style word sits at offsets 36-37 (content starts at 31, header
  // is 7); reserved bits live in the high byte.
  bytes[37] |= 0x80;
  rewriteCrc(bytes, length);
  dashboard::WidgetGridPackage decoded{};
  EXPECT_EQ(dashboard::decodeWidgetGridPackage(bytes.data(), length, decoded), dashboard::Status::InvalidArgument);
}

TEST(DashboardWidgetGrid, RejectsIconIdAbove64) {
  auto package = validPackage();
  package.widgets[0].style = dashboard::makeWidgetStyle(/*iconId=*/65, 0, 0);
  dashboard::PackageBytes bytes{};
  size_t length = 0;
  EXPECT_EQ(dashboard::encodeWidgetGridPackage(package, bytes, length), dashboard::Status::InvalidArgument);

  package.widgets[0].style = 0;
  ASSERT_EQ(dashboard::encodeWidgetGridPackage(package, bytes, length), dashboard::Status::Ok);
  bytes[36] = 65;
  rewriteCrc(bytes, length);
  dashboard::WidgetGridPackage decoded{};
  EXPECT_EQ(dashboard::decodeWidgetGridPackage(bytes.data(), length, decoded), dashboard::Status::InvalidArgument);
}

TEST(DashboardWidgetGrid, WidgetAndPackageSizesStayWithinBudget) {
  EXPECT_EQ(sizeof(dashboard::Widget), 42u);
  EXPECT_EQ(sizeof(dashboard::WidgetGridPackage), 2192u);
  EXPECT_LE(sizeof(dashboard::Widget) * dashboard::MAX_WIDGETS, 1024u);
}

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
  const dashboard::ListContent* decodedList = dashboard::listContentFor(decoded, decoded.widgets[2]);
  ASSERT_NE(decodedList, nullptr);
  ASSERT_EQ(decodedList->rowCount, 2U);
  EXPECT_TRUE(std::equal(decodedList->rows[1].timeBytes.begin(), decodedList->rows[1].timeBytes.begin() + 5,
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
  package.lists[package.widgets[2].listIndex].headingLength = 0;
  dashboard::PackageBytes bytes{};
  size_t length = 0;
  EXPECT_EQ(dashboard::encodeWidgetGridPackage(package, bytes, length), dashboard::Status::Ok);

  package = validPackage();
  package.lists[package.widgets[2].listIndex].rows[0].timeLength = 0;
  EXPECT_EQ(dashboard::encodeWidgetGridPackage(package, bytes, length), dashboard::Status::InvalidLength);
}

TEST(DashboardWidgetGrid, RejectsOversizedPackage) {
  // Three full lists of maximum-length rows is roughly 975 bytes of content,
  // well past MAX_PACKAGE_SIZE, without needing more list slots than the
  // decoded form can hold.
  dashboard::WidgetGridPackage package{};
  package.packageId = 9;
  package.generatedAt = 1000;
  package.validUntil = 2000;
  for (uint8_t index = 0; index < dashboard::MAX_LIST_WIDGETS; ++index) {
    package.widgets[index] =
        listWidget(package, /*column=*/0, /*row=*/index, "Vandaag", /*columnSpan=*/2, /*rowSpan=*/1);
    dashboard::ListContent& list = package.lists[package.widgets[index].listIndex];
    for (auto& listRow : list.rows) {
      setField(listRow.timeBytes, listRow.timeLength, "00:00-23:59");
      listRow.labelLength = dashboard::MAX_LIST_ROW_LABEL_SIZE;
      std::fill_n(listRow.labelBytes.begin(), listRow.labelLength, 'a');
    }
    list.rowCount = dashboard::MAX_LIST_ROWS;
  }
  package.widgetCount = static_cast<uint8_t>(dashboard::MAX_LIST_WIDGETS);
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
  ASSERT_EQ(length, 35u);

  dashboard::PackageHeader header{};
  EXPECT_EQ(dashboard::peekPackageHeader(bytes.data(), length, header), dashboard::Status::Ok);
  EXPECT_EQ(header.packageId, 5u);
  EXPECT_EQ(header.templateId, dashboard::TEMPLATE_WIDGET_GRID);
}

// Every Widget used to carry both content variants, so a KPI tile paid for a
// six-row ListContent it never used: 421 bytes per widget, 3400 for the
// package. That package is a stack local in renderWidgetGridTemplate()
// (BleHandoffReaderProbe.cpp) on a C3 whose task stacks are 2-4 KB, and it
// grew linearly with MAX_WIDGETS, which is what blocked raising it from 8
// toward the grid's 24 cells. The wire format is unaffected either way -
// encoding never wrote the unused variant.
TEST(DashboardWidgetGrid, WidgetSlotsAreCheapEnoughToCoverTheWholeGrid) {
  EXPECT_LE(sizeof(dashboard::Widget), 48u);
  // The whole grid's worth of slots must stay a fraction of the package, so
  // that widening the grid later stays a policy decision rather than a memory
  // one.
  EXPECT_LE(sizeof(dashboard::Widget) * dashboard::MAX_WIDGETS, 1024u);
}

// Hand-built bytes rather than encodeWidgetGridPackage output: the encoder
// refuses to produce this, which is exactly why the decoder has to be checked
// against it separately - the bytes arrive over BLE from a phone this firmware
// does not control.
TEST(DashboardWidgetGrid, DecodeRejectsMoreListsThanTheDecodedFormCanHold) {
  constexpr size_t widgetCount = dashboard::MAX_LIST_WIDGETS + 1;
  constexpr size_t total = 31 + widgetCount * 9 + dashboard::CRC_SIZE;
  ASSERT_LE(widgetCount, dashboard::GRID_COLUMNS);  // one 1x1 list per column, no overlaps

  std::array<uint8_t, dashboard::MAX_PACKAGE_SIZE> bytes{};
  bytes[0] = 'X';
  bytes[1] = '3';
  bytes[2] = 'D';
  bytes[3] = 'P';
  bytes[4] = dashboard::SCHEMA_V2;
  bytes[5] = dashboard::TEMPLATE_WIDGET_GRID;
  bytes[6] = static_cast<uint8_t>(total);
  bytes[8] = 9;     // packageId
  bytes[12] = 100;  // generatedAt
  bytes[20] = 200;  // validUntil
  bytes[28] = dashboard::GRID_COLUMNS;
  bytes[29] = static_cast<uint8_t>(widgetCount);
  bytes[30] = 0;  // global style byte
  for (size_t index = 0; index < widgetCount; ++index) {
    uint8_t* widget = bytes.data() + 31 + index * 9;
    widget[0] = static_cast<uint8_t>(dashboard::WidgetType::List);
    widget[1] = static_cast<uint8_t>(index);  // column
    widget[2] = 0;                            // row
    widget[3] = 1;                            // columnSpan
    widget[4] = 1;                            // rowSpan
    widget[5] = 0;                            // style, low byte
    widget[6] = 0;                            // style, high byte
    widget[7] = 0;                            // headingLength
    widget[8] = 0;                            // rowCount
  }
  const uint32_t crc = dashboard::crc32(bytes.data(), total - dashboard::CRC_SIZE);
  for (uint8_t index = 0; index < 4; ++index) {
    bytes[total - dashboard::CRC_SIZE + index] = static_cast<uint8_t>(crc >> (index * 8U));
  }

  dashboard::WidgetGridPackage decoded{};
  EXPECT_EQ(dashboard::decodeWidgetGridPackage(bytes.data(), total, decoded), dashboard::Status::InvalidLength);
}

// A dashboard of three KPI tiles plus a full agenda is about 500 bytes, which
// the original 256-byte budget could not hold: it forced a choice between
// tiles and agenda rows rather than fitting both.
TEST(DashboardWidgetGrid, CarriesAFullDashboardOfTilesAndAgendaRows) {
  dashboard::WidgetGridPackage package{};
  package.packageId = 9;
  package.generatedAt = 1000;
  package.validUntil = 2000;
  package.widgets[0] = kpiWidget(/*column=*/0, /*row=*/0, "Stappen", "8.432");
  package.widgets[1] = kpiWidget(/*column=*/1, /*row=*/0, "Batterij", "87%");
  package.widgets[2] = kpiWidget(/*column=*/2, /*row=*/0, "Woonkamer", "21.4C");
  package.widgets[3] = listWidget(package, /*column=*/0, /*row=*/1, "AGENDA", /*columnSpan=*/4, /*rowSpan=*/5);
  package.widgetCount = 4;

  dashboard::ListContent& list = package.lists[package.widgets[3].listIndex];
  for (auto& row : list.rows) {
    setField(row.timeBytes, row.timeLength, "09:00");
    row.labelLength = dashboard::MAX_LIST_ROW_LABEL_SIZE;
    std::fill_n(row.labelBytes.begin(), row.labelLength, 'a');
  }
  list.rowCount = dashboard::MAX_LIST_ROWS;

  dashboard::PackageBytes bytes{};
  size_t length = 0;
  ASSERT_EQ(dashboard::encodeWidgetGridPackage(package, bytes, length), dashboard::Status::Ok);
  EXPECT_GT(length, 256U);

  dashboard::WidgetGridPackage decoded{};
  ASSERT_EQ(dashboard::decodeWidgetGridPackage(bytes.data(), length, decoded), dashboard::Status::Ok);
  EXPECT_EQ(decoded.widgetCount, 4U);
  const dashboard::ListContent* decodedList = dashboard::listContentFor(decoded, decoded.widgets[3]);
  ASSERT_NE(decodedList, nullptr);
  EXPECT_EQ(decodedList->rowCount, dashboard::MAX_LIST_ROWS);
}

// The grid is four columns by six rows, but only eight widgets used to fit in
// a package, so two thirds of the cells the composer offered could never be
// filled. A widget costs 40 bytes of decoded package since list content moved
// out, so covering every cell is affordable.
TEST(DashboardWidgetGrid, FillsEveryCellOfTheGrid) {
  dashboard::WidgetGridPackage package{};
  package.packageId = 9;
  package.generatedAt = 1000;
  package.validUntil = 2000;
  ASSERT_GE(dashboard::MAX_WIDGETS, static_cast<size_t>(dashboard::GRID_COLUMNS) * dashboard::MAX_ROW_SPAN);

  uint8_t index = 0;
  for (uint8_t row = 0; row < dashboard::MAX_ROW_SPAN; ++row) {
    for (uint8_t column = 0; column < dashboard::GRID_COLUMNS; ++column) {
      package.widgets[index++] = kpiWidget(column, row, "Sensor", "21.4C");
    }
  }
  package.widgetCount = index;

  dashboard::PackageBytes bytes{};
  size_t length = 0;
  ASSERT_EQ(dashboard::encodeWidgetGridPackage(package, bytes, length), dashboard::Status::Ok);

  dashboard::WidgetGridPackage decoded{};
  ASSERT_EQ(dashboard::decodeWidgetGridPackage(bytes.data(), length, decoded), dashboard::Status::Ok);
  EXPECT_EQ(decoded.widgetCount, 24U);
}

// borderLevel occupies three bits but names only four treatments, and density
// two bits for three paddings. The renderer turns both into a table index, so a
// package naming an undefined one must be refused here rather than reaching it.
TEST(DashboardWidgetGrid, RejectsUndefinedGlobalStyleValues) {
  dashboard::WidgetGridPackage package{};
  package.packageId = 7;
  package.generatedAt = 1000;
  package.validUntil = 2000;
  package.widgets[0] = kpiWidget(0, 0, "Stappen", "8432");
  package.widgetCount = 1;

  dashboard::PackageBytes bytes{};
  size_t length = 0;

  package.style = dashboard::makeGlobalStyle(dashboard::MAX_BORDER_LEVEL + 1, 0, false);
  EXPECT_EQ(dashboard::encodeWidgetGridPackage(package, bytes, length), dashboard::Status::InvalidArgument);

  package.style = dashboard::makeGlobalStyle(0, dashboard::MAX_DENSITY + 1, false);
  EXPECT_EQ(dashboard::encodeWidgetGridPackage(package, bytes, length), dashboard::Status::InvalidArgument);

  package.style = dashboard::makeGlobalStyle(dashboard::MAX_BORDER_LEVEL, dashboard::MAX_DENSITY, true);
  ASSERT_EQ(dashboard::encodeWidgetGridPackage(package, bytes, length), dashboard::Status::Ok);

  dashboard::WidgetGridPackage decoded{};
  ASSERT_EQ(dashboard::decodeWidgetGridPackage(bytes.data(), length, decoded), dashboard::Status::Ok);
  EXPECT_EQ(dashboard::globalBorderLevel(decoded.style), dashboard::MAX_BORDER_LEVEL);
  EXPECT_EQ(dashboard::globalDensity(decoded.style), dashboard::MAX_DENSITY);
  EXPECT_TRUE(dashboard::globalListDividers(decoded.style));
}

TEST(DashboardWidgetGrid, DateWidgetCostsEightBytes) {
  dashboard::WidgetGridPackage package{};
  package.packageId = 7;
  package.generatedAt = 1000;
  package.validUntil = 2000;
  package.widgets[0] = dateWidget(0, 0, dashboard::DateField::Auto, 2, 2);
  package.widgetCount = 1;

  dashboard::PackageBytes bytes{};
  size_t length = 0;
  ASSERT_EQ(dashboard::encodeWidgetGridPackage(package, bytes, length), dashboard::Status::Ok);
  // 31-byte prefix + 8-byte date widget + 4-byte CRC.
  EXPECT_EQ(length, 43u);
}

TEST(DashboardWidgetGrid, DateWidgetRoundTripsEveryField) {
  const dashboard::DateField fields[] = {dashboard::DateField::Auto,  dashboard::DateField::Day,
                                         dashboard::DateField::Weekday, dashboard::DateField::Month,
                                         dashboard::DateField::Year,  dashboard::DateField::WeekNumber};
  for (const dashboard::DateField field : fields) {
    dashboard::WidgetGridPackage package{};
    package.packageId = 9;
    package.generatedAt = 1000;
    package.validUntil = 2000;
    package.widgets[0] = dateWidget(1, 2, field);
    package.widgetCount = 1;

    dashboard::PackageBytes bytes{};
    size_t length = 0;
    ASSERT_EQ(dashboard::encodeWidgetGridPackage(package, bytes, length), dashboard::Status::Ok);

    dashboard::WidgetGridPackage decoded{};
    ASSERT_EQ(dashboard::decodeWidgetGridPackage(bytes.data(), length, decoded), dashboard::Status::Ok);
    ASSERT_EQ(decoded.widgetCount, 1);
    EXPECT_EQ(decoded.widgets[0].type, dashboard::WidgetType::Date);
    EXPECT_EQ(decoded.widgets[0].dateField, field);
    EXPECT_EQ(decoded.widgets[0].column, 1);
    EXPECT_EQ(decoded.widgets[0].row, 2);
  }
}

TEST(DashboardWidgetGrid, DateWidgetRefusesUnknownField) {
  dashboard::WidgetGridPackage package{};
  package.packageId = 11;
  package.generatedAt = 1000;
  package.validUntil = 2000;
  package.widgets[0] = dateWidget(0, 0, static_cast<dashboard::DateField>(6));
  package.widgetCount = 1;

  dashboard::PackageBytes bytes{};
  size_t length = 0;
  EXPECT_EQ(dashboard::encodeWidgetGridPackage(package, bytes, length), dashboard::Status::InvalidArgument);
}

// The hand-derived vector the Swift side must reproduce byte for byte.
TEST(DashboardWidgetGrid, DateWidgetMatchesHandDerivedBytes) {
  dashboard::WidgetGridPackage package{};
  package.packageId = 0x01020304;
  package.generatedAt = 1000;
  package.validUntil = 2000;
  package.style = 0;
  package.widgets[0] = dateWidget(2, 3, dashboard::DateField::WeekNumber, 1, 1);
  package.widgets[0].style = dashboard::makeWidgetStyle(5, 2, 1);
  package.widgetCount = 1;

  dashboard::PackageBytes bytes{};
  size_t length = 0;
  ASSERT_EQ(dashboard::encodeWidgetGridPackage(package, bytes, length), dashboard::Status::Ok);

  // The widget's own 8 bytes start right after the 31-byte prefix.
  const uint8_t expected[] = {
      3,          // type = Date
      2,          // column
      3,          // row
      1,          // columnSpan
      1,          // rowSpan
      0x05, 0x03, // style: iconId 5 | sizeRung 2 << 7 | emphasis 1 << 9 = 0x0305, little endian
      5,          // field = WeekNumber
  };
  for (size_t index = 0; index < sizeof(expected); ++index) {
    EXPECT_EQ(bytes[31 + index], expected[index]) << "byte " << index;
  }
}
