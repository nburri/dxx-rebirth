#!/usr/bin/env python3
"""Textures for the fork's own procedural ships (CC0 1.0).

    python3 texture_ships.py <in.glb> <out.glb> <style> [--atlas atlas.png]

Reads an untextured ship from shipgen (one material per surface kind:
body, armour, accent, cockpit, engine, panel, ...), unwraps it into one
512 x 512 atlas and paints that atlas:

- UVs: the triangles of each part and material are grouped by the axis
  their normal faces most (box projection); every connected group is one
  chart, projected along its mean normal when it is flat, else along the
  axis.  Charts are packed on shelves at one texel density, with gutters.
- Paint, per texel from its point on the hull: a tileable plating texture
  (tiles/*.png, made with Scenario from text prompts) recoloured to the
  style's colour, dark creases and
  worn paint along the chart edges (the geometry's edges), soot towards
  the engines, grime, hazard stripes at the rear of side armour, vents,
  a hull number, glassy canopies and glowing nozzles.
- The player-colour zone (material "accent") gets a near-white painted
  plate with seams and light wear; the converter greys it and the game
  multiplies it with the player's colour.

Needs numpy, scipy, Pillow and pygltflib.  Deterministic (fixed seeds).
"""
import argparse, io, json, math, os, sys
import numpy as np
from PIL import Image
from scipy import ndimage
import pygltflib as G

HERE = os.path.dirname(os.path.abspath(__file__))
SIZE = 512
PAD = 4            # texels of gutter around every chart (8 between charts)

# ---------------------------------------------------------------------------------------------
# styles: per material (tile, tile size in units, colour, contrast) and decoration

STYLES = {
    "anvil": dict(seed=7, number="07", panel=(1.25, 0.85), mats={
        "body": ("gunmetal", 2.4, (0.43, 0.45, 0.48), 0.9),
        "armour": ("darkarmour", 2.0, (0.25, 0.26, 0.28), 0.9),
        "accent": ("whitepaint", 2.2, (0.90, 0.90, 0.90), 0.6),
    }),
    "manta": dict(seed=21, number="21", panel=(1.6, 0.7), mats={
        "body": ("composite", 2.6, (0.60, 0.62, 0.65), 0.8),
        "armour": ("gunmetal", 2.0, (0.29, 0.30, 0.33), 0.9),
        "accent": ("whitepaint", 2.2, (0.90, 0.90, 0.90), 0.6),
    }),
    "locust": dict(seed=34, number="34", panel=(1.1, 0.8), mats={
        "body": ("military", 2.2, (0.44, 0.46, 0.40), 1.0),
        "armour": ("darkarmour", 2.0, (0.25, 0.26, 0.23), 0.9),
        "accent": ("whitepaint", 2.2, (0.90, 0.90, 0.90), 0.6),
    }),
    "bulwark": dict(seed=52, number="52", panel=(1.2, 1.0), mats={
        "body": ("darkarmour", 2.4, (0.38, 0.37, 0.35), 1.0),
        "armour": ("military", 2.0, (0.23, 0.23, 0.24), 0.9),
        "accent": ("whitepaint", 2.2, (0.90, 0.90, 0.90), 0.6),
    }),
    # pod and panels (tiles darkplating and solarcell given with --tiles): dark grey pod,
    # black solar panels with a fine grid, the frames in the player colour
    "podpanel": dict(seed=3, number=None, panel=(0.9, 0.9), hazard=False, mats={
        "pod": ("darkplating", 1.8, (0.31, 0.32, 0.34), 0.9),
        "frame": ("darkarmour", 1.6, (0.17, 0.18, 0.19), 0.8),
        "panel": ("solarcell", 1.2, (0.055, 0.06, 0.075), 1.0),
        "accent": ("whitepaint", 2.2, (0.90, 0.90, 0.90), 0.6),
    }),
    # flat-coloured CC0 ships of the packs, prepared by prep_flat.py: Kenney's speeders
    # (light hull, blue-grey armour) and Quaternius' Rae (dark grey hull, orange zone)
    "speeder-c": dict(seed=43, number="43", panel=(1.0, 0.7), mats={
        "body": ("composite", 2.2, (0.66, 0.68, 0.71), 0.8),
        "armour": ("gunmetal", 2.0, (0.50, 0.53, 0.58), 0.9),
        "accent": ("whitepaint", 2.2, (0.90, 0.90, 0.90), 0.6),
    }),
    "speeder-d": dict(seed=58, number="58", panel=(1.1, 0.75), mats={
        "body": ("whitepaint", 2.2, (0.70, 0.71, 0.73), 0.9),
        "armour": ("military", 2.0, (0.48, 0.51, 0.56), 0.9),
        "accent": ("whitepaint", 2.2, (0.90, 0.90, 0.90), 0.6),
    }),
    "rae": dict(seed=66, number="66", panel=(1.3, 0.8), mats={
        "body": ("darkarmour", 2.2, (0.28, 0.29, 0.31), 1.0),
        "accent": ("whitepaint", 2.2, (0.90, 0.90, 0.90), 0.6),
    }),
}
DEFAULT_MAT = ("gunmetal", 2.0, (0.35, 0.36, 0.38), 0.9)


