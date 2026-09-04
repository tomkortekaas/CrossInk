import unittest
from build_gray import pack_pixels, draw_area
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

if __name__=='__main__':unittest.main()
