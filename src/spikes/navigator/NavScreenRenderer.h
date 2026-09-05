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

// The two metric captions of the four-gray overview footer, provided by the
// caller as already-localized strings. When a live fix is close enough to the
// route to trust along-route progress, the remaining pair replaces the total
// pair above the two value columns.
struct NavMapText {
  const char* totalRoute;         // whole-route distance caption
  const char* duration;           // whole-route estimated time caption
  const char* remainingRoute;     // distance-still-to-go caption
  const char* remainingDuration;  // time-still-to-go caption
};

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
  // Decides the two metric values of the gray footer for a fix. Remaining is
  // chosen only when a fix is present, its straight-line distance to the
  // route is within the off-route guard used by the footer estimate
  // (max(40 m, 2x accuracy)) and the route declares a positive total;
  // otherwise the established total metrics win.
  static NavFooterMetrics chooseFooterMetrics(const CurrentPosition* position, const RouteProximity& proximity,
                                              const RouteIndex& route);
  // Minutes left for `remainingMeters` of `totalMeters`, proportional to
  // `estimatedMinutes`, rounded half-up and clamped to `estimatedMinutes`.
  // Returns 0 when `totalMeters` is 0. Integer-only, no overflow.
  static uint16_t remainingMinutes(uint16_t estimatedMinutes, uint32_t remainingMeters, uint32_t totalMeters);

  // Production route-foundation screens: no schematic map or simulated GPS.
  static void drawMessage(uint8_t* frameBuffer, uint16_t widthPx, uint16_t heightPx, const char* title,
                          const char* detail);
  static bool drawOverview(uint8_t* frameBuffer, uint16_t widthPx, uint16_t heightPx, RouteByteSource& source,
                           const RouteIndex& index, WalkMapLayer* background = nullptr,
                           const CurrentPosition* position = nullptr, const char* statusText = nullptr,
                           const char* routeDistanceTitle = nullptr, GrayMapLayer* gray = nullptr,
                           NavGrayPlane plane = NavGrayPlane::Base, const NavMapText* mapText = nullptr);
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