# ---------------------------------------------------------------------------------------------
# glTF in and out

def read_glb(path):
    g = G.GLTF2().load(path)
    blob = g.binary_blob()

    def acc(i):
        a = g.accessors[i]
        bv = g.bufferViews[a.bufferView]
        n = {"VEC3": 3, "VEC2": 2, "SCALAR": 1}[a.type]
        off = (bv.byteOffset or 0) + (a.byteOffset or 0)
        return np.frombuffer(blob, np.float32, a.count * n, off).reshape(a.count, n).copy()
    parts = []
    for ni in g.scenes[g.scene or 0].nodes:
        root = g.nodes[ni]
        for ci in root.children or []:
            node = g.nodes[ci]
            prims = []
            for p in g.meshes[node.mesh].primitives:
                assert p.indices is None, "indexed primitives are not expected from shipgen"
                prims.append((g.materials[p.material].name, acc(p.attributes.POSITION), acc(p.attributes.NORMAL)))
            parts.append((node.name, prims))
    return g, parts


def write_glb(src, parts_uv, atlas_png, path, matnames):
    blob = bytearray()
    views, accessors, meshes = [], [], []

    def add(arr, target=None, minmax=False, typ=G.VEC3):
        off = len(blob)
        blob.extend(arr.tobytes())
        while len(blob) % 4:
            blob.append(0)
        views.append(G.BufferView(buffer=0, byteOffset=off, byteLength=arr.nbytes, target=target))
        a = G.Accessor(bufferView=len(views) - 1, componentType=G.FLOAT, count=len(arr), type=typ)
        if minmax:
            a.min = arr.min(0).tolist()
            a.max = arr.max(0).tolist()
        accessors.append(a)
        return len(accessors) - 1
    for pname, prims in parts_uv:
        gp = []
        for mat, pos, nrm, uv in prims:
            ip = add(pos.astype(np.float32), G.ARRAY_BUFFER, True)
            inn = add(nrm.astype(np.float32), G.ARRAY_BUFFER)
            iu = add(uv.astype(np.float32), G.ARRAY_BUFFER, typ=G.VEC2)
            gp.append(G.Primitive(attributes=G.Attributes(POSITION=ip, NORMAL=inn, TEXCOORD_0=iu),
                                  material=matnames.index(mat)))
        meshes.append(G.Mesh(name=pname, primitives=gp))
    off = len(blob)
    blob.extend(atlas_png)
    while len(blob) % 4:
        blob.append(0)
    views.append(G.BufferView(buffer=0, byteOffset=off, byteLength=len(atlas_png)))
    images = [G.Image(bufferView=len(views) - 1, mimeType="image/png", name="atlas")]
    src_mats = {m.name: m for m in src.materials}
    materials = []
    for name in matnames:
        sm = src_mats[name]
        materials.append(G.Material(name=name, pbrMetallicRoughness=G.PbrMetallicRoughness(
            baseColorFactor=[1.0, 1.0, 1.0, 1.0], baseColorTexture=G.TextureInfo(index=0),
            metallicFactor=sm.pbrMetallicRoughness.metallicFactor,
            roughnessFactor=sm.pbrMetallicRoughness.roughnessFactor), emissiveFactor=sm.emissiveFactor,
            doubleSided=False, extras=sm.extras))
    root = src.nodes[src.scenes[src.scene or 0].nodes[0]]
    nodes = [G.Node(name=root.name, children=list(range(1, 1 + len(parts_uv))), extras=root.extras)]
    for i, (pname, _) in enumerate(parts_uv):
        nodes.append(G.Node(name=pname, mesh=i))
    gl = G.GLTF2(asset=G.Asset(generator="d2x shipgen.py v2 + texture_ships.py", version="2.0"), scene=0,
                 scenes=[G.Scene(nodes=[0])], nodes=nodes, meshes=meshes, materials=materials,
                 accessors=accessors, bufferViews=views, buffers=[G.Buffer(byteLength=len(blob))],
                 images=images, samplers=[G.Sampler(magFilter=9729, minFilter=9987, wrapS=33071, wrapT=33071)],
                 textures=[G.Texture(sampler=0, source=0)])
    gl.set_binary_blob(bytes(blob))
    gl.save_binary(path)


