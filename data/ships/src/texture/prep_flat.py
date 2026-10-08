#!/usr/bin/env python3
"""Prepare a flat-coloured third-party ship (CC0) for texture_ships.py.

    python3 prep_flat.py <model.obj|model.gltf> <out.glb> <style>

Kenney's Space Kit speeders (OBJ, one colour per material) and Quaternius'
Rae (glTF, colours from a 32 x 32 palette atlas) have no texture of their
own.  This turns them into the layout texture_ships.py reads from shipgen:
one root node with one part node ("hull"), non-indexed triangles, and one
material per surface kind (body, armour, accent, cockpit, engine, panel),
chosen per ship below from the source material or palette colour and,
for the dark parts, from where the face is (nozzle recesses at the rear
are engines, the dark glass on top towards the nose is the cockpit).
The model is scaled to the width of the fork's own ships so that the
plating tiles and decals come out at the same size; shipconv scales the
result to the Pyro's size anyway.

Needs numpy, Pillow and pygltflib.
"""
import argparse, base64, io, os, sys
import numpy as np
from PIL import Image
import pygltflib as G

WIDTH = 7.6        # x extent of the fork's own ships (shipgen units)

KIND = {           # kind: (base colour, metallic, roughness, emissive, role)
    "body": ((0.6, 0.62, 0.65), 0.6, 0.5, (0, 0, 0), "hull"),
    "armour": ((0.3, 0.31, 0.34), 0.6, 0.6, (0, 0, 0), "dark armour plates"),
    "accent": ((0.92, 0.92, 0.92), 0.3, 0.5, (0, 0, 0), "player colour zone: multiply with player colour"),
    "cockpit": ((0.06, 0.12, 0.22), 0.1, 0.1, (0, 0, 0), "canopy"),
    "engine": ((1.0, 0.55, 0.15), 0.0, 0.5, (1.0, 0.5, 0.1), "engine glow (emissive)"),
    "panel": ((0.1, 0.1, 0.11), 0.4, 0.7, (0, 0, 0), "dark recessed panels"),
}


def kenney_dark(c, lo, hi):
    """Kenney's "dark" material: nozzle recesses at the rear, canopy towards the nose, else
    recessed panels (intakes, gun ports)."""
    if c[2] < lo[2] + 0.25 * (hi[2] - lo[2]):
        return "engine"
    if c[2] > 0.35 * hi[2]:
        return "cockpit"
    return "panel"


def kenney(mat, c, n, lo, hi):
    return {"metal": "body", "metalDark": "armour", "metalRed": "accent"}.get(mat) or kenney_dark(c, lo, hi)


def rae(col, c, n, lo, hi):
    if col == "BD5C20":
        return "accent"              # the orange hull: the colour zone, as before
    if col == "222222":
        return "cockpit"
    # the dark rear plate of the fuselage block: the main thruster
    if n[2] < -0.85 and abs(c[0]) < 0.1 * (hi[0] - lo[0]) and c[2] < lo[2] + 0.1 * (hi[2] - lo[2]) \
            and c[1] < lo[1] + 0.6 * (hi[1] - lo[1]):
        return "engine"
    return "body"


SHIPS = {"speeder-c": kenney, "speeder-d": kenney, "rae": rae}


def load_obj(path):
    V, out, mat = [], [], None
    for l in open(path):
        p = l.split()
        if not p:
            continue
        if p[0] == "v":
            V.append([float(x) for x in p[1:4]])
        elif p[0] == "usemtl":
            mat = p[1]
        elif p[0] == "f":
            idx = [int(x.split("/")[0]) - 1 for x in p[1:]]
            for k in range(1, len(idx) - 1):
                out.append((mat, np.array([V[idx[0]], V[idx[k]], V[idx[k + 1]]], float), None))
    return out


