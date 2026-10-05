# Custom player ships (design)

Status: design with the user's decisions (§10); implemented in stages
(first on a side branch, then brought to `experimental-netcode` with
protocol 112; protocol v2, `Documentation/network-protocol-v2.md`, cited
as "v2 §n"). D2X-Rebirth with OpenGL only. Line numbers are omitted; function and type names are the anchors.

Every player may fly a ship model of their own: new geometry and new
textures, not a repaint of the Pyro-GX. Other players see it and recognise
the pilot by it. The feature is purely cosmetic. Collision size, gun
positions, physics, hit detection and everything the host checks stay
exactly those of the Pyro-GX, for everyone, whichever model is drawn.

Decisions are marked **Decision Dn**. The options are kept for the record;
what the user decided (2026-10-04) is in §10 and overrides the
recommendations in the text.

## 1. What the engine does today

### 1.1 Polygon models

- `polymodel` (`common/main/polyobj.h`): a byte-code blob `model_data`
  plus per-submodel tables (`submodel_ptrs`, `submodel_offsets`,
  `submodel_norms`/`submodel_pnts` for the separating plane,
  `submodel_rads`, `submodel_parents`, `submodel_mins/maxs`), whole-model
  `mins`, `maxs`, `rad`, `first_texture`, `n_textures`, `n_models` and a
  `simpler_model` (LOD chain).
- Limits: `MAX_SUBMODELS` = 10; `MAX_POLYGON_MODELS` = 200 in D2, of which
  `N_D2_POLYGON_MODELS` = 166 come from `descent2.ham` and the rest are
  used by HXM robot replacements and the exit models (`bm_free_extra_models`
  in `similar/main/bm.cpp`); `MAX_POLYOBJ_TEXTURES` = 100;
  `MAX_POINTS_PER_POLY` = 64 (`common/include/3d.h`); at most 1000 rotated
  points per draw (`polygon_model_points` in `common/include/interp.h`);
  a POF file must fit `MODEL_BUF_SIZE` = 32 KiB (`similar/main/polyobj.cpp`).
- The byte code (`similar/3d/interp.cpp`) has `OP_DEFPOINTS`,
  `OP_DEFP_START`, `OP_FLATPOLY`, `OP_TMAPPOLY`, `OP_SORTNORM` (BSP order for
  the software renderer), `OP_RODBM`, `OP_SUBCALL`, `OP_GLOW`. Lighting is
  per polygon: the object light times a facing term from the polygon normal
  (`get_noglow_light`), or a glow value (`OP_GLOW`, used for the engine
  glow, which `draw_polygon_object` scales with thrust,
  `similar/main/object.cpp`).
- Loading: retail models come from the HAM (`bm_read_all`,
  `polymodel_read` + `polygon_model_data_read`, `similar/main/bm.cpp`),
  level replacements from HXM (`load_robot_replacements`), loose POF files
  through `load_polygon_model` → `read_model_file` (chunks `OHDR`, `SOBJ`,
  `GUNS`, `ANIM`, `TXTR`, `IDTA`; version 6–8).
- Textures: `ObjBitmapPtrs`/`ObjBitmaps` (`MAX_OBJ_BITMAPS` = 610) index
  into `GameBitmaps` (`MAX_BITMAP_FILES` = 2620), which are 8-bit palettised
  PIG bitmaps; `ogl_loadtexture` (`similar/arch/ogl/ogl.cpp`) expands them to
  RGBA through the palette. There is no path for true-colour object textures
  today.
- Drawing: `draw_polygon_model` (`similar/main/polyobj.cpp`) builds a
  `grs_bitmap *` list from `alt_textures` or the model's own textures, pages
  them in and calls `g3_draw_polygon_model`. OpenGL draws each polygon as a
  `GL_TRIANGLE_FAN` from client arrays (`_g3_draw_tmap`), with depth test on
  (`ogl_start_frame`). The fork's Windows build is `sdl2=1`, which requires
  OpenGL (`SConstruct`: "Rebirth does not support SDL2 without OpenGL").

### 1.2 The player ship

- `player_ship` (`common/main/player.h`): `model_num`, `expl_vclip_num`,
  `mass`, `drag`, `max_thrust`, `wiggle`, `max_rotthrust` and eight
  `gun_points`. There is exactly one (`only_player_ship`, read from the HAM
  by `player_ship_read`), reached through `Player_ship`.
- The **collision size** comes from the model: `console.size =
  Polygon_models[Player_ship->model_num].rad` (`init_player_object`,
  `object.cpp`), and the same in `nd_read_object` (demo playback).
- **Gun positions** are `Player_ship->gun_points`, never the drawn model:
  `Laser_player_fire_spread_delay` (`similar/main/laser.cpp`) rotates
  `Player_ship->gun_points[gun_num]` by the ship's orientation. The bots use
  the same table (`bot.cpp`). The v2 host checks a `FIRE` origin against the
  shooter's *ship position* (v2 §6.5: origin within 5 units of the rewound
  position), not against model geometry.
- **Physics** use `Player_ship->mass/drag/max_thrust/max_rotthrust/wiggle`
  (`multi_reset_player_object`, `controls.cpp`).
- Consequence: drawing a different model changes no gameplay value *as long
  as nobody derives `obj.size` from it*. The design keeps every
  `OBJ_PLAYER` on `Player_ship->model_num` and adds the custom model only as
  a draw-time substitution.

