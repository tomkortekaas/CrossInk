# X3 Route Foundation Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Import a GPX day walk on iPhone, encode and transfer a compact authoritative route, and render its geometry plus a supplied current position in the native X3 navigator.

**Architecture:** `DashboardCore` parses GPX into a dependency-free `WalkingRoute`, simplifies it without moving the source route, and emits a versioned binary Route Package v1. The existing dashboard receiver launches app0 with opcode `0x04`; the navigator then advertises the existing BLE service, receives the route transactionally, stores it under `/Navigation/Routes/active/` on SD, and projects it into the existing one-bit portrait renderer. Offline regional map data, long-lived BLE state updates, internal emergency fallback, off-route routing, and city planning are separate follow-up plans.

**Tech Stack:** Swift 6/Foundation/XMLParser/XCTest; C++17/GoogleTest; ESP32-C3 Arduino/FreeInk SDK/SD storage; existing CoreBluetooth transfer adapter.

**Spec:** `docs/superpowers/specs/2026-09-03-x3-walking-navigation-design.md`

## Global Constraints

- GPX geometry is authoritative; parsing and simplification may not map-match or relocate it.
- Optimize the normal 10–15 km day-walk case and accept routes up to 40 km.
- Route Package v1 is at most 65,535 bytes and uses bounded counts and UTF-8 lengths.
- Preserve the existing dashboard package protocol and navigation-launch opcode `0x04`.
- Do not change the partition table, `app1` offset/size, or SPIFFS offset/size.
- Do not modify or reset the user's dirty `freeink-sdk` submodule.
- New route storage is one atomic, checksummed SD file capped at 256 KiB; this plan does not write SPIFFS.
- All wire integers are little-endian; coordinates are signed E7 degrees.
- Every Swift/C++ wire-format test uses the same checked-in golden byte vector.
- No device flash is required until the final hardware task.

## Repository map

- Firmware repo: `/Users/tomkortekaas/Development/worktrees/CrossInk/dashboard-v3-firmware`
- iOS repo: `/Users/tomkortekaas/Development/worktrees/xteink-x3-dashboard-ios/3-dashboard-v3-ios-package-data-and-previe`
- Swift domain files live in `Sources/DashboardCore/Navigation/`.
- Swift route-package and framing files live in `Sources/DashboardCore/NavigationTransfer/`.
- Firmware route decoding/storage files live in `src/spikes/navigator/route/`.
- Firmware geometry/rendering files live in `src/spikes/navigator/map/`.
- Existing navigator state/layout remains in `src/spikes/navigator/`.

---

### Task 1: Swift GPX route model and parser

**Files:**
- Create: `Sources/DashboardCore/Navigation/WalkingRoute.swift`
- Create: `Sources/DashboardCore/Navigation/GPXRouteParser.swift`
- Create: `Tests/DashboardCoreTests/Navigation/GPXRouteParserTests.swift`

**Interfaces:**
- Produces: `GeoPoint(latitudeE7: Int32, longitudeE7: Int32, elevationDecimeters: Int16?)`
- Produces: `WalkingRoute(id: UInt32, name: String, sourcePoints: [GeoPoint], displayPoints: [GeoPoint], segmentStartIndices: [Int], maneuvers: [WalkingManeuver], totalDistanceMeters: UInt32, estimatedMinutes: UInt16)`
- Produces: `WalkingManeuverKind: UInt8` with `straight=0`, `left=1`, `right=2`, `slightLeft=3`, `slightRight=4`, `uTurn=5`, `arrive=6`; and `WalkingManeuver(pointIndex: UInt16, kind: WalkingManeuverKind, distanceFromStartMeters: UInt32, name: String)`
- Produces: `GPXRouteParser.parse(data: Data, routeId: UInt32) throws -> WalkingRoute`

- [ ] **Step 1: Write parser tests with literal GPX fixtures**

