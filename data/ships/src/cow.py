"""The Cow ship: the Cow of Quaternius' Ultimate Animated Animal Pack (CC0 1.0,
https://quaternius.com/packs/ultimateanimatedanimals.html) in its rest pose,
repainted as a Holstein whose spots are the player-colour zone (material
accent_spots), pink snout and udder, dark hooves, plus a collar (also the
colour zone) and a brass cow bell.  Head, legs, udder, tail and bell are
debris_* nodes.  The additions are CC0 1.0 as well.
Usage: cow.py <pack>/glTF/Cow.gltf out.glb   (python3 with numpy)
"""
import json, struct, sys, math
import numpy as np
import base64
CT={5126:np.float32,5123:np.uint16,5125:np.uint32,5121:np.uint8}
NC={'SCALAR':1,'VEC2':2,'VEC3':3,'VEC4':4,'MAT4':16}
def load(path):
    g=json.load(open(path))
    bufs=[base64.b64decode(b['uri'].split(',',1)[1]) for b in g['buffers']]
    def acc(i):
        a=g['accessors'][i]; bv=g['bufferViews'][a['bufferView']]
        dt=np.dtype(CT[a['componentType']]); n=NC[a['type']]
        off=bv.get('byteOffset',0)+a.get('byteOffset',0)
        stride=bv.get('byteStride',0) or dt.itemsize*n
        raw=np.frombuffer(bufs[bv['buffer']],np.uint8,count=stride*(a['count']-1)+dt.itemsize*n,offset=off)
        out=np.lib.stride_tricks.as_strided(raw,(a['count'],dt.itemsize*n),(stride,1)).copy().view(dt).reshape(a['count'],n)
        return out
    return g,acc



SRC, OUT = sys.argv[1], sys.argv[2]
g, acc = load(SRC)
sk = g['skins'][0]
JN = [g['nodes'][j]['name'] for j in sk['joints']]
IB = acc(sk['inverseBindMatrices']).reshape(-1, 4, 4)
JP = {n: np.linalg.inv(M.T)[:3, 3] for n, M in zip(JN, IB)}

# output materials: name -> RGBA (linear factors, glTF)
MATS = {
    'hide_white': (0.93, 0.92, 0.88, 1),
    'accent_spots': (1, 1, 1, 1),        # player colour zone (spots + collar)
    'pink': (0.95, 0.55, 0.60, 1),       # snout, udder
    'dark': (0.07, 0.06, 0.06, 1),       # hooves, eyes, tail tuft
    'horn': (0.86, 0.80, 0.62, 1),
    'eye_white': (1, 1, 1, 1),
    'brass': (0.85, 0.62, 0.18, 1),
}

# ---------------------------------------------------------------- source tris
tris = []   # (P[3,3], N[3,3], material, part)
LEGS = {
    'debris_leg_front_left': ('FrontLowerLeg.L', 'IKFrontLeg.L', 'FF.L'),
    'debris_leg_front_right': ('FrontLowerLeg.R', 'IKFrontLeg.R', 'FF.R'),
    'debris_leg_back_left': ('BackLowerLeg.L', 'IKBackLeg.L', 'FFB.L'),
    'debris_leg_back_right': ('BackLowerLeg.R', 'IKBackLeg.R', 'FFB.R'),
}
JPART = {j: p for p, js in LEGS.items() for j in js}
for j in ('Head', 'Neck3'):
    JPART[j] = 'debris_head'
for j in ('Tail3', 'Tail4', 'Tail5', 'Tail6', 'Tail7'):
    JPART[j] = 'debris_tail'

for prim in g['meshes'][0]['primitives']:
    mname = g['materials'][prim['material']]['name']
    a = prim['attributes']
    P = acc(a['POSITION']).astype(np.float64)
    N = acc(a['NORMAL']).astype(np.float64)
    J = acc(a['JOINTS_0']); W = acc(a['WEIGHTS_0'])
    dom = J[np.arange(len(J)), W.argmax(1)]
    I = acc(prim['indices']).reshape(-1, 3)
    for t in I:
        js = [JN[dom[k]] for k in t]
        parts = [JPART.get(j, 'body') for j in js]
        part = max(set(parts), key=parts.count)
        c = P[t].mean(0)
        if mname in ('Main', 'Main_Light'):
            mat = 'hide_white'
            if part == 'debris_tail' and c[1] < 2.75:
                mat = 'dark'                       # tail tuft
        elif mname == 'Muzzle':
            mat = 'pink'
            if c[1] < 2.6:
                part = 'debris_udder'
        elif mname in ('Hooves', 'Eye_Black'):
            mat = 'dark'
        elif mname == 'Horns':
            mat = 'horn'
        elif mname == 'Eye_White':
            mat = 'eye_white'
        tris.append((P[t], N[t], mat, part))

