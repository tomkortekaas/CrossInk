#pragma once

#include <cstdint>

#include "NavState.h"

// Draws a supplied NavState into a physical row-major 1-bpp framebuffer.
//
// Direct-to-framebuffer: no heap, no GfxRenderer, no reader/renderer/font
// linkage. draw() receives the PHYSICAL panel dimensions (widthPx x heightPx;
// the X3 panel is 792x528) but authors the screen in logical PORTRAIT
// coordinates of heightPx x widthPx and maps each logical pixel (lx, ly) to
// the physical pixel (ly, heightPx - 1 - lx) -- the repository GfxRenderer
// Portrait transform.
//
// Framebuffer layout matches FreeInkDisplay: row-major 1-bpp, MSB-first
// (bit 7 of each byte = leftmost pixel), 1 = white, 0 = black. Rows that are
// not byte-aligned keep their unused trailing bits white.
namespace navigator {

class RouteByteSource;
struct WalkMapLayer;     // defined in route/RoutePackageV1.h
struct RouteIndex;       // defined in route/RoutePackageV1.h
struct CurrentPosition;  // defined in map/RouteViewport.h
struct GrayMapLayer;
// Native SDK nudge masks: black/white are 0 in both overlay planes;
// dark gray is 1 in LSB+MSB; light gray is 1 only in MSB.
enum class NavGrayPlane : uint8_t { Base, Lsb, Msb };
struct RouteProximity;  // defined in map/RouteMapRenderer.h

// The localized caption strings of the four-gray overview footer, provided by
// the caller as already-localized strings. When a live fix is close enough to
// the route to trust along-route progress, the remaining pair replaces the
// total pair above the two value columns. Once the fix has reliably reached
// the route end (hasReachedRouteEnd), the footer's bottom status line is
// replaced by `arrivedStatus`.
struct NavMapText {
  const char* totalRoute;         // whole-route distance caption
  const char* duration;           // whole-route estimated time caption
  const char* remainingRoute;     // distance-still-to-go caption
  const char* remainingDuration;  // time-still-to-go caption
  const char* arrivedStatus;      // footer status when the route end is reached
};

// Caller-built content of the fixed maneuver instruction band shown above the
// four-gray overview map during a live walk. The caller (NavigatorMain) fills
// it from its RouteManeuverSelector after drawOverview returns the already
// computed RouteProximity, so the selector (never rendering code) owns which
// maneuver is current. `action` and `street` are already-localized caller
// strings: `action` is the translated turn label (tr(STR_*)) and `street` is
// an optional bounded route/maneuver name whose absence never suppresses the
// arrow, distance or action. The band painter truncates both further so they
// can never collide with the maneuver symbol or leave the band.
struct NavManeuverPresentation {
  Maneuver maneuver = Maneuver::Straight;
  uint16_t distanceMeters = 0;
  const char* action = nullptr;
  const char* street = nullptr;
};

// The presentation stays a plain value type so a band can live in static or
// stack storage beside the selector; no heap, no strings, no SDK linkage.
static_assert(sizeof(NavManeuverPresentation) <= 24, "maneuver presentation must stay small");

// Which metric pair the gray footer shows for a fix.
enum class NavMetricMode : uint8_t {
  Total = 0,  // whole-route distance and estimated duration
  Remaining,  // distance and minutes still to walk from the fix
};

struct NavFooterMetrics {
  NavMetricMode mode = NavMetricMode::Total;
  uint32_t distanceMeters = 0;  // value under the left caption
  uint16_t minutes = 0;         // value under the right caption
};

class NavScreenRenderer {
 public:
  // Fixed instruction band geometry (logical portrait pixels), shared by
  // drawOverview's reserve step and drawManeuverBand so both always agree.
  // The band sits between the route-name header and the map: its top is the
  // header height of the active overview mode and its height is a fixed
  // fraction of the logical panel height.
  static constexpr int kOverviewHeaderGrayPx = 64;    // header above the four-gray map
  static constexpr int kOverviewHeaderPlainPx = 120;  // header above the plain vector map
  static constexpr int kManeuverBandHeightPercent = 22;

  // A live fix may confirm arrival only while it is at most this accurate; a
  // coarser fix cannot tell "standing at the end" from "still one
  // accuracy-sized step before it".
  static constexpr uint16_t kMaxArrivalAccuracyMeters = 25;
  // Absolute floor of the arrival band: the along-route remaining is measured
  // through pixel-quantized geometry, so it never needs to read exactly 0 for
  // the walker to actually be at the end.
  static constexpr uint16_t kMinArrivalRemainingMeters = 10;

