"""Unit tests for the X3WM v1 walkmap converter (docs/superpowers/specs/2026-09-04-x3-walkmap-design.md).

The tests exercise the converter's actual binary output geometry, CRCs,
negative boundaries, crossing cells, node gaps, malformed/invalid inputs and
output preservation on failure.  The checked-in synthetic fixture is not
geographic coverage evidence.
"""

import hashlib
import os
import struct
import subprocess
import sys
import tempfile
import unittest
import xml.etree.ElementTree as ET
import zlib
from decimal import Decimal, ROUND_HALF_UP
from pathlib import Path

import build_walkmap as bwm


REPO_ROOT = Path(__file__).resolve().parents[2]
FIXTURES = REPO_ROOT / "test" / "fixtures"
FIXTURE_OSM = FIXTURES / "walkmap-v1.osm"
FIXTURE_BIN = FIXTURES / "walkmap-v1.bin"

# SHA-256 of the checked-in binary fixture, regenerated from FIXTURE_OSM with
# `python3 tools/walkmap/build_walkmap.py test/fixtures/walkmap-v1.osm
# test/fixtures/walkmap-v1.bin --force`.  Guards converter determinism.
FIXTURE_SHA256 = "adbe72f71b85af8ed757f1477cec9c49399e5f76edf6466f8e8c3aeadec85b5b"

# Independent struct layouts straight from the spec (little endian).
HDR_FMT = "<4sHHIiiIHHIIIII"   # 48 bytes
DIR_FMT = "<III"               # 12 bytes per directory entry
EDG_FMT = "<iiiiBBH"           # 20 bytes per edge


def _e7(text):
    """Degrees-decimal string -> signed integer E7 (mirrors converter rule)."""
    scaled = (Decimal(text) * Decimal(10000000)).to_integral_value(
        rounding=ROUND_HALF_UP
    )
    return int(scaled)


def _scratch_base():
    base = os.environ.get("X3WM_SCRATCH")
    if not base:
        candidate = "/Volumes/2TB/x3-navigation-next-20260904/test_scratch"
        if os.path.isdir(candidate):
            base = candidate
    if not base:
        base = tempfile.gettempdir()
    os.makedirs(base, exist_ok=True)
    return base


def _scratch_dir(prefix="x3wm-unit-"):
    return Path(tempfile.mkdtemp(prefix=prefix, dir=_scratch_base()))


def _write_xml(scratch, name, text):
    p = scratch / name
    p.write_text(text, encoding="utf-8")
    return p


class ParsedMap:
    """Independent reader for X3WM v1 used to assert on real output bytes."""

    def __init__(self, path):
        self.path = path
        with open(path, "rb") as fh:
            self.blob = fh.read()
        assert HDR_FMT is not None
        (
            self.magic,
            self.version,
            self.header_len,
            self.total_bytes,
            self.origin_lat,
            self.origin_lon,
            self.cell_size,
            self.rows,
            self.cols,
            self.dir_offset,
            self.data_offset,
            self.dir_crc,
            self.reserved,
            self.header_crc,
        ) = struct.unpack_from(HDR_FMT, self.blob, 0)
        self.dir_size = self.rows * self.cols * 12
        self.cells = {}  # cell index -> entry dict with decoded edges
        self.nonempty = []
        for r in range(self.rows):
            for c in range(self.cols):
                idx = r * self.cols + c
                off, count, crc = struct.unpack_from(
                    DIR_FMT, self.blob, self.dir_offset + idx * 12
                )
                if off == 0 and count == 0 and crc == 0:
                    continue
                edges = []
                for i in range(count):
                    base = off + i * 20
                    lat1, lon1, lat2, lon2, cls, flags, resv = struct.unpack_from(
                        EDG_FMT, self.blob, base
                    )
                    edges.append(
                        {
                            "lat1": lat1,
                            "lon1": lon1,
                            "lat2": lat2,
                            "lon2": lon2,
                            "cls": cls,
                            "flags": flags,
                            "reserved": resv,
                            "cell": (r, c),
                        }
                    )
                entry = {
                    "r": r,
                    "c": c,
                    "offset": off,
                    "count": count,
                    "crc": crc,
                    "edges": edges,
                }
                self.cells[r * self.cols + c] = entry
                self.nonempty.append(entry)

    def cell_bounds(self, r, c):
        lat0 = self.origin_lat + r * self.cell_size
        lat1 = self.origin_lat + (r + 1) * self.cell_size
        lon0 = self.origin_lon + c * self.cell_size
        lon1 = self.origin_lon + (c + 1) * self.cell_size
        return lat0, lat1, lon0, lon1

    def all_edges(self):
        for entry in self.nonempty:
            yield from entry["edges"]


