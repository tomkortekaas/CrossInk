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

// De ladder die de agenda/berichten werkelijk gebruikt: Lexend (regulier en vet)
// en Bitter voor leestekst op 10-16, plus de vette metermaattrap voor waarden.
// Volgorde is oplopend in maat zodat Tom de kleinste het eerst ziet en kan
// aflezen waar hij stopt met kunnen lezen.
constexpr FontTestRow FONT_TEST_ROWS[] = {
    {LEXENDDECA_10_FONT_ID, EpdFontFamily::REGULAR, "Lx R 10", "09:30 Teamoverleg"},
    {LEXENDDECA_10_FONT_ID, EpdFontFamily::BOLD, "Lx B 10", "09:30 Teamoverleg"},
    {LEXENDDECA_12_FONT_ID, EpdFontFamily::REGULAR, "Lx R 12", "09:30 Teamoverleg"},
    {LEXENDDECA_12_FONT_ID, EpdFontFamily::BOLD, "Lx B 12", "09:30 Teamoverleg"},
    {LEXENDDECA_14_FONT_ID, EpdFontFamily::REGULAR, "Lx R 14", "09:30 Teamoverleg"},
    {LEXENDDECA_14_FONT_ID, EpdFontFamily::BOLD, "Lx B 14", "09:30 Teamoverleg"},
    {LEXENDDECA_16_FONT_ID, EpdFontFamily::REGULAR, "Lx R 16", "09:30 Teamoverleg"},
    {LEXENDDECA_16_FONT_ID, EpdFontFamily::BOLD, "Lx B 16", "09:30 Teamoverleg"},

    {BITTER_10_FONT_ID, EpdFontFamily::REGULAR, "Bt R 10", "09:30 Teamoverleg"},
    {BITTER_10_FONT_ID, EpdFontFamily::BOLD, "Bt B 10", "09:30 Teamoverleg"},
    {BITTER_12_FONT_ID, EpdFontFamily::REGULAR, "Bt R 12", "09:30 Teamoverleg"},
    {BITTER_12_FONT_ID, EpdFontFamily::BOLD, "Bt B 12", "09:30 Teamoverleg"},
    {BITTER_14_FONT_ID, EpdFontFamily::REGULAR, "Bt R 14", "09:30 Teamoverleg"},
    {BITTER_14_FONT_ID, EpdFontFamily::BOLD, "Bt B 14", "09:30 Teamoverleg"},
    {BITTER_16_FONT_ID, EpdFontFamily::REGULAR, "Bt R 16", "09:30 Teamoverleg"},
    {BITTER_16_FONT_ID, EpdFontFamily::BOLD, "Bt B 16", "09:30 Teamoverleg"},

    {LEXENDDECA_22_BOLD_DASH_FONT_ID, EpdFontFamily::BOLD, "Lx 22", "09:30"},
    {LEXENDDECA_28_BOLD_DASH_FONT_ID, EpdFontFamily::BOLD, "Lx 28", "09:30"},
};

// Een kleine markering in de marge als visuele anker per kolom, zodat op de foto
// duidelijk is dat de tekstgrootte (niet de annotatie) de test is.
constexpr int LABEL_COLUMN_WIDTH = 78;
constexpr int FONT_TEST_LETTER_SPACING = 8;  // horizontale marge rond het fragment

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

// Tekent de lettertypes-testkaart: elke rij toont een realistisch agenda-fragment
// op één font/maat/gewicht, met een kleine annotatie links. Zo kan Tom het paneel
// fotograferen en per rij aflezen waar de leesbaarheid stopt.
void renderFontTestCardInternal(GfxRenderer& renderer) {
  const int width = renderer.getScreenWidth();

  constexpr int topMargin = 8;
  constexpr int rowGap = 4;
  constexpr int titleGap = 6;
  constexpr int annotationFontId = SMALL_FONT_ID;
  constexpr int titleFontId = UI_12_FONT_ID;

  const int titleAscender = renderer.getFontAscenderSize(titleFontId);

  // `y` is de bovenkant van een tekstblok (de renderer telt de ascender er zelf
  // bij op).
  int y = topMargin;

  renderer.drawText(titleFontId, 0, y, tr(STR_FONT_TEST), true, EpdFontFamily::BOLD);
  y += titleAscender + titleGap;

  const int labelX = 0;
  const int sampleX = LABEL_COLUMN_WIDTH;

  // Horizontale scheidingslijn onder de tittel, links-vrij zodat annotaties de
  // linkerrand raken en het fragment duidelijk rechts begint.
  renderer.drawLine(LABEL_COLUMN_WIDTH, y - titleGap / 2, width - FONT_TEST_LETTER_SPACING, y - titleGap / 2, true);

  // Plak de fragmenten aan de linkerkant van hun regel (niet gecentreerd), zodat
  // kolomvreemde witruimte de vergelijking niet verstoort en een lang fragment
  // dezelfde startkant houdt.
  for (const FontTestRow& row : FONT_TEST_ROWS) {
    renderer.drawText(annotationFontId, labelX, y, row.label, true, EpdFontFamily::REGULAR);
    // Alle fragmenten zijn vaste korte reeksen die ruim binnen `sampleWidth`
    // passen, dus er is geen ellipsis/allocatie nodig; `drawText` zelf alloceert
    // niets, wat dit op de render-taak heap-veilig houdt.
    renderer.drawText(row.fontId, labelX + sampleX, y, row.sample, true, row.style);
    y += renderer.getLineHeight(row.fontId) + rowGap;
  }
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
