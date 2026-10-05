# Making a custom ship

A custom ship is a `.dxship` file: a model that is drawn instead of the
Pyro-GX for one player. It is purely cosmetic: the collision size, the gun
positions and the handling stay the Pyro's (`Documentation/custom-ships.md`).
This page is for whoever makes or converts a ship.

## 1. The model

Any 3-D tool that exports **glTF 2.0** (`.glb` or `.gltf`, e.g. Blender:
File → Export → glTF 2.0) or **Wavefront OBJ** (with its `.mtl`).

| Item | Rule |
|---|---|
| Orientation | glTF convention: +Y up, the ship's nose towards +Z. Other tools: `--forward` and `--up`. |
| Size | Any; the converter centres the ship on its bounding sphere and scales it so that its outline seen from the front and the rear (40 % each; side and top 10 % each) equals the Pyro's, with its outermost point at most 1.47 × the Pyro's radius (4.735 units, the collision sphere of every ship). A ship that is still below 0.9 × at that limit is "too thin" (a needle seen nose-on): the converter warns, and such a ship is not bundled. Compact, wide ships suit the rule best; a large ship is shrunk. |
| Triangles | At most 10000; vertices at most 16000 (after splitting at UV and normal seams). |
| Textures | PNG or JPEG, any size; scaled to powers of two of at most 512 × 512 (`--texture-size` for less). At most 4 textures in the file, colour masks included. |
| Materials | At most 8 different ones after conversion (texture, colour zone, double-sided). Base colour factors and vertex colours are kept. Transparency is not: ships are opaque. |
| File | At most 1 MiB. |
| Animation, skins | Ignored (the rest pose is used). |

## 2. The player colour zone (required)

Every ship must show the player's colour (the team's colour in team games),
on at least 5 % of its surface; the converter refuses a ship without it.
Mark the zone in one of these ways:

- a **material** whose name contains `accent`, `player`, `colour` or
  `color` (or any material named with `--colour-material NAME`): all of it;
- **vertex colours** of pure magenta (#FF00FF);
- `--colour-key RRGGBB[:T]`: the texels of that colour (± T, default 24)
  in the textures, for palette-textured models;
- `--colour-variant other.png`: the texels that differ between the model's
  texture and the same texture recoloured (packs that come in several
  colours);
- `--colour-mask mask.png`: a painted mask, red channel = weight.

The converter greys the zone (keeping its shading) and the game tints it.
`--preview views.png` shows the result with the zone in red.

## 3. Debris

When the ship explodes, its parts fly apart. Name the nodes (glTF) or
objects/groups (OBJ) of the parts `debris_…` (`debris_wing_left`,
`debris_engine`, …; up to 9). Without any, the converter splits the ship
itself: the outer thirds left and right (or front and back) become debris.
`--no-debris` turns that off.

## 4. Gun markers (optional)

Empty nodes `gun0` … `gun7` mark where your model's guns are. They are
informational only: shots always leave from the Pyro's gun points, and the
converter warns when a marker is more than one unit away from them, so
that you can place the model's barrels where the shots appear.

## 5. Converting

```
shipconv model.glb -o viper.dxship --name viper --title "Viper"
         --author "Your Name" --licence CC-BY-4.0
         --source https://example.org/viper [--preview viper.png]
shipconv --check viper.dxship
```

`--name` is the ship's id (`a-z`, `0-9`, `_`, `-`, at most 24 characters);
`--licence` is required (an SPDX id such as `CC0-1.0` or `CC-BY-4.0`). The
title, author, licence and source travel inside the file, so the credit
goes wherever the ship goes. `shipconv` is in the Windows package next to
the game; on other systems build it with
`scons register_runtime_test_plain_link_targets=1 shipconv`.

## 6. Using it

Put the file into the `ships` folder of the game (next to the game data or
in the user folder). Ships of other players are sent to you by the host of
a game automatically (unless you switch that off); they are kept in
`ships/cache`.
