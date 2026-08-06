#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace probe {

inline constexpr char kAdvertisingName[] = "X3 Dashboard Probe";
inline constexpr char kServiceUuid[] = "78334442-4c45-5052-4f42-450000000001";
inline constexpr char kWriteUuid[] = "78334442-4c45-5052-4f42-450000000002";
inline constexpr char kStatusUuid[] = "78334442-4c45-5052-4f42-450000000003";

constexpr size_t kHeaderBytes = 11;
constexpr size_t kCrcBytes = 4;
constexpr size_t kMinFrameBytes = kHeaderBytes + kCrcBytes;
constexpr size_t kMaxPayloadBytes = 48;
constexpr size_t kMaxFrameBytes = kMinFrameBytes + kMaxPayloadBytes;

enum class Status : uint8_t {
  Ready,
  Accepted,
  BadMagic,
  UnsupportedVersion,
  InvalidLength,
  BadCrc,
  RenderFailed,
};

struct DecodeResult {
  Status status = Status::InvalidLength;
  bool hasMessageId = false;
  uint32_t messageId = 0;
  uint8_t payloadLength = 0;
  std::array<uint8_t, kMaxPayloadBytes> payload{};
};

uint32_t crc32(const uint8_t* bytes, size_t length);
bool isValidUtf8(const uint8_t* bytes, size_t length);
DecodeResult decodeFrame(const uint8_t* frame, size_t length);
const char* statusName(Status status);
bool formatStatus(Status status, bool hasMessageId, uint32_t messageId, char* output, size_t outputSize);

}  // namespace probe
