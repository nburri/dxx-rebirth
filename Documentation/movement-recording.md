# Movement recording (player styles for bots, step 1)

Status: step 1 (recording) implemented on branch `movement-recording`
(based on `experimental-netcode`). Steps 2 and 3 are planned below.

## 1. Goal

Bots (Documentation/multiplayer-bots.md) should be able to fly "in the
style of" a real player: strafe like them, reverse-thrust through turns like
them, hold the distance they like to fight at, burn the afterburner when they
do, dodge and retreat like them. That takes three steps:

1. **Record** (this document, implemented): the game writes, for every human
   player, how the ship moves, what the pilot does with the controls, and the
   fight situation it happens in, at a fixed rate, plus events.
2. **Analyse** (offline tool, planned): read many recordings of a player and
   reduce them to a small set of movement statistics.
3. **Profiles** (in game, planned): turn those statistics into bot style
   parameters, selectable in the bot setup as "<player> style".

The recording is designed so that step 2 needs nothing but the files and the
reader library (`common/main/movement_record_reader.h`), and so that the
format can grow without breaking old files or old readers.

## 2. Enabling

Command line or `d2x.ini` (one option per line):

| Option | Meaning |
|---|---|
| `-recordmoves` | Record. Off by default. |
| `-recordmoves-rate <n>` | Samples per second, 10 to 60 (default 30). |
| `-recordmoves-bots` | Also record the bots (default: humans only). |

There is no menu toggle (the setting is read once at start, like
`-auto-record-demo`); adding `-recordmoves` to `d2x.ini` makes it permanent.

**Where the files go.** One file per game session (from entering the game to
leaving it; all its levels are in the same file) in the folder `recordings/`
of the PhysFS write directory, which is the folder that holds
`gamelog.txt` and the pilot files:

