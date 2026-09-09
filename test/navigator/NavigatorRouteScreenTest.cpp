// Host tests for the routed NavScreenRenderer overload
// (src/spikes/navigator/NavScreenRenderer.h/.cpp).
//
// Task 6 - strict TDD: these tests were written first, before the routed
// overload existed. The routed overload must:
//   * keep the existing 4-argument draw() byte-identical (legacy splash and
//     every dynamic screen), and use that exact path when the route is null
//     or incomplete (no source or no index);
//   * draw the base UI first and, only when a validated route index plus a
//     valid viewport are available for a Navigating state, replace the
//     schematic map inside the logical map rectangle with the real route;
//   * keep every maneuver/status band, border, separator and bottom status
//     pixel byte-identical to the legacy screen (route ink is confined to the
//     logical map rectangle even on non-byte-aligned physical widths);
//   * never connect route segments (the golden fixture's second, far-away
//     segment stays disconnected and contributes no ink);
//   * draw the current-position marker last, over the route;
//   * fail visually safely: a renderer failure (short read / malformed index)
//     must restore the legacy schematic map byte-for-byte without a second
//     framebuffer;
//   * keep non-navigating status screens (Recalculating / OffRoute / Arrived)
//     dominant and byte-identical to the legacy path even when a valid route
//     is supplied.
//
// The golden fixture's second segment is deliberately in Tokyo, so a
// whole-route fit renders the Amsterdam segment as a sub-pixel cluster: those
// tests assert containment, the marker overlay on the Amsterdam segment and
// the empty disconnected corridor instead of polyline pixel counts. Visible
// polyline ink is exercised with a local two-segment route at walking scale.

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

#include "ManeuverBandPlanner.h"
#include "NavScreenRenderer.h"
#include "NavSplash.h"
#include "NavState.h"
#include "NavTestFrame.h"
#include "NavigationViewController.h"
#include "map/GrayMap.h"
#include "map/RouteMapRenderer.h"
#include "map/RouteViewport.h"
#include "route/RoutePackageV1.h"

