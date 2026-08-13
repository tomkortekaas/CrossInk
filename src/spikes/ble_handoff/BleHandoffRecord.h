#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace ble_handoff {

constexpr size_t MAX_PAYLOAD_SIZE = 64;
constexpr size_t RECORD_SIZE = 4 + 2 + 4 + 1 + MAX_PAYLOAD_SIZE + 4;

enum class Status : uint8_t {
  Ok,
  InvalidArgument,
  InvalidSize,
  InvalidMagic,
  InvalidVersion,
  InvalidLength,
  InvalidCrc,
  SequenceOverflow,
  NotFound,
  OpenFailed,
  ReadFailed,
  WriteFailed,
  CommitFailed,
  VerifyFailed,
};

using RecordBytes = std::array<uint8_t, RECORD_SIZE>;

struct DecodedRecord {
  uint32_t sequence = 0;
  uint8_t length = 0;
  std::array<uint8_t, MAX_PAYLOAD_SIZE> payload{};
};

uint32_t crc32(const uint8_t* data, size_t length);
Status buildRecord(const uint8_t* payload, size_t length, uint32_t sequence, RecordBytes& out);
Status validateRecord(const uint8_t* bytes, size_t size, DecodedRecord& out);
Status nextSequence(bool haveCurrent, uint32_t current, uint32_t& out);

}  // namespace ble_handoff
