#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <cstring>
#include <string>
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

// The payload formats as they appear on the wire. The compatibility tests use
// these literals rather than dashboard::v3::FORMAT_VERSION so that they pin the
// wire contract itself instead of agreeing with whatever the decoder currently
// calls current: 2 is the format the shipped phone build sends, 3 widened the
// quote id to a little-endian uint16_t, and 1 was never shipped by a phone.
constexpr uint8_t WIRE_FORMAT_NEVER_SHIPPED = 1;
constexpr uint8_t WIRE_FORMAT_V2 = 2;
constexpr uint8_t WIRE_FORMAT_V3 = 3;

// The quote id sits between the fixed V3 head and the three row counts. Format
// 3 widened it from one byte to a little-endian uint16_t, so the counts, the
// destination length and the CRC all sit one byte further along in a format-3
// package. The head is written at literal offsets on purpose: a fixture that
// reused the decoder's own field walk would agree with a decoder bug instead of
// catching it.
constexpr size_t QUOTE_ID_OFFSET = 73;

size_t agendaCountOffset(const std::vector<uint8_t>& bytes) {
  return QUOTE_ID_OFFSET + (bytes[31] >= WIRE_FORMAT_V3 ? 2 : 1);
}

// Rewrites the declared length and the CRC trailer. peekPackageHeader rejects a
// package whose declared length is not its actual size, so every fixture
// mutation that changes the size has to reseal it.
void sealPackage(std::vector<uint8_t>& bytes) {
  writeU16(bytes, 6, static_cast<uint16_t>(bytes.size()));
  const size_t offset = bytes.size() - dashboard::CRC_SIZE;
  writeU32(bytes, offset, dashboard::crc32(bytes.data(), offset));
}

// Appends one agenda row in front of the CRC and bumps the agenda count, so the
// bytes behind the quote id carry a length and content that a decoder which
// mis-read the id width cannot reproduce by accident.
void appendAgendaRow(std::vector<uint8_t>& bytes, const char* title, const char* detail) {
  bytes.resize(bytes.size() - dashboard::CRC_SIZE);
  bytes.push_back(0);  // dayOffset
  const size_t minuteOffset = bytes.size();
  bytes.resize(minuteOffset + 2);
  writeU16(bytes, minuteOffset, 9 * 60);  // minuteOfDay
  for (const char* field : {title, detail}) {
    const size_t length = std::strlen(field);
    bytes.push_back(static_cast<uint8_t>(length));
    bytes.insert(bytes.end(), field, field + length);
  }
  bytes[agendaCountOffset(bytes)] = static_cast<uint8_t>(bytes[agendaCountOffset(bytes)] + 1);
  bytes.resize(bytes.size() + dashboard::CRC_SIZE, 0);
  sealPackage(bytes);
}

std::vector<uint8_t> minimalSwiftPackage(const uint8_t formatVersion = dashboard::v3::FORMAT_VERSION,
                                         const uint16_t quoteId = 1) {
  // Hand-derived from DashboardV3Package: 31 shared + 42 fixed V3 fields up to
  // and including unreadTotal + the quote id (one byte in format 2, two in
  // format 3) + the three counts + one empty destination length + 4 CRC.
  const size_t quoteIdWidth = formatVersion >= WIRE_FORMAT_V3 ? 2 : 1;
  std::vector<uint8_t> bytes(81 + quoteIdWidth, 0);
  bytes[0] = 0x58; bytes[1] = 0x33; bytes[2] = 0x44; bytes[3] = 0x50;
  bytes[4] = dashboard::SCHEMA_V2;
  bytes[5] = dashboard::v3::TEMPLATE_DASHBOARD_V3;
  writeU16(bytes, 6, static_cast<uint16_t>(bytes.size()));
  writeU32(bytes, 8, 7);
  writeU64(bytes, 12, 1000);
  writeU64(bytes, 20, 2000);
  bytes[28] = 15; bytes[29] = 7; bytes[30] = 22;
  bytes[31] = formatVersion;
  bytes[32] = 3;                         // heating known + allowed
  bytes[33] = 17; bytes[34] = 13; bytes[35] = 20;
  bytes[36] = 2; bytes[37] = 8; bytes[38] = 12;
  writeU16(bytes, 39, 395); writeU16(bytes, 41, 1245); writeU16(bytes, 43, 397);
  bytes[45] = 0xC3;                      // earlier rain bucket is low nibble
  writeU16(bytes, 57, 8 * 60 + 15);      // rainStartMinute: the clock of bucket 0
  writeU16(bytes, 59, 39); writeU16(bytes, 61, 186);
  bytes[63] = 0; bytes[64] = 30; bytes[65] = 76; bytes[66] = 83;
  writeU16(bytes, 67, 7850); writeU16(bytes, 69, 10000);
  writeU16(bytes, 71, 24);
  bytes[QUOTE_ID_OFFSET] = static_cast<uint8_t>(quoteId & 0xFFU);
  if (quoteIdWidth == 2) bytes[QUOTE_ID_OFFSET + 1] = static_cast<uint8_t>(quoteId >> 8U);
  sealPackage(bytes);
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
  EXPECT_EQ(package.quoteId, 1);
}

TEST(DashboardV3Decode, ReadsRainStartMinuteAfterThePackedBuckets) {
  auto bytes = minimalSwiftPackage();
  bytes[56] = 0xAB;                  // last packed rain byte: buckets 22 and 23
  writeU16(bytes, 57, 23 * 60 + 59); // rainStartMinute must not bleed into the buckets
  sealPackage(bytes);

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
  sealPackage(bytes);

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
  bytes[agendaCountOffset(bytes)] = 6;
  sealPackage(bytes);
  dashboard::v3::DashboardV3Package package{};
  package.packageId = 99;

  EXPECT_NE(dashboard::v3::decodeDashboardV3(bytes.data(), bytes.size(), package), dashboard::Status::Ok);
  EXPECT_EQ(package.packageId, 99u);
}

