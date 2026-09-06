#pragma once

#include <cstdint>

// Pure startup display-submission controller for the calm X3 navigator entry
// (src/spikes/navigator/).
//
// Entry loads storage and the retained route without refreshing the panel,
// then submits exactly two navigator-owned frames:
//   1. a full-quality route-layout loading frame - the route geometry in the
//      final navigation layout with the compact localized loading status in
//      the normal footer position (never a separate splash or full-screen
//      loading page), and
//   2. one completed route frame, drawn once the latest fix and the best
//      available background map are open (it replaces the loading status in
//      place and may use the grayscale display sequence).
// After the completed frame this controller is finished: later frames are
// ordinary navigation refreshes governed by the refresh policy and are never
// routed through it. Explicit terminal failures (no route, no SD card, an
// unreadable map, unavailable Bluetooth) remain full-screen error decisions
// and are never suppressed merely to reduce flashing.
//
// The component is display-eligibility only: no framebuffer, no device calls,
// no allocation. It is a couple of fixed-width bytes of static/global state.
namespace navigator {

// What the navigator may submit next, from the pure startup controller's
// point of view.
enum class NavigationStartupDecision : uint8_t {
  None = 0,             // no startup submission is permitted right now
  LoadingLayout = 1,    // the quiet route-layout loading frame (Full refresh)
  CompletedLayout = 2,  // the first completed route frame
  TerminalFailure = 3,  // an explicit error screen remains displayable
};

class NavigationStartup {
 public:
  NavigationStartup() = default;

  // Begins a new calm entry (used once at navigator startup).
  void reset() {
    phase_ = Phase::Boot;
    failed_ = false;
  }

  // The retained route and storage are ready and the navigator owns the
  // display: permits the LoadingLayout frame exactly once, and only before
  // any completed frame or terminal failure.
  NavigationStartupDecision routeLayoutReady() {
    if (failed_ || phase_ != Phase::Boot) {
      return NavigationStartupDecision::None;
    }
    phase_ = Phase::LoadingShown;
    return NavigationStartupDecision::LoadingLayout;
  }

  // The fix/background resources are open: permits exactly one CompletedLayout
  // frame following the LoadingLayout. Any later call returns None, so
  // ordinary refreshes never look like startup submissions.
  NavigationStartupDecision resourcesReady() {
    if (failed_ || phase_ != Phase::LoadingShown) {
      return NavigationStartupDecision::None;
    }
    phase_ = Phase::Completed;
    return NavigationStartupDecision::CompletedLayout;
  }

  // A terminal failure (no route, no SD card, unreadable map, unavailable
  // Bluetooth): the caller keeps its explicit error screen and no calm
  // startup frame may follow.
  NavigationStartupDecision fail() {
    failed_ = true;
    return NavigationStartupDecision::TerminalFailure;
  }

  bool failed() const { return failed_; }
  bool completed() const { return phase_ == Phase::Completed; }

 private:
  enum class Phase : uint8_t { Boot = 0, LoadingShown = 1, Completed = 2 };

  Phase phase_ = Phase::Boot;
  bool failed_ = false;
};

static_assert(sizeof(NavigationStartup) <= 2, "startup controller must stay tiny");

}  // namespace navigator