  // Decides the two metric values of the gray footer for a fix. Remaining is
  // chosen only when a fix is present, its straight-line distance to the
  // route is within the off-route guard used by the footer estimate
  // (max(40 m, 2x accuracy)) and the route declares a positive total;
  // otherwise the established total metrics win.
  static NavFooterMetrics chooseFooterMetrics(const CurrentPosition* position, const RouteProximity& proximity,
                                              const RouteIndex& route);
  // Decides whether a live fix has reliably reached the route end, for the
  // four-gray overview's localized arrival footer status. Returns true only
  // when a fix is present, its along-route proximity result is valid, its
  // straight-line distance to the route is within the same off-route guard
  // used to trust the along-route projection (max(40 m, 2x accuracy)), its
  // accuracy is at most kMaxArrivalAccuracyMeters, the route declares a
  // positive total and the along-route distance still to walk is within a
  // conservative band of max(kMinArrivalRemainingMeters, 1x accuracy). Pure
  // integer decision: no heap, no floating point, no I18n.
  static bool hasReachedRouteEnd(const CurrentPosition* position, const RouteProximity& proximity,
                                 const RouteIndex& route);
  // Minutes left for `remainingMeters` of `totalMeters`, proportional to
  // `estimatedMinutes`, rounded half-up and clamped to `estimatedMinutes`.
  // Returns 0 when `totalMeters` is 0. Integer-only, no overflow.
  static uint16_t remainingMinutes(uint16_t estimatedMinutes, uint32_t remainingMeters, uint32_t totalMeters);

  // Production route-foundation screens: no schematic map or simulated GPS.
  static void drawMessage(uint8_t* frameBuffer, uint16_t widthPx, uint16_t heightPx, const char* title,
                          const char* detail);
  // Production four-gray (or plain vector) route overview. When `maneuver` is
  // non-null and the validated route declares maneuvers, a fixed instruction
  // band is reserved above the map before any map ink is drawn: the map top
  // moves down by the band height and the map rectangle shrinks by the same
  // amount, so the band can never overlap map or footer content. The caller
  // then updates its maneuver selector from the `outProximity` this pass
  // already computed (no second geometry scan) and paints the band content
  // into the reserved area with drawManeuverBand. A null/empty `maneuver`, a
  // route without maneuvers, and every pre-existing call site keep the
  // current map-only layout byte-for-byte.
  static bool drawOverview(uint8_t* frameBuffer, uint16_t widthPx, uint16_t heightPx, RouteByteSource& source,
                           const RouteIndex& index, WalkMapLayer* background = nullptr,
                           const CurrentPosition* position = nullptr, const char* statusText = nullptr,
                           const char* routeDistanceTitle = nullptr, GrayMapLayer* gray = nullptr,
                           NavGrayPlane plane = NavGrayPlane::Base, const NavMapText* mapText = nullptr,
                           const NavManeuverPresentation* maneuver = nullptr, RouteProximity* outProximity = nullptr);
  // Paints a maneuver presentation into the fixed instruction band that a
  // preceding drawOverview(maneuver != nullptr) reserved above the map. Must
  // be called with the same widthPx/heightPx/plane and the same `gray` mode
  // as that drawOverview; NavigatorMain calls it once per plane after updating
  // its selector, so Base/LSB/MSB stay deterministic and identical. Every
  // stroke is laid out strictly inside the reserved band rectangle, so the
  // band never overlaps the map or footer on any supported panel size.
  static void drawManeuverBand(uint8_t* frameBuffer, uint16_t widthPx, uint16_t heightPx,
                               const NavManeuverPresentation& presentation,
                               NavGrayPlane plane = NavGrayPlane::Base, bool gray = true);
  // A supplied position must be fresh and validated by the caller. It centers
  // the map at a 400-meter walking span. statusText is already localized.
  // Legacy screen. draw() is byte-identical to the historical static NavSplash
  // for the default NavState and draws the schematic map for every status.
  static void draw(uint8_t* frameBuffer, uint16_t widthPx, uint16_t heightPx, const NavState& state);

  // Routed screen. Draws the full legacy screen first (border, maneuver /
  // status band, separator, schematic map, bottom status). Only when the
  // caller supplies a *validated* route (routeSource + routeIndex, both from a
  // successful validateRoutePackageV1) and the state is Navigating does this
  // overload replace the schematic lower map area with the real route through
  // RouteMapRenderer, drawing the optional CurrentPosition marker last.
  //
  // A null/incomplete route (nullptr source or index), a non-Navigating status
  // or an unfit map rectangle keeps the exact legacy draw() output. If the
  // route renderer fails after clearing the map, the legacy schematic map is
  // redrawn in place, so the frame never shows a half-finished real route.
  // `position` may be null. No heap, recursion, exceptions, RTTI or libm.
  static void draw(uint8_t* frameBuffer, uint16_t widthPx, uint16_t heightPx, const NavState& state,
                   RouteByteSource* routeSource, const RouteIndex* routeIndex, const CurrentPosition* position);
};

}  // namespace navigator
