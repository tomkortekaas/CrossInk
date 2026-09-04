# Route foundation checkpoint — 2026-09-03

## Current integration checkpoint (supersedes the historical sections below)

- NavigatorMain now loads the validated SD route at startup and runs the native app0 BLE receiver. No embedded Amsterdam/Tokyo proof or fake GPS is used by this build.
- Native receiver uses the existing UUIDs and seven-byte statuses. Initial readable identity is `21 00 00 00 00 00 00`; callbacks enqueue a bounded 512-byte frame, while the main loop handles file IO/validation. It aborts staged transfers after disconnect, limits idle reception to three minutes, and ends advertising/link after COMMIT acknowledgement grace. No unsafe live-link BLE deinit.
- `NavigationRouteSession` owns separate static active/candidate indices, restores only a validated backup before a new transfer, refuses failed restoration, and re-resolves the old source when failed publication leaves it in backup. Invalid START lengths cannot cause recovery renames.
- Production overview is portrait, has a heavier route stroke, distance/time metadata and an explicit GPS-not-active message. No per-chunk refresh. No-route, unreadable-card and failed-storage screens replace misleading example data. Four inherited bitmap-digit errors were fixed with pixel-level regression coverage.
- Codex verification: 200 navigator tests passed both normally and under AddressSanitizer/UndefinedBehaviorSanitizer. Existing handoff/dashboard tests: 214 passed and one optional preview skipped; 11 manual-window tests passed.
- Cross-language smoke: the actual Swift app importer encoded the synthetic GPX into `smoke-route.bin`; the C++ receiver accepted it using 20-, 185- and 512-byte frames, persisted identical bytes and reloaded both segments. This is a host software-chain test, not physical BLE verification. Independent read-only review found no Critical or Important issues and judged the foundation ready for a first controlled hardware test.
- Clean `navigator-x3` build and final receiver-gate rebuild passed: PlatformIO program flash 756,091 bytes / 6,553,600; firmware.bin 767,856 bytes; static data RAM 42,212 bytes. IDF reports 126,325 bytes total DRAM sections including IRAM-resident code, leaving 194,971 bytes before runtime allocations. These numbers do NOT establish runtime heap headroom or battery life; framebuffer/radio/task allocation must be measured. Boot and accepted-route logs expose free/minimum/largest heap and stack high-water information.
- `dashboard-x3` build passed: firmware.bin 6,269,072 bytes, leaving 284,528 bytes in its existing app slot. Partition table unchanged. No device writes or reader/dashboard logic edits in this integration pass.
- Full repository host build is **not green**: unrelated `DifferentialRoundingTest` fails to link `EpdFontFamily::getTextDimensions/getFallbackCodepoint`. That is outside these navigation changes and is not hidden by the focused passing suites.
- iPhone route integration is implemented in the separate iOS worktree: `Wandelen` / `Kies GPX`, bounded asynchronous import, summary, existing launcher, same-device reconnect, readable image probe, stop-and-wait transfer and explicit `Route op X3` after matching COMMIT status. A running navigator skips launch; a retry that finds dashboard can launch again. Wake-wait and transfer time budgets are separate; late peripheral/characteristic callbacks are guarded, and dashboard payloads wait for receiver identification.
- Codex ran 726 Swift package tests successfully. The standalone smoke harness compiles the actual app importer and feeds the checked-in synthetic GPX through parsing, simplification and encoding; two segments are preserved, oversized input and malformed XML are rejected. The iOS simulator app build passed and the app launched with the `Wandelen`/`Kies GPX` section visible. The file-picker interaction and physical CoreBluetooth flow were NOT exercised; the importer smoke is not an end-to-end BLE test.
- DeepSeek implemented the first iPhone coordinator/status decoder, tests, importer and UI/sender wiring. Codex reviewed it, strengthened image probing/retry/timers/callback ownership and file-read bounds, and ran the real app builds/tests. The worker was interrupted after sandbox build workarounds; its `real_cost=$0` line had **no usage events**, so actual delegation cost is unavailable, not zero.
- A later final rebuild hit internal-disk exhaustion during dSYM generation. Only this turn's temporary iOS build directory was moved to `/Volumes/2TB/x3-route-verification.sSLZ8P/ios`; the final relocated-build result is recorded below. The simulator was returned to its original shutdown state. Internal disk space remains critically low and needs separate user attention; no user documents/caches were deleted.
- Final simulator rebuild at the relocated path: **BUILD SUCCEEDED** (including dSYM). Log: `/Volumes/2TB/x3-route-verification.sSLZ8P/ios-build.log`.
- Final physical-iPhone SDK build (`iphoneos`, generic iOS destination, code signing disabled) also **BUILD SUCCEEDED**. This proves compilation, not installation/signing/Bluetooth on a device. Log: `/Volumes/2TB/x3-route-verification.sSLZ8P/ios-device-build.log`.
- Frozen navigator image and previews: `/Volumes/2TB/x3-route-verification.sSLZ8P/navigator-app0.bin`, `navigator-overview.png`, `iphone-simulator.png`. Navigator SHA-256 matches the table below. Nothing was flashed; app0 installation still requires the existing backup/device/partition checks.
- Remaining hardware procedure: `2026-09-03-route-foundation-hardware-acceptance.md`. Still absent: live GPS, background roads/minor paths, partial-refresh/pocket power policy, authenticated pairing, regional SD maps and routing. BLE controller stays initialized after the receive window: this is not the final low-power implementation.

Current checked build hashes (SHA-256; a rebuild can change embedded build timestamps):

