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
#include "ManeuverBandPlanner.h"
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
navigator::WalkMapSdSource graySource;
navigator::GrayMap grayMap;  // bounded 48-byte header state; no raster/label allocation
navigator::GrayMapLayer grayLayer{&graySource, &grayMap};
navigator::NavigationRefreshPolicy refreshPolicy;
navigator::NavigationViewController viewController;  // session view mode (Overview default)
navigator::NavigationStartup startup;                // calm entry submission gate
navigator::ManeuverBandPlanner maneuverBand;         // next-turn selection for the instruction band (8 bytes)
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
  const navigator::NavMapText mapText{tr(STR_NAV_ARRIVED_STATUS)};
  // Maps are opened after this frame, so the route is drawn over the clean
  // white route-only map under the same compact chrome the completed
  // Overview frame always uses - the loading frame is the final navigation
  // layout, never a legacy plain-vector screen.
  // The instruction band is reserved from the very first frame, so the map
  // rectangle does not change height the moment a fix arrives. Without a fix
  // the band counts down from the route start to the first turn.
  const bool bandReserved = navigator::ManeuverBandPlanner::reservesBand(routes.index());
  navigator::NavManeuverPresentation presentation{};
  navigator::RouteProximity proximity{};
  const navigator::Rect mapRect =
      navigator::NavScreenRenderer::overviewMapRect(width, height, true, bandReserved);
  const auto selection = viewController.selectViewport(routes.index(), mapRect, nullptr);
  const bool drawn = navigator::NavScreenRenderer::drawOverview(
      display.getFrameBuffer(), width, height, routes.source(), routes.index(), nullptr, nullptr,
      tr(STR_NAV_LOADING_ROUTE), tr(STR_NAV_ROUTE_APPROX), nullptr, navigator::NavGrayPlane::Base, &mapText,
      bandReserved ? &presentation : nullptr, &proximity, &selection.viewport, true);
  if (drawn && bandReserved) {
    const auto plan = maneuverBand.plan(routes.index(), nullptr, proximity);
    presentation.maneuver = plan.maneuver;
    presentation.distanceMeters = plan.distanceMeters;
    navigator::NavScreenRenderer::drawManeuverBand(display.getFrameBuffer(), width, height, presentation,
                                                   navigator::NavGrayPlane::Base, true);
  }
  if (drawn) {
    display.displayBuffer(freeink::FreeInkDisplay::FULL_REFRESH);
    return true;
  }
  message("KAART NIET LEESBAAR", "STUUR ROUTE OPNIEUW");
  return false;
}