namespace {

using Bytes = std::vector<uint8_t>;

using navigator::CurrentPosition;
using navigator::DecodeStatus;
using navigator::GeoPoint;
using navigator::Maneuver;
using navigator::ManeuverBandPlanner;
using navigator::NavGrayPlane;
using navigator::NavManeuverPresentation;
using navigator::NavScreenRenderer;
using navigator::NavState;
using navigator::NavStatus;
using navigator::Rect;
using navigator::RouteIndex;
using navigator::RouteMapRenderer;
using navigator::RouteProximity;
using navigator::RouteViewport;
using navigator::ScreenPoint;

using navtest::countLogicalBlack;
using navtest::expectGuardsUntouched;
using navtest::expectLogicalBlack;
using navtest::expectLogicalWhite;
using navtest::Frame;
using navtest::kSentinel;
using navtest::logicalPixelIsBlack;
using navtest::rowBytes;

int clampInt(int value, int low, int high) { return value < low ? low : (value > high ? high : value); }

// ---------------------------------------------------------------------------
// Wire helpers (little-endian encoder + CRC), mirroring RouteMapRendererTest.
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

void putU16(Bytes& bytes, size_t at, uint16_t value) {
  bytes[at] = static_cast<uint8_t>(value & 0xFFu);
  bytes[at + 1] = static_cast<uint8_t>((value >> 8) & 0xFFu);
}

void putU32(Bytes& bytes, size_t at, uint32_t value) {
  bytes[at] = static_cast<uint8_t>(value & 0xFFu);
  bytes[at + 1] = static_cast<uint8_t>((value >> 8) & 0xFFu);
  bytes[at + 2] = static_cast<uint8_t>((value >> 16) & 0xFFu);
  bytes[at + 3] = static_cast<uint8_t>((value >> 24) & 0xFFu);
}

void putI32(Bytes& bytes, size_t at, int32_t value) { putU32(bytes, at, static_cast<uint32_t>(value)); }

int64_t quantizeE5(int32_t e7) {
  const int64_t v = e7;
  const int64_t magnitude = v < 0 ? -v : v;
  const int64_t quantized = (magnitude + 50) / 100;
  return v < 0 ? -quantized : quantized;
}

// Encodes a valid Route Package v1 from E7 points and per-segment starts
// (segment 0 starts at the header origin; later segments restart with an
// absolute E7 anchor; interior points are signed Int16 E5 deltas).
struct ManeuverSpec {
  uint8_t type = 0;  // wire kind byte 0..6, mapping 1:1 to navigator::Maneuver
  uint16_t pointIndex = 0;
  uint32_t distanceFromStartMeters = 0;
};

Bytes encodeRoute(const std::vector<GeoPoint>& points, const std::vector<uint16_t>& segmentStarts,
                  const std::vector<ManeuverSpec>& maneuvers = {}) {
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
  // Maneuver records follow the geometry: point index u16, type u8, name byte
  // count u8 (empty in this milestone), distance-from-start u32, then the
  // (empty) name bytes.
  for (const ManeuverSpec& maneuver : maneuvers) {
    appendU16(payload, maneuver.pointIndex);
    payload.push_back(maneuver.type);
    payload.push_back(0);
    appendU32(payload, maneuver.distanceFromStartMeters);
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
  putU16(bytes, 18, static_cast<uint16_t>(maneuvers.size()));
  putI32(bytes, 20, points.front().latitudeE7);
  putI32(bytes, 24, points.front().longitudeE7);
  putU32(bytes, 28, 0);
  putU16(bytes, 32, 0);
  putU16(bytes, 34, static_cast<uint16_t>(segmentStarts.size()));
  bytes[36] = 0;
  bytes[37] = 0;
  std::copy(payload.begin(), payload.end(), bytes.begin() + 38);
  const uint32_t crc = crc32Of(Bytes(bytes.begin(), bytes.begin() + (total - 4)));
  putU32(bytes, total - 4, crc);
  return bytes;
}

// ---------------------------------------------------------------------------
// Golden fixture loading and host sources.
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

// Reports the full package size but stops delivering bytes once a budget is
// exhausted, so a validated route can still fail mid-stream with a ShortRead.
class StallingSource : public navigator::RouteByteSource {
 public:
  StallingSource(Bytes bytes, uint32_t budget) : inner_(std::move(bytes)), budget_(budget) {}

  uint32_t size() const override { return inner_.size(); }

  uint32_t read(uint32_t offset, uint8_t* destination, uint32_t length) override {
    if (delivered_ >= budget_) {
      return 0;
    }
    const uint32_t n = std::min({length, budget_ - delivered_, inner_.size() - std::min(offset, inner_.size())});
    const uint32_t got = inner_.read(offset, destination, n);
    delivered_ += got;
    return got;
  }

 private:
  VectorRouteByteSource inner_;
  uint32_t budget_;
  uint32_t delivered_ = 0;
};

// A conforming source, but the most awkward one the contract allows: it never
// delivers more than `chunk` bytes per call, so every reader has to loop.
// RoutePackageV1.h documents this as permitted; StoredRoute's NVS-backed
// reads behave this way once a range straddles a page.
class ChunkedSource : public navigator::RouteByteSource {
 public:
  ChunkedSource(Bytes bytes, uint32_t chunk) : inner_(std::move(bytes)), chunk_(chunk) {}

  uint32_t size() const override { return inner_.size(); }

  uint32_t read(uint32_t offset, uint8_t* destination, uint32_t length) override {
    return inner_.read(offset, destination, std::min(length, chunk_));
  }

 private:
  VectorRouteByteSource inner_;
  uint32_t chunk_;
};

DecodeStatus decode(const Bytes& bytes, RouteIndex& out) {
  VectorRouteByteSource source(bytes);
  return navigator::validateRoutePackageV1(source, out);
}

// ---------------------------------------------------------------------------
// Routes.
// ---------------------------------------------------------------------------

// Golden fixture: 90-byte Route Package v1 (segment 0 near Amsterdam, segment
// 1 deliberately far away near Tokyo; decoded by RoutePackageV1Test).
const Bytes& goldenRouteBytes() {
  static const Bytes kBytes = loadFixtureBytes("route_package_v1.bin");
  return kBytes;
}

// Midpoint of the golden fixture's Amsterdam segment (point index 1). It lies
// exactly on segment 0's polyline so the rendered marker sits over the route.
constexpr GeoPoint kAmsterdamPosition{523'688'000, 49'049'000};

// Local two-segment route near Amsterdam at walking scale (~1.1 km tall), so
// the polyline is clearly visible on the X3 map. The segments are disjoint and
// must not be visually joined.
struct LocalRoute {
  std::vector<GeoPoint> points;
  std::vector<uint16_t> segmentStarts;
  Bytes bytes;

  static LocalRoute make() {
    LocalRoute route;
    route.points = {
        GeoPoint{523'676'000, 49'041'000}, GeoPoint{523'696'000, 49'061'000}, GeoPoint{523'716'000, 49'081'000},
        GeoPoint{523'736'000, 49'101'000}, GeoPoint{523'756'000, 49'121'000}, GeoPoint{523'776'000, 49'141'000},
    };
    route.segmentStarts = {0, 3};
    route.bytes = encodeRoute(route.points, route.segmentStarts);
    return route;
  }
};

// ---------------------------------------------------------------------------
// Screen geometry mirror (matches NavScreenRenderer.cpp's Layout; the routed
// map rectangle lives inside the border and above the bottom status text).
// ---------------------------------------------------------------------------

struct Geom {
  int LW;  // logical width  = physical height
  int LH;  // logical height = physical width
  int border;
  int sepY;
  int sepThick;
  int mapTop;
  int statusScale;
  int statusY;
  int mapH;  // logical map rect height (statusY - mapTop)
  int mapW;  // logical map rect width  (LW - 2*border)

  explicit Geom(int physW, int physH)
      : LW(physH),
        LH(physW),
        border(clampInt(LW / 300 + 1, 1, 4)),
        sepY(border + LH * 38 / 100),
        sepThick(std::max(1, LH / 200)),
        mapTop(sepY + sepThick),
        statusScale(clampInt((LH + 199) / 200, 1, 3)),
        statusY(LH - border - 7 * statusScale),
        mapH(statusY - mapTop),
        mapW(LW - 2 * border) {}

  Rect canvasMapRect() const { return Rect{0, 0, mapW, mapH}; }
  int mapX() const { return border; }
  int mapY() const { return mapTop; }
};

bool logicalPixelIsInsideMapRect(const Geom& g, int lx, int ly) {
  return lx >= g.mapX() && lx < g.mapX() + g.mapW && ly >= g.mapY() && ly < g.mapY() + g.mapH;
}

// Every logical pixel outside the map rectangle must be byte-identical between
// the legacy and routed frames (borders, bands, separator, bottom status and
// row padding are never touched by the route adapter).
void expectIdenticalOutsideMapRect(const Frame& legacy, const Frame& routed, const Geom& g) {
  ASSERT_EQ(legacy.width, routed.width);
  ASSERT_EQ(legacy.height, routed.height);
  for (int ly = 0; ly < g.LH; ++ly) {
    for (int lx = 0; lx < g.LW; ++lx) {
      if (logicalPixelIsInsideMapRect(g, lx, ly)) {
        continue;
      }
      EXPECT_EQ(logicalPixelIsBlack(legacy, lx, ly), logicalPixelIsBlack(routed, lx, ly))
          << "logical pixel (" << lx << "," << ly << ") changed outside the map rect";
    }
  }
}

// Number of differing logical pixels inside the map rectangle.
int countDifferingInsideMapRect(const Frame& legacy, const Frame& routed, const Geom& g) {
  int differing = 0;
  for (int ly = g.mapY(); ly < g.mapY() + g.mapH; ++ly) {
    for (int lx = g.mapX(); lx < g.mapX() + g.mapW; ++lx) {
      if (logicalPixelIsBlack(legacy, lx, ly) != logicalPixelIsBlack(routed, lx, ly)) {
        ++differing;
      }
    }
  }
  return differing;
}

// Replicates NavScreenRenderer's routed viewport: fitOverview over the
// canvas-local map rectangle with the viewport's default equal padding.
RouteViewport routedViewport(const RouteIndex& index, const Geom& g) {
  return RouteViewport::fitOverview(index, g.canvasMapRect(), RouteViewport::kDefaultPaddingPx);
}

// Expected screen-logical position of the renderer's position marker.
struct MarkerExpectation {
  int cx;
  int cy;
  int ringRadius;
};

MarkerExpectation expectedMarker(const RouteViewport& viewport, const Geom& g, const CurrentPosition& position) {
  const ScreenPoint m = viewport.project(position.point);
  MarkerExpectation expected;
  expected.cx = g.mapX() + m.x;
  expected.cy = g.mapY() + m.y;
  const int fit = std::min(std::min(m.x, g.mapW - 1 - m.x), std::min(m.y, g.mapH - 1 - m.y));
  int ring = viewport.pixelsForMeters(position.accuracyMeters);
  ring = std::clamp(ring, RouteMapRenderer::kMarkerMinRingRadiusPx,
                    static_cast<int>(RouteMapRenderer::kMarkerMaxRingRadiusPx));
  expected.ringRadius = std::min(ring, std::max(fit, 0));
  return expected;
}

void expectMarkerRendered(const Frame& frame, const MarkerExpectation& marker, const char* label) {
  SCOPED_TRACE(label);
  EXPECT_GE(marker.cx, 0);
  EXPECT_GE(marker.cy, 0);
  expectLogicalBlack(frame, marker.cx, marker.cy, "marker center disc");
  if (marker.ringRadius >= 1) {
    expectLogicalBlack(frame, marker.cx + marker.ringRadius, marker.cy, "marker ring east");
    expectLogicalBlack(frame, marker.cx - marker.ringRadius, marker.cy, "marker ring west");
    expectLogicalBlack(frame, marker.cx, marker.cy + marker.ringRadius, "marker ring south");
    expectLogicalBlack(frame, marker.cx, marker.cy - marker.ringRadius, "marker ring north");
  }
}

// ---------------------------------------------------------------------------
// Draw helpers.
// ---------------------------------------------------------------------------

void drawLegacy(const NavState& state, int width, int height, Frame* frame) {
  NavScreenRenderer::draw(frame->pixels(), static_cast<uint16_t>(width), static_cast<uint16_t>(height), state);
}

void drawRouted(const NavState& state, int width, int height, navigator::RouteByteSource& source,
                const RouteIndex& index, const CurrentPosition* position, Frame* frame) {
  NavScreenRenderer::draw(frame->pixels(), static_cast<uint16_t>(width), static_cast<uint16_t>(height), state, &source,
                          &index, position);
}

// ---------------------------------------------------------------------------
// Compatibility: the old overload and the null/incomplete routed path stay
// byte-identical.
// ---------------------------------------------------------------------------

TEST(NavigatorRouteScreenTest, LegacyOverloadDefaultStateRemainsByteIdentical792x528) {
  Frame splash(792, 528, kSentinel);
  NavSplash::draw(splash.pixels(), splash.width, splash.height);

  Frame dynamic(792, 528, kSentinel);
  drawLegacy(NavState(), dynamic.width, dynamic.height, &dynamic);

  ASSERT_EQ(dynamic.bytes.size(), splash.bytes.size());
  EXPECT_EQ(std::memcmp(dynamic.pixels(), splash.pixels(), rowBytes(792) * 528), 0)
      << "4-argument overload diverged from the legacy splash";
  expectGuardsUntouched(dynamic);
  expectGuardsUntouched(splash);
}

TEST(NavigatorRouteScreenTest, RoutedNullRouteUsesExactLegacyOutputForEveryStatus) {
  NavState states[4];
  states[1].status = NavStatus::Recalculating;
  states[2].status = NavStatus::OffRoute;
  states[3].status = NavStatus::Arrived;
  for (const NavState& state : states) {
    Frame legacy(792, 528, kSentinel);
    Frame routed(792, 528, kSentinel);
    drawLegacy(state, legacy.width, legacy.height, &legacy);
    NavScreenRenderer::draw(routed.pixels(), routed.width, routed.height, state, nullptr, nullptr, nullptr);
    EXPECT_EQ(std::memcmp(routed.pixels(), legacy.pixels(), rowBytes(792) * 528), 0)
        << "null-route routed draw diverged for status " << static_cast<int>(state.status);
    expectGuardsUntouched(routed);
  }
}

TEST(NavigatorRouteScreenTest, RoutedIncompleteRouteUsesExactLegacyOutput) {
  const Bytes golden = goldenRouteBytes();
  ASSERT_FALSE(golden.empty());
  RouteIndex index;
  ASSERT_EQ(decode(golden, index), DecodeStatus::Ok);
  VectorRouteByteSource source(golden);

  NavState state;
  Frame legacy(792, 528, kSentinel);
  drawLegacy(state, legacy.width, legacy.height, &legacy);

  // Source present, index missing -> incomplete -> legacy.
  Frame noIndex(792, 528, kSentinel);
  NavScreenRenderer::draw(noIndex.pixels(), noIndex.width, noIndex.height, state, &source, nullptr, nullptr);
  EXPECT_EQ(std::memcmp(noIndex.pixels(), legacy.pixels(), rowBytes(792) * 528), 0)
      << "source-without-index must keep the legacy screen";

  // Index present, source missing -> incomplete -> legacy.
  Frame noSource(792, 528, kSentinel);
  NavScreenRenderer::draw(noSource.pixels(), noSource.width, noSource.height, state, nullptr, &index, nullptr);
  EXPECT_EQ(std::memcmp(noSource.pixels(), legacy.pixels(), rowBytes(792) * 528), 0)
      << "index-without-source must keep the legacy screen";
  expectGuardsUntouched(noIndex);
  expectGuardsUntouched(noSource);
}

// ---------------------------------------------------------------------------
// Golden fixture: whole-route world fit. Both segments are sub-pixel clusters
// (segment 1 is deliberately ~9,000 km away), so the routed screen's only map
// ink is the marker on the Amsterdam segment; the Tokyo segment contributes
// nothing and the corridor between them stays empty (never connected).
// ---------------------------------------------------------------------------

TEST(NavigatorRouteScreenTest, GoldenRouteInkStaysInsideMapRectAndMarkerOverlaysAmsterdam792x528) {
  const Bytes golden = goldenRouteBytes();
  ASSERT_FALSE(golden.empty());
  RouteIndex index;
  ASSERT_EQ(decode(golden, index), DecodeStatus::Ok);
  ASSERT_EQ(index.segmentCount, 2U);

  NavState state;
  CurrentPosition position;
  position.point = kAmsterdamPosition;
  position.accuracyMeters = 0;  // ring clamps to the documented minimum

  Frame legacy(792, 528, kSentinel);
  Frame routed(792, 528, kSentinel);
  drawLegacy(state, legacy.width, legacy.height, &legacy);
  VectorRouteByteSource source(golden);
  drawRouted(state, routed.width, routed.height, source, index, &position, &routed);
  expectGuardsUntouched(routed);

  const Geom g(792, 528);

  // Base UI (everything outside the map rectangle) is byte-identical, so the
  // maneuver band, separator, border and bottom status remain untouched.
  expectIdenticalOutsideMapRect(legacy, routed, g);

  // The map area was replaced: the schematic map is gone and something new is
  // painted inside the map rectangle.
  EXPECT_GT(countDifferingInsideMapRect(legacy, routed, g), 0) << "routed map must differ from the schematic";

  // Marker overlays the Amsterdam segment at the exact projected position.
  const RouteViewport viewport = routedViewport(index, g);
  ASSERT_TRUE(viewport.isValid());
  const MarkerExpectation marker = expectedMarker(viewport, g, position);
  EXPECT_GE(marker.ringRadius, 1) << "marker must stay visible inside the map rect";
  expectMarkerRendered(routed, marker, "golden routed marker");

  // No cross-segment connection and no stray route ink: the only ink inside
  // the map rect is the marker cluster on the Amsterdam segment.
  const int mapInk = countLogicalBlack(routed, g.mapX(), g.mapY(), g.mapX() + g.mapW, g.mapY() + g.mapH);
  const int radius = std::max(marker.ringRadius, RouteMapRenderer::kMarkerDiscRadiusPx) + 1;
  const int markerWindow = countLogicalBlack(
      routed, std::max(g.mapX(), marker.cx - radius), std::max(g.mapY(), marker.cy - radius),
      std::min(g.mapX() + g.mapW, marker.cx + radius + 1), std::min(g.mapY() + g.mapH, marker.cy + radius + 1));
  EXPECT_EQ(mapInk, markerWindow) << "route ink found away from the Amsterdam marker (segment join?)";

  // The far-away Tokyo segment itself contributes no ink (its collapsed
  // polyline draws nothing and it is never connected to Amsterdam).
  const ScreenPoint tokyo =
      viewport.project(GeoPoint{index.overview[3].latitudeE5 * 100, index.overview[3].longitudeE5 * 100});
  expectLogicalWhite(routed, g.mapX() + tokyo.x, g.mapY() + tokyo.y, "no ink at the disconnected Tokyo segment");
}

TEST(NavigatorRouteScreenTest, GoldenRouteRoutedScreenSafeAtNonByteAlignedWidth417x240) {
  const Bytes golden = goldenRouteBytes();
  ASSERT_FALSE(golden.empty());
  RouteIndex index;
  ASSERT_EQ(decode(golden, index), DecodeStatus::Ok);

  NavState state;
  CurrentPosition position;
  position.point = kAmsterdamPosition;
  position.accuracyMeters = 0;

  constexpr int kWidth = 417;  // not a multiple of 8
  Frame legacy(kWidth, 240, kSentinel);
  Frame routed(kWidth, 240, kSentinel);
  drawLegacy(state, legacy.width, legacy.height, &legacy);
  VectorRouteByteSource source(golden);
  drawRouted(state, routed.width, routed.height, source, index, &position, &routed);

  const Geom g(kWidth, 240);
  expectIdenticalOutsideMapRect(legacy, routed, g);
  EXPECT_GT(countDifferingInsideMapRect(legacy, routed, g), 0);

  const RouteViewport viewport = routedViewport(index, g);
  ASSERT_TRUE(viewport.isValid());
  const MarkerExpectation marker = expectedMarker(viewport, g, position);
  EXPECT_GE(marker.ringRadius, 1);
  expectMarkerRendered(routed, marker, "golden routed marker 417x240");

  // Unused padding bits of every non-byte-aligned physical row stay white.
  const size_t wb = rowBytes(kWidth);
  const uint8_t padMask = static_cast<uint8_t>(0xFFU >> (kWidth % 8));
  for (int y = 0; y < routed.height; ++y) {
    const uint8_t lastByte = routed.pixels()[static_cast<size_t>(y) * wb + wb - 1U];
    EXPECT_EQ(lastByte & padMask, padMask) << "padding bits of row " << y << " not white";
  }
  expectGuardsUntouched(routed);
}

// ---------------------------------------------------------------------------
// Visible polyline: a local two-segment route at walking scale paints real
// route ink inside the map rectangle and a marker on top of it.
// ---------------------------------------------------------------------------

TEST(NavigatorRouteScreenTest, LocalRoutePolylineInkAppearsInsideMapRectWithMarkerOverlay792x528) {
  const LocalRoute route = LocalRoute::make();
  RouteIndex index;
  ASSERT_EQ(decode(route.bytes, index), DecodeStatus::Ok);
  ASSERT_EQ(index.segmentCount, 2U);

  NavState state;
  CurrentPosition position;
  position.point = route.points[1];  // a vertex on segment 0
  position.accuracyMeters = 30;

  Frame legacy(792, 528, kSentinel);
  Frame routed(792, 528, kSentinel);
  drawLegacy(state, legacy.width, legacy.height, &legacy);
  VectorRouteByteSource source(route.bytes);
  drawRouted(state, routed.width, routed.height, source, index, &position, &routed);
  expectGuardsUntouched(routed);

  const Geom g(792, 528);
  expectIdenticalOutsideMapRect(legacy, routed, g);
  EXPECT_GT(countDifferingInsideMapRect(legacy, routed, g), 0) << "real route must replace the schematic map";

  // Real polyline ink is present inside the map rectangle.
  const int mapInk = countLogicalBlack(routed, g.mapX(), g.mapY(), g.mapX() + g.mapW, g.mapY() + g.mapH);
  EXPECT_GT(mapInk, 300) << "visible route polyline ink missing";

  // The marker is drawn last over the route at the projected vertex.
  const RouteViewport viewport = routedViewport(index, g);
  ASSERT_TRUE(viewport.isValid());
  const MarkerExpectation marker = expectedMarker(viewport, g, position);
  EXPECT_GE(marker.ringRadius, 1);
  expectMarkerRendered(routed, marker, "local routed marker");
}

TEST(NavigatorRouteScreenTest, LocalRouteWithoutPositionDrawsPolylineButNoMarker) {
  const LocalRoute route = LocalRoute::make();
  RouteIndex index;
  ASSERT_EQ(decode(route.bytes, index), DecodeStatus::Ok);

  NavState state;
  Frame legacy(792, 528, kSentinel);
  Frame routed(792, 528, kSentinel);
  drawLegacy(state, legacy.width, legacy.height, &legacy);
  VectorRouteByteSource source(route.bytes);
  drawRouted(state, routed.width, routed.height, source, index, nullptr, &routed);

  const Geom g(792, 528);
  expectIdenticalOutsideMapRect(legacy, routed, g);
  const int mapInk = countLogicalBlack(routed, g.mapX(), g.mapY(), g.mapX() + g.mapW, g.mapY() + g.mapH);
  EXPECT_GT(mapInk, 300) << "polyline ink missing without a position";
}

// ---------------------------------------------------------------------------
// Status screens stay dominant and byte-identical to legacy even when a valid
// route is supplied.
// ---------------------------------------------------------------------------

TEST(NavigatorRouteScreenTest, StatusScreensRemainByteIdenticalWithValidRouteSupplied) {
  const Bytes golden = goldenRouteBytes();
  ASSERT_FALSE(golden.empty());
  RouteIndex index;
  ASSERT_EQ(decode(golden, index), DecodeStatus::Ok);
  VectorRouteByteSource source(golden);
  CurrentPosition position;
  position.point = kAmsterdamPosition;

  NavStatus statuses[] = {NavStatus::Recalculating, NavStatus::OffRoute, NavStatus::Arrived};
  for (const NavStatus status : statuses) {
    NavState state;
    state.status = status;
    Frame legacy(792, 528, kSentinel);
    Frame routed(792, 528, kSentinel);
    drawLegacy(state, legacy.width, legacy.height, &legacy);
    drawRouted(state, routed.width, routed.height, source, index, &position, &routed);
    EXPECT_EQ(std::memcmp(routed.pixels(), legacy.pixels(), rowBytes(792) * 528), 0)
        << "status screen " << static_cast<int>(status) << " must stay byte-identical with a route supplied";
    expectGuardsUntouched(routed);
  }
}

// ---------------------------------------------------------------------------
// Render-failure safety: the legacy schematic map is restored byte-for-byte
// (no second framebuffer) when RouteMapRenderer fails mid-draw.
// ---------------------------------------------------------------------------

TEST(NavigatorRouteScreenTest, RenderFailureRestoresLegacySchematicMapByteIdentical) {
  // RouteMapRenderer streams only the wire *geometry* (never the header), so a
  // local route with visible segment 0 is needed: the renderer draws segment 0
  // and then stalls inside segment 1's absolute anchor, leaving a partially
  // drawn real route that must be restored to the legacy schematic map.
  const LocalRoute route = LocalRoute::make();
  RouteIndex index;
  ASSERT_EQ(decode(route.bytes, index), DecodeStatus::Ok);
  // Geometry layout of the encoded local route: no route name, segment starts
  // at 38..41, segment 0's two deltas at 42..49, segment 1's absolute anchor at
  // 50. A budget of 12 delivers segment 0 fully and stalls mid-anchor.
  ASSERT_EQ(index.segments[1].sourceOffset, 50U);
  StallingSource stalling(route.bytes, 12);

  NavState state;
  CurrentPosition position;
  position.point = route.points[1];

  Frame legacy(792, 528, kSentinel);
  Frame routed(792, 528, kSentinel);
  drawLegacy(state, legacy.width, legacy.height, &legacy);
  drawRouted(state, routed.width, routed.height, stalling, index, &position, &routed);

  EXPECT_EQ(std::memcmp(routed.pixels(), legacy.pixels(), rowBytes(792) * 528), 0)
      << "failed route draw must restore the legacy schematic map";
  expectGuardsUntouched(routed);
}

TEST(NavigatorRouteScreenTest, MalformedIndexKeepsLegacySchematicMapByteIdentical) {
  const Bytes golden = goldenRouteBytes();
  ASSERT_FALSE(golden.empty());
  RouteIndex index;
  ASSERT_EQ(decode(golden, index), DecodeStatus::Ok);
  index.segments[0].startPointIndex = 2;  // violates the decoder invariant -> InvalidIndex

  NavState state;
  CurrentPosition position;
  position.point = kAmsterdamPosition;

  Frame legacy(792, 528, kSentinel);
  Frame routed(792, 528, kSentinel);
  drawLegacy(state, legacy.width, legacy.height, &legacy);
  VectorRouteByteSource source(golden);
  drawRouted(state, routed.width, routed.height, source, index, &position, &routed);

  EXPECT_EQ(std::memcmp(routed.pixels(), legacy.pixels(), rowBytes(792) * 528), 0)
      << "malformed index must leave the legacy schematic map untouched";
  expectGuardsUntouched(routed);
}

// ---------------------------------------------------------------------------
// Optional host preview artifact (mirrors DashboardV3RendererTest's PBM gate).
// Renders the real routed firmware path with a visible local route and writes
// the logical portrait framebuffer as a binary PBM (P4, 528x792) when the
// NAV_PREVIEW_DIR environment variable is set.
// ---------------------------------------------------------------------------

bool physicalPixelIsBlack(const Frame& frame, int x, int y) {
  if (x < 0 || y < 0 || x >= frame.width || y >= frame.height) {
    return false;
  }
  const uint8_t* p = frame.pixels();
  const size_t wb = rowBytes(frame.width);
  const uint8_t byte = p[static_cast<size_t>(y) * wb + static_cast<size_t>(x) / 8U];
  return ((byte >> (7 - (x & 7))) & 1U) == 0U;
}

// Writes the physical landscape framebuffer (792x528) transposed into the
// logical portrait image (528 wide x 792 tall) the navigator user sees.
bool writePortraitPbm(const Frame& frame, const std::string& path) {
  constexpr int kPortraitWidth = 528;   // = physical height of the X3 panel
  constexpr int kPortraitHeight = 792;  // = physical width of the X3 panel
  if (frame.width != kPortraitHeight || frame.height != kPortraitWidth) {
    return false;
  }
  std::ofstream out(path, std::ios::binary);
  if (!out.is_open()) {
    return false;
  }
  out << "P4\n" << kPortraitWidth << " " << kPortraitHeight << "\n";
  constexpr size_t kRowBytes = kPortraitWidth / 8;
  std::vector<uint8_t> row(kRowBytes, 0);
  for (int ly = 0; ly < kPortraitHeight; ++ly) {
    std::fill(row.begin(), row.end(), 0);
    for (int lx = 0; lx < kPortraitWidth; ++lx) {
      // Logical (lx, ly) is stored at physical (ly, physH - 1 - lx).
      if (physicalPixelIsBlack(frame, ly, (frame.height - 1) - lx)) {
        row[static_cast<size_t>(lx) / 8U] |= static_cast<uint8_t>(0x80U >> (lx & 7));
      }
    }
    out.write(reinterpret_cast<const char*>(row.data()), static_cast<std::streamsize>(kRowBytes));
  }
  return out.good();
}

TEST(NavigatorRouteScreenTest, RoutedScreenWritesPbmArtifactWhenEnvDirIsSet) {
  const char* artifactDir = std::getenv("NAV_PREVIEW_DIR");
  if (artifactDir == nullptr || artifactDir[0] == '\0') {
    GTEST_SKIP() << "NAV_PREVIEW_DIR is not set; skipping the PBM artifact write";
  }

  const LocalRoute route = LocalRoute::make();
  RouteIndex index;
  ASSERT_EQ(decode(route.bytes, index), DecodeStatus::Ok);

  NavState state;
  CurrentPosition position;
  position.point = route.points[1];
  position.accuracyMeters = 30;

  Frame routed(792, 528, kSentinel);
  VectorRouteByteSource source(route.bytes);
  drawRouted(state, routed.width, routed.height, source, index, &position, &routed);

  const std::string path = std::string(artifactDir) + "/navigator-route-preview.pbm";
  ASSERT_TRUE(writePortraitPbm(routed, path)) << "could not write " << path;

  // Read the artifact back: header must declare 528x792 and the payload must
  // be one bit per pixel, MSB-first, 66 bytes per row.
  std::ifstream in(path, std::ios::binary);
  ASSERT_TRUE(in.is_open()) << "could not reopen " << path;
  std::string magic;
  std::string dimensions;
  ASSERT_TRUE(std::getline(in, magic));
  ASSERT_TRUE(std::getline(in, dimensions));
  EXPECT_EQ(magic, "P4");
  EXPECT_EQ(dimensions, "528 792");
  in.seekg(0, std::ios::end);
  const std::streamoff fileBytes = in.tellg();
  EXPECT_EQ(fileBytes, std::streamoff(std::string("P4\n528 792\n").size()) + 66 * 792);
}

TEST(NavigatorRouteScreenTest, OverviewUsesRealRouteWithoutInventedGpsAndGuardsBuffer) {
  auto route = LocalRoute::make();
  route.points = {{523600000, 49000000}, {523630000, 49000000}, {523630000, 49040000}, {523680000, 49040000},
                  {523680000, 49100000}, {523720000, 49100000}, {523750000, 49080000}, {523760000, 49000000},
                  {523740000, 48940000}, {523690000, 48940000}, {523670000, 48900000}, {523610000, 48900000}};
  route.bytes = encodeRoute(route.points, {0});
  putU32(route.bytes, 28, 4200);
  putU16(route.bytes, 32, 63);
  route.bytes.resize(route.bytes.size() - 4);
  const auto crc = crc32Of(route.bytes);
  appendU32(route.bytes, crc);
  RouteIndex index;
  ASSERT_EQ(decode(route.bytes, index), DecodeStatus::Ok);
  VectorRouteByteSource source(route.bytes);
  Frame frame(792, 528, kSentinel);
  EXPECT_TRUE(NavScreenRenderer::drawOverview(frame.pixels(), 792, 528, source, index));
  expectGuardsUntouched(frame);
  EXPECT_GT(countLogicalBlack(frame, 10, 140, 508, 540), 100u);
  if (const char* dir = std::getenv("NAV_PREVIEW_DIR")) {
    EXPECT_TRUE(writePortraitPbm(frame, std::string(dir) + "/navigator-overview.pbm"));
  }
}

// The route name was the one range the overview read with a single bare
// read() call, demanding all of it at once. Against a source that splits
// reads the call returned short, and drawOverview answered false: the user
// lost the whole frame, not just the name. Every other range already looped
// through readFully, so this went unnoticed -- the fixtures above all carry
// an empty name, which skips the read entirely.
TEST(NavigatorRouteScreenTest, OverviewNameSurvivesASourceThatSplitsEveryRead) {
  // encodeRoute always emits an empty name, so build one by hand: the name
  // bytes sit directly after the 38-byte header, and header[36] is their
  // count (offset 32 is estimatedMinutes).
  const auto route = LocalRoute::make();
  const std::string name = "RONDJE ZUIDERPARK";
  Bytes bytes = encodeRoute(route.points, route.segmentStarts);
  bytes.resize(bytes.size() - 4);  // drop the crc; the name shifts everything after the header
  bytes.insert(bytes.begin() + 38, name.begin(), name.end());
  bytes[36] = static_cast<uint8_t>(name.size());
  putU32(bytes, 8, static_cast<uint32_t>(bytes.size() + 4));
  appendU32(bytes, crc32Of(bytes));

  RouteIndex index;
  ASSERT_EQ(decode(bytes, index), DecodeStatus::Ok);
  ASSERT_EQ(index.routeNameLength, name.size()) << "the name read must actually be exercised";

  Frame whole(792, 528, kSentinel);
  VectorRouteByteSource wholeSource(bytes);
  ASSERT_TRUE(NavScreenRenderer::drawOverview(whole.pixels(), 792, 528, wholeSource, index));

  Frame split(792, 528, kSentinel);
  ChunkedSource splitSource(bytes, 1);  // one byte per read
  EXPECT_TRUE(NavScreenRenderer::drawOverview(split.pixels(), 792, 528, splitSource, index));
  expectGuardsUntouched(split);
  EXPECT_EQ(whole.bytes, split.bytes) << "a split-read source must render the identical frame";

  // And the name genuinely reaches pixels, so the comparison above is not two
  // identically blank headers agreeing with each other.
  Bytes unnamed = bytes;
  unnamed.resize(unnamed.size() - 4);
  unnamed.erase(unnamed.begin() + 38, unnamed.begin() + 38 + static_cast<long>(name.size()));
  unnamed[36] = 0;
  putU32(unnamed, 8, static_cast<uint32_t>(unnamed.size() + 4));
  appendU32(unnamed, crc32Of(unnamed));
  RouteIndex unnamedIndex;
  ASSERT_EQ(decode(unnamed, unnamedIndex), DecodeStatus::Ok);
  Frame blank(792, 528, kSentinel);
  VectorRouteByteSource unnamedSource(unnamed);
  ASSERT_TRUE(NavScreenRenderer::drawOverview(blank.pixels(), 792, 528, unnamedSource, unnamedIndex));
  EXPECT_NE(whole.bytes, blank.bytes) << "the route name must be painted, or this test proves nothing";
}

TEST(NavigatorRouteScreenTest, OverviewRejectsUnreadableRouteRatherThanShowingExample) {
  const auto route = LocalRoute::make();
  RouteIndex index;
  ASSERT_EQ(decode(route.bytes, index), DecodeStatus::Ok);
  StallingSource source(route.bytes, 0);
  Frame frame(792, 528, kSentinel);
  EXPECT_FALSE(NavScreenRenderer::drawOverview(frame.pixels(), 792, 528, source, index));
  expectGuardsUntouched(frame);
  NavScreenRenderer::drawMessage(frame.pixels(), 792, 528, "KAART NIET LEESBAAR", "PROBEER OPNIEUW");
  expectGuardsUntouched(frame);
}

TEST(NavigatorRouteScreenTest, DigitsHaveReadableNotMirroredStrokes) {
  const char* digits[] = {"3", "6", "7", "9"};
  const uint8_t expected[][7] = {
      {15, 16, 16, 14, 16, 16, 15}, {14, 1, 1, 15, 17, 17, 14}, {31, 16, 8, 4, 2, 2, 2}, {14, 17, 17, 30, 16, 16, 14}};
  for (int digit = 0; digit < 4; ++digit) {
    Frame frame(792, 528, kSentinel);
    NavScreenRenderer::drawMessage(frame.pixels(), 792, 528, digits[digit], "");
    for (int row = 0; row < 7; ++row)
      for (int col = 0; col < 5; ++col) {
        EXPECT_EQ(logicalPixelIsBlack(frame, 254 + 4 * col, 264 + 4 * row), (expected[digit][row] & (1 << col)) != 0)
            << digits[digit] << " row " << row;
      }
  }
}

TEST(NavigatorRouteScreenTest, MapLettersHaveReadableStrokes) {
  const char* letters[] = {"C", "J", "S"};
  const uint8_t expected[][7] = {{14, 17, 1, 1, 1, 17, 14}, {30, 16, 16, 16, 16, 17, 14}, {30, 1, 1, 14, 16, 16, 15}};
  for (int letter = 0; letter < 3; ++letter) {
    Frame frame(792, 528, kSentinel);
    NavScreenRenderer::drawMessage(frame.pixels(), 792, 528, letters[letter], "");
    for (int row = 0; row < 7; ++row)
      for (int col = 0; col < 5; ++col)
        EXPECT_EQ(logicalPixelIsBlack(frame, 254 + 4 * col, 264 + 4 * row), (expected[letter][row] & (1 << col)) != 0);
  }
}

// ---------------------------------------------------------------------------
// Task 8: fixed maneuver instruction band on the production overview.
//
// drawOverview must reserve a fixed top band above the map when it is handed
// a non-null maneuver presentation and the validated route declares maneuvers,
// and must hand back the RouteProximity it already computed while drawing.
// NavigatorMain then updates its RouteManeuverSelector from that proximity and
// paints the band into the reserved area with drawManeuverBand. These tests
// drive that contract with real encoded routes and (for the four-gray planes)
// a synthetic all-white X3GM tile set.
// ---------------------------------------------------------------------------

// All-empty X3GM v1 grid (white tiles, no labels): every cell entry is zero,
// so the file is just the 48-byte header plus the 12-byte cell table.
Bytes encodeGrayMap(int32_t southE7, int32_t westE7, uint16_t rows, uint16_t cols) {
  const uint32_t cells = uint32_t(rows) * uint32_t(cols);
  const uint32_t data = 48 + cells * 12;
  Bytes bytes(data, 0);
  bytes[0] = 'X';
  bytes[1] = '3';
  bytes[2] = 'G';
  bytes[3] = 'M';
  putU16(bytes, 4, 1);   // version
  putU16(bytes, 6, 48);  // header size
  putU32(bytes, 8, data);
  putI32(bytes, 12, southE7);
  putI32(bytes, 16, westE7);
  putU32(bytes, 20, 100000);  // cell resolution (0.01 degree)
  putU16(bytes, 24, rows);
  putU16(bytes, 26, cols);
  putU16(bytes, 28, 512);
  putU16(bytes, 30, 832);
  putU32(bytes, 32, 48);  // label header offset (this encoder has none)
  putU32(bytes, 36, data);
  const Bytes entries(bytes.begin() + 48, bytes.end());
  putU32(bytes, 40, crc32Of(entries));
  const Bytes head(bytes.begin(), bytes.begin() + 44);
  putU32(bytes, 44, crc32Of(head));
  return bytes;
}

// X3GM v1 encoder with real raster payload tiles, for renderer-behaviour
// tests that need tone content or label payloads (encodeGrayMap above only
// produces empty all-white cells). `paint` fills every raster byte of each
// populated cell (0x55 = solid water/dark-gray tone 1, 0xFF = white). The
// populated cells cover rows [r0, r1] x columns [c0, c1]; when `labelAt` is
// non-null the first populated cell also carries one valid 48-byte label
// anchored at that E7 point.
Bytes encodeGrayMapData(int32_t southE7, int32_t westE7, uint16_t rows, uint16_t cols, int r0, int c0, int r1,
                        int c1, uint8_t paint, const GeoPoint* labelAt = nullptr) {
  constexpr uint32_t kTileBytes = 512u * 832u / 4u;
  const uint32_t cells = uint32_t(rows) * uint32_t(cols);
  const uint32_t populated = uint32_t(r1 - r0 + 1) * uint32_t(c1 - c0 + 1);
  const uint32_t labelCount = labelAt ? 1u : 0u;
  const uint32_t payloadPerCell = kTileBytes + labelCount * 48;
  const uint32_t data = 48 + cells * 12;
  Bytes bytes(data + populated * payloadPerCell, 0);
  bytes[0] = 'X';
  bytes[1] = '3';
  bytes[2] = 'G';
  bytes[3] = 'M';
  putU16(bytes, 4, 1);   // version
  putU16(bytes, 6, 48);  // header size
  putU32(bytes, 8, static_cast<uint32_t>(bytes.size()));
  putI32(bytes, 12, southE7);
  putI32(bytes, 16, westE7);
  putU32(bytes, 20, 100000);  // cell resolution (0.01 degree)
  putU16(bytes, 24, rows);
  putU16(bytes, 26, cols);
  putU16(bytes, 28, 512);
  putU16(bytes, 30, 832);
  putU32(bytes, 32, 48);  // label header offset (fixed)
  putU32(bytes, 36, data);
  uint32_t next = data;
  for (int r = r0; r <= r1; ++r) {
    for (int c = c0; c <= c1; ++c) {
      const uint32_t cellIndex = uint32_t(r) * cols + uint32_t(c);
      std::fill(bytes.begin() + next, bytes.begin() + next + kTileBytes, paint);
      if (labelAt != nullptr) {
        const uint32_t at = next + kTileBytes;
        putI32(bytes, at, labelAt->latitudeE7);
        putI32(bytes, at + 4, labelAt->longitudeE7);
        const char text[] = "TEST";
        std::memcpy(bytes.data() + at + 8, text, sizeof(text));
      }
      putU32(bytes, 48 + cellIndex * 12, next);
      putU32(bytes, 48 + cellIndex * 12 + 4, labelCount);
      const Bytes payload(bytes.begin() + next, bytes.begin() + next + payloadPerCell);
      putU32(bytes, 48 + cellIndex * 12 + 8, crc32Of(payload));
      next += payloadPerCell;
    }
  }
  const Bytes entries(bytes.begin() + 48, bytes.begin() + data);
  putU32(bytes, 40, crc32Of(entries));
  const Bytes head(bytes.begin(), bytes.begin() + 44);
  putU32(bytes, 44, crc32Of(head));
  return bytes;
}

// Route-package sources and regional map sources are deliberately independent
// interfaces (the regional maps may be much larger). The gray overview tests
// need an in-memory WalkMapByteSource over the synthetic X3GM fixture.
class VectorWalkMapByteSource : public navigator::WalkMapByteSource {
 public:
  explicit VectorWalkMapByteSource(Bytes bytes) : bytes_(std::move(bytes)) {}

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

// A local, maneuver-carrying route near Amsterdam at walking scale. The three
// wire maneuvers (left at 200 m, right at 480 m, arrival near the end) sit at
// plausible distances on the segment; names are empty this milestone.
struct TurnRoute {
  std::vector<GeoPoint> points;
  std::vector<uint16_t> segmentStarts;
  Bytes bytes;

  static TurnRoute make(uint32_t totalMeters) {
    TurnRoute route;
    route.points = {
        GeoPoint{523'676'000, 49'041'000}, GeoPoint{523'696'000, 49'061'000}, GeoPoint{523'716'000, 49'081'000},
        GeoPoint{523'736'000, 49'101'000}, GeoPoint{523'756'000, 49'121'000}, GeoPoint{523'776'000, 49'141'000},
    };
    route.segmentStarts = {0};
    const std::vector<ManeuverSpec> maneuvers = {
        {1, 1, 200},   // left
        {2, 3, 480},   // right
        {6, 5, 1180},  // arrival at the end
    };
    route.bytes = encodeRoute(route.points, route.segmentStarts, maneuvers);
    putU32(route.bytes, 28, totalMeters);
    putU16(route.bytes, 32, 45);
    route.bytes.resize(route.bytes.size() - 4);
    const uint32_t crc = crc32Of(route.bytes);
    appendU32(route.bytes, crc);
    return route;
  }
};

int overviewHeaderTop(bool gray) {
  return gray ? NavScreenRenderer::kOverviewHeaderGrayPx : NavScreenRenderer::kOverviewHeaderPlainPx;
}

int overviewBandHeight(int physW) { return physW * NavScreenRenderer::kManeuverBandHeightPercent / 100; }

// Logical rows [ly0, logical height) must be byte-identical between two
// frames. The overview chrome below the map (attribution, separators, metric
// columns and the status line) is fixed, so a reserved band must never change
// a single pixel of it.
void expectChromeIdenticalBelow(const Frame& withBand, const Frame& mapOnly, int ly0) {
  ASSERT_EQ(withBand.width, mapOnly.width);
  ASSERT_EQ(withBand.height, mapOnly.height);
  for (int ly = ly0; ly < mapOnly.width; ++ly) {
    for (int lx = 0; lx < mapOnly.height; ++lx) {
      EXPECT_EQ(logicalPixelIsBlack(withBand, lx, ly), logicalPixelIsBlack(mapOnly, lx, ly))
          << "chrome changed at logical (" << lx << "," << ly << ")";
    }
  }
}

// End-to-end mirror of the sequence NavigatorMain performs for one frame.
// It lives here because NavigatorMain sits behind CROSSINK_NAVIGATOR and
// pulls in Arduino, the display driver and I18n, so no host build compiles
// it; this test is what stands in for that compilation. Until this wiring
// existed drawManeuverBand had zero callers and the band was never reserved:
// the panel showed the map and the walker, never the turn.
TEST(NavigatorRouteScreenTest, TheNavigatorMainSequencePaintsAnInstructionBand) {
  const auto route = TurnRoute::make(1200);
  RouteIndex index;
  ASSERT_EQ(decode(route.bytes, index), DecodeStatus::Ok);
  ASSERT_EQ(index.maneuverCount, 3U);
  VectorRouteByteSource source(route.bytes);

  ManeuverBandPlanner planner;
  ASSERT_TRUE(ManeuverBandPlanner::reservesBand(index)) << "a route with maneuvers must get a band";

  // 1. Reserve: a non-null presentation shortens the map before any ink.
  NavManeuverPresentation presentation{};
  RouteProximity proximity{};
  Frame frame(792, 528, kSentinel);
  ASSERT_TRUE(NavScreenRenderer::drawOverview(frame.pixels(), 792, 528, source, index, nullptr, nullptr, "STATUS",
                                              "ROUTE", nullptr, NavGrayPlane::Base, nullptr, &presentation,
                                              &proximity, nullptr, true));

  // 2. Plan from the proximity that pass already computed. Without a fix the
  //    countdown runs from the route start to the first turn.
  const auto plan = planner.plan(index, nullptr, proximity);
  ASSERT_TRUE(plan.visible);
  EXPECT_EQ(plan.maneuver, Maneuver::Left);
  EXPECT_EQ(plan.distanceMeters, 200);
  presentation.maneuver = plan.maneuver;
  presentation.distanceMeters = plan.distanceMeters;

  // 3. Paint. The reserved band is still empty at this point, so any ink it
  //    gains is the instruction itself.
  const int bandTop = overviewHeaderTop(true);
  const int bandBottom = bandTop + overviewBandHeight(792);
  const int reservedButEmpty = countLogicalBlack(frame, 0, bandTop, 528, bandBottom);
  NavScreenRenderer::drawManeuverBand(frame.pixels(), 792, 528, presentation, NavGrayPlane::Base, true);
  EXPECT_GT(countLogicalBlack(frame, 0, bandTop, 528, bandBottom), reservedButEmpty)
      << "the reserved band never received the instruction";
  expectGuardsUntouched(frame);
}

TEST(NavigatorRouteScreenTest, OverviewNullManeuverPresentationKeepsMapOnlyBytes) {
  const auto route = TurnRoute::make(5000);
  RouteIndex index;
  ASSERT_EQ(decode(route.bytes, index), DecodeStatus::Ok);
  ASSERT_EQ(index.maneuverCount, 3U);
  VectorRouteByteSource source(route.bytes);

  // The defaulted call and an explicit null presentation must be identical on
  // a route that declares maneuvers: the null path never reserves the band.
  Frame defaults(792, 528, kSentinel);
  EXPECT_TRUE(NavScreenRenderer::drawOverview(defaults.pixels(), 792, 528, source, index));
  Frame explicitNull(792, 528, kSentinel);
  navigator::NavManeuverPresentation presentation;
  EXPECT_TRUE(NavScreenRenderer::drawOverview(explicitNull.pixels(), 792, 528, source, index, nullptr, nullptr, nullptr,
                                              nullptr, nullptr, navigator::NavGrayPlane::Base, nullptr, nullptr,
                                              nullptr));
  EXPECT_EQ(std::memcmp(defaults.pixels(), explicitNull.pixels(), rowBytes(792) * 528), 0)
      << "null presentation changed the map-only geometry";

  // A route without maneuvers never reserves either, even when the caller
  // passes a presentation: the map keeps its exact layout.
  const auto noManeuvers = LocalRoute::make();
  RouteIndex plainIndex;
  ASSERT_EQ(decode(noManeuvers.bytes, plainIndex), DecodeStatus::Ok);
  ASSERT_EQ(plainIndex.maneuverCount, 0U);
  VectorRouteByteSource plainSource(noManeuvers.bytes);
  Frame mapOnly(792, 528, kSentinel);
  EXPECT_TRUE(NavScreenRenderer::drawOverview(mapOnly.pixels(), 792, 528, plainSource, plainIndex));
  Frame withPresentation(792, 528, kSentinel);
  EXPECT_TRUE(NavScreenRenderer::drawOverview(withPresentation.pixels(), 792, 528, plainSource, plainIndex, nullptr,
                                              nullptr, nullptr, nullptr, nullptr, navigator::NavGrayPlane::Base,
                                              nullptr, &presentation, nullptr));
  EXPECT_EQ(std::memcmp(mapOnly.pixels(), withPresentation.pixels(), rowBytes(792) * 528), 0)
      << "presentation on a maneuver-less route must keep the map-only layout";
  expectGuardsUntouched(defaults);
  expectGuardsUntouched(withPresentation);
}

TEST(NavigatorRouteScreenTest, OverviewWritesAlreadyComputedRouteProximity) {
  const auto route = TurnRoute::make(5000);
  RouteIndex index;
  ASSERT_EQ(decode(route.bytes, index), DecodeStatus::Ok);
  VectorRouteByteSource source(route.bytes);

  // A fix standing on the route start projects onto the route: the proximity
  // the overview computed internally (and now hands back) is valid, close and
  // reports a bounded remaining distance.
  CurrentPosition position;
  position.point = route.points[0];
  position.accuracyMeters = 5;
  navigator::RouteProximity proximity;
  Frame frame(792, 528, kSentinel);
  EXPECT_TRUE(NavScreenRenderer::drawOverview(frame.pixels(), 792, 528, source, index, nullptr, &position, "STATUS",
                                              nullptr, nullptr, navigator::NavGrayPlane::Base, nullptr, nullptr,
                                              &proximity));
  ASSERT_TRUE(proximity.valid);
  EXPECT_LE(proximity.distanceMeters, 40U) << "fix on the route must be within the trust guard";
  EXPECT_GT(proximity.remainingDistanceMeters, 0U);
  EXPECT_LE(proximity.remainingDistanceMeters, index.totalDistanceMeters);

  // The proximity is the same value the footer metrics are derived from.
  const navigator::NavFooterMetrics metrics = NavScreenRenderer::chooseFooterMetrics(&position, proximity, index);
  EXPECT_EQ(metrics.mode, navigator::NavMetricMode::Remaining);
  EXPECT_EQ(metrics.distanceMeters, std::min(proximity.remainingDistanceMeters, index.totalDistanceMeters));

  // Without a live fix the geometry pass cannot produce a proximity.
  navigator::RouteProximity noFixProximity;
  Frame noFixFrame(792, 528, kSentinel);
  EXPECT_TRUE(NavScreenRenderer::drawOverview(noFixFrame.pixels(), 792, 528, source, index, nullptr, nullptr, nullptr,
                                              nullptr, nullptr, navigator::NavGrayPlane::Base, nullptr, nullptr,
                                              &noFixProximity));
  EXPECT_FALSE(noFixProximity.valid);

  // A failed draw leaves the caller's proximity untouched.
  navigator::RouteProximity untouched;
  untouched.valid = true;
  untouched.distanceMeters = 777;
  StallingSource stalling(route.bytes, 0);
  Frame failedFrame(792, 528, kSentinel);
  EXPECT_FALSE(NavScreenRenderer::drawOverview(failedFrame.pixels(), 792, 528, stalling, index, nullptr, &position,
                                               nullptr, nullptr, nullptr, navigator::NavGrayPlane::Base, nullptr,
                                               nullptr, &untouched));
  EXPECT_TRUE(untouched.valid);
  EXPECT_EQ(untouched.distanceMeters, 777U);
}

TEST(NavigatorRouteScreenTest, OverviewReservesFixedBandAboveMapWithoutTouchingFooter) {
  const auto route = TurnRoute::make(5000);
  RouteIndex index;
  ASSERT_EQ(decode(route.bytes, index), DecodeStatus::Ok);
  VectorRouteByteSource source(route.bytes);

  CurrentPosition position;
  position.point = route.points[0];
  position.accuracyMeters = 5;
  navigator::NavManeuverPresentation presentation;
  presentation.maneuver = navigator::Maneuver::Left;
  presentation.distanceMeters = 200;
  presentation.action = "LINKS";

  Frame mapOnly(792, 528, kSentinel);
  EXPECT_TRUE(NavScreenRenderer::drawOverview(mapOnly.pixels(), 792, 528, source, index, nullptr, &position, "STATUS"));

  navigator::RouteProximity proximity;
  Frame withBand(792, 528, kSentinel);
  EXPECT_TRUE(NavScreenRenderer::drawOverview(withBand.pixels(), 792, 528, source, index, nullptr, &position, "STATUS",
                                              nullptr, nullptr, navigator::NavGrayPlane::Base, nullptr, &presentation,
                                              &proximity));
  ASSERT_TRUE(proximity.valid);
  NavScreenRenderer::drawManeuverBand(withBand.pixels(), 792, 528, presentation, navigator::NavGrayPlane::Base, false);
  expectGuardsUntouched(withBand);

  // The plain-vector overview chrome below its map (rows LH-100 and below)
  // stays byte-identical: the band never touches attribution or status text.
  const int chromeTop = 792 - NavScreenRenderer::kOverviewFooterPlainPx;
  expectChromeIdenticalBelow(withBand, mapOnly, chromeTop);

  // The fixed band is reserved above the map: band ink lives in the reserved
  // rows and real route ink still appears below the band and above the chrome.
  const int bandTop = overviewHeaderTop(false);
  const int bandH = overviewBandHeight(792);
  EXPECT_GT(countLogicalBlack(withBand, 0, bandTop, 528, bandTop + bandH), 400) << "band content missing";
  EXPECT_GT(countLogicalBlack(withBand, 16, bandTop + bandH + 4, 512, chromeTop), 100)
      << "route map missing below band";
}

TEST(NavigatorRouteScreenTest, GrayOverviewBandReservedAndConfinedInEveryPlane) {
  const auto route = TurnRoute::make(5000);
  RouteIndex index;
  ASSERT_EQ(decode(route.bytes, index), DecodeStatus::Ok);
  VectorRouteByteSource source(route.bytes);

  // A synthetic all-white 6x6 X3GM grid around the local route lets the
  // four-gray overview draw its Base/LSB/MSB passes without SD fixtures.
  const Bytes grayBytes = encodeGrayMap(523'400'000, 48'800'000, 6, 6);
  VectorWalkMapByteSource graySource(grayBytes);
  navigator::GrayMap grayMap;
  ASSERT_EQ(grayMap.open(graySource), navigator::WalkMapStatus::Ok);
  navigator::GrayMapLayer grayLayer;
  grayLayer.source = &graySource;
  grayLayer.map = &grayMap;

  CurrentPosition position;
  position.point = route.points[0];
  position.accuracyMeters = 5;
  navigator::NavManeuverPresentation presentation;
  presentation.maneuver = navigator::Maneuver::Right;
  presentation.distanceMeters = 480;
  presentation.action = "RECHTS";

  const navigator::NavGrayPlane planes[] = {navigator::NavGrayPlane::Base, navigator::NavGrayPlane::Lsb,
                                            navigator::NavGrayPlane::Msb};
  const int bandTop = overviewHeaderTop(true);
  const int bandH = overviewBandHeight(792);
  const int chromeTop = 792 - NavScreenRenderer::kOverviewFooterGrayPx;
  for (const navigator::NavGrayPlane plane : planes) {
    SCOPED_TRACE("plane=" + std::to_string(static_cast<int>(plane)));

    Frame mapOnly(792, 528, kSentinel);
    navigator::GrayMapLayer mapOnlyLayer = grayLayer;
    EXPECT_TRUE(NavScreenRenderer::drawOverview(mapOnly.pixels(), 792, 528, source, index, nullptr, &position, "STATUS",
                                                nullptr, &mapOnlyLayer, plane));
    EXPECT_EQ(mapOnlyLayer.status, navigator::WalkMapStatus::Ok);

    navigator::RouteProximity proximity;
    Frame withBand(792, 528, kSentinel);
    navigator::GrayMapLayer withBandLayer = grayLayer;
    EXPECT_TRUE(NavScreenRenderer::drawOverview(withBand.pixels(), 792, 528, source, index, nullptr, &position,
                                                "STATUS", nullptr, &withBandLayer, plane, nullptr, &presentation,
                                                &proximity));
    EXPECT_EQ(withBandLayer.status, navigator::WalkMapStatus::Ok);
    ASSERT_TRUE(proximity.valid);
    NavScreenRenderer::drawManeuverBand(withBand.pixels(), 792, 528, presentation, plane, true);
    expectGuardsUntouched(withBand);

    // The gray overview chrome below the map stays byte-identical.
    expectChromeIdenticalBelow(withBand, mapOnly, chromeTop);

    // The reserved rows above the (shortened) map carry the band in every
    // plane: black ink in Base, white mask ink in LSB/MSB.
    int bandInk = 0;
    int mapInk = 0;
    for (int ly = bandTop; ly < bandTop + bandH; ++ly) {
      for (int lx = 0; lx < 528; ++lx) {
        const bool black = logicalPixelIsBlack(withBand, lx, ly);
        bandInk += (plane == navigator::NavGrayPlane::Base ? black : !black) ? 1 : 0;
      }
    }
    for (int ly = bandTop + bandH + 4; ly < chromeTop; ++ly) {
      for (int lx = 0; lx < 528; ++lx) {
        const bool black = logicalPixelIsBlack(withBand, lx, ly);
        mapInk += (plane == navigator::NavGrayPlane::Base ? black : !black) ? 1 : 0;
      }
    }
    EXPECT_GT(bandInk, 400) << "band content missing in plane " << static_cast<int>(plane);
    EXPECT_GT(mapInk, 100) << "gray map missing below the band in plane " << static_cast<int>(plane);
  }
}

// ---------------------------------------------------------------------------
// Task: compact four-gray overview chrome.
//
// The four-gray overview replaces the oversized route-name header and the
// two-column value/caption footer with one compact centered header line and
// one compact single-row summary (distance, minutes, compact GPS status), so
// the map area is maximized. Attribution stays; the plain layout is untouched.
// ---------------------------------------------------------------------------

TEST(NavigatorRouteScreenTest, GrayOverviewCompactChromePaintsSingleRowFooterAndMaximizedMap) {
  const auto route = TurnRoute::make(5000);
  RouteIndex index;
  ASSERT_EQ(decode(route.bytes, index), DecodeStatus::Ok);
  VectorRouteByteSource source(route.bytes);

  // A synthetic all-white X3GM grid makes the gray footer/attribution path
  // reachable without SD fixtures.
  const Bytes grayBytes = encodeGrayMap(523'400'000, 48'800'000, 6, 6);
  VectorWalkMapByteSource graySource(grayBytes);
  navigator::GrayMap grayMap;
  ASSERT_EQ(grayMap.open(graySource), navigator::WalkMapStatus::Ok);
  navigator::GrayMapLayer grayLayer;
  grayLayer.source = &graySource;
  grayLayer.map = &grayMap;

  CurrentPosition position;
  position.point = route.points[0];
  position.accuracyMeters = 5;
  const navigator::NavMapText mapText{nullptr};

  Frame frame(792, 528, kSentinel);
  EXPECT_TRUE(NavScreenRenderer::drawOverview(frame.pixels(), 792, 528, source, index, nullptr, &position, "GPS OK",
                                              nullptr, &grayLayer, navigator::NavGrayPlane::Base, &mapText));
  EXPECT_EQ(grayLayer.status, navigator::WalkMapStatus::Ok);
  expectGuardsUntouched(frame);

  // Compact chrome geometry: the map runs from the small gray header top down
  // to the compact gray footer chrome (LH - kOverviewFooterGrayPx).
  const int mapY = NavScreenRenderer::kOverviewHeaderGrayPx;
  const int chromeTop = 792 - NavScreenRenderer::kOverviewFooterGrayPx;
  const int lh = 792;

  // Attribution line preserved in the footer chrome, above the summary row.
  EXPECT_GT(countLogicalBlack(frame, 16, chromeTop + 2, 512, lh - 40), 8) << "gray attribution missing";

  // One compact single-row summary (distance, minutes, GPS status) near the
  // bottom of the same chrome.
  EXPECT_GT(countLogicalBlack(frame, 16, lh - 40, 512, lh - 2), 40) << "compact footer summary row missing";

  // The map itself still renders between the compact header and the compact
  // footer, and the chrome top now sits below the historical 144 px footer so
  // the map region is materially taller.
  EXPECT_GT(countLogicalBlack(frame, 16, mapY + 4, 512, chromeTop), 100) << "route map missing";
  EXPECT_GT(chromeTop, lh - 120) << "gray footer chrome did not shrink for the compact single-row layout";
}

// ---------------------------------------------------------------------------
// Correction: subordinate water and no labels on the calm four-gray map.
//
// On the physical four-gray panel the dark-gray water raster dominates the
// calm overview. The renderer must lighten the water tone with the same
// stable white-mix it already applies to green/open land, and it must never
// paint the tile label payloads that the vector detail layer would label -
// labels stay readable there, but on the calm gray background they crowd the
// panel. Both properties are renderer-behaviour level: they are asserted on
// the exact Base-plane pixels drawOverview submits.
// ---------------------------------------------------------------------------

TEST(NavigatorRouteScreenTest, GrayWaterToneIsSubordinateInsteadOfSolidDarkOnBase) {
  const auto route = LocalRoute::make();
  RouteIndex index;
  ASSERT_EQ(decode(route.bytes, index), DecodeStatus::Ok);
  VectorRouteByteSource source(route.bytes);

  // A full-coverage X3GM grid whose raster is solid water (tone 1, 0x55):
  // the whole visible map area is dark-gray water with only the GPX route on
  // top, so the Base-plane ink fraction measures exactly how the water tone
  // is mapped.
  const Bytes waterBytes = encodeGrayMapData(523'400'000, 48'800'000, 6, 6, 0, 0, 5, 5, 0x55);
  VectorWalkMapByteSource waterSource(waterBytes);
  navigator::GrayMap waterMap;
  ASSERT_EQ(waterMap.open(waterSource), navigator::WalkMapStatus::Ok);
  navigator::GrayMapLayer waterLayer;
  waterLayer.source = &waterSource;
  waterLayer.map = &waterMap;

  Frame frame(792, 528, kSentinel);
  EXPECT_TRUE(NavScreenRenderer::drawOverview(frame.pixels(), 792, 528, source, index, nullptr, nullptr, "STATUS",
                                              nullptr, &waterLayer, navigator::NavGrayPlane::Base));
  EXPECT_EQ(waterLayer.status, navigator::WalkMapStatus::Ok);
  expectGuardsUntouched(frame);

  // Whole-route overview of this local route stays inside the tile budget
  // (the existing all-white gray tests use the same 6x6 grid), so the whole
  // map rectangle below the compact header is water raster plus route.
  const int mapY = NavScreenRenderer::kOverviewHeaderGrayPx;
  const int chromeTop = 792 - NavScreenRenderer::kOverviewFooterGrayPx;
  const int area = (512 - 16) * (chromeTop - mapY);
  const int black = countLogicalBlack(frame, 16, mapY, 512, chromeTop);
  // Solid dark water would fill the map area (fraction ~1.0). The corrected
  // mapping mixes the water cells with white in stable 2x2 blocks, so the
  // Base plane carries roughly half the ink and the water recedes while the
  // black route stays the strongest element.
  EXPECT_GT(black, area / 3) << "water raster missing after lightening";
  EXPECT_LT(black, area * 2 / 3) << "water is still solid dark on the Base plane";
}

TEST(NavigatorRouteScreenTest, CalmGrayMapCarryingTileLabelsDrawsNoMapLabel) {
  const auto route = LocalRoute::make();
  RouteIndex index;
  ASSERT_EQ(decode(route.bytes, index), DecodeStatus::Ok);
  VectorRouteByteSource source(route.bytes);

  // Cell (2,2) of the standard 6x6 grid (south 5234, west 4880) lies inside
  // the route's whole-route view. One fixture gives that cell a white raster
  // payload plus a valid label anchored on the route's second vertex (well
  // inside the map and clear of the renderer's reserved centre band); the
  // other gives it the identical raster without the label. Everything else
  // stays an empty white cell, so the only possible pixel difference is the
  // label.
  const GeoPoint anchor{523'696'000, 49'061'000};
  const Bytes withLabelBytes = encodeGrayMapData(523'400'000, 48'800'000, 6, 6, 2, 2, 2, 2, 0xFF, &anchor);
  const Bytes plainBytes = encodeGrayMapData(523'400'000, 48'800'000, 6, 6, 2, 2, 2, 2, 0xFF);

  Frame labelled(792, 528, kSentinel);
  VectorWalkMapByteSource labelledSource(withLabelBytes);
  navigator::GrayMap labelledMap;
  ASSERT_EQ(labelledMap.open(labelledSource), navigator::WalkMapStatus::Ok);
  navigator::GrayMapLayer labelledLayer;
  labelledLayer.source = &labelledSource;
  labelledLayer.map = &labelledMap;
  EXPECT_TRUE(NavScreenRenderer::drawOverview(labelled.pixels(), 792, 528, source, index, nullptr, nullptr, "STATUS",
                                              nullptr, &labelledLayer, navigator::NavGrayPlane::Base));
  EXPECT_EQ(labelledLayer.status, navigator::WalkMapStatus::Ok);

  Frame plain(792, 528, kSentinel);
  VectorWalkMapByteSource plainSource(plainBytes);
  navigator::GrayMap plainMap;
  ASSERT_EQ(plainMap.open(plainSource), navigator::WalkMapStatus::Ok);
  navigator::GrayMapLayer plainLayer;
  plainLayer.source = &plainSource;
  plainLayer.map = &plainMap;
  EXPECT_TRUE(NavScreenRenderer::drawOverview(plain.pixels(), 792, 528, source, index, nullptr, nullptr, "STATUS",
                                              nullptr, &plainLayer, navigator::NavGrayPlane::Base));
  EXPECT_EQ(plainLayer.status, navigator::WalkMapStatus::Ok);

  expectGuardsUntouched(labelled);
  expectGuardsUntouched(plain);
  // The calm four-gray map never paints tile labels: the frame with a label
  // payload in its raster is byte-identical to the frame without one.
  EXPECT_EQ(std::memcmp(labelled.pixels(), plain.pixels(), rowBytes(792) * 528), 0)
      << "the calm gray map painted a tile label";
}

// ---------------------------------------------------------------------------
// Correction: the compact Overview never depends on GPS or on gray coverage.
//
// NavigatorMain now opens the calm gray background for both navigation views
// regardless of a live fix and always draws the compact four-gray chrome. A
// whole-route Overview therefore keeps that chrome (a) without any GPS fix,
// (b) with the gray raster when the route fits the tile budget, and (c) as a
// clean white route-only map when the raster is unavailable or the route
// exceeds the map's tile budget. The legacy plain-vector "ROUTE OP X3" screen
// is never an ordinary fallback for these frames.
// ---------------------------------------------------------------------------

TEST(NavigatorRouteScreenTest, CompactOverviewWithoutGrayLayerOrGpsKeepsCompactChromeAndWhiteRouteOnlyMap) {
  const auto route = LocalRoute::make();
  RouteIndex index;
  ASSERT_EQ(decode(route.bytes, index), DecodeStatus::Ok);
  VectorRouteByteSource source(route.bytes);

  const char* status = "WACHT OP GPS";
  Frame compactWhite(792, 528, kSentinel);
  EXPECT_TRUE(NavScreenRenderer::drawOverview(compactWhite.pixels(), 792, 528, source, index, nullptr, nullptr, status,
                                              nullptr, nullptr, navigator::NavGrayPlane::Base, nullptr, nullptr,
                                              nullptr, nullptr, true));
  expectGuardsUntouched(compactWhite);

  // No legacy plain-vector title in the historical header block (20,24).
  EXPECT_EQ(countLogicalBlack(compactWhite, 20, 24, 260, 56), 0) << "legacy 'ROUTE OP X3' title must not appear";

  // The compact chrome geometry is exactly the four-gray layout: the map runs
  // from the small gray header to the compact gray footer chrome.
  const int mapY = NavScreenRenderer::kOverviewHeaderGrayPx;
  const int chromeTop = 792 - NavScreenRenderer::kOverviewFooterGrayPx;
  const int lh = 792;

  // Compact single-row footer summary (distance, minutes, GPS status).
  EXPECT_GT(countLogicalBlack(compactWhite, 16, lh - 40, 512, lh - 2), 40) << "compact footer summary row missing";
  // No OSM attribution: without a successfully drawn gray/vector background
  // the white route-only map draws no background attribution line.
  EXPECT_EQ(countLogicalBlack(compactWhite, 16, chromeTop + 2, 512, lh - 40), 0)
      << "attribution must not appear on the white route-only map";
  // The route itself is the only map ink.
  EXPECT_GT(countLogicalBlack(compactWhite, 16, mapY + 4, 512, chromeTop - 4), 100) << "route map missing";
}

TEST(NavigatorRouteScreenTest, CompactOverviewWithoutGpsStillDrawsCalmGrayRaster) {
  const auto route = LocalRoute::make();
  RouteIndex index;
  ASSERT_EQ(decode(route.bytes, index), DecodeStatus::Ok);
  VectorRouteByteSource source(route.bytes);

  const Bytes grayBytes = encodeGrayMap(523'400'000, 48'800'000, 6, 6);
  VectorWalkMapByteSource graySource(grayBytes);
  navigator::GrayMap grayMap;
  ASSERT_EQ(grayMap.open(graySource), navigator::WalkMapStatus::Ok);
  navigator::GrayMapLayer grayLayer;
  grayLayer.source = &graySource;
  grayLayer.map = &grayMap;

  // No live fix (the exact no/expired-GPS Overview case): the frame still
  // uses the compact chrome with the calm gray raster, never the legacy
  // plain-vector overview.
  Frame frame(792, 528, kSentinel);
  EXPECT_TRUE(NavScreenRenderer::drawOverview(frame.pixels(), 792, 528, source, index, nullptr, nullptr, "WACHT OP GPS",
                                              nullptr, &grayLayer, navigator::NavGrayPlane::Base, nullptr, nullptr,
                                              nullptr, nullptr, true));
  EXPECT_EQ(grayLayer.status, navigator::WalkMapStatus::Ok);
  expectGuardsUntouched(frame);
  EXPECT_EQ(countLogicalBlack(frame, 20, 24, 260, 56), 0) << "legacy 'ROUTE OP X3' title must not appear";
  EXPECT_GT(countLogicalBlack(frame, 16, 792 - NavScreenRenderer::kOverviewFooterGrayPx + 2, 512, 792 - 2), 40)
      << "compact footer summary row missing without GPS";
  EXPECT_GT(countLogicalBlack(frame, 16, NavScreenRenderer::kOverviewHeaderGrayPx + 4, 512,
                              792 - NavScreenRenderer::kOverviewFooterGrayPx - 4), 100)
      << "route map missing without GPS";
}

TEST(NavigatorRouteScreenTest, GrayBudgetExceededOverviewStaysCompactWhiteRouteOnly) {
  // A route spanning roughly 0.03 degrees in both axes: its whole-route view
  // covers more than the 3x3 tile budget of the calm map, so GrayMap answers
  // BudgetExceeded and the frame must fall back to the compact white
  // route-only rendering - byte-identical to drawing the same route without
  // any gray layer - instead of the legacy plain-vector overview.
  std::vector<GeoPoint> points;
  points.reserve(13);
  for (int i = 0; i <= 12; ++i) {
    points.push_back(GeoPoint{523'550'000 + i * 25'000, 48'900'000 + i * 25'000});
  }
  const Bytes routeBytes = encodeRoute(points, {0});
  RouteIndex index;
  ASSERT_EQ(decode(routeBytes, index), DecodeStatus::Ok);
  VectorRouteByteSource source(routeBytes);

  const Bytes grayBytes = encodeGrayMap(523'400'000, 48'800'000, 6, 6);
  VectorWalkMapByteSource graySource(grayBytes);
  navigator::GrayMap grayMap;
  ASSERT_EQ(grayMap.open(graySource), navigator::WalkMapStatus::Ok);

  Frame withGray(792, 528, kSentinel);
  navigator::GrayMapLayer grayLayer;
  grayLayer.source = &graySource;
  grayLayer.map = &grayMap;
  EXPECT_TRUE(NavScreenRenderer::drawOverview(withGray.pixels(), 792, 528, source, index, nullptr, nullptr, "STATUS",
                                              nullptr, &grayLayer, navigator::NavGrayPlane::Base, nullptr, nullptr,
                                              nullptr, nullptr, true));
  EXPECT_EQ(grayLayer.status, navigator::WalkMapStatus::BudgetExceeded)
      << "the whole-route view must exceed the calm map tile budget";

  Frame whiteFallback(792, 528, kSentinel);
  EXPECT_TRUE(NavScreenRenderer::drawOverview(whiteFallback.pixels(), 792, 528, source, index, nullptr, nullptr,
                                              "STATUS", nullptr, nullptr, navigator::NavGrayPlane::Base, nullptr,
                                              nullptr, nullptr, nullptr, true));
  expectGuardsUntouched(withGray);
  expectGuardsUntouched(whiteFallback);
  EXPECT_EQ(std::memcmp(withGray.pixels(), whiteFallback.pixels(), rowBytes(792) * 528), 0)
      << "budget-exceeded gray overview must fall back to the compact white route-only frame";
  EXPECT_EQ(countLogicalBlack(withGray, 20, 24, 260, 56), 0) << "legacy 'ROUTE OP X3' title must not appear";
  EXPECT_GT(countLogicalBlack(withGray, 16, NavScreenRenderer::kOverviewHeaderGrayPx + 4, 512,
                              792 - NavScreenRenderer::kOverviewFooterGrayPx - 4), 100)
      << "authoritative full route missing on the budget-exceeded fallback";
}

// ---------------------------------------------------------------------------
// Task: inject one selected viewport into every render plane.
//
// drawOverview gains an optional trailing `const RouteViewport* viewport`.
// NavigatorMain selects the viewport once per submitted frame (whole-route fit
// for Overview, a fixed 250 m centred viewport for GPS zoom) and feeds the
// SAME object to the Base, LSB and MSB passes. The renderer must:
//   * use the injected viewport for route, background, marker and scale
//     geometry whenever it is non-null and valid;
//   * keep a null/invalid viewport on the exact historical per-call choice,
//     so existing map-only and live-position frames stay byte-identical;
//   * never let the three grayscale planes disagree geometrically;
//   * keep guard bytes, row padding, the maneuver band and the footer
//     untouched by the injected geometry.
// ---------------------------------------------------------------------------

// The equal padding drawOverview's own fit/centre path applies (the renderer
// keeps this mirror of the pure controller's kNavigationViewPaddingPx).
constexpr int kOverviewInjectPaddingPx = 24;

TEST(NavigatorRouteScreenTest, OverviewDefaultAndInjectedFitViewportAreByteIdentical) {
  const auto route = TurnRoute::make(5000);
  RouteIndex index;
  ASSERT_EQ(decode(route.bytes, index), DecodeStatus::Ok);
  VectorRouteByteSource source(route.bytes);

  // The historical no-viewport call fits the whole route into the map rect
  // that overviewMapRect() reports; injecting that same fit must not move a
  // single pixel (route, chrome, footer, row padding or guards).
  const Rect mapRect = NavScreenRenderer::overviewMapRect(792, 528, false, false);
  const RouteViewport fitted = RouteViewport::fitOverview(index, mapRect, kOverviewInjectPaddingPx);
  ASSERT_TRUE(fitted.isValid());

  Frame defaults(792, 528, kSentinel);
  EXPECT_TRUE(NavScreenRenderer::drawOverview(defaults.pixels(), 792, 528, source, index));
  Frame injected(792, 528, kSentinel);
  EXPECT_TRUE(NavScreenRenderer::drawOverview(injected.pixels(), 792, 528, source, index, nullptr, nullptr, nullptr,
                                              nullptr, nullptr, navigator::NavGrayPlane::Base, nullptr, nullptr,
                                              nullptr, &fitted));
  EXPECT_EQ(std::memcmp(defaults.pixels(), injected.pixels(), rowBytes(792) * 528), 0)
      << "injecting the fitted overview changed the whole-route frame";
  expectGuardsUntouched(defaults);
  expectGuardsUntouched(injected);
}

TEST(NavigatorRouteScreenTest, OverviewInjectedInvalidViewportFallsBackToDefaultFit) {
  const auto route = TurnRoute::make(5000);
  RouteIndex index;
  ASSERT_EQ(decode(route.bytes, index), DecodeStatus::Ok);
  VectorRouteByteSource source(route.bytes);

  // A null or invalid viewport keeps the historical per-call behaviour.
  const RouteViewport invalid;  // default-constructed: not valid
  ASSERT_FALSE(invalid.isValid());
  Frame defaults(792, 528, kSentinel);
  EXPECT_TRUE(NavScreenRenderer::drawOverview(defaults.pixels(), 792, 528, source, index));
  Frame invalidInjected(792, 528, kSentinel);
  EXPECT_TRUE(NavScreenRenderer::drawOverview(invalidInjected.pixels(), 792, 528, source, index, nullptr, nullptr,
                                              nullptr, nullptr, nullptr, navigator::NavGrayPlane::Base, nullptr,
                                              nullptr, nullptr, &invalid));
  EXPECT_EQ(std::memcmp(defaults.pixels(), invalidInjected.pixels(), rowBytes(792) * 528), 0)
      << "an invalid injected viewport must not change the frame";
}

TEST(NavigatorRouteScreenTest, InjectedCenteredViewportCentresPositionMarkerAtMapCentre) {
  const auto route = TurnRoute::make(5000);
  RouteIndex index;
  ASSERT_EQ(decode(route.bytes, index), DecodeStatus::Ok);
  VectorRouteByteSource source(route.bytes);

  // A GPS-zoom selection centres the supplied fix: the marker must land on
  // the logical centre of the map rectangle the renderer uses.
  CurrentPosition position;
  position.point = GeoPoint{523'700'000, 49'090'000};
  position.accuracyMeters = 5;
  const Rect mapRect = NavScreenRenderer::overviewMapRect(792, 528, false, false);
  const RouteViewport zoom =
      RouteViewport::centered(position.point, mapRect, navigator::kGpsZoomSpanMeters, kOverviewInjectPaddingPx);
  ASSERT_TRUE(zoom.isValid());

  Frame frame(792, 528, kSentinel);
  EXPECT_TRUE(NavScreenRenderer::drawOverview(frame.pixels(), 792, 528, source, index, nullptr, &position, "STATUS",
                                              nullptr, nullptr, navigator::NavGrayPlane::Base, nullptr, nullptr,
                                              nullptr, &zoom));
  expectGuardsUntouched(frame);

  const int mapX = 16;
  const int mapY = NavScreenRenderer::kOverviewHeaderPlainPx;
  const int cx = mapX + mapRect.width / 2;
  const int cy = mapY + mapRect.height / 2;
  expectLogicalBlack(frame, cx, cy, "position marker centre at the injected viewport centre");

  // The accuracy ring (clamped to the renderer's minimum) is centred on the
  // same point, so the injected geometry drives the marker ring too.
  const int ring = std::clamp(zoom.pixelsForMeters(position.accuracyMeters), RouteMapRenderer::kMarkerMinRingRadiusPx,
                              static_cast<int>(RouteMapRenderer::kMarkerMaxRingRadiusPx));
  ASSERT_GT(ring, 0);
  expectLogicalBlack(frame, cx + ring, cy, "marker ring east");
  expectLogicalBlack(frame, cx - ring, cy, "marker ring west");
  expectLogicalBlack(frame, cx, cy + ring, "marker ring south");
  expectLogicalBlack(frame, cx, cy - ring, "marker ring north");
}

TEST(NavigatorRouteScreenTest, InjectedViewportUsesIdenticalGeometryInEveryGrayPlane) {
  const auto route = TurnRoute::make(5000);
  RouteIndex index;
  ASSERT_EQ(decode(route.bytes, index), DecodeStatus::Ok);
  VectorRouteByteSource source(route.bytes);

  const Bytes grayBytes = encodeGrayMap(523'400'000, 48'800'000, 6, 6);
  VectorWalkMapByteSource graySource(grayBytes);
  navigator::GrayMap grayMap;
  ASSERT_EQ(grayMap.open(graySource), navigator::WalkMapStatus::Ok);

  CurrentPosition position;
  position.point = GeoPoint{523'700'000, 49'090'000};
  position.accuracyMeters = 5;
  const Rect mapRect = NavScreenRenderer::overviewMapRect(792, 528, true, false);
  const RouteViewport zoom =
      RouteViewport::centered(position.point, mapRect, navigator::kGpsZoomSpanMeters, kOverviewInjectPaddingPx);
  ASSERT_TRUE(zoom.isValid());

  const int mapX = 16;
  const int mapY = NavScreenRenderer::kOverviewHeaderGrayPx;
  const int cx = mapX + mapRect.width / 2;
  const int cy = mapY + mapRect.height / 2;
  const navigator::NavMapText mapText{nullptr};

  Frame frames[3] = {Frame(792, 528, kSentinel), Frame(792, 528, kSentinel), Frame(792, 528, kSentinel)};
  const navigator::NavGrayPlane planes[] = {navigator::NavGrayPlane::Base, navigator::NavGrayPlane::Lsb,
                                            navigator::NavGrayPlane::Msb};
  for (int i = 0; i < 3; ++i) {
    SCOPED_TRACE("plane=" + std::to_string(static_cast<int>(planes[i])));
    navigator::GrayMapLayer layer;
    layer.source = &graySource;
    layer.map = &grayMap;
    EXPECT_TRUE(NavScreenRenderer::drawOverview(frames[i].pixels(), 792, 528, source, index, nullptr, &position,
                                                "STATUS", nullptr, &layer, planes[i], &mapText, nullptr, nullptr,
                                                &zoom));
    EXPECT_EQ(layer.status, navigator::WalkMapStatus::Ok);
    expectGuardsUntouched(frames[i]);
  }

  // The marker centre sits at exactly the same logical pixel in all three
  // planes: ink in Base, mask (clear) ink in the LSB/MSB overlays.
  const bool baseBlack = logicalPixelIsBlack(frames[0], cx, cy);
  EXPECT_TRUE(baseBlack) << "base marker centre";
  EXPECT_FALSE(logicalPixelIsBlack(frames[1], cx, cy)) << "lsb marker centre matches geometry";
  EXPECT_FALSE(logicalPixelIsBlack(frames[2], cx, cy)) << "msb marker centre matches geometry";

  // The ring radius is the same in every plane: probe one logical ring pixel
  // (east) in each plane and confirm the plane polarity agrees.
  const int ring = std::clamp(zoom.pixelsForMeters(position.accuracyMeters), RouteMapRenderer::kMarkerMinRingRadiusPx,
                              static_cast<int>(RouteMapRenderer::kMarkerMaxRingRadiusPx));
  ASSERT_GT(ring, 0);
  EXPECT_NE(logicalPixelIsBlack(frames[1], cx + ring, cy), baseBlack) << "lsb ring east";
  EXPECT_NE(logicalPixelIsBlack(frames[2], cx + ring, cy), baseBlack) << "msb ring east";
}

}  // namespace
