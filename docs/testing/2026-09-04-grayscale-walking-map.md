# X3 grayscale walking map — 2026-09-04

The Garmin reference was translated into a native four-tone X3 map: dark
water, light buildings/green areas, white roads with a dark edge, a black GPX
route with white casing, proportional Noto Sans labels, scale and footer.

## Runtime and memory

- `GrayMap` streams 128-byte packed rows and retains only 32 bounded label
  anchors. It never allocates a tile or second framebuffer.
- The single 48 KiB display framebuffer is redrawn for the base, LSB and MSB
  masks. The existing FreeInk X3 grayscale waveform uploads those masks.
- Every visible tile is CRC checked before drawing. Failure, unsupported
  coverage and excessive tile count fall back to the existing vector map.

## Data evidence

- Source: Geofabrik Noord-Holland extract dated 2026-09-02.
- Output: `gray.x3gm`, 681,916,560 bytes, 10,584 directory cells, 6,339
  non-empty tiles and 139,820 label anchors.
- SHA-256: `d09a025e14cf70acb02a921c8f1717e2d9b8443f2396bf5bdfc3720b3190de31`.
- Full directory, tile CRC, label bounds and text validation passed. The SD
  copy was hashed after its atomic rename and matched the source.

## Host evidence

- Four-tone packing and polygon-hole tests pass.
- `GrayMapTest` covers fragmented reads, all four levels, CRC rejection,
  invalid dimensions, bounds and truncation.
- Navigator/map focused CTest set passes (42 tests, one optional artifact test
  skipped).
- A real renderer preview used the active route package from the SD card. All
  three native masks were combined and checked: LSB is a subset of MSB and no
  gray mask overlaps white base pixels.

## Hardware acceptance

Open the active route with a fresh foreground GPS fix. Expect one grayscale
refresh, filled map areas, a continuous dominant route, current-position
marker, Noto Sans labels and a 100 m scale. Press Back during loading to cancel.
The first accepted fix and an on/off-route transition refresh immediately.
Routine movement refreshes after at least 20 m and 30 seconds; stationary fixes
do not redraw. The middle position dot is seven pixels in radius.
If `/Navigation/Maps/gray.x3gm` is missing or invalid, expect the older vector
map rather than a partially drawn gray frame. Measure loading time and inspect
the two mid-gray levels outdoors before accepting the waveform/contrast.
