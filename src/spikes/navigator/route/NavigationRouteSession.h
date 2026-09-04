#pragma once

#include "StoredRoute.h"

namespace navigator {

// Main-loop owner: never call from a BLE callback. Keep this ~20 KiB object
// static/global, not on the ESP32 task stack. No full package in RAM.
class NavigationRouteSession {
 public:
  explicit NavigationRouteSession(RouteFileSystem& fs) : store_(fs), source_(fs) {}
  bool load() {
    source_.load(active_);
    return source_.isLoaded();
  }
  bool hasRoute() const { return source_.isLoaded(); }
  const RouteIndex& index() const { return active_; }
  RouteByteSource& source() { return source_; }
  bool transferring() const { return transfer_.isActive(); }
  void disconnect() { transfer_.abortTransfer(store_); load(); }
  RouteTransferStatus receive(const uint8_t* data, uint32_t length) {
    // Only a valid START may prepare storage; malformed lengths must not
    // cause recovery renames before the core rejects the command.
    if (data && length == RouteTransfer::kStartFrameBytes && data[0] == kRouteOpcodeStart &&
        !transfer_.isActive()) {
      const uint32_t declared = uint32_t(data[5]) | uint32_t(data[6]) << 8 |
                                uint32_t(data[7]) << 16 | uint32_t(data[8]) << 24;
      if (!declared || declared > kRoutePackageV1MaxBytes)
        return transfer_.handleFrame(data, length, store_, candidate_);
      if (!source_.prepareForTransfer(candidate_)) {
        load();
        const uint32_t id = uint32_t(data[1]) | uint32_t(data[2]) << 8 |
                            uint32_t(data[3]) << 16 | uint32_t(data[4]) << 24;
        return {RouteTransferCode::RouteStorageFailed, id, 0};
      }
      load();
    }
    const auto status = transfer_.handleFrame(data, length, store_, candidate_);
    // A failed publish may leave the old file at .bak. Re-resolve its source
    // without ever publishing the candidate index from a failed transaction.
    if (status.code == RouteTransferCode::RouteAccepted ||
        status.code == RouteTransferCode::RouteStorageFailed) load();
    return status;
  }
 private:
  RouteStore store_;
  StoredRoute source_;
  RouteTransfer transfer_;
  RouteIndex active_{};
  RouteIndex candidate_{};
};

}  // namespace navigator
