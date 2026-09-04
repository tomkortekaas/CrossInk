#pragma once
#include "WalkMap.h"
#ifdef ARDUINO
#include <SDCardManager.h>
namespace navigator {
// One explicitly scoped, read-only handle for a complete map operation.
// The global navigator owns this object; no per-read allocations or SD power changes.
class WalkMapSdSource final : public WalkMapByteSource {
public:
  bool beginRead(bool (*cancelRequested)() = nullptr, const char* path = "/Navigation/Maps/active.walkmap");
  bool endRead();
  uint32_t size() const override;
  uint32_t read(uint32_t offset, uint8_t *destination, uint32_t length) override;
private:
  FsFile file_;
  uint32_t size_ = 0;
  uint32_t position_ = 0;
  bool (*cancelRequested_)() = nullptr;
};
} // namespace navigator
#endif
