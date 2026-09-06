# X3 Navigation View and Calm Refresh Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Start X3 navigation in the final route layout and let Up/Down toggle between the whole route and a north-up, GPS-centred 250-metre view while OK refreshes and Back returns to the dashboard.

**Architecture:** Add one allocation-free domain component that owns session-only view state, maps released buttons to semantic actions, and selects either the existing overview viewport or a fixed centred viewport. Pass the selected viewport into the existing deterministic renderer for every grayscale plane, while `NavigatorMain` remains responsible for display submissions and translates semantic actions into refresh or exit behavior. Represent startup submissions with a tiny pure phase controller so the host suite can prove that only the loading-layout and completed-layout frames are eligible for display.

**Tech Stack:** C++20 host tests with CMake/GoogleTest, Arduino-ESP32/PlatformIO firmware, existing `RouteViewport`, `NavScreenRenderer`, `NavigationRefreshPolicy`, and FreeInk display APIs.

**Spec:** `docs/superpowers/specs/2026-09-06-x3-navigation-view-and-refresh-design.md`

## Global Constraints

- Only navigator-local X3 behavior changes; do not change BLE/iPhone protocols.
- Every navigation session starts in Overview; view selection is never persisted.
- GPS zoom is north-up, centred on the latest valid fix, and 250 metres wide within 5% integer-projection tolerance.
- Up and Down toggle view, OK forces refresh, and Back returns through one centralized action mapping.
- Preserve existing overview/map-only output, maneuver-band/footer geometry, guard bytes, row padding, and Base/LSB/MSB viewport alignment.
- Do not add a framebuffer, render-loop heap allocation, persistent write, or unbounded read.
- User-facing loading copy uses `tr(STR_*)`; terminal error screens remain explicit.
- Do not flash hardware in this plan.

---

### Task 1: Pure Navigation View and Action Controller

**Files:**
- Create: `src/spikes/navigator/NavigationViewController.h`
- Create: `src/spikes/navigator/NavigationViewController.cpp`
- Create: `test/navigator/NavigationViewControllerTest.cpp`
- Modify: `test/navigator/CMakeLists.txt`

**Interfaces:**
- Consumes: `InputManager::Button` values supplied by the firmware input loop and `NavigationRefresh` from `NavigationRefreshPolicy.h`.
- Produces: `enum class NavigationView : uint8_t { Overview, GpsZoom }`, `enum class NavigatorAction : uint8_t { None, ToggleView, ManualRefresh, ReturnToDashboard }`, `NavigatorAction mapNavigatorButton(uint8_t releasedButton)`, and `NavigationViewController::{view(), toggle(), selectViewport(...)}` returning a small `NavigationViewportSelection { RouteViewport viewport; bool waitingForGps; }`.

- [ ] **Step 1: Write failing controller tests**

  Add literal assertions proving default Overview, Up/Down mapping to ToggleView, Confirm mapping to ManualRefresh, Back mapping to ReturnToDashboard, unrelated buttons mapping to None, two toggles returning to Overview, overview selection matching `RouteViewport::fitOverview`, GPS selection centring a valid fix at the map-rectangle centre with a 250-metre horizontal span within 5%, and GPS selection without a fix returning the overview viewport with `waitingForGps == true`.

- [ ] **Step 2: Run the focused test and verify RED**

  Run: `cmake --build /tmp/crossink-navigator-tests --target NavigationViewControllerTest -j8 && /tmp/crossink-navigator-tests/navigator/NavigationViewControllerTest`

  Expected: compilation fails because `NavigationViewController` and its mapping do not exist.

- [ ] **Step 3: Add the minimal allocation-free implementation**

  Keep the controller a small value type, use fixed-width integer fields, call the existing `RouteViewport::fitOverview` unchanged for Overview/fallback, and use `RouteViewport::centered(fix->point, mapRect, 250, padding)` for GPS zoom. Map only release values; do not read display, BLE, storage, timing, or preferences in this component.

- [ ] **Step 4: Run the focused test and verify GREEN**

  Run: `cmake --build /tmp/crossink-navigator-tests --target NavigationViewControllerTest -j8 && /tmp/crossink-navigator-tests/navigator/NavigationViewControllerTest`

  Expected: all controller assertions pass.

### Task 2: Inject One Selected Viewport into Every Render Plane

