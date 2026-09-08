// Host tests for the bounded Route Transfer v1 receiver state machine
// (src/spikes/navigator/route/RouteTransfer.h/.cpp) driving the transactional
// RouteStore (src/spikes/navigator/route/RouteStore.h/.cpp) over an in-memory,
// bounded RouteFileSystem fake.
//
// Task 7 (firmware core) - strict TDD: these tests were written first against
// the wire contract shared with the Swift RouteTransferFrameBuilder and the
// checked-in golden fixture test/fixtures/route_package_v1.bin (90 bytes,
// route id 66051 = 0x00010203). The golden package carries two distinct CRC
// values that the tests pin independently:
//   * the internal trailer CRC over the package content (bytes 0..85),
//     0x4CB0201C, validated by validateRoutePackageV1; and
//   * the START whole-package transport CRC over every transmitted byte,
//     content *plus* that four-byte trailer. Because the trailer is a correct
//     IEEE CRC of the content, this whole-file CRC is the constant residue
//     0x2144DF1C - it is the transport's redundancy check, not a content
//     checksum.
//
// The receiver core under test must:
//   * parse START/CHUNK/COMMIT (0x05/0x06/0x07) with exact frame sizes,
//     little-endian u32 fields, and strict contiguity (no gaps, overlaps,
//     overshoot, zero payloads, wrong ids, START-while-active, or
//     commit-before-complete);
//   * pass accepted payload bytes straight to the RouteSink (no retained
//     package, no BLE frame copy, per-call input <= 512 bytes, work buffers
//     <= 1,024 bytes, no heap);
//   * treat a duplicate chunk that lies wholly inside already-staged bytes as
//     idempotent only when the staged file verifies the exact bytes;
//   * on COMMIT flush the staged file, stream-verify the whole-package CRC and
//     validateRoutePackageV1 from the staged RouteByteSource, then promote
//     transactionally - and on every failure clean route.tmp while the
//     previous active route stays usable;
//   * expose the five route statuses (0x21 route-ready, 0x22 route-progress,
//     0x23 route-accepted, 0x24 route-invalid, 0x25 route-storage-failed)
//     through a struct that serializes to the seven-byte BLE status envelope
//     (code, routeId u32 LE, received u16 LE).

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <string>
#include <vector>

#include "route/RoutePackageV1.h"
#include "route/RouteStore.h"
#include "route/RouteTransfer.h"
#include "route/NavigationRouteSession.h"

