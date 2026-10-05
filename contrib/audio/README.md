# Vendored audio decoders

Used only to read the player's own taunt file (`taunt.wav`, `taunt.ogg`,
`taunt.mp3`, `taunt.flac`; `common/misc/taunt_decode.cpp`,
`Documentation/taunts.md`).  They are compiled into the game, so the
decoding works the same in every build, whatever codecs the SDL_mixer of
the platform has.  Peers never decode these formats: a taunt travels as
the game's own 16-bit PCM.

| File | Upstream | Version | Licence |
|---|---|---|---|
| `dr_wav.h` | https://github.com/mackron/dr_libs, commit `dfe8377631000664666519fdb83da193fd8037f4` | v0.14.6 | public domain or MIT-0 (end of the file) |
| `dr_mp3.h` | same | v0.7.4 | public domain or MIT-0 (end of the file) |
| `dr_flac.h` | same | v0.13.4 | public domain or MIT-0 (end of the file) |
| `stb_vorbis.c` | https://github.com/nothings/stb, commit `1ee679ca2ef753a528db5ba6801e1067b40481b8` | v1.22 | MIT or public domain (end of the file) |

The files are unchanged except for the three lines at the top
(`#pragma GCC system_header`), which keep the game's warning flags out of
third-party code, and three lines in `stb_vorbis.c`, each marked
`DXX-Rebirth`, found by fuzzing (`common/unittest/taunt.cpp` mutates files
of every format):

- `setup_malloc` and `setup_temp_malloc` refuse negative and absurd sizes
  (a broken header asked for gigabytes);
- `vorbis_deinit` does nothing when the decoder lives in the caller's
  buffer (`stb_vorbis_alloc`, which `taunt_decode.cpp` always passes):
  freeing is a no-op there anyway, and walking the half set-up tables of
  a broken file read out of bounds.
