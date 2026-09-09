#pragma once

#include <cstddef>
#include <cstdint>

// Bounded, streaming Route Package v1 validator/indexer for the X3 navigator.
//
// Wire contract (authoritative; shared with the Swift encoder):
//   * 38-byte little-endian header: magic "X3RT", version 1, flags 0,
//     headerSize 38, total package length including the trailing CRC32,
//     route id u32, point count u16, maneuver count u16, origin latitude and
//     longitude as signed E7 i32, total distance u32, estimated minutes u16,
//     segment count u16, route-name UTF-8 byte count u8, reserved byte 0.
//   * Payload: route name UTF-8 bytes; then segmentCount sorted, strictly
//     increasing start indices (u16) beginning at 0 and all < pointCount;
//     then geometry; then maneuver records; then a reflected IEEE CRC-32
//     over every preceding byte.
//   * Geometry: segment 0 begins at the header origin. Every later segment
//     begins with an absolute signed E7 latitude/longitude i32 pair. The
//     remaining points inside each segment are signed Int16 E5 deltas from
//     the previous point. The decoder quantizes each segment anchor to E5
//     (nearest, ties away from zero), accumulates deltas, and folds output
//     longitude at the antimeridian. Deltas never cross a segment boundary.
//     The validator reconstructs every point with this exact math in its first
//     pass and requires |latitudeE5| <= 9,000,000 and |folded longitudeE5| <=
//     18,000,000, so no syntactically valid delta stream can push geometry
//     outside the world.
//   * Maneuver record: point index u16, type u8 in 0..6, name byte count u8,
//     distance from start u32, then name UTF-8 bytes.
//   * A package is at most 65,535 bytes.
//   * A route.bin is the package itself, not a container: the source must
//     expose exactly the declared total length. Trailing bytes beyond the
//     declared length are detectable corruption, never ignorable padding.
//
// RAM / stack discipline (ESP32-C3 safe):
//   * The decoder never allocates, never retains the whole package, and reads
//     it strictly sequentially through RouteByteSource with every single read
//     request <= kRoutePackageV1WorkBufferBytes (1,024) bytes.
//   * validateRoutePackageV1() performs a full validation pass (structure,
//     reconstructed-geometry bounds, semantics and CRC) before a second
//     deterministic decode pass writes anything into the caller-owned
//     RouteIndex, so `out` is left untouched on every rejection. The function
//     needs only ~1.3 KiB of stack (its 1,024-byte work buffer plus a small
//     segment-start array); RouteIndex itself (~9.8 KiB) must be owned by the
//     caller in static/global storage, never placed on a task stack.
//   * No recursion, no heap, no exceptions, no RTTI, no filesystem or other
//     host-only APIs; every wire integer is decoded little-endian from bytes
//     (no unaligned reinterpret casts). Compiles for ESP32-C3 with C++20.
//
// DecodeStatus semantics (the order below is also the validation order):
//   Ok                   package validated; `out` filled.
//   NullInput            source reports zero total bytes (empty input).
//   TooShort             fewer than 38 bytes available, or the declared total
//                        length exceeds the source size (truncated package),
//                        or a read stalled before delivering requested bytes.
//   TooLarge             declared total length > 65,535.
//   BadMagic             first four bytes are not "X3RT".
//   UnsupportedVersion   version byte is not 1.
//   BadHeader            malformed header fields, out-of-world absolute
//                        coordinates or reconstructed E5 geometry (header
//                        origin, any later segment anchor, or any point a
//                        delta stream reconstructs outside the world), or an
//                        invalid segment-start list.
//   BadLength            declared length smaller than the header, or the
//                        counts require more content bytes than the declared
//                        length provides (segment list, geometry, or a
//                        maneuver fixed record overrun), or the source size
//                        exceeds the declared package length (trailing
//                        appended bytes).
//   BadCrc               trailing CRC-32 does not match the streamed content.
//   TooManyPoints        point count exceeds RouteIndex::kMaxPoints.
//   TooManySegments      segment count exceeds RouteIndex::kMaxSegments.
//   TooManyManeuvers     maneuver count exceeds RouteIndex::kMaxManeuvers.
//   BadUtf8Length        a name's declared UTF-8 byte count cannot be
//                        satisfied inside the package (truncated name).
//   BadUtf8              a name's bytes are not structurally valid UTF-8.
//   BadManeuver          a maneuver kind is outside 0..6 or its point index
//                        is outside [0, pointCount).
//   TrailingPayload      the declared package contains bytes between the end
//                        of the parsed content and the trailing CRC.
//
// Only bytes [0, declaredLength) form the package, and validateRoutePackageV1
// requires the source to expose exactly declaredLength bytes: a source larger
// than the declared package is BadLength (trailing appended bytes are
// detectable corruption) and a source smaller than the declared package is
// TooShort. Two statuses beyond the minimum set were necessary because no
// required status describes them precisely: TooManySegments (the fixed
// segment-index capacity must reject explicitly) and BadUtf8 (UTF-8
// structural validity is not a length problem).

