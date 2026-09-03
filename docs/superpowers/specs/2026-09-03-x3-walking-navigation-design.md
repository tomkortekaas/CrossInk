# X3 Walking Navigation Design

**Date:** 2026-09-03
**Status:** Approved in conversation; awaiting review of this written specification

## Purpose

Add a native walking-navigation application to the Xteink X3 without replacing the existing dashboard/reader firmware. The design uses the constraints of the e-ink device deliberately: the iPhone performs location and routing work, while the X3 provides a calm, readable, offline-capable map and instruction display.

The primary use case is following imported GPX day walks of 10–15 km. Routes up to 40 km must remain usable. A later city-planning flow must produce the same internal route representation.

## Design goals

In priority order:

1. Reliable route following
2. Readability on a small monochrome e-ink display
3. Minimal display refresh and low battery use
4. Preservation of small walking paths
5. Simple implementation with reusable existing code
6. Isolation from dashboard and reader data

## System responsibilities

### iPhone

The iPhone owns:

- GPS acquisition and filtering;
- GPX import and preservation;
- route progress and maneuver selection;
- off-route detection;
- calculation of a temporary return route to the GPX;
- vibration and optional audible alerts;
- city-route planning;
- creation and transfer of compact X3 route packages;
- preparation or download of regional offline walking-map packages.

### X3

The X3 owns:

- display of the route, current position, next maneuver, and progress;
- local zoom and view selection through physical buttons;
- partial-refresh scheduling;
- reading offline walking-map cells from the SD card;
- retention of a small emergency route representation in internal storage;
- switching back to the existing dashboard/reader application.

The X3 does not acquire GPS, calculate the primary route, parse raw OpenStreetMap data, or run a general-purpose routing engine.

## Route model

Both GPX import and later city-route planning produce the same logical `WalkingRoute`:

- stable route identifier and version;
- route name, start, end, total distance, and estimated duration;
- original GPX geometry when the source is GPX;
- turn-preserving simplified geometry for the X3;
- ordered maneuvers with type, route offset, distance, and optional name;
- elevation summary when available;
- waypoints and useful route points;
- references to installed regional map packages;
- package checksum and schema version.

For imported walks, the original GPX is authoritative. Map matching may add path names, maneuver hints, and metadata, but may not silently move or replace the route geometry.

## Routing and off-route behavior

The initial product follows GPX routes. A city planner is added later using Valhalla over OpenStreetMap data, with MapLibre providing the iPhone map interface.

Off-route detection is automatic and resistant to GPS noise:

- several consecutive acceptable GPS fixes are required;
- the initial threshold is approximately 30–40 m in dense urban areas and 60 m elsewhere;
- thresholds are parameters that will be tuned in walking tests;
- the iPhone generates a temporary walking route back to the authoritative GPX;
- it may select a safe point slightly ahead only when doing so does not skip a meaningful part of the walk;
- otherwise it returns to the nearest suitable point;
- if online routing is unavailable, bearing and straight-line distance to the GPX remain available;
- normal GPX guidance resumes automatically after rejoining.

The iPhone vibrates for an approaching maneuver, uses a distinct pattern for leaving the route, and signals successful rejoining. Sound is separately optional.

## Offline walking maps

Regional walking-map packages are generated in advance from OpenStreetMap. Noord-Holland is the first validation package; the same packaging pipeline can later produce all of the Netherlands or other regions.

The X3 never parses a raw `.osm.pbf` file and does not use conventional raster tiles as its primary format. A compact read-only `walkmap` format contains:

- a spatial index divided into independently readable geographic cells;
- multiple geometry simplification levels;
- walking paths, footways, tracks, steps, bridleways, and pedestrian ways;
- marked walking-route relations where available;
- useful path attributes such as name, surface, and access;
- contextual roads and intersections;
- water, forest boundaries, railway lines, settlements, and selected landmarks;
- a shared string table and package metadata, including OSM attribution.

Small walking paths are core data and are not discarded merely because they are minor. When space or visual density must be reduced, buildings, addresses, parking detail, driveways, and irrelevant car-road detail are removed first. Geometry may be simplified while preserving topology and path junctions.