```swift
func testParsesTrackSegmentsWithoutJoiningTheirBoundaryDistance() throws {
    let route = try GPXRouteParser().parse(data: Data(twoSegmentGPX.utf8), routeId: 7)
    XCTAssertEqual(route.id, 7)
    XCTAssertEqual(route.name, "Duinwandeling")
    XCTAssertEqual(route.sourcePoints.count, 4)
    XCTAssertEqual(route.displayPoints, route.sourcePoints)
    XCTAssertEqual(route.segmentStartIndices, [0, 2])
    XCTAssertLessThan(route.totalDistanceMeters, 500)
}

func testRejectsTrackWithoutTwoValidPoints() {
    XCTAssertThrowsError(try GPXRouteParser().parse(data: Data(emptyGPX.utf8), routeId: 1)) {
        XCTAssertEqual($0 as? GPXRouteError, .insufficientPoints)
    }
}
```

Also cover `trk`, multiple `trkseg`, optional `ele`, escaped names, invalid coordinates, and UTF-8 input. Store fixtures as private multiline strings in the test file.

- [ ] **Step 2: Run the focused test and verify failure**

Run: `swift test --filter GPXRouteParserTests`

Expected: FAIL because `WalkingRoute` and `GPXRouteParser` do not exist.

- [ ] **Step 3: Implement the dependency-free model and streaming parser**

Use `Foundation.XMLParser`; append only valid `trkpt` values, record the first point index of each non-empty `trkseg`, calculate distance within segments using the existing `LocationMath` conventions, clamp elevation to `Int16` decimeters, and initialize `displayPoints == sourcePoints` with an empty maneuver list.

- [ ] **Step 4: Run focused and full Swift tests**

Run: `swift test --filter GPXRouteParserTests && swift test`

Expected: PASS; existing DashboardCore tests remain green.

- [ ] **Step 5: Commit the isolated parser change in the iOS repo**

```bash
git add Sources/DashboardCore/Navigation Tests/DashboardCoreTests/Navigation
git commit -m "feat: parse GPX walks into WalkingRoute"
```

### Task 2: Turn-preserving route simplification

**Files:**
- Create: `Sources/DashboardCore/Navigation/RouteSimplifier.swift`
- Create: `Tests/DashboardCoreTests/Navigation/RouteSimplifierTests.swift`

**Interfaces:**
- Consumes: `GeoPoint`, `WalkingRoute.segmentStartIndices`
- Produces: `RouteSimplifier.simplify(points: [GeoPoint], segmentStartIndices: [Int], toleranceMeters: Double, protectedIndices: Set<Int>) -> [GeoPoint]`

- [ ] **Step 1: Write geometry-preservation tests**

```swift
func testKeepsEndpointsProtectedTurnsAndSegmentStarts() {
    let result = RouteSimplifier().simplify(
        points: routePoints,
        segmentStartIndices: [0, 4],
        toleranceMeters: 8,
        protectedIndices: [2]
    )
    XCTAssertEqual(result.first, routePoints.first)
    XCTAssertTrue(result.contains(routePoints[2]))
    XCTAssertTrue(result.contains(routePoints[4]))
    XCTAssertEqual(result.last, routePoints.last)
}
```

Add cases for a straight 10 km trace reducing substantially, a sharp switchback surviving, duplicate points, antimeridian-safe local projection, and an empty/single-point input.

- [ ] **Step 2: Verify the tests fail**

Run: `swift test --filter RouteSimplifierTests`

Expected: FAIL because `RouteSimplifier` is missing.

- [ ] **Step 3: Implement segment-local iterative Ramer–Douglas–Peucker**

Project latitude/longitude to local meters per segment, split work ranges at endpoints, segment starts, and protected indices, use an explicit array stack rather than recursion, and always return original `GeoPoint` values rather than calculated replacements.

- [ ] **Step 4: Verify focused and full tests**