| Artifact | SHA-256 |
| --- | --- |
| navigator firmware.bin | `a93965222adf67282ecce2ac7b619b3573955502def8c7a0b82119c20df57a94` |
| dashboard firmware.bin | `481a93bae53ba304bcb220d95955725ee9c1a6ab040e7d277b5a00ef4e3b3d55` |
| partitions.csv | `8eab8ddac3fe3b14ab1f5b3a108b8982ac7a6851d7e3e28d1848d37f9a62d2ed` |

## Later SD adapter checkpoint

- Added `RouteSdFileSystem` using existing SdMan/FsFile, with canonical-path restrictions, bounded reads/writes, checked write/sync/close and no SD power-off. Clean navigator target compilation includes both adapter and loader objects.
- Added read-only `StoredRoute`: validates bin, otherwise backup; never selects temp and never changes files. Failed reload clears selection. Caller owns validation workspace; no full route buffer or loader heap allocation.
- Codex verified 17 loader tests and all 189 navigator tests, plus a clean navigator-x3 build. Corrected a missing null-destination guard (red/green regression) and an incorrect test expectation: failed loads preserve the preceding successfully decoded candidate, not zero it.
- DeepSeek authored initial code/tests but was interrupted after becoming stuck searching for unavailable test dependencies in its sandbox. Its reported zero cost had no usage events and is NOT evidence of a free run; actual cost unavailable. Codex ran the real tests and target build locally.
- NOT wired into NavigatorMain yet. No device, SD-card write, BLE transfer, flash, or reboot acceptance was performed. Current binary still uses proof setup and unused new code may be removed at link time.
- Critical next integration rule: when loader selects a valid backup next to an invalid bin, preserve/repair that backup BEFORE allowing RouteStore.begin (which otherwise removes backup next to bin). Read-only fallback is not complete transaction recovery. Serialize file/display operations; readiness alone does not prove the card is still inserted. FAT power-loss behavior remains unverified.

## Verified in this checkpoint

- Swift GPX parsing, simplification and Route Package v1 encoding are present from earlier steps.
- Portrait framebuffer route adapter and embedded proof route build successfully. The preview is a technical fixture, not an accepted walking-map design: route stroke/position visibility and a realistic local demonstration route still need improvement.
- Swift `RouteTransferFrameBuilder` and C++ `RouteTransfer` agree on START/CHUNK/COMMIT framing, with bounded streaming into `RouteStore`.
- Host-tested storage covers duplicate chunks, gaps, invalid ids, CRC failure, short writes/reads, quota, failed publication, rollback and best-effort temporary-file cleanup.
- `RouteStore` uses a file abstraction. No actual SD files were written by these tests.

Verification run by Codex after reviewing DeepSeek's implementation and corrections:

- Real GoogleTest: 172 navigator tests passed (including preview export).
- Real Swift package in the iOS worktree: 666 tests passed. Some rejection-test closures emit unused-result warnings; these are not failures.
- Clean `navigator-x3` target build passed. PlatformIO reports 395,244 bytes program flash and 26,720 bytes static data RAM. These are the current proof-image figures, NOT the final BLE/SD runtime budget. Dynamic framebuffer, stacks, radio and retained route indices need hardware measurement; unreferenced transfer code can be removed by the linker.
- No flash, partition change, reader/dashboard source change, or commit in this checkpoint.

## Protocol decisions to preserve

- START is 13 bytes: opcode 0x05, route id u32, total length u32, transport CRC32 u32 (little endian).
- CHUNK is opcode 0x06, route id u32, offset u32, payload. COMMIT is opcode 0x07 plus route id u32.
- Swift minimum total write length is 13 (not 10); 13/20/185/512 and Int.max are tested. X3 accepts frames up to 512 bytes.
- Transport CRC covers ALL transmitted package bytes, including the internal four-byte CRC trailer. Golden transport CRC is 0x2144DF1C; the golden internal content CRC remains 0x4CB0201C. A valid IEEE-CRC-appended package has constant full-file residue; neither checksum provides authentication.
- COMMIT verifies the decoded package route id against START before publication.
- The RouteIndex passed to `handleFrame` is a candidate workspace, never the live active index. It can change on a COMMIT whose later publication fails. Consume/swap it only after RouteAccepted. Caller owns its storage; do not put approximately 9.8 KiB indices on the BLE/task stack.
- Fixed paths: `/Navigation/Routes/active/route.tmp`, `route.bin`, `route.bak`. The package limit is 65,535 bytes; prospective retained-file quota is 256 KiB. Cleanup can fail and is best-effort; the backup must remain recoverable.

## Next steps — not implemented or hardware-verified yet

1. Implement the real file adapter using existing SDCardManager/SdFat, including exact short-write/sync/close behavior and startup recovery/validation of bin/bak. A failed rename is not proof of power-loss atomicity on FAT.
2. Add app0 BLE receiver using the existing service/write/status UUIDs and the same seven-byte status envelope. Serialize SD work and display work safely; do not render from BLE callbacks. Keep the reader/dashboard receiver untouched.
3. Wire candidate/active index ownership, stored-route load and UI failure states into NavigatorMain. Retain safe Back return. Do not present the schematic fallback as a real valid route.
4. Wire iPhone GPX import -> existing launch -> navigator reconnect -> route transfer -> acceptance. The new frame builder is not yet used by the app's BLE sender.
5. Improve visual proof, then hardware test SD removal, interrupted writes, reconnect and return to reader. Do not flash the current world-spanning golden fixture as a usability demo.
6. Live GPS, pocket/active modes, partial-refresh scheduling and regional background maps/minor walking paths are later planned blocks, not delivered by this checkpoint.

The worker's test-build cache was unavailable; Codex verified with a fresh real GoogleTest build at `/tmp/x3-nav-core-verify-20260903`. Do not use the worker's temporary test shim as acceptance evidence.
