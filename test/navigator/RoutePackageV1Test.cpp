// Host tests for the bounded Route Package v1 streaming validator/indexer
// (src/spikes/navigator/route/RoutePackageV1.h/.cpp).
//
// Task 4 - strict TDD: these tests were written first against the wire
// contract and the checked-in golden fixture test/fixtures/route_package_v1.bin
// (manifest test/fixtures/route_package_v1.json). The decoder under test must:
//   * stream through a RouteByteSource with every read request <= 1,024 bytes;
//   * never allocate or retain the whole package, never use the heap, and
//     publish `out` only after the full package validated (CRC included);
//   * reconstruct per-segment geometry from absolute E7 anchors plus signed
//     Int16 E5 deltas (ties away from zero, antimeridian longitude folding);
//   * reject every malformed input with a precise DecodeStatus and leave the
//     caller-owned RouteIndex untouched on failure.

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

#include "route/RoutePackageV1.h"

namespace {

using Bytes = std::vector<uint8_t>;
using navigator::DecodeStatus;
using navigator::RouteIndex;

// ---------------------------------------------------------------------------
// Little-endian helpers and a test-side CRC-32 (IEEE reflected, poly
// 0xEDB88320) matching the authoritative wire contract and the golden
// fixture's trailing CRC 0x4CB0201C.
// ---------------------------------------------------------------------------

uint32_t ru32(const Bytes& b, size_t at) {
  return static_cast<uint32_t>(b[at]) | (static_cast<uint32_t>(b[at + 1]) << 8) |
         (static_cast<uint32_t>(b[at + 2]) << 16) | (static_cast<uint32_t>(b[at + 3]) << 24);
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

uint32_t crc32Step(uint32_t crc, uint8_t byte) {
  crc ^= byte;
  for (int i = 0; i < 8; ++i) {
    crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
  }
  return crc;
}

uint32_t crc32Of(const Bytes& bytes) {
  uint32_t crc = 0xFFFFFFFFu;
  for (uint8_t byte : bytes) {
    crc = crc32Step(crc, byte);
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

// ---------------------------------------------------------------------------
// Golden fixture loading. The repo is located through __FILE__ when CMake
// compiled with an absolute path (out-of-source builds), with CWD-relative
// fallbacks for in-source builds or direct test invocation.
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

const Bytes& goldenFixture() {
  static const Bytes kBytes = loadFixtureBytes("route_package_v1.bin");
  return kBytes;
}

// ---------------------------------------------------------------------------
// Host RouteByteSource implementations (the decoder itself never sees these).
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

// A conforming source that fragments every read into pieces of at most
// `chunkBytes`. The decoder must tolerate short reads by re-requesting the
// remainder at offset+n.
class ChunkedReadSource : public navigator::RouteByteSource {
 public:
  ChunkedReadSource(Bytes bytes, uint32_t chunkBytes)
      : inner_(std::move(bytes)), chunk_(std::max<uint32_t>(chunkBytes, 1)) {}

  uint32_t size() const override { return inner_.size(); }

  uint32_t read(uint32_t offset, uint8_t* destination, uint32_t length) override {
    return inner_.read(offset, destination, std::min(length, chunk_));
  }

 private:
  VectorRouteByteSource inner_;
  uint32_t chunk_;
};

// A source that stops delivering bytes after `deliverAtMost` bytes in total
// and then returns 0, even though size() reports the full package length.
class StallingSource : public navigator::RouteByteSource {
 public:
  StallingSource(Bytes bytes, uint32_t deliverAtMost) : inner_(std::move(bytes)), budget_(deliverAtMost) {}

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

// Wraps a source and records the largest single read request the decoder
// issued, so tests can assert the guarded <= 1,024-byte work-buffer rule.
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

// ---------------------------------------------------------------------------
// Test-side Route Package v1 encoder mirroring the authoritative Swift
// encoder, used to build *valid* synthetic packages for reconstruction tests.
// ---------------------------------------------------------------------------

struct PointE7 {
  int32_t latitudeE7;
  int32_t longitudeE7;
};

struct TestManeuver {
  uint16_t pointIndex;
  uint8_t type;
  uint32_t distanceFromStartMeters;
  std::string name;
};

int64_t quantizeE5(int32_t e7) {
  const int64_t v = e7;
  const int64_t magnitude = v < 0 ? -v : v;
  const int64_t quantized = (magnitude + 50) / 100;
  return v < 0 ? -quantized : quantized;
}

Bytes encodeRoute(uint32_t routeId, const std::string& name, const std::vector<PointE7>& points,
                  std::vector<uint16_t> segmentStarts, const std::vector<TestManeuver>& maneuvers,
                  uint32_t totalDistanceMeters = 1250, uint16_t estimatedMinutes = 20) {
  EXPECT_FALSE(points.empty());
  EXPECT_FALSE(segmentStarts.empty());
  EXPECT_EQ(segmentStarts.front(), 0U);
  for (size_t i = 1; i < segmentStarts.size(); ++i) {
    EXPECT_GT(segmentStarts[i], segmentStarts[i - 1]);
    EXPECT_LT(static_cast<size_t>(segmentStarts[i]), points.size());
  }

  Bytes payload;
  const std::string nameBytes = name;
  EXPECT_LE(nameBytes.size(), 255U);
  for (char c : nameBytes) {
    payload.push_back(static_cast<uint8_t>(c));
  }
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
      EXPECT_GE(latDelta, INT16_MIN);
      EXPECT_LE(latDelta, INT16_MAX);
      EXPECT_GE(lonDelta, INT16_MIN);
      EXPECT_LE(lonDelta, INT16_MAX);
      appendU16(payload, static_cast<uint16_t>(static_cast<int16_t>(latDelta)));
      appendU16(payload, static_cast<uint16_t>(static_cast<int16_t>(lonDelta)));
    }
  }

  for (const TestManeuver& m : maneuvers) {
    EXPECT_LE(m.name.size(), 255U);
    appendU16(payload, m.pointIndex);
    payload.push_back(m.type);
    payload.push_back(static_cast<uint8_t>(m.name.size()));
    appendU32(payload, m.distanceFromStartMeters);
    for (char c : m.name) {
      payload.push_back(static_cast<uint8_t>(c));
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
  putU32(bytes, 12, routeId);
  putU16(bytes, 16, static_cast<uint16_t>(points.size()));
  putU16(bytes, 18, static_cast<uint16_t>(maneuvers.size()));
  putI32(bytes, 20, points.front().latitudeE7);
  putI32(bytes, 24, points.front().longitudeE7);
  putU32(bytes, 28, totalDistanceMeters);
  putU16(bytes, 32, estimatedMinutes);
  putU16(bytes, 34, static_cast<uint16_t>(segmentStarts.size()));
  bytes[36] = static_cast<uint8_t>(nameBytes.size());
  bytes[37] = 0;
  std::copy(payload.begin(), payload.end(), bytes.begin() + 38);
  putU32(bytes, total - 4, crc32Of(Bytes(bytes.begin(), bytes.begin() + (total - 4))));
  return bytes;
}

// Builds a single-segment package whose interior delta stream is supplied
// verbatim as signed Int16 E5 pairs. Unlike encodeRoute (which derives deltas
// from in-world E7 points and therefore can never leave the world), this
// builder can emit a syntactically valid delta stream whose reconstruction
// escapes +/-90 degrees latitude — exactly the corrupt-but-well-formed input
// the decoder's first pass must reject. The trailing CRC is always repaired.
Bytes encodeRawDeltaRoute(int32_t originLatitudeE7, int32_t originLongitudeE7,
                          const std::vector<std::pair<int16_t, int16_t>>& deltas) {
  const uint32_t pointCount = 1u + static_cast<uint32_t>(deltas.size());
  Bytes payload;
  appendU16(payload, 0);  // segment starts: single segment beginning at point 0
  for (const auto& delta : deltas) {
    appendU16(payload, static_cast<uint16_t>(delta.first));
    appendU16(payload, static_cast<uint16_t>(delta.second));
  }
  const uint32_t total = 38u + static_cast<uint32_t>(payload.size()) + 4u;
  Bytes bytes(total, 0);
  bytes[0] = 'X';
  bytes[1] = '3';
  bytes[2] = 'R';
  bytes[3] = 'T';
  bytes[4] = 1;
  bytes[5] = 0;
  putU16(bytes, 6, 38);
  putU32(bytes, 8, total);
  putU32(bytes, 12, 21);
  putU16(bytes, 16, static_cast<uint16_t>(pointCount));
  putU16(bytes, 18, 0);  // no maneuvers
  putI32(bytes, 20, originLatitudeE7);
  putI32(bytes, 24, originLongitudeE7);
  putU32(bytes, 28, 0);  // total distance meters
  putU16(bytes, 32, 0);  // estimated minutes
  putU16(bytes, 34, 1);  // segment count
  bytes[36] = 0;         // empty route name
  bytes[37] = 0;         // reserved
  std::copy(payload.begin(), payload.end(), bytes.begin() + 38);
  putU32(bytes, total - 4, crc32Of(Bytes(bytes.begin(), bytes.begin() + (total - 4))));
  return bytes;
}

// ---------------------------------------------------------------------------
// Helpers for corrupt-variant construction.
// ---------------------------------------------------------------------------

Bytes withLength(Bytes b, uint32_t length) {
  putU32(b, 8, length);
  return b;
}

Bytes withU16At(Bytes b, size_t at, uint16_t value) {
  putU16(b, at, value);
  return b;
}

Bytes withI32At(Bytes b, size_t at, int32_t value) {
  putI32(b, at, value);
  return b;
}

Bytes withByteAt(Bytes b, size_t at, uint8_t value) {
  b[at] = value;
  return b;
}

DecodeStatus decode(const Bytes& bytes, RouteIndex& out) {
  VectorRouteByteSource source(bytes);
  return navigator::validateRoutePackageV1(source, out);
}

// Every failing variant that must leave `out` untouched, with the exact
// DecodeStatus it must produce. Most variants corrupt the content without
// repairing the trailing CRC (the decoder validates structure before CRC, so
// each single-fault package still hits its structural status first); the
// exact-length and out-of-world-geometry variants are built with a repaired
// CRC because the fault itself is invisible to the CRC.
std::vector<std::pair<Bytes, DecodeStatus>> failingVariants() {
  std::vector<std::pair<Bytes, DecodeStatus>> cases;
  const Bytes& golden = goldenFixture();

  cases.emplace_back(Bytes(), DecodeStatus::NullInput);
  cases.emplace_back(Bytes(golden.begin(), golden.begin() + 37), DecodeStatus::TooShort);
  cases.emplace_back(withLength(golden, 100), DecodeStatus::TooShort);  // declared > source size
  cases.emplace_back(withLength(golden, 70'000), DecodeStatus::TooLarge);
  cases.emplace_back(withByteAt(golden, 0, 'Q'), DecodeStatus::BadMagic);
  cases.emplace_back(withByteAt(golden, 4, 2), DecodeStatus::UnsupportedVersion);
  cases.emplace_back(withU16At(golden, 6, 40), DecodeStatus::BadHeader);              // headerSize != 38
  cases.emplace_back(withByteAt(golden, 5, 1), DecodeStatus::BadHeader);              // nonzero flags
  cases.emplace_back(withByteAt(golden, 37, 1), DecodeStatus::BadHeader);             // reserved byte
  cases.emplace_back(withU16At(golden, 16, 0), DecodeStatus::BadHeader);              // zero points
  cases.emplace_back(withU16At(golden, 34, 0), DecodeStatus::BadHeader);              // zero segments
  cases.emplace_back(withU16At(golden, 34, 6), DecodeStatus::BadHeader);              // segments > points
  cases.emplace_back(withI32At(golden, 20, 1'000'000'000), DecodeStatus::BadHeader);  // origin lat out of world
  cases.emplace_back(withI32At(golden, 24, 1'900'000'000), DecodeStatus::BadHeader);  // origin lon out of world

  Bytes anchorOut = golden;
  putI32(anchorOut, 54, 1'000'000'000);  // segment 1 absolute anchor latitude
  cases.emplace_back(anchorOut, DecodeStatus::BadHeader);

  Bytes startsNotZero = golden;
  putU16(startsNotZero, 42, 1);  // first segment start must be 0
  cases.emplace_back(startsNotZero, DecodeStatus::BadHeader);
  Bytes startsDuplicate = golden;
  putU16(startsDuplicate, 44, 0);  // duplicate start index
  cases.emplace_back(startsDuplicate, DecodeStatus::BadHeader);
  Bytes startsOutOfRange = golden;
  putU16(startsOutOfRange, 44, 5);  // start index == pointCount
  cases.emplace_back(startsOutOfRange, DecodeStatus::BadHeader);

  cases.emplace_back(withU16At(golden, 16, RouteIndex::kMaxPoints + 1), DecodeStatus::TooManyPoints);
  Bytes manySegments = withU16At(golden, 16, 100);
  putU16(manySegments, 34, RouteIndex::kMaxSegments + 1);
  cases.emplace_back(manySegments, DecodeStatus::TooManySegments);

  Bytes manyManeuvers = withU16At(golden, 18, RouteIndex::kMaxManeuvers + 1);
  cases.emplace_back(manyManeuvers, DecodeStatus::TooManyManeuvers);

  cases.emplace_back(withLength(golden, 30), DecodeStatus::BadLength);     // declared < header
  cases.emplace_back(withLength(golden, 48), DecodeStatus::BadLength);     // segment list overruns content
  cases.emplace_back(withU16At(golden, 16, 40), DecodeStatus::BadLength);  // geometry overruns content
  cases.emplace_back(withU16At(golden, 18, 3), DecodeStatus::BadLength);   // maneuver record truncated

  cases.emplace_back(withByteAt(golden, 36, 200), DecodeStatus::BadUtf8Length);  // route name truncated
  Bytes maneuverNameTruncated = withByteAt(golden, 69, 200);                     // maneuver name length
  cases.emplace_back(maneuverNameTruncated, DecodeStatus::BadUtf8Length);

  Bytes badRouteUtf8 = golden;
  badRouteUtf8[38] = 0xC2;  // 0xC2 lead byte followed by non-continuation below
  badRouteUtf8[39] = 0x41;
  cases.emplace_back(badRouteUtf8, DecodeStatus::BadUtf8);
  Bytes badManeuverUtf8 = golden;
  badManeuverUtf8[74] = 0xE2;  // 3-byte lead; 0x28 is not a continuation byte
  badManeuverUtf8[75] = 0x28;
  cases.emplace_back(badManeuverUtf8, DecodeStatus::BadUtf8);

  cases.emplace_back(withByteAt(golden, 68, 7), DecodeStatus::BadManeuver);  // kind out of 0..6
  cases.emplace_back(withU16At(golden, 66, 5), DecodeStatus::BadManeuver);   // point index == pointCount

  Bytes trailing = golden;  // two extra content bytes between the maneuvers and CRC
  trailing.insert(trailing.begin() + 86, 2, 0);
  trailing = withLength(trailing, 92);
  cases.emplace_back(trailing, DecodeStatus::TrailingPayload);

  Bytes badCrc = golden;
  badCrc.back() ^= 0xFF;
  cases.emplace_back(badCrc, DecodeStatus::BadCrc);

  // A route.bin is the package itself, not a container: extra bytes after the
  // declared package are detectable corruption, not ignorable padding.
  Bytes appended = golden;
  appended.insert(appended.end(), 5, 0xAB);
  cases.emplace_back(appended, DecodeStatus::BadLength);

  // Syntactically valid Int16 E5 delta streams whose reconstruction escapes
  // +/-90 degrees latitude (300 x +32767 reaches 9,830,100 E5; 300 x -32768
  // reaches -9,830,400 E5). The decoder must reconstruct every point in its
  // first pass and reject the geometry.
  std::vector<std::pair<int16_t, int16_t>> northOverflow;
  std::vector<std::pair<int16_t, int16_t>> southOverflow;
  northOverflow.reserve(300);
  southOverflow.reserve(300);
  for (int i = 0; i < 300; ++i) {
    northOverflow.emplace_back(32767, 0);
    southOverflow.emplace_back(-32768, 0);
  }
  cases.emplace_back(encodeRawDeltaRoute(0, 0, northOverflow), DecodeStatus::BadHeader);
  cases.emplace_back(encodeRawDeltaRoute(0, 0, southOverflow), DecodeStatus::BadHeader);

  return cases;
}

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

TEST(RoutePackageV1Test, FixtureMatchesManifestCrc) {
  const Bytes& golden = goldenFixture();
  ASSERT_EQ(golden.size(), 90U);
  EXPECT_EQ(ru32(golden, 86), 0x4CB0201Cu);
  EXPECT_EQ(crc32Of(Bytes(golden.begin(), golden.begin() + 86)), 0x4CB0201Cu);
}

TEST(RoutePackageV1Test, GoldenFixtureDecodesAndMatchesManifestValues) {
  const Bytes& golden = goldenFixture();
  RouteIndex route;
  EXPECT_EQ(decode(golden, route), DecodeStatus::Ok);

  EXPECT_EQ(route.declaredLength, 90U);
  EXPECT_EQ(route.routeId, 66051U);  // 0x00010203 from the manifest, not the stale plan example
  EXPECT_EQ(route.pointCount, 5U);
  EXPECT_EQ(route.maneuverCount, 1U);
  EXPECT_EQ(route.segmentCount, 2U);
  EXPECT_EQ(route.totalDistanceMeters, 1250U);
  EXPECT_EQ(route.estimatedMinutes, 20U);
  EXPECT_EQ(route.routeNameLength, 4U);
  EXPECT_EQ(route.routeNameOffset, 38U);
  EXPECT_EQ(route.originLatitudeE7, 523'676'000);
  EXPECT_EQ(route.originLongitudeE7, 49'041'000);
}

TEST(RoutePackageV1Test, GoldenFixtureRecordsOffsets) {
  const Bytes& golden = goldenFixture();
  RouteIndex route;
  ASSERT_EQ(decode(golden, route), DecodeStatus::Ok);

  EXPECT_EQ(route.payloadOffset, 38U);
  EXPECT_EQ(route.segmentStartsOffset, 42U);
  EXPECT_EQ(route.geometryOffset, 46U);
  EXPECT_EQ(route.maneuverOffset, 66U);
  EXPECT_EQ(route.crcOffset, 86U);

  ASSERT_EQ(route.segmentCount, 2U);
  EXPECT_EQ(route.segments[0].sourceOffset, 46U);
  EXPECT_EQ(route.segments[0].startPointIndex, 0U);
  EXPECT_EQ(route.segments[0].overviewBegin, 0U);
  EXPECT_EQ(route.segments[0].overviewCount, 3U);
  EXPECT_EQ(route.segments[1].sourceOffset, 54U);
  EXPECT_EQ(route.segments[1].startPointIndex, 3U);
  EXPECT_EQ(route.segments[1].overviewBegin, 3U);
  EXPECT_EQ(route.segments[1].overviewCount, 2U);
}

TEST(RoutePackageV1Test, GoldenFixtureOverviewReconstructsQuantizedGeometry) {
  const Bytes& golden = goldenFixture();
  RouteIndex route;
  ASSERT_EQ(decode(golden, route), DecodeStatus::Ok);

  // The decoder stores the E5-quantized reconstruction, which equals
  // quantizeE5(source E7) exactly (deltas telescope back to the anchor).
  ASSERT_EQ(route.overviewPointCount, 5U);
  const struct {
    int32_t latE5;
    int32_t lonE5;
  } expected[5] = {
      {5'236'760, 490'410},    {5'236'880, 490'490},    {5'236'950, 490'520},
      {3'567'620, 13'965'030}, {3'567'670, 13'964'990},  // negative Int16 E5 longitude delta inside segment 1
  };
  for (int i = 0; i < 5; ++i) {
    EXPECT_EQ(route.overview[i].latitudeE5, expected[i].latE5) << "point " << i;
    EXPECT_EQ(route.overview[i].longitudeE5, expected[i].lonE5) << "point " << i;
  }
}

TEST(RoutePackageV1Test, GoldenFixtureRecordsManeuver) {
  const Bytes& golden = goldenFixture();
  RouteIndex route;
  ASSERT_EQ(decode(golden, route), DecodeStatus::Ok);

  ASSERT_EQ(route.maneuverCount, 1U);
  EXPECT_EQ(route.maneuvers[0].pointIndex, 2U);
  EXPECT_EQ(route.maneuvers[0].type, 4U);  // navigator::Maneuver::SlightRight
  EXPECT_EQ(route.maneuvers[0].distanceFromStartMeters, 850U);
  EXPECT_EQ(route.maneuvers[0].nameLength, 12U);
  EXPECT_EQ(route.maneuvers[0].nameOffset, 74U);
}

TEST(RoutePackageV1Test, FailureLeavesOutputUnchanged) {
  for (const auto& variant : failingVariants()) {
    const Bytes& bytes = variant.first;
    const DecodeStatus expected = variant.second;
    RouteIndex route;
    std::memset(&route, 0xA5, sizeof(route));  // poison every byte
    RouteIndex pristine = route;
    EXPECT_EQ(decode(bytes, route), expected) << "variant size " << bytes.size();
    EXPECT_EQ(std::memcmp(&route, &pristine, sizeof(route)), 0)
        << "out mutated on failure for status " << static_cast<int>(expected);
  }
}

TEST(RoutePackageV1Test, EveryRejectionPathReportsItsPreciseStatus) {
  for (const auto& variant : failingVariants()) {
    RouteIndex route;
    const DecodeStatus got = decode(variant.first, route);
    EXPECT_EQ(got, variant.second) << "variant size " << variant.first.size();
  }
}

TEST(RoutePackageV1Test, MaximumReadRequestStaysWithinWorkBuffer) {
  // Golden fixture: small reads.
  TrackingSource goldenSource(goldenFixture());
  RouteIndex route;
  ASSERT_EQ(navigator::validateRoutePackageV1(goldenSource, route), DecodeStatus::Ok);
  EXPECT_GE(goldenSource.maxRequested, 1U);
  EXPECT_LE(goldenSource.maxRequested, navigator::kRoutePackageV1WorkBufferBytes);

  // A route whose geometry spans far more than 1,024 bytes must still be read
  // in <= 1,024-byte requests (a real geometry chunk fills the work buffer).
  std::vector<PointE7> points;
  for (int i = 0; i < 3000; ++i) {
    points.push_back(PointE7{523'676'000 + i * 1'000, 49'041'000 + i * 2'000});
  }
  const Bytes big = encodeRoute(7, "L", points, {0}, {});
  TrackingSource bigSource(big);
  RouteIndex bigRoute;
  ASSERT_EQ(navigator::validateRoutePackageV1(bigSource, bigRoute), DecodeStatus::Ok);
  EXPECT_EQ(bigSource.maxRequested, navigator::kRoutePackageV1WorkBufferBytes);
}

TEST(RoutePackageV1Test, ChunkedPartialReadsStillDecode) {
  const Bytes& golden = goldenFixture();
  ChunkedReadSource source(golden, 7);  // at most 7 bytes per read call
  RouteIndex route;
  EXPECT_EQ(navigator::validateRoutePackageV1(source, route), DecodeStatus::Ok);
  EXPECT_EQ(route.routeId, 66051U);
}

TEST(RoutePackageV1Test, StalledReadIsReportedAsTooShort) {
  const Bytes& golden = goldenFixture();
  StallingSource source(golden, 20);  // delivers 20 bytes then goes silent
  RouteIndex route;
  std::memset(&route, 0xA5, sizeof(route));
  RouteIndex pristine = route;
  EXPECT_EQ(navigator::validateRoutePackageV1(source, route), DecodeStatus::TooShort);
  EXPECT_EQ(std::memcmp(&route, &pristine, sizeof(route)), 0);
}

TEST(RoutePackageV1Test, ExtraSourceBytesBeyondDeclaredPackageAreRejected) {
  const Bytes& golden = goldenFixture();
  Bytes padded = golden;
  padded.insert(padded.end(), 5, 0xAB);  // bytes after the declared package end
  RouteIndex route;
  std::memset(&route, 0xA5, sizeof(route));
  RouteIndex pristine = route;
  EXPECT_EQ(decode(padded, route), DecodeStatus::BadLength);
  EXPECT_EQ(std::memcmp(&route, &pristine, sizeof(route)), 0);
}

TEST(RoutePackageV1Test, DecodingIsRepeatableAndDeterministic) {
  const Bytes& golden = goldenFixture();
  RouteIndex first;
  RouteIndex second;
  std::memset(&first, 0, sizeof(first));    // zero padding too, so the raw
  std::memset(&second, 0, sizeof(second));  // comparison is fully deterministic
  ASSERT_EQ(decode(golden, first), DecodeStatus::Ok);
  ASSERT_EQ(decode(golden, second), DecodeStatus::Ok);
  EXPECT_EQ(std::memcmp(&first, &second, sizeof(first)), 0);
}

TEST(RoutePackageV1Test, AntimeridianLongitudeFolding) {
  // Eastward then westward crossing of +/-180 degrees inside one segment.
  const std::vector<PointE7> points = {
      PointE7{0, 1'799'995'000},
      PointE7{0, -1'799'995'000},
      PointE7{0, 1'799'995'000},
  };
  const Bytes bytes = encodeRoute(1, "", points, {0}, {});
  RouteIndex route;
  ASSERT_EQ(decode(bytes, route), DecodeStatus::Ok);
  ASSERT_EQ(route.overviewPointCount, 3U);
  EXPECT_EQ(route.overview[0].longitudeE5, 17'999'950);
  EXPECT_EQ(route.overview[1].longitudeE5, -17'999'950);  // folded at the antimeridian
  EXPECT_EQ(route.overview[2].longitudeE5, 17'999'950);
}

TEST(RoutePackageV1Test, SignedDeltasAndFarSegmentGapReconstructExactly) {
  // Segment 0 around Amsterdam; segment 1 starts with an absolute anchor near
  // Tokyo (a gap no Int16 E5 delta could cross) and moves west.
  const std::vector<PointE7> points = {
      PointE7{523'676'000, 49'041'000},    PointE7{523'688'000, 49'049'000},    PointE7{523'695'000, 49'052'000},
      PointE7{356'762'000, 1'396'503'000}, PointE7{356'767'000, 1'396'499'000}, PointE7{356'770'000, 1'396'494'000},
  };
  const Bytes bytes = encodeRoute(66051, "Walk", points, {0, 3}, {TestManeuver{2, 4, 850, "Achtergracht"}});
  RouteIndex route;
  ASSERT_EQ(decode(bytes, route), DecodeStatus::Ok);
  ASSERT_EQ(route.overviewPointCount, 6U);
  const int64_t expected[6][2] = {
      {5'236'760, 490'410},    {5'236'880, 490'490},    {5'236'950, 490'520},
      {3'567'620, 13'965'030}, {3'567'670, 13'964'990}, {3'567'700, 13'964'940},
  };
  for (int i = 0; i < 6; ++i) {
    EXPECT_EQ(route.overview[i].latitudeE5, expected[i][0]) << "point " << i;
    EXPECT_EQ(route.overview[i].longitudeE5, expected[i][1]) << "point " << i;
  }
  // The second segment is decoded from its own absolute anchor, never by
  // carrying deltas across the segment boundary.
  EXPECT_EQ(route.segments[1].startPointIndex, 3U);
  EXPECT_EQ(route.overview[3].latitudeE5, 3'567'620);
}

TEST(RoutePackageV1Test, NegativeHemisphereAndTieAwayFromZeroRounding) {
  // E7 values ending in 50 quantize to E5 with ties away from zero, including
  // for negative (southern/western) coordinates.
  const std::vector<PointE7> points = {
      PointE7{-523'676'050, -49'041'050},
      PointE7{-523'680'000, -49'060'000},
  };
  const Bytes bytes = encodeRoute(3, "S", points, {0}, {});
  RouteIndex route;
  ASSERT_EQ(decode(bytes, route), DecodeStatus::Ok);
  ASSERT_EQ(route.overviewPointCount, 2U);
  EXPECT_EQ(route.overview[0].latitudeE5, -5'236'761);  // -523676.05 ties away to -523676.1
  EXPECT_EQ(route.overview[0].longitudeE5, -490'411);
  EXPECT_EQ(route.overview[1].latitudeE5, -5'236'800);
  EXPECT_EQ(route.overview[1].longitudeE5, -490'600);
}

TEST(RoutePackageV1Test, ValidInt16DeltaStreamOverflowingLatitudeNorthIsRejected) {
  // 300 x +32767 from the equator reaches 9,830,100 E5, past +9,000,000 E5
  // (+90 degrees). Every delta is a valid signed Int16 and the CRC is valid,
  // so only first-pass per-point reconstruction can catch this.
  std::vector<std::pair<int16_t, int16_t>> deltas(300, {32767, 0});
  const Bytes bytes = encodeRawDeltaRoute(0, 0, deltas);
  RouteIndex route;
  std::memset(&route, 0xA5, sizeof(route));
  RouteIndex pristine = route;
  EXPECT_EQ(decode(bytes, route), DecodeStatus::BadHeader);
  EXPECT_EQ(std::memcmp(&route, &pristine, sizeof(route)), 0);
}

TEST(RoutePackageV1Test, ValidInt16DeltaStreamOverflowingLatitudeSouthIsRejected) {
  // 300 x -32768 from the equator reaches -9,830,400 E5, past -9,000,000 E5.
  std::vector<std::pair<int16_t, int16_t>> deltas(300, {-32768, 0});
  const Bytes bytes = encodeRawDeltaRoute(0, 0, deltas);
  RouteIndex route;
  std::memset(&route, 0xA5, sizeof(route));
  RouteIndex pristine = route;
  EXPECT_EQ(decode(bytes, route), DecodeStatus::BadHeader);
  EXPECT_EQ(std::memcmp(&route, &pristine, sizeof(route)), 0);
}

TEST(RoutePackageV1Test, ReconstructedLatitudeBoundaryAtThePolesIsAccepted) {
  // A delta that lands exactly on +/-9,000,000 E5 (90 degrees) is valid; only
  // geometry beyond the boundary is rejected.
  const std::vector<PointE7> north = {
      PointE7{899'000'000, 0},  // quantizes to 8,990,000 E5
      PointE7{900'000'000, 0},  // quantizes to 9,000,000 E5 (delta +10,000)
  };
  const Bytes northBytes = encodeRoute(31, "N", north, {0}, {});
  RouteIndex northRoute;
  ASSERT_EQ(decode(northBytes, northRoute), DecodeStatus::Ok);
  ASSERT_EQ(northRoute.overviewPointCount, 2U);
  EXPECT_EQ(northRoute.overview[1].latitudeE5, 9'000'000);

  const std::vector<PointE7> south = {
      PointE7{-899'000'000, 0},
      PointE7{-900'000'000, 0},
  };
  const Bytes southBytes = encodeRoute(32, "S", south, {0}, {});
  RouteIndex southRoute;
  ASSERT_EQ(decode(southBytes, southRoute), DecodeStatus::Ok);
  ASSERT_EQ(southRoute.overviewPointCount, 2U);
  EXPECT_EQ(southRoute.overview[1].latitudeE5, -9'000'000);
}

TEST(RoutePackageV1Test, LongitudeDeltaStreamFoldsWithinWorld) {
  // Longitude wraps on the sphere: a delta stream that crosses the antimeridian
  // repeatedly stays valid because every reconstructed longitude is folded back
  // into [-18,000,000, 18,000,000] E5.
  std::vector<std::pair<int16_t, int16_t>> deltas(700, {0, 32767});
  const Bytes bytes = encodeRawDeltaRoute(0, 0, deltas);
  RouteIndex route;
  EXPECT_EQ(decode(bytes, route), DecodeStatus::Ok);
}

TEST(RoutePackageV1Test, ValidMultibyteUtf8NamesDecode) {
  const std::string routeName = "Wâlk–Straße 東京";  // 2- and 3-byte sequences
  const std::vector<PointE7> points = {
      PointE7{523'676'000, 49'041'000},
      PointE7{523'688'000, 49'049'000},
  };
  const Bytes bytes = encodeRoute(9, routeName, points, {0}, {TestManeuver{1, 0, 500, "Ünïcode 東京"}});
  RouteIndex route;
  ASSERT_EQ(decode(bytes, route), DecodeStatus::Ok);
  EXPECT_EQ(route.routeNameLength, routeName.size());
  EXPECT_EQ(route.routeNameOffset, 38U);
  ASSERT_EQ(route.maneuverCount, 1U);
  EXPECT_EQ(route.maneuvers[0].nameLength, std::string("Ünïcode 東京").size());
  EXPECT_EQ(route.maneuvers[0].nameOffset, route.maneuverOffset + 8U);
}

TEST(RoutePackageV1Test, SinglePointRouteDecodes) {
  const std::vector<PointE7> points = {PointE7{523'676'000, 49'041'000}};
  const Bytes bytes = encodeRoute(4, "dot", points, {0}, {});
  RouteIndex route;
  ASSERT_EQ(decode(bytes, route), DecodeStatus::Ok);
  EXPECT_EQ(route.pointCount, 1U);
  EXPECT_EQ(route.overviewPointCount, 1U);
  EXPECT_EQ(route.overview[0].latitudeE5, 5'236'760);
  ASSERT_EQ(route.segmentCount, 1U);
  EXPECT_EQ(route.segments[0].overviewCount, 1U);
}

TEST(RoutePackageV1Test, SmallRouteOverviewIsFullFidelity) {
  const std::vector<PointE7> points = {
      PointE7{523'676'000, 49'041'000},
      PointE7{523'680'000, 49'050'000},
      PointE7{523'684'000, 49'060'000},
      PointE7{523'688'000, 49'070'000},
  };
  const Bytes bytes = encodeRoute(8, "full", points, {0}, {});
  RouteIndex route;
  ASSERT_EQ(decode(bytes, route), DecodeStatus::Ok);
  EXPECT_EQ(route.overviewPointCount, 4U);
  EXPECT_EQ(route.overview[1].latitudeE5, 5'236'800);
  EXPECT_EQ(route.overview[3].latitudeE5, 5'236'880);
}

TEST(RoutePackageV1Test, SampledOverviewKeepsSegmentBoundaries) {
  // Two long segments: the 1,024-point overview cannot hold every point, so
  // the decoder samples uniformly *within* each segment while always keeping
  // every segment's start and end point and never mixing segments.
  std::vector<PointE7> points;
  points.reserve(4000);
  for (int i = 0; i < 2000; ++i) {
    points.push_back(PointE7{523'676'000 + i * 1'000, 49'041'000 + i * 2'000});
  }
  for (int i = 0; i < 2000; ++i) {
    points.push_back(PointE7{356'762'000 + i * 1'000, 1'396'503'000 + i * 1'000});
  }
  const Bytes bytes = encodeRoute(11, "long", points, {0, 2000}, {});
  RouteIndex route;
  ASSERT_EQ(decode(bytes, route), DecodeStatus::Ok);
  EXPECT_EQ(route.pointCount, 4000U);
  EXPECT_EQ(route.overviewPointCount, RouteIndex::kMaxOverviewPoints);  // 1024
  ASSERT_EQ(route.segmentCount, 2U);

  EXPECT_EQ(route.segments[0].overviewBegin, 0U);
  EXPECT_EQ(route.segments[0].overviewCount, 512U);
  EXPECT_EQ(route.segments[1].overviewBegin, 512U);
  EXPECT_EQ(route.segments[1].overviewCount, 512U);

  // Segment 0 endpoints preserved exactly.
  EXPECT_EQ(route.overview[0].latitudeE5, 5'236'760);
  EXPECT_EQ(route.overview[511].latitudeE5, 5'236'760 + 1999 * 10);
  // Segment 1 endpoints preserved exactly.
  EXPECT_EQ(route.overview[512].latitudeE5, 3'567'620);
  EXPECT_EQ(route.overview[1023].latitudeE5, 3'567'620 + 1999 * 10);

  // Interior samples stay strictly inside each segment's endpoint band (the
  // route latitude rises by 10 E5 per point), the slices stay monotonic, and
  // the segment slices are disjoint: a renderer can draw them separately and
  // never connect the segments semantically.
  for (uint16_t i = 1; i + 1 < route.segments[0].overviewCount; ++i) {
    EXPECT_GT(route.overview[i].latitudeE5, route.overview[i - 1].latitudeE5);
    EXPECT_LT(route.overview[i].latitudeE5, 5'236'760 + 1999 * 10);
  }
  for (uint16_t i = 1; i + 1 < route.segments[1].overviewCount; ++i) {
    EXPECT_GT(route.overview[512 + i].latitudeE5, route.overview[512 + i - 1].latitudeE5);
    EXPECT_LT(route.overview[512 + i].latitudeE5, 3'567'620 + 1999 * 10);
  }
  EXPECT_EQ(route.overview[512].longitudeE5, 13'965'030);  // segment 1's own anchor
  EXPECT_GT(route.overview[512].longitudeE5, route.overview[511].longitudeE5 + 10'000'000);
}

TEST(RoutePackageV1Test, StaticCapacityAndSizeAssertions) {
  static_assert(RouteIndex::kMaxOverviewPoints == 1024, "overview line capacity");
  static_assert(RouteIndex::kMaxSegments == 64, "segment metadata capacity");
  static_assert(RouteIndex::kMaxManeuvers == 64, "maneuver metadata capacity");
  static_assert(RouteIndex::kMaxPoints == 16384, "decoder point-count ceiling");
  static_assert(navigator::kRoutePackageV1HeaderBytes == 38, "fixed header size");
  static_assert(navigator::kRoutePackageV1MaxBytes == 65535, "package ceiling");
  static_assert(navigator::kRoutePackageV1WorkBufferBytes == 1024, "work buffer");

  // The whole index must stay well under 10 KiB and far below one C3 task
  // stack; callers own it in static/global storage, never on a task stack.
  static_assert(sizeof(RouteIndex) < 10 * 1024, "RouteIndex must stay bounded");
  EXPECT_LT(sizeof(RouteIndex), 10u * 1024u);
  static_assert(sizeof(RouteIndex::overview) == sizeof(RouteIndex::Point) * RouteIndex::kMaxOverviewPoints,
                "overview array must match the documented capacity");
  static_assert(sizeof(RouteIndex::segments) / sizeof(RouteIndex::Segment) == RouteIndex::kMaxSegments,
                "segment array must match the documented capacity");
  static_assert(sizeof(RouteIndex::maneuvers) / sizeof(RouteIndex::Maneuver) == RouteIndex::kMaxManeuvers,
                "maneuver array must match the documented capacity");

  RouteIndex route;
  EXPECT_EQ(route.overviewPointCount, 0U);
  EXPECT_EQ(route.pointCount, 0U);
  EXPECT_EQ(route.maneuverCount, 0U);
  EXPECT_EQ(route.segmentCount, 0U);
}

}  // namespace
