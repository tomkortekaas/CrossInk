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
constexpr size_t MAX_WIDGETS = 8;
constexpr uint8_t MAX_ROW_SPAN = 6;

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

// `kpi` is meaningful only when `type == WidgetType::Kpi`; `list` only when
// `type == WidgetType::List`. Encoding writes only the selected variant's
// bytes to the wire, so an unused variant costs no package space.
struct Widget {
  WidgetType type = WidgetType::Kpi;
  uint8_t columnSpan = 1;
  uint8_t rowSpan = 1;
  KpiContent kpi{};
  ListContent list{};
};

struct WidgetGridPackage {
  uint8_t schema = SCHEMA_V1;
  uint8_t templateId = TEMPLATE_WIDGET_GRID;
  uint32_t packageId = 0;
  uint64_t generatedAt = 0;
  uint64_t validUntil = 0;
  std::array<Widget, MAX_WIDGETS> widgets{};
  uint8_t widgetCount = 0;
  uint32_t crc = 0;
};

Status encodeWidgetGridPackage(const WidgetGridPackage& package, PackageBytes& output, size_t& outputLength);
Status decodeWidgetGridPackage(const uint8_t* bytes, size_t size, WidgetGridPackage& output);

}  // namespace dashboard
