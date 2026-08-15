#include "DashboardWidgetGrid.h"

#include <algorithm>

namespace dashboard {
namespace {

constexpr size_t LENGTH_OFFSET = 6;
constexpr size_t PACKAGE_ID_OFFSET = 8;
constexpr size_t GENERATED_AT_OFFSET = 12;
constexpr size_t VALID_UNTIL_OFFSET = 20;
constexpr size_t GRID_COLUMNS_OFFSET = 28;
constexpr size_t WIDGET_COUNT_OFFSET = 29;
constexpr size_t STYLE_OFFSET = 30;
constexpr size_t CONTENT_OFFSET = 31;

void writeU16(uint8_t* out, uint16_t value) {
  out[0] = static_cast<uint8_t>(value);
  out[1] = static_cast<uint8_t>(value >> 8U);
}

void writeU32(uint8_t* out, uint32_t value) {
  for (uint8_t index = 0; index < 4; ++index) out[index] = static_cast<uint8_t>(value >> (index * 8U));
}

void writeU64(uint8_t* out, uint64_t value) {
  for (uint8_t index = 0; index < 8; ++index) out[index] = static_cast<uint8_t>(value >> (index * 8U));
}

uint16_t readU16(const uint8_t* in) { return static_cast<uint16_t>(in[0]) | (static_cast<uint16_t>(in[1]) << 8U); }

uint32_t readU32(const uint8_t* in) {
  uint32_t value = 0;
  for (uint8_t index = 0; index < 4; ++index) value |= static_cast<uint32_t>(in[index]) << (index * 8U);
  return value;
}

uint64_t readU64(const uint8_t* in) {
  uint64_t value = 0;
  for (uint8_t index = 0; index < 8; ++index) value |= static_cast<uint64_t>(in[index]) << (index * 8U);
  return value;
}

bool widgetsOverlap(const Widget& a, const Widget& b) {
  const bool columnsOverlap = a.column < b.column + b.columnSpan && b.column < a.column + a.columnSpan;
  const bool rowsOverlap = a.row < b.row + b.rowSpan && b.row < a.row + a.rowSpan;
  return columnsOverlap && rowsOverlap;
}

// Checked after every widget individually validates, so an in-bounds but
// overlapping arrangement is still rejected - free-form placement trusts the
// composer not to overlap widgets, but does not trust it blindly.
Status validateNoOverlaps(const WidgetGridPackage& package) {
  for (uint8_t first = 0; first < package.widgetCount; ++first) {
    for (uint8_t second = first + 1; second < package.widgetCount; ++second) {
      if (widgetsOverlap(package.widgets[first], package.widgets[second])) return Status::InvalidArgument;
    }
  }
  return Status::Ok;
}

Status validateGlobalStyle(const uint8_t style) {
  if ((style & GLOBAL_STYLE_RESERVED_MASK) != 0) return Status::InvalidArgument;
  if (globalBorderLevel(style) > MAX_BORDER_LEVEL) return Status::InvalidArgument;
  if (globalDensity(style) > MAX_DENSITY) return Status::InvalidArgument;
  return Status::Ok;
}

Status validateWidget(const WidgetGridPackage& package, const Widget& widget) {
  if (widget.columnSpan == 0 || widget.columnSpan > GRID_COLUMNS || widget.rowSpan == 0 ||
      widget.rowSpan > MAX_ROW_SPAN) {
    return Status::InvalidLength;
  }
  if (widget.column >= GRID_COLUMNS || widget.row >= MAX_ROW_SPAN || widget.column + widget.columnSpan > GRID_COLUMNS ||
      widget.row + widget.rowSpan > MAX_ROW_SPAN) {
    return Status::InvalidLength;
  }
  if ((widget.style & WIDGET_STYLE_RESERVED_MASK) != 0) return Status::InvalidArgument;
  if (widgetIconId(widget.style) > MAX_ICON_ID) return Status::InvalidArgument;
  if (widget.type == WidgetType::Kpi) {
    const KpiContent& kpi = widget.kpi;
    if (kpi.labelLength == 0 || kpi.labelLength > MAX_KPI_LABEL_SIZE || kpi.valueLength == 0 ||
        kpi.valueLength > MAX_KPI_VALUE_SIZE) {
      return Status::InvalidLength;
    }
    Status status = validateUtf8(kpi.labelBytes.data(), kpi.labelLength);
    if (status != Status::Ok) return status;
    return validateUtf8(kpi.valueBytes.data(), kpi.valueLength);
  }
  if (widget.type == WidgetType::List) {
    const ListContent* content = listContentFor(package, widget);
    if (content == nullptr) return Status::InvalidArgument;
    const ListContent& list = *content;
    if (list.headingLength > MAX_LIST_HEADING_SIZE || list.rowCount > MAX_LIST_ROWS) return Status::InvalidLength;
    if (list.headingLength > 0) {
      const Status status = validateUtf8(list.headingBytes.data(), list.headingLength);
      if (status != Status::Ok) return status;
    }
    for (uint8_t index = 0; index < list.rowCount; ++index) {
      const ListRow& row = list.rows[index];
      if (row.timeLength == 0 || row.timeLength > MAX_LIST_ROW_TIME_SIZE || row.labelLength == 0 ||
          row.labelLength > MAX_LIST_ROW_LABEL_SIZE) {
        return Status::InvalidLength;
      }
      Status status = validateUtf8(row.timeBytes.data(), row.timeLength);
      if (status != Status::Ok) return status;
      status = validateUtf8(row.labelBytes.data(), row.labelLength);
      if (status != Status::Ok) return status;
    }
    return Status::Ok;
  }
  return Status::InvalidArgument;
}

size_t widgetContentLength(const WidgetGridPackage& package, const Widget& widget) {
  size_t total = 7;  // type + column + row + columnSpan + rowSpan + style, written for every widget.
  if (widget.type == WidgetType::Kpi) return total + 2 + widget.kpi.labelLength + widget.kpi.valueLength;
  const ListContent* content = listContentFor(package, widget);
  if (content == nullptr) return total + 2;
  total += 2 + content->headingLength;
  for (uint8_t index = 0; index < content->rowCount; ++index) {
    total += 2 + content->rows[index].timeLength + content->rows[index].labelLength;
  }
  return total;
}

size_t writeWidget(uint8_t* out, const WidgetGridPackage& package, const Widget& widget) {
  out[0] = static_cast<uint8_t>(widget.type);
  out[1] = widget.column;
  out[2] = widget.row;
  out[3] = widget.columnSpan;
  out[4] = widget.rowSpan;
  writeU16(out + 5, widget.style);
  size_t offset = 7;
  if (widget.type == WidgetType::Kpi) {
    out[offset] = widget.kpi.labelLength;
    out[offset + 1] = widget.kpi.valueLength;
    offset += 2;
    std::copy_n(widget.kpi.labelBytes.begin(), widget.kpi.labelLength, out + offset);
    offset += widget.kpi.labelLength;
    std::copy_n(widget.kpi.valueBytes.begin(), widget.kpi.valueLength, out + offset);
    offset += widget.kpi.valueLength;
    return offset;
  }
  // validateWidget() has already rejected a list widget without content, so a
  // null here would mean encoding an unvalidated package; write an empty list
  // rather than dereferencing.
  const ListContent* content = listContentFor(package, widget);
  if (content == nullptr) {
    out[offset] = 0;
    out[offset + 1] = 0;
    return offset + 2;
  }
  out[offset] = content->headingLength;
  out[offset + 1] = content->rowCount;
  offset += 2;
  std::copy_n(content->headingBytes.begin(), content->headingLength, out + offset);
  offset += content->headingLength;
  for (uint8_t index = 0; index < content->rowCount; ++index) {
    const ListRow& row = content->rows[index];
    out[offset] = row.timeLength;
    out[offset + 1] = row.labelLength;
    offset += 2;
    std::copy_n(row.timeBytes.begin(), row.timeLength, out + offset);
    offset += row.timeLength;
    std::copy_n(row.labelBytes.begin(), row.labelLength, out + offset);
    offset += row.labelLength;
  }
  return offset;
}

// Returns SIZE_MAX on malformed input instead of a Status so callers can
// distinguish "ran out of bytes" from every specific field-validity error,
// which the field-level validateWidget() pass reports precisely afterward.
size_t readWidget(const uint8_t* bytes, size_t offset, size_t size, WidgetGridPackage& package, Widget& widget) {
  if (offset + 7 > size) return SIZE_MAX;
  const uint8_t rawType = bytes[offset];
  if (rawType != static_cast<uint8_t>(WidgetType::Kpi) && rawType != static_cast<uint8_t>(WidgetType::List)) {
    return SIZE_MAX;
  }
  widget.type = static_cast<WidgetType>(rawType);
  widget.column = bytes[offset + 1];
  widget.row = bytes[offset + 2];
  widget.columnSpan = bytes[offset + 3];
  widget.rowSpan = bytes[offset + 4];
  widget.style = readU16(bytes + offset + 5);
  offset += 7;

  if (widget.type == WidgetType::Kpi) {
    if (offset + 2 > size) return SIZE_MAX;
    widget.kpi.labelLength = bytes[offset];
    widget.kpi.valueLength = bytes[offset + 1];
    offset += 2;
    if (widget.kpi.labelLength > MAX_KPI_LABEL_SIZE || widget.kpi.valueLength > MAX_KPI_VALUE_SIZE ||
        offset + widget.kpi.labelLength + widget.kpi.valueLength > size) {
      return SIZE_MAX;
    }
    std::copy_n(bytes + offset, widget.kpi.labelLength, widget.kpi.labelBytes.begin());
    offset += widget.kpi.labelLength;
    std::copy_n(bytes + offset, widget.kpi.valueLength, widget.kpi.valueBytes.begin());
    offset += widget.kpi.valueLength;
    return offset;
  }

  if (offset + 2 > size) return SIZE_MAX;
  // A package may declare more list widgets than the decoded form can hold;
  // that is a size failure like any other, not a silently truncated dashboard.
  if (package.listCount >= MAX_LIST_WIDGETS) return SIZE_MAX;
  widget.listIndex = package.listCount;
  ListContent& list = package.lists[package.listCount];
  ++package.listCount;
  list.headingLength = bytes[offset];
  list.rowCount = bytes[offset + 1];
  offset += 2;
  if (list.headingLength > MAX_LIST_HEADING_SIZE || list.rowCount > MAX_LIST_ROWS ||
      offset + list.headingLength > size) {
    return SIZE_MAX;
  }
  std::copy_n(bytes + offset, list.headingLength, list.headingBytes.begin());
  offset += list.headingLength;
  for (uint8_t index = 0; index < list.rowCount; ++index) {
    if (offset + 2 > size) return SIZE_MAX;
    ListRow& row = list.rows[index];
    row.timeLength = bytes[offset];
    row.labelLength = bytes[offset + 1];
    offset += 2;
    if (row.timeLength > MAX_LIST_ROW_TIME_SIZE || row.labelLength > MAX_LIST_ROW_LABEL_SIZE ||
        offset + row.timeLength + row.labelLength > size) {
      return SIZE_MAX;
    }
    std::copy_n(bytes + offset, row.timeLength, row.timeBytes.begin());
    offset += row.timeLength;
    std::copy_n(bytes + offset, row.labelLength, row.labelBytes.begin());
    offset += row.labelLength;
  }
  return offset;
}

}  // namespace

const ListContent* listContentFor(const WidgetGridPackage& package, const Widget& widget) {
  if (widget.type != WidgetType::List || widget.listIndex >= package.listCount) return nullptr;
  return &package.lists[widget.listIndex];
}

Status encodeWidgetGridPackage(const WidgetGridPackage& package, PackageBytes& output, size_t& outputLength) {
  outputLength = 0;
  if (package.schema != SCHEMA_V2) return Status::UnsupportedSchema;
  if (package.templateId != TEMPLATE_WIDGET_GRID) return Status::UnsupportedTemplate;
  if (package.generatedAt == 0 || package.validUntil < package.generatedAt) return Status::InvalidTimestamp;
  if (package.widgetCount > MAX_WIDGETS) return Status::InvalidLength;
  if (package.listCount > MAX_LIST_WIDGETS) return Status::InvalidLength;
  {
    const Status status = validateGlobalStyle(package.style);
    if (status != Status::Ok) return status;
  }

  size_t contentLength = 0;
  for (uint8_t index = 0; index < package.widgetCount; ++index) {
    const Status status = validateWidget(package, package.widgets[index]);
    if (status != Status::Ok) return status;
    contentLength += widgetContentLength(package, package.widgets[index]);
  }
  {
    const Status status = validateNoOverlaps(package);
    if (status != Status::Ok) return status;
  }

  const size_t totalLength = CONTENT_OFFSET + contentLength + CRC_SIZE;
  if (totalLength > MAX_PACKAGE_SIZE) return Status::InvalidLength;

  output.fill(0);
  output[0] = 'X';
  output[1] = '3';
  output[2] = 'D';
  output[3] = 'P';
  output[4] = package.schema;
  output[5] = package.templateId;
  writeU16(output.data() + LENGTH_OFFSET, static_cast<uint16_t>(totalLength));
  writeU32(output.data() + PACKAGE_ID_OFFSET, package.packageId);
  writeU64(output.data() + GENERATED_AT_OFFSET, package.generatedAt);
  writeU64(output.data() + VALID_UNTIL_OFFSET, package.validUntil);
  output[GRID_COLUMNS_OFFSET] = GRID_COLUMNS;
  output[WIDGET_COUNT_OFFSET] = package.widgetCount;
  output[STYLE_OFFSET] = package.style;

  size_t offset = CONTENT_OFFSET;
  for (uint8_t index = 0; index < package.widgetCount; ++index) {
    offset += writeWidget(output.data() + offset, package, package.widgets[index]);
  }
  writeU32(output.data() + offset, crc32(output.data(), offset));
  outputLength = totalLength;
  return Status::Ok;
}

Status decodeWidgetGridPackage(const uint8_t* bytes, const size_t size, WidgetGridPackage& output) {
  if (bytes == nullptr) return Status::InvalidArgument;
  if (size < CONTENT_OFFSET + CRC_SIZE || size > MAX_PACKAGE_SIZE) return Status::InvalidSize;
  if (bytes[0] != 'X' || bytes[1] != '3' || bytes[2] != 'D' || bytes[3] != 'P') return Status::InvalidMagic;
  if (readU16(bytes + LENGTH_OFFSET) != size) return Status::InvalidSize;
  if (bytes[4] != SCHEMA_V2) return Status::UnsupportedSchema;
  if (bytes[5] != TEMPLATE_WIDGET_GRID) return Status::UnsupportedTemplate;
  if (crc32(bytes, size - CRC_SIZE) != readU32(bytes + size - CRC_SIZE)) return Status::InvalidCrc;
  if (bytes[GRID_COLUMNS_OFFSET] != GRID_COLUMNS) return Status::UnsupportedTemplate;

  // Static rather than a stack local: a decoded package covering the whole
  // grid is a couple of kilobytes, more than this task's stack can spare.
  // Decoding stays two-phase - a rejected package must not overwrite the
  // caller's - and the dashboard path is single-threaded, so one workspace is
  // enough.
  static WidgetGridPackage candidate;
  candidate = {};
  candidate.schema = bytes[4];
  candidate.templateId = bytes[5];
  candidate.packageId = readU32(bytes + PACKAGE_ID_OFFSET);
  candidate.generatedAt = readU64(bytes + GENERATED_AT_OFFSET);
  candidate.validUntil = readU64(bytes + VALID_UNTIL_OFFSET);
  candidate.style = bytes[STYLE_OFFSET];
  candidate.widgetCount = bytes[WIDGET_COUNT_OFFSET];
  if (candidate.generatedAt == 0 || candidate.validUntil < candidate.generatedAt) return Status::InvalidTimestamp;
  {
    const Status status = validateGlobalStyle(candidate.style);
    if (status != Status::Ok) return status;
  }
  if (candidate.widgetCount > MAX_WIDGETS) return Status::InvalidLength;

  size_t offset = CONTENT_OFFSET;
  for (uint8_t index = 0; index < candidate.widgetCount; ++index) {
    offset = readWidget(bytes, offset, size, candidate, candidate.widgets[index]);
    if (offset == SIZE_MAX) return Status::InvalidLength;
  }
  if (offset + CRC_SIZE != size) return Status::InvalidLength;

  for (uint8_t index = 0; index < candidate.widgetCount; ++index) {
    const Status status = validateWidget(candidate, candidate.widgets[index]);
    if (status != Status::Ok) return status;
  }
  {
    const Status status = validateNoOverlaps(candidate);
    if (status != Status::Ok) return status;
  }
  candidate.crc = readU32(bytes + size - CRC_SIZE);
  output = candidate;
  return Status::Ok;
}

}  // namespace dashboard
