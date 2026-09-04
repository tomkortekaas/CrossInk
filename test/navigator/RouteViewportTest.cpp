// Host tests for the allocation-free route viewport / local equirectangular
// projection (src/spikes/navigator/map/RouteViewport.h/.cpp).
//
// Task 5 - strict TDD: these tests were written first, before any production
// code existed. The viewport under test must:
//   * fit the bounded RouteIndex overview into a logical portrait map rect
//     with equal padding, centered, without overflowing the outer rect;
//   * center a supplied point at a requested meters span for follow mode;
//   * project E7 GeoPoints to integer screen coordinates with fixed-point
//     integer math (no libm at runtime), one longitude-cosine scale fixed per
//     viewport, int64 intermediates and safe saturation;
//   * behave at Dutch latitudes, southern/western coordinates,
//     antimeridian-local routes, zero-extent bounds, a 40 km walk, one-point
//     routes, and points far outside the viewport;
//   * never divide by zero and reject invalid rects/inputs with an invalid
//     viewport.

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

#include "map/RouteViewport.h"
#include "route/RoutePackageV1.h"

namespace {

using navigator::GeoPoint;
using navigator::Rect;
using navigator::RouteIndex;
using navigator::RouteViewport;
using navigator::ScreenPoint;

// E7 -> E5 quantization shared with the wire contract (ties away from zero).
// RouteViewportTest only feeds E7 values that are exact multiples of 100 E7,
// so the E5 overview is exact; the helper exists to mirror the decoder's
// overview domain.
int64_t quantizeE5(int32_t e7) {
  const int64_t v = e7;
  const int64_t magnitude = v < 0 ? -v : v;
  const int64_t quantized = (magnitude + 50) / 100;
  return v < 0 ? -quantized : quantized;
}

// Builds a RouteIndex whose overview line holds `points` (E7, multiples of
// 100) split into segments exactly like the decoder's overview: every segment
// keeps all of its points (small routes), with per-segment slices stored in
// overviewBegin/overviewCount so fitOverview consumes bounded data.
RouteIndex makeIndex(const std::vector<GeoPoint>& points, const std::vector<uint16_t>& segmentStarts) {
  RouteIndex route{};
  if (points.empty() || segmentStarts.empty() || segmentStarts.front() != 0 ||
      points.size() > RouteIndex::kMaxOverviewPoints) {
    EXPECT_FALSE(points.empty()) << "makeIndex requires points";
    return route;
  }
  for (size_t i = 1; i < segmentStarts.size(); ++i) {
    if (segmentStarts[i] <= segmentStarts[i - 1] || segmentStarts[i] >= points.size()) {
      return route;
    }
  }

  route.pointCount = static_cast<uint16_t>(points.size());
  route.segmentCount = static_cast<uint16_t>(segmentStarts.size());
  route.originLatitudeE7 = points.front().latitudeE7;
  route.originLongitudeE7 = points.front().longitudeE7;

  uint16_t cursor = 0;
  for (uint16_t seg = 0; seg < route.segmentCount; ++seg) {
    const size_t begin = segmentStarts[seg];
    const size_t end = seg + 1 < segmentStarts.size() ? segmentStarts[seg + 1] : points.size();
    route.segments[seg].startPointIndex = static_cast<uint16_t>(begin);
    route.segments[seg].overviewBegin = cursor;
    for (size_t i = begin; i < end; ++i) {
      route.overview[cursor].latitudeE5 = static_cast<int32_t>(quantizeE5(points[i].latitudeE7));
      route.overview[cursor].longitudeE5 = static_cast<int32_t>(quantizeE5(points[i].longitudeE7));
      ++cursor;
    }
    route.segments[seg].overviewCount = static_cast<uint16_t>(cursor - route.segments[seg].overviewBegin);
  }
  route.overviewPointCount = cursor;
  return route;
}

// One-point helper.
RouteIndex makeIndex(const GeoPoint& point) { return makeIndex({point}, {0}); }

// Approximate E7 offset for `meters` north/south (1 E7 lat ~= 0.011132 m).
int32_t metersToLatE7(double meters) { return static_cast<int32_t>(std::llround(meters / 0.011132)); }

// Approximate E7 offset for `meters` east/west at `latitudeE7` using the real
// cosine (host tests may use libm; production code must not).
int32_t metersToLonE7(double meters, int32_t latitudeE7) {
  const double cosLat = std::cos(static_cast<double>(latitudeE7) / 10000000.0 * (3.14159265358979323846 / 180.0));
  return static_cast<int32_t>(std::llround(meters / (0.011132 * cosLat)));
}

// A portrait map rectangle with a simple area around Amsterdam.
constexpr Rect kMapRect = Rect{0, 0, 200, 320};
constexpr int kPad = 10;
// Amsterdam city center (52.3676 N, 4.9041 E) in E7.
constexpr int32_t kLatE7 = 523'676'000;
constexpr int32_t kLonE7 = 49'041'000;

// Projected bounding box helper.
struct BBox {
  int minX;
  int minY;
  int maxX;
  int maxY;
  int width() const { return maxX - minX + 1; }
  int height() const { return maxY - minY + 1; }
  int midX() const { return (minX + maxX) / 2; }
  int midY() const { return (minY + maxY) / 2; }
};

BBox projectBox(const RouteViewport& viewport, const std::vector<GeoPoint>& points) {
  BBox box{};
  bool first = true;
  for (const GeoPoint& p : points) {
    const ScreenPoint s = viewport.project(p);
    if (first) {
      box = BBox{s.x, s.y, s.x, s.y};
      first = false;
    } else {
      box.minX = std::min(box.minX, s.x);
      box.minY = std::min(box.minY, s.y);
      box.maxX = std::max(box.maxX, s.x);
      box.maxY = std::max(box.maxY, s.y);
    }
  }
  return box;
}

}  // namespace

