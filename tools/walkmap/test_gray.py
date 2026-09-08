import unittest
from build_gray import classify, pack_pixels, draw_area
from PIL import Image

class GrayMapTests(unittest.TestCase):
    def test_four_levels_msb_first(self):
        self.assertEqual(pack_pixels(bytes([0,85,170,255,255,170,85,0])), bytes([0x1b,0xe4]))

    def test_hole_preserves_underlying_map(self):
        image=Image.new('L',(16,16),85)
        draw_area(image,[[(1,1),(14,1),(14,14),(1,14)],[(5,5),(10,5),(10,10),(5,10)]],170)
        self.assertEqual(image.getpixel((3,3)),170)
        self.assertEqual(image.getpixel((7,7)),85)
        self.assertEqual(image.getpixel((0,0)),85)

    def test_building_areas_are_excluded_from_raster_layers(self):
        # A building footprint is never selected for the calm four-tone raster,
        # regardless of which other landuse/natural tags the polygon carries.
        self.assertIsNone(classify({'building': 'yes'}))
        self.assertIsNone(classify({'building': 'house', 'landuse': 'residential'}))
        self.assertIsNone(classify({'building': 'yes', 'natural': 'water'}))
        self.assertIsNone(classify({'building': 'yes', 'leisure': 'park'}))

    def test_water_and_green_keep_distinguishable_tone_layers(self):
        # Water stays the dark raster layer (1) and green/open land the light
        # layer (0), so the two remain distinguishable after buildings are gone.
        self.assertEqual(classify({'natural': 'water'}), 1)
        self.assertEqual(classify({'waterway': 'riverbank'}), 1)
        self.assertEqual(classify({'landuse': 'reservoir'}), 1)
        self.assertEqual(classify({'landuse': 'forest'}), 0)
        self.assertEqual(classify({'natural': 'wood'}), 0)
        self.assertEqual(classify({'leisure': 'park'}), 0)
        self.assertIsNone(classify({}))
        self.assertIsNone(classify({'landuse': 'residential'}))

if __name__=='__main__':unittest.main()