# ---------------------------------------------------------------------------------------------
# charts

def tri_normal(t):
    n = np.cross(t[1] - t[0], t[2] - t[0])
    l = np.linalg.norm(n)
    return n / l if l > 0 else np.array([0.0, 1.0, 0.0])


def frame_for(n):
    """Right/up of a viewer looking at the surface from outside (glTF: +Y up, +Z forward)."""
    up = np.array([0.0, 1.0, 0.0]) if abs(n[1]) < 0.85 else np.array([0.0, 0.0, 1.0])
    r = np.cross(up, n)
    r /= np.linalg.norm(r)
    return r, np.cross(n, r)


def make_charts(tris, tri_part, tri_mat, nrm):
    """Connected groups of triangles of one part and material facing the same box side."""
    axis = np.argmax(np.abs(nrm), 1)
    side = np.sign(nrm[np.arange(len(nrm)), axis]).astype(int)
    key = list(zip(tri_part, tri_mat, axis, side))
    parent = list(range(len(tris)))

    def find(i):
        while parent[i] != i:
            parent[i] = parent[parent[i]]
            i = parent[i]
        return i
    edges = {}
    q = lambda p: tuple(np.round(p, 4))
    for i, t in enumerate(tris):
        for a in range(3):
            e = tuple(sorted((q(t[a]), q(t[(a + 1) % 3]))))
            edges.setdefault((key[i], e), []).append(i)
    for (_, _), ids in edges.items():
        for j in ids[1:]:
            a, b = find(ids[0]), find(j)
            if a != b:
                parent[a] = b
    groups = {}
    for i in range(len(tris)):
        groups.setdefault(find(i), []).append(i)
    charts = []
    for ids in groups.values():
        ids = np.array(ids)
        area = np.array([np.linalg.norm(np.cross(tris[i][1] - tris[i][0], tris[i][2] - tris[i][0])) / 2 for i in ids])
        m = (nrm[ids] * area[:, None]).sum(0)
        m /= np.linalg.norm(m)
        if np.min(nrm[ids] @ m) > math.cos(math.radians(12)):
            n = m                                   # flat: along its own normal
        else:
            n = np.zeros(3)
            n[axis[ids[0]]] = side[ids[0]]          # curved: along the box axis
        r, u = frame_for(n)
        pts = tris[ids].reshape(-1, 3)
        p2 = np.stack([pts @ r, pts @ u], 1)
        charts.append(dict(ids=ids, n=n, r=r, u=u, lo=p2.min(0), hi=p2.max(0), area=area.sum(),
                           mat=tri_mat[ids[0]], part=tri_part[ids[0]]))
    return charts