// ---------------------------------------------------------------------------
// Fit and padding
// ---------------------------------------------------------------------------

TEST(RouteViewportTest, FitBalancedRouteStaysInsidePaddedPortraitRectAndCenters) {
  // A ~600 m x ~600 m ground square near Amsterdam. E7 offsets are exact
  // multiples of 100 so the E5 overview matches the source coordinates.
  const int32_t halfLat = metersToLatE7(300.0);
  const int32_t halfLon = metersToLonE7(300.0, kLatE7);
  const std::vector<GeoPoint> square = {
      GeoPoint{kLatE7 - halfLat, kLonE7 - halfLon},
      GeoPoint{kLatE7 + halfLat, kLonE7 - halfLon},
      GeoPoint{kLatE7 + halfLat, kLonE7 + halfLon},
      GeoPoint{kLatE7 - halfLat, kLonE7 + halfLon},
  };
  const RouteViewport viewport = RouteViewport::fitOverview(makeIndex(square, {0}), kMapRect, kPad);
  ASSERT_TRUE(viewport.isValid());

  const BBox box = projectBox(viewport, square);
  // Strictly inside the outer map rect...
  EXPECT_GE(box.minX, 0);
  EXPECT_GE(box.minY, 0);
  EXPECT_LT(box.maxX, kMapRect.width);
  EXPECT_LT(box.maxY, kMapRect.height);
  // ...centered in the padded rect...
  EXPECT_LE(std::abs(box.midX() - kMapRect.width / 2), 3);
  EXPECT_LE(std::abs(box.midY() - kMapRect.height / 2), 3);
  // ...and filling the shorter (width) inner axis, the limiting one for a
  // ground square in a portrait rect.
  const int innerW = kMapRect.width - 2 * kPad;
  EXPECT_GE(box.width(), innerW - 6);
  EXPECT_LE(box.width(), innerW + 6);
  EXPECT_GE(box.height(), innerW - 6);
  EXPECT_LE(box.height(), innerW + 6);
}

