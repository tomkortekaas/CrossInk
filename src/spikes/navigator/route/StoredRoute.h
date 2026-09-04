#pragma once

#include <cstdint>

#include "RoutePackageV1.h"
#include "RouteStore.h"

// Read-only loader for the single stored active walking route (Task 7,
// firmware core, next bounded part). Hardware-independent: it talks only to
// the RouteFileSystem abstraction, so host tests drive it over an in-memory
// fake and the hardware build feeds it RouteSdFileSystem (SD card adapter).
//
// Load policy (strictly read-only, no startup "transaction recovery"):
//   * load() validates route.bin first; if it is absent, fails validation, or
//     cannot be read, load() validates route.bak. It NEVER reads route.tmp and
//     never promotes a temp or backup file, and it never writes, deletes, or
//     renames ANY file. A valid route.bin is always preferred, even when a
//     backup also exists.
//   * The caller owns the ~9.8 KiB RouteIndex validation workspace
//     (static/global storage on the C3 - never a task-stack local) and passes
//     it by reference; it is filled only on a successful load (the validator
//     leaves it untouched on every rejection).
//   * After a successful load the loader keeps reading from the validated
//     canonical file (bin or bak) as a RouteByteSource, so rendering can read
//     the route straight off the card in <= 1,024-byte requests without ever
//     holding the package in RAM.
//   * A failed load leaves the source unreadable (selection None), never a
//     stale previous selection.
//
// Before a new transfer, prepareForTransfer() explicitly restores a validated
// backup when needed. This is separate from read-only startup load(); failure
// must prevent RouteStore::begin() from removing a needed backup.

namespace navigator {

// Which canonical route file a StoredRoute currently reads from. None means
// the source is unreadable (nothing loaded yet or the last load failed).
enum class StoredRouteFile : uint8_t {
  None = 0,
  Bin,
  Backup,
};

// Typed outcome of a read-only load. The active/backup distinction is the
// storage distinction: a route served from route.bak is a degraded but valid
// fallback of the last known-good active route.
enum class StoredRouteLoadResult : uint8_t {
  LoadedActive = 0,  // route.bin validated and selected
  LoadedBackup,      // route.bin absent/invalid; route.bak validated/selected
  NotFound,          // neither canonical route file exists
  Invalid,           // at least one exists, but none validates
};

class StoredRoute final : public RouteByteSource {
 public:
  explicit StoredRoute(RouteFileSystem& fs) : fs_(fs) {}

  StoredRoute(const StoredRoute&) = delete;
  StoredRoute& operator=(const StoredRoute&) = delete;

  // Read-only validation + selection. Reads route.bin then route.bak through
  // `candidate`; never allocates, never mutates the file system.
  StoredRouteLoadResult load(RouteIndex& candidate);

  // Explicit write preparation, unlike read-only load(). Validates before
  // restoring backup to bin; on failure the caller MUST NOT start RouteStore.
  // An unreadable/corrupt store without a validated copy is left untouched.
  bool prepareForTransfer(RouteIndex& candidate);

  StoredRouteFile selectedFile() const { return selected_; }
  bool isLoaded() const { return selected_ != StoredRouteFile::None; }

  // RouteByteSource over the selected canonical file. Only meaningful after a
  // successful load; with no selection both return 0.
  uint32_t size() const override;
  uint32_t read(uint32_t offset, uint8_t* destination, uint32_t length) override;

 private:
  const char* selectedPath() const;

  RouteFileSystem& fs_;
  StoredRouteFile selected_ = StoredRouteFile::None;
};

}  // namespace navigator
