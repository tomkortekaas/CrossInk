# X3 foreground live GPS test build — 2026-09-04

Firmware: navigator-live-view-app0.bin, SHA256 466cc4626bc0330a93d684d24e8db1d8544656cd72a7ed90ab2ef5233e2e8f4a. app0 at 0x10000 only. Built from the current development receiver including LiveNavigationSession, unlike the previous installed map-only snapshot. Do not change app1, bootloader, partition table, NVS or SD files.

Working installed recovery candidate: /Volumes/2TB/x3-map-speed-20260904/navigator-loading-app0.bin (previous user-confirmed map-only version). Device contents must still be checked before installation.

## Behavior

The iPhone Start wandeling action uses the existing route import/transfer path and the same BLE central/peer. Only after route acceptance does it begin a live session. Both ATT write completion and matching application acknowledgement are required before advancing. Each pending frame has a 30-second timeout. No queued GPS history or coordinate logging; frames contain only the current real CoreLocation fix. Accuracy 1–50m, age 0–15s accepted by the sender. The existing C++ cache uses its original 30s freshness limit. RouteProgressTracker computes the off-route flag from source GPX segments.

The X3 OK button renders the latest valid position centered at a 400m walking span. Receipt itself does not redraw. No fresh fix means the route overview and explicit GPS status. A fix that ages out during map drawing is discarded before refresh. The display is a snapshot until OK is pressed again.

Stop ends location immediately and sends STOP after any outstanding frame completes; errors/disconnects close the connection and end GPS. A fresh user start is required after disconnect. This is deliberately a FOREGROUND PHONE test: backgrounding or locking the phone stops the walk. Background walking/battery behavior is not implemented or claimed. Existing dashboard significant-change location service is separate and untouched.

## Verification

- Real CMake NavigatorCoreTest: 198 pass; two existing optional artifact/import tests skipped (missing explicit environment inputs).
- WalkMapBackgroundTest passes, including new 400m view, center marker, null-overview byte identity, footer isolation and existing corrupt map fallback.
- Existing standalone LiveNavigationSessionTest compiled and passed.
- Swift: 773 tests passed, including five new stop-and-wait sender tests; red compile captured before implementation.
- Clean new navigator-x3 build directory succeeded; static RAM 42436 bytes, flash 788289 bytes. New buffer use: no second framebuffer, no new heap allocation in renderer, small stack position structs only. Physical heap/stack/BLE still requires device test.
- Signed iPhone Debug build succeeded for iPhoneOS. Installation/launch recorded separately below.
- DeepSeek was attempted but interrupted after investigation without edits. Codex implemented and reviewed all changes. No reliable worker cost was captured.

## Physical test still required

1. Connect X3 via USB; save/verify installed app0 and OTA metadata before app0 update.
2. Open updated phone app. Choose a local GPX, then Start wandeling. On the X3 reader home choose Ontvangen (or OK to reopen receiver when already in navigator).
3. Keep phone app foreground and allow precise when-in-use location. Wait for GPS status, then press OK on X3. Expect 400m map and real position marker.
4. Walk a short distance and press OK again; position/map should move. Route outside the local map region may have no background but should still show route/marker.
5. Stop wandeling; location updates stop. Press OK on X3 and verify no live position. Back must return to reader. Check disconnect and denied location paths.

No device GPS or power behavior is claimed until user confirmation.

## Installation status

Updated iPhone app installed successfully on paired iPhone 13 via devicectl. X3 USB serial device absent at this checkpoint, so no firmware write or hardware GPS test performed.

## X3 installation completed

X3 appeared at /dev/cu.usbmodem31401. Saved metadata (partition table, NVS and OTA metadata) to preinstall-metadata.bin; no metadata written. Partition layout confirmed app0 0x10000 / app1 0x650000; OTA sequences 83/84, valid state 2, select app1. Saved installed app0 bytes to preinstall-app0.bin (798016 bytes); SHA256 9548e130c1204c30c30822234a79c70f845c28c4a1e03093c378f74c0d8867a5 exactly matches the user-confirmed recovery build.

Installed navigator-live-view-app0.bin to 0x10000 only (800064 bytes). esptool returned success, verified data hash, and reset the device. Firmware installation complete; physical GPS behavior awaits user test. Reader/app1 and SD map asset were not written.