**Files:**
- Modify: `src/spikes/navigator/NavScreenRenderer.h`
- Modify: `src/spikes/navigator/NavScreenRenderer.cpp`
- Modify: `test/navigator/NavigatorRouteScreenTest.cpp`

**Interfaces:**
- Consumes: optional `const RouteViewport* viewport` selected once by the controller/orchestrator.
- Produces: extended `NavScreenRenderer::drawOverview(..., const RouteViewport* viewport = nullptr)` that preserves the existing fitted overview when null and uses the supplied valid viewport for route, background, marker, footer proximity, and all gray planes.

- [ ] **Step 1: Write failing renderer tests**

  Add tests that render a hand-constructed centred viewport and assert the GPS marker is centred, the same supplied geometry is used in Base/LSB/MSB, guard bytes and row padding stay intact, and the old call with no viewport remains byte-identical to a call using the existing fitted overview. Retain the existing map-only and maneuver-band assertions unchanged.

- [ ] **Step 2: Run the focused renderer cases and verify RED**

  Run: `cmake --build /tmp/crossink-navigator-tests --target NavigatorCoreTest -j8 && /tmp/crossink-navigator-tests/navigator/NavigatorCoreTest --gtest_filter='*Viewport*:*Overview*:*ManeuverBand*:*GuardBytes*'`

  Expected: compilation or behavior failure because `drawOverview` cannot consume the selected viewport.

- [ ] **Step 3: Implement minimal viewport injection**

  Select `*viewport` only when the pointer is non-null and valid; otherwise execute the exact existing `fitOverview(index, mapRect, 24)` path. Reuse that same selected object throughout one draw call and pass the identical selection from `NavigatorMain` for Base, LSB, and MSB.

- [ ] **Step 4: Run focused renderer tests and verify GREEN**

  Run: `cmake --build /tmp/crossink-navigator-tests --target NavigatorCoreTest -j8 && /tmp/crossink-navigator-tests/navigator/NavigatorCoreTest --gtest_filter='*Viewport*:*Overview*:*ManeuverBand*:*GuardBytes*'`

  Expected: all selected tests pass with old map-only behavior unchanged.

### Task 3: Refresh and Startup Submission Decisions

**Files:**
- Create: `src/spikes/navigator/NavigationStartup.h`
- Create: `test/navigator/NavigationStartupTest.cpp`
- Modify: `test/navigator/NavigationRefreshPolicyTest.cpp`
- Modify: `test/navigator/CMakeLists.txt`

**Interfaces:**
- Consumes: startup events `RouteLayoutReady`, `ResourcesReady`, and terminal failure, plus `NavigationRefreshPolicy::decide(now, fix, manual, viewportChanged)`.
- Produces: allocation-free startup decision values that permit exactly a full-quality route-layout loading frame followed by one completed route frame, and explicit refresh-policy coverage for viewport changes versus routine fixes.

- [ ] **Step 1: Write failing policy/startup tests**

  Assert that a viewport change requests Full immediately, a routine moved fix after the configured interval requests Fast, manual refresh bypasses timing without changing view, and a normal startup event sequence emits exactly `[LoadingLayout, CompletedLayout]` while storage/no-route/map/Bluetooth terminal failures remain displayable decisions.

- [ ] **Step 2: Run tests and verify RED where behavior is missing**

  Run: `cmake --build /tmp/crossink-navigator-tests --target NavigationRefreshPolicyTest NavigationStartupTest -j8 && /tmp/crossink-navigator-tests/navigator/NavigationRefreshPolicyTest && /tmp/crossink-navigator-tests/navigator/NavigationStartupTest`

  Expected: startup target fails to compile before the pure submission controller exists; existing policy assertions document already-supported semantics.

- [ ] **Step 3: Add the minimal pure startup controller**

  Model only display eligibility, with no framebuffer or device calls. Reject intermediate storage/map-read progress submissions; allow terminal failure presentation without hiding errors.

- [ ] **Step 4: Run policy/startup tests and verify GREEN**

  Run the same command and expect both executables to exit successfully.

### Task 4: Wire Semantic Input, Selected View, and Calm Startup

**Files:**
- Modify: `src/spikes/navigator/NavigatorMain.cpp`
- Modify: `platformio.ini` only if the existing navigator source filter does not already include the new `.cpp`
- Modify: `lib/I18n/translations/dutch.yaml`
- Modify: `lib/I18n/translations/english.yaml`
- Modify generated i18n outputs only through `scripts/gen_i18n.py`
- Modify: `CHANGELOG.md`