namespace {

using Bytes = std::vector<uint8_t>;

// ---------------------------------------------------------------------------
// Little-endian helpers and the test-side CRC-32 (IEEE reflected,
// poly 0xEDB88320) matching the authoritative wire contract.
// ---------------------------------------------------------------------------

uint32_t rdU32(const uint8_t* p) {
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) | (static_cast<uint32_t>(p[2]) << 16) |
         (static_cast<uint32_t>(p[3]) << 24);
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

// ---------------------------------------------------------------------------
// The 90-byte golden Route Package v1, pinned from the checked-in fixture
// (and byte-identical to the Swift golden vector). A hex transcription keeps
// these host tests hermetic; a sanity test below re-derives the manifest CRC
// and decodes the bytes so a transcription error cannot hide.
// ---------------------------------------------------------------------------

const char* const kGoldenHex =
    "58 33 52 54 01 00 26 00 5a 00 00 00 03 02 01 00 "
    "05 00 01 00 60 a9 36 1f 68 4e ec 02 e2 04 00 00 "
    "14 00 02 00 04 00 57 61 6c 6b 00 00 03 00 78 00 "
    "50 00 46 00 1e 00 90 c1 43 15 d8 f1 3c 53 32 00 "
    "d8 ff 02 00 04 0c 52 03 00 00 41 63 68 74 65 72 "
    "67 72 61 63 68 74 1c 20 b0 4c";

Bytes hexToBytes(const char* hex) {
  Bytes bytes;
  while (*hex != '\0') {
    if (*hex == ' ' || *hex == '\n' || *hex == '\r' || *hex == '\t') {
      ++hex;
      continue;
    }
    auto nibble = [](char c) -> uint8_t {
      if (c >= '0' && c <= '9') return static_cast<uint8_t>(c - '0');
      if (c >= 'a' && c <= 'f') return static_cast<uint8_t>(c - 'a' + 10);
      return static_cast<uint8_t>(c - 'A' + 10);
    };
    bytes.push_back(static_cast<uint8_t>((nibble(hex[0]) << 4) | nibble(hex[1])));
    hex += 2;
  }
  return bytes;
}

const Bytes& goldenPackage() {
  static const Bytes kBytes = hexToBytes(kGoldenHex);
  return kBytes;
}

// ---------------------------------------------------------------------------
// Wire frame builders mirroring RouteTransferFrameBuilder exactly: START reads
// id/length/CRC from the package itself, chunks are contiguous slices capped
// at (maximumWriteLength - 9) payload bytes, COMMIT closes the sequence.
// ---------------------------------------------------------------------------

Bytes makeStart(uint32_t routeId, uint32_t totalLength, uint32_t wholeCrc) {
  Bytes frame(13, 0);
  frame[0] = 0x05;
  putU32(frame, 1, routeId);
  putU32(frame, 5, totalLength);
  putU32(frame, 9, wholeCrc);
  return frame;
}

Bytes makeChunk(uint32_t routeId, uint32_t offset, const Bytes& payload) {
  Bytes frame(9 + payload.size(), 0);
  frame[0] = 0x06;
  putU32(frame, 1, routeId);
  putU32(frame, 5, offset);
  std::copy(payload.begin(), payload.end(), frame.begin() + 9);
  return frame;
}

Bytes makeCommit(uint32_t routeId) {
  Bytes frame(5, 0);
  frame[0] = 0x07;
  putU32(frame, 1, routeId);
  return frame;
}

struct TransferFrames {
  Bytes start;
  std::vector<Bytes> chunks;
  Bytes commit;
};

TransferFrames buildTransferFrames(const Bytes& package, uint32_t maximumWriteLength) {
  TransferFrames frames;
  const uint32_t routeId = rdU32(package.data() + 12);
  const uint32_t length = static_cast<uint32_t>(package.size());
  // Whole-package transport CRC: every transmitted byte, including the
  // 4-byte internal trailer (for a valid package this is the constant IEEE
  // residue, 0x2144DF1C for the golden fixture).
  const uint32_t wholeCrc = crc32Of(package.data(), package.size());
  frames.start = makeStart(routeId, length, wholeCrc);

  const uint32_t payloadCapacity = maximumWriteLength - 9;
  uint32_t offset = 0;
  while (offset < length) {
    const uint32_t count = std::min(payloadCapacity, length - offset);
    Bytes payload(package.begin() + offset, package.begin() + offset + count);
    frames.chunks.push_back(makeChunk(routeId, offset, payload));
    offset += count;
  }
  frames.commit = makeCommit(routeId);
  return frames;
}

// A deterministic single-segment, no-name route whose deltas all stay at the
// origin: valid, cheap to build, and large enough (pointCount ~ thousands) to
// exercise multi-chunk transfers and > 1,024-byte staged reads.
Bytes buildFlatRoute(uint32_t routeId, uint16_t pointCount) {
  Bytes payload;
  appendU16(payload, 0);  // segment starts: single segment at point 0
  for (uint16_t i = 1; i < pointCount; ++i) {
    appendU16(payload, 0);  // latitude delta E5
    appendU16(payload, 0);  // longitude delta E5
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
  putU32(bytes, 20, 0);  // origin latitude E7
  putU32(bytes, 24, 0);  // origin longitude E7
  putU32(bytes, 28, 0);  // total distance
  putU16(bytes, 32, 0);  // estimated minutes
  putU16(bytes, 34, 1);  // segment count
  bytes[36] = 0;         // empty route name
  bytes[37] = 0;         // reserved
  std::copy(payload.begin(), payload.end(), bytes.begin() + 38);
  putU32(bytes, total - 4, crc32Of(bytes.data(), total - 4));
  return bytes;
}

// ---------------------------------------------------------------------------
// Bounded in-memory RouteFileSystem fake. Files keep their reserved capacity
// for the whole test so measured no-allocation windows stay allocation-free;
// move() copies into the destination's pre-reserved buffer instead of
// transferring ownership.
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

  // One-shot failure injection (consumed when it triggers).
  bool failNextCreate = false;
  bool failNextCreateLeavesPartial = false;  // createEmpty leaves an empty file, then fails
  bool failNextAppend = false;
  bool failNextFlush = false;
  bool failNextRemove = false;
  uint32_t failReadCall = 0;  // 1-based read-call number that returns 0 (stall)
  uint32_t overReportReadCall = 0;  // 1-based read-call number that returns length + 1
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
    if (overReportReadCall != 0 && readCalls == overReportReadCall) {
      // Violate the RouteByteSource contract: report more bytes than requested.
      return length + 1u;
    }
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
      return false;  // rename semantics: destination must be absent
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

// ---------------------------------------------------------------------------
// Harness: real RouteStore over the fake file system, driven by one
// RouteTransfer state machine with a caller-owned RouteIndex workspace (the
// `candidate` staging buffer, never the caller's live active index).
// ---------------------------------------------------------------------------

// Minimal random-access source over in-memory bytes, used only to seed
// RouteIndex fixtures (the production path always streams from staged files).
class ByteVectorSource final : public navigator::RouteByteSource {
 public:
  explicit ByteVectorSource(const Bytes& bytes) : bytes_(bytes) {}
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

struct TransferHarness {
  InMemoryRouteFs fs;
  navigator::RouteStore store{fs};
  navigator::RouteTransfer transfer;
  navigator::RouteIndex out{};

  navigator::RouteTransferStatus feed(const Bytes& frame) {
    return transfer.handleFrame(frame.data(), static_cast<uint32_t>(frame.size()), store, out);
  }

  // Feeds START, every chunk, then COMMIT and returns the final status.
  navigator::RouteTransferStatus feedAll(const TransferFrames& frames) {
    navigator::RouteTransferStatus last = feed(frames.start);
    for (const Bytes& chunk : frames.chunks) {
      last = feed(chunk);
    }
    return feed(frames.commit);
  }
};

bool fileEquals(const InMemoryRouteFs& fs, const char* path, const Bytes& expected) {
  if (!fs.fileExists(path) || fs.fileBytes(path).size() != expected.size()) {
    return false;
  }
  const Bytes& actual = fs.fileBytes(path);
  return std::equal(actual.begin(), actual.end(), expected.begin());
}

// A small previous active route, distinct from the golden fixture, used to
// prove failures never disturb the route that was active before a transfer.
Bytes oldActiveBytes() {
  Bytes bytes(64, 0xA5);
  for (size_t i = 0; i < bytes.size(); ++i) {
    bytes[i] = static_cast<uint8_t>(0xA0 + (i % 16));
  }
  return bytes;
}

// ---------------------------------------------------------------------------
// Global allocation counter + operator new override (defined here; referenced
// from RouteStoreTest.cpp through the extern below). Deltas over synchronous
// production calls assert the transfer/store core never allocates.
// ---------------------------------------------------------------------------

TEST(NavigationRouteSessionTest, ImportedIPhonePackageRoundTripsThroughStoredRoute) {
  const char* path = std::getenv("NAV_IMPORTED_PACKAGE");
  if (!path) GTEST_SKIP() << "Set NAV_IMPORTED_PACKAGE to real app importer output";
  FILE* file = std::fopen(path, "rb");
  ASSERT_NE(file, nullptr);
  Bytes bytes(65535);
  const size_t count = std::fread(bytes.data(), 1, bytes.size(), file);
  const bool complete = std::fgetc(file) == EOF && !std::ferror(file);
  std::fclose(file);
  ASSERT_TRUE(complete);
  ASSERT_GT(count, 38u);
  bytes.resize(count);
  for (const uint32_t mtu : {20u, 185u, 512u}) {
    InMemoryRouteFs fs;
    fs.seed(navigator::kRouteActiveBinPath, goldenPackage());
    navigator::NavigationRouteSession session(fs);
    ASSERT_TRUE(session.load());
    auto frames = buildTransferFrames(bytes, mtu);
    ASSERT_EQ(session.receive(frames.start.data(), frames.start.size()).code,
              navigator::RouteTransferCode::RouteReady);
    for (const auto& chunk : frames.chunks) {
      ASSERT_EQ(session.receive(chunk.data(), chunk.size()).code,
                navigator::RouteTransferCode::RouteProgress);
    }
    const auto committed = session.receive(frames.commit.data(), frames.commit.size());
    ASSERT_EQ(committed.code, navigator::RouteTransferCode::RouteAccepted);
    EXPECT_EQ(committed.received, count);
    EXPECT_EQ(session.index().routeId, rdU32(bytes.data() + 12));
    EXPECT_EQ(session.index().segmentCount, 2u);
    EXPECT_GT(session.index().totalDistanceMeters, 0u);
    EXPECT_EQ(fs.fileBytes(navigator::kRouteActiveBinPath), bytes);
    session.disconnect();
    EXPECT_TRUE(session.load());
    EXPECT_EQ(session.index().segmentCount, 2u);
  }
}

TEST(NavigationRouteSessionTest, PublishesOnlyAfterCommitAndPreservesOldOnDisconnect) {
  InMemoryRouteFs fs;
  fs.seed(navigator::kRouteActiveBinPath, buildFlatRoute(42, 2));
  navigator::NavigationRouteSession session(fs);
  ASSERT_TRUE(session.load());
  auto frames = buildTransferFrames(goldenPackage(), 185);
  EXPECT_EQ(session.receive(frames.start.data(), frames.start.size()).code,
            navigator::RouteTransferCode::RouteReady);
  for (const auto& chunk : frames.chunks) session.receive(chunk.data(), chunk.size());
  EXPECT_EQ(session.index().routeId, 42u);
  session.disconnect();
  EXPECT_EQ(session.index().routeId, 42u);
  EXPECT_FALSE(fs.exists(navigator::kRouteActiveTempPath));
  session.receive(frames.start.data(), frames.start.size());
  for (const auto& chunk : frames.chunks) session.receive(chunk.data(), chunk.size());
  EXPECT_EQ(session.receive(frames.commit.data(), frames.commit.size()).code,
            navigator::RouteTransferCode::RouteAccepted);
  EXPECT_EQ(session.index().routeId, 66051u);
  EXPECT_TRUE(session.hasRoute());
}

TEST(NavigationRouteSessionTest, BackupRepairFailureRefusesTransfer) {
  InMemoryRouteFs fs;
  fs.seed(navigator::kRouteActiveBinPath, Bytes{1, 2, 3});
  fs.seed(navigator::kRouteActiveBackupPath, goldenPackage());
  navigator::NavigationRouteSession session(fs);
  ASSERT_TRUE(session.load());
  fs.failMoveBackupToActive = true;
  auto frames = buildTransferFrames(goldenPackage(), 185);
  EXPECT_EQ(session.receive(frames.start.data(), frames.start.size()).code,
            navigator::RouteTransferCode::RouteStorageFailed);
  EXPECT_EQ(fs.fileBytes(navigator::kRouteActiveBackupPath), goldenPackage());
  EXPECT_FALSE(fs.exists(navigator::kRouteActiveTempPath));
  EXPECT_TRUE(session.hasRoute());
}

TEST(NavigationRouteSessionTest, CorruptStoreWithoutBackupStillAcceptsStart) {
  InMemoryRouteFs fs;
  fs.seed(navigator::kRouteActiveBinPath, Bytes{1, 2, 3});
  navigator::NavigationRouteSession session(fs);
  EXPECT_FALSE(session.load());  // corrupt store, nothing valid to serve
  auto frames = buildTransferFrames(goldenPackage(), 185);
  // A corrupt store must not reject the START with RouteStorageFailed: there is
  // nothing valid to protect, so the replacement transfer proceeds normally.
  EXPECT_EQ(session.receive(frames.start.data(), frames.start.size()).code,
            navigator::RouteTransferCode::RouteReady);
}

TEST(NavigationRouteSessionTest, InvalidStartLengthCannotTriggerStorageRecovery) {
  InMemoryRouteFs fs;
  const Bytes corrupt{1, 2, 3};
  fs.seed(navigator::kRouteActiveBinPath, corrupt);
  fs.seed(navigator::kRouteActiveBackupPath, goldenPackage());
  navigator::NavigationRouteSession session(fs);
  ASSERT_TRUE(session.load());
  const auto start = makeStart(66051, 0, 0);
  EXPECT_EQ(session.receive(start.data(), start.size()).code, navigator::RouteTransferCode::RouteInvalid);
  EXPECT_EQ(fs.fileBytes(navigator::kRouteActiveBinPath), corrupt);
  EXPECT_EQ(fs.fileBytes(navigator::kRouteActiveBackupPath), goldenPackage());
}

TEST(NavigationRouteSessionTest, FailedPublishKeepsPreviousIndexAndSource) {
  InMemoryRouteFs fs;
  fs.seed(navigator::kRouteActiveBinPath, buildFlatRoute(42, 2));
  navigator::NavigationRouteSession session(fs);
  ASSERT_TRUE(session.load());
  auto frames = buildTransferFrames(goldenPackage(), 185);
  session.receive(frames.start.data(), frames.start.size());
  for (const auto& chunk : frames.chunks) session.receive(chunk.data(), chunk.size());
  fs.failMoveTempToActive = true;
  fs.failMoveBackupToActive = true;
  EXPECT_EQ(session.receive(frames.commit.data(), frames.commit.size()).code,
            navigator::RouteTransferCode::RouteStorageFailed);
  EXPECT_EQ(session.index().routeId, 42u);
  EXPECT_TRUE(session.hasRoute());
  EXPECT_GT(session.source().size(), 0u);
}

}  // namespace

namespace test_alloc {
std::size_t gAllocationCount = 0;
}  // namespace test_alloc

void* operator new(std::size_t size) {
  ++test_alloc::gAllocationCount;
  if (void* p = std::malloc(size)) {
    return p;
  }
  std::abort();
}

namespace {

// ---------------------------------------------------------------------------
// Sanity: the embedded golden bytes are the checked-in fixture.
// ---------------------------------------------------------------------------

TEST(RouteTransferTest, EmbeddedGoldenBytesMatchManifest) {
  const Bytes& golden = goldenPackage();
  ASSERT_EQ(golden.size(), 90u);
  EXPECT_EQ(rdU32(golden.data() + 8), 90u);      // declared length
  EXPECT_EQ(rdU32(golden.data() + 12), 66051u);  // route id 0x00010203
  // Internal trailer CRC over the content (bytes 0..85) is the manifest CRC.
  EXPECT_EQ(rdU32(golden.data() + 86), 0x4CB0201Cu);
  EXPECT_EQ(crc32Of(golden.data(), 86), 0x4CB0201Cu);
  // Whole-package transport CRC over all 90 bytes (content + trailer) is the
  // constant IEEE residue for a correctly appended CRC.
  EXPECT_EQ(crc32Of(golden.data(), 90), 0x2144DF1Cu);

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
}

// ---------------------------------------------------------------------------
// Golden Swift-compatible frames at write lengths 20 / 185 / 512.
// ---------------------------------------------------------------------------

TEST(RouteTransferTest, GoldenFramesAtWriteLength20ReconstructAndAccept) {
  TransferHarness harness;
  const Bytes& golden = goldenPackage();
  const TransferFrames frames = buildTransferFrames(golden, 20);

  ASSERT_EQ(frames.chunks.size(), 9u);  // 90 bytes at 11 payload bytes per chunk

  const navigator::RouteTransferStatus start = harness.feed(frames.start);
  EXPECT_EQ(start.code, navigator::RouteTransferCode::RouteReady);
  EXPECT_EQ(start.routeId, 66051u);
  EXPECT_EQ(start.received, 0u);
  EXPECT_TRUE(harness.transfer.isActive());

  uint32_t expectedReceived = 0;
  for (size_t i = 0; i < frames.chunks.size(); ++i) {
    const navigator::RouteTransferStatus status = harness.feed(frames.chunks[i]);
    expectedReceived += static_cast<uint32_t>(frames.chunks[i].size() - 9);
    EXPECT_EQ(status.code, navigator::RouteTransferCode::RouteProgress) << "chunk " << i;
    EXPECT_EQ(status.routeId, 66051u) << "chunk " << i;
    EXPECT_EQ(status.received, expectedReceived) << "chunk " << i;
  }
  EXPECT_EQ(expectedReceived, 90u);

  const navigator::RouteTransferStatus commit = harness.feed(frames.commit);
  EXPECT_EQ(commit.code, navigator::RouteTransferCode::RouteAccepted);
  EXPECT_EQ(commit.routeId, 66051u);
  EXPECT_EQ(commit.received, 90u);
  EXPECT_FALSE(harness.transfer.isActive());

  EXPECT_TRUE(fileEquals(harness.fs, navigator::kRouteActiveBinPath, golden));
  EXPECT_FALSE(harness.fs.fileExists(navigator::kRouteActiveTempPath));
  EXPECT_FALSE(harness.fs.fileExists(navigator::kRouteActiveBackupPath));
}

TEST(RouteTransferTest, GoldenFramesAtWriteLength185AcceptInOneChunk) {
  TransferHarness harness;
  const Bytes& golden = goldenPackage();
  const TransferFrames frames = buildTransferFrames(golden, 185);
  ASSERT_EQ(frames.chunks.size(), 1u);

  EXPECT_EQ(harness.feed(frames.start).code, navigator::RouteTransferCode::RouteReady);
  const navigator::RouteTransferStatus progress = harness.feed(frames.chunks[0]);
  EXPECT_EQ(progress.code, navigator::RouteTransferCode::RouteProgress);
  EXPECT_EQ(progress.received, 90u);
  const navigator::RouteTransferStatus commit = harness.feed(frames.commit);
  EXPECT_EQ(commit.code, navigator::RouteTransferCode::RouteAccepted);
  EXPECT_EQ(commit.received, 90u);
  EXPECT_TRUE(fileEquals(harness.fs, navigator::kRouteActiveBinPath, golden));
}

TEST(RouteTransferTest, GoldenFramesAtWriteLength512AcceptInOneChunk) {
  TransferHarness harness;
  const Bytes& golden = goldenPackage();
  const TransferFrames frames = buildTransferFrames(golden, 512);
  ASSERT_EQ(frames.chunks.size(), 1u);
  EXPECT_EQ(frames.chunks[0].size(), 99u);  // 9 header + 90 payload

  EXPECT_EQ(harness.feed(frames.start).code, navigator::RouteTransferCode::RouteReady);
  EXPECT_EQ(harness.feed(frames.chunks[0]).code, navigator::RouteTransferCode::RouteProgress);
  EXPECT_EQ(harness.feed(frames.commit).code, navigator::RouteTransferCode::RouteAccepted);
  EXPECT_TRUE(fileEquals(harness.fs, navigator::kRouteActiveBinPath, golden));
}

TEST(RouteTransferTest, AcceptedTransferFillsCallerRouteIndex) {
  TransferHarness harness;
  const Bytes& golden = goldenPackage();
  const TransferFrames frames = buildTransferFrames(golden, 20);
  ASSERT_EQ(harness.feedAll(frames).code, navigator::RouteTransferCode::RouteAccepted);

  EXPECT_EQ(harness.out.routeId, 66051u);
  EXPECT_EQ(harness.out.declaredLength, 90u);
  EXPECT_EQ(harness.out.pointCount, 5u);
  EXPECT_EQ(harness.out.segmentCount, 2u);
  EXPECT_EQ(harness.out.maneuverCount, 1u);
  EXPECT_EQ(harness.out.crcOffset, 86u);
}

// ---------------------------------------------------------------------------
// Statuses: exact fields and the seven-byte little-endian status envelope.
// ---------------------------------------------------------------------------

TEST(RouteTransferTest, StatusesSerializeToTheSevenByteEnvelope) {
  const navigator::RouteTransferStatus ready{navigator::RouteTransferCode::RouteReady, 66051u, 0u};
  uint8_t readyBytes[7];
  ready.writeEnvelope(readyBytes);
  const uint8_t expectedReady[7] = {0x21, 0x03, 0x02, 0x01, 0x00, 0x00, 0x00};
  EXPECT_EQ(std::memcmp(readyBytes, expectedReady, 7), 0);

  const navigator::RouteTransferStatus progress{navigator::RouteTransferCode::RouteProgress, 66051u, 11u};
  uint8_t progressBytes[7];
  progress.writeEnvelope(progressBytes);
  const uint8_t expectedProgress[7] = {0x22, 0x03, 0x02, 0x01, 0x00, 0x0B, 0x00};
  EXPECT_EQ(std::memcmp(progressBytes, expectedProgress, 7), 0);

  const navigator::RouteTransferStatus accepted{navigator::RouteTransferCode::RouteAccepted, 66051u, 90u};
  uint8_t acceptedBytes[7];
  accepted.writeEnvelope(acceptedBytes);
  const uint8_t expectedAccepted[7] = {0x23, 0x03, 0x02, 0x01, 0x00, 0x5A, 0x00};
  EXPECT_EQ(std::memcmp(acceptedBytes, expectedAccepted, 7), 0);

  const navigator::RouteTransferStatus invalid{navigator::RouteTransferCode::RouteInvalid, 7u, 3u};
  uint8_t invalidBytes[7];
  invalid.writeEnvelope(invalidBytes);
  const uint8_t expectedInvalid[7] = {0x24, 0x07, 0x00, 0x00, 0x00, 0x03, 0x00};
  EXPECT_EQ(std::memcmp(invalidBytes, expectedInvalid, 7), 0);

  const navigator::RouteTransferStatus storage{navigator::RouteTransferCode::RouteStorageFailed, 9u, 22u};
  uint8_t storageBytes[7];
  storage.writeEnvelope(storageBytes);
  const uint8_t expectedStorage[7] = {0x25, 0x09, 0x00, 0x00, 0x00, 0x16, 0x00};
  EXPECT_EQ(std::memcmp(storageBytes, expectedStorage, 7), 0);
}

TEST(RouteTransferTest, LiveStatusesCarryExactEnvelopeFields) {
  TransferHarness harness;
  const Bytes& golden = goldenPackage();
  const TransferFrames frames = buildTransferFrames(golden, 20);

  uint8_t envelope[7];
  harness.feed(frames.start).writeEnvelope(envelope);
  EXPECT_EQ(std::memcmp(envelope, "\x21\x03\x02\x01\x00\x00\x00", 7), 0);

  harness.feed(frames.chunks[0]).writeEnvelope(envelope);  // 11 bytes staged
  EXPECT_EQ(std::memcmp(envelope, "\x22\x03\x02\x01\x00\x0B\x00", 7), 0);

  harness.feed(frames.chunks[1]).writeEnvelope(envelope);  // 22 bytes staged
  EXPECT_EQ(std::memcmp(envelope, "\x22\x03\x02\x01\x00\x16\x00", 7), 0);

  harness.feed(frames.chunks[2]).writeEnvelope(envelope);  // 33 bytes staged
  EXPECT_EQ(std::memcmp(envelope, "\x22\x03\x02\x01\x00\x21\x00", 7), 0);

  for (size_t i = 3; i < frames.chunks.size(); ++i) {
    harness.feed(frames.chunks[i]);
  }
  harness.feed(frames.commit).writeEnvelope(envelope);
  EXPECT_EQ(std::memcmp(envelope, "\x23\x03\x02\x01\x00\x5A\x00", 7), 0);
}

// ---------------------------------------------------------------------------
// Duplicate idempotence (verified against stored bytes).
// ---------------------------------------------------------------------------

TEST(RouteTransferTest, DuplicateChunkWhollyWithinStagedBytesIsIdempotent) {
  TransferHarness harness;
  const Bytes& golden = goldenPackage();
  const TransferFrames frames = buildTransferFrames(golden, 20);
  ASSERT_EQ(frames.chunks.size(), 9u);

  harness.feed(frames.start);
  harness.feed(frames.chunks[0]);                                               // staged 11
  harness.feed(frames.chunks[1]);                                               // staged 22
  const navigator::RouteTransferStatus third = harness.feed(frames.chunks[2]);  // staged 33
  EXPECT_EQ(third.code, navigator::RouteTransferCode::RouteProgress);
  EXPECT_EQ(third.received, 33u);

  // Re-send chunk 2 (offset 22, wholly inside [0, 33)): identical bytes must be
  // verified against the staged file and reported as progress without growing
  // the staged file.
  const navigator::RouteTransferStatus duplicate = harness.feed(frames.chunks[2]);
  EXPECT_EQ(duplicate.code, navigator::RouteTransferCode::RouteProgress);
  EXPECT_EQ(duplicate.received, 33u);
  EXPECT_TRUE(harness.transfer.isActive());
  EXPECT_EQ(harness.fs.size(navigator::kRouteActiveTempPath), 33u);

  // Re-send the very first chunk too (offset 0).
  EXPECT_EQ(harness.feed(frames.chunks[0]).code, navigator::RouteTransferCode::RouteProgress);
  EXPECT_EQ(harness.fs.size(navigator::kRouteActiveTempPath), 33u);

  for (size_t i = 3; i < frames.chunks.size(); ++i) {
    EXPECT_EQ(harness.feed(frames.chunks[i]).code, navigator::RouteTransferCode::RouteProgress);
  }
  const navigator::RouteTransferStatus commit = harness.feed(frames.commit);
  EXPECT_EQ(commit.code, navigator::RouteTransferCode::RouteAccepted);
  EXPECT_EQ(commit.received, 90u);
  EXPECT_TRUE(fileEquals(harness.fs, navigator::kRouteActiveBinPath, golden));
  EXPECT_EQ(harness.fs.size(navigator::kRouteActiveTempPath), 0u);
}

// ---------------------------------------------------------------------------
// Protocol rejections (all route-invalid; the active session stays usable).
// ---------------------------------------------------------------------------

TEST(RouteTransferTest, RejectsGapBeforeContiguousOffset) {
  TransferHarness harness;
  const Bytes& golden = goldenPackage();
  harness.feed(buildTransferFrames(golden, 185).start);

  const Bytes payload(golden.begin() + 11, golden.begin() + 22);
  const navigator::RouteTransferStatus status =
      harness.feed(makeChunk(66051, 11, payload));  // first chunk must be at 0
  EXPECT_EQ(status.code, navigator::RouteTransferCode::RouteInvalid);
  EXPECT_EQ(harness.transfer.receivedBytes(), 0u);
  EXPECT_TRUE(harness.transfer.isActive());
  EXPECT_EQ(harness.fs.size(navigator::kRouteActiveTempPath), 0u);
}

TEST(RouteTransferTest, RejectsPartialOverlapThatExtendsPastReceived) {
  TransferHarness harness;
  const Bytes& golden = goldenPackage();
  harness.feed(buildTransferFrames(golden, 185).start);

  const Bytes first(golden.begin(), golden.begin() + 11);
  EXPECT_EQ(harness.feed(makeChunk(66051, 0, first)).code, navigator::RouteTransferCode::RouteProgress);

  // Overlaps [0,11) but extends to 16 > 11: neither contiguous nor wholly
  // contained, so it must be rejected.
  const Bytes overlap(golden.begin() + 5, golden.begin() + 16);
  const navigator::RouteTransferStatus status = harness.feed(makeChunk(66051, 5, overlap));
  EXPECT_EQ(status.code, navigator::RouteTransferCode::RouteInvalid);
  EXPECT_EQ(harness.transfer.receivedBytes(), 11u);
  EXPECT_EQ(harness.fs.size(navigator::kRouteActiveTempPath), 11u);
}

TEST(RouteTransferTest, RejectsDuplicateWithDifferingBytes) {
  TransferHarness harness;
  const Bytes& golden = goldenPackage();
  harness.feed(buildTransferFrames(golden, 185).start);

  const Bytes first(golden.begin(), golden.begin() + 11);
  EXPECT_EQ(harness.feed(makeChunk(66051, 0, first)).code, navigator::RouteTransferCode::RouteProgress);

  Bytes corrupted = first;
  corrupted[0] ^= 0xFF;  // same offset/range, different bytes
  const navigator::RouteTransferStatus status = harness.feed(makeChunk(66051, 0, corrupted));
  EXPECT_EQ(status.code, navigator::RouteTransferCode::RouteInvalid);
  EXPECT_EQ(harness.transfer.receivedBytes(), 11u);
  EXPECT_EQ(harness.fs.size(navigator::kRouteActiveTempPath), 11u);
}

TEST(RouteTransferTest, RejectsChunkWithWrongRouteId) {
  TransferHarness harness;
  const Bytes& golden = goldenPackage();
  harness.feed(buildTransferFrames(golden, 185).start);

  const Bytes payload(golden.begin(), golden.begin() + 11);
  const navigator::RouteTransferStatus status = harness.feed(makeChunk(7, 0, payload));
  EXPECT_EQ(status.code, navigator::RouteTransferCode::RouteInvalid);
  EXPECT_EQ(harness.transfer.receivedBytes(), 0u);
  EXPECT_TRUE(harness.transfer.isActive());
}

TEST(RouteTransferTest, RejectsZeroPayloadChunk) {
  TransferHarness harness;
  const Bytes& golden = goldenPackage();
  harness.feed(buildTransferFrames(golden, 185).start);

  const Bytes empty;
  const navigator::RouteTransferStatus status = harness.feed(makeChunk(66051, 0, empty));
  EXPECT_EQ(status.code, navigator::RouteTransferCode::RouteInvalid);
  EXPECT_EQ(harness.transfer.receivedBytes(), 0u);
}

TEST(RouteTransferTest, RejectsOvershootingChunk) {
  TransferHarness harness;
  harness.feed(makeStart(66051, 90, 0x2144DF1Cu));

  const Bytes payload(20, 0xAB);
  const navigator::RouteTransferStatus status = harness.feed(makeChunk(66051, 80, payload));
  EXPECT_EQ(status.code, navigator::RouteTransferCode::RouteInvalid);
  EXPECT_EQ(harness.transfer.receivedBytes(), 0u);
  EXPECT_TRUE(harness.transfer.isActive());
}

TEST(RouteTransferTest, RejectsStartWithLengthBeyond65535) {
  TransferHarness harness;
  const navigator::RouteTransferStatus status = harness.feed(makeStart(66051, 70000, 0x12345678u));
  EXPECT_EQ(status.code, navigator::RouteTransferCode::RouteInvalid);
  EXPECT_FALSE(harness.transfer.isActive());
  EXPECT_FALSE(harness.fs.fileExists(navigator::kRouteActiveTempPath));
}

TEST(RouteTransferTest, RejectsZeroLengthStart) {
  TransferHarness harness;
  const navigator::RouteTransferStatus status = harness.feed(makeStart(66051, 0, 0u));
  EXPECT_EQ(status.code, navigator::RouteTransferCode::RouteInvalid);
  EXPECT_FALSE(harness.transfer.isActive());
}

TEST(RouteTransferTest, RejectsStartWhileActiveAndKeepsOriginalSession) {
  TransferHarness harness;
  const Bytes& golden = goldenPackage();
  const TransferFrames frames = buildTransferFrames(golden, 185);
  harness.feed(frames.start);

  const navigator::RouteTransferStatus second = harness.feed(makeStart(7, 90, 0x12345678u));
  EXPECT_EQ(second.code, navigator::RouteTransferCode::RouteInvalid);
  EXPECT_TRUE(harness.transfer.isActive());
  EXPECT_EQ(harness.transfer.routeId(), 66051u);

  // The original session is unaffected and can still complete.
  EXPECT_EQ(harness.feed(frames.chunks[0]).code, navigator::RouteTransferCode::RouteProgress);
  EXPECT_EQ(harness.feed(frames.commit).code, navigator::RouteTransferCode::RouteAccepted);
}

TEST(RouteTransferTest, RejectsChunkAndCommitWhileIdle) {
  TransferHarness harness;
  const Bytes& golden = goldenPackage();
  const Bytes payload(golden.begin(), golden.begin() + 11);
  EXPECT_EQ(harness.feed(makeChunk(66051, 0, payload)).code, navigator::RouteTransferCode::RouteInvalid);
  EXPECT_EQ(harness.feed(makeCommit(66051)).code, navigator::RouteTransferCode::RouteInvalid);
  EXPECT_FALSE(harness.fs.fileExists(navigator::kRouteActiveTempPath));
}

TEST(RouteTransferTest, RejectsMalformedFrameSizes) {
  TransferHarness harness;
  EXPECT_EQ(harness.feed(Bytes(12, 0x05)).code, navigator::RouteTransferCode::RouteInvalid);  // short START
  EXPECT_EQ(harness.feed(Bytes(14, 0x05)).code, navigator::RouteTransferCode::RouteInvalid);  // long START
  EXPECT_EQ(harness.feed(Bytes(4, 0x07)).code, navigator::RouteTransferCode::RouteInvalid);   // short COMMIT
  EXPECT_EQ(harness.feed(Bytes(6, 0x07)).code, navigator::RouteTransferCode::RouteInvalid);   // long COMMIT
  EXPECT_EQ(harness.feed(Bytes(8, 0x06)).code, navigator::RouteTransferCode::RouteInvalid);   // truncated CHUNK
  EXPECT_EQ(harness.feed(Bytes(3, 0x01)).code, navigator::RouteTransferCode::RouteInvalid);   // unknown opcode
  EXPECT_FALSE(harness.transfer.isActive());
}

TEST(RouteTransferTest, RejectsCommitBeforeCompleteAndFinishesLater) {
  TransferHarness harness;
  const Bytes& golden = goldenPackage();
  const TransferFrames frames = buildTransferFrames(golden, 20);
  harness.feed(frames.start);
  harness.feed(frames.chunks[0]);
  harness.feed(frames.chunks[1]);

  const navigator::RouteTransferStatus early = harness.feed(frames.commit);
  EXPECT_EQ(early.code, navigator::RouteTransferCode::RouteInvalid);
  EXPECT_TRUE(harness.transfer.isActive());
  EXPECT_EQ(harness.transfer.receivedBytes(), 22u);

  for (size_t i = 2; i < frames.chunks.size(); ++i) {
    EXPECT_EQ(harness.feed(frames.chunks[i]).code, navigator::RouteTransferCode::RouteProgress);
  }
  EXPECT_EQ(harness.feed(frames.commit).code, navigator::RouteTransferCode::RouteAccepted);
  EXPECT_TRUE(fileEquals(harness.fs, navigator::kRouteActiveBinPath, golden));
}

// ---------------------------------------------------------------------------
// Failure handling: every failure cleans route.tmp and preserves the previous
// active route.
// ---------------------------------------------------------------------------

TEST(RouteTransferTest, BeginStorageFailurePreservesOldActive) {
  TransferHarness harness;
  const Bytes old = oldActiveBytes();
  harness.fs.seed(navigator::kRouteActiveBinPath, old);
  harness.fs.failNextCreate = true;

  const Bytes& golden = goldenPackage();
  const navigator::RouteTransferStatus status =
      harness.feed(makeStart(66051, static_cast<uint32_t>(golden.size()), 0x2144DF1Cu));
  EXPECT_EQ(status.code, navigator::RouteTransferCode::RouteStorageFailed);
  EXPECT_FALSE(harness.transfer.isActive());
  EXPECT_FALSE(harness.fs.fileExists(navigator::kRouteActiveTempPath));
  EXPECT_TRUE(fileEquals(harness.fs, navigator::kRouteActiveBinPath, old));
}

TEST(RouteTransferTest, ShortWriteCleansTempAndPreservesOldActive) {
  TransferHarness harness;
  const Bytes old = oldActiveBytes();
  harness.fs.seed(navigator::kRouteActiveBinPath, old);

  const Bytes& golden = goldenPackage();
  const TransferFrames frames = buildTransferFrames(golden, 20);
  harness.feed(frames.start);
  harness.feed(frames.chunks[0]);
  harness.feed(frames.chunks[1]);
  harness.fs.failNextAppend = true;

  const navigator::RouteTransferStatus status = harness.feed(frames.chunks[2]);
  EXPECT_EQ(status.code, navigator::RouteTransferCode::RouteStorageFailed);
  EXPECT_FALSE(harness.transfer.isActive());
  EXPECT_FALSE(harness.fs.fileExists(navigator::kRouteActiveTempPath));
  EXPECT_TRUE(fileEquals(harness.fs, navigator::kRouteActiveBinPath, old));
}

TEST(RouteTransferTest, DuplicateVerifyReadStallCleansTempAndPreservesOldActive) {
  TransferHarness harness;
  const Bytes old = oldActiveBytes();
  harness.fs.seed(navigator::kRouteActiveBinPath, old);

  const Bytes& golden = goldenPackage();
  const TransferFrames frames = buildTransferFrames(golden, 20);
  harness.feed(frames.start);
  harness.feed(frames.chunks[0]);
  harness.fs.failReadCall = 1;  // first read (the duplicate verify) stalls

  const navigator::RouteTransferStatus status = harness.feed(frames.chunks[0]);
  EXPECT_EQ(status.code, navigator::RouteTransferCode::RouteStorageFailed);
  EXPECT_FALSE(harness.transfer.isActive());
  EXPECT_FALSE(harness.fs.fileExists(navigator::kRouteActiveTempPath));
  EXPECT_TRUE(fileEquals(harness.fs, navigator::kRouteActiveBinPath, old));
}

TEST(RouteTransferTest, FlushFailureCleansTempAndPreservesOldActive) {
  TransferHarness harness;
  const Bytes old = oldActiveBytes();
  harness.fs.seed(navigator::kRouteActiveBinPath, old);

  const Bytes& golden = goldenPackage();
  const TransferFrames frames = buildTransferFrames(golden, 185);
  harness.feed(frames.start);
  harness.feed(frames.chunks[0]);
  harness.fs.failNextFlush = true;

  const navigator::RouteTransferStatus status = harness.feed(frames.commit);
  EXPECT_EQ(status.code, navigator::RouteTransferCode::RouteStorageFailed);
  EXPECT_FALSE(harness.transfer.isActive());
  EXPECT_FALSE(harness.fs.fileExists(navigator::kRouteActiveTempPath));
  EXPECT_TRUE(fileEquals(harness.fs, navigator::kRouteActiveBinPath, old));
}

TEST(RouteTransferTest, CommitReadStallCleansTempAndPreservesOldActive) {
  TransferHarness harness;
  const Bytes old = oldActiveBytes();
  harness.fs.seed(navigator::kRouteActiveBinPath, old);

  const Bytes& golden = goldenPackage();
  const TransferFrames frames = buildTransferFrames(golden, 185);
  harness.feed(frames.start);
  harness.feed(frames.chunks[0]);
  harness.fs.failReadCall = 1;  // first read of the whole-package CRC pass stalls

  const navigator::RouteTransferStatus status = harness.feed(frames.commit);
  EXPECT_EQ(status.code, navigator::RouteTransferCode::RouteStorageFailed);
  EXPECT_FALSE(harness.transfer.isActive());
  EXPECT_FALSE(harness.fs.fileExists(navigator::kRouteActiveTempPath));
  EXPECT_TRUE(fileEquals(harness.fs, navigator::kRouteActiveBinPath, old));
}

TEST(RouteTransferTest, TransportCrcMismatchCleansTempAndPreservesOldActive) {
  TransferHarness harness;
  const Bytes old = oldActiveBytes();
  harness.fs.seed(navigator::kRouteActiveBinPath, old);

  const Bytes& golden = goldenPackage();
  const Bytes& package = golden;
  const uint32_t length = static_cast<uint32_t>(package.size());
  harness.feed(makeStart(66051, length, 0xDEADBEEFu));  // wrong whole-package CRC
  const TransferFrames honest = buildTransferFrames(golden, 185);
  harness.feed(honest.chunks[0]);

  const navigator::RouteTransferStatus status = harness.feed(honest.commit);
  EXPECT_EQ(status.code, navigator::RouteTransferCode::RouteInvalid);
  EXPECT_FALSE(harness.transfer.isActive());
  EXPECT_FALSE(harness.fs.fileExists(navigator::kRouteActiveTempPath));
  EXPECT_TRUE(fileEquals(harness.fs, navigator::kRouteActiveBinPath, old));
}

TEST(RouteTransferTest, PackageDecodeFailureCleansTempAndPreservesOldActive) {
  TransferHarness harness;
  const Bytes old = oldActiveBytes();
  harness.fs.seed(navigator::kRouteActiveBinPath, old);

  // Corrupt the magic byte but re-declare the START whole-package CRC over the
  // corrupt bytes, so the transport CRC passes and Route Package validation
  // fails.
  const Bytes& golden = goldenPackage();
  Bytes corrupt = golden;
  corrupt[0] = 'Q';
  const uint32_t length = static_cast<uint32_t>(corrupt.size());
  harness.feed(makeStart(66051, length, crc32Of(corrupt.data(), corrupt.size())));
  const TransferFrames frames = buildTransferFrames(corrupt, 185);
  harness.feed(frames.chunks[0]);

  const navigator::RouteTransferStatus status = harness.feed(frames.commit);
  EXPECT_EQ(status.code, navigator::RouteTransferCode::RouteInvalid);
  EXPECT_FALSE(harness.transfer.isActive());
  EXPECT_FALSE(harness.fs.fileExists(navigator::kRouteActiveTempPath));
  EXPECT_TRUE(fileEquals(harness.fs, navigator::kRouteActiveBinPath, old));
}

// ---------------------------------------------------------------------------
// Whole-package transport CRC regressions (correction round).
// ---------------------------------------------------------------------------

TEST(RouteTransferTest, TransportCrcCoversWholePackageIncludingTrailer) {
  TransferHarness harness;
  const Bytes old = oldActiveBytes();
  harness.fs.seed(navigator::kRouteActiveBinPath, old);

  // Regression: declaring the content-only CRC (the internal trailer value
  // 0x4CB0201C) as the START whole-package CRC must now fail the commit CRC
  // pass, because the transport CRC covers the trailer bytes too.
  const Bytes& golden = goldenPackage();
  harness.feed(makeStart(66051, static_cast<uint32_t>(golden.size()), 0x4CB0201Cu));
  const TransferFrames honest = buildTransferFrames(golden, 185);
  harness.feed(honest.chunks[0]);

  const navigator::RouteTransferStatus status = harness.feed(honest.commit);
  EXPECT_EQ(status.code, navigator::RouteTransferCode::RouteInvalid);
  EXPECT_FALSE(harness.transfer.isActive());
  EXPECT_FALSE(harness.fs.fileExists(navigator::kRouteActiveTempPath));
  EXPECT_TRUE(fileEquals(harness.fs, navigator::kRouteActiveBinPath, old));
}

TEST(RouteTransferTest, CorruptTrailerWithRecomputedTransportCrcFailsValidation) {
  TransferHarness harness;
  const Bytes old = oldActiveBytes();
  harness.fs.seed(navigator::kRouteActiveBinPath, old);

  // Corrupt one trailer byte and re-declare the START whole-package CRC over
  // the corrupt bytes: the transport CRC pass succeeds, but Route Package v1
  // validation must still reject the package because its internal content CRC
  // no longer matches the corrupted trailer.
  const Bytes& golden = goldenPackage();
  Bytes corrupt = golden;
  corrupt[corrupt.size() - 1] ^= 0xFF;
  const uint32_t length = static_cast<uint32_t>(corrupt.size());
  harness.feed(makeStart(66051, length, crc32Of(corrupt.data(), corrupt.size())));
  const TransferFrames frames = buildTransferFrames(corrupt, 185);
  harness.feed(frames.chunks[0]);

  const navigator::RouteTransferStatus status = harness.feed(frames.commit);
  EXPECT_EQ(status.code, navigator::RouteTransferCode::RouteInvalid);
  EXPECT_FALSE(harness.transfer.isActive());
  EXPECT_FALSE(harness.fs.fileExists(navigator::kRouteActiveTempPath));
  EXPECT_TRUE(fileEquals(harness.fs, navigator::kRouteActiveBinPath, old));
}

TEST(RouteTransferTest, RejectsPackageWhoseRouteIdDiffersFromStartRouteId) {
  TransferHarness harness;
  const Bytes old = oldActiveBytes();
  harness.fs.seed(navigator::kRouteActiveBinPath, old);

  // A structurally valid package (internal CRC recomputed over its content)
  // whose header route id (7) disagrees with the START route id (66051) must
  // be rejected before publish; route.tmp is cleaned and the previous active
  // route stays usable.
  Bytes mismatched = goldenPackage();
  putU32(mismatched, 12, 7);
  putU32(mismatched, static_cast<uint32_t>(mismatched.size()) - 4,
         crc32Of(mismatched.data(), mismatched.size() - 4));
  const uint32_t length = static_cast<uint32_t>(mismatched.size());
  harness.feed(makeStart(66051, length, crc32Of(mismatched.data(), mismatched.size())));
  EXPECT_EQ(harness.feed(makeChunk(66051, 0, mismatched)).code, navigator::RouteTransferCode::RouteProgress);

  const navigator::RouteTransferStatus status = harness.feed(makeCommit(66051));
  EXPECT_EQ(status.code, navigator::RouteTransferCode::RouteInvalid);
  // The decode itself succeeded (valid whole-file CRC and valid internal CRC);
  // rejection is specifically the START-vs-package route-id mismatch, so the
  // candidate workspace already holds the decoded header before the reject.
  EXPECT_EQ(harness.out.routeId, 7u);
  EXPECT_FALSE(harness.transfer.isActive());
  EXPECT_FALSE(harness.fs.fileExists(navigator::kRouteActiveTempPath));
  EXPECT_TRUE(fileEquals(harness.fs, navigator::kRouteActiveBinPath, old));
}

TEST(RouteTransferTest, FailedPublishOverwritesCandidateButLeavesSeparateActiveIndexUntouched) {
  TransferHarness harness;

  // The caller owns its live ACTIVE RouteIndex separately and never hands it
  // to RouteTransfer; the workspace passed in is a staging/validation buffer.
  navigator::RouteIndex activeIndex{};
  {
    const Bytes& golden = goldenPackage();
    ByteVectorSource source(golden);
    ASSERT_EQ(navigator::validateRoutePackageV1(source, activeIndex), navigator::DecodeStatus::Ok);
    EXPECT_EQ(activeIndex.routeId, 66051u);
  }
  Bytes activeSnapshot(sizeof(navigator::RouteIndex));
  std::memcpy(activeSnapshot.data(), &activeIndex, sizeof(activeIndex));

  // A different valid route (id 7) is transferred through the workspace
  // candidate while publication is made to fail after validation.
  const Bytes old = oldActiveBytes();
  harness.fs.seed(navigator::kRouteActiveBinPath, old);
  const Bytes replacement = buildFlatRoute(7, 5);
  const TransferFrames frames = buildTransferFrames(replacement, 185);
  harness.feed(frames.start);
  harness.feed(frames.chunks[0]);
  harness.fs.failMoveTempToActive = true;

  const navigator::RouteTransferStatus status = harness.feed(frames.commit);
  EXPECT_EQ(status.code, navigator::RouteTransferCode::RouteStorageFailed);

  // The candidate workspace was overwritten by the successful decode even
  // though COMMIT failed at publish (the caller may consume it only after
  // RouteAccepted)...
  EXPECT_EQ(harness.out.routeId, 7u);
  EXPECT_EQ(harness.out.declaredLength, replacement.size());
  // ...while the separately held active index is byte-for-byte untouched.
  EXPECT_EQ(std::memcmp(activeSnapshot.data(), &activeIndex, sizeof(activeIndex)), 0);
  // And the previous active route file is restored.
  EXPECT_TRUE(fileEquals(harness.fs, navigator::kRouteActiveBinPath, old));
  EXPECT_FALSE(harness.fs.fileExists(navigator::kRouteActiveTempPath));
}

TEST(RouteTransferTest, CrcSourceOverReadIsStorageFailure) {
  TransferHarness harness;
  const Bytes old = oldActiveBytes();
  harness.fs.seed(navigator::kRouteActiveBinPath, old);

  const Bytes& golden = goldenPackage();
  const TransferFrames frames = buildTransferFrames(golden, 185);
  harness.feed(frames.start);
  harness.feed(frames.chunks[0]);

  // A commit-CRC source read that reports more bytes than requested (a
  // RouteByteSource contract violation) must be treated as a storage failure
  // before the buffer is consumed or the byte count advances.
  harness.fs.overReportReadCall = 1;  // first read of the whole-package CRC pass
  const navigator::RouteTransferStatus status = harness.feed(frames.commit);
  EXPECT_EQ(status.code, navigator::RouteTransferCode::RouteStorageFailed);
  EXPECT_FALSE(harness.transfer.isActive());
  EXPECT_FALSE(harness.fs.fileExists(navigator::kRouteActiveTempPath));
  EXPECT_TRUE(fileEquals(harness.fs, navigator::kRouteActiveBinPath, old));
}

TEST(RouteTransferTest, BeginQuotaRejectionPreservesOversizedActive) {
  TransferHarness harness;
  // A pre-existing active file that alone already exhausts the directory
  // quota: begin must reject before creating route.tmp and must leave the
  // oversized active file untouched.
  const Bytes oversized(navigator::kRouteStoreQuotaBytes, 0xA5);
  harness.fs.seed(navigator::kRouteActiveBinPath, oversized);

  const Bytes& golden = goldenPackage();
  const navigator::RouteTransferStatus status =
      harness.feed(makeStart(66051, static_cast<uint32_t>(golden.size()), 0x2144DF1Cu));
  EXPECT_EQ(status.code, navigator::RouteTransferCode::RouteStorageFailed);
  EXPECT_FALSE(harness.transfer.isActive());
  EXPECT_FALSE(harness.fs.fileExists(navigator::kRouteActiveTempPath));
  EXPECT_TRUE(fileEquals(harness.fs, navigator::kRouteActiveBinPath, oversized));
}

TEST(RouteTransferTest, FailedBeginRemovesPartialTempBestEffort) {
  TransferHarness harness;
  const Bytes old = oldActiveBytes();
  harness.fs.seed(navigator::kRouteActiveBinPath, old);

  const Bytes& golden = goldenPackage();
  harness.fs.failNextCreateLeavesPartial = true;  // create "fails" with a temp left behind
  const navigator::RouteTransferStatus status =
      harness.feed(makeStart(66051, static_cast<uint32_t>(golden.size()), 0x2144DF1Cu));
  EXPECT_EQ(status.code, navigator::RouteTransferCode::RouteStorageFailed);
  EXPECT_FALSE(harness.transfer.isActive());
  EXPECT_FALSE(harness.fs.fileExists(navigator::kRouteActiveTempPath));
  EXPECT_TRUE(fileEquals(harness.fs, navigator::kRouteActiveBinPath, old));
}

TEST(RouteTransferTest, FailedBeginAbandonsPartialTempEvenWhenStoreCleanupFails) {
  TransferHarness harness;
  const Bytes old = oldActiveBytes();
  harness.fs.seed(navigator::kRouteActiveBinPath, old);

  const Bytes& golden = goldenPackage();
  // The store's own best-effort cleanup of the partial temp fails (remove
  // error), so the receiver must still call abandon() on the failed begin;
  // that second remove attempt clears the leftover route.tmp.
  harness.fs.failNextCreateLeavesPartial = true;
  harness.fs.failNextRemove = true;
  const navigator::RouteTransferStatus status =
      harness.feed(makeStart(66051, static_cast<uint32_t>(golden.size()), 0x2144DF1Cu));
  EXPECT_EQ(status.code, navigator::RouteTransferCode::RouteStorageFailed);
  EXPECT_FALSE(harness.transfer.isActive());
  EXPECT_FALSE(harness.fs.fileExists(navigator::kRouteActiveTempPath));
  EXPECT_TRUE(fileEquals(harness.fs, navigator::kRouteActiveBinPath, old));
}

TEST(RouteTransferTest, FailedPromotionCleansAndRestoresOldActive) {
  TransferHarness harness;
  const Bytes old = oldActiveBytes();
  harness.fs.seed(navigator::kRouteActiveBinPath, old);

  const Bytes& golden = goldenPackage();
  const TransferFrames frames = buildTransferFrames(golden, 185);
  harness.feed(frames.start);
  harness.feed(frames.chunks[0]);
  harness.fs.failMoveTempToActive = true;

  const navigator::RouteTransferStatus status = harness.feed(frames.commit);
  EXPECT_EQ(status.code, navigator::RouteTransferCode::RouteStorageFailed);
  EXPECT_FALSE(harness.transfer.isActive());
  EXPECT_FALSE(harness.fs.fileExists(navigator::kRouteActiveTempPath));
  EXPECT_TRUE(fileEquals(harness.fs, navigator::kRouteActiveBinPath, old));
  EXPECT_FALSE(harness.fs.fileExists(navigator::kRouteActiveBackupPath));
}

TEST(RouteTransferTest, FailedBackupMovePreservesOldActiveAndCleansTemp) {
  TransferHarness harness;
  const Bytes old = oldActiveBytes();
  harness.fs.seed(navigator::kRouteActiveBinPath, old);

  const Bytes& golden = goldenPackage();
  const TransferFrames frames = buildTransferFrames(golden, 185);
  harness.feed(frames.start);
  harness.feed(frames.chunks[0]);
  harness.fs.failMoveActiveToBackup = true;

  const navigator::RouteTransferStatus status = harness.feed(frames.commit);
  EXPECT_EQ(status.code, navigator::RouteTransferCode::RouteStorageFailed);
  EXPECT_FALSE(harness.transfer.isActive());
  EXPECT_FALSE(harness.fs.fileExists(navigator::kRouteActiveTempPath));
  EXPECT_TRUE(fileEquals(harness.fs, navigator::kRouteActiveBinPath, old));
}

// ---------------------------------------------------------------------------
// Larger transfers: bounded reads and a multi-chunk big route.
// ---------------------------------------------------------------------------

TEST(RouteTransferTest, BigRouteMultiChunkTransferReadsStayWithin1024) {
  TransferHarness harness;
  const Bytes big = buildFlatRoute(66051, 3000);  // 12,040 bytes
  ASSERT_GT(big.size(), 1024u);
  const TransferFrames frames = buildTransferFrames(big, 185);
  ASSERT_GT(frames.chunks.size(), 1u);

  const navigator::RouteTransferStatus accepted = harness.feedAll(frames);
  EXPECT_EQ(accepted.code, navigator::RouteTransferCode::RouteAccepted);
  EXPECT_EQ(accepted.received, big.size());
  EXPECT_TRUE(fileEquals(harness.fs, navigator::kRouteActiveBinPath, big));
  EXPECT_LE(harness.fs.maxReadRequested, navigator::kRoutePackageV1WorkBufferBytes);
  EXPECT_GE(harness.fs.readCalls, 10u);  // both CRC and validation streamed
}

TEST(RouteTransferTest, AbortCleansStagedTempAndPreservesOldActive) {
  TransferHarness harness;
  const Bytes old = oldActiveBytes();
  harness.fs.seed(navigator::kRouteActiveBinPath, old);
  const Bytes& golden = goldenPackage();
  const TransferFrames frames = buildTransferFrames(golden, 20);
  harness.feed(frames.start);
  harness.feed(frames.chunks[0]);
  harness.feed(frames.chunks[1]);

  harness.transfer.abortTransfer(harness.store);
  EXPECT_FALSE(harness.transfer.isActive());
  EXPECT_FALSE(harness.fs.fileExists(navigator::kRouteActiveTempPath));
  EXPECT_TRUE(fileEquals(harness.fs, navigator::kRouteActiveBinPath, old));

  // A fresh START can now begin a new transfer.
  EXPECT_EQ(harness.feed(frames.start).code, navigator::RouteTransferCode::RouteReady);
}

// ---------------------------------------------------------------------------
// No-allocation proof across the whole transfer/store/validation path.
// ---------------------------------------------------------------------------

TEST(RouteTransferTest, FullTransferCoreNeverAllocates) {
  TransferHarness harness;
  const Bytes& golden = goldenPackage();
  const TransferFrames frames = buildTransferFrames(golden, 20);

  // Warm the golden fixture and fixture-derived helpers outside the window.
  EXPECT_EQ(harness.feedAll(frames).code, navigator::RouteTransferCode::RouteAccepted);

  const std::size_t before = test_alloc::gAllocationCount;
  for (int round = 0; round < 25; ++round) {
    const navigator::RouteTransferStatus start = harness.transfer.handleFrame(
        frames.start.data(), static_cast<uint32_t>(frames.start.size()), harness.store, harness.out);
    ASSERT_EQ(start.code, navigator::RouteTransferCode::RouteReady) << "round " << round;
    for (const Bytes& chunk : frames.chunks) {
      const navigator::RouteTransferStatus progress =
          harness.transfer.handleFrame(chunk.data(), static_cast<uint32_t>(chunk.size()), harness.store, harness.out);
      ASSERT_EQ(progress.code, navigator::RouteTransferCode::RouteProgress) << "round " << round;
    }
    const navigator::RouteTransferStatus accepted = harness.transfer.handleFrame(
        frames.commit.data(), static_cast<uint32_t>(frames.commit.size()), harness.store, harness.out);
    ASSERT_EQ(accepted.code, navigator::RouteTransferCode::RouteAccepted) << "round " << round;
    ASSERT_FALSE(harness.transfer.isActive());
  }
  EXPECT_EQ(test_alloc::gAllocationCount, before);
  EXPECT_TRUE(fileEquals(harness.fs, navigator::kRouteActiveBinPath, golden));
}

}  // namespace
