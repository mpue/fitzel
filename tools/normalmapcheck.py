"""normalmapcheck -- which way does a normal map lean in lit.frag?

A tangent-space normal map's green points UP THE PICTURE. lit.frag builds its
basis from how v runs across the surface, and whether v runs up or down the
picture depends on how the texture was stored: the library loads bottom row
first (flipVertically), imported models top row first (glTF's way). Get the sign
wrong and every ridge is lit from its shadowed side -- a picture that looks
"somehow off" and that nobody can name. Until 2026-09-29 it was wrong for every
imported model.

This builds a scene of six floor plates under viewcheck's sun (from +X, +Z):
three glTF quads and three library-material planes, each with a normal map
that leans every texel to the picture's top, one that is flat, and one that
leans to the bottom. The picture's top is put on the sunny side on all of them,
so on BOTH paths the "top" plate must come out brighter than the "bottom" one.
It renders through viewcheck (scenesubmit + the real lit.frag), measures, and
exits non-zero if either path leans the wrong way.

    python tools/normalmapcheck.py [--keep DIR]

Needs Pillow and numpy, and build/release/bin/viewcheck.exe.
"""
import io
import json
import os
import shutil
import struct
import subprocess
import sys
import tempfile

import numpy as np
from PIL import Image

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BIN = os.path.join(ROOT, 'build', 'release', 'bin')
SHADERS = os.path.join(ROOT, 'sandbox', 'assets', 'shaders')


def png(img):
    b = io.BytesIO()
    img.save(b, 'PNG')
    return b.getvalue()


def build_scene(d):
    for sub in ('materials', 'textures', 'models'):
        os.makedirs(os.path.join(d, sub), exist_ok=True)
    marker = Image.new('RGB', (64, 64))
    for y in range(64):
        for x in range(64):   # top half reddish: shows where the picture's top lands
            marker.putpixel((x, y), (200, 120, 110) if y < 32 else (110, 130, 200))
    imgs = {'marker': marker,
            'up': Image.new('RGB', (16, 16), (128, 190, 230)),    # leans to the picture's top
            'flat': Image.new('RGB', (16, 16), (128, 128, 255)),
            'down': Image.new('RGB', (16, 16), (128, 66, 230))}   # ...to its bottom
    tex = {k: 'a1' * 15 + '%02d' % i for i, k in enumerate(imgs)}
    for k, im in imgs.items():
        im.save(os.path.join(d, 'textures', k + '.png'))
        json.dump({'guid': tex[k], 'importer': {'flipVertically': True, 'sRGB': k == 'marker'},
                   'type': 'Texture'}, open(os.path.join(d, 'textures', k + '.png.meta'), 'w'))

    def glb(path, nrm):
        # 2x2 m in the XZ plane facing +Y; glTF uv with v = 0 (the picture's top)
        # on the +X edge, where the sun comes from.
        P = [(-1, 0, -1), (-1, 0, 1), (1, 0, 1), (1, 0, -1)]
        UV = [((z + 1) / 2, (1 - x) / 2) for (x, _, z) in P]
        binb = bytearray()
        views = []

        def add(data, target=None):
            while len(binb) % 4:
                binb.append(0)
            v = {'buffer': 0, 'byteOffset': len(binb), 'byteLength': len(data)}
            if target:
                v['target'] = target
            binb.extend(data)
            views.append(v)

        add(b''.join(struct.pack('<3f', *p) for p in P), 34962)
        add(b''.join(struct.pack('<3f', 0, 1, 0) for _ in P), 34962)
        add(b''.join(struct.pack('<2f', *u) for u in UV), 34962)
        add(b''.join(struct.pack('<I', i) for i in (0, 1, 2, 0, 2, 3)), 34963)
        add(png(imgs['marker']))
        add(png(imgs[nrm]))
        while len(binb) % 4:
            binb.append(0)
        j = {'asset': {'version': '2.0'}, 'scene': 0, 'scenes': [{'nodes': [0]}], 'nodes': [{'mesh': 0}],
             'meshes': [{'primitives': [{'attributes': {'POSITION': 0, 'NORMAL': 1, 'TEXCOORD_0': 2},
                                         'indices': 3, 'material': 0}]}],
             # A name of its own: the model library shares materials by name.
             'materials': [{'name': 'Quad_' + nrm, 'normalTexture': {'index': 1},
                            'pbrMetallicRoughness': {'baseColorTexture': {'index': 0},
                                                     'metallicFactor': 0, 'roughnessFactor': 0.9}}],
             'textures': [{'source': 0}, {'source': 1}],
             'images': [{'bufferView': 4, 'mimeType': 'image/png'}, {'bufferView': 5, 'mimeType': 'image/png'}],
             'accessors': [{'bufferView': 0, 'componentType': 5126, 'count': 4, 'type': 'VEC3',
                            'min': [-1, 0, -1], 'max': [1, 0, 1]},
                           {'bufferView': 1, 'componentType': 5126, 'count': 4, 'type': 'VEC3'},
                           {'bufferView': 2, 'componentType': 5126, 'count': 4, 'type': 'VEC2'},
                           {'bufferView': 3, 'componentType': 5125, 'count': 6, 'type': 'SCALAR'}],
             'bufferViews': views, 'buffers': [{'byteLength': len(binb)}]}
        t = json.dumps(j).encode()
        while len(t) % 4:
            t += b' '
        with open(path, 'wb') as f:
            f.write(struct.pack('<III', 0x46546C67, 2, 12 + 8 + len(t) + 8 + len(binb)))
            f.write(struct.pack('<II', len(t), 0x4E4F534A) + t)
            f.write(struct.pack('<II', len(binb), 0x004E4942) + bytes(binb))

    ents = [{'active': True, 'center': [0, 0, 0], 'components': [{'color': [1, 0.97, 0.9], 'intensity': 1.0,
                                                                   'type': 'sun'}],
             'half': [1, 1, 1], 'id': 0, 'name': 'Sun', 'parent': -1, 'rotation': [0, 0, 0], 'type': 5}]
    for i, (k, z) in enumerate((('up', 2.6), ('flat', 0.0), ('down', -2.6))):
        mg, fg = 'b2' * 15 + '%02d' % i, 'c3' * 15 + '%02d' % i
        glb(os.path.join(d, 'models', 'quad_%s.glb' % k), k)
        json.dump({'guid': mg, 'type': 'Model'}, open(os.path.join(d, 'models', 'quad_%s.glb.meta' % k), 'w'))
        json.dump({'albedo': [1, 1, 1], 'alphaCutoff': 0.5, 'alphaMode': 0, 'emission': [0, 0, 0],
                   'emissionStrength': 1.0, 'glass': False, 'name': 'lib_' + k, 'opacity': 1.0,
                   'reflectivity': 0.0, 'roughness': 0.9, 'texture': tex['marker'], 'normalMap': tex[k]},
                  open(os.path.join(d, 'materials', 'lib_%s.fmat' % k), 'w'))
        json.dump({'guid': fg, 'type': 'Material'}, open(os.path.join(d, 'materials', 'lib_%s.fmat.meta' % k), 'w'))
        ents.append({'active': True, 'center': [-2.5, 0, z], 'half': [1, 0.01, 1], 'id': 10 + i,
                     'components': [{'model': mg, 'modelFile': 'quad_%s.glb' % k, 'scale': 1.0, 'type': 'model'}],
                     'name': 'Gltf_' + k, 'parent': -1, 'rotation': [0, 0, 0], 'type': 6})
        # A library plane puts the picture's top on +Z, the sun's other side.
        ents.append({'active': True, 'center': [2.5, 0, z], 'half': [1, 1, 1], 'id': 20 + i,
                     'components': [{'material': fg, 'type': 'material'}],
                     'name': 'Lib_' + k, 'parent': -1, 'rotation': [0, 0, 0], 'type': 8})
    json.dump({'entities': ents, 'settings': {}, 'version': 3},
              open(os.path.join(d, 'normalmapcheck.fitzel'), 'w', encoding='utf-8'))


