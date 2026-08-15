#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "BleHandoffRecord.h"

namespace dashboard {

// A third dashboard template: an ordered set of widgets placed into a fixed
// 4-column grid, flowing left-to-right/top-to-bottom by each widget's
// declared column/row span. The iPhone composes the set and layout per
// package; adding a new arrangement (or dropping/adding widgets) never
// requires a firmware change, only a new widget TYPE does. Shares the same
// 28-byte common prefix and CRC trailer as the other templates so a
// template-agnostic reader can dispatch on byte 5.
constexpr uint8_t TEMPLATE_WIDGET_GRID = 3;
constexpr uint8_t GRID_COLUMNS = 4;
constexpr uint8_t MAX_ROW_SPAN = 6;
// One per grid cell, so any arrangement the composer can draw also fits in a
// package. Affordable only because list content lives in the package rather
// than in every Widget: a widget slot costs 40 bytes here, not 421.
constexpr size_t MAX_WIDGETS = static_cast<size_t>(GRID_COLUMNS) * MAX_ROW_SPAN;
// How many of those widgets may be lists. A ListContent is over ten times the
// size of a KpiContent, so storing one per widget would dominate the decoded
// package (see WidgetGridPackage below). Three full-width lists stacked in a
// 4x6 grid is already more than the wire budget can fill with real rows.
constexpr size_t MAX_LIST_WIDGETS = 3;

// Global style byte at offset 30, introduced with schema 2. Bits 0-2 select
// the border treatment, bits 3-4 the tile padding density, bit 5 toggles list
// row dividers, and bits 6-7 are reserved and must be zero.
constexpr uint8_t MAX_ICON_ID = 64;
constexpr uint8_t GLOBAL_STYLE_BORDER_LEVEL_SHIFT = 0;
constexpr uint8_t GLOBAL_STYLE_DENSITY_SHIFT = 3;
constexpr uint8_t GLOBAL_STYLE_LIST_DIVIDERS_SHIFT = 5;
constexpr uint8_t GLOBAL_STYLE_RESERVED_MASK = 0xC0;
// Both fields are wider than the treatments they name: three bits for four
// border levels, two for three densities. The renderer turns each into a table
// index, so validation refuses the unnamed values rather than letting them
// reach it.
constexpr uint8_t MAX_BORDER_LEVEL = 3;
constexpr uint8_t MAX_DENSITY = 2;

// Border treatments, indexing the renderer's border pass.
constexpr uint8_t BORDER_NONE = 0;
constexpr uint8_t BORDER_HAIRLINE = 1;  // thin rule between neighbouring tiles
constexpr uint8_t BORDER_LIGHT = 2;     // dithered outline per tile
constexpr uint8_t BORDER_SOLID = 3;     // full black outline per tile

// Per-widget emphasis. Unlike borderLevel and density these use their bits
// exactly, so every value is named and no range check is needed.
constexpr uint8_t EMPHASIS_NONE = 0;
constexpr uint8_t EMPHASIS_LIGHT = 1;     // 25% dither behind the content
constexpr uint8_t EMPHASIS_DARK = 2;      // 50% dither behind the content
constexpr uint8_t EMPHASIS_INVERTED = 3;  // solid black, content drawn white

constexpr uint8_t globalBorderLevel(const uint8_t style) {
  return static_cast<uint8_t>((style >> GLOBAL_STYLE_BORDER_LEVEL_SHIFT) & 0x7U);
}

constexpr uint8_t globalDensity(const uint8_t style) {
  return static_cast<uint8_t>((style >> GLOBAL_STYLE_DENSITY_SHIFT) & 0x3U);
}

constexpr bool globalListDividers(const uint8_t style) {
  return ((style >> GLOBAL_STYLE_LIST_DIVIDERS_SHIFT) & 0x1U) != 0;
}

constexpr uint8_t makeGlobalStyle(const uint8_t borderLevel, const uint8_t density, const bool listDividers) {
  return static_cast<uint8_t>((borderLevel << GLOBAL_STYLE_BORDER_LEVEL_SHIFT) |
                              (density << GLOBAL_STYLE_DENSITY_SHIFT) |
                              (static_cast<uint8_t>(listDividers) << GLOBAL_STYLE_LIST_DIVIDERS_SHIFT));
}

// Per-widget little-endian uint16 style word. Bits 0-6 are the icon id, bits
// 7-8 the font size rung, bits 9-10 the emphasis, and bits 11-15 are reserved
// and must be zero.
constexpr uint16_t WIDGET_STYLE_ICON_ID_MASK = 0x007F;
constexpr uint8_t WIDGET_STYLE_SIZE_RUNG_SHIFT = 7;
constexpr uint8_t WIDGET_STYLE_EMPHASIS_SHIFT = 9;
constexpr uint16_t WIDGET_STYLE_RESERVED_MASK = 0xF800;

constexpr uint8_t widgetIconId(const uint16_t style) { return static_cast<uint8_t>(style & WIDGET_STYLE_ICON_ID_MASK); }

constexpr uint8_t widgetSizeRung(const uint16_t style) {
  return static_cast<uint8_t>((style >> WIDGET_STYLE_SIZE_RUNG_SHIFT) & 0x3U);
}

constexpr uint8_t widgetEmphasis(const uint16_t style) {
  return static_cast<uint8_t>((style >> WIDGET_STYLE_EMPHASIS_SHIFT) & 0x3U);
}

constexpr uint16_t makeWidgetStyle(const uint8_t iconId, const uint8_t sizeRung, const uint8_t emphasis) {
  return static_cast<uint16_t>(iconId) | (static_cast<uint16_t>(sizeRung) << WIDGET_STYLE_SIZE_RUNG_SHIFT) |
         (static_cast<uint16_t>(emphasis) << WIDGET_STYLE_EMPHASIS_SHIFT);
}

constexpr size_t MAX_KPI_LABEL_SIZE = 16;
constexpr size_t MAX_KPI_VALUE_SIZE = 16;

constexpr size_t MAX_LIST_HEADING_SIZE = 32;
constexpr size_t MAX_LIST_ROWS = 6;
constexpr size_t MAX_LIST_ROW_TIME_SIZE = 16;
constexpr size_t MAX_LIST_ROW_LABEL_SIZE = 40;

enum class WidgetType : uint8_t { Kpi = 1, List = 2, Date = 3 };

// Which field of today's date a Date widget shows. `Auto` lets the renderer
// pick a layout from the tile's size; every other value pins the tile to one
// field at whatever size fits. The date itself never travels in the package —
// the X3 reads its own RTC — so this byte is a Date widget's entire payload.
enum class DateField : uint8_t { Auto = 0, Day = 1, Weekday = 2, Month = 3, Year = 4, WeekNumber = 5 };
constexpr uint8_t MAX_DATE_FIELD = 5;

struct ListRow {
  std::array<uint8_t, MAX_LIST_ROW_TIME_SIZE> timeBytes{};
  uint8_t timeLength = 0;
  std::array<uint8_t, MAX_LIST_ROW_LABEL_SIZE> labelBytes{};
  uint8_t labelLength = 0;
};

struct KpiContent {
  std::array<uint8_t, MAX_KPI_LABEL_SIZE> labelBytes{};
  uint8_t labelLength = 0;
  std::array<uint8_t, MAX_KPI_VALUE_SIZE> valueBytes{};
  uint8_t valueLength = 0;
};

struct ListContent {
  std::array<uint8_t, MAX_LIST_HEADING_SIZE> headingBytes{};
  uint8_t headingLength = 0;
  std::array<ListRow, MAX_LIST_ROWS> rows{};
  uint8_t rowCount = 0;
};

// `kpi` is meaningful only when `type == WidgetType::Kpi`; `listIndex` only
// when `type == WidgetType::List`, where it selects this widget's content from
// the package's `lists`; `dateField` only when `type == WidgetType::Date`.
// Encoding writes only the selected variant's bytes to the wire, so an unused
// variant costs no package space.
//
// List content lives in the package rather than in the widget because a
// ListContent is over ten times the size of a KpiContent: inlining both made a
// Widget 421 bytes and the package 3400, which is a lot to place on a C3 task
// stack (decodeWidgetGridPackage and renderWidgetGridTemplate both hold one)
// and it grew with every extra widget slot. The wire format is unchanged.
//
// `column`/`row` are the widget's explicit top-left grid cell (0-based,
// column < GRID_COLUMNS, row < MAX_ROW_SPAN). Placement is free-form: the
// iPhone assigns any widget to any cell, independent of `widgets` array
// order. There is no auto-flow/shelf-packing on the wire or in
// computeGridLayout() - only in the now-removed shelf logic this replaced.
struct Widget {
  WidgetType type = WidgetType::Kpi;
  uint8_t column = 0;
  uint8_t row = 0;
  uint8_t columnSpan = 1;
  uint8_t rowSpan = 1;
  // One byte whose meaning follows `type`: a List widget's index into the
  // package's `lists`, or a Date widget's field. A widget is never both, and
  // sharing the byte is not a micro-optimisation: giving Date its own byte
  // padded Widget from 42 to 44, and MAX_WIDGETS of those is 1056 bytes -
  // past the 1 KB budget WidgetSlotsAreCheapEnoughToCoverTheWholeGrid guards
  // so that widening the grid stays a policy decision rather than a memory one.
  union {
    uint8_t listIndex = 0;
    // Initialised through listIndex above: DateField(0) is Auto, so a
    // value-initialised Widget is a valid date widget as well as a valid list.
    DateField dateField;
  };
  // Schema 2 style word: iconId | sizeRung<<7 | emphasis<<9. Kept as the raw
  // little-endian uint16 so decoding never needs to reinterpret bytes; use
  // widgetIconId/widgetSizeRung/widgetEmphasis to read the fields.
  uint16_t style = 0;
  KpiContent kpi{};
};

struct WidgetGridPackage {
  uint8_t schema = SCHEMA_V2;
  uint8_t templateId = TEMPLATE_WIDGET_GRID;
  uint32_t packageId = 0;
  uint64_t generatedAt = 0;
  uint64_t validUntil = 0;
  // Schema 2 global style byte: borderLevel | density<<3 | listDividers<<5.
  uint8_t style = 0;
  std::array<Widget, MAX_WIDGETS> widgets{};
  // Content for the list-typed widgets, in the order they appear in `widgets`.
  // `listCount` is how many are in use, never more than MAX_LIST_WIDGETS.
  std::array<ListContent, MAX_LIST_WIDGETS> lists{};
  uint8_t widgetCount = 0;
  uint8_t listCount = 0;
  uint32_t crc = 0;
};

// The list content `widget` refers to, or nullptr when it is not a list widget
// or its index is out of range. Callers that render or measure a widget must
// go through this rather than indexing `lists` directly.
const ListContent* listContentFor(const WidgetGridPackage& package, const Widget& widget);

Status encodeWidgetGridPackage(const WidgetGridPackage& package, PackageBytes& output, size_t& outputLength);
Status decodeWidgetGridPackage(const uint8_t* bytes, size_t size, WidgetGridPackage& output);

}  // namespace dashboard