At whole-route scale, relevant paths remain visible as thin lines and the GPX is drawn much thicker. Labels and secondary context appear progressively at closer zoom levels.

## Storage architecture

The current flash layout remains unchanged for the MVP:

- `app0`: native navigator application;
- `app1`: existing dashboard/reader application;
- existing SPIFFS: remains primarily available to existing firmware and is not used for full regional maps;
- SD card: GPX files, route packages, and regional `walkmap` packages.

Suggested SD layout:

```text
/Navigation/
  /Maps/
    noord-holland.walkmap
  /Routes/
    <route-id>/
      original.gpx
      route.bin
      manifest.json
```

Navigator files use an isolated directory and atomic replacement: a newly received package is written to a temporary name, verified by length and checksum, and renamed only after validation.

Internal flash retains only the active route identifier, current progress, simplified route geometry, maneuvers, and last valid position. The fallback is one bounded, atomically replaced SPIFFS file with an initial maximum size of 256 KiB; detailed surroundings are never stored there. This permits basic line navigation when the SD card is missing or temporarily unreadable without allowing navigator data to consume the reader's filesystem.

No flash repartitioning is required for the MVP. If later measurements demonstrate a need for isolated internal map storage, part of the existing `app0` region may be redesigned only after the boot-switch and update mechanisms have been audited. The address and size of `app1` and the existing SPIFFS region must remain unchanged in such a redesign.

## Communication

BLE is used during a navigation session for small state messages. Larger regional maps are installed ahead of time through Wi-Fi or USB/SD access. A complete route package may use BLE when small enough, but its transfer is transactional and resumable.

Navigation-state messages include:

- route and session identifiers;
- sequence number and timestamp;
- latitude, longitude, bearing, speed, and accuracy;
- route progress and cross-track distance;
- next maneuver, distance, and display text;
- remaining distance and time;
- navigation state: on-route, returning, recalculating, paused, arrived;
- phone battery and connection state where available.

Messages are versioned, bounded, validated, and allocation-conscious on the X3. Stale or out-of-order messages are ignored.

## Session and power behavior

During an active walk, the X3 uses a low-power navigation session rather than returning to the dashboard's quarter-hour deep-sleep schedule. BLE remains available sufficiently to receive a small status update approximately every 10–15 seconds. The display is not refreshed merely because a message arrived.

Two user modes are provided:

### Pocket mode (default)

- BLE state continues to arrive and is cached in RAM;
- the screen remains unchanged in the pocket;
- pressing the physical power button displays the most recently received state;
- the target perceived wake-to-useful-display time is under one second for cached state;
- after a short viewing window, the device returns to its low-power navigation state.

### Active view mode

- position updates are displayed approximately every 5–10 seconds;
- a position redraw requires meaningful movement, initially 8–12 m, and an acceptable GPS fix;
- only the union of the old and new marker areas is redrawn while the viewport is stable;
- the map recenters only after the position enters an outer screen zone;
- maneuver and distance regions may refresh independently;
- periodic cleanup refreshes prevent accumulated ghosting.

The precise BLE interval, wake latency, current draw, movement threshold, and ghosting limit are acceptance measurements rather than assumptions.

## User interface

The portrait navigation screen prioritizes:

1. large maneuver symbol;
2. distance and path/street name;
3. thick GPX route and current position;
4. remaining distance and time;
5. compact battery and connection state.

Automatic states include normal navigation, approaching maneuver, route left, temporary return to GPX, route resumed, recalculating, and destination reached.

Physical controls:

- **Power:** wake and show the latest cached navigation state;
- **Up/Down:** zoom out/in;
- **OK short:** switch between map and large-maneuver view;
- **OK long:** switch between Pocket and Active view mode;
- **Back short:** show route overview;
- **Back long:** open pause/end actions, requiring confirmation.

## Rendering strategy

The X3 renders vector geometry directly into a one-bit portrait framebuffer. The route is visually dominant. Background paths are thinner, and labels are collision-limited.

The renderer reads only the visible map cells from SD and retains the current and likely next cells in a bounded cache. It does not load a province map into RAM. Map panning redraws the map region; marker-only movement restores the affected local background before drawing the new marker.

