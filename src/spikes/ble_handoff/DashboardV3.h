#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "BleHandoffRecord.h"

namespace dashboard::v3 {

constexpr uint8_t TEMPLATE_DASHBOARD_V3 = 5;
constexpr uint8_t FORMAT_VERSION = 1;
constexpr size_t RAIN_BUCKET_COUNT = 24;
// Row ceilings, mirroring DashboardV3PackageLayout in the iOS repository. They
// are not typical counts: the phone's composer fits real content under its
// 512-byte target, and the renderer draws only as many rows as the band has
// height for. Raising a ceiling is backwards compatible in this direction —
// the sections are count-prefixed, so a phone still sending fewer rows decodes
// unchanged.
constexpr size_t MAX_AGENDA_ROWS = 8;
constexpr size_t MAX_MARKETS = 3;
constexpr size_t MAX_CHATS = 7;
constexpr size_t MAX_DESTINATION_BYTES = 16;
constexpr size_t MAX_AGENDA_TITLE_BYTES = 32;
constexpr size_t MAX_AGENDA_DETAIL_BYTES = 24;
constexpr size_t MAX_MARKET_LABEL_BYTES = 12;
constexpr size_t MAX_CHAT_NAME_BYTES = 16;

struct Weather {
  int8_t currentCelsius = INT8_MIN;
  int8_t minimumCelsius = INT8_MIN;
  int8_t maximumCelsius = INT8_MIN;
  uint8_t conditionIconId = 0;
  uint8_t windKilometersPerHour = UINT8_MAX;
  uint8_t windDirection = UINT8_MAX;
  uint16_t sunriseTodayMinute = UINT16_MAX;
  uint16_t sunsetTodayMinute = UINT16_MAX;
  uint16_t sunriseTomorrowMinute = UINT16_MAX;
};

struct Traffic {
  uint16_t travelMinutes = UINT16_MAX;
  uint16_t nationalCongestionKilometers = UINT16_MAX;
  uint8_t classification = UINT8_MAX;
  std::array<uint8_t, MAX_DESTINATION_BYTES> destination{};
  uint8_t destinationLength = 0;
};

struct StatusValues {
  uint8_t x3Battery = UINT8_MAX;
  uint8_t vehicleBattery = UINT8_MAX;
  uint8_t homeBattery = UINT8_MAX;
  uint16_t steps = UINT16_MAX;
  uint16_t stepGoal = UINT16_MAX;
};

struct AgendaRow {
  uint8_t dayOffset = 0;
  uint16_t minuteOfDay = 0;
  std::array<uint8_t, MAX_AGENDA_TITLE_BYTES> title{};
  uint8_t titleLength = 0;
  std::array<uint8_t, MAX_AGENDA_DETAIL_BYTES> detail{};
  uint8_t detailLength = 0;
};

struct MarketRow {
  std::array<uint8_t, MAX_MARKET_LABEL_BYTES> label{};
  uint8_t labelLength = 0;
  int16_t changeBasisPoints = INT16_MIN;
};

struct ChatRow {
  std::array<uint8_t, MAX_CHAT_NAME_BYTES> name{};
  uint8_t nameLength = 0;
  uint16_t unreadCount = 0;
  uint16_t lastMessageMinuteOfDay = 0;
};

struct DashboardV3Package {
  uint8_t schema = SCHEMA_V2;
  uint8_t templateId = TEMPLATE_DASHBOARD_V3;
  uint32_t packageId = 0;
  uint64_t generatedAt = 0;
  uint64_t validUntil = 0;
  uint8_t refreshIntervalMinutes = 0;
  uint8_t wakeWindowStartHour = 0;
  uint8_t wakeWindowEndHour = 0;
  Weather weather{};
  std::array<uint8_t, RAIN_BUCKET_COUNT> rain{};
  bool heatingKnown = false;
  bool heatingAllowed = false;
  /// Whether the rain forecast is known at all. Twenty-four zeroes is a valid
  /// forecast — two dry hours — so absence cannot be read from the buckets.
  bool rainKnown = true;
  Traffic traffic{};
  StatusValues status{};
  uint16_t unreadTotal = 0;
  uint8_t quoteId = 0;
  std::array<AgendaRow, MAX_AGENDA_ROWS> agenda{};
  std::array<MarketRow, MAX_MARKETS> markets{};
  std::array<ChatRow, MAX_CHATS> chats{};
  uint8_t agendaCount = 0;
  uint8_t marketCount = 0;
  uint8_t chatCount = 0;
  uint32_t crc = 0;
};

Status decodeDashboardV3(const uint8_t* bytes, size_t size, DashboardV3Package& output);

}  // namespace dashboard::v3
