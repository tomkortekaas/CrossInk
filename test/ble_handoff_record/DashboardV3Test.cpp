#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <vector>

#include "BleHandoffRecord.h"
#include "DashboardV3.h"

namespace {

void writeU16(std::vector<uint8_t>& bytes, size_t offset, uint16_t value) {
  bytes[offset] = static_cast<uint8_t>(value);
  bytes[offset + 1] = static_cast<uint8_t>(value >> 8U);
}

void writeU32(std::vector<uint8_t>& bytes, size_t offset, uint32_t value) {
  for (uint8_t index = 0; index < 4; ++index) bytes[offset + index] = static_cast<uint8_t>(value >> (index * 8U));
}

void writeU64(std::vector<uint8_t>& bytes, size_t offset, uint64_t value) {
  for (uint8_t index = 0; index < 8; ++index) bytes[offset + index] = static_cast<uint8_t>(value >> (index * 8U));
}

std::vector<uint8_t> minimalSwiftPackage() {
  // Hand-derived from DashboardV3Package: 31 shared + 46 fixed V3 +
  // one empty destination length + 4 CRC = 82 bytes.
  std::vector<uint8_t> bytes(82, 0);
  bytes[0] = 0x58; bytes[1] = 0x33; bytes[2] = 0x44; bytes[3] = 0x50;
  bytes[4] = dashboard::SCHEMA_V2;
  bytes[5] = dashboard::v3::TEMPLATE_DASHBOARD_V3;
  writeU16(bytes, 6, static_cast<uint16_t>(bytes.size()));
  writeU32(bytes, 8, 7);
  writeU64(bytes, 12, 1000);
  writeU64(bytes, 20, 2000);
  bytes[28] = 15; bytes[29] = 7; bytes[30] = 22;
  bytes[31] = dashboard::v3::FORMAT_VERSION;
  bytes[32] = 3;                         // heating known + allowed
  bytes[33] = 17; bytes[34] = 13; bytes[35] = 20;
  bytes[36] = 2; bytes[37] = 8; bytes[38] = 12;
  writeU16(bytes, 39, 395); writeU16(bytes, 41, 1245); writeU16(bytes, 43, 397);
  bytes[45] = 0xC3;                      // earlier rain bucket is low nibble
  writeU16(bytes, 57, 8 * 60 + 15);      // rainStartMinute: the clock of bucket 0
  writeU16(bytes, 59, 39); writeU16(bytes, 61, 186);
  bytes[63] = 0; bytes[64] = 30; bytes[65] = 76; bytes[66] = 83;
  writeU16(bytes, 67, 7850); writeU16(bytes, 69, 10000);
  writeU16(bytes, 71, 24); bytes[73] = 1;
  bytes[74] = 0; bytes[75] = 0; bytes[76] = 0; bytes[77] = 0;
  writeU32(bytes, 78, dashboard::crc32(bytes.data(), 78));
  return bytes;
}

TEST(DashboardV3Decode, ReadsSwiftFieldsAndRainNibbleOrder) {
  const auto bytes = minimalSwiftPackage();
  dashboard::v3::DashboardV3Package package{};
  ASSERT_EQ(dashboard::v3::decodeDashboardV3(bytes.data(), bytes.size(), package), dashboard::Status::Ok);
  EXPECT_EQ(package.templateId, 5);
  EXPECT_EQ(package.packageId, 7u);
  EXPECT_EQ(package.weather.currentCelsius, 17);
  EXPECT_TRUE(package.heatingKnown);
  EXPECT_TRUE(package.heatingAllowed);
  EXPECT_EQ(package.rain[0], 3);
  EXPECT_EQ(package.rain[1], 12);
  EXPECT_EQ(package.rainStartMinute, 8 * 60 + 15);
  EXPECT_EQ(package.traffic.travelMinutes, 39);
  EXPECT_EQ(package.status.steps, 7850);
  EXPECT_EQ(package.unreadTotal, 24);
}

TEST(DashboardV3Decode, ReadsRainStartMinuteAfterThePackedBuckets) {
  auto bytes = minimalSwiftPackage();
  bytes[56] = 0xAB;                  // last packed rain byte: buckets 22 and 23
  writeU16(bytes, 57, 23 * 60 + 59); // rainStartMinute must not bleed into the buckets
  writeU32(bytes, 78, dashboard::crc32(bytes.data(), 78));

  dashboard::v3::DashboardV3Package package{};
  ASSERT_EQ(dashboard::v3::decodeDashboardV3(bytes.data(), bytes.size(), package), dashboard::Status::Ok);
  EXPECT_EQ(package.rain[22], 0x0B);
  EXPECT_EQ(package.rain[23], 0x0A);
  EXPECT_EQ(package.rainStartMinute, 23 * 60 + 59);
  EXPECT_EQ(package.traffic.travelMinutes, 39);
}

TEST(DashboardV3Decode, AcceptsRainStartMinuteSentinelAsUnknown) {
  auto bytes = minimalSwiftPackage();
  writeU16(bytes, 57, UINT16_MAX);
  writeU32(bytes, 78, dashboard::crc32(bytes.data(), 78));

  dashboard::v3::DashboardV3Package package{};
  ASSERT_EQ(dashboard::v3::decodeDashboardV3(bytes.data(), bytes.size(), package), dashboard::Status::Ok);
  EXPECT_EQ(package.rainStartMinute, UINT16_MAX);
}

TEST(DashboardV3Decode, RejectsBadCrcAndTruncation) {
  auto bytes = minimalSwiftPackage();
  dashboard::v3::DashboardV3Package package{};
  bytes[33] ^= 1;
  EXPECT_EQ(dashboard::v3::decodeDashboardV3(bytes.data(), bytes.size(), package), dashboard::Status::InvalidCrc);
  bytes = minimalSwiftPackage();
  EXPECT_NE(dashboard::v3::decodeDashboardV3(bytes.data(), bytes.size() - 1, package), dashboard::Status::Ok);
}

TEST(DashboardV3Decode, RejectsCountsBeyondFixedArraysWithoutChangingOutput) {
  auto bytes = minimalSwiftPackage();
  bytes[74] = 6;
  writeU32(bytes, 78, dashboard::crc32(bytes.data(), 78));
  dashboard::v3::DashboardV3Package package{};
  package.packageId = 99;

  EXPECT_NE(dashboard::v3::decodeDashboardV3(bytes.data(), bytes.size(), package), dashboard::Status::Ok);
  EXPECT_EQ(package.packageId, 99u);
}

TEST(DashboardV3Decode, RejectsReservedHeatingBits) {
  auto bytes = minimalSwiftPackage();
  bytes[32] = 0x80;
  writeU32(bytes, 78, dashboard::crc32(bytes.data(), 78));
  dashboard::v3::DashboardV3Package package{};
  EXPECT_EQ(dashboard::v3::decodeDashboardV3(bytes.data(), bytes.size(), package), dashboard::Status::InvalidArgument);
}

// The format-version rejection is its own status so an old persisted package
// (which heals once the phone sends a fresh one) is not lumped in with a
// corrupt package. The decoder reports the same status for both directions;
// telling "old package" apart from "newer firmware" is the reader's job.
TEST(DashboardV3Decode, RejectsOlderFormatVersionAsUnsupportedVersion) {
  auto bytes = minimalSwiftPackage();
  bytes[31] = dashboard::v3::FORMAT_VERSION - 1;  // the last-known-good package is older
  writeU32(bytes, 78, dashboard::crc32(bytes.data(), 78));
  dashboard::v3::DashboardV3Package package{};

  const dashboard::Status status = dashboard::v3::decodeDashboardV3(bytes.data(), bytes.size(), package);
  EXPECT_EQ(status, dashboard::Status::UnsupportedVersion);
  EXPECT_NE(status, dashboard::Status::InvalidArgument);
}

TEST(DashboardV3Decode, RejectsNewerFormatVersionAsUnsupportedVersion) {
  auto bytes = minimalSwiftPackage();
  bytes[31] = dashboard::v3::FORMAT_VERSION + 1;  // firmware is behind the phone
  writeU32(bytes, 78, dashboard::crc32(bytes.data(), 78));
  dashboard::v3::DashboardV3Package package{};

  EXPECT_EQ(dashboard::v3::decodeDashboardV3(bytes.data(), bytes.size(), package),
            dashboard::Status::UnsupportedVersion);
}

TEST(DashboardV3Decode, AcceptsMessageIcon65AndRejectsIconIdAbove65) {
  auto bytes = minimalSwiftPackage();
  dashboard::v3::DashboardV3Package package{};
  bytes[36] = 65;
  writeU32(bytes, 78, dashboard::crc32(bytes.data(), 78));
  ASSERT_EQ(dashboard::v3::decodeDashboardV3(bytes.data(), bytes.size(), package), dashboard::Status::Ok);
  EXPECT_EQ(package.weather.conditionIconId, 65);

  bytes[36] = 66;
  writeU32(bytes, 78, dashboard::crc32(bytes.data(), 78));
  EXPECT_EQ(dashboard::v3::decodeDashboardV3(bytes.data(), bytes.size(), package), dashboard::Status::InvalidArgument);
}

}  // namespace