def find_mirrors(charts, tris):
    """Charts that are the mirror image (in x) of another chart share its texels (the ships are
    symmetric about a plane x = xm): chart["mirror_of"] = index of the painted chart."""
    from scipy.spatial import cKDTree
    allp = tris.reshape(-1, 3)
    xm = (allp[:, 0].min() + allp[:, 0].max()) / 2
    pts = [tris[c["ids"]].reshape(-1, 3) for c in charts]
    cen = np.array([p.mean(0) for p in pts])
    for i, c in enumerate(charts):
        if "mirror_of" in c or c.get("is_master"):
            continue
        mp = pts[i] * [-1, 1, 1] + [2 * xm, 0, 0]
        mc = mp.mean(0)
        for j, d in enumerate(charts):
            if j == i or d["mat"] != c["mat"] or len(d["ids"]) != len(c["ids"]) or "mirror_of" in d:
                continue
            if np.linalg.norm(cen[j] - mc) > 2e-3:
                continue
            if cKDTree(pts[j]).query(mp)[0].max() < 2e-3 and cKDTree(mp).query(pts[j])[0].max() < 2e-3:
                c["mirror_of"] = j
                d["is_master"] = True
                c["xm"] = xm
                break


def pack(charts, density):
    """Skyline packing at `density` texels per unit; returns False if it does not fit."""
    todo = [i for i, c in enumerate(charts) if "mirror_of" not in c]
    order = sorted(todo, key=lambda i: -max(charts[i]["hi"] - charts[i]["lo"]))
    sky = np.zeros(SIZE, int)
    for i in order:
        c = charts[i]
        w, h = (c["hi"] - c["lo"]) * density
        best = None
        for rot in (False, True):
            W, H = (int(math.ceil(h if rot else w)) + 2 * PAD, int(math.ceil(w if rot else h)) + 2 * PAD)
            if W > SIZE:
                continue
            # lowest position (then leftmost) where the chart fits on the skyline
            win = np.lib.stride_tricks.sliding_window_view(sky, W).max(1)
            x = int(np.argmin(win))
            y = int(win[x])
            if y + H <= SIZE and (best is None or (y + H, y) < (best[0] + best[3], best[0])):
                best = (y, x, W, H, rot)
        if best is None:
            return False
        y, x, W, H, rot = best
        sky[x:x + W] = y + H
        c["at"], c["rot"] = (x + PAD, y + PAD), rot
    return True


def chart_uv(c, p, density):
    """Texel coordinates of points p (N,3) in chart c."""
    a = (p @ c["r"] - c["lo"][0]) * density
    b = (c["hi"][1] - p @ c["u"]) * density     # image rows go down
    if c["rot"]:
        a, b = b, (c["hi"][0] - c["lo"][0]) * density - a
    return np.stack([a + c["at"][0], b + c["at"][1]], 1)


# ---------------------------------------------------------------------------------------------
# noise and tiles

def value_noise3(p, scale, seed, octaves=4):
    rng = np.random.default_rng(seed)
    out = np.zeros(len(p))
    amp, tot = 1.0, 0.0
    for o in range(octaves):
        g = rng.random((32, 32, 32))
        q = p / scale * (2 ** o) + rng.random(3) * 32
        out += amp * ndimage.map_coordinates(g, q.T, order=1, mode="grid-wrap")
        tot += amp
        amp *= 0.5
    return out / tot


_tiles = {}
TILE_DIRS = [os.path.join(HERE, "tiles")]


def tile(name, px):
    k = (name, px)
    if k not in _tiles:
        path = next(p for d in TILE_DIRS for p in (os.path.join(d, name + ".png"), os.path.join(d, "tile-" + name + ".png"))
                    if os.path.exists(p))
        im = Image.open(path).convert("RGB")
        _tiles[k] = np.asarray(im.resize((px, px), Image.LANCZOS), np.float64) / 255.0
    return _tiles[k]


def smoothstep(a, b, x):
    t = np.clip((x - a) / (b - a), 0, 1)
    return t * t * (3 - 2 * t)


# seven-segment stencil digits in a 0..1 x 0..1.8 box
SEG = {"a": (0.1, 1.6, 0.9, 1.8), "b": (0.75, 0.95, 0.95, 1.7), "c": (0.75, 0.1, 0.95, 0.85),
       "d": (0.1, 0.0, 0.9, 0.2), "e": (0.05, 0.1, 0.25, 0.85), "f": (0.05, 0.95, 0.25, 1.7),
       "g": (0.15, 0.8, 0.85, 1.0)}
