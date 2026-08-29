# Manual dashboard refresh from the home screen

Date: 2026-08-26

## Purpose

Give the X3 a way to open a BLE receive window on demand, instead of only on the
quarter-hour timer wake.

The motivating failure: on 2026-08-26 the panel sat at `wo 18:08` for over an
hour. The device opened its windows at 16:31, 16:46 and 17:01 UTC and all three
returned `receiver=timedout` — the phone never appeared. The trace for the week
puts the acceptance rate at 81%, 70% and 55% on 24, 25 and 26 August. The
windows are not the scarce resource; a phone that answers them is.

The phone-side log makes the failure sharper than "the window was missed". After
the 16:31 timeout the auto-watch loop should have armed a fresh session and
written `send timedout package=0` roughly 16 minutes later, then again at 19:03,
19:19 and 19:35 local. **None of those lines exist.** The app's own timer never
fired, so the process was not merely scanning and blind — iOS had suspended it.
On the same day, at 07:00, 08:38 and 14:36 UTC, those lines are present, so the
log does record them whenever the process runs.

The X3 side is healthy. With the USB cable out, the windows at 17:48, 18:04 and
18:19 UTC were accepted three for three, confirmed on both sides (`wake=timer
receiver=accepted` against `send accepted package=1396/1397/1398`).

iOS keeps a background BLE central alive unreliably (see the 2026-08-24 finding),
but a foregrounded app scans without restriction. This feature pairs a window
with the one moment the phone is dependable: the user is holding the device and
can open the app.

A second, smaller motivation: every button press resets `lastActivityTime`
(`src/main.cpp`), which postpones deep sleep, which postpones the next timer
wake. Picking the device up to check whether it refreshed is what stops it from
refreshing. A manual trigger turns that gesture from harmful into useful.

## What the evidence does not establish

Two variables changed together when the cycle recovered: the USB cable came out
*and* three quarters of an hour passed. This run does not isolate which mattered,
and the correlation with USB should not be promoted to a cause. An earlier pass
at this investigation did exactly that — it concluded USB halts the wake cycle,
which the trace refutes outright (timer wakes fired at 16:31, 16:46 and 17:01,
all after `reset=USB` at 16:10).

Why the OS-level scan did not relaunch the app at 16:46 and 17:01 UTC, when it
did at 17:48, remains open. Candidates: weaker advertising with the cable
attached, the phone out of range, or iOS throttling relaunches. Nothing on hand
separates them.

A `recordWatchArmed()` line, added to `DashboardEventLog` and called from
`startPendingAutoSession`, closes that gap for the next occurrence: it makes
"armed and saw nothing" distinguishable from "never armed", which is exactly the
distinction the 2026-08-26 log could not answer.

## Non-goals

This does not make background sync reliable. A manual window still times out
when the phone's central is suspended. The chronic background-BLE problem needs
an architecture decision and is explicitly out of scope here.

No change to the iOS app. `BleDashboardSender` re-arms its auto-watch scan after
every terminal outcome and filters on the service UUID, so a foregrounded app
already discovers a device that starts advertising.

## Architecture

The receiver already exists as a separate OTA partition, reached by
`dashboard_boot::switchToReceiver()` plus `ESP.restart()`. This feature adds a
second way to enter that path and a way to report what happened. It links no new
code into the reader binary.

That constraint is not stylistic. The reader binary is 6,335,728 bytes against a
6,553,600-byte `app0`, leaving 217,872 bytes. NimBLE measured 200,912 bytes when
it was linked into the reader for the 2026-08-07 probe. Linking BLE into the
reader would leave under 17 KB of margin, so the partition split is load-bearing
and every design here works within it.

### Trigger

Power + Up, held on the home screen.

Power + Down is taken by the screenshot combo (`src/main.cpp`, the
`screenshotComboActive` block). The new combo copies its shape — the same
`ComboActive`/`ButtonsReleased` edge detection, the same `RenderLock` — so the
two behave alike and neither can retrigger while a button is still down.

It is gated on `activityManager.isHomeActivity()`, the same permission
`UsbSerialFileTransfer::process` already takes. The combo cannot fire mid-book.

### Requesting the window

On a confirmed press the reader:

1. paints a single line ("Verbinden met telefoon…") under `RenderLock`;
2. calls `dashboard::retainReceiverResult(ReceiverResult::AwaitingWindow)`;
3. sets the manual flag (below);
4. appends a trace line with a new `BootTraceStage`, so the trace distinguishes a
   requested window from a timed one;
5. calls `switchToReceiver()` and `ESP.restart()`.

Step 1 matters because the reboot costs roughly four seconds and several e-ink
flashes. Without a painted marker the user cannot tell a registered press from a
missed one, and will press again — re-entering the reboot.

If `switchToReceiver()` fails, the reader clears the manual flag, restores
`ReceiverResult::None` and stays on the home screen. A failed hand-off must not
leave a flag that makes the next ordinary timer wake report itself as manual.

