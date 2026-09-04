"""Independent Codex acceptance regressions for preservation and publication."""
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch
import build_walkmap as bwm

GOOD = '<osm version="0.6"><node id="1" lat="52.001" lon="4.001"/><node id="2" lat="52.002" lon="4.002"/><way id="1"><nd ref="1"/><nd ref="2"/><tag k="highway" v="path"/></way></osm>'
class AcceptanceTests(unittest.TestCase):
    def work(self):
        temp=tempfile.TemporaryDirectory(dir='/Volumes/2TB/x3-navigation-next-20260904')
        self.addCleanup(temp.cleanup)
        root=Path(temp.name);source=root/'input.osm';source.write_text(GOOD)
        return source,root/'output.walkmap'
    def test_missing_accepted_way_node_fails_entire_build(self):
        source,out=self.work();source.write_text(GOOD.replace('ref="2"','ref="99"'))
        with self.assertRaises(bwm.BuildError): bwm.build(source,out)
        self.assertFalse(out.exists())
    def test_invalid_node_fails_instead_of_publishing_partial_map(self):
        source,out=self.work();source.write_text(GOOD.replace('52.002','NaN'))
        with self.assertRaises(bwm.BuildError): bwm.build(source,out)
        self.assertFalse(out.exists())
    def test_late_created_destination_is_not_overwritten(self):
        source,out=self.work();validate=bwm._validate_file
        def concurrent_publish(path):
            result=validate(path);out.write_bytes(b'other writer');return result
        with patch.object(bwm,'_validate_file',side_effect=concurrent_publish):
            with self.assertRaises(bwm.BuildError): bwm.build(source,out)
        self.assertEqual(out.read_bytes(),b'other writer')
    def test_input_cannot_be_its_own_forced_output(self):
        source,_=self.work()
        with self.assertRaises(bwm.BuildError): bwm.build(source,source,force=True)
        self.assertEqual(source.read_text(),GOOD)
    def test_one_e7_segment_at_boundary_keeps_both_endpoints(self):
        pieces=bwm.split_edge(200000,200000,200001,200001,origin_lat=0,origin_lon=0,cell=200000,rows=3,cols=3)
        self.assertEqual(pieces,[((1,1),(200000,200000),(200001,200001))])

    def test_unused_distant_nodes_do_not_expand_walking_grid(self):
        source,out=self.work()
        source.write_text(GOOD.replace('</osm>', '<node id="777" lat="0" lon="90"/></osm>'))
        result=bwm.build(source,out)
        self.assertLess(result['rows'],10)
        self.assertLess(result['columns'],10)