def _parse_osm(xml_path):
    """Return (node coords id->(lat,lon), ways) from a trusted OSM fixture."""
    root = ET.parse(str(xml_path)).getroot()
    nodes = {}
    for nd in root.findall("node"):
        nodes[int(nd.get("id"))] = (_e7(nd.get("lat")), _e7(nd.get("lon")))
    ways = []
    for w in root.findall("way"):
        refs = [int(n.get("ref")) for n in w.findall("nd")]
        tags = {t.get("k"): t.get("v") for t in w.findall("tag")}
        ways.append(
            {
                "id": int(w.get("id")),
                "refs": refs,
                "tags": tags,
                "coords": [nodes[i] for i in refs if i in nodes],
            }
        )
    return nodes, ways


def _retained_fixture_ways():
    """(way_id, class, restricted) for every way the converter must retain."""
    return {
        1: (1, False),
        2: (1, False),
        3: (1, False),
        4: (1, False),
        5: (1, False),
        6: (1, False),
        7: (2, False),
        8: (2, False),
        9: (1, True),
        10: (1, True),
        14: (1, False),
        15: (1, False),
    }


_CACHE = {}


def _built_fixture():
    """Build the fixture once per test run into a scratch directory."""
    if "path" not in _CACHE:
        work = _scratch_dir("x3wm-fixture-")
        out = work / "walkmap-v1.bin"
        stats = bwm.build(str(FIXTURE_OSM), str(out), force=True)
        _CACHE["path"] = str(out)
        _CACHE["stats"] = stats
    return _CACHE["path"], _CACHE["stats"]