def load_gltf(path):
    """Triangles (palette colour, world positions, vertex normals) of a palette-textured glTF."""
    g = G.GLTF2().load(path)
    blob = g.binary_blob()
    if blob is None:
        blob = base64.b64decode(g.buffers[0].uri.split(",", 1)[1])

    def acc(i):
        a = g.accessors[i]
        bv = g.bufferViews[a.bufferView]
        n = {"VEC3": 3, "VEC2": 2, "SCALAR": 1, "VEC4": 4}[a.type]
        dt = {5126: np.float32, 5123: np.uint16, 5125: np.uint32, 5121: np.uint8}[a.componentType]
        assert not bv.byteStride or bv.byteStride == n * np.dtype(dt).itemsize
        off = (bv.byteOffset or 0) + (a.byteOffset or 0)
        return np.frombuffer(blob, dt, a.count * n, off).reshape(a.count, n).astype(np.float64)
    bv = g.bufferViews[g.images[0].bufferView]
    pal = np.asarray(Image.open(io.BytesIO(blob[bv.byteOffset or 0:(bv.byteOffset or 0) + bv.byteLength])).convert("RGB"))

    def local(node):
        if node.matrix:
            return np.array(node.matrix, float).reshape(4, 4).T
        x, y, z, w = node.rotation or [0, 0, 0, 1]
        R = np.array([[1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w)],
                      [2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w)],
                      [2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y)]])
        M = np.eye(4)
        M[:3, :3] = R * np.array(node.scale or [1, 1, 1])
        M[:3, 3] = node.translation or [0, 0, 0]
        return M
    out = []

    def walk(ni, parent):
        node = g.nodes[ni]
        M = parent @ local(node)
        if node.mesh is not None:
            nm = np.linalg.inv(M[:3, :3]).T
            for p in g.meshes[node.mesh].primitives:
                pos = acc(p.attributes.POSITION)
                pos = pos @ M[:3, :3].T + M[:3, 3]
                nrm = acc(p.attributes.NORMAL) @ nm.T
                nrm /= np.linalg.norm(nrm, axis=1, keepdims=True)
                uv = acc(p.attributes.TEXCOORD_0)
                idx = acc(p.indices).astype(int).ravel() if p.indices is not None else np.arange(len(pos))
                for k in range(0, len(idx), 3):
                    t = idx[k:k + 3] if np.linalg.det(M[:3, :3]) > 0 else idx[k:k + 3][::-1]
                    u = uv[t].mean(0)
                    h, w = pal.shape[:2]
                    col = pal[int(np.clip(u[1] * h, 0, h - 1)), int(np.clip(u[0] * w, 0, w - 1))]
                    out.append(("%02X%02X%02X" % tuple(col), pos[t], nrm[t]))
        for c in node.children or []:
            walk(c, M)
    for ni in g.scenes[g.scene or 0].nodes:
        walk(ni, np.eye(4))
    return out


def prep(src, dst, ship):
    tris = load_obj(src) if src.lower().endswith(".obj") else load_gltf(src)
    allp = np.concatenate([t for _, t, _ in tris])
    lo, hi = allp.min(0), allp.max(0)
    rule = SHIPS[ship]
    groups = {}
    for key, t, vn in tris:
        fn = np.cross(t[1] - t[0], t[2] - t[0])
        l = np.linalg.norm(fn)
        if l < 1e-12:
            continue
        fn /= l
        kind = rule(key, t.mean(0), fn, lo, hi)
        if vn is None:
            vn = np.repeat(fn[None], 3, 0)
        groups.setdefault(kind, []).append((t, vn))
    # centre in x and z, scale to WIDTH
    ctr = np.array([(lo[0] + hi[0]) / 2, 0.0, (lo[2] + hi[2]) / 2])
    s = WIDTH / (hi[0] - lo[0])
    blob = bytearray()
    views, accessors, prims, materials = [], [], [], []

    def add(arr, minmax=False):
        off = len(blob)
        blob.extend(arr.astype(np.float32).tobytes())
        views.append(G.BufferView(buffer=0, byteOffset=off, byteLength=arr.astype(np.float32).nbytes, target=G.ARRAY_BUFFER))
        a = G.Accessor(bufferView=len(views) - 1, componentType=G.FLOAT, count=len(arr), type=G.VEC3)
        if minmax:
            a.min, a.max = arr.min(0).tolist(), arr.max(0).tolist()
        accessors.append(a)
        return len(accessors) - 1
    for kind in KIND:
        if kind not in groups:
            continue
        pos = np.concatenate([t for t, _ in groups[kind]]).reshape(-1, 3)
        nrm = np.concatenate([n for _, n in groups[kind]]).reshape(-1, 3)
        ip, inn = add((pos - ctr) * s, True), add(nrm)
        base, metal, rough, emis, role = KIND[kind]
        materials.append(G.Material(name=kind, pbrMetallicRoughness=G.PbrMetallicRoughness(
            baseColorFactor=list(base) + [1.0], metallicFactor=metal, roughnessFactor=rough),
            emissiveFactor=list(emis), extras={"role": role}))
        prims.append(G.Primitive(attributes=G.Attributes(POSITION=ip, NORMAL=inn), material=len(materials) - 1))
    gl = G.GLTF2(asset=G.Asset(generator="d2x prep_flat.py", version="2.0"), scene=0, scenes=[G.Scene(nodes=[0])],
                 nodes=[G.Node(name=ship, children=[1]), G.Node(name="hull", mesh=0)],
                 meshes=[G.Mesh(name="hull", primitives=prims)], materials=materials, accessors=accessors,
                 bufferViews=views, buffers=[G.Buffer(byteLength=len(blob))])
    gl.set_binary_blob(bytes(blob))
    gl.save_binary(dst)
    return {k: len(v) for k, v in groups.items()}


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("input")
    ap.add_argument("output")
    ap.add_argument("ship", choices=sorted(SHIPS))
    a = ap.parse_args()
    print(prep(a.input, a.output, a.ship))


if __name__ == "__main__":
    main()