# ------------------------------------------------------------------- spots
rng = np.random.default_rng(7)


def subdivide(P, N):
    m = [(P[i] + P[(i + 1) % 3]) / 2 for i in range(3)]
    mn = [N[i] + N[(i + 1) % 3] for i in range(3)]
    mn = [v / np.linalg.norm(v) for v in mn]
    return [(np.array([P[0], m[0], m[2]]), np.array([N[0], mn[0], mn[2]])),
            (np.array([m[0], P[1], m[1]]), np.array([mn[0], N[1], mn[1]])),
            (np.array([m[2], m[1], P[2]]), np.array([mn[2], mn[1], N[2]])),
            (np.array(m), np.array(mn))]


# spot centres: random points on the white hide of the body, head and upper legs
white = [t for t in tris if t[2] == 'hide_white' and t[3] in ('body', 'debris_head')]
cand = np.array([t[0].mean(0) for t in white])
centres, radii = [], []
# a patch round the left eye (Holstein face), then irregular patches, each a
# cluster of overlapping blobs, kept off the collar
centres.append(np.array([0.6, 3.95, 4.15])); radii.append(0.38)
collar_c = JP['Neck2'] + (JP['Neck3'] - JP['Neck2']) * 0.35
seeds = []
for k in rng.permutation(len(cand)):
    p = cand[k]
    if p[1] < 1.9 or np.linalg.norm(p - collar_c) < 1.3 or p[2] > 3.4:
        continue
    if all(np.linalg.norm(p - c) > 1.55 for c in seeds):
        seeds.append(p)
    if len(seeds) >= 13:
        break
for p in seeds:
    r0 = rng.uniform(0.55, 0.85)
    centres.append(p); radii.append(r0)
    for _ in range(2):
        d = rng.normal(size=3); d /= np.linalg.norm(d)
        centres.append(p + d * r0 * rng.uniform(0.6, 1.0)); radii.append(r0 * rng.uniform(0.5, 0.8))
centres = np.array(centres); radii = np.array(radii)
wob = rng.normal(size=(len(centres), 3, 3))


def field(p):
    """>0 inside a spot: max over wobbly blobs."""
    d = np.linalg.norm(p[None, :] - centres, axis=1)
    v = p[None, :] - centres
    s = 0
    for i in range(3):
        s = s + 0.12 * np.sin(3.1 * np.einsum('kj,kj->k', v, wob[:, i]) + 1.3 * i)
    return np.max(radii * (1 + s) - d)


def clip(P, N, f):
    """Split a triangle at the zero contour of f; returns [(P, N, inside)]."""
    inside = f > 0
    if inside.all() or (~inside).all():
        return [(P, N, bool(inside[0]))]
    # rotate so vertex 0 is the odd one out
    odd = 0 if inside[0] != inside[1] and inside[0] != inside[2] else (1 if inside[1] != inside[0] and inside[1] != inside[2] else 2)
    o = [odd, (odd + 1) % 3, (odd + 2) % 3]
    P, N, f = P[o], N[o], f[o]

    def cut(i, j):
        t = f[i] / (f[i] - f[j])
        n = N[i] + t * (N[j] - N[i])
        return P[i] + t * (P[j] - P[i]), n / np.linalg.norm(n)
    a, na = cut(0, 1)
    b, nb = cut(0, 2)
    io = bool(f[0] > 0)
    return [(np.array([P[0], a, b]), np.array([N[0], na, nb]), io),
            (np.array([a, P[1], P[2]]), np.array([na, N[1], N[2]]), not io),
            (np.array([a, P[2], b]), np.array([na, N[2], nb]), not io)]


