#pragma once

#include <cstdint>

#include "RoutePackageV1.h"
#include "RouteTransfer.h"

// Transactional storage of the single active walking route, behind a
// testable file abstraction (Task 7, firmware core).
//
// Storage layout (canonical fixed paths; no dynamic path is ever built):
//   * /Navigation/Routes/active/route.tmp - the staged transfer. Payload bytes
//     are appended here as chunks arrive; it is only ever renamed to route.bin
//     after the whole-package transport CRC and Route Package v1 validation
//     (streamed through stagedSource()) both passed.
//   * /Navigation/Routes/active/route.bin - the active route the navigator
//     reads. It is replaced atomically-ish via the backup slot and is never
//     touched while a transfer is staged, fails, or is quota-rejected.
//   * /Navigation/Routes/active/route.bak - recoverable copy of the previous
//     active route held across the promotion rename.
//
// Storage policy: the active-route directory is capped at a 256 KiB quota
// (kRouteStoreQuotaBytes) which comfortably fits route.tmp + route.bin +
// route.bak at the 65,535-byte Route Package v1 transport ceiling. A staged
// route never exceeds kRoutePackageV1MaxBytes. RouteStore::begin() enforces
// the quota prospectively - the declared route length plus the sizes of the
// canonical files that will remain after its cleanup (route.bin, and route.bak
// only while route.bin is absent as the recovery copy), summed in uint64_t -
// before any file is touched, so an oversized pre-existing active route or
// recovery backup makes begin() fail while both stay byte-for-byte untouched.
//
// RAM / stack discipline (ESP32-C3 safe):
//   * RouteStore keeps no buffers in its state; reads for duplicate
//     verification use a transient <= 1,024-byte stack buffer, and the staged
//     RouteByteSource forwards RoutePackageV1's own <= 1,024-byte requests.
//   * No heap, no exceptions, no RTTI, no recursion, no libm, no
//     std::vector/string/function. The whole route is never loaded into RAM.
//
// The production SD adapter (over the existing SD card manager) is a separate
// later change; host tests drive RouteStore over an in-memory bounded fake.

namespace navigator {

// Canonical fixed paths of the active-route store.
inline constexpr char kRouteActiveTempPath[] = "/Navigation/Routes/active/route.tmp";
inline constexpr char kRouteActiveBinPath[] = "/Navigation/Routes/active/route.bin";
inline constexpr char kRouteActiveBackupPath[] = "/Navigation/Routes/active/route.bak";

// Storage-policy quota for the active-route directory (256 KiB).
inline constexpr uint32_t kRouteStoreQuotaBytes = 256u * 1024u;

// Synchronous, complete-operation file abstraction over the three canonical
// route files. Every call names one of the canonical paths above and each call
// opens, operates, and closes internally, so an implementation (in-memory fake
// on the host, SD card adapter on hardware) never needs more than one file
// open at a time. Paths never escape this layer and are never built
// dynamically.
//
// Exact semantics implementations MUST follow:
//   * read(path, offset, destination, length) copies up to `length` bytes
//     starting at `offset` and returns the number copied; it never reads past
//     the file end, returns 0 when the file is absent or offset >= size, and
//     may return fewer bytes than requested only to model a stalled read.
//   * append(path, bytes, length) is all-or-nothing: false unless the whole
//     range was written.
//   * move(from, to) moves `from` onto `to`; `to` must not exist (plain
//     rename). false only when nothing moved, in which case `from` is
//     unchanged and `to` keeps its previous (absent) state.
//   * remove(path) of an absent file is success.
class RouteFileSystem {
 public:
  virtual ~RouteFileSystem() = default;

  virtual bool exists(const char* path) = 0;
  virtual uint32_t size(const char* path) = 0;
  virtual bool createEmpty(const char* path) = 0;
  virtual bool append(const char* path, const uint8_t* bytes, uint32_t length) = 0;
  virtual bool flush(const char* path) = 0;
  virtual uint32_t read(const char* path, uint32_t offset, uint8_t* destination, uint32_t length) = 0;
  virtual bool remove(const char* path) = 0;
  virtual bool move(const char* fromPath, const char* toPath) = 0;
};

// Transactional staged-write + promotion manager for the active route. Implements
// RouteSink so the RouteTransfer receiver can drive it directly.
class RouteStore final : public RouteSink {
 public:
  explicit RouteStore(RouteFileSystem& fs) : fs_(fs), tempSource_(fs) {}

  RouteStore(const RouteStore&) = delete;
  RouteStore& operator=(const RouteStore&) = delete;

  bool begin(uint32_t declaredLength) override;
  bool append(const uint8_t* payload, uint32_t length) override;
  VerifyResult matchesStaged(uint32_t offset, const uint8_t* payload, uint32_t length) override;
  bool flush() override;
  RouteByteSource& stagedSource() override;
  void abandon() override;
  bool publish() override;

 private:
  // Random-access RouteByteSource over the staged temp file. It forwards the
  // decoder's bounded reads straight to the file system; the whole file never
  // enters RAM.
  class TempByteSource final : public RouteByteSource {
   public:
    explicit TempByteSource(RouteFileSystem& fs) : fs_(fs) {}

    uint32_t size() const override { return fs_.size(kRouteActiveTempPath); }

    uint32_t read(uint32_t offset, uint8_t* destination, uint32_t length) override {
      return fs_.read(kRouteActiveTempPath, offset, destination, length);
    }

   private:
    RouteFileSystem& fs_;
  };

  RouteFileSystem& fs_;
  TempByteSource tempSource_;
};

}  // namespace navigator
