#include "WalkMapSdSource.h"
#ifdef ARDUINO
#include <Logging.h>
#include <algorithm>
namespace navigator {
bool WalkMapSdSource::beginRead(bool (*cancelRequested)(), const char* path) {
  if (!endRead() || !SdMan.ready()) return false;
  file_ = SdMan.open(path, O_RDONLY);
  if (!file_ || !file_.isFile() || file_.fileSize() > WalkMap::kMaxBytes) {
    endRead();
    return false;
  }
  size_ = static_cast<uint32_t>(file_.fileSize());
  position_ = 0;
  cancelRequested_ = cancelRequested;
  return true;
}
bool WalkMapSdSource::endRead() {
  size_ = position_ = 0;
  cancelRequested_ = nullptr;
  if (file_ && !file_.close()) {
    LOG_ERR("NAV", "map close failed");
    return false;
  }
  return true;
}
uint32_t WalkMapSdSource::size() const { return size_; }
uint32_t WalkMapSdSource::read(uint32_t offset, uint8_t *destination, uint32_t length) {
  if (!destination || !length || !file_ || !SdMan.ready() || offset >= size_ ||
      (cancelRequested_ && cancelRequested_())) return 0;
  if (offset != position_ && !file_.seekSet(offset)) {
    LOG_ERR("NAV", "map seek failed");
    return 0;
  }
  const uint32_t bytes = std::min(std::min<uint32_t>(length, 256), size_ - offset);
  const int got = file_.read(destination, bytes);
  if (got <= 0 || static_cast<uint32_t>(got) > bytes) {
    LOG_ERR("NAV", "map read failed");
    return 0;
  }
  position_ = offset + static_cast<uint32_t>(got);
  return static_cast<uint32_t>(got);
}
} // namespace navigator
#endif
