# Movement recording (player styles for bots, steps 1 and 2)

Status: step 1 (recording), step 2 (the analysis tool and the bot style
profile format, section 8) and step 3 (bots that fly a profile, section
8.7) are implemented on `experimental-netcode`.

## 1. Goal

Bots (Documentation/multiplayer-bots.md) should be able to fly "in the
style of" a real player: strafe like them, reverse-thrust through turns like
them, hold the distance they like to fight at, burn the afterburner when they
do, dodge and retreat like them. That takes three steps:

1. **Record** (this document, implemented): the game writes, for every human
   player, how the ship moves, what the pilot does with the controls, and the
   fight situation it happens in, at a fixed rate, plus events.
2. **Analyse** (offline tool `movrec-analyse`, section 8, implemented): read
   many recordings of a player, reduce them to a movement profile, and
   propose the bot parameters that fit it as a `.botstyle` text file.
3. **Profiles** (in game, implemented): load those files, selectable in
   the bot setup as "<player> style".

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
| `-sharemoves` | Record (implies `-recordmoves`), and as a client also send your exact controls to the host, so that the host's recording has them (section 3.1). |

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
| flags2 | shields/energy exact here, afterburner known, bot, flown here, steering a guided missile, headlight, fire buttons known, controls shared (minor 2) | |
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
| enemy relative position | enemy minus me, world frame; beyond 2048 units the whole vector is scaled down to fit, keeping its direction, and the context says so (`rel_pos_scaled`) | 1/16 unit |
| enemy relative velocity | enemy minus me, world frame; scaled likewise beyond 512 units/s (`rel_vel_scaled`) | 1/64 unit/s |
| controls (optional) | forward, sideways, vertical thrust; pitch, heading, bank: 1.0 = full deflection, forward up to 2.0 with the afterburner | 1/60 |

**Controls.** The controls are the thrust and rotational thrust the ship was
given in the frame of the sample (`apply_pilot_controls`), normalised to the
ship's maximum; they are *exact for every ship flown on the recording
machine* (the local player, and the host's bots) and include keyboard, mouse
and joystick alike. A client that records itself (`-recordmoves` on the
client) records its own controls exactly.

For ships flown elsewhere (the clients, on the host) the controls are only
known when the client shares them: a client started with `-sharemoves`
appends its controls of the frame to every `INPUT` chunk it sends the host
(6 bytes, the same values and units as the recording; protocol 107,
Documentation/network-protocol-v2.md §5.3). The host records them for that
client's samples, marked `controls shared` (flags2 bit 7), and takes the
afterburner from them as for a ship flown here. The host uses them for
nothing else: they never reach the flight, the checks or other players. It
clamps them to what a pilot can give (forward −1 to 2, the other axes −1
to 1) and uses only controls that arrived within the last 100 ms; in a gap
(lost packets, a guided missile being steered, dead) the sample has no
controls and step 2 estimates them from the motion (§8.3), as it does for
every client without `-sharemoves`. Without the switch a client sends
nothing extra: sharing is each player's own choice. The host marks a
client that shares in its `player` record (flag 16) from the first
controls that arrive, and a sharing client marks itself in its own file.
`movrec-dump` prints per player how many samples have controls and how
many of them were shared.

**Afterburner.** Exact for ships flown here and for shared controls (the
forward thrust exceeds 1); for other clients the host reads bit 1 of the client's `INPUT` chunk (§5.3 of the
protocol), which clients since v0.61-exp-25 set while the afterburner
pushes. Older clients always send 0 there, which would read as "never
burns"; so a client's afterburner is `known` only from the first time its
bit was 1 (a per-player note in the recorder, without a protocol change).
Before that, and for a client that never burns, `afterburner known` is
clear, and the analysis estimates the afterburner from the thrust (section
8.3).

**Shields and energy** are exact for ships flown here (`vitals exact`). Until
stage 4 of the network protocol (host-side damage) the host's copy of a
client's shields is only what the v1 messages bring; the flag tells.

### 3.2 Events