DIGITS = {"0": "abcdef", "1": "bc", "2": "abged", "3": "abgcd", "4": "fgbc", "5": "afgcd", "6": "afgedc",
          "7": "abc", "8": "abcdefg", "9": "abcfgd"}


def digits_mask(x, y, text, height):
    """x, y: local coordinates (units) around the decal centre; returns 0..1 coverage."""
    s = height / 1.8
    w = len(text) * 1.25 - 0.25
    X, Y = x / s + w / 2, y / s + 0.9
    m = np.zeros_like(x)
    for k, ch in enumerate(text):
        for sg in DIGITS[ch]:
            x0, y0, x1, y1 = SEG[sg]
            x0, x1 = x0 + 1.25 * k, x1 + 1.25 * k
            m = np.maximum(m, (X >= x0) & (X <= x1) & (Y >= y0) & (Y <= y1))
    return m


# ---------------------------------------------------------------------------------------------

def texture(in_glb, out_glb, style_name, atlas_out=None, report=None):
    st = STYLES[style_name]
    src, parts = read_glb(in_glb)
    tris, tri_part, tri_mat, tri_n, refs = [], [], [], [], []
    matnames = []
    for pi, (pname, prims) in enumerate(parts):
        for qi, (mat, pos, nrm) in enumerate(prims):
            if mat not in matnames:
                matnames.append(mat)
            for k in range(len(pos) // 3):
                t = pos[3 * k:3 * k + 3].astype(np.float64)
                tris.append(t)
                tri_part.append(pi)
                tri_mat.append(mat)
                n = nrm[3 * k].astype(np.float64)
                g = tri_normal(t)        # the face's own normal (sources may be smooth-shaded)
                tri_n.append(g if g @ n >= 0 else -g)
                refs.append((pi, qi, k))
    tris = np.array(tris)
    tri_n = np.array(tri_n)
    charts = make_charts(tris, tri_part, tri_mat, tri_n)
    find_mirrors(charts, tris)
    lo, hi = 5.0, 200.0
    for _ in range(30):
        mid = (lo + hi) / 2
        if pack(charts, mid):
            lo = mid
        else:
            hi = mid
    density = lo
    while not pack(charts, density):      # the packing is not monotonic in the density
        density *= 0.99

    # rasterise: per texel chart id, point and barycentric triangle
    chart_id = -np.ones((SIZE, SIZE), int)
    P = np.zeros((SIZE, SIZE, 3))
    tri_uv = np.zeros((len(tris), 3, 2))
    overlap = 0
    for ci, c in enumerate(charts):
        if "mirror_of" in c:
            m = charts[c["mirror_of"]]
            for ti in c["ids"]:
                tri_uv[ti] = chart_uv(m, tris[ti] * [-1, 1, 1] + [2 * c["xm"], 0, 0], density)
            continue
        for ti in c["ids"]:
            t = tris[ti]
            uv = chart_uv(c, t, density)
            tri_uv[ti] = uv
            x0, y0 = np.floor(uv.min(0)).astype(int)
            x1, y1 = np.ceil(uv.max(0)).astype(int)
            xs, ys = np.meshgrid(np.arange(x0, x1 + 1), np.arange(y0, y1 + 1))
            px, py = xs.ravel() + 0.5, ys.ravel() + 0.5
            (ax, ay), (bx, by), (cx, cy) = uv
            d = (by - cy) * (ax - cx) + (cx - bx) * (ay - cy)
            if abs(d) < 1e-12:
                continue
            l0 = ((by - cy) * (px - cx) + (cx - bx) * (py - cy)) / d
            l1 = ((cy - ay) * (px - cx) + (ax - cx) * (py - cy)) / d
            l2 = 1 - l0 - l1
            ins = (l0 >= -1e-6) & (l1 >= -1e-6) & (l2 >= -1e-6)
            X, Y = xs.ravel()[ins], ys.ravel()[ins]
            ok = (X >= 0) & (X < SIZE) & (Y >= 0) & (Y < SIZE)
            X, Y = X[ok], Y[ok]
            L = np.stack([l0[ins][ok], l1[ins][ok], l2[ins][ok]], 1)
            overlap += int(np.sum((chart_id[Y, X] >= 0) & (chart_id[Y, X] != ci)))
            chart_id[Y, X] = ci
            P[Y, X] = L @ t
    covered = chart_id >= 0
    # distance (texels) from each covered texel to its chart's edge
    edge_d = ndimage.distance_transform_edt(covered)
    # gutters: copy the nearest chart texel (point extrapolated along the chart plane)
    _, (iy, ix) = ndimage.distance_transform_edt(~covered, return_indices=True)
    cid = chart_id[iy, ix]
    Pf = P[iy, ix].copy()
    gy, gx = np.nonzero(~covered)
    for k in range(len(gy)):
        y, x = gy[k], gx[k]
        c = charts[cid[y, x]]
        dx, dy = (x - ix[y, x]) / density, (y - iy[y, x]) / density
        if c["rot"]:
            dx, dy = -dy, dx
        Pf[y, x] += dx * c["r"] - dy * c["u"]
    edge_d[~covered] = 0.0

    # per-texel attributes
    flat = cid.ravel()
    pos = Pf.reshape(-1, 3)
    R = np.array([c["r"] for c in charts])[flat]
    U = np.array([c["u"] for c in charts])[flat]
    N = np.array([c["n"] for c in charts])[flat]
    mat = np.array([c["mat"] for c in charts])[flat]
    w2 = np.stack([(pos * R).sum(1), (pos * U).sum(1)], 1)   # coordinates in the chart plane (units)
    ed = edge_d.ravel()
    zmin, zmax = tris[..., 2].min(), tris[..., 2].max()
    zrel = (pos[:, 2] - zmin) / (zmax - zmin)                 # 0 rear .. 1 nose
    rng = np.random.default_rng(st["seed"])
    seed = st["seed"]
    col = np.zeros((SIZE * SIZE, 3))

    # grime and wear noise
    grime = value_noise3(pos, 1.6, seed + 1)
    fine = value_noise3(pos, 0.25, seed + 2, octaves=3)
    streak = value_noise3(pos * [3.0, 3.0, 0.35], 1.0, seed + 3, octaves=3)

    # panel seams in the chart plane: rows of height sv, panels of width su with a brick offset
    su, sv = st["panel"]
    row = np.floor(w2[:, 1] / sv + 0.37)
    off = (row * 0.618 % 1.0) * su
    du = np.abs(((w2[:, 0] + off) / su + 0.5) % 1.0 - 0.5) * su * density
    dv = np.abs((w2[:, 1] / sv + 0.37 + 0.5) % 1.0 - 0.5) * sv * density
    dseam = np.minimum(du, dv)
    seam = np.clip(1.2 - dseam, 0, 1)                             # dark groove, about 1 texel
    bevel = np.clip(1.2 - np.abs(dseam - 1.5), 0, 1)             # light lip beside it

    for m in set(mat):
        sel = mat == m
        if m in ("cockpit", "window"):
            # glass: deep blue, a sky reflection on the upper faces, a sharp diagonal glint
            up = np.clip(N[sel, 1], 0, 1)
            g = 0.5 + 0.5 * np.sin((w2[sel, 0] * 0.8 + w2[sel, 1] * 1.6) * 2.2)
            glint = smoothstep(0.93, 0.99, g)
            base = np.array([0.05, 0.10, 0.19]) if m == "cockpit" else np.array([0.03, 0.04, 0.06])
            c = base + up[:, None] * np.array([0.07, 0.14, 0.22]) + glint[:, None] * np.array([0.35, 0.45, 0.5])
            frame = np.clip(1.8 - ed[sel], 0, 1)[:, None]
            c = c * (1 - frame) + np.array([0.12, 0.13, 0.14]) * frame
            col[sel] = c
            continue
        if m == "engine":
            # glowing nozzle: white-hot centre, orange, a dark red rim
            cids = cid.ravel()[sel]
            dmax = np.zeros(len(charts))
            np.maximum.at(dmax, cids, ed[sel])
            t = np.clip(ed[sel] / np.maximum(dmax[cids], 1.0), 0, 1)
            hot = np.array([1.0, 0.97, 0.80])
            mid = np.array([1.0, 0.62, 0.18])
            rim = np.array([0.55, 0.16, 0.05])
            c = np.where(t[:, None] > 0.45, mid + (hot - mid) * smoothstep(0.45, 1.0, t)[:, None],
                         rim + (mid - rim) * smoothstep(0.0, 0.45, t)[:, None])
            col[sel] = c
            continue
        tname, tsize, target, contrast = st["mats"].get(m, DEFAULT_MAT)
        if m == "panel" and m not in st["mats"]:
            tname, tsize, target, contrast = ("darkarmour", 1.6, (0.09, 0.095, 0.11), 0.8)
        px = max(16, int(round(tsize * density)))
        tl = tile(tname, px)
        tm = tl.reshape(-1, 3).mean(0)
        coords = np.stack([(-w2[sel, 1] / tsize) * px, (w2[sel, 0] / tsize) * px], 0)
        smp = np.stack([ndimage.map_coordinates(tl[..., k], coords, order=1, mode="grid-wrap") for k in range(3)], 1)
        detail = np.clip(smp / tm, 0, 3) ** contrast
        c = np.array(target) * detail
        if m == "accent":
            # painted zone: shallow seams, light chipping, no dirt (the colour must stay clear)
            c = c * (1 - 0.22 * seam[sel, None]) * (1 + 0.06 * bevel[sel, None])
            chip = (fine[sel] > 0.70) & (ed[sel] < 4)
            c[chip] *= 0.7
            c *= (1 - 0.6 * np.clip(1.5 - ed[sel], 0, 1))[:, None]
            col[sel] = c
            continue
        if m == "panel" and tname == "solarcell":
            # solar panel: the tile's black cells and fine grid, a faint sheen, dark frame edge
            sheen = 0.75 + 0.5 * value_noise3(pos[sel], 2.5, seed + 9, 2)
            c = c * sheen[:, None] * (1 - 0.5 * np.clip(2.0 - ed[sel], 0, 1))[:, None]
            col[sel] = c
            continue
        # plating (the tiles carry their own seams and rivets): edge creases, worn edges, grime, soot
        e = ed[sel]
        crease = np.clip(1.6 - e, 0, 1)
        wear = np.clip(3.5 - e, 0, 1) * (fine[sel] > 0.55)
        c = c * (1 + 0.55 * wear[:, None])
        c = c * (1 - 0.5 * crease[:, None])
        dirt = smoothstep(0.45, 0.8, grime[sel]) * 0.22 + smoothstep(0.55, 0.85, streak[sel]) * 0.15
        soot = np.clip(1 - zrel[sel] / 0.25, 0, 1) ** 2 * 0.35
        c = c * (1 - np.clip(dirt + soot, 0, 0.6))[:, None] + np.array([0.04, 0.035, 0.03]) * dirt[:, None]
        col[sel] = c

    # decals on chosen charts
    decals = []
    painted = lambda c: "mirror_of" not in c
    sidecharts = [i for i, c in enumerate(charts) if painted(c) and c["mat"] in ("armour", "body") and abs(c["n"][0]) > 0.75]
    if st.get("hazard", True):
        # hazard stripes on the rear end of the larger side faces
        big = sorted(sidecharts, key=lambda i: -charts[i]["area"])[:4]
        for i in big:
            sel = cid.ravel() == i
            zc = pos[sel, 2]
            z0 = zc.min()
            band = (zc < z0 + 0.55) & (zc > z0 + 0.08)
            stripe = ((pos[sel, 1] + pos[sel, 2]) / 0.14 % 1.0) < 0.5
            c = col[sel]
            yellow = np.array([0.82, 0.62, 0.10]) * (0.85 + 0.3 * fine[sel])[:, None]
            black = np.array([0.05, 0.05, 0.05])
            paint = np.where(stripe[:, None], yellow, black)
            worn = fine[sel] > 0.72
            m_ = band & ~worn
            c[m_] = paint[m_]
            col[sel] = c
            decals.append(("hazard", i))
    tops = [i for i, c in enumerate(charts) if painted(c) and not c.get("is_master") and c["mat"] in ("body",) and c["n"][1] > 0.8]
    if st.get("number") and tops:
        # hull number on the largest upward face, reading from the rear
        i = max(tops, key=lambda i: charts[i]["area"])
        c_ = charts[i]
        sel = cid.ravel() == i
        ctr = tris[c_["ids"]].reshape(-1, 3).mean(0)
        size = min(c_["hi"] - c_["lo"])
        h = min(0.55, size * 0.45)
        rel = pos[sel] - ctr
        x, y = rel[:, 0] * -1.0, rel[:, 2]           # glTF x is mirrored: -x is the pilot's right
        msk = digits_mask(x, y, st["number"], h) * (fine[sel] < 0.8)
        cc = col[sel]
        cc = cc * (1 - msk[:, None]) + np.array([0.86, 0.85, 0.80]) * msk[:, None] * (0.85 + 0.2 * fine[sel])[:, None]
        col[sel] = cc
        decals.append(("number " + st["number"], i))
    # vents: dark slatted grilles on the two largest upward armour faces
    vtops = sorted([i for i, c in enumerate(charts) if painted(c) and c["mat"] == "armour" and c["n"][1] > 0.6],
                   key=lambda i: -charts[i]["area"])[:2]
    for i in vtops:
        c_ = charts[i]
        sel = cid.ravel() == i
        ctr = tris[c_["ids"]].reshape(-1, 3).mean(0)
        rel = pos[sel] - ctr
        w, l = min(0.5, (c_["hi"] - c_["lo"]).min() * 0.5), 0.6
        inside = (np.abs(rel[:, 0]) < w / 2) & (np.abs(rel[:, 2]) < l / 2)
        slat = (rel[:, 2] / 0.1 % 1.0) < 0.45
        cc = col[sel]
        cc[inside] = np.where(slat[inside, None], np.array([0.03, 0.03, 0.035]), cc[inside] * 0.75)
        rim = inside & ((np.abs(rel[:, 0]) > w / 2 - 1.2 / density) | (np.abs(rel[:, 2]) > l / 2 - 1.2 / density))
        cc[rim] = np.array([0.08, 0.08, 0.085])
        col[sel] = cc
        decals.append(("vent", i))

    img = np.clip(col.reshape(SIZE, SIZE, 3), 0, 1)
    img8 = (img * 255 + 0.5).astype(np.uint8)
    buf = io.BytesIO()
    Image.fromarray(img8).save(buf, "PNG", optimize=True)
    png = buf.getvalue()
    if atlas_out:
        open(atlas_out, "wb").write(png)

    # UVs of every vertex, per part and material as in the source
    parts_uv = []
    for pi, (pname, prims) in enumerate(parts):
        out = []
        for qi, (m, p, n) in enumerate(prims):
            out.append([m, p, n, np.zeros((len(p), 2))])
        parts_uv.append((pname, out))
    for ti, (pi, qi, k) in enumerate(refs):
        parts_uv[pi][1][qi][3][3 * k:3 * k + 3] = tri_uv[ti] / SIZE
    write_glb(src, [(n, [tuple(x) for x in ps]) for n, ps in parts_uv], png, out_glb, matnames)
    info = dict(style=style_name, charts=len(charts), mirrored=sum("mirror_of" in c for c in charts), texels_per_unit=round(density, 1),
                coverage=round(float(covered.mean()), 3), overlap_texels=overlap, png_bytes=len(png),
                decals=[d for d, _ in decals])
    if report:
        json.dump(info, open(report, "w"), indent=1)
    return info


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("input")
    ap.add_argument("output")
    ap.add_argument("style", choices=sorted(STYLES))
    ap.add_argument("--atlas")
    ap.add_argument("--tiles", help="another folder with tile PNGs, searched first")
    a = ap.parse_args()
    if a.tiles:
        TILE_DIRS.insert(0, a.tiles)
    print(json.dumps(texture(a.input, a.output, a.style, a.atlas)))


if __name__ == "__main__":
    main()
