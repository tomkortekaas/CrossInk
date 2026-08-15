#pragma once

#include "BleHandoffRecord.h"

namespace dashboard {

enum class PersistStatus : uint8_t { Ok, NotFound, Stale, InvalidPackage, OpenFailed, ReadFailed, WriteFailed, CommitFailed, VerifyFailed };

struct PersistedPackage {
  PackageBytes bytes{};
  uint16_t length = 0;
  PackageHeader header{};
  int8_t slot = -1;
};

PersistStatus readLastKnownGood(PersistedPackage& output);
// `detailOut`, when given, receives the underlying validation status behind a
// PersistStatus::InvalidPackage result, so the receiver can report *why* a
// package was rejected instead of only that it was. Left untouched for every
// other PersistStatus, where the returned value is already the whole story.
PersistStatus persistIfNewer(const uint8_t* bytes, size_t length, PersistedPackage& output,
                             Status* detailOut = nullptr);

}  // namespace dashboard
