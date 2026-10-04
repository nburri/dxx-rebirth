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

## Download

Builds with libcurl (`use_curl=1`, the default; Windows, Linux AppImage
and macOS releases) download the AI packs of
[nburri/d2xx-ai-textures](https://github.com/nburri/d2xx-ai-textures)
while *HD texture packs (AI)* is on.  Code:
`common/include/texture_download.h`, `common/main/texture_download.cpp`
(libcurl, background thread) and `common/main/texture_download_format.cpp`
(manifest, zip and name checks; unit test `test-texture-download`).

- At startup a background thread reads the pack list
  `https://github.com/nburri/d2xx-ai-textures/releases/download/manifest/manifest.json`
  (`{"format":1,"packs":{"corona":{"version":1,"url":…,"size":…,"sha256":…,"files":163}}}`).
- When a level loads and the manifest has a pack for its mission that is
  missing or newer than the installed one, the zip downloads in the
  background (a dim progress line at the bottom of the game screen; the
  menu line shows the state).  The game never waits for it: the level
  uses what is installed, and the new pack from the next level load on.
- Checks: HTTPS only, links only below
  `https://github.com/nburri/d2xx-ai-textures/releases/download/`, size
  and SHA-256 from the manifest, then the zip: only stored (uncompressed)
  `textures/<mission>/<name>.png` / `<name>#<frame>.png` entries
  (lower case, no other paths: an archive with anything else is refused),
  CRC-32 and PNG signature per file, at most 8 MB per picture, 4096
  pictures and 512 MB per pack, exactly as many pictures as the manifest
  says.
- Installation: into the user folder (`~/.d2x-rebirth/textures/<mission>/`
  on Linux, the game folder on Windows unless configured otherwise),
  unpacked in `texture-downloads/<mission>.new/` and swapped in by
  renaming the folder, so the game sees the old or the new pack, never a
  half one.  `textures/<mission>/pack-version.txt` records the version.
  A `textures/<mission>/` folder without that file was installed by hand
  and is never replaced or deleted.
- Menu: *Download all packs now* (fetches the list again and every pack),
  *Delete downloaded packs* (only folders with `pack-version.txt`).
- `-notexturedownload`: no network access at all.  `-notexturepack` or
  the toggle off: no downloads either.  `-botarena` never downloads;
  `-visshot` waits for the download of its mission (up to 10 minutes),
  so that the pictures show the pack.
- The console and `gamelog.txt` get lines starting with
  `Texture download:` (pack list, download size and time, source host,
  checks, errors).  A failed pack is tried again after 10 minutes at the
  earliest; a failed pack list after a minute.

Hosts for firewalls: `github.com` (release links) redirects to
`release-assets.githubusercontent.com` (older GitHub setups:
`objects.githubusercontent.com`), both HTTPS (443).  Proxies set in
`HTTPS_PROXY` are honoured (libcurl).  Certificates: the Windows
certificate store; on Linux the usual CA bundle files of the
distribution; on macOS the system's.