Run: `swift test --filter RouteSimplifierTests && swift test`

Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add Sources/DashboardCore/Navigation/RouteSimplifier.swift Tests/DashboardCoreTests/Navigation/RouteSimplifierTests.swift
git commit -m "feat: simplify walking routes without moving geometry"
```

### Task 3: Swift Route Package v1 encoder and golden vector

**Files:**
- Create: `Sources/DashboardCore/NavigationTransfer/RoutePackageV1.swift`
- Create: `Tests/DashboardCoreTests/NavigationTransfer/RoutePackageV1Tests.swift`
- Create: `test/fixtures/route_package_v1.bin` in the firmware repo
- Create: `test/fixtures/route_package_v1.json` in the firmware repo

**Interfaces:**
- Consumes: `WalkingRoute`
- Produces: `RoutePackageV1.encode(_ route: WalkingRoute) throws -> Data`
- Produces wire layout:
  - bytes `0...3`: ASCII `X3RT`
  - byte `4`: version `1`
  - byte `5`: flags (`bit0 = has elevation summary`, all other bits zero)
  - bytes `6...7`: header size `36`
  - bytes `8...11`: total package length including CRC32
  - bytes `12...15`: route id
  - bytes `16...17`: point count including origin
  - bytes `18...19`: maneuver count
  - bytes `20...23`: origin latitude E7
  - bytes `24...27`: origin longitude E7
  - bytes `28...31`: total distance meters
  - bytes `32...33`: estimated minutes
  - byte `34`: route-name UTF-8 byte count
  - byte `35`: reserved zero
  - payload: route name, `(pointCount - 1)` pairs of signed `Int16` E5 deltas, then maneuvers
  - maneuver: point index `UInt16`, type `UInt8`, name length `UInt8`, distance from start `UInt32`, name UTF-8
  - final 4 bytes: CRC32 over every preceding byte

- [ ] **Step 1: Write exact-layout, bounds, and determinism tests**

```swift
func testEncodesApprovedGoldenVector() throws {
    let encoded = try RoutePackageV1().encode(Self.sampleRoute)
    XCTAssertEqual(encoded, Self.goldenBytes)
    XCTAssertEqual(encoded.prefix(4), Data("X3RT".utf8))
}

