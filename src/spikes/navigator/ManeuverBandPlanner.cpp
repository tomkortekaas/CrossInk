#include "ManeuverBandPlanner.h"

#include "NavScreenRenderer.h"
#include "map/RouteMapRenderer.h"
#include "map/RouteViewport.h"

namespace navigator {

bool ManeuverBandPlanner::reservesBand(const RouteIndex& route) {
  return route.maneuverCount > 0 && route.maneuverCount <= RouteIndex::kMaxManeuvers;
}

ManeuverBandPlan ManeuverBandPlanner::plan(const RouteIndex& route, const CurrentPosition* position,
                                          const RouteProximity& proximity) {
  ManeuverBandPlan out;
  if (!reservesBand(route)) {
    // Clear any previous route's selection so a reused planner can never
    // report a stale maneuver for a route that declares none.
    selector_.update(route, 0, false);
    return out;
  }

  // The footer already decided whether this fix may drive route progress at
  // all, and what the authoritative remaining distance is. Read both back
  // instead of re-deriving them: Remaining is exactly the selector's
  // `trusted`, and total - remaining is the distance walked from the start.
  const NavFooterMetrics metrics = NavScreenRenderer::chooseFooterMetrics(position, proximity, route);
  const bool trusted = metrics.mode == NavMetricMode::Remaining;
  // chooseFooterMetrics clamps remaining to [0, total] before returning it in
  // Remaining mode, so this subtraction cannot underflow.
  const uint32_t walked = trusted ? route.totalDistanceMeters - metrics.distanceMeters : 0;

  const ManeuverSelection selection = selector_.update(route, walked, trusted);
  out.visible = selection.valid;
  out.maneuver = selection.maneuver;
  out.distanceMeters = selection.distanceMeters;
  out.progressTrusted = trusted;
  return out;
}

}  // namespace navigator
