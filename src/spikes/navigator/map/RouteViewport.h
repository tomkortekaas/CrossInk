#pragma once

#include <cstdint>

// Allocation-free route viewport and local equirectangular projection for the
// X3 navigator (src/spikes/navigator/map/).
//
// Task 5 - route projection. The viewport consumes the *bounded* overview
// inside a decoded RouteIndex (route/RoutePackageV1.h) and answers the
// question "where does this GeoPoint land on the map?" for the detailed
// polyline renderer. Everything is fixed-width integer math: no heap, no
// recursion, no exceptions, no libm on the runtime path. Longitude is scaled
// once per viewport by a fixed-point cosine of a reference latitude (local
// equirectangular approximation), so equal ground meters map to equal pixels
// in both axes for the region the viewport was built for. Products that can
// grow are computed in int64_t and saturate safely.
//
// Coordinate conventions:
//   * GeoPoint is signed E7 (1e-7 degree) - the wire domain of the header
//     origin and segment anchors.
//   * ScreenPoint/Rect are logical *portrait* map pixels: x grows right
//     (east), y grows down (south). North projects to smaller y.
//   * A valid map Rect has width > 0 and height > 0; factories additionally
//     require a positive padded inner rect (padding is equal on all sides).
//   * project() returns coordinates in the map rect's pixel space and clamps
//     the final value to +/- RouteViewport::kMaxProjectedPx, so callers can
//     clip/reject far-outside positions without overflow.

namespace navigator {

struct RouteIndex;  // defined in route/RoutePackageV1.h; only the .cpp needs it.

// Compact fixed-width geographic point in 1e-7 degree units (E7).
struct GeoPoint {
  int32_t latitudeE7 = 0;
  int32_t longitudeE7 = 0;
};

static_assert(sizeof(GeoPoint) == 8, "GeoPoint must stay compact");

// Integer screen coordinate in logical portrait map pixels.
struct ScreenPoint {
  int x = 0;
  int y = 0;
};

static_assert(sizeof(ScreenPoint) == 8, "ScreenPoint must stay compact");

// Logical map rectangle (pixel space of the map region).
struct Rect {
  int x = 0;
  int y = 0;
  int width = 0;
  int height = 0;
};

static_assert(sizeof(Rect) == 16, "Rect must stay compact");

// A supplied current position. `accuracyMeters` sizes the position ring at
// the viewport scale; `bearingDegrees` is carried for the follow-up live
// session task and is not rendered in the foundation phase.
struct CurrentPosition {
  GeoPoint point;
  uint16_t accuracyMeters = 0;
  uint16_t bearingDegrees = 0;
};

static_assert(sizeof(CurrentPosition) <= 12, "CurrentPosition must stay compact");

class RouteViewport {
 public:
  // Equal padding applied on all four sides of the map rect when fitting.
  static constexpr int kDefaultPaddingPx = 8;
  // Ground span (meters across the shorter inner axis) used when fitting a
  // zero-extent route so a one-point route still gets a usable walking zoom.
  static constexpr uint32_t kDefaultFitSpanMeters = 200;
  // project() saturates screen coordinates to this range; anything beyond it
  // is many screens away and will be clipped away before drawing.
  static constexpr int32_t kMaxProjectedPx = 1 << 24;

  RouteViewport() = default;  // invalid until filled by a factory

  // Fits the bounded RouteIndex overview (every overview point) into
  // `mapRect` with `paddingPx` on every side, preserving the ground aspect
  // ratio and centering the content. Zero-extent bounds (one point / all
  // points equal) fall back to kDefaultFitSpanMeters centered on the point.
  // Returns an invalid viewport for an empty overview or an unusable rect.
  static RouteViewport fitOverview(const RouteIndex& route, const Rect& mapRect,
                                   int paddingPx = kDefaultPaddingPx);

  // Centers `point` at a walking scale: `spanMeters` of ground fits exactly
  // across the shorter inner axis (the width on a portrait map) after
  // `paddingPx` margins. Used for the follow view. `spanMeters` must be > 0.
  static RouteViewport centered(const GeoPoint& point, const Rect& mapRect, uint32_t spanMeters,
                                int paddingPx = kDefaultPaddingPx);

  bool isValid() const { return valid_; }

  const Rect& mapRect() const { return mapRect_; }

  // Projects an E7 point to integer map pixels (clamped to
  // +/- kMaxProjectedPx). Longitude deltas are folded at the antimeridian so
  // a point just across the 180 meridian lands next to the viewport instead
  // of ~360 degrees away. Calling on an invalid viewport returns {0, 0}.
  ScreenPoint project(const GeoPoint& point) const;

  // Pixel length of `meters` on the ground at this viewport's scale (latitude
  // axis; both axes agree in ground meters). Used for accuracy-scaled marker
  // rings. Returns 0 for an invalid viewport or meters == 0.
  int pixelsForMeters(uint32_t meters) const;

  // --- read-only geometry helpers (exposed for tests / the renderer) ---
  int32_t centerLatitudeE7() const { return centerLatitudeE7_; }
  int32_t centerLongitudeE7() const { return centerLongitudeE7_; }
  int paddingPx() const { return paddingPx_; }

 private:
  int64_t scaledPixel(int64_t projectedUnits) const;

  Rect mapRect_{};
  int32_t centerLatitudeE7_ = 0;
  int32_t centerLongitudeE7_ = 0;
  uint32_t cosScaleQ16_ = 0;  // cos(reference latitude) * 65536
  uint64_t scaleQ32_ = 0;     // uniform pixels per projected E7 unit, Q32
  int paddingPx_ = 0;
  bool valid_ = false;
};

static_assert(sizeof(RouteViewport) <= 64, "RouteViewport must stay compact");

}  // namespace navigator