TEST(DashboardV3Decode, RejectsReservedHeatingBits) {
  auto bytes = minimalSwiftPackage();
  bytes[32] = 0x80;
  sealPackage(bytes);
  dashboard::v3::DashboardV3Package package{};
  EXPECT_EQ(dashboard::v3::decodeDashboardV3(bytes.data(), bytes.size(), package), dashboard::Status::InvalidArgument);
}

// The format-version rejection is its own status so an old persisted package
// (which heals once the phone sends a fresh one) is not lumped in with a
// corrupt package. The decoder reports the same status for both directions;
// telling "old package" apart from "newer firmware" is the reader's job.
TEST(DashboardV3Decode, RejectsAFormatOlderThanTheOldestSupportedAsUnsupportedVersion) {
  auto bytes = minimalSwiftPackage(WIRE_FORMAT_NEVER_SHIPPED);  // no phone ever sent format 1
  dashboard::v3::DashboardV3Package package{};

  const dashboard::Status status = dashboard::v3::decodeDashboardV3(bytes.data(), bytes.size(), package);
  EXPECT_EQ(status, dashboard::Status::UnsupportedVersion);
  EXPECT_NE(status, dashboard::Status::InvalidArgument);
}

TEST(DashboardV3Decode, RejectsNewerFormatVersionAsUnsupportedVersion) {
  auto bytes = minimalSwiftPackage();
  bytes[31] = dashboard::v3::FORMAT_VERSION + 1;  // firmware is behind the phone
  sealPackage(bytes);
  dashboard::v3::DashboardV3Package package{};

  EXPECT_EQ(dashboard::v3::decodeDashboardV3(bytes.data(), bytes.size(), package),
            dashboard::Status::UnsupportedVersion);
}

TEST(DashboardV3Decode, AcceptsMessageIcon65AndRejectsIconIdAbove65) {
  auto bytes = minimalSwiftPackage();
  dashboard::v3::DashboardV3Package package{};
  bytes[36] = 65;
  sealPackage(bytes);
  ASSERT_EQ(dashboard::v3::decodeDashboardV3(bytes.data(), bytes.size(), package), dashboard::Status::Ok);
  EXPECT_EQ(package.weather.conditionIconId, 65);

  bytes[36] = 66;
  sealPackage(bytes);
  EXPECT_EQ(dashboard::v3::decodeDashboardV3(bytes.data(), bytes.size(), package), dashboard::Status::InvalidArgument);
}

// --- Payload format compatibility -----------------------------------------
//
// The phone build already in the field sends format 2 with a one-byte quote id,
// and such a package can still be sitting in a device's cache when this
// firmware lands, so both shipped formats have to decode. The literal version
// numbers in these fixtures are deliberate: they pin the wire contract rather
// than the decoder's own constants.

TEST(DashboardV3Decode, DecodesFormat2OneByteQuoteIdWithoutShiftingTheFollowingCounts) {
  auto bytes = minimalSwiftPackage(WIRE_FORMAT_V2, 200);
  appendAgendaRow(bytes, "TANDARTS", "10:15");

  dashboard::v3::DashboardV3Package package{};
  ASSERT_EQ(dashboard::v3::decodeDashboardV3(bytes.data(), bytes.size(), package), dashboard::Status::Ok);
  EXPECT_EQ(package.quoteId, 200);
  ASSERT_EQ(package.agendaCount, 1);
  EXPECT_EQ(std::string(reinterpret_cast<const char*>(package.agenda[0].title.data()), package.agenda[0].titleLength),
            "TANDARTS");
  EXPECT_EQ(package.agenda[0].detailLength, 5);
  EXPECT_EQ(package.marketCount, 0);
  EXPECT_EQ(package.chatCount, 0);
  EXPECT_EQ(package.traffic.travelMinutes, 39) << "the head in front of the id still decodes";
}

TEST(DashboardV3Decode, DecodesFormat3LittleEndianQuoteId) {
  // 312 is the first id a single byte cannot carry, so it is the id that proves
  // the widening instead of the fixture's own byte order.
  auto bytes = minimalSwiftPackage(WIRE_FORMAT_V3, 312);
  EXPECT_EQ(bytes[QUOTE_ID_OFFSET], 0x38) << "312 goes out low byte first";
  EXPECT_EQ(bytes[QUOTE_ID_OFFSET + 1], 0x01);

  dashboard::v3::DashboardV3Package package{};
  ASSERT_EQ(dashboard::v3::decodeDashboardV3(bytes.data(), bytes.size(), package), dashboard::Status::Ok);
  EXPECT_EQ(package.quoteId, 312);
  EXPECT_EQ(package.unreadTotal, 24);
  EXPECT_EQ(package.agendaCount, 0);
}

TEST(DashboardV3Decode, Format3RowsAndCountsLineUpBehindTheWidenedQuoteId) {
  auto bytes = minimalSwiftPackage(WIRE_FORMAT_V3, 364);
  appendAgendaRow(bytes, "TANDARTS", "10:15");

  dashboard::v3::DashboardV3Package package{};
  ASSERT_EQ(dashboard::v3::decodeDashboardV3(bytes.data(), bytes.size(), package), dashboard::Status::Ok);
  EXPECT_EQ(package.quoteId, 364);
  ASSERT_EQ(package.agendaCount, 1);
  EXPECT_EQ(package.agenda[0].minuteOfDay, 9 * 60);
}

}  // namespace
