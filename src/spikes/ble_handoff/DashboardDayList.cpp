#include "DashboardDayList.h"

#include <algorithm>

namespace dashboard {
namespace {

constexpr size_t LENGTH_OFFSET = 6;
constexpr size_t PACKAGE_ID_OFFSET = 8;
constexpr size_t GENERATED_AT_OFFSET = 12;
constexpr size_t VALID_UNTIL_OFFSET = 20;
constexpr size_t HEADING_LENGTH_OFFSET = 28;
constexpr size_t ENTRY_COUNT_OFFSET = 29;
constexpr size_t CONTENT_OFFSET = 30;

void writeU16(uint8_t* out, uint16_t value) {
  out[0] = static_cast<uint8_t>(value);
  out[1] = static_cast<uint8_t>(value >> 8U);
}

void writeU32(uint8_t* out, uint32_t value) {
  for (uint8_t index = 0; index < 4; ++index) out[index] = static_cast<uint8_t>(value >> (index * 8U));
}

void writeU64(uint8_t* out, uint64_t value) {
  for (uint8_t index = 0; index < 8; ++index) out[index] = static_cast<uint8_t>(value >> (index * 8U));
}

uint16_t readU16(const uint8_t* in) { return static_cast<uint16_t>(in[0]) | (static_cast<uint16_t>(in[1]) << 8U); }

uint32_t readU32(const uint8_t* in) {
  uint32_t value = 0;
  for (uint8_t index = 0; index < 4; ++index) value |= static_cast<uint32_t>(in[index]) << (index * 8U);
  return value;
}

uint64_t readU64(const uint8_t* in) {
  uint64_t value = 0;
  for (uint8_t index = 0; index < 8; ++index) value |= static_cast<uint64_t>(in[index]) << (index * 8U);
  return value;
}

Status validateFields(const DayListPackage& package) {
  if (package.headingLength > MAX_DAY_HEADING_SIZE || package.entryCount > MAX_DAY_ENTRIES) {
    return Status::InvalidLength;
  }
  if (package.headingLength > 0) {
    const Status status = validateUtf8(package.headingBytes.data(), package.headingLength);
    if (status != Status::Ok) return status;
  }
  for (uint8_t index = 0; index < package.entryCount; ++index) {
    const DayEntry& item = package.entries[index];
    if (item.timeLength == 0 || item.timeLength > MAX_DAY_ENTRY_TIME_SIZE || item.labelLength == 0 ||
        item.labelLength > MAX_DAY_ENTRY_LABEL_SIZE) {
      return Status::InvalidLength;
    }
    Status status = validateUtf8(item.timeBytes.data(), item.timeLength);
    if (status != Status::Ok) return status;
    status = validateUtf8(item.labelBytes.data(), item.labelLength);
    if (status != Status::Ok) return status;
  }
  return Status::Ok;
}

size_t contentLength(const DayListPackage& package) {
  size_t total = package.headingLength;
  for (uint8_t index = 0; index < package.entryCount; ++index) {
    total += 2 + package.entries[index].timeLength + package.entries[index].labelLength;
  }
  return total;
}

}  // namespace

Status encodeDayListPackage(const DayListPackage& package, PackageBytes& output, size_t& outputLength) {
  outputLength = 0;
  if (package.schema != SCHEMA_V1) return Status::UnsupportedSchema;
  if (package.templateId != TEMPLATE_DAY_LIST) return Status::UnsupportedTemplate;
  if (package.generatedAt == 0 || package.validUntil < package.generatedAt) return Status::InvalidTimestamp;
  const Status fieldStatus = validateFields(package);
  if (fieldStatus != Status::Ok) return fieldStatus;

  const size_t totalLength = CONTENT_OFFSET + contentLength(package) + CRC_SIZE;
  if (totalLength > MAX_PACKAGE_SIZE) return Status::InvalidLength;

  output.fill(0);
  output[0] = 'X';
  output[1] = '3';
  output[2] = 'D';
  output[3] = 'P';
  output[4] = package.schema;
  output[5] = package.templateId;
  writeU16(output.data() + LENGTH_OFFSET, static_cast<uint16_t>(totalLength));
  writeU32(output.data() + PACKAGE_ID_OFFSET, package.packageId);
  writeU64(output.data() + GENERATED_AT_OFFSET, package.generatedAt);
  writeU64(output.data() + VALID_UNTIL_OFFSET, package.validUntil);
  output[HEADING_LENGTH_OFFSET] = package.headingLength;
  output[ENTRY_COUNT_OFFSET] = package.entryCount;

  size_t offset = CONTENT_OFFSET;
  std::copy_n(package.headingBytes.begin(), package.headingLength, output.begin() + offset);
  offset += package.headingLength;
  for (uint8_t index = 0; index < package.entryCount; ++index) {
    const DayEntry& item = package.entries[index];
    output[offset] = item.timeLength;
    output[offset + 1] = item.labelLength;
    offset += 2;
    std::copy_n(item.timeBytes.begin(), item.timeLength, output.begin() + offset);
    offset += item.timeLength;
    std::copy_n(item.labelBytes.begin(), item.labelLength, output.begin() + offset);
    offset += item.labelLength;
  }
  writeU32(output.data() + offset, crc32(output.data(), offset));
  outputLength = totalLength;
  return Status::Ok;
}

Status decodeDayListPackage(const uint8_t* bytes, const size_t size, DayListPackage& output) {
  if (bytes == nullptr) return Status::InvalidArgument;
  if (size < CONTENT_OFFSET + CRC_SIZE || size > MAX_PACKAGE_SIZE) return Status::InvalidSize;
  if (bytes[0] != 'X' || bytes[1] != '3' || bytes[2] != 'D' || bytes[3] != 'P') return Status::InvalidMagic;
  if (readU16(bytes + LENGTH_OFFSET) != size) return Status::InvalidSize;
  if (bytes[4] != SCHEMA_V1) return Status::UnsupportedSchema;
  if (bytes[5] != TEMPLATE_DAY_LIST) return Status::UnsupportedTemplate;
  if (crc32(bytes, size - CRC_SIZE) != readU32(bytes + size - CRC_SIZE)) return Status::InvalidCrc;

  DayListPackage candidate{};
  candidate.schema = bytes[4];
  candidate.templateId = bytes[5];
  candidate.packageId = readU32(bytes + PACKAGE_ID_OFFSET);
  candidate.generatedAt = readU64(bytes + GENERATED_AT_OFFSET);
  candidate.validUntil = readU64(bytes + VALID_UNTIL_OFFSET);
  candidate.headingLength = bytes[HEADING_LENGTH_OFFSET];
  candidate.entryCount = bytes[ENTRY_COUNT_OFFSET];
  if (candidate.generatedAt == 0 || candidate.validUntil < candidate.generatedAt) return Status::InvalidTimestamp;
  if (candidate.headingLength > MAX_DAY_HEADING_SIZE || candidate.entryCount > MAX_DAY_ENTRIES) {
    return Status::InvalidLength;
  }

  size_t offset = CONTENT_OFFSET;
  if (offset + candidate.headingLength > size) return Status::InvalidLength;
  std::copy_n(bytes + offset, candidate.headingLength, candidate.headingBytes.begin());
  offset += candidate.headingLength;
  for (uint8_t index = 0; index < candidate.entryCount; ++index) {
    if (offset + 2 > size) return Status::InvalidLength;
    DayEntry& item = candidate.entries[index];
    item.timeLength = bytes[offset];
    item.labelLength = bytes[offset + 1];
    offset += 2;
    if (item.timeLength > MAX_DAY_ENTRY_TIME_SIZE || item.labelLength > MAX_DAY_ENTRY_LABEL_SIZE ||
        offset + item.timeLength + item.labelLength > size) {
      return Status::InvalidLength;
    }
    std::copy_n(bytes + offset, item.timeLength, item.timeBytes.begin());
    offset += item.timeLength;
    std::copy_n(bytes + offset, item.labelLength, item.labelBytes.begin());
    offset += item.labelLength;
  }
  if (offset + CRC_SIZE != size) return Status::InvalidLength;
  const Status fieldStatus = validateFields(candidate);
  if (fieldStatus != Status::Ok) return fieldStatus;
  candidate.crc = readU32(bytes + size - CRC_SIZE);
  output = candidate;
  return Status::Ok;
}

}  // namespace dashboard