**Interfaces:**
- Consumes: `mapNavigatorButton`, session `NavigationViewController`, `NavigationStartup`, optional selected viewport, and existing refresh policy.
- Produces: final firmware behavior: quiet route-layout loading frame, completed frame, immediate full refresh on toggles, forced refresh on OK, and dashboard return on Back.

- [ ] **Step 1: Add/confirm failing integration-facing host coverage**

  Ensure Tasks 1-3 fail if Up/Down are swapped away from ToggleView, if OK toggles state, if a missing GPS fix invents a centre, if a viewport change is downgraded from Full, or if an intermediate loading page is admitted.

- [ ] **Step 2: Wire the firmware minimally**

  Reset view state to Overview during setup. Replace direct button branches with one action switch. For ToggleView, update session state and call the refresh policy with `viewportChanged=true`; for ManualRefresh use `manual=true`; for ReturnToDashboard retain the established marker/slot/restart cleanup. Compute one viewport selection per submitted frame and reuse it for Base/LSB/MSB. Replace the full-screen loading call with `drawOverview` in the final layout using localized compact `Route laden…` status, then open the fix/background resources without additional display submission and replace it with the completed frame.

- [ ] **Step 3: Regenerate translations and inspect generated changes**

  Run: `scripts/gen_i18n.py`

  Expected: only the intended navigation loading string additions/changes appear in generated i18n files; no hand edits to generated files.

- [ ] **Step 4: Run all focused navigation tests**

  Run the controller, startup, refresh-policy, viewport, and renderer test targets/cases from Tasks 1-3. Expected: all pass.

- [ ] **Step 5: Update the changelog**

  Add one human-facing entry under the current unreleased Added/Changed section describing X3 whole-route/GPS-zoom toggling and quieter route loading.

### Task 5: Full Verification, Review, Commit, and Push

**Files:**
- Inspect: every changed file and the complete branch diff from `51e49a15`.

**Interfaces:**
- Consumes: completed implementation and tests.
- Produces: accepted, committed, pushed branch only if every required gate is green.

- [ ] **Step 1: Configure and run the complete navigator host suite**

  Run: `cmake -S test -B /tmp/crossink-navigator-tests -DCMAKE_BUILD_TYPE=Release && cmake --build /tmp/crossink-navigator-tests --target NavigatorCoreTest NavigationViewControllerTest NavigationStartupTest NavigationRefreshPolicyTest RouteManeuverSelectorTest WalkMapAcceptanceTest WalkMapViewportTest WalkMapBackgroundTest WalkMapSdSourceTest WalkMapDetailTest GrayMapTest -j8 && ctest --test-dir /tmp/crossink-navigator-tests -R 'Navigator|Navigation|Route|WalkMap|GrayMap' --output-on-failure`

  Expected: all relevant navigator tests pass. Do not broaden or repair the known unrelated `DifferentialRoundingTest` linker failure.

- [ ] **Step 2: Clean-build both firmware targets**

  Run: `pio run -e navigator-x3 -t clean && pio run -e navigator-x3 && pio run -e dashboard-x3 -t clean && pio run -e dashboard-x3`

  Expected: both builds exit 0 and report image sizes within their configured partitions.

- [ ] **Step 3: Inspect status and complete diff**

  Run: `git status --short && git diff --stat 51e49a15 && git diff 51e49a15`

  Confirm no BLE/iPhone protocol, persistent-settings, dependency, unrelated, ignored, build-output, or secret files changed; confirm one writer has remained active.

- [ ] **Step 4: Request independent code review and resolve findings**

  Review the complete diff against the approved spec, ESP32-C3 memory/integer constraints, renderer geometry, and test evidence. Fix every Critical/Important finding and rerun affected tests/builds.

- [ ] **Step 5: Commit and push only after fresh green evidence**

  Run: `git add <only reviewed source/test/docs/i18n/changelog files> && git commit -m "feat: add X3 navigation view toggle" && git push origin feat/dashboard-v3-firmware`

  Do not flash hardware. Record hardware verification as a follow-up requiring fresh OTA metadata/active-slot inspection and recovery-image confirmation.