- Linux: `~/.d2x-rebirth/recordings/`
- macOS: `~/Library/Preferences/D2X Rebirth/recordings/`
- Windows: the game's folder (or the user folder PhysFS chose;
  `gamelog.txt` is next to it), `recordings\`

The name is `moves-YYYYMMDD-HHMMSS.dmr` (local time of the start; a suffix
`-1`, `-2` ... if that name exists). The console and `gamelog.txt` show
`movement recording: <file>, 30 samples per second` at the start and
`movement recording: closed, <n> bytes` at the end.

**Who records what.** The **host** of a network game is the right machine to
record: it receives every client's ship state every tick and holds every
player's pose in one consistent world (Documentation/network-protocol-v2.md
§2.4, §5.3). A client with `-recordmoves` records too, from its own view: its
own ship exactly, the others as its interpolation shows them. In single
player and in a game against bots only, the local player is recorded (and the
bots with `-recordmoves-bots`).

## 3. What is recorded

### 3.1 Samples

Every recording tick (default 30 per second of game time), one `tick` record
and then one `sample` per recorded player:

| Field | Content | Resolution |
|---|---|---|
| player | player number | |
| flags | alive, dying, controls present, afterburner, fire primary held, fire secondary held, cloaked, invulnerable | |
| flags2 | shields/energy exact here, afterburner known, bot, flown here, steering a guided missile, headlight, fire buttons known | |
| segment | segment the ship is in | |
| position | world, x y z | 1/256 unit |
| orientation | unit quaternion (w ≥ 0) | 1/32767 |
| velocity | world frame | 1/64 unit/s |
| rotational velocity | ship frame (pitch, heading, bank) | 1/4096 rev/s |
| weapons | selected primary and secondary | |
| shields, energy | whole units (rounded up), 0 to 255 | 1 |
| attacked mask | players whose shots hit this ship in the last 2 s | |
| aimed-at mask | enemy players now within 15° of their nose pointing at this ship, within 800 units, with a line of sight | |
| enemy | the nearest enemy (player; or robot in single player, coop and robot games) with a line of sight; if none has one, the nearest: kind, number, line of sight, in my 30° cone, me in its 30° cone, cloaked | |
| enemy relative position | enemy minus me, world frame | 1/16 unit |
| enemy relative velocity | enemy minus me, world frame | 1/64 unit/s |
| controls (optional) | forward, sideways, vertical thrust; pitch, heading, bank: 1.0 = full deflection, forward up to 2.0 with the afterburner | 1/60 |

**Controls.** The controls are the thrust and rotational thrust the ship was
given in the frame of the sample (`apply_pilot_controls`), normalised to the
ship's maximum; they are *exact for every ship flown on the recording
machine* (the local player, and the host's bots) and include keyboard, mouse
and joystick alike. For ships flown elsewhere (the clients, on the host) the
network does not carry controls; their samples have no controls field, and
step 2 estimates them from the motion (§8.1). A client that records itself
(`-recordmoves` on the client) records its own controls exactly.

**Afterburner.** Exact for ships flown here (the forward thrust exceeds 1);
for clients the host reads bit 1 of the client's `INPUT` chunk (§5.3 of the
protocol), which clients of this version now set while the afterburner
pushes. `afterburner known` is clear when neither source is available (an
older client).

**Shields and energy** are exact for ships flown here (`vitals exact`). Until
stage 4 of the network protocol (host-side damage) the host's copy of a
client's shields is only what the v1 messages bring; the flag tells.

### 3.2 Events

Written when they happen, each with its game time in milliseconds:

| Event | pid | other | kind | id | value | flags |
|---|---|---|---|---|---|---|
| `fire` | shooter | | 0 primary, 1 secondary | weapon index | | weapon flags (quad, spread toggle, helix) |
| `hit` | victim | attacker player or 255 | attacker kind: 1 player, 2 robot, 3 other | weapon id (`Weapon_info` index) | damage, 1/256 shield | bit 0: this machine applies the damage |
| `kill` | victim | killer player or 255 | killer kind | | | |
| `death` | player | | | | segment | |
| `respawn` | player | | | | segment | |
| `pickup` | player | | | powerup id | | |
| `weapon` | player | | | new primary | new secondary | |
| `end` | | | 0 closed by the game, 1 size limit | | | |

Sources: `fire` from `do_laser_firing_player` and `do_missile_firing` (the
local player and bots) and from `MULTI_FIRE` (remote players; flares are not
recorded); `hit` from `collide_player_and_weapon` on the recording machine
(direct hits; splash damage of missiles and mines is not a separate event);
`kill` from `multi_compute_kill` (network games); `death`, `respawn` and
`weapon` from the change between two samples; `pickup` from `do_powerup`
(local player) and from the host's pickup grants (clients).

`level` and `player` records describe the context: a `level` record at the
start of every level (number, name, mission, segment count, game mode) and a
`player` record for every slot at the level start and whenever a slot's
callsign, team or flags (connected, bot, flown here, recorded) change.

## 4. File format

All integers little-endian. Version 1.

```
file    = header chunk*
header  = magic "DXXMOVES" (8)
          version u16 | header_size u16 (whole header, CRC included)
          tick_rate u16 | flags u16 (1 multiplayer, 2 host, 4 bots recorded)
          start_time i64 (Unix seconds) | game_mode u32 | local_player u8
          program str8 | mission str8 | level_name str8 | level_num i8
          player_count u8, player_count × { pid u8, flags u8, team u8, callsign str8 }
          crc32 u32 (of all header bytes before it)
chunk   = magic "MRCK" (u32 0x4b43524d) | sequence u32 | payload_size u32 | crc32 u32 (of payload)
          payload: whole records, at most 32752 bytes
