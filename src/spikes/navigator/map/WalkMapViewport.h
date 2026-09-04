#pragma once
#include "RouteViewport.h"
#include "WalkMap.h"
namespace navigator {
// Conservative geographic bounds of visible pixels. Regional v1 maps do not
// wrap the dateline; a crossing view returns false rather than omitting a side.
bool walkMapBounds(const RouteViewport &viewport, GeoBounds &result);
} // namespace navigator