### 1.3 Player colours

`multi_reset_object_texture` (`similar/main/multi.cpp`) sets
`pobj_info.alt_textures` = player id (or team in team games) and fills
`multi_player_textures[id-1]` (`N_PLAYER_SHIP_TEXTURES` = 32 slots) with the
Pyro's textures, replacing slots 4 and 5 by the per-player pair at
`First_multi_bitmap_num + 2·id`. That is the whole colour mechanism: two
textures swapped per player. The HUD colours are `player_rgb_normal`
(`gauges.cpp`).

### 1.4 Death, debris, views

- `explode_model` (`similar/main/fireball.cpp`) switches to
  `Dying_modelnums[model]` and, if the model has more than one submodel,
  creates one debris object per submodel 1..n-1
  (`object_create_debris`: `model_num` of the parent, `subobj_flags = 1 <<
  submodel`), leaving the parent drawing only submodel 0. The Pyro has a
  dying model (`model_name_dying` in `bmread.cpp`, `Dying_modelnums` in the
  HAM).
- `maybe_delete_object` uses `Dead_modelnums` (none for the Pyro; the player
  object goes to `RT_NONE`).
- Where a player ship is visible: other players in the normal view; your own
  ship in the death camera (`Dead_player_camera`, `object.cpp`), the
  end-level fly-out, demo playback, guided-missile and marker views. The
  rear view (`Rear_view` in `render.cpp`) looks backwards *from* the ship
  and draws no own model; the cockpit is 2-D art. Neither changes.
- Cloak: `draw_cloaked_object` fades the model (`draw_tmap_flat` while
  fully cloaked); `BrightPlayers` raises the light (`draw_polygon_object`).
- The debug model viewer in `menu.cpp` uses `draw_model_picture`.

### 1.5 Demos

`nd_write_object` writes no model number for `OBJ_PLAYER`;
`nd_read_object` sets `Player_ship->model_num`. An unknown demo event hits
`Int3()` in `newdemo_read_frame_information` and the stream desyncs, so a
new event breaks older readers.

### 1.6 Network (v2)

Star topology: everything goes through the host. `PLAYER_LIST` (0x09) is
8 × {callsign 9, connected, rank, team} = 96 bytes; reliable messages are
≤ 1024 bytes (`NET_V2_MAX_MESSAGE`), never fragmented by the transport,
split at the application layer when larger (v2 §3.4, §4.4); the reliable
send queue is bounded at 512 messages / 96 KiB per connection (v2 §3.6),
the level snapshot is paced at 32 KiB/s. The newest message id is
`CTF_NOTICE` 0x4A, protocol 111 (`MULTI_PROTO_VERSION` in
`common/main/multi.h`). `crc32_update` exists in
`common/main/net_v2_session.h`; there is no cryptographic hash in the tree.

### 1.7 D2X-XL (for comparison)

From D2X-XL's public sources and documentation (not part of this tree, not
re-verified here): it replaced *model numbers globally* with high-polygon
models loaded from a `models` folder in ASE (3ds Max ASCII export) and OOF
(Descent 3) formats, drawn by its own shader renderer, and it offered ship
types that also changed handling. Lessons for this design: a global
replacement does not let players tell each other apart; ASE/OOF are dead
formats with no current exporter; and changing handling is exactly what
this feature must not do.

### 1.8 Tools and packaging

`common/tools/movrec_dump.cpp` and `movrec_analyse.cpp` are built as
`RuntimeTest` targets in `SConstruct`. Releases zip the per-platform package
directories (`.github/workflows/release.yml`); the Windows package is
assembled by `contrib/packaging/windows/build_package.sh`. SDL2_image is
already a dependency (PCX loading, `similar/2d/pcx.cpp`), so PNG decoding is
available without a new library.

## 2. Principles

1. **Gameplay never sees the custom model.** `obj.size`, `model_num`, gun
   points, physics, the host's checks, the demo's object records and the
   network object data stay Pyro. The custom ship is chosen at draw time
   from a per-player table.
2. **Fallback is always the Pyro.** Missing, invalid, refused or still
   downloading → the player is drawn as the coloured Pyro, as today.
3. **Untrusted input.** A ship file from another player is data from the
   network; the parser is bounded, there is no code, no paths, no scripts.
4. **Independent of the renderer.** It works on today's fixed-function
   OpenGL path; a later shader path could use the extra data (normals,
   emissive mask) without format changes.

## 3. Model pipeline

### 3.1 Authoring

Blender (or any tool) → **glTF 2.0** (`.glb`, binary). glTF carries meshes,
normals, UVs, PNG textures, named nodes (for gun markers and debris parts)
and is exported by every current tool; OBJ is accepted as a fallback (no
named empties, so no markers or parts).

Authoring rules (shipped as `Documentation/custom-ships-authoring.md` with
the converter, plus a Blender template `.blend` with the Pyro's bounding box,
radius sphere and gun points as reference empties, generated from the HAM
values by the converter, so nothing of the retail data is redistributed):

