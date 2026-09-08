#pragma once
#include "RouteViewport.h"
#include "WalkMap.h"
namespace navigator {
class RouteCanvas;
// X3GM v1. Immutable source for open/draw lifetime. One 128-byte row on stack,
// no raster allocation. The calm four-gray background deliberately never
// paints tile labels (they crowd the panel); label payloads in the tile files
// are covered by the tile CRC but are not read or drawn by the gray map.
class GrayMap {
 public:
  WalkMapStatus open(WalkMapByteSource&);
  WalkMapStatus draw(WalkMapByteSource&, const RouteViewport&, RouteCanvas&);
  bool valid() const { return valid_; }

 private:
  uint8_t header_[48]{};
  bool valid_ = false;
};
struct GrayMapLayer {
  WalkMapByteSource* source;
  GrayMap* map;
  WalkMapStatus status = WalkMapStatus::NotOpen;
};
}  // namespace navigator
