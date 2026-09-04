#pragma once

#include <cstdint>

#include "RoutePackageV1.h"

// Bounded, hardware-independent receiver core for the Route Package v1 BLE
// transfer protocol (the firmware half of Task 7). The wire contract is shared
// with the Swift RouteTransferFrameBuilder and is little endian:
//   * 0x05 ROUTE_START  : opcode + routeId u32 + totalLength u32 +
//                         wholePackageCRC32 u32   (exactly 13 bytes)
//   The whole-package transport CRC is computed over every transmitted
//   package byte - content plus the package's trailing four-byte internal
//   CRC. For a correctly formed package that internal CRC is the content's
//   IEEE CRC, so the whole-file CRC evaluates to the constant residue
//   0x2144DF1C; it is the transport's redundancy check, distinct from the
//   content CRC that validateRoutePackageV1 still validates inside the
//   package.
//   * 0x06 ROUTE_CHUNK  : opcode + routeId u32 + offset u32 + a nonempty,
//                         contiguous payload slice of the package
//   * 0x07 ROUTE_COMMIT : opcode + routeId u32   (exactly 5 bytes)
//
// RAM / stack discipline (ESP32-C3 safe):
//   * RouteTransfer is a fixed-size state machine: no heap, no exceptions, no
//     RTTI, no recursion, no libm, no std::vector/string/function.
//   * It retains no full package and no BLE frame copy: accepted chunk payload
//     bytes are handed straight to the RouteSink, and a single inbound frame is
//     at most kMaxFrameBytes (512) bytes.
//   * The caller owns a RouteIndex (~9.8 KiB) staging/validation workspace in
//     static/global storage and passes it by reference (the `candidate`
//     parameter). It is NOT the caller's active route index: COMMIT's
//     successful decode writes into it even when publication later fails, so
//     the caller must pass a separate buffer, never its live active index,
//     and may consume or swap the candidate only after RouteAccepted.
//   * Commit uses one transient <= 1,024-byte work buffer for the streaming
//     whole-package CRC pass; every RouteByteSource read is <= 1,024 bytes.
//
// Receiver semantics:
//   * Frames are validated for exact sizes, opcodes, route id, and strictly
//     contiguous offsets. A chunk is appended only at offset == received.
//   * A duplicate chunk that lies wholly inside already-staged bytes is
//     idempotent only when the sink verifies the stored bytes match exactly
//     (never appended twice); anything else (gap, partial overlap, differing
//     duplicate, wrong id, zero payload, overshoot, START-while-active,
//     COMMIT-before-complete) is rejected as route-invalid without touching
//     the staged file or the active session.
//   * Storage failures abort the session, ask the sink to abandon route.tmp,
//     and are reported as route-storage-failed; the previous active route is
//     never disturbed by a failed transfer.
//   * COMMIT (with every byte staged) flushes the sink, stream-verifies the
//     whole-package transport CRC32 over every staged byte (including the
//     package's four-byte internal trailer), runs validateRoutePackageV1 over
//     the staged RouteByteSource into the candidate workspace, rejects a
//     decoded package whose header route id disagrees with the START route id,
//     and only then publishes transactionally. Every failure path cleans
//     route.tmp while the previous active route stays usable; the candidate
//     workspace may already hold a decode when COMMIT fails after validation.

namespace navigator {

// Route transfer wire opcodes (distinct from the dashboard 0x01...0x04
// framing that stays untouched).
inline constexpr uint8_t kRouteOpcodeStart = 0x05;
inline constexpr uint8_t kRouteOpcodeChunk = 0x06;
inline constexpr uint8_t kRouteOpcodeCommit = 0x07;

// Statuses exposed by the route-transfer core; the numeric values are the
// status bytes of the later seven-byte BLE status envelope.
enum class RouteTransferCode : uint8_t {
  RouteReady = 0x21,         // START accepted; receiver armed for chunks
  RouteProgress = 0x22,      // a contiguous chunk was accepted
  RouteAccepted = 0x23,      // COMMIT validated and promoted the route
  RouteInvalid = 0x24,       // frame/protocol or package validation rejection
  RouteStorageFailed = 0x25  // storage I/O failure (temp cleaned, active kept)
};

// One receiver result. The three fields are exactly the payload of the
// seven-byte BLE status envelope (status code, routeId u32, received u16);
// writeEnvelope() serializes them little endian into seven bytes.
struct RouteTransferStatus {
  RouteTransferCode code = RouteTransferCode::RouteInvalid;
  uint32_t routeId = 0;
  uint16_t received = 0;  // package bytes accepted so far (<= 65,535)

