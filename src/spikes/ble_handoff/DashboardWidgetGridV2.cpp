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
