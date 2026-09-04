#!/usr/bin/env python3
"""Build optional X3WM v2 detail map from a local OSM PBF. Pyosmium on host only.
Geometry is streamed into SQLite; the device needs no SQL, polygons or network.
Water hatch uses even/odd scanlines, preserving islands/holes in assembled areas.
"""
import argparse
from pathlib import Path
import os, sqlite3, struct, tempfile, unicodedata, zlib
from build_walkmap import split_edge, HEADER_STRUCT, WALKING_HIGHWAYS, ROAD_HIGHWAYS, _restricted

RECORD=struct.Struct('<iiiiBBBB44s')
CELL=100000
MAIN={'motorway','motorway_link','trunk','trunk_link','primary','primary_link','secondary','secondary_link','tertiary','tertiary_link'}

def text_label(value):
    return ''.join(c for c in unicodedata.normalize('NFKD',value).encode('ascii','ignore').decode().upper() if 32<=ord(c)<=126)[:44]

def hatch(rings, spacing=1200, south=None, north=None):
    if not rings or not rings[0]:return
    lo=min(p[0] for p in rings[0]);hi=max(p[0] for p in rings[0])
    if south is not None:lo=max(lo,south)
    if north is not None:hi=min(hi,north)
    for y in range((lo//spacing+1)*spacing,hi,spacing):
        xs=[]
        for ring in rings:
            for (ay,ax),(by,bx) in zip(ring,ring[1:]):
                if (ay<=y<by) or (by<=y<ay):xs.append(round(ax+(y-ay)*(bx-ax)/(by-ay)))
        xs.sort()
        for i in range(0,len(xs)-1,2):
            if xs[i]<xs[i+1]:yield (y,xs[i]),(y,xs[i+1])

def clip(a,b,south,west,north,east):
    ay,ax=a;by,bx=b;dx=bx-ax;dy=by-ay;t0=0.;t1=1.
    for p,q in [(-dx,ax-west),(dx,east-ax),(-dy,ay-south),(dy,north-ay)]:
        if p==0:
            if q<0:return None
        elif p<0:t0=max(t0,q/p)
        else:t1=min(t1,q/p)
        if t0>t1:return None
    return ((max(south,min(north,round(ay+t0*dy))),max(west,min(east,round(ax+t0*dx)))),
            (max(south,min(north,round(ay+t1*dy))),max(west,min(east,round(ax+t1*dx)))))

def build(source, base_map, output):
    import osmium
    output=Path(output)
    if output.exists():raise ValueError('Output already exists; choose a new path')
    with open(base_map,'rb') as f:h=HEADER_STRUCT.unpack(f.read(48))
    if h[0]!=b'X3WM' or h[1]!=1 or h[6]!=200000:raise ValueError('Expected a v1 base map')
    south,west=h[4],h[5];rows,cols=h[7]*2,h[8]*2
    if rows*cols>65536:raise ValueError('Detail region exceeds grid bound')
    north,east=south+rows*CELL,west+cols*CELL
    output.parent.mkdir(parents=True,exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='x3-detail-',dir=output.parent) as tmp:
        db=sqlite3.connect(str(Path(tmp)/'records.db'))
        db.execute('PRAGMA journal_mode=OFF');db.execute('PRAGMA synchronous=OFF')
        db.execute('CREATE TABLE records (cell INTEGER, layer INTEGER, record BLOB)')
        count=0
        def record(a,b,kind,flags=0,name=''):
            nonlocal count
            text=text_label(name).encode('ascii')
            if kind==7:
                if not text or not(south<a[0]<north and west<a[1]<east):return
                pieces=[(((a[0]-south-1)//CELL,(a[1]-west-1)//CELL),a,a)]
            else:
                clipped=clip(a,b,south,west,north,east)
                if not clipped:return
                pieces=split_edge(*clipped[0],*clipped[1],origin_lat=south,origin_lon=west,cell=CELL,rows=rows,cols=cols)
            for piece in pieces:
                if piece is None:continue
                (r,c),p,q=piece
                layer={5:0,6:1,4:2,2:3,3:4,1:5,7:6}[kind]
                raw=RECORD.pack(*p,*q,kind,flags,len(text),0,text.ljust(44,b'\0'))
                db.execute('INSERT INTO records VALUES(?,?,?)',(r*cols+c,layer,raw));count+=1
                if count%250000==0:db.commit();print('records',count,flush=True)
        def points(nodes):return [(round(n.lat*1e7),round(n.lon*1e7)) for n in nodes]
        class Handler(osmium.SimpleHandler):
            def node(self,n):
                tags=dict(n.tags)
                if tags.get('name') and (tags.get('amenity') in {'place_of_worship','school','townhall','hospital'} or tags.get('tourism') in {'museum','attraction'} or 'historic' in tags):
                    p=(round(n.location.lat*1e7),round(n.location.lon*1e7));record(p,p,7,name=tags['name'])
            def way(self,w):
                tags=dict(w.tags);highway=tags.get('highway')
                if highway not in WALKING_HIGHWAYS|ROAD_HIGHWAYS:return
                try:ps=points(w.nodes)
                except osmium.InvalidLocationError:return
                kind=1 if highway in WALKING_HIGHWAYS else (3 if highway in MAIN else 2)
                for a,b in zip(ps,ps[1:]):record(a,b,kind,int(_restricted(tags)))
                if len(ps)>1 and tags.get('name'):
                    # One anchor for each source way; renderer deduplicates names.
                    a,b=ps[(len(ps)-1)//2:][:2];mid=((a[0]+b[0])//2,(a[1]+b[1])//2)
                    record(mid,mid,7,name=tags['name'])
            def area(self,area):
                tags=dict(area.tags)
                water=tags.get('natural')=='water' or tags.get('waterway')=='riverbank' or tags.get('landuse') in {'reservoir','basin'}
                building='building' in tags and tags['building']!='no'
                if not water and not building:return
                for outer in area.outer_rings():
                    rings=[points(outer)]+[points(r) for r in area.inner_rings(outer)]
                    for ring in rings:
                        for a,b in zip(ring,ring[1:]):record(a,b,6 if water else 4)
                    if water:
                        for a,b in hatch(rings,south=south,north=north):record(a,b,5)
        handler=Handler()
        handler.apply_file(str(source),locations=True,idx='sparse_file_array,'+str(Path(tmp)/'nodes.idx'))
        db.commit();print('sorting',count,flush=True)
        db.execute('CREATE INDEX ordered ON records(cell,layer)');db.commit()
        data_offset=48+rows*cols*12
        directory=bytearray(rows*cols*12);temp=Path(tmp)/'detail.walkmap'
        with temp.open('w+b') as f:
            f.write(bytes(data_offset));offset=data_offset;cell=-1;cellcount=0;crc=0;start=offset
            def finish():
                if cell>=0:struct.pack_into('<III',directory,cell*12,start,cellcount,crc)
            for current,raw in db.execute('SELECT cell,record FROM records ORDER BY cell,layer'):
                if current!=cell:
                    finish();cell=current;start=offset;cellcount=0;crc=0
                cellcount+=1
                if cellcount>65536:raise ValueError('cell too dense')
                crc=zlib.crc32(raw,crc);f.write(raw);offset+=64
                if offset>1536*1024*1024:raise ValueError('Map exceeds 1536MiB')
            finish()
            head=HEADER_STRUCT.pack(b'X3WM',2,48,offset,south,west,CELL,rows,cols,48,data_offset,zlib.crc32(directory),0,0)
            head=head[:44]+struct.pack('<I',zlib.crc32(head[:44]))
            f.seek(0);f.write(head);f.write(directory);f.flush();os.fsync(f.fileno())
        db.close();os.replace(temp,output)
        print('output',output,'bytes',offset,'records',count,flush=True)

if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('source');p.add_argument('base_map');p.add_argument('output');a=p.parse_args()
    build(a.source,a.base_map,a.output)
