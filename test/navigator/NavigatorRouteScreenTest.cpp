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

#include "NavScreenRenderer.h"
#include "NavSplash.h"
#include "NavState.h"
#include "NavTestFrame.h"
#include "map/RouteMapRenderer.h"
#include "map/RouteViewport.h"
#include "route/RoutePackageV1.h"

namespace {

using Bytes = std::vector<uint8_t>;

using navigator::CurrentPosition;
using navigator::DecodeStatus;
using navigator::GeoPoint;
using navigator::NavScreenRenderer;
using navigator::NavState;
using navigator::NavStatus;
using navigator::Rect;
using navigator::RouteIndex;
using navigator::RouteMapRenderer;
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
Bytes encodeRoute(const std::vector<GeoPoint>& points, const std::vector<uint16_t>& segmentStarts) {
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

}  // namespace
