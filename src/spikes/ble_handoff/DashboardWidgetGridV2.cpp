#include "DashboardWidgetGridV2.h"

#include <algorithm>

namespace dashboard {
namespace v2 {

// Uitgerekend: 16 widgets à 58 = 928, 3 lijsten à 498 = 1494, 5 groepen à 339
// = 1695, plus de kop. Samen ongeveer 4,2 KB. Die staat permanent in DRAM
// omdat renderWidgetGridTemplate zijn pakket static houdt - te groot voor de
// renderstack. 5 KB is de grens met wat lucht; loopt dit erover, verklein dan
// MAX_GROUP_WIDGETS of MAX_LIST_ROWS in plaats van de grens op te rekken.
static_assert(sizeof(WidgetGridPackageV2) <= 5120, "template 4 package past niet in het DRAM-budget");

namespace {

Status validateGroup(const GroupContent& group) {
  if (group.shape > MAX_GROUP_SHAPE) return Status::InvalidArgument;
  if (group.itemCount == 0 || group.itemCount > MAX_GROUP_ITEMS) return Status::InvalidLength;
  if (group.headingLength > MAX_GROUP_HEADING_SIZE) return Status::InvalidLength;
  Status status = validateUtf8(group.headingBytes.data(), group.headingLength);
  if (status != Status::Ok) return status;
  for (uint8_t index = 0; index < group.itemCount; ++index) {
    const GroupItem& item = group.items[index];
    if (item.labelLength > MAX_GROUP_LABEL_SIZE || item.valueLength > MAX_GROUP_VALUE_SIZE ||
        item.detailLength > MAX_GROUP_DETAIL_SIZE) {
      return Status::InvalidLength;
    }
    // FILL_NONE is geldig en betekent "dit item heeft geen meter"; alles
    // daartussen en boven MAX_FILL is een encoderfout.
    if (item.fill > MAX_FILL && item.fill != FILL_NONE) return Status::InvalidArgument;
    status = validateUtf8(item.labelBytes.data(), item.labelLength);
    if (status != Status::Ok) return status;
    status = validateUtf8(item.valueBytes.data(), item.valueLength);
    if (status != Status::Ok) return status;
    status = validateUtf8(item.detailBytes.data(), item.detailLength);
    if (status != Status::Ok) return status;
  }
  return Status::Ok;
}

constexpr size_t LENGTH_OFFSET = 6;
constexpr size_t PACKAGE_ID_OFFSET = 8;
constexpr size_t GENERATED_AT_OFFSET = 12;
constexpr size_t VALID_UNTIL_OFFSET = 20;
constexpr size_t GRID_COLUMNS_OFFSET = 28;
constexpr size_t WIDGET_COUNT_OFFSET = 29;
constexpr size_t STYLE_OFFSET = 30;
constexpr size_t CONTENT_OFFSET = 31;

// Eigen kopieën in plaats van delen met DashboardWidgetGrid.cpp: die staan daar
// in een anonieme namespace, en template 3 aanraken om ze te kunnen delen zou
// precies de scheiding doorbreken die dit bestand bestaansrecht geeft.
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

bool widgetsOverlapV2(const WidgetV2& a, const WidgetV2& b) {
  const bool columnsOverlap = a.column < b.column + b.columnSpan && b.column < a.column + a.columnSpan;
  const bool rowsOverlap = a.row < b.row + b.rowSpan && b.row < a.row + a.rowSpan;
  return columnsOverlap && rowsOverlap;
}

size_t widgetContentLengthV2(const WidgetGridPackageV2& package, const WidgetV2& widget) {
  size_t total = 7;  // type + column + row + columnSpan + rowSpan + style, voor elk widget.
  if (widget.type == WidgetType::Kpi) return total + 2 + widget.kpi.labelLength + widget.kpi.valueLength;
  if (widget.type == WidgetType::Date) return total + 1;
  if (widget.type == WidgetType::Group) {
    const GroupContent* group = groupContentFor(package, widget);
    if (group == nullptr) return total + 3;
    total += 3 + group->headingLength;
    for (uint8_t index = 0; index < group->itemCount; ++index) {
      const GroupItem& item = group->items[index];
      total += 4 + item.labelLength + item.valueLength + item.detailLength;
    }
    return total;
  }
  const ListContentV2* content = listContentFor(package, widget);
  if (content == nullptr) return total + 2;
  total += 2 + content->headingLength;
  for (uint8_t index = 0; index < content->rowCount; ++index) {
    total += 2 + content->rows[index].timeLength + content->rows[index].labelLength;
  }
  return total;
}

size_t writeWidgetV2(uint8_t* out, const WidgetGridPackageV2& package, const WidgetV2& widget) {
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
    return offset + widget.kpi.valueLength;
  }
  if (widget.type == WidgetType::Date) {
    out[offset] = static_cast<uint8_t>(widget.dateField);
    return offset + 1;
  }
  if (widget.type == WidgetType::Group) {
    const GroupContent* group = groupContentFor(package, widget);
    if (group == nullptr) {
      out[offset] = GROUP_SHAPE_ARC;
      out[offset + 1] = 0;
      out[offset + 2] = 0;
      return offset + 3;
    }
    out[offset] = group->shape;
    out[offset + 1] = group->headingLength;
    out[offset + 2] = group->itemCount;
    offset += 3;
    std::copy_n(group->headingBytes.begin(), group->headingLength, out + offset);
    offset += group->headingLength;
    for (uint8_t index = 0; index < group->itemCount; ++index) {
      const GroupItem& item = group->items[index];
      out[offset] = item.labelLength;
      out[offset + 1] = item.valueLength;
      out[offset + 2] = item.detailLength;
      out[offset + 3] = item.fill;
      offset += 4;
      std::copy_n(item.labelBytes.begin(), item.labelLength, out + offset);
      offset += item.labelLength;
      std::copy_n(item.valueBytes.begin(), item.valueLength, out + offset);
      offset += item.valueLength;
      std::copy_n(item.detailBytes.begin(), item.detailLength, out + offset);
      offset += item.detailLength;
    }
    return offset;
  }

