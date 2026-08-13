#include <gtest/gtest.h>

#include <array>

#include "DashboardTransfer.h"

namespace {

std::array<uint8_t, 11> startFrame(uint32_t id, uint16_t length, uint32_t crc) {
  return {1, static_cast<uint8_t>(id), static_cast<uint8_t>(id >> 8), static_cast<uint8_t>(id >> 16),
          static_cast<uint8_t>(id >> 24), static_cast<uint8_t>(length), static_cast<uint8_t>(length >> 8),
          static_cast<uint8_t>(crc), static_cast<uint8_t>(crc >> 8), static_cast<uint8_t>(crc >> 16),
          static_cast<uint8_t>(crc >> 24)};
}

TEST(DashboardTransfer, AssemblesContiguousChunksAndCommits) {
  dashboard::TransferAssembler assembler;
  const auto start = startFrame(7, 4, 0xB63CFBCDU);  // CRC32 of {1,2,3,4}.
  EXPECT_EQ(assembler.accept(start.data(), start.size()).status, dashboard::TransferStatus::Ready);
  const std::array<uint8_t, 9> first = {2, 7, 0, 0, 0, 0, 0, 1, 2};
  EXPECT_EQ(assembler.accept(first.data(), first.size()).status, dashboard::TransferStatus::Progress);
  const std::array<uint8_t, 9> second = {2, 7, 0, 0, 0, 2, 0, 3, 4};
  EXPECT_EQ(assembler.accept(second.data(), second.size()).received, 4U);
  const std::array<uint8_t, 5> commit = {3, 7, 0, 0, 0};
  EXPECT_EQ(assembler.accept(commit.data(), commit.size()).status, dashboard::TransferStatus::Complete);
  EXPECT_EQ(assembler.length(), 4U);
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
  const auto oversized = startFrame(9, 257, 0);
  EXPECT_EQ(assembler.accept(oversized.data(), oversized.size()).status, dashboard::TransferStatus::InvalidFrame);
}

}  // namespace
