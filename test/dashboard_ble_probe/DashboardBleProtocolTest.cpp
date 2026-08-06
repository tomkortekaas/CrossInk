#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <vector>

#include "DashboardBleProtocol.h"

namespace {

std::vector<uint8_t> makeFrame(const uint32_t messageId, const std::vector<uint8_t>& payload) {
  std::vector<uint8_t> frame = {'X', '3', 'B', 'P', 1,
                                static_cast<uint8_t>(messageId), static_cast<uint8_t>(messageId >> 8),
                                static_cast<uint8_t>(messageId >> 16), static_cast<uint8_t>(messageId >> 24),
                                static_cast<uint8_t>(payload.size()), 0};
  frame.insert(frame.end(), payload.begin(), payload.end());
  const uint32_t crc = probe::crc32(payload.data(), payload.size());
  frame.push_back(static_cast<uint8_t>(crc));
  frame.push_back(static_cast<uint8_t>(crc >> 8));
  frame.push_back(static_cast<uint8_t>(crc >> 16));
  frame.push_back(static_cast<uint8_t>(crc >> 24));
  return frame;
}

}  // namespace

TEST(DashboardBleProtocol, UsesStandardCrc32Vector) {
  const uint8_t bytes[] = {'1', '2', '3', '4', '5', '6', '7', '8', '9'};
  EXPECT_EQ(probe::crc32(bytes, sizeof(bytes)), 0xCBF43926u);
}

TEST(DashboardBleProtocol, AcceptsKnownValidFrameByteForByte) {
  const uint8_t frame[] = {'X', '3', 'B', 'P', 1, 0x78, 0x56, 0x34, 0x12,
                           3,   0,   'a', 'b', 'c', 0xC2, 0x41, 0x24, 0x35};
  const auto result = probe::decodeFrame(frame, sizeof(frame));
  EXPECT_EQ(result.status, probe::Status::Accepted);
  EXPECT_TRUE(result.hasMessageId);
  EXPECT_EQ(result.messageId, 0x12345678u);
  ASSERT_EQ(result.payloadLength, 3);
  EXPECT_EQ(result.payload[0], 'a');
  EXPECT_EQ(result.payload[1], 'b');
  EXPECT_EQ(result.payload[2], 'c');
}

TEST(DashboardBleProtocol, AcceptsPayloadBoundaries) {
  const auto empty = makeFrame(1, {});
  EXPECT_EQ(probe::decodeFrame(empty.data(), empty.size()).status, probe::Status::Accepted);

  const std::vector<uint8_t> maxPayload(48, 'x');
  const auto maximum = makeFrame(2, maxPayload);
  const auto result = probe::decodeFrame(maximum.data(), maximum.size());
  EXPECT_EQ(result.status, probe::Status::Accepted);
  EXPECT_EQ(result.payloadLength, 48);
}

TEST(DashboardBleProtocol, RejectsTruncatedHeaderWithoutMessageId) {
  const uint8_t frame[] = {'X', '3', 'B', 'P', 1, 0x78, 0x56, 0x34};
  const auto result = probe::decodeFrame(frame, sizeof(frame));
  EXPECT_EQ(result.status, probe::Status::InvalidLength);
  EXPECT_FALSE(result.hasMessageId);
}

TEST(DashboardBleProtocol, ExtractsMessageIdFromCompleteHeaderOnLaterFailure) {
  auto frame = makeFrame(0x12345678u, {'a'});
  frame[0] = 'Y';
  const auto result = probe::decodeFrame(frame.data(), frame.size());
  EXPECT_EQ(result.status, probe::Status::BadMagic);
  EXPECT_TRUE(result.hasMessageId);
  EXPECT_EQ(result.messageId, 0x12345678u);
}

TEST(DashboardBleProtocol, RejectsUnsupportedVersion) {
  auto frame = makeFrame(3, {'a'});
  frame[4] = 2;
  EXPECT_EQ(probe::decodeFrame(frame.data(), frame.size()).status, probe::Status::UnsupportedVersion);
}

TEST(DashboardBleProtocol, RejectsPayloadLengthAboveMaximum) {
  auto frame = makeFrame(4, std::vector<uint8_t>(48, 'x'));
  frame[9] = 49;
  EXPECT_EQ(probe::decodeFrame(frame.data(), frame.size()).status, probe::Status::InvalidLength);
}

TEST(DashboardBleProtocol, RejectsExactLengthMismatch) {
  auto frame = makeFrame(5, {'a', 'b'});
  frame.pop_back();
  EXPECT_EQ(probe::decodeFrame(frame.data(), frame.size()).status, probe::Status::InvalidLength);
}

TEST(DashboardBleProtocol, RejectsInvalidUtf8) {
  const auto frame = makeFrame(6, {0xC0, 0xAF});
  EXPECT_EQ(probe::decodeFrame(frame.data(), frame.size()).status, probe::Status::InvalidLength);
}

TEST(DashboardBleProtocol, RejectsBadCrc) {
  auto frame = makeFrame(7, {'h', 'e', 'l', 'l', 'o'});
  frame.back() ^= 1;
  EXPECT_EQ(probe::decodeFrame(frame.data(), frame.size()).status, probe::Status::BadCrc);
}

TEST(DashboardBleProtocol, MapsEveryStatusToWireName) {
  EXPECT_STREQ(probe::statusName(probe::Status::Ready), "ready");
  EXPECT_STREQ(probe::statusName(probe::Status::Accepted), "accepted");
  EXPECT_STREQ(probe::statusName(probe::Status::BadMagic), "bad_magic");
  EXPECT_STREQ(probe::statusName(probe::Status::UnsupportedVersion), "unsupported_version");
  EXPECT_STREQ(probe::statusName(probe::Status::InvalidLength), "invalid_length");
  EXPECT_STREQ(probe::statusName(probe::Status::BadCrc), "bad_crc");
  EXPECT_STREQ(probe::statusName(probe::Status::RenderFailed), "render_failed");
}

TEST(DashboardBleProtocol, FormatsBoundedStatusValues) {
  char value[32];
  ASSERT_TRUE(probe::formatStatus(probe::Status::Ready, false, 0, value, sizeof(value)));
  EXPECT_STREQ(value, "ready");
  ASSERT_TRUE(probe::formatStatus(probe::Status::Accepted, true, 4294967295u, value, sizeof(value)));
  EXPECT_STREQ(value, "accepted:4294967295");
}

TEST(DashboardBleProtocol, RejectsStatusBufferThatIsTooSmall) {
  char value[8] = {};
  EXPECT_FALSE(probe::formatStatus(probe::Status::RenderFailed, true, 7, value, sizeof(value)));
}
