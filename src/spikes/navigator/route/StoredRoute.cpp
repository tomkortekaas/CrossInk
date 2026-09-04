#include "StoredRoute.h"

namespace navigator {

const char* StoredRoute::selectedPath() const {
  switch (selected_) {
    case StoredRouteFile::Bin:
      return kRouteActiveBinPath;
    case StoredRouteFile::Backup:
      return kRouteActiveBackupPath;
    case StoredRouteFile::None:
      return nullptr;
  }
  return nullptr;
}

StoredRouteLoadResult StoredRoute::load(RouteIndex& candidate) {
  // A previous load must never leak into this one: a failed reload leaves the
  // source unreadable (selection None), so stale bytes can never be served
  // after route.bin/route.bak changed underneath us.
  selected_ = StoredRouteFile::None;

  const bool binPresent = fs_.exists(kRouteActiveBinPath);
  const bool backupPresent = fs_.exists(kRouteActiveBackupPath);

  // route.bin is the primary copy. While validating, this source reads from
  // it; validateRoutePackageV1() leaves `candidate` untouched on rejection,
  // so a failed bin only costs the validation reads before route.bak is tried.
  if (binPresent) {
    selected_ = StoredRouteFile::Bin;
    if (validateRoutePackageV1(*this, candidate) == DecodeStatus::Ok) {
      return StoredRouteLoadResult::LoadedActive;
    }
    selected_ = StoredRouteFile::None;
  }

  if (backupPresent) {
    selected_ = StoredRouteFile::Backup;
    if (validateRoutePackageV1(*this, candidate) == DecodeStatus::Ok) {
      return StoredRouteLoadResult::LoadedBackup;
    }
    selected_ = StoredRouteFile::None;
  }

  // Nothing validated. Distinguish "nothing exists at all" (NotFound) from
  // "something exists but is unusable" (Invalid) so a caller can tell a fresh
  // device from a corrupted store without probing the file system itself.
  return (binPresent || backupPresent) ? StoredRouteLoadResult::Invalid
                                       : StoredRouteLoadResult::NotFound;
}

uint32_t StoredRoute::size() const {
  const char* path = selectedPath();
  if (path == nullptr) {
    return 0;
  }
  // A selected file was validated as a Route Package v1, so its size equals
  // the package's declared length (<= 65,535); no extra clamp is needed here.
  return fs_.size(path);
}

bool StoredRoute::prepareForTransfer(RouteIndex& candidate) {
  const auto result = load(candidate);
  if (result == StoredRouteLoadResult::Invalid) return false;
  if (result != StoredRouteLoadResult::LoadedBackup) return true;
  // Only remove an invalid bin AFTER a backup was successfully validated.
  // If rename fails, the known-good bytes remain at backup and writes stop.
  if (fs_.exists(kRouteActiveBinPath) && !fs_.remove(kRouteActiveBinPath)) return false;
  if (!fs_.move(kRouteActiveBackupPath, kRouteActiveBinPath)) return false;
  selected_ = StoredRouteFile::Bin;
  return true;
}

uint32_t StoredRoute::read(uint32_t offset, uint8_t* destination, uint32_t length) {
  const char* path = selectedPath();
  if (path == nullptr || destination == nullptr || length == 0) {
    return 0;
  }
  // Bound every production read to the shared 1,024-byte work buffer, whatever
  // the caller asks for: RouteFileSystem reads never exceed that, the validator
  // already requests <= 1,024 bytes, and the RouteByteSource contract tolerates
  // a source that answers a request in <= 1,024-byte pieces (the decoder
  // re-requests the remainder). Offsets are guarded by the underlying
  // RouteFileSystem read contract (0 when offset >= size, never past the end).
  const uint32_t bounded = length > kRoutePackageV1WorkBufferBytes ? kRoutePackageV1WorkBufferBytes : length;
  return fs_.read(path, offset, destination, bounded);
}

}  // namespace navigator
