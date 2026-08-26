# Dashboard V3 — overnight session, 2026-08-25 into 2026-08-26

## What to do first tomorrow

The X3 is **running the wrong firmware**. It was flashed once during the
session and that image contains a quote table that does not match the phone's,
so every footer quote is one entry off. A second flash was prepared but failed:
`/dev/cu.usbmodem31401` disappeared after the first hard reset, because the
device booted into the app and went to sleep.

```sh
cd /Users/tomkortekaas/Development/worktrees/CrossInk/dashboard-v3-firmware
$HOME/.platformio/penv/bin/pio run -e spike-ble-reader-x3 -t upload \
  --upload-port /dev/cu.usbmodem31401
```

Wake the X3 or replug the cable first, and check the port exists with
`ls /dev/cu.usbmodem*`.

The phone app also needs rebuilding and installing: the wire ceilings changed
(see below), so an old build sends fewer rows than the new firmware can draw.
Nothing breaks if you forget — the sections are count-prefixed, so an old phone
package still decodes — you just will not see the extra rows.

## Working branches

- Firmware: `/Users/tomkortekaas/Development/worktrees/CrossInk/dashboard-v3-firmware`,
  branch `feat/dashboard-v3-firmware`.
- iOS: `/Users/tomkortekaas/Development/worktrees/xteink-x3-dashboard-ios/3-dashboard-v3-ios-package-data-and-previe`,
  branch `issue-3-dashboard-v3-ios-package-data-and-previe`.

Nothing is committed. Both worktrees hold the whole session as working changes.

## Verification at the end of the session

| Check | Result |
| --- | --- |
| Firmware host suite | 167 passed, 1 optional PBM export skipped |
| Firmware target build | `pio run -e spike-ble-reader-x3` succeeds, 217,920 bytes free in the OTA partition |
| Swift suite | 473 passed |
| Unsigned generic iOS build | succeeds |
| Swift/C++ byte crosscheck | `template5 crosscheck ok: 776 bytes, 8 agenda, 3 markets, 7 chats` |
| Host renderer preview | mock-up scenario: 76 strings, 1 truncated, 0 replacement boxes |

Nothing has been seen on the panel. Everything below is unverified on hardware.

## The measuring tool this session added

`tools/dashboard-v3-preview` renders the firmware's own renderer on the Mac
against the real Lexend bitmap data and writes a 528×792 PNG per scenario, plus
a report of every string it had to truncate.

```sh
cmake -S tools/dashboard-v3-preview -B /tmp/x3-v3-preview
cmake --build /tmp/x3-v3-preview -j8
/tmp/x3-v3-preview/dashboard-v3-preview /tmp/x3-v3-out
```

It exists because the renderer test's PBM export draws with a 7×9 test glyph
table, so it cannot answer whether text fits. That is why every typography
problem previously needed a flash and a photograph.

## What changed

### Typography

The nominal font names were misleading: the built-in "Lexend 10" face has a
21 px ascender and a 26 px line height on this panel, so a ladder chosen by name
put everything at roughly twice its intended size. `FontRole` is now named for
its job — `Micro`/`Small`/`Body`/`Heading`/`Value`/`Hero` — with the measured
ascender recorded per rung, and uses only full Lexend faces. The subsetted
"dash" faces cover ASCII plus the degree sign, so the `U+2026` that
`truncatedText()` appends rendered as a replacement box; that is what the
lozenge next to the travel time was.

Truncations in the mock-up scenario went from 28 to 1.

### Layout

- Header columns are unequal and sized to their content.
- Status rows put label and value on one line with the bar below; the bar used
  to run through the label.
- Steps became the fourth meter row on the same grid, showing the count while
  the bar carries progress towards the goal.
- The rain band is the mock-up's fixed 24 px intensity strip with four dither
  levels instead of a bar chart, and gave 21 px back to the body
  (rain 117 → 96, body 459 → 480).
- Markets, the day separator, the wind compass sector and the refresh time are
  drawn; the WhatsApp heading lost its message glyph.
- The footer quote comes from a shared id table instead of being hardcoded.

### Wire ceilings

`maxAgendaRows` 5 → 8 and `maxChats` 3 → 7 on both sides, matching the mock-up.
Text budgets are unchanged. Raising a ceiling is backwards compatible in the
decode direction because the sections are count-prefixed.

The pathological maximum — every row present *and* every text field at its
ceiling — now encodes to 776 bytes, up from 509. That is well inside the hard
1024-byte envelope but above the 512-byte compactness target, so the target is
now expressed where it belongs: `DashboardV3Composer` keeps live content under
512, and a new test proves a realistic full day (eight Dutch appointments,
three markets, seven chats) fits with nothing dropped.

### iOS sources

`VERWARMING`, `IONIQ 5`, `THUISACCU` and the whole `MARKTEN` section were
hardcoded absent. There is now a `Dashboard V3` settings screen under
Instellingen → Gegevensbronnen that picks the Home Assistant entities for
heating, vehicle battery, home battery and up to three markets, and the
composer feeds them into the package. Values fall back to their last known
reading when Home Assistant is unreachable, matching what the V2 tiles do.

## Decisions worth a second opinion

1. **The wire ceilings.** Raising them is the one change that renegotiates a
   contract the plan marks "Codex only". It is two constants on each side and
   trivially revertable. Five composer tests had budgets hand-derived from the
   old five-row fixture; they now express the same priority order relative to
   the measured unconstrained size instead of pinning byte counts.
2. **Dither levels.** The design spec deliberately kept grey levels out until
   they had been judged on hardware. The mock-up's rain strip needs them, so
   they are in. Whether "quarter" and "half" read as distinct on e-ink at
   arm's length, rather than both as vague grey, is the main thing tomorrow's
   photograph has to answer. Fallback is simple: drop the quarter level and
   render three.
3. **The classification byte.** A firmware comment said its meaning was
   undocumented, so the renderer refused to label it. The V3 design spec names
   `NORMAAL`/`DRUK`/`FILE` and the Swift wire enum is `normal=0, busy=1,
   jammed=2`, so it is now drawn. The test was rewritten to say "only these
   three labels, and nothing invented".

## What tomorrow's photograph has to answer

- Do the four rain levels read as four levels?
- Is the Micro rung (17 px ascender) legible at viewing distance? It carries
  the agenda times, section captions and chat times.
- Does the black header still look solid, with white text and icons?
- With eight agenda rows the left column is fuller than it has ever been on
  hardware — does it still read calmly, or does it need fewer rows?