out = []
for P, N, mat, part in tris:
    if mat != 'hide_white' or part in ('debris_tail',):
        out.append((P, N, mat, part)); continue
    stack = [(P, N, 0)]
    while stack:
        sp, sn, lvl = stack.pop()
        f = np.array([field(p) for p in sp])
        edge = max(np.linalg.norm(sp[i] - sp[(i + 1) % 3]) for i in range(3))
        straddle = (f > 0).any() and (f <= 0).any()
        if straddle and lvl < 1 and edge > 0.18:
            stack.extend((a, b, lvl + 1) for a, b in subdivide(sp, sn))
            continue
        for cp, cn, ins in clip(sp, sn, f):
            out.append((cp, cn, 'accent_spots' if ins else 'hide_white', part))
tris = out

# ------------------------------------------------------- collar and cow bell
n1, n2 = JP['Neck2'], JP['Neck3']
axis = (n2 - n1) / np.linalg.norm(n2 - n1)
cen = n1 + (n2 - n1) * 0.35
# neck radius: hide vertices near the collar plane
hp = np.array([p for t in tris if t[3] in ('body', 'debris_head') and t[2] in ('hide_white', 'accent_spots') for p in t[0]])
dist_plane = (hp - cen) @ axis
near = hp[np.abs(dist_plane) < 0.25]
radial = near - cen - np.outer((near - cen) @ axis, axis)
rn = np.linalg.norm(radial, axis=1)
u = np.array([1.0, 0, 0]); v = np.cross(axis, u); v /= np.linalg.norm(v)   # v points ~down/forward


def neck_r(ang):
    d = math.cos(ang) * u + math.sin(ang) * v
    sel = (radial @ d) > rn * 0.85
    return np.percentile(rn[sel], 80) if sel.any() else np.median(rn)


def add_quad_strip(ring_a, ring_b, na, nb, mat, part):
    k = len(ring_a)
    for i in range(k):
        j = (i + 1) % k
        tris.append((np.array([ring_a[i], ring_b[i], ring_b[j]]), np.array([na[i], nb[i], nb[j]]), mat, part))
        tris.append((np.array([ring_a[i], ring_b[j], ring_a[j]]), np.array([na[i], nb[j], na[j]]), mat, part))


K = 28
angs = [2 * math.pi * i / K for i in range(K)]
rads = [neck_r(a) + 0.03 for a in angs]
dirs = [math.cos(a) * u + math.sin(a) * v for a in angs]
w = 0.09
outer_f = [cen + axis * w + d * r for d, r in zip(dirs, rads)]
outer_b = [cen - axis * w + d * r for d, r in zip(dirs, rads)]
add_quad_strip(outer_b, outer_f, dirs, dirs, 'accent_spots', 'body')
inner_f = [cen + axis * w + d * (r - 0.12) for d, r in zip(dirs, rads)]
inner_b = [cen - axis * w + d * (r - 0.12) for d, r in zip(dirs, rads)]
add_quad_strip(outer_f, inner_f, [axis] * K, [axis] * K, 'accent_spots', 'body')
add_quad_strip(inner_b, outer_b, [-axis] * K, [-axis] * K, 'accent_spots', 'body')
# lowest point of the collar
low = min(range(K), key=lambda i: outer_f[i][1])
top = (outer_f[low] + outer_b[low]) / 2 + np.array([0, -0.02, 0])


def lathe(profile, centre, mat, part, seg=20):
    """profile: [(r, y)] from top to bottom, rotated round the vertical axis."""
    rings, norms = [], []
    for k, (r, y) in enumerate(profile):
        y0 = profile[max(k - 1, 0)]; y1 = profile[min(k + 1, len(profile) - 1)]
        dr, dy = y1[0] - y0[0], y1[1] - y0[1]
        nrm2 = np.array([-dy, dr]); nrm2 /= np.linalg.norm(nrm2) + 1e-12
        if nrm2[0] < 0:
            nrm2 = -nrm2
        ring, nring = [], []
        for i in range(seg):
            a = 2 * math.pi * i / seg
            c, s = math.cos(a), math.sin(a)
            ring.append(centre + np.array([r * c, y, r * s]))
            n = np.array([nrm2[0] * c, nrm2[1], nrm2[0] * s])
            nring.append(n / (np.linalg.norm(n) + 1e-12))
        rings.append(ring); norms.append(nring)
    for k in range(len(rings) - 1):
        add_quad_strip(rings[k], rings[k + 1], norms[k], norms[k + 1], mat, part)