class ConverterContractTests(unittest.TestCase):
    def test_build_matches_checked_in_fixture_and_sha(self):
        out, stats = _built_fixture()
        with open(out, "rb") as fh:
            produced = fh.read()
        with open(str(FIXTURE_BIN), "rb") as fh:
            checked_in = fh.read()
        self.assertEqual(produced, checked_in, "fixture .bin is stale or non-deterministic")
        digest = hashlib.sha256(produced).hexdigest()
        self.assertEqual(stats["sha256"], digest)
        self.assertEqual(FIXTURE_SHA256, digest)

    def test_fixture_stats_reported(self):
        _out, stats = _built_fixture()
        self.assertEqual(stats["nodes_total"], 35)
        self.assertEqual(stats["nodes_indexed"], 35)
        self.assertEqual(stats["nodes_invalid"], 0)
        self.assertEqual(stats["ways_total"], 14)
        self.assertEqual(stats["ways_retained"], 12)
        self.assertEqual(stats["ways_skipped"], 2)
        self.assertEqual(stats["walking_ways"], 10)
        self.assertEqual(stats["road_ways"], 2)
        self.assertEqual(stats["restricted_ways"], 2)
        self.assertEqual(stats["skipped"]["unsupported_highway"], 1)
        self.assertEqual(stats["skipped"]["missing_nodes"], 0)
        self.assertEqual(stats["skipped"]["empty_geometry"], 1)
        self.assertEqual(stats["skipped"]["not_highway"], 0)
        self.assertEqual(stats["rows"], 6)
        self.assertEqual(stats["columns"], 6)
        self.assertEqual(stats["cells"], 36)
        self.assertEqual(stats["cell_size"], 200000)
        self.assertEqual(stats["origin_lat"], 519600000)
        self.assertEqual(stats["origin_lon"], 47600000)
        self.assertGreater(stats["edges"], 0)
        self.assertIn("sha256", stats)

    def test_header_fields_and_crcs(self):
        out, _ = _built_fixture()
        m = ParsedMap(out)
        self.assertEqual(m.magic, b"X3WM")
        self.assertEqual(m.version, 1)
        self.assertEqual(m.header_len, 48)
        self.assertEqual(m.cell_size, 200000)
        self.assertEqual(m.dir_offset, 48)
        self.assertEqual(m.data_offset, 48 + m.rows * m.cols * 12)
        self.assertEqual(m.reserved, 0)
        self.assertEqual(len(m.blob), m.total_bytes)
        self.assertLessEqual(m.total_bytes, 512 * 1024 * 1024)
        # Header CRC covers the first 44 bytes only.
        self.assertEqual(
            m.header_crc, zlib.crc32(m.blob[:44]) & 0xFFFFFFFF
        )
        # Directory CRC covers the whole dense directory.
        dir_bytes = m.blob[m.dir_offset : m.data_offset]
        self.assertEqual(len(dir_bytes), m.rows * m.cols * 12)
        self.assertEqual(m.dir_crc, zlib.crc32(dir_bytes) & 0xFFFFFFFF)
        # World bounds within spec limits.
        self.assertGreaterEqual(m.origin_lat, -850000000)
        self.assertLessEqual(m.origin_lat + m.rows * m.cell_size, 850000000)
        self.assertGreaterEqual(m.origin_lon, -1800000000)
        self.assertLessEqual(m.origin_lon + m.cols * m.cell_size, 1800000000)

    def test_directory_layout_and_cell_crcs(self):
        out, _ = _built_fixture()
        m = ParsedMap(out)
        self.assertGreater(m.rows * m.cols, 0)
        self.assertLessEqual(m.rows * m.cols, 65536)
        self.assertTrue(any(e["count"] > 0 for e in m.nonempty))
        # There must be all-zero (empty) directory entries in the fixture.
        self.assertLess(len(m.nonempty), m.rows * m.cols)
        for entry in m.nonempty:
            self.assertGreater(entry["count"], 0)
            self.assertLessEqual(entry["count"], 16384)
            self.assertGreaterEqual(entry["offset"], m.data_offset)
            self.assertLessEqual(
                entry["offset"] + entry["count"] * 20, m.total_bytes
            )
            payload = m.blob[entry["offset"] : entry["offset"] + entry["count"] * 20]
            self.assertEqual(entry["crc"], zlib.crc32(payload) & 0xFFFFFFFF)

    def test_output_is_deterministic(self):
        work = _scratch_dir()
        out1 = work / "a.walkmap"
        out2 = work / "b.walkmap"
        s1 = bwm.build(str(FIXTURE_OSM), str(out1), force=True)
        s2 = bwm.build(str(FIXTURE_OSM), str(out2), force=True)
        self.assertEqual(s1["sha256"], s2["sha256"])
        self.assertEqual(out1.read_bytes(), out2.read_bytes())

    def test_edge_geometry_within_cells_and_record_shape(self):
        out, _ = _built_fixture()
        m = ParsedMap(out)
        seen = 0
        for e in m.all_edges():
            seen += 1
            r, c = e["cell"]
            lat0, lat1, lon0, lon1 = m.cell_bounds(r, c)
            for lat, lon in ((e["lat1"], e["lon1"]), (e["lat2"], e["lon2"])):
                self.assertGreaterEqual(lat, lat0)
                self.assertLessEqual(lat, lat1)
                self.assertGreaterEqual(lon, lon0)
                self.assertLessEqual(lon, lon1)
            self.assertNotEqual((e["lat1"], e["lon1"]), (e["lat2"], e["lon2"]))
            self.assertIn(e["cls"], (1, 2))
            self.assertEqual(e["flags"] & ~1, 0, "only access-restricted bit allowed")
            self.assertEqual(e["reserved"], 0)
        self.assertGreater(seen, 0)
        self.assertEqual(seen, sum(e["count"] for e in m.nonempty))

    def test_retained_way_vertices_preserved_with_class_and_flags(self):
        out, stats = _built_fixture()
        m = ParsedMap(out)
        _nodes, ways = _parse_osm(FIXTURE_OSM)
        by_id = {w["id"]: w for w in ways}
        expected = _retained_fixture_ways()
        self.assertEqual(len(expected), stats["ways_retained"])
        endpoint_usage = {}
        for e in m.all_edges():
            for lat, lon in ((e["lat1"], e["lon1"]), (e["lat2"], e["lon2"])):
                endpoint_usage.setdefault((lat, lon), []).append(e)
        for way_id, (cls, restricted) in expected.items():
            for lat, lon in by_id[way_id]["coords"]:
                hits = endpoint_usage.get((lat, lon), [])
                self.assertTrue(
                    hits,
                    "vertex (%d,%d) of retained way %d was dropped"
                    % (lat, lon, way_id),
                )
                self.assertTrue(
                    any(e["cls"] == cls and bool(e["flags"] & 1) == restricted
                        for e in hits),
                    "vertex of way %d lacks expected class/restriction" % way_id,
                )

    def test_no_invented_joins(self):
        out, _ = _built_fixture()
        m = ParsedMap(out)
        _nodes, ways = _parse_osm(FIXTURE_OSM)
        retained = _retained_fixture_ways()
        original = set()
        consecutive = set()
        for w in ways:
            if w["id"] not in retained:
                continue
            for lat, lon in w["coords"]:
                original.add((lat, lon))
            for a, b in zip(w["coords"], w["coords"][1:]):
                consecutive.add((a, b))
                consecutive.add((b, a))
        for e in m.all_edges():
            p = (e["lat1"], e["lon1"])
            q = (e["lat2"], e["lon2"])
            if p in original and q in original:
                self.assertIn(
                    (p, q),
                    consecutive,
                    "edge joins two original vertices that are not consecutive "
                    "in one source way (invented join)",
                )

    def test_split_points_lie_on_grid_lines(self):
        out, _ = _built_fixture()
        m = ParsedMap(out)
        _nodes, ways = _parse_osm(FIXTURE_OSM)
        retained = _retained_fixture_ways()
        original = set()
        for w in ways:
            if w["id"] in retained:
                original.update(w["coords"])
        for e in m.all_edges():
            for lat, lon in ((e["lat1"], e["lon1"]), (e["lat2"], e["lon2"])):
                if (lat, lon) in original:
                    continue
                on_lat_line = (lat - m.origin_lat) % m.cell_size == 0
                on_lon_line = (lon - m.origin_lon) % m.cell_size == 0
                self.assertTrue(
                    on_lat_line or on_lon_line,
                    "non-source endpoint (%d,%d) is not on a cell boundary"
                    % (lat, lon),
                )

    def test_crossing_way_split_across_cells(self):
        out, _ = _built_fixture()
        m = ParsedMap(out)
        endpoint_cells = {}
        for e in m.all_edges():
            for lat, lon in ((e["lat1"], e["lon1"]), (e["lat2"], e["lon2"])):
                endpoint_cells.setdefault((lat, lon), set()).add(e["cell"])
        # Way 1 (footway 1001..1004) start/end vertices both present.
        self.assertIn((519900000, 47900000), endpoint_cells)
        self.assertIn((520500000, 48500000), endpoint_cells)
        # Way 1 must have been split across several directory cells.
        way1_vertices = {
            (519900000, 47900000),
            (520100000, 48050000),
            (520300000, 48200000),
            (520500000, 48500000),
        }
        cells_with_way1 = set()
        for v in way1_vertices:
            cells_with_way1.update(endpoint_cells.get(v, set()))
        self.assertGreaterEqual(len(cells_with_way1), 4)
        # Way 7 crosses the cell corner (52.00, 4.80): the corner must be
        # shared as an endpoint between edges in at least two different cells.
        corner = (520000000, 48000000)
        self.assertIn(corner, endpoint_cells)
        self.assertGreaterEqual(len(endpoint_cells[corner]), 2)
        # Grid-line shared endpoint between two adjacent cells (continuity).
        shared = [
            (pt, cells)
            for pt, cells in endpoint_cells.items()
            if len(cells) >= 2
            and (
                (pt[0] - m.origin_lat) % m.cell_size == 0
                or (pt[1] - m.origin_lon) % m.cell_size == 0
            )
        ]
        self.assertTrue(shared, "no split point shared between two cells")

    def test_removed_invalid_fixture_way_is_not_bridged(self):
        out, stats = _built_fixture()
        m = ParsedMap(out)
        self.assertEqual(stats["skipped"]["missing_nodes"], 0)
        # Way 11's defined endpoints must never appear (whole way rejected,
        # no invented connection across the missing middle node).
        for e in m.all_edges():
            self.assertNotEqual(
                (e["lat1"], e["lon1"]), (520010000, 48050000)
            )
            self.assertNotEqual(
                (e["lat2"], e["lon2"]), (520020000, 48050000)
            )