TEST(RouteViewportTest, FitNorthSouthRouteUsesFullInnerHeightAndKeepsLonCentered) {
  // A 40 km north-south walk: the height axis is the limiting one.
  const int32_t spanE7 = metersToLatE7(40000.0);
  const std::vector<GeoPoint> walk = {
      GeoPoint{kLatE7 - spanE7, kLonE7},
      GeoPoint{kLatE7 + spanE7, kLonE7},
  };
  const RouteViewport viewport = RouteViewport::fitOverview(makeIndex(walk, {0}), kMapRect, kPad);
  ASSERT_TRUE(viewport.isValid());

  const ScreenPoint south = viewport.project(walk[0]);
  const ScreenPoint north = viewport.project(walk[1]);
  const int innerH = kMapRect.height - 2 * kPad;
  const int dy = std::abs(south.y - north.y);
  EXPECT_GE(dy, innerH - 8);
  EXPECT_LE(dy, innerH + 8);
  EXPECT_GE(south.y, 0);
  EXPECT_LT(south.y, kMapRect.height);
  EXPECT_GE(north.y, 0);
  EXPECT_LT(north.y, kMapRect.height);
  // Both points share the longitude, so both sit on the map's vertical axis.
  EXPECT_EQ(south.x, kMapRect.width / 2);
  EXPECT_EQ(north.x, kMapRect.width / 2);
}

TEST(RouteViewportTest, FitEastWestRouteUsesFullInnerWidthAndKeepsLatCentered) {
  const int32_t spanE7 = metersToLonE7(40000.0, kLatE7);
  const std::vector<GeoPoint> walk = {
      GeoPoint{kLatE7, kLonE7 - spanE7},
      GeoPoint{kLatE7, kLonE7 + spanE7},
  };
  const RouteViewport viewport = RouteViewport::fitOverview(makeIndex(walk, {0}), kMapRect, kPad);
  ASSERT_TRUE(viewport.isValid());

  const ScreenPoint west = viewport.project(walk[0]);
  const ScreenPoint east = viewport.project(walk[1]);
  const int innerW = kMapRect.width - 2 * kPad;
  EXPECT_GE(west.x, 0);
  EXPECT_LT(east.x, kMapRect.width);
  EXPECT_EQ(west.y, kMapRect.height / 2);
  EXPECT_EQ(east.y, kMapRect.height / 2);
  EXPECT_GE(east.x - west.x, innerW - 8);
  EXPECT_LE(east.x - west.x, innerW + 8);
}

// ---------------------------------------------------------------------------
// Zero extent, one point, degenerate routes
// ---------------------------------------------------------------------------

TEST(RouteViewportTest, OnePointRouteFallsBackToDefaultSpanCenteredAtMapCenter) {
  const GeoPoint point{kLatE7, kLonE7};
  const RouteViewport viewport = RouteViewport::fitOverview(makeIndex(point), kMapRect, kPad);
  ASSERT_TRUE(viewport.isValid());

  const ScreenPoint center = viewport.project(point);
  EXPECT_EQ(center.x, kMapRect.width / 2);
  EXPECT_EQ(center.y, kMapRect.height / 2);

  // The fallback span is documented and usable: a point one default-span to
  // the west lands at the inner-left edge area, well inside the outer rect.
  const int innerW = kMapRect.width - 2 * kPad;
  const int32_t halfSpanE7 =
      metersToLonE7(static_cast<double>(RouteViewport::kDefaultFitSpanMeters) / 2.0, kLatE7);
  const ScreenPoint west = viewport.project(GeoPoint{kLatE7, kLonE7 - halfSpanE7});
  EXPECT_LT(west.x, center.x);
  EXPECT_GE(west.x, 0);
  EXPECT_LE(std::abs(west.x - (center.x - innerW / 2)), 6);
}

TEST(RouteViewportTest, IdenticalPointsAndZeroWidthOrZeroHeightRoutesStayValidAndCentered) {
  const GeoPoint a{kLatE7, kLonE7};
  const GeoPoint b{kLatE7 + metersToLatE7(500.0), kLonE7};  // vertical: zero width
  const GeoPoint c{kLatE7, kLonE7 + metersToLonE7(500.0, kLatE7)};  // horizontal: zero height

  const RouteViewport allSame = RouteViewport::fitOverview(makeIndex({a, a}, {0}), kMapRect, kPad);
  ASSERT_TRUE(allSame.isValid());
  EXPECT_EQ(allSame.project(a).x, kMapRect.width / 2);
  EXPECT_EQ(allSame.project(a).y, kMapRect.height / 2);

  const RouteViewport vertical = RouteViewport::fitOverview(makeIndex({a, b}, {0}), kMapRect, kPad);
  ASSERT_TRUE(vertical.isValid());
  EXPECT_EQ(vertical.project(a).x, vertical.project(b).x);
  EXPECT_LT(vertical.project(b).y, vertical.project(a).y);  // b is north -> up

  const RouteViewport horizontal = RouteViewport::fitOverview(makeIndex({a, c}, {0}), kMapRect, kPad);
  ASSERT_TRUE(horizontal.isValid());
  EXPECT_EQ(horizontal.project(a).y, horizontal.project(c).y);
  EXPECT_GT(horizontal.project(c).x, horizontal.project(a).x);  // c is east -> right
}

