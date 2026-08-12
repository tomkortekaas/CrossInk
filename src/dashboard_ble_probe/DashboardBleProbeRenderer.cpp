#include "DashboardBleProbeRenderer.h"

#include <cstdio>

#include <GfxRenderer.h>

#include "activities/RenderLock.h"
#include "fontIds.h"

namespace probe {

DashboardBleProbeRenderer::DashboardBleProbeRenderer(GfxRenderer& renderer) : renderer_(renderer) {}

bool DashboardBleProbeRenderer::renderAccepted(const uint32_t messageId, const uint8_t payloadLength) {
  if (!drawAccepted(messageId, payloadLength)) {
    return false;
  }
  lastMessageId_ = messageId;
  lastPayloadLength_ = payloadLength;
  hasAcceptedFrame_ = true;
  return true;
}

bool DashboardBleProbeRenderer::renderForSleep() {
  return hasAcceptedFrame_ && drawAccepted(lastMessageId_, lastPayloadLength_);
}

void DashboardBleProbeRenderer::showPasskey(const uint32_t passkey) {
  char passkeyLine[16];
  std::snprintf(passkeyLine, sizeof(passkeyLine), "%06lu", static_cast<unsigned long>(passkey));
  RenderLock lock;
  renderer_.setOrientation(GfxRenderer::Portrait);
  renderer_.setRenderMode(GfxRenderer::BW);
  renderer_.clearScreen();
  renderer_.drawText(UI_12_FONT_ID, 32, 96, "Koppelcode", true, EpdFontFamily::BOLD);
  renderer_.drawText(UI_12_FONT_ID, 32, 148, passkeyLine);
  renderer_.displayBuffer(HalDisplay::HALF_REFRESH);
}

bool DashboardBleProbeRenderer::drawAccepted(const uint32_t messageId, const uint8_t payloadLength) {
  char messageLine[32];
  char bytesLine[32];
  std::snprintf(messageLine, sizeof(messageLine), "Message: %lu", static_cast<unsigned long>(messageId));
  std::snprintf(bytesLine, sizeof(bytesLine), "Bytes: %u", static_cast<unsigned>(payloadLength));

  RenderLock lock;
  renderer_.setOrientation(GfxRenderer::Portrait);
  renderer_.setRenderMode(GfxRenderer::BW);
  renderer_.clearScreen();
  renderer_.drawText(UI_12_FONT_ID, 32, 96, "BLE OK", true, EpdFontFamily::BOLD);
  renderer_.drawText(UI_10_FONT_ID, 32, 148, messageLine);
  renderer_.drawText(UI_10_FONT_ID, 32, 184, bytesLine);
  renderer_.displayBuffer(HalDisplay::HALF_REFRESH);
  return true;
}

}  // namespace probe
