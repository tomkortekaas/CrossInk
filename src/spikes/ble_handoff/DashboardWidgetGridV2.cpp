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
