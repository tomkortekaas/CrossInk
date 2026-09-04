# Offline walkmap software increment — 2026-09-04

Implements the next block of the approved walking-navigation design. User authorized autonomous safe software work; installation and device acceptance remain separate.

## Scope

Build a dependency-free converter from OSM XML to a versioned read-only map, a bounded C++ cell reader, and optional background rendering in the navigator overview. The converter preserves all vertices of accepted highway ways and splits edges at cell boundaries. Footways, paths, tracks, steps, pedestrian and bridleway geometry are never removed for density. Context roads are a separate class. Access restrictions are retained as flags; this is display context, not a routing/access guarantee. Unknown ways and unsupported features are reported in converter statistics. Names, water/forest polygons, route relations, LODs, and an iPhone map installer remain later increments.

The first prototype uses OSM XML (local input, no network in converter). A regional XML export or externally converted PBF can supply Noord-Holland. Test fixtures are synthetic and explicitly labeled. No fabricated regional coverage claim.

## File contract X3WM v1

All integers little endian; coordinates signed E7. Fixed 48-byte header:

| Offset | Field |
| --- | --- |
| 0 | magic X3WM |
| 4 | version u16 = 1 |
| 6 | header length u16 = 48 |
| 8 | total bytes u32 (maximum 512 MiB) |
| 12 | origin latitude i32, southwest grid corner |
| 16 | origin longitude i32 |
| 20 | cell size E7 u32 = 200000 (0.02 degree) |
| 24 | rows u16 |
| 26 | columns u16 |
| 28 | directory offset u32 = 48 |
| 32 | data offset u32 = 48 + rows * columns * 12 |
| 36 | directory CRC32 u32 |
| 40 | reserved u32 = 0 |
| 44 | header CRC32 u32 over first 44 bytes |

Dense row-major south-to-north directory; each 12-byte entry has absolute offset u32, edge count u32, CRC32 u32 over its edge bytes. Empty entry is all zero. Nonempty ranges must lie wholly within payload; cells have at most 16384 edges. Maximum 65536 cells. Header bounds must remain within latitude +/-85 degrees and longitude +/-180 degrees. Dateline-crossing input is rejected explicitly in this regional version.

Each edge is 20 bytes: first latitude i32, longitude i32, second latitude i32, longitude i32, class u8 (1 walking path, 2 contextual road), flags u8 (bit 0 restricted access), reserved u16 zero. Coordinates lie inside their cell including its closed upper edge. Source ways are never connected across missing nodes: converter rejects missing references for accepted ways. Reject invalid coordinates and nonfinite values.

No full-map CRC scan at boot. Validate header and directory CRC at open using <=256 byte workspace, then validate only selected cells on draw. A failed cell, changed source, malformed record, short read or exceeded drawing budget returns failure; caller clears map and renders the route alone. Never leave a plausible half-drawn background. Per-cell CRC is corruption detection, not authentication.

## Runtime

Use a separate `WalkMapByteSource` to avoid weakening RouteByteSource's 65535-byte contract. A fixed canonical read-only source points to `/Navigation/Maps/active.walkmap`; every operation closes its SD handle. No directory creation, rename, delete or map writing on X3. Route storage remains unchanged.

Caller selects a bounded geographic window; reader computes cell range, visits only intersecting cells and streams edge records to a function-pointer callback. At most 1024 cells and 100000 edges per draw; exceed either limit explicitly, never silently drop small paths. No heap allocation, recursion or extra framebuffer. SDK target build confirms integration; host tests cannot establish read latency/current draw.

The renderer clears once, draws background at one pixel, route at five pixels, then optional real position last. Reuse existing clipping math through a common map-edge helper; preserve existing default rendering byte-for-byte when no map is supplied. Overview selection uses conservative bounds enclosing its viewport. If map fails, draw route-only and show map unavailable; if beyond render budget, show zoom required. Attribution accompanies a successfully drawn OSM background: OpenStreetMap contributors and copyright URL in the install documentation.

## Verification and acceptance

Converter tests cover retained minor types, access flag, node gaps, crossing cells, negative coordinates, determinism, malformed XML, bounds, and binary checksums. C++ tests consume converter output and corrupt headers/directories/cells, guard reads, validate budgets and callback geometry. Framebuffer tests verify thin background beneath dominant disconnected GPX and absence of partial background after failure. Build navigator cleanly and retain artifact/logs externally; do not rebuild unchanged iOS or dashboard code merely for this increment.

Hardware acceptance: manually install active.walkmap on a backed-up test SD, compare a known walk and minor paths, remove/corrupt test map and verify route-only fallback, measure load/render time/heap, and return to app1. No installation or flashing is authorized by this document.