namespace navigator {

// Wire and decoder constants shared by the validator and its callers.
inline constexpr uint32_t kRoutePackageV1MaxBytes = 65535;
inline constexpr uint32_t kRoutePackageV1HeaderBytes = 38;
inline constexpr uint32_t kRoutePackageV1WorkBufferBytes = 1024;
inline constexpr uint32_t kRoutePackageV1MaxNameBytes = 255;

enum class DecodeStatus : uint8_t {
  Ok = 0,
  NullInput,
  TooShort,
  TooLarge,
  BadMagic,
  UnsupportedVersion,
  BadHeader,
  BadLength,
  BadCrc,
  TooManyPoints,
  TooManySegments,
  TooManyManeuvers,
  BadUtf8Length,
  BadUtf8,
  BadManeuver,
  TrailingPayload,
};

// Random-access, bounded byte source over a route package. Host tests use
// in-memory vectors; the hardware build adapts an SD/HalStorage file later.
// Exact semantics implementations MUST follow:
//   * size() returns the total number of bytes the source can deliver. It may
//     not exceed 65,535 bytes: a route.bin is the package itself, not a
//     container, and validation requires size() == declaredLength exactly
//     (size() > declaredLength is BadLength, size() < declaredLength is
//     TooShort).
//   * read(offset, destination, length) copies up to `length` bytes starting
//     at `offset` into `destination` and returns the number copied. It never
//     reads past size(); when offset >= size() or length == 0 it returns 0.
//     A conforming implementation returns min(length, size() - offset) in one
//     call. The decoder additionally tolerates implementations that split one
//     request into several smaller reads (it re-requests the remainder at
//     offset + n). Returning 0 while bytes are still required is treated as
//     truncation (TooShort).
//   * `destination` must point to at least `length` writable bytes. Offsets,
//     lengths and sizes are uint32_t: packages are capped at 65,535 bytes and
//     the decoder never requests more than kRoutePackageV1WorkBufferBytes at
//     a time. Reads must be deterministic: a source that returns different
//     content for the same offset on a second pass violates the contract.
class RouteByteSource {
 public:
  virtual ~RouteByteSource() = default;

  virtual uint32_t size() const = 0;
  virtual uint32_t read(uint32_t offset, uint8_t* destination, uint32_t length) = 0;
};

// Reads exactly `length` bytes starting at `offset`, honouring the split-read
// tolerance documented above: read() may deliver fewer bytes than asked, so
// every caller that needs a whole range must re-request the remainder. A
// source that returns 0 while bytes are still required is treated as
// truncation and yields false. Every individual request is <= `length`.
bool readFully(RouteByteSource& source, uint32_t offset, uint8_t* destination, uint32_t length);

// Fixed-size, allocation-free result of a successful validation. The caller
// owns it (static/global storage on the C3) and zero-initializes it; only
// slots [0, segmentCount), [0, maneuverCount) and [0, overviewPointCount) are
// meaningful after a successful decode.
struct RouteIndex {
  // Fixed capacities (see rationale below). All three are compile-time so a
  // future firmware revision can raise them after measurement.
  //
  // kMaxPoints: the largest route point count the decoder accepts. 16,384 is
  // far beyond any turn-preserving simplified day-walk and is also larger
  // than any point count a <= 65,535-byte package can physically carry
  // (a single-segment package with no name/maneuvers fits at most 16,373
  // points), so this ceiling never rejects a representable valid package.
  //
  // kMaxSegments/kMaxManeuvers: RAM-derived metadata ceilings. Each segment
  // costs 12 bytes and each maneuver 12 bytes, so 64 each is ~1.5 KiB of
  // caller-owned RAM and covers day-walk GPX traces with ample margin.
  static constexpr uint16_t kMaxPoints = 16384;
  static constexpr uint16_t kMaxSegments = 64;
  static constexpr uint16_t kMaxManeuvers = 64;
  static constexpr uint16_t kMaxOverviewPoints = 1024;