  const ListContentV2* content = listContentFor(package, widget);
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
    const ListRowV2& row = content->rows[index];
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

}  // namespace

Status validateWidgetV2(const WidgetGridPackageV2& package, const WidgetV2& widget) {
  if (widget.columnSpan == 0 || widget.columnSpan > GRID_COLUMNS || widget.rowSpan == 0 ||
      widget.rowSpan > MAX_ROW_SPAN) {
    return Status::InvalidArgument;
  }
  if (widget.column >= GRID_COLUMNS || widget.row >= MAX_ROW_SPAN ||
      widget.column + widget.columnSpan > GRID_COLUMNS || widget.row + widget.rowSpan > MAX_ROW_SPAN) {
    return Status::InvalidArgument;
  }
  if (widget.type == WidgetType::Group) {
    const GroupContent* group = groupContentFor(package, widget);
    if (group == nullptr) return Status::InvalidArgument;
    return validateGroup(*group);
  }
  if (widget.type == WidgetType::Kpi) {
    if (widget.kpi.labelLength > MAX_KPI_LABEL_SIZE || widget.kpi.valueLength > MAX_KPI_VALUE_SIZE) {
      return Status::InvalidLength;
    }
    Status status = validateUtf8(widget.kpi.labelBytes.data(), widget.kpi.labelLength);
    if (status != Status::Ok) return status;
    return validateUtf8(widget.kpi.valueBytes.data(), widget.kpi.valueLength);
  }
  if (widget.type == WidgetType::Date) {
    if (static_cast<uint8_t>(widget.dateField) > MAX_DATE_FIELD) return Status::InvalidArgument;
    return Status::Ok;
  }
  if (widget.type == WidgetType::List) {
    const ListContentV2* list = listContentFor(package, widget);
    if (list == nullptr) return Status::InvalidArgument;
    if (list->rowCount > MAX_LIST_ROWS || list->headingLength > MAX_LIST_HEADING_SIZE) {
      return Status::InvalidLength;
    }
    Status status = validateUtf8(list->headingBytes.data(), list->headingLength);
    if (status != Status::Ok) return status;
    for (uint8_t index = 0; index < list->rowCount; ++index) {
      const ListRowV2& row = list->rows[index];
      if (row.timeLength == 0 || row.timeLength > MAX_LIST_ROW_TIME_SIZE || row.labelLength == 0 ||
          row.labelLength > MAX_LIST_ROW_LABEL_SIZE) {
        return Status::InvalidLength;
      }
      status = validateUtf8(row.timeBytes.data(), row.timeLength);
      if (status != Status::Ok) return status;
      status = validateUtf8(row.labelBytes.data(), row.labelLength);
      if (status != Status::Ok) return status;
    }
    return Status::Ok;
  }
  return Status::InvalidArgument;
}

Status encodeWidgetGridPackageV2(const WidgetGridPackageV2& package, PackageBytes& output, size_t& outputLength) {
  if (package.schema != SCHEMA_V2) return Status::UnsupportedSchema;
  if (package.templateId != TEMPLATE_WIDGET_GRID_V2) return Status::UnsupportedTemplate;
  if (package.generatedAt == 0 || package.validUntil < package.generatedAt) return Status::InvalidTimestamp;
  if (package.widgetCount > MAX_WIDGETS) return Status::InvalidLength;
  if (package.listCount > MAX_LIST_WIDGETS || package.groupCount > MAX_GROUP_WIDGETS) return Status::InvalidLength;

  size_t contentLength = 0;
  for (uint8_t index = 0; index < package.widgetCount; ++index) {
    const Status status = validateWidgetV2(package, package.widgets[index]);
    if (status != Status::Ok) return status;
    contentLength += widgetContentLengthV2(package, package.widgets[index]);
  }
  // Overlap pas nadat elk widget op zichzelf klopt, zodat een fout in één
  // widget niet als overlap gemeld wordt.
  for (uint8_t a = 0; a < package.widgetCount; ++a) {
    for (uint8_t b = static_cast<uint8_t>(a + 1); b < package.widgetCount; ++b) {
      if (widgetsOverlapV2(package.widgets[a], package.widgets[b])) return Status::InvalidArgument;
    }
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
    offset += writeWidgetV2(output.data() + offset, package, package.widgets[index]);
  }

  const uint32_t crc = crc32(output.data(), offset);
  writeU32(output.data() + offset, crc);
  outputLength = totalLength;
  return Status::Ok;
}

const ListContentV2* listContentFor(const WidgetGridPackageV2& package, const WidgetV2& widget) {
  if (widget.type != WidgetType::List || widget.listIndex >= package.listCount) return nullptr;
  return &package.lists[widget.listIndex];
}

const GroupContent* groupContentFor(const WidgetGridPackageV2& package, const WidgetV2& widget) {
  if (widget.type != WidgetType::Group || widget.groupIndex >= package.groupCount) return nullptr;
  return &package.groups[widget.groupIndex];
}

}  // namespace v2
}  // namespace dashboard
