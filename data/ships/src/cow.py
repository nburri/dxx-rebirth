"""The Cow ship: the Cow of Quaternius' Ultimate Animated Animal Pack (CC0 1.0,
https://quaternius.com/packs/ultimateanimatedanimals.html) in its rest pose,
repainted as a matte Holstein (cream hide with baked shading: ambient
occlusion, fur noise, darker belly and legs, as vertex colours) whose spots
are the player-colour zone (material accent_spots), pink snout and udder,
dark hooves.  A collar (colour zone too) and a leather harness carry
weapons at the Pyro's gun points, where the game's shots come from:
laser cannons (guns 0/1), quad-laser stub wings (2/3), missile pods with
noses in the player's colour (4/5), a rotary cannon under the chin (6:
vulcan, gauss, spreadfire, helix, flares) and a milk churn under the udder (7: mines,
smart and mega missiles, earthshakers); gun0..gun7 marker nodes let
shipconv check them.  A small bell sits on the collar.  Head, legs,
udder (with churn), tail and cannon are debris_* nodes.  The additions are CC0 1.0 as well.
Usage: cow.py <pack>/glTF/Cow.gltf out.glb   (python3 with numpy; ~1 min)
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
    'hide_white': (0.80, 0.77, 0.69, 1),  # matte cream; shading in the vertex colours
    'accent_spots': (1, 1, 1, 1),        # player colour zone (spots + collar)
    'pink': (0.95, 0.55, 0.60, 1),       # snout, udder
    'dark': (0.07, 0.06, 0.06, 1),       # hooves, eyes, tail tuft
    'horn': (0.86, 0.80, 0.62, 1),
    'eye_white': (1, 1, 1, 1),
    'brass': (0.78, 0.56, 0.18, 1),
    'gunmetal': (0.20, 0.21, 0.23, 1),
    'steel': (0.62, 0.63, 0.65, 1),
    'leather': (0.28, 0.16, 0.08, 1),
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

def add_quad_strip(ring_a, ring_b, na, nb, mat, part):
    k = len(ring_a)
    for i in range(k):
        j = (i + 1) % k
        tris.append((np.array([ring_a[i], ring_b[i], ring_b[j]]), np.array([na[i], nb[i], nb[j]]), mat, part))
        tris.append((np.array([ring_a[i], ring_b[j], ring_a[j]]), np.array([na[i], nb[j], na[j]]), mat, part))


# ------------------------------------------------------- shapes
def frame(axis):
    axis = axis / np.linalg.norm(axis)
    e1 = np.cross(axis, [0, 1, 0] if abs(axis[1]) < 0.9 else [1, 0, 0]); e1 /= np.linalg.norm(e1)
    return axis, e1, np.cross(axis, e1)


def lathe(profile, origin, axis, mat, part, seg=16):
    """profile: [(r, t)]; t along axis from origin."""
    axis, e1, e2 = frame(np.asarray(axis, float))
    rings, norms = [], []
    for k, (r, t) in enumerate(profile):
        p0 = profile[max(k - 1, 0)]; p1 = profile[min(k + 1, len(profile) - 1)]
        dr, dt = p1[0] - p0[0], p1[1] - p0[1]
        nr, nt = dt, -dr                      # outward normal in (r, t)
        l = math.hypot(nr, nt) + 1e-12
        nr, nt = nr / l, nt / l
        if nr < 0:
            nr, nt = -nr, -nt
        ring, nring = [], []
        for i in range(seg):
            a = 2 * math.pi * i / seg
            d = math.cos(a) * e1 + math.sin(a) * e2
            ring.append(origin + axis * t + d * r)
            n = d * nr + axis * nt
            nring.append(n / (np.linalg.norm(n) + 1e-12))
        rings.append(ring); norms.append(nring)
    for k in range(len(rings) - 1):
        add_quad_strip(rings[k], rings[k + 1], norms[k], norms[k + 1], mat, part)


def tube(p0, p1, r, mat, part, seg=12, caps=True):
    p0 = np.asarray(p0, float); p1 = np.asarray(p1, float)
    L = np.linalg.norm(p1 - p0)
    prof = [(r, 0), (r, L)]
    if caps:
        prof = [(0, 0)] + prof + [(0, L)]
    lathe(prof, p0, p1 - p0, mat, part, seg)


def beam(p0, p1, width, thick, mat, part):
    """A flat box from p0 to p1, wide in the horizontal plane (a stub wing)."""
    p0 = np.asarray(p0, float); p1 = np.asarray(p1, float)
    d = (p1 - p0) / np.linalg.norm(p1 - p0)
    w = np.cross([0, 1, 0], d); w /= np.linalg.norm(w)
    h = np.cross(d, w)
    c = [(-1, -1), (1, -1), (1, 1), (-1, 1)]
    a = [p0 + w * x * width / 2 + h * y * thick / 2 for x, y in c]
    b = [p1 + w * x * width / 2 + h * y * thick / 2 for x, y in c]
    norms = [-h, w, h, -w]
    for i in range(4):
        j = (i + 1) % 4
        n = norms[i]
        tris.append((np.array([a[i], b[i], b[j]]), np.array([n, n, n]), mat, part))
        tris.append((np.array([a[i], b[j], a[j]]), np.array([n, n, n]), mat, part))
    for q, n in ((a, -d), (b, d)):
        tris.append((np.array([q[0], q[1], q[2]]), np.array([n, n, n]), mat, part))
        tris.append((np.array([q[0], q[2], q[3]]), np.array([n, n, n]), mat, part))


def band(cen, axis, width, thick, mat, part, ymin=-1e9, K=28):
    """A strap round the body (or neck) in the plane through cen normal to axis."""
    axis, u, v = frame(np.asarray(axis, float))
    hp = np.array([p for t in tris if t[2] in ('hide_white', 'accent_spots') and t[3] in ('body', 'debris_head') for p in t[0]])
    hp = hp[hp[:, 1] > ymin]
    near = hp[np.abs((hp - cen) @ axis) < 0.25]
    radial = near - cen - np.outer((near - cen) @ axis, axis)
    rn = np.linalg.norm(radial, axis=1)
    dirs, rads = [], []
    for i in range(K):
        a = 2 * math.pi * i / K
        d = math.cos(a) * u + math.sin(a) * v
        sel = (radial @ d) > rn * 0.85
        dirs.append(d); rads.append((np.percentile(rn[sel], 80) if sel.any() else np.median(rn)) + 0.03)
    of = [cen + axis * width + d * r for d, r in zip(dirs, rads)]
    ob = [cen - axis * width + d * r for d, r in zip(dirs, rads)]
    inf = [cen + axis * width + d * (r - thick) for d, r in zip(dirs, rads)]
    inb = [cen - axis * width + d * (r - thick) for d, r in zip(dirs, rads)]
    add_quad_strip(ob, of, dirs, dirs, mat, part)
    add_quad_strip(of, inf, [axis] * K, [axis] * K, mat, part)
    add_quad_strip(inb, ob, [-axis] * K, [-axis] * K, mat, part)
    return of, ob


# ------------------------------------------------------- gun points
# The Pyro's gun points (Player_ship->gun_points, as in shipconv.cpp) in
# game units; every ship fires from them.  They map into this model's
# space through the converter's transform (x mirrored, re-centred, scaled),
# found by converting and measuring until the markers gun0..gun7 match
# (shipconv warns when a marker is more than 1 unit off).
PYRO_GUNS = np.array([[2.23, -0.91, 0.55], [-2.25, -0.91, 0.53], [3.39, -1.81, 2.26], [-3.41, -1.80, 2.26],
                      [2.33, 0.0, -1.39], [-2.39, 0.0, -1.39], [0.02, -1.34, 2.82], [-0.02, -1.34, -2.91]])
CONV_SCALE = float(sys.argv[3]) if len(sys.argv) > 3 else 0.9998
CONV_CENTRE = np.array([float(x) for x in sys.argv[4].split(',')]) if len(sys.argv) > 4 else np.array([0, 2.23785, 1.0539])
GUN = PYRO_GUNS / CONV_SCALE + CONV_CENTRE
GUN[:, 0] = -(PYRO_GUNS[:, 0] / CONV_SCALE)

# ------------------------------------------------------- harness and weapons
# Two leather girths round the barrel of the body carry the guns.
gz_front, gz_rear = 1.05, -0.75
band(np.array([0, 2.6, gz_front]), [0, 0, 1], 0.1, 0.06, 'leather', 'body', ymin=1.65)
band(np.array([0, 2.6, gz_rear]), [0, 0, 1], 0.1, 0.06, 'leather', 'body', ymin=1.65)

for side, (gl_, gq, gm) in ((-1, (0, 2, 4)), (1, (1, 3, 5))):
    L, Q, M = GUN[gl_], GUN[gq], GUN[gm]
    part = 'body'
    # guns 0/1: laser cannon under the flank, muzzle at the gun point
    lathe([(0, -1.25), (0.17, -1.25), (0.17, -0.45), (0.13, -0.40), (0.13, -0.1), (0.17, -0.08), (0.17, 0), (0.09, 0), (0.09, -0.15), (0, -0.15)],
          L, [0, 0, 1], 'gunmetal', part, 14)
    tube(L + [0, 0.02, -0.85], [side * 0.55, L[1] + 0.25, gz_front - 0.2], 0.07, 'gunmetal', part, 8)
    tube(L + [0, 0.02, -0.4], [side * 0.6, L[1] + 0.3, gz_front + 0.05], 0.06, 'gunmetal', part, 8)
    # guns 2/3: quad outrigger, an arm from the cannon to a short barrel
    beam(L + [0, 0, -0.75], Q + [0, 0, -0.45], 0.42, 0.07, 'gunmetal', part)
    lathe([(0, -0.7), (0.12, -0.7), (0.12, -0.12), (0.09, -0.08), (0.09, 0), (0.05, 0), (0.05, -0.1), (0, -0.1)],
          Q, [0, 0, 1], 'gunmetal', part, 12)
    tube(Q + [0, 0, -0.45], Q + [0, 0, -0.3], 0.135, 'brass', part, 12)
    # guns 4/5: missile pod on the flank, open at the front, a missile nose in the player's colour
    lathe([(0, -1.35), (0.2, -1.3), (0.24, -1.15), (0.24, -0.05), (0.26, -0.02), (0.26, 0), (0.2, 0), (0.2, -0.3)],
          M, [0, 0, 1], 'gunmetal', part, 14)
    lathe([(0.17, -0.25), (0.17, -0.12), (0.1, 0.0), (0, 0.06)], M + [0, 0, -0.08], [0, 0, 1], 'accent_spots', part, 12)
    tube(M + [0, 0, -1.0], M + [0, 0, -0.85], 0.27, 'brass', part, 14, caps=False)
    tube(M + [-side * 0.1, 0, -0.95], [side * 0.7, M[1] + 0.05, gz_rear], 0.07, 'gunmetal', part, 8)
    tube(M + [-side * 0.1, 0, -0.25], [side * 0.75, M[1] + 0.05, gz_front - 0.15], 0.07, 'gunmetal', part, 8)

# collar (neck) with a small bell sitting tight on it (decoration), and gun 6
# (vulcan, gauss, spreadfire, helix, flares): a compact rotary cannon under the
# chin, six barrels round a spindle, its muzzle at the gun point.
n2, n3 = JP['Neck2'], JP['Neck3']
outer_f, outer_b = band(n2 + (n3 - n2) * 0.35, n3 - n2, 0.09, 0.12, 'accent_spots', 'body')
low = min(range(len(outer_f)), key=lambda i: outer_f[i][1])
ccen = n2 + (n3 - n2) * 0.35
B = GUN[6]
cax = np.array([0.0, 0.0, 1.0])
clen = 0.95
c0 = B - cax * clen
lathe([(0, 0), (0.15, 0), (0.19, 0.05), (0.19, 0.36), (0.16, 0.40), (0, 0.40)], c0, cax, 'gunmetal', 'debris_gun', 14)
for k in range(6):
    a_ = 2 * math.pi * (k + 0.5) / 6
    off = np.array([math.cos(a_), math.sin(a_), 0]) * 0.085
    tube(c0 + off + cax * 0.38, B + off, 0.03, 'steel', 'debris_gun', 6)
tube(c0 + cax * 0.38, B - cax * 0.02, 0.03, 'gunmetal', 'debris_gun', 6)        # spindle
tube(B - cax * 0.47, B - cax * 0.40, 0.135, 'gunmetal', 'debris_gun', 14, caps=False)
tube(B - cax * 0.07, B, 0.135, 'gunmetal', 'debris_gun', 14, caps=False)
# a yoke from the laser cannons (guns 0/1) carries it
for side in (-1, 1):
    L = GUN[0 if side < 0 else 1]
    beam(L + [0, 0.0, -0.3], c0 + cax * 0.2 + [side * 0.17, 0, 0], 0.2, 0.07, 'gunmetal', 'debris_gun')

# the bell: small, its crown on the collar a little to the side, mouth outwards
bi = (low + 3) % len(outer_f)
bpos = (outer_f[bi] + outer_b[bi]) / 2
bout = bpos - ccen
bout -= (n3 - n2) / np.linalg.norm(n3 - n2) * (bout @ ((n3 - n2) / np.linalg.norm(n3 - n2)))
bout /= np.linalg.norm(bout)
s_ = 0.42
bell = [(0.0, 0.0), (0.17 * s_, 0.02 * s_), (0.22 * s_, 0.10 * s_), (0.24 * s_, 0.24 * s_), (0.27 * s_, 0.42 * s_),
        (0.34 * s_, 0.55 * s_), (0.36 * s_, 0.8 * s_), (0.30 * s_, 0.8 * s_), (0.24 * s_, 0.6 * s_), (0.18 * s_, 0.45 * s_), (0, 0.45 * s_)]
lathe(bell, bpos - bout * 0.02, bout, 'brass', 'body', 14)

# gun 7 (mines, smart and mega missiles, earthshakers): a milk churn
# hanging under the udder, its open mouth at the gun point.
ud = np.array([p for t in tris if t[3] == 'debris_udder' for p in t[0]])
C = GUN[7]
utop = ud[:, 1].min() + 0.12
h = utop - C[1]
churn = [(0, h), (0.09, h), (0.09, h - 0.08), (0.13, h - 0.16), (0.2, h - 0.26), (0.2, 0.06),
         (0.22, 0.04), (0.22, 0), (0.16, 0), (0.16, 0.08), (0, 0.08)]
lathe(churn, np.array([0, C[1], C[2]]), [0, 1, 0], 'steel', 'debris_udder', 14)
tube([0, C[1] + h * 0.55, C[2]], [0, C[1] + h * 0.55 + 0.04, C[2]], 0.215, 'leather', 'debris_udder', 14, caps=False)

# ------------------------------------------------------- matte shading
# Vertex colours (multiplied with the base colours by shipconv; the colour
# zone stays flat grey there): local ambient occlusion by ray casting,
# soft fur noise, a darker belly and legs.
allP = np.array([t[0] for t in tris])                    # (T, 3, 3)
v0, e1_, e2_ = allP[:, 0], allP[:, 1] - allP[:, 0], allP[:, 2] - allP[:, 0]


def occlusion(points, normals, nray=24, reach=0.9):
    rng2 = np.random.default_rng(3)
    dirs = rng2.normal(size=(nray, 3)); dirs /= np.linalg.norm(dirs, axis=1, keepdims=True)
    out = np.zeros(len(points))
    for k in range(0, len(points), 16):
        p = points[k:k + 16]; n = normals[k:k + 16]
        d = dirs[None, :, :] * np.sign(np.einsum('rj,pj->pr', dirs, n))[:, :, None]   # hemisphere
        cosw = np.einsum('prj,pj->pr', d, n)
        o = p + n * 0.02
        D = d.reshape(-1, 3); O = np.repeat(o, nray, axis=0)
        pv = np.cross(D[:, None, :], e2_[None])          # (R, T, 3)
        det = np.einsum('rtj,tj->rt', pv, e1_)
        inv = 1 / np.where(np.abs(det) < 1e-9, 1e-9, det)
        tv = O[:, None, :] - v0[None]
        u = np.einsum('rtj,rtj->rt', tv, pv) * inv
        qv = np.cross(tv, e1_[None])
        v = np.einsum('rj,rtj->rt', D, qv) * inv
        tt = np.einsum('tj,rtj->rt', e2_, qv) * inv
        hit = (np.abs(det) > 1e-9) & (u >= 0) & (v >= 0) & (u + v <= 1) & (tt > 1e-3) & (tt < reach)
        dist = np.where(hit, tt, np.inf).min(1)
        occ = np.where(np.isfinite(dist), 1 - dist / reach, 0).reshape(-1, nray)
        out[k:k + 16] = (occ * cosw).sum(1) / cosw.sum(1)
    return out


def fur(p):
    s = 0
    for f, a, ph in ((3.1, 0.05, (0.3, 1.7, 2.9)), (7.3, 0.03, (1.1, 0.4, 2.2)), (15.0, 0.02, (2.5, 0.9, 0.1))):
        s += a * np.sin(f * p[:, 0] + ph[0]) * np.sin(f * 0.9 * p[:, 1] + ph[1]) * np.sin(f * 1.1 * p[:, 2] + ph[2]) * 2
    return s


shade_mats = {'hide_white', 'pink', 'horn', 'leather', 'gunmetal', 'brass', 'steel', 'dark'}
SP = np.array([p for t in tris for p in t[0]]); SN = np.array([n for t in tris for n in t[1]])
key = np.round(SP, 4)
uk, inv = np.unique(key, axis=0, return_inverse=True)
inv = inv.reshape(-1)
nsum = np.zeros_like(uk); np.add.at(nsum, inv, SN)
nsum /= np.linalg.norm(nsum, axis=1, keepdims=True) + 1e-12
ao = occlusion(uk, nsum)
yb = uk[:, 1]
belly = np.clip((2.4 - yb) / 1.6, 0, 1)                  # 0 above the flank, 1 at the hooves
under = np.clip(-nsum[:, 1], 0, 1)
vshade = (1 - 0.55 * ao) * (1 + fur(uk)) * (1 - 0.18 * belly) * (1 - 0.12 * under)
vshade = np.clip(vshade, 0.35, 1.0)
VC = vshade[inv].reshape(len(tris), 3)
tris = [(P, N, m, part, VC[i] if m in shade_mats else np.ones(3)) for i, (P, N, m, part) in enumerate(tris)]

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
        Cc = np.repeat(np.array([t[4] for t in sel], np.float32).reshape(-1, 1), 3, axis=1)
        key = np.round(np.hstack([P, N, Cc[:, :1]]), 4)
        uniq, inv = np.unique(key, axis=0, return_inverse=True)
        inv = inv.reshape(-1)
        Pu = uniq[:, :3].astype(np.float32); Nu = uniq[:, 3:6].astype(np.float32)
        Nu /= np.linalg.norm(Nu, axis=1, keepdims=True)
        Cu = np.hstack([np.repeat(uniq[:, 6:7], 3, axis=1), np.ones((len(uniq), 1))]).astype(np.float32)
        idx = inv.astype(np.uint16 if len(uniq) < 65535 else np.uint32)
        pa = add(Pu, 34962, 'VEC3', 5126, True)
        na = add(Nu, 34962, 'VEC3', 5126)
        ca = add(Cu, 34962, 'VEC4', 5126)
        ia = add(idx, 34963, 'SCALAR', 5123 if idx.dtype == np.uint16 else 5125)
        prims.append({'attributes': {'POSITION': pa, 'NORMAL': na, 'COLOR_0': ca}, 'indices': ia, 'material': mi})
        ntri += len(sel)
        stats[m] = stats.get(m, 0) + len(sel)
    meshes.append({'name': part, 'primitives': prims})
    nodes.append({'name': part, 'mesh': len(meshes) - 1})
for i, gp in enumerate(GUN):
    nodes.append({'name': 'gun%d' % i, 'translation': [float(x) for x in gp]})

gl = {'asset': {'version': '2.0', 'generator': 'cow.py'},
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
print('ao mean %.2f, shade min %.2f mean %.2f' % (ao.mean(), vshade.min(), vshade.mean()))
