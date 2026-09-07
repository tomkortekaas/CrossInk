# Live Navigation Progress Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Stop remaining-distance jumps on loop routes by transmitting the iPhone's stateful route progress to the X3.

**Architecture:** Negotiate live protocol v3 through START, append route progress to v3 FIX frames, and automatically fall back to v2 when older firmware rejects v3. The X3 uses received progress for route-order metrics while retaining its geometric projection for marker placement, route-distance trust, and legacy sessions.

**Tech Stack:** Swift 6/XCTest/CoreBluetooth/CoreLocation; C++17/GoogleTest/PlatformIO/ESP32-C3; little-endian BLE frames.

**Spec:** `docs/superpowers/specs/2026-09-07-live-navigation-progress-design.md`

## Global Constraints

- START v1/v2 plus 20-byte FIX must remain supported by new firmware.
- A new app must retry START v2 exactly once only after a matching START-v3 `Invalid` response.
- START v3 selects an exact 24-byte FIX containing `distanceFromStartMeters u32` at offset 20.
- GPS remains authoritative for marker, viewport, accuracy, and off-route trust; progress controls remaining route metrics.
- STOP, disconnect, expiry, session replacement, and route mismatch must not retain progress.
- Do not change route-transfer frames, stored route packages, dependencies, or partition layout.
- Flash only navigator app0 at `0x10000` after re-reading the physical partition table and OTA metadata.

---

### Task 1: iOS v3 wire codec

**Files:**
- Modify: `Sources/DashboardCore/NavigationTransfer/LiveNavigationProtocol.swift`
- Test: `tests/DashboardCoreTests/NavigationTransfer/LiveNavigationProtocolTests.swift`

**Interfaces:**
- Produces: `LiveNavigationFrame.start(..., protocolVersion:)` for versions 2 and 3.
- Produces: `LiveNavigationFrame.fix(..., distanceFromStartMeters: UInt32?)`, emitting 20 bytes without progress and 24 bytes with progress.

- [ ] **Step 1: Write failing golden-byte tests**

Add tests asserting START v3 byte 1 is `0x03`; FIX with progress `0x0102_0304` is 24 bytes and ends `04 03 02 01`; and FIX without progress preserves the current 20-byte golden frame.

- [ ] **Step 2: Run the codec tests and verify RED**

Run: `swift test --filter LiveNavigationProtocolTests`

Expected: compile failures because the version/progress parameters do not exist.

- [ ] **Step 3: Implement the minimal codec extension**

Add constants for protocol versions 2/3, legacy/v3 FIX sizes, and progress offset 20. Validate only supported START versions; append the optional UInt32 without changing bytes 0...19.

- [ ] **Step 4: Run the codec tests and verify GREEN**

Run: `swift test --filter LiveNavigationProtocolTests`

Expected: all protocol tests pass.

- [ ] **Step 5: Commit**

```bash
git add Sources/DashboardCore/NavigationTransfer/LiveNavigationProtocol.swift tests/DashboardCoreTests/NavigationTransfer/LiveNavigationProtocolTests.swift
git commit -m "feat: encode live navigation progress"
```

### Task 2: iOS v3 negotiation and v2 fallback

**Files:**
- Modify: `Sources/DashboardCore/NavigationTransfer/LiveNavigationSender.swift`
- Test: `tests/DashboardCoreTests/NavigationTransfer/LiveNavigationSenderTests.swift`

**Interfaces:**
- Consumes: Task 1 codec APIs.
- Produces: sender-owned negotiated protocol version and `fix(..., distanceFromStartMeters: UInt32)`.

- [ ] **Step 1: Write failing sender-state tests**

Test that a new session initially arms START v3; a matching `Invalid` during `.starting` arms START v2 once; successful v3 sends a 24-byte FIX; successful fallback sends a 20-byte FIX; a second rejection fails; and foreign-session or non-start rejection never downgrades.

- [ ] **Step 2: Run the sender tests and verify RED**

Run: `swift test --filter LiveNavigationSenderTests`

Expected: failures on START version, fallback state, or missing progress argument.

- [ ] **Step 3: Implement negotiation state**

Retain route ID and refresh mode until START completes, track whether fallback was attempted, and special-case only the identity-matched `.invalid` response while expecting `.ready`. Re-arm START v2 with fresh write/ack flags; otherwise preserve existing failure behavior. Select optional FIX progress from the negotiated version.

- [ ] **Step 4: Run sender and protocol tests and verify GREEN**

