#pragma once

#include "BleHandoffRecord.h"

namespace dashboard {

enum class PersistStatus : uint8_t { Ok, NotFound, Stale, InvalidPackage, OpenFailed, ReadFailed, WriteFailed, CommitFailed, VerifyFailed };

struct PersistedPackage {
  PackageBytes bytes{};
  uint16_t length = 0;
  Package package{};
  int8_t slot = -1;
};

PersistStatus readLastKnownGood(PersistedPackage& output);
PersistStatus persistIfNewer(const uint8_t* bytes, size_t length, PersistedPackage& output);

}  // namespace dashboard
