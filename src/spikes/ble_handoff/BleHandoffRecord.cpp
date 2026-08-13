#include "BleHandoffRecord.h"

#include <algorithm>
#include <limits>

namespace ble_handoff {
namespace {

constexpr uint32_t RECORD_MAGIC = 0x58424C45U;
constexpr uint16_t RECORD_VERSION = 1;
constexpr size_t MAGIC_OFFSET = 0;
constexpr size_t VERSION_OFFSET = MAGIC_OFFSET + sizeof(uint32_t);
constexpr size_t SEQUENCE_OFFSET = VERSION_OFFSET + sizeof(uint16_t);
constexpr size_t LENGTH_OFFSET = SEQUENCE_OFFSET + sizeof(uint32_t);
constexpr size_t PAYLOAD_OFFSET = LENGTH_OFFSET + sizeof(uint8_t);
constexpr size_t CRC_OFFSET = PAYLOAD_OFFSET + MAX_PAYLOAD_SIZE;

static_assert(CRC_OFFSET + sizeof(uint32_t) == RECORD_SIZE);

void writeU16(uint8_t* destination, const uint16_t value) {
  destination[0] = static_cast<uint8_t>(value);
  destination[1] = static_cast<uint8_t>(value >> 8U);
}

void writeU32(uint8_t* destination, const uint32_t value) {
  destination[0] = static_cast<uint8_t>(value);
  destination[1] = static_cast<uint8_t>(value >> 8U);
  destination[2] = static_cast<uint8_t>(value >> 16U);
  destination[3] = static_cast<uint8_t>(value >> 24U);
}

uint16_t readU16(const uint8_t* source) {
  return static_cast<uint16_t>(source[0]) | (static_cast<uint16_t>(source[1]) << 8U);
}

uint32_t readU32(const uint8_t* source) {
  return static_cast<uint32_t>(source[0]) | (static_cast<uint32_t>(source[1]) << 8U) |
         (static_cast<uint32_t>(source[2]) << 16U) | (static_cast<uint32_t>(source[3]) << 24U);
}

}  // namespace

uint32_t crc32(const uint8_t* data, const size_t length) {
  if (data == nullptr && length != 0) return 0;

  uint32_t crc = 0xFFFFFFFFU;
  for (size_t index = 0; index < length; ++index) {
    crc ^= data[index];
    for (uint8_t bit = 0; bit < 8; ++bit) {
      const uint32_t mask = 0U - (crc & 1U);
      crc = (crc >> 1U) ^ (0xEDB88320U & mask);
    }
  }
  return ~crc;
}

Status buildRecord(const uint8_t* payload, const size_t length, const uint32_t sequence, RecordBytes& out) {
  if (length == 0 || length > MAX_PAYLOAD_SIZE) return Status::InvalidLength;
  if (payload == nullptr) return Status::InvalidArgument;

  out.fill(0);
  writeU32(out.data() + MAGIC_OFFSET, RECORD_MAGIC);
  writeU16(out.data() + VERSION_OFFSET, RECORD_VERSION);
  writeU32(out.data() + SEQUENCE_OFFSET, sequence);
  out[LENGTH_OFFSET] = static_cast<uint8_t>(length);
  std::copy_n(payload, length, out.data() + PAYLOAD_OFFSET);

  const size_t coveredLength = sizeof(uint16_t) + sizeof(uint32_t) + sizeof(uint8_t) + length;
  writeU32(out.data() + CRC_OFFSET, crc32(out.data() + VERSION_OFFSET, coveredLength));
  return Status::Ok;
}

Status validateRecord(const uint8_t* bytes, const size_t size, DecodedRecord& out) {
  if (bytes == nullptr) return Status::InvalidArgument;
  if (size != RECORD_SIZE) return Status::InvalidSize;
  if (readU32(bytes + MAGIC_OFFSET) != RECORD_MAGIC) return Status::InvalidMagic;
  if (readU16(bytes + VERSION_OFFSET) != RECORD_VERSION) return Status::InvalidVersion;

  const uint8_t length = bytes[LENGTH_OFFSET];
  if (length == 0 || length > MAX_PAYLOAD_SIZE) return Status::InvalidLength;

  const size_t coveredLength = sizeof(uint16_t) + sizeof(uint32_t) + sizeof(uint8_t) + length;
  if (crc32(bytes + VERSION_OFFSET, coveredLength) != readU32(bytes + CRC_OFFSET)) return Status::InvalidCrc;

  out.sequence = readU32(bytes + SEQUENCE_OFFSET);
  out.length = length;
  out.payload.fill(0);
  std::copy_n(bytes + PAYLOAD_OFFSET, length, out.payload.begin());
  return Status::Ok;
}

Status nextSequence(const bool haveCurrent, const uint32_t current, uint32_t& out) {
  if (!haveCurrent) {
    out = 1;
    return Status::Ok;
  }
  if (current == std::numeric_limits<uint32_t>::max()) return Status::SequenceOverflow;
  out = current + 1U;
  return Status::Ok;
}

}  // namespace ble_handoff