Written when they happen, each with its game time in milliseconds:

| Event | pid | other | kind | id | value | flags |
|---|---|---|---|---|---|---|
| `fire` | shooter | | 0 primary, 1 secondary | weapon index | | primary: weapon flags (quad, spread toggle, helix); secondary: 0 |
| `hit` | victim | attacker player or 255 | attacker kind: 1 player, 2 robot, 3 other | weapon id (`Weapon_info` index; 255: the blast of no weapon) | damage, 1/256 shield | bit 0: this machine applies the damage; bit 1: splash damage |
| `kill` | victim | killer player or 255 | killer kind | | | |
| `death` | player | | | | segment | |
| `respawn` | player | | | | segment | |
| `pickup` | player | | | powerup id | | |
| `weapon` | player | | | new primary | new secondary | |
| `end` | | | 0 closed by the game, 1 size limit | | | |

Sources: `fire` from `do_laser_firing_player` and `do_missile_firing` (the
local player and bots) and from `FIRE` (remote players, acted out by
`multi_do_fire`; flares are not
recorded; a missile's `FIRE` flags are its gun and a guided missile's
generation, which mean nothing to the analysis, so missiles are recorded
with flags 0 from both sources); `hit` from `collide_player_and_weapon`
(direct hits) and from the blast in `object_create_explosion_with_damage`
(splash damage of `explode_badass_weapon`: mega, smart, earthshaker, mines,
and of a ship or robot that blows up; flag bit 1, with the attacker, the
weapon that exploded and the damage after the distance falloff) on the
recording machine; a player's own blast is recorded with itself as the
attacker but does not count as "attacked by" in the samples. In a network
game (protocol v2 stage 4) the host decides the damage, so `hit` comes
from its decision instead: on the host where it applies the damage, on a
client where its `DAMAGE` arrives (for every victim; bit 0 on the
victim's machine), with the host's amount, so a collision the host
refused is not recorded and a hit the shooter saw but the victim did not
is. Damage reported by the victim's machine without a player's weapon is
recorded only when a robot, the reactor or a robot's mine did it (weapon
255); walls, lava, bumps and the fusion overcharge are no hits, as before.
`kill` from `multi_compute_kill` (network games); `death`, `respawn` and
`weapon` from the change between two samples; `pickup` from `do_powerup`
(local player) and from the host's pickup grants (clients).
`fire` and `kill` are recorded for every player, also for one whose
samples are not (a bot without `-recordmoves-bots`): the analysis of a
recorded player needs its enemies' shots (dodging) and deaths. `hit` is
recorded when its victim or its attacker is recorded; `death`, `respawn`,
`weapon` and `pickup` only for recorded players.

**A slot that changes hands.** When another player (another callsign, a bot
in place of a human, or a player who left and came back) takes a slot, the
recorder forgets what it knew of the slot: whether the ship lived, its
weapons, who hit it and whom it hit. The newcomer's first sample therefore
gives no `respawn` or `weapon` event and no "attacked by" of the player
before.

**Session clock.** In a network game a `sync` record at every level start
and once per second links the file's time to the host's clock (the host's
own on the host, the client's estimate of it on a client, the same clock the
interpolation uses) and names the network session (`session_id`). With it,
recordings of one game made on several machines can be put on one time line
(section 8.2).

`level` and `player` records describe the context: a `level` record at the
start of every level (number, name, mission, segment count, game mode) and a
`player` record for every slot at the level start and whenever a slot's
callsign, team or flags (connected, bot, flown here, recorded, shares its
controls) change.

## 4. File format

All integers little-endian. Version 1, minor 2. The minor counts additions
that an older reader skips without harm (new record types, new flag bits,
fields appended to the header); the version changes only when old fields
change. Minor 0 is the first release (v0.61-exp-25); minor 1 adds the
`minor` field itself, the `sync` record, splash hits (`hit` flag bit 1), the
scaled relative vectors (context bits 6 and 7) and the rules of section 3
for the afterburner, missile flags and slots. Minor 2 adds the shared
controls (`-sharemoves`): sample flags2 bit 7 `controls shared` and player
flag 16 `shares controls`; the layout is unchanged (a shared sample is a
sample with controls), so a minor 1 reader reads a minor 2 file and takes
the shared controls for exact ones, which they are. A minor 0 file reads as
before (its header has no `minor` field: 0).

