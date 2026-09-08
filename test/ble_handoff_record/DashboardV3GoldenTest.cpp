#include <gtest/gtest.h>

#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "BleHandoffRecord.h"
#include "DashboardV3.h"

// The cross-language contract for the dashboard package.
//
// test/fixtures/dashboard_v3_package.bin holds bytes produced by the iOS
// encoder (V3GoldenPackage.swift), which pins their SHA-256 on its side. This
// file decodes the same bytes and checks every field arrives intact.
//
// Before this existed, DashboardV3Test.cpp built its bytes by hand - its own
// comment said "Hand-derived from DashboardV3Package" - so a change to the
// Swift encoder left both test suites green and only the panel wrong. The
// route package already had a shared golden fixture; the dashboard, the daily
// driver, did not.
//
// When this fails, the two sides disagree. Do not edit the expectations here
// to match: regenerate the fixture from the iOS repository and update both
// sides together, or fix whichever side actually moved.
namespace {

using Bytes = std::vector<uint8_t>;

bool readWholeFile(const std::string& path, Bytes& out) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return false;
  out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
  return !in.bad();
}

// Mirrors the loader in test/navigator/RoutePackageV1Test.cpp: ctest runs from
// the build tree, so the path is resolved from __FILE__ with working-directory
// fallbacks rather than assumed.
Bytes loadFixtureBytes(const char* name) {
  std::vector<std::string> candidates;
  const std::string self = __FILE__;
  const size_t marker = self.find("/test/ble_handoff_record/");
  if (marker != std::string::npos) {
    candidates.push_back(self.substr(0, marker) + "/test/fixtures/" + name);
  }
  candidates.push_back("test/fixtures/" + std::string(name));
  candidates.push_back("../../../test/fixtures/" + std::string(name));
  candidates.push_back("../../test/fixtures/" + std::string(name));
  for (const std::string& candidate : candidates) {
    Bytes bytes;
    if (readWholeFile(candidate, bytes)) return bytes;
  }
  ADD_FAILURE() << "could not open fixture " << name;
  return Bytes();
}

const Bytes& goldenBytes() {
  static const Bytes kBytes = loadFixtureBytes("dashboard_v3_package.bin");
  return kBytes;
}

// The decoded text fields are length-prefixed byte arrays, not C strings.
std::string text(const uint8_t* bytes, const uint8_t length) {
  return std::string(reinterpret_cast<const char*>(bytes), length);
}

TEST(DashboardV3Golden, FixtureHasTheLengthTheManifestDeclares) {
  ASSERT_EQ(goldenBytes().size(), 778U) << "fixture replaced without updating this test";
}

TEST(DashboardV3Golden, DecodesTheEnvelopeTheIosEncoderProduced) {
  const Bytes& bytes = goldenBytes();
  ASSERT_FALSE(bytes.empty());
  dashboard::v3::DashboardV3Package package{};

  ASSERT_EQ(dashboard::v3::decodeDashboardV3(bytes.data(), bytes.size(), package), dashboard::Status::Ok);

  EXPECT_EQ(package.schema, dashboard::SCHEMA_V2);
  EXPECT_EQ(package.templateId, dashboard::v3::TEMPLATE_DASHBOARD_V3);
  EXPECT_EQ(package.packageId, 0x01020304U);
  EXPECT_EQ(package.generatedAt, 1757000000ULL);
  EXPECT_EQ(package.validUntil, 1757003600ULL);
  EXPECT_EQ(package.refreshIntervalMinutes, 15);
  EXPECT_EQ(package.wakeWindowStartHour, 7);
  EXPECT_EQ(package.wakeWindowEndHour, 22);
}

TEST(DashboardV3Golden, DecodesWeatherIncludingTheNegativeMinimum) {
  const Bytes& bytes = goldenBytes();
  dashboard::v3::DashboardV3Package package{};
  ASSERT_EQ(dashboard::v3::decodeDashboardV3(bytes.data(), bytes.size(), package), dashboard::Status::Ok);

  EXPECT_EQ(package.weather.currentCelsius, 17);
  // Negative on purpose: the wire carries temperatures as a byte the decoder
  // reinterprets as int8_t, and a fixture with only positive values would not
  // notice that going wrong.
  EXPECT_EQ(package.weather.minimumCelsius, -3);
  EXPECT_EQ(package.weather.maximumCelsius, 20);
  EXPECT_EQ(package.weather.conditionIconId, 2);
  EXPECT_EQ(package.weather.windKilometersPerHour, 8);
  EXPECT_EQ(package.weather.windDirection, 12);
  EXPECT_EQ(package.weather.sunriseTodayMinute, 395);
  EXPECT_EQ(package.weather.sunsetTodayMinute, 1245);
  EXPECT_EQ(package.weather.sunriseTomorrowMinute, 397);
}

