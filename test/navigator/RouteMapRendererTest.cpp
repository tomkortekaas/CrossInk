// Host tests for the allocation-free detailed polyline renderer
// (src/spikes/navigator/map/RouteMapRenderer.h/.cpp) and the driver-
// independent RouteCanvas it draws through.
//
// Task 5 - strict TDD: these tests were written first, before any production
// code existed. The renderer under test must:
//   * clear the canvas first and exactly once;
//   * stream the *detailed* wire geometry segment by segment through
//     RouteByteSource with every single read request <= 1,024 bytes, decoding
//     absolute E7 anchors plus E5 deltas exactly like the Route Package v1
//     validator (it must not draw the bounded overview instead);
//   * draw each segment as its own polyline, never joining segment boundaries;
//   * clip every line to the map rect so the canvas never receives an
//     out-of-rect endpoint, and cull edges that lie entirely outside;
//   * draw the current-position marker ring/disc last and clip marker
//     center/radius too; draw nothing when no position is supplied;
//   * return a typed RenderStatus and fail safely (no out-of-bounds canvas
//     access, no crash) on short reads, malformed indexes and invalid
//     viewports;
//   * keep the GPX route pen the dominant stroke.

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <utility>
#include <vector>

#include "map/RouteMapRenderer.h"
#include "map/RouteViewport.h"
#include "route/RoutePackageV1.h"

namespace {

using Bytes = std::vector<uint8_t>;
using navigator::CurrentPosition;
using navigator::DecodeStatus;
using navigator::GeoPoint;
using navigator::Rect;
using navigator::RenderStatus;
using navigator::RouteCanvas;
using navigator::RouteIndex;
using navigator::RouteMapRenderer;
using navigator::RouteProximity;
using navigator::RouteViewport;
using navigator::ScreenPoint;

// ---------------------------------------------------------------------------
// Little-endian wire helpers and a test-side CRC-32 mirroring the Swift
// encoder, plus a test-side Route Package v1 encoder for synthetic routes.
// ---------------------------------------------------------------------------

uint32_t crc32Of(const Bytes& bytes) {
  uint32_t crc = 0xFFFFFFFFu;
  for (uint8_t byte : bytes) {
    crc ^= byte;
    for (int i = 0; i < 8; ++i) {
      const uint32_t mask = 0u - (crc & 1u);
      crc = (crc >> 1) ^ (0xEDB88320u & mask);
    }
  }
  return crc ^ 0xFFFFFFFFu;
}

void putU16(Bytes& b, size_t at, uint16_t v) {
  b[at] = static_cast<uint8_t>(v & 0xFFu);
  b[at + 1] = static_cast<uint8_t>((v >> 8) & 0xFFu);
}

void putU32(Bytes& b, size_t at, uint32_t v) {
  b[at] = static_cast<uint8_t>(v & 0xFFu);
  b[at + 1] = static_cast<uint8_t>((v >> 8) & 0xFFu);
  b[at + 2] = static_cast<uint8_t>((v >> 16) & 0xFFu);
  b[at + 3] = static_cast<uint8_t>((v >> 24) & 0xFFu);
}

void putI32(Bytes& b, size_t at, int32_t v) { putU32(b, at, static_cast<uint32_t>(v)); }

void appendU16(Bytes& bytes, uint16_t value) {
  bytes.push_back(static_cast<uint8_t>(value & 0xFFu));
  bytes.push_back(static_cast<uint8_t>((value >> 8) & 0xFFu));
}

void appendU32(Bytes& bytes, uint32_t value) {
  for (unsigned shift = 0; shift < 32; shift += 8) {
    bytes.push_back(static_cast<uint8_t>((value >> shift) & 0xFFu));
  }
}

void appendI32(Bytes& bytes, int32_t value) { appendU32(bytes, static_cast<uint32_t>(value)); }

int64_t quantizeE5(int32_t e7) {
  const int64_t v = e7;
  const int64_t magnitude = v < 0 ? -v : v;
  const int64_t quantized = (magnitude + 50) / 100;
  return v < 0 ? -quantized : quantized;
}

// Encodes a valid Route Package v1 from E7 points and per-segment start
// indices, mirroring the authoritative Swift encoder used for the golden
// fixture (segment 0 starts at the header origin; later segments restart with
// an absolute E7 anchor; interior points are signed Int16 E5 deltas). The
// optional declared totals fill the header fields the along-route progress
// tests scale against; the existing callers keep the historical 0/0 header.
Bytes encodeRoute(const std::vector<GeoPoint>& points, const std::vector<uint16_t>& segmentStarts,
                  uint32_t totalMeters = 0, uint16_t estimatedMinutes = 0) {
  if (points.empty() || segmentStarts.empty() || segmentStarts.front() != 0) {
    ADD_FAILURE() << "encodeRoute requires non-empty points starting at 0";
    return Bytes();
  }
  Bytes payload;
  for (uint16_t start : segmentStarts) {
    appendU16(payload, start);
  }
  const auto segmentEnd = [&](size_t index) {
    return index + 1 < segmentStarts.size() ? static_cast<size_t>(segmentStarts[index + 1]) : points.size();
  };
  for (size_t segment = 0; segment < segmentStarts.size(); ++segment) {
    const size_t spanStart = segmentStarts[segment];
    const size_t spanEnd = segmentEnd(segment);
    if (segment > 0) {
      appendI32(payload, points[spanStart].latitudeE7);
      appendI32(payload, points[spanStart].longitudeE7);
    }
    for (size_t i = spanStart + 1; i < spanEnd; ++i) {
      const int64_t latDelta = quantizeE5(points[i].latitudeE7) - quantizeE5(points[i - 1].latitudeE7);
      int64_t lonDelta = quantizeE5(points[i].longitudeE7) - quantizeE5(points[i - 1].longitudeE7);
      lonDelta %= 36'000'000;
      if (lonDelta > 18'000'000) {
        lonDelta -= 36'000'000;
      } else if (lonDelta < -18'000'000) {
        lonDelta += 36'000'000;
      }
      if (latDelta < INT16_MIN || latDelta > INT16_MAX || lonDelta < INT16_MIN || lonDelta > INT16_MAX) {
        ADD_FAILURE() << "encodeRoute delta out of Int16 range";
        return Bytes();
      }
      appendU16(payload, static_cast<uint16_t>(static_cast<int16_t>(latDelta)));
      appendU16(payload, static_cast<uint16_t>(static_cast<int16_t>(lonDelta)));
    }
  }

  const uint32_t total = 38U + static_cast<uint32_t>(payload.size()) + 4U;
  Bytes bytes(total, 0);
  bytes[0] = 'X';
  bytes[1] = '3';
  bytes[2] = 'R';
  bytes[3] = 'T';
  bytes[4] = 1;
  bytes[5] = 0;
  putU16(bytes, 6, 38);
  putU32(bytes, 8, total);
  putU32(bytes, 12, 0x05060708U);
  putU16(bytes, 16, static_cast<uint16_t>(points.size()));
  putU16(bytes, 18, 0);
  putI32(bytes, 20, points.front().latitudeE7);
  putI32(bytes, 24, points.front().longitudeE7);
  putU32(bytes, 28, totalMeters);
  putU16(bytes, 32, estimatedMinutes);
  putU16(bytes, 34, static_cast<uint16_t>(segmentStarts.size()));
  bytes[36] = 0;
  bytes[37] = 0;
  std::copy(payload.begin(), payload.end(), bytes.begin() + 38);
  putU32(bytes, total - 4, crc32Of(Bytes(bytes.begin(), bytes.begin() + (total - 4))));
  return bytes;
}

// ---------------------------------------------------------------------------
// Golden fixture loading (same resolution strategy as RoutePackageV1Test).
// ---------------------------------------------------------------------------

bool readWholeFile(const std::string& path, Bytes& out) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    return false;
  }
  out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
  return !in.bad();
}

