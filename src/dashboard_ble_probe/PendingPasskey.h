#pragma once

#include <atomic>
#include <cstdint>

namespace probe {

class PendingPasskey {
 public:
  void request(const uint32_t passkey) { passkey_.store(passkey, std::memory_order_release); }

  bool take(uint32_t& passkey) {
    const uint32_t pending = passkey_.exchange(0, std::memory_order_acq_rel);
    if (pending == 0) {
      return false;
    }
    passkey = pending;
    return true;
  }

 private:
  std::atomic<uint32_t> passkey_{0};
};

}  // namespace probe
