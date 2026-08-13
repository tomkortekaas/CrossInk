#include "BleHandoffReaderProbe.h"

#ifdef CROSSINK_BLE_HANDOFF_READER

#include <GfxRenderer.h>
#include <HalDisplay.h>
#include <Logging.h>

#include "BleHandoffNvs.h"
#include "fontIds.h"

namespace BleHandoffReaderProbe {
namespace {
static dashboard::PersistedPackage persisted;
static char title[dashboard::MAX_TITLE_SIZE + 1];
static char timeLine[dashboard::MAX_TIME_LINE_SIZE + 1];
static char footer[dashboard::MAX_FOOTER_SIZE + 1];

void copyText(char* output, size_t capacity, const dashboard::TextField& field) {
  const size_t length = std::min<size_t>(field.length, capacity - 1);
  std::copy_n(field.bytes.begin(), length, output);
  output[length] = '\0';
}
}  // namespace

void logPersistedPayload() {
  const auto status = dashboard::readLastKnownGood(persisted);
  LOG_INF("BLEPAY", "dashboard status=%u package=%u length=%u", static_cast<unsigned>(status),
          status == dashboard::PersistStatus::Ok ? persisted.package.packageId : 0, persisted.length);
}

bool renderAgendaCard(GfxRenderer& renderer) {
  if (dashboard::readLastKnownGood(persisted) != dashboard::PersistStatus::Ok) return false;
  copyText(title, sizeof(title), persisted.package.title);
  copyText(timeLine, sizeof(timeLine), persisted.package.timeLine);
  copyText(footer, sizeof(footer), persisted.package.footer);
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

}  // namespace BleHandoffReaderProbe
#endif