# bell: hanger loop, flared body, clapper ball
lathe([(0.0, 0.0), (0.07, -0.0), (0.07, -0.08), (0.0, -0.08)], top, 'brass', 'debris_bell', 10)
bell = [(0.0, -0.08), (0.17, -0.10), (0.22, -0.18), (0.24, -0.32), (0.27, -0.46),
        (0.34, -0.56), (0.36, -0.60), (0.30, -0.60), (0.24, -0.56), (0.2, -0.5)]
lathe(bell, top, 'brass', 'debris_bell', 20)
clap = [(0.0, -0.52), (0.06, -0.55), (0.08, -0.6), (0.06, -0.65), (0.0, -0.68)]
lathe(clap, top, 'dark', 'debris_bell', 10)

# ------------------------------------------------------------------ write glb
parts = sorted(set(t[3] for t in tris), key=lambda p: (p != 'body', p))
mats = list(MATS)
blob = bytearray(); views = []; accs = []; meshes = []; nodes = []


def add(arr, target, typ, ctype, minmax=False):
    while len(blob) % 4:
        blob.append(0)
    off = len(blob); blob.extend(arr.tobytes())
    views.append({'buffer': 0, 'byteOffset': off, 'byteLength': arr.nbytes, 'target': target})
    a = {'bufferView': len(views) - 1, 'componentType': ctype, 'count': len(arr), 'type': typ}
    if minmax:
        a['min'] = arr.min(0).tolist(); a['max'] = arr.max(0).tolist()
    accs.append(a)
    return len(accs) - 1


ntri = 0
stats = {}
for part in parts:
    prims = []
    for mi, m in enumerate(mats):
        sel = [t for t in tris if t[3] == part and t[2] == m]
        if not sel:
            continue
        P = np.array([t[0] for t in sel], np.float32).reshape(-1, 3)
        N = np.array([t[1] for t in sel], np.float32).reshape(-1, 3)
        # weld identical vertices
        key = np.round(np.hstack([P, N]), 5)
        uniq, inv = np.unique(key, axis=0, return_inverse=True)
        Pu = uniq[:, :3].astype(np.float32); Nu = uniq[:, 3:].astype(np.float32)
        Nu /= np.linalg.norm(Nu, axis=1, keepdims=True)
        idx = inv.reshape(-1).astype(np.uint16 if len(uniq) < 65535 else np.uint32)
        pa = add(Pu, 34962, 'VEC3', 5126, True)
        na = add(Nu, 34962, 'VEC3', 5126)
        ia = add(idx, 34963, 'SCALAR', 5123 if idx.dtype == np.uint16 else 5125)
        prims.append({'attributes': {'POSITION': pa, 'NORMAL': na}, 'indices': ia, 'material': mi})
        ntri += len(sel)
        stats[m] = stats.get(m, 0) + len(sel)
    meshes.append({'name': part, 'primitives': prims})
    nodes.append({'name': part, 'mesh': len(meshes) - 1})

gl = {'asset': {'version': '2.0', 'generator': 'build_cow.py'},
      'scene': 0, 'scenes': [{'nodes': list(range(len(nodes)))}], 'nodes': nodes, 'meshes': meshes,
      'materials': [{'name': m, 'pbrMetallicRoughness': {'baseColorFactor': list(MATS[m]), 'metallicFactor': 0, 'roughnessFactor': 1}} for m in mats],
      'accessors': accs, 'bufferViews': views, 'buffers': [{'byteLength': len(blob)}]}
js = json.dumps(gl).encode()
js += b' ' * (-len(js) % 4)
while len(blob) % 4:
    blob.append(0)
with open(OUT, 'wb') as f:
    f.write(struct.pack('<III', 0x46546C67, 2, 12 + 8 + len(js) + 8 + len(blob)))
    f.write(struct.pack('<II', len(js), 0x4E4F534A)); f.write(js)
    f.write(struct.pack('<II', len(blob), 0x004E4942)); f.write(blob)


def area(sel):
    return sum(0.5 * np.linalg.norm(np.cross(t[0][1] - t[0][0], t[0][2] - t[0][0])) for t in sel)


tot = area(tris)
print('triangles', ntri, 'parts', parts)
print('per material', stats)
print('accent area %.1f %%' % (100 * area([t for t in tris if t[2] == 'accent_spots']) / tot))
