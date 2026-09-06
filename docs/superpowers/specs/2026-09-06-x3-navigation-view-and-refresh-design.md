# X3 Navigation View Toggle and Calm Startup Design

**Date:** 2026-09-06
**Status:** Approved in conversation; awaiting review of this written specification

## Purpose

Make starting and using the X3 walking navigator calmer and more useful on foot. The navigator must avoid a sequence of full-screen loading messages, let the walker choose between the complete route and a close GPS-centered view on the X3 itself, and remain easy to remap to different physical buttons later.

## Scope

This change is local to the navigator firmware. It does not add an iPhone setting or change the BLE route or live-position protocols. The phone continues to send the same route and GPS fixes. The X3 decides how to frame that data.

The two map views are:

- **Overview:** the full route fitted into the map region, with the current GPS position shown when available.
- **GPS zoom:** a north-up view centered on the current valid GPS position and 250 metres wide, within integer-projection rounding.

Every new navigation session starts in Overview. The selected view is session state only and is not written to NVS or the SD card.

## Physical controls

Navigator input is translated through a small, pure action-mapping layer before application behavior is selected. The initial mapping is:

- Up or Down: toggle between Overview and GPS zoom.
- OK: manually refresh the current navigation view.
- Back: stop navigation and return to the dashboard.

The mapping is deliberately centralized and host-tested so a later task can assign different buttons without changing rendering, viewport, or session logic. This task does not add a settings screen or user-configurable persistent mapping.

If GPS zoom is requested before a valid position exists, the requested mode remains selected but the renderer temporarily shows the Overview framing with the existing waiting-for-GPS status. The first valid fix automatically activates the 250-metre framing.

## Viewport behavior

Overview keeps the existing route-fit calculation and therefore preserves current map-only and route overview behavior.

GPS zoom uses the existing centered `RouteViewport` path with these rules:

- center: latest non-stale accepted GPS position;
- orientation: north-up;
- horizontal ground span: 250 metres across the usable map rectangle, within 5% integer-projection tolerance;
- aspect ratio: derived from the existing map rectangle, without distorting geography;
- background-map and route geometry: clipped to the same map rectangle as Overview;
- maneuver band and footer: unchanged by the selected viewport.

The fixed span avoids continuous automatic zoom changes. Ordinary GPS updates move the center only when the refresh policy accepts the fix; the screen does not redraw merely because another BLE packet arrived.

Changing view mode is an intentional viewport change. It renders immediately with one full-quality refresh to prevent remnants from the previous scale. Subsequent accepted GPS updates use the existing fast-refresh cadence and periodic cleanup refresh.

## Calm startup and loading status

The current navigator can show a full-screen route, then a full-screen loading message, then the route again. The revised flow uses the final navigation layout from the first navigator-owned display update onward.

On navigator entry:

1. Initialize storage and load the retained route without refreshing the panel between these steps.
2. Draw the route geometry in the final navigation layout, with the normal footer/status area showing compact `Route laden…` status, and submit one full-quality refresh. Do not display a separate splash or full-screen loading page.
3. Collect the latest valid fix and open the best available background map without further intermediate submissions.
4. Draw and submit the first complete route frame once. This replaces the loading status in place and may use the grayscale display sequence when available.
5. Use only accepted navigation refreshes after that; do not introduce another startup transition.

If no route, no SD card, an unreadable map, or unavailable Bluetooth is the terminal state, the existing explicit error screen remains allowed. Error reporting must not be suppressed merely to reduce flashing.

Route replacement during an active navigator session follows the same rule: receive and validate without per-chunk display updates, then replace the visible route with one complete rendered frame.

## Components

### Navigation view state

A small enum represents Overview and GPS zoom. A controller owns the current mode, defaults it to Overview at navigator startup, toggles it from mapped input actions, and reports whether a valid fix is required or available.

### Input action mapping

A pure function maps released physical buttons to semantic navigator actions: toggle view, manual refresh, return to dashboard, or no action. It has no display, BLE, storage, or timing dependency.

### Viewport selection

Viewport construction receives the selected navigation view, route bounds, map rectangle, and optional current position. It delegates Overview to the existing fit behavior and constructs the fixed 250-metre centered viewport for GPS zoom. When GPS is unavailable it returns the existing Overview viewport plus a waiting-for-GPS presentation state.

### Render orchestration

`NavigatorMain` gathers route, position, and map readiness before submitting a framebuffer. `NavScreenRenderer` continues to render a complete deterministic frame for Base/LSB/MSB. The orchestrator decides when to submit that frame and which refresh quality is required.

## Memory and storage constraints

- No second framebuffer.
- No new heap allocation in the render or input loop.
- No persistent setting or flash write for the selected view.
- View state and action state remain a few bytes of static/global navigator state.
- Route and background sources retain their existing streaming and bounded-read behavior.
- The same viewport decision is reused for Base, LSB, and MSB so grayscale planes cannot disagree geometrically.

## Testing

Host tests must cover:

- the default view is Overview;
- Up and Down both map to toggle-view, OK maps to manual refresh, and Back maps to return;
- toggling twice returns to Overview;
- Overview remains byte-compatible with the existing route-fit path when no new mode is requested;
- GPS zoom is centered on the supplied position, north-up, and 250 metres wide within 5%;
- GPS zoom without a valid fix falls back to Overview framing and exposes waiting-for-GPS status;
- view switching is classified as a viewport change and requests an immediate high-quality refresh;
- routine fixes after the switch return to the configured fast/economical refresh policy;
- Base/LSB/MSB use identical viewport geometry;
- startup submits only the route-layout loading frame and the completed route frame, with no intermediate full-screen message;
- no-route, storage, map, and Bluetooth terminal failures remain visible;
- guard bytes, row padding, maneuver band, footer, and map-only regression tests remain green.

Verification order:

1. focused input, viewport, refresh-policy, and renderer host tests;
2. complete navigator host suite;
3. clean `navigator-x3` and `dashboard-x3` builds;
4. flash both intended OTA slots only after inspecting the active slot and retaining a recovery image;
5. physical X3 test with a DEBUG synthetic walk: startup, Overview, GPS zoom, repeated toggles, Begin, Halverwege, Buiten route, Bestemming, manual refresh, and return to dashboard;
6. repeat once with a real GPS walking session and inspect ghosting after at least twenty partial updates.

## Acceptance criteria

- Starting navigation shows at most one unavoidable boot transition, one route-layout loading refresh, and one completed-route refresh—not a chain of unrelated full-screen pages.
- Loading progress appears only as a compact status in the final route layout.
- Every session starts with the entire route visible.
- Up or Down toggles immediately between the whole route and a north-up GPS-centered view 250 metres wide within 5%.
- The current position remains visible in Overview when it lies within the route view.
- GPS zoom waits safely for a valid fix and never invents a position.
- OK still performs a manual refresh and Back still returns to the dashboard.
- The physical mapping can be changed later in one isolated mapping component.
- Map-only behavior, maneuver-band confinement, footer layout, and grayscale-plane alignment do not regress.
- No extra framebuffer, persistent setting, or unbounded allocation is introduced.

## Non-goals

- Automatic rotation with walking direction.
- Automatic zoom based on speed or maneuver distance.
- Pinch zoom, arbitrary pan, or multiple zoom levels.
- Phone-side view controls or a BLE protocol revision.
- A persistent button-mapping settings UI.
- Eliminating the physical e-ink flash inherent in a required full refresh.
