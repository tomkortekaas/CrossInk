#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>

#include "BleHandoffRecord.h"

namespace {

dashboard::TextField text(const char* value) {
  dashboard::TextField field{};
  field.length = static_cast<uint8_t>(std::strlen(value));
  std::copy_n(reinterpret_cast<const uint8_t*>(value), field.length, field.bytes.begin());
  return field;
}

dashboard::Package validPackage() {
  dashboard::Package package{};
  package.packageId = 42;
  package.generatedAt = 1000;
  package.validUntil = 2000;
  package.title = text("Meet");
  package.timeLine = text("10:00");
  package.footer = text("");
  package.staleLine = text("Old");
  return package;
}

TEST(DashboardPackage, UsesStandardCrc32) {
  constexpr std::array<uint8_t, 9> input = {'1', '2', '3', '4', '5', '6', '7', '8', '9'};
  EXPECT_EQ(dashboard::crc32(input.data(), input.size()), 0xCBF43926U);
}

TEST(DashboardPackage, EncodesSpecifiedLittleEndianLayoutAndRoundTrips) {
  dashboard::PackageBytes bytes{};
  size_t length = 0;
  ASSERT_EQ(dashboard::encodePackage(validPackage(), bytes, length), dashboard::Status::Ok);
  ASSERT_EQ(length, 48U);
  const std::array<uint8_t, 44> expectedPrefix = {
      'X', '3', 'D', 'P', 1, 1, 48, 0, 42, 0, 0,   0,   0xE8, 3,   0,   0,   0,   0,   0,   0,   0xD0, 7,
      0,   0,   0,   0,   0, 0, 4,  5, 0,  3, 'M', 'e', 'e',  't', '1', '0', ':', '0', '0', 'O', 'l',  'd'};
  EXPECT_TRUE(std::equal(expectedPrefix.begin(), expectedPrefix.end(), bytes.begin()));

  dashboard::Package decoded{};
  ASSERT_EQ(dashboard::decodePackage(bytes.data(), length, decoded), dashboard::Status::Ok);
  EXPECT_EQ(decoded.packageId, 42U);
  EXPECT_EQ(decoded.generatedAt, 1000U);
  EXPECT_EQ(decoded.validUntil, 2000U);
  EXPECT_EQ(decoded.title.length, 4U);
  EXPECT_TRUE(std::equal(decoded.title.bytes.begin(), decoded.title.bytes.begin() + 4,
                         reinterpret_cast<const uint8_t*>("Meet")));
}

TEST(DashboardPackage, RejectsCorruptionAndInconsistentSize) {
  dashboard::PackageBytes bytes{};
  size_t length = 0;
  ASSERT_EQ(dashboard::encodePackage(validPackage(), bytes, length), dashboard::Status::Ok);
  bytes[32] ^= 1;
  dashboard::Package decoded{};
  EXPECT_EQ(dashboard::decodePackage(bytes.data(), length, decoded), dashboard::Status::InvalidCrc);
  EXPECT_EQ(dashboard::decodePackage(bytes.data(), length - 1, decoded), dashboard::Status::InvalidSize);
}

TEST(DashboardPackage, RejectsInvalidFieldLengthsAndTimestamps) {
  dashboard::PackageBytes bytes{};
  size_t length = 0;
  auto package = validPackage();
  package.title.length = 0;
  EXPECT_EQ(dashboard::encodePackage(package, bytes, length), dashboard::Status::InvalidLength);
  package = validPackage();
  package.generatedAt = 0;
  EXPECT_EQ(dashboard::encodePackage(package, bytes, length), dashboard::Status::InvalidTimestamp);
  package = validPackage();
  package.validUntil = 999;
  EXPECT_EQ(dashboard::encodePackage(package, bytes, length), dashboard::Status::InvalidTimestamp);
}

TEST(DashboardPackage, RejectsNulControlAndMalformedUtf8) {
  dashboard::PackageBytes bytes{};
  size_t length = 0;
  auto package = validPackage();
  package.title.bytes[1] = 0;
  EXPECT_EQ(dashboard::encodePackage(package, bytes, length), dashboard::Status::InvalidText);
  package = validPackage();
  package.title.bytes[1] = '\n';
  EXPECT_EQ(dashboard::encodePackage(package, bytes, length), dashboard::Status::InvalidText);
  package = validPackage();
  package.title.bytes[0] = 0xC3;
  package.title.bytes[1] = 0x28;
  EXPECT_EQ(dashboard::encodePackage(package, bytes, length), dashboard::Status::InvalidUtf8);

  package = validPackage();
  package.title.length = 2;
  package.title.bytes[0] = 0xC0;
  package.title.bytes[1] = 0xAF;
  EXPECT_EQ(dashboard::encodePackage(package, bytes, length), dashboard::Status::InvalidUtf8);

  package = validPackage();
  package.title.length = 3;
  package.title.bytes[0] = 0xED;
  package.title.bytes[1] = 0xA0;
  package.title.bytes[2] = 0x80;
  EXPECT_EQ(dashboard::encodePackage(package, bytes, length), dashboard::Status::InvalidUtf8);
}

TEST(DashboardPackage, DistinguishesUnsupportedSchemaAndTemplate) {
  dashboard::PackageBytes bytes{};
  size_t length = 0;
  auto package = validPackage();
  package.schema = 2;
  EXPECT_EQ(dashboard::encodePackage(package, bytes, length), dashboard::Status::UnsupportedSchema);
  package = validPackage();
  package.templateId = 2;
  EXPECT_EQ(dashboard::encodePackage(package, bytes, length), dashboard::Status::UnsupportedTemplate);
}

TEST(DashboardPackage, RequiresStrictlyIncreasingPackageIds) {
  EXPECT_EQ(dashboard::comparePackageId(false, 99, 1), dashboard::Status::Ok);
  EXPECT_EQ(dashboard::comparePackageId(true, 41, 42), dashboard::Status::Ok);
  EXPECT_EQ(dashboard::comparePackageId(true, 42, 42), dashboard::Status::StalePackage);
  EXPECT_EQ(dashboard::comparePackageId(true, 42, 41), dashboard::Status::StalePackage);
}

void rewriteCrc(dashboard::PackageBytes& bytes, size_t length) {
  const uint32_t crc = dashboard::crc32(bytes.data(), length - dashboard::CRC_SIZE);
  for (size_t i = 0; i < dashboard::CRC_SIZE; ++i) {
    bytes[length - dashboard::CRC_SIZE + i] = static_cast<uint8_t>(crc >> (i * 8));
  }
}

TEST(PackageHeaderPeek, ReadsCommonFieldsWithoutDecodingTemplateContent) {
  dashboard::PackageBytes bytes{};
  size_t length = 0;
  ASSERT_EQ(dashboard::encodePackage(validPackage(), bytes, length), dashboard::Status::Ok);

  dashboard::PackageHeader header{};
  ASSERT_EQ(dashboard::peekPackageHeader(bytes.data(), length, header), dashboard::Status::Ok);
  EXPECT_EQ(header.templateId, dashboard::TEMPLATE_AGENDA);
  EXPECT_EQ(header.packageId, 42U);
  EXPECT_EQ(header.generatedAt, 1000U);
  EXPECT_EQ(header.validUntil, 2000U);
}

TEST(PackageHeaderPeek, AcceptsAnUnrecognizedTemplateIdInsteadOfRejectingIt) {
  dashboard::PackageBytes bytes{};
  size_t length = 0;
  ASSERT_EQ(dashboard::encodePackage(validPackage(), bytes, length), dashboard::Status::Ok);
  bytes[5] = 99;  // a template this decoder does not know how to render
  rewriteCrc(bytes, length);

  dashboard::PackageHeader header{};
  EXPECT_EQ(dashboard::peekPackageHeader(bytes.data(), length, header), dashboard::Status::Ok);
  EXPECT_EQ(header.templateId, 99U);
  EXPECT_EQ(header.packageId, 42U);
}

TEST(PackageHeaderPeek, RejectsBadMagicSchemaAndCrc) {
  dashboard::PackageBytes bytes{};
  size_t length = 0;
  ASSERT_EQ(dashboard::encodePackage(validPackage(), bytes, length), dashboard::Status::Ok);
  dashboard::PackageHeader header{};

  auto corrupted = bytes;
  corrupted[0] = 'Y';
  EXPECT_EQ(dashboard::peekPackageHeader(corrupted.data(), length, header), dashboard::Status::InvalidMagic);

  corrupted = bytes;
  corrupted[4] = 3;  // schema 2 is valid for TEMPLATE_WIDGET_GRID now
  EXPECT_EQ(dashboard::peekPackageHeader(corrupted.data(), length, header), dashboard::Status::UnsupportedSchema);

  corrupted = bytes;
  corrupted[32] ^= 1;
  EXPECT_EQ(dashboard::peekPackageHeader(corrupted.data(), length, header), dashboard::Status::InvalidCrc);
}

TEST(PackageHeaderPeek, RejectsInvalidTimestamps) {
  dashboard::PackageBytes bytes{};
  size_t length = 0;
  ASSERT_EQ(dashboard::encodePackage(validPackage(), bytes, length), dashboard::Status::Ok);
  for (size_t i = 0; i < 8; ++i) bytes[20 + i] = 0;  // validUntil = 0, now below generatedAt
  rewriteCrc(bytes, length);

  dashboard::PackageHeader header{};
  EXPECT_EQ(dashboard::peekPackageHeader(bytes.data(), length, header), dashboard::Status::InvalidTimestamp);
}

}  // namespace
