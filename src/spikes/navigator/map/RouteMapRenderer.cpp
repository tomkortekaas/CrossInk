#include "RouteMapRenderer.h"

#include <algorithm>
#include <climits>
#include <cstdint>
#include <cstring>

#include "../route/RoutePackageV1.h"
#include "GrayMap.h"
#include "WalkMapViewport.h"

// Allocation-free detailed polyline renderer.
//
// Wire decoding mirrors the Route Package v1 validator exactly (see
// route/RoutePackageV1.cpp): segment 0 begins at the E5-quantized header
// origin, every later segment at its absolute E7 anchor quantized to E5, and
// interior points are signed Int16 E5 deltas accumulated per segment with
// antimeridian longitude folding. The renderer streams the deltas through one
// 1,024-byte stack buffer; every individual read request is <= 1,024 bytes.
//
// Clipping uses integer Liang-Barsky against the map rect, so no line endpoint
// outside the rect ever reaches the canvas, and edges that lie entirely
// outside are culled. Each segment is drawn as its own polyline: consecutive
// points inside a segment are joined, but the last point of one segment is
// never joined to the first point of the next.

namespace navigator {
namespace {

constexpr int64_t kE5HalfTurn = 18000000;  // +/-180 degrees in E5
constexpr int64_t kE5FullTurn = 36000000;

inline uint16_t readU16LE(const uint8_t* p) {
  return static_cast<uint16_t>(p[0]) | static_cast<uint16_t>(static_cast<uint16_t>(p[1]) << 8);
}

inline int16_t readI16LE(const uint8_t* p) { return static_cast<int16_t>(readU16LE(p)); }

inline uint32_t readU32LE(const uint8_t* p) {
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) | (static_cast<uint32_t>(p[2]) << 16) |
         (static_cast<uint32_t>(p[3]) << 24);
}

inline int32_t readI32LE(const uint8_t* p) { return static_cast<int32_t>(readU32LE(p)); }

// E7 -> E5 quantization: nearest multiple of 100 E7, ties away from zero.
// Mirrors the validator so the renderer reconstructs the exact same points.
inline int64_t quantizeE5(int32_t e7) {
  const int64_t value = e7;
  const int64_t magnitude = value < 0 ? -value : value;
  const int64_t quantized = (magnitude + 50) / 100;
  return value < 0 ? -quantized : quantized;
}

// Folds an accumulated E5 longitude back into [-180, +180] degrees. The
// renderer state is always folded after each point, so the input is within
// +/- (18,000,000 + 32,767) and a single correction step is exact (this is
// the same result the validator's while loops produce for bounded input).
inline int64_t foldLongitudeE5(int64_t e5) {
  if (e5 > kE5HalfTurn) {
    e5 -= kE5FullTurn;
  } else if (e5 < -kE5HalfTurn) {
    e5 += kE5FullTurn;
  }
  return e5;
}

// Folds a longitude difference expressed in E7 units to the shortest arc,
// [-180, +180] degrees. The input is the difference of two values that are
// themselves folded into [-180, +180] degrees, so a single correction step is
// exact.
inline int64_t foldLongitudeDeltaE7(int64_t e7) {
  if (e7 > 1800000000LL) {
    e7 -= 3600000000LL;
  } else if (e7 < -1800000000LL) {
    e7 += 3600000000LL;
  }
  return e7;
}

// readFully is declared in route/RoutePackageV1.h; this file kept an
// identical private copy until the route name needed the same loop.

// Projects one reconstructed E5 point. E5 -> E7 is exact for validated
// geometry; a corrupt source could grow latitude past int32, so the widening
// saturates before the GeoPoint conversion (longitude is folded per point and
// stays bounded).
ScreenPoint projectE5(const RouteViewport& viewport, int64_t latitudeE5, int64_t longitudeE5) {
  const int64_t latE7 = latitudeE5 * 100;
  const int64_t lonE7 = longitudeE5 * 100;
  GeoPoint point;
  point.latitudeE7 = static_cast<int32_t>(
      std::clamp<int64_t>(latE7, static_cast<int64_t>(INT32_MIN), static_cast<int64_t>(INT32_MAX)));
  point.longitudeE7 = static_cast<int32_t>(
      std::clamp<int64_t>(lonE7, static_cast<int64_t>(INT32_MIN), static_cast<int64_t>(INT32_MAX)));
  return viewport.project(point);
}

// Rounds a / b (b > 0) to the nearest integer, half away from zero.
inline int64_t roundDiv(int64_t a, int64_t b) { return a >= 0 ? (a + b / 2) / b : -(((-a) + b / 2) / b); }

// Integer Liang-Barsky clip of segment (ax, ay)-(bx, by) against the inclusive
// rect [xmin, xmax] x [ymin, ymax]. Returns false when the whole segment is
// outside. On success the endpoints are replaced by the clipped endpoints,
// which are clamped into the rect so the canvas never sees an out-of-rect
// coordinate.
bool clipSegment(int xmin, int ymin, int xmax, int ymax, int& ax, int& ay, int& bx, int& by) {
  const int64_t dx = static_cast<int64_t>(bx) - ax;
  const int64_t dy = static_cast<int64_t>(by) - ay;
  const int64_t p[4] = {-dx, dx, -dy, dy};
  const int64_t q[4] = {static_cast<int64_t>(ax) - xmin, static_cast<int64_t>(xmax) - ax,
                        static_cast<int64_t>(ay) - ymin, static_cast<int64_t>(ymax) - ay};

  int64_t t0n = 0;
  int64_t t0d = 1;
  int64_t t1n = 1;
  int64_t t1d = 1;
  for (int i = 0; i < 4; ++i) {
    if (p[i] == 0) {
      if (q[i] < 0) {
        return false;  // parallel to the boundary and outside it
      }
      continue;
    }
    // r = q / p normalized to a positive denominator.
    int64_t rn;
    int64_t rd;
    if (p[i] < 0) {
      rn = -q[i];
      rd = -p[i];
    } else {
      rn = q[i];
      rd = p[i];
    }
    if (p[i] < 0) {
      // Entering boundary: t0 = max(t0, r).
      if (rn * t0d > t0n * rd) {
        t0n = rn;
        t0d = rd;
      }
    } else {
      // Leaving boundary: t1 = min(t1, r).
      if (rn * t1d < t1n * rd) {
        t1n = rn;
        t1d = rd;
      }
    }
  }
  if (t0n * t1d > t1n * t0d) {
    return false;  // t0 > t1: the visible part is empty
  }

  const int64_t x0 = ax;
  const int64_t y0 = ay;
  ax = static_cast<int>(x0 + roundDiv(t0n * dx, t0d));
  ay = static_cast<int>(y0 + roundDiv(t0n * dy, t0d));
  bx = static_cast<int>(x0 + roundDiv(t1n * dx, t1d));
  by = static_cast<int>(y0 + roundDiv(t1n * dy, t1d));

  // Rounding may put an intersection one pixel past a corner; clamp so the
  // canvas contract ("endpoints inside the map rect") always holds.
  ax = std::clamp(ax, xmin, xmax);
  ay = std::clamp(ay, ymin, ymax);
  bx = std::clamp(bx, xmin, xmax);
  by = std::clamp(by, ymin, ymax);
  return true;
}

// Clips one polyline edge and, when anything visible remains, draws it with
// the dominant route pen. Zero-length edges draw nothing.
void drawEdge(RouteCanvas& canvas, int xmin, int ymin, int xmax, int ymax, int ax, int ay, int bx, int by,
              int stroke = RouteMapRenderer::kRouteLineWidthPx) {
  if (!clipSegment(xmin, ymin, xmax, ymax, ax, ay, bx, by)) {
    return;
  }
  if (ax == bx && ay == by) {
    return;
  }
  canvas.line(ax, ay, bx, by, stroke);
}

// Raster-space nearest segment, evaluated before clipping. Fixed-point
// interpolation keeps products below int64 limits at kMaxProjectedPx. When
// this edge is closer than `best`, updates best/nearest and returns true.
bool nearestSegment(const ScreenPoint& marker, const ScreenPoint& a, const ScreenPoint& b, uint64_t& best,
                    ScreenPoint& nearest) {
  const int64_t dx = int64_t(b.x) - a.x, dy = int64_t(b.y) - a.y;
  const int64_t length2 = dx * dx + dy * dy;
  const int64_t dot = (int64_t(marker.x) - a.x) * dx + (int64_t(marker.y) - a.y) * dy;
  const int64_t t = length2 ? std::clamp<int64_t>(dot * 4096 / length2, 0, 4096) : 0;
  const ScreenPoint p{int(a.x + dx * t / 4096), int(a.y + dy * t / 4096)};
  const int64_t x = int64_t(p.x) - marker.x, y = int64_t(p.y) - marker.y;
  const uint64_t d = x * x + y * y;
  if (d < best) {
    best = d;
    nearest = p;
    return true;
  }
  return false;
}
uint64_t integerRoot(uint64_t n) {
  uint64_t root = 0, bit = uint64_t(1) << 62;
  while (bit > n) bit >>= 2;
  while (bit) {
    if (n >= root + bit) {
      n -= root + bit;
      root = (root >> 1) + bit;
    } else
      root >>= 1;
    bit >>= 2;
  }
  return root;
}
void patternedEdge(RouteCanvas& canvas, const Rect& r, ScreenPoint a, ScreenPoint b, int dash, int gap) {
  if (!clipSegment(r.x, r.y, r.x + r.width - 1, r.y + r.height - 1, a.x, a.y, b.x, b.y)) return;
  const int dx = b.x - a.x, dy = b.y - a.y, length = std::max(std::abs(dx), std::abs(dy));
  for (int t = 0; t < length; t += dash + gap) {
    const int end = std::min(t + dash, length);
    canvas.line(a.x + dx * t / length, a.y + dy * t / length, a.x + dx * end / length, a.y + dy * end / length, 1);
  }
}

// ---------------------------------------------------------------------------
// Streamed detailed-geometry walker and the trusted phone-progress route split
// ---------------------------------------------------------------------------
//
// streamDetailedGeometry() decodes the wire geometry exactly like the single
// streaming pass below and calls a visitor once per segment-start vertex (a
// zero-length edge, so a fix exactly at a segment start is measured there)
// and once per consecutive point pair inside a segment. Segments are never
// joined, so the visitor never measures or draws across a recording gap.

struct RouteEdge {
  ScreenPoint a;
  ScreenPoint b;
  int64_t aLatE5 = 0;
  int64_t aLonE5 = 0;
  int64_t bLatE5 = 0;
  int64_t bLonE5 = 0;
  uint64_t lengthU = 0;  // 0 when length measurement is disabled
};

using RouteEdgeVisitor = void (*)(void* opaque, const RouteEdge& edge);

// Streams every segment of the detailed geometry in route order through the
// shared <= 1,024-byte work buffer. When `measureLength` is set, lengthU is
// the edge's ground length in the same U units (E7 latitude, longitude scaled
// by the viewport's fixed Q16 cosine) the along-route estimate uses, so a
// later split boundary can be measured with identical math. Returns false when
// the source truncates before the declared geometry is fully read (ShortRead).
bool streamDetailedGeometry(RouteByteSource& source, const RouteIndex& route, const RouteViewport& viewport,
                            uint8_t* work, bool measureLength, void* opaque, RouteEdgeVisitor visit) {
  const int64_t measureCosQ16 = measureLength ? viewport.cosScaleQ16() : 65536;
  for (uint16_t seg = 0; seg < route.segmentCount; ++seg) {
    const uint32_t startIndex = route.segments[seg].startPointIndex;
    const uint32_t endIndex = seg + 1 < route.segmentCount ? route.segments[seg + 1].startPointIndex : route.pointCount;
    const uint32_t span = endIndex - startIndex;  // >= 1 by the draw() guards
    uint32_t pos = route.segments[seg].sourceOffset;

    int64_t latitudeE5 = 0;
    int64_t longitudeE5 = 0;
    if (seg == 0) {
      latitudeE5 = quantizeE5(route.originLatitudeE7);
      longitudeE5 = quantizeE5(route.originLongitudeE7);
    } else {
      if (!readFully(source, pos, work, 8)) {
        return false;
      }
      pos += 8;
      latitudeE5 = quantizeE5(readI32LE(work));
      longitudeE5 = quantizeE5(readI32LE(work + 4));
    }

    ScreenPoint previous = projectE5(viewport, latitudeE5, longitudeE5);
    int64_t edgeLatitudeE5 = latitudeE5;
    int64_t edgeLongitudeE5 = longitudeE5;
    RouteEdge start;
    start.a = previous;
    start.b = previous;
    start.aLatE5 = latitudeE5;
    start.aLonE5 = longitudeE5;
    start.bLatE5 = latitudeE5;
    start.bLonE5 = longitudeE5;
    visit(opaque, start);

    uint32_t deltaBytes = (span - 1) * 4;
    while (deltaBytes > 0) {
      const uint32_t chunk = std::min(deltaBytes, static_cast<uint32_t>(kRoutePackageV1WorkBufferBytes));
      if (!readFully(source, pos, work, chunk)) {
        return false;
      }
      pos += chunk;
      const uint32_t pairs = chunk / 4;  // chunk is always a multiple of 4
      for (uint32_t i = 0; i < pairs; ++i) {
        const int16_t deltaLatitudeE5 = readI16LE(work + i * 4);
        const int16_t deltaLongitudeE5 = readI16LE(work + i * 4 + 2);
        latitudeE5 += deltaLatitudeE5;
        longitudeE5 = foldLongitudeE5(longitudeE5 + deltaLongitudeE5);

        const ScreenPoint current = projectE5(viewport, latitudeE5, longitudeE5);
        RouteEdge edge;
        edge.a = previous;
        edge.b = current;
        edge.aLatE5 = edgeLatitudeE5;
        edge.aLonE5 = edgeLongitudeE5;
        edge.bLatE5 = latitudeE5;
        edge.bLonE5 = longitudeE5;
        if (measureLength) {
          const int64_t dLatE7 = (latitudeE5 - edgeLatitudeE5) * 100;
          const int64_t dLonE7 = foldLongitudeE5(longitudeE5 - edgeLongitudeE5) * 100;
          const int64_t dxU = dLatE7;
          const int64_t dyU = roundDiv(dLonE7 * measureCosQ16, 65536);
          edge.lengthU = integerRoot(uint64_t(dxU * dxU + dyU * dyU));
        }
        visit(opaque, edge);
        previous = current;
        edgeLatitudeE5 = latitudeE5;
        edgeLongitudeE5 = longitudeE5;
      }
      deltaBytes -= chunk;
    }
  }
  return true;
}

// Measure-only visitor: sums the whole route's measured length so the split
// boundary (a share of the declared total) can be placed on the geometry.
struct MeasureUnitsState {
  uint64_t totalUnits = 0;
};

void accumulateRouteUnits(void* opaque, const RouteEdge& edge) {
  auto& state = *static_cast<MeasureUnitsState*>(opaque);
  state.totalUnits += edge.lengthU;
}

// Per-edge state threaded through the drawing pass: the along-route
// measurement accumulators (proximity) and the trusted-progress split
// boundary, both driven strictly in route order.
struct DrawRouteState {
  RouteCanvas* canvas = nullptr;
  int xmin = 0;
  int ymin = 0;
  int xmax = 0;
  int ymax = 0;
  const CurrentPosition* position = nullptr;
  RouteProximity* proximity = nullptr;
  ScreenPoint positionPixel;
  uint64_t routeUnits = 0;       // measured length of every edge so far
  uint64_t bestBeforeUnits = 0;  // routeUnits when the closest edge began
  uint64_t bestEdgeUnits = 0;    // measured length of the closest edge
  uint32_t bestT4096 = 0;        // clamped projection parameter on that edge
  uint64_t nearestDistance = UINT64_MAX;
  ScreenPoint nearest;
  int64_t measureCosQ16 = 65536;
  bool splitActive = false;
  uint64_t splitWalkedUnits = 0;  // measured units already walked
};

// Draws one streamed edge. The un-walked part keeps the dominant route pen;
// a trusted fix's phone-tracked progress turns the already-walked prefix into
// the thin dashed stroke. Zero-length vertex edges can still become the
// nearest measured edge but draw nothing.
void drawRouteEdge(void* opaque, const RouteEdge& edge) {
  auto& state = *static_cast<DrawRouteState*>(opaque);
  const uint64_t lengthU = edge.lengthU;

  if (state.position != nullptr && state.proximity != nullptr) {
    // Projection parameter along this edge measured in the same U space as
    // the length sums, so a fix between two route points lands exactly where
    // it is on the ground rather than where a pixel-grid projection happened
    // to put it. The clamped branch order keeps every product inside int64:
    // when 0 < dot < length2 (the only case that needs precision), dot is
    // below length2 (~2.1e13), so dot * 4096 cannot overflow.
    if (nearestSegment(state.positionPixel, edge.a, edge.b, state.nearestDistance, state.nearest)) {
      const int64_t dLatE7 = (edge.bLatE5 - edge.aLatE5) * 100;
      const int64_t dLonE7 = foldLongitudeE5(edge.bLonE5 - edge.aLonE5) * 100;
      const int64_t dxU = dLatE7;
      const int64_t dyU = roundDiv(dLonE7 * state.measureCosQ16, 65536);
      const int64_t mdxU = int64_t(state.position->point.latitudeE7) - edge.aLatE5 * 100;
      const int64_t mdyU = roundDiv(
          foldLongitudeDeltaE7(int64_t(state.position->point.longitudeE7) - edge.aLonE5 * 100) *
              state.measureCosQ16,
          65536);
      const int64_t length2 = dxU * dxU + dyU * dyU;
      const int64_t dot = mdxU * dxU + mdyU * dyU;
      int64_t t = 0;
      if (length2 > 0) {
        if (dot <= 0) {
          t = 0;
        } else if (dot >= length2) {
          t = 4096;
        } else {
          t = dot * 4096 / length2;
        }
      }
      state.bestBeforeUnits = state.routeUnits;
      state.bestEdgeUnits = lengthU;
      state.bestT4096 = static_cast<uint32_t>(t);
    }
  }

  if (state.splitActive) {
    if (lengthU > 0) {
      const uint64_t begin = state.routeUnits;
      const uint64_t end = begin + lengthU;
      const Rect rect{state.xmin, state.ymin, state.xmax - state.xmin + 1, state.ymax - state.ymin + 1};
      if (state.splitWalkedUnits <= begin) {
        drawEdge(*state.canvas, state.xmin, state.ymin, state.xmax, state.ymax, edge.a.x, edge.a.y, edge.b.x, edge.b.y);
      } else if (state.splitWalkedUnits >= end) {
        patternedEdge(*state.canvas, rect, edge.a, edge.b, RouteMapRenderer::kRouteWalkedDashPx,
                      RouteMapRenderer::kRouteWalkedGapPx);
      } else {
        // Split inside this edge: the walked prefix is dashed and the remaining
        // suffix stays continuous with the dominant pen. The split parameter is
        // measured in U units; the projection is affine along the edge, so the
        // same parameter splits the drawn screen segment.
        const int64_t t = int64_t(((state.splitWalkedUnits - begin) * 65536) / lengthU);  // in (0, 65536)
        const ScreenPoint p{edge.a.x + int(roundDiv(int64_t(edge.b.x - edge.a.x) * t, 65536)),
                            edge.a.y + int(roundDiv(int64_t(edge.b.y - edge.a.y) * t, 65536))};
        patternedEdge(*state.canvas, rect, edge.a, p, RouteMapRenderer::kRouteWalkedDashPx,
                      RouteMapRenderer::kRouteWalkedGapPx);
        drawEdge(*state.canvas, state.xmin, state.ymin, state.xmax, state.ymax, p.x, p.y, edge.b.x, edge.b.y);
      }
      state.routeUnits = end;
    }
    return;
  }
  // No split: draw every edge with the dominant pen (zero-length pixel edges
  // draw nothing) exactly as the historical single-pass renderer did.
  drawEdge(*state.canvas, state.xmin, state.ymin, state.xmax, state.ymax, edge.a.x, edge.a.y, edge.b.x, edge.b.y);
  state.routeUnits += lengthU;
}

}  // namespace

