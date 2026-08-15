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

constexpr size_t MAX_KPI_LABEL_SIZE = 16;
constexpr size_t MAX_KPI_VALUE_SIZE = 16;

constexpr size_t MAX_LIST_HEADING_SIZE = 32;
constexpr size_t MAX_LIST_ROWS = 6;
constexpr size_t MAX_LIST_ROW_TIME_SIZE = 16;
constexpr size_t MAX_LIST_ROW_LABEL_SIZE = 40;

enum class WidgetType : uint8_t { Kpi = 1, List = 2 };

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
// the package's `lists`. Encoding writes only the selected variant's bytes to
// the wire, so an unused variant costs no package space.
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
  uint8_t listIndex = 0;
  KpiContent kpi{};
};

struct WidgetGridPackage {
  uint8_t schema = SCHEMA_V1;
  uint8_t templateId = TEMPLATE_WIDGET_GRID;
  uint32_t packageId = 0;
  uint64_t generatedAt = 0;
  uint64_t validUntil = 0;
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
