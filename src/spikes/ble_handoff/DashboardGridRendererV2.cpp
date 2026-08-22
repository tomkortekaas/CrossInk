#include "DashboardGridRendererV2.h"

#ifdef CROSSINK_BLE_HANDOFF_READER

#include <GfxRenderer.h>
#include <HalClock.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <iterator>
#include <string>

#include "DashboardArc.h"
#include "DashboardDateFields.h"
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
constexpr int TILE_CORNER_RADIUS = 6;

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

// De dashboardklok wordt op dezelfde voorwaarden vertrouwd als de rest van de
// firmware: CrossPointSettings.cpp weigert RTC-datums vóór 2025 omdat de X3-
// hardware van vóór die migratie dateert. Een verzonnen datum op een
// wanddisplay is erger dan een zichtbaar lege.
constexpr uint16_t MIN_TRUSTED_YEAR = 2025;

// Layoutdrempels op de binnenmaat van de tegel, niet op columnSpan/rowSpan, zodat
// een ongebruikelijke span ergens redelijks landt. Overgenomen uit template 3;
// in het v2-raster (cel 42x64) vallen ze ruim buiten de maten die het raster
// werkelijk produceert, dus een tegel flipt nooit over één pixel.
constexpr int DATE_WIDE_THRESHOLD = 200;
constexpr int DATE_TALL_THRESHOLD = 170;

// Eigen ladder voor de meterwaarden, losgekoppeld van sizeRung. Dat veld is
// twee bits en indexeert een tabel van vier; deze ladder heeft er meer en wordt
// met een pas-zoekende keuze gebruikt, zoals fittingFontId van template 3.
constexpr int GAUGE_FONT_LADDER[] = {LEXENDDECA_14_FONT_ID, LEXENDDECA_18_BOLD_DASH_FONT_ID,
                                     LEXENDDECA_22_BOLD_DASH_FONT_ID, LEXENDDECA_28_BOLD_DASH_FONT_ID,
                                     LEXENDDECA_34_BOLD_DASH_FONT_ID};

// sizeRung 0 betekent "automatisch": de volledige ladder blijft beschikbaar.
// 1/2/3 begrenst tot ladderindex 1/2/3 (18/22/28 px), nooit erboven. Een rung
// is dus een maximum, geen geforceerde maat: fittingGaugeFontId blijft naar
// beneden zoeken als de gekozen maat niet past.
int maxGaugeLadderIndex(const uint8_t sizeRung) {
  return sizeRung == 0 ? static_cast<int>(std::size(GAUGE_FONT_LADDER)) - 1
                       : std::min<int>(sizeRung, static_cast<int>(std::size(GAUGE_FONT_LADDER)) - 1);
}

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

