#include "RouteStore.h"

#include <cstring>

namespace navigator {
namespace {

inline uint32_t min32(uint32_t a, uint32_t b) { return a < b ? a : b; }

}  // namespace

bool RouteStore::begin(uint32_t declaredLength) {
  // The transport ceiling is the Route Package v1 ceiling (65,535); an empty
  // package cannot exist. RouteTransfer enforces this before calling begin, but
  // the store keeps the invariant itself.
  if (declaredLength == 0 || declaredLength > kRoutePackageV1MaxBytes) {
    return false;
  }

  // Enforce the active-route directory quota before touching any file, so a
  // rejection leaves the previous active route and any recovery backup exactly
  // as they were. The prospective directory state once this transfer is fully
  // staged holds:
  //   * route.tmp at `declaredLength` bytes (a stale temp is removed below);
  //   * route.bin when an active route exists; and
  //   * route.bak only when route.bin is absent - next to a route.bin it is a
  //     leftover of a completed promotion and is dropped below.
  // The sum is computed in uint64_t because file sizes are arbitrary u32
  // values on a real file system.
  uint64_t prospective = declaredLength;
  if (fs_.exists(kRouteActiveBinPath)) {
    prospective += fs_.size(kRouteActiveBinPath);
  } else if (fs_.exists(kRouteActiveBackupPath)) {
    prospective += fs_.size(kRouteActiveBackupPath);
  }
  if (prospective > kRouteStoreQuotaBytes) {
    return false;
  }

  // A stale route.tmp from an interrupted transfer must not survive a new one.
  if (fs_.exists(kRouteActiveTempPath) && !fs_.remove(kRouteActiveTempPath)) {
    return false;
  }
  // A leftover route.bak is only safe to drop while route.bin still holds the
  // active route. When route.bin is missing, route.bak may be the only copy of
  // the previous active route (interrupted promotion) and must be kept.
  if (fs_.exists(kRouteActiveBackupPath) && fs_.exists(kRouteActiveBinPath)) {
    if (!fs_.remove(kRouteActiveBackupPath)) {
      return false;
    }
  }
  // Opening (creating/truncating) route.tmp now surfaces write-path failures at
  // START instead of on the first chunk. A failed create may still have left a
  // partial route.tmp behind, so remove it best-effort; if that remove also
  // fails the leftover is stale state only and the previous active route is
  // untouched either way.
  if (!fs_.createEmpty(kRouteActiveTempPath)) {
    if (fs_.exists(kRouteActiveTempPath)) {
      fs_.remove(kRouteActiveTempPath);
    }
    return false;
  }
  return true;
}

bool RouteStore::append(const uint8_t* payload, uint32_t length) {
  if (length == 0) {
    return true;
  }
  // A route.bin is at most the Route Package v1 ceiling; enforce it on the
  // staged file independently of the receiver so the store is safe on its own.
  const uint32_t current = fs_.size(kRouteActiveTempPath);
  if (static_cast<uint64_t>(current) + length > kRoutePackageV1MaxBytes) {
    return false;
  }
  return fs_.append(kRouteActiveTempPath, payload, length);
}

RouteSink::VerifyResult RouteStore::matchesStaged(uint32_t offset, const uint8_t* payload, uint32_t length) {
  // Read the staged range back in <= 1,024-byte requests and compare it with
  // the delivered payload; only an exact byte-for-byte match is idempotent.
  if (length == 0) {
    return VerifyResult::Match;
  }
  if (static_cast<uint64_t>(offset) + length > fs_.size(kRouteActiveTempPath)) {
    return VerifyResult::ReadFailed;  // outside staged bytes: cannot verify
  }
  uint8_t buffer[kRoutePackageV1WorkBufferBytes];
  uint32_t done = 0;
  while (done < length) {
    const uint32_t want = min32(length - done, kRoutePackageV1WorkBufferBytes);
    const uint32_t got = fs_.read(kRouteActiveTempPath, offset + done, buffer, want);
    if (got != want) {
      return VerifyResult::ReadFailed;
    }
    if (std::memcmp(buffer, payload + done, want) != 0) {
      return VerifyResult::Differ;
    }
    done += want;
  }
  return VerifyResult::Match;
}

bool RouteStore::flush() { return fs_.flush(kRouteActiveTempPath); }

RouteByteSource& RouteStore::stagedSource() { return tempSource_; }

void RouteStore::abandon() {
  if (fs_.exists(kRouteActiveTempPath)) {
    fs_.remove(kRouteActiveTempPath);
  }
}

bool RouteStore::publish() {
  // (1) Move the current active route to route.bak so a failure below can
  // restore it. A route.bak that exists next to route.bin is a leftover from an
  // earlier completed promotion and can be dropped; a route.bak without
  // route.bin is the recovery copy of an interrupted promotion and is kept.
  if (fs_.exists(kRouteActiveBackupPath) && fs_.exists(kRouteActiveBinPath)) {
    if (!fs_.remove(kRouteActiveBackupPath)) {
      fs_.remove(kRouteActiveTempPath);
      return false;
    }
  }
  if (fs_.exists(kRouteActiveBinPath)) {
    if (!fs_.move(kRouteActiveBinPath, kRouteActiveBackupPath)) {
      // The old active route is still at route.bin; drop the unvalidated staged
      // file and report the storage failure.
      fs_.remove(kRouteActiveTempPath);
      return false;
    }
  }
  // (2) Promote the validated staged file. On failure restore the previous
  // active route from route.bak (when one exists); if even the restore rename
  // fails, route.bak keeps the old active route recoverable.
  if (!fs_.move(kRouteActiveTempPath, kRouteActiveBinPath)) {
    if (fs_.exists(kRouteActiveBackupPath) && !fs_.exists(kRouteActiveBinPath)) {
      fs_.move(kRouteActiveBackupPath, kRouteActiveBinPath);
    }
    fs_.remove(kRouteActiveTempPath);
    return false;
  }
  // (3) Success: drop the backup copy of the superseded active route. A
  // leftover route.bak is cleaned by the next begin()/publish(); it is never a
  // reason to roll back a completed promotion.
  if (fs_.exists(kRouteActiveBackupPath)) {
    fs_.remove(kRouteActiveBackupPath);
  }
  return true;
}

}  // namespace navigator
