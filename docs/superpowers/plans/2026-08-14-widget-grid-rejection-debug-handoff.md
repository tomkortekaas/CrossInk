# Widget-grid rejection debug — Handoff

**Date:** 2026-08-14 (evening, fourth session on this feature)
**For:** whoever (human or agent) picks this up next, in a fresh chat with no memory of this session.
**Repos:**
- Firmware: `/Volumes/2TB/Development/Projects/CrossInk`, branch `feat/ble-handoff`, HEAD `7c8121e1` — **6 files modified, uncommitted** (listed below).
- iPhone app: `/Volumes/2TB/Development/Projects/xteink-x3-dashboard-ios`, branch `feat/native-ios-app`, HEAD `59b6054` — **2 files modified, uncommitted** (listed below).

Neither repo's changes from this session are committed. Both prior handoffs (`2026-08-14-widget-dashboard-handoff.md` in this repo, `2026-08-14-widget-grid-composer-handoff.md` in the iOS repo) are now stale — read this one instead, it supersedes both.

## Where this came from

The user reported "no agenda updates on the X3 for a few hours, even after being back in BLE range for hours." Investigating that led here:

1. Reflashing the X3 with current `feat/ble-handoff` HEAD made the plain single-event `TEMPLATE_AGENDA` path work again — but this was **not conclusively diagnosed**. The working theory (firmware/app wire-format mismatch from the same-day free-form-placement change, commits `ba99962` Swift / `7c8121e1` CrossInk) was never proven; the retest only exercised the old agenda path, not the new widget-grid one. Treat "why it broke that first time" as still open, low-priority (it's currently working).
2. The user then tried the widget-grid composer for the first time on real hardware — revealing two real, distinct bugs (one fixed, one open — see below).

## Bug 1 — FIXED: composing a new layout didn't take effect until a stale session finished

`BleDashboardSender.send()` (iOS) was guarded by `guard !isActive else { return }`. The standing background auto-watch (`DashboardViewModel.enableAutoRefresh()` → `startAutoWatch`) keeps a session `waitingForX3` almost continuously, so:
- The manual "Stuur naar X3" button was disabled almost all the time (`ContentView.swift:46`, `.disabled(!hasPackage || sender.isActive)`).
- Saving a new composition (`setComposition`) updated `widgets` state, but an **already in-flight session had already captured its `package` bytes** before the save — so the stale content kept being sent, for up to 16 minutes (the session's `overallTimeout`), before the fresh layout would ever go out.

**Fix applied** (uncommitted): `BleDashboardSender.swift` — factored `send()`'s body into a private `startSession(package:packageId:)`, added a public `resendNow(package:packageId:)` that calls it without the `!isActive` guard. `DashboardViewModel.setComposition()` now calls `refresh()` then `sender.resendNow(...)` immediately, interrupting any in-flight session. Built and installed on the user's iPhone (device id below) — compiles clean, not yet proven against Bug 2 below since sends never got accepted.

## Bug 2 — OPEN: the X3 never accepts a `TEMPLATE_WIDGET_GRID` package

This is the real remaining work. Reproduced repeatedly, consistently:

- Phone connects to the X3's BLE receiver, sends all frames (type=1 header, type=2 data, type=3 commit) — confirmed complete and CRC-valid every time (`TransferAssembler::accept()` returns `Complete`, i.e. `status=2` in the `BLE-RX frame ...` serial log).
- The X3 never sends back an `accepted` status. The phone disconnects (via its own retry-on-disconnect logic, `BleDashboardSender.restartScanningIfActive()`) and reconnects, repeating the *identical* package id over and over (confirmed via serial: `package=55`/`58`/`59` each retried 8-12 times) until either the X3's 20-second receive window times out (`ReceiverWindow.cpp`, `RECEIVER_WINDOW_MS = 20'000`) or the phone's own 16-minute session timeout fires.
- The plain `TEMPLATE_AGENDA` path (single calendar event) still works fine and gets accepted in one shot — this is specific to `TEMPLATE_WIDGET_GRID`.

### What's been tried

`persistIfNewer()` in `BleHandoffNvs.cpp` is the function that decides accept/reject (`BleReceiverMain.cpp:169-178`, dispatches to `notify(0x10/0x12/0x13/...)` on failure, `packageAccepted = true` + `"PERSISTED ..."` log on success). Added `LOG_ERR(...)` calls at every failure branch inside `persistIfNewer` (uncommitted, still in the file) to see exactly which `PersistStatus` was returned — **none of them ever printed**, despite the function definitely being reached (confirmed via object/binary rebuild timestamps and the fact that `TransferStatus::Complete` was logged every time).

**Suspected cause of the missing logs, not the missing accept:** `Logging.cpp`'s `logPrintf()` gates on `if (logSerial) { logSerial.print(buf); }`, and there's a code comment in that file describing exactly this failure mode on other boards ("HWCDC `operator bool` reads false under `pio device monitor`... logs would otherwise be silently dropped, e.g. Sticky"). Raw `Serial.printf(...)` calls elsewhere in the same files (e.g. the `"BLE-RX frame ..."` line in `BleReceiverMain.cpp`) always showed up fine, so the working hypothesis is that `LOG_ERR`/`LOG_INF` specifically get dropped on this board under certain USB-CDC connection states, while raw `Serial.printf` doesn't.

**Fix for the visibility problem (uncommitted, flashed, NOT yet captured):** added one more raw `Serial.printf` directly in `BleReceiverMain.cpp` right after the `persistIfNewer` call:
```cpp
Serial.printf("BLE-RX persistIfNewer status=%u\n", static_cast<unsigned>(persistedStatus));
```
This line is on the X3 right now (as of this session's last flash) but its output was **never actually captured** — every attempt to get a full 15-minute Agenda-wake cycle to align with an active serial capture this session ran out of time (see "Environment gotchas" below for why this kept failing).

### Next step (do this first)

1. Get the X3 into Agenda sleep and capture serial through one full wake cycle while the phone attempts a widget-grid send (see gotchas below for timing).
2. Read the `BLE-RX persistIfNewer status=N` line. Decode `N` against `PersistStatus` (`BleHandoffNvs.h`): `Ok=0, NotFound=1, Stale=2, InvalidPackage=3, OpenFailed=4, ReadFailed=5, WriteFailed=6, CommitFailed=7, VerifyFailed=8`.
3. If `InvalidPackage` (3): the more detailed reason is in the `LOG_ERR("BLEPAY", "peekPackageHeader failed status=%u length=%u", ...)` call at `BleHandoffNvs.cpp:88` — but that's an `LOG_ERR`, which may not show up for the same reason as above. If it doesn't, convert it to a raw `Serial.printf` too (same pattern as the fix above) and reflash. Decode against `Status` (`BleHandoffRecord.h`): `Ok=0, InvalidArgument=1, InvalidSize=2, InvalidMagic=3, InvalidSchema=4, UnsupportedSchema=5, UnsupportedTemplate=6, InvalidLength=7, InvalidTimestamp=8, InvalidText=9, InvalidUtf8=10, InvalidCrc=11, StalePackage=12`.
4. Leading unverified hypothesis: something in the free-form-placement wire-format change (5-byte-per-widget header, `ba99962`/`7c8121e1`) — e.g. the package's internal declared length field vs. actual transmitted size, a CRC mismatch between the Swift and C++ implementations, or `validateNoOverlaps()` wrongly rejecting a legitimate layout. This is a genuinely never-before-exercised code path (both prior handoffs flagged it as untested) — the debug line will settle it with real evidence instead of more guessing.

## Bug 3 — NEW FEATURE, built this session, NOT verified overnight

The user asked for the Agenda BLE wake cycle to only run between 07:00 and 22:00 local time, not around the clock. Implemented (uncommitted):

- `AgendaWakePolicy.h`/`.cpp`: `sleepTimerIntervalUs()` signature changed from `(bool agendaSleep)` to `(bool agendaSleep, uint8_t currentHour, uint8_t currentMinute)`. Outside 07:00–22:00, sleeps in one block straight to the next 07:00 instead of ticking every 15 minutes; the last tick before 22:00 is capped so it doesn't overshoot the window.
- `main.cpp`: new helper `resolveAgendaWakeLocalTime(hour, minute)` reads `halClock.getDateTime()` + applies `SETTINGS.clockUtcOffsetQ` (same offset math as `ReadingStatsUtils.cpp`, not reused directly to avoid pulling in an unrelated module). Falls back to midday (safely inside the window) if the RTC isn't ready, so a clock read failure can never accidentally suppress wake-ups entirely. Both call sites of `sleepTimerIntervalUs` updated.
- `test/ble_handoff_record/AgendaWakePolicyTest.cpp`: updated for the new signature, plus two new test cases (night sleeps straight to 07:00; last daytime tick is capped at the window edge).
- Verified via a throwaway `clang++ -std=c++20` host harness against `AgendaWakePolicy.cpp` directly (compiled, ran, all assertions passed, then deleted — not committed, same discipline as prior sessions; **cmake/nix are still not available in this sandbox**, so the real `ctest` suite has still never been run against this change).
- **Not verified on real hardware overnight** — would need to actually wait past 22:00, or temporarily fake the RTC/hour, to see the long-sleep branch trigger for real. The daytime 15-minute-tick behavior (unchanged for hours inside the window) was implicitly exercised by every BLE test this session, so that part is low-risk.

## Environment gotchas hit repeatedly this session (save yourself the time)

- **X3 USB serial (`/dev/cu.usbmodem31301`) disappears whenever the device deep-sleeps** and reappears on wake. Any serial-capture script needs a reconnect-tolerant loop. Pattern used throughout this session:
  ```python
  import serial, time
  end = time.time() + 960  # capture window in seconds
  buf = b''
  ser = None
  while time.time() < end:
      if ser is None:
          try:
              ser = serial.Serial('/dev/cu.usbmodem31301', 115200, timeout=1)
          except Exception:
              time.sleep(1); continue
      try:
          data = ser.read(4096)
          if data: buf += data
      except Exception:
          try: ser.close()
          except Exception: pass
          ser = None
          time.sleep(1)
  open('/tmp/whatever.log', 'wb').write(buf)
  ```
  Run this via a backgrounded Bash call (`run_in_background: true`), not foreground — captures of a full 15-minute Agenda cycle need up to 16 minutes.
- **The X3 has its own, separate idle-inactivity auto-sleep timeout** (`SETTINGS.sleepTimeout`, `CrossPointSettings.h`), default 10 minutes, unrelated to the Agenda BLE-wake interval. After a fresh flash the device boots into the normal reader (awake) and will NOT enter Agenda sleep (and therefore never starts the BLE wake cycle) until either this idle timeout elapses on its own or the user manually sleeps it with a short power-button press. **Always ask the user to manually sleep it** rather than waiting out the idle timeout — much faster for testing.
- **Flashing requires the X3 to be awake and connected** (`pio run -e default -t upload --upload-port /dev/cu.usbmodem31301` from the CrossInk repo root, using `~/.platformio/penv/bin/pio`). If the port disappears mid-session, ask the user to wake the device (short button press or plug into USB) before retrying.
- A test build previously used `-DCROSSINK_AGENDA_WAKE_INTERVAL_MINUTES=1` in `platformio.ini`'s `env:default` build_flags to speed up iteration (1-minute cycle instead of 15). **This has been reverted** — `platformio.ini` is back to the real 15-minute default, do not reintroduce this without remembering to revert it again before the user resumes daily use.
- iPhone: `xcrun devicectl` device id `837B3AD6-71E0-5E45-8CF6-F309BE23C1B5` ("iPhone 13"), team `F5M939PSPR`. Build: `xcodebuild -project X3DashboardApp.xcodeproj -scheme X3DashboardApp -destination 'id=837B3AD6-71E0-5E45-8CF6-F309BE23C1B5' -allowProvisioningUpdates build`, then find the `.app` under `~/Library/Developer/Xcode/DerivedData/X3DashboardApp-*/Build/Products/Debug-iphoneos/X3DashboardApp.app` and `xcrun devicectl device install app --device <id> <path>`. Launch requires the phone to be unlocked — ask the user rather than retrying blindly.
- cmake/nix are still not available in this sandbox (same as every prior session). Use throwaway `clang++ -std=c++20` harnesses for host-testable C++ logic, delete them after use, never commit them.

## Uncommitted files, this session

CrossInk (`git status --short`):
```
 M src/main.cpp
 M src/spikes/ble_handoff/AgendaWakePolicy.cpp
 M src/spikes/ble_handoff/AgendaWakePolicy.h
 M src/spikes/ble_handoff/BleHandoffNvs.cpp
 M src/spikes/ble_handoff/BleReceiverMain.cpp
 M test/ble_handoff_record/AgendaWakePolicyTest.cpp
```
xteink-x3-dashboard-ios (`git status --short`):
```
 M X3DashboardApp/BleDashboardSender.swift
 M X3DashboardApp/DashboardViewModel.swift
```

None of this has been committed — the user wanted to see this handoff first. The `BleHandoffNvs.cpp` `LOG_ERR` additions are debug-only and can be dropped once Bug 2 is solved (or kept if they turn out to work once the `logSerial` readiness issue is understood); the `Serial.printf` line in `BleReceiverMain.cpp` is the one that actually matters right now.

## Working style established this session (worth continuing)

- Verify on real hardware, don't trust "it builds" — this session's two real bugs were both things that looked fine in code review and only showed up on the device.
- When a `LOG_*` call mysteriously produces no output, don't assume the code path wasn't reached — check whether the logging transport itself is the problem first (raw `Serial.printf` as a fallback proved reliable here).
- State plainly what's verified vs. not in commit messages and handoffs, same as every prior session on this feature.
