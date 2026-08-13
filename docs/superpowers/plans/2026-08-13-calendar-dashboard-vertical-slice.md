# Calendar Dashboard Vertical Slice Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Send the next iPhone calendar event as a validated dashboard package and render it on the X3 dashboard sleep screen through isolated BLE receiver and BLE-free reader images.

**Architecture:** Dependency-free C++ and Swift codecs implement the same 256-byte package and framed transfer protocol. The normal reader in OTA slot `app0` enters the isolated receiver in `app1` through `Back + Power`; two NVS slots preserve last-known-good and the receiver returns to `app0` only after verified persistence.

**Tech Stack:** C++20, PlatformIO, GoogleTest, Arduino-ESP32 BLE/NVS/OTA APIs, Swift 6, SwiftUI, EventKit, CoreBluetooth, XCTest/Swift Testing.

**Spec:** `docs/superpowers/specs/2026-08-13-calendar-dashboard-vertical-slice-design.md`

## Global Constraints

- Work only in the existing `feat/ble-handoff` linked worktree; preserve the dirty main worktree.
- Maximum encoded package size is 256 bytes; no dynamic allocation in firmware codec, BLE callback, persistence, or render paths.
- Reader remains BLE-free; receiver excludes reader, display, SD, settings, fonts, activities, and EPUB units.
- Reader is installed in `app0`; receiver is installed in `app1`; no partition-table redesign.
- Do not upload firmware or mutate the physical device until build and static gates pass.
- Full iOS app build and device execution require full Xcode; Command Line Tools alone may run only portable Swift tests.

---

### Task 1: Shared package codec and transfer state machine

**Files:**
- Replace: `src/spikes/ble_handoff/BleHandoffRecord.h`
- Replace: `src/spikes/ble_handoff/BleHandoffRecord.cpp`
- Create: `src/spikes/ble_handoff/DashboardTransfer.h`
- Create: `src/spikes/ble_handoff/DashboardTransfer.cpp`
- Replace: `test/ble_handoff_record/BleHandoffRecordTest.cpp`
- Create: `test/ble_handoff_record/DashboardTransferTest.cpp`

**Interfaces:**
- Produces: `dashboard::decodePackage(const uint8_t*, size_t, Package&) -> Status`
- Produces: `dashboard::encodePackage(const Package&, PackageBytes&, size_t&) -> Status`
- Produces: `dashboard::TransferAssembler::accept(const uint8_t*, size_t) -> TransferResult`
- Produces: standard CRC-32 and fixed package/field limits from the spec.

- [ ] Write codec tests for exact bytes, round-trip, CRC, lengths, timestamps, UTF-8, controls, schema/template, and package-id comparison.
- [ ] Run the native GoogleTest target and confirm failure because the new interfaces do not exist.
- [ ] Implement the allocation-free codec and run the tests to green.
- [ ] Write transfer tests for START/CHUNK/COMMIT, contiguous offsets, reset, incomplete commit, overflow, id mismatch, and duplicate frames.
- [ ] Run and confirm the transfer tests fail for missing behavior.
- [ ] Implement the fixed-buffer state machine and run all codec/transfer tests to green.
- [ ] Format the touched C++ and commit `feat: add dashboard package protocol`.

### Task 2: Two-slot persistence and OTA boot switching

**Files:**
- Replace: `src/spikes/ble_handoff/BleHandoffNvs.h`
- Replace: `src/spikes/ble_handoff/BleHandoffNvs.cpp`
- Create: `src/spikes/ble_handoff/DashboardSlotSelection.h`
- Create: `src/spikes/ble_handoff/DashboardSlotSelection.cpp`
- Create: `src/spikes/ble_handoff/DashboardBootSwitch.h`
- Create: `src/spikes/ble_handoff/DashboardBootSwitch.cpp`
- Create: `test/ble_handoff_record/DashboardSlotSelectionTest.cpp`

**Interfaces:**
- Consumes: `dashboard::Package`, `dashboard::PackageBytes`, and `dashboard::decodePackage`.
- Produces: `dashboard::readLastKnownGood(PersistedPackage&) -> PersistStatus`
- Produces: `dashboard::persistIfNewer(const uint8_t*, size_t, PersistedPackage&) -> PersistStatus`
- Produces: `dashboard_boot::switchToReader()` and `switchToReceiver()` with validated OTA targets.

- [ ] Write pure slot-selection tests covering empty slots, newest valid id, corrupt newest fallback, invalid selection recovery, duplicate/lower id, and interrupted selection update.
- [ ] Run and confirm failure for missing selection code.
- [ ] Implement pure selection logic and run it green.
- [ ] Replace the single NVS blob with two fixed records plus selection record, using write/commit/readback validation and retaining the old slot.
- [ ] Add a small OTA switch adapter using the repository's existing OTA-data switch mechanism and validating image magic and expected subtype.
- [ ] Build both firmware environments to catch ESP API/link errors.
- [ ] Commit `feat: persist last-known-good dashboard packages`.

