#!/usr/bin/env python3
"""X3WM v1 walkmap converter (task 1 of docs/superpowers/plans/2026-09-04-x3-walkmap.md).

Converts OSM XML into the versioned, read-only X3WM v1 map described in
docs/superpowers/specs/2026-09-04-x3-walkmap-design.md.  Python standard
library only; no network access; no dependencies.

Wire format (all little endian):
  * 48-byte header: magic "X3WM", version u16=1, header length u16=48,
    total bytes u32 (<=512 MiB), origin latitude/longitude i32 (SW corner),
    cell size E7 u32=200000 (0.02 deg), rows u16, columns u16,
    directory offset u32=48, data offset u32=48+rows*cols*12,
    directory CRC32 u32, reserved u32=0, header CRC32 u32 over first 44 bytes.
  * dense row-major south-to-north directory; each 12-byte entry holds an
    absolute offset u32, edge count u32, CRC32 u32 over its edge bytes.
    An empty entry is all zero.  Cells hold at most 16384 edges and the grid
    holds at most 65536 cells.
  * each edge is 20 bytes: first lat/lon i32, second lat/lon i32,
    class u8 (1 walking path, 2 contextual road), flags u8 (bit 0 =
    restricted access), reserved u16 = 0.

Geometry conventions (documented because the spec leaves cell membership
partly implicit):
  * The grid is anchored at (origin_lat, origin_lon) and cells are indexed
    south-to-north / west-to-east with cell size CELL_SIZE E7.
  * A stored edge lies inside its cell including the cell's closed upper
    (north/east) edge; edges that run exactly along a grid line are stored in
    the cell below (latitude) or to the left (longitude) of that line.
  * Edges crossing a cell boundary are split at every grid line strictly
    between their endpoints; the split point is shared between the two
    adjacent cells so no vertex is dropped and no gap opens up.  Split points
    always lie exactly on at least one grid line.
  * Source ways are never connected across missing nodes: a way that
    references an unindexed/invalid node is rejected whole.

Build strategy: the input is streamed with expat (memory bounded, no DOM),
in three sweeps:
  1. nodes -> sqlite temp index placed beside the output,
  2. accepted ways -> per-cell edge counts and CRCs (bounded memory),
  3. accepted ways -> payload bytes written at precomputed offsets.
The temporary package is validated by re-reading it before it is atomically
published over the destination (which is never touched unless force=True).
"""

import argparse
import functools
import hashlib
import os
import sqlite3
import struct
import sys
import tempfile
import xml.parsers.expat
import zlib
from decimal import Decimal, ROUND_HALF_UP, InvalidOperation


MAGIC = b"X3WM"
VERSION = 1
HEADER_SIZE = 48
CELL_SIZE = 200000          # 0.02 degree in E7 units (fixed for v1)
MAX_CELLS = 65536
MAX_EDGES_PER_CELL = 16384
MAX_TOTAL_BYTES = 512 * 1024 * 1024
MAX_LAT_E7 = 850000000      # +/-85 degrees
MAX_LON_E7 = 1800000000     # +/-180 degrees
WORLD_LAT_E7 = 900000000    # valid OSM coordinate limit for nodes
WORLD_LON_E7 = 1800000000

EDGE_SIZE = 20
ENTRY_SIZE = 12

HEADER_STRUCT = struct.Struct("<4sHHIiiIHHIIIII")
ENTRY_STRUCT = struct.Struct("<III")
EDGE_STRUCT = struct.Struct("<iiiiBBH")

# Highway values stored as class 1 walking paths (never culled for density).
WALKING_HIGHWAYS = frozenset(
    {"footway", "path", "track", "steps", "pedestrian", "bridleway"}
)
# Highway values stored as class 2 contextual roads.
ROAD_HIGHWAYS = frozenset(
    {
        "motorway",
        "motorway_link",
        "trunk",
        "trunk_link",
        "primary",
        "primary_link",
        "secondary",
        "secondary_link",
        "tertiary",
        "tertiary_link",
        "unclassified",
        "residential",
        "service",
        "living_street",
    }
)

_RESTRICTED_ACCESS = frozenset(
    {
        "no",
        "private",
        "destination",
        "customers",
        "permit",
        "agricultural",
        "forestry",
        "military",
        "delivery",
        "use_sidepath",
        "discouraged",
    }
)


