// Standalone native navigator in app0. Reader/dashboard remains in app1.
#ifdef CROSSINK_NAVIGATOR
#include <Arduino.h>
#include <BoardConfig.h>
#include <EInkDisplay.h>
#include <I18n.h>
#include <InputManager.h>
#include <Logging.h>
#include <XteinkDetect.h>

#include "../ble_handoff/DashboardBootSwitch.h"
#include "NavHandoff.h"
#include "NavScreenRenderer.h"
#include "NavigationRefreshPolicy.h"
#include "NavigatorRouteReceiver.h"
#include "map/GrayMap.h"
#include "map/RouteMapRenderer.h"
#include "map/WalkMapSdSource.h"
#include "route/RouteSdFileSystem.h"

namespace {
freeink::FreeInkDisplay display(8, 10, 21, 4, 5, 6);
InputManager input;
navigator::RouteSdFileSystem routeFs;
navigator::NavigationRouteSession routes(routeFs);  // ~20 KiB, never task-stack local
navigator::WalkMapSdSource mapSource;
navigator::WalkMapSdSource graySource;
navigator::GrayMap grayMap;  // bounded label cache, no tile/frame allocation
navigator::GrayMapLayer grayLayer{&graySource, &grayMap};
navigator::WalkMap backgroundMap;  // <=64 bytes; no retained province index
navigator::WalkMapLayer mapLayer{&mapSource, &backgroundMap};
navigator::NavigationRefreshPolicy refreshPolicy;
bool ready = false;
bool sdReady = false;
bool lastReceiverOpen = false;
bool backRequested = false;
bool cancelMapRead() {
  input.update();
  if (input.wasReleased(InputManager::BTN_BACK)) backRequested = true;
  return backRequested;
}

void message(const char* title, const char* detail) {
  navigator::NavScreenRenderer::drawMessage(display.getFrameBuffer(), display.getDisplayWidth(),
                                            display.getDisplayHeight(), title, detail);
  display.displayBuffer(freeink::FreeInkDisplay::FULL_REFRESH);
}
void showRoute(bool showLoading = true, navigator::NavigationRefresh refresh = navigator::NavigationRefresh::Full) {
  if (!sdReady) {
    message("SD KAART ONTBREEKT", "PLAATS KAART EN HERSTART");
    return;
  }
  if (!routes.hasRoute()) {
    message("GEEN ROUTE", navigator::routeReceiverOpen() ? "STUUR GPX VANAF IPHONE" : "OK - ROUTE ONTVANGEN");
    return;
  }
  if (showLoading) message(tr(STR_NAV_LOADING_MAP), tr(STR_NAV_LOADING_DETAIL));
  const uint32_t started = millis();
  navigator::LivePosition fix{};
  bool hasPosition = navigator::navigationPosition(millis(), fix);
  const bool grayReady = hasPosition && display.supportsStripGrayscale() &&
                         graySource.beginRead(cancelMapRead, "/Navigation/Maps/gray.x3gm") &&
                         grayMap.open(graySource) == navigator::WalkMapStatus::Ok;
  bool detailReady = grayReady;
  if (!grayReady && hasPosition && mapSource.beginRead(cancelMapRead, "/Navigation/Maps/detail.walkmap"))
    detailReady = backgroundMap.open(mapSource) == navigator::WalkMapStatus::Ok;
  if (!detailReady && !backRequested) {
    mapSource.beginRead(cancelMapRead);
    backgroundMap.open(mapSource);
  }
  hasPosition = navigator::navigationPosition(millis(), fix);
  navigator::CurrentPosition position{{fix.latitudeE7, fix.longitudeE7}, fix.accuracyMeters, 0};
  const auto statusText = [&]() {
    if (hasPosition) return (fix.offRoute ? tr(STR_NAV_OFF_ROUTE_SNAPSHOT) : tr(STR_NAV_POSITION_SNAPSHOT));
    return (navigator::navigationSessionActive() ? tr(STR_NAV_WAITING_GPS) : tr(STR_NAV_GPS_INACTIVE));
  };
  const navigator::NavMapText mapText{tr(STR_NAV_TOTAL_ROUTE), tr(STR_NAV_DURATION), tr(STR_NAV_REMAINING_ROUTE),
                                      tr(STR_NAV_REMAINING_DURATION), tr(STR_NAV_ARRIVED_STATUS)};
  bool useGray = grayReady;
  bool drawn = navigator::NavScreenRenderer::drawOverview(
      display.getFrameBuffer(), display.getDisplayWidth(), display.getDisplayHeight(), routes.source(), routes.index(),
      &mapLayer, hasPosition ? &position : nullptr, statusText(), tr(STR_NAV_ROUTE_APPROX),
      useGray ? &grayLayer : nullptr, navigator::NavGrayPlane::Base, &mapText);
  if (useGray && grayLayer.status != navigator::WalkMapStatus::Ok && !backRequested) {
    // Unsupported coverage or damaged gray tile: use the existing vector map.
    LOG_INF("NAV", "gray fallback status=%u", static_cast<unsigned>(grayLayer.status));
    useGray = false;
    graySource.endRead();
    if (!mapSource.beginRead(cancelMapRead, "/Navigation/Maps/detail.walkmap") ||
        backgroundMap.open(mapSource) != navigator::WalkMapStatus::Ok) {
      mapSource.beginRead(cancelMapRead);
      backgroundMap.open(mapSource);
    }
    drawn = navigator::NavScreenRenderer::drawOverview(
        display.getFrameBuffer(), display.getDisplayWidth(), display.getDisplayHeight(), routes.source(),
        routes.index(), &mapLayer, hasPosition ? &position : nullptr, statusText(), tr(STR_NAV_ROUTE_APPROX));
  }
  if (drawn && useGray && !backRequested) {
    // SDK's calibrated differential gray base, then two masks. Reuse the one
    // framebuffer; source stays open and immutable for all three passes.
    display.displayGrayscaleBase(freeink::FreeInkDisplay::HALF_REFRESH);
    for (auto plane : {navigator::NavGrayPlane::Lsb, navigator::NavGrayPlane::Msb}) {
      drawn = navigator::NavScreenRenderer::drawOverview(
          display.getFrameBuffer(), display.getDisplayWidth(), display.getDisplayHeight(), routes.source(),
          routes.index(), nullptr, &position, statusText(), tr(STR_NAV_ROUTE_APPROX), &grayLayer, plane, &mapText);
      if (!drawn || grayLayer.status != navigator::WalkMapStatus::Ok || backRequested) {
        drawn = false;
        break;
      }
      if (plane == navigator::NavGrayPlane::Lsb)
        display.copyGrayscaleLsbBuffers(display.getFrameBuffer());
      else
        display.copyGrayscaleMsbBuffers(display.getFrameBuffer());
    }
    if (!drawn && !backRequested) {
      // Never display a partially loaded plane. A normal BW refresh also
      // restores controller RAM after an interrupted gray-mask upload.
      useGray = false;
      drawn = navigator::NavScreenRenderer::drawOverview(display.getFrameBuffer(), display.getDisplayWidth(),
                                                         display.getDisplayHeight(), routes.source(), routes.index(),
                                                         nullptr, &position, statusText(), tr(STR_NAV_ROUTE_APPROX));
    }
  }
  // Slow SD reads can age a fix out. Never publish it as a current position.
  // Fall back without the background to avoid another long regional read.
  if (hasPosition && !navigator::navigationPosition(millis(), fix) && !backRequested) {
    hasPosition = false;
    useGray = false;
    drawn = navigator::NavScreenRenderer::drawOverview(display.getFrameBuffer(), display.getDisplayWidth(),
                                                       display.getDisplayHeight(), routes.source(), routes.index(),
                                                       nullptr, nullptr, statusText(), tr(STR_NAV_ROUTE_APPROX));
  }
  mapSource.endRead();
  graySource.endRead();
  LOG_INF("NAV", "map load ms=%u cancelled=%d", millis() - started, backRequested);
  if (backRequested) return;
  if (!drawn) {
    message("KAART NIET LEESBAAR", "STUUR ROUTE OPNIEUW");
    return;
  }
  LOG_INF("NAV", "map status=%u edges=%u", static_cast<unsigned>(mapLayer.status), mapLayer.edgeCount);
  if (useGray)
    display.displayGrayBuffer();
  else
    display.displayBuffer(refresh == navigator::NavigationRefresh::Full ? freeink::FreeInkDisplay::FULL_REFRESH
                                                                        : freeink::FreeInkDisplay::FAST_REFRESH);
  refreshPolicy.rendered(millis(), hasPosition ? &fix : nullptr, refresh);
}
}  // namespace