### Distinguishing manual from automatic

`RTC_CNTL_STORE0_REG` carries the `ReceiverResult` word today, packed as a
24-bit magic plus an 8-bit enum by `encodeReceiverResultWord` /
`decodeReceiverResultWord`, both host-tested.

The manual flag goes in a **separate RTC register, `RTC_CNTL_STORE1_REG`**, with
its own magic, rather than stealing a bit from the existing low byte. Keeping it
out of STORE0 leaves the existing encoding and its tests untouched. Both
registers survive deep sleep and a SW reset and clear on power-on, which is the
retention behaviour the existing flow already depends on.

Accessors live beside the existing ones in `AgendaWakeRetention.h`, which the
receiver build already includes, so both partitions read the same flag.

### Window duration

The manual window is **60 seconds**; the automatic window stays at 20.

Sixty seconds is what it takes to unlock a phone and foreground the app after
pressing the combo. Twenty would require the app to be open beforehand, which
defeats the point.

`ReceiverWindow` currently hardcodes `RECEIVER_WINDOW_MS`. It takes the duration
as a constructor argument instead, and `BleReceiverMain` picks 60 s or 20 s from
the manual flag. `ReceiverWindow::actionAt` stays a pure function over
`(nowMs, accepted)` and remains host-testable.

### The return boot

`main.cpp` already branches on the retained result at the top of `setup()`. The
manual flag adds one case:

| result | manual | behaviour |
| --- | --- | --- |
| Accepted | either | unchanged: `resumeAgendaAfterAccepted` renders the fresh card and sleeps |
| TimedOut | no | unchanged: straight back to Agenda sleep, old card intact |
| TimedOut | **yes** | **paint "Geen verbinding met de telefoon", then sleep** |

The third row is the reason the flag exists. Today a timed-out window sleeps
silently on the old card, which from the user's side is indistinguishable from a
combo that never registered. A requested action that can fail has to say so.

The reader clears the manual flag on every return boot, before acting on it, so
one press can never colour two boots.

The accepted path needs no change. It already renders the updated card and
sleeps — which is the right ending for a manual refresh too: press, put the
device down, and the fresh panel is what it shows.

## Failure handling

- Hand-off fails: flag cleared, result restored, stay on home. Logged.
- Window times out: reported on screen (manual) or silent (automatic); the
  agenda cycle resumes on its normal schedule either way.
- Flag set but result is `None` (receiver crashed before writing a verdict): the
  return boot treats it as a manual timeout, because from the user's side
  nothing arrived and silence is the failure mode being designed out.
- Combo pressed off the home screen: ignored, no reboot.

## Testing

Host tests (native, alongside the existing `AgendaWakePolicy` suite):

- manual-flag encode/decode round-trip, including a garbage register value
  decoding to "not manual";
- `ReceiverWindow::actionAt` returning `Listen` at 59 s and `ReturnTimedOut` at
  60 s for a manual window, and at 19 s / 20 s for an automatic one;
- the return-boot decision as a pure function over
  `(ReceiverResult, manual flag)` producing one of the three table rows above,
  so the branch is checkable without a device.

Hardware verification, on battery, cable out:

- press the combo on home, confirm the marker paints and the device reboots;
- with the app foregrounded, confirm the panel returns showing a card whose
  "Bijgewerkt" time is the moment of the press, and that the trace gains a
  manual-stage line followed by `receiver=accepted`;
- with the app force-quit, confirm the failure message appears and the trace
  shows the manual stage followed by `receiver=timedout`;
- confirm an ordinary timer wake in between still reports as automatic.

## Adjacent finding, deliberately not part of this design

`startPendingAutoSession` defers composing until the X3 is discovered, precisely
so a package's baked-in "Bijgewerkt" time cannot go stale while the session waits
for the next wake. That protection ends at discovery. Once a session has composed
and then fails to complete, it keeps scanning with those bytes for the remainder
of its 16-minute timeout — and since the wake interval is 15 minutes, such a
session always spans the following window carrying older content.

2026-08-26 has an instance. After 1394 was accepted at 16:08 the auto-watch
re-armed, immediately rediscovered the X3 still advertising in the same
20-second window, composed 1395 and tried to send it into a window that was
already closing. Had 1395 landed at 16:31 the panel would have shown 18:08
regardless, because that is when its content was composed.

This is real but separate: it degrades freshness, not delivery, and it belongs in
its own spec against the app repository rather than bolted onto a firmware
change. Noted here so the next reader does not rediscover it from scratch.

## Open risk

The combo shares the power button with the wake-hold check and the settings
shortcut (`readerPowerButtonOpensSettings()`). The screenshot combo already
navigates this with `suppressNextPowerConfirmRelease()`; the new combo needs the
same suppression, and the hardware pass should confirm that a Power + Up press
does not also open settings or trigger a sleep on release.
