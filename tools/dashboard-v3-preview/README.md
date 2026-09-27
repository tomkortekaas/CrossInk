# Dashboard V3 preview

Renders the dashboard V3 layout on the host with the firmware's own built-in
Lexend Deca faces and writes a 528x792 PNG per scenario, plus a report of every
string that had to be truncated.

```sh
cmake -S tools/dashboard-v3-preview -B /tmp/x3-v3-preview
cmake --build /tmp/x3-v3-preview -j8
/tmp/x3-v3-preview/dashboard-v3-preview /tmp/x3-v3-out
```

## Scenarios

Legacy format-2/3 fixtures (`mockup`, `empty`, `maximum`, `before-sunrise`,
`solo-market`, `missing-lead-market`) keep the V3 band layout. The format-5
fixtures exercise the 8A design:

- `agenda-8a` — the demonstration day (Sunday 2026-09-13 11:15 UTC) with all
  bands populated and one soft agenda block.
- `agenda-8a-allday` — the same day plus an all-day row and one agenda row with
  an unknown duration, so the ribbon draws a mark instead of an interval.
- `agenda-8a-fill` — six appointments today and seven tomorrow, so the last rows
  fold into the standalone `+N meer` line.
- `agenda-8a-one`, `agenda-8a-six`, `agenda-8a-eleven` — one day holding that
  many appointments, the counts the readability refinement calls out: the zebra
  pattern and the band's fit at each.
- `agenda-8a-long-title` — a full-width wire title in both the focus row and an
  ordinary row, the longest thing those two boxes ever have to set.
- `agenda-8a-same-start` — a double booking: two appointments at one minute, each
  keeping its own row.
- `agenda-8a-no-stove` / `agenda-8a-no-heating` — the stove verdict withheld
  (nothing drawn) and forbidden (the flame with a strong diagonal strike), both
  with no text on the badge.
- `empty-8a` — every missing marker the wire format has.
- `missing-8a` — every band present but one value in each band missing.
- `no-movers-8a` — the demonstration day without the strongest-mover block.
- `extreme-8a` — the wire maximums (longest labels/durations, biggest counts,
  edge-of-range signed values).

The normal-value 8A fixtures (`agenda-8a`, `agenda-8a-fill`, `agenda-8a-allday`,
`agenda-8a-one`, `agenda-8a-six`, `agenda-8a-eleven`, `agenda-8a-long-title`,
`agenda-8a-same-start`, `agenda-8a-no-stove`, `agenda-8a-no-heating`,
`no-movers-8a`) must end with zero truncated strings; the tool prints that
verdict per scenario and returns a non-zero exit status if one truncates.
`extreme-8a` is expected to truncate and is reported without failing the run.

## 8A measurement section

For every format-4-or-newer scenario the report also prints an explicit 8A measurement
section:

- the rain band's single row: the outlook string `formatRainLine` returns, its
  measured width against the budget the row leaves it, both of the window's
  clocks, the width the intensity strip gets (with the renderer's 96 px minimum),
  and which heating badge the package asks for (flame, struck flame, or nothing);
- the four fixed market slots (AEX, S&P, NDX, BTC) and the mover block at the
  Micro rung, their sum, and the market band's available width (the market band
  uses a 10 px pad, not the 14 px pad used elsewhere);
- the KPI left column's reserved value width and the aligned bar geometry, plus
  the right column's value strings measured against their Body-bold boxes;
- the in-agenda focus row's time width, the hairline position that results from
  it, and the location its second line leads with (the caption text itself is
  the renderer's decision, so the picture shows the finished line).

All widths come from `FontBook::textWidth`, the same real Lexend metrics the
draw path uses.

## Why it exists

The renderer test's PBM export draws with a 7x9 test glyph table, not with
Lexend, so it cannot show whether text fits. Every typography problem therefore
only appeared after a flash and a photograph.

The nominal font sizes are misleading: the built-in "Lexend 10" face has a 21 px
ascender and a 26 px line height on this panel. A ladder chosen by name puts
every string at roughly twice its intended size. Run the tool with no arguments
except an output directory and it prints the measured ladder first.

## What it proves, and what it does not

It uses the real `EpdFontFamily` for measurement, so widths, kerning and the
`truncatedText()` ellipsis behave exactly as they do on the device. It also
flags a string whose font lacks one of its codepoints, which the firmware draws
as a U+FFFD replacement box — the subsetted "dash" faces cover only ASCII plus
the degree sign, so an ellipsis appended to a truncated value used to render as
a black lozenge.

It does not prove e-ink appearance. Contrast, ghosting and legibility at
reading distance still need a photograph of the real panel.

## Keeping it honest

`PreviewCanvas::fontIdFor` must resolve each `FontRole` to the same font id as
`fontId()` in `src/spikes/ble_handoff/DashboardV3Renderer.cpp`. If they drift,
the preview measures a font the firmware never draws.

`PreviewCanvas::measureText` must stay a direct wrapper around
`FontBook::textWidth`, matching the firmware's `GfxDashboardV3Canvas`. The 8A
renderer sizes its value boxes from that call; without it the preview falls back
to a character-count estimate and the report measures a font the firmware never
draws.
