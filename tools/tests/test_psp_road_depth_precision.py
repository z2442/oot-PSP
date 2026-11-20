#!/usr/bin/env python3
"""Check real Hyrule Field overlays with GE matrix/viewport precision.

Compile the production projection upload and depth viewport setup, then compare
road/terrain depth at the same screen sample. This models float24 matrix rows,
viewport arithmetic, floored vertex Z, and 1/16-pixel XY. The reciprocal uses a
truncated divide instead of the hardware lookup table; this is a precision
regression, not a bit-exact GE emulator. Hardware still verifies final images.

Arithmetic reference: https://www.ppsspp.org/docs/psp-hardware/gpu/arithmetic/
Fixtures: spot00 room overlay display lists 030139A8 and 030143C8.
"""
from pathlib import Path
import math
import os
import re
import struct
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
frontend = (ROOT / 'src/port/psp/gfx/gfx_fast3d.c').read_text()
backend = (ROOT / 'src/port/psp/gfx/gfx_scegu.c').read_text()


def function(source, name):
    start = re.search(r'^static [^\n]*\b' + name + r'\([^;]*?\)\s*\{', source, re.M).start()
    return source[start:source.index('\n}', source.index('{', start)) + 2] + '\n'


def f24(value):
    return struct.unpack('<f', struct.pack('<I', struct.unpack('<I', struct.pack('<f', value))[0] & 0xFFFFFF00))[0]


def exponent(value):
    return math.frexp(abs(value))[1] - 1 if value else -200


def row_sum(position, coefficients):
    step = 2.0 ** max([exponent(a) + exponent(b) - 15
                      for a, b in zip(position, coefficients) if a and b] or [-200])
    return f24(sum(math.trunc(a * b / step) * step for a, b in zip(position, coefficients)))


def ge_add(a, b):
    step = 2.0 ** (max(exponent(a), exponent(b)) - 15)
    return math.trunc(a / step) * step + math.trunc(b / step) * step


def sub(a, b):
    return tuple(x - y for x, y in zip(a, b))


def dot(a, b):
    return sum(x * y for x, y in zip(a, b))


def cross(a, b):
    return (a[1]*b[2]-a[2]*b[1], a[2]*b[0]-a[0]*b[2], a[0]*b[1]-a[1]*b[0])


def normalized(a):
    return tuple(x / math.sqrt(dot(a, a)) for x in a)


# Actual positions and signed lit normals; independent underlying terrain mesh.
fixtures = [
    ((670.822975, -3.133848, 9345.858996),
     [(476,-139,8225,11,119,3),(0,10,7564,12,118,14),(0,-37,7964,12,118,14)],
     [(0,-100,8500),(760,-162,8061),(0,100,6800)]),
    ((1245.070458,49.105320,9022.121494),
     [(705,-124,8569,5,119,-8),(476,-139,8225,11,119,3),(258,-121,8351,11,119,3)],
     [(760,-162,8061),(0,-100,8500),(875,-119,8739)]),
    ((1602.777116,-26.536758,8627.576157),
     [(1472,-220,7650,8,119,10),(1685,-229,7773,5,119,1),(1807,-202,7457,5,118,19)],
     [(2944,-140,6800),(1472,-220,7650),(2270,-253,8111)]),
]
matrices = []
for eye, road, terrain in fixtures:
    center = tuple(sum(v[i] for v in road) / 3 for i in range(3))
    forward = normalized(sub(center, eye))
    right = normalized(cross(forward, (0,1,0)))
    up = cross(right, forward)
    zscale = -(12800+10)/(12800-10)
    zadd = -2*12800*10/(12800-10)
    columns = [(*[x*1.3 for x in right], -dot(eye,right)*1.3),
               (*[x*1.7 for x in up], -dot(eye,up)*1.7),
               (*[-x*zscale for x in forward], dot(eye,forward)*zscale+zadd),
               (*forward, -dot(eye,forward))]
    matrices.append([columns[col][row] for row in range(4) for col in range(4)])

# Exercise the unchanged full depth range with both gameplay near planes,
# projection scales, and orthographic UI/item projections.
for near in (1.0,10.0):
    for projection_scale in (0.5,1.0,2.0):
        far=12800.0
        m=[0.0]*16
        m[0]=1.3*projection_scale;m[5]=1.7*projection_scale
        m[10]=-(far+near)/(far-near)*projection_scale
        m[11]=-projection_scale;m[14]=-2*far*near/(far-near)*projection_scale
        matrices.append(m)
matrices.append([1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1])

