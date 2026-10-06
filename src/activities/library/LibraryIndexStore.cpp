#include "LibraryIndexStore.h"

#include <Arduino.h>
#include <Epub.h>
#include <FsHelpers.h>
#include <HalStorage.h>
#include <Logging.h>
#include <Xtc.h>

#include "util/SdMetadataEntries.h"

namespace LibraryIndexStore {
namespace {
constexpr int kMaxScanDepth = 8;

class FileWriter final : public LibraryByteWriter {
 public:
  explicit FileWriter(FsFile& f) : file(f) {}
  bool write(const void* data, size_t len) override { return file.write(data, len) == len; }

 private:
  FsFile& file;
};

class FileReader final : public LibraryByteReader {
 public:
  explicit FileReader(FsFile& f) : file(f) {}
  bool read(void* data, size_t len) override { return file.read(data, len) == static_cast<int>(len); }

 private:
  FsFile& file;
};

void scanDirectory(const std::string& dirPath, const int depth, std::vector<std::string>& out) {
  if (depth > kMaxScanDepth) {
    LOG_ERR("LIB", "Scan depth limit reached at %s", dirPath.c_str());
    return;
  }
  auto dir = Storage.open(dirPath.c_str());
  if (!dir || !dir.isDirectory()) {
    LOG_ERR("LIB", "Cannot open directory %s", dirPath.c_str());
    return;
  }
  // Static to keep 256 bytes off the stack per recursion level; the name is copied into
  // childPath before recursing, so sharing the buffer between levels is safe.
  static char name[256];
  for (auto file = dir.openNextFile(); file; file = dir.openNextFile()) {
    file.getName(name, sizeof(name));
    const bool isDir = file.isDirectory();
    file.close();
    if (name[0] == '.' || SdMetadataEntries::isMacOS(name) || SdMetadataEntries::isWindows(name)) continue;
    std::string childPath = dirPath == "/" ? "/" + std::string(name) : dirPath + "/" + name;
    if (isDir) {
      scanDirectory(childPath, depth + 1, out);
      continue;
    }
    if (!isLibraryBook(name)) continue;
    if (childPath.size() > LibraryIndexCodec::kMaxPathLength) {
      LOG_ERR("LIB", "Path too long, skipped: %s", childPath.c_str());
      continue;
    }
    if (out.size() >= LibraryIndexCodec::kMaxBooks) {
      LOG_ERR("LIB", "Library cap of %u books reached", static_cast<unsigned>(LibraryIndexCodec::kMaxBooks));
      break;
    }
    out.push_back(std::move(childPath));
  }
  dir.close();
}
}  // namespace

bool isLibraryBook(const std::string_view filename) {
  return FsHelpers::hasEpubExtension(filename) || FsHelpers::hasXtcExtension(filename) ||
         FsHelpers::hasTxtExtension(filename) || FsHelpers::hasMarkdownExtension(filename);
}

bool load(LibraryIndexData& out) {
  out = LibraryIndexData{};
  if (!Storage.exists(kIndexPath)) return false;
  FsFile file;
  if (!Storage.openFileForRead("LIB", kIndexPath, file)) {
    LOG_ERR("LIB", "Cannot open %s", kIndexPath);
    return false;
  }
  FileReader reader(file);
  const bool ok = LibraryIndexCodec::decode(reader, out);
  file.close();
  if (!ok) {
    LOG_ERR("LIB", "Library index unreadable, rebuilding");
    out = LibraryIndexData{};
  }
  return ok;
}

bool save(const LibraryIndexData& data) {
  FsFile file;
  if (!Storage.openFileForWrite("LIB", kIndexPath, file)) {
    LOG_ERR("LIB", "Cannot write %s", kIndexPath);
    return false;
  }
  FileWriter writer(file);
  const bool ok = LibraryIndexCodec::encode(data, writer);
  file.close();
  if (!ok) LOG_ERR("LIB", "Library index write failed");
  return ok;
}

void scanBookPaths(std::vector<std::string>& outPaths) { scanDirectory("/", 0, outPaths); }

void loadMetadata(LibraryEntry& entry) {
  entry.metadataLoaded = true;
  if (FsHelpers::hasEpubExtension(entry.path)) {
    Epub epub(entry.path, "/.crosspoint");
    if (!epub.load(true, true, Epub::XLocationLoadMode::Skip)) {
      if (epub.getLastLoadFailure() == Epub::OpenFailure::OutOfMemory) {
        entry.metadataLoaded = false;  // retry the metadata on the next open
        LOG_ERR("LIB", "EPUB metadata out of memory: %s free=%u maxAlloc=%u", entry.path.c_str(),
                static_cast<unsigned>(ESP.getFreeHeap()), static_cast<unsigned>(ESP.getMaxAllocHeap()));
      } else {
        LOG_ERR("LIB", "EPUB metadata unavailable: %s", entry.path.c_str());
      }
      return;
    }
    if (!epub.getTitle().empty()) entry.title = epub.getTitle();
    entry.author = epub.getAuthor();
    return;
  }
  if (FsHelpers::hasXtcExtension(entry.path)) {
    Xtc xtc(entry.path, "/.crosspoint");
    if (!xtc.load()) {
      LOG_ERR("LIB", "XTC metadata unavailable: %s", entry.path.c_str());
      return;
    }
    const std::string title = xtc.getTitle();
    if (!title.empty()) entry.title = title;
    entry.author = xtc.getAuthor();
  }
}
}  // namespace LibraryIndexStore
