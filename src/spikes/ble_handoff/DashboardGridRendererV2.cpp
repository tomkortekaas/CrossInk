#include "DashboardGridRendererV2.h"

#ifdef CROSSINK_BLE_HANDOFF_READER

#include <GfxRenderer.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <string>

#include "DashboardArc.h"
#include "DashboardGridLayoutV2.h"
#include "fontIds.h"

namespace dashboard {
namespace v2 {
namespace {

constexpr int GROUP_PADDING = 6;
constexpr int GROUP_TEXT_PADDING = 2;
constexpr int GROUP_STACK_GAP = 4;
constexpr int GROUP_ARC_LABEL_GAP = 4;
constexpr int ARC_VALUE_MARGIN = 4;

constexpr int BAR_VALUE_LABEL_GAP = 12;
constexpr int BAR_VALUE_BAR_GAP = 8;
constexpr int BAR_HEIGHT = 12;
constexpr int BAR_TICK_OVERHANG = 6;

constexpr int HEADING_FONT_ID = LEXENDDECA_14_FONT_ID;
// Het plan noemt 11 px voor het label, maar de beschikbare Lexend-ladder heeft
// geen 11: 12 voor het label en 10 voor de detailregel houdt de hierarchie
// intact en gebruikt alleen reeds geregistreerde fonts.
constexpr int LABEL_FONT_ID = LEXENDDECA_12_FONT_ID;
constexpr int DETAIL_FONT_ID = LEXENDDECA_10_FONT_ID;

// Eigen ladder voor de meterwaarden, losgekoppeld van sizeRung. Dat veld is
// twee bits en indexeert een tabel van vier; deze ladder heeft er meer en wordt
// met een pas-zoekende keuze gebruikt, zoals fittingFontId van template 3.
constexpr int GAUGE_FONT_LADDER[] = {LEXENDDECA_14_FONT_ID, LEXENDDECA_18_BOLD_DASH_FONT_ID,
                                     LEXENDDECA_22_BOLD_DASH_FONT_ID, LEXENDDECA_28_BOLD_DASH_FONT_ID,
                                     LEXENDDECA_34_BOLD_DASH_FONT_ID};

struct ArcPlotContext {
  GfxRenderer* renderer;
  bool filled;
};

template <size_t N>
void copyTextBytes(const std::array<uint8_t, N>& bytes, const uint8_t length, char (&out)[N + 1]) {
  const size_t bounded = std::min<size_t>(length, N);
  std::copy_n(bytes.begin(), bounded, out);
  out[bounded] = '\0';
}

// GfxRenderer::drawText neemt de BOVENKANT van de tekst, niet de basislijn: het
// telt de ascender er zelf bij op. `y` is hier dus de bovenkant van het
// tekstblok, net als in template 3. Dezelfde fout als daar (ascender er nog eens
// bijtellen) zou de waarde en het label over elkaar schuiven.
void drawTextCenteredInRect(GfxRenderer& renderer, const int fontId, const WidgetRectV2& rect, const char* text,
                            const EpdFontFamily::Style style, const int y, const int padding, const bool ink) {
  const int maxWidth = std::max(1, rect.width - 2 * padding);
  const std::string bounded = renderer.truncatedText(fontId, text, maxWidth, style);
  const int textWidth = renderer.getTextWidth(fontId, bounded.c_str(), style);
  const int x = rect.x + std::max(padding, (rect.width - textWidth) / 2);
  renderer.drawText(fontId, x, rect.y + y, bounded.c_str(), ink, style);
}

// De grootste maat uit de ladder waarvan `text` nog binnen `maxWidth` past.
// Valt terug op de kleinste maat, waarna de caller (drawTextCenteredInRect)
// zo nodig trunkeert - dezelfde fallback als template 3.
int fittingGaugeFontId(const GfxRenderer& renderer, const char* text, const int maxWidth) {
  for (int index = static_cast<int>(std::size(GAUGE_FONT_LADDER)) - 1; index >= 0; --index) {
    if (renderer.getTextWidth(GAUGE_FONT_LADDER[index], text, EpdFontFamily::BOLD) <= maxWidth) {
      return GAUGE_FONT_LADDER[index];
    }
  }
  return GAUGE_FONT_LADDER[0];
}

void plotArcPixel(const int x, const int y, void* const context) {
  auto* ctx = static_cast<ArcPlotContext*>(context);
  if (ctx == nullptr || ctx->renderer == nullptr) return;
  if (ctx->filled) {
    ctx->renderer->drawPixel(x, y, true);
  } else {
    ctx->renderer->fillRectDither(x, y, 1, 1, Color::LightGray);
  }
}

// Waarde + label + detail als gecentreerd tekstblok, zonder meter. Gebruikt
// voor STRIP-items en voor Group-items met FILL_NONE: die twee mogen niet op
// een lege ring lijken, en een lege thuisaccu (fill 0) moet juist wél een
// meter tekenen.
void renderTextBlock(GfxRenderer& renderer, const WidgetRectV2& rect, const char* value, const char* label,
                     const char* detail) {
  const bool hasValue = value[0] != '\0';
  const bool hasLabel = label[0] != '\0';
  const bool hasDetail = detail[0] != '\0';

  const int valueFontId = hasValue ? fittingGaugeFontId(renderer, value, rect.width - 2 * GROUP_TEXT_PADDING) : 0;
  const int valueAscender = hasValue ? renderer.getFontAscenderSize(valueFontId) : 0;
  const int labelAscender = hasLabel ? renderer.getFontAscenderSize(LABEL_FONT_ID) : 0;
  const int detailAscender = hasDetail ? renderer.getFontAscenderSize(DETAIL_FONT_ID) : 0;

  int blockHeight = 0;
  if (hasValue) blockHeight += valueAscender;
  if (hasValue && hasLabel) blockHeight += GROUP_STACK_GAP;
  if (hasLabel) blockHeight += labelAscender;
  if ((hasValue || hasLabel) && hasDetail) blockHeight += GROUP_STACK_GAP;
  if (hasDetail) blockHeight += detailAscender;

  int y = std::max(0, (rect.height - blockHeight) / 2);
  if (hasValue) {
    drawTextCenteredInRect(renderer, valueFontId, rect, value, EpdFontFamily::BOLD, y, GROUP_TEXT_PADDING, true);
    y += valueAscender;
    if (hasLabel || hasDetail) y += GROUP_STACK_GAP;
  }
  if (hasLabel) {
    drawTextCenteredInRect(renderer, LABEL_FONT_ID, rect, label, EpdFontFamily::REGULAR, y, GROUP_TEXT_PADDING, true);
    y += labelAscender;
    if (hasDetail) y += GROUP_STACK_GAP;
  }
  if (hasDetail) {
    drawTextCenteredInRect(renderer, DETAIL_FONT_ID, rect, detail, EpdFontFamily::REGULAR, y, GROUP_TEXT_PADDING, true);
  }
}

// De boog schaalt mee met zijn item: buitendiameter ~70% van de kleinste
// binnenmaat, dikte ~straal/3,5. Een vaste maat maakt een clusteritem van 126 px
// te krap en een stappen-tegel van 168 px te leeg.
void renderArcGroup(GfxRenderer& renderer, const WidgetRectV2& rect, const GroupContent& group, const int contentTop,
                    const int contentHeight) {
  if (group.itemCount == 0) return;
  const int itemCount = group.itemCount;
  const int itemWidth = rect.width / itemCount;

  for (uint8_t index = 0; index < group.itemCount; ++index) {
    const GroupItem& item = group.items[index];
    char value[MAX_GROUP_VALUE_SIZE + 1];
    char label[MAX_GROUP_LABEL_SIZE + 1];
    char detail[MAX_GROUP_DETAIL_SIZE + 1];
    copyTextBytes(item.valueBytes, item.valueLength, value);
    copyTextBytes(item.labelBytes, item.labelLength, label);
    copyTextBytes(item.detailBytes, item.detailLength, detail);

    const int itemLeft = rect.x + index * itemWidth;
    const WidgetRectV2 itemRect{itemLeft, contentTop, itemWidth, contentHeight};

    if (item.fill == FILL_NONE) {
      renderTextBlock(renderer, itemRect, value, label, detail);
      continue;
    }

    const bool hasLabel = label[0] != '\0';
    const bool hasDetail = detail[0] != '\0';
    const int labelAscender = hasLabel ? renderer.getFontAscenderSize(LABEL_FONT_ID) : 0;
    const int detailAscender = hasDetail ? renderer.getFontAscenderSize(DETAIL_FONT_ID) : 0;
    int labelBlockHeight = 0;
    if (hasLabel) labelBlockHeight += labelAscender;
    if (hasLabel && hasDetail) labelBlockHeight += GROUP_STACK_GAP;
    if (hasDetail) labelBlockHeight += detailAscender;
    const int labelGap = labelBlockHeight > 0 ? GROUP_ARC_LABEL_GAP : 0;

    const int ringSpaceHeight = std::max(1, contentHeight - labelBlockHeight - labelGap);
    const int innerMeasure = std::min(itemWidth, ringSpaceHeight);
    const int outerDiameter = std::max(6, innerMeasure * 7 / 10);
    // `radius` is de straal die forEachArcPixel meekrijgt; met dikte = straal/3,5
    // is de binnenruimte 2*straal - dikte. Voor een clusteritem van 126 px is
    // dat ~76 px, precies de maat uit de uitrastering waarop de fontkeuze is
    // gebaseerd.
    const int outerRadius = outerDiameter / 2;
    const int thickness = std::max(2, (outerRadius * 2 + 3) / 7);

    const int blockHeight = 2 * outerRadius + labelGap + labelBlockHeight;
    const int blockTop = contentTop + std::max(0, (contentHeight - blockHeight) / 2);
    const int centerX = itemLeft + itemWidth / 2;
    const int centerY = blockTop + outerRadius;

    ArcPlotContext trackCtx{&renderer, false};
    dashboard::forEachArcPixel(centerX, centerY, outerRadius, thickness, 135, 270, plotArcPixel, &trackCtx);
    const int fillSweep = dashboard::arcSweepForFill(item.fill, 270);
    ArcPlotContext fillCtx{&renderer, true};
    dashboard::forEachArcPixel(centerX, centerY, outerRadius, thickness, 135, fillSweep, plotArcPixel, &fillCtx);

    if (value[0] != '\0') {
      // forEachArcPixel maakt van `radius` een band van radius±thickness/2; de
      // werkelijke binnenruimte is dus 2*radius - thickness.
      const int innerDiameter = std::max(1, 2 * outerRadius - thickness);
      const int valueMaxWidth = std::max(1, innerDiameter - 2 * ARC_VALUE_MARGIN);
      const int valueFontId = fittingGaugeFontId(renderer, value, valueMaxWidth);
      const int valueAscender = renderer.getFontAscenderSize(valueFontId);
      const WidgetRectV2 ringRect{centerX - outerRadius, blockTop, 2 * outerRadius, 2 * outerRadius};
      const int ringPadding = thickness / 2 + ARC_VALUE_MARGIN;
      const int valueTop = std::max(0, centerY - ringRect.y - valueAscender / 2);
      drawTextCenteredInRect(renderer, valueFontId, ringRect, value, EpdFontFamily::BOLD, valueTop, ringPadding, true);
    }

    int cursorY = blockTop + 2 * outerRadius;
    if (labelBlockHeight > 0) cursorY += labelGap;
    if (hasLabel) {
      drawTextCenteredInRect(renderer, LABEL_FONT_ID, itemRect, label, EpdFontFamily::REGULAR, cursorY - contentTop,
                             GROUP_TEXT_PADDING, true);
      cursorY += labelAscender;
      if (hasDetail) cursorY += GROUP_STACK_GAP;
    }
    if (hasDetail) {
      drawTextCenteredInRect(renderer, DETAIL_FONT_ID, itemRect, detail, EpdFontFamily::REGULAR, cursorY - contentTop,
                             GROUP_TEXT_PADDING, true);
    }
  }
}

void renderBarRow(GfxRenderer& renderer, const WidgetRectV2& rect, const GroupItem& item, const int rowTop,
                  const int rowHeight) {
  char value[MAX_GROUP_VALUE_SIZE + 1];
  char label[MAX_GROUP_LABEL_SIZE + 1];
  copyTextBytes(item.valueBytes, item.valueLength, value);
  copyTextBytes(item.labelBytes, item.labelLength, label);

  const int innerLeft = rect.x + GROUP_PADDING;
  const int innerWidth = rect.width - 2 * GROUP_PADDING;
  const bool hasValue = value[0] != '\0';
  const bool hasLabel = label[0] != '\0';

  const int labelWidth = hasLabel ? renderer.getTextWidth(LABEL_FONT_ID, label, EpdFontFamily::REGULAR) : 0;
  const int valueMaxWidth = std::max(1, innerWidth - (hasLabel ? labelWidth + BAR_VALUE_LABEL_GAP : 0));
  const int valueFontId = hasValue ? fittingGaugeFontId(renderer, value, valueMaxWidth) : 0;
  const int valueAscender = hasValue ? renderer.getFontAscenderSize(valueFontId) : 0;
  const int labelAscender = hasLabel ? renderer.getFontAscenderSize(LABEL_FONT_ID) : 0;

  const int textLineHeight = std::max(valueAscender, labelAscender);
  const int blockHeight = textLineHeight + BAR_VALUE_BAR_GAP + BAR_HEIGHT;
  const int textTop = rowTop + std::max(0, (rowHeight - blockHeight) / 2);

  if (hasValue) {
    const std::string bounded = renderer.truncatedText(valueFontId, value, valueMaxWidth, EpdFontFamily::BOLD);
    renderer.drawText(valueFontId, innerLeft, textTop, bounded.c_str(), true, EpdFontFamily::BOLD);
  }
  if (hasLabel) {
    const std::string bounded = renderer.truncatedText(LABEL_FONT_ID, label, innerWidth, EpdFontFamily::REGULAR);
    const int drawnLabelWidth = renderer.getTextWidth(LABEL_FONT_ID, bounded.c_str(), EpdFontFamily::REGULAR);
    const int labelTop = textTop + std::max(0, (valueAscender - labelAscender) / 2);
    renderer.drawText(LABEL_FONT_ID, innerLeft + innerWidth - drawnLabelWidth, labelTop, bounded.c_str(), true,
                      EpdFontFamily::REGULAR);
  }

  if (item.fill != FILL_NONE) {
    const int barTop = textTop + textLineHeight + BAR_VALUE_BAR_GAP;
    const int fillBounded = std::min<int>(item.fill, MAX_FILL);
    const int filledWidth = innerWidth * fillBounded / MAX_FILL;
    if (filledWidth > 0) {
      renderer.fillRect(innerLeft, barTop, filledWidth, BAR_HEIGHT, true);
    }
    if (filledWidth < innerWidth) {
      renderer.fillRectDither(innerLeft + filledWidth, barTop, innerWidth - filledWidth, BAR_HEIGHT, Color::LightGray);
    }
    // Het streepje van 2 px op de grens, 6 px boven en onder de balk. Bij 0 en
    // 100 blijft het binnen de balkuiteinden in plaats van erbuiten te steken.
    const int markerX = std::clamp(innerLeft + filledWidth - 1, innerLeft, innerLeft + innerWidth - 2);
    renderer.fillRect(markerX, barTop - BAR_TICK_OVERHANG, 2, BAR_HEIGHT + 2 * BAR_TICK_OVERHANG, true);
  }
}

void renderBarGroup(GfxRenderer& renderer, const WidgetRectV2& rect, const GroupContent& group, const int contentTop,
                    const int contentHeight) {
  if (group.itemCount == 0) return;
  const int rowHeight = contentHeight / group.itemCount;
  for (uint8_t index = 0; index < group.itemCount; ++index) {
    renderBarRow(renderer, rect, group.items[index], contentTop + index * rowHeight, rowHeight);
  }
}

void renderStripGroup(GfxRenderer& renderer, const WidgetRectV2& rect, const GroupContent& group, const int contentTop,
                      const int contentHeight) {
  if (group.itemCount == 0) return;
  const int itemWidth = rect.width / group.itemCount;
  for (uint8_t index = 0; index < group.itemCount; ++index) {
    const GroupItem& item = group.items[index];
    char value[MAX_GROUP_VALUE_SIZE + 1];
    char label[MAX_GROUP_LABEL_SIZE + 1];
    char detail[MAX_GROUP_DETAIL_SIZE + 1];
    copyTextBytes(item.valueBytes, item.valueLength, value);
    copyTextBytes(item.labelBytes, item.labelLength, label);
    copyTextBytes(item.detailBytes, item.detailLength, detail);

    const WidgetRectV2 itemRect{rect.x + index * itemWidth, contentTop, itemWidth, contentHeight};
    renderTextBlock(renderer, itemRect, value, label, detail);
  }
}

int drawGroupHeading(GfxRenderer& renderer, const WidgetRectV2& rect, const char* heading) {
  if (heading[0] == '\0') return rect.y + GROUP_PADDING;
  const int headingAscender = renderer.getFontAscenderSize(HEADING_FONT_ID);
  const std::string bounded =
      renderer.truncatedText(HEADING_FONT_ID, heading, rect.width - 2 * GROUP_PADDING, EpdFontFamily::BOLD);
  renderer.drawText(HEADING_FONT_ID, rect.x + GROUP_PADDING, rect.y + GROUP_PADDING, bounded.c_str(), true,
                    EpdFontFamily::BOLD);
  return rect.y + GROUP_PADDING + headingAscender + GROUP_STACK_GAP;
}

void renderGroupWidget(GfxRenderer& renderer, const WidgetRectV2& rect, const GroupContent& group) {
  char heading[MAX_GROUP_HEADING_SIZE + 1];
  copyTextBytes(group.headingBytes, group.headingLength, heading);

  const int contentTop = drawGroupHeading(renderer, rect, heading);
  const int contentBottom = rect.y + rect.height - GROUP_PADDING;
  const int contentHeight = std::max(1, contentBottom - contentTop);

  switch (group.shape) {
    case GROUP_SHAPE_ARC:
      renderArcGroup(renderer, rect, group, contentTop, contentHeight);
      break;
    case GROUP_SHAPE_BAR:
      renderBarGroup(renderer, rect, group, contentTop, contentHeight);
      break;
    case GROUP_SHAPE_STRIP:
      renderStripGroup(renderer, rect, group, contentTop, contentHeight);
      break;
    default:
      // Unreachable: decoding bounds shape to MAX_GROUP_SHAPE.
      break;
  }
}

}  // namespace

void renderWidgetGridV2(GfxRenderer& renderer, const WidgetGridPackageV2& package) {
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

  std::array<WidgetRectV2, MAX_WIDGETS> rects{};
  computeGridLayoutV2(package, canvasWidth, canvasHeight, rects, originX, originY);

  for (uint8_t index = 0; index < package.widgetCount; ++index) {
    const WidgetV2& widget = package.widgets[index];
    if (widget.type != WidgetType::Group) continue;
    if (const GroupContent* content = groupContentFor(package, widget); content != nullptr) {
      renderGroupWidget(renderer, rects[index], *content);
    }
  }
}

}  // namespace v2
}  // namespace dashboard

#endif
