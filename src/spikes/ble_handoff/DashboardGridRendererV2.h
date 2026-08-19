#pragma once

#ifdef CROSSINK_BLE_HANDOFF_READER

#include "DashboardWidgetGridV2.h"

class GfxRenderer;

namespace dashboard {
namespace v2 {

// Draws the Group widgets of `package` into the renderer's current logical
// canvas using computeGridLayoutV2() for placement. Template 4's design uses
// only Group widgets, so KPI/List/Date widgets are skipped rather than drawn
// with template 3 code. Does not clear the screen or call displayBuffer(); the
// caller controls the render cycle (matches BleHandoffReaderProbe).
void renderWidgetGridV2(GfxRenderer& renderer, const WidgetGridPackageV2& package);

}  // namespace v2
}  // namespace dashboard

#endif
