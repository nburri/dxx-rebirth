# Taunts (horn)

Status: on `experimental-netcode`, protocol 113 (first built on a side
branch in two phases: everything but sending the players' own samples,
then the samples travelling with the custom ships' asset transfer).

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
  2.0 s with a 50 ms fade-out; its loudness is evened out to 40 % of full
  scale (RMS over the audible part; a quiet file is raised at most 16
  times), with a soft limiter that keeps the peaks under 95 % instead of
  clipping them. The starter horns get the same. A file that cannot be
  read, is silent or is shorter than 50 ms is not used.
- **How loud:** clearly over gunfire: about 1.6 times as loud as a laser
  shot (+7 dB; see 2. Design). *Horn volume* (same menu, 0 to 200 %,
  default 100 %, saved in `descent.cfg` as `HornVolume`) scales all horns
  you hear, your own and the others', on top of the sound effects
  volume; moving it plays your horn.
- **Who hears it:** everyone near the ship, from where the ship is: at
  full volume within 80 units (along the way through the mine), fading
  to nothing at 400 units (a weapon's sound fades from its source and is
  gone at 320). Your own horn sounds at full volume. A new horn of the
  same player stops that player's last one.
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
  slots: 8 players and the menu's preview); these are the mixer's
  channels 0 to 8, which no game sound takes, so a horn is never dropped
  when many sounds play. Volume and pan follow the ship every frame
  (`horn_location`: `digi_sound_location` asked for 320 units, its
  linear fade turned into full within 80 and nothing at 400); the horn
  stops when the ship dies or leaves.
- **Loudness:** the game's weapon sounds (8-bit, `DESCENT2.S22`) peak at
  full scale; their loudness (RMS over 50 ms windows, without those 20 dB
  below the loudest) is 0.25 to 0.29 for the lasers (`laser01`-`05`),
  0.33 for the plasma, 0.42 for the vulcan (your own vulcan plays at
  half volume). They play at the mixer channel volume of the sound
  effects setting, which is at most half of SDL_mixer's range (64 of
  128). A horn is normalised to 0.40 (`taunt::normalise`: gain, then
  `soft_limit`, unchanged up to 0.70 and bent towards 0.95; the gain is
  found again from the original until the limited sound is on target; at
  most 16 times, and peaks are squashed at most 2 times, so a spiky file
  stays quieter) and its channel plays 1.5 times as loud as a game sound
  (`taunt::MIX_GAIN`) times *Horn volume*: 0.40 × 1.5 = 0.60 against
  0.25-0.29, +6.3 to +7.6 dB, about 1.6 times as loud to the ear. The part
  of the gain the channel volume cannot give (*Horn volume* over 133 %
  at full sound effects volume) is applied to the samples with the same
  soft limiter. Before, a horn was at most 0.20 RMS with peaks at 0.70
  (3 to 6 dB under a laser shot).
- **Received samples:** a receiver normalises a sample again, but only
  lowers it (the sender's gain and limiting must not apply twice: a
  quiet file raised 16 times would otherwise be raised again), so
  everyone hears the owner's horn as the owner does. The transfer format
  is unchanged: an own sample from an older version (at most 0.20 RMS)
  stays at its old level; an older receiver lowers a new one to its old
  level. The starter horns are made on every machine at the new level.
- **Transfer format "DXT1":** magic, rate u16 = 22050, flags u16 = 0,
  count u32 (1 102 to 44 100), then mono int16 samples, little-endian:
  at most 88 212 bytes (≤ 100 KB, uncompressed so that it needs no
  decoder). A receiver checks every field and the length, then cuts, fades
  and normalises again. The id of a sample is the SHA-256 of these bytes.
- **Network** (Documentation/network-protocol-v2.md §6.9a): a client sends
  `TAUNT_REQUEST` (sample kind, size, SHA-256); the host checks the rate,
  plays it and relays `TAUNT` (pid, kind, size, SHA-256) to all others. The
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

The custom ships (stage S3) brought a generic host-relayed, chunked,
SHA-256-checked asset transfer: `ASSET_REQUEST` 0x4C, `ASSET_DATA` 0x4D,
`ASSET_UNAVAILABLE` 0x4E, each with a kind u8 and the 32-byte SHA-256
(`common/main/net_v2_ships.h`, `similar/main/net_ships.cpp`). The taunts
are kind 2 (at most 128 KiB; a DXT1 sample is at most 88 212 bytes):

- `TAUNT_REQUEST` and `TAUNT` name an own sample by its SHA-256 and size.
- The host learns from a `TAUNT_REQUEST` that the sender owns the sample
  and fetches it for itself (unless it does not play that player's
  taunts); a client that hears a `TAUNT` for a sample it lacks asks the
  host, which serves its copy or fetches it from the owner. Players never
  talk to each other.
- Until it arrives, the horn sounds as Horn 1; the next taunt plays the
  own sample. A transfer is paced like the ships' (16 KiB/s in a level):
  a sample of 2 s takes about 5 s.
- A received sample is checked (SHA-256, every field of the format, the
  limits applied again) and kept in `taunts/cache/<sha256>.dxt` in the
  user folder; at most 64 are kept, the oldest go (16 are kept decoded
  in memory). A player who uses the same file as you is heard with it at
  once. A failed transfer is tried again at the player's next taunt (from
  another owner, if one is known).
- Muted players' samples and, with "Hear other players' horns" off, all
  samples are not fetched. "Accept ships from the host" and "Show custom
  ships" do not apply to taunts (the asset exchange's `accept` covers
  kind 1 only).

## 4. Tests

- `test-taunt` (run from the top of the tree): the rate limiter (bursts,
  lockout, steady taunting, the host's allowed sequences bunched by the
  network pass the receivers), the normalisation (quiet, very quiet,
  normal, loud, clipped, square and spiky input: loudness on the target
  or stopped by the gain limits, peaks under 95 %, normalising again
  changes nothing, a received sample is never raised, the limiter's
  curve), WAV in four sample formats and five rates,
  the Ogg/MP3/FLAC files of `common/unittest/data` (made by
  `make_taunt_files.sh`, CC0), trimming, cut and fade, loudness limits,
  hostile input (truncated and bit-flipped files of every format; the
  stb_vorbis fixes of `contrib/audio/README.md` came from this), the
  transfer format and its broken variants, the starter horns, the
  messages, `/mute`.
- `test-movement-record`, `test-bot-presets`: the new event kind, the
  `BotTaunt` profile line.
- `test-net-v2-ships`: a taunt sample (kind 2) fetched by the host from
  its owner once and served to a client that refuses ships; an oversized
  one never asked for.
- Not done: a two-instance network test (the game has no unattended
  client join).
- Arena: `tools/botarena-run.sh -x -botarena-taunt ...` records the bots'
  taunts (`movrec-dump` lists them as level events).
