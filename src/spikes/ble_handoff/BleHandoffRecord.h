#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace dashboard {

constexpr size_t MAX_PACKAGE_SIZE = 256;
constexpr size_t FIXED_HEADER_SIZE = 32;
constexpr size_t CRC_SIZE = 4;
constexpr size_t MIN_PACKAGE_SIZE = FIXED_HEADER_SIZE + CRC_SIZE;
constexpr size_t MAX_TITLE_SIZE = 96;
constexpr size_t MAX_TIME_LINE_SIZE = 48;
constexpr size_t MAX_FOOTER_SIZE = 40;
constexpr size_t MAX_STALE_LINE_SIZE = 48;
constexpr uint8_t SCHEMA_V1 = 1;
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
  TextField title{};
  TextField timeLine{};
  TextField footer{};
  TextField staleLine{};
  uint32_t crc = 0;
};

uint32_t crc32(const uint8_t* data, size_t length);
// Rejects embedded NUL/control bytes, overlong or malformed UTF-8, and
// surrogate code points. Shared by every package template's field validation.
Status validateUtf8(const uint8_t* bytes, size_t length);
Status encodePackage(const Package& package, PackageBytes& output, size_t& outputLength);
Status decodePackage(const uint8_t* bytes, size_t size, Package& output);
Status comparePackageId(bool haveCurrent, uint32_t currentId, uint32_t candidateId);

}  // namespace dashboard