```
file    = header chunk*
header  = magic "DXXMOVES" (8)
          version u16 | header_size u16 (whole header, CRC included)
          tick_rate u16 | flags u16 (1 multiplayer, 2 host, 4 bots recorded)
          start_time i64 (Unix seconds) | game_mode u32 | local_player u8
          program str8 | mission str8 | level_name str8 | level_num i8
          player_count u8, player_count × { pid u8, flags u8, team u8, callsign str8 }
          minor u16 (absent in minor 0)
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
| 2 `player` | pid u8, flags u8 (1 connected, 2 bot, 4 flown here, 8 recorded, 16 shares its controls (minor 2)), team u8 (255 no teams), callsign str8 |
| 3 `tick` (8) | tick u32 (counts from 0 at the session start at `tick_rate`), time_ms u32 (game time since the session start) |
| 4 `sample` (54, 60 with controls) | pid u8, flags u8, flags2 u8, segment u16, position 3 × i24, quaternion 4 × i16, velocity 3 × i16, rotvel 3 × i16, weapons u8, shields u8, energy u8, attacked u8, aimed_at u8, context u8, enemy_id u16, enemy_rel_pos 3 × i16, enemy_rel_vel 3 × i16, [controls 6 × i8] |
| 5–12 events (11) | time_ms u32, pid u8, other u8, kind u8, id u8, value u16, flags u8 |
| 13 `sync` (17, minor 1) | time_ms u32, session_id u32 (0: no network session), host_ms i64 (the host's clock at `time_ms`, milliseconds), flags u8 (1 the clock is known, 2 this machine is the host) |

The bit assignments are in `common/main/movement_record_format.h`
(`sample_flag`, `sample_flag2`, `context_flag`, ...), which is the normative
definition.

**Compatibility rules.** A reader skips record types it does not know (by the
size byte) and ignores bytes after the fields it knows in a record, so a
later version may add record types and append fields to records and to the
header (before its CRC; `header_size` says where that is), and increments
`minor`. A change of the meaning or order of existing fields increments
`version`.

**Robustness.** Records are collected in a chunk in memory and written as one
unit with its CRC once per second of game time (or when the 32 KiB chunk is
full), then flushed to the operating system. A crash or power loss therefore
loses at most the last second. The reader checks every chunk's CRC; a file
that ends inside a chunk is reported as "cut short" and everything before is
used; a damaged chunk is skipped and the reader resumes at the next chunk
magic; missing chunks show as gaps in the sequence numbers. A file closed by
the game ends with an `end` record.

**Size.** About 62 bytes per player sample with controls, 56 without, plus
10 bytes per tick, 13 per event and 19 per second for the `sync` record. At the default 30 Hz that is
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
  event_record, sync_record>`; returns the header and the file's health (`read_stats`:
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
climbing, afterburner share, the samples with controls (flown here, or
shared by the player's machine), turning hard and the share of that with
reverse thrust (players with controls), enemy in sight and mean distance, under
attack, aimed at, shots, hits dealt and taken with damage (and how many of
them splash), kills, deaths, suicides, respawns, pickups, weapon switches;
for a network game the session id and the number of `sync` records. `--csv DIR` writes
`DIR/<file>-p<N>-<callsign>.csv` (one row per sample, in game units and in
the ship's frame; the last column `controls_shared` tells shared controls)
and `DIR/<file>-events.csv`, ready for a spreadsheet,
Python or R. `--records` prints every record.

## 6. Privacy

A recording contains the callsigns of everyone in the game, when they
played, and how they flew; nothing else (no chat, no addresses, no system
data). It is written only on a machine where `-recordmoves` is set, and it
never leaves that machine by itself. A player's exact controls reach
another machine only when that player starts with `-sharemoves`. A host that records should tell the
players (for example in the game name or the chat). Anyone can be left out
of an analysis (`movrec-dump --player`, `movrec-analyse --player`), and a
recording can simply be deleted. A `.botstyle` profile holds a callsign and
numbers about how that player flies; ask before passing one on.

## 7. Sending recordings

Zip the `.dmr` files from the `recordings/` folder (§2) and send them, or
attach them to the playtest issue or pull request. Files cut short by a crash
are still useful; there is no need to repair them. Mention which callsign is
you, which machine was the host, and the game mode.

## 8. Step 2: analysis and bot style profiles

### 8.1 The tool

`movrec-analyse` (a separate program like `movrec-dump`, not part of the
game) reads any number of recordings and prints, per player, a report and a
proposed bot style:

```
scons sdl2=1 d1x=0 d2x=1 register_runtime_test_plain_link_targets=1 movrec-analyse
build/common/movrec-analyse [--out DIR] [--player CALLSIGN]... [--bots]
                            [--skill NAME] [--min-seconds N] FILE...
```

| Option | Meaning |
|---|---|
| `--out DIR` | Write `DIR/<callsign>.botstyle` (the profile) and `DIR/<callsign>.report.txt` per player. Without it the profile is printed after the report. |
| `--player CALLSIGN` | Only this player (may be repeated). |
| `--bots` | Also the recorded bots (`-recordmoves-bots`); their files end in `-bot`. |
| `--skill NAME` | The skill the profile's skill-relative values are scaled for (Trainee … Insane; default Hotshot). |
| `--min-seconds N` | Skip players alive for less than N seconds (default 20). |

Give it every recording you have of a player: all files of all evenings, the
host's and the clients'. It first lists the files (format, length, host or
client, cut short or damaged) and the games it found in them, then per player
(most time alive first):

- **data**: games, minutes recorded, alive and in fights, kills and deaths,
  how much of the controls is exact (and how much of that was shared by the
  player's machine over the network) and how much estimated;
- **Traits**: the key habits in plain words with the numbers that carry them
  ("Heavy strafer: sideways or vertical thrust in 84% of the fight time, a
  run in one direction lasts 0.5 s …");
- **Numbers**: every statistic of section 8.4;
- **Proposed bot style**: the values of section 8.5 with their confidence.

To try it without a recording of your own, `build/common/test-movement-analysis
-w DIR` writes four synthetic recordings (section 8.6) into `DIR`.

The work is done by `common/main/movement_analysis.h` (header-only, standard
C++ plus the bots' pure headers for their constants), in stages that can be
used one by one: `load_recording` → `group_sessions` → `merge_session` →
`build_track` → `analyse` → `propose_profile` → `write_report`;
`analyse_recordings` runs them all.

### 8.2 Several recordings of one game

A game recorded on several machines gives several files with the same
players. They are put together so that every moment of every player counts
once, from the best source:

1. **Same game?** Files with the same `session_id` (`sync` records, format
   minor 1) are one game. Files without one (minor 0, or no network clock
   yet) are taken for the same game if they are multiplayer recordings of the
   same mission, started within half an hour of each other, with at least one
   callsign in common and not both by a host.
2. **One clock.** With `sync` records every file's time maps to the host's
   clock (piecewise: the file's time stands still between two levels, the
   host's clock does not). Without them the files are aligned by the path of
   a player both recorded: every position in one file votes for the time
   offsets to the moments the other file has the ship at the same place
   (within 2.5 units), per level; the offset that 70 % of the positions agree
   on wins. That is right to about a tick (each machine sees the other's ship
   a little late). A file that belongs to a game by rule 1 but cannot be
   aligned is analysed as a game of its own, with a note (its players then
   count twice).
3. **Best source per player and moment.** A player's own machine (exact
   controls, exact shields) before the host (one consistent world) before
   another client's view. Where the best source has no samples (it joined
   later, left earlier, crashed), the next one fills in.
4. **Events once.** An event is taken from the file that is the source of
   its player at that moment (`pid`: the shooter, the victim, the one who
   picked up), so a shot recorded on both machines counts once, and a hit
   comes from the victim's machine, which applies the damage.

Players are told apart by callsign (not case sensitive) across slots, levels
and games; a bot and a human of the same name are two players.

### 8.3 Controls: exact or estimated

Samples of ships flown on the recording machine carry the controls, and so
do the host's samples of a client with `-sharemoves` (shared controls count
as exact; the profile notes their share as
`measured.shared_controls_share`). For the others (a client, recorded by the
host only) and for the gaps in the shared controls the analysis estimates
them from the motion. The ship's flight model is known (`physics.cpp`): under a thrust
`c` (a share of the maximum per axis) the velocity goes toward
`c × max_speed` at a fixed rate,

```
v(t + dt) = v(t) × R + c × max_speed × (1 − R),   R = exp(−rate × dt)
```

with `max_speed` 58.5 units/s and `rate` 2.12/s for the Pyro-GX (mass 4, drag
0.033, thrust 7.8), and the same for the rotation (0.41 revolutions/s about
one axis, 5.34/s). Solved for `c` between two samples and turned into the
ship's frame, that is the thrust; it is averaged over three samples, and a
value beyond what any thrust gives (a wall, a blast, a respawn) is dropped.
The afterburner of a client that does not report it is "forward thrust above
1.3".

Every recording with exact controls tests the estimator: the report's
`controls:` line gives the root mean square difference between the estimate
and the recorded controls where both exist (0.02 to 0.09 of full thrust on
the synthetic recordings; with real walls and network jitter expect more).
Values that rest on controls get at most medium confidence when most of them
were estimated.

### 8.4 The movement profile

All shares are of the time in the situation named. "Fight" is an enemy with
a line of sight within 400 units. "Uses a control" is more than 0.3 of full
deflection. The thresholds are the constants of `analysis::limits`.

| Group | Statistic | How |
|---|---|---|
| Speed | mean, p10, median, p90; seconds per 20 units/s; share above 85 % and below 20 % of the top speed | alive samples |
| Thrust | share with forward, reverse, sideways, vertical thrust, none, roll; the same in a fight | samples with controls |
| Strafe | length of a run in one direction (median, quartiles), reversals per minute, vertical share (0 flat, 1 as much up/down as left/right), thrust and speed across | in a fight: a run lasts while the sideways/vertical thrust keeps its direction (less than 90° change) |
| Large turns | how many; share flown with reverse, sideways, forward thrust; time per 180°; rotation rate; backward speed reached; push forward afterwards, with afterburner | runs of rotation above 35 % of the top rate through at least 110° in at most 2.5 s per 180° (the bots' `REVERSE_TURN_START`); the thrust during 40 % or more of the turn names it; the push is a mean forward thrust above 0.6 in the 0.8 s after |
| Afterburner | share of the time; share while chasing (enemy in sight ahead, closing in, nose steady), fleeing (enemy behind, moving away), with no enemy in sight, otherwise; distance to the enemy while chasing with it | samples with the afterburner known (or estimated) |
| Distance | to the enemy in sight: p10 … p90, seconds per band (35, 60, 95, 150, 250: the bots' fight band and weapon bands); distance at each primary shot | |
| Approach and retreat | by shields (25 each): own speed toward the enemy; share closing in, backing off while facing it, flying away turned from it. The **retreat level**: the shields that split "flies away" below from "does not" above most clearly (at least 3 s of fight on each side, a difference of 15 percentage points) | fight samples |
| Dodging | share of the enemy's bursts (first shot after a pause of 1 s, the enemy facing the player within 300 units) followed within 0.7 s by a change of the velocity across the line of fire of 30 % of the top speed; the same share in quiet moments (no shot 1.5 s before to 0.7 s after); the excess is the dodge probability; median time to the start of the sidestep | not measurable when the player moves across in more than half of the quiet moments too (a constant strafer); the report warns when the player took hits from other players but the recordings hold none of their shots (a recording of an older build without the bots' shots) |
| Weapons | primary shots per weapon and range band (< 60, 60–150, > 150, as the bots' weapon table); secondary shots per weapon, distance | fire events with the enemy in sight |
| Missile volleys | volleys (missiles at most 0.7 s apart), size, time between two volleys of one fight | mines are left out |
| Pickups | per minute; share taken off course (the course 1.5 s before pointed more than 40° away from the pickup); share in a fight | |
| Pursuit | how often the player follows an enemy that left its sight, and for how long (until it has not thrust toward it for 1 s, it is in sight again, or another enemy took its place in the record) | losses of sight while not already flying away |
| Hits | dealt (direct, splash), taken, damage, direct hits per primary shot (an estimate of accuracy; a shot of several bolts can hit more than once; the hits are those the victim's or the recording machine saw) | hit events |

### 8.5 The bot style profile

A profile is a text file `<callsign>.botstyle`, one `key = value` per line:

```
# D2X-Rebirth bot style profile
format = 1
name = Nico style
callsign = Nico
source = 3 games, 41.2 min alive, 12.5 min in fights, controls 74% exact
base_skill = Hotshot
base_style = Aggressive

# shields below which the bot retreats
style.retreat_shields = 20
confidence.style.retreat_shields = medium
style.range_scale = 0.62
skill.strafe = 1
skill.strafe_min_ms = 420
tune.reverse_turn = 0.85
measured.turn_180_ms = 1350
```

Rules (`common/main/bot_style_profile.h`: `write_style_profile`,
`parse_style_profile`, `apply_style_profile`):

- Spaces around the key and the value do not count; a line starting with `#`
  and an empty line are skipped; a line that is not understood is skipped.
- `format` must be there and not newer than the reader (1).
- `name` is what the bot setup will show, `callsign` the player, `source`
  free text. `base_skill` is the skill the skill-relative values were scaled
  for. `base_style` is the built-in style nearest to the measured one; the
  bot takes from it every value the profile leaves out.
- `style.<field>`: a field of `style_params`. `skill.<field>`: a movement
  field of `skill_params` (the skill still decides aim, reaction and senses).
  `tune.<name>`: a constant of the bot code that was the same for every
  bot; step 3 made each of them per-bot (§8.7). `measured.<name>`:
  plain statistics for people, not read by the game.
- `confidence.<key>` is `low`, `medium` or `high` (high if absent). The
  loader goes the share 0.25, 0.7 or 1 of the way from the base value to the
  profile's.
- Known keys are clamped to a range (below); unknown keys are kept and
  ignored, so a later version can add keys.

The keys, their range, and what they are computed from:

| Key | Range | From |
|---|---|---|
| `style.retreat_shields` | 5–90 | the retreat level; 10 if the player flies away in less than 5 % of at least a minute of fights |
| `style.engage_weight` | 0.5–1.8 | 0.6 + the share of the moving-toward-or-away fight time spent closing in, mixed 60:40 with the share of lost enemies it follows |
| `style.collect_weight` | 0.5–2 | 0.7 + the share of pickups off course + 0.6 × the share in a fight |
| `style.range_scale` | 0.5–3 | median distance when firing (else with the enemy in sight) / 65, the middle of the bots' 35–95 band |
| `style.chase_memory` | 0.4–2.5 | median pursuit time / the base skill's `pursuit_seconds` |
| `style.close_scale` | 0.5–1.25 | mean speed toward or away when moving so / `COMBAT_CLOSE_SPEED` (0.9 of the top speed since §9.12 of the bots' document; 0.8 before) |
| `style.burn_chase_distance` | 40–1000 | the 10th percentile of the distance while chasing with the afterburner (not the push after a turn); 1000 if it does not |
| `style.dodge_bonus`, `mine_interval`, `strafe_scale`, `behind_engage`, `outgunned_retreat` | | not measured: never written, the base style's |
| `skill.strafe` | 0/1 | sideways or vertical thrust in 20 % or more of the fight time |
| `skill.strafe_min_ms`, `strafe_max_ms` | 150–3000, 250–5000 | the quartiles of the run length |
| `skill.strafe_vertical` | 0–1 | vertical / sideways thrust while strafing |
| `skill.strafe_speed` | 0–1 | mean thrust across while strafing: the thrust of the bot's strafe keys (Documentation/multiplayer-bots.md §9.12; 0.9 was the cap before) |
| `skill.dodge_prob` | 0–0.95 | the dodge probability, where measurable |
| `tune.range_lo`, `tune.range_hi` | 15–400, 30–800 | the quartiles of the firing distance (`BOT_RANGE_LO`/`HI`) |
| `tune.reverse_turn` | 0–1 | share of the large turns flown backwards (the bots' `turn_habits::reverse`, 0.12) |
| `tune.reverse_turn_speed` | 0.3–1 | backward speed reached in them (`REVERSE_TURN_SPEED`) |
| `tune.turn_boost`, `tune.turn_boost_burn` | 0–1 | share of large turns followed by a push, and of those with the afterburner (`turn_habits::boost`, 0.8, `boost_burn`, 0.3) |
| `tune.burn_retreat`, `tune.burn_roam` | 0–1 | share of the time fleeing / with no enemy in sight with the afterburner |
| `tune.missile_interval_scale` | 0.3–4 | median time between volleys of one fight / the base skill's `missile_interval` (only with 3 such pairs or more) |
| `tune.volley_size` | 1–8 | missiles per volley |
| `tune.pursuit_seconds` | 0–30 | median pursuit time (0: lets the enemy go) |
| `tune.grab_detour` | 0–1 | share of pickups off course |

A value the recordings say nothing about is left out; the confidence of the
others comes from how much evidence there is (for example fights: low below
one minute, high from five; large turns: low below 8, high from 30) and is
at most medium where it rests on estimated controls.

`apply_style_profile(profile, skill)` gives the `skill_params` and
`style_params` of a bot that flies the profile at a skill: a skill that does
not strafe or dodge at all (Trainee) still does not.

### 8.6 Validation with synthetic recordings

There are no recordings of real players in the repository, so
`test-movement-analysis` makes its own: a small flight simulation with the
ship's flight model flies scripted pilots through a fixed programme (a fight
with three bursts of enemy fire, the enemy behind, the enemy out of sight and
moving off, a flight with no enemy and a pickup; 40 s, repeated for 12
minutes, with the shields at 100, 80, 60, 45, 30, 15 in turn) and writes what
the game would record. The analysis must find the scripted habits again:

| Pilot | Scripted | Found |
|---|---|---|
| strafer | full sideways thrust, 0.5 s per run | strafes 84 % of the fight time, runs 500 ms, 99 reversals/min, vertical 0; `skill.strafe = 1`, runs 500–600 ms |
| bobber | strafe with equal vertical, 1.2 s runs, slides through turns | vertical 0.92, runs 1200 ms, 100 % sliding turns |
| reverse turner | turns round with reverse thrust, pushes forward after with the afterburner, chases with it beyond 150 units | 18 of 18 turns reverse, 1.4 s per 180°, push after 100 % (afterburner 100 %); `style.burn_chase_distance` 151 |
| sniper | fights at 250 units, gauss at range, flees below 50 shields with the afterburner, single missiles | fires at a median of 250; `style.range_scale` 3 (the cap); gauss 100 % beyond 150; retreat level 50; afterburner 97 % of the fleeing time; base style Cautious |
| brawler | fights at 40 units, never flees, follows a lost enemy for 6 s, volleys of three 4 s apart, leaves its course for pickups | fires at 40; `style.retreat_shields` 10; follows 18 of 18 for 6.0 s; volleys of 3.0, 4.0 s apart; 100 % of pickups off course; base style Aggressive |
| dodger | sidesteps 80 % of the bursts (46 of 54 in this run) after 250 ms | dodge probability 0.82 (baseline 0.16), reaction 300 ms; the others 0 |

The same flights recorded by another machine (no controls) give the same
picture from estimated thrust: shares within 0.06–0.08, the same turn
classification and run lengths, confidence medium instead of high. A game
recorded on the host and on a client that joined 50 s late and left early
merges into one: every tick of every player once, the client's controls exact
where its own file has them, every shot and hit once; by `sync` records, and
without them by the ships' paths (33 ms off, the lag with which each machine
sees the other). Also tested: two games that only look alike stay two, a file
cut short, a slot that changes hands, several games of one player, and the
profile's file format (round trip, hand-written files, clamping, confidence
blending).

Real recordings will differ: walls and blasts disturb the thrust estimate,
the network smooths a client's motion, and no human repeats a habit 18 times
in a row. The thresholds of `analysis::limits` and the mapping of section
8.5 are first guesses to be tuned against the first real recordings.

### 8.7 Step 3: bots that fly a profile

Put the `.botstyle` files into `botstyles/` in the PhysFS write
directory (next to `recordings/` and the pilot files; the game makes the
folder). The host reads them when the bot setup starts, when a Bots
screen opens and when a game starts; they appear in every style slider
after the four built-in styles, by name ("EC style"), and as a
style word in the chat (`/bot add hot EC`, `/bot style all
EC`). A bot gets `apply_style_profile(profile, its skill)` in place
of `style_of(style)` and `skill_of(skill)`, every `style.`, `skill.` and
`tune.` key included; the setup and the pilot's `.ngp` keep the
profile's name (`BotStyle<n>=`) besides the base style, which a host
without the file flies (with a console line). Only the host needs the
files. Details, limits for untrusted files and the table of what each
key sets: Documentation/multiplayer-bots.md §9.13.

## 9. Code

| File | Content |
|---|---|
| `common/main/movement_record_format.h` | Format: constants, records, encode/decode, quantisation, CRC, chunk builder, tick schedule (standard C++ only) |
| `common/main/movement_record_reader.h` | Reader library (header-only) |
| `common/main/movement_record.h`, `similar/main/movement_record.cpp` | The game's side: session file, sampling, context, event hooks |
| `common/tools/movrec_dump.cpp` | The dump tool |
| `common/unittest/movement_record.cpp` | Tests: round trips, header, chunks, truncation at every byte, damaged chunks, unknown records, the tick schedule at 20 to 1000 fps, quantisation and frames, the minor 1 additions |
| `common/main/movement_analysis.h` | Step 2: loading, sessions and clocks, merging, tracks and the control estimate, the movement profile, the proposal, the report (header-only) |
| `common/main/bot_style_profile.h` | The `.botstyle` format: keys and ranges, write, parse, apply to `skill_params`/`style_params`/`tune_params` (header-only, for the game too) |
| `common/main/bot_style_library.h` | Step 3: the styles of `botstyles/`: names, chat words, limits (header-only) |
| `common/tools/movrec_analyse.cpp` | The analysis tool |
| `common/unittest/movement_analysis.cpp` | Tests: synthetic recordings of scripted pilots (section 8.6) |

Hooks in the game (one call each): `GameProcessFrame` (sample, after the
bots fired), the game window's close (end of the session),
`do_laser_firing_player` and `do_missile_firing` (fire), `multi_do_fire`
(remote fire), `collide_player_and_weapon` (hit),
`object_create_explosion_with_damage` (splash hit), `multi_compute_kill`
(kill), `do_powerup` and the host's pickup grant log (pickup). `net_v2.cpp`:
clients set the afterburner bit of `INPUT`; `host_input_afterburner` reads
it on the host; a client with `-sharemoves` appends its controls
(`movement_record_shared_controls`) and `host_input_controls` gives the
host the fresh ones; `recording_clock` gives the session id and the host's clock
for the `sync` records.

The recording tick is its own schedule of game time (`tick_scheduler`): tick
`k` is due `k / rate` seconds of game time after the session start; a frame
samples at most once, a frame longer than a tick skips ticks instead of
writing duplicates, and pauses (no game frames) do not advance it.