void setup() {
  BoardConfig::holdPowerRails();
  delay(250);
  Serial.begin(115200);
#if LOG_SERIAL_HAS_TX_TIMEOUT
  logSerial.setTxTimeoutMs(1);
#endif
  navigator::NavMarker marker{};
  const bool present = navigator::readNavMarker(marker);
#if defined(CROSSINK_NAV_AUTOSTART)
  constexpr bool devOverride = true;
#else
  constexpr bool devOverride = false;
#endif
  if (navigator::chooseNavBootRoute(present, devOverride) == navigator::NavLaunchDecision::ReturnToReaderDashboard) {
    navigator::clearNavMarker();
    if (dashboard_boot::switchToSlot1()) {
      delay(100);
      ESP.restart();
    }
    return;
  }
  const bool x3 = freeink::selectXteinkDevice();
  if (!x3) freeink::applyXteinkDisplayController();
  input.begin();
  I18N.setLanguage(Language::NL);  // This standalone navigator currently uses Dutch UI.
  sdReady = routeFs.begin();
  if (sdReady) {
    routes.load();
  }
  if (x3) display.setDisplayX3();
  display.begin();
  if (!display.framebufferReady()) return;
  ready = true;
  // Draw before advertising: slow e-paper startup cannot consume a BLE frame timeout.
  if (sdReady && !routes.hasRoute())
    message("GEEN ROUTE", "STUUR GPX VANAF IPHONE");
  else
    showRoute();
  if (!backRequested && !navigator::beginRouteReceiver())
    message("BLUETOOTH NIET BESCHIKBAAR", "TERUG EN PROBEER OPNIEUW");
  lastReceiverOpen = navigator::routeReceiverOpen();
  refreshPolicy.setActiveView(lastReceiverOpen);
  LOG_INF("NAV", "ready sd=%d route=%d heap=%u largest=%u", sdReady, routes.hasRoute(), ESP.getFreeHeap(),
          ESP.getMaxAllocHeap());
}

