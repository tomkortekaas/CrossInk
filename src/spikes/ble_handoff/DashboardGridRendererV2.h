#pragma once

#ifdef CROSSINK_BLE_HANDOFF_READER

#include "DashboardWidgetGridV2.h"

class GfxRenderer;

namespace dashboard {
namespace v2 {

// Draws every widget of `package` into the renderer's current logical canvas,
// using computeGridLayoutV2() for placement. Does not clear the screen or call
// displayBuffer(); the caller controls the render cycle (matches
// BleHandoffReaderProbe).
//
// This used to draw only Group widgets, on the assumption that template 4's
// design needed nothing else. That was wrong twice over: the design carries the
// agenda and the messages as List widgets, and the date tile is a Date widget
// precisely so it stays correct when the phone is out of range. The cost of the
// assumption was a date tile that vanished from the panel without a trace,
// because an unhandled type was silently skipped.
//
// The dispatch is now a switch over WidgetType with no default, so adding a
// value to the enum breaks the build instead of quietly dropping the tile.
void renderWidgetGridV2(GfxRenderer& renderer, const WidgetGridPackageV2& package);

}  // namespace v2
}  // namespace dashboard

#endif
