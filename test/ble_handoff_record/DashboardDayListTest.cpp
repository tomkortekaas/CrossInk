#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>

#include "DashboardDayList.h"

namespace {

template <size_t N>
void setField(std::array<uint8_t, N>& bytes, uint8_t& length, const char* value) {
  length = static_cast<uint8_t>(std::strlen(value));
  std::copy_n(reinterpret_cast<const uint8_t*>(value), length, bytes.begin());
}

dashboard::DayEntry entry(const char* time, const char* label) {
  dashboard::DayEntry result{};
  setField(result.timeBytes, result.timeLength, time);
  setField(result.labelBytes, result.labelLength, label);
  return result;
}

dashboard::DayListPackage validPackage() {
  dashboard::DayListPackage package{};
  package.packageId = 7;
  package.generatedAt = 1000;
  package.validUntil = 2000;
  setField(package.headingBytes, package.headingLength, "Vandaag");
  package.entries[0] = entry("09:00", "Stand-up");
  package.entries[1] = entry("14:00", "Tandarts");
  package.entryCount = 2;
  return package;
}

}  // namespace

TEST(DashboardDayList, EncodesAndRoundTrips) {
  dashboard::PackageBytes bytes{};
  size_t length = 0;
  ASSERT_EQ(dashboard::encodeDayListPackage(validPackage(), bytes, length), dashboard::Status::Ok);
  EXPECT_EQ(bytes[5], dashboard::TEMPLATE_DAY_LIST);

  dashboard::DayListPackage decoded{};
  ASSERT_EQ(dashboard::decodeDayListPackage(bytes.data(), length, decoded), dashboard::Status::Ok);
  EXPECT_EQ(decoded.packageId, 7U);
  EXPECT_EQ(decoded.generatedAt, 1000U);
  EXPECT_EQ(decoded.validUntil, 2000U);
  EXPECT_EQ(decoded.headingLength, 7U);
  ASSERT_EQ(decoded.entryCount, 2U);
  EXPECT_EQ(decoded.entries[0].timeLength, 5U);
  EXPECT_TRUE(std::equal(decoded.entries[0].timeBytes.begin(), decoded.entries[0].timeBytes.begin() + 5,
                        reinterpret_cast<const uint8_t*>("09:00")));
  EXPECT_TRUE(std::equal(decoded.entries[0].labelBytes.begin(), decoded.entries[0].labelBytes.begin() + 8,
                        reinterpret_cast<const uint8_t*>("Stand-up")));
  EXPECT_TRUE(std::equal(decoded.entries[1].timeBytes.begin(), decoded.entries[1].timeBytes.begin() + 5,
                        reinterpret_cast<const uint8_t*>("14:00")));
}

TEST(DashboardDayList, AllowsZeroEntriesAsNoAppointmentsCard) {
  auto package = validPackage();
  package.entryCount = 0;
  dashboard::PackageBytes bytes{};
  size_t length = 0;
  ASSERT_EQ(dashboard::encodeDayListPackage(package, bytes, length), dashboard::Status::Ok);

  dashboard::DayListPackage decoded{};
  ASSERT_EQ(dashboard::decodeDayListPackage(bytes.data(), length, decoded), dashboard::Status::Ok);
  EXPECT_EQ(decoded.entryCount, 0U);
}

TEST(DashboardDayList, RejectsTooManyEntries) {
  auto package = validPackage();
  package.entryCount = dashboard::MAX_DAY_ENTRIES + 1;
  dashboard::PackageBytes bytes{};
  size_t length = 0;
  EXPECT_EQ(dashboard::encodeDayListPackage(package, bytes, length), dashboard::Status::InvalidLength);
}

TEST(DashboardDayList, RejectsEntryWithZeroTimeOrLabelLength) {
  auto package = validPackage();
  package.entries[0].timeLength = 0;
  dashboard::PackageBytes bytes{};
  size_t length = 0;
  EXPECT_EQ(dashboard::encodeDayListPackage(package, bytes, length), dashboard::Status::InvalidLength);

  package = validPackage();
  package.entries[1].labelLength = 0;
  EXPECT_EQ(dashboard::encodeDayListPackage(package, bytes, length), dashboard::Status::InvalidLength);
}

TEST(DashboardDayList, RejectsOversizedPackage) {
  auto package = validPackage();
  for (auto& item : package.entries) {
    item = entry("00:00-23:59", std::string(dashboard::MAX_DAY_ENTRY_LABEL_SIZE, 'a').c_str());
  }
  package.entryCount = dashboard::MAX_DAY_ENTRIES;
  dashboard::PackageBytes bytes{};
  size_t length = 0;
  EXPECT_EQ(dashboard::encodeDayListPackage(package, bytes, length), dashboard::Status::InvalidLength);
}

TEST(DashboardDayList, RejectsInvalidUtf8InLabel) {
  auto package = validPackage();
  package.entries[0].labelBytes[0] = 0xFF;
  package.entries[0].labelLength = 1;
  dashboard::PackageBytes bytes{};
  size_t length = 0;
  EXPECT_EQ(dashboard::encodeDayListPackage(package, bytes, length), dashboard::Status::InvalidUtf8);
}

TEST(DashboardDayList, DecodeRejectsCorruptionAndWrongTemplate) {
  dashboard::PackageBytes bytes{};
  size_t length = 0;
  ASSERT_EQ(dashboard::encodeDayListPackage(validPackage(), bytes, length), dashboard::Status::Ok);
  bytes[length - 1] ^= 1;
  dashboard::DayListPackage decoded{};
  EXPECT_EQ(dashboard::decodeDayListPackage(bytes.data(), length, decoded), dashboard::Status::InvalidCrc);

  ASSERT_EQ(dashboard::encodeDayListPackage(validPackage(), bytes, length), dashboard::Status::Ok);
  bytes[5] = dashboard::TEMPLATE_AGENDA;
  EXPECT_EQ(dashboard::decodeDayListPackage(bytes.data(), length, decoded), dashboard::Status::UnsupportedTemplate);
}

TEST(DashboardDayList, RejectsInvalidTimestamp) {
  auto package = validPackage();
  package.validUntil = package.generatedAt - 1;
  dashboard::PackageBytes bytes{};
  size_t length = 0;
  EXPECT_EQ(dashboard::encodeDayListPackage(package, bytes, length), dashboard::Status::InvalidTimestamp);
}