Run: `swift test --filter 'LiveNavigation(Sender|Protocol)Tests'`

Expected: both suites pass.

- [ ] **Step 5: Commit**

```bash
git add Sources/DashboardCore/NavigationTransfer/LiveNavigationSender.swift tests/DashboardCoreTests/NavigationTransfer/LiveNavigationSenderTests.swift
git commit -m "feat: negotiate live progress protocol"
```

### Task 3: Feed tracked iPhone progress into live fixes

**Files:**
- Modify: `X3DashboardApp/BleDashboardSender.swift`
- Test: the narrowest existing app/coordinator test target covering `sendWalkingLocation`; if no callable seam exists, extract a pure internal frame-input helper beside the sender and test it in `tests/DashboardCoreTests/NavigationTransfer/LiveNavigationSenderTests.swift`.

**Interfaces:**
- Consumes: `RouteProgress.distanceFromStartMeters` and Task 2 `fix` API.
- Produces: every accepted real or synthetic fix carries tracker-derived progress.

- [ ] **Step 1: Write a failing forwarding test**

Drive two positions on an overlapping/loop fixture and assert the second emitted FIX contains the tracker's forward progress rather than a fresh nearest-edge choice.

- [ ] **Step 2: Run the focused test and verify RED**

Run the discovered Xcode/Swift test target and confirm the v3 progress bytes are absent or the new call does not compile.

- [ ] **Step 3: Forward accepted tracker progress**

Pass `progress.distanceFromStartMeters` into `walkSender.fix`. Keep refresh policy and off-route behavior unchanged. Route DEBUG synthetic coordinates through the same tracker result.

- [ ] **Step 4: Run the focused test and full Swift suite**

Run: `swift test`

Expected: all package tests pass; also run the repository's documented Xcode test/build command for `X3DashboardApp` to compile the app-only integration.

- [ ] **Step 5: Commit**

```bash
git add X3DashboardApp/BleDashboardSender.swift tests
git commit -m "fix: send tracked walking progress"
```

### Task 4: Firmware v3 session parsing and mailbox support

**Files:**
- Modify: `src/spikes/navigator/LiveNavigationSession.h`
- Modify: `src/spikes/navigator/LiveNavigationSession.cpp`
- Modify: `src/spikes/navigator/LiveFrameMailbox.h`
- Test: `test/navigator/LiveNavigationSessionTest.cpp`
- Test: `test/navigator/LiveFrameMailboxTest.cpp`

**Interfaces:**
- Produces: `LivePosition::{hasRouteProgress,distanceFromStartMeters}`.
- Produces: session-version-aware exact FIX validation and 20/24-byte mailbox coalescing.

- [ ] **Step 1: Write failing protocol/session tests**

Cover START v3 acceptance; v3 progress parsing; v1/v2 rejection of 24-byte FIX; v3 rejection of 20-byte FIX; replay comparison including progress; and clearing progress on STOP/disconnect/route mismatch.

- [ ] **Step 2: Write failing mailbox tests**

Assert valid 24-byte FIX frames from the same session coalesce, a newer 24-byte progress value replaces the older value, and malformed/mixed-session frames still overflow rather than replace.

- [ ] **Step 3: Run both test executables and verify RED**

Run the navigator CMake build followed by `ctest -R 'LiveNavigationSession|LiveFrameMailbox' --output-on-failure`.

Expected: v3 cases fail because only version 2/20-byte frames exist.

- [ ] **Step 4: Implement bounded version-aware storage**

Increase the retained frame to 24 bytes, store negotiated version/length, accept START v3 with the existing refresh byte, enforce exact FIX size by version, compare/copy the full frame, expose parsed progress only for v3, and reset capability on every session exit. Update the size assertion only to the smallest bound that fits the actual object.

- [ ] **Step 5: Extend mailbox validation**

Accept exactly 20 or 24 bytes when all shared FIX fields are valid; preserve same-session replacement and copy the full incoming length.

- [ ] **Step 6: Run focused and full host tests**

Run: `ctest --output-on-failure`

Expected: all navigator tests pass.

- [ ] **Step 7: Commit**

```bash
git add src/spikes/navigator/LiveNavigationSession.h src/spikes/navigator/LiveNavigationSession.cpp src/spikes/navigator/LiveFrameMailbox.h test/navigator/LiveNavigationSessionTest.cpp test/navigator/LiveFrameMailboxTest.cpp
git commit -m "feat: receive live route progress"
```

### Task 5: Use authoritative progress for X3 metrics and arrival

