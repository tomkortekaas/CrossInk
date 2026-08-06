#include "DashboardBleProtocol.h"

#include <algorithm>

namespace probe {
namespace {

constexpr std::array<uint8_t, 4> kMagic = {'X', '3', 'B', 'P'};
constexpr uint8_t kVersion = 1;

uint16_t readLe16(const uint8_t* bytes) {
  return static_cast<uint16_t>(bytes[0]) | (static_cast<uint16_t>(bytes[1]) << 8);
}

uint32_t readLe32(const uint8_t* bytes) {
  return static_cast<uint32_t>(bytes[0]) | (static_cast<uint32_t>(bytes[1]) << 8) |
         (static_cast<uint32_t>(bytes[2]) << 16) | (static_cast<uint32_t>(bytes[3]) << 24);
}

}  // namespace

uint32_t crc32(const uint8_t* bytes, const size_t length) {
  uint32_t crc = 0xFFFFFFFFu;
  for (size_t i = 0; i < length; ++i) {
    crc ^= bytes[i];
    for (uint8_t bit = 0; bit < 8; ++bit) {
      crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
  }
  return crc ^ 0xFFFFFFFFu;
}

bool isValidUtf8(const uint8_t* bytes, const size_t length) {
  size_t i = 0;
  while (i < length) {
    const uint8_t first = bytes[i++];
    if (first <= 0x7F) continue;

    uint32_t codePoint = 0;
    size_t continuationCount = 0;
    uint32_t minimum = 0;
    if (first >= 0xC2 && first <= 0xDF) {
      codePoint = first & 0x1Fu;
      continuationCount = 1;
      minimum = 0x80;
    } else if (first >= 0xE0 && first <= 0xEF) {
      codePoint = first & 0x0Fu;
      continuationCount = 2;
      minimum = 0x800;
    } else if (first >= 0xF0 && first <= 0xF4) {
      codePoint = first & 0x07u;
      continuationCount = 3;
      minimum = 0x10000;
    } else {
      return false;
    }

    if (continuationCount > length - i) return false;
    for (size_t j = 0; j < continuationCount; ++j) {
      const uint8_t next = bytes[i++];
      if ((next & 0xC0u) != 0x80u) return false;
      codePoint = (codePoint << 6) | (next & 0x3Fu);
    }
    if (codePoint < minimum || codePoint > 0x10FFFFu ||
        (codePoint >= 0xD800u && codePoint <= 0xDFFFu)) {
      return false;
    }
  }
  return true;
}

DecodeResult decodeFrame(const uint8_t* frame, const size_t length) {
  DecodeResult result;
  if (frame == nullptr) return result;

  if (length >= kHeaderBytes) {
    result.hasMessageId = true;
    result.messageId = readLe32(frame + 5);
  }
  if (length < kMinFrameBytes) return result;

  if (!std::equal(kMagic.begin(), kMagic.end(), frame)) {
    result.status = Status::BadMagic;
    return result;
  }
  if (frame[4] != kVersion) {
    result.status = Status::UnsupportedVersion;
    return result;
  }

  const uint16_t payloadLength = readLe16(frame + 9);
  if (payloadLength > kMaxPayloadBytes || length != kMinFrameBytes + payloadLength) return result;

  const uint8_t* payload = frame + kHeaderBytes;
  if (!isValidUtf8(payload, payloadLength)) return result;

  const uint32_t expectedCrc = readLe32(payload + payloadLength);
  if (crc32(payload, payloadLength) != expectedCrc) {
    result.status = Status::BadCrc;
    return result;
  }

  result.payloadLength = static_cast<uint8_t>(payloadLength);
  std::copy_n(payload, payloadLength, result.payload.begin());
  result.status = Status::Accepted;
  return result;
}

const char* statusName(const Status status) {
  switch (status) {
    case Status::Ready:
      return "ready";
    case Status::Accepted:
      return "accepted";
    case Status::BadMagic:
      return "bad_magic";
    case Status::UnsupportedVersion:
      return "unsupported_version";
    case Status::InvalidLength:
      return "invalid_length";
    case Status::BadCrc:
      return "bad_crc";
    case Status::RenderFailed:
      return "render_failed";
  }
  return "invalid_length";
}

}  // namespace probe
