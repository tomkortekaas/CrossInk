#pragma once
#include "RouteViewport.h"
#include "WalkMap.h"
namespace navigator {
class RouteCanvas;
// X3GM v1. Immutable source for open/draw lifetime. One 128-byte row on stack,
// no raster allocation. The bounded labels live in the global map instance.
class GrayMap {
 public:
  WalkMapStatus open(WalkMapByteSource&);
  WalkMapStatus draw(WalkMapByteSource&, const RouteViewport&, RouteCanvas&);
  bool valid() const { return valid_; }

 private:
  uint8_t header_[48]{};
  struct Label {
    GeoPoint point;
    char text[40];
  } labels_[32]{};
  bool valid_ = false;
};
struct GrayMapLayer {
  WalkMapByteSource* source;
  GrayMap* map;
  WalkMapStatus status = WalkMapStatus::NotOpen;
};
}  // namespace navigator
