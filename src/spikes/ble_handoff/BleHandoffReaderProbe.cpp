#include "BleHandoffReaderProbe.h"

#ifdef CROSSINK_BLE_HANDOFF_READER

#include <GfxRenderer.h>
#include <HalDisplay.h>
#include <I18n.h>
#include <Logging.h>

#include <algorithm>

#include "BleHandoffNvs.h"
#include "DashboardGridRenderer.h"
#include "DashboardGridRendererV2.h"
#include "DashboardWidgetGrid.h"
#include "DashboardWidgetGridV2.h"
#include "fontIds.h"

namespace BleHandoffReaderProbe {
namespace {

// Eén rij van de lettertypes-testkaart: een vaste annotatie links en een
// realistisch agenda/bericht-fragment rechts, getekend op `fontId`/`style`.
// `label` is bewust klein (annotatie), het fragment is de eigenlijke test voor
// leesbaarheid op de desbetreffende maat.
struct FontTestRow {
  int fontId;
  EpdFontFamily::Style style;
  const char* label;
  const char* sample;
};

// Twee kolommen: links Lexend, rechts Bitter. Binnen elke kolom loopt de maat
// oplopend 8 t/m 16 (regulier op de bovenste, vet net daaronder), zodat je de
// minimaal leesbare maat in één oogopslag naast het standaardminimum ziet.
// Volgorde is oplopend in maat zodat Tom de kleinste het eerst ziet.
constexpr FontTestRow FONT_TEST_ROWS[] = {
    {LEXENDDECA_8_FONT_ID, EpdFontFamily::REGULAR, "Lx 8R", "09:30"},
    {LEXENDDECA_8_FONT_ID, EpdFontFamily::BOLD, "Lx 8B", "09:30"},
    {LEXENDDECA_9_FONT_ID, EpdFontFamily::REGULAR, "Lx 9R", "09:30"},
    {LEXENDDECA_9_FONT_ID, EpdFontFamily::BOLD, "Lx 9B", "09:30"},
    {LEXENDDECA_10_FONT_ID, EpdFontFamily::REGULAR, "Lx 10R", "09:30 Appt"},
    {LEXENDDECA_10_FONT_ID, EpdFontFamily::BOLD, "Lx 10B", "09:30 Appt"},
    {LEXENDDECA_12_FONT_ID, EpdFontFamily::REGULAR, "Lx 12R", "09:30 Appt"},
    {LEXENDDECA_12_FONT_ID, EpdFontFamily::BOLD, "Lx 12B", "09:30 Appt"},
    {LEXENDDECA_14_FONT_ID, EpdFontFamily::REGULAR, "Lx 14R", "09:30 Appt"},
    {LEXENDDECA_14_FONT_ID, EpdFontFamily::BOLD, "Lx 14B", "09:30 Appt"},
    {LEXENDDECA_16_FONT_ID, EpdFontFamily::REGULAR, "Lx 16R", "09:30 Appt"},
    {LEXENDDECA_16_FONT_ID, EpdFontFamily::BOLD, "Lx 16B", "09:30 Appt"},

    {BITTER_8_FONT_ID, EpdFontFamily::REGULAR, "Bt 8R", "09:30"},
    {BITTER_8_FONT_ID, EpdFontFamily::BOLD, "Bt 8B", "09:30"},
    {BITTER_9_FONT_ID, EpdFontFamily::REGULAR, "Bt 9R", "09:30"},
    {BITTER_9_FONT_ID, EpdFontFamily::BOLD, "Bt 9B", "09:30"},
    {BITTER_10_FONT_ID, EpdFontFamily::REGULAR, "Bt 10R", "09:30 Appt"},
    {BITTER_10_FONT_ID, EpdFontFamily::BOLD, "Bt 10B", "09:30 Appt"},
    {BITTER_12_FONT_ID, EpdFontFamily::REGULAR, "Bt 12R", "09:30 Appt"},
    {BITTER_12_FONT_ID, EpdFontFamily::BOLD, "Bt 12B", "09:30 Appt"},
    {BITTER_14_FONT_ID, EpdFontFamily::REGULAR, "Bt 14R", "09:30 Appt"},
    {BITTER_14_FONT_ID, EpdFontFamily::BOLD, "Bt 14B", "09:30 Appt"},
    {BITTER_16_FONT_ID, EpdFontFamily::REGULAR, "Bt 16R", "09:30 Appt"},
    {BITTER_16_FONT_ID, EpdFontFamily::BOLD, "Bt 16B", "09:30 Appt"},

    {LEXENDDECA_22_BOLD_DASH_FONT_ID, EpdFontFamily::BOLD, "Lx 22", "42 800"},
    {LEXENDDECA_28_BOLD_DASH_FONT_ID, EpdFontFamily::BOLD, "Lx 28", "42 800"},
};
// De kaart splitst de rij-ladder hard in blokken (Lexend|Bitter|waarden); pas de
// constanten in de renderer aan als je deze volgorde/het aantal verandert.
static_assert(sizeof(FONT_TEST_ROWS) / sizeof(FONT_TEST_ROWS[0]) == 26, "font test ladder changed shape");

// Vaste siermaten voor de kaart zelf (annotaties kop/voet).
constexpr int FONT_TEST_LETTER_SPACING = 8;   // horizontale marge rond het fragment

static dashboard::PersistedPackage persisted;
static char title[dashboard::MAX_TITLE_SIZE + 1];
static char timeLine[dashboard::MAX_TIME_LINE_SIZE + 1];
static char footer[dashboard::MAX_FOOTER_SIZE + 1];

void copyText(char* output, size_t capacity, const dashboard::TextField& field) {
  const size_t length = std::min<size_t>(field.length, capacity - 1);
  std::copy_n(field.bytes.begin(), length, output);
  output[length] = '\0';
}

bool renderAgendaTemplate(GfxRenderer& renderer) {
  dashboard::Package package{};
  if (dashboard::decodePackage(persisted.bytes.data(), persisted.length, package) != dashboard::Status::Ok) {
    return false;
  }
  copyText(title, sizeof(title), package.title);
  copyText(timeLine, sizeof(timeLine), package.timeLine);
  copyText(footer, sizeof(footer), package.footer);
  renderer.setOrientation(GfxRenderer::Orientation::Portrait);
  renderer.clearScreen();
  const int centerY = renderer.getScreenHeight() / 2;
  renderer.drawCenteredText(UI_10_FONT_ID, centerY - 150, "AGENDA", true, EpdFontFamily::BOLD);
  renderer.drawCenteredText(UI_12_FONT_ID, centerY - 65, title, true, EpdFontFamily::BOLD);
  renderer.drawCenteredText(UI_10_FONT_ID, centerY + 20, timeLine, true, EpdFontFamily::BOLD);
  renderer.drawCenteredText(SMALL_FONT_ID, centerY + 150, footer);
  renderer.displayBuffer(HalDisplay::HALF_REFRESH, true);
  return true;
}

bool renderWidgetGridTemplate(GfxRenderer& renderer) {
  // Static for the same reason as `persisted` above: a decoded package
  // covering the whole grid is too large to place on the render task's stack.
  static dashboard::WidgetGridPackage package;
  package = {};
  if (dashboard::decodeWidgetGridPackage(persisted.bytes.data(), persisted.length, package) !=
      dashboard::Status::Ok) {
    return false;
  }
  renderer.setOrientation(GfxRenderer::Orientation::Portrait);
  renderer.clearScreen();
  dashboard::renderWidgetGrid(renderer, package);
  renderer.displayBuffer(HalDisplay::HALF_REFRESH, true);
  return true;
}

bool renderWidgetGridV2Template(GfxRenderer& renderer) {
  // Static for the same reason as `persisted` above: a decoded template-4
  // package covering the whole grid is too large to place on the render task's
  // stack.
  static dashboard::v2::WidgetGridPackageV2 package;
  package = {};
  if (dashboard::v2::decodeWidgetGridPackageV2(persisted.bytes.data(), persisted.length, package) !=
      dashboard::Status::Ok) {
    return false;
  }
  renderer.setOrientation(GfxRenderer::Orientation::Portrait);
  renderer.clearScreen();
  dashboard::v2::renderWidgetGridV2(renderer, package);
  renderer.displayBuffer(HalDisplay::HALF_REFRESH, true);
  return true;
}

// Tekent een druk-/voordruk-kaart op één scherm, in vier banden:
//   1) LETTERS  — een ladder klein→groot (8..16) in twee kolommen (Lexend/Bitter),
//      inclusief de nieuw toegevoegde maten 8/9;
//   2) WAARDEN  — de vette metermaattrap (22/28) zoals een KPI-cijfer;
//   3) LIJNEN   — horizontale lijnen van 1, 2 en 3 px dik;
//   4) KPI      — een nagebootste metertegel: groot vet cijfer met dun label.
// Zo fotografeert Tom het paneel en leest per rij af wat leesbaar is en wat niet.
void renderFontTestCardInternal(GfxRenderer& renderer) {
  const int width = renderer.getScreenWidth();

  constexpr int topMargin = 8;
  constexpr int rowGap = 4;
  constexpr int bandGap = 10;
  constexpr int sectionFontId = SMALL_FONT_ID;   // bandkoppen
  constexpr int titleFontId = UI_12_FONT_ID;
  constexpr int valueFontId = LEXENDDECA_28_BOLD_DASH_FONT_ID;
  constexpr int labelRowFontId = SMALL_FONT_ID;      // annotaties links van fragment
  constexpr int kpiLabelFontId = UI_10_FONT_ID;      // dun label vóór KPI-cijfer

  constexpr int leftColumnX = 8;
  constexpr int rightColumnX = 248;
  constexpr int rowLabelX = 6;
  constexpr int rowSampleXOffset = 46;

  const int titleAscender = renderer.getFontAscenderSize(titleFontId);
  const int sectionAscender = renderer.getFontAscenderSize(sectionFontId);

  int y = topMargin;

  // Titel
  renderer.drawText(titleFontId, 0, y, tr(STR_FONT_TEST), true, EpdFontFamily::BOLD);
  y += titleAscender + rowGap;

  // ------ Band 1: LETTERS ------
  renderer.drawText(sectionFontId, 2, y, "LETTERS (klein->groot)", true, EpdFontFamily::BOLD);
  y += sectionAscender + rowGap;
  renderer.drawLine(2, y - rowGap, width - FONT_TEST_LETTER_SPACING, y - rowGap, true);

  // De eerste 12 rijen = Lexend (linkerkolom), de volgende 12 = Bitter (rechterkolom).
  constexpr int kRowsPerColumn = 12;
  constexpr int kFirstBitterRow = kRowsPerColumn;
  for (int i = 0; i < kRowsPerColumn; ++i) {
    const FontTestRow& lx = FONT_TEST_ROWS[i];
    const FontTestRow& bt = FONT_TEST_ROWS[kFirstBitterRow + i];

    const int rowHeightLx = renderer.getLineHeight(lx.fontId) + rowGap;
    const int rowHeightBt = renderer.getLineHeight(bt.fontId) + rowGap;

    // Linkerkolom: Lexend
    renderer.drawText(labelRowFontId, rowLabelX, y, lx.label, true, EpdFontFamily::REGULAR);
    renderer.drawText(lx.fontId, rowLabelX + rowSampleXOffset, y, lx.sample, true, lx.style);

    // Rechterkolom: Bitter
    renderer.drawText(labelRowFontId, rightColumnX + rowLabelX, y, bt.label, true, EpdFontFamily::REGULAR);
    renderer.drawText(bt.fontId, rightColumnX + rowLabelX + rowSampleXOffset, y, bt.sample, true, bt.style);

    y += (rowHeightLx > rowHeightBt ? rowHeightLx : rowHeightBt);
  }

  // ------ Band 2: WAARDEN (vette metertrap) ------
  y += bandGap;
  renderer.drawText(sectionFontId, 2, y, "WAARDEN (KPI-cijfer)", true, EpdFontFamily::BOLD);
  y += sectionAscender + rowGap;
  renderer.drawLine(2, y - rowGap, width - FONT_TEST_LETTER_SPACING, y - rowGap, true);
  const FontTestRow& v22 = FONT_TEST_ROWS[24];
  const FontTestRow& v28 = FONT_TEST_ROWS[25];
  const int v22h = renderer.getLineHeight(v22.fontId) + rowGap;
  renderer.drawText(labelRowFontId, rowLabelX, y, v22.label, true, EpdFontFamily::REGULAR);
  renderer.drawText(v22.fontId, rowLabelX + rowSampleXOffset, y, v22.sample, true, v22.style);
  renderer.drawText(labelRowFontId, rightColumnX + rowLabelX, y, v28.label, true, EpdFontFamily::REGULAR);
  renderer.drawText(v28.fontId, rightColumnX + rowLabelX + rowSampleXOffset, y, v28.sample, true, v28.style);
  y += v22h;

  // ------ Band 3: LIJNEN ------
  y += bandGap;
  renderer.drawText(sectionFontId, 2, y, "LIJNEN (dikte in beeldpunten)", true, EpdFontFamily::BOLD);
  y += sectionAscender + rowGap;
  constexpr int lineY = 12;
  constexpr int lineRun = 260;
  const int labelX = rowLabelX;
  const int my = y + lineY;
  renderer.drawText(labelRowFontId, labelX, my - 4, "1px", true, EpdFontFamily::REGULAR);
  renderer.drawLine(labelX + 32, my, labelX + 32 + lineRun, my, true);            // 1 px
  renderer.drawText(labelRowFontId, labelX, my + lineY - 2, "2px", true, EpdFontFamily::REGULAR);
  renderer.drawLine(labelX + 32, my + lineY - 2, labelX + 32 + lineRun, my + lineY - 2, 2, true);  // 2 px
  renderer.drawText(labelRowFontId, labelX, my + 2 * lineY, "3px", true, EpdFontFamily::REGULAR);
  renderer.drawLine(labelX + 32, my + 2 * lineY, labelX + 32 + lineRun, my + 2 * lineY, 3, true); // 3 px
  y += 3 * lineY + rowGap + 8;

  // ------ Band 4: KPI ------
  y += bandGap;
  renderer.drawText(sectionFontId, 2, y, "KPI (groot vet cijfer, dun label)", true, EpdFontFamily::BOLD);
  y += sectionAscender + rowGap;
  renderer.drawLine(2, y - rowGap, width - FONT_TEST_LETTER_SPACING, y - rowGap, true);
  // Dun label erboven / naast het cijfer.
  renderer.drawText(kpiLabelFontId, 2, y, "STAPPEN VANDAAG", true, EpdFontFamily::REGULAR);
  const int valTop = y + renderer.getLineHeight(kpiLabelFontId) + rowGap;
  renderer.drawText(valueFontId, 2, valTop, "42800", true, EpdFontFamily::BOLD);
  // Dunne - vet A/B-vergelijking op één maat, op een eigen rij eronder.
  const int abY = valTop + renderer.getLineHeight(valueFontId) + rowGap;
  renderer.drawText(kpiLabelFontId, 2, abY, "DUN", true, EpdFontFamily::REGULAR);
  renderer.drawText(kpiLabelFontId, 2 + 48, abY, "VET", true, EpdFontFamily::BOLD);
  y = abY;
}

}  // namespace

void logPersistedPayload() {
  const auto status = dashboard::readLastKnownGood(persisted);
  LOG_INF("BLEPAY", "dashboard status=%u package=%u length=%u", static_cast<unsigned>(status),
          status == dashboard::PersistStatus::Ok ? persisted.header.packageId : 0, persisted.length);
}

const char* dashboardSkipReasonText(const DashboardSkipReason reason) {
  switch (reason) {
    case DashboardSkipReason::NoPackage:
      return "Geen dashboard ontvangen";
    case DashboardSkipReason::UnknownTemplate:
      return "Dashboard nieuwer dan deze firmware";
    case DashboardSkipReason::Undecodable:
      return "Dashboard onleesbaar - firmware bijwerken";
    case DashboardSkipReason::None:
      return "";
  }
  return "";
}

bool renderDashboardCard(GfxRenderer& renderer, DashboardSkipReason* const reasonOut) {
  const auto setReason = [reasonOut](const DashboardSkipReason reason) {
    if (reasonOut != nullptr) *reasonOut = reason;
  };
  setReason(DashboardSkipReason::None);

  if (dashboard::readLastKnownGood(persisted) != dashboard::PersistStatus::Ok) {
    setReason(DashboardSkipReason::NoPackage);
    return false;
  }
  switch (persisted.header.templateId) {
    case dashboard::TEMPLATE_AGENDA:
      if (renderAgendaTemplate(renderer)) return true;
      setReason(DashboardSkipReason::Undecodable);
      return false;
    case dashboard::TEMPLATE_WIDGET_GRID:
      if (renderWidgetGridTemplate(renderer)) return true;
      setReason(DashboardSkipReason::Undecodable);
      return false;
    case dashboard::v2::TEMPLATE_WIDGET_GRID_V2:
      if (renderWidgetGridV2Template(renderer)) return true;
      setReason(DashboardSkipReason::Undecodable);
      return false;
    default:
      // Niet corrupt, alleen niet iets dat deze build kan tekenen.
      LOG_ERR("BLEPAY", "unknown templateId=%u", static_cast<unsigned>(persisted.header.templateId));
      setReason(DashboardSkipReason::UnknownTemplate);
      return false;
  }
}

void renderFontTestCard(GfxRenderer& renderer) {
  renderer.setOrientation(GfxRenderer::Orientation::Portrait);
  renderer.clearScreen();
  renderFontTestCardInternal(renderer);
  renderer.displayBuffer(HalDisplay::HALF_REFRESH, true);
}

}  // namespace BleHandoffReaderProbe
#endif
