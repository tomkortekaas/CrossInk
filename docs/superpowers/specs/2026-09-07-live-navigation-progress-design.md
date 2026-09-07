# Live Navigation Progress Protocol Design

## Problem

The X3 currently derives remaining distance by projecting every incoming GPS
fix onto the geometrically nearest route edge. That stateless calculation is
ambiguous for loops and self-intersections. When the route starts and ends at
the same location, normal GPS drift can alternate between the first and last
edge, making the footer jump between a few metres and almost the full route.

The iPhone already owns a stateful `RouteProgressTracker`. It resolves
overlapping geometry using previous accepted progress and produces a monotonic
`distanceFromStartMeters`. The live protocol does not currently carry that
value, so the X3 discards the information needed to disambiguate a loop.

## Decision

Make the iPhone's accepted route progress authoritative for route-order
metrics during a live walk. Continue using the GPS coordinates on the X3 for
the marker, viewport, accuracy ring, and distance-to-route/off-route safety
checks.

The new field is negotiated as live protocol version 3. This preserves both
upgrade directions:

- old iPhone to new X3: START v1/v2 and the existing 20-byte FIX remain valid;
- new iPhone to old X3: the app first tries START v3, then retries START v2
  after the matching `Invalid` response and sends legacy FIX frames;
- new iPhone to new X3: START v3 selects progress-bearing FIX frames.

## Wire contract

### START v3

START v3 remains 11 bytes and retains the v2 layout:

`[0x08, version=3, routeId u32, sessionId u32, refreshMode u8]`

Version 3 means that FIX frames for this session use the extended layout. The
X3 stores the negotiated session version; START replay remains idempotent only
when route, session, and negotiated version agree.

### FIX v3

FIX v3 appends one field to the existing frame and is 24 bytes:

`[existing FIX bytes 0...19, distanceFromStartMeters u32]`

`distanceFromStartMeters` is unsigned little-endian and represents the
iPhone tracker's accepted progress along the packaged route. The existing
coordinates, accuracy, age, flags, session identity, sequence, ACK, replay,
and stop-and-wait semantics are unchanged.

For a v1/v2 session the X3 accepts exactly 20-byte FIX frames. For a v3
session it accepts exactly 24-byte FIX frames. A length/version mismatch is
`Invalid` and does not mutate the last accepted fix.

## iPhone behavior

At session start, the sender arms START v3. If the matching response is
`Invalid`, and both the BLE write and that application response belong to the
still-pending START, it performs one compatibility retry using START v2 with
the same route ID, session ID, and refresh mode. Any rejection after that, or
an `Invalid` response in another phase, remains a terminal sender failure.

The sender records the negotiated version. In v3 it requires the caller's
`distanceFromStartMeters` and emits a 24-byte FIX. In the v2 fallback it emits
the existing 20-byte FIX while ignoring the progress field only for wire
compatibility. `BleDashboardSender` passes the value already returned by
`RouteProgressTracker.update` into every real walking fix. Debug/synthetic
walking fixes must obtain progress through that same tracker instead of
inventing a second progression rule.

No retry is triggered by a foreign session ID, wrong sequence, timeout, or an
unexpected status code. This prevents stale notifications from silently
downgrading the session.

## X3 behavior

`LiveNavigationSession` accepts START v3, stores the negotiated version, and
retains either the legacy 20-byte or v3 24-byte last fix in bounded RAM.
Disconnect, STOP, and expiry clear both the fix and negotiated capability.
Duplicate FIX comparison covers the complete negotiated frame, including
progress. The BLE mailbox recognizes both structurally valid FIX lengths so
newer fixes can still coalesce during an e-paper refresh.

The live position exposed to rendering includes:

- latitude, longitude, accuracy, and off-route as today;
- `hasRouteProgress`;
- `distanceFromStartMeters`.

The renderer clamps authoritative progress to the route's declared total and
computes:

`remaining = totalDistanceMeters - min(progress, totalDistanceMeters)`

That remaining value drives the footer distance, proportional remaining time,
and arrival check. The geometrically projected remaining distance stays in
`RouteProximity` as the fallback for legacy sessions.

Authoritative progress is used only when the current fix passes the existing
proximity and accuracy trust gates. If the fix is too far off route, the
footer continues to show whole-route totals and arrival remains false. This
keeps a corrupt or displaced position from declaring arrival merely because
its progress field is high.

## State and monotonicity

The X3 does not independently force the received number to be monotonic. The
iPhone tracker is the single route-order state machine and may make small,
intentional corrections under its existing hysteresis rules. The X3 stores
the newest accepted sequenced value and clamps it only to the declared route
total. Adding a second monotonic filter on the X3 would create divergent state
and could prevent legitimate tracker corrections.

Session, route, and sequence validation prevent progress from one walk from
leaking into another. A new START begins without a position or progress until
its first valid FIX.

## Tests

The iOS suite will cover golden START v3 and FIX v3 bytes, progress boundaries,
START-v3-to-v2 compatibility retry, rejection scoping, negotiated FIX length,
and forwarding of `RouteProgressTracker` output.

The firmware suite will cover START v1/v2/v3 negotiation, strict FIX length by
version, v3 parsing and replay identity, full-frame mailbox coalescing, reset
paths, progress clamping, legacy projection fallback, and arrival/footer
behavior on a colocated-start/end loop.

End-to-end hardware acceptance uses a loop route with the phone stationary at
the shared start/end point. Before walking, remaining distance must stay near
the full route; after simulated or real progress reaches the end, it must stay
near zero and must not jump back to the full route while GPS drifts around the
shared point.

## Non-goals

- Replacing the iPhone `RouteProgressTracker` algorithm.
- Moving GPS acquisition or off-route detection to the X3.
- Changing route-transfer frames or stored route-package format.
- Persisting live progress across STOP, disconnect, reboot, or a new session.
- Adding a separate maneuver system as part of this bug fix; any existing or
  future maneuver consumer should use the same authoritative progress value.