def measure(picture):
    # Looking down +Z (yaw 90): screen left = world +X = the library planes,
    # screen top = +Z = the "up" row.
    a = np.asarray(Image.open(picture).convert('RGB')).astype(float)
    fg = np.abs(a - a[5, 5]).sum(axis=2) > 12
    h, w = fg.shape
    ys = np.unique(np.nonzero(fg)[0])
    gaps = np.argsort(np.diff(ys))[-2:]
    cuts = sorted(int(ys[g]) + 1 for g in gaps)
    bands = [(int(ys[0]), cuts[0]), (cuts[0], cuts[1]), (cuts[1], int(ys[-1]) + 1)]
    res = {}
    for (y0, y1), row in zip(bands, ('up', 'flat', 'down')):
        for (x0, x1), col in (((0, w // 2), 'library'), ((w // 2, w), 'model')):
            m = np.zeros_like(fg)
            m[y0:y1, x0:x1] = True
            py, px = np.nonzero(fg & m)
            r = a[py.min() + 6:py.max() - 6, px.min() + 6:px.max() - 6]
            res[(col, row)] = (r[..., 0] * 0.2126 + r[..., 1] * 0.7152 + r[..., 2] * 0.0722).mean()
    return res


def main():
    keep = sys.argv[sys.argv.index('--keep') + 1] if '--keep' in sys.argv else None
    d = keep or tempfile.mkdtemp(prefix='normalmapcheck_')
    try:
        build_scene(d)
        out = os.path.join(d, 'picture.png')
        subprocess.run([os.path.join(BIN, 'viewcheck.exe'), d, out, '--size', '900x700',
                        '--pitch', '-80', '--yaw', '90', '--shaders', SHADERS],
                       cwd=BIN, check=True, capture_output=True)
        res = measure(out)
        bad = 0
        for col in ('model', 'library'):
            u, f, dn = res[(col, 'up')], res[(col, 'flat')], res[(col, 'down')]
            ok = u > dn + 2.0
            bad += not ok
            print('  [%s] %-7s maps lean to the picture\'s top: up %.1f  flat %.1f  down %.1f'
                  % (' OK ' if ok else 'FAIL', col, u, f, dn))
        print('all checks passed' if not bad else '%d check(s) FAILED' % bad)
        return 1 if bad else 0
    finally:
        if not keep:
            shutil.rmtree(d, ignore_errors=True)


if __name__ == '__main__':
    sys.exit(main())