record  = type u8 | size u8 | payload (size bytes)
str8    = length u8 | bytes (UTF-8 as the game has them)
```

Record payloads (sizes without the 2 byte record header):

| Type | Payload |
|---|---|
| 1 `level` | level_num i8, segments u16, game_mode u32, mission str8, level_name str8 |
| 2 `player` | pid u8, flags u8 (1 connected, 2 bot, 4 flown here, 8 recorded), team u8 (255 no teams), callsign str8 |
| 3 `tick` (8) | tick u32 (counts from 0 at the session start at `tick_rate`), time_ms u32 (game time since the session start) |
| 4 `sample` (54, 60 with controls) | pid u8, flags u8, flags2 u8, segment u16, position 3 × i24, quaternion 4 × i16, velocity 3 × i16, rotvel 3 × i16, weapons u8, shields u8, energy u8, attacked u8, aimed_at u8, context u8, enemy_id u16, enemy_rel_pos 3 × i16, enemy_rel_vel 3 × i16, [controls 6 × i8] |
| 5–12 events (11) | time_ms u32, pid u8, other u8, kind u8, id u8, value u16, flags u8 |

The bit assignments are in `common/main/movement_record_format.h`
(`sample_flag`, `sample_flag2`, `context_flag`, ...), which is the normative
definition.

**Compatibility rules.** A reader skips record types it does not know (by the
size byte) and ignores bytes after the fields it knows in a record, so a
later version may add record types and append fields to records. A change of
the meaning or order of existing fields increments `version`.

**Robustness.** Records are collected in a chunk in memory and written as one
unit with its CRC once per second of game time (or when the 32 KiB chunk is
full), then flushed to the operating system. A crash or power loss therefore
loses at most the last second. The reader checks every chunk's CRC; a file
that ends inside a chunk is reported as "cut short" and everything before is
used; a damaged chunk is skipped and the reader resumes at the next chunk
magic; missing chunks show as gaps in the sequence numbers. A file closed by
the game ends with an `end` record.

**Size.** About 62 bytes per player sample with controls, 56 without, plus
10 bytes per tick and 13 per event. At the default 30 Hz that is
**about 1.1 MB per recorded player per 10 minutes** (a 2 player game: about
2.3 MB per 10 minutes; 4 players: about 4.5 MB; 60 Hz doubles it). The
recording stops at 64 MiB per file (an `end` record with reason 1 and a
console message): about 80 minutes of an 8 player game at 30 Hz, 5 hours of a 2 player game.

**Cost.** Nothing is done off the recording tick except a time check: the
work is per tick, not per frame, so it does not grow with the frame rate
(500 fps costs the same as 60). Per tick and recorded player: one sample
encoded into a stack buffer and copied into the static chunk buffer (no
allocation), a distance scan over the enemies, and at most a few line of
sight tests (`find_vector_intersection`), cached per pair of players and
tick: at most 4 for the nearest enemies plus the "aimed at" tests, which only
run for ships whose nose points at the player. One file write per second.

## 5. Reader library and dump tool

`common/main/movement_record_reader.h` (header-only, standard C++ only):

- `read_recording(bytes, callback)`: the header, then every record in file
  order as a `std::variant<level_record, player_record, tick_record, sample,
  event_record>`; returns the header and the file's health (`read_stats`:
  good and damaged chunks, gaps, unknown and malformed records, cut short,
  closed).
- `to_units(sample)`: game units, the ship's axes, the velocity and the
  enemy's relative position in the ship's frame (right, up, forward),
  distance, closing speed, angle off the nose, controls as fractions.
- CSV writers for samples and events.

`movrec-dump` (a separate program, not part of the game):

```
scons sdl2=1 d1x=0 d2x=1 register_runtime_test_plain_link_targets=1 movrec-dump
build/common/movrec-dump [--csv DIR] [--player N] [--records] FILE...
```

Without options it prints the header, the file's health, the levels, and per
player: time alive, mean speed, share of time reversing / strafing /
climbing, afterburner share, turning hard and the share of that with reverse
thrust (players with controls), enemy in sight and mean distance, under
attack, aimed at, shots, hits dealt and taken with damage, kills, deaths,
suicides, respawns, pickups, weapon switches. `--csv DIR` writes
`DIR/<file>-p<N>-<callsign>.csv` (one row per sample, in game units and in
the ship's frame) and `DIR/<file>-events.csv`, ready for a spreadsheet,
Python or R. `--records` prints every record.

## 6. Privacy

A recording contains the callsigns of everyone in the game, when they
played, and how they flew; nothing else (no chat, no addresses, no system
data). It is written only on a machine where `-recordmoves` is set, and it
never leaves that machine by itself. A host that records should tell the
players (for example in the game name or the chat). Anyone can be left out
of an analysis with `movrec-dump --player`, and a recording can simply be
deleted.

## 7. Sending recordings

Zip the `.dmr` files from the `recordings/` folder (§2) and send them, or
attach them to the playtest issue or pull request. Files cut short by a crash
are still useful; there is no need to repair them. Mention which callsign is
you, which machine was the host, and the game mode.

## 8. Steps 2 and 3 (planned)

### 8.1 Step 2: analysis tool

A second program next to `movrec-dump` (`movrec-analyse`), built on the
reader library, reads any number of recordings, groups the samples by
callsign (all files, all levels) and writes one profile per player as a small
text file (`<callsign>.botstyle`, key = value). Statistics, each over the
samples in the matching situation:

- **Strafing**: share of time with |sideways velocity or thrust| above a
  threshold while an enemy is in sight; mean duration of a strafe direction
  (sign changes of the sideways component); vertical share (up/down against
  left/right). Maps to the bots' `strafe`, `strafe_min_ms`/`strafe_max_ms`,
  `strafe_vertical`, `strafe_speed`.
- **Reverse thrust during turns**: when the heading/pitch rate is high, the
  distribution of the forward thrust (or forward velocity); "turns backing
  off" against "turns pushing in".
- **Afterburner**: share of time, and when: chasing (enemy ahead, distance
  growing), fleeing (enemy behind, low shields), crossing the level (no enemy
  in sight). Maps to `burn_chase_distance` and new burn triggers.
- **Preferred fight distance**: distribution of the distance to the enemy in
  sight while firing, per weapon class. Maps to `range_scale`.
- **Dodging**: after an enemy fires (fire events of the enemy in sight, or
  `aimed at` set), the lateral acceleration in the next 0.5 s against the
  baseline. Maps to `dodge_prob`/`dodge_bonus`.
- **Retreat**: shields at which the player turns away from an enemy in sight
  (closing speed negative, enemy leaves the forward cone) and how long.
  Maps to `retreat_shields`, `outgunned_retreat`, `chase_memory`.
- **Aggression**: engage share (time closing in with an enemy in sight),
  chase duration after losing sight, pickups while enemies are near. Maps to
  `engage_weight`, `collect_weight`.

For samples without controls (clients recorded on the host) the analysis
estimates them from the motion: the ship physics is known (`Player_ship`:
mass, drag, maximum thrust and rotational thrust; `do_physics_sim`), so the
thrust between two samples is `mass × (Δv/Δt + drag term × v)` in the ship's
frame, and the rotational thrust likewise from the rotational velocity.
Recordings of ships flown on the recording machine carry both the exact
controls and the motion, and are the test set for this estimator.

### 8.2 Step 3: bot profiles

The bots' behaviour is already parameterised (`skill_params` for skill,
`style_params` for style, `common/main/bot_brain.h`). A profile sets a
`style_params` (and the movement fields of `skill_params`) from a
`.botstyle` file, clamped to the ranges the built-in styles use, while the
skill still decides aim and reaction. The bot setup menu lists the profiles
found in `botstyles/` next to the built-in styles as "<player> style"; the
profile's name travels with the bot's configuration like the built-in style
(Documentation/multiplayer-bots.md section 9.7).

## 9. Code

| File | Content |
|---|---|
| `common/main/movement_record_format.h` | Format: constants, records, encode/decode, quantisation, CRC, chunk builder, tick schedule (standard C++ only) |
| `common/main/movement_record_reader.h` | Reader library (header-only) |
| `common/main/movement_record.h`, `similar/main/movement_record.cpp` | The game's side: session file, sampling, context, event hooks |
| `common/tools/movrec_dump.cpp` | The dump tool |
| `common/unittest/movement_record.cpp` | Tests: round trips, header, chunks, truncation at every byte, damaged chunks, unknown records, the tick schedule at 20 to 1000 fps, quantisation and frames |

Hooks in the game (one call each): `GameProcessFrame` (sample, after the
bots fired), the game window's close (end of the session),
`do_laser_firing_player` and `do_missile_firing` (fire), `multi_do_fire`
(remote fire), `collide_player_and_weapon` (hit), `multi_compute_kill`
(kill), `do_powerup` and the host's pickup grant log (pickup). `net_v2.cpp`:
clients set the afterburner bit of `INPUT`; `host_input_afterburner` reads
it on the host.

The recording tick is its own schedule of game time (`tick_scheduler`): tick
`k` is due `k / rate` seconds of game time after the session start; a frame
samples at most once, a frame longer than a tick skips ticks instead of
writing duplicates, and pauses (no game frames) do not advance it.