**Files:**
- Modify: `src/spikes/navigator/map/RouteViewport.h`
- Modify: `src/spikes/navigator/NavigatorMain.cpp`
- Modify: `src/spikes/navigator/NavScreenRenderer.cpp`
- Test: `test/navigator/NavScreenRendererTest.cpp`
- Test: `test/navigator/NavigatorRouteScreenTest.cpp`

**Interfaces:**
- Consumes: Task 4 live-position progress.
- Produces: `CurrentPosition::{hasRouteProgress,distanceFromStartMeters}` and one shared remaining-distance selection used by footer and arrival.

- [ ] **Step 1: Write failing footer tests**

Construct a colocated-start/end loop whose geometric projection reports nearly the full route. Assert progress near the end produces near-zero footer distance, proportional time, and arrival; progress near zero produces the full remaining route even while GPS is also near the endpoint.

- [ ] **Step 2: Write failing guard and legacy tests**

Assert progress above total clamps to zero remaining; off-route/untrusted fixes retain whole-route totals and never arrive; no-progress legacy positions continue using `RouteProximity.remainingDistanceMeters`.

- [ ] **Step 3: Run renderer tests and verify RED**

Run: `ctest -R 'NavScreenRenderer|NavigatorRouteScreen' --output-on-failure`.

Expected: loop assertions fail because the renderer uses geometric remaining.

- [ ] **Step 4: Implement one remaining-distance selector**

Extend `CurrentPosition` with bounded progress fields and centralize selection: after existing validity/proximity guards, use `total - min(progress,total)` when progress exists, otherwise use the current clamped geometric remaining. Call the same selector from `chooseFooterMetrics` and `hasReachedRouteEnd`.

- [ ] **Step 5: Propagate live progress from `NavigatorMain`**

Populate `CurrentPosition` from `LivePosition` without changing viewport behavior. Update affected aggregate initializers explicitly so tests cannot accidentally treat a bearing as progress.

- [ ] **Step 6: Run focused and full host tests**

Run: `ctest --output-on-failure`

Expected: all tests pass, including the loop regression.

- [ ] **Step 7: Commit**

```bash
git add src/spikes/navigator/map/RouteViewport.h src/spikes/navigator/NavigatorMain.cpp src/spikes/navigator/NavScreenRenderer.cpp test/navigator/NavScreenRendererTest.cpp test/navigator/NavigatorRouteScreenTest.cpp
git commit -m "fix: retain progress on loop routes"
```

### Task 6: Cross-repository verification and hardware flash

**Files:**
- Verify only; update a testing note only if actual commands/results need preserving.

**Interfaces:**
- Consumes: completed app and firmware implementations.
- Produces: accepted binaries and a recoverable, byte-verified app0 flash.

- [ ] **Step 1: Verify repository scope**

In both worktrees inspect `git status`, recent commits, and the complete diff from the pre-change commit. Confirm no dependencies, partitions, secrets, route-transfer formats, or unrelated files changed.

- [ ] **Step 2: Run complete software verification**

Run the full Swift package suite and documented iOS app build. Reconfigure the firmware host-test build from clean state, run full `ctest`, then clean-build the explicit `navigator-x3` PlatformIO environment. Record binary/RAM/flash sizes and compare them with the previous navigator build.

- [ ] **Step 3: Review protocol symmetry**

Compare golden bytes and constants across Swift and C++: versions 2/3, START length 11, FIX lengths 20/24, progress offset 20, flags offset 19, little-endian interpretation, fallback behavior, and reset semantics.

- [ ] **Step 4: Push both branches**

Push the iOS branch and firmware branch only after all checks pass.

- [ ] **Step 5: Audit attached X3 before writing**

Identify the ESP32-C3 port/MAC, read the physical partition table and OTA metadata, verify app0 is navigator at `0x10000` with size `0x640000`, and create/read-hash a current app0 recovery image. Abort if layout or identity differs from the established device.

- [ ] **Step 6: Flash and verify app0 only**

Write the clean navigator binary explicitly at `0x10000`, read back exactly the binary length, compare SHA-256 byte-for-byte, and re-read OTA metadata to prove it was not modified. Never use the generic PlatformIO upload target.

- [ ] **Step 7: Perform physical acceptance**

Start the loop route with shared start/end, confirm the initial display stays near full distance despite GPS drift, advance through several simulated/real points, return to the shared endpoint, and confirm remaining distance stays near zero across multiple refreshes and main/detail toggles. Also test back/return GPS retention and STOP/disconnect clearing.