// Draws one complete route frame for the current navigation view and submits
// it with `refresh`. Both navigation views (Overview and GPS zoom) always use
// the compact four-gray chrome; a live fix only adds the position marker and
// the calm gray raster is opened for the route area regardless of GPS, so no
// missing/expired fix ever demotes the frame to the legacy plain-vector
// overview. A whole-route Overview whose gray coverage is absent or over the
// map's tile budget keeps the compact chrome over a clean white route-only
// map. One viewport selection is made per submitted frame and the same object
// feeds the Base, LSB and MSB planes, so the grayscale planes cannot disagree
// geometrically. Terminal storage/route failures keep their explicit
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
  // Keep session entry immediate: the whole-route Overview is the compact
  // white route-only map and never scans the large gray-map index. Open the
  // calm raster only for GPS zoom once a valid centre is available.
  const bool grayReady =
      viewController.shouldOpenGrayBackground(hasPosition) && display.supportsStripGrayscale() && !backRequested &&
      graySource.beginRead(cancelMapRead, "/Navigation/Maps/gray.x3gm") &&
      grayMap.open(graySource) == navigator::WalkMapStatus::Ok;
  // Slow SD reads can age a fix out; re-read it before composing the frame.
  hasPosition = navigator::navigationPosition(millis(), fix);
  navigator::CurrentPosition position{{fix.latitudeE7, fix.longitudeE7}, fix.accuracyMeters, 0,
                                      fix.hasRouteProgress, fix.distanceFromStartMeters};
  const auto statusText = [&](bool waitingForGps) {
    if (waitingForGps) return tr(STR_NAV_WAITING_GPS);
    if (hasPosition) return (fix.offRoute ? tr(STR_NAV_OFF_ROUTE_SNAPSHOT) : tr(STR_NAV_POSITION_SNAPSHOT));
    return (navigator::navigationSessionActive() ? tr(STR_NAV_WAITING_GPS) : tr(STR_NAV_GPS_INACTIVE));
  };
  const navigator::NavMapText mapText{tr(STR_NAV_ARRIVED_STATUS)};
  // The instruction band. Whether it is reserved depends only on the route,
  // never on the fix, so every plane of this frame -- and the loading frame
  // before it -- agree about where the map starts. `presentation` is filled
  // once, after the Base pass hands back the RouteProximity it already
  // computed, and the auxiliary planes then repaint that same content.
  const bool bandReserved = navigator::ManeuverBandPlanner::reservesBand(routes.index());
  navigator::NavManeuverPresentation presentation{};
  navigator::RouteProximity proximity{};
  const auto bandArg = [&]() { return bandReserved ? &presentation : nullptr; };
  const auto paintBand = [&](navigator::NavGrayPlane plane) {
    if (bandReserved)
      navigator::NavScreenRenderer::drawManeuverBand(display.getFrameBuffer(), width, height, presentation, plane,
                                                     true);
  };
  // Both views share the compact chrome, so the map rectangle is always the
  // compact one, shortened by the band when one is reserved.
  const auto selectFor = [&](const navigator::CurrentPosition* fixPosition) {
    return viewController.selectViewport(
        routes.index(), navigator::NavScreenRenderer::overviewMapRect(width, height, true, bandReserved),
        fixPosition);
  };
  bool useGray = grayReady;
  navigator::NavigationViewportSelection selection = selectFor(hasPosition ? &position : nullptr);
  bool drawn = navigator::NavScreenRenderer::drawOverview(
      display.getFrameBuffer(), width, height, routes.source(), routes.index(), nullptr,
      hasPosition ? &position : nullptr, statusText(selection.waitingForGps), tr(STR_NAV_ROUTE_APPROX),
      useGray ? &grayLayer : nullptr, navigator::NavGrayPlane::Base, &mapText, bandArg(), &proximity,
      &selection.viewport, true);
  if (drawn && bandReserved) {
    // The selector is updated exactly once per submitted frame, from the
    // proximity this pass already produced; the auxiliary planes below reuse
    // the result so Base/LSB/MSB cannot show different instructions.
    const auto plan = maneuverBand.plan(routes.index(), hasPosition ? &position : nullptr, proximity);
    presentation.maneuver = plan.maneuver;
    presentation.distanceMeters = plan.distanceMeters;
    paintBand(navigator::NavGrayPlane::Base);
  }
  if (drawn && useGray && grayLayer.status != navigator::WalkMapStatus::Ok && !backRequested) {
    // Coverage absent/damaged, or the whole-route view spans more than the
    // map's tile budget. RouteMapRenderer cleared the map to white before
    // drawing the route, so this Base frame already is the compact white
    // route-only fallback - it is never repainted in the legacy plain-vector
    // style.
    LOG_INF("NAV", "gray coverage fallback status=%u", static_cast<unsigned>(grayLayer.status));
    useGray = false;
  }
  if (drawn && useGray && !backRequested) {
    // SDK's calibrated differential gray base, then two masks. Reuse the one
    // framebuffer; source stays open and immutable for all three passes.
    display.displayGrayscaleBase(freeink::FreeInkDisplay::HALF_REFRESH);
    for (auto plane : {navigator::NavGrayPlane::Lsb, navigator::NavGrayPlane::Msb}) {
      drawn = navigator::NavScreenRenderer::drawOverview(
          display.getFrameBuffer(), width, height, routes.source(), routes.index(), nullptr,
          hasPosition ? &position : nullptr, statusText(selection.waitingForGps), tr(STR_NAV_ROUTE_APPROX),
          &grayLayer, plane, &mapText, bandArg(), &proximity, &selection.viewport, true);
      if (!drawn || grayLayer.status != navigator::WalkMapStatus::Ok || backRequested) {
        drawn = false;
        break;
      }
      // Same presentation as the Base pass: the selector is not consulted
      // again, so the three planes are painted from one decision.
      paintBand(plane);
      if (plane == navigator::NavGrayPlane::Lsb)
        display.copyGrayscaleLsbBuffers(display.getFrameBuffer());
      else
        display.copyGrayscaleMsbBuffers(display.getFrameBuffer());
    }
    if (!drawn && !backRequested) {
      // Never display a partially loaded plane. A normal BW refresh also
      // restores controller RAM after an interrupted gray-mask upload.
      useGray = false;
      selection = selectFor(hasPosition ? &position : nullptr);
      drawn = navigator::NavScreenRenderer::drawOverview(
          display.getFrameBuffer(), width, height, routes.source(), routes.index(), nullptr,
          hasPosition ? &position : nullptr, statusText(selection.waitingForGps), tr(STR_NAV_ROUTE_APPROX), nullptr,
          navigator::NavGrayPlane::Base, &mapText, bandArg(), &proximity, &selection.viewport, true);
      if (drawn) paintBand(navigator::NavGrayPlane::Base);
    }
  }
  // Slow SD reads can age a fix out. Never publish it as a current position:
  // redraw the compact white route-only frame without the marker.
  if (hasPosition && !navigator::navigationPosition(millis(), fix) && !backRequested) {
    hasPosition = false;
    useGray = false;
    selection = selectFor(nullptr);
    drawn = navigator::NavScreenRenderer::drawOverview(
        display.getFrameBuffer(), width, height, routes.source(), routes.index(), nullptr, nullptr,
        statusText(selection.waitingForGps), tr(STR_NAV_ROUTE_APPROX), nullptr, navigator::NavGrayPlane::Base,
        &mapText, bandArg(), &proximity, &selection.viewport, true);
    // Deliberately no second plan() call: the fix that aged out is gone, and
    // re-planning without one would measure the countdown from the route
    // start again and jump the number backwards mid-walk. Repaint what the
    // frame above already decided.
    if (drawn) paintBand(navigator::NavGrayPlane::Base);
  }
  graySource.endRead();
  LOG_INF("NAV", "map load ms=%u cancelled=%d", millis() - started, backRequested);
  if (backRequested) return;
  if (!drawn) {
    message("KAART NIET LEESBAAR", "STUUR ROUTE OPNIEUW");
    return;
  }
  LOG_INF("NAV", "gray status=%u", static_cast<unsigned>(grayLayer.status));
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
      // Deterministic controls: Up selects GPS zoom and Down selects
      // Overview. Both selections are idempotent, so a repeated press of the
      // view you are already on reports no change and draws no new frame -
      // and Down can always bring the whole route back.
      case navigator::NavigatorAction::SelectGpsZoom:
        viewChanged = viewController.selectGpsZoom() || viewChanged;
        break;
      case navigator::NavigatorAction::SelectOverview:
        viewChanged = viewController.selectOverview() || viewChanged;
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
      // The selection belongs to the route that was replaced, so drop it
      // explicitly rather than relying on the route-id check alone.
      maneuverBand.reset();
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
  // Centralized released-button mapping (see mapNavigatorButton): Up selects
  // GPS zoom, Down selects Overview, OK refreshes the current view, and Back
  // (above) returns to the dashboard. The mapping is pure and host-tested,
  // so a later physical-button remap touches only that component.
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
