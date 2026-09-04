#include <gtest/gtest.h>

#include <array>

#include "DashboardTransfer.h"

namespace {

std::array<uint8_t, 11> startFrame(uint32_t id, uint16_t length, uint32_t crc) {
  return {1,
          static_cast<uint8_t>(id),
          static_cast<uint8_t>(id >> 8),
          static_cast<uint8_t>(id >> 16),
          static_cast<uint8_t>(id >> 24),
          static_cast<uint8_t>(length),
          static_cast<uint8_t>(length >> 8),
          static_cast<uint8_t>(crc),
          static_cast<uint8_t>(crc >> 8),
          static_cast<uint8_t>(crc >> 16),
          static_cast<uint8_t>(crc >> 24)};
}

TEST(DashboardTransfer, AssemblesContiguousChunksAndCommits) {
  dashboard::TransferAssembler assembler;
  const auto start = startFrame(7, 8, 0xB63CFBCDU);  // CRC32 of {1,2,3,4}, followed by that CRC.
  EXPECT_EQ(assembler.accept(start.data(), start.size()).status, dashboard::TransferStatus::Ready);
  const std::array<uint8_t, 9> first = {2, 7, 0, 0, 0, 0, 0, 1, 2};
  EXPECT_EQ(assembler.accept(first.data(), first.size()).status, dashboard::TransferStatus::Progress);
  const std::array<uint8_t, 13> second = {2, 7, 0, 0, 0, 2, 0, 3, 4, 0xCD, 0xFB, 0x3C, 0xB6};
  EXPECT_EQ(assembler.accept(second.data(), second.size()).received, 8U);
  const std::array<uint8_t, 5> commit = {3, 7, 0, 0, 0};
  EXPECT_EQ(assembler.accept(commit.data(), commit.size()).status, dashboard::TransferStatus::Complete);
  EXPECT_EQ(assembler.length(), 8U);
}

TEST(DashboardTransfer, CommitsPackageUsingItsEmbeddedCrc) {
  dashboard::TransferAssembler assembler;
  const std::array<uint8_t, 48> package = {0x58, 0x33, 0x44, 0x50, 0x01, 0x01, 0x30, 0x00, 0x2A, 0x00, 0x00, 0x00,
                                           0xE8, 0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xD0, 0x07, 0x00, 0x00,
                                           0x00, 0x00, 0x00, 0x00, 0x04, 0x05, 0x00, 0x03, 0x4D, 0x65, 0x65, 0x74,
                                           0x31, 0x30, 0x3A, 0x30, 0x30, 0x4F, 0x6C, 0x64, 0xBC, 0xD5, 0x11, 0x06};
  const auto start = startFrame(42, package.size(), 0x0611D5BCU);
  ASSERT_EQ(assembler.accept(start.data(), start.size()).status, dashboard::TransferStatus::Ready);
  std::array<uint8_t, 55> chunk{};
  chunk[0] = 2;
  chunk[1] = 42;
  std::copy(package.begin(), package.end(), chunk.begin() + 7);
  ASSERT_EQ(assembler.accept(chunk.data(), chunk.size()).status, dashboard::TransferStatus::Progress);
  const std::array<uint8_t, 5> commit = {3, 42, 0, 0, 0};
  EXPECT_EQ(assembler.accept(commit.data(), commit.size()).status, dashboard::TransferStatus::Complete);
}

TEST(DashboardTransfer, RejectsCommitBeforeCompleteAndBadOffsets) {
  dashboard::TransferAssembler assembler;
  const auto start = startFrame(7, 4, 0xB63CFBCDU);
  ASSERT_EQ(assembler.accept(start.data(), start.size()).status, dashboard::TransferStatus::Ready);
  const std::array<uint8_t, 8> badOffset = {2, 7, 0, 0, 0, 1, 0, 1};
  EXPECT_EQ(assembler.accept(badOffset.data(), badOffset.size()).status, dashboard::TransferStatus::InvalidFrame);
  const std::array<uint8_t, 5> commit = {3, 7, 0, 0, 0};
  EXPECT_EQ(assembler.accept(commit.data(), commit.size()).status, dashboard::TransferStatus::InvalidFrame);

  const std::array<uint8_t, 12> overflow = {2, 7, 0, 0, 0, 0, 0, 1, 2, 3, 4, 5};
  EXPECT_EQ(assembler.accept(overflow.data(), overflow.size()).status, dashboard::TransferStatus::InvalidFrame);
}

TEST(DashboardTransfer, NewStartResetsAndRejectsIdMismatchOrOverflow) {
  dashboard::TransferAssembler assembler;
  auto start = startFrame(7, 2, 0xB6CC4292U);
  ASSERT_EQ(assembler.accept(start.data(), start.size()).status, dashboard::TransferStatus::Ready);
  start = startFrame(8, 1, 0xD202EF8DU);
  EXPECT_EQ(assembler.accept(start.data(), start.size()).packageId, 8U);
  const std::array<uint8_t, 8> wrongId = {2, 7, 0, 0, 0, 0, 0, 0};
  EXPECT_EQ(assembler.accept(wrongId.data(), wrongId.size()).status, dashboard::TransferStatus::InvalidFrame);
  const auto oversized = startFrame(9, static_cast<uint16_t>(dashboard::MAX_PACKAGE_SIZE + 1), 0);
  EXPECT_EQ(assembler.accept(oversized.data(), oversized.size()).status, dashboard::TransferStatus::InvalidFrame);
}

TEST(DashboardTransfer, NavigationLaunchIsAStandaloneReadyFrame) {
  dashboard::TransferAssembler assembler;
  const std::array<uint8_t, 9> launch = {4, 0x78, 0x56, 0x34, 0x12, 0, 0, 0, 0};
  const dashboard::TransferResult result = assembler.accept(launch.data(), launch.size());
  EXPECT_EQ(result.status, dashboard::TransferStatus::Ready);
  EXPECT_EQ(result.packageId, 0x12345678U);
  EXPECT_TRUE(result.navigationLaunchRequested);
}

TEST(DashboardTransfer, NavigationLaunchIsRejectedDuringAPackage) {
  dashboard::TransferAssembler assembler;
  const auto start = startFrame(7, 8, 0xB63CFBCDU);
  ASSERT_EQ(assembler.accept(start.data(), start.size()).status, dashboard::TransferStatus::Ready);
  const std::array<uint8_t, 9> launch = {4, 9, 0, 0, 0, 0, 0, 0, 0};
  EXPECT_EQ(assembler.accept(launch.data(), launch.size()).status, dashboard::TransferStatus::InvalidFrame);
}

TEST(DashboardTransfer, NavigationLaunchRejectsWrongSizeOrNonzeroFlags) {
  dashboard::TransferAssembler assembler;
  const std::array<uint8_t, 10> oversized = {4, 9, 0, 0, 0, 0, 0, 0, 0, 0};
  EXPECT_EQ(assembler.accept(oversized.data(), oversized.size()).status, dashboard::TransferStatus::InvalidFrame);
  const std::array<uint8_t, 9> unsupportedFlags = {4, 9, 0, 0, 0, 1, 0, 0, 0};
  EXPECT_EQ(assembler.accept(unsupportedFlags.data(), unsupportedFlags.size()).status,
            dashboard::TransferStatus::InvalidFrame);
}

}  // namespace