void loop() {
  if (!ready) {
    delay(100);
    return;
  }
  input.update();
  if (backRequested || input.wasReleased(InputManager::BTN_BACK)) {
    if (navigator::clearNavMarker() && dashboard_boot::switchToSlot1()) {
      navigator::stopRouteReceiver();
      routes.disconnect();
      delay(100);
      ESP.restart();
    }
    return;
  }
  navigator::RouteTransferStatus result;
  if (navigator::pollRouteReceiver(routes, result)) {
    if (result.code == navigator::RouteTransferCode::RouteAccepted) {
      showRoute();
      LOG_INF("NAV", "route=%u heap=%u minHeap=%u largest=%u stackHighWater=%u", routes.index().routeId,
              ESP.getFreeHeap(), ESP.getMinFreeHeap(), ESP.getMaxAllocHeap(), uxTaskGetStackHighWaterMark(nullptr));
    } else if (result.code == navigator::RouteTransferCode::RouteStorageFailed)
      message("OPSLAAN MISLUKT", "CONTROLEER SD KAART");
    // No per-chunk screen refresh. Validation errors are reported to the phone.
  }
  if (input.wasReleased(InputManager::BTN_CONFIRM) && !routes.transferring()) {
    if (!navigator::routeReceiverOpen()) {
      sdReady = routeFs.begin();
      if (sdReady) {
        routes.load();
      }
      message("ROUTE ONTVANGEN", "STUUR GPX VANAF IPHONE");
      navigator::beginRouteReceiver();
    } else {
      showRoute();
    }
  }
  navigator::LivePosition automaticPosition{};
  const navigator::LivePosition* automaticFix =
      navigator::navigationPosition(millis(), automaticPosition) ? &automaticPosition : nullptr;
  // A fix flagged forceRefresh is an urgent/synthetic position that must reach
  // the screen immediately; route it through the policy's manual path so it
  // bypasses the 30-second routine-movement throttle. Ordinary fixes keep the
  // throttled path (manual=false).
  // Consume the request once. Keeping it on the retained position would make
  // every 20 ms loop redraw the same e-ink frame until the fix ages out.
  const bool forced = automaticFix != nullptr && navigator::takeNavigationForceRefresh();
  const auto automaticRefresh = refreshPolicy.decide(millis(), automaticFix, forced, false);
  if (automaticRefresh != navigator::NavigationRefresh::None && !routes.transferring())
    showRoute(false, automaticRefresh);
  const bool receiverOpen = navigator::routeReceiverOpen();
  if (lastReceiverOpen && !receiverOpen && !routes.hasRoute()) showRoute();
  lastReceiverOpen = receiverOpen;
  refreshPolicy.setActiveView(receiverOpen);
  delay(20);
}
#endif
