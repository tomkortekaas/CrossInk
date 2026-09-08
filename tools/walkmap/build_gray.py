#!/usr/bin/env python3
"""Offline four-tone X3GM map. Pillow/Pyosmium run on the Mac only.

Tiles are geographic (not Web Mercator), 512x832 at 0.01 degree: about
1.3 metres per source pixel in Noord-Holland. Labels remain vector anchors.
The device reads one 128-byte raster row at a time; no tile allocation.
The four tones are white (background), light gray (green / open land), dark
gray (water) and black (road/track corridors and the route). Individual
building footprints are deliberately excluded from the raster: at a calm
four-tone rendering they read as noise, and removing them keeps the map
quiet while water and green/open land stay distinguishable.
"""
import argparse, math, os, pickle, sqlite3, struct, tempfile, zlib
from pathlib import Path
from PIL import Image, ImageDraw
from build_detail import RECORD, CELL
from build_walkmap import HEADER_STRUCT

WIDTH, HEIGHT = 512, 832
HEADER = struct.Struct('<4sHHIiiIHHHHIIII')
LABEL = struct.Struct('<ii40s')

def pack_pixels(data):
    return Image.frombytes('L',(len(data),1),data).point(lambda v:v//85,'P').tobytes('raw','P;2')

def draw_area(image,rings,tone):
    mask=Image.new('1',image.size)
    draw=ImageDraw.Draw(mask)
    draw.polygon(rings[0],fill=1)
    for ring in rings[1:]:draw.polygon(ring,fill=0)
    image.paste(tone,(0,0),mask)

def classify(tags):
    """Pick the gray raster layer for one OSM area polygon, or None to skip it.

    Pure tag decision (no geometry), so the converter tests can drive it
    directly. Returns 1 for water (the dark tone), 0 for green/open land (the
    light tone) and None for everything else. Building footprints always
    return None: the calm four-tone gray map never rasterizes buildings, even
    when a footprint also carries a water/green/leisure tag.
    """
    if tags.get('building', 'no') != 'no':
        return None
    water = (tags.get('natural') == 'water' or tags.get('waterway') == 'riverbank'
             or tags.get('landuse') in {'reservoir', 'basin'})
    if water:
        return 1
    green = (tags.get('landuse') in {'forest', 'grass', 'meadow', 'recreation_ground'}
             or tags.get('natural') in {'wood', 'scrub', 'grassland'}
             or tags.get('leisure') in {'park', 'garden'})
    return 0 if green else None

def build(source,base,detail,output):
    import osmium
    output=Path(output)
    if output.exists():raise ValueError('Refusing to overwrite existing output')
    with open(base,'rb') as f:h=HEADER_STRUCT.unpack(f.read(48))
    if h[0]!=b'X3WM' or h[1]!=1:raise ValueError('v1 base required')
    south,west=h[4:6];rows,cols=h[7]*2,h[8]*2
    output.parent.mkdir(parents=True,exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='gray-',dir=output.parent) as tmp:
        db=sqlite3.connect(str(Path(tmp)/'areas.db'))
        db.execute('PRAGMA journal_mode=OFF');db.execute('PRAGMA synchronous=OFF')
        db.execute('CREATE TABLE areas(cell INTEGER,layer INTEGER,geometry BLOB)')
        count=0
        class Handler(osmium.SimpleHandler):
            def area(self,area):
                nonlocal count
                tags=dict(area.tags)
                layer=classify(tags)
                if layer is None:return
                for outer in area.outer_rings():
                    rings=[[(round(n.lon*1e7),round(n.lat*1e7)) for n in ring] for ring in [outer,*area.inner_rings(outer)]]
                    if len(rings[0])<3:continue
                    xs,ys=zip(*rings[0]);r0=max(0,(min(ys)-south)//CELL);r1=min(rows-1,(max(ys)-south)//CELL)
                    c0=max(0,(min(xs)-west)//CELL);c1=min(cols-1,(max(xs)-west)//CELL)
                    raw=pickle.dumps(rings,protocol=4)
                    for r in range(r0,r1+1):
                        for c in range(c0,c1+1):db.execute('INSERT INTO areas VALUES(?,?,?)',(r*cols+c,layer,raw))
                    count+=1
                    if count%100000==0:db.commit();print('areas',count,flush=True)
        Handler().apply_file(str(source),locations=True,idx='sparse_file_array,'+str(Path(tmp)/'nodes.idx'))
        db.commit();db.execute('CREATE INDEX ordered ON areas(cell,layer)');db.commit()
        directory=bytearray(rows*cols*12);dataoff=48+len(directory)
        with open(detail,'rb') as vectors, open(Path(tmp)/'map','w+b') as out:
            dh=HEADER_STRUCT.unpack(vectors.read(48))
            if dh[0]!=b'X3WM' or dh[1]!=2 or dh[4:9]!=(south,west,CELL,rows,cols):raise ValueError('Detail grid mismatch')
            vd=vectors.read(rows*cols*12)
            out.write(bytes(dataoff))
            for cell in range(rows*cols):
                r,c=divmod(cell,cols);lat=south+(r+1)*CELL;lon=west+c*CELL
                def project(p):return ((p[0]-lon)*WIDTH/CELL,(lat-p[1])*HEIGHT/CELL)
                im=Image.new('L',(WIDTH,HEIGHT),255)
                for layer,raw in db.execute('SELECT layer,geometry FROM areas WHERE cell=? ORDER BY layer',(cell,)):
                    rings=[[project(p) for p in ring] for ring in pickle.loads(raw)]
                    draw_area(im,rings,85 if layer==1 else 170)
                draw=ImageDraw.Draw(im);labels=[]
                off,n,crc=struct.unpack_from('<III',vd,cell*12)
                vectors.seek(off);raw=vectors.read(n*64)
                if len(raw)!=n*64 or zlib.crc32(raw)!=crc:raise ValueError('Corrupt input detail cell')
                for ay,ax,by,bx,kind,flags,length,reserved,text in RECORD.iter_unpack(raw):
                    a,b=project((ax,ay)),project((bx,by))
                    if kind in (2,3):
                        width=7 if kind==3 else 5
                        draw.line([a,b],fill=85,width=width)
                        draw.line([a,b],fill=255,width=width-2)
                    elif kind==1:
                        dx,dy=b[0]-a[0],b[1]-a[1];lengthpx=math.hypot(dx,dy)
                        if lengthpx:
                            for start in range(0,math.ceil(lengthpx),8):
                                end=min(start+4,lengthpx)
                                draw.line([(a[0]+dx*start/lengthpx,a[1]+dy*start/lengthpx),(a[0]+dx*end/lengthpx,a[1]+dy*end/lengthpx)],fill=85,width=1)
                    elif kind==7:
                        label=text[:min(length,39)]
                        labels.append(LABEL.pack(ay,ax,label.ljust(40,b'\0')))
                if len(labels)>1024:raise ValueError('Too many labels in tile')
                payload=im.point(lambda v:v//85,'P').tobytes('raw','P;2')+b''.join(labels)
# Uniform white cells have an all-zero directory entry.
                if labels or im.getextrema()!=(255,255):
                    struct.pack_into('<III',directory,cell*12,out.tell(),len(labels),zlib.crc32(payload))
                    out.write(payload)
                if cell%250==0:print('tiles',cell,'/',rows*cols,'bytes',out.tell(),flush=True)
            size=out.tell()
            if size>1536*1024*1024:raise ValueError('Map too large for SD adapter')
            header=HEADER.pack(b'X3GM',1,48,size,south,west,CELL,rows,cols,WIDTH,HEIGHT,48,dataoff,zlib.crc32(directory),0)
            header=header[:44]+struct.pack('<I',zlib.crc32(header[:44]))
            out.seek(0);out.write(header);out.write(directory);out.flush();os.fsync(out.fileno())
        db.close();os.replace(Path(tmp)/'map',output)
        print('complete',output,size,flush=True)

if __name__=='__main__':
    p=argparse.ArgumentParser()
    for name in ('source','base','detail','output'):p.add_argument(name)
    build(**vars(p.parse_args()))
