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
#include "NavigationStartup.h"
#include "NavigationViewController.h"
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
navigator::NavigationViewController viewController;  // session view mode (Overview default)
navigator::NavigationStartup startup;                // calm entry submission gate
bool ready = false;
bool sdReady = false;
bool lastReceiverOpen = false;
bool backRequested = false;
bool cancelMapRead() {
  input.update();
  if (input.wasReleased(InputManager::BTN_BACK) &&
      navigator::mapNavigatorButton(InputManager::BTN_BACK) == navigator::NavigatorAction::ReturnToDashboard)
    backRequested = true;
  return backRequested;
}

void message(const char* title, const char* detail) {
  navigator::NavScreenRenderer::drawMessage(display.getFrameBuffer(), display.getDisplayWidth(),
                                            display.getDisplayHeight(), title, detail);
  display.displayBuffer(freeink::FreeInkDisplay::FULL_REFRESH);
}
// Draws the quiet route-layout loading frame of a calm navigator entry: the
// route geometry in the final navigation layout, with the compact localized
// loading status in the normal footer/status area, submitted as one
// full-quality refresh. No separate splash or full-screen loading page.
bool showRouteLoadingLayout() {
  const uint16_t width = display.getDisplayWidth();
  const uint16_t height = display.getDisplayHeight();
  const navigator::NavMapText mapText{tr(STR_NAV_TOTAL_ROUTE), tr(STR_NAV_DURATION), tr(STR_NAV_REMAINING_ROUTE),
                                      tr(STR_NAV_REMAINING_DURATION), tr(STR_NAV_ARRIVED_STATUS)};
  // Maps are opened after this frame, so it uses the plain overview layout
  // (the completed frame at entry has no live fix yet and uses it too).
  const navigator::Rect mapRect = navigator::NavScreenRenderer::overviewMapRect(width, height, false);
  const auto selection = viewController.selectViewport(routes.index(), mapRect, nullptr);
  const bool drawn = navigator::NavScreenRenderer::drawOverview(
      display.getFrameBuffer(), width, height, routes.source(), routes.index(), nullptr, nullptr,
      tr(STR_NAV_LOADING_ROUTE), tr(STR_NAV_ROUTE_APPROX), nullptr, navigator::NavGrayPlane::Base, &mapText, nullptr,
      nullptr, &selection.viewport);
  if (drawn) {
    display.displayBuffer(freeink::FreeInkDisplay::FULL_REFRESH);
    return true;
  }
  message("KAART NIET LEESBAAR", "STUUR ROUTE OPNIEUW");
  return false;
}

