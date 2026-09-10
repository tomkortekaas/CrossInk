#pragma once

#include <cstdint>

#include "RouteViewport.h"
#include "WalkMap.h"

// Allocation-free detailed polyline renderer for the X3 navigator map
// (src/spikes/navigator/map/).
//
// Task 5 - route canvas and renderer. RouteCanvas is a display-driver-
// independent drawing surface with only four primitives. RouteMapRenderer
// streams the *detailed* wire geometry of a validated RouteIndex from a
// RouteByteSource, projects it through a RouteViewport and draws it.
//
// RouteCanvas contract (shared with the host tests and the later one-bit
// navigator adapter):
//   * Coordinates are logical portrait map pixels (x east, y south) whose
//     origin is the map's top-left corner. The canvas logical size equals the
//     viewport's map rect.
//   * `clear()` fills the whole canvas with the background (white); the
//     renderer calls it exactly once, first, on every draw attempt that
//     passes index/viewport validation.
//   * All primitives draw ink (black) and are internally clipped: a line's
//     pen may overhang its endpoints by up to (widthPx - 1) / 2 pixels and the
//     canvas clips it to its own bounds. The renderer only ever passes
//     endpoints that are inside the map rect, and disc/ring primitives are
//     fully inside it (the renderer clips marker center/radius as well,
//     because the canvas has no per-primitive clip rectangle).
//   * A zero-length line draws nothing.
//
// Drawing conventions (visual dominance):
//   * The GPX route is the widest pen on the canvas (kRouteLineWidthPx) and is
//     drawn first, segment by segment, with each segment clipped independently
//     and never joined to the next one.
//   * When CurrentPosition carries trusted phone-tracked route progress
//     (hasRouteProgress and a declared positive total), the drawn route is
//     split at that progress in strict route order. The already-walked prefix
//     is one continuous *solid* line in the subordinate walked pen
//     (kRouteWalkedWidthPx), and the part still ahead is one continuous
//     *cased* stroke: a black outline/contour band whose white core is punched
//     back through it (RouteCanvas::caseContour + caseCore). The split is
//     measured on the same streamed detailed geometry as the along-route
//     remaining estimate; no synthetic joins are ever drawn or measured.
//   * The current-position marker is drawn last (ring first, then the filled
//     disc on top). The ring radius encodes accuracyMeters at the viewport
//     scale, clamped to [kMarkerMinRingRadiusPx, kMarkerMaxRingRadiusPx] and
//     then to the map rect; an absent position draws no marker at all.
//
// RenderStatus semantics:
//   Ok               everything drawn.
//   InvalidViewport  the viewport is not valid; nothing is drawn.
//   InvalidIndex     the RouteIndex counters are self-inconsistent (counts
//                    outside the decoder's fixed capacities or segment start
//                    indices that do not start at 0 and strictly increase);
//                    nothing is drawn.
//   ShortRead        the source stopped delivering bytes before the declared
//                    geometry was fully read (a validated route never does
//                    this); drawing stops at the failure point.

namespace navigator {

class RouteByteSource;  // defined in route/RoutePackageV1.h
struct RouteIndex;      // defined in route/RoutePackageV1.h
struct GrayMapLayer;

// Display-driver-independent drawing surface (see file comment).
class RouteCanvas {
 public:
  virtual ~RouteCanvas() = default;

  // Fills the whole canvas with the background (white).
  virtual void clear() = 0;

  // Ink stroke between two map-rect points, widthPx >= 1.
  virtual void line(int x0, int y0, int x1, int y1, int widthPx) = 0;

  // Cased route stroke, painted in two passes so the white core stays
  // continuous through polyline vertices: the renderer draws every
  // `caseContour` of the cased route first (the black outline/contour band
  // around `penWidthPx`) and only then every `caseCore` (the same stroke
  // punched back to the canvas background). Drawing an edge's contour after a
  // neighbour's core would clip that core at every shared vertex and turn the
  // white centre into dots. `penWidthPx` is the route pen the casing is built
  // around; the canvas decides the actual contour band and core widths for its
  // display (its own minimum stroke, its own background).
  //
  // Both default to a canvas without casing support: the contour is the
  // historical solid ink stroke and the core is dropped, so a plain BW display
  // keeps the dominant solid route and never loses route ink.
  virtual void caseContour(int x0, int y0, int x1, int y1, int penWidthPx) { line(x0, y0, x1, y1, penWidthPx); }
  virtual void caseCore(int, int, int, int, int) {}

  // Filled ink circle.
  virtual void disc(int centerX, int centerY, int radiusPx) = 0;

  // Ink ring (annulus) with pen widthPx >= 1.
  virtual void ring(int centerX, int centerY, int radiusPx, int widthPx) = 0;
  virtual void label(int, int, const char*) {}
  // Four-tone map run, 0 black / 1 dark / 2 light / 3 white.
  virtual void toneSpan(int, int, int, uint8_t) {}
};

enum class RenderStatus : uint8_t {
  Ok = 0,
  InvalidViewport,
  InvalidIndex,
  ShortRead,
};

struct RouteProximity {
  bool valid = false;
  uint32_t distanceMeters = 0;  // approximate local projection, not walking distance
  int dx = 0, dy = 0;
  // Along-route estimate for a live fix: meters still to walk to the end of
  // the route, measured along the streamed detailed geometry from the
  // projection of the fix onto its closest edge. Bounded to
  // [0, totalDistanceMeters]; a route with no measurable geometry keeps the
  // whole declared total. Never counts a synthetic join between segments.
  uint32_t remainingDistanceMeters = 0;
};
struct MapLabel {
  GeoPoint point;
  char text[45]{};
};
struct WalkMapLayer {
  WalkMapByteSource* source = nullptr;
  const WalkMap* map = nullptr;
  WalkMapStatus status = WalkMapStatus::NotOpen;
  uint32_t edgeCount = 0;
  // Bounded label cache in the global layer, not the constrained task stack.
  MapLabel labels[32]{};
  uint8_t labelCount = 0;
};

class RouteMapRenderer {
 public:
  // GPX route pen: the visually dominant stroke on the canvas.
  static constexpr int kRouteLineWidthPx = 3;
  // Walked-route pen when a live fix carries trusted phone-tracked progress
  // (CurrentPosition::hasRouteProgress): the already-walked prefix is drawn as
  // one continuous solid line in this subordinate pen - never as dashes - so
  // it reads as the part already behind the walker while the cased part still
  // ahead stays the visually dominant stroke.
  static constexpr int kRouteWalkedWidthPx = 1;
  // Position marker: thin ring plus small filled disc, drawn over the route.
  static constexpr int kMarkerRingWidthPx = 2;
  static constexpr int kMarkerDiscRadiusPx = 7;
  static constexpr int kMarkerMinRingRadiusPx = 11;
  static constexpr int kMarkerMaxRingRadiusPx = 64;

  // Streams the detailed geometry of every segment in `route` from `source`
  // and draws it onto `canvas` through `viewport`. `position` may be null.
  // No heap, no recursion, no exceptions, no RTTI; one <= 1,024-byte stack
  // buffer for delta reads, each individual read request <= 1,024 bytes. A
  // trusted-progress split streams that geometry up to three times (the
  // length measure, the casing contour, the casing core).
  static RenderStatus draw(RouteCanvas& canvas, RouteByteSource& source, const RouteIndex& route,
                           const RouteViewport& viewport, const CurrentPosition* position,
                           WalkMapLayer* background = nullptr, RouteProximity* proximity = nullptr,
                           GrayMapLayer* gray = nullptr);

 private:
  RouteMapRenderer() = delete;
};

}  // namespace navigator