// ---------------------------------------------------------------------------
// Southern / western coordinates
// ---------------------------------------------------------------------------

TEST(RouteViewportTest, SouthernAndWesternCoordinatesProjectWithCorrectDirections) {
  // Sydney, Australia: southern + eastern hemisphere.
  const int32_t sydneyLatE7 = -338'688'000;
  const int32_t sydneyLonE7 = 1'512'093'000;
  const GeoPoint center{sydneyLatE7, sydneyLonE7};
  const RouteViewport viewport =
      RouteViewport::centered(center, kMapRect, 2000, kPad);
  ASSERT_TRUE(viewport.isValid());

  const ScreenPoint at = viewport.project(center);
  EXPECT_EQ(at.x, kMapRect.width / 2);
  EXPECT_EQ(at.y, kMapRect.height / 2);

  const GeoPoint north{sydneyLatE7 + metersToLatE7(500.0), sydneyLonE7};
  const GeoPoint south{sydneyLatE7 - metersToLatE7(500.0), sydneyLonE7};
  const GeoPoint east{sydneyLatE7, sydneyLonE7 + metersToLonE7(500.0, sydneyLatE7)};
  const GeoPoint west{sydneyLatE7, sydneyLonE7 - metersToLonE7(500.0, sydneyLatE7)};
  EXPECT_LT(viewport.project(north).y, at.y);
  EXPECT_GT(viewport.project(south).y, at.y);
  EXPECT_GT(viewport.project(east).x, at.x);
  EXPECT_LT(viewport.project(west).x, at.x);
  // Equal ground offsets should map to roughly equal pixel offsets (square).
  EXPECT_LE(std::abs(std::abs(viewport.project(north).y - at.y) - (at.x - viewport.project(west).x)), 8);

  // New York: western hemisphere (negative longitude).
  const int32_t nyLatE7 = 407'126'000;
  const int32_t nyLonE7 = -740'059'000;
  const GeoPoint nyCenter{nyLatE7, nyLonE7};
  const RouteViewport ny = RouteViewport::centered(nyCenter, kMapRect, 2000, kPad);
  ASSERT_TRUE(ny.isValid());
  const GeoPoint nyEast{nyLatE7, nyLonE7 + metersToLonE7(500.0, nyLatE7)};
  const GeoPoint nyNorth{nyLatE7 + metersToLatE7(500.0), nyLonE7};
  EXPECT_GT(ny.project(nyEast).x, ny.project(nyCenter).x);
  EXPECT_LT(ny.project(nyNorth).y, ny.project(nyCenter).y);
}

// ---------------------------------------------------------------------------
// Dutch latitude cosine scale
// ---------------------------------------------------------------------------

TEST(RouteViewportTest, DutchLatitudeLongitudeScaleCompressesLikeGroundMeters) {
  // At 52.3676 N, 500 m east-west spans cos(52.4) ~= 0.61x the longitude
  // degrees of 500 m north-south. The fixed-per-viewport cosine scale must
  // make both render at nearly the same pixel length.
  const GeoPoint center{kLatE7, kLonE7};
  const RouteViewport viewport = RouteViewport::centered(center, kMapRect, 1000, kPad);
  ASSERT_TRUE(viewport.isValid());

  const ScreenPoint at = viewport.project(center);
  const GeoPoint north{kLatE7 + metersToLatE7(500.0), kLonE7};
  const GeoPoint east{kLatE7, kLonE7 + metersToLonE7(500.0, kLatE7)};
  const int dy = std::abs(viewport.project(north).y - at.y);
  const int dx = std::abs(viewport.project(east).x - at.x);
  // Each leg is half the 1000 m span shown across the ~184 px inner width.
  EXPECT_GE(dy, 86);
  EXPECT_LE(dy, 98);
  EXPECT_GE(dx, 86);
  EXPECT_LE(dx, 98);
  // Without the cosine scale dx would be ~1.6x dy; with it they agree.
  EXPECT_LE(std::abs(dx - dy) * 100 / std::max(dx, dy), 8);
}

