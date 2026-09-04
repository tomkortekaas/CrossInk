#pragma once

#include "RouteStore.h"

// SD-card adapter for the transactional active-route store (Task 7, firmware
// core, production adapter). Implements the RouteFileSystem abstraction from
// RouteStore.h over the FreeInk SDK's existing SDCardManager singleton (SdMan)
// and SdFat's FsFile. Those SDK types only exist under ARDUINO, so the whole
// class is compiled out elsewhere; host tests drive RouteStore/StoredRoute
// over in-memory fakes because the real card behavior cannot be proven on the
// host. The exact FsFile method signatures used here were inspected in the
// pinned SdFat (lib_deps greiman/SdFat, src/FsLib/FsFile.h).
//
// Contract notes (layered on the exact RouteFileSystem semantics in
// RouteStore.h):
//   * The constructor does NOT mount anything. Call begin() once AFTER board
//     detection (SdMan.begin() reads BoardConfig::ACTIVE); begin() never
//     powers the SD rail down, only up when the manager is not ready yet.
//   * Every operation accepts exactly the three canonical route paths
//     (kRouteActiveTempPath / kRouteActiveBinPath / kRouteActiveBackupPath).
//     Null or any other path is rejected. A directory squatting on a canonical
//     path is rejected as not-a-file (no recursive deletion, ever).
//   * Every operation opens and closes its own handle, so at most one file is
//     open at a time, and reads/writes are bounded to <= 1,024 bytes per call
//     (no full-file buffers anywhere).
//   * size() reads the uint64 FsFile::fileSize() and saturates at UINT32_MAX
//     (see the .cpp): RouteStore sums sizes in uint64_t against the 256 KiB
//     quota, so a saturated size can only force a conservative rejection,
//     never a wrap.
//   * append() and createEmpty() are valid only for route.tmp and create the
//     /Navigation, /Navigation/Routes and /Navigation/Routes/active
//     directories through the SDK's ensureDirectoryExists() when opening it.
//     Read-only operations (exists/size/read) never create directories.
//   * move() is a plain rename that never replaces an existing destination.
//   * No atomicity is promised on FAT: a failed append/sync/close can leave a
//     partial route.tmp behind, and the SDK's ready() does not reliably detect
//     physical card removal - every operation reports its own failure instead
//     of assuming the card is still there.

#ifdef ARDUINO

namespace navigator {

class RouteSdFileSystem final : public RouteFileSystem {
 public:
  // Brings the SD card manager up if it is not ready yet. Must be called by
  // the caller after board detection; never powers the rail down.
  bool begin();

  bool exists(const char* path) override;
  uint32_t size(const char* path) override;
  bool createEmpty(const char* path) override;
  bool append(const char* path, const uint8_t* bytes, uint32_t length) override;
  bool flush(const char* path) override;
  uint32_t read(const char* path, uint32_t offset, uint8_t* destination, uint32_t length) override;
  bool remove(const char* path) override;
  bool move(const char* fromPath, const char* toPath) override;
};

}  // namespace navigator

#endif  // ARDUINO
