#!/usr/bin/env python3
"""Report what a wake cycle costs in battery, from /crossink-ble-trace.txt.

The trace carries a `mah=` reading on every line, so consumption can be read
back afterwards without a meter. The trick is separating the two things that
drain the battery: a baseline that runs whether or not anything happens, and
the wake cycles themselves.

The night window is what makes that possible. Between the window end and the
next window start the device sleeps without waking, so consumption over that
stretch is the baseline alone. Subtract that rate from a stretch that does have
wakes, and what remains is what the cycles cost.

Usage:
    python3 scripts/trace_power_report.py /tmp/x3pull/crossink-ble-trace.txt
"""
import re
import sys
from datetime import datetime, timedelta

LINE = re.compile(r"(\d{4}-\d\d-\d\d \d\d:\d\d) UTC wake=(\w+).*?mah=(\d+)")


def read(path):
    rows = []
    for line in open(path):
        m = LINE.match(line)
        if m:
            rows.append(
                (datetime.strptime(m.group(1), "%Y-%m-%d %H:%M"), m.group(2), int(m.group(3)))
            )
    return rows


# A rise of more than this is charging, not measurement noise. The reading
# wobbles by a unit or two between wakes even while draining.
CHARGE_JUMP_MAH = 3


def discharge_segments(rows):
    """Split on charging, since a stretch that spans the cable reads as
    negative consumption and turns every number after it into nonsense."""
    segments, current = [], [rows[0]]
    for prev, row in zip(rows, rows[1:]):
        if row[2] > prev[2] + CHARGE_JUMP_MAH:
            if len(current) > 1:
                segments.append(current)
            current = [row]
        else:
            current.append(row)
    if len(current) > 1:
        segments.append(current)
    return segments


def rate(rows, start, end):
    """mA and cycle count over a stretch, or None when it holds too little."""
    seg = [r for r in rows if start <= r[0] <= end]
    if len(seg) < 2:
        return None
    hours = (seg[-1][0] - seg[0][0]).total_seconds() / 3600
    used = seg[0][2] - seg[-1][2]
    if hours <= 0:
        return None
    cycles = sum(1 for r in seg if r[1] == "timer")
    return hours, used, used / hours, cycles


def longest_quiet_stretch(rows):
    """The longest gap between two consecutive lines — the night window.

    A line is only written when the device wakes, so a gap between consecutive
    lines is by definition a stretch where nothing happened at all. Looking for
    "no timer wakes" instead would let button presses in, and those cost power
    too: that inflates the baseline and hides the very cost being measured.
    """
    best = None
    for prev, row in zip(rows, rows[1:]):
        gap = (row[0] - prev[0]).total_seconds() / 3600
        if gap >= 3 and (best is None or gap > best[2]):
            best = (prev[0], row[0], gap)
    return best


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    all_rows = read(sys.argv[1])
    if len(all_rows) < 10:
        sys.exit(f"te weinig bruikbare regels ({len(all_rows)})")

    print(f"Trace: {all_rows[0][0]} .. {all_rows[-1][0]} UTC, {len(all_rows)} regels")

    segments = discharge_segments(all_rows)
    rows = max(segments, key=lambda s: (s[-1][0] - s[0][0]).total_seconds()) if segments else []
    if len(rows) < 10:
        sys.exit("geen bruikbare ontlaadperiode; het toestel heeft te vaak aan de lader gehangen")
    if len(segments) > 1:
        print(f"{len(segments)} ontlaadperiodes (opladen onderbreekt de meting); "
              f"de langste is gebruikt:")
    print(f"Gebruikt: {rows[0][0]} .. {rows[-1][0]} UTC, {len(rows)} regels\n")

    quiet = longest_quiet_stretch(rows)
    if not quiet:
        sys.exit("geen rustige periode van 3+ uur gevonden; laat het toestel een nacht op accu staan")
    qs, qe, qh = quiet
    base = rate(rows, qs, qe)
    print(f"Rustperiode  {qs:%d-%m %H:%M} -> {qe:%d-%m %H:%M}  ({qh:.1f} u)")
    print(f"  basisverbruik: {base[2]:.2f} mA ({base[1]} mAh)\n")

    # A busy stretch: from the end of the quiet one, as far as the trace goes.
    busy = rate(rows, qe, rows[-1][0])
    if not busy or busy[3] == 0:
        sys.exit("na de rustperiode staan er geen timerwakes; niets te vergelijken")
    bh, bu, br, bc = busy
    print(f"Actieve periode {qe:%d-%m %H:%M} -> {rows[-1][0]:%d-%m %H:%M}  ({bh:.1f} u, {bc} cycli)")
    print(f"  totaal: {br:.2f} mA ({bu} mAh)")

    extra = bu - base[2] * bh
    per_cycle_uah = (extra / bc) * 1000 if bc else 0
    print(f"  boven de basis: {extra:.1f} mAh  ->  {per_cycle_uah:.0f} uAh per cyclus\n")

    cycles_per_day = 60  # ~15 uur venster, elk kwartier
    wake_day = per_cycle_uah / 1000 * cycles_per_day
    base_day = base[2] * 24
    total = base_day + wake_day
    print(f"Geschat etmaal: basis {base_day:.0f} mAh + wakes {wake_day:.0f} mAh = {total:.0f} mAh")
    if total > 0:
        print(f"Aandeel wakes: {100 * wake_day / total:.0f}%")
    print("\nLet op: mah heeft een resolutie van 1 mAh, dus over korte stukken is dit ruw.")
    print("Vergelijk alleen metingen van vergelijkbare lengte met elkaar.")


if __name__ == "__main__":
    main()
