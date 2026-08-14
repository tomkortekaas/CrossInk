#include "BleHandoffRecord.h"

#include <algorithm>

namespace dashboard {
namespace {

constexpr size_t LENGTH_OFFSET = 6;
constexpr size_t PACKAGE_ID_OFFSET = 8;
constexpr size_t GENERATED_AT_OFFSET = 12;
constexpr size_t VALID_UNTIL_OFFSET = 20;
constexpr size_t TITLE_LENGTH_OFFSET = 28;
constexpr size_t TIME_LENGTH_OFFSET = 29;
constexpr size_t FOOTER_LENGTH_OFFSET = 30;
constexpr size_t STALE_LENGTH_OFFSET = 31;

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

}  // namespace

Status validateUtf8(const uint8_t* bytes, size_t length) {
  for (size_t index = 0; index < length;) {
    const uint8_t lead = bytes[index];
    if (lead == 0 || lead < 0x20U || lead == 0x7FU) return Status::InvalidText;
    if (lead < 0x80U) {
      ++index;
      continue;
    }

    size_t continuationCount = 0;
    uint32_t codePoint = 0;
    uint32_t minimum = 0;
    if ((lead & 0xE0U) == 0xC0U) {
      continuationCount = 1;
      codePoint = lead & 0x1FU;
      minimum = 0x80U;
    } else if ((lead & 0xF0U) == 0xE0U) {
      continuationCount = 2;
      codePoint = lead & 0x0FU;
      minimum = 0x800U;
    } else if ((lead & 0xF8U) == 0xF0U) {
      continuationCount = 3;
      codePoint = lead & 0x07U;
      minimum = 0x10000U;
    } else {
      return Status::InvalidUtf8;
    }
    if (index + continuationCount >= length) return Status::InvalidUtf8;
    for (size_t offset = 1; offset <= continuationCount; ++offset) {
      const uint8_t next = bytes[index + offset];
      if ((next & 0xC0U) != 0x80U) return Status::InvalidUtf8;
      codePoint = (codePoint << 6U) | (next & 0x3FU);
    }
    if (codePoint < minimum || codePoint > 0x10FFFFU || (codePoint >= 0xD800U && codePoint <= 0xDFFFU)) {
      return Status::InvalidUtf8;
    }
    index += continuationCount + 1;
  }
  return Status::Ok;
}

namespace {

Status validateFields(const Package& package) {
  if (package.title.length == 0 || package.title.length > MAX_TITLE_SIZE || package.timeLine.length == 0 ||
      package.timeLine.length > MAX_TIME_LINE_SIZE || package.footer.length > MAX_FOOTER_SIZE ||
      package.staleLine.length == 0 || package.staleLine.length > MAX_STALE_LINE_SIZE) {
    return Status::InvalidLength;
  }
  const TextField* fields[] = {&package.title, &package.timeLine, &package.footer, &package.staleLine};
  for (const TextField* field : fields) {
    const Status status = validateUtf8(field->bytes.data(), field->length);
    if (status != Status::Ok) return status;
  }
  return Status::Ok;
}

}  // namespace

uint32_t crc32(const uint8_t* data, const size_t length) {
  if (data == nullptr && length != 0) return 0;
  uint32_t crc = 0xFFFFFFFFU;
  for (size_t index = 0; index < length; ++index) {
    crc ^= data[index];
    for (uint8_t bit = 0; bit < 8; ++bit) crc = (crc >> 1U) ^ (0xEDB88320U & (0U - (crc & 1U)));
  }
  return ~crc;
}

Status encodePackage(const Package& package, PackageBytes& output, size_t& outputLength) {
  outputLength = 0;
  if (package.schema != SCHEMA_V1) return Status::UnsupportedSchema;
  if (package.templateId != TEMPLATE_AGENDA) return Status::UnsupportedTemplate;
  if (package.generatedAt == 0 || package.validUntil < package.generatedAt) return Status::InvalidTimestamp;
  const Status fieldStatus = validateFields(package);
  if (fieldStatus != Status::Ok) return fieldStatus;

  const size_t textLength = package.title.length + package.timeLine.length + package.footer.length + package.staleLine.length;
  const size_t totalLength = FIXED_HEADER_SIZE + textLength + CRC_SIZE;
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
  output[TITLE_LENGTH_OFFSET] = package.title.length;
  output[TIME_LENGTH_OFFSET] = package.timeLine.length;
  output[FOOTER_LENGTH_OFFSET] = package.footer.length;
  output[STALE_LENGTH_OFFSET] = package.staleLine.length;

  size_t offset = FIXED_HEADER_SIZE;
  for (const TextField* field : {&package.title, &package.timeLine, &package.footer, &package.staleLine}) {
    std::copy_n(field->bytes.begin(), field->length, output.begin() + offset);
    offset += field->length;
  }
  writeU32(output.data() + offset, crc32(output.data(), offset));
  outputLength = totalLength;
  return Status::Ok;
}

Status decodePackage(const uint8_t* bytes, const size_t size, Package& output) {
  if (bytes == nullptr) return Status::InvalidArgument;
  if (size < MIN_PACKAGE_SIZE || size > MAX_PACKAGE_SIZE) return Status::InvalidSize;
  if (bytes[0] != 'X' || bytes[1] != '3' || bytes[2] != 'D' || bytes[3] != 'P') return Status::InvalidMagic;
  if (readU16(bytes + LENGTH_OFFSET) != size) return Status::InvalidSize;
  if (bytes[4] != SCHEMA_V1) return Status::UnsupportedSchema;
  if (bytes[5] != TEMPLATE_AGENDA) return Status::UnsupportedTemplate;
  if (crc32(bytes, size - CRC_SIZE) != readU32(bytes + size - CRC_SIZE)) return Status::InvalidCrc;

  Package candidate{};
  candidate.schema = bytes[4];
  candidate.templateId = bytes[5];
  candidate.packageId = readU32(bytes + PACKAGE_ID_OFFSET);
  candidate.generatedAt = readU64(bytes + GENERATED_AT_OFFSET);
  candidate.validUntil = readU64(bytes + VALID_UNTIL_OFFSET);
  candidate.title.length = bytes[TITLE_LENGTH_OFFSET];
  candidate.timeLine.length = bytes[TIME_LENGTH_OFFSET];
  candidate.footer.length = bytes[FOOTER_LENGTH_OFFSET];
  candidate.staleLine.length = bytes[STALE_LENGTH_OFFSET];
  if (candidate.generatedAt == 0 || candidate.validUntil < candidate.generatedAt) return Status::InvalidTimestamp;
  const Status lengthStatus = validateFields(candidate);
  if (lengthStatus == Status::InvalidLength) return lengthStatus;

  const size_t textLength = candidate.title.length + candidate.timeLine.length + candidate.footer.length +
                            candidate.staleLine.length;
  if (FIXED_HEADER_SIZE + textLength + CRC_SIZE != size) return Status::InvalidLength;
  size_t offset = FIXED_HEADER_SIZE;
  for (TextField* field : {&candidate.title, &candidate.timeLine, &candidate.footer, &candidate.staleLine}) {
    std::copy_n(bytes + offset, field->length, field->bytes.begin());
    offset += field->length;
  }
  const Status textStatus = validateFields(candidate);
  if (textStatus != Status::Ok) return textStatus;
  candidate.crc = readU32(bytes + size - CRC_SIZE);
  output = candidate;
  return Status::Ok;
}

Status peekPackageHeader(const uint8_t* bytes, const size_t size, PackageHeader& output) {
  if (bytes == nullptr) return Status::InvalidArgument;
  if (size < MIN_PACKAGE_SIZE || size > MAX_PACKAGE_SIZE) return Status::InvalidSize;
  if (bytes[0] != 'X' || bytes[1] != '3' || bytes[2] != 'D' || bytes[3] != 'P') return Status::InvalidMagic;
  if (readU16(bytes + LENGTH_OFFSET) != size) return Status::InvalidSize;
  if (bytes[4] != SCHEMA_V1) return Status::UnsupportedSchema;
  if (crc32(bytes, size - CRC_SIZE) != readU32(bytes + size - CRC_SIZE)) return Status::InvalidCrc;

  PackageHeader candidate{};
  candidate.schema = bytes[4];
  candidate.templateId = bytes[5];
  candidate.packageId = readU32(bytes + PACKAGE_ID_OFFSET);
  candidate.generatedAt = readU64(bytes + GENERATED_AT_OFFSET);
  candidate.validUntil = readU64(bytes + VALID_UNTIL_OFFSET);
  if (candidate.generatedAt == 0 || candidate.validUntil < candidate.generatedAt) return Status::InvalidTimestamp;
  candidate.crc = readU32(bytes + size - CRC_SIZE);
  output = candidate;
  return Status::Ok;
}

Status comparePackageId(const bool haveCurrent, const uint32_t currentId, const uint32_t candidateId) {
  return !haveCurrent || candidateId > currentId ? Status::Ok : Status::StalePackage;
}

}  // namespace dashboard
