# Dashboard V3 handoff before the CrossInk upstream update

## Purpose

Preserve the working Dashboard V3 implementation while the original CrossInk
firmware is updated first. Do not merge or delete either feature worktree until
the upstream update has been integrated and the hardware path has been tested
again.

## Working branches

### Firmware

- Repository: `/Volumes/2TB/Development/Projects/X3/firmware`
- Worktree: `/Users/tomkortekaas/Development/worktrees/CrossInk/dashboard-v3-firmware`
- Branch: `feat/dashboard-v3-firmware`
- Base branch: `feat/ble-handoff`
- Base commit: `7ea9be21`
- Dashboard V3 tip at handoff: `510c983f`
- Upstream remote: `https://github.com/uxjulia/CrossInk.git`

Dashboard V3 firmware commits after the base:

1. `b1683942` decode Dashboard V3 packages
2. `b94e29c9` fixed V3 geometry
3. `fc7badb2` dashboard message icon
4. `652e4f21` renderer core
5. `f5088078` content rows
6. `ed4538d3` PBM preview test
7. `3f62509e` visual hierarchy
8. `eeaf688c` template-5 dispatch
9. `a66d3b0f` first X3 hardware refinements
10. `510c983f` latest hardware layout and device-battery fixes

Template 5 is V3. Template 4/V2 remains available as the fallback and must not
be removed during the upstream update.

### iOS

- Repository: `/Volumes/2TB/Development/Projects/X3/ios`
- Worktree: `/Users/tomkortekaas/Development/worktrees/xteink-x3-dashboard-ios/3-dashboard-v3-ios-package-data-and-previe`
- Branch: `issue-3-dashboard-v3-ios-package-data-and-previe`
- Base: `origin/feat/native-ios-app` at `2de4bf7`
- Tip at handoff: `a420fe4`

The iOS branch contains the bounded V3 package, preview and composer, makes V3
the primary send template, and supplies structured WeatherKit weather data,
Buienradar rain buckets and TomTom route data. Unknown heating, markets,
vehicle battery and home battery are deliberately not invented. WhatsApp is
included only when package space permits.

## Hardware state at handoff

- Device: XTEINK X3, MAC `D4:05:92:90:13:74`
- USB port used: `/dev/cu.usbmodem31401`
- Firmware `510c983f` was successfully built and flashed on 2026-08-25.
- The signed iOS app from `a420fe4` was installed and launched on the paired
  iPhone 13 (`837B3AD6-71E0-5E45-8CF6-F309BE23C1B5`).
- Latest firmware size: 23.2% RAM, 96.4% flash, 220,608 bytes free in the OTA
  application partition.

Latest hardware-oriented changes:

- Header date is one compact line (`DI 25 AUG`) without a colliding calendar
  icon.
- Empty traffic data no longer produces large dash-shaped black blocks.
- `THUIS` and `STAPPEN` have wider text bounds.
- The footer quote uses the real, readable Lexend 12px rung.
- The X3 status row overrides only its own value with the hardware battery
  percentage; iOS does not pretend that the phone battery is the X3 battery.
- During daylight the sun column shows today's sunset. Before sunrise it shows
  today's sunrise; after sunset it shows tomorrow's sunrise.

## Verification already completed

- Firmware host suite: 163 passed, 1 optional PBM-export test skipped.
- Firmware target build: successful for `spike-ble-reader-x3`.
- iOS Swift suite: 457 tests, 0 failures.
- Unsigned generic iOS build: successful.
- Signed iPhone 13 build, installation and launch: successful.
- Flash verification: image hash verified and X3 hard-reset successfully.

## Critical uncommitted submodule change

The firmware worktree intentionally reports `m freeink-sdk`. This is not a
Dashboard V3 residue and must not be staged, reset or discarded as part of the
parent CrossInk branch.

Inside `freeink-sdk`, three files contain the X3 battery-latch/timer-wake fix:

- `libs/hardware/BoardConfig/include/BoardConfig.h`
- `libs/hardware/PowerManager/include/PowerManager.h`
- `libs/hardware/PowerManager/src/PowerManager.cpp`

The patch declares GPIO13 as the X3 battery latch, prevents the sleep rail
shutdown from cutting a pin that is actually a power latch, and adds
`holdPowerRailsForTimerWake()`. USB power can mask the original failure. Preserve
this patch separately before changing or updating the submodule.

## Recommended upstream-update sequence

1. Record or export the dirty `freeink-sdk` patch before touching submodules.
2. Update the original CrossInk base branch in its main worktree from `upstream`.
3. Integrate that result into `feat/ble-handoff` first and run its tests.
4. Rebase or merge `feat/dashboard-v3-firmware` onto the updated
   `feat/ble-handoff`; do not flatten away template 4.
5. Reapply or reconcile the battery-latch patch against the updated
   `freeink-sdk` revision. Upstream may now contain overlapping power-management
   work, so inspect this conflict semantically rather than accepting one side.
6. Run the host suite and the `spike-ble-reader-x3` target build.
7. Confirm the OTA partition still fits. The current margin is small enough that
   a larger upstream image may require reclaiming flash; a prior project change
   already increased the limit, so check partition configuration before removing
   V3 features.
8. Flash the X3, launch the V3 iOS app, resend once, and review a fresh hardware
   photo before merging either feature branch.

## Commands for resuming verification

Firmware host suite:

```sh
export PATH="$HOME/.platformio/packages/tool-cmake/bin:$PATH"
cmake --build /private/tmp/crossink-v3-host-build --target BleHandoffRecordTest -j8
/private/tmp/crossink-v3-host-build/ble_handoff_record/BleHandoffRecordTest
```

Firmware target build:

```sh
$HOME/.platformio/penv/bin/pio run -e spike-ble-reader-x3
```

Firmware upload:

```sh
$HOME/.platformio/penv/bin/pio run -e spike-ble-reader-x3 -t upload --upload-port /dev/cu.usbmodem31401
```

iOS tests:

```sh
HOME=/private/tmp/swifthome swift test --disable-sandbox
```

## Remaining acceptance step

The latest flashed layout still needs one new photograph after a successful V3
Bluetooth send. Judge the real panel for date centering, complete status labels,
quote fit, live weather/rain/traffic values and whether WhatsApp remains useful
with the available space.
