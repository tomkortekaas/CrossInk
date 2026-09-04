#include "RouteSdFileSystem.h"

#ifdef ARDUINO

#include <Logging.h>
#include <SDCardManager.h>

#include <cstdint>
#include <cstring>

namespace navigator {
namespace {

// I/O bound shared with the Route Package v1 transfer/load cores: staged
// writes and reads never exceed one 1,024-byte work chunk, so no adapter
// buffer is ever larger than that and the whole package never enters RAM.
constexpr uint32_t kRouteFileOpMaxBytes = 1024;

// The single parent directory of every canonical route file. Only write
// operations that open route.tmp may create it (via the SDK's recursive
// ensureDirectoryExists()); read-only operations never touch directories.
constexpr char kRouteActiveDirectory[] = "/Navigation/Routes/active";

inline bool isCanonicalRoutePath(const char* path) {
  if (path == nullptr) {
    return false;
  }
  return std::strcmp(path, kRouteActiveTempPath) == 0 || std::strcmp(path, kRouteActiveBinPath) == 0 ||
         std::strcmp(path, kRouteActiveBackupPath) == 0;
}

// Opens `path` for reading and returns true only when it names a regular file.
// Directories are rejected (isFile() false) and the handle is left open on
// success. Every caller closes it explicitly before returning.
bool openReadOnlyFile(const char* path, FsFile& file) {
  file = SdMan.open(path, O_RDONLY);
  if (!file || !file.isFile()) {
    if (file) {
      file.close();
    }
    return false;
  }
  return true;
}

}  // namespace

bool RouteSdFileSystem::begin() {
  if (SdMan.ready()) {
    return true;
  }
  return SdMan.begin();
}

bool RouteSdFileSystem::exists(const char* path) {
  if (!isCanonicalRoutePath(path) || !SdMan.ready()) {
    return false;
  }
  FsFile file;
  if (!openReadOnlyFile(path, file)) {
    return false;  // absent, or a directory squatting on the path
  }
  file.close();
  return true;
}

uint32_t RouteSdFileSystem::size(const char* path) {
  if (!isCanonicalRoutePath(path) || !SdMan.ready()) {
    return 0;
  }
  FsFile file;
  if (!openReadOnlyFile(path, file)) {
    return 0;
  }
  const uint64_t fileSize = file.fileSize();
  file.close();

  // RouteFileSystem::size is uint32_t, but a foreign/junk file on a FAT32 or
  // exFAT card can exceed 4 GiB. Saturate instead of truncating: RouteStore
  // sums sizes in uint64_t against the 256 KiB quota, so a saturated size can
  // only force a conservative rejection (or a conservative load "invalid"),
  // never a silent wrap to a small number.
  if (fileSize > UINT32_MAX) {
    return UINT32_MAX;
  }
  return static_cast<uint32_t>(fileSize);
}

bool RouteSdFileSystem::createEmpty(const char* path) {
  if (!isCanonicalRoutePath(path) || !SdMan.ready()) {
    return false;
  }
  // RouteStore only ever (re)creates the staged temp file; the active route
  // and its backup must never be created or truncated by this operation.
  if (std::strcmp(path, kRouteActiveTempPath) != 0) {
    return false;
  }
  // ensureDirectoryExists() is recursive, so one call safely creates
  // /Navigation, /Navigation/Routes and /Navigation/Routes/active.
  if (!SdMan.ensureDirectoryExists(kRouteActiveDirectory)) {
    LOG_ERR("NAV", "route store: cannot create directory %s", kRouteActiveDirectory);
    return false;
  }
  // Reject a directory squatting on route.tmp explicitly: SdFat would refuse
  // the write-mode open below anyway, but this keeps "no directory I/O"
  // explicit and deterministic.
  {
    FsFile existing = SdMan.open(path, O_RDONLY);
    if (existing) {
      const bool isFile = existing.isFile();
      existing.close();
      if (!isFile) {
        return false;
      }
    }
  }
  FsFile file = SdMan.open(path, O_RDWR | O_CREAT | O_TRUNC);
  if (!file) {
    LOG_ERR("NAV", "route store: cannot open %s for creation", path);
    return false;
  }
  return file.close();
}

bool RouteSdFileSystem::append(const char* path, const uint8_t* bytes, uint32_t length) {
  if (bytes == nullptr && length != 0) {
    return false;
  }
  if (!isCanonicalRoutePath(path) || !SdMan.ready()) {
    return false;
  }
  // Append is the staged-transfer write path: only route.tmp, bounded to one
  // 1,024-byte chunk per call.
  if (std::strcmp(path, kRouteActiveTempPath) != 0 || length > kRouteFileOpMaxBytes) {
    return false;
  }
  if (length == 0) {
    return true;
  }
  if (!SdMan.ensureDirectoryExists(kRouteActiveDirectory)) {
    LOG_ERR("NAV", "route store: cannot create directory %s", kRouteActiveDirectory);
    return false;
  }
  FsFile file = SdMan.open(path, O_RDWR);
  if (!file || !file.isFile()) {
    if (file) {
      file.close();
    }
    LOG_ERR("NAV", "route store: append open failed for %s", path);
    return false;
  }
  if (!file.seekEnd(0)) {
    file.close();
    LOG_ERR("NAV", "route store: append seek-end failed for %s", path);
    return false;
  }
  // FAT buffers writes in a sector cache; only a full write plus sync() plus a
  // successful close() prove the chunk is durable. A partial failure can leave
  // a partial route.tmp behind - RouteTransfer's abandon()/publish() handles
  // that leftover and the previous active route is untouched either way. No
  // atomic rename or write is promised on FAT.
  const size_t written = file.write(bytes, length);
  const bool synced = file.sync();
  const bool closed = file.close();
  if (written != length || !synced || !closed) {
    LOG_ERR("NAV", "route store: append to %s failed (wrote %u of %u sync=%d close=%d)", path,
            static_cast<unsigned>(written), static_cast<unsigned>(length), synced ? 1 : 0, closed ? 1 : 0);
    return false;
  }
  return true;
}

bool RouteSdFileSystem::flush(const char* path) {
  if (!isCanonicalRoutePath(path) || !SdMan.ready()) {
    return false;
  }
  // RouteStore only ever flushes the staged route.tmp, the only canonical file
  // this adapter writes incrementally. The open is O_RDWR so sync() can flush
  // any pending directory/FAT state for the file.
  FsFile file = SdMan.open(path, O_RDWR);
  if (!file || !file.isFile()) {
    if (file) {
      file.close();
    }
    LOG_ERR("NAV", "route store: flush open failed for %s", path);
    return false;
  }
  const bool synced = file.sync();
  const bool closed = file.close();
  if (!synced || !closed) {
    LOG_ERR("NAV", "route store: flush of %s failed (sync=%d close=%d)", path, synced ? 1 : 0, closed ? 1 : 0);
    return false;
  }
  return true;
}

uint32_t RouteSdFileSystem::read(const char* path, uint32_t offset, uint8_t* destination, uint32_t length) {
  if (destination == nullptr || !isCanonicalRoutePath(path) || !SdMan.ready()) {
    return 0;
  }
  if (length == 0) {
    return 0;
  }
  // Bounded reads: never hand the SDK more than one 1,024-byte work chunk.
  const uint32_t bounded = length > kRouteFileOpMaxBytes ? kRouteFileOpMaxBytes : length;
  FsFile file;
  if (!openReadOnlyFile(path, file)) {
    return 0;
  }
  const uint64_t fileSize = file.fileSize();
  if (fileSize > UINT32_MAX) {
    // Not addressable through the uint32_t RouteFileSystem API and far beyond
    // any route package; refuse rather than truncate the range.
    file.close();
    return 0;
  }
  if (static_cast<uint64_t>(offset) >= fileSize) {
    file.close();
    return 0;  // offset at/past EOF: nothing to deliver
  }
  if (!file.seekSet(offset)) {
    file.close();
    LOG_ERR("NAV", "route store: seek failed for %s at offset %u", path, offset);
    return 0;
  }
  // FsFile::read returns the bytes copied, and -1 on an I/O error. Fewer than
  // requested near EOF is normal; fewer mid-file models a stalled read, which
  // the RouteByteSource contract tolerates (callers re-request the remainder).
  const int got = file.read(destination, bounded);
  file.close();
  if (got <= 0) {
    return 0;
  }
  return static_cast<uint32_t>(got);
}

bool RouteSdFileSystem::remove(const char* path) {
  if (!isCanonicalRoutePath(path) || !SdMan.ready()) {
    return false;
  }
  FsFile file = SdMan.open(path, O_RDONLY);
  if (!file) {
    return true;  // removing an absent file is success (RouteFileSystem rule)
  }
  if (!file.isFile()) {
    // A directory at a canonical path is not a route file; never delete
    // directories (no recursive deletion anywhere in the adapter).
    file.close();
    return false;
  }
  file.close();  // close before remove: SdFat forbids removing an open file
  return SdMan.remove(path);
}

bool RouteSdFileSystem::move(const char* fromPath, const char* toPath) {
  if (!isCanonicalRoutePath(fromPath) || !isCanonicalRoutePath(toPath)) {
    return false;
  }
  if (std::strcmp(fromPath, toPath) == 0) {
    return false;
  }
  if (!SdMan.ready()) {
    return false;
  }
  FsFile from = SdMan.open(fromPath, O_RDONLY);
  if (!from || !from.isFile()) {
    if (from) {
      from.close();
    }
    return false;
  }
  from.close();
  // Plain-rename semantics (RouteFileSystem rule): `to` must not exist. Check
  // it explicitly rather than relying on FAT rename behavior with an existing
  // target, so a directory or file at `to` is never replaced.
  FsFile to = SdMan.open(toPath, O_RDONLY);
  if (to) {
    to.close();
    return false;
  }
  return SdMan.rename(fromPath, toPath);
}

}  // namespace navigator

#endif  // ARDUINO
