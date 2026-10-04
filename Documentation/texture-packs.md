# Texture packs

The OpenGL renderer can replace the game's 64x64 wall textures with PNG
pictures of any size (power-of-two sizes are best; others are scaled).
The code is in `common/include/texture_pack.h`, `common/main/texture_pack.cpp`
and `similar/arch/ogl/ogl.cpp`.

## Files

Searched in the game folder and the user folder (the PhysFS search path):

- `textures/<mission>/<name>.png`: only for that mission; `<mission>` is the
  mission file name without extension, in lower case (`corona.mn2` ->
  `textures/corona/`).
- `textures/<name>.png`: for every mission.  A bitmap that the level
  replaced with its own art (a `.POG` file) uses only the mission folder.

`<name>` is the bitmap name in the PIG file; frames of an animation are
`<name>#<frame>.png` (e.g. `lava03#2.png`).  Alpha 0 is transparent; in a
supertransparent overlay, alpha 0 with the colour #FF00FF is a hole that
also hides the base texture.

## Settings

- Options -> Graphics -> Visual Quality -> *HD texture packs (AI)*
  (`TexturePack` in `descent.cfg`, default on).  The line below the
  checkbox lists the installed packs (the folders in `textures/`, and
  "all missions" for pictures directly in it).
- The change applies when you leave the Graphics menu (all textures load
  again); a level that starts with another mission also reloads them.
- `-notexturepack` on the command line turns packs off for one run.
- The console (`-verbose`) shows each replacement; after a level loads, a
  line gives the number of replaced textures, their memory and load time.
