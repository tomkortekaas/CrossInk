#pragma once

#include "BleHandoffRecord.h"

namespace dashboard {

enum class TransferStatus : uint8_t { Ready, Progress, Complete, InvalidFrame, InvalidPackage };

struct TransferResult {
  TransferStatus status = TransferStatus::InvalidFrame;
  uint32_t packageId = 0;
  uint16_t received = 0;
  bool navigationLaunchRequested = false;
};

class TransferAssembler {
 public:
  TransferResult accept(const uint8_t* frame, size_t length);
  const PackageBytes& bytes() const;
  size_t length() const;

 private:
  PackageBytes bytes_{};
  uint32_t packageId_ = 0;
  uint32_t expectedCrc_ = 0;
  uint16_t expectedLength_ = 0;
  uint16_t received_ = 0;
  bool started_ = false;
};

}  // namespace dashboard