RenderStatus RouteMapRenderer::draw(RouteCanvas& canvas, RouteByteSource& source, const RouteIndex& route,
                                    const RouteViewport& viewport, const CurrentPosition* position,
                                    WalkMapLayer* background, RouteProximity* proximity, GrayMapLayer* gray) {
  if (proximity) *proximity = {};
  if (!viewport.isValid()) {
    return RenderStatus::InvalidViewport;
  }
  // Index guards mirror the decoder's invariants: bounded counts and segment
  // start indices that begin at 0 and strictly increase below pointCount.
  // Everything is validated before the canvas is touched.
  if (route.pointCount == 0 || route.segmentCount == 0 || route.segmentCount > route.pointCount ||
      route.pointCount > RouteIndex::kMaxPoints || route.segmentCount > RouteIndex::kMaxSegments) {
    return RenderStatus::InvalidIndex;
  }
  if (route.segments[0].startPointIndex != 0) {
    return RenderStatus::InvalidIndex;
  }
  for (uint16_t s = 1; s < route.segmentCount; ++s) {
    if (route.segments[s].startPointIndex <= route.segments[s - 1].startPointIndex ||
        route.segments[s].startPointIndex >= route.pointCount) {
      return RenderStatus::InvalidIndex;
    }
  }

  const ScreenPoint positionPixel = position ? viewport.project(position->point) : ScreenPoint{};
  canvas.clear();
  if (gray) {
    gray->status =
        gray->source && gray->map ? gray->map->draw(*gray->source, viewport, canvas) : WalkMapStatus::NotOpen;
    if (gray->status != WalkMapStatus::Ok) canvas.clear();
  }
  if (background && (!gray || gray->status != WalkMapStatus::Ok)) {
    background->status = WalkMapStatus::NotOpen;
    background->edgeCount = 0;
    background->labelCount = 0;
    GeoBounds bounds{};
    if (background->source && background->map && background->map->valid() && walkMapBounds(viewport, bounds)) {
      struct Context {
        RouteCanvas& canvas;
        const RouteViewport& view;
        WalkMapLayer& layer;
        uint32_t count;
      } context{canvas, viewport, *background, 0};
      const auto drawBackgroundEdge = [](void* opaque, const WalkMapEdge& edge) {
        auto& c = *static_cast<Context*>(opaque);
        const auto a = c.view.project(GeoPoint{edge.latitude1E7, edge.longitude1E7});
        const auto b = c.view.project(GeoPoint{edge.latitude2E7, edge.longitude2E7});
        const auto r = c.view.mapRect();
        if (edge.kind == 7) {
          if (a.x < r.x + 10 || a.y < r.y + 16 || a.x >= r.x + r.width - 10 || a.y >= r.y + r.height - 40) return;
          for (uint8_t i = 0; i < c.layer.labelCount; ++i)
            if (std::strcmp(c.layer.labels[i].text, edge.label) == 0) return;
          if (c.layer.labelCount < 32) {
            auto& label = c.layer.labels[c.layer.labelCount++];
            label.point = {edge.latitude1E7, edge.longitude1E7};
            std::memcpy(label.text, edge.label, 45);
          }
        } else if (edge.kind == 1)
          patternedEdge(c.canvas, r, a, b, 6, 5);
        else if (edge.kind == 5)
          patternedEdge(c.canvas, r, a, b, 2, 5);
        else
          drawEdge(c.canvas, r.x, r.y, r.x + r.width - 1, r.y + r.height - 1, a.x, a.y, b.x, b.y,
                   edge.kind == 3 ? 2 : 1);
        ++c.count;
      };
      background->status = background->map->visit(*background->source, bounds, drawBackgroundEdge, &context);
      if (background->status == WalkMapStatus::Ok) background->edgeCount = context.count;
    }
    // A later cell/read can fail after earlier lines were drawn. Always erase
    // the incomplete basemap before rendering the authoritative GPX alone.
    if (background->status != WalkMapStatus::Ok)
      canvas.clear();
    else
      for (uint8_t i = 0; i < background->labelCount; ++i) {
        const auto& label = background->labels[i];
        const auto p = viewport.project(label.point);
        canvas.label(p.x, p.y, label.text);
      }
  }

  const Rect& rect = viewport.mapRect();
  const int xmin = rect.x;
  const int ymin = rect.y;
  const int xmax = rect.x + rect.width - 1;
  const int ymax = rect.y + rect.height - 1;

  uint8_t work[kRoutePackageV1WorkBufferBytes];

  // A live v3 fix carries trusted phone-tracked progress from the route
  // start. When it does (and the route declares a positive total), the drawn
  // route is split at that progress in strict route order: the already-walked
  // prefix becomes the thin dashed stroke and the remaining route keeps the
  // dominant pen. The split share of the declared total is placed on the
  // *measured* geometry (routeUnits), so it agrees with the along-route
  // remaining estimate; a second streaming pass sums that geometry first.
  // Everything stays fixed-width integer math: no heap, no recursion.
  const bool splitActive =
      position != nullptr && position->hasRouteProgress && route.totalDistanceMeters > 0;
  uint64_t splitWalkedUnits = 0;
  if (splitActive) {
    MeasureUnitsState measure;
    if (!streamDetailedGeometry(source, route, viewport, work, true, &measure, &accumulateRouteUnits)) {
      return RenderStatus::ShortRead;  // the source cannot describe the whole route
    }
    const uint32_t walkedMeters = std::min(position->distanceFromStartMeters, route.totalDistanceMeters);
    const uint64_t walkedQ16 = (static_cast<uint64_t>(walkedMeters) << 16) / route.totalDistanceMeters;
    splitWalkedUnits = (measure.totalUnits * walkedQ16) >> 16;
  }

  // One streamed drawing pass (plus the measure pass above when a split is
  // active). Length is accumulated in U units only when the route must be
  // measured (a fix with an output proximity, or an active split), so the
  // legacy no-fix path keeps its single read pass untouched.
  DrawRouteState state;
  state.canvas = &canvas;
  state.xmin = xmin;
  state.ymin = ymin;
  state.xmax = xmax;
  state.ymax = ymax;
  state.position = position;
  state.proximity = proximity;
  state.positionPixel = positionPixel;
  const bool measureLength = splitActive || (position != nullptr && proximity != nullptr);
  state.measureCosQ16 = measureLength ? viewport.cosScaleQ16() : 65536;
  state.splitActive = splitActive;
  state.splitWalkedUnits = splitWalkedUnits;
  if (!streamDetailedGeometry(source, route, viewport, work, measureLength, &state, &drawRouteEdge)) {
    return RenderStatus::ShortRead;
  }

  if (position != nullptr && proximity != nullptr && state.nearestDistance != UINT64_MAX) {
    const int pixelsPerKm = viewport.pixelsForMeters(1000);
    if (pixelsPerKm > 0) {
      proximity->valid = true;
      proximity->distanceMeters = uint32_t(integerRoot(state.nearestDistance) * 1000 / uint32_t(pixelsPerKm));
      proximity->dx = state.nearest.x - positionPixel.x;
      proximity->dy = state.nearest.y - positionPixel.y;
    }
    // Distance still to walk to the end of the route, measured along the
    // route itself from the projection of the fix onto its closest edge and
    // scaled to the package's declared total. routeUnits > 0 because the
    // closest edge was found, so both divisions below are safe.
    if (state.routeUnits == 0) {
      proximity->remainingDistanceMeters = route.totalDistanceMeters;
    } else {
      const uint64_t closestUnits =
          state.bestBeforeUnits +
          static_cast<uint64_t>(roundDiv(int64_t(state.bestEdgeUnits) * int64_t(state.bestT4096), 4096));
      const uint64_t aheadUnits = closestUnits <= state.routeUnits ? state.routeUnits - closestUnits : 0;
      // Q16 share of the route still ahead (65536 == the whole route), half-up.
      const uint64_t aheadQ16 = (2 * aheadUnits * 65536 + state.routeUnits) / (2 * state.routeUnits);
      const uint64_t fraction = aheadQ16 <= 65536 ? aheadQ16 : 65536;
      const uint64_t remaining = (uint64_t(route.totalDistanceMeters) * fraction + 32768) / 65536;
      proximity->remainingDistanceMeters = static_cast<uint32_t>(remaining);
    }
    // A short direction arrow only: never a fabricated traversable connection.
    if (proximity->valid && proximity->distanceMeters > std::max<uint32_t>(40, 2u * position->accuracyMeters)) {
      const int size = std::max(std::abs(proximity->dx), std::abs(proximity->dy));
      if (size) {
        const int dx = proximity->dx * 72 / size, dy = proximity->dy * 72 / size;
        const int x = positionPixel.x + dx, y = positionPixel.y + dy;
        drawEdge(canvas, xmin, ymin, xmax, ymax, positionPixel.x, positionPixel.y, x, y, 2);
        drawEdge(canvas, xmin, ymin, xmax, ymax, x, y, x - dx / 4 - dy / 8, y - dy / 4 + dx / 8, 2);
        drawEdge(canvas, xmin, ymin, xmax, ymax, x, y, x - dx / 4 + dy / 8, y - dy / 4 - dx / 8, 2);
      }
    }
  }

  // Current-position marker, drawn last over the whole route. The renderer
  // clips center and radius to the map rect (the canvas has no clip rect of
  // its own for circles): a center outside the rect draws nothing, and a
  // radius that would cross the border is shrunk to fit.
  if (position != nullptr) {
    const ScreenPoint marker = viewport.project(position->point);
    if (marker.x >= xmin && marker.x <= xmax && marker.y >= ymin && marker.y <= ymax) {
      const int fit = std::min(std::min(marker.x - xmin, xmax - marker.x), std::min(marker.y - ymin, ymax - marker.y));
      if (fit >= 1) {
        int ringRadius = viewport.pixelsForMeters(position->accuracyMeters);
        ringRadius = std::clamp(ringRadius, kMarkerMinRingRadiusPx, kMarkerMaxRingRadiusPx);
        ringRadius = std::min(ringRadius, fit);
        if (ringRadius >= 1) {
          canvas.ring(marker.x, marker.y, ringRadius, kMarkerRingWidthPx);
        }
        const int discRadius = std::min(kMarkerDiscRadiusPx, fit);
        if (discRadius >= 1) {
          canvas.disc(marker.x, marker.y, discRadius);
        }
      }
    }
  }
  return RenderStatus::Ok;
}

}  // namespace navigator