Bytes loadFixtureBytes(const char* name) {
  std::vector<std::string> candidates;
  const std::string self = __FILE__;
  const size_t marker = self.find("/test/navigator/");
  if (marker != std::string::npos) {
    candidates.push_back(self.substr(0, marker) + "/test/fixtures/" + name);
  }
  candidates.push_back("test/fixtures/" + std::string(name));
  candidates.push_back("../../../test/fixtures/" + std::string(name));
  candidates.push_back("../../test/fixtures/" + std::string(name));
  for (const std::string& candidate : candidates) {
    Bytes bytes;
    if (readWholeFile(candidate, bytes)) {
      return bytes;
    }
  }
  ADD_FAILURE() << "could not open fixture " << name << "; tried:\n";
  for (const std::string& candidate : candidates) {
    std::cerr << "  " << candidate << "\n";
  }
  return Bytes();
}

// ---------------------------------------------------------------------------
// Host RouteByteSource implementations.
// ---------------------------------------------------------------------------

class VectorRouteByteSource : public navigator::RouteByteSource {
 public:
  explicit VectorRouteByteSource(Bytes bytes) : bytes_(std::move(bytes)) {}

  uint32_t size() const override { return static_cast<uint32_t>(bytes_.size()); }

  uint32_t read(uint32_t offset, uint8_t* destination, uint32_t length) override {
    if (offset >= bytes_.size() || length == 0) {
      return 0;
    }
    const uint32_t n = std::min(length, static_cast<uint32_t>(bytes_.size()) - offset);
    std::memcpy(destination, bytes_.data() + offset, n);
    return n;
  }

 private:
  Bytes bytes_;
};

// Records the largest single read request and the number of reads the
// renderer issued, so tests can assert the <= 1,024-byte work-buffer rule.
class TrackingSource : public navigator::RouteByteSource {
 public:
  explicit TrackingSource(Bytes bytes) : inner_(std::move(bytes)) {}

  uint32_t size() const override { return inner_.size(); }

  uint32_t read(uint32_t offset, uint8_t* destination, uint32_t length) override {
    ++readCalls;
    maxRequested = std::max(maxRequested, length);
    return inner_.read(offset, destination, length);
  }

  uint32_t maxRequested = 0;
  uint32_t readCalls = 0;

 private:
  VectorRouteByteSource inner_;
};

DecodeStatus decode(const Bytes& bytes, RouteIndex& out) {
  VectorRouteByteSource source(bytes);
  return navigator::validateRoutePackageV1(source, out);
}

// ---------------------------------------------------------------------------
// Recording canvas: a RouteCanvas that records every primitive call and
// simultaneously asserts the "guard range" contract (nothing outside the map
// rect may ever reach the canvas).
// ---------------------------------------------------------------------------

class RecordingCanvas : public RouteCanvas {
 public:
  enum class Kind : uint8_t { Clear, Line, Disc, Ring };

  struct Line {
    int x0;
    int y0;
    int x1;
    int y1;
    int width;
  };

  struct Circle {
    int centerX;
    int centerY;
    int radius;
    int width;  // 0 for a filled disc
  };

  RecordingCanvas(int width, int height) : width_(width), height_(height) {
    EXPECT_GT(width_, 0);
    EXPECT_GT(height_, 0);
  }

  void clear() override {
    kinds_.push_back(Kind::Clear);
    ++clearCalls_;
  }

  void line(int x0, int y0, int x1, int y1, int widthPx) override {
    EXPECT_GE(x0, 0) << "line endpoint left of canvas";
    EXPECT_GE(y0, 0) << "line endpoint above canvas";
    EXPECT_LT(x0, width_) << "line endpoint right of canvas";
    EXPECT_LT(y0, height_) << "line endpoint below canvas";
    EXPECT_GE(x1, 0) << "line endpoint left of canvas";
    EXPECT_GE(y1, 0) << "line endpoint above canvas";
    EXPECT_LT(x1, width_) << "line endpoint right of canvas";
    EXPECT_LT(y1, height_) << "line endpoint below canvas";
    EXPECT_GE(widthPx, 1);
    kinds_.push_back(Kind::Line);
    lines_.push_back(Line{x0, y0, x1, y1, widthPx});
  }

