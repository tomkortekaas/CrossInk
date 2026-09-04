// Host tests for the transactional RouteStore
// (src/spikes/navigator/route/RouteStore.h/.cpp) behind the RouteFileSystem
// abstraction.
//
// Task 7 (firmware core) - strict TDD: these tests were written first against
// the storage contract before RouteStore existed. The store under test must:
//   * stage incoming payload bytes in the canonical fixed temp file
//     /Navigation/Routes/active/route.tmp and only ever promote to
//     /Navigation/Routes/active/route.bin after validation (no dynamic
//     paths, no full package in RAM);
//   * expose the staged file as a RouteByteSource for the whole-package CRC
//     and Route Package v1 validation without loading it into memory;
//   * verify duplicate ranges against stored bytes with reads <= 1,024;
//   * keep the previous active route usable on every failure - a failed
//     promotion restores route.bin from route.bak, cleans route.tmp, and only
//     keeps route.bak when even the restore rename fails (recoverable);
//   * succeed with exactly route.bin present (no route.tmp, no route.bak);
//   * never allocate from the production core.

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "route/RoutePackageV1.h"
#include "route/RouteStore.h"
#include "route/RouteTransfer.h"

namespace {

using Bytes = std::vector<uint8_t>;

// ---------------------------------------------------------------------------
// Little-endian helpers, CRC, and fixture builders (test-side mirrors).
// ---------------------------------------------------------------------------

uint32_t crc32Step(uint32_t crc, uint8_t byte) {
  crc ^= byte;
  for (unsigned i = 0; i < 8; ++i) {
    crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
  }
  return crc;
}

uint32_t crc32Of(const uint8_t* data, size_t length) {
  uint32_t crc = 0xFFFFFFFFu;
  for (size_t i = 0; i < length; ++i) {
    crc = crc32Step(crc, data[i]);
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

// A deterministic, valid single-segment route with `pointCount` points that
// never moves (deltas are all zero at the origin).
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

// The 90-byte golden package, transcribed from test/fixtures/route_package_v1.bin.
Bytes goldenPackage() {
  static const Bytes kBytes = [] {
    static const uint8_t kData[90] = {
        0x58, 0x33, 0x52, 0x54, 0x01, 0x00, 0x26, 0x00, 0x5A, 0x00, 0x00, 0x00, 0x03, 0x02, 0x01, 0x00, 0x05, 0x00,
        0x01, 0x00, 0x60, 0xA9, 0x36, 0x1F, 0x68, 0x4E, 0xEC, 0x02, 0xE2, 0x04, 0x00, 0x00, 0x14, 0x00, 0x02, 0x00,
        0x04, 0x00, 0x57, 0x61, 0x6C, 0x6B, 0x00, 0x00, 0x03, 0x00, 0x78, 0x00, 0x50, 0x00, 0x46, 0x00, 0x1E, 0x00,
        0x90, 0xC1, 0x43, 0x15, 0xD8, 0xF1, 0x3C, 0x53, 0x32, 0x00, 0xD8, 0xFF, 0x02, 0x00, 0x04, 0x0C, 0x52, 0x03,
        0x00, 0x00, 0x41, 0x63, 0x68, 0x74, 0x65, 0x72, 0x67, 0x72, 0x61, 0x63, 0x68, 0x74, 0x1C, 0x20, 0xB0, 0x4C,
    };
    return Bytes(kData, kData + 90);
  }();
  return kBytes;
}

Bytes oldActiveBytes() {
  Bytes bytes(64, 0);
  for (size_t i = 0; i < bytes.size(); ++i) {
    bytes[i] = static_cast<uint8_t>(0xA0 + (i % 16));
  }
  return bytes;
}

// ---------------------------------------------------------------------------
// Bounded in-memory RouteFileSystem fake (see RouteTransferTest.cpp for the
// contract; this is a per-TU copy because no shared test header may be added).
// ---------------------------------------------------------------------------

class InMemoryRouteFs final : public navigator::RouteFileSystem {
 public:
  // Capacity at the directory quota (not the 65,535-byte package ceiling) so
  // the fake can model a pre-existing oversized active/backup file for quota
  // rejection tests.
  static constexpr uint32_t kFileCapacity = navigator::kRouteStoreQuotaBytes;

  InMemoryRouteFs() {
    files_[0].path = navigator::kRouteActiveTempPath;
    files_[1].path = navigator::kRouteActiveBinPath;
    files_[2].path = navigator::kRouteActiveBackupPath;
    for (File& file : files_) {
      file.bytes.reserve(kFileCapacity);
    }
  }

  void seed(const char* path, const Bytes& bytes) {
    File& file = *find(path);
    file.bytes.assign(bytes.begin(), bytes.end());
    file.exists = true;
  }

  bool fileExists(const char* path) const { return find(path)->exists; }
  const Bytes& fileBytes(const char* path) const { return find(path)->bytes; }

  uint32_t readCalls = 0;
  uint32_t maxReadRequested = 0;

  bool failNextCreate = false;
  bool failNextCreateLeavesPartial = false;  // createEmpty leaves an empty file, then fails
  bool failNextAppend = false;
  bool failNextFlush = false;
  bool failNextRemove = false;
  uint32_t failReadCall = 0;
  bool failMoveActiveToBackup = false;
  bool failMoveTempToActive = false;
  bool failMoveBackupToActive = false;

  bool exists(const char* path) override { return find(path)->exists; }

  uint32_t size(const char* path) override {
    const File& file = *find(path);
    return file.exists ? static_cast<uint32_t>(file.bytes.size()) : 0;
  }

  bool createEmpty(const char* path) override {
    if (failNextCreateLeavesPartial) {
      failNextCreateLeavesPartial = false;
      // Model a create that "failed" after already materializing the file (a
      // partial route.tmp), so begin() must clean it up best-effort.
      File& file = *find(path);
      file.exists = true;
      file.bytes.clear();
      return false;
    }
    if (failNextCreate) {
      failNextCreate = false;
      return false;
    }
    File& file = *find(path);
    file.exists = true;
    file.bytes.clear();
    return true;
  }

  bool append(const char* path, const uint8_t* data, uint32_t length) override {
    if (failNextAppend) {
      failNextAppend = false;
      return false;
    }
    File& file = *find(path);
    if (!file.exists || file.bytes.size() + length > file.bytes.capacity()) {
      return false;
    }
    file.bytes.insert(file.bytes.end(), data, data + length);
    return true;
  }

  bool flush(const char* path) override {
    if (failNextFlush) {
      failNextFlush = false;
      return false;
    }
    return find(path)->exists;
  }

  uint32_t read(const char* path, uint32_t offset, uint8_t* destination, uint32_t length) override {
    ++readCalls;
    maxReadRequested = std::max(maxReadRequested, length);
    if (failReadCall != 0 && readCalls == failReadCall) {
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
    if (failNextRemove) {
      failNextRemove = false;
      return false;
    }
    File& file = *find(path);
    if (!file.exists) {
      return true;
    }
    file.exists = false;
    file.bytes.clear();
    return true;
  }

  bool move(const char* fromPath, const char* toPath) override {
    File& from = *find(fromPath);
    File& to = *find(toPath);
    if (!from.exists || to.exists) {
      return false;
    }
    if (fromPath == navigator::kRouteActiveBinPath && failMoveActiveToBackup) {
      failMoveActiveToBackup = false;
      return false;
    }
    if (fromPath == navigator::kRouteActiveTempPath && failMoveTempToActive) {
      failMoveTempToActive = false;
      return false;
    }
    if (fromPath == navigator::kRouteActiveBackupPath && failMoveBackupToActive) {
      failMoveBackupToActive = false;
      return false;
    }
    to.bytes.assign(from.bytes.begin(), from.bytes.end());
    to.exists = true;
    from.exists = false;
    from.bytes.clear();
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

bool fileEquals(const InMemoryRouteFs& fs, const char* path, const Bytes& expected) {
  if (!fs.fileExists(path) || fs.fileBytes(path).size() != expected.size()) {
    return false;
  }
  const Bytes& actual = fs.fileBytes(path);
  return std::equal(actual.begin(), actual.end(), expected.begin());
}

}  // namespace

// The production allocation counter lives in RouteTransferTest.cpp (single
// global operator new override per test binary); deltas are measured here too.
namespace test_alloc {
extern std::size_t gAllocationCount;
}  // namespace test_alloc

namespace {

static_assert(navigator::kRouteStoreQuotaBytes == 256u * 1024u, "storage policy quota is 256 KiB");
static_assert(navigator::kRouteStoreQuotaBytes > 3u * navigator::kRoutePackageV1MaxBytes,
              "the 256 KiB quota fits temp + active + backup at the transport cap");

// ---------------------------------------------------------------------------
// Canonical paths and policy constants.
// ---------------------------------------------------------------------------

TEST(RouteStoreTest, CanonicalPathsAreFixed) {
  EXPECT_STREQ(navigator::kRouteActiveTempPath, "/Navigation/Routes/active/route.tmp");
  EXPECT_STREQ(navigator::kRouteActiveBinPath, "/Navigation/Routes/active/route.bin");
  EXPECT_STREQ(navigator::kRouteActiveBackupPath, "/Navigation/Routes/active/route.bak");
  EXPECT_NE(navigator::kRouteActiveTempPath[0], '\0');
  EXPECT_NE(navigator::kRouteActiveBinPath[0], '\0');
  EXPECT_NE(navigator::kRouteActiveBackupPath[0], '\0');
}

// ---------------------------------------------------------------------------
// Successful replacement.
// ---------------------------------------------------------------------------

TEST(RouteStoreTest, SuccessfulReplacementYieldsExactActiveRouteNoTempOrBackup) {
  InMemoryRouteFs fs;
  navigator::RouteStore store(fs);
  const Bytes old = oldActiveBytes();
  fs.seed(navigator::kRouteActiveBinPath, old);

  const Bytes& package = goldenPackage();
  ASSERT_TRUE(store.begin(static_cast<uint32_t>(package.size())));
  ASSERT_TRUE(store.append(package.data(), static_cast<uint32_t>(package.size())));
  ASSERT_TRUE(store.flush());

  navigator::RouteIndex out{};
  ASSERT_EQ(navigator::validateRoutePackageV1(store.stagedSource(), out), navigator::DecodeStatus::Ok);

  EXPECT_TRUE(store.publish());
  EXPECT_TRUE(fileEquals(fs, navigator::kRouteActiveBinPath, package));
  EXPECT_FALSE(fs.fileExists(navigator::kRouteActiveTempPath));
  EXPECT_FALSE(fs.fileExists(navigator::kRouteActiveBackupPath));
}

TEST(RouteStoreTest, FirstEverPublishLeavesNoBackup) {
  InMemoryRouteFs fs;
  navigator::RouteStore store(fs);
  const Bytes& package = goldenPackage();
  ASSERT_TRUE(store.begin(static_cast<uint32_t>(package.size())));
  ASSERT_TRUE(store.append(package.data(), static_cast<uint32_t>(package.size())));
  ASSERT_TRUE(store.flush());
  EXPECT_TRUE(store.publish());

  EXPECT_TRUE(fileEquals(fs, navigator::kRouteActiveBinPath, package));
  EXPECT_FALSE(fs.fileExists(navigator::kRouteActiveTempPath));
  EXPECT_FALSE(fs.fileExists(navigator::kRouteActiveBackupPath));
}

// ---------------------------------------------------------------------------
// Failed promotion recovery.
// ---------------------------------------------------------------------------

TEST(RouteStoreTest, FailedPromotionRestoresPreviousActiveAndCleansTemp) {
  InMemoryRouteFs fs;
  navigator::RouteStore store(fs);
  const Bytes old = oldActiveBytes();
  fs.seed(navigator::kRouteActiveBinPath, old);

  const Bytes& package = goldenPackage();
  ASSERT_TRUE(store.begin(static_cast<uint32_t>(package.size())));
  ASSERT_TRUE(store.append(package.data(), static_cast<uint32_t>(package.size())));
  ASSERT_TRUE(store.flush());

  fs.failMoveTempToActive = true;
  EXPECT_FALSE(store.publish());

  EXPECT_TRUE(fileEquals(fs, navigator::kRouteActiveBinPath, old));
  EXPECT_FALSE(fs.fileExists(navigator::kRouteActiveTempPath));
  EXPECT_FALSE(fs.fileExists(navigator::kRouteActiveBackupPath));
}

TEST(RouteStoreTest, FailedPromotionKeepsRecoverableBackupWhenRestoreFails) {
  InMemoryRouteFs fs;
  navigator::RouteStore store(fs);
  const Bytes old = oldActiveBytes();
  fs.seed(navigator::kRouteActiveBinPath, old);

  const Bytes& package = goldenPackage();
  ASSERT_TRUE(store.begin(static_cast<uint32_t>(package.size())));
  ASSERT_TRUE(store.append(package.data(), static_cast<uint32_t>(package.size())));
  ASSERT_TRUE(store.flush());

  fs.failMoveTempToActive = true;
  fs.failMoveBackupToActive = true;
  EXPECT_FALSE(store.publish());

  // The old active could not be restored to route.bin, so it must still be
  // reachable at route.bak (recoverable), and route.tmp is gone.
  EXPECT_FALSE(fs.fileExists(navigator::kRouteActiveBinPath));
  EXPECT_TRUE(fileEquals(fs, navigator::kRouteActiveBackupPath, old));
  EXPECT_FALSE(fs.fileExists(navigator::kRouteActiveTempPath));
}

TEST(RouteStoreTest, FailedBackupMovePreservesActiveAndCleansTemp) {
  InMemoryRouteFs fs;
  navigator::RouteStore store(fs);
  const Bytes old = oldActiveBytes();
  fs.seed(navigator::kRouteActiveBinPath, old);

  const Bytes& package = goldenPackage();
  ASSERT_TRUE(store.begin(static_cast<uint32_t>(package.size())));
  ASSERT_TRUE(store.append(package.data(), static_cast<uint32_t>(package.size())));
  ASSERT_TRUE(store.flush());

  fs.failMoveActiveToBackup = true;
  EXPECT_FALSE(store.publish());

  EXPECT_TRUE(fileEquals(fs, navigator::kRouteActiveBinPath, old));
  EXPECT_FALSE(fs.fileExists(navigator::kRouteActiveTempPath));
}

TEST(RouteStoreTest, InterruptedPromotionRecoversOnTheNextTransfer) {
  InMemoryRouteFs fs;
  navigator::RouteStore store(fs);
  const Bytes old = oldActiveBytes();
  // A crash between "old active moved to backup" and "temp promoted" leaves
  // route.bak as the only copy of the old active and no route.bin.
  fs.seed(navigator::kRouteActiveBackupPath, old);

  const Bytes& package = goldenPackage();
  ASSERT_TRUE(store.begin(static_cast<uint32_t>(package.size())));
  // begin() must keep the recovery backup because route.bin is missing.
  EXPECT_TRUE(fileEquals(fs, navigator::kRouteActiveBackupPath, old));
  ASSERT_TRUE(store.append(package.data(), static_cast<uint32_t>(package.size())));
  ASSERT_TRUE(store.flush());

  EXPECT_TRUE(store.publish());
  EXPECT_TRUE(fileEquals(fs, navigator::kRouteActiveBinPath, package));
  EXPECT_FALSE(fs.fileExists(navigator::kRouteActiveTempPath));
  EXPECT_FALSE(fs.fileExists(navigator::kRouteActiveBackupPath));
}

// ---------------------------------------------------------------------------
// begin() / append() / abandon() bounds.
// ---------------------------------------------------------------------------

TEST(RouteStoreTest, BeginRejectsZeroAndOversizedDeclaredLengths) {
  InMemoryRouteFs fs;
  navigator::RouteStore store(fs);
  EXPECT_FALSE(store.begin(0));
  EXPECT_FALSE(store.begin(navigator::kRoutePackageV1MaxBytes + 1));
  EXPECT_FALSE(fs.fileExists(navigator::kRouteActiveTempPath));
}

// ---------------------------------------------------------------------------
// begin() directory-quota enforcement (correction round).
// ---------------------------------------------------------------------------

TEST(RouteStoreTest, BeginRejectsWhenActiveFileWouldExceedQuota) {
  InMemoryRouteFs fs;
  navigator::RouteStore store(fs);
  // A pre-existing active route that alone already sits at the 256 KiB quota.
  const Bytes oversized(navigator::kRouteStoreQuotaBytes, 0xA5);
  fs.seed(navigator::kRouteActiveBinPath, oversized);

  EXPECT_FALSE(store.begin(90));
  // Rejection happens before any file is touched: no route.tmp is created and
  // the oversized active route is preserved byte-for-byte.
  EXPECT_FALSE(fs.fileExists(navigator::kRouteActiveTempPath));
  EXPECT_TRUE(fileEquals(fs, navigator::kRouteActiveBinPath, oversized));
  EXPECT_FALSE(fs.fileExists(navigator::kRouteActiveBackupPath));
}

TEST(RouteStoreTest, BeginQuotaBoundaryIsExactFitAndRejectsOneByteOver) {
  InMemoryRouteFs fs;
  navigator::RouteStore store(fs);
  const uint32_t declared = 90;

  // Exact fit: active (quota - declared) + staged declared == quota.
  Bytes fit(navigator::kRouteStoreQuotaBytes - declared, 0x5A);
  fs.seed(navigator::kRouteActiveBinPath, fit);
  ASSERT_TRUE(store.begin(declared));
  store.abandon();

  // One byte over: begin must reject and leave the active file untouched.
  Bytes over(navigator::kRouteStoreQuotaBytes - declared + 1, 0x5A);
  fs.seed(navigator::kRouteActiveBinPath, over);
  EXPECT_FALSE(store.begin(declared));
  EXPECT_FALSE(fs.fileExists(navigator::kRouteActiveTempPath));
  EXPECT_TRUE(fileEquals(fs, navigator::kRouteActiveBinPath, over));
}

TEST(RouteStoreTest, BeginRejectsWhenRecoveryBackupWouldExceedQuota) {
  InMemoryRouteFs fs;
  navigator::RouteStore store(fs);
  // With route.bin absent, route.bak is the recovery copy of an interrupted
  // promotion and is kept, so it counts against the prospective quota.
  const Bytes oversized(navigator::kRouteStoreQuotaBytes, 0x3C);
  fs.seed(navigator::kRouteActiveBackupPath, oversized);

  EXPECT_FALSE(store.begin(90));
  EXPECT_FALSE(fs.fileExists(navigator::kRouteActiveTempPath));
  EXPECT_FALSE(fs.fileExists(navigator::kRouteActiveBinPath));
  EXPECT_TRUE(fileEquals(fs, navigator::kRouteActiveBackupPath, oversized));
}

TEST(RouteStoreTest, FailedCreateEmptyRemovesPartialTempBestEffort) {
  InMemoryRouteFs fs;
  navigator::RouteStore store(fs);
  const Bytes old = oldActiveBytes();
  fs.seed(navigator::kRouteActiveBinPath, old);

  // createEmpty leaves a partial route.tmp behind and then reports failure;
  // begin() must clean the leftover best-effort and preserve the active route.
  fs.failNextCreateLeavesPartial = true;
  EXPECT_FALSE(store.begin(90));
  EXPECT_FALSE(fs.fileExists(navigator::kRouteActiveTempPath));
  EXPECT_TRUE(fileEquals(fs, navigator::kRouteActiveBinPath, old));
}

TEST(RouteStoreTest, FailedCreateEmptyKeepsPartialTempWhenRemoveFails) {
  InMemoryRouteFs fs;
  navigator::RouteStore store(fs);
  const Bytes old = oldActiveBytes();
  fs.seed(navigator::kRouteActiveBinPath, old);

  // When even the best-effort remove of the partial route.tmp fails, begin()
  // still reports failure and the previous active route is preserved; cleanup
  // is best-effort only and may leave the stale temp behind.
  fs.failNextCreateLeavesPartial = true;
  fs.failNextRemove = true;
  EXPECT_FALSE(store.begin(90));
  EXPECT_TRUE(fs.fileExists(navigator::kRouteActiveTempPath));
  EXPECT_TRUE(fileEquals(fs, navigator::kRouteActiveBinPath, old));
}

TEST(RouteStoreTest, BeginCleansStaleTempAndStaleBackupNextToActive) {
  InMemoryRouteFs fs;
  navigator::RouteStore store(fs);
  fs.seed(navigator::kRouteActiveTempPath, Bytes(10, 0x11));
  fs.seed(navigator::kRouteActiveBinPath, oldActiveBytes());
  fs.seed(navigator::kRouteActiveBackupPath, Bytes(10, 0x22));  // stale, active exists

  ASSERT_TRUE(store.begin(90));
  EXPECT_TRUE(fs.fileExists(navigator::kRouteActiveTempPath));
  EXPECT_EQ(fs.size(navigator::kRouteActiveTempPath), 0u);
  EXPECT_FALSE(fs.fileExists(navigator::kRouteActiveBackupPath));
  EXPECT_TRUE(fileEquals(fs, navigator::kRouteActiveBinPath, oldActiveBytes()));
}

TEST(RouteStoreTest, AppendEnforcesStagedCeiling) {
  InMemoryRouteFs fs;
  navigator::RouteStore store(fs);
  ASSERT_TRUE(store.begin(navigator::kRoutePackageV1MaxBytes));
  Bytes full(navigator::kRoutePackageV1MaxBytes, 0xAB);
  EXPECT_TRUE(store.append(full.data(), navigator::kRoutePackageV1MaxBytes));
  const uint8_t one = 0xCD;
  EXPECT_FALSE(store.append(&one, 1));
  EXPECT_EQ(fs.size(navigator::kRouteActiveTempPath), navigator::kRoutePackageV1MaxBytes);
}

TEST(RouteStoreTest, AbandonCleansTempAndPreservesActive) {
  InMemoryRouteFs fs;
  navigator::RouteStore store(fs);
  const Bytes old = oldActiveBytes();
  fs.seed(navigator::kRouteActiveBinPath, old);
  const Bytes& package = goldenPackage();

  ASSERT_TRUE(store.begin(static_cast<uint32_t>(package.size())));
  ASSERT_TRUE(store.append(package.data(), static_cast<uint32_t>(package.size())));
  store.abandon();

  EXPECT_FALSE(fs.fileExists(navigator::kRouteActiveTempPath));
  EXPECT_TRUE(fileEquals(fs, navigator::kRouteActiveBinPath, old));
}

TEST(RouteStoreTest, FlushFailureIsReported) {
  InMemoryRouteFs fs;
  navigator::RouteStore store(fs);
  const Bytes& package = goldenPackage();
  ASSERT_TRUE(store.begin(static_cast<uint32_t>(package.size())));
  ASSERT_TRUE(store.append(package.data(), static_cast<uint32_t>(package.size())));
  fs.failNextFlush = true;
  EXPECT_FALSE(store.flush());
}

TEST(RouteStoreTest, StagedMatchesVerifiesExactBytes) {
  InMemoryRouteFs fs;
  navigator::RouteStore store(fs);
  const Bytes& package = goldenPackage();
  ASSERT_TRUE(store.begin(static_cast<uint32_t>(package.size())));
  ASSERT_TRUE(store.append(package.data(), 30));

  EXPECT_EQ(store.matchesStaged(0, package.data(), 30), navigator::RouteSink::VerifyResult::Match);
  EXPECT_EQ(store.matchesStaged(10, package.data() + 10, 20), navigator::RouteSink::VerifyResult::Match);

  Bytes corrupted = Bytes(package.begin(), package.begin() + 30);
  corrupted[0] ^= 0xFF;
  EXPECT_EQ(store.matchesStaged(0, corrupted.data(), 30), navigator::RouteSink::VerifyResult::Differ);

  fs.failReadCall = fs.readCalls + 1;  // fail the next read call
  EXPECT_EQ(store.matchesStaged(0, package.data(), 30), navigator::RouteSink::VerifyResult::ReadFailed);
}

// ---------------------------------------------------------------------------
// Staged RouteByteSource: whole-file validation without loading it into RAM.
// ---------------------------------------------------------------------------

TEST(RouteStoreTest, StagedSourceStreamsBigRouteWithBoundedReads) {
  InMemoryRouteFs fs;
  navigator::RouteStore store(fs);
  const Bytes big = buildFlatRoute(7, 3000);  // 12,040 bytes, far past 1,024
  ASSERT_GT(big.size(), 1024u);

  ASSERT_TRUE(store.begin(static_cast<uint32_t>(big.size())));
  ASSERT_TRUE(store.append(big.data(), static_cast<uint32_t>(big.size())));
  ASSERT_TRUE(store.flush());

  navigator::RouteIndex out{};
  ASSERT_EQ(navigator::validateRoutePackageV1(store.stagedSource(), out), navigator::DecodeStatus::Ok);
  EXPECT_EQ(out.routeId, 7u);
  EXPECT_EQ(out.declaredLength, big.size());
  EXPECT_EQ(out.pointCount, 3000u);

  EXPECT_LE(fs.maxReadRequested, navigator::kRoutePackageV1WorkBufferBytes);
  EXPECT_GE(fs.readCalls, 12u);
}

// ---------------------------------------------------------------------------
// No-allocation proof across the store API (allocation counter is defined in
// RouteTransferTest.cpp; files keep their reserved capacity).
// ---------------------------------------------------------------------------

TEST(RouteStoreTest, StoreOperationsNeverAllocate) {
  InMemoryRouteFs fs;
  navigator::RouteStore store(fs);
  const Bytes& package = goldenPackage();
  navigator::RouteIndex out{};
  const uint32_t length = static_cast<uint32_t>(package.size());

  // Warm-up outside the measured window.
  ASSERT_TRUE(store.begin(length));
  ASSERT_TRUE(store.append(package.data(), length));
  ASSERT_TRUE(store.flush());
  ASSERT_EQ(navigator::validateRoutePackageV1(store.stagedSource(), out), navigator::DecodeStatus::Ok);
  ASSERT_TRUE(store.publish());

  const std::size_t before = test_alloc::gAllocationCount;
  for (int round = 0; round < 20; ++round) {
    const bool began = store.begin(length);
    const bool appended = began && store.append(package.data(), length);
    const bool flushed = appended && store.flush();
    const bool verified =
        flushed && navigator::validateRoutePackageV1(store.stagedSource(), out) == navigator::DecodeStatus::Ok;
    const bool published = verified && store.publish();
    if (!(began && appended && flushed && verified && published)) {
      ADD_FAILURE() << "store round " << round << " failed";
    }
  }
  EXPECT_EQ(test_alloc::gAllocationCount, before);
  EXPECT_TRUE(fileEquals(fs, navigator::kRouteActiveBinPath, package));
  EXPECT_FALSE(fs.fileExists(navigator::kRouteActiveTempPath));
  EXPECT_FALSE(fs.fileExists(navigator::kRouteActiveBackupPath));
}

}  // namespace
