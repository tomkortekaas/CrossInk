#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace dashboard {

// Sized so a dashboard can carry a screenful of tiles *and* a full agenda
// rather than forcing a choice between them: three KPI tiles plus six
// maximum-length agenda rows is about 500 bytes. Everything that scales with
// this is static or caller-owned - see BleReceiverMain's frame buffers and
// BleHandoffNvs's SlotRecord workspace - so raising it costs flash-backed RAM,
// not stack. A package still has to cross BLE inside the receiver's 20-second
// window, which at this size takes well under a second.
constexpr size_t MAX_PACKAGE_SIZE = 1024;
constexpr size_t FIXED_HEADER_SIZE = 35;
constexpr size_t CRC_SIZE = 4;
// TEMPLATE_AGENDA's own minimum: its 35-byte fixed header (which includes the
// four text-length bytes at offsets 31..34) plus the CRC trailer.
constexpr size_t MIN_PACKAGE_SIZE = FIXED_HEADER_SIZE + CRC_SIZE;
// The prefix every template shares - magic, schema, templateId, declared
// length, packageId, generatedAt, validUntil, refreshIntervalMinutes,
// wakeWindowStartHour, wakeWindowEndHour - ending where template-specific
// content begins at offset 31. Smaller than FIXED_HEADER_SIZE, so peeking at
// any template's envelope must not use the agenda-only minimum: a widget grid
// carrying no widgets is a valid 38-byte package.
constexpr size_t SHARED_PREFIX_SIZE = 31;
constexpr size_t MIN_ENVELOPE_SIZE = SHARED_PREFIX_SIZE + CRC_SIZE;
constexpr size_t MAX_TITLE_SIZE = 96;
constexpr size_t MAX_TIME_LINE_SIZE = 48;
constexpr size_t MAX_FOOTER_SIZE = 40;
constexpr size_t MAX_STALE_LINE_SIZE = 48;
constexpr uint8_t SCHEMA_V1 = 1;
// TEMPLATE_WIDGET_GRID packages moved to schema 2 to add the global style byte
// and the per-widget style word without disturbing agenda packages, which stay
// on schema 1.
constexpr uint8_t SCHEMA_V2 = 2;
constexpr uint8_t TEMPLATE_AGENDA = 1;

enum class Status : uint8_t {
  Ok,
  InvalidArgument,
  InvalidSize,
  InvalidMagic,
  InvalidSchema,
  UnsupportedSchema,
  UnsupportedTemplate,
  InvalidLength,
  InvalidTimestamp,
  InvalidText,
  InvalidUtf8,
  InvalidCrc,
  StalePackage,
};

using PackageBytes = std::array<uint8_t, MAX_PACKAGE_SIZE>;

struct TextField {
  std::array<uint8_t, MAX_TITLE_SIZE> bytes{};
  uint8_t length = 0;
};

struct Package {
  uint8_t schema = SCHEMA_V1;
  uint8_t templateId = TEMPLATE_AGENDA;
  uint32_t packageId = 0;
  uint64_t generatedAt = 0;
  uint64_t validUntil = 0;
  // Phone-supplied wake schedule, shared by every template. See
  // AgendaWakePolicy.h's clampWakeSettings for how the firmware turns these
  // into a safe interval and window.
  uint8_t refreshIntervalMinutes = 0;
  uint8_t wakeWindowStartHour = 0;
  uint8_t wakeWindowEndHour = 0;
  TextField title{};
  TextField timeLine{};
  TextField footer{};
  TextField staleLine{};
  uint32_t crc = 0;
};

// The fields every template's 28-byte common prefix carries, independent of
// which template-specific decoder (decodePackage, decodeWidgetGridPackage,
// ...) would be needed to read the rest. Lets persistence validate, compare
// package ids, and store any current or future template without knowing its
// content shape.
struct PackageHeader {
  uint8_t schema = SCHEMA_V1;
  uint8_t templateId = 0;
  uint32_t packageId = 0;
  uint64_t generatedAt = 0;
  uint64_t validUntil = 0;
  uint8_t refreshIntervalMinutes = 0;
  uint8_t wakeWindowStartHour = 0;
  uint8_t wakeWindowEndHour = 0;
  uint32_t crc = 0;
};

uint32_t crc32(const uint8_t* data, size_t length);
// Rejects embedded NUL/control bytes, overlong or malformed UTF-8, and
// surrogate code points. Shared by every package template's field validation.
Status validateUtf8(const uint8_t* bytes, size_t length);
Status encodePackage(const Package& package, PackageBytes& output, size_t& outputLength);
Status decodePackage(const uint8_t* bytes, size_t size, Package& output);
// Validates magic, declared length, schema, and CRC, and reads the common
// prefix without decoding template-specific content. An unrecognized
// templateId is not rejected here - only a caller that needs to render or
// otherwise interpret the content decides whether it supports it.
Status peekPackageHeader(const uint8_t* bytes, size_t size, PackageHeader& output);
Status comparePackageId(bool haveCurrent, uint32_t currentId, uint32_t candidateId);

}  // namespace dashboard