func testRejectsAnUnrepresentableCoordinateDelta() {
    XCTAssertThrowsError(try RoutePackageV1().encode(Self.routeWithHugePointGap)) {
        XCTAssertEqual($0 as? RoutePackageError, .coordinateDeltaOutOfRange)
    }
}
```

Also reject zero points, more than `UInt16.max` points/maneuvers, strings over 255 UTF-8 bytes, package length over 65,535, and nonzero reserved flags.

- [ ] **Step 2: Verify failure**

Run: `swift test --filter RoutePackageV1Tests`

Expected: FAIL because the encoder does not exist.

- [ ] **Step 3: Implement the encoder using existing `CRC32` and little-endian helpers**

Quantize each coordinate to E5 only for delta storage; reconstructing from the origin must stay within approximately 1.2 m per point. Do not encode `sourcePoints`; encode only `displayPoints`. Truncate nothing silently: return a typed error.

- [ ] **Step 4: Generate and check in one golden vector plus readable manifest**

The JSON manifest records every expected field and decoded coordinate. Write the binary from the tested Swift encoder, then inspect its SHA-256 and length before adding it to the firmware repo.

- [ ] **Step 5: Run tests and commit once in each affected repo**

Run in iOS repo: `swift test --filter RoutePackageV1Tests && swift test`

Run in firmware repo: `shasum -a 256 test/fixtures/route_package_v1.bin`

Expected: Swift tests PASS; fixture length and hash match constants in `RoutePackageV1Tests`.

### Task 4: Bounded C++ Route Package v1 decoder

**Files:**
- Create: `src/spikes/navigator/route/RoutePackageV1.h`
- Create: `src/spikes/navigator/route/RoutePackageV1.cpp`
- Create: `test/navigator/RoutePackageV1Test.cpp`
- Modify: `test/navigator/CMakeLists.txt`

**Interfaces:**
- Produces: `DecodeStatus decodeRoutePackageV1(const uint8_t* bytes, size_t length, RoutePackage& out)`
- Produces bounded `RoutePackage` with `std::array<RoutePoint, 4096> points`, `std::array<RouteManeuver, 512> maneuvers`, fixed `char name[64]`, counts, distance, and duration.
- `DecodeStatus`: `Ok`, `NullInput`, `TooShort`, `TooLarge`, `BadMagic`, `UnsupportedVersion`, `BadHeader`, `BadLength`, `BadCrc`, `TooManyPoints`, `TooManyManeuvers`, `BadUtf8Length`, `BadManeuver`, `TrailingPayload`.

- [ ] **Step 1: Add decoder tests against the Swift fixture and corrupt copies**

```cpp
TEST(RoutePackageV1Test, DecodesSwiftGoldenVector) {
  const auto bytes = loadFixture("route_package_v1.bin");
  navigator::RoutePackage route{};
  ASSERT_EQ(navigator::decodeRoutePackageV1(bytes.data(), bytes.size(), route),
            navigator::DecodeStatus::Ok);
  EXPECT_EQ(route.id, 0x01020304U);
  EXPECT_EQ(route.pointCount, 4U);
}
```

Test every status, unchanged guard bytes around input/output, maximum arrays, signed deltas, CRC mismatch, truncated maneuver names, and no heap allocation in the decoder.

- [ ] **Step 2: Verify focused test failure**

Run: `cmake --build build/test --target NavigatorCoreTest && ./build/test/test/navigator/NavigatorCoreTest --gtest_filter='RoutePackageV1Test.*'`

Expected: FAIL at compile because the decoder is absent.

- [ ] **Step 3: Implement cursor-based checked decoding**

Validate header and CRC before mutating `out`; decode into a static/local candidate whose size is reviewed against ESP32 stack constraints, then assign only on success. If the candidate is too large for stack, make the caller own it and decode fields only after full structural validation.

- [ ] **Step 4: Run navigator and full host suites**

Run: `cmake --build build/test --target NavigatorCoreTest && ctest --test-dir build/test --output-on-failure`

Expected: all tests PASS, with only the existing documented optional PBM skip if still present.

- [ ] **Step 5: Commit firmware decoder and shared fixtures**

```bash
git add src/spikes/navigator/route test/navigator test/fixtures
git commit -m "feat: decode X3 walking route packages"
```

### Task 5: Route projection and polyline canvas

**Files:**
- Create: `src/spikes/navigator/map/RouteViewport.h`
- Create: `src/spikes/navigator/map/RouteViewport.cpp`
- Create: `src/spikes/navigator/map/RouteMapRenderer.h`
- Create: `src/spikes/navigator/map/RouteMapRenderer.cpp`
- Create: `test/navigator/RouteViewportTest.cpp`
- Create: `test/navigator/RouteMapRendererTest.cpp`
- Modify: `test/navigator/CMakeLists.txt`

**Interfaces:**
- Consumes: decoded `RoutePackage`, current position E7, logical portrait map rectangle.
- Produces: `CurrentPosition { GeoPoint point; uint16_t accuracyMeters; uint16_t bearingDegrees; }`.
- Produces: `RouteViewport::fitOverview(...)`, `RouteViewport::centered(...)`, and `project(GeoPoint) -> ScreenPoint`.
- Produces: `RouteMapRenderer::draw(RouteCanvas&, const RoutePackage&, const RouteViewport&, const CurrentPosition*)`.
- `RouteCanvas` exposes only clipped `line`, `disc`, and `ring`; it has no display-driver dependency.

- [ ] **Step 1: Write projection tests**

Cover portrait bounds, equal aspect padding, zero-width route bounds, southern/western coordinates, a 40 km route, centered view, and saturation outside the clip rectangle.

- [ ] **Step 2: Write recording-canvas renderer tests**

Assert background is cleared, each segment is clipped, GPX width is visually dominant, segment boundaries are not joined, current-position ring is drawn last, and absent position draws no marker.

- [ ] **Step 3: Verify both suites fail**

Run: `cmake --build build/test --target NavigatorCoreTest`

Expected: compile failure for missing viewport/renderer.

- [ ] **Step 4: Implement integer/fixed-point projection and line clipping**

Use an equirectangular local projection with one cosine scale fixed per viewport; keep intermediate multiplication in `int64_t`. Use Cohen–Sutherland or Liang–Barsky clipping so no primitive receives coordinates outside a small guarded range.

- [ ] **Step 5: Run all host tests and commit**

Run: `ctest --test-dir build/test --output-on-failure`

Expected: PASS.

### Task 6: Integrate real geometry into the existing portrait navigator

**Files:**
- Modify: `src/spikes/navigator/NavScreenRenderer.h`
- Modify: `src/spikes/navigator/NavScreenRenderer.cpp`
- Modify: `src/spikes/navigator/NavigatorMain.cpp`
- Modify: `test/navigator/NavScreenRendererTest.cpp`
- Create: `test/navigator/NavigatorRouteScreenTest.cpp`

**Interfaces:**
- Adds: `NavScreenRenderer::draw(..., const NavState&, const RoutePackage*, const CurrentPosition*)`.
- Preserves: existing overload and byte-identical default proof screen when route is null.

- [ ] **Step 1: Add compatibility and routed-screen tests**

Verify the old overload remains byte-identical, the golden route appears inside the map rectangle, current position overlays the route, status screens remain dominant, and framebuffer guards/padding stay untouched at 792×528 and a non-byte-aligned width.

- [ ] **Step 2: Verify test failure**

Run: `cmake --build build/test --target NavigatorCoreTest && ./build/test/test/navigator/NavigatorCoreTest --gtest_filter='NavigatorRouteScreenTest.*'`

Expected: FAIL because the routed overload is absent.

- [ ] **Step 3: Add a one-bit `RouteCanvas` adapter and integrate overview rendering**

Keep physical/logical portrait conversion in one adapter. Do not duplicate pixel primitives. For this foundation phase, `NavigatorMain` may load the checked golden route when no stored route exists, clearly guarded by `CROSSINK_NAV_ROUTE_PROOF`.

- [ ] **Step 4: Generate and visually inspect a host preview**

Use the existing navigator preview path to produce a PNG from the routed framebuffer. Confirm portrait orientation, thick GPX, visible marker, clean text, and no clipping.

- [ ] **Step 5: Run tests and build navigator firmware**

Run: `ctest --test-dir build/test --output-on-failure && pio run -e navigator-x3`

Expected: tests PASS; firmware stays below the 6.4 MB `app0` limit with build-size output recorded.

- [ ] **Step 6: Commit**

```bash
git add src/spikes/navigator test/navigator
git commit -m "feat: render transferred route geometry in navigator"
```

### Task 7: Transactional route transfer and SD storage

**Files:**
- Create: `Sources/DashboardCore/NavigationTransfer/RouteTransferProtocol.swift`
- Create: `Tests/DashboardCoreTests/NavigationTransfer/RouteTransferProtocolTests.swift`
- Create: `src/spikes/navigator/route/RouteTransfer.h`
- Create: `src/spikes/navigator/route/RouteTransfer.cpp`
- Create: `src/spikes/navigator/route/RouteStore.h`
- Create: `src/spikes/navigator/route/RouteStore.cpp`
- Create: `src/spikes/navigator/NavigatorRouteReceiver.h`
- Create: `src/spikes/navigator/NavigatorRouteReceiver.cpp`
- Create: `test/navigator/RouteTransferTest.cpp`
- Create: `test/navigator/RouteStoreTest.cpp`
- Modify: `src/spikes/navigator/NavigatorMain.cpp`
- Modify: `test/navigator/CMakeLists.txt`

**Interfaces:**
- App0 uses BLE opcodes `0x05 ROUTE_START`, `0x06 ROUTE_CHUNK`, `0x07 ROUTE_COMMIT`; the dashboard receiver's opcode `0x04` remains launch and is not changed.
- START: opcode, route id `UInt32`, length `UInt32`, CRC32 `UInt32`.
- CHUNK: opcode, route id `UInt32`, offset `UInt32`, payload.
- COMMIT: opcode, route id `UInt32`.
- Status values: `0x21 route-ready`, `0x22 route-progress`, `0x23 route-accepted`, `0x24 route-invalid`, `0x25 route-storage-failed`; all use the existing seven-byte status envelope.
- Stores `/Navigation/Routes/active/route.bin` only after package decode and CRC validation.

- [ ] **Step 1: Write matching Swift frame tests and C++ assembler tests**

Use the same golden package and maximum BLE write lengths 20, 185, and 512. Assert contiguous offsets, duplicate-chunk idempotence, gap rejection, wrong-id rejection, over-65,535 rejection, and commit-before-complete rejection.

- [ ] **Step 2: Verify both sides fail**

Run Swift: `swift test --filter RouteTransferProtocolTests`

Run C++: `cmake --build build/test --target NavigatorCoreTest`

Expected: missing protocol types.

- [ ] **Step 3: Implement pure frame builder and bounded assembler**

The assembler owns no filesystem and exposes completed immutable bytes only after CRC verification. Keep dashboard package framing unchanged.

- [ ] **Step 4: Implement SD storage behind a testable file abstraction**

Write `/Navigation/Routes/active/route.tmp`, flush/close, decode from disk, rename to `route.bin`, and delete the temporary file on every failure. Enforce canonical fixed paths and a 256 KiB active-route quota even though Route Package v1 is capped lower. The host test uses an in-memory fake; the hardware adapter uses the existing `HalStorage`/SD-card manager.

- [ ] **Step 5: Add the minimal app0 route receiver**

After app0 starts, advertise the existing service/write/status UUIDs under device name `X3-Navigator`. Accept only the three route opcodes, return the explicit route statuses, and stop advertising after a committed route is loaded. Do not modify `InProcessReceiver` or add periodic navigation-state updates in this task.

- [ ] **Step 6: Run Swift, C++, and firmware builds**

Run Swift: `swift test`

Run firmware: `ctest --test-dir build/test --output-on-failure && pio run -e dashboard-v3-x3 && pio run -e navigator-x3`

Expected: PASS; record both flash/RAM sizes and compare dashboard margin with the pre-change baseline.

- [ ] **Step 7: Commit independently in each repository**

Commit messages: `feat: frame X3 route transfers` and `feat: store X3 routes transactionally`.

### Task 8: iPhone import-to-send application flow

**Files:**
- Create: `X3DashboardApp/Navigation/GPXImportView.swift`
- Create: `X3DashboardApp/Navigation/NavigationRouteViewModel.swift`
- Create: `X3DashboardApp/Navigation/RouteSummaryView.swift`
- Modify: `X3DashboardApp/HomeView.swift`
- Modify: `X3DashboardApp/BleDashboardSender.swift`
- Modify: `X3DashboardApp.xcodeproj/project.pbxproj` to add the three new app source files to the existing `X3DashboardApp` group and Sources build phase.
- Create: `Sources/DashboardCore/Navigation/NavigationRouteCoordinator.swift`
- Create: `Tests/DashboardCoreTests/Navigation/NavigationRouteCoordinatorTests.swift`

**Interfaces:**
- Consumes: security-scoped GPX URL, parser, simplifier, Route Package encoder, route frame builder, existing launch state.
- Produces sequence: select GPX → show summary → send existing navigation launch → reconnect to `X3-Navigator` → transfer route → receive route acceptance → display outcome.

- [ ] **Step 1: Confirm the existing concrete integration path**

Run: `rg -n "navigationLaunch|TransferFrameBuilder|maximumWriteValueLength|CBPeripheral" X3DashboardApp Sources`

Expected: `HomeView.navigationSection`, `BleDashboardSender.startNavigation()`, and `BleDashboardSender` frame writing at its existing `maximumWriteValueLength(for: .withResponse)` call; extend these rather than creating a second BLE manager.

- [ ] **Step 2: Write coordinator state-machine tests**

Cover import failure, summary success, launch acceptance before route transfer, navigator reconnection, transfer progress, transfer rejection, stale callback rejection, cancel, and retry with a new request id.

- [ ] **Step 3: Verify the coordinator test fails**

Run: `swift test --filter NavigationRouteCoordinatorTests`

Expected: FAIL because the coordinator is absent.

- [ ] **Step 4: Implement minimal nontechnical UI and coordinator**

UI copy: `Kies GPX`, route name/distance/estimated duration, `Stuur naar X3`, transfer percentage, `Navigatie gestart`, and actionable failure text. Do not add city planning, map editing, or GPX correction.

- [ ] **Step 5: Run core tests, app build, and simulator smoke test**

Run: `swift test && xcodebuild -project X3DashboardApp.xcodeproj -scheme X3DashboardApp -sdk iphonesimulator -configuration Debug CODE_SIGNING_ALLOWED=NO build`

Expected: all tests and build PASS; import sheet opens and a fixture route reaches summary without BLE hardware.

- [ ] **Step 6: Commit**

```bash
git add X3DashboardApp Sources Tests X3DashboardApp.xcodeproj/project.pbxproj
git commit -m "feat: import and send GPX routes to X3"
```

### Task 9: Safe hardware acceptance

**Files:**
- Create: `docs/testing/x3-route-foundation-hardware-checklist.md`
- Modify code only if a separately reproduced defect is found; use the systematic-debugging workflow before any fix.

**Interfaces:**
- Consumes: built app0/app1 images, physical iPhone, one short GPX fixture, existing full-flash backup.
- Produces: recorded hashes, sizes, serial observations, screenshots, and pass/fail results.

- [ ] **Step 1: Record recoverability and exact binaries before flashing**

Record the existing backup path/hash, device port/MAC, partition table, app0/app1 binary SHA-256, and `git status`. Refuse the flash if `freeink-sdk` changes would be overwritten or if the detected partition table differs.

- [ ] **Step 2: Flash only the navigator image first**

Use the repository's established non-destructive app0 flash command with the explicit offset `0x10000`. Read app0 back and compare the flashed byte range byte-for-byte.

- [ ] **Step 3: Verify compiled route rendering and return behavior**

Launch navigator using the already verified iPhone action. Confirm portrait route overview, marker, buttons, and Back returning to dashboard/reader.

- [ ] **Step 4: Flash dashboard receiver only after app0 acceptance**

Use the established app1 offset `0x650000`, verify the flashed byte range, and confirm ordinary dashboard reception still works before route transfer testing.

- [ ] **Step 5: Exercise an actual GPX transfer end to end**

Import the short fixture, launch app0, reconnect, send the route, compare route shape/metadata to the iPhone summary, reboot both apps, remove/reinsert SD, and verify the navigator reports the missing route without altering existing reader files.

- [ ] **Step 6: Record timing and failure behavior**

Measure GPX parse time, package bytes, BLE transfer time, launch time, render time, and return time. Interrupt one transfer and corrupt one test package; both must leave the previous active route usable.

- [ ] **Step 7: Run final regression suites and commit the acceptance record**

Run: full Swift tests, firmware `ctest`, both PlatformIO builds, and `git diff --check` in both repositories.

Expected: PASS with measured evidence attached to the checklist; no claim about battery life or sub-second cached wake is made by this foundation plan.

## Follow-up plans

After this foundation is accepted, create separate plans in this order:

1. `x3-offline-walkmap`: Noord-Holland OSM extraction, `walkmap` format, SD cell reader, minor-path rendering, USB/Wi-Fi installation.
2. `x3-live-navigation-session`: CoreLocation progress, 10–15 second BLE state updates, Pocket/Active modes, partial refresh, latency and battery measurement.
3. `x3-return-to-gpx`: GPS-noise filtering, off-route thresholds, Valhalla return route, haptics/sound, offline bearing fallback.
4. `x3-city-walking-planner`: MapLibre planning UI and Valhalla route creation through the same `WalkingRoute` pipeline.
