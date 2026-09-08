// Host tests for the read-only stored-route loader
// (src/spikes/navigator/route/StoredRoute.h/.cpp) behind the RouteFileSystem
// abstraction. The hardware SD adapter (RouteSdFileSystem) is ARDUINO-only and
// cannot be host-proven; these tests pin the loader contract that both the
// host fake and the real adapter must honor.
//
// Task 7 (firmware core) - strict TDD: the loader under test must:
//   * validate route.bin first and prefer it whenever it validates; when it is
//     absent, corrupt, or unreadable, validate route.bak and load from it;
//   * report LoadedActive / LoadedBackup / NotFound / Invalid - never "loaded"
//     when no canonical file validates, and never retain a stale previous
//     selection after a failed reload;
//   * NEVER read route.tmp, NEVER write/delete/rename any file during load,
//     and never allocate (the candidate RouteIndex is caller-owned, ~9.8 KiB,
//     static/global on the C3 - never a local inside the loader);
//   * clamp every production source read to <= 1,024 bytes and guard offsets;
//   * after a successful load serve the validated package straight from the
//     selected canonical file as a RouteByteSource (no full route in RAM).
//
// Fixture: the checked-in golden package test/fixtures/route_package_v1.bin
// (90 bytes, route id 66051 = 0x00010203), loaded from disk exactly like
// RoutePackageV1Test.cpp does.

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "route/RoutePackageV1.h"
#include "route/RouteStore.h"
#include "route/StoredRoute.h"

namespace {

using Bytes = std::vector<uint8_t>;

// ---------------------------------------------------------------------------
// Golden fixture loading (same hermetic __FILE__ strategy as
// RoutePackageV1Test.cpp; no shared test header may be added).
// ---------------------------------------------------------------------------

bool readWholeFile(const std::string& path, Bytes& out) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    return false;
  }
  out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
  return !in.bad();
}

Bytes loadFixtureBytes(const char* name) {
  std::vector<std::string> candidates;
  const std::string self = __FILE__;
  const size_t marker = self.find("/test/navigator/");
  if (marker != std::string::npos) {
    candidates.push_back(self.substr(0, marker) + "/test/fixtures/" + name);
  }
  candidates.push_back("test/fixtures/" + std::string(name));
  candidates.push_back("../../../test/fixtures/" + std::string(name));
  candidates.push_back("../../test/fixtures/" + std::string(name));
  for (const std::string& candidate : candidates) {
    Bytes bytes;
    if (readWholeFile(candidate, bytes)) {
      return bytes;
    }
  }
  ADD_FAILURE() << "could not open fixture " << name << "; tried:\n";
  for (const std::string& candidate : candidates) {
    std::cerr << "  " << candidate << "\n";
  }
  return Bytes();
}

const Bytes& goldenPackage() {
  static const Bytes kBytes = loadFixtureBytes("route_package_v1.bin");
  return kBytes;
}

// ---------------------------------------------------------------------------
// A second, byte-valid package builder (distinct route id) so tests can prove
// route.bin wins when both files are valid.
// ---------------------------------------------------------------------------

