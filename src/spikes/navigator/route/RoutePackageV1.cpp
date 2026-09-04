#include "RoutePackageV1.h"

#include <cstdint>

namespace navigator {
namespace {

constexpr uint8_t kVersion = 1;
constexpr uint8_t kMaxManeuverKind = 6;
constexpr int32_t kLatitudeMaxE7 = 900000000;
constexpr int32_t kLatitudeMinE7 = -900000000;
constexpr int32_t kLongitudeMaxE7 = 1800000000;
constexpr int32_t kLongitudeMinE7 = -1800000000;
// Reconstructed geometry lives in E5. Latitude is bounded by the world
// (+/-90 degrees = +/-9,000,000 E5); longitude is folded at the antimeridian,
// so the folded value is always within +/-18,000,000 E5.
constexpr int64_t kLatitudeMaxE5 = 9000000;
constexpr int64_t kLatitudeMinE5 = -9000000;
constexpr int64_t kLongitudeMaxE5 = 18000000;
constexpr int64_t kLongitudeMinE5 = -18000000;
constexpr int64_t kLongitudeFullTurnE5 = 36000000;
constexpr int64_t kLongitudeHalfTurnE5 = 18000000;

inline uint16_t readU16(const uint8_t* p) {
  return static_cast<uint16_t>(p[0]) | static_cast<uint16_t>(static_cast<uint16_t>(p[1]) << 8);
}

inline uint32_t readU32(const uint8_t* p) {
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) | (static_cast<uint32_t>(p[2]) << 16) |
         (static_cast<uint32_t>(p[3]) << 24);
}

inline int32_t readI32(const uint8_t* p) { return static_cast<int32_t>(readU32(p)); }

// Reflected IEEE CRC-32 (polynomial 0xEDB88320), identical to the encoder's
// CRC32.checksum and to zlib crc32: init 0xFFFFFFFF, final XOR 0xFFFFFFFF.
inline uint32_t crcStep(uint32_t crc, uint8_t byte) {
  crc ^= byte;
  for (unsigned i = 0; i < 8; ++i) {
    const uint32_t mask = 0u - (crc & 1u);
    crc = (crc >> 1) ^ (0xEDB88320u & mask);
  }
  return crc;
}

// Reads exactly `length` bytes starting at `offset`. A conforming source
// returns the whole range in one call; sources that split a request into
// smaller reads are tolerated by re-requesting the remainder. Returns false
// when the source stops delivering bytes before `length` were received.
// Every single request passed to RouteByteSource::read is <= `length`, and
// callers never pass more than kRoutePackageV1WorkBufferBytes.
bool readFully(RouteByteSource& source, uint32_t offset, uint8_t* destination, uint32_t length) {
  uint32_t done = 0;
  while (done < length) {
    const uint32_t got = source.read(offset + done, destination + done, length - done);
    if (got == 0) {
      return false;
    }
    done += got;
  }
  return true;
}

// E7 -> E5 quantization: nearest multiple of 100 E7 units, ties away from
// zero. Deterministic integer arithmetic shared with the Swift encoder.
inline int64_t quantizeE5(int32_t e7) {
  const int64_t value = e7;
  const int64_t magnitude = value < 0 ? -value : value;
  const int64_t quantized = (magnitude + 50) / 100;
  return value < 0 ? -quantized : quantized;
}

// Folds an accumulated E5 longitude back into [-180, +180] degrees. Valid
// packages keep the accumulator within one full turn of the fold boundary, so
// this loop runs a handful of times even for adversarial delta streams.
inline int64_t foldLongitudeE5(int64_t e5) {
  while (e5 > kLongitudeHalfTurnE5) {
    e5 -= kLongitudeFullTurnE5;
  }
  while (e5 < -kLongitudeHalfTurnE5) {
    e5 += kLongitudeFullTurnE5;
  }
  return e5;
}

inline bool insideWorldE7(int32_t latitudeE7, int32_t longitudeE7) {
  return latitudeE7 >= kLatitudeMinE7 && latitudeE7 <= kLatitudeMaxE7 && longitudeE7 >= kLongitudeMinE7 &&
         longitudeE7 <= kLongitudeMaxE7;
}

inline bool insideWorldE5(int64_t latitudeE5, int64_t longitudeE5) {
  return latitudeE5 >= kLatitudeMinE5 && latitudeE5 <= kLatitudeMaxE5 && longitudeE5 >= kLongitudeMinE5 &&
         longitudeE5 <= kLongitudeMaxE5;
}

// Advances one reconstructed route point by a signed Int16 E5 delta pair,
// folding longitude at the antimeridian exactly as the Swift encoder does.
// Both the validation pass and the fill pass call this so the two passes
// cannot disagree about the reconstruction for a deterministic source.
inline void applyE5Delta(int64_t& latitudeE5, int64_t& longitudeE5, int16_t deltaLatitudeE5,
                         int16_t deltaLongitudeE5) {
  latitudeE5 += deltaLatitudeE5;
  longitudeE5 = foldLongitudeE5(longitudeE5 + deltaLongitudeE5);
}

// Minimal RFC 3629 structural UTF-8 validation: no overlong encodings, no
// surrogate code points, no code points above U+10FFFF, no stray continuation
// bytes, and no truncated sequences. Opaque byte blobs otherwise; the decoder
// never transcodes names.
bool structurallyValidUtf8(const uint8_t* bytes, uint32_t length) {
  uint32_t i = 0;
  while (i < length) {
    const uint8_t lead = bytes[i];
    if (lead <= 0x7F) {
      ++i;
      continue;
    }
    if (lead >= 0xC2 && lead <= 0xDF) {
      if (i + 1 >= length || (bytes[i + 1] & 0xC0u) != 0x80u) {
        return false;
      }
      i += 2;
      continue;
    }
    if (lead >= 0xE0 && lead <= 0xEF) {
      if (i + 2 >= length) {
        return false;
      }
      const uint8_t second = bytes[i + 1];
      const uint8_t third = bytes[i + 2];
      if ((second & 0xC0u) != 0x80u || (third & 0xC0u) != 0x80u) {
        return false;
      }
      if (lead == 0xE0 && second < 0xA0) {
        return false;  // overlong 3-byte encoding
      }
      if (lead == 0xED && second > 0x9F) {
        return false;  // surrogate code point
      }
      i += 3;
      continue;
    }
    if (lead >= 0xF0 && lead <= 0xF4) {
      if (i + 3 >= length) {
        return false;
      }
      const uint8_t second = bytes[i + 1];
      const uint8_t third = bytes[i + 2];
      const uint8_t fourth = bytes[i + 3];
      if ((second & 0xC0u) != 0x80u || (third & 0xC0u) != 0x80u || (fourth & 0xC0u) != 0x80u) {
        return false;
      }
      if (lead == 0xF0 && second < 0x90) {
        return false;  // overlong 4-byte encoding
      }
      if (lead == 0xF4 && second > 0x8F) {
        return false;  // above U+10FFFF
      }
      i += 4;
      continue;
    }
    return false;  // 0x80..0xC1 stray continuation or 0xF5..0xFF lead
  }
  return true;
}

// Number of route points in segment `segment` (>= 1 by construction).
inline uint32_t segmentSpan(const uint16_t* starts, uint16_t segmentCount, uint16_t segment, uint16_t pointCount) {
  const uint32_t next =
      static_cast<uint32_t>(segment) + 1u < static_cast<uint32_t>(segmentCount) ? starts[segment + 1] : pointCount;
  return next - starts[segment];
}

// Scalar header fields plus the package offsets derived from them. Computed
// during the validation pass and reused verbatim by the fill pass, so the two
// passes cannot disagree about layout.
struct ParsedHeader {
  uint32_t declaredLength = 0;
  uint32_t routeId = 0;
  uint32_t totalDistanceMeters = 0;
  int32_t originLatitudeE7 = 0;
  int32_t originLongitudeE7 = 0;
  uint16_t pointCount = 0;
  uint16_t segmentCount = 0;
  uint16_t maneuverCount = 0;
  uint16_t estimatedMinutes = 0;
  uint8_t routeNameLength = 0;
  uint32_t segmentStartsOffset = 0;
  uint32_t geometryOffset = 0;
  uint32_t maneuverOffset = 0;
  uint32_t crcOffset = 0;
};

// Second, guaranteed-successful decode pass. Runs only after validation (and
// CRC) passed, so it performs no checks of its own and can only fail when a
// non-conforming source delivers different data than during validation. It
// reconstructs geometry through the same applyE5Delta math the validation pass
// used, so for a conforming source the two passes cannot disagree. Populates
// every RouteIndex field; `out` is written exclusively here.
bool fillRouteIndex(RouteByteSource& source, uint8_t* work, const ParsedHeader& header, RouteIndex& out) {
  uint32_t pos = 0;
  const auto readAt = [&](uint32_t count) -> const uint8_t* {
    if (count == 0) {
      return work;
    }
    if (count > kRoutePackageV1WorkBufferBytes || !readFully(source, pos, work, count)) {
      return nullptr;
    }
    pos += count;
    return work;
  };

  if (readAt(kRoutePackageV1HeaderBytes) == nullptr) {
    return false;
  }
  if (readAt(header.routeNameLength) == nullptr) {
    return false;
  }
  const uint32_t segmentStartsBytes = static_cast<uint32_t>(header.segmentCount) * 2u;
  uint16_t starts[RouteIndex::kMaxSegments];
  {
    const uint8_t* startsBytes = readAt(segmentStartsBytes);
    if (startsBytes == nullptr) {
      return false;
    }
    for (uint16_t i = 0; i < header.segmentCount; ++i) {
      starts[i] = readU16(startsBytes + 2u * static_cast<uint32_t>(i));
    }
  }

  // Uniform bounded overview sampler. Every multi-point segment always keeps
  // its first and last route point (single-point segments keep their one
  // point); remaining overview capacity is distributed among interior points
  // in proportion to each segment's interior size, all in integer arithmetic.
  // The sampler is documented as approximate for very long routes: segment
  // endpoints and disjoint slices are preserved, interior spacing is not
  // guaranteed to be perfectly even.
  uint16_t interiorSamples[RouteIndex::kMaxSegments];
  {
    uint32_t reserved = 0;
    uint32_t totalInterior = 0;
    for (uint16_t seg = 0; seg < header.segmentCount; ++seg) {
      const uint32_t span = segmentSpan(starts, header.segmentCount, seg, header.pointCount);
      const uint32_t interior = span >= 2 ? span - 2 : 0;
      totalInterior += interior;
      reserved += span == 1 ? 1u : 2u;
    }
    const uint32_t capacity = static_cast<uint32_t>(RouteIndex::kMaxOverviewPoints);
    const uint32_t available = reserved < capacity ? capacity - reserved : 0;
    for (uint16_t seg = 0; seg < header.segmentCount; ++seg) {
      const uint32_t span = segmentSpan(starts, header.segmentCount, seg, header.pointCount);
      const uint32_t interior = span >= 2 ? span - 2 : 0;
      uint32_t samples = interior;
      if (totalInterior > available && interior > 0) {
        samples = static_cast<uint32_t>((static_cast<uint64_t>(interior) * available) / totalInterior);
        if (samples > interior) {
          samples = interior;
        }
      }
      interiorSamples[seg] = static_cast<uint16_t>(samples);
    }
  }

  out.declaredLength = header.declaredLength;
  out.routeId = header.routeId;
  out.totalDistanceMeters = header.totalDistanceMeters;
  out.originLatitudeE7 = header.originLatitudeE7;
  out.originLongitudeE7 = header.originLongitudeE7;
  out.routeNameOffset = static_cast<uint16_t>(kRoutePackageV1HeaderBytes);
  out.routeNameLength = header.routeNameLength;
  out.pointCount = header.pointCount;
  out.segmentCount = header.segmentCount;
  out.maneuverCount = header.maneuverCount;
  out.estimatedMinutes = header.estimatedMinutes;
  out.payloadOffset = static_cast<uint32_t>(kRoutePackageV1HeaderBytes);
  out.segmentStartsOffset = header.segmentStartsOffset;
  out.geometryOffset = header.geometryOffset;
  out.maneuverOffset = header.maneuverOffset;
  out.crcOffset = header.crcOffset;
  out.overviewPointCount = 0;

  uint16_t overviewCursor = 0;
  for (uint16_t seg = 0; seg < header.segmentCount; ++seg) {
    const uint32_t span = segmentSpan(starts, header.segmentCount, seg, header.pointCount);
    RouteIndex::Segment& segment = out.segments[seg];
    segment.startPointIndex = starts[seg];
    segment.sourceOffset = seg == 0 ? out.geometryOffset : pos;
    segment.overviewBegin = overviewCursor;

    int64_t latitudeE5 = 0;
    int64_t longitudeE5 = 0;
    if (seg == 0) {
      latitudeE5 = quantizeE5(header.originLatitudeE7);
      longitudeE5 = quantizeE5(header.originLongitudeE7);
    } else {
      const uint8_t* anchor = readAt(8);
      if (anchor == nullptr) {
        return false;
      }
      latitudeE5 = quantizeE5(readI32(anchor));
      longitudeE5 = quantizeE5(readI32(anchor + 4));
    }

    const auto storePoint = [&](int64_t latE5, int64_t lonE5) {
      if (overviewCursor < RouteIndex::kMaxOverviewPoints) {
        out.overview[overviewCursor].latitudeE5 = static_cast<int32_t>(latE5);
        out.overview[overviewCursor].longitudeE5 = static_cast<int32_t>(lonE5);
        ++overviewCursor;
      }
    };
    storePoint(latitudeE5, longitudeE5);  // mandatory first point of the segment

    if (span >= 2) {
      const uint32_t interiorCount = span - 2;
      const uint32_t samples = interiorSamples[seg];
      uint32_t chosen = 0;
      for (uint32_t j = 1; j < span; ++j) {
        const uint8_t* deltaBytes = readAt(4);
        if (deltaBytes == nullptr) {
          return false;
        }
        applyE5Delta(latitudeE5, longitudeE5, static_cast<int16_t>(readU16(deltaBytes)),
                     static_cast<int16_t>(readU16(deltaBytes + 2)));

        const bool isEnd = j + 1 == span;
        bool store = isEnd;
        if (!store && j <= interiorCount && chosen < samples) {
          if (samples >= interiorCount) {
            store = true;
          } else {
            const uint32_t offset = ((chosen + 1u) * interiorCount) / (samples + 1u);
            if (j == offset) {
              store = true;
              ++chosen;
            }
          }
        }
        if (store) {
          storePoint(latitudeE5, longitudeE5);
        }
      }
    }
    segment.overviewCount = static_cast<uint16_t>(overviewCursor - segment.overviewBegin);
  }

  for (uint16_t m = 0; m < header.maneuverCount; ++m) {
    const uint8_t* record = readAt(8);
    if (record == nullptr) {
      return false;
    }
    RouteIndex::Maneuver& maneuver = out.maneuvers[m];
    maneuver.pointIndex = readU16(record);
    maneuver.type = record[2];
    maneuver.nameLength = record[3];
    maneuver.distanceFromStartMeters = readU32(record + 4);
    maneuver.nameOffset = pos;
    if (readAt(maneuver.nameLength) == nullptr) {
      return false;
    }
  }
  out.overviewPointCount = overviewCursor;
  return true;
}

}  // namespace

