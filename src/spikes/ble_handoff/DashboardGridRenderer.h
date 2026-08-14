#pragma once

#ifdef CROSSINK_BLE_HANDOFF_READER

#include "DashboardWidgetGrid.h"

class GfxRenderer;

namespace dashboard {

// Draws `package` into the renderer's current logical canvas using
// computeGridLayout() for placement: KPI widgets as a bordered value+label
// tile, List widgets as a heading plus up to their configured row count.
// Does not clear the screen or call displayBuffer(); the caller controls the
// render cycle (matches BleHandoffReaderProbe::renderAgendaCard()).
void renderWidgetGrid(GfxRenderer& renderer, const WidgetGridPackage& package);

}  // namespace dashboard

#endif