// Draws one complete route frame for the current navigation view and submits
// it with `refresh`. It first reads the latest valid fix and opens the best
// available background map without any intermediate display submission, so a
// route replacement or refresh replaces the visible frame in one pass. One
// viewport selection is made per submitted frame and the same object feeds
// the Base, LSB and MSB planes, so the grayscale planes cannot disagree
// geometrically. Terminal storage/route/map failures keep their explicit
// full-screen errors.
void showRouteFrame(navigator::NavigationRefresh refresh = navigator::NavigationRefresh::Full) {
  const uint16_t width = display.getDisplayWidth();
  const uint16_t height = display.getDisplayHeight();
  if (!sdReady) {
    message("SD KAART ONTBREEKT", "PLAATS KAART EN HERSTART");
    return;
  }
  if (!routes.hasRoute()) {
    message("GEEN ROUTE", navigator::routeReceiverOpen() ? "STUUR GPX VANAF IPHONE" : "OK - ROUTE ONTVANGEN");
    return;
  }
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
  const auto statusText = [&](bool waitingForGps) {
    if (waitingForGps) return tr(STR_NAV_WAITING_GPS);
    if (hasPosition) return (fix.offRoute ? tr(STR_NAV_OFF_ROUTE_SNAPSHOT) : tr(STR_NAV_POSITION_SNAPSHOT));
    return (navigator::navigationSessionActive() ? tr(STR_NAV_WAITING_GPS) : tr(STR_NAV_GPS_INACTIVE));
  };
  const navigator::NavMapText mapText{tr(STR_NAV_TOTAL_ROUTE), tr(STR_NAV_DURATION), tr(STR_NAV_REMAINING_ROUTE),
                                      tr(STR_NAV_REMAINING_DURATION), tr(STR_NAV_ARRIVED_STATUS)};
  const auto selectFor = [&](bool grayMode, const navigator::CurrentPosition* fixPosition) {
    return viewController.selectViewport(
        routes.index(), navigator::NavScreenRenderer::overviewMapRect(width, height, grayMode), fixPosition);
  };
  bool useGray = grayReady;
  navigator::NavigationViewportSelection selection = selectFor(useGray, hasPosition ? &position : nullptr);
  bool drawn = navigator::NavScreenRenderer::drawOverview(
      display.getFrameBuffer(), width, height, routes.source(), routes.index(), &mapLayer,
      hasPosition ? &position : nullptr, statusText(selection.waitingForGps), tr(STR_NAV_ROUTE_APPROX),
      useGray ? &grayLayer : nullptr, navigator::NavGrayPlane::Base, &mapText, nullptr, nullptr, &selection.viewport);
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
    selection = selectFor(false, hasPosition ? &position : nullptr);
    drawn = navigator::NavScreenRenderer::drawOverview(
        display.getFrameBuffer(), width, height, routes.source(), routes.index(), &mapLayer,
        hasPosition ? &position : nullptr, statusText(selection.waitingForGps), tr(STR_NAV_ROUTE_APPROX), nullptr,
        navigator::NavGrayPlane::Base, nullptr, nullptr, nullptr, &selection.viewport);
  }
  if (drawn && useGray && !backRequested) {
    // SDK's calibrated differential gray base, then two masks. Reuse the one
    // framebuffer; source stays open and immutable for all three passes.
    display.displayGrayscaleBase(freeink::FreeInkDisplay::HALF_REFRESH);
    for (auto plane : {navigator::NavGrayPlane::Lsb, navigator::NavGrayPlane::Msb}) {
      drawn = navigator::NavScreenRenderer::drawOverview(
          display.getFrameBuffer(), width, height, routes.source(), routes.index(), nullptr, &position,
          statusText(selection.waitingForGps), tr(STR_NAV_ROUTE_APPROX), &grayLayer, plane, &mapText, nullptr, nullptr,
          &selection.viewport);
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
      selection = selectFor(false, &position);
      drawn = navigator::NavScreenRenderer::drawOverview(
          display.getFrameBuffer(), width, height, routes.source(), routes.index(), nullptr, &position,
          statusText(selection.waitingForGps), tr(STR_NAV_ROUTE_APPROX), nullptr, navigator::NavGrayPlane::Base,
          nullptr, nullptr, nullptr, &selection.viewport);
    }
  }
  // Slow SD reads can age a fix out. Never publish it as a current position.
  // Fall back without the background to avoid another long regional read.
  if (hasPosition && !navigator::navigationPosition(millis(), fix) && !backRequested) {
    hasPosition = false;
    useGray = false;
    selection = selectFor(false, nullptr);
    drawn = navigator::NavScreenRenderer::drawOverview(
        display.getFrameBuffer(), width, height, routes.source(), routes.index(), nullptr, nullptr,
        statusText(selection.waitingForGps), tr(STR_NAV_ROUTE_APPROX), nullptr, navigator::NavGrayPlane::Base, nullptr,
        nullptr, nullptr, &selection.viewport);
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
  // Every navigation session starts in Overview; the mode is session state
  // only and is never persisted.
  viewController.reset();
  // Draw before advertising: slow e-paper startup cannot consume a BLE frame
  // timeout. Storage and the retained route loaded above without refreshing
  // the panel. Calm entry shows the route in the final navigation layout
  // first (compact loading status, one full-quality refresh), then opens the
  // fix/background resources without any intermediate submission and draws
  // the first complete route frame in place. No full-screen loading page.
  if (!sdReady) {
    startup.fail();
    message("SD KAART ONTBREEKT", "PLAATS KAART EN HERSTART");
  } else if (!routes.hasRoute()) {
    startup.fail();
    message("GEEN ROUTE", "STUUR GPX VANAF IPHONE");
  } else {
    if (startup.routeLayoutReady() == navigator::NavigationStartupDecision::LoadingLayout && !backRequested) {
      if (!showRouteLoadingLayout()) startup.fail();
    }
    if (!backRequested && startup.resourcesReady() == navigator::NavigationStartupDecision::CompletedLayout) {
      showRouteFrame(navigator::NavigationRefresh::Full);
    }
  }
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
  bool viewChanged = false;
  bool manualRefresh = false;
  bool returnToDashboard = backRequested;
  const uint8_t navButtons[] = {InputManager::BTN_BACK, InputManager::BTN_UP, InputManager::BTN_DOWN,
                                InputManager::BTN_CONFIRM};
  for (const uint8_t button : navButtons) {
    if (!input.wasReleased(button)) continue;
    switch (navigator::mapNavigatorButton(button)) {
      case navigator::NavigatorAction::ToggleView:
        viewController.toggle();
        viewChanged = true;
        break;
      case navigator::NavigatorAction::ManualRefresh:
        manualRefresh = true;
        break;
      case navigator::NavigatorAction::ReturnToDashboard:
        returnToDashboard = true;
        break;
      case navigator::NavigatorAction::None:
        break;
    }
  }
  if (returnToDashboard) {
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
      // Route replacement: receive and validation never submitted per-chunk
      // display updates; replace the visible route with one complete frame.
      showRouteFrame(navigator::NavigationRefresh::Full);
      LOG_INF("NAV", "route=%u heap=%u minHeap=%u largest=%u stackHighWater=%u", routes.index().routeId,
              ESP.getFreeHeap(), ESP.getMinFreeHeap(), ESP.getMaxAllocHeap(), uxTaskGetStackHighWaterMark(nullptr));
      // The accepted replacement is the sole display submission in this loop
      // iteration. Handle any queued button or forced-position refresh on the
      // next pass so replacement never produces two consecutive frames.
      delay(20);
      return;
    } else if (result.code == navigator::RouteTransferCode::RouteStorageFailed)
      message("OPSLAAN MISLUKT", "CONTROLEER SD KAART");
    // No per-chunk screen refresh. Validation errors are reported to the phone.
  }
  // Centralized released-button mapping (see mapNavigatorButton): Up/Down
  // toggle Overview/GPS zoom, OK refreshes the current view, and Back (above)
  // returns to the dashboard. The mapping is pure and host-tested, so a later
  // physical-button remap touches only that component.
  // OK always refreshes the current navigation frame. Receiver recovery is
  // intentionally not coupled to this semantic action.
  navigator::LivePosition automaticPosition{};
  const navigator::LivePosition* automaticFix =
      navigator::navigationPosition(millis(), automaticPosition) ? &automaticPosition : nullptr;
  // A fix flagged forceRefresh is an urgent/synthetic position that must reach
  // the screen immediately; route it through the policy's manual path so it
  // bypasses the selected routine interval's movement throttle. Ordinary fixes
  // keep the throttled path (manual=false).
  // Consume the request once. Keeping it on the retained position would make
  // every 20 ms loop redraw the same e-ink frame until the fix ages out.
  const bool forced = automaticFix != nullptr && navigator::takeNavigationForceRefresh();
  // A released Up/Down is a viewport change: the policy answers an immediate
  // Full refresh (it is never throttled or downgraded), and routine fixes
  // after the switch return to the configured fast/economical cadence.
  // navigationPosition() above expires a stale session, so derive the mode
  // here (after expiry, before decide): an active session applies its
  // LIVE_START v2 mode, otherwise the policy keeps the Economical cadence.
  refreshPolicy.setMode(navigator::navigationSessionActive() ? navigator::navigationRefreshMode()
                                                             : navigator::WalkingRefreshMode::Economical);
  const auto refresh = refreshPolicy.decide(millis(), automaticFix, forced || manualRefresh, viewChanged);
  if (refresh != navigator::NavigationRefresh::None && !routes.transferring()) showRouteFrame(refresh);
  const bool receiverOpen = navigator::routeReceiverOpen();
  if (lastReceiverOpen && !receiverOpen && !routes.hasRoute()) showRouteFrame();
  lastReceiverOpen = receiverOpen;
  refreshPolicy.setActiveView(receiverOpen);
  delay(20);
}
#endif
