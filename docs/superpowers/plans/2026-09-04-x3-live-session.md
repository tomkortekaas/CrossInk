# X3 live GPX session software plan

> Execute with the executing-plans workflow. User authorized autonomous safe software work. Codex owns protocol/session architecture, location privacy, BLE integration and acceptance. Delegate only bounded codecs/tests. No device writes, commits or deployment.

**Goal:** Receive real iPhone positions for the accepted GPX and show them on request, independently from map/route transfer.
**Architecture:** Existing BLE central and same peer; explicit user start/stop; small versioned frames; bounded native state cache; existing renderer. Initial software policy suppresses automatic pocket redraws. Actual sleep/current and partial-refresh tuning remain device acceptance items.
**Spec:** Parent `../specs/2026-09-03-x3-walking-navigation-design.md`; the wire contract below narrows its live-state increment.

## Wire contract

All integers little endian. No changes to 0x04 launch or 0x05–0x07 route transfer.

- START: 10 bytes: opcode 0x08, version 1, routeId u32, sessionId u32. IDs nonzero; route must match the validated active SD route. New session can begin only without a route transfer in progress. A repeated identical START is idempotent; a different active session must first stop or expire.
- FIX: 20 bytes: opcode 0x09, sessionId u32, sequence u16, latitude E7 i32, longitude E7 i32, accuracy meters u16, age milliseconds u16, flags u8 (bit0 off-route; bit1 force refresh; only bits 0–1 defined, all other bits rejected). Accuracy 1–50 m, age 0–15000 ms, world-valid coordinates. Accept only the current session. Exact duplicate is idempotent; same sequence with different content is rejected. New sequence uses the forward half of the modulo-65536 range.
- STOP: 5 bytes: opcode 0x0B and sessionId u32; only current session. Duplicate matching STOP is idempotent.
- Status: existing 7-byte envelope: code u8, sessionId u32, sequence u16. Codes 0x26 ready, 0x27 fix accepted, 0x28 stopped, 0x29 invalid. Ready/stopped sequence zero. Each sender must match active session/request; status parsing is not authorization.
- This protocol has no pairing/authentication; trusted test context remains required. No security claim from random IDs or CRCs.

## Tasks

- [ ] Swift codec and golden/status tests in `NavigationTransfer/LiveNavigationProtocol.swift` and matching test file. Require 20-byte transport compatibility and validate Data slices.
- [ ] Portable C++ `LiveNavigationSession` and host tests: matching route/session, version/length/field validation, replay/order/wrap, duplicate equality, local monotonic freshness using receive time plus reported age, disconnect/stop/expiry. Cache only one bounded fix; never invent GPS.
- [ ] Extend the native receiver dispatch on its existing callback queue/main loop, keep SD operations serialized, preserve route-only flow when no live session is requested, and close expired sessions. No BLE deinit on a live callback.
- [ ] Extend existing iPhone central with stop-and-wait live frames and exact same-peer reconnect. No second BLE central. Explicit session lifecycle from the walking UI, with clear failure/cancel handling.
- [ ] Add a thin CoreLocation controller that starts only after user action and stops on end/cancel. Accurate fixes feed the reviewed route progress core; denied/reduced/stale location is explicit. Do not silently repurpose the dashboard's significant-change provider. Location remains local to the iPhone/X3.
- [ ] Render real cached position at a useful walking scale, preserve GPX discontinuities and optional SD background. Stale/no position is explicit. Pocket receipt alone does not redraw; refresh policy and actual driver behavior are distinguished.
- [ ] Run focused Swift/C++ tests, cross-language golden smoke, firmware target compilation and simulator/unsigned phone SDK builds when app integration is wired. Record evidence, new hashes and exact unperformed device acceptance steps. No claims about battery life or physical BLE until measured.

No city routing, fabricated maneuvers, online rerouting service, or new map dependencies are introduced by this increment. Geographic matching at self-crossings remains a heuristic pending walking tests.