class SplitUnitTests(unittest.TestCase):
    """Direct geometric tests of the cell-splitting helper."""

    ORIGIN_LAT = 519600000  # 51.96
    ORIGIN_LON = 47600000   # 4.76
    CELL = 200000
    ROWS = 6
    COLS = 6

    def split(self, a, b, rows=ROWS, cols=COLS):
        return bwm.split_edge(
            a[0], a[1], b[0], b[1],
            origin_lat=self.ORIGIN_LAT,
            origin_lon=self.ORIGIN_LON,
            cell=self.CELL,
            rows=rows,
            cols=cols,
        )

    def test_segment_crossing_row_and_column(self):
        a = (519900000, 47900000)   # 51.99, 4.79
        b = (520100000, 48050000)   # 52.01, 4.805
        pieces = self.split(a, b)
        self.assertEqual([p[0] for p in pieces], [(1, 1), (2, 1), (2, 2)])
        self.assertEqual(pieces[0][1], a)
        self.assertEqual(pieces[-1][2], b)
        # Split points are shared between consecutive pieces.
        self.assertEqual(pieces[0][2], pieces[1][1])
        self.assertEqual(pieces[1][2], pieces[2][1])
        # The lat-line crossing is exactly 52.00 with interpolated lon 4.7975.
        self.assertEqual(pieces[0][2], (520000000, 47975000))
        # The lon-line crossing is exactly 4.80 with interpolated lat 52.0033333.
        self.assertEqual(pieces[1][2], (520033333, 48000000))

    def test_edge_along_lat_grid_line_belongs_to_cell_below(self):
        a = (520000000, 47900000)   # exactly on lat 52.00 line
        b = (520000000, 48010000)
        pieces = self.split(a, b)
        self.assertEqual([p[0] for p in pieces], [(1, 1), (1, 2)])
        # Both pieces keep endpoints exactly on the shared lat line.
        self.assertEqual(pieces[0][1][0], 520000000)
        self.assertEqual(pieces[1][2][0], 520000000)

    def test_edge_along_lon_grid_line_belongs_to_cell_left(self):
        a = (520010000, 48000000)   # exactly on lon 4.80 line
        b = (520020000, 48000000)
        pieces = self.split(a, b)
        self.assertEqual([p[0] for p in pieces], [(2, 1)])
        self.assertEqual(pieces[0][1], a)
        self.assertEqual(pieces[0][2], b)

    def test_vertex_exactly_on_boundary_crosses_without_degenerate_edge(self):
        # 51.995 -> 52.00 (on line) -> 52.005
        p1 = self.split((519950000, 48040000), (520000000, 48040000))
        p2 = self.split((520000000, 48040000), (520050000, 48040000))
        self.assertEqual([p[0] for p in p1], [(1, 2)])
        self.assertEqual([p[0] for p in p2], [(2, 2)])
        for pieces in (p1, p2):
            for _cell, q, r in pieces:
                self.assertNotEqual(q, r)

    def test_single_cell_segment_unsplit(self):
        a = (520010000, 48010000)
        b = (520020000, 48020000)
        pieces = self.split(a, b)
        self.assertEqual(len(pieces), 1)
        self.assertEqual(pieces[0][0], (2, 2))
        self.assertEqual(pieces[0][1], a)
        self.assertEqual(pieces[0][2], b)

    def test_negative_coordinates_split(self):
        origin_lat = -339000000
        origin_lon = -706800000
        a = (-338700000, -706600000)   # -33.87, -70.66 (col 1 west edge)
        b = (-338600000, -706300000)   # -33.86, -70.63 (crosses lon -70.64)
        pieces = bwm.split_edge(
            a[0], a[1], b[0], b[1],
            origin_lat=origin_lat,
            origin_lon=origin_lon,
            cell=self.CELL,
            rows=4,
            cols=4,
        )
        self.assertEqual([p[0] for p in pieces], [(1, 1), (1, 2)])
        self.assertEqual(pieces[0][1], a)
        self.assertEqual(pieces[-1][2], b)
        # Split point sits exactly on the -70.64 longitude line.
        self.assertEqual(pieces[0][2], (-338633333, -706400000))
        self.assertEqual(pieces[0][2], pieces[1][1])

    def test_degenerate_zero_length_input_rejected(self):
        a = (520010000, 48010000)
        pieces = self.split(a, a)
        self.assertEqual(pieces, [])


