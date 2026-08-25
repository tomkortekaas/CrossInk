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
  // Hand-derived from DashboardV3Package: 31 shared + 44 fixed V3 +
  // one empty destination length + 4 CRC = 80 bytes.
  std::vector<uint8_t> bytes(80, 0);
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
  writeU16(bytes, 57, 39); writeU16(bytes, 59, 186);
  bytes[61] = 0; bytes[62] = 30; bytes[63] = 76; bytes[64] = 83;
  writeU16(bytes, 65, 7850); writeU16(bytes, 67, 10000);
  writeU16(bytes, 69, 24); bytes[71] = 1;
  bytes[72] = 0; bytes[73] = 0; bytes[74] = 0; bytes[75] = 0;
  writeU32(bytes, 76, dashboard::crc32(bytes.data(), 76));
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
  EXPECT_EQ(package.traffic.travelMinutes, 39);
  EXPECT_EQ(package.status.steps, 7850);
  EXPECT_EQ(package.unreadTotal, 24);
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
  bytes[72] = 6;
  writeU32(bytes, 76, dashboard::crc32(bytes.data(), 76));
  dashboard::v3::DashboardV3Package package{};
  package.packageId = 99;

  EXPECT_NE(dashboard::v3::decodeDashboardV3(bytes.data(), bytes.size(), package), dashboard::Status::Ok);
  EXPECT_EQ(package.packageId, 99u);
}

TEST(DashboardV3Decode, RejectsReservedHeatingBits) {
  auto bytes = minimalSwiftPackage();
  bytes[32] = 0x80;
  writeU32(bytes, 76, dashboard::crc32(bytes.data(), 76));
  dashboard::v3::DashboardV3Package package{};
  EXPECT_EQ(dashboard::v3::decodeDashboardV3(bytes.data(), bytes.size(), package), dashboard::Status::InvalidArgument);
}

TEST(DashboardV3Decode, AcceptsMessageIcon65AndRejectsIconIdAbove65) {
  auto bytes = minimalSwiftPackage();
  dashboard::v3::DashboardV3Package package{};
  bytes[36] = 65;
  writeU32(bytes, 76, dashboard::crc32(bytes.data(), 76));
  ASSERT_EQ(dashboard::v3::decodeDashboardV3(bytes.data(), bytes.size(), package), dashboard::Status::Ok);
  EXPECT_EQ(package.weather.conditionIconId, 65);

  bytes[36] = 66;
  writeU32(bytes, 76, dashboard::crc32(bytes.data(), 76));
  EXPECT_EQ(dashboard::v3::decodeDashboardV3(bytes.data(), bytes.size(), package), dashboard::Status::InvalidArgument);
}

}  // namespace
