#include "DashboardGridRenderer.h"

#ifdef CROSSINK_BLE_HANDOFF_READER

#include <GfxRenderer.h>

#include <algorithm>
#include <array>
#include <cstring>

#include "DashboardGridLayout.h"
#include "fontIds.h"

namespace dashboard {
namespace {

constexpr int TILE_PADDING = 6;
constexpr int TILE_CORNER_RADIUS = 6;

void drawTextCenteredInRect(GfxRenderer& renderer, const int fontId, const WidgetRect& rect, const char* text,
                            const EpdFontFamily::Style style, const int y) {
  const std::string bounded = renderer.truncatedText(fontId, text, rect.width - 2 * TILE_PADDING, style);
  const int textWidth = renderer.getTextWidth(fontId, bounded.c_str(), style);
  const int x = rect.x + std::max(TILE_PADDING, (rect.width - textWidth) / 2);
  renderer.drawText(fontId, x, rect.y + y, bounded.c_str(), true, style);
}

void renderKpiWidget(GfxRenderer& renderer, const WidgetRect& rect, const KpiContent& kpi) {
  renderer.drawRoundedRect(rect.x + 2, rect.y + 2, rect.width - 4, rect.height - 4, 1, TILE_CORNER_RADIUS, true);

  char value[MAX_KPI_VALUE_SIZE + 1];
  std::copy_n(kpi.valueBytes.begin(), kpi.valueLength, value);
  value[kpi.valueLength] = '\0';
  char label[MAX_KPI_LABEL_SIZE + 1];
  std::copy_n(kpi.labelBytes.begin(), kpi.labelLength, label);
  label[kpi.labelLength] = '\0';

  const int valueY = rect.height / 2 - 4;
  const int labelY = rect.height - TILE_PADDING - renderer.getFontAscenderSize(SMALL_FONT_ID);
  drawTextCenteredInRect(renderer, UI_10_FONT_ID, rect, value, EpdFontFamily::BOLD, valueY);
  drawTextCenteredInRect(renderer, SMALL_FONT_ID, rect, label, EpdFontFamily::REGULAR, labelY);
}

void renderListWidget(GfxRenderer& renderer, const WidgetRect& rect, const ListContent& list) {
  int y = TILE_PADDING + renderer.getFontAscenderSize(SMALL_FONT_ID);
  if (list.headingLength > 0) {
    char heading[MAX_LIST_HEADING_SIZE + 1];
    std::copy_n(list.headingBytes.begin(), list.headingLength, heading);
    heading[list.headingLength] = '\0';
    renderer.drawText(SMALL_FONT_ID, rect.x + TILE_PADDING, rect.y + y, heading, true, EpdFontFamily::BOLD);
    y += renderer.getLineHeight(SMALL_FONT_ID);
  }

  const int lineHeight = renderer.getLineHeight(UI_10_FONT_ID);
  // time + two-space separator + label, bounded well under the stack-usage
  // guidance (CLAUDE.md #1) even at every field's maximum size.
  char line[MAX_LIST_ROW_TIME_SIZE + 2 + MAX_LIST_ROW_LABEL_SIZE + 1];
  for (uint8_t index = 0; index < list.rowCount && y + lineHeight <= rect.height - TILE_PADDING; ++index) {
    const ListRow& row = list.rows[index];
    size_t offset = 0;
    std::copy_n(row.timeBytes.begin(), row.timeLength, line + offset);
    offset += row.timeLength;
    line[offset++] = ' ';
    line[offset++] = ' ';
    std::copy_n(row.labelBytes.begin(), row.labelLength, line + offset);
    offset += row.labelLength;
    line[offset] = '\0';

    const std::string bounded =
        renderer.truncatedText(UI_10_FONT_ID, line, rect.width - 2 * TILE_PADDING, EpdFontFamily::REGULAR);
    renderer.drawText(UI_10_FONT_ID, rect.x + TILE_PADDING, rect.y + y, bounded.c_str());
    y += lineHeight;
  }
}

}  // namespace

void renderWidgetGrid(GfxRenderer& renderer, const WidgetGridPackage& package) {
  std::array<WidgetRect, MAX_WIDGETS> rects{};
  computeGridLayout(package, renderer.getScreenWidth(), renderer.getScreenHeight(), rects);
  for (uint8_t index = 0; index < package.widgetCount; ++index) {
    const Widget& widget = package.widgets[index];
    switch (widget.type) {
      case WidgetType::Kpi:
        renderKpiWidget(renderer, rects[index], widget.kpi);
        break;
      case WidgetType::List:
        renderListWidget(renderer, rects[index], widget.list);
        break;
    }
  }
}

}  // namespace dashboard

#endif
