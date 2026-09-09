#pragma once

#include <cstdint>

#include "NavState.h"
#include "route/RouteManeuverSelector.h"

// Decides the content of the fixed instruction band above the four-gray
// overview map for one submitted frame (src/spikes/navigator/).
//
// This is the missing link between two components that already existed and
// were fully tested but never met: NavScreenRenderer::drawManeuverBand had no
// callers, and RouteManeuverSelector was never instantiated. The result on
// hardware was a navigator that drew the map and the walker's position but
// never a turn instruction.
//
// The planner is deliberately pure and host-testable, because NavigatorMain
// sits behind CROSSINK_NAVIGATOR and pulls in Arduino, the display driver and
// I18n -- no host build compiles it, so no logic may live there. NavigatorMain
// keeps only what it alone can do: call the renderer and translate the
// selected Maneuver into a localized label.
//
// Trust is not re-derived here. It is read back from
// NavScreenRenderer::chooseFooterMetrics, which already owns the
// max(40 m, 2x accuracy) off-route gate and the "phone-tracked progress wins
// over the geometric estimate" rule. Reusing it means the band's countdown and
// the footer's remaining distance are computed from one number and cannot
// disagree on screen.
namespace navigator {

struct CurrentPosition;  // map/RouteViewport.h
struct RouteProximity;   // map/RouteMapRenderer.h

// One frame's band decision. `visible` is the reservation: when it is set,
// drawOverview must be handed a non-null presentation so it shortens the map,
// and drawManeuverBand must paint the same band afterwards.
struct ManeuverBandPlan {
  bool visible = false;
  Maneuver maneuver = Maneuver::Straight;
  uint16_t distanceMeters = 0;
  // Whether the countdown was driven by a fix the footer also trusts. False
  // means the distance is measured from the route start, which is what a
  // walker without a usable fix should see.
  bool progressTrusted = false;
};

class ManeuverBandPlanner {
 public:
  // Whether this route gets a band at all. Pure and frame-independent on
  // purpose: the reservation must be decided before the frame is composed and
  // must be identical for the Base, LSB and MSB passes, or the grayscale
  // planes disagree about where the map starts.
  static bool reservesBand(const RouteIndex& route);

  // The band content for one frame. `proximity` is the result drawOverview
  // already computed for this pass, so no geometry is scanned twice.
  ManeuverBandPlan plan(const RouteIndex& route, const CurrentPosition* position, const RouteProximity& proximity);

  // Returns to the initial state. Call on route acceptance and live-session
  // identity change, exactly like the selector it wraps.
  void reset() { selector_.reset(); }

 private:
  RouteManeuverSelector selector_;
};

static_assert(sizeof(ManeuverBandPlanner) <= 8, "the planner must stay as small as the selector it wraps");

}  // namespace navigator