  void disc(int centerX, int centerY, int radiusPx) override {
    EXPECT_GE(centerX - radiusPx, 0) << "disc crosses the left canvas edge";
    EXPECT_GE(centerY - radiusPx, 0) << "disc crosses the top canvas edge";
    EXPECT_LT(centerX + radiusPx, width_) << "disc crosses the right canvas edge";
    EXPECT_LT(centerY + radiusPx, height_) << "disc crosses the bottom canvas edge";
    EXPECT_GE(radiusPx, 0);
    kinds_.push_back(Kind::Disc);
    circles_.push_back(Circle{centerX, centerY, radiusPx, 0});
  }

  void ring(int centerX, int centerY, int radiusPx, int widthPx) override {
    EXPECT_GE(centerX - radiusPx, 0) << "ring crosses the left canvas edge";
    EXPECT_GE(centerY - radiusPx, 0) << "ring crosses the top canvas edge";
    EXPECT_LT(centerX + radiusPx, width_) << "ring crosses the right canvas edge";
    EXPECT_LT(centerY + radiusPx, height_) << "ring crosses the bottom canvas edge";
    EXPECT_GE(radiusPx, 1);
    EXPECT_GE(widthPx, 1);
    kinds_.push_back(Kind::Ring);
    circles_.push_back(Circle{centerX, centerY, radiusPx, widthPx});
  }

  int width() const { return width_; }
  int height() const { return height_; }
  size_t callCount() const { return kinds_.size(); }
  size_t clearCount() const { return clearCalls_; }
  size_t lineCount() const { return lines_.size(); }
  size_t discCount() const { return static_cast<size_t>(std::count(kinds_.begin(), kinds_.end(), Kind::Disc)); }
  size_t ringCount() const { return static_cast<size_t>(std::count(kinds_.begin(), kinds_.end(), Kind::Ring)); }
  const std::vector<Kind>& kinds() const { return kinds_; }
  const std::vector<Line>& lines() const { return lines_; }
  const std::vector<Circle>& circles() const { return circles_; }

 private:
  int width_;
  int height_;
  std::vector<Kind> kinds_;
  std::vector<Line> lines_;
  std::vector<Circle> circles_;
  size_t clearCalls_ = 0;
};

// Convenience wrapper: draw through a locally owned (non-tracking) source.
RenderStatus drawBytes(RecordingCanvas& canvas, const Bytes& bytes, const RouteIndex& index,
                       const RouteViewport& viewport, const CurrentPosition* position) {
  TrackingSource source(bytes);
  return RouteMapRenderer::draw(canvas, source, index, viewport, position);
}

// ---------------------------------------------------------------------------
// Small route fixtures
// ---------------------------------------------------------------------------

// A local two-segment route: segment 0 runs A0->A1->A2, segment 1 restarts at
// the absolute anchor B0 and runs B0->B1->B2. Every coordinate is an exact
// multiple of 100 E7 so the E5 wire quantization is lossless and the decoded
// geometry equals the input exactly.
struct TwoSegmentRoute {
  Bytes bytes;
  RouteIndex index{};
  std::vector<GeoPoint> points;
};

TwoSegmentRoute makeTwoSegmentRoute() {
  TwoSegmentRoute route;
  route.points = {
      GeoPoint{523'676'000, 49'041'000},  // A0
      GeoPoint{523'696'000, 49'061'000},  // A1
      GeoPoint{523'716'000, 49'081'000},  // A2
      GeoPoint{523'736'000, 49'101'000},  // B0 (absolute anchor)
      GeoPoint{523'756'000, 49'121'000},  // B1
      GeoPoint{523'776'000, 49'141'000},  // B2
  };
  route.bytes = encodeRoute(route.points, {0, 3});
  EXPECT_FALSE(route.bytes.empty());
  EXPECT_EQ(decode(route.bytes, route.index), DecodeStatus::Ok);
  return route;
}

// Centered follow view that shows the whole two-segment route (route ~667 m
// tall, viewport spans 3000 m across the inner width of a 300x400 map).
RouteViewport viewFor(const std::vector<GeoPoint>& points, const Rect& mapRect) {
  int64_t sumLat = 0;
  int64_t sumLon = 0;
  for (const GeoPoint& p : points) {
    sumLat += p.latitudeE7;
    sumLon += p.longitudeE7;
  }
  const GeoPoint middle{static_cast<int32_t>(sumLat / static_cast<int64_t>(points.size())),
                        static_cast<int32_t>(sumLon / static_cast<int64_t>(points.size()))};
  return RouteViewport::centered(middle, mapRect, 3000, 8);
}

// Draws a synthetic route through the real production renderer with a live
// fix and returns the RouteProximity it fills, so tests exercise the exact
// along-route progress computation (measurement + declared-total scaling)
// rather than a test-side copy of it. A walking-scale viewport centered on
// the route origin keeps the geometry inside the 600x600 map.
struct ProximityOutcome {
  RenderStatus status;
  RouteProximity proximity;
};

ProximityOutcome drawWithFix(const Bytes& bytes, const RouteIndex& index, const GeoPoint& fix,
                             uint16_t accuracyMeters = 5) {
  ProximityOutcome outcome;
  const Rect mapRect{0, 0, 600, 600};
  const GeoPoint origin{index.originLatitudeE7, index.originLongitudeE7};
  const RouteViewport viewport = RouteViewport::centered(origin, mapRect, 6000, 8);
  if (!viewport.isValid()) {
    outcome.status = RenderStatus::InvalidViewport;
    return outcome;
  }
  RecordingCanvas canvas(mapRect.width, mapRect.height);
  TrackingSource source(bytes);
  const CurrentPosition position{fix, accuracyMeters, 0, false, 0};
  outcome.status = RouteMapRenderer::draw(canvas, source, index, viewport, &position, nullptr, &outcome.proximity);
  return outcome;
}

}  // namespace

// ---------------------------------------------------------------------------
// Golden fixture
// ---------------------------------------------------------------------------

