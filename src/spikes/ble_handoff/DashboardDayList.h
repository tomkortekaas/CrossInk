#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "BleHandoffRecord.h"

namespace dashboard {

// A second dashboard template alongside TEMPLATE_AGENDA: a bounded list of
// today's remaining appointments instead of a single next-event card. Shares
// the agenda template's 28-byte common prefix (magic/schema/template/length/
// packageId/generatedAt/validUntil) and CRC trailer; only the content
// between those is shaped differently, so a template-agnostic reader can
// still peek byte 5 to route to the right decoder.
constexpr uint8_t TEMPLATE_DAY_LIST = 2;
constexpr size_t MAX_DAY_ENTRIES = 6;
constexpr size_t MAX_DAY_HEADING_SIZE = 32;
constexpr size_t MAX_DAY_ENTRY_TIME_SIZE = 16;
constexpr size_t MAX_DAY_ENTRY_LABEL_SIZE = 40;

struct DayEntry {
  std::array<uint8_t, MAX_DAY_ENTRY_TIME_SIZE> timeBytes{};
  uint8_t timeLength = 0;
  std::array<uint8_t, MAX_DAY_ENTRY_LABEL_SIZE> labelBytes{};
  uint8_t labelLength = 0;
};

struct DayListPackage {
  uint8_t schema = SCHEMA_V1;
  uint8_t templateId = TEMPLATE_DAY_LIST;
  uint32_t packageId = 0;
  uint64_t generatedAt = 0;
  uint64_t validUntil = 0;
  std::array<uint8_t, MAX_DAY_HEADING_SIZE> headingBytes{};
  uint8_t headingLength = 0;
  std::array<DayEntry, MAX_DAY_ENTRIES> entries{};
  uint8_t entryCount = 0;
  uint32_t crc = 0;
};

Status encodeDayListPackage(const DayListPackage& package, PackageBytes& output, size_t& outputLength);
Status decodeDayListPackage(const uint8_t* bytes, size_t size, DayListPackage& output);

}  // namespace dashboard
