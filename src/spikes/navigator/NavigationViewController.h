#pragma once

#include <cstdint>

#include "map/RouteViewport.h"

// Session-only view state and pure input mapping for the X3 walking navigator
// (src/spikes/navigator/).
//
// The walker picks between two map framings on the X3 itself:
//   * Overview - the whole route fitted into the map region (the session
//     default; the current GPS position is shown when it lies inside the
//     view);
//   * GpsZoom  - a fixed 250 m, north-up view centred on the latest valid
//     GPS fix.
// The selection is session state only: it is never written to NVS or the SD
// card and every navigation session starts in Overview. Physical released
// buttons are translated through mapNavigatorButton() before application
// behaviour is chosen, so a later task can assign different buttons without
// touching rendering, viewport or session logic. This component has no
// display, BLE, storage, timing or preferences dependency; it stays a few
// fixed-width bytes and never allocates.

namespace navigator {

struct RouteIndex;  // route/RoutePackageV1.h; only selectViewport's caller needs it

// The two session map framings (small fixed-width enum, never persisted).
enum class NavigationView : uint8_t { Overview = 0, GpsZoom = 1 };

// Semantic navigator actions produced by the centralized button mapping.
enum class NavigatorAction : uint8_t {
  None = 0,
  ToggleView = 1,     // switch Overview <-> GPS zoom
  ManualRefresh = 2,  // refresh the current view now
  ReturnToDashboard = 3,
};

// Maps a released FreeInk InputManager::BTN_* index to a semantic navigator
// action: Up/Down toggle the view, OK (Confirm) refreshes manually and Back
// returns to the dashboard; every other release (and unknown values) maps to
// None. Pure input mapping: no display, BLE, storage, timing or preferences.
NavigatorAction mapNavigatorButton(uint8_t releasedButton);

// Fixed horizontal ground span of the GPS zoom view across the usable map
// rectangle (see the X3 navigation view spec).
inline constexpr uint32_t kGpsZoomSpanMeters = 250;
// Equal padding the navigator applies when fitting/centring map content; it
// must match the padding NavScreenRenderer::drawOverview uses for its own
// map rectangle so an injected viewport stays pixel-aligned.
inline constexpr int kNavigationViewPaddingPx = 24;

// Result of selecting a viewport for the current view. `viewport` is invalid
// when the route or map rectangle cannot be fitted; `waitingForGps` is set
// when GPS zoom is requested but no valid fix exists yet, in which case
// `viewport` holds the Overview (whole-route fit) framing and the caller
// shows the waiting-for-GPS status.
struct NavigationViewportSelection {
  RouteViewport viewport;
  bool waitingForGps = false;
};

static_assert(sizeof(NavigationViewportSelection) <= sizeof(RouteViewport) + 8, "view selection must stay compact");

// Small allocation-free controller owning the session view mode and selecting
// the viewport for the current frame. One instance lives for the whole
// navigator session (a few bytes of static/global state); no heap, no writes.
class NavigationViewController {
 public:
  NavigationViewController() = default;

  NavigationView view() const { return view_; }

  // Every navigation session starts in Overview.
  void reset() { view_ = NavigationView::Overview; }

  // Toggles between Overview and GPS zoom (session state only).
  void toggle() { view_ = view_ == NavigationView::Overview ? NavigationView::GpsZoom : NavigationView::Overview; }

  // Selects the viewport that renders the current view over `mapRect` for
  // `route`:
  //   * Overview always uses the existing whole-route fit (byte-compatible
  //     with the route-fit path), even when a valid fix is available;
  //   * GPS zoom centres a valid `fix` north-up with a fixed
  //     kGpsZoomSpanMeters horizontal span;
  //   * GPS zoom without a valid fix keeps the mode but returns the Overview
  //     fit and flags waitingForGps - a centre is never invented.
  NavigationViewportSelection selectViewport(const RouteIndex& route, const Rect& mapRect,
                                             const CurrentPosition* fix) const {
    NavigationViewportSelection selection;
    if (view_ == NavigationView::GpsZoom && fix != nullptr) {
      selection.viewport = RouteViewport::centered(fix->point, mapRect, kGpsZoomSpanMeters, kNavigationViewPaddingPx);
      return selection;
    }
    selection.waitingForGps = view_ == NavigationView::GpsZoom;
    selection.viewport = RouteViewport::fitOverview(route, mapRect, kNavigationViewPaddingPx);
    return selection;
  }

 private:
  NavigationView view_ = NavigationView::Overview;
};

static_assert(sizeof(NavigationViewController) == 1, "controller must stay a single byte");

}  // namespace navigator