start = backend.index('    sceGuDepthRange(', backend.index('static void gfx_scegu_configure_gu_buffers('))
viewport = backend[start:backend.index('    sceGuScissor(', start)]
code = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef struct {float m[4][4];} ScePspFMatrix4;
#define GU_PROJECTION 0
static float sUploadedProjection[4][4],sClipProjection[4][4],encoded[16];
static bool sUploadedProjectionValid;
static unsigned sAppliedDepthBias=~0U;
static float zScale,zCenter;
static void sceGuSetMatrix(unsigned mode,const ScePspFMatrix4* matrix) {
    assert(mode==GU_PROJECTION);
    for (unsigned i=0;i<16;i++) {
        uint32_t bits;memcpy(&bits,((const float*)matrix)+i,4);bits&=0xFFFFFF00U;
        memcpy(&encoded[i],&bits,4);
    }
}
static void sceGuDepthRange(int near,int far) {
    assert(near==65535 && far==0);zScale=-32768;zCenter=32767;
}
static void sceGuSendCommandf(unsigned command,float value) {
    if (command==0x44) zScale=value;
    else {assert(command==0x47);zCenter=value;}
}
''' + function(frontend, 'gfx_upload_projection_matrix') + function(backend, 'gfx_scegu_set_depth_bias')
code += 'static void setup(void) {\n' + viewport + '}\n'
code += 'static const float matrices[][4][4]={\n'
code += ',\n'.join('{' + ','.join('{' + ','.join(format(x,'.12g') + 'f' if '.' in format(x,'.12g') or 'e' in format(x,'.12g') else format(x,'.12g')+'.0f' for x in m[row*4:row*4+4]) + '}' for row in range(4)) + '}' for m in matrices)
code += r'''
};
int main(void) {
    setup();gfx_scegu_set_depth_bias(2);
    printf("%a %a\n",(double)zScale,(double)zCenter);
    for (unsigned i=0;i<sizeof(matrices)/sizeof(matrices[0]);i++) {
        gfx_upload_projection_matrix(matrices[i]);
        for (unsigned j=0;j<16;j++) printf("%a ",(double)encoded[j]);
        puts("");
    }
}
'''
with tempfile.TemporaryDirectory(prefix='oot-road-precision-') as directory:
    path = Path(directory)
    (path/'test.c').write_text(code)
    subprocess.run([os.environ.get('CC','cc'),'-std=c11','-O2','-Wall','-Wextra','-Werror',
                    str(path/'test.c'),'-o',str(path/'test')], check=True)
    output = subprocess.check_output([str(path/'test')], text=True).splitlines()
scale, bias = map(float.fromhex, output[0].split())
encoded = [[float.fromhex(x) for x in line.split()] for line in output[1:]]


def project(v, matrix, scale, offset):
    position = [*[f24(x) for x in v[:3]],1.0]
    x,y,z,w = [row_sum(position, matrix[col::4]) for col in range(4)]
    assert w > 10
    reciprocal = f24(1/w)
    x = ge_add(f24(f24(x*reciprocal)*240),2048)
    y = ge_add(f24(f24(y*reciprocal)*-136),2048)
    z = ge_add(f24(f24(z*reciprocal)*scale),offset)
    return math.floor(x*16)/16, math.floor(y*16)/16, math.floor(z)


def depth(triangle, x, y):
    (x0,y0,z0),(x1,y1,z1),(x2,y2,z2) = triangle
    den = (y1-y2)*(x0-x2)+(x2-x1)*(y0-y2)
    a = ((y1-y2)*(x-x2)+(x2-x1)*(y-y2))/den
    b = ((y2-y0)*(x-x2)+(x0-x2)*(y-y2))/den
    c = 1-a-b
    assert min(a,b,c) >= 0
    return math.floor(a*z0+b*z1+c*z2)


old_failures = 0
for index, (_, road, terrain) in enumerate(fixtures):
    # Keep the existing bounded one-unit lift and two-unit rounding offset.
    lifted = [tuple(v[i]+normalized(v[3:])[i] for i in range(3)) for v in road]
    for old in (False, True):
        matrix = [f24(x) for x in matrices[index]] if old else encoded[index]
        s,o = (-32768,32767+2) if old else (scale,bias)
        r = [project(v,matrix,s,o) for v in lifted]
        g = [project(v,matrix,s,o-2) for v in terrain]
        x,y = [sum(v[i] for v in r)/3 for i in range(2)]
        road_depth, grass_depth = depth(r,x,y), depth(g,x,y)
        if old:
            old_failures += road_depth < grass_depth
        else:
            assert road_depth >= grass_depth, (index,road_depth,grass_depth)
assert old_failures > 0
print(f'Road depth precision: {old_failures} old grass-over-road failures reproduced; '
      'all real-mesh fixtures pass with the existing lift, bias and occlusion test')

# Near/far ordering and the mathematical full-range mapping are preserved.
# Compare against N64 depth, independently of the GE expression under test.
assert scale==65535 and bias==2
for index,matrix in enumerate(matrices[len(fixtures):-1],len(fixtures)):
    near=1.0 if index-len(fixtures)<3 else 10.0
    previous=65536
    for distance in (near,near*2,100,1000,12800):
        ge=encoded[index]
        clip_z=-distance*ge[10]+ge[14]
        clip_w=-distance*ge[11]+ge[15]
        actual=65535*clip_z/clip_w
        expected=65535*((12800*near/distance)-near)/(12800-near)
        assert abs(actual-expected)<2,(near,distance,actual,expected)
        assert actual<=previous
        previous=actual
for z in (-1.0,-0.5,0.0,0.5,1.0):
    ge=encoded[-1]
    actual=(z*ge[10]+ge[14])*65535
    assert actual==(1-z)*32767.5
print('Depth convention: near/far ordering, both gameplay near planes, projection scales and orthographic mapping passed')
