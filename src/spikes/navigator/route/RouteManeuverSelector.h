#pragma once

#include <cstdint>

#include "../NavState.h"
#include "RoutePackageV1.h"

// Fixed-size, allocation-free next-maneuver selection for the X3 navigator
// (src/spikes/navigator/route/).
//
// Task 7 (firmware core): the selector owns progression and change detection
// over the *validated* maneuver list of a RouteIndex; rendering code never
// decides which maneuver is current. Inputs are the validated index, a
// projected along-route distance from start, and whether that progress is
// trustworthy (the same max(40 m, 2x accuracy) guard the footer uses).
//
// Semantics (see test/navigator/RouteManeuverSelectorTest.cpp):
//   * With maneuvers present the first selection is index 0; selection then
//     advances monotonically, and only while progress is trusted and has
//     passed a maneuver's distanceFromStartMeters by the exact
//     kPassHysteresisMeters = 12. Several passed maneuvers may advance in one
//     update (the +12 comparison saturates at UINT32_MAX so it can never
//     wrap).
//   * The stored index never moves backward and untrusted progress never
//     advances it; distanceMeters is always the saturating remaining distance
//     from the supplied progress to the selected maneuver (clamped to the
//     UInt16 display range), so the countdown stays deterministic across
//     trust transitions.
//   * The arrival maneuver is terminal: once selected it is never advanced
//     past merely by +12 and stays on screen through and after the
//     destination. An arrival placed earlier in the list therefore also
//     stops progression deterministically, without any scan beyond
//     route.maneuverCount.
//   * An empty (or beyond-capacity) maneuver list returns valid=false and
//     clears any previous route's selection. Route identity is the validated
//     RouteIndex::routeId: handing the selector a different RouteIndex resets
//     it so a stale or out-of-range maneuver can never survive a route
//     change. Callers additionally call reset() on explicit route acceptance
//     and live-session identity changes.
//
// No heap, no strings, no SDK or Arduino dependency; the whole state is three
// scalars so it can live in static storage beside the ~10 KiB RouteIndex.
namespace navigator {

// Result of one RouteManeuverSelector::update(). `valid` is false only when
// the route carries no selectable maneuvers (the map keeps its current
// map-only layout); otherwise `routeManeuverIndex` selects the current
// maneuver record, `maneuver` is the existing navigator::Maneuver for the
// instruction band, and `distanceMeters` is the saturating remaining distance
// from the supplied progress to that maneuver.
struct ManeuverSelection {
  bool valid = false;
  uint16_t routeManeuverIndex = 0;
  Maneuver maneuver = Maneuver::Straight;
  uint16_t distanceMeters = 0;
};

class RouteManeuverSelector {
 public:
  // Pass hysteresis: a maneuver is only considered passed once trusted
  // progress is more than 12 m beyond its distanceFromStartMeters. The value
  // is fixed by the walking-guidance spec (and the iPhone tracker's pass
  // hysteresis) and must stay exactly 12.
  static constexpr uint32_t kPassHysteresisMeters = 12;

  // Returns the current selection for `route`. The maneuver list is read from
  // the validated index only; the selector never scans beyond
  // route.maneuverCount and never copies name bytes or geometry.
  ManeuverSelection update(const RouteIndex& route, uint32_t distanceFromStartMeters, bool trusted);

  // Returns to the initial state: the next update on any route starts a fresh
  // selection at index 0. Call on route acceptance and on live-session
  // identity change.
  void reset() {
    routeId_ = 0;
    index_ = 0;
    hasSelection_ = false;
  }

 private:
  uint32_t routeId_ = 0;  // validated route identity the selection belongs to
  uint16_t index_ = 0;    // current maneuver index inside [0, maneuverCount)
  bool hasSelection_ = false;
};

static_assert(sizeof(RouteManeuverSelector) <= 8, "selector state must stay tiny (<= 8 bytes) for static storage");
static_assert(RouteManeuverSelector::kPassHysteresisMeters == 12, "pass hysteresis is fixed at 12 meters");

}  // namespace navigator