class NegativeAndBoundsTests(unittest.TestCase):
    def test_negative_coordinates_roundtrip(self):
        work = _scratch_dir()
        src = _write_xml(
            work,
            "negative.osm",
            """<?xml version="1.0"?>
<osm version="0.6">
 <node id="1" lat="-33.8700000" lon="-70.6600000"/>
 <node id="2" lat="-33.8600000" lon="-70.6500000"/>
 <node id="3" lat="-33.8680000" lon="-70.6480000"/>
 <way id="10"><nd ref="1"/><nd ref="2"/><tag k="highway" v="footway"/></way>
 <way id="11"><nd ref="2"/><nd ref="3"/><tag k="highway" v="path"/></way>
</osm>
""",
        )
        out = work / "negative.bin"
        stats = bwm.build(str(src), str(out), force=True)
        self.assertEqual(stats["origin_lat"], -339000000)
        self.assertEqual(stats["origin_lon"], -706800000)
        self.assertEqual(stats["ways_retained"], 2)
        m = ParsedMap(out)
        endpoints = set()
        for e in m.all_edges():
            for lat, lon in ((e["lat1"], e["lon1"]), (e["lat2"], e["lon2"])):
                endpoints.add((lat, lon))
        for expected in (
            (-338700000, -706600000),
            (-338600000, -706500000),
            (-338680000, -706480000),
        ):
            self.assertIn(expected, endpoints)

    def test_oversized_grid_rejected(self):
        work = _scratch_dir()
        lines = ['<?xml version="1.0"?>', '<osm version="0.6">']
        nid = 1
        lat = -80.0
        while lat <= 80.0:
            lines.append(
                '<node id="%d" lat="%.7f" lon="-80.0000000"/>' % (nid, lat)
            )
            nid += 1
            lat += 10.0
        lat = -80.0
        while lat <= 80.0:
            lines.append(
                '<node id="%d" lat="%.7f" lon="80.0000000"/>' % (nid, lat)
            )
            nid += 1
            lat += 10.0
        lines.append('<node id="%d" lat="0.0000000" lon="0.0000000"/>' % nid)
        lines.append("</osm>")
        src = _write_xml(work, "wide.osm", "\n".join(lines))
        out = work / "wide.bin"
        with self.assertRaises(bwm.BuildError) as cm:
            bwm.build(str(src), str(out), force=True)
        self.assertIn("grid", str(cm.exception).lower())
        self.assertIn("65536", str(cm.exception))
        self.assertFalse(out.exists())

    def test_world_latitude_bounds_rejected(self):
        work = _scratch_dir()
        src = _write_xml(
            work,
            "polar.osm",
            """<?xml version="1.0"?>
<osm version="0.6">
 <node id="1" lat="86.0000000" lon="4.0000000"/>
</osm>
""",
        )
        out = work / "polar.bin"
        with self.assertRaises(bwm.BuildError) as cm:
            bwm.build(str(src), str(out), force=True)
        self.assertIn("latitude", str(cm.exception).lower())
        self.assertFalse(out.exists())

    def test_dateline_extent_rejected(self):
        work = _scratch_dir()
        src = _write_xml(
            work,
            "dateline.osm",
            """<?xml version="1.0"?>
<osm version="0.6">
 <node id="1" lat="0.0000000" lon="-179.9000000"/>
 <node id="2" lat="0.0100000" lon="179.9000000"/>
</osm>
""",
        )
        out = work / "dateline.bin"
        with self.assertRaises(bwm.BuildError) as cm:
            bwm.build(str(src), str(out), force=True)
        self.assertIn("dateline", str(cm.exception).lower())
        self.assertFalse(out.exists())

    def test_no_indexable_nodes_rejected(self):
        work = _scratch_dir()
        src = _write_xml(
            work,
            "nonodes.osm",
            """<?xml version="1.0"?>
<osm version="0.6">
 <way id="1"><nd ref="7"/><nd ref="8"/><tag k="highway" v="footway"/></way>
</osm>
""",
        )
        out = work / "nonodes.bin"
        with self.assertRaises(bwm.BuildError) as cm:
            bwm.build(str(src), str(out), force=True)
        self.assertIn("node", str(cm.exception).lower())
        self.assertFalse(out.exists())

    def test_unused_ways_yield_valid_empty_map(self):
        work = _scratch_dir()
        src = _write_xml(
            work,
            "empty.osm",
            """<?xml version="1.0"?>
<osm version="0.6">
 <node id="1" lat="52.0010000" lon="4.8010000"/>
 <node id="2" lat="52.0020000" lon="4.8020000"/>
 <way id="20"><nd ref="1"/><nd ref="2"/><tag k="highway" v="cycleway"/></way>
</osm>
""",
        )
        out = work / "empty.bin"
        stats = bwm.build(str(src), str(out), force=True)
        self.assertEqual(stats["ways_retained"], 0)
        self.assertEqual(stats["edges"], 0)
        self.assertEqual(stats["skipped"]["unsupported_highway"], 1)
        m = ParsedMap(out)
        self.assertEqual(m.nonempty, [])
        self.assertEqual(m.header_crc, zlib.crc32(m.blob[:44]) & 0xFFFFFFFF)
        self.assertEqual(
            m.dir_crc,
            zlib.crc32(m.blob[m.dir_offset : m.data_offset]) & 0xFFFFFFFF,
        )