  // Writes the little-endian seven-byte envelope into `out` (must hold 7
  // bytes): [code][routeId u32 LE][received u16 LE].
  void writeEnvelope(uint8_t out[7]) const;
};

// Abstract sink for one staged route transaction (implemented by RouteStore
// over the RouteFileSystem abstraction; a host test may substitute a fake).
// The receiver hands each accepted chunk payload straight to the sink - it
// never buffers a whole package.
class RouteSink {
 public:
  virtual ~RouteSink() = default;

  enum class VerifyResult : uint8_t {
    Match,      // staged file holds exactly `payload` at `offset`
    Differ,     // staged bytes disagree with `payload`
    ReadFailed  // the staged file could not be read back
  };

  // Opens a fresh staged transaction for a package of `declaredLength` bytes
  // (ROUTE_START). false on storage failure - including a prospective
  // directory-quota rejection that leaves any active route and recovery
  // backup untouched - and no session may start then. A failed begin may have
  // left a partial route.tmp behind; RouteTransfer best-effort abandons it.
  virtual bool begin(uint32_t declaredLength) = 0;

  // Appends one accepted contiguous payload slice at the end of the staged
  // file. All-or-nothing: false means the slice was not durably staged.
  virtual bool append(const uint8_t* payload, uint32_t length) = 0;

  // Verifies that the staged file already holds exactly `payload` at `offset`
  // (duplicate-chunk idempotence check).
  virtual VerifyResult matchesStaged(uint32_t offset, const uint8_t* payload, uint32_t length) = 0;

  // Makes the staged bytes durable before commit validation.
  virtual bool flush() = 0;

  // A bounded RouteByteSource over the staged file, used by the commit CRC
  // pass and Route Package v1 validation without loading the file into RAM.
  // Valid until publish() moves the staged file away.
  virtual RouteByteSource& stagedSource() = 0;

  // Best-effort abandonment of the staged transaction: route.tmp is removed
  // when the file system allows it and any previously active route is
  // untouched. Cleanup is best-effort only - if the file system reports a
  // remove failure, the stale route.tmp may remain and callers must not assume
  // it is gone.
  virtual void abandon() = 0;

  // Transactionally promotes the validated staged file to the active route.
  // On false the previous active route remains usable (restored to route.bin
  // or kept recoverably at route.bak).
  virtual bool publish() = 0;
};

// Fixed-size, allocation-free Route Transfer v1 receiver state machine.
class RouteTransfer {
 public:
  // Largest inbound frame the receiver accepts (the maximum BLE write length
  // the transfer contract tests: 20 / 185 / 512).
  static constexpr uint32_t kMaxFrameBytes = 512;
  static constexpr uint32_t kStartFrameBytes = 13;
  static constexpr uint32_t kChunkHeaderBytes = 9;  // opcode + id + offset
  static constexpr uint32_t kCommitFrameBytes = 5;
  static constexpr uint32_t kMaxChunkPayloadBytes = kMaxFrameBytes - kChunkHeaderBytes;

  RouteTransfer() = default;

  bool isActive() const { return active_; }
  uint32_t routeId() const { return routeId_; }
  uint32_t totalLength() const { return totalLength_; }
  uint32_t receivedBytes() const { return received_; }

  // Handles one inbound frame (up to kMaxFrameBytes bytes, no copy made).
  // `candidate` is the caller-owned RouteIndex staging/validation workspace
  // (static/global storage on the C3) used only by COMMIT. It is NOT the
  // caller's active route index: COMMIT decodes the validated package into it,
  // so it may be overwritten even when publication later fails; the caller may
  // consume or swap the candidate only after RouteAccepted.
  RouteTransferStatus handleFrame(const uint8_t* frame, uint32_t frameLength, RouteSink& sink,
                                  RouteIndex& candidate);

  // Drops any active session and asks `sink` to abandon the staged file.
  // Callers (e.g. the BLE layer) use this on disconnect/retry.
  void abortTransfer(RouteSink& sink);

 private:
  RouteTransferStatus handleStart(const uint8_t* frame, uint32_t frameLength, RouteSink& sink);
  RouteTransferStatus handleChunk(const uint8_t* frame, uint32_t frameLength, RouteSink& sink);
  RouteTransferStatus handleCommit(const uint8_t* frame, uint32_t frameLength, RouteSink& sink,
                                   RouteIndex& candidate);
  RouteTransferStatus makeStatus(RouteTransferCode code) const;
  void clearSession();

  bool active_ = false;
  uint32_t routeId_ = 0;
  uint32_t totalLength_ = 0;
  uint32_t expectedCrc_ = 0;
  uint32_t received_ = 0;
};

}  // namespace navigator