| Item | Rule |
|---|---|
| Orientation | +Z forward (Descent `fvec`), +Y up; the converter converts from glTF's +Y-up/−Z-forward convention. |
| Origin | The ship's centre of mass; the converter re-centres on the bounding-sphere centre and warns if that moves it more than 10 % of the radius. |
| Size | Scaled so that the outline seen from the front and the rear (with a little of the side and top views) equals the Pyro's, the outermost point at most 1.47 × the Pyro's `rad` (Decision D3). |
| Triangles | ≤ 10000 (the free ships of §9.2 have up to 4500; 8 ships ≈ 80 k triangles per frame, trivial for any GPU from the last 15 years). |
| Vertices | ≤ 16000 (16-bit indices). |
| Textures | One albedo PNG, optional mask PNG; power of two, ≤ 512 × 512 (Decision D4); RGBA8. |
| Mask channels | R = player-colour amount (tinted with the player's or team colour), G = emissive (engine glow, scaled with thrust like `OP_GLOW`), B = reserved (0), A = unused. |
| Gun markers | Optional empties `gun0`…`gun7`. They never move the guns: the converter compares them with `Player_ship->gun_points` and warns when a marker is more than 1 unit away (muzzle flashes and shots appear at the Pyro's points). |
| Debris parts | Optional child objects `part1`…`part9` (Decision D6). |
| LOD | Optional `lod1` mesh, ≤ 25 % of the triangles, used beyond `Simple_model_threshhold_scale × rad` like the Pyro's `simpler_model`. |
| Animation | None in v1 of the format (submodel animation is a robot feature; a rotating part can come later as a flag on a part). |

### 3.2 Runtime format

**Decision D1 — runtime format.**

- **D1-A, extended POF through the existing interpreter.** Pros: debris,
  cloak, LOD, software renderer work unchanged. Cons: textures must become
  8-bit palettised and occupy `GameBitmaps`/`ObjBitmaps` slots (2620 / 610,
  shared with HXM replacements; up to 8 ships × several textures each);
  1000-point and 32 KiB limits; the converter must build a BSP
  (`OP_SORTNORM`) and N-gons; no smooth normals, no true colour, no emissive
  mask. Colour zones only as the existing "two swapped textures".
- **D1-B, own mesh format `.dxship`, drawn by a new OpenGL mesh path.**
  Triangles, float positions/normals/UVs, RGBA textures uploaded directly
  (new `ogl_loadtexture_rgba`, bypassing the palette), one
  `glDrawElements` per material. Pros: real custom look, true colour,
  colour mask, emissive, ready for per-pixel lighting. Cons: OpenGL only
  (acceptable: the fork's builds are all OpenGL), cloak/debris/LOD need
  their own small implementations (§6).
- **Recommendation: D1-B.** Floats are fine because nothing about the model
  is simulated; determinism is not needed for drawing.

`.dxship` is **one file** holding everything: manifest, mesh, textures.
Hash and transfer then concern a single blob, and there is nothing to
canonicalise. Little-endian layout (as implemented, format version 1;
`common/misc/dxship_format.cpp` is the reference):

```
header   magic "DXSH" | version u16 (1) | flags u16 (0) | file_size u32
         | section_count u16 (<= 24) | reserved u16 (0)
section* type u32 (FourCC) | size u32 | payload, zero padded to 4 bytes

MANI  "key=value\n" lines, printable ASCII: name (id, [a-z0-9_-], <= 24),
      title (<= 32), author (<= 48), licence (required, <= 32),
      source (<= 160), description (<= 160), converter
BNDS  radius f32 (the Pyro's), centre f32x3, mins f32x3, maxs f32x3
VERT  count u32 (<= 16000), per vertex: pos f32x3, normal snorm16x3,
      part u8, tint u8 (player colour weight), uv f32x2, colour RGBA8
      (32 bytes)
INDX  count u32 (multiple of 3, <= 30000), u16 indices
MATL  count u8 (<= 8), 3 zero bytes, per material: first index u32,
      index count u32, base colour RGBA8, texture u8, mask u8 (0xFF =
      none), flags u8 (1 tint the whole material, 2 double-sided), 0
TEXR  one PNG per section (<= 4), 8-bit grey/RGB/RGBA, power-of-two
      sides <= 512; a mask texture's red channel is the colour weight
PART  optional: count u8 (2..10; part 0 is the body), 3 zero bytes,
      per part: centre f32x3, radius f32
GUNS  optional: 8 x {present u8, 3 zero bytes, pos f32x3}
```

Unknown section types are skipped, so a later version can add sections
that older readers ignore. The colour zone is a material flag, a vertex
tint or a mask texture; the reader refuses a file without any (D2).

The content hash is SHA-256 over the whole file, all 32 bytes on the
wire (Decision D5). The manifest is inside the file, so the
author/licence travel with the model.

### 3.3 Converter `shipconv` (first named `dxship-convert`)

`common/tools/dxship_convert.cpp`, built like the `movrec-*` tools
(`RuntimeTest` in `SConstruct`) and shipped in the release package so a
modeller on Windows can run it. glTF parsing with **cgltf** (MIT, single
header, vendored under `contrib/`), PNG reading/writing with the
already-present SDL2_image or the vendored stb_image/stb_image_write
(public domain) for the tool only.

```
dxship-convert ship.glb --name viper --author "…" --licence CC-BY-4.0
               [--albedo a.png] [--mask m.png] [--ham descent2.ham]
               [--max-texture 512] [-o ships/viper.dxship]
dxship-convert --check ships/viper.dxship      # validate + print summary
dxship-convert --reference --ham descent2.ham -o pyro-reference.glb
```

Steps: load → triangulate → convert axes → re-centre → scale to the Pyro
radius (read from `--ham`, or from a built-in constant the game verifies at
load) → split parts by `partN` nodes → compute normals if missing → check
the budgets of §3.1 → downscale textures over the limit → write → re-read
with the *game's* parser (shared header `common/main/dxship_format.h`) and
fail if it rejects the result. `--reference` writes a glTF of the Pyro's
bounding sphere, box and gun points (numbers only, no retail geometry) for
the Blender template.

Size (D3): the converter scales the ship itself and prints one `size:` line
(the weighted outline before and after, front, side and top, scale,
outermost point); warnings when the result is outside 0.9–1.1 ×, and
"too thin" when the radius limit stops it below 0.9 ×. `--check` prints
the same ratios for any file. Error: colour-mask zone under 5 % of the
visible texel area (§8). Warnings: gun markers far from the Pyro points,
an emissive area larger than 25 % of the texture.

## 4. Packaging and selection

### 4.1 Where ships live

```
ships/<name>.dxship          bundled or installed by the user
ships/cache/<hash32>.dxship  received from a host (§5.3), LRU, ≤ 50 MiB
ships/src/<name>/…           optional sources (glb, png, licence text);
                             never loaded by the game
```

Mounted through PhysFS like the other data directories. The game scans
`ships/` at start (and when the ship menu opens), parses each file fully,
and lists the valid ones; invalid files are listed greyed with the reason.

### 4.2 Pilot setting

- New key `ship=<name>` in the `[D2X OPTIONS]` section of the `.plx` file
  (`similar/main/playsave.cpp`), default empty = Pyro-GX.
- Menu "Ship" under Options (or the pilot menu): a list of ships, a
  rotating preview of the selected one (rendered with the new mesh path
  into the menu canvas, like `draw_model_picture`), tinted in the player's
  colour, with title, author and licence. Changing it in a game takes effect
  at the next level (the choice is announced in the lobby, §5.1).
- Bots: the host may give bots ships (a `ship` field in the bot setup and
  `.botstyle` presets, `Documentation/multiplayer-bots.md`); bots then
  announce like humans.

## 5. Network

### 5.1 Announcing the choice (stage 2)

New reliable message **`SHIP_INFO` (0x4B)**, protocol **112**:

| Offset | Size | Field |
|---|---|---|
| 0 | 1 | `pid` |
| 1 | 1 | `flags` (bit 0: none = Pyro) |
| 2 | 24 | `name` (NUL-padded) |
| 26 | 16 | `hash` (first 128 bits of SHA-256) |
| 42 | 4 | `size` (bytes of the `.dxship`) |

- Client → host after `JOIN_ACCEPT` and on a change in the lobby; the host
  validates (`size` ≤ cap, `name` charset) and relays to all. The host sends
  `SHIP_INFO` for every occupied slot right after `PLAYER_LIST` to a joining
  player, and its own.
- `PLAYER_LIST` stays 96 bytes; no existing message changes.
- Receivers look up `hash` in `ships/` and `ships/cache/`. Found and valid →
  draw it. Otherwise → Pyro, and in stage 3 maybe request it.
- The choice is fixed for a level: a `SHIP_INFO` received during a level is
  stored and applied at the next `LEVEL_START` (no mid-fight model swaps).
- Host setting in `GAME_SETTINGS` (new byte, uses the protocol bump):
  `CustomShips` = 0 off (everyone sees Pyros) / 1 local only (show ships
  players already have) / 2 transfer (§5.3). Default 1.

### 5.2 Distribution — Option A: ship packs in the release (stage 2)

Freely licensed or own ships (CC0, CC-BY, CC-BY-SA, or the author's explicit
permission recorded in the manifest and in `ships/src/<name>/LICENCE`) are
committed under `contrib/ships/` and copied into the package by
`build_package.sh` (and the Linux/macOS equivalents). Everyone on the same
release has the same ships; `SHIP_INFO` alone is enough. Cost: a release per
new ship; each ship is ~100–500 KiB.

### 5.3 Distribution — Option B: host relays missing ships (stage 3)

Messages (reliable, protocol 112 as built; the design's table below
named them `SHIP_*`, the built ones are `ASSET_*`, §11.3):

| Id | Name | Direction | Payload |
|---|---|---|---|
| 0x4C | `SHIP_REQUEST` | client → host, host → owner | `hash` 16 |
| 0x4D | `SHIP_DATA` | owner → host, host → client | `hash` 16, `part` u16, `count` u16, ≤ 960 data bytes |
| 0x4E | `SHIP_UNAVAILABLE` | host → client | `hash` 16, `reason` u8 (too big, owner left, refused, invalid) |

- A client missing a hash sends `SHIP_REQUEST`. If the host has the file
  (own, bundled or cached), it sends it; otherwise it requests it from the
  owner, verifies it and caches it, then serves every requester from that
  copy. Peers never talk to each other (star topology, v2 §2.1).
- Size cap **1 MiB** per ship (Decision D4), checked against `SHIP_INFO.size`
  before anything is sent and again on every part.
- Pacing: the sender keeps at most 16 `SHIP_DATA` messages queued per
  connection (well inside 512 messages / 96 KiB, v2 §3.6) and limits itself
  to 32 KiB/s in the lobby, 8 KiB/s during a level (a 500 KiB ship takes
  ~16 s in the lobby). Transfers never delay gameplay messages: they are
  queued only when the connection's reliable backlog is under 8 KiB.
- The receiver assembles in memory, checks `count`, part order, total size,
  the SHA-256 and a full parse (§7) before writing to `ships/cache/`. A
  mismatch → discard, `SHIP_UNAVAILABLE` locally, Pyro.
- Consent (Decision D8): a pilot option "Download ships from the host:
  ask / always / never"; "ask" shows title, author, licence and size in the
  lobby. The host's `CustomShips` = 2 is required for any transfer.
- Joining mid-level: the transfer runs at the in-level rate; the player is
  drawn as a Pyro until the next level.

**Decided (D8, D9): both, in one stage.** The free ships are bundled in
the release packages, and the host sends any ship a client lacks
automatically, with no prompt; the only consent is the pilot option to
refuse ships from the host. The safety rules of §7 are what makes the
automatic transfer acceptable.

## 6. Rendering

### 6.1 Hook

`draw_polygon_object` (`similar/main/object.cpp`): for `OBJ_PLAYER` whose
player id has a loaded custom ship and whose `subobj_flags == 0`, call
`draw_custom_ship(canvas, ship, obj.pos, obj.orient, light, engine_glow,
tint, fade)` instead of `draw_polygon_model`. Everything that computes
`light` (`compute_object_light`, `BrightPlayers`) and the engine glow
value (thrust-scaled) is reused unchanged.

`draw_custom_ship` (new `similar/main/custom_ship.cpp` +
`similar/arch/ogl/ogl_mesh.cpp`):

- `g3_start_instance_matrix(pos, orient)` like `draw_polygon_model`, then
  the mesh in one or a few `glDrawElements` calls with client arrays (the
  renderer is fixed-function; VBOs are an optional optimisation).
- Lighting today: per-vertex `light × (ambient + k·max(0, n·v))` with the
  same constants as `get_noglow_light`, so a custom ship sits in the level's
  light like a Pyro. Emissive (mask G) adds `engine_glow` brightness.
- Colour: albedo × lerp(1, player colour, mask R). Player colour = the
  team colour in team games, else the player's colour, from the same table
  as the Pyro's swapped textures (`player_rgb_normal`), so the HUD and the
  ship agree. Done on the CPU per draw into the colour array, or with
  `GL_TEXTURE_ENV` combiners; no shader needed.
- LOD: `lod1` beyond the same distance rule as `simpler_model`.
- Cloak: the same fade value `draw_cloaked_object` computes; drawn with
  alpha blending, fully cloaked → flat dark like `draw_tmap_flat`.
- `-gl_*` texture filtering options apply to the RGBA textures too.

### 6.2 A later shader path

If the game ever gets per-pixel lighting, `draw_custom_ship` can pass
vertex normals and the mask to its shader, and the emissive channel could
feed a glow. Nothing in the format changes; the fixed-function path stays
the fallback.

### 6.3 Death and debris

Decision D6:

- **D6-A authored parts:** `partN` groups become debris. In
  `explode_model`, for a player with a custom ship, create debris objects
  as today (`object_create_debris`: size from the *Pyro's* submodel radii,
  so physics and lifetimes stay identical) but record in a side table
  `debris objnum → (player, part, signature)` which part to draw; the
  object keeps the Pyro `model_num`, so nothing else changes. Part 0 (the
  rest) is drawn on the dying player until the fireball removes it.
- **D6-B automatic:** the converter splits the mesh into up to 9 connected
  islands or spatial octants when no parts are authored.
- **D6-C none:** fireball only, no debris.
- **Recommendation:** A with B as the automatic fallback; the number of
  debris objects created stays the Pyro's (`n_models − 1`), extra parts are
  merged, missing ones reuse a part, so object counts (and the host's
  object id space) are unaffected.

### 6.4 Views

- Other players: the main use.
- Death camera, end-level fly-out, guided/marker views: through the same
  hook, nothing extra.
- Rear view and cockpit: unchanged (no own model is drawn there).
- External chase camera: none exists today; if added later it uses the
  hook.
- Automap: unchanged (players are markers).

### 6.5 Demos and recordings

- Demos (`newdemo.cpp`): player objects carry no model number, so playback
  draws the Pyro unless told otherwise. A new `ND_EVENT_SHIP_INFO` in the
  stream would hit `Int3()` in older readers and desync them, and bumping
  `DEMO_VERSION` does not help: `newdemo_read_demo_start` rejects only
  *older* versions. So the ship choices go into a **side file**
  `<demo>.ships` (text: one line `pid name hash` per player, rewritten at
  every level start), written by `newdemo_start_recording` next to the
  demo. Older builds ignore it; playback looks each hash up locally and
  falls back to the Pyro. Sending a demo with its ships means sending the
  side file (and the `.dxship` files if the receiver lacks them).
- Movement recordings (`Documentation/movement-recording.md`): add the ship
  name and hash to the per-player header record (format minor bump); the
  analysis tools ignore it.

## 7. Safety

- **Parser bounds:** every count and size is checked against the remaining
  section bytes before use; caps: file ≤ 1 MiB, ≤ 16 sections, ≤ 8000
  vertices, ≤ 12000 indices (4000 triangles) per LOD, ≤ 4 materials,
  ≤ 4 textures, ≤ 9 parts; every index < vertex count; floats finite and
  inside 4 × the Pyro radius; part ids ≤ part count.
- **Images:** PNG header read first; width/height ≤ the cap and power of
  two before decoding (`IMG_LoadPNG_RW` on an SDL_RWops over the section
  bytes); decoded size bounded by 4 × 512 × 512 × 4 bytes in total.
- **Text:** manifest values limited to printable ASCII, lengths capped,
  shown with the game font only (no format strings: always `"%s"`).
- **No code, no paths:** the file references nothing outside itself;
  received files are stored by hash, never by a name from the network.
- **Fuzzing:** a unit test (`common/unittest/dxship_format.cpp`) feeds
  truncated, bit-flipped and oversized files to the parser; CI runs it.
- **Network:** `SHIP_DATA` from anyone but the host (clients) or the
  requested owner (host) is dropped; per-connection cap of one transfer in
  flight; `SHIP_REQUEST` rate-limited to 4/s.
- **Licence:** required in the manifest; the converter refuses to write a
  file without it; bundling in releases additionally requires a free
  licence or recorded permission (§5.2). The Pyro's geometry and textures
  are never exported or redistributed.
- **Content:** a host with `CustomShips` = 0 shows everyone as Pyros; a
  player can hide a specific ship locally (menu: "Show as Pyro").

## 8. Fairness

The ship is cosmetic, but *visibility* is gameplay: a much smaller or
thinner model is harder to see and to aim at, a larger one easier. Hence
the normalisation of every ship to the Pyro's outline from the front and rear (D3). Dark albedo, near-invisible textures or huge emissive areas are
discouraged by converter warnings; the colour zone is mandatory (≥ 5 % of
the visible texel area) so team colours always show.

## 9. Staged plan

| Stage | PR content |
|---|---|
| S1 | `.dxship` reader and writer (`dxship_format.h`), SHA-256, the converter `shipconv` (glTF 2.0 / `.glb` and OBJ input, validation, scaling, colour zone, textures, debris parts, manifest), unit tests incl. a fuzz test of the reader. No game change. |
| S2 | Game: load `ships/`, RGBA texture upload, the mesh draw path (lighting, tint, cloak), debris, rear view and external camera, pilot setting "Ship" with a rotating preview; the CC0 ships bundled in `data/ships/` and the release packages. |
| S3 | Network (protocol bump): `SHIP_INFO`, the automatic transfer through the host (`SHIP_REQUEST/DATA/UNAVAILABLE`), the cache, the refuse option, bots with ships, the demo side record. |
| later | Emissive mask, LOD, VBOs, rotating parts. |

Each stage was a PR with its own review; the whole feature then came to
`experimental-netcode` in one PR (protocol 112) with the pilot option
"Show custom ships" (§11.4).

### 9.1 Tests

- **Converter:** unit tests on small synthetic glTF files (a cube, a cube
  with parts and markers, a 5000-triangle mesh that must fail, a missing
  licence that must fail) comparing the written file byte for byte with a
  golden file; `--check` on every bundled ship in CI.
- **Parser:** fuzz/mutation test in CI (§7).
- **Rendering:** a headless bot-arena run (`-botarena`) with eight bots on
  eight different ships, screenshots at fixed times compared by eye in the
  PR, and a frame-time comparison against all-Pyro (the frame-time probe).
- **Gameplay unchanged:** the same bot-arena seed with custom ships on and
  off must produce identical movement recordings (positions, hits, kills);
  this proves nothing gameplay-relevant reads the model.
- **Network:** two-player test (host + client): ship shown on both sides,
  Pyro fallback when one side lacks the file, (S4) transfer with an
  injected-loss link (`netcode-lagtest` tooling), corrupted transfer
  rejected, host setting 0 shows Pyros.

### 9.2 What the group must provide

- A modeller, or free models: CC0 or CC-BY spaceships (e.g. from
  OpenGameArt, Poly Pizza, Kenney, Sketchfab with a CC licence) re-scaled and
  textured with a colour mask. Each needs a licence note and the author's
  name for the manifest.
- One volunteer to test the authoring guide with the Blender template
  before S2 merges (a real ship to test with).
- For each ship: the name players will choose it by, and whether it may be
  bundled in public releases.

## 10. Decisions (user, 2026-10-04)

| Id | Question | Decision |
|---|---|---|
| D1 | Runtime format | **B**: own `.dxship` file and a new OpenGL mesh draw path. |
| D2 | Player identification | **Mandatory colour zone**, tinted with the player's colour (the team colour in team games). |
| D3 | Size | **Changed twice: scaled to the Pyro's outline as players see it.** Players see each other mostly head-on or from behind, so the converter scales each ship (uniformly) until its outline seen from the front (40 %), the rear (40 %), the side (10 %) and above (10 %) is 1.0 × the Pyro's (band 0.9–1.1 ×; `common/include/ship_silhouette.h`: the Pyro's 8.56, 13.71 and 35.82 square units from the front, side and top, measured from model 108), with its outermost point at most 1.47 × the Pyro's collision radius (4.735 units, which stays every ship's collision sphere; the reader accepts 1.5 ×). A ship that stays below 0.9 × at that limit is "too thin": the converter warns, and it is not bundled. The first rule (PR #103) matched the mean silhouette over all directions with a 1.3 × radius cap; before that ships were scaled to the collision radius. |
| D4 | Budgets | **512 × 512** textures, **1 MiB** per ship file. |
| D5 | Hash | **SHA-256** (full 32 bytes on the wire). |
| D6 | Debris | **A with B as fallback**: author-marked parts (`debris_*` nodes), else an automatic split. |
| D7 | Same ship for several players | **Allowed**; the colour zone tells them apart. |
| D8 | Consent to downloads | **Changed:** no prompt. Ships come automatically; a pilot option "Accept ships from the host" (default on) lets a player refuse. |
| D9 | Distribution | **Changed:** the host sends missing ships to the clients automatically, in the lobby and on a join, already in the first networked version (the group must not install anything by hand). Free ships are also bundled in the release packages. |
| D10 | Converter | **C++ tool** `shipconv` in `common/tools`. |

Consequences for the text above: the converter is `shipconv`
(`common/tools/shipconv.cpp`), debris parts are glTF nodes named
`debris_*` (not `partN`), §5.1 and §5.3 are one stage (S3), and
"ask" in §5.3 is replaced by the refuse option of D8.

## 11. As implemented

### 11.1 S1: format and converter

`dxship_format.h` (§3.2), `sha256.h`, `common/tools/shipconv.cpp`
(`Documentation/custom-ships-authoring.md`). The converter greys the
colour zone's texels (keeping their relative brightness) so that the game
only multiplies them with the player's colour. Ships were first scaled to the
Pyro's collision radius (0.46–0.83 × the Pyro's mean silhouette), then
to its mean silhouette over all directions (PR #103). Playtesters still
found them smaller than a Pyro, so D3 now weights what players see in
combat: the outline from the front and the rear. A debug build of the
side branch measured it in the game (a tool not carried over): the
pixels each ship covers at the same place and distance as a Pyro, seen
straight from the front, rear, side and top. At 40 units (corona level
1, 1280 × 720), weighted 40/40/10/10:

| Ship | Converter, weighted (front) | In game before: front, rear, weighted | In game now: front, rear, weighted | Outermost point |
|---|---|---|---|---|
| Striker | 0.99 × (0.87) | 0.64, 0.75, 0.79 | 0.80, 1.00, 1.02 | 1.47 × radius |
| Dispatcher | 1.00 × (1.04) | 0.88, 1.03, 0.93 | 0.95, 1.10, 1.00 | 1.41 × radius |
| Zenith | 1.00 × (1.11) | 1.15, 1.51, 1.15 | 1.05, 1.37, 1.05 | 1.15 × radius |
| Pancake | 1.00 × (1.06) | 1.16, 1.13, 1.06 | 1.06, 1.04, 0.98 | 1.05 × radius |
| Spitfire | 1.00 × (1.24) | 1.49, 1.41, 1.17 | 1.25, 1.18, 0.98 | 1.05 × radius |
| Executioner | 1.00 × (1.05) | 0.78, 0.92, 0.81 | 0.94, 1.19, 1.01 | 1.47 × radius |
| Rae | 0.95 × (0.88) | 0.67, 0.83, 0.77 | 0.85, 1.08, 1.00 | 1.47 × radius |

The game's numbers differ from the converter's by perspective (a long
ship's near end looks bigger) and lighting at the edges; all seven are
within 0.9–1.1 × weighted, none is too thin.

Files made by older converters still load; ships made by this one can
reach beyond 1.25 × the Pyro's radius, which readers before this change
reject (such a client sees a Pyro).

### 11.2 S2: drawing

- `common/arch/ogl/ogl_ship.cpp`: decodes the PNGs (vendored stb_image,
  after the reader's header checks; the decoded size must equal the
  header's), keeps per material and part index lists, uploads the albedo
  with the colour zone already tinted, one texture set per player colour,
  with CPU-built mipmaps. Each draw
  multiplies the 3D library's instance matrix into the modelview, turns
  culling off (both sides, lit by |n·view|), computes the vertex colours
  as the polygon models do (`get_noglow_light`: object light ×
  (1/4 + 3/4 facing), at most 1 like the light that multiplies a polygon
  model's texture, so that an untextured ship keeps its colours in a
  bright room) times vertex colour, base colour and the tint of
  untextured zones, and calls `glDrawElements` per material and part.
- `similar/main/custom_ship.cpp`: the registry of `ships/` and
  `ships/cache/` (scanned on first use and when the menu opens; every
  file fully checked), the per-player table, the hooks:
  `draw_polygon_object` and `draw_cloaked_object` (`object.cpp`) draw the
  custom ship for an `OBJ_PLAYER` (fading: the light scaled; cloaked:
  flat black with the cloak's alpha), else the Pyro as before.
- Death (D6): `explode_badass_player` (every player death: the local
  death sequence, a remote `MULTI_PLAYER_DERES`, a bot) starts the
  ship's parts as cosmetic pieces: no objects, no collisions, drawn after
  the mine (`render_frame`), each flying from its place with the ship's
  velocity plus an outward push and a spin, dimming from glowing to
  charred, gone after 1.2–3 s or when it leaves the mine. The Pyro's
  own debris objects of the local death (which still fly, as before) are
  not drawn for a custom ship. Remote deaths had no debris before and now
  show the pieces, without any object being created.
- Views: every view that draws player objects uses the hook: other ships
  in the normal and rear view, your own ship in the death camera and the
  end-level fly-out, guided-missile and marker views. There is no
  third-person camera in the game (it would show around corners in
  multiplayer); none was added.
- Pilot setting: `[ships]` section of the `.plx` (`ship=`, `accept=`,
  `show=`); Options → Ship... (`custom_ship_menu.cpp`): the list, a
  turning preview in a player colour (C cycles the colours), author,
  licence and source; applies at the next level start. A toggles "Accept
  ships from the host", S toggles "Show custom ships" (§11.4).
- Debug: `-shipfor pid:name,...` gives other players or bots ships on
  this machine only.

### 11.2.1 Textures of the fork's own ships

Anvil, Manta, Locust and Bulwark carry one 512 × 512 atlas each
(`data/ships/src/texture/texture_ships.py`, run by `convert-own.sh`):
box-projected charts (flat groups along their own normal), the mirrored
half of a symmetric ship sharing the texels of the other (about 33
texels per unit), skyline-packed with 4-texel gutters. The atlas is
painted per texel from its point on the hull: a tileable hull tile per
material (AI tiles from text prompts, CC0, `tiles/`), recoloured to the
ship's style, with creases and worn paint along the geometry's edges,
soot towards the rear, grime, hazard stripes, vents and a hull number.
Canopies and nozzles are painted bright; the renderer has no emissive
channel (mask G is not implemented), so they follow the room light like
everything else. The colour zone stays the material `accent`: its plate
is near white with shallow seams and little wear, so after the
converter's greying the player's colour reads clearly. Files: 360–480 KB.

### 11.3 S3: network

`common/main/net_v2_ships.h` (the protocol and the whole exchange as a
state machine, tested by `test-net-v2-ships`, also over the real
transport with loss) and `similar/main/net_ships.cpp` (the game's side).
Protocol 112 (`NET_V2_PROTO_VERSION`, `MULTI_PROTO_VERSION`): every
player needs a build with custom ships.

| Id | Message | Layout |
|---|---|---|
| 0x4b | `SHIP_INFO` | pid u8, flags u8 (1 = Pyro), size u32, SHA-256 32, name 25 (NUL padded); client → host for itself, host → all for every player and its bots |
| 0x4c | `ASSET_REQUEST` | kind u8, SHA-256 32; client → host, host → the owner |
| 0x4d | `ASSET_DATA` | kind u8, SHA-256 32, total u32, offset u32, ≤ 896 bytes, in order |
| 0x4e | `ASSET_UNAVAILABLE` | kind u8, SHA-256 32, reason u8 (unknown, refused, owner left, invalid) |

Asset kinds: 1 ship (≤ 1 MiB), 2 reserved for the taunts' sounds
(≤ 128 KiB); a kind announces its assets its own way and transfers
through `ship_exchange::request` / `note_owner`.

- A player announces its ship on joining (and again when the pilot picks
  another between levels); the host checks that a client speaks only for
  itself, relays to everyone and tells a joining client every player's
  ship. Bots: the host gives each one of its own ships, chosen by the
  bot's name (the same ship for the whole session); none → Pyro.
- A client that lacks an announced ship asks the host (no prompt, D8/D9)
  unless the pilot switched "Accept ships from the host" or "Show
  custom ships" off (§11.4). The host
  sends its copy, or first fetches it from the client who flies it (and
  keeps it), then serves everyone who waits. Data only from the peer
  that was asked, in order, of the announced size; the whole file must
  have its SHA-256 and pass the reader before it is stored in
  `ships/cache/<sha256>.dxship` and drawn. Peers never talk to each
  other.
- A player who changes its ship again within 3 s: the host keeps the
  newest change and applies it when the 3 s have passed.
- Pacing: 96 KiB/s in the lobby, 16 KiB/s during a level, and never more
  than 12 KiB of ship data waiting in a connection's reliable queue, so
  gameplay messages do not wait behind a transfer (a 600 KB ship relayed
  through the host takes about 15 s in the lobby).
- A ship that arrives during a level is drawn from then on.
- Demos: `<demo>.ships` next to the demo (`pid sha256 name` per line),
  written when recording starts and whenever a player's ship changes,
  renamed or deleted with the demo, read at playback; older builds
  ignore it.

Not done: a two-instance network test (the game has no unattended host
and join), the movement-recording header, a per-bot ship setting (bots
get one of the host's ships).

### 11.4 Show custom ships (pilot option)

Options → Ship..., key S: "Show custom ships" (`[ships] show=` in the
`.plx`, default on). Off: every player, the pilot's own ship included
(rear view, death camera), is drawn as the classic Pyro-GX in its player
colour, whatever ship it chose; no ship pieces fly at a death. The
pilot's own choice is still announced, so the others see it. The machine
fetches no ship for drawing (a client asks the host for none, a host
fetches none for itself); a host still relays ships to the clients who
want them. Turning it on again (also in a game) fetches the announced
ships this machine lacks (`ship_exchange::request_missing_ships`) and
draws them as they arrive (downloads already running when it is
turned off finish).