uint32_t crc32Of(const uint8_t* data, size_t length) {
  uint32_t crc = 0xFFFFFFFFu;
  for (size_t i = 0; i < length; ++i) {
    crc ^= data[i];
    for (unsigned bit = 0; bit < 8; ++bit) {
      crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
  }
  return crc ^ 0xFFFFFFFFu;
}

void putU16(Bytes& b, size_t at, uint16_t value) {
  b[at] = static_cast<uint8_t>(value & 0xFFu);
  b[at + 1] = static_cast<uint8_t>((value >> 8) & 0xFFu);
}

void putU32(Bytes& b, size_t at, uint32_t value) {
  for (unsigned shift = 0; shift < 32; shift += 8) {
    b[at + shift / 8] = static_cast<uint8_t>((value >> shift) & 0xFFu);
  }
}

void appendU16(Bytes& b, uint16_t value) {
  b.push_back(static_cast<uint8_t>(value & 0xFFu));
  b.push_back(static_cast<uint8_t>((value >> 8) & 0xFFu));
}

// A deterministic, valid single-segment route that never moves (deltas are all
// zero at the origin). 48 bytes for two points: 38-byte header + 2-byte
// segment-start list + one 4-byte delta + 4-byte CRC trailer.
Bytes buildFlatRoute(uint32_t routeId, uint16_t pointCount) {
  Bytes payload;
  appendU16(payload, 0);  // segment starts
  for (uint16_t i = 1; i < pointCount; ++i) {
    appendU16(payload, 0);
    appendU16(payload, 0);
  }
  const uint32_t total = 38u + static_cast<uint32_t>(payload.size()) + 4u;
  Bytes bytes(total, 0);
  bytes[0] = 'X';
  bytes[1] = '3';
  bytes[2] = 'R';
  bytes[3] = 'T';
  bytes[4] = 1;
  bytes[5] = 0;
  putU16(bytes, 6, 38);
  putU32(bytes, 8, total);
  putU32(bytes, 12, routeId);
  putU16(bytes, 16, pointCount);
  putU16(bytes, 18, 0);
  putU32(bytes, 20, 0);
  putU32(bytes, 24, 0);
  putU32(bytes, 28, 0);
  putU16(bytes, 32, 0);
  putU16(bytes, 34, 1);
  bytes[36] = 0;
  bytes[37] = 0;
  std::copy(payload.begin(), payload.end(), bytes.begin() + 38);
  putU32(bytes, total - 4, crc32Of(bytes.data(), total - 4));
  return bytes;
}

Bytes corruptByte(const Bytes& in, size_t at) {
  Bytes out = in;
  out[at] = static_cast<uint8_t>(out[at] ^ 0x5A);
  return out;
}

// ---------------------------------------------------------------------------
// Small bounded in-memory RouteFileSystem fake. Unlike RouteStoreTest's large
// fake this one only needs what the read-only loader uses, but it also counts
// every mutating call so tests can prove the loader never mutates files.
// ---------------------------------------------------------------------------

class RecordingRouteFs final : public navigator::RouteFileSystem {
 public:
  RecordingRouteFs() {
    files_[0].path = navigator::kRouteActiveTempPath;
    files_[1].path = navigator::kRouteActiveBinPath;
    files_[2].path = navigator::kRouteActiveBackupPath;
  }

  void seed(const char* path, const Bytes& bytes) {
    File& file = *find(path);
    file.bytes = bytes;
    file.exists = true;
  }

  void clear(const char* path) {
    File& file = *find(path);
    file.bytes.clear();
    file.exists = false;
  }

  bool fileExists(const char* path) const { return find(path)->exists; }
  const Bytes& fileBytes(const char* path) const { return find(path)->bytes; }

  // Set to a canonical path to model a stalled/removed card: reads for that
  // file then return 0 even though exists()/size() still report it.
  const char* stallPath = nullptr;

  uint32_t createEmptyCalls = 0;
  uint32_t appendCalls = 0;
  uint32_t flushCalls = 0;
  uint32_t removeCalls = 0;
  uint32_t moveCalls = 0;
  uint32_t readCalls = 0;
  uint32_t maxReadRequested = 0;
  bool allowRecovery = false;
  bool failMove = false;

  bool exists(const char* path) override { return find(path)->exists; }

  uint32_t size(const char* path) override {
    const File& file = *find(path);
    return file.exists ? static_cast<uint32_t>(file.bytes.size()) : 0;
  }

  bool createEmpty(const char* path) override {
    (void)path;
    ++createEmptyCalls;
    return false;
  }

  bool append(const char* path, const uint8_t* bytes, uint32_t length) override {
    (void)path;
    (void)bytes;
    (void)length;
    ++appendCalls;
    return false;
  }

  bool flush(const char* path) override {
    (void)path;
    ++flushCalls;
    return false;
  }

  uint32_t read(const char* path, uint32_t offset, uint8_t* destination, uint32_t length) override {
    ++readCalls;
    maxReadRequested = std::max(maxReadRequested, length);
    if (destination == nullptr) {
      return 0;
    }
    if (stallPath != nullptr && std::strcmp(stallPath, path) == 0) {
      return 0;
    }
    const File& file = *find(path);
    if (!file.exists || offset >= file.bytes.size() || length == 0) {
      return 0;
    }
    const uint32_t n = static_cast<uint32_t>(std::min<uint64_t>(length, file.bytes.size() - offset));
    std::memcpy(destination, file.bytes.data() + offset, n);
    return n;
  }

  bool remove(const char* path) override {
    ++removeCalls;
    if (!allowRecovery) return false;
    clear(path);
    return true;
  }

  bool move(const char* fromPath, const char* toPath) override {
    ++moveCalls;
    if (!allowRecovery || failMove || !find(fromPath)->exists || find(toPath)->exists) return false;
    seed(toPath, find(fromPath)->bytes);
    clear(fromPath);
    return true;
  }

 private:
  struct File {
    const char* path = nullptr;
    Bytes bytes;
    bool exists = false;
  };

  File* find(const char* path) {
    for (File& file : files_) {
      if (std::strcmp(file.path, path) == 0) {
        return &file;
      }
    }
    return &files_[0];
  }

  const File* find(const char* path) const {
    for (const File& file : files_) {
      if (std::strcmp(file.path, path) == 0) {
        return &file;
      }
    }
    return &files_[0];
  }

  File files_[3];
};

void expectNeverMutated(const RecordingRouteFs& fs) {
  EXPECT_EQ(fs.createEmptyCalls, 0u);
  EXPECT_EQ(fs.appendCalls, 0u);
  EXPECT_EQ(fs.flushCalls, 0u);
  EXPECT_EQ(fs.removeCalls, 0u);
  EXPECT_EQ(fs.moveCalls, 0u);
}

}  // namespace