DecodeStatus validateRoutePackageV1(RouteByteSource& source, RouteIndex& out) {
  const uint32_t available = source.size();
  if (available == 0) {
    return DecodeStatus::NullInput;
  }
  if (available < kRoutePackageV1HeaderBytes) {
    return DecodeStatus::TooShort;
  }

  // Single 1,024-byte work buffer for the whole function (the sanctioned
  // streaming buffer from the wire design); every source read is <= 1,024.
  uint8_t work[kRoutePackageV1WorkBufferBytes];
  uint32_t pos = 0;
  uint32_t crc = 0xFFFFFFFFu;
  const auto readContent = [&](uint32_t count) -> const uint8_t* {
    if (count == 0) {
      return work;
    }
    if (count > kRoutePackageV1WorkBufferBytes || !readFully(source, pos, work, count)) {
      return nullptr;
    }
    for (uint32_t i = 0; i < count; ++i) {
      crc = crcStep(crc, work[i]);
    }
    pos += count;
    return work;
  };

  const uint8_t* header = readContent(kRoutePackageV1HeaderBytes);
  if (header == nullptr) {
    return DecodeStatus::TooShort;
  }
  if (header[0] != 'X' || header[1] != '3' || header[2] != 'R' || header[3] != 'T') {
    return DecodeStatus::BadMagic;
  }
  if (header[4] != kVersion) {
    return DecodeStatus::UnsupportedVersion;
  }
  const uint16_t headerSize = readU16(header + 6);
  const uint8_t flags = header[5];
  if (headerSize != kRoutePackageV1HeaderBytes || flags != 0 || header[37] != 0) {
    return DecodeStatus::BadHeader;
  }

  ParsedHeader parsed;
  parsed.declaredLength = readU32(header + 8);
  parsed.routeId = readU32(header + 12);
  parsed.pointCount = readU16(header + 16);
  parsed.maneuverCount = readU16(header + 18);
  parsed.originLatitudeE7 = readI32(header + 20);
  parsed.originLongitudeE7 = readI32(header + 24);
  parsed.totalDistanceMeters = readU32(header + 28);
  parsed.estimatedMinutes = readU16(header + 32);
  parsed.segmentCount = readU16(header + 34);
  parsed.routeNameLength = header[36];

  // Header-level consistency and fixed decoder capacities.
  if (parsed.pointCount == 0 || parsed.segmentCount == 0 || parsed.segmentCount > parsed.pointCount) {
    return DecodeStatus::BadHeader;
  }
  if (!insideWorldE7(parsed.originLatitudeE7, parsed.originLongitudeE7)) {
    return DecodeStatus::BadHeader;
  }
  if (parsed.pointCount > RouteIndex::kMaxPoints) {
    return DecodeStatus::TooManyPoints;
  }
  if (parsed.segmentCount > RouteIndex::kMaxSegments) {
    return DecodeStatus::TooManySegments;
  }
  if (parsed.maneuverCount > RouteIndex::kMaxManeuvers) {
    return DecodeStatus::TooManyManeuvers;
  }

  // Declared package length consistency (all arithmetic in uint64_t).
  if (parsed.declaredLength > kRoutePackageV1MaxBytes) {
    return DecodeStatus::TooLarge;
  }
  if (parsed.declaredLength < kRoutePackageV1HeaderBytes) {
    return DecodeStatus::BadLength;
  }
  // A route.bin is the package itself, not a container: the source must
  // expose exactly the declared package length. Extra trailing bytes are
  // detectable corruption (BadLength); a truncated package stays TooShort.
  if (parsed.declaredLength > available) {
    return DecodeStatus::TooShort;
  }
  if (parsed.declaredLength < available) {
    return DecodeStatus::BadLength;
  }
  const uint32_t contentEnd = parsed.declaredLength - 4;

  const uint64_t nameOffset = kRoutePackageV1HeaderBytes;
  const uint64_t segmentStartsOffset = nameOffset + parsed.routeNameLength;
  const uint64_t segmentStartsBytes = static_cast<uint64_t>(parsed.segmentCount) * 2u;
  const uint64_t geometryBytes =
      (static_cast<uint64_t>(parsed.segmentCount) - 1u) * 8u +
      (static_cast<uint64_t>(parsed.pointCount) - static_cast<uint64_t>(parsed.segmentCount)) * 4u;
  const uint64_t maneuverOffset = segmentStartsOffset + segmentStartsBytes + geometryBytes;

  if (nameOffset + parsed.routeNameLength > contentEnd) {
    return DecodeStatus::BadUtf8Length;
  }
  if (segmentStartsOffset + segmentStartsBytes > contentEnd || maneuverOffset > contentEnd) {
    return DecodeStatus::BadLength;
  }

  parsed.segmentStartsOffset = static_cast<uint32_t>(segmentStartsOffset);
  parsed.geometryOffset = static_cast<uint32_t>(segmentStartsOffset + segmentStartsBytes);
  parsed.maneuverOffset = static_cast<uint32_t>(maneuverOffset);
  parsed.crcOffset = parsed.declaredLength - 4;

  // Route name bytes: structural UTF-8 only; the name itself is referenced by
  // offset later, never copied.
  const uint8_t* name = readContent(parsed.routeNameLength);
  if (name == nullptr) {
    return DecodeStatus::TooShort;
  }
  if (!structurallyValidUtf8(name, parsed.routeNameLength)) {
    return DecodeStatus::BadUtf8;
  }

  // Segment start list: first element 0, strictly increasing, all < pointCount
  // (each segment therefore holds at least one point).
  uint16_t starts[RouteIndex::kMaxSegments];
  {
    const uint8_t* startsBytes = readContent(static_cast<uint32_t>(segmentStartsBytes));
    if (startsBytes == nullptr) {
      return DecodeStatus::TooShort;
    }
    for (uint16_t i = 0; i < parsed.segmentCount; ++i) {
      starts[i] = readU16(startsBytes + 2u * static_cast<uint32_t>(i));
    }
    if (starts[0] != 0) {
      return DecodeStatus::BadHeader;
    }
    for (uint16_t i = 1; i < parsed.segmentCount; ++i) {
      if (starts[i] <= starts[i - 1] || starts[i] >= parsed.pointCount) {
        return DecodeStatus::BadHeader;
      }
    }
  }

  // Geometry region: consume every byte (CRC) and reconstruct the full E5
  // geometry exactly as the fill pass will, so a syntactically valid Int16
  // delta stream can never move a reconstructed point outside the world.
  // Segment 0 starts at the E5-quantized header origin; every later segment
  // starts at its absolute E7 anchor (validated against world bounds)
  // quantized to E5. Each reconstructed point must satisfy
  // |latitudeE5| <= 9,000,000 and |folded longitudeE5| <= 18,000,000.
  for (uint16_t seg = 0; seg < parsed.segmentCount; ++seg) {
    const uint32_t span = segmentSpan(starts, parsed.segmentCount, seg, parsed.pointCount);
    int64_t latitudeE5 = 0;
    int64_t longitudeE5 = 0;
    if (seg > 0) {
      const uint8_t* anchor = readContent(8);
      if (anchor == nullptr) {
        return DecodeStatus::TooShort;
      }
      const int32_t anchorLat = readI32(anchor);
      const int32_t anchorLon = readI32(anchor + 4);
      if (!insideWorldE7(anchorLat, anchorLon)) {
        return DecodeStatus::BadHeader;
      }
      latitudeE5 = quantizeE5(anchorLat);
      longitudeE5 = quantizeE5(anchorLon);
    } else {
      latitudeE5 = quantizeE5(parsed.originLatitudeE7);
      longitudeE5 = quantizeE5(parsed.originLongitudeE7);
    }
    if (!insideWorldE5(latitudeE5, longitudeE5)) {
      return DecodeStatus::BadHeader;
    }

    // Interior points are signed Int16 E5 delta pairs, consumed in bounded
    // chunks (never more than the 1,024-byte work buffer; each chunk holds a
    // whole number of 4-byte pairs). The CRC sees every byte in order.
    uint32_t interior = span - 1u;
    while (interior > 0) {
      const uint32_t chunkBytes = interior * 4u <= kRoutePackageV1WorkBufferBytes
                                      ? interior * 4u
                                      : static_cast<uint32_t>(kRoutePackageV1WorkBufferBytes);
      const uint8_t* deltaChunk = readContent(chunkBytes);
      if (deltaChunk == nullptr) {
        return DecodeStatus::TooShort;
      }
      for (uint32_t i = 0; i < chunkBytes; i += 4) {
        applyE5Delta(latitudeE5, longitudeE5, static_cast<int16_t>(readU16(deltaChunk + i)),
                     static_cast<int16_t>(readU16(deltaChunk + i + 2)));
        if (!insideWorldE5(latitudeE5, longitudeE5)) {
          return DecodeStatus::BadHeader;
        }
      }
      interior -= chunkBytes / 4u;
    }
  }

  // Maneuver records.
  for (uint16_t m = 0; m < parsed.maneuverCount; ++m) {
    if (static_cast<uint64_t>(pos) + 8 > contentEnd) {
      return DecodeStatus::BadLength;
    }
    const uint8_t* record = readContent(8);
    if (record == nullptr) {
      return DecodeStatus::TooShort;
    }
    const uint16_t pointIndex = readU16(record);
    const uint8_t type = record[2];
    const uint8_t nameLength = record[3];
    if (pointIndex >= parsed.pointCount || type > kMaxManeuverKind) {
      return DecodeStatus::BadManeuver;
    }
    if (static_cast<uint64_t>(pos) + nameLength > contentEnd) {
      return DecodeStatus::BadUtf8Length;
    }
    const uint8_t* maneuverName = readContent(nameLength);
    if (maneuverName == nullptr) {
      return DecodeStatus::TooShort;
    }
    if (!structurallyValidUtf8(maneuverName, nameLength)) {
      return DecodeStatus::BadUtf8;
    }
  }

  // Exact payload consumption: every byte of the declared content must be
  // accounted for by the header, name, segment list, geometry and maneuvers.
  if (pos != contentEnd) {
    return DecodeStatus::TrailingPayload;
  }

  // Streaming CRC over bytes [0, contentEnd), compared against the stored
  // little-endian CRC-32 at [contentEnd, declaredLength).
  uint8_t storedCrcBytes[4];
  if (!readFully(source, contentEnd, storedCrcBytes, 4)) {
    return DecodeStatus::TooShort;
  }
  const uint32_t storedCrc = readU32(storedCrcBytes);
  if (storedCrc != (crc ^ 0xFFFFFFFFu)) {
    return DecodeStatus::BadCrc;
  }

  // Everything validated (including the CRC): only now publish `out`.
  if (!fillRouteIndex(source, work, parsed, out)) {
    // Only a source that violates the deterministic/read contract can fail
    // here; report it as truncation rather than silently publishing a
    // partially filled index.
    return DecodeStatus::TooShort;
  }
  return DecodeStatus::Ok;
}

}  // namespace navigator