TEST(RouteMapRendererTest, GoldenFixtureDrawsIndependentSegmentsWithBoundedReads) {
  const Bytes golden = loadFixtureBytes("route_package_v1.bin");
  ASSERT_FALSE(golden.empty());
  RouteIndex index;
  ASSERT_EQ(decode(golden, index), DecodeStatus::Ok);
  ASSERT_EQ(index.segmentCount, 2U);

  const Rect mapRect{0, 0, 300, 400};
  const RouteViewport viewport = RouteViewport::fitOverview(index, mapRect, 10);
  ASSERT_TRUE(viewport.isValid());

  RecordingCanvas canvas(mapRect.width, mapRect.height);
  TrackingSource source(golden);
  const RenderStatus status = RouteMapRenderer::draw(canvas, source, index, viewport, nullptr);
  EXPECT_EQ(status, RenderStatus::Ok);

  // Clear happens exactly once and is the very first primitive.
  EXPECT_EQ(canvas.clearCount(), 1U);
  ASSERT_GE(canvas.callCount(), 1U);
  EXPECT_EQ(canvas.kinds().front(), RecordingCanvas::Kind::Clear);
  // No position supplied -> no marker primitives; only route lines remain.
  EXPECT_EQ(canvas.discCount(), 0U);
  EXPECT_EQ(canvas.ringCount(), 0U);
  // Segment 0 has 3 points (2 edges), segment 1 has 2 points (1 edge); the
  // renderer may drop edges whose endpoints collapse to one pixel at this
  // world scale, but it can never draw more than one line per consecutive
  // pair and never a line across the segment boundary.
  EXPECT_LE(canvas.lineCount(), 3U);
  EXPECT_GE(canvas.lineCount(), 0U);
  for (const auto& line : canvas.lines()) {
    EXPECT_EQ(line.width, RouteMapRenderer::kRouteLineWidthPx);
  }
  // The renderer read the wire geometry in bounded requests.
  EXPECT_GT(source.readCalls, 0U);
  EXPECT_LE(source.maxRequested, navigator::kRoutePackageV1WorkBufferBytes);
}

// ---------------------------------------------------------------------------
// Exact segment counts / no cross-segment joins
// ---------------------------------------------------------------------------

TEST(RouteMapRendererTest, TwoSegmentsDrawExactIndependentLinesNeverJoined) {
  const TwoSegmentRoute route = makeTwoSegmentRoute();
  const Rect mapRect{0, 0, 300, 400};
  const RouteViewport viewport = viewFor(route.points, mapRect);
  ASSERT_TRUE(viewport.isValid());

  RecordingCanvas canvas(mapRect.width, mapRect.height);
  TrackingSource source(route.bytes);
  const RenderStatus status = RouteMapRenderer::draw(canvas, source, route.index, viewport, nullptr);
  ASSERT_EQ(status, RenderStatus::Ok);

  // Segment 0: A0->A1, A1->A2 (2 lines). Segment 1: B0->B1, B1->B2 (2 lines).
  // A renderer that joined segments would emit a 5th line A2->B0 or merge
  // everything into fewer, longer primitives.
  ASSERT_EQ(canvas.lineCount(), 4U);
  EXPECT_EQ(canvas.discCount(), 0U);
  EXPECT_EQ(canvas.ringCount(), 0U);

  const auto proj = [&](size_t i) { return viewport.project(route.points[i]); };
  const std::vector<ScreenPoint> expected = {proj(0), proj(1), proj(2), proj(3), proj(4), proj(5)};
  const auto& lines = canvas.lines();
  EXPECT_EQ(lines[0].x0, expected[0].x);
  EXPECT_EQ(lines[0].y0, expected[0].y);
  EXPECT_EQ(lines[0].x1, expected[1].x);
  EXPECT_EQ(lines[0].y1, expected[1].y);
  EXPECT_EQ(lines[1].x0, expected[1].x);
  EXPECT_EQ(lines[1].y0, expected[1].y);
  EXPECT_EQ(lines[1].x1, expected[2].x);
  EXPECT_EQ(lines[1].y1, expected[2].y);
  EXPECT_EQ(lines[2].x0, expected[3].x);
  EXPECT_EQ(lines[2].y0, expected[3].y);
  EXPECT_EQ(lines[2].x1, expected[4].x);
  EXPECT_EQ(lines[2].y1, expected[4].y);
  EXPECT_EQ(lines[3].x0, expected[4].x);
  EXPECT_EQ(lines[3].y0, expected[4].y);
  EXPECT_EQ(lines[3].x1, expected[5].x);
  EXPECT_EQ(lines[3].y1, expected[5].y);
  for (const auto& line : lines) {
    EXPECT_EQ(line.width, RouteMapRenderer::kRouteLineWidthPx);
  }
}

// ---------------------------------------------------------------------------
// Clipping
// ---------------------------------------------------------------------------