class BuildError(Exception):
    """Raised for any condition that prevents producing a map."""


def _ceil_div(a, b):
    """Integer ceil(a/b) for integer b > 0."""
    return -((-a) // b)


def _round_div(num, den):
    """Round num/den to the nearest integer, halves away from zero (den > 0)."""
    sign = -1 if num < 0 else 1
    q = abs(num) // den
    if 2 * (abs(num) - q * den) >= den:
        q += 1
    return sign * q


def _parse_e7(text):
    """Parse an OSM decimal-degree string into integer E7 (deterministic)."""
    if not isinstance(text, str) or not text:
        raise ValueError("missing coordinate")
    try:
        dec = Decimal(text)
    except InvalidOperation:
        raise ValueError("non-numeric coordinate %r" % (text,))
    if not dec.is_finite():
        raise ValueError("non-finite coordinate %r" % (text,))
    scaled = dec * Decimal(10000000)
    return int(scaled.to_integral_value(rounding=ROUND_HALF_UP))


def _point_cell(lat, lon, origin_lat, origin_lon, cell):
    """Canonical (row, col) for a point, or None when off-grid."""
    dlat = lat - origin_lat
    dlon = lon - origin_lon
    if dlat <= 0 or dlon <= 0:
        return None
    return (dlat - 1) // cell, (dlon - 1) // cell


def split_edge(
    a_lat,
    a_lon,
    b_lat,
    b_lon,
    *,
    origin_lat,
    origin_lon,
    cell,
    rows,
    cols,
):
    """Split segment a->b at every cell boundary between its endpoints.

    Returns a list of ((row, col), (lat, lon), (lat, lon)) sub-edges ordered
    from a to b.  Each sub-edge lies inside its cell (inclusive bounds) and
    consecutive sub-edges share the split point that sits exactly on a grid
    line.  Zero-length or degenerate input yields [].
    """
    a = (a_lat, a_lon)
    b = (b_lat, b_lon)
    if a == b:
        return []
    events = []  # (t_num, t_den, axis, grid_value); t_num/t_den positive

    def _axis_events(a0, b0, origin, axis):
        if a0 == b0:
            return
        lo, hi = (a0, b0) if a0 < b0 else (b0, a0)
        den_raw = b0 - a0
        k_first = (lo - origin) // cell + 1
        k_last = _ceil_div(hi - origin, cell) - 1
        for k in range(k_first, k_last + 1):
            value = origin + k * cell
            num = value - a0
            den = den_raw
            if den < 0:
                num, den = -num, -den
            events.append((num, den, axis, value))

    _axis_events(a_lat, b_lat, origin_lat, "lat")
    _axis_events(a_lon, b_lon, origin_lon, "lon")
    if not events:
        return [_assign_subedge(a, b, origin_lat, origin_lon, cell, rows, cols)]

    def _cmp(e1, e2):
        lhs = e1[0] * e2[1]
        rhs = e2[0] * e1[1]
        return (lhs > rhs) - (lhs < rhs)

    events.sort(key=functools.cmp_to_key(_cmp))

    # Walk the sorted crossings; crossings with an equal parameter value are
    # merged (the segment passes exactly through a cell corner there).
    points = [a]
    i = 0
    while i < len(events):
        num, den, _axis, _value = events[i]
        j = i
        while j < len(events) and events[j][0] * den == num * events[j][1]:
            j += 1
        lat = None
        lon = None
        for t_num, t_den, t_axis, t_value in events[i:j]:
            if t_axis == "lat":
                lat = t_value
            else:
                lon = t_value
        if lat is None:
            lat = a_lat + _round_div(num * (b_lat - a_lat), den)
        if lon is None:
            lon = a_lon + _round_div(num * (b_lon - a_lon), den)
        points.append((lat, lon))
        i = j
    points.append(b)

    pieces = []
    for p, q in zip(points, points[1:]):
        piece = _assign_subedge(p, q, origin_lat, origin_lon, cell, rows, cols)
        if piece is not None:
            pieces.append(piece)
    return pieces


def _assign_subedge(p, q, origin_lat, origin_lon, cell, rows, cols):
    """Assign one sub-segment to the cell containing its interior."""
    if p == q:
        return None
    # Use the exact doubled midpoint; flooring a midpoint onto a cell edge
    # would assign a one-E7 segment to the wrong cell and then erase it.
    r = (p[0] + q[0] - 2 * origin_lat - 1) // (2 * cell)
    c = (p[1] + q[1] - 2 * origin_lon - 1) // (2 * cell)
    if not (0 <= r < rows and 0 <= c < cols):
        raise BuildError(
            "sub-edge midpoint row/col (%d,%d) outside %dx%d grid"
            % (r, c, rows, cols)
        )
    lat_lo = origin_lat + r * cell
    lat_hi = origin_lat + (r + 1) * cell
    lon_lo = origin_lon + c * cell
    lon_hi = origin_lon + (c + 1) * cell
    if not all(lat_lo <= lat <= lat_hi and lon_lo <= lon <= lon_hi for lat, lon in (p, q)):
        raise BuildError("split geometry falls outside assigned cell; refusing to move a vertex")
    if p == q:
        return None
    return (r, c), p, q


def _restricted(tags):
    """Restricted-access flag for pedestrians given OSM tags."""
    foot = tags.get("foot")
    if foot is not None:
        return foot in _RESTRICTED_ACCESS
    access = tags.get("access")
    if access is not None:
        return access in _RESTRICTED_ACCESS
    return False


def _pack_edge(p, q, cls, flags):
    return EDGE_STRUCT.pack(p[0], p[1], q[0], q[1], cls, flags, 0)


class _OsmStream(object):
    """Streaming (expat based) OSM XML reader; memory bounded, no DOM."""

    def __init__(self, path, on_node=None, on_way=None):
        self.path = path
        self.on_node = on_node
        self.on_way = on_way
        self.active_way = None

    def _start(self, name, attrs):
        if name == "way":
            self.active_way = {"nds": [], "tags": {}}
        elif name == "nd":
            if self.active_way is not None:
                self.active_way["nds"].append(attrs.get("ref"))
        elif name == "tag":
            if self.active_way is not None:
                self.active_way["tags"][attrs.get("k")] = attrs.get("v")
        elif name == "node":
            if self.on_node is not None:
                self.on_node(attrs)

    def _end(self, name):
        if name == "way":
            way = self.active_way
            self.active_way = None
            if way is not None and self.on_way is not None:
                self.on_way(way["nds"], way["tags"])

    @staticmethod
    def _doctype(*_args):
        raise BuildError(
            "DOCTYPE declarations are not supported (refusing DTD/entity input)"
        )

    def run(self):
        parser = xml.parsers.expat.ParserCreate()
        parser.StartElementHandler = self._start
        parser.EndElementHandler = self._end
        parser.StartDoctypeDeclHandler = self._doctype
        try:
            with open(self.path, "rb") as fh:
                while True:
                    chunk = fh.read(1 << 20)
                    if not chunk:
                        break
                    try:
                        parser.Parse(chunk, False)
                    except BuildError:
                        raise
                    except xml.parsers.expat.ExpatError as exc:
                        raise BuildError(
                            "input %s is not well-formed OSM XML: %s"
                            % (self.path, exc)
                        )
            try:
                parser.Parse(b"", True)
            except BuildError:
                raise
            except xml.parsers.expat.ExpatError as exc:
                raise BuildError(
                    "input %s is not well-formed OSM XML: %s" % (self.path, exc)
                )
        except OSError as exc:
            raise BuildError("cannot read input %s: %s" % (self.path, exc))
        if self.active_way is not None:
            raise BuildError("input %s ends inside an unclosed <way>" % self.path)


def _grid_for(min_lat, max_lat, min_lon, max_lon, cell):
    """Derive origin/rows/cols with a one-cell margin and validate limits."""
    if max_lon - min_lon > MAX_LON_E7:
        raise BuildError(
            "dateline-crossing longitude extent not supported: span %d E7 "
            "exceeds 180 degrees" % (max_lon - min_lon)
        )
    origin_lat = (min_lat // cell) * cell - cell
    origin_lon = (min_lon // cell) * cell - cell
    rows = _ceil_div(max_lat - origin_lat, cell) + 1
    cols = _ceil_div(max_lon - origin_lon, cell) + 1
    if rows > 65535 or cols > 65535 or rows * cols > MAX_CELLS:
        raise BuildError(
            "grid too large: %d rows x %d columns = %d cells exceeds the "
            "X3WM maximum of 65536" % (rows, cols, rows * cols)
        )
    lat_top = origin_lat + rows * cell
    lon_top = origin_lon + cols * cell
    if origin_lat < -MAX_LAT_E7 or lat_top > MAX_LAT_E7:
        raise BuildError(
            "region would exceed X3WM latitude bounds (+/-85 degrees): "
            "grid spans %d..%d E7" % (origin_lat, lat_top)
        )
    if origin_lon < -MAX_LON_E7 or lon_top > MAX_LON_E7:
        raise BuildError(
            "region would exceed X3WM longitude bounds (+/-180 degrees): "
            "grid spans %d..%d E7" % (origin_lon, lon_top)
        )
    return origin_lat, origin_lon, rows, cols


def _validate_file(path):
    """Re-read a finished package and validate header, directory and cells.

    Returns the number of edge records.  Raises BuildError on any mismatch.
    """
    try:
        with open(path, "rb") as fh:
            head = fh.read(HEADER_SIZE)
            if len(head) != HEADER_SIZE:
                raise BuildError("truncated header")
            (
                magic,
                version,
                header_len,
                total_bytes,
                origin_lat,
                origin_lon,
                cell,
                rows,
                cols,
                dir_offset,
                data_offset,
                dir_crc,
                reserved,
                header_crc,
            ) = HEADER_STRUCT.unpack(head)
            if magic != MAGIC:
                raise BuildError("bad magic")
            if version != VERSION:
                raise BuildError("unsupported version %d" % version)
            if header_len != HEADER_SIZE or dir_offset != HEADER_SIZE:
                raise BuildError("bad header layout")
            if reserved != 0:
                raise BuildError("reserved header field not zero")
            if cell != CELL_SIZE:
                raise BuildError("unsupported cell size %d" % cell)
            if rows == 0 or cols == 0 or rows * cols > MAX_CELLS:
                raise BuildError("invalid grid dimensions")
            stat_size = os.path.getsize(path)
            if total_bytes != stat_size:
                raise BuildError(
                    "total bytes %d != file size %d" % (total_bytes, stat_size)
                )
            if total_bytes > MAX_TOTAL_BYTES:
                raise BuildError("map exceeds 512 MiB limit")
            if data_offset != dir_offset + rows * cols * ENTRY_SIZE:
                raise BuildError("data offset does not follow the directory")
            if zlib.crc32(head[:44]) & 0xFFFFFFFF != header_crc:
                raise BuildError("header CRC mismatch")
            dir_bytes = fh.read(rows * cols * ENTRY_SIZE)
            if len(dir_bytes) != rows * cols * ENTRY_SIZE:
                raise BuildError("truncated directory")
            if zlib.crc32(dir_bytes) & 0xFFFFFFFF != dir_crc:
                raise BuildError("directory CRC mismatch")
            lat_hi_world = origin_lat + rows * cell
            lon_hi_world = origin_lon + cols * cell
            if (
                origin_lat < -MAX_LAT_E7
                or lat_hi_world > MAX_LAT_E7
                or origin_lon < -MAX_LON_E7
                or lon_hi_world > MAX_LON_E7
            ):
                raise BuildError("header bounds exceed world limits")
            edge_count = 0
            for idx in range(rows * cols):
                offset, count, crc = ENTRY_STRUCT.unpack_from(dir_bytes, idx * 12)
                r = idx // cols
                c = idx % cols
                if offset == 0 and count == 0 and crc == 0:
                    continue
                if (
                    count == 0
                    or count > MAX_EDGES_PER_CELL
                    or offset < data_offset
                    or offset + count * EDGE_SIZE > total_bytes
                ):
                    raise BuildError("cell %d has an invalid range" % idx)
                fh.seek(offset)
                chunk = fh.read(count * EDGE_SIZE)
                if len(chunk) != count * EDGE_SIZE:
                    raise BuildError("truncated payload")
                if zlib.crc32(chunk) & 0xFFFFFFFF != crc:
                    raise BuildError("cell %d CRC mismatch" % idx)
                lat_lo = origin_lat + r * cell
                lat_hi = origin_lat + (r + 1) * cell
                lon_lo = origin_lon + c * cell
                lon_hi = origin_lon + (c + 1) * cell
                for i in range(count):
                    (
                        lat1,
                        lon1,
                        lat2,
                        lon2,
                        cls,
                        flags,
                        resv,
                    ) = EDGE_STRUCT.unpack_from(chunk, i * EDGE_SIZE)
                    if resv != 0:
                        raise BuildError("edge reserved bytes not zero")
                    if cls not in (1, 2):
                        raise BuildError("edge class %d invalid" % cls)
                    if flags & ~1:
                        raise BuildError("edge uses undefined flag bits")
                    if (lat1, lon1) == (lat2, lon2):
                        raise BuildError("zero-length edge stored")
                    for lat, lon in ((lat1, lon1), (lat2, lon2)):
                        if not (lat_lo <= lat <= lat_hi):
                            raise BuildError("edge latitude outside cell")
                        if not (lon_lo <= lon <= lon_hi):
                            raise BuildError("edge longitude outside cell")
                    edge_count += 1
            return edge_count
    except OSError as exc:
        raise BuildError("cannot validate output %s: %s" % (path, exc))


def _sha256_file(path):
    digest = hashlib.sha256()
    with open(path, "rb") as fh:
        while True:
            chunk = fh.read(1 << 20)
            if not chunk:
                break
            digest.update(chunk)
    return digest.hexdigest()


def build(input_path, output_path, force=False):
    """Build an X3WM v1 map from OSM XML at input_path.

    Existing output is preserved unless force=True.  The output is written to
    a private sibling temporary package and atomically published only after a
    full read-back validation.  Returns a statistics dict.
    """
    input_path = os.path.abspath(input_path)
    output_path = os.path.abspath(output_path)
    out_dir = os.path.dirname(output_path) or "."
    base = os.path.basename(output_path)
    if not os.path.isfile(input_path):
        raise BuildError("input file not found: %s" % input_path)
    if not os.path.isdir(out_dir):
        raise BuildError(
            "output directory does not exist or is not writable: %s" % out_dir
        )
    if os.path.realpath(input_path) == os.path.realpath(output_path) or (
        os.path.exists(output_path) and os.path.samefile(input_path, output_path)
    ):
        raise BuildError("input and output must be different files")
    if os.path.exists(output_path) and not force:
        raise BuildError(
            "output file already exists: %s "
            "(pass force=True or CLI --force to overwrite)" % output_path
        )

    try:
        baseline_stat = os.stat(input_path)
    except OSError as exc:
        raise BuildError("cannot stat input %s: %s" % (input_path, exc))

    def _check_input_unchanged():
        try:
            st = os.stat(input_path)
        except OSError as exc:
            raise BuildError("cannot stat input %s: %s" % (input_path, exc))
        if (st.st_size, st.st_mtime_ns, st.st_ino) != (
            baseline_stat.st_size,
            baseline_stat.st_mtime_ns,
            baseline_stat.st_ino,
        ):
            raise BuildError("input changed while building; refusing to continue")

    stats = {
        "input": input_path,
        "output": output_path,
        "nodes_total": 0,
        "nodes_indexed": 0,
        "nodes_invalid": 0,
        "ways_total": 0,
        "ways_retained": 0,
        "ways_skipped": 0,
        "skipped": {
            "not_highway": 0,
            "unsupported_highway": 0,
            "missing_nodes": 0,
            "empty_geometry": 0,
        },
        "walking_ways": 0,
        "road_ways": 0,
        "restricted_ways": 0,
    }

    sqlite_fd, sqlite_path = tempfile.mkstemp(
        dir=out_dir, prefix=".%s.nodes-" % base, suffix=".sqlite"
    )
    os.close(sqlite_fd)
    tmp_fd, tmp_path = tempfile.mkstemp(
        dir=out_dir, prefix=".%s.pkg-" % base, suffix=".tmp"
    )
    os.close(tmp_fd)
    conn = None
    try:
        # ---- Sweep 1: index nodes into a temporary sqlite database. -------
        conn = sqlite3.connect(sqlite_path)
        conn.execute("PRAGMA synchronous=OFF")
        conn.execute(
            "CREATE TABLE nodes (id INTEGER PRIMARY KEY, "
            "lat INTEGER NOT NULL, lon INTEGER NOT NULL)"
        )
        batch = []

        def on_node(attrs):
            stats["nodes_total"] += 1
            try:
                raw_id = attrs.get("id")
                if raw_id is None:
                    raise ValueError("node without id")
                nid = int(raw_id)
                lat_e7 = _parse_e7(attrs.get("lat"))
                lon_e7 = _parse_e7(attrs.get("lon"))
            except (TypeError, ValueError) as exc:
                raise BuildError("invalid node coordinate/id: %s" % exc)
            if (
                lat_e7 < -WORLD_LAT_E7
                or lat_e7 > WORLD_LAT_E7
                or lon_e7 < -WORLD_LON_E7
                or lon_e7 > WORLD_LON_E7
            ):
                raise BuildError("node coordinates outside world bounds")
            batch.append((nid, lat_e7, lon_e7))
            stats["nodes_indexed"] += 1
            if len(batch) >= 2000:
                conn.executemany(
                    "INSERT OR REPLACE INTO nodes (id, lat, lon) VALUES (?, ?, ?)",
                    batch,
                )
                del batch[:]

        conn.execute("CREATE TABLE used_nodes (id INTEGER PRIMARY KEY)")
        def index_used_nodes(refs, tags):
            highway = tags.get("highway")
            if highway not in WALKING_HIGHWAYS and highway not in ROAD_HIGHWAYS:
                return
            try:
                identifiers = [(int(ref),) for ref in refs]
                if any(not -(1 << 63) <= row[0] < (1 << 63) for row in identifiers):
                    raise ValueError("reference outside signed 64-bit range")
            except (TypeError, ValueError) as exc:
                raise BuildError("invalid accepted-way node reference: %s" % exc)
            conn.executemany("INSERT OR IGNORE INTO used_nodes(id) VALUES (?)", identifiers)

        _OsmStream(input_path, on_node=on_node, on_way=index_used_nodes).run()
        if batch:
            conn.executemany(
                "INSERT OR REPLACE INTO nodes (id, lat, lon) VALUES (?, ?, ?)",
                batch,
            )
        conn.commit()
        _check_input_unchanged()

        row = conn.execute(
            "SELECT MIN(n.lat), MAX(n.lat), MIN(n.lon), MAX(n.lon) "
            "FROM used_nodes u JOIN nodes n ON n.id = u.id"
        ).fetchone()
        if row[0] is None:
            # Preserve the explicit empty-map case; accepted ways with missing
            # references are still rejected in the geometry pass below.
            row = conn.execute("SELECT MIN(lat), MAX(lat), MIN(lon), MAX(lon) FROM nodes").fetchone()
        if row[0] is None:
            raise BuildError(
                "input contains no indexable nodes (need at least one node "
                "with valid finite coordinates)"
            )
        min_lat, max_lat, min_lon, max_lon = row
        origin_lat, origin_lon, rows, cols = _grid_for(
            min_lat, max_lat, min_lon, max_lon, CELL_SIZE
        )

        lookup = conn.cursor()

        def _lookup(ref):
            try:
                nid = int(ref)
            except (TypeError, ValueError):
                return None
            lookup.execute("SELECT lat, lon FROM nodes WHERE id = ?", (nid,))
            found = lookup.fetchone()
            if found is None:
                return None
            return found[0], found[1]

        # ---- Sweep 2: accepted ways -> per-cell counts and CRCs. ----------
        cells = {}  # cell index -> [count, crc32]
        total_edges = 0

        def on_way(refs, tags):
            stats["ways_total"] += 1
            highway = tags.get("highway")
            if highway is None:
                stats["skipped"]["not_highway"] += 1
                return
            if highway in WALKING_HIGHWAYS:
                cls = 1
            elif highway in ROAD_HIGHWAYS:
                cls = 2
            else:
                stats["skipped"]["unsupported_highway"] += 1
                return
            flags = 1 if _restricted(tags) else 0
            coords = []
            for ref in refs:
                point = _lookup(ref)
                if point is None:
                    raise BuildError("accepted highway references missing node %r; refusing incomplete map" % ref)
                coords.append(point)
            unique = []
            for point in coords:
                if not unique or unique[-1] != point:
                    unique.append(point)
            if len(unique) < 2:
                stats["skipped"]["empty_geometry"] += 1
                return
            emitted = 0
            for p, q in zip(unique, unique[1:]):
                for (_r, _c), sp, sq in split_edge(
                    p[0],
                    p[1],
                    q[0],
                    q[1],
                    origin_lat=origin_lat,
                    origin_lon=origin_lon,
                    cell=CELL_SIZE,
                    rows=rows,
                    cols=cols,
                ):
                    packed = _pack_edge(sp, sq, cls, flags)
                    idx = _r * cols + _c
                    entry = cells.get(idx)
                    if entry is None:
                        cells[idx] = [1, zlib.crc32(packed) & 0xFFFFFFFF]
                    else:
                        entry[0] += 1
                        entry[1] = zlib.crc32(packed, entry[1]) & 0xFFFFFFFF
                    if cells[idx][0] > MAX_EDGES_PER_CELL:
                        raise BuildError(
                            "cell %d exceeds %d edges" % (idx, MAX_EDGES_PER_CELL)
                        )
                    emitted += 1
            if emitted == 0:
                stats["skipped"]["empty_geometry"] += 1
                return
            stats["ways_retained"] += 1
            if cls == 1:
                stats["walking_ways"] += 1
            else:
                stats["road_ways"] += 1
            if flags:
                stats["restricted_ways"] += 1

        _OsmStream(input_path, on_way=on_way).run()
        _check_input_unchanged()
        for entry in cells.values():
            total_edges += entry[0]

        dir_size = rows * cols * ENTRY_SIZE
        data_offset = HEADER_SIZE + dir_size
        total_bytes = data_offset + total_edges * EDGE_SIZE
        if total_bytes > MAX_TOTAL_BYTES:
            raise BuildError(
                "map would be %d bytes, exceeding the 512 MiB limit" % total_bytes
            )
        offsets = {}
        cursor = data_offset
        for idx in range(rows * cols):
            if idx in cells:
                offsets[idx] = cursor
                cursor += cells[idx][0] * EDGE_SIZE
        if cursor != total_bytes:
            raise BuildError("internal layout error")
        dir_bytes = bytearray(dir_size)
        for idx, (count, crc) in cells.items():
            ENTRY_STRUCT.pack_into(
                dir_bytes, idx * ENTRY_SIZE, offsets[idx], count, crc
            )
        dir_crc = zlib.crc32(dir_bytes) & 0xFFFFFFFF

        # ---- Sweep 3: write payload at precomputed cell offsets. ----------
        emitted_by_cell = {}
        with open(tmp_path, "w+b") as fh:
            fh.seek(data_offset - dir_size)
            fh.write(dir_bytes)

            def emit(sp, sq, cls, flags, _r, _c):
                packed = _pack_edge(sp, sq, cls, flags)
                idx = _r * cols + _c
                if idx not in offsets:
                    raise BuildError("internal error: unexpected cell %d" % idx)
                written = emitted_by_cell.get(idx, 0)
                if written >= cells[idx][0]:
                    raise BuildError(
                        "cell %d emitted more edges than measured" % idx
                    )
                fh.seek(offsets[idx] + written * EDGE_SIZE)
                fh.write(packed)
                emitted_by_cell[idx] = written + 1

            def on_way_emit(refs, tags):
                highway = tags.get("highway")
                if highway in WALKING_HIGHWAYS:
                    cls = 1
                elif highway in ROAD_HIGHWAYS:
                    cls = 2
                else:
                    return
                flags = 1 if _restricted(tags) else 0
                coords = []
                for ref in refs:
                    point = _lookup(ref)
                    if point is None:
                        raise BuildError("accepted highway references missing node %r" % ref)
                    coords.append(point)
                unique = []
                for point in coords:
                    if not unique or unique[-1] != point:
                        unique.append(point)
                for p, q in zip(unique, unique[1:]):
                    for (_r, _c), sp, sq in split_edge(
                        p[0],
                        p[1],
                        q[0],
                        q[1],
                        origin_lat=origin_lat,
                        origin_lon=origin_lon,
                        cell=CELL_SIZE,
                        rows=rows,
                        cols=cols,
                    ):
                        emit(sp, sq, cls, flags, _r, _c)

            _OsmStream(input_path, on_way=on_way_emit).run()
            _check_input_unchanged()
            for idx, entry in cells.items():
                count = entry[0]
                if emitted_by_cell.get(idx, 0) != count:
                    raise BuildError(
                        "cell %d emitted %d edges, expected %d"
                        % (idx, emitted_by_cell.get(idx, 0), count)
                    )
            fh.seek(0, os.SEEK_END)
            actual_size = fh.tell()
            if actual_size != total_bytes:
                raise BuildError(
                    "package size %d does not match expected %d"
                    % (actual_size, total_bytes)
                )
            header = HEADER_STRUCT.pack(
                MAGIC,
                VERSION,
                HEADER_SIZE,
                total_bytes,
                origin_lat,
                origin_lon,
                CELL_SIZE,
                rows,
                cols,
                HEADER_SIZE,
                data_offset,
                dir_crc,
                0,
                0,
            )
            header_crc = zlib.crc32(header[:44]) & 0xFFFFFFFF
            header = header[:44] + struct.pack("<I", header_crc)
            fh.seek(0)
            fh.write(header)
            fh.flush()
            os.fsync(fh.fileno())

        # ---- Validate the finished temporary package before publishing. ---
        validated_edges = _validate_file(tmp_path)
        if validated_edges != total_edges:
            raise BuildError(
                "validation found %d edges, expected %d"
                % (validated_edges, total_edges)
            )
        if force:
            os.replace(tmp_path, output_path)
        else:
            # Atomic no-clobber publication, including another writer creating
            # the destination after the initial existence check.
            try:
                os.link(tmp_path, output_path)
            except FileExistsError:
                raise BuildError("output appeared while building; refusing to overwrite")
            os.unlink(tmp_path)
        try:
            dir_fd = os.open(out_dir, os.O_RDONLY)
            try:
                os.fsync(dir_fd)
            finally:
                os.close(dir_fd)
        except OSError:
            pass  # directory fsync is best effort

        stats.update(
            {
                "rows": rows,
                "columns": cols,
                "cells": rows * cols,
                "origin_lat": origin_lat,
                "origin_lon": origin_lon,
                "cell_size": CELL_SIZE,
                "edges": total_edges,
                "directory_bytes": dir_size,
                "total_bytes": total_bytes,
                "sha256": _sha256_file(output_path),
            }
        )
        stats["ways_skipped"] = sum(stats["skipped"].values())
        return stats
    finally:
        if conn is not None:
            try:
                conn.close()
            except sqlite3.Error:
                pass
        for leftover in (tmp_path, sqlite_path):
            try:
                if os.path.exists(leftover):
                    os.unlink(leftover)
            except OSError:
                pass


def main(argv=None):
    parser = argparse.ArgumentParser(
        prog="build_walkmap.py",
        description="Build an X3WM v1 walkmap from local OSM XML (no network).",
    )
    parser.add_argument("input", metavar="INPUT.osm", help="OSM XML input file")
    parser.add_argument(
        "output", metavar="OUTPUT.walkmap", help="X3WM v1 output file"
    )
    parser.add_argument(
        "--force",
        "--overwrite",
        action="store_true",
        dest="force",
        help="overwrite an existing output file",
    )
    args = parser.parse_args(argv)
    try:
        stats = build(args.input, args.output, force=args.force)
    except BuildError as exc:
        print("error: %s" % exc, file=sys.stderr)
        return 1
    print("wrote %s" % stats["output"])
    print("  size %d bytes, sha256 %s" % (stats["total_bytes"], stats["sha256"]))
    print(
        "  nodes %d (indexed %d, invalid %d)"
        % (stats["nodes_total"], stats["nodes_indexed"], stats["nodes_invalid"])
    )
    skipped = ", ".join(
        "%s %d" % (key, value)
        for key, value in sorted(stats["skipped"].items())
        if value
    )
    print(
        "  ways retained %d (walking %d, road %d, restricted %d), skipped %d"
        " (%s)"
        % (
            stats["ways_retained"],
            stats["walking_ways"],
            stats["road_ways"],
            stats["restricted_ways"],
            stats["ways_skipped"],
            skipped or "none",
        )
    )
    print(
        "  grid %dx%d origin (%d,%d) cell size %d edges %d"
        % (
            stats["rows"],
            stats["columns"],
            stats["origin_lat"],
            stats["origin_lon"],
            stats["cell_size"],
            stats["edges"],
        )
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
