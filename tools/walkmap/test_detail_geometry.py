import unittest
from build_detail import hatch, text_label

class DetailGeometryTests(unittest.TestCase):
    def test_hole_is_not_filled(self):
        outer=[(0,0),(0,10000),(10000,10000),(10000,0),(0,0)]
        inner=[(3000,3000),(3000,7000),(7000,7000),(7000,3000),(3000,3000)]
        segments=list(hatch([outer,inner],1000))
        row=[(a[1],b[1]) for a,b in segments if a[0]==5000]
        self.assertEqual(row,[(0,3000),(7000,10000)])
    def test_ascii_length_is_bounded(self):
        self.assertEqual(text_label('Caféstraat'),'CAFESTRAAT')
        self.assertEqual(len(text_label('x'*100)),44)
        self.assertNotIn('\n',text_label('A\nB'))

if __name__ == '__main__':unittest.main()