TEST(RouteMapRendererTest, ClipsLinesToMapRectAndCullsFullyOutsideEdges) {
  // A vertical 6-point route straddling a walking-scale 300x400 viewport.
  // Offsets are exact multiples of 100 E7 (200.4 m and 400.8 m steps).
  const int32_t latE7 = 523'700'000;
  const int32_t lonE7 = 49'041'000;
  const std::vector<GeoPoint> points = {
      GeoPoint{latE7 + 64'800, lonE7},  // p0: ~721 m north, far above the map
      GeoPoint{latE7 + 36'000, lonE7},  // p1: ~401 m north, above the map
      GeoPoint{latE7 + 18'000, lonE7},  // p2: ~200 m north, inside near top
      GeoPoint{latE7 - 18'000, lonE7},  // p3: ~200 m south, inside near bottom
      GeoPoint{latE7 - 36'000, lonE7},  // p4: ~401 m south, below the map
      GeoPoint{latE7 - 64'800, lonE7},  // p5: ~721 m south, far below the map
  };
  const Bytes bytes = encodeRoute(points, {0});
  RouteIndex index;
  ASSERT_EQ(decode(bytes, index), DecodeStatus::Ok);

  const GeoPoint center{latE7, lonE7};
  const Rect mapRect{0, 0, 300, 400};
  const RouteViewport viewport = RouteViewport::centered(center, mapRect, 400, 8);
  ASSERT_TRUE(viewport.isValid());

  RecordingCanvas canvas(mapRect.width, mapRect.height);
  TrackingSource source(bytes);
  const RenderStatus status = RouteMapRenderer::draw(canvas, source, index, viewport, nullptr);
  ASSERT_EQ(status, RenderStatus::Ok);

  // p0->p1 and p4->p5 are entirely above/below the map and must be culled;
  // the crossing edges p1->p2 and p3->p4 are clipped to the top/bottom border;
  // p2->p3 is fully visible. Total = 3 line primitives.
  ASSERT_EQ(canvas.lineCount(), 3U);
  const auto proj = [&](size_t i) { return viewport.project(points[i]); };
  // p1 clipped to the top border (x is the vertical axis through the center).
  EXPECT_EQ(canvas.lines()[0].x0, proj(2).x);
  EXPECT_EQ(canvas.lines()[0].y0, 0);
  EXPECT_EQ(canvas.lines()[0].x1, proj(2).x);
  EXPECT_EQ(canvas.lines()[0].y1, proj(2).y);
  // p2->p3 unclipped.
  EXPECT_EQ(canvas.lines()[1].x0, proj(2).x);
  EXPECT_EQ(canvas.lines()[1].y0, proj(2).y);
  EXPECT_EQ(canvas.lines()[1].x1, proj(3).x);
  EXPECT_EQ(canvas.lines()[1].y1, proj(3).y);
  // p3->p4 clipped to the bottom border.
  EXPECT_EQ(canvas.lines()[2].x0, proj(3).x);
  EXPECT_EQ(canvas.lines()[2].y0, proj(3).y);
  EXPECT_EQ(canvas.lines()[2].x1, proj(3).x);
  EXPECT_EQ(canvas.lines()[2].y1, mapRect.height - 1);

  // p0 and p5 (the far-offscreen endpoints) never appear as a drawn endpoint;
  // the clipped edges reach the top/bottom border instead.
  for (const auto& line : canvas.lines()) {
    EXPECT_FALSE(line.x0 == proj(0).x && line.y0 == proj(0).y) << "p0 reached the canvas";
    EXPECT_FALSE(line.x1 == proj(0).x && line.y1 == proj(0).y) << "p0 reached the canvas";
    EXPECT_FALSE(line.x0 == proj(5).x && line.y0 == proj(5).y) << "p5 reached the canvas";
    EXPECT_FALSE(line.x1 == proj(5).x && line.y1 == proj(5).y) << "p5 reached the canvas";
  }
}

// ---------------------------------------------------------------------------
// Position marker
// ---------------------------------------------------------------------------

TEST(RouteMapRendererTest, CurrentPositionMarkerDrawnLastRingThenDiscDominantRoute) {
  const TwoSegmentRoute route = makeTwoSegmentRoute();
  const Rect mapRect{0, 0, 300, 400};
  const RouteViewport viewport = viewFor(route.points, mapRect);
  ASSERT_TRUE(viewport.isValid());

  // Position sits exactly at the viewport center (on the route's midpoint).
  const ScreenPoint centerProj = viewport.project(route.points[3]);
  const CurrentPosition position{route.points[3], 30, 137, false, 0};
  RecordingCanvas canvas(mapRect.width, mapRect.height);
  TrackingSource source(route.bytes);
  const RenderStatus status = RouteMapRenderer::draw(canvas, source, route.index, viewport, &position);
  ASSERT_EQ(status, RenderStatus::Ok);

  ASSERT_EQ(canvas.lineCount(), 4U);
  ASSERT_EQ(canvas.ringCount(), 1U);
  ASSERT_EQ(canvas.discCount(), 1U);

  // The marker is drawn after every route line: ring first, disc last.
  const auto& kinds = canvas.kinds();
  ASSERT_EQ(kinds.size(), 7U);  // clear + 4 route lines + ring + disc
  EXPECT_EQ(kinds[0], RecordingCanvas::Kind::Clear);
  EXPECT_EQ(kinds[1], RecordingCanvas::Kind::Line);
  EXPECT_EQ(kinds[2], RecordingCanvas::Kind::Line);
  EXPECT_EQ(kinds[3], RecordingCanvas::Kind::Line);
  EXPECT_EQ(kinds[4], RecordingCanvas::Kind::Line);
  EXPECT_EQ(kinds[5], RecordingCanvas::Kind::Ring);
  EXPECT_EQ(kinds[6], RecordingCanvas::Kind::Disc);

  // Ring radius encodes accuracy at the walking scale (30 m ~ 2.8 px at this
  // span, so it clamps up to the documented minimum).
  const auto& ring = canvas.circles()[0];
  EXPECT_EQ(ring.centerX, centerProj.x);
  EXPECT_EQ(ring.centerY, centerProj.y);
  EXPECT_EQ(ring.radius, RouteMapRenderer::kMarkerMinRingRadiusPx);
  EXPECT_EQ(ring.width, RouteMapRenderer::kMarkerRingWidthPx);
  const auto& disc = canvas.circles()[1];
  EXPECT_EQ(disc.centerX, centerProj.x);
  EXPECT_EQ(disc.centerY, centerProj.y);
  EXPECT_EQ(disc.radius, 7);

  // Route pen is the widest pen on the canvas (visual dominance convention).
  EXPECT_GT(RouteMapRenderer::kRouteLineWidthPx, RouteMapRenderer::kMarkerRingWidthPx);
  for (const auto& line : canvas.lines()) {
    EXPECT_EQ(line.width, RouteMapRenderer::kRouteLineWidthPx);
  }
}

TEST(RouteMapRendererTest, PositionMarkerAccuracyScalesRingAndClipsToMapRect) {
  // Vertical route + 400 m span viewport from the clipping test.
  const int32_t latE7 = 523'700'000;
  const int32_t lonE7 = 49'041'000;
  const std::vector<GeoPoint> points = {
      GeoPoint{latE7 + 64'800, lonE7}, GeoPoint{latE7 + 36'000, lonE7}, GeoPoint{latE7 + 18'000, lonE7},
      GeoPoint{latE7 - 18'000, lonE7}, GeoPoint{latE7 - 36'000, lonE7}, GeoPoint{latE7 - 64'800, lonE7},
  };
  const Bytes bytes = encodeRoute(points, {0});
  RouteIndex index;
  ASSERT_EQ(decode(bytes, index), DecodeStatus::Ok);
  const Rect mapRect{0, 0, 300, 400};
  const RouteViewport viewport = RouteViewport::centered(GeoPoint{latE7, lonE7}, mapRect, 400, 8);
  ASSERT_TRUE(viewport.isValid());

  // Marker on p2 (~200 m north): visible near the top edge. Accuracy 400 m
  // wants a ~284 px ring at this scale, far larger than the distance from the
  // center to the top border, so the renderer must clip the radius.
  const CurrentPosition nearTop{points[2], 400, 0, false, 0};
  RecordingCanvas canvas(mapRect.width, mapRect.height);
  const ScreenPoint p2 = viewport.project(points[2]);
  ASSERT_EQ(drawBytes(canvas, bytes, index, viewport, &nearTop), RenderStatus::Ok);
  ASSERT_EQ(canvas.ringCount(), 1U);
  ASSERT_EQ(canvas.discCount(), 1U);
  EXPECT_EQ(canvas.circles()[0].centerX, p2.x);
  EXPECT_EQ(canvas.circles()[0].centerY, p2.y);
  // Radius clipped to the top margin (smallest of the four margins).
  EXPECT_EQ(canvas.circles()[0].radius, p2.y);
  EXPECT_EQ(canvas.circles()[1].radius, RouteMapRenderer::kMarkerDiscRadiusPx);

  // Marker fully outside the map (p0, ~721 m north) draws no marker at all.
  const CurrentPosition outside{points[0], 400, 0, false, 0};
  RecordingCanvas outsideCanvas(mapRect.width, mapRect.height);
  ASSERT_EQ(drawBytes(outsideCanvas, bytes, index, viewport, &outside), RenderStatus::Ok);
  EXPECT_EQ(outsideCanvas.ringCount(), 0U);
  EXPECT_EQ(outsideCanvas.discCount(), 0U);
  EXPECT_EQ(outsideCanvas.lineCount(), 3U);  // route itself still drawn
}

TEST(RouteMapRendererTest, AbsentPositionDrawsNoMarker) {
  const TwoSegmentRoute route = makeTwoSegmentRoute();
  const Rect mapRect{0, 0, 300, 400};
  const RouteViewport viewport = viewFor(route.points, mapRect);
  ASSERT_TRUE(viewport.isValid());

  RecordingCanvas canvas(mapRect.width, mapRect.height);
  ASSERT_EQ(drawBytes(canvas, route.bytes, route.index, viewport, nullptr), RenderStatus::Ok);
  EXPECT_EQ(canvas.ringCount(), 0U);
  EXPECT_EQ(canvas.discCount(), 0U);
  EXPECT_EQ(canvas.lineCount(), 4U);
}

// ---------------------------------------------------------------------------
// Streaming detail: bounded reads, full geometry, no overview shortcut
// ---------------------------------------------------------------------------

TEST(RouteMapRendererTest, StreamsLongDetailedSegmentWithBoundedReadsToTheEnd) {
  // One 3021-point segment whose visible part sits at its very end. Because
  // the wire is delta encoded, a correct renderer must stream every delta
  // (12,080 bytes) through <= 1,024-byte reads before it can draw the last
  // point; a renderer that cheated with the bounded overview would read
  // nothing and could not draw the exact final edge.
  std::vector<GeoPoint> points;
  points.reserve(3021);
  for (int i = 0; i <= 3020; ++i) {
    points.push_back(GeoPoint{523'676'000 + i * 1000, 49'041'000 + i * 1000});
  }
  const Bytes bytes = encodeRoute(points, {0});
  RouteIndex index;
  ASSERT_EQ(decode(bytes, index), DecodeStatus::Ok);
  EXPECT_EQ(index.overviewPointCount, RouteIndex::kMaxOverviewPoints);  // 1024, sampled
  EXPECT_EQ(index.pointCount, 3021U);

  const Rect mapRect{0, 0, 300, 400};
  const RouteViewport viewport = RouteViewport::centered(points.back(), mapRect, 200, 8);
  ASSERT_TRUE(viewport.isValid());

  RecordingCanvas canvas(mapRect.width, mapRect.height);
  TrackingSource source(bytes);
  const RenderStatus status = RouteMapRenderer::draw(canvas, source, index, viewport, nullptr);
  ASSERT_EQ(status, RenderStatus::Ok);

  // The geometry region is 3020 deltas * 4 bytes = 12,080 bytes, so a
  // 1,024-byte buffer forces at least 12 read requests, each <= 1,024.
  EXPECT_GE(source.readCalls, 12U);
  EXPECT_EQ(source.maxRequested, navigator::kRoutePackageV1WorkBufferBytes);

  // The end of the route is inside the viewport and the final edge is drawn
  // exactly at the reconstructed final point.
  EXPECT_GE(canvas.lineCount(), 5U);
  const ScreenPoint last = viewport.project(points.back());
  const auto& lines = canvas.lines();
  EXPECT_EQ(lines.back().x1, last.x);
  EXPECT_EQ(lines.back().y1, last.y);
  EXPECT_NE(lines.back().x0, lines.back().x1);  // non-degenerate final edge
}

// ---------------------------------------------------------------------------
// Failure modes
// ---------------------------------------------------------------------------

TEST(RouteMapRendererTest, ShortReadFailsSafelyWithTypedStatus) {
  const TwoSegmentRoute route = makeTwoSegmentRoute();
  const Rect mapRect{0, 0, 300, 400};
  const RouteViewport viewport = viewFor(route.points, mapRect);
  ASSERT_TRUE(viewport.isValid());

  // Truncate inside segment 1's 8-byte absolute anchor: segment 0 (2 lines)
  // has already been streamed, segment 1 can never start.
  const uint32_t anchorOffset = route.index.segments[1].sourceOffset;
  ASSERT_GT(anchorOffset, 0U);
  Bytes truncated(route.bytes.begin(), route.bytes.begin() + anchorOffset + 4);
  RecordingCanvas canvas(mapRect.width, mapRect.height);
  const RenderStatus status = drawBytes(canvas, truncated, route.index, viewport, nullptr);
  EXPECT_EQ(status, RenderStatus::ShortRead);
  EXPECT_EQ(canvas.clearCount(), 1U);
  EXPECT_EQ(canvas.lineCount(), 2U);  // only segment 0 was drawn
  EXPECT_EQ(canvas.ringCount(), 0U);
  EXPECT_EQ(canvas.discCount(), 0U);

  // Truncate before the geometry even starts: nothing can be drawn.
  Bytes tiny(route.bytes.begin(), route.bytes.begin() + 30);
  RecordingCanvas tinyCanvas(mapRect.width, mapRect.height);
  EXPECT_EQ(drawBytes(tinyCanvas, tiny, route.index, viewport, nullptr), RenderStatus::ShortRead);
  EXPECT_EQ(tinyCanvas.clearCount(), 1U);
  EXPECT_EQ(tinyCanvas.lineCount(), 0U);
}

TEST(RouteMapRendererTest, MalformedIndexRejectedBeforeAnyCanvasCall) {
  const TwoSegmentRoute route = makeTwoSegmentRoute();
  const Rect mapRect{0, 0, 300, 400};
  const RouteViewport viewport = viewFor(route.points, mapRect);
  ASSERT_TRUE(viewport.isValid());

  RouteIndex tooManyPoints = route.index;
  tooManyPoints.pointCount = RouteIndex::kMaxPoints + 1;
  RecordingCanvas c1(mapRect.width, mapRect.height);
  EXPECT_EQ(drawBytes(c1, route.bytes, tooManyPoints, viewport, nullptr), RenderStatus::InvalidIndex);
  EXPECT_EQ(c1.callCount(), 0U);

  RouteIndex noSegments = route.index;
  noSegments.segmentCount = 0;
  RecordingCanvas c2(mapRect.width, mapRect.height);
  EXPECT_EQ(drawBytes(c2, route.bytes, noSegments, viewport, nullptr), RenderStatus::InvalidIndex);
  EXPECT_EQ(c2.callCount(), 0U);

  RouteIndex segmentsBeyondPoints = route.index;
  segmentsBeyondPoints.segmentCount = 7;  // > pointCount (6)
  RecordingCanvas c3(mapRect.width, mapRect.height);
  EXPECT_EQ(drawBytes(c3, route.bytes, segmentsBeyondPoints, viewport, nullptr), RenderStatus::InvalidIndex);
  EXPECT_EQ(c3.callCount(), 0U);

  RouteIndex badFirstStart = route.index;
  badFirstStart.segments[0].startPointIndex = 2;  // first segment must start at 0
  RecordingCanvas c4(mapRect.width, mapRect.height);
  EXPECT_EQ(drawBytes(c4, route.bytes, badFirstStart, viewport, nullptr), RenderStatus::InvalidIndex);
  EXPECT_EQ(c4.callCount(), 0U);

  RouteIndex nonIncreasingStarts = route.index;
  nonIncreasingStarts.segments[1].startPointIndex = 0;  // not > segments[0].start (0)
  RecordingCanvas c5(mapRect.width, mapRect.height);
  EXPECT_EQ(drawBytes(c5, route.bytes, nonIncreasingStarts, viewport, nullptr), RenderStatus::InvalidIndex);
  EXPECT_EQ(c5.callCount(), 0U);
}

TEST(RouteMapRendererTest, InvalidViewportFailsBeforeAnyCanvasCall) {
  const TwoSegmentRoute route = makeTwoSegmentRoute();
  RecordingCanvas canvas(300, 400);
  EXPECT_EQ(drawBytes(canvas, route.bytes, route.index, RouteViewport{}, nullptr), RenderStatus::InvalidViewport);
  EXPECT_EQ(canvas.callCount(), 0U);
}

// ---------------------------------------------------------------------------
// RenderStatus domain sanity
// ---------------------------------------------------------------------------

TEST(RouteMapRendererTest, RenderStatusEnumHasStableOrder) {
  EXPECT_LT(static_cast<int>(RenderStatus::Ok), static_cast<int>(RenderStatus::InvalidViewport));
  EXPECT_LT(static_cast<int>(RenderStatus::InvalidViewport), static_cast<int>(RenderStatus::InvalidIndex));
  EXPECT_LT(static_cast<int>(RenderStatus::InvalidIndex), static_cast<int>(RenderStatus::ShortRead));
}

// ---------------------------------------------------------------------------
// Along-route remaining distance for a live fix
// ---------------------------------------------------------------------------

TEST(RouteMapRendererTest, RemainingDistanceAtRouteStartMiddleAndEnd) {
  // Straight north-south route: four equal 100,000 E7 (~1.1 km) edges whose
  // E5 wire quantization is lossless. The declared total (4,000 m) is what
  // the remaining estimate scales to.
  const int32_t baseLat = 520'000'000;
  const int32_t baseLon = 40'000'000;
  std::vector<GeoPoint> points;
  for (int i = 0; i < 5; ++i) {
    points.push_back(GeoPoint{baseLat + i * 100'000, baseLon});
  }
  const Bytes bytes = encodeRoute(points, {0}, 4000, 100);
  RouteIndex index;
  ASSERT_EQ(decode(bytes, index), DecodeStatus::Ok);
  ASSERT_EQ(index.totalDistanceMeters, 4000U);
  ASSERT_EQ(index.estimatedMinutes, 100U);

  const ProximityOutcome start = drawWithFix(bytes, index, points[0]);
  ASSERT_EQ(start.status, RenderStatus::Ok);
  EXPECT_TRUE(start.proximity.valid);
  EXPECT_EQ(start.proximity.remainingDistanceMeters, 4000U);

  const ProximityOutcome middle = drawWithFix(bytes, index, points[2]);
  ASSERT_EQ(middle.status, RenderStatus::Ok);
  EXPECT_TRUE(middle.proximity.valid);
  EXPECT_EQ(middle.proximity.remainingDistanceMeters, 2000U);

  const ProximityOutcome end = drawWithFix(bytes, index, points[4]);
  ASSERT_EQ(end.status, RenderStatus::Ok);
  EXPECT_TRUE(end.proximity.valid);
  EXPECT_EQ(end.proximity.remainingDistanceMeters, 0U);
}

TEST(RouteMapRendererTest, RemainingDistanceAlongLongitudeUsesSameScale) {
  // The same start/middle/end walk along the longitude axis: the renderer
  // measures through the viewport's cosine-scaled U units, so the remaining
  // share must match the north-south route even though every edge has no
  // latitude component.
  const int32_t baseLat = 520'000'000;
  const int32_t baseLon = 40'000'000;
  std::vector<GeoPoint> points;
  for (int i = 0; i < 5; ++i) {
    points.push_back(GeoPoint{baseLat, baseLon + i * 100'000});
  }
  const Bytes bytes = encodeRoute(points, {0}, 4000, 100);
  RouteIndex index;
  ASSERT_EQ(decode(bytes, index), DecodeStatus::Ok);

  EXPECT_EQ(drawWithFix(bytes, index, points[0]).proximity.remainingDistanceMeters, 4000U);
  EXPECT_EQ(drawWithFix(bytes, index, points[2]).proximity.remainingDistanceMeters, 2000U);
  EXPECT_EQ(drawWithFix(bytes, index, points[4]).proximity.remainingDistanceMeters, 0U);
}

TEST(RouteMapRendererTest, RemainingDistanceClampsBeforeStartPastEndAndInsideEdge) {
  const int32_t baseLat = 520'000'000;
  const int32_t baseLon = 40'000'000;
  std::vector<GeoPoint> points;
  for (int i = 0; i < 5; ++i) {
    points.push_back(GeoPoint{baseLat + i * 100'000, baseLon});
  }
  const Bytes bytes = encodeRoute(points, {0}, 4000, 100);
  RouteIndex index;
  ASSERT_EQ(decode(bytes, index), DecodeStatus::Ok);

  // A fix before the first point still has the whole route ahead; a fix past
  // the last point has nothing ahead. Both stay inside [0, total].
  EXPECT_EQ(drawWithFix(bytes, index, GeoPoint{baseLat - 100'000, baseLon}).proximity.remainingDistanceMeters,
            4000U);
  EXPECT_EQ(drawWithFix(bytes, index, GeoPoint{baseLat + 5 * 100'000, baseLon}).proximity.remainingDistanceMeters,
            0U);
  // A fix 25% into the P1->P2 edge has walked 1.25 of the 4 equal edges:
  // (4 - 1.25) / 4 * 4000 m = 2,750 m remain.
  EXPECT_EQ(drawWithFix(bytes, index, GeoPoint{baseLat + 125'000, baseLon}).proximity.remainingDistanceMeters,
            2750U);
}

TEST(RouteMapRendererTest, RemainingDistanceNeverMeasuresAcrossSegmentGap) {
  // Segment 0 is two equal edges; segment 1 restarts far north (a ~111 km
  // gap) at an absolute anchor and runs two more equal edges. A renderer
  // that joined the segments would count the gap as walked route, so a fix
  // at the end of segment 0 and one at the start of segment 1 must report
  // the same remaining share: the second half of the route.
  const int32_t baseLat = 520'000'000;
  const int32_t baseLon = 40'000'000;
  const std::vector<GeoPoint> points = {
      GeoPoint{baseLat, baseLon},              // A0
      GeoPoint{baseLat + 100'000, baseLon},    // A1
      GeoPoint{baseLat + 200'000, baseLon},    // A2 (end of segment 0)
      GeoPoint{baseLat + 10'200'000, baseLon},  // B0 (absolute anchor)
      GeoPoint{baseLat + 10'300'000, baseLon},  // B1
      GeoPoint{baseLat + 10'400'000, baseLon},  // B2 (end of route)
  };
  const Bytes bytes = encodeRoute(points, {0, 3}, 4000, 100);
  RouteIndex index;
  ASSERT_EQ(decode(bytes, index), DecodeStatus::Ok);
  ASSERT_EQ(index.segmentCount, 2U);

  EXPECT_EQ(drawWithFix(bytes, index, points[0]).proximity.remainingDistanceMeters, 4000U);
  EXPECT_EQ(drawWithFix(bytes, index, points[2]).proximity.remainingDistanceMeters, 2000U);
  EXPECT_EQ(drawWithFix(bytes, index, points[3]).proximity.remainingDistanceMeters, 2000U);
  EXPECT_EQ(drawWithFix(bytes, index, points[5]).proximity.remainingDistanceMeters, 0U);
}

TEST(RouteMapRendererTest, RemainingDistanceWithoutMeasurableGeometryKeepsDeclaredTotal) {
  // A route whose geometry collapses to a single point has nothing measured,
  // so the renderer reports the whole declared total instead of guessing 0.
  const std::vector<GeoPoint> points = {GeoPoint{520'000'000, 40'000'000},
                                        GeoPoint{520'000'000, 40'000'000}};
  const Bytes bytes = encodeRoute(points, {0}, 4000, 100);
  RouteIndex index;
  ASSERT_EQ(decode(bytes, index), DecodeStatus::Ok);

  const ProximityOutcome outcome = drawWithFix(bytes, index, points[0]);
  ASSERT_EQ(outcome.status, RenderStatus::Ok);
  EXPECT_TRUE(outcome.proximity.valid);
  EXPECT_EQ(outcome.proximity.remainingDistanceMeters, 4000U);
}
