"""Generate synthetic EPUBs and covers for simulator Library acceptance (no personal SD data)."""
from pathlib import Path
import argparse
import io
import zipfile

from PIL import Image, ImageDraw

parser = argparse.ArgumentParser()
parser.add_argument('directory', type=Path)
args = parser.parse_args()
(args.directory / 'books').mkdir(parents=True, exist_ok=True)
for number in range(24):
    cover = Image.new('RGB', (240, 360), 'white')
    draw = ImageDraw.Draw(cover)
    draw.rectangle((10, 10, 230, 350), outline='black', width=8)
    draw.rectangle((25, 120, 215, 230), fill='black' if number % 2 else '#888888')
    draw.text((35, 55), f'BOOK {number}', fill='black')
    image = io.BytesIO()
    cover.save(image, 'PNG')
    has_cover = number % 3 != 1
    manifest_cover = '<item id="cover" href="cover.png" media-type="image/png" properties="cover-image"/>' if has_cover else ''
    meta_cover = '<meta name="cover" content="cover"/>' if has_cover else ''
    opf = f'''<?xml version="1.0" encoding="UTF-8"?>
<package xmlns="http://www.idpf.org/2007/opf" version="3.0" unique-identifier="id">
<metadata xmlns:dc="http://purl.org/dc/elements/1.1/"><dc:identifier id="id">fixture-{number}</dc:identifier>
<dc:title>Library test {number:02}</dc:title><dc:creator>Test Author</dc:creator><dc:language>en</dc:language>{meta_cover}</metadata>
<manifest>{manifest_cover}<item id="chapter" href="chapter.xhtml" media-type="application/xhtml+xml"/>
<item id="nav" href="nav.xhtml" media-type="application/xhtml+xml" properties="nav"/></manifest>
<spine><itemref idref="chapter"/></spine></package>'''
    with zipfile.ZipFile(args.directory / 'books' / f'book{number}.epub', 'w') as book:
        book.writestr('mimetype', 'application/epub+zip', compress_type=zipfile.ZIP_STORED)
        book.writestr('META-INF/container.xml', '<?xml version="1.0"?><container version="1.0" xmlns="urn:oasis:names:tc:opendocument:xmlns:container"><rootfiles><rootfile full-path="OEBPS/content.opf" media-type="application/oebps-package+xml"/></rootfiles></container>')
        book.writestr('OEBPS/content.opf', opf)
        book.writestr('OEBPS/chapter.xhtml', '<html xmlns="http://www.w3.org/1999/xhtml"><head><title>Chapter</title></head><body><h1>Library fixture</h1>' + '<p>A synthetic book for testing physical button navigation and saved reading progress.</p>' * 60 + '</body></html>')
        book.writestr('OEBPS/nav.xhtml', '<html xmlns="http://www.w3.org/1999/xhtml" xmlns:epub="http://www.idpf.org/2007/ops"><head><title>Contents</title></head><body><nav epub:type="toc"><ol><li><a href="chapter.xhtml">Chapter</a></li></ol></nav></body></html>')
        if has_cover:
            book.writestr('OEBPS/cover.png', b'invalid cover fixture' if number % 3 == 2 else image.getvalue())
print(f'Created 24 synthetic EPUBs in {args.directory}')
