# Readable X3 walking map — 2026-09-04

## Ready to install, no device write yet

- Firmware navigator-detail-app0.bin SHA256 c8b48613d443d18e9ce8f96d0671bfbf24bf1fbbefa6c1fb282b78398aa71773, app0 offset 0x10000 only.
- Optional map detail.walkmap SHA256 1d97f581f3b14288cec71af4d470059594286dc7294b36f15926ea0db164e20b, 1009364432 bytes (~963 MiB). Install as /Navigation/Maps/detail.walkmap on the X3 SD card, preserving active.walkmap.
- Keep /Volumes/2TB/x3-live-view-20260904/navigator-live-view-app0.bin as the prior user-tested GPS recovery image.
- No SD card or X3 USB serial device visible at final check. /Volumes/media is an unrelated SMB share, not the SD card.

## Behavior

Optional v2 details at a fresh GPS position: named roads/selected landmarks, building outlines, water outlines and sparse hatching with polygon holes preserved. Paths dashed, main roads thicker, GPX remains dominant. Names are printable uppercase ASCII for the current tiny font; long names truncated at 44 characters in data and to available screen width at draw time. At most 32 label candidates are retained (global static layer, ~1792 bytes); up to 12 non-overlapping labels drawn, leaving the center clear for marker/direction. Marker ring is at least 11 px, disc 4 px. North and a 100m scale are shown at the 400m live span.

A short direction arrow and approximate straight-line distance to the nearest streamed GPX segment are shown beyond max(40m, 2*accuracy). This is NOT walking directions or a traversable return route. Separate GPX segments are never connected. Distance uses local equirectangular screen projection with fixed-point nearest-segment interpolation; not an exact geodesic distance.

Off-route investigation: iPhone RouteProgressTracker intentionally needs 3 consecutive distinct accepted far fixes; the X3 screen only changes on OK. Its existing 13 tests pass. A single photo does not prove that pipeline is broken. The new independent geometric direction/distance gives immediate orientation from a valid fix without altering debounce semantics. No iPhone code changes in this increment.

Missing/invalid detail header falls back to existing active.walkmap. A corrupt visible detail cell clears partial background and leaves the authoritative GPX and marker, preserving the existing failure rule. The old regional map remains the overview source. V1 stays bounded to 512MiB; v2 maximum is 1536MiB, with unchanged streamed IO and no whole-region allocation. Map layer static RAM increased; whole firmware static RAM 44228 bytes, flash 794401 bytes. Physical latency/heap/stack still need measurement for this much larger file.

## Evidence

- NavigatorCoreTest: 198 pass, 2 optional environment-based tests skipped.
- Five WalkMap CTest targets pass, including v1 corruption/bounds and new v2 validation.
- 40 converter tests pass, including water holes and bounded ASCII labels.
- Existing Swift RouteProgressTrackerTests: 13 pass, no Swift edits.
- Entire detail file: every cell CRC and every record structure checked. 15,769,334 records; largest cell 37,390. 10,535,443 building edges; 1,084,359 water hatch edges; 2,357,430 water outlines; 139,820 name records; remainder paths/roads.
- Fresh navigator-x3 build succeeded in a new build directory; final incremental build succeeded after label changes.
- Visual preview from real OSM data near Zaandam, using a TEST position and test GPX; not the user's live position. Preview is rendered by the actual firmware framebuffer renderer.
- Source: existing verified Geofabrik Noord-Holland PBF dated 2026-09-02. Pyosmium area assembly includes relations/holes. Primary API reference: https://docs.osmcode.org/pyosmium/latest/reference/Handler-Processing/

## Hardware acceptance

Copy detail map to user-mounted SD using a temporary name, verify hash, rename only after success. Safely eject. User reinserts it in X3 and reconnects USB. Save/verify prior app0, then write only 0x10000. Start existing phone walking flow, allow GPS, OK to render; inspect street names/water/buildings, marker, scale, approximate route direction, loading time, and Back. Preserve installed reader/app1, boot metadata, NVS, current GPX and active.walkmap. No physical success claim until tested.
