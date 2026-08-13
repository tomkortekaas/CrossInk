#include "DashboardTransfer.h"

#include <algorithm>

namespace dashboard {
namespace {

uint16_t readU16(const uint8_t* in) { return static_cast<uint16_t>(in[0]) | (static_cast<uint16_t>(in[1]) << 8U); }

uint32_t readU32(const uint8_t* in) {
  return static_cast<uint32_t>(in[0]) | (static_cast<uint32_t>(in[1]) << 8U) |
         (static_cast<uint32_t>(in[2]) << 16U) | (static_cast<uint32_t>(in[3]) << 24U);
}

TransferResult result(TransferStatus status, uint32_t packageId, uint16_t received) {
  return {status, packageId, received};
}

}  // namespace

TransferResult TransferAssembler::accept(const uint8_t* frame, const size_t frameLength) {
  if (frame == nullptr || frameLength < 5) return result(TransferStatus::InvalidFrame, packageId_, received_);
  const uint8_t type = frame[0];
  const uint32_t id = readU32(frame + 1);
  if (type == 1) {
    if (frameLength != 11) return result(TransferStatus::InvalidFrame, id, 0);
    const uint16_t expectedLength = readU16(frame + 5);
    if (expectedLength == 0 || expectedLength > MAX_PACKAGE_SIZE) return result(TransferStatus::InvalidFrame, id, 0);
    packageId_ = id;
    expectedLength_ = expectedLength;
    expectedCrc_ = readU32(frame + 7);
    received_ = 0;
    started_ = true;
    bytes_.fill(0);
    return result(TransferStatus::Ready, id, 0);
  }
  if (!started_ || id != packageId_) return result(TransferStatus::InvalidFrame, id, received_);
  if (type == 2) {
    if (frameLength <= 7 || readU16(frame + 5) != received_) {
      return result(TransferStatus::InvalidFrame, id, received_);
    }
    const size_t chunkLength = frameLength - 7;
    if (received_ + chunkLength > expectedLength_) return result(TransferStatus::InvalidFrame, id, received_);
    std::copy_n(frame + 7, chunkLength, bytes_.begin() + received_);
    received_ = static_cast<uint16_t>(received_ + chunkLength);
    return result(TransferStatus::Progress, id, received_);
  }
  if (type == 3) {
    if (frameLength != 5 || received_ != expectedLength_ || received_ < sizeof(uint32_t) ||
        readU32(bytes_.data() + received_ - sizeof(uint32_t)) != expectedCrc_ ||
        crc32(bytes_.data(), received_ - sizeof(uint32_t)) != expectedCrc_) {
      return result(TransferStatus::InvalidFrame, id, received_);
    }
    started_ = false;
    return result(TransferStatus::Complete, id, received_);
  }
  return result(TransferStatus::InvalidFrame, id, received_);
}

const PackageBytes& TransferAssembler::bytes() const { return bytes_; }

size_t TransferAssembler::length() const { return received_; }

}  // namespace dashboard
