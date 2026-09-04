#pragma once
#include <cstdint>
namespace navigator {
// Independent from the small route-package source: regional maps may be larger.
class WalkMapByteSource {
public:
  virtual ~WalkMapByteSource() = default;
  virtual uint32_t size() const = 0;
  virtual uint32_t read(uint32_t offset, uint8_t *destination,
                        uint32_t length) = 0;
};
struct GeoBounds {
  int32_t southE7, westE7, northE7, eastE7;
};
struct WalkMapEdge {
  int32_t latitude1E7, longitude1E7, latitude2E7, longitude2E7;
  uint8_t kind, flags;
  char label[45]{}; // v2 labels: bounded printable ASCII; v1 stays empty.
};
enum class WalkMapStatus : uint8_t {
  Ok,
  NotOpen,
  InvalidData,
  ReadFailed,
  BudgetExceeded
};
using WalkMapVisitor = void (*)(void *, const WalkMapEdge &);
// No heap. Header only is retained; directory and visible cells are streamed.
// Source bytes must stay immutable during each call. On any visit failure the
// caller must clear previously drawn background. CRCs are not authentication.
class WalkMap {
public:
  static constexpr uint32_t kMaxBytes = 1536u * 1024u * 1024u;
  static constexpr uint32_t kCellE7 = 200000;
  WalkMapStatus open(WalkMapByteSource &source);
  WalkMapStatus visit(WalkMapByteSource &source, const GeoBounds &bounds,
                      WalkMapVisitor visitor, void *context) const;
  bool valid() const { return valid_; }

private:
  uint8_t header_[48]{};
  bool valid_ = false;
};
static_assert(sizeof(WalkMap) <= 64, "Map metadata must remain small");
} // namespace navigator