class InvalidInputTests(unittest.TestCase):
    def _xml(self, text):
        work = _scratch_dir()
        src = _write_xml(work, "input.osm", text)
        out = work / "out.bin"
        return src, out

    def test_missing_and_invalid_nodes_refuse_partial_output(self):
        for coordinate in ("NaN", "Infinity", "91.0", "not-a-number"):
            src, out = self._xml('<osm><node id="1" lat="52.0" lon="4.8"/>'
                                '<node id="2" lat="%s" lon="4.8"/>'
                                '<way id="1"><nd ref="1"/><nd ref="2"/>'
                                '<tag k="highway" v="path"/></way></osm>' % coordinate)
            with self.assertRaises(bwm.BuildError):
                bwm.build(str(src), str(out), force=True)
            self.assertFalse(out.exists())

    def test_malformed_xml_rejected(self):
        for text in (
            "this is not xml at all <<<",
            "<?xml version='1.0'?><osm><node id='1' lat='52.0' lon='4.8'",
        ):
            src, out = self._xml(text)
            with self.assertRaises(bwm.BuildError):
                bwm.build(str(src), str(out), force=True)
            self.assertFalse(out.exists())

    def test_doctype_and_external_entities_rejected(self):
        src, out = self._xml(
            """<?xml version="1.0"?>
<!DOCTYPE osm [<!ENTITY xxe SYSTEM "file:///etc/hosts">]>
<osm version="0.6">
 <node id="1" lat="52.0010000" lon="4.8010000"/>
 <node id="2" lat="52.0020000" lon="4.8020000"/>
 <way id="1"><nd ref="1"/><nd ref="2"/>
   <tag k="highway" v="footway"/><tag k="note" v="&xxe;"/></way>
</osm>
"""
        )
        with self.assertRaises(bwm.BuildError) as cm:
            bwm.build(str(src), str(out), force=True)
        self.assertIn("doctype", str(cm.exception).lower())
        self.assertFalse(out.exists())

    def test_undefined_entity_rejected(self):
        src, out = self._xml(
            """<?xml version="1.0"?>
<osm version="0.6">
 <node id="1" lat="52.0010000" lon="4.8010000"/>
 <node id="2" lat="52.0020000" lon="4.8020000"/>
 <way id="1"><nd ref="1"/><nd ref="2"/>
   <tag k="highway" v="footway"/><tag k="note" v="&bogus;"/></way>
</osm>
"""
        )
        with self.assertRaises(bwm.BuildError):
            bwm.build(str(src), str(out), force=True)
        self.assertFalse(out.exists())