  // One E5-quantized overview vertex (1 E5 = 1e-5 degree). E5 storage is the
  // wire geometry domain, needs no per-point folding state, and cannot
  // overflow int32 even for adversarial delta streams bounded by kMaxPoints.
  struct Point {
    int32_t latitudeE5 = 0;
    int32_t longitudeE5 = 0;
  };

  // Per-segment metadata: sourceOffset is the absolute package byte offset
  // where this segment's geometry begins (segment 0: its first delta; later
  // segments: their 8-byte absolute anchor), enabling later detailed
  // streaming reads. overviewBegin/overviewCount slice the overview array so
  // a renderer can draw each segment separately and never joins segments.
  struct Segment {
    uint32_t sourceOffset = 0;
    uint16_t startPointIndex = 0;
    uint16_t overviewBegin = 0;
    uint16_t overviewCount = 0;
  };

  // A maneuver is recorded by reference (offset into the source), never
  // copied, so the index stays small. `type` is the wire kind byte 0..6,
  // which maps 1:1 to navigator::Maneuver.
  struct Maneuver {
    uint32_t nameOffset = 0;
    uint32_t distanceFromStartMeters = 0;
    uint16_t pointIndex = 0;
    uint8_t type = 0;
    uint8_t nameLength = 0;
  };

  // --- Header metadata ---
  uint32_t declaredLength = 0;
  uint32_t routeId = 0;
  uint32_t totalDistanceMeters = 0;
  int32_t originLatitudeE7 = 0;
  int32_t originLongitudeE7 = 0;
  uint16_t routeNameOffset = 0;
  uint16_t routeNameLength = 0;
  uint16_t pointCount = 0;
  uint16_t segmentCount = 0;
  uint16_t maneuverCount = 0;
  uint16_t estimatedMinutes = 0;

  // --- Package offsets (absolute byte positions inside [0, declaredLength)) ---
  uint32_t payloadOffset = 0;        // start of the route name bytes (= 38)
  uint32_t segmentStartsOffset = 0;  // start of the segment start-index list
  uint32_t geometryOffset = 0;       // start of the geometry region
  uint32_t maneuverOffset = 0;       // start of the maneuver records
  uint32_t crcOffset = 0;            // start of the trailing CRC-32 (= declaredLength - 4)

  // --- Decoded, bounded overview line ---
  // The overview stores every route point when the whole route fits
  // (kMaxOverviewPoints); larger routes are sampled uniformly *within* each
  // segment while every segment's start and end point is always kept, so
  // segment discontinuities survive in the index and are never connected.
  uint16_t overviewPointCount = 0;
  Point overview[kMaxOverviewPoints];
  Segment segments[kMaxSegments];
  Maneuver maneuvers[kMaxManeuvers];
};

static_assert(sizeof(RouteIndex) < 10 * 1024, "RouteIndex must stay bounded (< 10 KiB)");

// Validates the Route Package v1 behind `source` and, on success, fills the
// caller-owned `out`. `out` is left byte-for-byte unchanged on every failure.
// See the file comment for the exact DecodeStatus semantics and validation
// order. Needs ~1.3 KiB of stack (its own 1,024-byte work buffer); RouteIndex
// must not live on a task stack. Never allocates, never reads the whole
// package into memory, and issues no source read larger than 1,024 bytes.
DecodeStatus validateRoutePackageV1(RouteByteSource& source, RouteIndex& out);

}  // namespace navigator
