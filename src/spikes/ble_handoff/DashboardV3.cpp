#include "DashboardV3.h"

#include <algorithm>

namespace dashboard::v3 {
static_assert(sizeof(DashboardV3Package) <= 1024, "V3 decoded package exceeds its static DRAM budget");
namespace {

uint16_t readU16(const uint8_t* bytes) {
  return static_cast<uint16_t>(bytes[0]) | (static_cast<uint16_t>(bytes[1]) << 8U);
}

struct Cursor {
  const uint8_t* bytes;
  size_t offset;
  size_t end;

  bool read8(uint8_t& value) {
    if (offset >= end) return false;
    value = bytes[offset++];
    return true;
  }

  bool read16(uint16_t& value) {
    if (offset + 2 > end) return false;
    value = readU16(bytes + offset);
    offset += 2;
    return true;
  }

  template <size_t Capacity>
  Status readText(std::array<uint8_t, Capacity>& destination, uint8_t& length, bool allowEmpty) {
    uint8_t candidateLength = 0;
    if (!read8(candidateLength) || candidateLength > Capacity || offset + candidateLength > end) {
      return Status::InvalidLength;
    }
    if (!allowEmpty && candidateLength == 0) return Status::InvalidLength;
    const Status textStatus = validateUtf8(bytes + offset, candidateLength);
    if (textStatus != Status::Ok) return textStatus;
    std::copy_n(bytes + offset, candidateLength, destination.begin());
    length = candidateLength;
    offset += candidateLength;
    return Status::Ok;
  }
};

bool validPercent(uint8_t value) { return value == UINT8_MAX || value <= 100; }
bool validMinute(uint16_t value) { return value == UINT16_MAX || value < 1440; }

}  // namespace

Status decodeDashboardV3(const uint8_t* bytes, size_t size, DashboardV3Package& output) {
  PackageHeader header{};
  Status status = peekPackageHeader(bytes, size, header);
  if (status != Status::Ok) return status;
  if (header.schema != SCHEMA_V2) return Status::UnsupportedSchema;
  if (header.templateId != TEMPLATE_DASHBOARD_V3) return Status::UnsupportedTemplate;

  Cursor cursor{bytes, SHARED_PREFIX_SIZE, size - CRC_SIZE};
  // Static rather than a stack local: a decoded V3 package is 816 bytes, more
  // than this task's stack can spare. Decoding stays two-phase - a rejected
  // package must not overwrite the caller's - and the dashboard render path is
  // single-threaded, so one workspace is enough. Host tests won't catch this:
  // the stack is roomy there and it only bites on the C3.
  static DashboardV3Package candidate;
  candidate = {};
  candidate.packageId = header.packageId;
  candidate.generatedAt = header.generatedAt;
  candidate.validUntil = header.validUntil;
  candidate.refreshIntervalMinutes = header.refreshIntervalMinutes;
  candidate.wakeWindowStartHour = header.wakeWindowStartHour;
  candidate.wakeWindowEndHour = header.wakeWindowEndHour;
  candidate.crc = header.crc;

  uint8_t version = 0;
  uint8_t flags = 0;
  uint8_t temperature = 0;
  if (!cursor.read8(version)) return Status::InvalidArgument;
  // A version mismatch is its own status, not InvalidArgument: an older package
  // is a stale cache that heals once the phone sends a fresh one, not a corrupt
  // payload. The decoder reports only "unsupported"; which direction it is
  // (package older vs firmware behind) is the reader's call.
  if (version != FORMAT_VERSION) return Status::UnsupportedVersion;
  if (!cursor.read8(flags) || (flags & 0xF8U) != 0) return Status::InvalidArgument;
  candidate.heatingKnown = (flags & 1U) != 0;
  candidate.heatingAllowed = (flags & 2U) != 0;
  // Bit 2 marks the forecast absent, so a package from a phone build that
  // predates the flag still decodes as known.
  candidate.rainKnown = (flags & 4U) == 0;
  if (!candidate.heatingKnown && candidate.heatingAllowed) return Status::InvalidArgument;
  if (!cursor.read8(temperature)) return Status::InvalidLength;
  candidate.weather.currentCelsius = static_cast<int8_t>(temperature);
  if (!cursor.read8(temperature)) return Status::InvalidLength;
  candidate.weather.minimumCelsius = static_cast<int8_t>(temperature);
  if (!cursor.read8(temperature)) return Status::InvalidLength;
  candidate.weather.maximumCelsius = static_cast<int8_t>(temperature);
  if (!cursor.read8(candidate.weather.conditionIconId) || !cursor.read8(candidate.weather.windKilometersPerHour) ||
      !cursor.read8(candidate.weather.windDirection) || !cursor.read16(candidate.weather.sunriseTodayMinute) ||
      !cursor.read16(candidate.weather.sunsetTodayMinute) || !cursor.read16(candidate.weather.sunriseTomorrowMinute)) {
    return Status::InvalidLength;
  }
  if (candidate.weather.conditionIconId > MAX_CONDITION_ICON_ID ||
      (candidate.weather.windDirection != UINT8_MAX && candidate.weather.windDirection >= 16) ||
      !validMinute(candidate.weather.sunriseTodayMinute) || !validMinute(candidate.weather.sunsetTodayMinute) ||
      !validMinute(candidate.weather.sunriseTomorrowMinute)) return Status::InvalidArgument;

  for (size_t index = 0; index < RAIN_BUCKET_COUNT; index += 2) {
    uint8_t packed = 0;
    if (!cursor.read8(packed)) return Status::InvalidLength;
    candidate.rain[index] = packed & 0x0FU;
    candidate.rain[index + 1] = packed >> 4U;
  }
  if (!cursor.read16(candidate.rainStartMinute) || !cursor.read16(candidate.traffic.travelMinutes) ||
      !cursor.read16(candidate.traffic.nationalCongestionKilometers) ||
      !cursor.read8(candidate.traffic.classification) || !cursor.read8(candidate.status.x3Battery) ||
      !cursor.read8(candidate.status.vehicleBattery) || !cursor.read8(candidate.status.homeBattery) ||
      !cursor.read16(candidate.status.steps) || !cursor.read16(candidate.status.stepGoal) ||
      !cursor.read16(candidate.unreadTotal) || !cursor.read8(candidate.quoteId) ||
      !cursor.read8(candidate.agendaCount) || !cursor.read8(candidate.marketCount) || !cursor.read8(candidate.chatCount)) {
    return Status::InvalidLength;
  }
  if ((candidate.traffic.classification != UINT8_MAX && candidate.traffic.classification > 2) ||
      !validPercent(candidate.status.x3Battery) || !validPercent(candidate.status.vehicleBattery) ||
      !validPercent(candidate.status.homeBattery) || !validMinute(candidate.rainStartMinute) ||
      candidate.agendaCount > MAX_AGENDA_ROWS || candidate.marketCount > MAX_MARKETS ||
      candidate.chatCount > MAX_CHATS) return Status::InvalidArgument;

  status = cursor.readText(candidate.traffic.destination, candidate.traffic.destinationLength, true);
  if (status != Status::Ok) return status;
  for (uint8_t index = 0; index < candidate.agendaCount; ++index) {
    AgendaRow& row = candidate.agenda[index];
    if (!cursor.read8(row.dayOffset) || !cursor.read16(row.minuteOfDay) || row.minuteOfDay >= 1440) {
      return Status::InvalidArgument;
    }
    status = cursor.readText(row.title, row.titleLength, false);
    if (status != Status::Ok) return status;
    status = cursor.readText(row.detail, row.detailLength, true);
    if (status != Status::Ok) return status;
  }
  for (uint8_t index = 0; index < candidate.marketCount; ++index) {
    MarketRow& row = candidate.markets[index];
    status = cursor.readText(row.label, row.labelLength, false);
    uint16_t rawChange = 0;
    if (status != Status::Ok) return status;
    if (!cursor.read16(rawChange)) return Status::InvalidLength;
    row.changeBasisPoints = static_cast<int16_t>(rawChange);
  }
  for (uint8_t index = 0; index < candidate.chatCount; ++index) {
    ChatRow& row = candidate.chats[index];
    status = cursor.readText(row.name, row.nameLength, false);
    if (status != Status::Ok) return status;
    if (!cursor.read16(row.unreadCount) || !cursor.read16(row.lastMessageMinuteOfDay)) return Status::InvalidLength;
    if (row.lastMessageMinuteOfDay >= 1440) return Status::InvalidArgument;
  }
  if (cursor.offset != cursor.end) return Status::InvalidLength;
  output = candidate;
  return Status::Ok;
}

}  // namespace dashboard::v3