class OutputProtectionTests(unittest.TestCase):
    def test_existing_output_protected_without_force(self):
        work = _scratch_dir()
        out = work / "out.bin"
        first = bwm.build(str(FIXTURE_OSM), str(out), force=True)
        before = out.read_bytes()
        with self.assertRaises(bwm.BuildError) as cm:
            bwm.build(str(FIXTURE_OSM), str(out))
        self.assertIn("exists", str(cm.exception).lower())
        self.assertIn("force", str(cm.exception).lower())
        self.assertEqual(out.read_bytes(), before)
        # With force the output is replaced.
        second = bwm.build(str(FIXTURE_OSM), str(out), force=True)
        self.assertEqual(first["sha256"], second["sha256"])
        self.assertEqual(out.read_bytes(), before)

    def test_failure_preserves_existing_output(self):
        work = _scratch_dir()
        out = work / "out.bin"
        good = _write_xml(
            work,
            "good.osm",
            """<?xml version="1.0"?>
<osm version="0.6">
 <node id="1" lat="52.0010000" lon="4.8010000"/>
 <node id="2" lat="52.0020000" lon="4.8020000"/>
 <way id="1"><nd ref="1"/><nd ref="2"/><tag k="highway" v="footway"/></way>
</osm>
""",
        )
        bwm.build(str(good), str(out), force=True)
        sentinel = out.read_bytes()
        bad = _write_xml(work, "bad.osm", "<not xml")
        with self.assertRaises(bwm.BuildError):
            bwm.build(str(bad), str(out), force=True)
        self.assertEqual(out.read_bytes(), sentinel)

    def test_failed_build_leaves_no_temp_files(self):
        work = _scratch_dir()
        out = work / "out.bin"
        src = _write_xml(work, "bad.osm", "<not xml")
        with self.assertRaises(bwm.BuildError):
            bwm.build(str(src), str(out), force=True)
        leftovers = [p.name for p in work.iterdir()]
        self.assertEqual(leftovers, ["bad.osm"])

    def test_cli_overwrite_flag(self):
        work = _scratch_dir()
        out = work / "out.bin"
        module = Path(bwm.__file__).resolve()
        ok = subprocess.run(
            [sys.executable, str(module), str(FIXTURE_OSM), str(out), "--force"],
            capture_output=True,
            text=True,
        )
        self.assertEqual(ok.returncode, 0, ok.stderr)
        self.assertIn("sha256", ok.stdout.lower())
        before = out.read_bytes()
        blocked = subprocess.run(
            [sys.executable, str(module), str(FIXTURE_OSM), str(out)],
            capture_output=True,
            text=True,
        )
        self.assertNotEqual(blocked.returncode, 0)
        self.assertIn("--force", blocked.stderr)
        self.assertEqual(out.read_bytes(), before)
        again = subprocess.run(
            [sys.executable, str(module), str(FIXTURE_OSM), str(out), "--force"],
            capture_output=True,
            text=True,
        )
        self.assertEqual(again.returncode, 0, again.stderr)
        self.assertEqual(out.read_bytes(), before)


if __name__ == "__main__":
    unittest.main()
