# A fresh card at the moment the device is put down

Date: 2026-08-30

## Purpose

Open one BLE receive window when a session ends, so the sleep card the user
looks at is the one composed a moment ago rather than the one that happened to
land before they picked the device up.

The quarter-hour grid only runs during deep sleep. `sleepTimerIntervalUs` is
called from `enterDeepSleepInternal` (`src/main.cpp:830`) and nowhere else, so a
device that is awake has no timer at all. An hour of reading is an hour of no
attempts, and the card rendered on the way into sleep is whatever
`readLastKnownGood` returns — up to an hour old at the exact moment the user
starts looking at it.

Every reading session ends at that same function. Either the user presses power,
or the inactivity timer fires (`src/main.cpp:1429`); both call `enterDeepSleep`.
There is no path that leaves a session without passing through it. One window
placed there covers both "fresh when I put it down" and "no gap after a long
session", and it works away from home, because the phone stays the source.

## What the user gets

- Put the device down after reading; within roughly half a minute the panel
  shows a card composed at that moment.
- Nothing new on screen: no message, no combo to remember, no extra state. The
  refresh is visually identical to an ordinary quarter-hour wake, which the
  device already performs unseen.
- A glance costs nothing. Picking the device up and putting it down two minutes
  later does not open a window, because the card is still fresh.
- One attempt, not a retry loop. If the phone does not answer, the old card
  stays and the normal grid resumes. No failure message on a panel that sits on
  a desk for the rest of the day; the trace file is where a failure is recorded.

## Two findings that shape the design

### The accepted path re-enters the function being changed

After a window is accepted, `src/main.cpp:1196` calls
`enterDeepSleepInternal(false, true)` to render the fresh card and sleep. That is
the same function that would decide to shorten the sleep. A rule that only asks
"are we going to sleep after a session?" is true again there, and the device
refreshes forever.

The freshness rule below is what breaks this, and it does so without a flag: by
the time that second call runs, the accepted package is seconds old, so the rule
declines and the ordinary grid interval is used.

### The in-process route has no handler for a timed-out window

The partition route has one. `src/main.cpp:990` catches a return boot carrying
`ReceiverResult::TimedOut`, re-arms `AwaitingWindow`, and deep-sleeps straight to
the next grid tick without waking the reader.

The in-process route (`src/main.cpp:1074`) has no equivalent. `runReceiverWindow`
returns, the result is retained at `src/main.cpp:1086`, and nothing reads it.
Execution falls through to the SD mount, settings load, `setupDisplayAndFonts`
and the `BootResume` switch. Because `AGENDA_SLEEP` is not a quick-resume mode,
`APP_STATE.showBootScreen` is true (`src/main.cpp:794`), so the resume is
`BootResume::Splash`: a full reader boot, splash included, left on the home
screen until the inactivity timeout puts it back to sleep.

Acceptance was 81%, 70% and 55% on 24, 25 and 26 August, so this would be one
wake in four or five.

**Not confirmed on hardware.** This is read from the code; the trace file on the
device can settle it, and the hardware pass below does. If it holds, the fix
belongs in this change regardless of the rest: without it, adding a window at
standby makes the failure case worse — every unanswered attempt would end with
the reader on the home screen instead of the dashboard on the panel.

## Architecture

### Mechanism

In `enterDeepSleepInternal`, when the sleep is an agenda sleep and the card is
stale, `timerWakeUs` is set to a short delay instead of the grid interval.
Nothing else changes. The wake that follows is the existing timer path: route
chosen by `chooseAgendaBootRoute`, window run in-process before the SD mount with
the whole heap free, fresh card rendered by `src/main.cpp:1194`.

No new receive code and no new render code. The change is which number goes into
`powerManager.startDeepSleep`.

Opening a window in place, without sleeping, was rejected: at that point the
reader is fully loaded, and the reboot is precisely what buys back the heap the
receiver needs. This is also why the delay cannot be zero — it is a real deep
sleep and a real boot.

The delay is 2 seconds, after the panel has settled
(`POST_SLEEP_SCREEN_SETTLE_MS` already runs on this path).

### The freshness rule

Shorten only when the age of `PackageHeader::generatedAt`
(`src/spikes/ble_handoff/BleHandoffRecord.h:99`) is at least
`wakeSettings.intervalMinutes`. `resolveWakeSettings` already reads the persisted
package at this point (`src/main.cpp:780`), so no new read is introduced, and the
clock is available.

