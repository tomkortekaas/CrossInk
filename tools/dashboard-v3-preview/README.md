# Dashboard V3 preview

Renders the dashboard V3 layout on the host with the firmware's own built-in
Lexend Deca faces and writes a 528x792 PNG per scenario, plus a report of every
string that had to be truncated.

```sh
cmake -S tools/dashboard-v3-preview -B /tmp/x3-v3-preview
cmake --build /tmp/x3-v3-preview -j8
/tmp/x3-v3-preview/dashboard-v3-preview /tmp/x3-v3-out
```

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
