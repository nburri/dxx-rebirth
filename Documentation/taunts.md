# Taunts (horn)

Status: phase 1 on the side branch `exp-visuals` (protocol 28784 (0x7000 + 112)). Phase 2,
the transfer of the players' own samples, rides on the custom ships' asset
transfer (Documentation/custom-ships.md §5.3) once that is merged.

## 1. For players

- **Key:** *Taunt / Horn* in the keyboard, joystick and mouse controls.
  New pilots and pilots from before this version get **V** on the
  keyboard (unless another control uses V); nothing on the joystick and
  the mouse. The horn sounds only from a living ship.
- **Sound:** *Options → Sound Effects & Music*, "Your horn":
  - *Own file (taunt.wav/mp3/ogg/flac)* (the default): a file called
    `taunt.wav`, `taunt.mp3`, `taunt.ogg` or `taunt.flac` (tried in this
    order; an unusable one is skipped) in the game's user folder, the one with the pilot files and
    `descent.cfg`. The line below says what was found ("taunt.mp3, 1.8 s",
    or why the file is not used). Without a usable file, Horn 1 sounds.
  - *Horn 1* to *Horn 4*: car horn, bike bell, air horn, beep beep.
  - *Off*: the key does nothing but say so.
  - Choosing an entry plays it. A changed file is read again when *Own
    file* is chosen again (or at the next start).
- **What happens to the own file:** WAV (8/16/24-bit, float), MP3, Ogg
  Vorbis (not Opus) or FLAC, any sample rate from 4 to 192 kHz, mono or up
  to 8 channels, at most 16 MiB. The channels are mixed to mono at
  22050 Hz; silence at the start and the end is cut; the sound is cut to
  2.0 s with a 50 ms fade-out; its loudness is evened out (peak at most
  70 % and RMS at most 20 % of full scale, a quiet file raised at most 8
  times), so no horn is louder than a weapon's sound. A file that cannot
  be read, is silent or is shorter than 50 ms is not used.
- **Who hears it:** everyone near the ship, from where the ship is, as far
  as a weapon's sound carries (256 units). A new horn of the same player
  stops that player's last one.
- **Limits:** at most 3 horns within 2 s and 4 within 10 s; one more locks
  the horn for 5 s ("Horn cooling down (5 s)").
- **Muting:** *Hear other players' horns* (same menu) turns all others
  off; in the chat (F8), `/mute name` mutes one player (the start of the
  callsign is enough), `/unmute name` undoes it, `/mute` lists the muted.
  Muting lasts until the game is closed and holds across a rejoin under
  the same callsign.
- **Bots:** *Bots taunt after kills* on the host's Bots screens (setup
  and in game, saved with *Save as default setup*; default off). A bot
  that killed a player sounds a starter horn about every other time,
  within the same limits.
- Taunts need SDL_mixer (every release build has it); with `-nosdlmixer`
  nothing plays.

## 2. Design

- `common/include/taunt_sample.h`, `common/misc/taunt_sample.cpp`: the
  limits (one place), resampling, trimming, fade, loudness, the transfer
  format, the starter horns (synthesised in code, our own work, CC0:
  every machine makes the same ones), the rate limiter, the messages,
  `/mute` parsing. No game dependencies; tested by `test-taunt`.
- `common/misc/taunt_decode.cpp`: decoding of the own file with vendored
  single-file decoders (`contrib/audio`, public domain / MIT-0 / MIT):
  the same in every build, whatever codecs the platform's SDL_mixer has.
  Only the local file is decoded; peers never decode MP3/Ogg/FLAC.
- `similar/main/taunt.cpp`: the key, playback, network, bots, muting.
  Playback uses one mixer channel per player (`digi_play_custom`, 9
  slots: 8 players and the menu's preview) with the volume and pan of a
  sound linked to the ship (`digi_sound_location`), updated every frame;
  the horn stops when the ship dies or leaves.
- **Transfer format "DXT1":** magic, rate u16 = 22050, flags u16 = 0,
  count u32 (1 102 to 44 100), then mono int16 samples, little-endian:
  at most 88 212 bytes (≤ 100 KB, uncompressed so that it needs no
  decoder). A receiver checks every field and the length, then cuts, fades
  and normalises again. The id of a sample is the CRC-32 of these bytes.
- **Network** (Documentation/network-protocol-v2.md §6.9a): a client sends
  `TAUNT_REQUEST` (sample kind, id, size); the host checks the rate,
  plays it and relays `TAUNT` (pid, kind, id, size) to all others. The
  host's own and its bots' taunts go out as `TAUNT` directly.
- **Spam protection:** sliding windows over the last taunt times
  (`rate_limiter`): 3 in 2 s, 4 in 10 s, 5 s lockout at the sender; the
  host counts the same over windows 300 ms shorter (1.7 s, 9.7 s), so the
  jitter between sender and host does not make it refuse what the sender
  played; receivers allow one more in each window, since the network may
  bunch what the host allowed. The limits start afresh at every level, and
  the samples are prepared then (no hitch at the first horn).
- **Recording:** a `level_event` of kind 6 (movement recording minor 7,
  Documentation/movement-recording.md) for every taunt a machine accepts:
  pid, sample kind, flags 1 if not played here.
- **Key bindings in old pilot files:** the binding arrays of a `.plr` are
  fixed-size; the new slots held 0 (button 0 on a joystick or the mouse)
  in files written before. A pilot whose `.plx` lacks `tauntbindings=1`
  gets the taunt bindings reset to the defaults once.

## 3. Phase 2: the own samples to the other players

The custom ships (stage S3) bring a generic host-relayed, chunked,
SHA-256-checked asset transfer with a cache: `ASSET_REQUEST` 0x4C,
`ASSET_DATA` 0x4D, `ASSET_UNAVAILABLE` 0x4E, each with a kind u8 and the
32-byte SHA-256 (`common/main/net_v2_ships.h`). Kind 2 is reserved for
taunt samples (at most 128 KiB; a DXT1 sample is at most 88 212 bytes).
Phase 2 names a player's own sample by its SHA-256 instead of the CRC-32
and fetches missing samples with kind 2, under the same caching rules; a
peer that lacks a sample plays Horn 1 until it arrives. The protocol
version is bumped again for it (exp-visuals numbers its protocols
0x7000 + n).

## 4. Tests

- `test-taunt` (run from the top of the tree): the rate limiter (bursts,
  lockout, steady taunting, the host's allowed sequences bunched by the
  network pass the receivers), WAV in four sample formats and five rates,
  the Ogg/MP3/FLAC files of `common/unittest/data` (made by
  `make_taunt_files.sh`, CC0), trimming, cut and fade, loudness limits,
  hostile input (truncated and bit-flipped files of every format; the
  stb_vorbis fixes of `contrib/audio/README.md` came from this), the
  transfer format and its broken variants, the starter horns, the
  messages, `/mute`.
- `test-movement-record`, `test-bot-presets`: the new event kind, the
  `BotTaunt` profile line.
- Arena: `tools/botarena-run.sh -x -botarena-taunt ...` records the bots'
  taunts (`movrec-dump` lists them as level events).