`generatedAt` is UTC epoch seconds — `DashboardV3Renderer.cpp:141` shifts it into
local time for display, and the RTC reads UTC — so the age is a subtraction with
no offset involved. Not `validUntil`: that is the phone's opinion of how long the
content stays usable, while the question here is how long ago the grid last
succeeded, which is what `intervalMinutes` is measured against.

One number, doing three jobs:

- No new setting. A card younger than the interval is one the grid would not have
  refreshed either.
- A glance costs nothing.
- It breaks the accepted loop, as described above.

**Fail-safe direction.** If the age cannot be established — no package, a read
failure, a clock that is not available or that reports a `generatedAt` in the
future — the rule declines and the normal grid interval is used. The failure mode
of this feature is a 2-second wake loop that flattens the battery overnight, so
every uncertainty resolves toward the existing behaviour.

### The missing handler

Mirror `src/main.cpp:990-1004` for the in-process route: on `TimedOut`, re-arm
`AwaitingWindow`, resolve the wake time from the NVS-mirrored offset, and
`startDeepSleep` to the next grid tick. The reader is never brought up.

This runs before the SD mount, like the handler it mirrors, so it reads the same
NVS mirrors rather than `SETTINGS`.

### Exactly one attempt per put-down

Follows from the two rules together, not from a counter:

- Accepted: the card is seconds old, the freshness rule declines, normal grid.
- Timed out: the new handler sleeps to the next grid tick and never reaches
  `enterDeepSleepInternal`.

### Build flag

The whole feature sits behind `CROSSINK_STANDBY_REFRESH`, defaulting on in
`env:dashboard-x3`. Rolling back is then a rebuild, not a revert. The missing
in-process timeout handler is **not** behind the flag: it is a fix to existing
behaviour and is wanted either way.

### Trace

A new `BootTraceStage` for a standby-requested window, so the trace can separate
the acceptance rate of this moment from that of the grid. Without it, a week of
running produces no evidence about whether the feature works, which is the whole
reason for the change.

## Failure handling

| situation | behaviour |
| --- | --- |
| Card younger than the interval | No window. Normal grid interval. |
| Age cannot be established | No window. Normal grid interval. |
| Window accepted | Fresh card rendered, sleep to the next grid tick. |
| Window times out | Old card stays, sleep to the next grid tick, reader never wakes. |
| Not an agenda sleep | Unchanged in every respect. |

## Testing

Host tests, alongside the existing `AgendaWakePolicy` suite, both over pure
functions so the branches are checkable without a device:

- the shortening decision over `(agendaSleep, packageAgeMinutes, intervalMinutes,
  ageKnown)`, including every fail-safe input resolving to "do not shorten";
- the in-process return decision over `ReceiverResult`, producing sleep-to-grid
  for `TimedOut` and continue-boot for `Accepted`.

Hardware, on battery, cable out:

- read for over 15 minutes, press power: the trace shows the standby stage
  followed by `receiver=accepted`, and the card's "Bijgewerkt" is the moment of
  the press;
- with the app force-quit: exactly one attempt, then the grid; the panel keeps
  the old card and the device does **not** reach the home screen. This is also
  the run that confirms or refutes the second finding above;
- wake, glance, put down within the interval: no window, no extra boot;
- overnight on battery: the wake count in the trace matches the grid plus one
  per put-down, with no runaway.

## Rollback

- The SDK patches carrying the battery-latch wake fix are unpushed submodule
  working-tree changes. Verified 2026-08-30 as byte-for-byte identical to
  `X3/sdk-patches/crossink-freeink-sdk-battery-latch.patch`, so a rebuild cannot
  silently lose them.
- The source state that works today is `9c8749bb`.
- A full 16 MB flash dump was taken on 2026-08-30, before flashing, to
  `X3/backups/x3-flash-backup-2026-08-30-16MB.bin` (sha256 `3be97841…`). All three
  images in it carry a valid `0xE9` header. It is the only artefact that restores
  the previously-running binary exactly.
- `partitions.csv` still carries two 6.4 MB app slots, and
  `dashboard_boot::switchToReader/switchToReceiver` flip `otadata`, so a known-good
  build can be kept in the other slot.