### Task 3: Isolated receiver, reader activation, and sleep rendering

**Files:**
- Modify: `src/spikes/ble_handoff/BleReceiverMain.cpp`
- Replace: `src/spikes/ble_handoff/BleHandoffReaderProbe.h`
- Replace: `src/spikes/ble_handoff/BleHandoffReaderProbe.cpp`
- Create: `src/spikes/ble_handoff/DashboardCardRenderer.h`
- Create: `src/spikes/ble_handoff/DashboardCardRenderer.cpp`
- Modify: `src/main.cpp`
- Modify: `src/activities/boot_sleep/SleepActivity.h`
- Modify: `src/activities/boot_sleep/SleepActivity.cpp`
- Modify: `platformio.ini`
- Create: `test/ble_handoff_record/DashboardTextTest.cpp`

**Interfaces:**
- Consumes: transfer assembler, two-slot persistence, boot switching, and decoded package fields.
- Produces: receiver status characteristic `8c9f9d12-7c6d-4c8e-a2cb-49586da45d10`.
- Produces: `BleHandoffReaderProbe::renderAgendaCard(GfxRenderer&) -> bool`, returning false for fallback.

- [ ] Write tests for UTF-8-safe bounded text extraction/truncation and supported-versus-unsupported render decisions; confirm red.
- [ ] Implement the bounded render model helpers and confirm green.
- [ ] Update the receiver to advertise write plus status characteristics, process frames outside callbacks, persist only complete valid newer packages, notify status, and switch to `app0` after `accepted`.
- [ ] Add the early `Back + Power` reader route to validated `app1` switching before expensive initialization.
- [ ] Replace the logging probe with a dashboard reader adapter and render the agenda card only in `DASHBOARD_SLEEP`; preserve the existing dashboard as fallback.
- [ ] Pin reader and receiver upload offsets/targets to their specified OTA slots without changing `partitions.csv`.
- [ ] Build native tests, simulator, reader, and receiver; inspect symbols/build inputs for image isolation.
- [ ] Commit `feat: render transferred agenda card on sleep`.

### Task 4: Minimal native iPhone application

**Files:**
- Create under sibling directory: `../../../../iphone-test-app/Package.swift` and portable core sources/tests
- Create under sibling directory: `../../../../iphone-test-app/X3DashboardApp.xcodeproj/project.pbxproj`
- Create under sibling directory: `../../../../iphone-test-app/X3DashboardApp/` SwiftUI, EventKit, CoreBluetooth, plist, and asset files
- Remove: `../../../../iphone-test-app/.gitkeep`

**Interfaces:**
- Produces: Swift `DashboardPackage.encode()`, `TransferFrameBuilder`, `CalendarCardProvider`, and `BleDashboardSender`.
- Consumes: the exact package offsets, CRC, UUIDs, frame types, and status codes from Tasks 1 and 3.

- [ ] Create portable Swift tests for exact C++ parity vector, CRC, event selection/formatting, no-event card, package-id incrementing, and MTU-sized transfer frames.
- [ ] Run `swift test` and confirm red for missing implementations.
- [ ] Implement the portable model/codec/frame layer and run `swift test` green.
- [ ] Add EventKit authorization and next-24-hours selection with no transfer of private event metadata beyond rendered strings.
- [ ] Add CoreBluetooth scanning, notification subscription, write-with-response sequencing, progress, accepted-only success, retryable errors, and UUID constants.
- [ ] Add the single SwiftUI screen and required calendar/Bluetooth usage descriptions.
- [ ] Run `swift test`; if full Xcode is unavailable, record the iOS project build as blocked rather than passed.
- [ ] Commit the iPhone app in its own repository if one exists; otherwise leave the sibling artifact uncommitted and report that boundary explicitly.

### Task 5: Milestone verification and handoff

**Files:**
- Create: `../../../../measurements/calendar-dashboard-slice.md`
- Modify: `CHANGELOG.md` only if the slice is promoted beyond spike-only build flags.

**Interfaces:**
- Consumes all prior deliverables.
- Produces a dated, evidence-based PASS, FAIL, or NOT RUN record.

- [ ] Run fresh native C++ tests and portable Swift tests.
- [ ] Run fresh simulator, reader, and receiver builds plus targeted static analysis.
- [ ] Record firmware sizes and inspect reader for absence of BLE symbols and receiver build for absence of reader/display/EPUB units.
- [ ] If an X3 and physical iPhone are connected, install the two slots without erasing NVS and execute the spec's one-cycle hardware gate, corrupt/older attempt, heap measurements, and 20 page turns.
- [ ] If either device or full Xcode is unavailable, stop at the verified software boundary and mark hardware/iOS-device gates `NOT RUN` with exact prerequisites.
- [ ] Restore/confirm the BLE-free reader as final device state if any upload occurred.
- [ ] Run `git diff --check`, inspect all diffs and statuses in both repositories/directories, and record verification evidence.