// ---------------------------------------------------------------------------
// Antimeridian
// ---------------------------------------------------------------------------

TEST(RouteViewportTest, AntimeridianLocalRouteStaysNearbyAndOrdersEastRightOfWest) {
  // A route that crosses the 180 meridian: -179.95 is EAST of +179.95 along
  // the short arc, so it must project to the right, not ~360 degrees away.
  const GeoPoint west{0, 1'799'500'000};   // 179.95 E
  const GeoPoint east{0, -1'799'500'000};  // 179.95 W == 180 - 0.05
  const RouteViewport viewport = RouteViewport::fitOverview(makeIndex({west, east}, {0}), kMapRect, kPad);
  ASSERT_TRUE(viewport.isValid());

  const ScreenPoint pw = viewport.project(west);
  const ScreenPoint pe = viewport.project(east);
  EXPECT_GT(pe.x, pw.x);
  EXPECT_GE(pw.x, 0);
  EXPECT_LT(pe.x, kMapRect.width);
  EXPECT_EQ(pw.y, pe.y);
}

TEST(RouteViewportTest, CenteredAntimeridianPointProjectsEastAndWestSides) {
  // Center exactly on the fold boundary (180.0 E): points at 179.99 and
  // -179.99 are +/-0.01 degrees away and must project onto the correct side
  // at short distances instead of wrapping ~360 degrees.
  const int32_t lonCenterE7 = 1'800'000'000;  // 180.0 E
  const GeoPoint center{0, lonCenterE7};
  const RouteViewport viewport = RouteViewport::centered(center, kMapRect, 4000, kPad);
  ASSERT_TRUE(viewport.isValid());

  const ScreenPoint at = viewport.project(center);
  EXPECT_EQ(at.x, kMapRect.width / 2);
  // -179.99 is 0.01 degrees EAST of 180.0 along the short arc.
  const GeoPoint justEast{0, -1'799'900'000};
  const GeoPoint justWest{0, 1'799'900'000};
  EXPECT_GT(viewport.project(justEast).x, at.x);
  EXPECT_LT(viewport.project(justWest).x, at.x);
  EXPECT_GE(viewport.project(justEast).x, 0);
  EXPECT_LT(viewport.project(justEast).x, kMapRect.width);
  EXPECT_GE(viewport.project(justWest).x, 0);
  EXPECT_LT(viewport.project(justWest).x, kMapRect.width);
  EXPECT_LE(std::abs(viewport.project(justEast).x - at.x - (at.x - viewport.project(justWest).x)), 4);
}

// ---------------------------------------------------------------------------
// Centered follow view
// ---------------------------------------------------------------------------

TEST(RouteViewportTest, CenteredViewPlacesPointAtRectCenterAndScalesSpan) {
  const GeoPoint point{kLatE7, kLonE7};
  const uint32_t spanMeters = 1000;
  const RouteViewport viewport = RouteViewport::centered(point, kMapRect, spanMeters, kPad);
  ASSERT_TRUE(viewport.isValid());

  EXPECT_EQ(viewport.project(point).x, kMapRect.width / 2);
  EXPECT_EQ(viewport.project(point).y, kMapRect.height / 2);

  // The requested span maps across the shorter (width) inner axis.
  const int innerW = kMapRect.width - 2 * kPad;
  EXPECT_LE(std::abs(viewport.pixelsForMeters(spanMeters) - innerW), 2);
  const int half = viewport.pixelsForMeters(spanMeters / 2);
  EXPECT_LE(std::abs(half - innerW / 2), 2);
  // Doubling the distance doubles the pixels (within rounding).
  EXPECT_LE(std::abs(viewport.pixelsForMeters(2 * spanMeters) - 2 * innerW), 4);
}

TEST(RouteViewportTest, PixelsForMetersIsLinearWithLatitudeScale) {
  const GeoPoint point{kLatE7, kLonE7};
  const RouteViewport viewport = RouteViewport::centered(point, kMapRect, 1000, kPad);
  ASSERT_TRUE(viewport.isValid());
  const int innerW = kMapRect.width - 2 * kPad;  // ~184 px for 1000 m
  // 184 px / 1000 m * 250 m ~= 46 px.
  EXPECT_LE(std::abs(viewport.pixelsForMeters(250) - innerW / 4), 2);
  EXPECT_EQ(viewport.pixelsForMeters(0), 0);
}

// ---------------------------------------------------------------------------
// Saturation of far-outside positions
// ---------------------------------------------------------------------------

TEST(RouteViewportTest, FarOutsidePositionsSaturateWithoutOverflow) {
  // Extremely zoomed-in follow view (1 m across the inner width).
  const RouteViewport viewport = RouteViewport::centered(GeoPoint{0, 0}, kMapRect, 1, 0);
  ASSERT_TRUE(viewport.isValid());

  // Half the world away east and south saturates to the documented bound.
  const ScreenPoint east = viewport.project(GeoPoint{0, 1'800'000'000});
  const ScreenPoint south = viewport.project(GeoPoint{-900'000'000, 0});
  EXPECT_EQ(east.x, RouteViewport::kMaxProjectedPx);
  EXPECT_EQ(east.y, kMapRect.height / 2);
  EXPECT_EQ(south.x, kMapRect.width / 2);
  // South maps DOWN (screen y grows south), so it saturates positive.
  EXPECT_EQ(south.y, RouteViewport::kMaxProjectedPx);

  // A near point is finite, ordered, and far smaller than the saturating one.
  const ScreenPoint nearEast = viewport.project(GeoPoint{0, 1'000});  // ~11 m east
  EXPECT_LT(nearEast.x, east.x);
  EXPECT_GT(nearEast.x, kMapRect.width / 2);

  // Points even further out on the same side stay clamped at the bound.
  const ScreenPoint pole = viewport.project(GeoPoint{900'000'000, 0});
  EXPECT_EQ(pole.y, -RouteViewport::kMaxProjectedPx);
}

// ---------------------------------------------------------------------------
// Invalid inputs
// ---------------------------------------------------------------------------

TEST(RouteViewportTest, InvalidRectsAndInputsYieldInvalidViewport) {
  const GeoPoint point{kLatE7, kLonE7};
  const RouteIndex emptyRoute{};

  EXPECT_FALSE(RouteViewport{}.isValid());
  EXPECT_FALSE(RouteViewport::centered(point, Rect{0, 0, 0, 320}, 1000, kPad).isValid());
  EXPECT_FALSE(RouteViewport::centered(point, Rect{0, 0, 200, 0}, 1000, kPad).isValid());
  EXPECT_FALSE(RouteViewport::centered(point, Rect{0, 0, -200, 320}, 1000, kPad).isValid());
  EXPECT_FALSE(RouteViewport::centered(point, Rect{0, 0, 200, 320}, 0, kPad).isValid());
  // Padding that eats the whole rect.
  EXPECT_FALSE(RouteViewport::centered(point, Rect{0, 0, 200, 320}, 1000, 100).isValid());
  EXPECT_FALSE(RouteViewport::centered(point, Rect{0, 0, 200, 320}, 1000, -1).isValid());
  EXPECT_FALSE(RouteViewport::fitOverview(emptyRoute, kMapRect, kPad).isValid());
  EXPECT_FALSE(RouteViewport::fitOverview(makeIndex(point), Rect{0, 0, 0, 320}, kPad).isValid());
}

TEST(RouteViewportTest, ProjectionOnInvalidViewportIsSafeNoop) {
  const RouteViewport invalid{};
  const ScreenPoint p = invalid.project(GeoPoint{kLatE7, kLonE7});
  EXPECT_EQ(p.x, 0);
  EXPECT_EQ(p.y, 0);
}