TEST(DashboardV3Golden, DecodesRainBucketsInTheAgreedNibbleOrder) {
  const Bytes& bytes = goldenBytes();
  dashboard::v3::DashboardV3Package package{};
  ASSERT_EQ(dashboard::v3::decodeDashboardV3(bytes.data(), bytes.size(), package), dashboard::Status::Ok);

  EXPECT_TRUE(package.rainKnown);
  EXPECT_EQ(package.rainStartMinute, 840);
  // 0,1,2,3 repeating. A swapped nibble order turns this into 1,0,3,2.
  for (size_t index = 0; index < dashboard::v3::RAIN_BUCKET_COUNT; ++index) {
    EXPECT_EQ(package.rain[index], index % 4) << "rain bucket " << index;
  }
}

TEST(DashboardV3Golden, DecodesTrafficAndStatus) {
  const Bytes& bytes = goldenBytes();
  dashboard::v3::DashboardV3Package package{};
  ASSERT_EQ(dashboard::v3::decodeDashboardV3(bytes.data(), bytes.size(), package), dashboard::Status::Ok);

  EXPECT_EQ(text(package.traffic.destination.data(), package.traffic.destinationLength), "Werk............");
  EXPECT_EQ(package.traffic.travelMinutes, 39);
  EXPECT_EQ(package.traffic.nationalCongestionKilometers, 186);

  EXPECT_TRUE(package.heatingKnown);
  EXPECT_TRUE(package.heatingAllowed);

  EXPECT_EQ(package.status.x3Battery, 30);
  EXPECT_EQ(package.status.vehicleBattery, 76);
  EXPECT_EQ(package.status.homeBattery, 83);
  EXPECT_EQ(package.status.steps, 7850);
  EXPECT_EQ(package.status.stepGoal, 10000);

  EXPECT_EQ(package.unreadTotal, 24);
  EXPECT_EQ(package.quoteId, 1);
}

TEST(DashboardV3Golden, DecodesEveryAgendaRowInOrder) {
  const Bytes& bytes = goldenBytes();
  dashboard::v3::DashboardV3Package package{};
  ASSERT_EQ(dashboard::v3::decodeDashboardV3(bytes.data(), bytes.size(), package), dashboard::Status::Ok);

  ASSERT_EQ(package.agendaCount, dashboard::v3::MAX_AGENDA_ROWS);
  for (size_t index = 0; index < package.agendaCount; ++index) {
    const auto& row = package.agenda[index];
    EXPECT_EQ(row.dayOffset, index) << "agenda row " << index;
    EXPECT_EQ(row.minuteOfDay, 480 + index * 60) << "agenda row " << index;
    // Distinct per row, so two rows swapping places fails here rather than
    // encoding identically.
    EXPECT_EQ(text(row.title.data(), row.titleLength), "Afspraak " + std::to_string(index) + std::string(22, '.'));
    EXPECT_EQ(text(row.detail.data(), row.detailLength), "Locatie " + std::to_string(index) + std::string(15, '.'));
  }
}

TEST(DashboardV3Golden, DecodesMarketsIncludingTheNegativeChange) {
  const Bytes& bytes = goldenBytes();
  dashboard::v3::DashboardV3Package package{};
  ASSERT_EQ(dashboard::v3::decodeDashboardV3(bytes.data(), bytes.size(), package), dashboard::Status::Ok);

  ASSERT_EQ(package.marketCount, dashboard::v3::MAX_MARKETS);
  EXPECT_EQ(text(package.markets[0].label.data(), package.markets[0].labelLength), "Markt 0.....");
  EXPECT_EQ(package.markets[0].changeBasisPoints, 125);
  // Signed: a decoder reading this as unsigned yields 65286, not -250.
  EXPECT_EQ(package.markets[1].changeBasisPoints, -250);
  EXPECT_EQ(package.markets[2].changeBasisPoints, 127);
}

TEST(DashboardV3Golden, DecodesEveryChatRowInOrder) {
  const Bytes& bytes = goldenBytes();
  dashboard::v3::DashboardV3Package package{};
  ASSERT_EQ(dashboard::v3::decodeDashboardV3(bytes.data(), bytes.size(), package), dashboard::Status::Ok);

  ASSERT_EQ(package.chatCount, dashboard::v3::MAX_CHATS);
  for (size_t index = 0; index < package.chatCount; ++index) {
    const auto& row = package.chats[index];
    EXPECT_EQ(text(row.name.data(), row.nameLength), "Chat " + std::to_string(index) + std::string(10, '.'));
    EXPECT_EQ(row.unreadCount, index + 1) << "chat row " << index;
    EXPECT_EQ(row.lastMessageMinuteOfDay, 1000 + index) << "chat row " << index;
  }
}

}  // namespace