Partial refresh is preferred for marker, maneuver, and distance changes. A full or high-quality refresh occurs after a configurable number of partial updates, on a major viewport change, or when switching screens.

## Existing, reused, and new components

### Already present

- dual-app flash layout and boot switching;
- dashboard BLE handoff work;
- physical-button handling;
- e-ink display driver and portrait navigator renderer;
- SD-card manager and shared storage abstraction;
- dynamic navigator state and host-side renderer tests;
- iOS BLE launch protocol and transfer infrastructure.

### Reused external software and data

- OpenStreetMap as map and path data;
- Valhalla for future walking-route calculation and return routing;
- MapLibre Native for iPhone map presentation;
- existing GPX conventions and parsers where licensing and footprint fit.

### Built specifically for this product

- `WalkingRoute` model and GPX enrichment pipeline;
- versioned route-package protocol shared by Swift and C++;
- walking-focused `walkmap` generation pipeline and file format;
- bounded X3 map-cell reader and vector renderer;
- navigation-session BLE state protocol;
- off-route state machine and return-to-GPX presentation;
- adaptive e-ink refresh policy.

## MVP scope

The MVP includes:

- importing one GPX day route on iPhone;
- transferring a compact route package;
- showing route overview, current position, next maneuver, and remaining distance;
- Pocket mode and Active view mode;
- an SD-based Noord-Holland walking-map prototype retaining minor paths;
- automatic off-route detection and return guidance;
- iPhone vibration alerts;
- safe return to the dashboard application;
- host tests and walking measurements on physical hardware.

The MVP excludes an on-X3 route planner, raw OSM parsing, continuous Wi-Fi, spoken turn-by-turn instructions, multi-day route management, worldwide map distribution, and polished city-route planning.

## Implementation sequence

1. Define and test the Swift `WalkingRoute` model and GPX import.
2. Define a versioned binary route package and golden test vectors shared by Swift and C++.
3. Render transferred route geometry and cached position in the existing X3 navigator.
4. Create a Noord-Holland `walkmap` generator prototype and measure real package sizes.
5. Add the bounded SD map reader, spatial cell lookup, and walking-path rendering.
6. Implement the BLE navigation-session state flow and cached wake display.
7. Add adaptive partial-refresh behavior and ghosting cleanup.
8. Add off-route detection, temporary return-to-GPX guidance, and iPhone alerts.
9. Measure battery, latency, readability, SD behavior, and 10 km, 15 km, and 40 km route performance.
10. Add the city planner only after the shared route flow is stable.

Bounded low- and medium-risk implementation tasks may be delegated to DeepSeek. Architecture, storage boundaries, protocol acceptance, hardware safety, and final verification remain Codex responsibilities.

## Acceptance criteria

- An imported GPX remains geometrically authoritative.
- A typical 10–15 km route can be followed without internet after preparation.
- Minor walking paths are visible in route overview and local map views.
- Pocket mode causes no periodic display flashing.
- Pressing power normally shows a useful cached position within one second.
- Active view updates position without leaving stale marker trails.
- Leaving the route triggers an iPhone alert and automatic guidance back to the GPX.
- Removing or failing the SD card does not damage dashboard/reader data and leaves basic route-line guidance available.
- Installing or replacing a map package cannot overwrite files outside `/Navigation`.
- Returning from navigator launches the existing dashboard/reader application.
- Battery and refresh targets are supported by recorded hardware measurements before release.

## Primary risks and mitigations

- **iOS background BLE variability:** cache state opportunistically, use a navigation session, and test locked-screen behavior early.
- **E-ink ghosting:** threshold movement, isolate dirty rectangles, and schedule cleanup refreshes.
- **Dense map rendering:** use zoom-specific geometry, label limits, and walking-first feature priorities.
- **SD latency or removal:** bounded cell cache, checked reads, atomic packages, and an internal emergency route.
- **GPS noise:** require accurate consecutive fixes and use different urban/rural thresholds.
- **OSM data quality:** preserve the GPX as authority and treat enrichment as advisory.
- **Update-slot assumptions:** leave the partition table unchanged for the MVP and audit update behavior before any later repartitioning.
