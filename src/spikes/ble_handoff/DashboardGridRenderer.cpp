#include "DashboardGridRenderer.h"

#ifdef CROSSINK_BLE_HANDOFF_READER

#include <GfxRenderer.h>

#include <algorithm>
#include <array>
#include <iterator>
#include <cstring>

#include "DashboardGridLayout.h"
#include "components/icons/dashboardIconTable.h"
#include "fontIds.h"

namespace dashboard {
namespace {

constexpr int TILE_CORNER_RADIUS = 6;

// Indexed by the package's density field, which decoding has already bounded to
// MAX_DENSITY — see validateGlobalStyle() in DashboardWidgetGrid.cpp. Every
// table in this file is indexed by an already-validated field for that reason;
// none of them may be reached with an out-of-range value.
constexpr int TILE_PADDING_FOR_DENSITY[] = {4, 8, 14};
static_assert(std::size(TILE_PADDING_FOR_DENSITY) == MAX_DENSITY + 1, "one padding per density");

// Indexed by a widget's sizeRung. One family (Lexend Deca) so the dashboard
// reads as one thing; only the size changes. All four are already registered by
// main.cpp, so the ladder costs no flash.
constexpr int VALUE_FONT_FOR_RUNG[] = {LEXENDDECA_10_FONT_ID, LEXENDDECA_12_FONT_ID, LEXENDDECA_14_FONT_ID,
                                       LEXENDDECA_16_FONT_ID};
// The label sits a fixed step below whatever the value is, rather than scaling
// with it: the point of the rung is the gap between value and label.
constexpr int LABEL_FONT_ID = LEXENDDECA_10_FONT_ID;

int tilePadding(const WidgetGridPackage& package) { return TILE_PADDING_FOR_DENSITY[globalDensity(package.style)]; }

// True when the tile is painted solid black, so its text and icon must be drawn
// in white to be visible at all.
bool isInverted(const Widget& widget) { return widgetEmphasis(widget.style) == EMPHASIS_INVERTED; }

// Fills the tile behind its content. Pure 1-bit ordered dithering
// (GfxRenderer::drawPixelDither), so this needs no grayscale refresh and costs
// nothing beyond the pixels it sets.
void fillTile(GfxRenderer& renderer, const WidgetRect& rect, const uint8_t emphasis) {
  switch (emphasis) {
    case EMPHASIS_NONE:
      return;
    case EMPHASIS_LIGHT:
      renderer.fillRectDither(rect.x, rect.y, rect.width, rect.height, Color::LightGray);
      return;
    case EMPHASIS_DARK:
      renderer.fillRectDither(rect.x, rect.y, rect.width, rect.height, Color::DarkGray);
      return;
    case EMPHASIS_INVERTED:
      renderer.fillRect(rect.x, rect.y, rect.width, rect.height, true);
      return;
    default:
      // Unreachable: decoding bounds emphasis to two bits, all four named.
      return;
  }
}

void drawTextCenteredInRect(GfxRenderer& renderer, const int fontId, const WidgetRect& rect, const char* text,
                            const EpdFontFamily::Style style, const int y, const int padding, const bool ink) {
  const std::string bounded = renderer.truncatedText(fontId, text, rect.width - 2 * padding, style);
  const int textWidth = renderer.getTextWidth(fontId, bounded.c_str(), style);
  const int x = rect.x + std::max(padding, (rect.width - textWidth) / 2);
  renderer.drawText(fontId, x, rect.y + y, bounded.c_str(), ink, style);
}

// Draws the widget's icon centred above its value, and returns how much
// vertical room it claimed (0 when the widget has no icon). The larger variant
// is used only on tiles more than one row tall, where 24px would look lost.
int drawTileIcon(GfxRenderer& renderer, const WidgetRect& rect, const Widget& widget, const int padding) {
  const uint8_t iconId = widgetIconId(widget.style);
  if (iconId == 0) return 0;

  const bool large = widget.rowSpan > 1;
  const freeink::Icon* icon = large ? DASHBOARD_ICONS_32[iconId] : DASHBOARD_ICONS_24[iconId];
  if (icon == nullptr) return 0;

  const int x = rect.x + (rect.width - icon->w) / 2;
  const int y = rect.y + padding;
  if (isInverted(widget)) {
    renderer.drawIconInverted(icon->bits, x, y, icon->w, icon->h);
  } else {
    renderer.drawIcon(icon->bits, x, y, icon->w, icon->h);
  }
  return icon->h;
}

void renderKpiWidget(GfxRenderer& renderer, const WidgetRect& rect, const Widget& widget, const int padding) {
  const bool ink = !isInverted(widget);
  fillTile(renderer, rect, widgetEmphasis(widget.style));
  const int iconHeight = drawTileIcon(renderer, rect, widget, padding);

  const KpiContent& kpi = widget.kpi;
  char value[MAX_KPI_VALUE_SIZE + 1];
  std::copy_n(kpi.valueBytes.begin(), kpi.valueLength, value);
  value[kpi.valueLength] = '\0';
  char label[MAX_KPI_LABEL_SIZE + 1];
  std::copy_n(kpi.labelBytes.begin(), kpi.labelLength, label);
  label[kpi.labelLength] = '\0';

  const int valueFontId = VALUE_FONT_FOR_RUNG[widgetSizeRung(widget.style)];
  // Without an icon the value sits on the tile's centre line, as before. With
  // one, it drops below the icon so the two never collide on a short tile.
  const int valueY = iconHeight == 0
                         ? rect.height / 2 - 4
                         : std::max(rect.height / 2 - 4, padding + iconHeight + renderer.getFontAscenderSize(valueFontId));
  const int labelY = rect.height - padding - renderer.getFontAscenderSize(LABEL_FONT_ID);
  drawTextCenteredInRect(renderer, valueFontId, rect, value, EpdFontFamily::BOLD, valueY, padding, ink);
  drawTextCenteredInRect(renderer, LABEL_FONT_ID, rect, label, EpdFontFamily::REGULAR, labelY, padding, ink);
}

void renderListWidget(GfxRenderer& renderer, const WidgetRect& rect, const Widget& widget, const ListContent& list,
                      const int padding, const bool dividers) {
  const bool ink = !isInverted(widget);
  fillTile(renderer, rect, widgetEmphasis(widget.style));

  const int rowFontId = VALUE_FONT_FOR_RUNG[widgetSizeRung(widget.style)];
  int y = padding + renderer.getFontAscenderSize(LABEL_FONT_ID);
  if (list.headingLength > 0) {
    char heading[MAX_LIST_HEADING_SIZE + 1];
    std::copy_n(list.headingBytes.begin(), list.headingLength, heading);
    heading[list.headingLength] = '\0';
    renderer.drawText(LABEL_FONT_ID, rect.x + padding, rect.y + y, heading, ink, EpdFontFamily::BOLD);
    y += renderer.getLineHeight(LABEL_FONT_ID);
  }

  const int lineHeight = renderer.getLineHeight(rowFontId);
  // time + two-space separator + label, bounded well under the stack-usage
  // guidance (CLAUDE.md #1) even at every field's maximum size.
  char line[MAX_LIST_ROW_TIME_SIZE + 2 + MAX_LIST_ROW_LABEL_SIZE + 1];
  for (uint8_t index = 0; index < list.rowCount && y + lineHeight <= rect.height - padding; ++index) {
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
        renderer.truncatedText(rowFontId, line, rect.width - 2 * padding, EpdFontFamily::REGULAR);
    renderer.drawText(rowFontId, rect.x + padding, rect.y + y, bounded.c_str(), ink);
    y += lineHeight;

    // A rule under every row but the last, so the list reads as rows without
    // the heavier per-tile border the grid pass would draw.
    if (dividers && index + 1 < list.rowCount && y + lineHeight <= rect.height - padding) {
      const int ruleY = rect.y + y - lineHeight + renderer.getFontAscenderSize(rowFontId) / 2;
      renderer.fillRectDither(rect.x + padding, ruleY, rect.width - 2 * padding, 1,
                              ink ? Color::DarkGray : Color::White);
    }
  }
}

// One pass over every tile after its content is drawn, so a list widget is
// framed exactly like a KPI tile. Previously only KPI tiles drew a border, from
// inside their own render function, which is why the grid looked half framed.
void drawTileBorders(GfxRenderer& renderer, const WidgetGridPackage& package,
                     const std::array<WidgetRect, MAX_WIDGETS>& rects, const int gridRight,
                     const int gridBottom) {
  const uint8_t level = globalBorderLevel(package.style);
  if (level == BORDER_NONE) return;

  for (uint8_t index = 0; index < package.widgetCount; ++index) {
    const WidgetRect& rect = rects[index];
    switch (level) {
      case BORDER_HAIRLINE:
        // Only the shared edges, not a box: a rule down the right side and
        // along the bottom. Tiles on the grid's far edge get nothing, so the
        // dashboard has no outer frame.
        if (rect.x + rect.width < gridRight) {
          renderer.fillRect(rect.x + rect.width - 1, rect.y, 1, rect.height, true);
        }
        if (rect.y + rect.height < gridBottom) {
          renderer.fillRect(rect.x, rect.y + rect.height - 1, rect.width, 1, true);
        }
        break;
      case BORDER_LIGHT: {
        // No dithered-outline primitive exists, so the frame is four one-pixel
        // strips filled with the 25% pattern.
        const int x = rect.x + 2;
        const int y = rect.y + 2;
        const int w = rect.width - 4;
        const int h = rect.height - 4;
        renderer.fillRectDither(x, y, w, 1, Color::DarkGray);
        renderer.fillRectDither(x, y + h - 1, w, 1, Color::DarkGray);
        renderer.fillRectDither(x, y, 1, h, Color::DarkGray);
        renderer.fillRectDither(x + w - 1, y, 1, h, Color::DarkGray);
        break;
      }
      case BORDER_SOLID:
        renderer.drawRoundedRect(rect.x + 2, rect.y + 2, rect.width - 4, rect.height - 4, 1, TILE_CORNER_RADIUS, true);
        break;
      default:
        // Unreachable: decoding bounds borderLevel to MAX_BORDER_LEVEL.
        break;
    }
  }
}

}  // namespace

void renderWidgetGrid(GfxRenderer& renderer, const WidgetGridPackage& package) {
  // The panel's outermost pixels sit under the bezel, so the grid is inset by
  // the viewable margins plus a little breathing room — a full-width agenda
  // drawn from x=0 reads as if it is falling off the screen. GRID_MARGIN is the
  // one number to turn if the dashboard wants to sit tighter or looser.
  int marginTop = 0;
  int marginRight = 0;
  int marginBottom = 0;
  int marginLeft = 0;
  renderer.getOrientedViewableTRBL(&marginTop, &marginRight, &marginBottom, &marginLeft);
  constexpr int GRID_MARGIN = 6;
  const int originX = marginLeft + GRID_MARGIN;
  const int originY = marginTop + GRID_MARGIN;
  const int canvasWidth = renderer.getScreenWidth() - originX - marginRight - GRID_MARGIN;
  const int canvasHeight = renderer.getScreenHeight() - originY - marginBottom - GRID_MARGIN;

  std::array<WidgetRect, MAX_WIDGETS> rects{};
  computeGridLayout(package, canvasWidth, canvasHeight, rects, originX, originY);
  const int padding = tilePadding(package);
  const bool dividers = globalListDividers(package.style);
  for (uint8_t index = 0; index < package.widgetCount; ++index) {
    const Widget& widget = package.widgets[index];
    switch (widget.type) {
      case WidgetType::Kpi:
        renderKpiWidget(renderer, rects[index], widget, padding);
        break;
      case WidgetType::List:
        // A decoded package always carries content for every list widget; skip
        // rather than draw garbage if a caller hand-built one that does not.
        if (const ListContent* content = listContentFor(package, widget); content != nullptr) {
          renderListWidget(renderer, rects[index], widget, *content, padding, dividers);
        }
        break;
    }
  }
  drawTileBorders(renderer, package, rects, originX + canvasWidth, originY + canvasHeight);
}

}  // namespace dashboard

#endif
