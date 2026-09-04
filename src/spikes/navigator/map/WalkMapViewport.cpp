#include "WalkMapViewport.h"
#include <algorithm>
namespace navigator {
namespace {
template <class Predicate>
int32_t lower(int64_t lo, int64_t hi, Predicate predicate) {
  while (lo < hi) {
    const int64_t mid = lo + (hi - lo) / 2;
    if (predicate(int32_t(mid)))
      hi = mid;
    else
      lo = mid + 1;
  }
  return int32_t(lo);
}
} // namespace
bool walkMapBounds(const RouteViewport &v, GeoBounds &out) {
  if (!v.isValid() || v.centerLatitudeE7() < -850000000 ||
      v.centerLatitudeE7() > 850000000)
    return false;
  const auto rect = v.mapRect();
  const int64_t left = int64_t(rect.x) - 2,
                right = int64_t(rect.x) + rect.width + 1,
                top = int64_t(rect.y) - 2,
                bottom = int64_t(rect.y) + rect.height + 1;
  const int32_t lat = v.centerLatitudeE7(), lon = v.centerLongitudeE7();
  // Projection is monotone on this interval, which never crosses its wrapped
  // longitude discontinuity. Binary inversion avoids floating-point scales.
  const int32_t lonMin =
      int32_t(std::max(-1800000000ll, int64_t(lon) - 1799999999ll));
  const int32_t lonMax =
      int32_t(std::min(1800000000ll, int64_t(lon) + 1799999999ll));
  auto x = [&](int32_t p) { return v.project(GeoPoint{lat, p}).x; };
  auto y = [&](int32_t p) { return v.project(GeoPoint{p, lon}).y; };
  if (x(lonMin) >= left || x(lonMax) <= right)
    return false;
  out.westE7 = lower(lonMin, lonMax, [&](int32_t p) { return x(p) >= left; });
  out.eastE7 = lower(lonMin, lonMax, [&](int32_t p) { return x(p) > right; });
  out.southE7 =
      lower(-850000000, 850000000, [&](int32_t p) { return y(p) <= bottom; });
  out.northE7 =
      lower(-850000000, 850000000, [&](int32_t p) { return y(p) < top; });
  return true;
}
} // namespace navigator