// De grootste maat uit de ladder waarvan `text` binnen `maxWidth` EN `maxHeight`
// past. Valt terug op de kleinste maat, waarna de caller
// (drawTextCenteredInRect) zo nodig trunkeert - dezelfde fallback als template 3.
//
// De hoogte moet mee: op alleen breedte kiezen betekent dat een strip van 64 px
// hoog met een item van 252 px breed rustig een maat van 34 px pakt, waarna het
// label eronder van het paneel valt. Dat is precies wat er de eerste keer op
// hardware gebeurde.
int fittingGaugeFontId(const GfxRenderer& renderer, const char* text, const int maxWidth, const int maxHeight,
                       const int maxLadderIndex) {
  for (int index = maxLadderIndex; index >= 0; --index) {
    const int fontId = GAUGE_FONT_LADDER[index];
    if (renderer.getTextWidth(fontId, text, EpdFontFamily::BOLD) <= maxWidth &&
        renderer.getFontAscenderSize(fontId) <= maxHeight) {
      return fontId;
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
                     const char* detail, const int maxLadderIndex) {
  const bool hasValue = value[0] != '\0';
  const bool hasLabel = label[0] != '\0';
  const bool hasDetail = detail[0] != '\0';

  // Label en detail staan op vaste maten, dus hun hoogte is vooraf bekend. Wat
  // daarna overblijft is het hoogtebudget voor de waarde - anders kiest die een
  // maat die de rest van het blok van de tegel duwt.
  const int labelAscender = hasLabel ? renderer.getFontAscenderSize(LABEL_FONT_ID) : 0;
  const int detailAscender = hasDetail ? renderer.getFontAscenderSize(DETAIL_FONT_ID) : 0;
  const int reservedBelow = (hasLabel ? labelAscender + GROUP_STACK_GAP : 0) +
                            (hasDetail ? detailAscender + GROUP_STACK_GAP : 0);
  const int valueHeightBudget = std::max(1, rect.height - reservedBelow - 2 * GROUP_TEXT_PADDING);

  const int valueFontId = hasValue ? fittingGaugeFontId(renderer, value, rect.width - 2 * GROUP_TEXT_PADDING,
                                                        valueHeightBudget, maxLadderIndex)
                                   : 0;
  const int valueAscender = hasValue ? renderer.getFontAscenderSize(valueFontId) : 0;

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

struct TodaysDate {
  bool valid = false;
  uint16_t year = 0;
  uint8_t month = 0;
  uint8_t day = 0;
};

TodaysDate readTodaysDate() {
  TodaysDate today{};
  uint16_t year = 0;
  uint8_t month = 0;
  uint8_t day = 0;
  uint8_t hour = 0;
  uint8_t minute = 0;
  if (!halClock.getDateTime(year, month, day, hour, minute)) return today;
  if (year < MIN_TRUSTED_YEAR || !isValidDate(year, month, day)) return today;
  today.valid = true;
  today.year = year;
  today.month = month;
  today.day = day;
  return today;
}

// Een weekdag die past, met terugval op de tweeletterafkorting in plaats van op
// een truncatie als "donderda". Callers moeten de stijl meegeven waarin de tekst
// straks echt getekend wordt: BOLD is breder dan REGULAR, dus in de verkeerde
// stijl meten laat de volle naam door en trunkeert hem daarna alsnog.
const char* fittingWeekday(const GfxRenderer& renderer, const Weekday weekday, const int fontId, const int maxWidth,
                           const EpdFontFamily::Style style) {
  const char* full = weekdayName(weekday);
  if (renderer.getTextWidth(fontId, full, style) <= maxWidth) return full;
  return weekdayAbbreviation(weekday);
}

// De boog schaalt mee met zijn item: buitendiameter ~70% van de kleinste
// binnenmaat, dikte ~straal/3,5. Een vaste maat maakt een clusteritem van 126 px
// te krap en een stappen-tegel van 168 px te leeg.
void renderArcGroup(GfxRenderer& renderer, const WidgetRectV2& rect, const GroupContent& group, const int contentTop,
                    const int contentHeight, const int maxLadderIndex) {
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
      renderTextBlock(renderer, itemRect, value, label, detail, maxLadderIndex);
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
      // De ruimte binnen de ring is rond, dus het hoogtebudget is hetzelfde als
      // het breedtebudget.
      const int valueFontId = fittingGaugeFontId(renderer, value, valueMaxWidth, valueMaxWidth, maxLadderIndex);
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
                  const int rowHeight, const int maxLadderIndex) {
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
  // De waarderegel deelt de rij met de balk en de bijschriften eronder, dus
  // hooguit de helft van de rijhoogte.
  const int valueHeightBudget = std::max(1, rowHeight / 2);
  const int valueFontId = hasValue ? fittingGaugeFontId(renderer, value, valueMaxWidth, valueHeightBudget,
                                                        maxLadderIndex)
                                   : 0;
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
                    const int contentHeight, const int maxLadderIndex) {
  if (group.itemCount == 0) return;
  const int rowHeight = contentHeight / group.itemCount;
  for (uint8_t index = 0; index < group.itemCount; ++index) {
    renderBarRow(renderer, rect, group.items[index], contentTop + index * rowHeight, rowHeight, maxLadderIndex);
  }
}

void renderStripGroup(GfxRenderer& renderer, const WidgetRectV2& rect, const GroupContent& group, const int contentTop,
                      const int contentHeight, const int maxLadderIndex) {
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
    renderTextBlock(renderer, itemRect, value, label, detail, maxLadderIndex);
  }
}

void renderKpiWidgetV2(GfxRenderer& renderer, const WidgetRectV2& rect, const WidgetV2& widget) {
  constexpr int padding = GROUP_PADDING;

  char value[MAX_KPI_VALUE_SIZE + 1];
  char label[MAX_KPI_LABEL_SIZE + 1];
  copyTextBytes(widget.kpi.valueBytes, widget.kpi.valueLength, value);
  copyTextBytes(widget.kpi.labelBytes, widget.kpi.labelLength, label);

  const bool hasValue = value[0] != '\0';
  const bool hasLabel = label[0] != '\0';
  const int maxLadderIndex = maxGaugeLadderIndex(widgetSizeRung(widget.style));

  // Waarde + label als één blok, net als renderTextBlock voor Group-items. Het
  // label heeft een vaste maat, dus die reserveer je eerst; wat overblijft is
  // het hoogtebudget voor de waarde.
  const int labelAscender = hasLabel ? renderer.getFontAscenderSize(LABEL_FONT_ID) : 0;
  const int reservedBelow = hasLabel ? labelAscender + GROUP_STACK_GAP : 0;
  const int valueHeightBudget = std::max(1, rect.height - reservedBelow - 2 * padding);
  const int valueFontId =
      hasValue ? fittingGaugeFontId(renderer, value, rect.width - 2 * padding, valueHeightBudget, maxLadderIndex) : 0;
  const int valueAscender = hasValue ? renderer.getFontAscenderSize(valueFontId) : 0;

  int blockHeight = 0;
  if (hasValue) blockHeight += valueAscender;
  if (hasValue && hasLabel) blockHeight += GROUP_STACK_GAP;
  if (hasLabel) blockHeight += labelAscender;

  int y = std::max(padding, (rect.height - blockHeight) / 2);
  if (hasValue) {
    drawTextCenteredInRect(renderer, valueFontId, rect, value, EpdFontFamily::BOLD, y, padding, true);
    y += valueAscender;
    if (hasLabel) y += GROUP_STACK_GAP;
  }
  if (hasLabel) {
    drawTextCenteredInRect(renderer, LABEL_FONT_ID, rect, label, EpdFontFamily::REGULAR, y, padding, true);
  }
}

// Een tegel vastgepind op één datumveld: de waarde zo groot als past, met daar
// alleen een klein label boven als het getal alleen een raadsel zou zijn ("33").
void renderDateFieldWidgetV2(GfxRenderer& renderer, const WidgetRectV2& rect, const WidgetV2& widget,
                             const TodaysDate& today) {
  constexpr int padding = GROUP_PADDING;
  const int innerWidth = std::max(1, rect.width - 2 * padding);
  const int maxLadderIndex = maxGaugeLadderIndex(widgetSizeRung(widget.style));

  char value[24] = {};
  const char* label = "";
  switch (widget.dateField) {
    case DateField::Day:
      std::snprintf(value, sizeof(value), "%u", static_cast<unsigned>(today.day));
      break;
    case DateField::Weekday:
      // Gemeten in de kleinste maat en in BOLD, de stijl waarin deze tegel
      // tekent: de volle naam maakt plaats voor de afkorting alleen als hij op
      // geen enkele maat past, en fittingGaugeFontId laat daarna groeien wat won.
      std::snprintf(value, sizeof(value), "%s",
                    fittingWeekday(renderer, weekdayFromDate(today.year, today.month, today.day), DETAIL_FONT_ID,
                                   innerWidth, EpdFontFamily::BOLD));
      break;
    case DateField::Month:
      std::snprintf(value, sizeof(value), "%s", monthName(today.month));
      break;
    case DateField::Year:
      std::snprintf(value, sizeof(value), "%u", static_cast<unsigned>(today.year));
      break;
    case DateField::WeekNumber:
      std::snprintf(value, sizeof(value), "%u",
                    static_cast<unsigned>(isoWeekFromDate(today.year, today.month, today.day).week));
      label = "week";
      break;
    case DateField::Auto:
      // renderDateAutoWidgetV2 tekent deze; de caller routeert hem nooit hierheen.
      return;
  }

  const int labelAscender = renderer.getFontAscenderSize(LABEL_FONT_ID);
  const int labelHeight = label[0] != '\0' ? labelAscender + GROUP_STACK_GAP : 0;
  const int valueHeightBudget = std::max(1, rect.height - labelHeight - 2 * padding);
  const int valueFontId = fittingGaugeFontId(renderer, value, innerWidth, valueHeightBudget, maxLadderIndex);
  const int valueAscender = renderer.getFontAscenderSize(valueFontId);

  int y = std::max(padding, (rect.height - labelHeight - valueAscender) / 2);
  if (labelHeight > 0) {
    drawTextCenteredInRect(renderer, LABEL_FONT_ID, rect, label, EpdFontFamily::REGULAR, y, padding, true);
    y += labelHeight;
  }
  drawTextCenteredInRect(renderer, valueFontId, rect, value, EpdFontFamily::BOLD, y, padding, true);
}

// DateField::Auto: de tegel laat zoveel van de datum zien als zijn vorm toelaat.
void renderDateAutoWidgetV2(GfxRenderer& renderer, const WidgetRectV2& rect, const WidgetV2& widget,
                            const TodaysDate& today) {
  constexpr int padding = GROUP_PADDING;
  const int innerWidth = std::max(1, rect.width - 2 * padding);
  const int maxLadderIndex = maxGaugeLadderIndex(widgetSizeRung(widget.style));
  const int innerHeight = std::max(1, rect.height - 2 * padding);
  const Weekday weekday = weekdayFromDate(today.year, today.month, today.day);
  const IsoWeek isoWeek = isoWeekFromDate(today.year, today.month, today.day);
  const int labelAscender = renderer.getFontAscenderSize(LABEL_FONT_ID);

  char day[4] = {};
  std::snprintf(day, sizeof(day), "%u", static_cast<unsigned>(today.day));
  char week[12] = {};
  std::snprintf(week, sizeof(week), "week %u", static_cast<unsigned>(isoWeek.week));

  const bool wide = innerWidth >= DATE_WIDE_THRESHOLD;
  const bool tall = innerHeight >= DATE_TALL_THRESHOLD;

  if (wide && !tall) {
    // Eén regel: "za 15 aug", met het weeknummer eronder als er ruimte is. v2
    // kent geen iconen, dus de volle binnenbreedte is voor de tekst.
    char line[32] = {};
    std::snprintf(line, sizeof(line), "%s %u %s", weekdayAbbreviation(weekday),
                  static_cast<unsigned>(today.day), monthAbbreviation(today.month));
    const int lineFontId = fittingGaugeFontId(renderer, line, innerWidth, innerHeight, maxLadderIndex);
    const int lineAscender = renderer.getFontAscenderSize(lineFontId);
    const bool showWeek = lineAscender + GROUP_STACK_GAP + labelAscender <= innerHeight;
    const int blockHeight = lineAscender + (showWeek ? GROUP_STACK_GAP + labelAscender : 0);
    int y = std::max(padding, (rect.height - blockHeight) / 2);
    drawTextCenteredInRect(renderer, lineFontId, rect, line, EpdFontFamily::BOLD, y, padding, true);
    if (showWeek) {
      y += lineAscender + GROUP_STACK_GAP;
      drawTextCenteredInRect(renderer, LABEL_FONT_ID, rect, week, EpdFontFamily::REGULAR, y, padding, true);
    }
    return;
  }

  if (wide && tall) {
    // Het volle blad: een zwarte kopbalk met maand en jaar, het dagnummer groot,
    // de weekdag voluit en het weeknummer als voetnoot.
    char header[24] = {};
    std::snprintf(header, sizeof(header), "%s %u", monthName(today.month), static_cast<unsigned>(today.year));
    const int headerHeight = labelAscender + 2 * GROUP_STACK_GAP;
    renderer.fillRect(rect.x, rect.y, rect.width, headerHeight, true);
    drawTextCenteredInRect(renderer, LABEL_FONT_ID, rect, header, EpdFontFamily::BOLD, GROUP_STACK_GAP, padding, false);

    const int dayHeightBudget =
        std::max(1, innerHeight - labelAscender - GROUP_STACK_GAP - labelAscender - GROUP_STACK_GAP);
    const int dayFontId = fittingGaugeFontId(renderer, day, innerWidth, dayHeightBudget, maxLadderIndex);
    const int dayAscender = renderer.getFontAscenderSize(dayFontId);
    const char* weekdayText = fittingWeekday(renderer, weekday, LABEL_FONT_ID, innerWidth, EpdFontFamily::REGULAR);
    const int blockHeight = dayAscender + GROUP_STACK_GAP + labelAscender + GROUP_STACK_GAP + labelAscender;
    int y = headerHeight + std::max(padding, (rect.height - headerHeight - blockHeight) / 2);
    drawTextCenteredInRect(renderer, dayFontId, rect, day, EpdFontFamily::BOLD, y, padding, true);
    y += dayAscender + GROUP_STACK_GAP;
    drawTextCenteredInRect(renderer, LABEL_FONT_ID, rect, weekdayText, EpdFontFamily::REGULAR, y, padding, true);
    y += labelAscender + GROUP_STACK_GAP;
    drawTextCenteredInRect(renderer, LABEL_FONT_ID, rect, week, EpdFontFamily::REGULAR, y, padding, true);
    return;
  }

  // Smal: weekdag boven het dagnummer, met de afgekorte maand eronder alleen
  // als de tegel hoog genoeg is voor een derde regel.
  const char* weekdayText = fittingWeekday(renderer, weekday, LABEL_FONT_ID, innerWidth, EpdFontFamily::REGULAR);
  const int dayHeightBudget =
      std::max(1, innerHeight - labelAscender - GROUP_STACK_GAP - (tall ? labelAscender + GROUP_STACK_GAP : 0));
  const int dayFontId = fittingGaugeFontId(renderer, day, innerWidth, dayHeightBudget, maxLadderIndex);
  const int dayAscender = renderer.getFontAscenderSize(dayFontId);
  const bool showMonth =
      tall && labelAscender + GROUP_STACK_GAP + dayAscender + GROUP_STACK_GAP + labelAscender <= innerHeight;
  const int blockHeight =
      labelAscender + GROUP_STACK_GAP + dayAscender + (showMonth ? GROUP_STACK_GAP + labelAscender : 0);
  int y = std::max(padding, (rect.height - blockHeight) / 2);
  drawTextCenteredInRect(renderer, LABEL_FONT_ID, rect, weekdayText, EpdFontFamily::REGULAR, y, padding, true);
  y += labelAscender + GROUP_STACK_GAP;
  drawTextCenteredInRect(renderer, dayFontId, rect, day, EpdFontFamily::BOLD, y, padding, true);
  if (showMonth) {
    y += dayAscender + GROUP_STACK_GAP;
    drawTextCenteredInRect(renderer, LABEL_FONT_ID, rect, monthAbbreviation(today.month), EpdFontFamily::REGULAR, y,
                           padding, true);
  }
}

// Wat een datumtegel toont als de RTC niet te vertrouwen is. Een streepje leest
// op armlengte als "geen data"; een verkeerde datum doet dat niet.
void renderDatePlaceholderV2(GfxRenderer& renderer, const WidgetRectV2& rect) {
  constexpr int padding = GROUP_PADDING;
  const int fontId = LABEL_FONT_ID;
  const int ascender = renderer.getFontAscenderSize(fontId);
  const int y = std::max(padding, (rect.height - ascender) / 2);
  drawTextCenteredInRect(renderer, fontId, rect, "—", EpdFontFamily::REGULAR, y, padding, true);
}

void renderDateWidgetV2(GfxRenderer& renderer, const WidgetRectV2& rect, const WidgetV2& widget,
                        const TodaysDate& today) {
  if (!today.valid) {
    renderDatePlaceholderV2(renderer, rect);
  } else if (widget.dateField == DateField::Auto) {
    renderDateAutoWidgetV2(renderer, rect, widget, today);
  } else {
    renderDateFieldWidgetV2(renderer, rect, widget, today);
  }
}

void renderListWidgetV2(GfxRenderer& renderer, const WidgetRectV2& rect, const WidgetV2& /*widget*/,
                        const ListContentV2& list) {
  constexpr int padding = GROUP_PADDING;
  const int rowFontId = LEXENDDECA_8_FONT_ID;

  int y = padding;
  if (list.headingLength > 0) {
    char heading[MAX_LIST_HEADING_SIZE + 1];
    copyTextBytes(list.headingBytes, list.headingLength, heading);
    renderer.drawText(LABEL_FONT_ID, rect.x + padding, rect.y + y, heading, true, EpdFontFamily::BOLD);
    y += renderer.getLineHeight(LABEL_FONT_ID);
  }

  const int lineHeight = renderer.getLineHeight(rowFontId);
  // time + twee spaties + label, ruim onder de stackgrens (CLAUDE.md #1), ook
  // op de maximale veldlengte.
  char line[MAX_LIST_ROW_TIME_SIZE + 2 + MAX_LIST_ROW_LABEL_SIZE + 1];
  for (uint8_t index = 0; index < list.rowCount && y + lineHeight <= rect.height - padding; ++index) {
    const ListRowV2& row = list.rows[index];
    size_t offset = 0;
    std::copy_n(row.timeBytes.begin(), row.timeLength, line + offset);
    offset += row.timeLength;
    line[offset++] = ' ';
    line[offset++] = ' ';
    std::copy_n(row.labelBytes.begin(), row.labelLength, line + offset);
    offset += row.labelLength;
    line[offset] = '\0';

    // De eerste regel is vet als ankerpunt voor het oog; de rest regulier.
    const EpdFontFamily::Style rowStyle = index == 0 ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR;
    const std::string bounded = renderer.truncatedText(rowFontId, line, rect.width - 2 * padding, rowStyle);
    renderer.drawText(rowFontId, rect.x + padding, rect.y + y, bounded.c_str(), true, rowStyle);
    y += lineHeight;
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

void renderGroupWidget(GfxRenderer& renderer, const WidgetRectV2& rect, const GroupContent& group,
                       const int maxLadderIndex) {
  char heading[MAX_GROUP_HEADING_SIZE + 1];
  copyTextBytes(group.headingBytes, group.headingLength, heading);

  const int contentTop = drawGroupHeading(renderer, rect, heading);
  const int contentBottom = rect.y + rect.height - GROUP_PADDING;
  const int contentHeight = std::max(1, contentBottom - contentTop);

  switch (group.shape) {
    case GROUP_SHAPE_ARC:
      renderArcGroup(renderer, rect, group, contentTop, contentHeight, maxLadderIndex);
      break;
    case GROUP_SHAPE_BAR:
      renderBarGroup(renderer, rect, group, contentTop, contentHeight, maxLadderIndex);
      break;
    case GROUP_SHAPE_STRIP:
      renderStripGroup(renderer, rect, group, contentTop, contentHeight, maxLadderIndex);
      break;
    default:
      // Unreachable: decoding bounds shape to MAX_GROUP_SHAPE.
      break;
  }
}

// Eén pas over elke tegel nadat de inhoud getekend is, gespiegeld naar
// drawTileBorders van template 3. Omlijning is de enige globale stijl die
// template 4 kent; density en list-dividers bestaan hier niet.
void drawTileBordersV2(GfxRenderer& renderer, const WidgetGridPackageV2& package,
                       const std::array<WidgetRectV2, MAX_WIDGETS>& rects, const int gridRight,
                       const int gridBottom) {
  const uint8_t level = globalBorderLevel(package.style);
  if (level == dashboard::BORDER_NONE) return;

  for (uint8_t index = 0; index < package.widgetCount; ++index) {
    const WidgetRectV2& rect = rects[index];
    switch (level) {
      case dashboard::BORDER_HAIRLINE:
        // Alleen de gedeelde randen, geen doos: een lijn langs rechts en een
        // langs onder. Tegels op de buitenrand krijgen niets, zodat het
        // dashboard geen buitenkader heeft.
        if (rect.x + rect.width < gridRight) {
          renderer.fillRect(rect.x + rect.width - 1, rect.y, 1, rect.height, true);
        }
        if (rect.y + rect.height < gridBottom) {
          renderer.fillRect(rect.x, rect.y + rect.height - 1, rect.width, 1, true);
        }
        break;
      case dashboard::BORDER_LIGHT: {
        // Geen dithered-omtrekprimitief, dus het kader is vier stroken van één
        // pixel met het 25%-patroon.
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
      case dashboard::BORDER_SOLID:
        renderer.drawRoundedRect(rect.x + 2, rect.y + 2, rect.width - 4, rect.height - 4, 1, TILE_CORNER_RADIUS,
                                 true);
        break;
      default:
        // Unreachable: decode begrenst borderLevel tot MAX_BORDER_LEVEL.
        break;
    }
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

  // Read once for the whole grid, not once per tile: two date tiles drawn either
  // side of midnight would otherwise disagree about what day it is.
  const TodaysDate today = readTodaysDate();
  for (uint8_t index = 0; index < package.widgetCount; ++index) {
    const WidgetV2& widget = package.widgets[index];
    switch (widget.type) {
      case WidgetType::Group:
        if (const GroupContent* content = groupContentFor(package, widget); content != nullptr) {
          renderGroupWidget(renderer, rects[index], *content,
                            maxGaugeLadderIndex(widgetSizeRung(widget.style)));
        }
        break;
      case WidgetType::Kpi:
        renderKpiWidgetV2(renderer, rects[index], widget);
        break;
      case WidgetType::List:
        if (const ListContentV2* list = listContentFor(package, widget); list != nullptr) {
          renderListWidgetV2(renderer, rects[index], widget, *list);
        }
        break;
      case WidgetType::Date:
        renderDateWidgetV2(renderer, rects[index], widget, today);
        break;
    }
  }
  drawTileBordersV2(renderer, package, rects, originX + canvasWidth, originY + canvasHeight);
}

}  // namespace v2
}  // namespace dashboard

#endif