// The production allocation counter lives in RouteTransferTest.cpp (single
// global operator new override per test binary); deltas are measured here too.
namespace test_alloc {
extern std::size_t gAllocationCount;
}  // namespace test_alloc

namespace {

using navigator::StoredRouteFile;
using navigator::StoredRouteLoadResult;

// ---------------------------------------------------------------------------
// Golden fixture sanity (decodes; route id 66051).
// ---------------------------------------------------------------------------

TEST(StoredRouteTest, GoldenFixtureDecodes) {
  const Bytes& golden = goldenPackage();
  ASSERT_EQ(golden.size(), 90u);

  class VectorSource : public navigator::RouteByteSource {
   public:
    explicit VectorSource(const Bytes& bytes) : bytes_(bytes) {}
    uint32_t size() const override { return static_cast<uint32_t>(bytes_.size()); }
    uint32_t read(uint32_t offset, uint8_t* destination, uint32_t length) override {
      if (offset >= bytes_.size() || length == 0) {
        return 0;
      }
      const uint32_t n = static_cast<uint32_t>(std::min<uint64_t>(length, bytes_.size() - offset));
      std::memcpy(destination, bytes_.data() + offset, n);
      return n;
    }

   private:
    const Bytes& bytes_;
  };
  VectorSource source(golden);
  navigator::RouteIndex route{};
  EXPECT_EQ(navigator::validateRoutePackageV1(source, route), navigator::DecodeStatus::Ok);
  EXPECT_EQ(route.routeId, 66051u);
}

// ---------------------------------------------------------------------------
// Successful loads.
// ---------------------------------------------------------------------------

TEST(StoredRouteTest, ValidActiveLoadsFromBin) {
  RecordingRouteFs fs;
  navigator::StoredRoute route(fs);
  const Bytes& golden = goldenPackage();
  fs.seed(navigator::kRouteActiveBinPath, golden);

  navigator::RouteIndex candidate{};
  ASSERT_EQ(route.load(candidate), StoredRouteLoadResult::LoadedActive);
  EXPECT_EQ(route.selectedFile(), StoredRouteFile::Bin);
  EXPECT_TRUE(route.isLoaded());
  EXPECT_EQ(route.size(), golden.size());
  EXPECT_EQ(candidate.routeId, 66051u);
  EXPECT_EQ(candidate.declaredLength, golden.size());

  // Rendering reads come back from the selected bin file.
  uint8_t buffer[1024];
  const uint32_t got = route.read(0, buffer, sizeof(buffer));
  ASSERT_EQ(got, golden.size());
  EXPECT_EQ(std::memcmp(buffer, golden.data(), golden.size()), 0);
  expectNeverMutated(fs);
}

TEST(StoredRouteTest, AbsentActiveWithValidBackupLoadsFromBackup) {
  RecordingRouteFs fs;
  navigator::StoredRoute route(fs);
  const Bytes& golden = goldenPackage();
  fs.seed(navigator::kRouteActiveBackupPath, golden);

  navigator::RouteIndex candidate{};
  ASSERT_EQ(route.load(candidate), StoredRouteLoadResult::LoadedBackup);
  EXPECT_EQ(route.selectedFile(), StoredRouteFile::Backup);
  EXPECT_TRUE(route.isLoaded());
  EXPECT_EQ(route.size(), golden.size());
  EXPECT_EQ(candidate.routeId, 66051u);

  uint8_t buffer[1024];
  const uint32_t got = route.read(0, buffer, sizeof(buffer));
  ASSERT_EQ(got, golden.size());
  EXPECT_EQ(std::memcmp(buffer, golden.data(), golden.size()), 0);
  expectNeverMutated(fs);
}

TEST(StoredRouteTest, CorruptActiveWithValidBackupLoadsFromBackup) {
  RecordingRouteFs fs;
  navigator::StoredRoute route(fs);
  const Bytes& golden = goldenPackage();
  fs.seed(navigator::kRouteActiveBinPath, corruptByte(golden, 89));  // bad trailer CRC
  fs.seed(navigator::kRouteActiveBackupPath, golden);

  navigator::RouteIndex candidate{};
  ASSERT_EQ(route.load(candidate), StoredRouteLoadResult::LoadedBackup);
  EXPECT_EQ(route.selectedFile(), StoredRouteFile::Backup);
  EXPECT_EQ(route.size(), golden.size());
  EXPECT_EQ(candidate.routeId, 66051u);

  uint8_t buffer[1024];
  const uint32_t got = route.read(0, buffer, sizeof(buffer));
  ASSERT_EQ(got, golden.size());
  EXPECT_EQ(std::memcmp(buffer, golden.data(), golden.size()), 0);
  expectNeverMutated(fs);
}

TEST(StoredRouteTest, UnreadableActiveWithValidBackupLoadsFromBackup) {
  RecordingRouteFs fs;
  navigator::StoredRoute route(fs);
  const Bytes& golden = goldenPackage();
  fs.seed(navigator::kRouteActiveBinPath, golden);
  fs.seed(navigator::kRouteActiveBackupPath, golden);
  fs.stallPath = navigator::kRouteActiveBinPath;  // card read failure on bin

  navigator::RouteIndex candidate{};
  ASSERT_EQ(route.load(candidate), StoredRouteLoadResult::LoadedBackup);
  EXPECT_EQ(route.selectedFile(), StoredRouteFile::Backup);
  EXPECT_EQ(route.size(), golden.size());
  EXPECT_EQ(candidate.routeId, 66051u);
  expectNeverMutated(fs);
}

TEST(StoredRouteTest, BinPreferredWhenBothFilesValid) {
  RecordingRouteFs fs;
  navigator::StoredRoute route(fs);
  const Bytes& golden = goldenPackage();
  const Bytes flat = buildFlatRoute(777, 2);
  fs.seed(navigator::kRouteActiveBinPath, golden);
  fs.seed(navigator::kRouteActiveBackupPath, flat);

  navigator::RouteIndex candidate{};
  ASSERT_EQ(route.load(candidate), StoredRouteLoadResult::LoadedActive);
  EXPECT_EQ(route.selectedFile(), StoredRouteFile::Bin);
  EXPECT_EQ(route.size(), golden.size());
  EXPECT_EQ(candidate.routeId, 66051u);
  expectNeverMutated(fs);
}

// ---------------------------------------------------------------------------
// Failure outcomes: nothing is loaded, the source becomes unreadable, and no
// stale selection survives.
// ---------------------------------------------------------------------------

TEST(StoredRouteTest, BothFilesInvalidYieldsInvalidAndUnreadable) {
  RecordingRouteFs fs;
  navigator::StoredRoute route(fs);
  const Bytes& golden = goldenPackage();
  fs.seed(navigator::kRouteActiveBinPath, corruptByte(golden, 89));
  fs.seed(navigator::kRouteActiveBackupPath, corruptByte(golden, 88));

  navigator::RouteIndex candidate{};
  ASSERT_EQ(route.load(candidate), StoredRouteLoadResult::Invalid);
  EXPECT_FALSE(route.isLoaded());
  EXPECT_EQ(route.selectedFile(), StoredRouteFile::None);
  EXPECT_EQ(route.size(), 0u);
  uint8_t buffer[16];
  EXPECT_EQ(route.read(0, buffer, sizeof(buffer)), 0u);
  expectNeverMutated(fs);
}

TEST(StoredRouteTest, OnlyBackupPresentAndInvalidYieldsInvalid) {
  RecordingRouteFs fs;
  navigator::StoredRoute route(fs);
  const Bytes& golden = goldenPackage();
  fs.seed(navigator::kRouteActiveBackupPath, corruptByte(golden, 89));

  navigator::RouteIndex candidate{};
  ASSERT_EQ(route.load(candidate), StoredRouteLoadResult::Invalid);
  EXPECT_FALSE(route.isLoaded());
  EXPECT_EQ(route.size(), 0u);
}

TEST(StoredRouteTest, NeitherFileExistsYieldsNotFound) {
  RecordingRouteFs fs;
  navigator::StoredRoute route(fs);

  navigator::RouteIndex candidate{};
  ASSERT_EQ(route.load(candidate), StoredRouteLoadResult::NotFound);
  EXPECT_FALSE(route.isLoaded());
  EXPECT_EQ(route.selectedFile(), StoredRouteFile::None);
  EXPECT_EQ(route.size(), 0u);
  expectNeverMutated(fs);
}

TEST(StoredRouteTest, TempOnlyIsNeverLoadedAndNeverRead) {
  RecordingRouteFs fs;
  navigator::StoredRoute route(fs);
  const Bytes& golden = goldenPackage();
  fs.seed(navigator::kRouteActiveTempPath, golden);

  navigator::RouteIndex candidate{};
  ASSERT_EQ(route.load(candidate), StoredRouteLoadResult::NotFound);
  EXPECT_FALSE(route.isLoaded());
  EXPECT_EQ(fs.readCalls, 0u);  // route.tmp was not even opened for reading
  expectNeverMutated(fs);
}

TEST(StoredRouteTest, FailedReloadClearsStaleSelection) {
  RecordingRouteFs fs;
  navigator::StoredRoute route(fs);
  const Bytes& golden = goldenPackage();
  fs.seed(navigator::kRouteActiveBinPath, golden);

  navigator::RouteIndex candidate{};
  ASSERT_EQ(route.load(candidate), StoredRouteLoadResult::LoadedActive);
  ASSERT_TRUE(route.isLoaded());
  ASSERT_EQ(route.size(), golden.size());

  // The store is now empty (e.g. SD card swap): a reload must leave the source
  // unreadable instead of serving the previously selected route.
  fs.clear(navigator::kRouteActiveBinPath);
  fs.clear(navigator::kRouteActiveBackupPath);
  ASSERT_EQ(route.load(candidate), StoredRouteLoadResult::NotFound);
  EXPECT_FALSE(route.isLoaded());
  EXPECT_EQ(route.selectedFile(), StoredRouteFile::None);
  EXPECT_EQ(route.size(), 0u);
  uint8_t buffer[16];
  EXPECT_EQ(route.read(0, buffer, sizeof(buffer)), 0u);
}

TEST(StoredRouteTest, FailedReloadFromValidToCorruptClearsStaleSelection) {
  RecordingRouteFs fs;
  navigator::StoredRoute route(fs);
  const Bytes& golden = goldenPackage();
  fs.seed(navigator::kRouteActiveBinPath, golden);

  navigator::RouteIndex candidate{};
  ASSERT_EQ(route.load(candidate), StoredRouteLoadResult::LoadedActive);
  ASSERT_TRUE(route.isLoaded());

  fs.seed(navigator::kRouteActiveBinPath, corruptByte(golden, 89));
  fs.seed(navigator::kRouteActiveBackupPath, corruptByte(golden, 88));
  ASSERT_EQ(route.load(candidate), StoredRouteLoadResult::Invalid);
  EXPECT_FALSE(route.isLoaded());
  EXPECT_EQ(route.selectedFile(), StoredRouteFile::None);
  EXPECT_EQ(route.size(), 0u);
}

// ---------------------------------------------------------------------------
// No loader path mutates files.
// ---------------------------------------------------------------------------

TEST(StoredRouteTest, NoLoaderPathMutatesFiles) {
  const Bytes& golden = goldenPackage();
  const Bytes flat = buildFlatRoute(999, 3);
  const Bytes corrupt1 = corruptByte(golden, 89);
  const Bytes corrupt2 = corruptByte(golden, 88);

  const struct {
    const char* bin;
    const char* backup;
    StoredRouteLoadResult expected;
  } scenarios[] = {
      {nullptr, nullptr, StoredRouteLoadResult::NotFound},
      {nullptr, "flat", StoredRouteLoadResult::LoadedBackup},
      {"golden", nullptr, StoredRouteLoadResult::LoadedActive},
      {"golden", "flat", StoredRouteLoadResult::LoadedActive},
      {"corrupt1", "flat", StoredRouteLoadResult::LoadedBackup},
      {"corrupt1", "corrupt2", StoredRouteLoadResult::Invalid},
      {"golden", "corrupt2", StoredRouteLoadResult::LoadedActive},
      {"corrupt1", nullptr, StoredRouteLoadResult::Invalid},
  };

  for (const auto& scenario : scenarios) {
    RecordingRouteFs roundFs;
    navigator::StoredRoute roundRoute(roundFs);
    if (scenario.bin != nullptr) {
      const Bytes* binBytes = std::strcmp(scenario.bin, "golden") == 0   ? &golden
                              : std::strcmp(scenario.bin, "flat") == 0   ? &flat
                                                                          : &corrupt1;
      roundFs.seed(navigator::kRouteActiveBinPath, *binBytes);
    }
    if (scenario.backup != nullptr) {
      const Bytes* backupBytes = std::strcmp(scenario.backup, "golden") == 0    ? &golden
                                 : std::strcmp(scenario.backup, "flat") == 0    ? &flat
                                 : std::strcmp(scenario.backup, "corrupt2") == 0 ? &corrupt2
                                                                                  : &corrupt1;
      roundFs.seed(navigator::kRouteActiveBackupPath, *backupBytes);
    }

    navigator::RouteIndex candidate{};
    EXPECT_EQ(roundRoute.load(candidate), scenario.expected);
    expectNeverMutated(roundFs);
  }
}

// ---------------------------------------------------------------------------
// Bounded reads and guarded offsets.
// ---------------------------------------------------------------------------

TEST(StoredRouteTest, SourceReadsAreBoundedAndOffsetsGuarded) {
  RecordingRouteFs fs;
  navigator::StoredRoute route(fs);
  const Bytes& golden = goldenPackage();
  fs.seed(navigator::kRouteActiveBinPath, golden);

  navigator::RouteIndex candidate{};
  ASSERT_EQ(route.load(candidate), StoredRouteLoadResult::LoadedActive);

  // Validation reads never exceeded the work-buffer bound.
  EXPECT_LE(fs.maxReadRequested, navigator::kRoutePackageV1WorkBufferBytes);

  // A rendering caller asking for more than 1,024 bytes is clamped to one
  // 1,024-byte request at the RouteFileSystem layer.
  uint8_t big[4096];
  const uint32_t got = route.read(0, big, sizeof(big));
  EXPECT_EQ(got, golden.size());
  EXPECT_LE(fs.maxReadRequested, navigator::kRoutePackageV1WorkBufferBytes);
  EXPECT_EQ(std::memcmp(big, golden.data(), golden.size()), 0);

  // Offsets at/past the end return 0 and never wrap.
  uint8_t tail[16];
  EXPECT_EQ(route.read(golden.size(), tail, sizeof(tail)), 0u);
  EXPECT_EQ(route.read(0xFFFFFFFFu, tail, sizeof(tail)), 0u);

  // A zero-length read is a no-op.
  EXPECT_EQ(route.read(0, tail, 0), 0u);
  expectNeverMutated(fs);
}

// ---------------------------------------------------------------------------
// The loader never allocates: no heap, no full-route buffer, no RouteIndex of
// its own (the ~9.8 KiB candidate is caller-owned).
// ---------------------------------------------------------------------------

TEST(StoredRouteTest, LoadPathNeverAllocates) {
  // Pre-built scenarios (all seeding happens outside the measured window).
  RecordingRouteFs activeFs;
  navigator::StoredRoute activeRoute(activeFs);
  RecordingRouteFs backupFs;
  navigator::StoredRoute backupRoute(backupFs);
  RecordingRouteFs corruptFs;
  navigator::StoredRoute corruptRoute(corruptFs);
  RecordingRouteFs emptyFs;
  navigator::StoredRoute emptyRoute(emptyFs);
  RecordingRouteFs tempOnlyFs;
  navigator::StoredRoute tempOnlyRoute(tempOnlyFs);

  const Bytes& golden = goldenPackage();
  activeFs.seed(navigator::kRouteActiveBinPath, golden);
  backupFs.seed(navigator::kRouteActiveBackupPath, golden);
  corruptFs.seed(navigator::kRouteActiveBinPath, corruptByte(golden, 89));
  corruptFs.seed(navigator::kRouteActiveBackupPath, corruptByte(golden, 88));
  tempOnlyFs.seed(navigator::kRouteActiveTempPath, golden);

  // Warm-up outside the measured window so no first-call laziness is counted.
  {
    navigator::RouteIndex warm{};
    ASSERT_EQ(activeRoute.load(warm), StoredRouteLoadResult::LoadedActive);
  }

  const std::size_t before = test_alloc::gAllocationCount;
  for (int round = 0; round < 20; ++round) {
    navigator::RouteIndex candidate{};
    EXPECT_EQ(activeRoute.load(candidate), StoredRouteLoadResult::LoadedActive);
    EXPECT_EQ(backupRoute.load(candidate), StoredRouteLoadResult::LoadedBackup);
    EXPECT_EQ(corruptRoute.load(candidate), StoredRouteLoadResult::Invalid);
    EXPECT_EQ(emptyRoute.load(candidate), StoredRouteLoadResult::NotFound);
    EXPECT_EQ(tempOnlyRoute.load(candidate), StoredRouteLoadResult::NotFound);
    EXPECT_EQ(candidate.routeId, 66051u);  // failures preserve the preceding successful decode
  }
  EXPECT_EQ(test_alloc::gAllocationCount, before);

  // The loaded sources still serve the whole package without any allocation.
  uint8_t buffer[1024];
  EXPECT_EQ(activeRoute.read(0, buffer, sizeof(buffer)), golden.size());
  EXPECT_EQ(backupRoute.read(0, buffer, sizeof(buffer)), golden.size());
}

TEST(StoredRouteTest, NullDestinationDoesNotReachFileSystem) {
  RecordingRouteFs fs;
  navigator::StoredRoute route(fs);
  fs.seed(navigator::kRouteActiveBinPath, goldenPackage());
  navigator::RouteIndex candidate{};
  ASSERT_EQ(route.load(candidate), StoredRouteLoadResult::LoadedActive);
  const auto reads = fs.readCalls;
  EXPECT_EQ(route.read(0, nullptr, 8), 0u);
  EXPECT_EQ(fs.readCalls, reads);
}

TEST(StoredRouteTest, LargeRouteReadStopsAtWorkBufferLimit) {
  RecordingRouteFs fs;
  navigator::StoredRoute route(fs);
  const auto bytes = buildFlatRoute(42, 1000);
  fs.seed(navigator::kRouteActiveBinPath, bytes);
  navigator::RouteIndex candidate{};
  ASSERT_EQ(route.load(candidate), StoredRouteLoadResult::LoadedActive);
  uint8_t buffer[2048];
  EXPECT_EQ(route.read(0, buffer, sizeof(buffer)), 1024u);
  EXPECT_EQ(std::memcmp(buffer, bytes.data(), 1024), 0);
  EXPECT_LE(fs.maxReadRequested, 1024u);
}

TEST(StoredRouteTest, PrepareRestoresValidBackupBeforeNewTransfer) {
  RecordingRouteFs fs;
  fs.allowRecovery = true;
  fs.seed(navigator::kRouteActiveBinPath, corruptByte(goldenPackage(), 89));
  fs.seed(navigator::kRouteActiveBackupPath, goldenPackage());
  navigator::StoredRoute route(fs);
  navigator::RouteIndex candidate{};
  ASSERT_TRUE(route.prepareForTransfer(candidate));
  EXPECT_EQ(fs.fileBytes(navigator::kRouteActiveBinPath), goldenPackage());
  EXPECT_FALSE(fs.fileExists(navigator::kRouteActiveBackupPath));
  EXPECT_EQ(route.selectedFile(), StoredRouteFile::Bin);
}

TEST(StoredRouteTest, InvalidStoreWithoutValidBackupAllowsTransfer) {
  RecordingRouteFs fs;
  navigator::StoredRoute route(fs);
  // route.bin is present but corrupt and there is no route.bak: load() reports
  // Invalid, and with nothing valid to protect the transfer must proceed so a
  // corrupt store never strands the phone without a recovery path.
  fs.seed(navigator::kRouteActiveBinPath, corruptByte(goldenPackage(), 89));
  navigator::RouteIndex candidate{};
  EXPECT_TRUE(route.prepareForTransfer(candidate));
  expectNeverMutated(fs);
}

TEST(StoredRouteTest, FailedRecoveryNeverAllowsTransferOrDeletesValidBackup) {
  RecordingRouteFs fs;
  fs.allowRecovery = true;
  fs.failMove = true;
  fs.seed(navigator::kRouteActiveBinPath, corruptByte(goldenPackage(), 89));
  fs.seed(navigator::kRouteActiveBackupPath, goldenPackage());
  navigator::StoredRoute route(fs);
  navigator::RouteIndex candidate{};
  EXPECT_FALSE(route.prepareForTransfer(candidate));
  EXPECT_EQ(fs.fileBytes(navigator::kRouteActiveBackupPath), goldenPackage());
  EXPECT_EQ(route.selectedFile(), StoredRouteFile::Backup);
}

TEST(StoredRouteTest, UnreadableStoreWithNoValidCopyAllowsTransferButNeverMutates) {
  RecordingRouteFs fs;
  fs.allowRecovery = true;
  fs.seed(navigator::kRouteActiveBinPath, goldenPackage());
  fs.stallPath = navigator::kRouteActiveBinPath;
  navigator::StoredRoute route(fs);
  navigator::RouteIndex candidate{};
  // The bin cannot be read, so nothing validates: like a corrupted store this
  // is Invalid, and with no validated backup there is nothing to protect - the
  // transfer proceeds. prepareForTransfer must still never remove/move on this
  // path, because there is no validated backup to promote.
  EXPECT_TRUE(route.prepareForTransfer(candidate));
  expectNeverMutated(fs);
}

}  // namespace
