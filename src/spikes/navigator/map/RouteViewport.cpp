#include "RouteViewport.h"

#include <algorithm>

#include "../route/RoutePackageV1.h"

// Fixed-point local equirectangular projection.
//
// Projected units ("U") are E7 steps scaled so that one U is the same ground
// distance on both axes: latitude is used as-is, longitude is multiplied by a
// fixed Q16 cosine of the reference (center) latitude. One degree of latitude
// is 1e7 U and ~111,320 m, so one U is ~11,132 um on the ground. The pixel
// scale is a Q32 fixed point (pixels per U) chosen per viewport.
//
// All products are int64_t and saturate; on-screen coordinates are clamped to
// +/- kMaxProjectedPx so far-outside positions stay finite and clipable.

namespace navigator {
namespace {

constexpr int64_t kLonHalfTurnE7 = 1800000000LL;
constexpr int64_t kLonFullTurnE7 = 3600000000LL;
constexpr int64_t kLatMaxE7 = 900000000LL;
constexpr int64_t kMicroMetersPerE7 = 11132;  // 111,320 m / 1e7 E7, in um

// Fold a longitude difference (E7) to the shortest arc, [-180, +180] degrees.
inline int64_t foldLongitudeE7(int64_t e7) {
  e7 %= kLonFullTurnE7;  // result in (-full, full)
  if (e7 > kLonHalfTurnE7) {
    e7 -= kLonFullTurnE7;
  } else if (e7 < -kLonHalfTurnE7) {
    e7 += kLonFullTurnE7;
  }
  return e7;
}

// v * m / 2^s rounded half away from zero (m and s non-negative).
inline int64_t mulShiftRound(int64_t v, int64_t m, unsigned s) {
  const int64_t r = v * m;
  const int64_t half = int64_t{1} << (s - 1);
  return r >= 0 ? (r + half) >> s : -(((-r) + half) >> s);
}

// cos(degrees) * 65536 for latitudes inside [-90, 90] degrees E7. Static
// 5-degree lookup with linear interpolation: monotone, bounded, no libm, and
// within ~0.1% of the true cosine - far below any rendering error that
// matters for a local route viewport.
constexpr int32_t kCosTableQ16[19] = {
    65536, 65287, 64540, 63303, 61584, 59396, 56756, 53684, 50203, 46341,
    42126, 37590, 32768, 27697, 22415, 16962, 11380, 5712,  0,
};

int32_t cosineScaleQ16(int32_t latitudeE7) {
  int64_t absLat = latitudeE7 < 0 ? -static_cast<int64_t>(latitudeE7) : latitudeE7;
  absLat = std::min(absLat, kLatMaxE7);
  constexpr int64_t kStepE7 = 50000000;  // 5 degrees
  const int64_t index = absLat / kStepE7;  // 0..18 (18 only at exactly 90 deg)
  const int64_t frac = absLat % kStepE7;
  if (index >= 18) {
    return kCosTableQ16[18];  // exactly 90 degrees -> cos 0
  }
  const int64_t base = kCosTableQ16[index];
  const int64_t delta = static_cast<int64_t>(kCosTableQ16[index + 1]) - base;
  int64_t product = delta * frac;
  const int64_t half = kStepE7 / 2;
  const int64_t offset = product >= 0 ? (product + half) / kStepE7 : -(((-product) + half) / kStepE7);
  return static_cast<int32_t>(base + offset);
}

}  // namespace

RouteViewport RouteViewport::fitOverview(const RouteIndex& route, const Rect& mapRect, int paddingPx) {
  RouteViewport viewport;
  const int innerW = mapRect.width - 2 * paddingPx;
  const int innerH = mapRect.height - 2 * paddingPx;
  if (mapRect.width <= 0 || mapRect.height <= 0 || paddingPx < 0 || innerW < 1 || innerH < 1) {
    return viewport;
  }
  if (route.overviewPointCount == 0) {
    return viewport;
  }

  // Bounding box over the bounded overview. Longitudes are measured as
  // shortest-arc deltas from the first point so antimeridian-local routes
  // stay local instead of spanning the world.
  const int64_t referenceLon = static_cast<int64_t>(route.overview[0].longitudeE5) * 100;
  int64_t minLat = static_cast<int64_t>(route.overview[0].latitudeE5) * 100;
  int64_t maxLat = minLat;
  int64_t minDx = 0;
  int64_t maxDx = 0;
  for (uint16_t i = 0; i < route.overviewPointCount; ++i) {
    const int64_t lat = static_cast<int64_t>(route.overview[i].latitudeE5) * 100;
    const int64_t lon = static_cast<int64_t>(route.overview[i].longitudeE5) * 100;
    const int64_t dx = foldLongitudeE7(lon - referenceLon);
    if (i > 0) {
      minLat = std::min(minLat, lat);
      maxLat = std::max(maxLat, lat);
      minDx = std::min(minDx, dx);
      maxDx = std::max(maxDx, dx);
    }
  }

  const int64_t centerLat = (minLat + maxLat) / 2;
  const int64_t centerLon = foldLongitudeE7(referenceLon + (minDx + maxDx) / 2);
  const int64_t latExtent = maxLat - minLat;
  const int64_t lonExtent = maxDx - minDx;
  const int32_t cosQ16 = cosineScaleQ16(static_cast<int32_t>(centerLat));

  uint64_t scale = 0;
  if (lonExtent > 0) {
    const int64_t xUExtent = mulShiftRound(lonExtent, cosQ16, 16);
    if (xUExtent > 0) {
      scale = (static_cast<uint64_t>(innerW) << 32) / static_cast<uint64_t>(xUExtent);
    }
  }
  if (latExtent > 0) {
    const uint64_t yScale = (static_cast<uint64_t>(innerH) << 32) / static_cast<uint64_t>(latExtent);
    scale = scale == 0 ? yScale : std::min(scale, yScale);
  }

  if (scale == 0) {
    // Zero-extent overview (one point / all points equal): use the default
    // walking span so the fallback still has a usable, documented zoom.
    const int shorter = std::min(innerW, innerH);
    const uint64_t spanU = static_cast<uint64_t>(kDefaultFitSpanMeters) * 1000000ULL / kMicroMetersPerE7;
    if (spanU == 0) {
      return viewport;
    }
    scale = (static_cast<uint64_t>(shorter) << 32) / spanU;
  }

  viewport.mapRect_ = mapRect;
  viewport.paddingPx_ = paddingPx;
  viewport.centerLatitudeE7_ = static_cast<int32_t>(centerLat);
  viewport.centerLongitudeE7_ = static_cast<int32_t>(centerLon);
  viewport.cosScaleQ16_ = static_cast<uint32_t>(cosQ16);
  viewport.scaleQ32_ = scale;
  viewport.valid_ = true;
  return viewport;
}

RouteViewport RouteViewport::centered(const GeoPoint& point, const Rect& mapRect, uint32_t spanMeters,
                                      int paddingPx) {
  RouteViewport viewport;
  const int innerW = mapRect.width - 2 * paddingPx;
  const int innerH = mapRect.height - 2 * paddingPx;
  if (mapRect.width <= 0 || mapRect.height <= 0 || paddingPx < 0 || innerW < 1 || innerH < 1 ||
      spanMeters == 0) {
    return viewport;
  }
  // spanMeters of ground across the shorter inner axis; one meter is
  // 1e6 / 11132 projected units.
  const uint64_t spanU = static_cast<uint64_t>(spanMeters) * 1000000ULL / kMicroMetersPerE7;
  if (spanU == 0) {
    return viewport;
  }
  const int shorter = std::min(innerW, innerH);
  const uint64_t scale = (static_cast<uint64_t>(shorter) << 32) / spanU;

  viewport.mapRect_ = mapRect;
  viewport.paddingPx_ = paddingPx;
  viewport.centerLatitudeE7_ = point.latitudeE7;
  viewport.centerLongitudeE7_ = static_cast<int32_t>(foldLongitudeE7(point.longitudeE7));
  viewport.cosScaleQ16_ = static_cast<uint32_t>(cosineScaleQ16(point.latitudeE7));
  viewport.scaleQ32_ = scale;
  viewport.valid_ = true;
  return viewport;
}

int64_t RouteViewport::scaledPixel(int64_t projectedUnits) const {
  if (scaleQ32_ == 0) {
    return 0;
  }
  const int64_t scale = static_cast<int64_t>(scaleQ32_);
  // Pre-multiply clamp: far-outside positions are meaningless on screen, so
  // saturate the projected delta before the Q32 multiply instead of risking
  // an int64 overflow.
  const int64_t limit = (int64_t{1} << 62) / scale;
  const int64_t clamped = projectedUnits < -limit ? -limit : (projectedUnits > limit ? limit : projectedUnits);
  return mulShiftRound(clamped, scale, 32);
}

ScreenPoint RouteViewport::project(const GeoPoint& point) const {
  if (!valid_) {
    return ScreenPoint{0, 0};
  }
  const int64_t dxE7 = foldLongitudeE7(static_cast<int64_t>(point.longitudeE7) - centerLongitudeE7_);
  const int64_t dyE7 = static_cast<int64_t>(point.latitudeE7) - centerLatitudeE7_;
  const int64_t xU = mulShiftRound(dxE7, cosScaleQ16_, 16);
  const int64_t centerX = mapRect_.x + mapRect_.width / 2;
  const int64_t centerY = mapRect_.y + mapRect_.height / 2;
  int64_t sx = centerX + scaledPixel(xU);
  int64_t sy = centerY - scaledPixel(dyE7);
  sx = std::clamp(sx, static_cast<int64_t>(-kMaxProjectedPx), static_cast<int64_t>(kMaxProjectedPx));
  sy = std::clamp(sy, static_cast<int64_t>(-kMaxProjectedPx), static_cast<int64_t>(kMaxProjectedPx));
  return ScreenPoint{static_cast<int>(sx), static_cast<int>(sy)};
}

int RouteViewport::pixelsForMeters(uint32_t meters) const {
  if (!valid_ || meters == 0) {
    return 0;
  }
  const int64_t units = static_cast<int64_t>(meters) * 1000000LL / kMicroMetersPerE7;
  const int64_t px = scaledPixel(units);
  const int64_t clamped = std::clamp(px, static_cast<int64_t>(-kMaxProjectedPx),
                                     static_cast<int64_t>(kMaxProjectedPx));
  return static_cast<int>(clamped);
}

}  // namespace navigator
