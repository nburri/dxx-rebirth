# Custom player ships (design)

Status: design only, nothing implemented. Target branch:
`experimental-netcode` (protocol v2, `Documentation/network-protocol-v2.md`,
cited as "v2 §n"). D2X-Rebirth with OpenGL only. Line numbers are omitted;
function and type names are the anchors.

Every player may fly a ship model of their own: new geometry and new
textures, not a repaint of the Pyro-GX. Other players see it and recognise
the pilot by it. The feature is purely cosmetic. Collision size, gun
positions, physics, hit detection and everything the host checks stay
exactly those of the Pyro-GX, for everyone, whichever model is drawn.

Decisions that need the user are marked **Decision Dn** with options and a
recommendation. They are collected in §10.

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
4. **Independent of the renderer upgrade.** It works on today's OpenGL
   path; when the exp-visuals work (per-pixel lighting, bloom) lands it can
   use the extra data (normals, emissive mask) without format changes.

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
| Size | Scaled so that the bounding radius equals the Pyro's `rad` (Decision D3). |
| Triangles | ≤ 4000 (Pyro-class budget with headroom; 8 ships ≈ 32 k triangles per frame, trivial for any GPU but bounded for weak PCs). |
| Vertices | ≤ 8000 (16-bit indices). |
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
canonicalise. Little-endian layout:

```
header   magic "DXSH" u32 | format_version u16 | flags u16 | file_size u32
         | section_count u16 | reserved u16
section* type u32 (FourCC) | size u32 | payload (4-byte aligned)

MANI  manifest: key=value lines, UTF-8 restricted to printable ASCII:
      name (id, [a-z0-9_-], ≤ 24), title (≤ 32), author (≤ 32),
      licence (SPDX id, required), version, description (≤ 120),
      converter version, source hash (of the .glb, informational)
BNDS  radius f32, centre f32×3, mins f32×3, maxs f32×3
VERT  count u32, then per vertex: pos f32×3, normal i16×3 (snorm),
      uv f32×2, part u8, pad u8                                (28 bytes)
INDX  count u32 (multiple of 3), u16 indices
MATL  count u8 (≤ 4), per material: first index u32, index count u32,
      albedo texture id u8, mask texture id u8 (0xFF = none), flags u8
TEXR  per texture: id u8, PNG bytes (size from the section)
PART  count u8 (≤ 9), per part: centre f32×3, radius f32
LOD1  optional second VERT/INDX/MATL set
GUNS  optional 8 × {present u8, pos f32×3} (informational, §3.1)
```

The content hash is SHA-256 over the whole file, truncated to 128 bits
for the wire (Decision D5). The manifest is inside the file, so the
author/licence travel with the model.

### 3.3 Converter `dxship-convert`

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

Silhouette area from the three axis views compared with the Pyro's (D3:
error outside 0.7–1.4 ×, warning outside 0.8–1.25 ×). Error: colour-mask zone under 5 % of the
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

Messages (reliable, protocol 113):

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

**Recommendation: staged.** Stage 2 = Option A plus `SHIP_INFO` with Pyro
fallback (no transfer code, nothing untrusted from the network). Stage 3 =
Option B once the group wants ships that are not in a release. Option B is
the only part with real risk (untrusted files, bandwidth), so it comes last
and behind a host setting.

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

### 6.2 exp-visuals

If the per-pixel lighting work lands, `draw_custom_ship` passes vertex
normals and the mask to its shader, and the emissive channel feeds bloom.
Nothing in the format changes; the fixed-function path stays the fallback.

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
the normalisation to the Pyro's bounding radius and the silhouette check
(D3). Dark albedo, near-invisible textures or huge emissive areas are
discouraged by converter warnings; the colour zone is mandatory (≥ 5 % of
the visible texel area) so team colours always show.

## 9. Staged plan

| Stage | PR content | Effort |
|---|---|---|
| S1 | `common/main/dxship_format.h` parser + writer, `dxship-convert` (cgltf), `--check`, `--reference`, unit tests incl. fuzz cases, authoring guide and Blender template. No game change. | 3–4 days |
| S2 | Game: load `ships/`, RGBA texture upload, `draw_custom_ship` (lighting, tint, emissive, cloak, LOD), debris side table, pilot setting + ship menu with preview. No protocol change: you see your own ship (preview, death camera, end-level fly-out), and a debug option `-shipfor <pid>:<name>` assigns ships to other players or bots locally for screenshots and tests. | 4–5 days |
| S3 | Protocol 112: `SHIP_INFO`, `CustomShips` game setting, per-player table, apply at `LEVEL_START`, bots with ships, demo side file, movement-recording header. Bundled ships in the release packages (Option A). | 2–3 days |
| S4 | Protocol 113: transfer (Option B): `SHIP_REQUEST/DATA/UNAVAILABLE`, pacing, cache, consent UI, lobby display of who is missing which ship. | 3–4 days |
| S5 (optional) | exp-visuals integration (per-pixel lighting, bloom from the mask), VBOs, a part that rotates (flag on a part). | 2 days |

Each stage is a PR into `experimental-netcode` with its own review; S1 and
S2 do not touch the protocol and can merge before the group has a model.

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

## 10. Decisions for the user

| Id | Question | Options | Recommendation |
|---|---|---|---|
| D1 | Runtime format | A extended POF (palettised, limits, software renderer) / B own `.dxship` + OpenGL mesh path | B |
| D2 | Player identification beyond the model | colour mask tinted with player/team colour mandatory / author's fixed colours | Mandatory mask zone, team colour in team games |
| D3 | Size normalisation | strict: bounding radius = Pyro, silhouette 0.8–1.25 enforced / radius only, silhouette warning | Radius enforced, silhouette as error outside 0.7–1.4, warning outside 0.8–1.25 |
| D4 | Budgets | textures 512² and file 1 MiB / 1024² and 2 MiB | 512² and 1 MiB (transfer ≈ 16 s worst case) |
| D5 | Hash | SHA-256 (vendored public-domain implementation, ~200 lines) / existing CRC32 | SHA-256, 128 bits on the wire |
| D6 | Debris | A authored parts / B automatic split / C none | A with B as fallback |
| D7 | Two players with the same ship | allowed (colour distinguishes) / host enforces unique | Allowed |
| D8 | Downloads from the host | ask / always / never as pilot option, default | Default "ask" |
| D9 | Distribution | A bundled only / B host relay / staged | Staged: A in S3, B in S4 |
| D10 | Converter | C++ tool with cgltf in `common/tools` / Blender add-on (Python) | C++ tool first; an add-on that calls it can follow |