- Neither the bootloader nor the partition table is touched, so a USB reflash
  remains available in every case.

### Which slot boots, and how to tell what is on it

**The device boots `app1`, not `app0`.** The `otadata` entries read sequence 67 and
68, both valid, and the IDF selects `(seq - 1) % 2` — so 68 picks `ota_1`. The
older build in `app0` is the in-device fallback, reachable through
`dashboard_boot::switchToReader()`.

This matters because `board_upload.offset_address = 0x10000` is `app0`, and
`pio run -t upload` writes there **regardless of the env's offset** — a trap
already hit once and documented in
`docs/superpowers/plans/2026-08-15-widget-grid-scale-up-handoff.md`. Flashing that
way leaves the new firmware in the slot that does not boot, and the device comes
back running the old image, which reads exactly like a feature that does not work.
Flash `app1` explicitly:

```bash
esptool --chip esp32c3 --port /dev/cu.usbmodemXXXXX --baud 921600 \
  write-flash 0x650000 .pio/build/dashboard-x3/firmware.bin
```

To identify what is on a slot, read the ESP-IDF `esp_app_desc_t` at the start of
the image: `version[32]` at offset **0x30**, `project_name[32]` at **0x50**.

```bash
xxd -s 0x10030 -l 32 x3-flash-backup-2026-08-30-16MB.bin    # app0: v1.5.0-105-gafc43af5-dirty
xxd -s 0x650030 -l 32 x3-flash-backup-2026-08-30-16MB.bin   # app1: v1.5.0-114-gd5c98531-dirty
```

Both read out of the pre-flash dump above, which is what those two slots held
before this change was written to `app1`.

IDF derives that version from `git describe`, so it appears in no source file and
`grep` will not find it. One caveat, learned the hard way here: an **incremental**
PlatformIO build can carry a stale descriptor. A `firmware.bin` was observed
reporting `v1.5.0-114-gd5c98531` while `git describe` said `127-gfad6607b`; a full
rebuild produced the correct value. Rebuild before treating the version as
evidence, and note that it says nothing about which `-D` flags were set — for that,
`pio run -e <env> -t idedata`.

## Non-goals

Background BLE reliability. A window at standby still times out when the phone's
central is suspended; this feature adds a moment, it does not fix the phone.

No change to the iOS app.

## Deliberately separate: keeping the radio alive while reading

Measured on hardware 2026-08-29: linking BLE into the reader costs 27,360 bytes
of heap whether or not the radio is switched on, and only 2,688 of those come
back from `esp_bt_mem_release`. Keeping the radio usable across a whole boot
therefore costs 2,688 bytes, not 27.4 KB — a `deinit(false)` and the removal of
two one-shot guards in `InProcessReceiver.cpp`.

That opens a better shape than more windows: advertise continuously while the
user reads, and let a package that arrives be persisted without being rendered.
The phone then delivers whenever iOS lets it run, instead of having to strike a
20-second window.

It is not part of this spec because one measurement decides it. `setPowerSaving`
already force-disables frequency scaling whenever WiFi is up
(`lib/hal/HalPowerManager.cpp:55`), and a reading session normally spends most of
its time at 10 MHz (`lib/hal/HalPowerManager.h:35`, no PSRAM on this C3). If a
live BLE stack pins the CPU at full frequency the same way, the cost is not the
advertising — which is a fraction of a milliamp — but the clock, for the whole
session.

The measurement, in order, each step able to make the next unnecessary:

1. Heap: `getFreeHeap`/`getMaxAllocHeap` with a book open, before and after
   `BLEDevice::init`, and whether a chapter still lays out against
   `MemoryBudget::EPUB_TEXT_LAYOUT_MIN_FREE`.
2. Current draw across a reading session with and without a live radio, and
   whether `setPowerSaving(true)` still reaches 10 MHz.
3. If it does not: `CONFIG_PM_ENABLE` with tickless idle, plus the untouched
   NimBLE trim list — `env:dashboard-x3` sets only `BT_ENABLED`,
   `BT_NIMBLE_ENABLED` and `BT_CONTROLLER_ENABLED`, leaving central and observer
   roles, three connections, the security manager and the scan duplicate cache
   all at their defaults on a peripheral-only device. Re-measure RAM, flash and
   current. Flash recovered here can buy back some of the 23 languages the radio
   cost.
