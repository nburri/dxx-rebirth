# Multiplayer bots (design)

Status: design; stage B0 (the pilot refactor, §3.2.1), stage B1 (the
first bot with its setup menus, §9.1), stage B3 (pickups, resources and
weapon choice, §9.2), stage B4 (secondaries, §9.4, §9.6) and stage B2
(skill presets, styles, mixed bots, persistence, the `BOT` marker and
humans replacing bots, §9.7) and stage B5 (managing bots during the game:
the in-game Bots screen and the `/bot` chat command, §6.4, §9.11) are
implemented; §9.12 retunes their flight after the first recordings of a
human against them. Target branch: `experimental-netcode`
(protocol v2, `Documentation/network-protocol-v2.md`, cited as "v2 §n").
D2X-Rebirth only (v2 decision 6). Line numbers are omitted; function names are
the anchors.

Bots are extra *players* in a network game, simulated by the host. They fly
the same ship, with the same physics, weapons, energy and inventory rules as a
human, and they are driven through the same control inputs a human produces.
To every client a bot is an ordinary remote player. Clients need no bot code
to play against one.

---

## 1. Goals and non-goals

Goals:

- Anarchy, team anarchy and bounty first. CTF and hoard come in a later stage (§9, stage B7).
- The host configures bots in the game setup menu (count, name, skill, style)
  and can add, remove and re-skill them during a running game.
- Bots are fun to fight. They navigate any custom level, fight, collect
  powerups and retreat when weak. Their mistakes are deliberate and tunable
  (aim error, reaction time, awareness). They are never aimbots and never
  see through walls.
- Bots behave the same at any host frame rate (30 to 500 fps) and cost well
  under 1 ms of host CPU per frame with 7 bots.
- Protocol v2 needs no change to its wire layouts, except one optional
  `PLAYER_LIST` flag bit (§2.2).

Non-goals (for now): robot anarchy and coop (deferred on this branch, v2
decision 3), bots on clients, and single-player bots outside a netgame. A host
can start a netgame alone with bots, which gives offline practice for free.
Also out of scope: guided missiles steered by bots, bots that chat, and
learning or adaptive AI.

---

## 2. Representation

### 2.1 A bot is a player slot owned by the host

| Aspect | Human remote player (today) | Bot |
|---|---|---|
| Slot | `0 < pid < MAX_PLAYERS` (8), fixed at `JOIN_ACCEPT` | same range; allocated by the host |
| `Players[pid]` | callsign, `objnum`, `connected = playing` | same; callsign from the bot config |
| `Netgame.players[pid]` | callsign, rank, `protocol.udp.addr`, ping | callsign, `rank = None`, addr zero, ping 0 |
| `net_v2::S.peers[pid]` | a `peer` with a `connection` | `ph == none`, no connection |
| Ship object on the host | `OBJ_PLAYER`, `control_source = remote`, pose written by `net_interp_apply_all` from `INPUT` | `OBJ_PLAYER`, `control_source = remote` (unchanged, so snapshots and `object_rw` are identical); moved by `do_physics_sim` from the bot's controls |
| Ship object on clients | driven by interpolation of bundle records | identical |
| Team, colour | `team_vector` bit, `alt_textures = pid` | identical |
| Kill matrix, scores | `kill_matrix[pid]`, `net_kills_total`, … | identical |

The host keeps a `bot_state` per bot slot, in a new `similar/main/bot.cpp`, in
`per_player_array<std::optional<bot_state>> Bots`. `bot_is_local(pid)` is true
on the host for those slots. Everything the brain needs lives there: config,
controls, pilot state, memory and path. Nothing about bots goes into `player`,
`object` or `netgame_info`. The one exception is the optional flag in §2.2.

### 2.2 How clients see a bot

- **Bundle** (v2 §5.2): `build_common_bundle` already has a branch for the
  host's own ship (`i == Player_num`: pose from the object, `sample_age 0`,
  `dying` from `Player_dead_state`). Bots take the same branch through
  `i == Player_num || bot_is_local(i)`, with `dying` coming from the bot's own
  death state. Without this, the `else` branch would find no valid
  `S.inputs[i]` and send a ghost record. Clients interpolate the record like
  any ship. Nothing changes on the wire.
- **Events and messages**: every gameplay message the bot causes (fire, kill,
  deres, reappear, inventory, drop blobs, sounds) is a normal v1 record, or
  later a v2 message, with the bot's `pid` as its originator. The transport
  already has the field: `send_legacy_reliable(originator, …)` and the
  `EVENT_U` header byte. Today `dispatch_table::send_data` always passes
  `Player_num`. It gets an originator parameter (§3.2). On the receiving end,
  a client's `receive_legacy_mdata` takes the originator the host names
  without question. Only the host checks `originator == peer_slot(p)`, and
  only for messages from its peers, so records the host originates for a bot
  pass unchanged.
- **Optional `PLAYER_LIST` flag**: bit 7 of the per-slot `connected` byte
  means "bot". Clients that understand it show `BOT` instead of a ping in the
  kill list and a small `[B]` after the name tag. The host uses it so that
  the rejoin-by-callsign rule (v2 §4.6) never gives a bot's slot to a human
  with the same name. This bumps `NET_V2_PROTO_VERSION`, which each stage
  does anyway. Without the flag, bots still work; they just look like humans
  with 0 ping.
- **Callsigns**: at most 8 characters (`CALLSIGN_LEN`). The host makes each
  one unique against every connected callsign (`Havoc` → `Havoc2`).

### 2.3 Lifecycle

- **Allocation**: in the lobby (`starting`), bots do not hold slots. Joiners
  fill holes as v2 stage 1 does. At *Start Game*, bots take the lowest free
  slots below `max_numplayers`. If humans plus bots exceed that limit, the
  host starts only as many bots as fit and says so. `max_numplayers` counts
  humans and bots together.
- **Level start**: the host sets bot slots up in `multi_prep_level_player`
  like any remote slot. It then places them with the spawn logic generalised
  from `InitPlayerPosition` (§3.2) and treats them as `LEVEL_READY` at once.
  Every host loop that expects a peer must skip bot slots: `LEVEL_READY`
  counting, `KICK(endlevel)` in `host_end_level`, timeouts, the extras queue
  and ping statistics (§10, risk R2).
- **Join in progress**: the joiner's snapshot contains the bot's ship object
  (`SNAPSHOT_OBJECTS`), its `connected` state, and its kill matrix row and
  column (`SNAPSHOT_GAME`). Before stage 3 the bot's inventory reaches the
  joiner through the extras (`multi_send_player_inventory`, sent by the host
  for each bot with the bot as originator). After stage 3 it arrives in
  `SNAPSHOT_INVENTORY`, one part per player, which covers bots automatically.
- **Humans replace bots**: with the option on (default), a `JOIN_REQUEST`
  that finds the game full removes the most recently added bot first
  (before a disconnected human's slot is reused). The
  removal is broadcast as `PLAYER_LEFT(quit)`, the same as a quitting player.
  The human then gets that slot.
- **Removal**: the host treats a removed bot like a player who sent `LEAVE`
  (v2 §4.6): ghost the ship, drop its eggs, `PLAYER_LEFT(quit)`, slot becomes
  `disconnected`. Its score stays in the kill list like any departed player's.
  A later joiner may reuse the slot, and `new_player` zeroes the scores.
- **Host leaves**: the game ends (`HOST_SHUTDOWN`) and the bots go with it.
  There is no host migration in v2, so nothing more is needed.
- **Level change**: bots persist across levels with their scores, like
  humans (`LEVEL_START` → `SNAPSHOT_GAME`). The host's level load carries
  every slot's scores to its new ship object (`net_score_carry.h`), the
  bots' included.

---

## 3. Driving the bot

### 3.1 Controls, not velocity

| | A: produce a `control_info` and run the human code path | B: set `velocity`/`orient` directly |
|---|---|---|
| Physics fidelity | exact: same `max_thrust`, drag, turn-roll, wiggle, bumps and wall slides as a human | the bot must re-implement limits, so it drifts from human feel and can cheat by accident |
| Afterburner, energy, fire rates | come for free from `read_flying_controls` / `do_laser_firing_player` | re-implemented |
| Frame-rate independence | already solved for humans (remainders in `phys_info`, `rate_divider`) | must be solved again |
| Validation (v2 §5.5, stage 4) | a bot can never produce a state a human could not | needs trust |
| Cost | a refactor to remove the "local player" globals (§3.2) | less code up front |

**Recommendation: A.** The bot brain outputs a `bot_input` of normalised axes
(pitch, heading, bank, forward, sideways and vertical, each in `[-1, 1]`)
plus buttons (fire primary, fire secondary, afterburner, drop bomb). Every
frame the pilot adapter turns it into `control_info` times: `axis × FrameTime`,
the same unit a held key produces (`pitch_time` etc.). It then runs the
generalised `read_flying_controls`, and `object_move_one` →
`do_physics_sim` moves the ship. Aim precision comes from the rotation axes
alone. There is no direct orientation writing (§4.4).

### 3.2 Removing the "local player" assumption (the enabling refactor)

The human code path assumes one pilot per machine. The code shows it with
these globals and checks:

| Code | Assumption | Change |
|---|---|---|
| `read_flying_controls` (`controls.cpp`) | returns unless `get_player_id(obj) == Player_num`; uses globals `Afterburner_charge`, `Local_player_rate_dividers`, `Player_dead_state`, `Drop_afterburner_blob_flag` | `apply_pilot_controls(object &, pilot &, const control_info &)`; move those globals into `struct pilot` (the human's is a global `Local_pilot`) |
| `do_laser_firing_player` (`laser.cpp`) | `Player_dead_state`; fires from `get_local_player().objnum`; `auto_select_primary_weapon` uses `PlayerCfg` order and HUD messages | take `pilot &`; bots pass their own weapon choice (§4.5) and no HUD |
| `do_missile_firing` | guided missile of `Player_num`; `Player_dead_state` | take `pilot &` |
| `multi_send_fire`, `multi_send_player_deres`, `multi_send_reappear`, `multi_send_kill`, `multi_send_player_inventory`, `multi_send_drop_blobs` | write `Player_num` into the record and send it as the local player | take `playernum_t pid` (default `Player_num`) and pass it to `send_data(buf, priority, originator)` |
| `apply_damage_to_player` (`collide.cpp`) | applies damage only if `get_player_id == Player_num` (the victim decides, pre-stage 4) | also when `bot_is_local(pid)`; palette flash only for `Player_num` |
| `collide_player_and_wall`, lava/volatile walls, `wall_hit_process` door opening | local player only | also for local bots (bots open doors, take lava damage) |
| `collide_player_and_powerup` → `do_powerup` | local player only | bots use stage 3's `powerup_apply(pid, obj)` directly (host = authority) |
| `object_move_one`: fuel/repair centres, `check_trigger` (with `get_local_plrobj()`) | local player only | fuel/repair for bots; triggers from bots in anarchy only for doors in stage B1 (full triggers follow v2 stage 6's `TRIGGER` path with `pid`) |
| `do_cloak_stuff`, `do_invulnerable_stuff` (`game.cpp`) | expiry side effects (`maybe_drop_net_powerup`, decloak message) only for `Player_num` | also for local bots |
| `start_player_death_sequence`, `dead_player_frame`, `DoPlayerDead` | one global death state, camera, cockpit | bots get a small separate death state machine (§4.8) and never touch these |
| `InitPlayerPosition` (`gameseq.cpp`) | places `ConsoleObject` | split into `choose_spawn(pid)` (the `SecludedSpawns` logic) and `place_player(pid, spawn)`; v2 stage 4 needs this split for `PLAYER_SPAWN` anyway |

Rejected alternative: "possession", which means swapping `Player_num`,
`ConsoleObject` and `Controls` to the bot around its update. It is tempting
because it needs almost no refactor. But `multi_i_am_master()` is literally
`Player_num == 0`, so the host would stop being the host in the middle of a
frame. The HUD, palette flashes, sounds, cockpit and demo recording would all
fire for the bot, and one missed restore corrupts the local player. Many of
these functions change anyway in stages 3 and 4 (host-side pickups and damage
need a `pid`), so the refactor above is work v2 needs regardless.

### 3.2.1 B0 as implemented

B0 changes no behaviour: every human call site passes `Local_pilot`,
`Player_num` or the local ship, so the random number sequence, the wire
records and the demo records are unchanged. Where the code differed from
the table above:

- **`struct pilot`** (`common/main/pilot.h`) holds `dead_state`,
  `missile_firing_count` (was `Global_missile_firing_count`) and, in D2,
  `afterburner_charge`, `rate_dividers` (the type `local_player_rate_dividers`
  is now `pilot_rate_dividers`) and `drop_afterburner_blob_flag`. The human's
  is `Local_pilot`. The old global names stay as *references* to its
  members. They are used by code that only ever concerns the human (HUD,
  cockpit, death camera, demo, savegame, weapon selection UI), so those
  call sites are untouched. Code that a bot will run takes a `pilot &`.
- **Fire timers and the weapon selection** already live in `player_info`
  (`Next_laser_fire_time`, `Primary_weapon`, …), so they did not move.
- **Controls**: `apply_pilot_controls(object &, pilot &, const control_info &)`
  holds the body of `read_flying_controls`, which is now the human's
  wrapper (the `Player_num` check, then `Local_pilot`). The guided missile
  it steers is the one of `get_player_id(obj)`.
- **Firing**: `do_laser_firing_player(pilot &, vmobjptridx_t)`,
  `do_missile_firing(pilot &, …)`, `omega_charge_frame(pilot &, player_info &)`
  and `allowed_to_fire_laser(pilot &, …)`. Still human-specific inside and
  left for B1: `auto_select_primary_weapon` (`PlayerCfg` order, HUD text),
  `cheats.rapidfire`, `do_laser_firing` sending `MULTI_FIRE` only when the
  shooter is the local ship, `Laser_player_fire` finding a homing target
  only for `ConsoleObject`, and `release_local_guided_missile` setting
  `Missile_viewer`.
- **Messages**: `multi_send_fire`, `multi_send_player_deres`,
  `multi_send_reappear`, `multi_send_cloak`, `multi_send_decloak` and
  `multi_send_sound_function` take a `playernum_t pnum = Player_num`;
  `multi_send_drop_blobs` already had one. `multi_send_kill` takes the pid
  from the dead ship it is given. `multi_send_player_inventory` does not
  exist any more on this branch (stage 3 replaced it with the inventory
  messages of `net_objects.cpp`). `multi_send_player_deres` flushes the
  inventory and ends the life in `net_objects` only for `Player_num`; for a
  ship the host flies itself, B1/B3 must make the host's inventory copy
  (`A.mirrors[pid]`, read by `net_objects_host_drop_player_eggs`) current.
- **Originator**: `dispatch_table::send_data(data, priority, originator)`;
  `multi_send_data` defaults it to `Player_num`. An `EVENT_U` names one
  originator, so a record of another originator flushes the pending events
  first, and a reliable record only takes pending events of its own
  originator along.
- **Spawn**: `choose_spawn(vmobjptr, pid, random_flag)` returns a
  `spawn_choice` (`none`: no usable site, leave the ship; `in_place`:
  deathmatch level start, reset only; `site`: move to `Player_init[site]`),
  and `place_player(vmsegptridx, plrobj, spawn)` applies it.
  `InitPlayerPosition` is `reset_cruise()` plus these two for `Player_num`.
  The ranking and the draw are pure functions in `common/main/spawn_site.h`,
  which `test-spawn-site` checks against a verbatim copy of the old code.
  `choose_spawn` still reseeds and draws the global `d_rand` (as the human
  does); only bot *decisions* use the bot's own RNG (§3.4).
  In a network deathmatch the host assigns every spawn, its bots' included
  (`choose_bot_spawn` shares the host's reservations of `assign_spawn`:
  a site assigned in the last 2.5 s, to a bot, the host or a client,
  counts as a ship and is left out while a free open site remains), so a
  bot and a human respawning at the same moment no longer take the same
  site (network-protocol-v2.md §8, "Host-assigned spawns").
- **Not moved** (human-only presentation, bots get their own in B1/B3):
  the afterburner sound state of `do_afterburner_stuff`, the fusion charge
  sound and palette flash in `FireLaser`, the headlight drain.

### 3.3 Order within a host frame

In `GameProcessFrame`, host only, `Game_mode & GM_NETWORK`:

1. `bot_frame_all()` runs before `game_move_all_objects`. It advances the bot
   tick accumulator and, for each tick that is due, runs perception,
   decision, navigation and aim (§4). The result is each bot's `bot_input`
   for the frame.
2. In `object_move_one`, a bot's ship (`control_source == remote` and
   `bot_is_local`) calls `apply_pilot_controls` in the `remote` case, then
   `do_physics_sim` as for the local ship.
3. After the move, where `FireLaser` / `do_laser_firing_player` run for the
   human, `bot_fire_all()` runs the generalised firing for each bot whose fire
   button is down. Fire timing is `Next_laser_fire_time` in `player_info`,
   already per player and frame-rate independent.

### 3.4 Frame-rate independence

- **Decisions on a fixed tick.** The brain runs at 60 Hz from its own
  `tick_accumulator` (`common/main/net_interp.h`, already tested exactly
  from 2 ms to 100 ms frames). At 30 fps two ticks run in one frame. At
  500 fps most frames run none and keep the last `bot_input`. Slower layers
  are fixed divisors of that tick: perception at 20 Hz, strategy at 5 Hz.
  Reaction delays are counted in ticks.
- **Continuous control per frame.** Only the steering controller runs every
  frame. It turns the goal direction (fixed at the last tick) into rotation
  axes: `axis = clamp(angle_error / (max_turn_rate × FrameTime), -cap, cap)`.
  That is proportional steering that cannot overshoot within one frame, so
  the path it traces does not depend on the frame length. Physics already
  integrates thrust with frame-rate-independent remainders.
- **Own RNG.** Each bot has a `std::minstd_rand` seeded from (session id,
  slot, level). Bots never call `d_rand()` for decisions, so their choices do
  not depend on how many frames consumed the global sequence. This also makes
  them reproducible in tests.
- **Test**: the offline arena test (§8.2) runs the same scenario at
  30/60/144/500 fps and checks that the trajectories agree within 0.5 unit
  over 10 s, and that each tick's decision sequence is identical.

### 3.5 CPU budget (per bot, per second of game time)

| Work | Rate | Cost estimate |
|---|---|---|
| Perception: one `find_vector_intersection` per candidate target (≤ 7) | 20 Hz | 140 fvi ≈ 1–3 ms |
| Threat scan: weapon objects within 150 units (object list ≤ 350) | 20 Hz | < 0.3 ms |
| Strategy: powerup and goal scoring | 5 Hz | < 0.2 ms |
| Path planning: A* over ≤ 9000 segments with a cached graph | ≤ 2 per s, and only on goal change or when stuck | 0.2–1 ms each |
| Path smoothing and wall probes: 2–3 fvi | 20 Hz | ≈ 1 ms |
| Steering and controls | per frame | negligible |

That is about 5 ms per second per bot, or 35 ms per second for 7 bots: under
4 % of one core, spread over frames. Bots are staggered (bot `k` runs its
20 Hz and 5 Hz work on tick `k mod 3` / `k mod 12`), so no single frame pays
for all of them. A debug counter prints the bot CPU time per second on the
host console, like `connection_stats`.

---

## 4. Brain

### 4.1 Layers

```
           5 Hz  STRATEGY   utility scores -> goal: Engage(t) | Hunt(last seen) |
                            Collect(p) | Retreat(to health/energy) | Roam
          20 Hz  PERCEPTION visible/heard players, memory, threats, visible powerups
          60 Hz  TACTICS    path following, range keeping, strafe pattern, dodge,
                            aim point (lead + error), fire/weapon decisions
       per frame STEERING   goal direction -> rotation axes; thrust axes -> control_info
```

`bot_brain` is pure logic over a `bot_world_view` snapshot (positions,
velocities, visibility bits, inventory numbers). It lives in
`common/main/bot_brain.h` and uses only the standard library plus `fix` and
`vms_vector`, so it can be unit tested (§8.1). `similar/main/bot.cpp` fills
the view from the game (fvi, objects, segments) and applies the result.

### 4.2 Perception

- **Line of sight**: `find_vector_intersection` from the bot's eye (its
  position) to the target's position, with `FQ_TRANSWALL` so grates and
  windows can be seen through. Whether a shot can reach the target is a
  second query without `FQ_TRANSWALL`, using the weapon's radius. This is the
  same fvi call `player_is_visible_from_object` makes in `ai.cpp`, and it
  never looks through solid walls.
- **Field of view and awareness**: a target is *seen* if LOS holds, it is
  within `awareness_radius`, and it lies within the skill's field of view
  (`dot(fvec, dir) > cos(fov)`). A target behind the bot is only noticed by
  sound or damage.
- **Hearing**: a player firing, or a nearby afterburner, reveals its position
  (not continuous tracking) to bots within `hearing_radius` of the path
  distance. That is roughly what a human with sound gets.
- **Damage**: when a bot is hit, it learns the attacker's approximate
  direction, and its position with ±20 units of error.
- **Cloak**: a cloaked player is only seen within 40 units, or for 0.5 s
  after it fires, matching what a human sees of the shimmer and muzzle flash.
- **Memory**: per player it keeps `{last_seen_pos, seg, vel, time, confidence}`.
  Confidence decays linearly over `memory_seconds`. Hunting goes to the last
  seen position and then searches neighbouring segments.
- **Reaction delay**: the tactics layer reads perceived target state from a
  ring buffer `reaction_ticks` old. A bot therefore reacts to a sudden turn by
  its target after the reaction time, not instantly. That one mechanism
  produces most of the believable misses.

### 4.3 Navigation

`create_path_points` (`aipath.cpp`) is the natural candidate, but it cannot be
used as is:

- It searches breadth-first by hop count, not distance.
- Its door rule `ai_door_is_openable` only lets `ConsoleObject` or robots
  with `robot_info` through doors. A bot's ship has `robptr == nullptr`, so
  every door is closed to it.
- It writes into the shared robot pool `Point_segs`, which
  `ai_path_garbage_collect` compacts on the assumption that robots own it.
- It consumes `d_rand()`.

The design keeps its structure but uses a separate module:

- **`common/main/bot_nav.h`** (pure): A* over a graph given as
  `{node count, neighbours(node) → (node, edge id, cost)}` with a pluggable
  `passable(edge)`. It is unit tested on synthetic graphs, including graphs
  as large as `MAX_SEGMENTS`.
- **`similar/main/bot_nav.cpp`**: at level load it caches segment centres,
  side centres and the child adjacency (`IS_CHILD`). Edge cost is the
  distance between centres through the shared side's centre. Passability is
  evaluated live, because doors and walls change:
  `WALL_IS_DOORWAY(...) & WALL_IS_DOORWAY_FLAG::fly`, or a `WALL_DOOR` that a
  player could open (not `door_locked`, keys held). Illusion walls are
  passable. Closed walls, force fields and blastable walls not yet blown are
  blocked. Lava segments get an extra cost.
- **Path points**: each segment centre plus, as `insert_center_points` does
  for robots, the centre of the side between two segments, so a path never
  cuts a corner through solid geometry.
- **String pulling**: at 20 Hz the bot steers at the furthest of the next 4
  path points that a ship-radius fvi from its position reaches. This is
  `polish_path` generalised, and it lets the bot fly straight across big rooms.
- **Wall avoidance**: a forward probe along the velocity, of length
  `speed × 0.4 s`. If it hits, the bot thrusts along the hit normal and
  brakes.
- **Stuck recovery**: if progress along the path is below 2 units per second
  for 1.5 s, the bot reverses and strafes randomly for 0.5 s and replans with
  the current segment's exit edge penalised. After 3 failures it picks a new
  goal.
- **Doors**: a closed door on the path is flown into. The bot's collision
  opens it through the same `wall_hit_process` a human triggers (§3.2).

### 4.4 Target selection and aiming

**Target score**, recomputed at 5 Hz with 20 % hysteresis for the current
target:

`score = visible ? 1 : memory_confidence × 0.5`
`       × range_factor(dist)` (1 at close range, falling to 0.3 at the awareness radius)
`       × (1 + 0.5 × recently_damaged_me)` (revenge, 3 s)
`       × (1 + 0.3 × low_shields(target))` (finishing a weak target)
`       × (bounty target ? 2 : 1)`

Teammates in team modes, ghosts, and players in their spawn invulnerability
(if the style says so) score 0.

**Aim point**: solve the intercept `|P + V·t − S| = w·t`. Here `P` and `V`
are the perceived (delayed) target position and velocity, `S` is the bot's
gun point, and `w` is the weapon speed from `Weapon_info[...].speed`
(`ai.cpp` `lead_player` uses the same idea with a linear time estimate).
Skill enters in three places:

- **Lead accuracy** `ℓ`: the bot leads with `k × V`, where `k` is 1 on
  average with an error of `(1 − ℓ) × 0.35`, drawn and held like the aim
  error (section 9.3; B1 used `ℓ × V`, a systematic under-lead). At
  `ℓ = 0` it aims at where the target is.
- **Aim error**: an angular offset drawn from `N(0, σ)` in two axes, held for
  `aim_drift_period` and then eased toward a new sample. Aim therefore
  wanders smoothly instead of jittering, and a steady target is hit more
  often than a juking one.
- **Turn-rate cap**: the steering axes are clamped at `turn_cap` × the ship's
  maximum, so at the low skills a fast side-strafe outruns the bot's turn.

**Trigger discipline**: the bot fires the primary only when the angle between
`fvec` and the aim direction is below `fire_cone` and the shot LOS is clear.
In team games it also checks that no teammate is the first object on the line
(an `fvi_query` that passes `LevelUniqueObjectState`, so objects are tested). Weapons with a charge or spin-up (fusion charge,
omega) use their own rules (§4.5).

### 4.5 Weapons

**Primary choice**, a table indexed by range band and weapon, re-evaluated at
5 Hz. A switch costs `REARM_TIME` exactly as `select_primary_weapon(...,
wait_for_rearm = 1)` does. Bots never skip the rearm.

| Range | Preference (highest owned wins; energy < 20 prefers ammo weapons) |
|---|---|
| < 60 | fusion (Hotshot+), omega (if energy > 60), spreadfire, helix, super laser/quad |
| 60–150 | plasma, helix, super laser/quad, gauss, vulcan, phoenix (in corridors) |
| > 150 | gauss, vulcan, laser; hold fire when the expected hit chance is below 10 % (the bot will not spam at long range) |

Below `weapon_smarts` 2 the bot keeps whatever autoselect picks, using the
default order `DefaultPrimaryOrder` (not the host's `PlayerCfg`).

**Secondary use** (by `weapon_smarts`):

| Weapon | Rule |
|---|---|
| Concussion, homing, mercury | target visible, 40–200 units, and at least 1.5 s since the last missile; homing preferred against fast strafers |
| Smart missile | target within 120 units, even without direct LOS if the bot saw it within 1 s (children home) |
| Mega, earthshaker | target > 70 units away (outside its own blast), shot LOS clear, target slow or cornered; at most one per engagement; Ace+ only |

Section 9.6 replaces the heavy missiles' row: an expected-outcome rule
by skill and style, aims at walls and corners, and hugging.
| Proximity bomb, smart mine | dropped while retreating with a pursuer behind within 80 units, or at a doorway on its own path while being chased; Hotshot+ |
| Flash missile | when the target is facing the bot within 100 units (cosmetic advantage only) |
| Guided missile | fired as an unguided missile that is released immediately; steering is a non-goal |

### 4.6 Movement in combat and evasion

- **Range keeping**: each weapon has a preferred distance band. The bot
  thrusts forward or back to stay in it and sidesteps while firing.
- **Strafe pattern**: lateral strafing whose direction flips every
  0.4–1.2 s (random within the style's range), plus vertical bobbing for
  Ace+. Circle-strafing happens when the target is in the band.
  (Section 9.12: both are keys held as a human holds them.)
- **Dodge**: at 20 Hz the bot scans weapon objects within 150 units whose
  parent is not itself (nor a teammate or coop partner when friendly fire
  is off). For each it predicts the closest approach over the
  next 0.7 s, assuming the projectile flies straight. If a projectile will
  pass within `ship radius + 3`, the bot dodges with probability
  `dodge_prob`, after `reaction_ticks`: full thrust perpendicular to the
  projectile's velocity, away from the nearer wall. An incoming homing
  missile also triggers afterburner and a turn to break LOS.
- **Bots see projectiles late**: the host's copy of a human's projectile
  appears one uplink plus one tick after the shot. A dodge decided on the
  host is therefore slightly handicapped against high-ping shooters. This is
  fair, because the bot is the host's local player.

### 4.7 Resources, powerups, special items

- **Shield and energy thresholds** (per style, §5.2): below `retreat_shields`
  the bot's strategy strongly prefers Retreat. Retreat means flying away from
  the attacker along a path whose goal is the best known shield source. That
  is a visible shield powerup, a remembered one, or an energy centre for
  energy. With energy below 30 and no energy weapon ammo, the bot prefers
  Collect(energy) or `segment_special::fuelcen` segments. Fuel and repair
  centres work for bots through §3.2.
- **Powerup knowledge**: bots know every powerup they have seen, plus the
  level's initial powerup layout within `map_knowledge` path distance.
  Humans know their maps; a Trainee does not. The Collect score is value ×
  need ÷ path distance. Value is a table (e.g. a better primary than the
  current best is worth 5; quad 4; shield 1–4 by need; energy by need;
  missiles 1). Collecting stops when an enemy is seen, unless the style is
  Collector.
- **Pickups** go through the host's authority path (stage 3): a bot touching
  a powerup on the host calls `powerup_apply(pid, obj)` and the host
  broadcasts the grant. No request or round trip is needed.
- **Afterburner**: used when chasing a target more than 150 units away,
  while retreating, when dodging homing missiles, and in long straight path
  segments while roaming. Only if the charge is above 30 % (Rookie+).
- **Cloak and invulnerability** activate on pickup, as for humans. The
  strategy raises Engage and lowers Retreat while they last. Invulnerable
  bots ignore their retreat threshold.
- **Energy→shield converter**: Ace+ press the converter when their shields
  are below 60 and their energy is above 100.
- **Headlight**: never.

### 4.8 Death and respawn

When `apply_damage_to_player` takes a local bot below 0 shields, the bot runs
its own death state. It does not use `Player_dead_state`.

1. **Pre-stage 4**: the host runs `multi_compute_kill` and sends
   `MULTI_KILL_HOST` with the bot in byte 1. This is the path the host
   already uses to relay other players' deaths. The bot's record gets the
   `dying` flag and the ship tumbles for the usual death time (it keeps its
   physics and gets no input).
2. **Explosion**: `drop_player_eggs` on the bot's inventory. Before stage 3
   this is `MULTI_PLAYER_DERES` with the bot as originator; after, it is host
   `OBJ_CREATE`s. The ship becomes a ghost and the bundle sends a ghost record.
3. **Respawn** after `respawn_delay`: 1.0–2.5 s, drawn from the bot's RNG,
   since humans also take a moment to press fire. The host uses
   `choose_spawn(pid)` / `place_player(pid)`, `InvulAppear`, and the spawn
   grants from `Netgame.SpawnGrantedItems`, then sends `MULTI_REAPPEAR` as
   the bot (after stage 4, `PLAYER_SPAWN`). Memory is cleared except for
   "who killed me", which gets a revenge bonus.

---

## 5. Skill levels and styles

### 5.1 Skill presets

The names match D2's difficulty names. The presets are `constexpr` tables in
`bot_brain.h`.

| Parameter | Trainee | Rookie | Hotshot | Ace | Insane |
|---|---|---|---|---|---|
| Reaction delay | 550 ms | 400 ms | 280 ms | 200 ms | 140 ms |
| Aim error σ (per axis) | 7° | 4.5° | 2.8° | 1.7° | 1.0° |
| Aim drift period | 0.6 s | 0.5 s | 0.4 s | 0.3 s | 0.25 s |
| Lead accuracy ℓ | 0 | 0.4 | 0.7 | 0.9 | 1.0 |
| Turn-rate cap (× ship max) | 0.45 | 0.6 | 0.75 | 0.9 | 1.0 |
| Fire cone | 12° | 9° | 6° | 4° | 3° |
| Field of view (half angle) | 45° | 60° | 70° | 80° | 90° |
| Awareness radius | 150 | 250 | 350 | 450 | 600 |
| Hearing radius (path distance) | 0 | 80 | 150 | 250 | 350 |
| Memory | 2 s | 3 s | 5 s | 7 s | 10 s |
| Dodge probability | 0 | 0.2 | 0.45 | 0.7 | 0.85 |
| Weapon smarts (0–4) | 0 | 1 | 2 | 3 | 4 |
| Secondary use | none | concussion/homing | + smart, mines | + mega | + earthshaker, all combos |
| Afterburner use | never | chase only | chase, retreat | + dodge | + tactical roaming |
| Strafe while fighting | no | slow flips | yes | yes, + bobbing | yes, + circle |
| Map knowledge (powerup layout) | 0 | 3 segments | 8 | 15 | whole level |

Rules that hold for every skill: bots use exactly the human ship's thrust,
turn rate, fire rates and energy costs. They perceive only through LOS,
hearing and damage. No preset has zero reaction time or zero aim error.
Insane is meant to feel like a very good human, not a machine. Every value
can be overridden per bot in `.ngp` for tuning (§6.4), but the menu offers
only the five presets.

### 5.2 Styles (personality)

Styles are multipliers on top of the skill.

| Style | Retreat at shields | Engage/Collect weighting | Range | Other |
|---|---|---|---|---|
| Balanced (default) | 45 | 1.0 / 1.0 | weapon default | — |
| Aggressive | 30 | 1.5 / 0.6 | −25 % | chases 2× longer, fewer mines, uses mega more readily (section 9.6: more self-risk, hugs) |
| Cautious | 65 | 0.8 / 1.2 | +25 % | dodge prob. +0.1, drops mines when retreating, breaks off when outgunned |
| Collector | 50 | 0.7 / 1.8 | weapon default | roams powerup-rich areas and hoards before fighting |

(The retreat thresholds are those of section 9.12; B2 had 35, 20, 55, 40.)

---

## 6. User interface

### 6.1 Host setup menu (`net_udp_setup_game`)

A new item, "Bots...", goes after the "Maximum players" slider. Its label
shows the current setup. `param_opt::m` grows from 22 to 23 and the
`Assert(optnum <= 20)` bound grows by one.

```
                NETGAME SETUP
   Start Game
   Description:
   [Nic's game______________]
   Level (1-27)
   [1__]
   Options
   (*) Anarchy        ( ) Team Anarchy
   ...
   ( ) Open game  ( ) Closed game  ( ) Restricted Game
   Maximum players: 8        [=========|]
   Bots: 3 (Hotshot) ...                      <- new
   Advanced Options
```

Robot anarchy and coop are greyed out on this branch. The Bots item is
disabled with the text "Bots: not in this mode" in CTF and hoard until stage
B7.

### 6.2 Bots screen (setup and in game, one implementation)

```
                      BOTS
   Number of bots:          3   [===|-----]   (0 .. max players - 1)
   Default skill:     Hotshot   [==|==]       (applies to new bots)
   Default style:    Balanced   [|===]
   [x] Humans replace bots when the game is full
   -------------------------------------------------
   1. Ravager    Hotshot   Balanced     ...
   2. Havoc      Ace       Aggressive   ...
   3. Sparky     Rookie    Cautious     ...
   -------------------------------------------------
   Set all bots to default skill
   New random names
   Done
```

- The count slider adds or removes list lines immediately. New lines take
  the default skill and style and the next unused name from the built-in
  list (`Ravager, Havoc, Sparky, Nomad, Wraith, Talon, Viper, Blitz, Rook,
  Jinx, Vortex, Grinder, Cinder, Specter, Brick, Dart`).
- Choosing a bot line opens the per-bot screen.
- The same menu class is used in game (§6.3). The count slider becomes
  "Add bot" / "Remove last bot" items there, because each change is a join
  or a leave.

### 6.3 Per-bot screen

```
                    BOT 2
   Name:   [Havoc___]            (8 chars; in game the bot is renamed)
   Skill:        Ace   [===|=]
   Style: Aggressive   [=|==]
   Team:        Auto   [|==]     (Auto / Blue / Red; team modes only)
   Remove this bot
   Done
```

### 6.4 In game

As implemented in stage B5 (details in §9.11). Only the host manages
bots, in the modes bots play (anarchy, team anarchy, bounty).

- **ESC "Game Menu"** (`HandleSystemKey(KEY_ESC)`, `gamecntl.cpp`) has a
  third choice for the host: `Abort Game`, `Options...`, `Bots...`. It
  opens the in-game Bots screen. The game runs on underneath, as under
  the game menu itself.

  ```
                        BOTS
                    in this game
     New bots' skill: Hotshot   [==|==]
     New bots' style: Balanced  [|===]
     [x] Humans replace bots when full
     Players: 5 of 8, 3 bots

     1. ravager   Hotshot Bal  Blue
     2. havoc     Ace     Aggr Red
     3. sparky    Rookie  Caut Blue

     Add a bot
     Add a bot: name, skill, style...
     Save as default setup
     Done
  ```

  - The list shows the bots playing (name, skill, style, and the team in
    team modes), in the order they were added. It is rebuilt at once
    when the bots change by any way (this screen, `/bot`, `/kick`, a
    human replacing a bot).
  - **Add a bot** adds one with the "new bots" skill and style and the
    next built-in name nobody in the game has or had. **Add a bot:
    name, skill, style...** opens the per-bot screen first (§6.3 with
    "Add this bot" / "Cancel").
  - A bot line opens the per-bot screen of §6.3 for that bot: name,
    skill, style, team (team modes), "Remove this bot", "Done". Only
    "Done" applies the changes; Escape discards them (unlike the setup,
    where nothing is at stake while the screen is open).
  - As every in-game menu of a network game, the screens close by
    themselves when the host is hit, dies or the level ends
    (`game_leave_menus`); nothing half-edited is applied then. The chat
    commands are the way to manage bots under fire.
  - The two sliders and the checkbox are the options of this game only;
    they start as the setup's.
  - **Save as default setup** makes the bots playing (and the three
    options) the saved setup (§6.5).
- **Chat commands** on the host, next to `/kick:` and `/move:` in
  `multi_send_message_end`:

  | Command | Effect |
  |---|---|
  | `/bot add [skill] [style] [name]` | adds a bot; what is left out is the "new bots" skill or style, and the next built-in name |
  | `/bot remove <name\|all>` | removes the bot, or all |
  | `/bot skill <name\|all> <skill>` | trainee, rookie, hotshot, ace, insane |
  | `/bot style <name\|all> <style>` | balanced, aggressive, cautious, collector |
  | `/bot list` | one HUD line per bot, for the host only |
  | `/bot save` | as "Save as default setup" |
  | `/bot`, `/bot help` | the usage |

  A skill or style is its full name or exactly its first three letters
  (`hot`, `agg`; also the list's `Aggr`, `Caut`, `Coll`), in any case. A
  bot is named by its name or by a unique beginning of it. No bot is
  called by a reserved word: `all`, a skill or style word (full, three
  letters, the list's short names) or a command word (`add`, `remove`,
  `rm`, `kick`, `skill`, `style`, `list`, `save`, `help`, `bot`). So
  `/bot add col ace` (style before skill) is an error that says the
  order, not a bot named "ace"; the Bots screens refuse such a name with
  a message, and the setup keeps the old name. The answers
  (and the errors) are HUD messages on the host. `/kick: <botname>` still
  removes a bot (as kicked). A client that types `/bot ...` reads "Only
  the host can manage bots"; the line is not sent as chat. A chat line
  holds 33 characters, so a long command needs the short words
  (`/bot add hot agg ravager`).
- **Limits.** A bot is added only while a level is played (not between
  levels, not during the reactor countdown), and not while the host
  serves a human's join (a moment later it works). It takes the slot a
  joining human would get without anyone leaving: a free slot below
  `max_numplayers`, else a departed bot's. A disconnected human's slot
  is kept for that human, so the game can be "full" for bots with fewer
  players than the limit. A bot added during the game is the newest, so
  it is the first a joining human replaces.
- **Kill list**: bots show `BOT` in the ping column (with the flag of
  §2.2).

### 6.5 Defaults and persistence

- Defaults: 0 bots, Hotshot, Balanced, humans replace bots.
- Persistence goes in the pilot's netgame profile (`<pilot>.ngp`,
  `read_netgame_profile` / `write_netgame_profile`), which already stores
  the setup menu. The format is key=value, unknown keys are ignored, and
  lines are under 50 characters:

  ```
  BotCount=3
  BotDefault=2,0
  BotReplace=1
  Bot0=Ravager,2,0,0
  Bot1=Havoc,3,1,0
  Bot2=Sparky,1,2,0
  ```

  The fields are name, skill 0–4, style 0–3, and team (0 auto, 1 blue, 2 red).
  The file is written when the setup menu closes. What the host changes
  during a game (§6.4) stays in that game; the saved setup changes only
  with "Save as default setup" (or `/bot save`), which writes the bots
  then playing, the "new bots" skill and style and the replace option.
  So a bot added for one evening, or a bot a human replaced, does not
  alter what the next game starts with unless the host says so.

---

## 7. Networking and authority

### 7.1 Now: stages 2–3, victim decides damage

The bot is the host's second local player.

| Event | Who decides | Path |
|---|---|---|
| Bot movement | host (it is the owner) | pose into the bundle like the host's ship; no `INPUT` |
| Bot fires | host | `MULTI_FIRE` with the bot as originator; clients spawn the shot at the bot's interpolated gun, like any remote shot |
| Human's shot hits a bot | host: the victim's machine | the host's copy of the human's projectile collides with the bot's ship; `apply_damage_to_player` for the local bot |
| Bot's shot hits a human | the human's client (victim) | unchanged v1 rule; the kill names the bot's ship as killer and the host credits the bot |
| Bot kills a bot or the host | host | all local |
| Pickups (after stage 3) | host, instantly | `powerup_apply(pid)` + grant broadcast |
| Death drops | host | `MULTI_PLAYER_DERES` as the bot (pre-stage 3), `OBJ_CREATE` (stage 3) |

Consequence: shooting a bot feels exactly like shooting the host. The human
must lead by their own latency to the host, because the host checks hits
against its current bot position. That matches today's experience with the
host's ship and needs no new code.

### 7.2 After stage 4: host decides with lag compensation

- **History**: each tick the host writes bot positions into the v2 §6.6
  history ring, with `host_time = now`, as for its own ship. A human's
  `WEAPON_HIT` on a bot is validated and rewound like a hit on any player.
  Shooting bots then feels like shooting any remote player, lag-compensated.
- **Bot shots**: a bot is a shooter at the host with zero latency, like a
  robot. The host detects its projectiles' hits itself and rewinds the victim
  to `host_now − one_way_latency(victim)`: the robot rule of v2 §6.5,
  reused as is.
- **Death and respawn**: the host's generic `PLAYER_KILLED` / `PLAYER_SPAWN`
  path works for bots. The bot-specific death code of §4.8 shrinks to
  "request respawn after the delay", the bot's equivalent of `WANT_RESPAWN`.
- **Validation** (v2 §5.5) never applies to bots: they have no `INPUT`, and
  they fly the human physics, so they cannot produce an invalid state.

### 7.3 Bandwidth

A bot adds one 43-byte player record per tick to every client's bundle:
≈ 2.6 KB/s per client at 60 Hz, or 20 kbit/s. Its fire and inventory messages
cost the same as a human's. With 1 human host, 3 human clients and 4 bots,
the host uploads to 3 clients the 8-record bundle §7 of the v2 document
already sizes for: 8 slots are 8 slots, whoever fills them. There is **no
upstream from bots**, so the host's download is lower than with 8 humans.
Bots add nothing to clients' upload.

---

## 8. Testing

### 8.1 Unit tests (CI, standard library only, like `test-net-v2-interp`)

The new `test-bot-brain` and `test-bot-nav` cover:

- **Intercept solver**: exact on straight-line targets; no solution when the
  target outruns the shot; ℓ scaling.
- **Aim error model**: the empirical σ and drift period match each preset
  over 10⁵ samples; results are deterministic for a given seed.
- **Reaction buffer**: the perceived state is exactly `reaction_ticks` old.
- **Dodge predictor**: closest-approach maths; a miss is not dodged.
- **Target and utility scoring**: hysteresis; teammates excluded; the bounty
  bonus; the ordering of retreat thresholds per style.
- **Weapon choice tables**: every range band and inventory combination
  returns an owned weapon; energy starvation prefers ammo weapons.
- **A\***: optimal on synthetic grids and random graphs, compared with
  Dijkstra; respects `passable`; a 9000-node graph within the time budget;
  deterministic tie-breaking.
- **Tick accumulator**: the brain's decision sequence is identical for frame
  times from 2 ms to 100 ms (reusing the harness of `test-net-v2-interp`).
- **`.ngp` bot line parser**: round trip, truncation, bad values.

### 8.2 Offline arena test (local, needs game data)

A debug-only command-line option, `-botarena <mission> <level> <nbots>
<seconds> [-fixedfps N]`, starts a netgame with no network peers. The host's
own ship is parked as a ghost spectator. The bots fight for the given time
and the test prints kills, deaths, accuracy per skill, stuck events, average
path length and bot CPU per second. It is not in CI (no hog files there), but
it is the main tuning tool:

- **Frame-rate check**: run the same seed at `-fixedfps 30` and `500` and
  compare the decision logs.
- **Skill ladder**: Insane beats Ace beats Hotshot, and so on, in 1-vs-1
  over 5 minutes. Target kill ratio between adjacent skills: 1.5–2.5 : 1.
  Anything steeper means the gaps are too wide.
- **Level soak**: every level of the user's custom missions for 2 minutes
  with 7 bots. The pass criteria are zero asserts, stuck events below 1 per
  bot-minute, and every bot scoring at least one kill.

### 8.3 Play-test checklist (real network game)

- A client sees bots move, fire and die exactly like humans: engine glow,
  muzzle flashes on the gun (the stage 2 carried-flash fix), death tumble,
  and eggs where the bot exploded.
- Kill feed and kill matrix agree on all machines after 50 bot kills.
- Join in progress with 3 bots: the joiner sees them at once, with correct
  scores and inventory.
- Full game: a joining human replaces a bot.
- In-game add and remove, and a skill change during a fight.
- Team anarchy: bots do not shoot teammates when friendly fire is off, and
  avoid it when it is on.
- A host at 30 fps and at 500 fps: the bots feel the same.
- The host's in-game menu open for 30 s: bots keep playing and the host's
  ship is vulnerable, as today.
- Bot names in the chat commands; `/kick:` on a bot.
- Trainee is beatable by a new player; Insane is hard for the group's best
  player but hittable.

---

## 9. Staged implementation plan

Each PR compiles with `-Werror`, keeps the game playable, and is one branch
and one PR per change.

| PR | Content | Depends on | Size |
|---|---|---|---|
| **B0** | Pilot refactor (§3.2) with no behaviour change: `struct pilot`, `apply_pilot_controls`, `pid`/originator parameters on the `multi_send_*` functions a bot needs, `send_data(..., originator)`, `choose_spawn`/`place_player` split. Human play unchanged. | — | M |
| **B1** | **First bot**: slot allocation at game start (count only, fixed Hotshot, default names). Bundle branch, bot tick, perception (LOS and FOV), A* nav with string pulling and stuck recovery, aim with lead and error, primary fire with spawn-granted weapons, damage to and from bots, kill/deres/reappear as the bot, respawn. Setup UI (§6.1–§6.3, decision 1): Bots item in the host setup menu, Bots screen, per-bot screen (name; skill and style fields present, presets filled in B2). No pickups or secondaries. `test-bot-nav`, `test-bot-brain` (solver, aim, reaction, tick). | B0 | L |
| **B2** | `.ngp` persistence, five skill presets, styles, `PLAYER_LIST` bot flag and `BOT` in the kill list, humans replace bots. | B1 | M |
| **B3** | Pickups and resources: `powerup_apply` for bots, collect and retreat goals, fuel centres, death drops from the bot's inventory, weapon choice tables, afterburner. | B1 and v2 stage 3 | M |
| **B4** | Secondaries and mines, dodge, strafe patterns, cloak/invul behaviour, converter. `-botarena` test mode. | B3 | M |
| **B5** | In-game bot menu, chat commands, add/remove during play, join in progress with bots (extras inventory until stage 5). Implemented: §9.11. | B2 | S |
| **B6** | Move to stage 4 authority: bots in the history ring, robot-style hit detection for bot shots, generic `PLAYER_KILLED`/`PLAYER_SPAWN`; delete the bot-specific kill path. Done with stage 4 (network-protocol-v2.md, "Stage 4 as implemented"): the host records its bots' positions every frame, a bot's hits are applied where the host shows the target, damage to a bot goes through `bot_take_damage` from the host's decision, and a bot's death and respawn are announced with `PLAYER_KILLED`/`PLAYER_SPAWN`. | v2 stage 4 | S |
| **B7** | CTF and hoard: goal segments (`fuelcen_check_for_goal` / `_hoard_goal` for bots on the host), flag and orb roles (attack/defend/escort), team coordination through a shared host-side blackboard. | v2 stage 6 | M |

B1 is shippable on its own for playtesting against "target practice" bots.
B1–B3 make a fun anarchy bot.

### 9.1 B1 as implemented

**Files.** `common/main/bot_vec.h` (the double-precision vector and frame
of the pure logic), `common/main/bot_nav.h` (graph, A*, string pulling,
path following, stuck detector, edge penalties), `common/main/bot_brain.h`
(tick layers, skill and style tables, the bot's RNG, intercept solver, aim
error, reaction delay line, memory, target scoring, steering and velocity
controllers, strafing, trigger discipline, the B1 primary choice),
`common/main/bot.h` (the game interface and the setup), `similar/main/bot.cpp`
(the bots on the host) and `similar/main/bot_menu.cpp` (the setup and its
menus). Tests: `test-bot-nav`, `test-bot-brain` and `test-bot-flight`
(§8.1; the `.ngp` parser comes with B2).

**Setup (§6.1–§6.3).** "Bots..." follows "Maximum players" in the host
setup menu; its label is `Bots: none...`, `Bots: 3 (Hotshot)...` (or
`(mixed)`), or `Bots: not in this mode` outside anarchy, team anarchy and
bounty. The Bots screen has the count slider (0 to max players − 1), the
default skill and style, one line per bot, "Set all bots to default skill"
(and style), "New random names" and "Done". It is rebuilt whenever the list
changes, so its lines always match the list: moving the count slider closes
the screen from its change event (newmenu now honours a close that a
callback requests from `newmenu_changed`, which no other menu does) and
`bots_setup_menu` reopens it with the new list; the item indices are taken
from the list as displayed. The per-bot screen edits the
name, skill, style and (in team modes) team, and removes the bot. Names are
stored lower case like every callsign. Not yet: the "Humans replace bots"
checkbox and the `.ngp` persistence (B2), the in-game screen (B5). The
developer switch `-bots N` sets the initial count of the setup menu.

**Every bot plays Hotshot in B1**, whatever its skill and style fields say
(decision 5); the presets and styles take effect in B2.

**Slots (§2.3).** `bots_allocate_slots` runs in `net_udp_select_players`
after the lobby closes (the host may start alone): each configured bot takes
the lowest slot below `max_numplayers` that has no player, no lobby
callsign and no connection (`host_slot_has_peer`: a lobby player left out of
the game is kicked with a 1 s linger, whose end would disconnect the slot;
the rule is `choose_bot_slot` in `bot_brain.h`, tested), gets a callsign
unique in the game (`havoc` → `havoc2`), `rank = None`, a zero address, and
`connected = playing`. If they do not all fit, the host is told. A slot
with a connection is never a bot (`bot_is_local`), and a bot is forgotten
(`bot_slot_released`) when its slot is disconnected (`multi_disconnect_player`)
or given to a human (`accept_peer`), so a joiner that takes a former bot's
slot is never flown by the host. The team
menu starts with the bots' team preferences. A game that does not start,
and the end of a session (`net_v2::session_reset`), frees the bot slots.

**Every host loop over slots (risk R2).**

| Loop | Bots |
|---|---|
| `host_begin_level_wait`, the level wait (`net_udp_request_poll` counts `Players`) | a bot slot is `playing` at once; otherwise the host waited for ever |
| kicks in `net_udp_send_sync`, `net_udp_select_players` (team menu abort), `net_udp_wait_for_requests` | skip bots (they have no address) |
| `host_send_level_start`, `host_end_level` (`KICK(endlevel)`), `send_endlevel_status`, the extras queue, `join_stalled`, the per-connection timeouts and ping statistics, `set_state_for_peer` | iterate `S.peers` and need a connection: bots are never touched |
| `build_common_bundle` | the bot takes the host's own branch (pose from the object, `dying` from its death state, ghost when dead), §2.2 |
| the score screen (`kmatrix`) waits for every player still in the level | `bots_level_end` sets the bots `end_menu` in `multi_endlevel_score` and `dispatch_table::end_current_level` |
| join in progress | the snapshot carries the bots' ships and scores; the extras' `net_objects_send_all_inventories` sends the host's copies, which are kept current (below) |
| admission | a bot slot is occupied and connected: never given to a joiner; a joiner with a bot's name is refused as a duplicate (humans replacing bots is B2) |

**Brain (§4).** The tick is a `tick_accumulator` at 60 Hz driven by
`GameTime64`, so a paused game pauses the bots; perception runs at 20 Hz
and strategy at 5 Hz, staggered by slot. Perception: LOS by fvi with
`FQ_TRANSWALL`, awareness radius, field of view (skipped for the player
that hit the bot in the last 3 s), cloaked players only within 40 units.
The reaction delay line holds one percept per perception tick; the tactics
layer dead-reckons the target from the state it saw a reaction time ago
(a straight flight is tracked, a turn is noticed late), then aims with
lead `ℓ` and the drifting aim error. Target choice with 20 % hysteresis,
revenge, weak targets and the bounty. The engagement keeps a 35–95 unit
band, closing in and backing off inside it, and jukes across the line of
sight (below, "Movement and combat fixes"). A hit tells the bot the
attacker's position ±20 units.

**Steering (deviation).** The design's `axis = angle_error / (max_turn_rate
× FrameTime)` depends on knowing the ship's turn rate and ignores its
rotational inertia. B1 asks physics for the ship's real response
(`compute_rotation_response` / `compute_thrust_response` in `physics.cpp`,
from the frame-rate-independent drag model) and uses a cascaded
controller: the wanted rate is proportional to the error (critically
damped for the ship's time constant) plus the rate at which the wanted
direction turns (feed-forward), capped at `turn_cap` × the ship's
maximum; the axis is the feed-forward plus rate feedback. It uses only the
current state, so it is frame-rate independent (the test settles a 90°
turn in the same time at frame lengths from 2 to 50 ms). The thrust axes
come from a velocity controller in all six directions, as a human flies.
The steering works in the frame without the turn roll, as
`do_physics_sim_rot` does.

**Navigation (§4.3).** At level start the host caches segment centres and
side centres and builds the child graph (cost through the side centre).
A* expands at most 4000 segments and returns a partial path toward the goal
otherwise. Passable: `WALL_IS_DOORWAY … fly`, or a door that is not locked
and whose key the bot holds. Path points: side centre, then segment
centre; the last point is the goal position when known. String pulling
tries the next 4 points with an fvi of 2/3 of the ship's radius, furthest
first. Stuck: less than 3 units of progress in 90 ticks; 0.5 s of backing
off with a random sidestep; the edge gets a 200-unit penalty; replan; after
3 failures a new goal. A wall probe along the velocity (0.4 s) pushes the
bot off walls. Goals: hunt (the target's segment, also while fighting it
in the open; replanned every 2 s, or after 0.5 s when the target moved to
another segment) and roam (a random segment at least 120 units away).
Doors open
through `bot_hit_wall` in `collide_player_and_wall`, the door part of
`wall_hit_process` without its HUD text.

**Weapons (B1).** Only the primaries the ship was granted at spawn (no
pickups yet): helix, plasma, spreadfire, then gauss or vulcan with
ammunition (first when energy is below 10), phoenix, the laser. Fusion
(needs the human's charge trigger) and omega (its charge model is the
human's) are not used. A switch costs `REARM_TIME`. `do_laser_firing_player`
runs the human's autoselect and the rapid fire cheat only for
`Local_pilot`; `do_laser_firing` sends `MULTI_FIRE` for a bot's ship with
the bot as originator. The trigger needs the aim within the fire cone, the
range and a clear line of fire: fvi with objects, blocked by a wall, a
teammate, the reactor (decision 6) or a robot, not by a shot in flight.
Bots have no homing weapon, missile or guided missile in B1, so the homing
target acquisition, the missile camera and the HUD are never theirs.

**Damage, death, respawn (§4.8, §7.1).** `apply_damage_to_player` hands a
bot's ship to `bot_take_damage` first, so a bot takes damage while the
host is dead. Friendly fire is judged by the victim's team
(`multi_maybe_disable_friendly_fire` takes the victim). The death starts
in the next frame, outside the collision handling, as the human's does:
`multi_send_kill` computes the kill on the host and sends
`MULTI_KILL_HOST` — from the host now for every victim (B0 sent it with
the victim as originator, which clients drop). The tumble lasts 2 s with
`MULTI_CREATE_EXPLOSION` fireballs as the bot (it was always sent as the
host). Then `multi_send_player_deres(deres_explode, bot)` first brings the
host's copy of the bot's inventory up to date and sends it; the host drops
the eggs from that copy (`net_objects_host_drop_player_eggs`), explodes the
ship and makes it a ghost. After 1–2.5 s (the bot's RNG) it respawns with
`choose_spawn` / `place_player` (the host's assignment with its
reservations, as for every spawn in a network deathmatch),
`multi_make_ghost_player` (spawn grants),
the spawn invulnerability (which the bot code also expires) and
`MULTI_REAPPEAR` as the bot. No respawn during the reactor countdown. A bot
killed during the countdown (D2 marks it `died_in_mine` with the kill)
still tumbles and explodes (deres, eggs, ghost), as a human does; its
brain stops.

**Inventory.** `net_objects_host_own_ship_inventory(pid, force)` keeps the
host's copy (`A.mirrors`) equal to the bot's ship and sends `INVENTORY`
when it changes (rate limited like a client's report; forced at spawn and
death). So death drops, joiners' extras and `MultiLevelInv` see the real
inventory.

**Messages.** `multi_send_fire`, `multi_send_player_deres`,
`multi_send_reappear`, `multi_send_cloak` and `multi_send_decloak` have no
`Player_num` default any more; every caller names the player.

**`/kick`.** `/kick <botname>` (or `#n`) removes the bot: a bot still
tumbling explodes first, otherwise the host's copy of its inventory is
brought up to date; then `net_v2::host_remove_player` disconnects the slot
(`multi_disconnect_player`: "has left the game", the eggs dropped from the
host's copy, the ghost, and `player_left` to the clients with reason
`kicked`) and the bot is forgotten. The setup keeps the bot for the next
game.

**Movement and combat fixes (after the v0.61-exp-8 playtest).** The first
playtest (one human against Hotshot bots, anarchy, 500 fps) found bots that
stayed near one spot, shaking, and practically never hit. The causes, found
by tracing the code and simulating the ship (`test-bot-flight`):

1. *The aim lagged every moving target.* The steering's wanted rate was
   proportional to the angle error only, so the ship trailed a target
   whose direction turns at ω by ω × τ (τ = 0.19 s, the ship's turn time
   constant): 9° behind a player strafing at 50 units/s at 60 units, at
   every frame rate. The Hotshot fire cone is 6°, so the bots almost never
   fired, and the rare shots went behind the target. Fix: the tactics
   layer computes the angular velocity of the line to the aim point
   (`line_of_sight_rate`, from the target's perceived velocity and the
   bot's own) and the controller adds it as feed-forward
   (`rotation_axis(…, feed_forward)`). Tracking error on the same target:
   0.1–1.7° from 500 to 30 fps (the simulation: 100 % of the time inside
   the cone, 0 % before).
2. *The steering errors were the direction's heading and pitch angles*,
   whose heading swings by up to 180° for a direction near straight up or
   down (the heading of a point just above the nose flips sides with a
   tiny motion): the ship shook left and right under or over its target.
   Fix: the errors are the components of the shortest rotation onto the
   direction (angle × unit axis), continuous everywhere but straight
   behind, where the preferred side is kept.
3. *The fight stood still.* Inside the 35–95 unit band the approach speed
   was zero and the strafe went straight left and right at 0.6 × top
   speed, reversing every 0.4–1.2 s; the ship (0.47 s to reach speed) only
   swung to and fro. In anarchy every bot is every other bot's enemy, so
   bots met each other, stopped and shook, and (item 1) could not kill
   each other. Fix: `juke_state`: each run of the strafe takes a new
   direction across the line of sight at least 90° from the last (with a
   vertical share, `strafe_vertical`: Hotshot 0.5, Ace 0.8, Insane 1) and
   a new preferred distance in the band; `combat_velocity` closes in or
   backs off to it (at most 0.8 × top speed) and strafes at 0.7 × top
   speed. Run lengths: Trainee 1.2–2 s, Rookie 0.9–1.8 s, Hotshot
   0.6–1.4 s, Ace 0.5–1.3 s, Insane 0.4–1.1 s. In the simulation a Hotshot
   fighting a still target covers 130 × 75 × 60 units in 30 s at half the
   top speed on average, keeps 45–90 units, and has the target in its
   fire cone all the time.
4. *The bot stopped dead whenever it had a target but no clear shot yet.*
   A direct engagement dropped the path; until the reaction delay had
   passed (280 ms at Hotshot), whenever the line of fire closed, and
   whenever the target left the field of view, the tactics layer had no
   path and wanted zero velocity. Fix: the hunt path to the target's
   segment is kept (and replanned) during the fight, and the bot chases
   along it in those moments.
5. *Wall avoidance braked at every bend.* The probe along the velocity
   (0.4 s) meets the wall of every corridor bend the path is about to
   take, and each hit pushed back at half the top speed for 150 ms. Now a
   wall beyond the path point the bot steers at is ignored while it follows
   its path and flies toward that point (velocity within 40° of the
   direction to it; `wall_hit_is_bend`), and a hit takes away the speed
   into the wall (more when near) plus a push of 0.1 × top speed.
6. *The thrust axes were taken in the frame without the turn roll*, but
   `apply_pilot_controls` applies them in the rolled frame: a sideways
   command leaked into vertical by the roll angle (up to about 20°). Now the
   rotation uses the unrolled frame and the thrust the rolled one
   (`steer_controls`).
7. *Small axes were biased at high frame rates.* `fixmul(axis, FrameTime)`
   truncates toward minus infinity; at 500 fps (FrameTime 131) that is a
   bias of half a step (0.4 % of full thrust) toward negative. The held
   time is now rounded. (Not a cause of the shaking: the physics carries
   its remainders, and the simulation flies the same path at 30 and 500
   fps within 4 %.) The rounding is `held_axis_time` (bot_brain.h), which
   the flight test's ship model uses too.
8. *The stuck recovery could outlive the moment.* It was counted down
   only while the bot followed its path; a fight in the open (a clear
   shot) does not follow it, and a replan to the same segment did not end
   it, so the bot could fly at full speed along the recovery direction
   into walls for the whole fight. Now the 0.5 s run out on the tactics
   tick whatever moves the bot (`stuck_detector::tick_recovery`), and
   combat movement or a new path ends the recovery at once.

Also checked and found correct: the rotation and thrust signs (positive
pitch lowers the nose, positive heading turns right, as
`vm_angles_2_matrix` composes them), the units (`rotvel` in revolutions
per second, the physics' steady states from `compute_*_response`), the
controls applied every frame through `apply_pilot_controls` (not only on
the tick), the fire path (`bots_fire` → `do_laser_firing_player` with the
bot's pilot and fire timer, `MULTI_FIRE` as the bot), the line-of-fire
check, and the roam goals (a random segment; arrival 1.5 ship radii).

Added with the fixes: roam goals at least 120 units away with a
neighbour (`pick_roam_goal`, tested for spread and reachability), and a
first dodge (§4.6): at 20 Hz each projectile within 150 units is judged;
if it will pass within the ship's radius + 3 in the next 0.7 s, the bot,
with the skill's `dodge_prob` (Hotshot 0.45), thrusts across its flight,
away from where it passes, for 0.35 s after half its reaction time. Each
projectile gets exactly one roll, however heavy the fire: the roll is a
hash of the projectile's signature and a salt the bot draws once per life
(`dodge_roll`), not an entry in a list of judged projectiles. The bot's
own shots are not dodged, nor, with friendly fire off, a partner's
(teammate, or anyone in cooperative; `shot_worth_dodging`).

Presets unchanged (§5.1): a Monte Carlo of the aim (reaction delay with
dead reckoning, lead 0.7, σ 2.8°, a target juking at 35–58 units/s every
0.3–1.2 s, laser speed 120) gives Hotshot about 50 % hits at 40 units, 30 %
at 60 and 10–15 % at 90, and much more against a target that flies
straight: the reaction delay, not the lead, is what a human beats. The
tests: `test-bot-flight` (the steering and a model of the Pyro-GX with
the game's control quantisation and drag model at 30/60/144/500 fps:
turns without overshoot or hunting, including straight up and just above
and behind; a strafing target tracked inside the fire cone, and not
without the feed-forward; a waypoint course at the same pace at every
frame rate without weaving; a fight that moves around), `test-bot-brain`
(the errors near the vertical, the feed-forward, the line-of-sight rate,
the jukes, the combat velocity, the dodge with its shot filter and one
roll per projectile, the bend exception, the trigger) and `test-bot-nav`
(the roam goals, the stuck recovery's expiry).

**Not in B1:** pickups, fuel centres and energy (a bot that runs dry can
only fire ammunition weapons until it dies; B3), secondaries, afterburner,
the full dodge (homing missiles, the nearer wall; B4), wall, force
field and lava damage to bots, triggers, the presets and styles, the
`PLAYER_LIST` bot flag, `.ngp` persistence and humans replacing bots
(B2), adding and removing bots in game (B5).

### 9.2 B3 as implemented

**Files.** `common/main/bot_goals.h` (pure: powerup values by need, the
goal choice, the map knowledge and the memory of powerups, the weapon
table, the long range trigger rule, the afterburner rule), `nav_distances`
in `common/main/bot_nav.h` (bounded Dijkstra: path cost and segment count
to everything within reach), `similar/main/bot.cpp` (the bots' side),
`net_objects_bot_touch` / `net_objects_bot_can_use` in
`similar/main/net_objects.cpp`, one branch each in
`collide_player_and_powerup` (`collide.cpp`) and `do_cloak_stuff`
(`game.cpp`). Tests: `test-bot-goals` (new) and `test-bot-nav`
(`nav_distances` against the reference Dijkstra, the cost bound, the node
limit, the segment counts).

**Pickups (§4.7, v2 §6.2 and "Stage 3 as implemented").** A bot's ship
that touches a powerup on the host (`do_physics_sim` finds it, as it does
for any moving ship) goes through `bot_touch_powerup` →
`net_objects_bot_touch`: the host's copy of the bot's inventory is set from
the ship (`inventory_mirror::assign`: the ship is the truth, no grant is in
flight), then `host_decide` judges it exactly as a client's request (object
still there and of that type, the bot alive and not dropped, in range, not
spat by it within 2 s, and `evaluate_pickup`: not full, not "already
have"), and `host_grant_remote` applies it to the copy and the ship,
removes the object or leaves a cannon with its remaining rounds, and sends
`PICKUP_GRANT` with the bot's life counter. The range check takes the bot's ship position, not the
interpolation ring (a bot has none; the ghost snapshot of its last death
would have denied every later pickup). Nothing is requested and nothing
waits, as for the host's own ship. What `do_powerup` does besides the
inventory is done for the bot: the cloak's time and `MULTI_CLOAK`, a real
invulnerability's time (`FakingInvul` cleared), a full afterburner charge;
only then does the bot's `INVENTORY` go out (`bot_touch_powerup`), so the
clients never see the cloaked flag before the cloak's start, nor a faked
invulnerability for a real one. Keys are taken as a human takes them in a multiplayer game (they stay).
Death drops need nothing new: `multi_send_player_deres` brings the copy up
to date and the host drops from it, so what a bot picked up (missiles
included) is dropped where it dies. When a bot's cloak runs out
(`do_cloak_stuff`, `bot_cloak_expired`) or its real invulnerability
(`life_frame`), the host sends `MULTI_DECLOAK` and puts the item back into
the level (`maybe_drop_net_powerup`), as the human's game does for its own.
The respawn bookkeeping counts a bot's items from its ship like any
player's (it is `playing` and not a ghost until its drop); the host's
accounting log (network-protocol-v2.md, "Stage 3 as implemented") shows
bots as `bot` in its grant, drop and count lines, and
`test-net-v2-authority` covers a bot carrying the only earthshaker, its
death drop and a bot racing a client for one powerup.

**Fusion and omega are picked up** (decision; since section 9.5 they
are also fired): a bot still cannot fire them (B1), but taking them denies them to the others and a death drops
them, as a human's would. They are worth little (1, a spare), so a bot
takes one only when nothing better is near. Headlight and full map are
never sought (value 0) but taken when flown through, as a human takes
them; a bot never switches the headlight on.

**Knowledge (§4.7, decision 4).** Each strategy tick (5 Hz) the bot
computes the path cost and segment count from its segment to everything
within 2500 units (at most 3000 segments), with the passability and
penalties of its own paths. It then learns: the level's initial powerups
(level net ids) within `map_knowledge` segments (Trainee none, Rookie 3,
Hotshot 8, Ace 15, Insane the whole level); powerups it sees (awareness,
field of view, line of sight through grates; at most 6 line checks per
tick); and a new powerup appearing (a respawn, a death's drop) within its
hearing radius during its first second. It remembers what it learned
(`powerup_memory`: position, type, rounds) and forgets an entry when it
sees the place empty or comes within 15 units of the empty place, when it
takes it, and
after `30 s + 3 × memory` (Hotshot 45 s). The memory holds 96 powerups;
when it is full, a powerup known only from the map is not learned (else a
level with more powerups would churn the set every tick), and one seen or
heard pushes out the oldest entry that is not being ignored (an unreachable
powerup's 5 s or 10 s ignore survives). So a bot may fly to a powerup
someone else took, as a human does, but it never knows about a respawn it
did not see or hear. Fuel and repair centres are known to every bot (they
are the level's geometry).

**Values and goals (§4.1, §4.7).** Values (`item_value`): shields
0.5–4 by need (need 0 at 100 shields, 1 at 20), energy 0.3–3.3 by need
(half the need with a vulcan or gauss that has rounds), a primary that
raises the bot's armament (its best weapon in the mid band) by more than
0.2 is worth 5, a spare 1, one it has 0; its own cannon's rounds 1–3 by how
empty it is; laser, super laser 3 while they make the bot stronger, quad 4
(else 2); missiles 1 (smart, mega 1.5, earthshaker 2, mines 0.8);
afterburner 2, cloak 2.5, invulnerability 4, converter 0.8, ammo rack 0.6.
Only what the rules let the bot take counts (`net_objects_bot_can_use`,
the same `evaluate_pickup`), so a bot never flies to what it would bounce
off. Collection utility = value × 60 / (60 + path cost). The goal is the
highest utility, the current goal counting 20 % more: roam 0.2; engage
(target in sight) 2 × target score × engage weight; hunt the same for a
remembered target; collect × collect weight, × 0.35 while an enemy is in
sight unless the powerup is within 40 units or the style is Collector;
refuel likewise; retreat 3–5 when threatened (a target known, or hit in
the last 3 s), not invulnerable, and below the style's retreat shields.
Retreat goes to the best known shield source (a shield powerup, a repair
centre) that does not lie toward the threat, else to the place of ten drawn
that is furthest from the threat for the least flying; the place is kept
until reached. Collecting, retreating or refuelling, a bot that sees an
enemy aims and fires at it but flies its path (backward, when it retreats
facing its pursuer); only engage and hunt use the combat movement of B1.
A powerup it reached without taking, or could not reach (stuck three
times), is no goal for 5 s (10 s).

**Fuel and repair centres.** A bot flies to a centre when that is its best
goal (value of the need over the path), hovers in the segment, and stays
until full (100), unless an enemy comes into sight while it has 40 or more.
`refuel_frame` gives it what `object_move_one` gives the local player (25
per second up to 100; the sound is played on the host only).

**Weapons (§4.5).** `choose_primary_for`, at 5 Hz with the range band of
the target (close < 60, mid 60–150, far > 150; without a target the mid
band). Scores per band: helix 3.7/3.5/1.2, spreadfire 3.5/2.2/0.6, plasma
3.2/3.8/2.0, gauss 3.0/3.3/3.6, vulcan 2.4/2.6/2.8, phoenix 2.0/2.4/0.8,
the laser 1.0/1.0/0.8 × its level (1, 1.25, 1.5, 1.75, super 2.3, 2.6) ×
1.3 with quad. So close range prefers helix, spreadfire, super laser with
quad; mid range plasma, helix, super quad, gauss; far range gauss, vulcan,
the lasers. Energy: nothing but vulcan and gauss fires below 1; below 20
the energy weapons count half (the laser 0.8), between 20 and 50 they are
spared a little; vulcan and gauss need rounds. The current weapon counts
15 % more, so a switch (which costs `REARM_TIME`, as before) happens only
for a clear gain. Below weapon smarts 2 (Trainee, Rookie) the fixed order
of B1 stays. (The energy rule, the laser's close range row, the margin
and the value of a better weapon were revised after the exp-12
playtest, section 9.3.) Far band trigger discipline: the bot holds fire when the
chance of a hit (the target's radius against the spread of the aim error
and of half the target's lateral motion during the shot's flight) is below
10 %.

**Afterburner (§4.7).** `want_afterburner`: lit above 30 % charge and kept
down to 5 %, only while the wanted velocity is at least half the top
speed and within 25° of the nose (the afterburner is full forward thrust),
for these reasons by skill: Rookie when hunting a target more than 150
away, Hotshot also when retreating, Ace also when dodging, Insane also on
long straight legs (more than 100 units to the steer point) while roaming
or collecting. The ship's own afterburner code drains and recharges the
charge from the bot's `pilot`; the host drops the trail's blobs
(`MULTI_DROP_BLOB` as the bot) and plays and sends the afterburner sound
(`MULTI_SOUND_FUNCTION` as the bot), and stops it when the bot dies, the
level ends, or the bot is kicked or its slot released (the loop is linked to
its ship object, which stays as a ghost). All bots still play Hotshot (B2 applies the presets), so in
this stage they burn when chasing and retreating.

**Unchanged:** the human's pickups on host and clients, non-bot games and
single player (every new branch asks `find_bot`, which is empty there),
the protocol (no new message; `MULTI_PROTO_VERSION` unchanged).

**Not in B3:** secondary use, the converter, cloak/invulnerability tactics
(B4); the presets and styles taking effect (B2); CTF/hoard items (B7).

### 9.3 After the v0.61-exp-12 playtest: weapon choice and aim

The playtest (one human against Hotshot bots) found bots that play much
better but (a) still mostly fire the laser, (b) never fire missiles (B4,
section 9.4) and (c) hit badly with the slow projectile guns (plasma,
phoenix, helix, spreadfire) compared with the laser, vulcan and gauss.

**(a) The laser.** Traced end to end: the pickups are right (a granted
primary sets the ship's `primary_weapon_flags` through
`host_grant_remote` → `write_inventory`; a cannon comes with its rounds),
the choice is applied (`choose_weapon` writes the ship's
`Primary_weapon`, which `do_laser_firing_player` fires; the human's
autoselect runs only for `Local_pilot`), and nothing else writes a bot's
`Primary_weapon`. The causes were in the scores:

1. *Every life starts with the laser, and a better weapon was not worth a
   detour in a fight.* A better primary was worth 5 whatever it replaced,
   and with an enemy in sight the collection counts 0.35: plasma lying
   80 units away scored 5 × 60/140 × 0.35 = 0.75 against an engagement's
   1–2. Bots die often and fight most of the time, so they fought each
   life with the spawn laser. Now an armament item is worth 3–7 by how
   much stronger it makes the bot (`upgrade_ratio`, `upgrade_value`:
   3 + 2 × (ratio − 1), capped at 7; the spawn laser to plasma is 7), and
   one that makes it at least 1.5 times stronger (`BIG_UPGRADE_RATIO`)
   counts 0.8 in a fight (`goal_inputs::collect_upgrade`). The bot shoots
   at what it sees on its way (section 9.2).
2. *The laser was spared by the energy rule.* Between 20 and 50 energy
   every energy weapon but the laser lost up to 25 %, below 20 half (the
   laser 20 %): with a strong laser the bot kept it after 15–20 s of fire.
   Now each energy weapon is judged by the seconds of continuous fire its
   own cost leaves (`energy_factor`: the game's `energy_usage /
   fire_wait`, helix twice in multiplayer, filled per weapon by
   `energy_rates` in bot.cpp): full for 8 s and more, down to one half
   with nothing left.
3. *The hysteresis kept the held laser.* The spawn weapon is always the
   current one first, and the current one counted 15 % more: the super
   lasers with quad (3.4 at close range) were within 15 % of spreadfire
   (3.5) and helix (3.7), so the close range order never took effect. The
   laser's close range row is now 0.9 and the margin 12 %. The margin was
   there against flipping at a band border, which the fights (35–95
   units) straddle at 60: the band itself now has a hysteresis of 8 units
   (`band_of(distance, previous)`), and the test checks for every pair of
   weapons that a switch at the border is not undone.

**(c) Aim with slow shots.** Traced per weapon: the speed is the weapon
data's `speed[Difficulty_level]` of the current primary, which is what
`Laser_create_new` gives the shot (in multiplayer the netgame's
difficulty); a shot does not inherit its shooter's velocity (only mines
do), so the intercept with the target's velocity in the world is right;
`speedvar` (a random slower shot) is the data's. The causes:

1. *A systematic under-lead.* The lead used the fraction ℓ itself (Hotshot
   0.7) of the target's velocity: 30 % of the shot's flight short, a miss
   of 0.3 × the target's speed × the flight time. That grows with the
   flight time, so it hit the slow shots hardest. Against a ship crossing
   at 35–58 units/s, 60 units away, B1's Hotshot hit with 20 % of its
   laser shots (speed 120), 2 % at speed 80 and 79 % at 300; at 90 units,
   5 %, 0.2 % and 47 %. Now the lead factor is 1 on average with an error
   drawn from N(0, (1 − ℓ) × 0.35), held for the aim drift period and
   eased toward (`aim_lead`, like the aim error); Trainee (ℓ = 0) still
   does not lead. The same ship at 60 units: 82 % at 120, 67 % at 80,
   91 % at 300 (`test_hit_rates_by_speed`, which requires every speed to
   hit at least 70 % (below 100 units/s) or 80 % as often as the laser).
   Against a ship that strafes in new directions every 1.5–3 s the slow
   shots stay behind (at 60 units: 15 % at 80, 29 % at 120, 54 % at 300),
   as they do for a human: the ship has more time to turn away.
2. *The lead was solved from the ship's centre*; the shots leave from
   their guns and fly parallel to the nose. Now from the average of the
   weapon's guns (`gun_local`: guns 0 and 1 for laser, plasma, phoenix,
   fusion, 0–3 with quad, the centre gun for vulcan, spreadfire, gauss,
   helix).
3. *The pattern weapons had the single shot's cone.* Spreadfire and helix
   fire patterns 1/16 and 2/16 off the nose; their fire cone is wider by
   half the pattern's half angle (`fire_cone_with_spread`).
4. Weapons with thrust (the missiles) are led with three quarters of
   their top speed (`effective_shot_speed`; they start at half of it). The
   reach (speed × lifetime × 0.8) no longer has a floor of 60 units that
   could exceed it (now 20).

### 9.4 B4 as implemented

**Files.** `common/main/bot_weapons.h` (pure: the role of each
secondary, the skill that uses it, the missile choice, the blast safety,
the release, the converter rule, the cloak and invulnerability tactics,
the homing dodge), `similar/main/bot.cpp` (`missile_tick`, the firing in
`bots_fire`, `convert_frame`, the tactics in `think`, the dodge),
`similar/main/laser.cpp` (two small changes, below). Test:
`test-bot-weapons` (new).

**Firing (sections 4.5, 7.1).** Everything goes through the human's
path: `bots_fire` calls `do_missile_firing(bot's pilot, weapon, ship)`,
which creates the missile or mine from the bot's gun, takes the round,
puts a fired missile back into the level (`maybe_drop_net_powerup`, as
for a human on the host), applies the mega and earthshaker recoil, and
sends `MULTI_FIRE` with the bot as originator (bombs with their object
number, homing missiles with their target), so the clients see exactly a
human's shot. A weapon whose `fire_count` is above 1 fires its further
rounds frame by frame, as the human's `Global_missile_firing_count`
does. In `do_missile_firing` the rapid fire cheat and the secondary
autoselect now apply to `Local_pilot` only (as `do_laser_firing_player`
does for the primary): the host's cheat never speeds up a bot, and a bot
has no autoselect (it chooses its missiles itself).

**A bot's mines are filed under the bot.** `multi_send_fire` maps a bomb
(or guided missile) it sends with `map_objnum_local_to_local(objnum,
pnum)`, under the originator: for a human that is `Player_num` as
before; for a bot flown on the host it is the bot, the owner the clients
file their copy under (`map_objnum_local_to_remote(..., pnum)`). So a
homing target, an object removal or the late joiner's snapshot that
names a bot's mine resolves to the same object on the host and the
clients (it was filed under the host before).

**Homing for a bot.** `Laser_player_fire` picked a homing missile's
target (`find_homing_object`) only for `ConsoleObject`; for any other
shooter it took the target from the `MULTI_FIRE`. A bot's missile on the
host therefore flew without a target until the missile's own rescan.
Now the shooter flown on this machine, the local player or a bot on the
host (`bot_is_local`), picks its target at launch, as a human's missile
does; the target goes to the clients in the `MULTI_FIRE`.
`find_homing_object` itself was never tied to the local player.

**Choice and release (`missile_tick`, 60 Hz).** The bot either waits for
the release of the missile it chose or chooses one
(`choose_secondary`); a chosen missile that cannot be released within
1.5 s is dropped. A missile that flies straight (concussion, mercury,
flash, mega, earthshaker) is aimed with its own speed (three quarters of
its top speed, it accelerates) from its own guns while it waits. Rules,
in order:

| Weapon | Rule |
|---|---|
| Smart mine, proximity bomb | the bot flies away (retreat, collect, refuel) at more than 15 units/s with its target seen within 1 s behind it (more than 107° off its flight) within 100 units, or 150 at a doorway on its path (the next path point is a side's centre); the smart mine first; one per `mine_interval` (Hotshot 3 s); never with a teammate following (team game, friendly fire on: a teammate in sight behind the bot within 40 units, or within 150 flying towards it) |
| Earthshaker | a clear shot, 110–260 units, at least 2 blast radii + 12 away |
| Mega | a clear shot, 70–220 units, at least 1.5 blast radii + 12 away |
| (both) | not cloaked, at most one per 8 s and one per target per 25 s; below Ace only at a target crossing slower than 30 units/s |
| Smart | 30–120 units, a clear shot, or the target out of sight but seen within 1 s (its children find it round a corner); a target in sight needs the clear shot, as for the others |
| Flash | a clear shot within 100 units at a target that faces the bot (within 30°) |
| Homing | 40–200 units, a clear shot, the target crossing faster than 25 units/s or further than 90 |
| Mercury, then concussion | 30–200 units, a clear shot |
| Homing | as the fallback of the straight ones |
| Guided | never (decision below) |

Missiles go at most one per `missile_interval` (Rookie 4 s, Hotshot 2.5
s, Ace 1.8 s, Insane 1.2 s). A missile is released when the nose is
within its cone (the fire cone; homing 20°, smart 30°: they find the
target themselves) and the first wall along the nose, or the target (or
another enemy ship first on the line of fire) if nearer, is far enough
for its blast (`blast_safe`: 2 blast radii for the
earthshaker, whose children burst around the impact, 1.5 for the mega, 1
for the others, plus 12 units). The blast radius is the weapon data's
`damage_radius`. So a bot never fires a mega or an earthshaker at point
blank, nor into a wall next to it (the user's own death by an earthshaker
fired near a wall), whatever the target's distance. An invulnerable bot
only avoids point blank (30 units), and only while the invulnerability
covers the danger: it must be real (not the spawn's faked one, which
the first hit ends) and last longer than the missile's flight to the
impact (at half its speed for a thrust missile), plus 2 s for the
earthshaker's children and 0.5 s spare (`blast_danger_seconds`);
otherwise the full rule applies.

**Skill (section 5.1).** Trainee fires no secondary; Rookie concussion,
homing, flash and mercury; Hotshot and better all but the guided missile.
Decision: the design gave mega to Ace and the earthshaker to Insane, but
the presets are not applied before B2 and every bot plays Hotshot, so
Hotshot uses them too, under the rules above (a slow target, once per
target, the blast safety); Ace fires them at fast crossers as well.

**The guided missile is never fired** (decision): its steering, its
camera and its release are the local human's
(`Guided_missile`, the missile view, `release_local_guided_missile`), and
a guided missile that nobody steers is a slow concussion. It is still
picked up, for 0.3 (denied to the others, dropped at death).

**Dodge (section 4.6).** A homing missile whose target is the bot's ship
turns after it, so its straight flight is only a rough prediction: it is
judged with 3 times the pass radius and dodged with the skill's
probability + 0.25 (`dodge_radius`, `dodge_chance`). The Ace
afterburner rule for dodging already covers the burn.

**Converter (section 4.7).** As the human's key works it
(`transfer_energy_to_shield`: only the energy above 100, 20 a second, two
for one), without the HUD text and the local sound: Rookie below 50
shields, Hotshot below 80, Ace below 100, Insane below 110.

**Cloak and invulnerability (section 4.7).** Invulnerable (not the
spawn's faked one), the bot engages 1.6 times as readily, collects half as
readily and fights at 0.6 of its distance band (retreat was already off).
Cloaked, it engages 1.3 times as readily, fights at 0.7 of its band, and
holds its primary fire beyond 90 units and every missile beyond 100 (the
shots show where it is), and fires no heavy missile.

**Unchanged:** the human's firing (the cheat and autoselect are the
local pilot's as before, the homing target is picked as before for the
local player), clients (every new branch asks `bot_is_local`, which is
false there), the protocol.

**Not in B4:** the `-botarena` test mode, the strafe patterns beyond
B1's jukes, mines at chokepoints the bot is not passing itself, dodging
mines.


### 9.5 After the v0.61-exp-13 playtest: pickups, guns, big missiles, turning

The playtest (one human against Hotshot bots) found: (1) no bot ever
fired an earthshaker or a mega (nor fusion or omega), even one that had
just picked an earthshaker up; (2) bots switch weapons but still mostly
fire the laser; (3) bots fly past powerups, even close ones; later also
(4) bots are easy to kill: they do not react to shots from behind and
turn round slowly, standing still; (5) on a level whose spawn area is a
dense core inside large rings, bots never leave the core.  Nothing here
could be run with game data, so each cause was traced in the code, and
the bots now write what they decide to the log (below).

**Pickups: the path from "a powerup near the bot" to "granted".**

- *The touch and the grant are right* (confirmed by reading):
  a bot's ship moves by `do_physics_sim` (bots are not interpolation
  driven), whose object sweep finds powerups for any player
  (`check_vector_to_object`: ship radius + powerup radius);
  `collide_player_and_powerup` hands a ship that is not the local
  player's to `bot_touch_powerup`; `net_objects_bot_touch` judges it as a
  client's request and `host_grant_remote` writes the grant into the
  bot's own ship (`write_inventory`, the object `missile_tick` and
  `bots_fire` read).  So the earthshaker the user dropped did reach the
  bot's `secondary_ammo`.  No `Player_num` guard skips a bot's touch.
  `A.dropped` (set at death) is cleared by `multi_send_reappear` for bots
  too.
- *Knowledge starved* (confirmed, `learn_powerups`): a powerup had to be
  within the field of view to be seen, and at most six lines of sight
  were checked per strategy tick, in the order of the object slots.  The
  level's powerups behind walls (low slots, within the awareness and the
  field of view, never seen, so checked again at every tick) used the
  checks up; a death's drop or a respawn (high slots) was never checked.
  Now the candidates are checked nearest first (eight per tick), and a
  powerup within `POWERUP_NOTICE_DISTANCE` (60) is noticed outside the
  field of view.  The map knowledge (Hotshot: 8 segments) was counted in
  segments only: in a spawn area of many small segments it did not reach
  the rooms round it; it now also reaches 40 units of path per segment
  (`MAP_KNOWLEDGE_UNITS_PER_SEGMENT`, Hotshot 320 units).
- *Goal lower* (confirmed, the utilities with real numbers,
  `test_pickup_scenarios`): a Hotshot bot with the spawn laser fighting
  an enemy 100 units away engages at 1.85 (x 1.2 for the current goal).
  Plasma 20 or 80 units away wins (5.25, 3.0 with the upgrade rule of
  section 9.3), if known.  But an earthshaker 20 units away scored 1.5,
  a concussion pack or energy the bot needs less, and a powerup beyond
  40 units counted a third in a fight: the bot flew past them.
  *Opportunistic pickup* (`grab_worthwhile`, `grab_applies`): a powerup
  worth at least 0.75 within 45 units (path 70) is taken whatever the goal
  (utility `GRAB_UTILITY` 4), unless the bot is in danger (it would
  retreat); shields are grabbed in danger too.  It flies there as a
  collecting bot does, shooting at what it sees.  Shields at 60 and below
  now count as much in a fight as a big upgrade (`collect_in_fight`).
- *Steering to it*: the path's last point is the powerup's centre, the
  approach speed `3 d + 8`, arrival at 2 units, far inside the touch
  radius (ship + powerup, about 7 units): not a cause.

**(2) The laser.**  The choice and the firing are right (section 9.3;
`choose_weapon` writes the ship's `Primary_weapon`, which
`do_laser_firing_player` fires for the bot's ship; no code resets it but
the spawn, after which the choice runs at once).
`test_owned_weapon_selection` checks that a bot holding the laser that
owns any one other gun takes it in every band where the table ranks it
higher, at 100, 60 and 15 energy.  The laser preference was the pickups
(a bot that dies often fights each life with the spawn laser) and fusion
and omega, which bots did not fire at all.

**(1) Heavy missiles** (confirmed, `choose_secondary`): the earthshaker
needed 110 units and the mega 70, both at a target crossing slower than
30 units/s, while a fight keeps 35-95 units and a human strafes faster:
the rules practically never held.  Now (`heavy_check`, which also names
the failing rule for the log):

| | B4 | Now |
|---|---|---|
| Earthshaker | 110-260 units, 2 blast radii + 12 | 55-260 units, 1.2 blast radii + 12 |
| Mega | 70-220 units, 1.5 blast radii + 12 | 45-220 units, 1 blast radius + 12 |
| Crossing target (below Ace) | < 30 units/s | any speed if the missile homes (the data's `homing_flag`); else its crossing during the flight within 0.8 blast radii, or < 45 units/s |
| Interval, per target | 8 s, 25 s | 5 s, 10 s |

*Measured on the user's tight level "Earth Shaker"* (`eshaker.rl2`, 250
segments; parsed outside the repository): 8710 sampled engagements
(random points in two segments with a line of sight, 25-260 units
apart; quartiles 62, 106, 140 units) against the rules, by blast radius
(the game's data is not here; the log prints it).  At the fight's 35-95
units B4 allowed an earthshaker in none of them for any radius from 40
to 80, a mega in 43 % at radius 40 and none from 60 (before its
crossing rule, which blocked most of the rest).  The first revision
(1.5 and 1.2 blast radii) still allowed an earthshaker in 0-43 %.  Now:
radius 40: earthshaker 60 %, mega 82 %; 50: 43 %, 50 %; 60: 18 %, 43 %;
80: 0 %, 2 %; with the standoff (35-140 units) 78/90, 69/73, 55/69 and
35/47 %.  The missile's own blast never reaches the bot (the damage is 0
beyond its radius; the unit test sweeps radii, distances and the speeds
of the bot and of the target).  The earthshaker's children are not
bounded by this rule: they do not collide with the ship that fired
them, so one flying back passes through the bot and bursts on the wall
behind it (see *Review fixes* below).

The blast does no damage beyond its radius (the damage falls linearly to
0 at `damage_radius`), so the bot stays outside it; the release still
checks the wall along the nose (the user's earthshaker death).  A bot
with a heavy missile ready keeps the distance it needs
(`heavy_standoff`: the combat band starts there), so it gets its shot.
A homing heavy missile is released within 15 degrees.  The blast is
judged at the impact along the nose (the target when it is nearer than
the wall behind it: a target 120 units down a corridor is a shot, a wall
30 units ahead never) and where the bot is when the missile bursts: its
own flight toward the impact meanwhile is subtracted, and so is the
target's toward the bot when the target is the impact (a target rushing
the bot at v meets the missile at d s / (s + v)); flying away is
credited up to 20 units/s, for either (`distance_at_burst`).  Only the
earthshaker needs free distance in another direction: behind the bot
(below).

**Fusion and omega are fired** (decision; B1-B4 only picked them up).
Fusion is charged by the bot as `FireLaser` charges the human's (2
energy, then 1 a second, `Fusion_charge` on the bot's ship), from when
the target is in sight and in range, and released through
`do_laser_firing_player` when the aim is on the target and the charge is
the skill's (`fusion_release_charge`: Hotshot 1 s), at the latest at 1.8
s (from 2 s the charge hurts the ship).  The warm-up sound is heard on
the host only.  Omega recharges from the bot's energy with its own
`pilot` (`omega_charge_frame`); `do_omega_stuff` now takes the charge
of a bot's shot on the host as it does the local player's (before, a
bot's omega would have fired without charge).  A bot also does not fire
or choose omega without the charge for a shot (`omega_can_fire`: an
eighth of the charge, or some charge and no energy; `omega_factor` is
0 below it): the host would delete the shot while the clients, told of
it by `MULTI_FIRE`, drew a full discharge.  Its fire cone is its lock
cone (18 degrees), its reach 72 units.  Table rows: fusion 3.6/3.5/1.4,
omega 3.9/1.6/0 (x its charge).

**(4) Survival.**
- *Hit from behind* (`react_to_hit`): the attacker's place was learnt
  and the bot turned after the target choice (up to 200 ms), the
  reaction time and a turn at the skill's rate; meanwhile it did
  nothing.  Now an unseen attacker makes the bot thrust across the line
  of fire at once (0.6 s), take the attacker as target, and turn to it a
  reaction time later; weak, it then retreats (the goal choice).  A
  teammate's hit (friendly fire on) is no attack: no evasion, turn,
  target or memory of it.  The projectile dodge never had a
  field-of-view filter.
- *Turning round* (`steer_errors_local`, `keep_moving_in_turn`): the
  errors were the shortest rotation, which for a target behind and above
  (or straight behind) turns one axis only; the ship turns each axis at
  its own top rate, so both at once are up to 1.41 times as fast.
  Beyond 115 degrees the errors blend to both axes at full.  And a bot
  that wanted to hold its place (no path, the fight's band) braked to a
  stop while it turned (`velocity_command` of nothing is full reverse
  thrust): now, more than 60 degrees off, it slides across the line of
  sight at 0.6 of its top speed, the way it already moves.  In the flight
  model a 180 degree turn at Hotshot takes 1.4 s instead of 1.6-1.8 s
  and the ship keeps above 25 units/s (`test_turn_round`).
- The aim (error, reaction, lead) is unchanged: Hotshot matches
  section 5.1; the harder presets come with B2.

**(5) Leaving the spawn area.**  The user's level (Schwarzbrenner
Outpost, `VLR08-03.RL2`: 272 segments, 8 spawn sites, 105 powerups, a
central structure with spokes to a ring of 124 segments at 200-250
units) was parsed outside the repository and the bots' own navigation
code (`nav_distances`, `astar_search`, `pick_roam_goal`) run on its
graph, with the passability of `edge_passable`.  Found:

- *Two of the eight spawn sites are sealed cells* (segments 79 and 119:
  9 segments each; their exits are closed walls that a one-shot "open
  wall" trigger on a wall switch opens, the switch being shot by a
  human).  A bot never shoots switches: spawned there, every plan
  failed and it stayed for the life.  Now the host judges the sites at
  level start (`spawn_site_open`: a quarter of the level or 150
  segments reachable) and a bot respawns elsewhere: `choose_bot_spawn`
  leaves the sealed sites out before the ranking and the draw of
  `choose_spawn` (all of them only if none is open) and draws with the
  bot's own random numbers, so bot code neither reseeds nor advances
  the game's `d_rand` (a human's spawn is unchanged); one placed in a
  cell at level start is moved.
- *The map knowledge did not reach the ring*: counted in segments
  (Hotshot 8), the bots in the central structure knew 19-22 of the 105
  powerups, 0-3 of the 55 out in the spokes and ring; with the path
  distance (`MAP_KNOWLEDGE_UNITS_PER_SEGMENT`) 23-41, up to 10 of them.
  The central structure holds 44 powerups, the nearer and known ones: a
  bot collected there and fought there.
- The roam itself was not the cause here (B1's draws went to the ring
  60-76 % of the time from the open sites), but 10-13 % of them were
  out of reach (the cells, a locked door), a strategy tick lost each.
  Exploring (`pick_explore_goal`): 24 draws within reach of the path
  costs, the best by the path cost (capped at 900) and the time since
  the bot was there (full after 90 s; each bot marks the segments it
  passes).  On this level the farthest places by path are the top of
  the central structure, reached only round through the ring; on a
  synthetic dense core inside a ring (`test_explore_core_and_ring`) the
  bot visits most of the ring, while B1's roam left the core in a
  fifth of the draws, mostly into the corridors.

**The log (-verbose, in gamelog.txt).**  Nothing is formatted without
`-verbose`.  Once a second per bot:

    bots: 'havoc' goal=engage 1.85 (next collect 1.50, collect 1.50) tgt=P#0 84u vis clr | plasma [laser1,plasma sec=conc2,shaker1] | sh=87 en=64 | pu=shaker 24u v2.0 goal-lower | heavy=blast min=80 keep=88

the goal and its utility, the best other goal and the collection's
(`grab` when the grab rule holds), the target (distance, in sight,
clear shot), the primary and what the bot has, shields and energy, the
nearest powerup worth at least 0.75 within 120 units with its value and
why it is not taken (`collecting`, `not-known`, `ignored`,
`unreachable`, `cannot-use`, `goal-lower`, `denied:<reason>`), and the
heavy missiles' last verdict (`fire`, `none-owned`, `skill`,
`no-target`, `not-visible`, `no-clear-shot`, `cloaked`, `cooldown`,
`used-on-target`, `too-fast`, `too-close`, `too-far`, `blast`, and once
chosen `aiming`, `nose-blast` or `wall-behind`) with the distance it needs and the
distance it keeps.  Each change of the heavy verdict while the bot has
one is logged with the target's distance, crossing and closing speeds,
the distance needed, the blast radius and whether the missile homes.
Events: each pickup (`takes powerup N (granted)`
with the inventory after it) or denied touch (`denied (gone | dead |
range | spat | cannot-use | not-arbitrated | no-netid)`, once a second
per powerup), each weapon switch with the band and scores, each heavy
missile and fusion shot, each reaction to an unseen attacker, each roam
goal (explore or random, path, last visit).

**Review fixes (PR #34).**
- *A target rushing the bot*: the blast rule took only the bot's own
  speed toward the impact; a target flying at the missile meets it
  sooner and nearer (at 90 units a still bot, the target at 60 units/s:
  51 units, inside an earthshaker's 66).  Now `distance_at_burst` takes
  both closing speeds (`missile_situation::target_closing_speed`, and at
  the release the target's when a ship, not the wall, is the impact);
  `test_closing_target` sweeps both speeds against the real meeting
  point.
- *After the release*: the cooldown dropped the standoff to 0, the band
  fell back to 35-95 units, the juke rerolled and the fight could fly
  the bot toward its own burst.  Now the bot keeps the missile's
  standoff (`blast_hold`) and does not close in on the target until the
  missile's flight and blast are over (`blast_danger_seconds`).
- *The earthshaker's children* (not bounded by the rule above): an
  earthshaker is released only with the wall behind the bot at least
  half a child's blast radius (the weapon data's `children`) plus 12
  units away (`shaker_behind_safe`, verdict `wall-behind`; waived while
  a real invulnerability outlasts the danger); a partial bound, kept
  small for the tight levels (deliberate risk and indirect fire come
  later).  A bot backing off to a standoff does not back to within 25
  units of a wall behind it.
- Spawn sites, omega without charge and a teammate's hit: above.

**Unchanged:** the human's firing, pickups, spawn and omega (the charge
rule applies to the local player as before), clients, the protocol.

### 9.6 Heavy missile tactics: risk, corners, hugging

The user plays the tight level "Earth Shaker" a lot: there "it is
almost impossible to avoid getting a suicide every now and then,
because if you see an enemy and fire they will usually hug your ship to
make the explosion take you out as well.  Shooting towards edges and
corners is essential in this level."  The rules of sections 9.4 and 9.5
never accept any self-damage, so on such levels the bots hardly fired
an earthshaker or a mega.  Now (`bot_weapons.h`, the game side in
`bot.cpp`: `plan_heavy`, `missile_tick`, `notice_heavy_holders`,
`find_duck_point`, `fvi_geometry`):

**1. The expected outcome replaces the safety gate** (heavy missiles
only; the others keep `blast_safe`).  For a shot along a direction the
bot works out where it bursts and what that does (`evaluate_burst`):

- *The burst*: the first wall along the aim (`geo.cast`), or the target
  if the aim is at it and it is nearer (it meets the missile sooner when
  it flies at it, as `distance_at_burst`).  The flight takes the
  distance over half the missile's speed (a thrust missile).
- *The places at the burst*: the bot's own flight meanwhile (away
  credited up to 20 units/s, as before); the target's half of its flight
  for a wall burst, the burst itself when the missile meets it.  Each
  with an uncertainty: the target's 3 units plus 0.6 of its crossing
  during the flight (0.15 for a missile that homes: the data's
  `homing_flag`), plus the aim error times the distance; for a target out
  of sight 12 units per second since it was seen; the bot's 2 units plus
  a fifth of its own speed during the flight.
- *The blast* does what the game does (`object_create_explosion_with_damage`):
  the strength at the centre, falling linearly to 0 at `damage_radius`,
  and only to what sees the burst (`object_to_object_visibility`,
  through grates: `geo.sees`).  So a bot round a corner from its burst
  is safe, and a target round a corner is not hit.  The expectation
  over the uncertainty is a fixed 6-point quadrature
  (`expected_blast_damage`), and so is the chance of any damage
  (`blast_chance`).
- *The earthshaker's children* (6, `NUM_SMART_CHILDREN`; they home on a
  player they see from the burst within 150 units, `MAX_SMART_DISTANCE`,
  never on the ship that fired and never collide with it): if the
  target is seen from the burst, half of them are counted as reaching
  it (at the target: next to the bot if the target hugs it); the rest
  fly off at random and burst on the walls round the burst.  These are
  probed in the 12 directions of an icosahedron and in the cone of
  directions that pass within a child's blast of the bot (weighted by
  its share of the sphere): such a child passes through the bot and
  bursts on the wall behind it (this replaces `shaker_behind_safe` for
  bots, which remains for reference).
- Real invulnerability that outlasts the danger (`blast_danger_seconds`)
  makes the bot's damage 0.

The rule (`judge_blast`), in order: never at point blank (the impact
or the burst distance nearer than 30 units, invulnerable or not; the
burst distance is where the bot is when the missile bursts, the impact
less the closing of both during the flight, as `distance_at_burst`); never when the blast
without the uncertainties would kill the bot (`lethal`: an almost
certain suicide); at least a fifth of the blast's damage (up to the
target's shields) to the target (`low-value`); the expected
self-damage within the budget and the chance of any within the
profile's (`risky`); the expected damage to the target (up to its
shields, plus 30 for a likely kill) at least `trade` times the
expected self-damage (`poor-trade`).

**Risk profile** (`risk_profile_of`), from the bot's skill and style as
stored in its settings (`bot_config`, section 6.3; since B2 every bot
plays its own skill and style, section 9.7):

| Style | Budget (share of shields) | Chance of any self-damage | Trade | Hug | Standoff |
|---|---|---|---|---|---|
| Balanced | 0.12 | 0.20 | 2.5 | 0.45 | 1.0 |
| Aggressive | 0.30 | 0.40 | 1.5 | 0.80 | 0.8 |
| Cautious | 0.02 | 0 (the strict rule, with the uncertainty) | 8 | 0.15 | 1.2 |
| Collector | 0.06 | 0.10 | 4 | 0.25 | 1.1 |

Skill scales the budget and the chance (Trainee 0.3, Rookie 0.6,
Hotshot 1, Ace 1.25, Insane 1.5), the trade (1.3, 1.15, 1, 0.9, 0.8)
and the hug (0, 0.3, 1, 1.15, 1.3).  Indirect fire from Hotshot (the
skill that uses heavy missiles).  The standoff scales the distance the
bot keeps with a heavy missile ready (`heavy_standoff`) and after its
release (`blast_hold`).

**2. Indirect fire** (`aim_candidates`, `choose_heavy_aim`).  Besides the
target itself: the wall right behind it (its risk is also the risk of
the missile meeting the target on the way); where a fan of rays from
the bot round the line to the target meets a wall within the blast
radius of the target (8, 16 and 28 degrees, 8 spokes: the edges and
corners next to it); where probes from the target in the icosahedron's
directions meet a wall the missile reaches.  A target out of sight but
seen within `CORNER_SEEN_WITHIN` (3 s) is aimed at from its last known
place: the fan also takes the point where the line of sight to it
breaks, and for an earthshaker the reach is 60 units (its children find
the target from a burst that sees it).  Candidates more than 50 degrees
off the line to the target, or nearer than 30 units, are left out.
Each is weighed as above; the best favourable one by its value less
`trade` times its self-damage wins (the direct shot gets 15 % extra:
the easiest aim).  An aim at a wall while the target is in sight may
meet the target on the way (`may_meet_target`): a homing missile turns
to a target within its homing cone (`HOMING_MIN_TRACKABLE_DOT`, 0.75),
any missile meets a target within 10 units of its line plus the
target's crossing during the flight.  Such an aim bears the direct
shot's outcome too, the worse of both (`merge_meet`: the self-damage,
its chance, the impact and the burst distance; the value stays the
aim's).  As mega and earthshaker home, a wall aim next to a target in
sight is then rarely better than the direct shot: indirect fire is
mostly the corner shot at a hidden target, and the wall behind a
target out of the cone.  The weighing runs at the strategy rate (5 Hz) while
the bot may fire (a target, no cooldown, not cloaked, not yet hit on
this target); `heavy_check` takes its verdict (`m.heavy_risk`) instead
of the distance rules (`too-fast`, `too-close`, `blast`), and an aim at a
wall or a corner needs no clear line to the target.  The bot then turns
to the aim point (the steering aims at a point; with the aim error of
its skill), holds its place for a corner shot, and releases when the
nose is within the missile's cone and the outcome *along the nose* is
favourable (verdict `nose-blast` otherwise).  At the release the scene
is the current one (the plan may be a strategy period and the pending
time old): along the nose, and for a wall aim that may meet the target,
also the direct shot with the current distance and closing speeds, the
worse of both.

*Objects on the line.*  An indirect aim needs no clear line to the
target, so the line to the aim point is checked with the objects
(`aim_line_clear`, fvi as `shot_line_clear`): a teammate, the reactor,
a robot or clutter first on it refuses the aim (`no-clear-shot` at the
release); so does another enemy ship nearer than the blast radius plus
the margin (the burst would be near the bot); the target on it is the
meeting above.  In the weighing only an indirect aim that would become
the best is checked (one fvi call, usually); refused, the next best
wins (the log counts them).

*Cost.*  The weighing is bounded (`aim_search`): per plan half the fan's
spokes and half the probes from the target, alternating between plans;
the last best indirect aim weighed again; of the other indirect
candidates (near duplicates within 3 units merged) the 8 most promising
by a cheap estimate (the burst near the target, away from the bot).
The level's segments are looked up once per weighing (`fvi_geometry`'s
cache), `find_point_seg` starting from the nearest point already known
(the bot's, the target's, where casts ended) instead of scanning the
level.  All bots share an fvi budget of 300 calls per brain tick (60
Hz): a due weighing waits for the next tick when it is spent (an
overdue one may use up to twice it), a release check likewise.  On
the Earth Shaker level (below) a weighing costs 205 fvi calls for the
earthshaker and 49 for the mega on average (the unbounded search: 519
and 107; before this review: 524 and 108), so seven bots holding both
weigh about 9000 calls per second (before: 22000), under the budget's
18000.  The shot goes through the
normal firing path as the bot (section 9.4); `laser.cpp` is unchanged.

**3. Hugging, both ways.**
- *(a) Towards an enemy with a heavy missile*: a bot knows that an
  enemy holds one only as a human does: it saw it fire one (a mega or
  earthshaker in flight from that enemy, within the awareness and in
  sight) or pick one up (a heavy powerup next to the enemy in sight, 14
  units, is gone at the next look), until the enemy dies or as counted below.
  The bot counts them (`heavy_holding`): a pickup one more, a shot one
  less (each missile once, by its signature); its last known one fired,
  the enemy is forgotten; a shot not seen picked up means it may hold
  more, for 10 s; a pickup is remembered 25 s.
  An enemy so known that faces the bot (30 degrees) at 25-160 units is
  hugged with the profile's chance, drawn once per target (`want_hug`),
  unless the bot's own heavy missile is usable within 2 s
  (`heavy_usable_soon`: held, cooldowns ending, not cloaked, not used
  on this target; it keeps its standoff then) or it is weak (below its
  style's retreat shields): the fight band becomes 8 units to
  `hug_distance` (a third of the blast radius, 14-22 units), inside the
  enemy's own blast.  It keeps hugging while the enemy is within 160
  units and its own missile is not usable soon.  One mode at a time: a
  hugging bot does not duck, a hug or its end holds 1.5 s (no
  flapping), and a hug ends when the enemy is out of sight for 1.5 s
  (it is re-weighed while the enemy is hidden).
- *(b) The bot with the heavy missile*: its standoff makes it back off
  (as before).  A target that closes in within the standoff (flying at
  the bot, or within 0.6 of it, or with a wall close behind the bot)
  while no aim is favourable makes it duck out of sight
  (`want_duck`): of the segment centres up to three segments away,
  15-90 units off, ranked by the distance from the target less half the
  distance from the bot, the first of six that the bot can fly to in a
  straight line and the target cannot see (`pick_duck_point`); it flies
  there for up to 2 s facing where the target will come from.  Out of
  sight, the corner shot of (2) applies; the target coming round the
  corner is a direct shot at the standoff again.  A hugging target
  close to a wall can also be caught by a favourable indirect aim (1).

**The blast radii** are still unknown here: the game code has no
constants for them (`Weapon_info` comes from the HAM/HXM data:
`damage_radius`, `strength`, `children`), and the data is not in this
environment.  The bots read them from `Weapon_info` at the game's
difficulty (`missile_data_of`: blast radius and damage, the children's
radius and damage, their count), so the rules follow the real values.
The log prints them at every heavy shot:

    bots: 'havoc' fires shaker at P#0, 84 units, aim wall (impact 96; expected damage to it 180, to itself 2, shields 100; blast 60 damage 200, homing; children 6 blast 48 damage 100)

The evaluation below sweeps the radii instead.

**Movement while engaged** (`engaged_movement`): one mode per tick, the
fight (`combat_velocity`: the band, strafing, standoff, blast hold, the
wall behind) with a clear shot and no path goal, else the path, and the
duck overriding both while it lasts.

**Measured on "Earth Shaker"** (`eshaker.rl2`, 250 segments, parsed
outside the repository; the side planes and walls of each segment, a
ray walker like fvi's, and the pure functions of `bot_weapons.h`
themselves).  2000 fights with a line of sight, 20-140 units apart
(quartiles 56, 83, 112), random speeds (bot up to 35 units/s, target up
to 55, random directions), 100 shields each, Hotshot aim error; 1000
targets out of sight 25-100 units away (seen 1 s ago).  Per shot fired,
20 realisations of the places (the model's uncertainties) and of the
scattered children's directions (random), counting any self-damage and
self-kills.  Assumed data: mega 150 damage, earthshaker 200, its children
100 at 0.8 of its radius, both homing.  Blast radius mega 50 /
earthshaker 60:

| Profile | Earthshaker allowed (9.5 rule) | of which indirect | Corner shot at a hidden target | Shots with self-damage (9.5 rule's shots) | Suicides per shot |
|---|---|---|---|---|---|
| Cautious Hotshot | 19 % (40 %) | 0 % | 5 % | 3.9 % (3.0 %) | 0.00 % |
| Balanced Hotshot | 43 % | 0 % | 7 % | 4.7 % | 0.01 % |
| Aggressive Hotshot | 59 % | 0 % | 9 % | 5.6 % | 0.07 % |
| Aggressive Insane | 65 % | 0 % | 10 % | 7.3 % | 0.05 % |

| Profile | Mega allowed (9.5 rule) | of which indirect | Shots with self-damage |
|---|---|---|---|
| Cautious Hotshot | 71 % (60 %) | 0.2 % | 0.1 % |
| Balanced / Aggressive Hotshot | 78 % | 0.3 % | 1.4-1.5 % |
| Aggressive Insane | 79 % | 0.3 % | 1.7 % |

The bounded search of the game gives the same shares as the unbounded
one within half a point.  After the review the model counts the
homing missile meeting a target in sight on the way to a wall aim
(before: 42/60/72/83 % earthshaker, 94-99 % mega, about a fifth of it
indirect, with up to 8.1 % of the shots hurting the bot; those
indirect shots were really direct ones and are no longer taken), and
the point blank rule applies to the burst distance too (198 rejections
instead of 153 of 2000).  The self-damage of a cautious bot comes from
the earthshaker's scattered children (the main blast: 0.0 %), as much
as with the 9.5 rule; a shot that would put its own blast on the bot
is never taken.  Other radii (earthshaker allowed for cautious /
balanced / aggressive Hotshot / aggressive Insane, self-damage of the
aggressive Insane shots): mega 40, earthshaker 50: 26/55/70/75 % (9.5
rule 53 %), 6.5 %; mega 60, earthshaker 80: 11/22/39/47 % (9.5 rule
22 %), 8.0 %.  Missiles that do not home: 20/44/59/66 % earthshaker
(4-7 % of it indirect), 74-82 % mega (27-30 % indirect), 8.3 %.
Suicides stay below 0.2 % of the shots for every profile and radius.
In play the cooldowns (5 s between heavy missiles, 10 s per target)
bound the rate.  Ducking: within the standoff without a favourable aim
in 30-61 % of the earthshaker fights (the more the more cautious); a
place to duck to (three segments) in about 20 % of those; otherwise the
bot backs off as before.

**Log (-verbose).**  The heavy verdict line gains the weighing: how
many aims were weighed, favourable, indirect favourable, and the best
one's kind (`direct`, `behind`, `wall`, `corner`); new verdicts
`lethal`, `risky`, `poor-trade`, `low-value`.  The summary line gains
`risk=<budget>/<trade>` and ` hug`, ` duck`.  Events: `saw P#n fire a
heavy missile (k more known[, forgotten])`, `saw P#n pick up a heavy
missile (k known)`, `hugs P#n` / `stops
hugging`, `ducks out of P#n's sight` / `finds no cover`, and the shot
line above.

**Tests** (`test-bot-weapons`): the profiles' order by style and skill;
point blank never for any profile, the lethal blast never,
invulnerability (point blank still out, the rest harmless while it
lasts); the edge of the blast accepted by an aggressive bot and not by a
cautious one; the poor trade and the low value; a target rushing the
bot; the children behind the bot and at a hugging target; on synthetic
box levels: a corridor (the wall behind and the walls round the target
are candidates; indirect ones favourable), an L-shaped corner (the
earthshaker's corner shot finds the hidden target, the mega's does not
reach; a child flying back makes it too risky for a cautious bot unless
the wall behind is far), a hugging target (only an invulnerable bot
fires, into the wall, not at point blank; after the review: not at
all, the missile would meet the target on the way); `heavy_check` with
the weighing; hugging and ducking decisions and the duck point.  After
the review: the movement mode per tick (`engaged_movement`: the duck
overrides, the fight needs a clear shot and no path goal); the point
blank rule on the burst distance of a rushing target; the meeting of
the target by a homing and a straight missile, `merge_meet`; the
bounded search (subsets, the cap, the last best aim) and a refused
line (a corner shot refused leaves no aim); the enemy's missiles
counted and forgotten; `heavy_usable_soon`; the hug held, then left for
the standoff, dropped out of sight.

**Unchanged:** the human's firing and every human path (all changes are
in the bots' code; `laser.cpp` untouched), non-bot games, clients, the
protocol; the other missiles' rules.

### 9.7 B2 as implemented

**Files.** `common/main/bot_brain.h` (the final skill and style tables,
the style helpers, `fight_advantage`, `fire_burst`),
`common/main/bot_weapons.h` (the duck tendency in `risk_profile`, the
mine interval scale), `common/main/bot_profile.h` (new, pure: the bot
lines of the `.ngp`), `common/main/net_v2_session.h` (the `PLAYER_LIST`
bot flag, the admission with bots), `similar/main/bot.cpp` (each bot's
own presets, removal for a human), `similar/main/bot_menu.cpp` (the
screens, the persistence glue, the slot flags), `net_v2.cpp` (the flag
on the wire, the replacement), `playsave.cpp` (the `.ngp`), `gauges.cpp`,
`gamerend.cpp`, `kmatrix.cpp` (the marker). Tests: `test-bot-presets`
(new) and `test-net-v2-session` (the flag, the admission with bots).
`MULTI_PROTO_VERSION` and `NET_V2_PROTO_VERSION` are 104.

**Each bot plays its own skill and style.** B1-B4 flew every bot with
the Hotshot preset (the risk profile of §9.6 already read the bot's
settings). Now `bot_state::apply_config` takes the skill, the style and
the risk profile from the bot's setup line at creation and at every
respawn, so bots of different skills and styles play in the same game.
Every parameter reads them: reaction, aim error and drift, lead, turn
cap, fire cone, field of view, awareness, hearing, target memory,
dodge, weapon smarts (primary table from Hotshot, secondaries, missile
and mine intervals, fusion charge, converter), afterburner rule, map
knowledge, powerup memory, strafe (runs, vertical share, speed), the
trigger's duty, and from the style: retreat threshold, engage and
collect weights, range band, hunt memory, dodge bonus, mines, strafe
and closing pace, afterburner chase distance, heavy-missile risk budget,
trade, hug and duck. Hotshot Balanced plays exactly as before (every new
factor is 1 there, and a full trigger duty draws no random numbers).

**Skill presets (final).** Monotonic from Trainee to Insane in every
row (`test_skill_monotonic`):

| Parameter | Trainee | Rookie | Hotshot | Ace | Insane |
|---|---|---|---|---|---|
| Reaction delay | 550 ms | 400 ms | 280 ms | 200 ms | 140 ms |
| Aim error σ (per axis) | 7° | 4.5° | 2.8° | 1.7° | 1.0° |
| Aim drift period | 0.6 s | 0.5 s | 0.4 s | 0.3 s | 0.25 s |
| Lead accuracy ℓ (error (1 − ℓ) × 0.35) | 0 (no lead) | 0.4 | 0.7 | 0.9 | 1.0 |
| Turn-rate cap (× ship max) | 0.45 | 0.6 | 0.75 | 0.9 | 1.0 |
| Fire cone | 12° | 9° | 6° | 4° | 3° |
| Field of view (half angle) | 45° | 60° | 70° | 80° | 90° |
| Awareness radius | 150 | 250 | 350 | 450 | 600 |
| Hearing radius | 0 | 80 | 150 | 250 | 350 |
| Target memory | 2 s | 3 s | 5 s | 7 s | 10 s |
| Dodge probability | 0 | 0.2 | 0.45 | 0.7 | 0.85 |
| Weapon smarts | 0 | 1 | 2 | 3 | 4 |
| Primary choice | fixed order | fixed order | range table | range table | range table |
| Secondaries | none | concussion, homing, flash, mercury | all but guided (smart, mines, mega, shaker) | all | all |
| Missile / mine interval | — | 4 s / no mines | 2.5 / 3 s | 1.8 / 2.5 s | 1.2 / 2 s |
| Heavy missiles: indirect fire, risk scale, hug scale | no, 0.3, 0 | no, 0.6, 0.3 | yes, 1, 1 | yes, 1.25, 1.15 | yes, 1.5, 1.3 |
| Fusion release charge | 0.5 s | 0.6 s | 1.0 s | 1.3 s | 1.5 s |
| Converter below shields | never | 50 | 80 | 100 | 110 |
| Afterburner | never | chase | + retreat, long legs with a reserve | + dodge | + long legs sooner |
| Strafe (§9.12: key runs, key thrust, share of runs with an up/down key) | none | runs 0.4–1.0 s, 0.7, 10 % | 0.25–0.7 s, 0.95, 20 % | 0.22–0.65 s, 1.0, 20 % | 0.2–0.6 s, 1.0, 22 % |
| Trigger duty (`fire_burst`) | 0.55 | 0.8 | 1 | 1 | 1 |
| Map knowledge | 0 | 3 seg. (120 u) | 8 (320 u) | 15 (600 u) | whole level |
| Powerup memory | 36 s | 39 s | 45 s | 51 s | 60 s |

Trainee is meant to lose to a new player: slow and imprecise, no lead,
no strafe, no dodge, no missiles, no afterburner, no map knowledge, a
narrow view, and a trigger that pauses between bursts (0.5–1.1 s
bursts, pauses making 55 % duty; every life starts with a burst; new
in B2, `fire_burst`). Insane is
hard but no aimbot: 140 ms reaction (the tactics layer still aims at
where it saw the target 140 ms ago), a 1° aim error and a 3° fire
cone, and a dodge chance of 0.85 at most (0.95 with Cautious). No
preset has a zero reaction or a zero aim error (`test_skill_extremes`).

**Styles (final).** Multipliers and offsets on the skill:

| | Balanced | Aggressive | Cautious | Collector |
|---|---|---|---|---|
| Retreat at shields (§9.12) | 45 | 30 | 65 | 50 |
| … when outgunned (advantage < 0.6) | 45 | 30 | 90 | 65 |
| Engage / collect weight | 1.0 / 1.0 | 1.5 / 0.6 | 0.8 / 1.2 | 0.7 / 1.8 (keeps collecting in sight of enemies) |
| Engage weight when behind (advantage ≤ 0.5) | × 1 | × 1 | × 0.8 | × 0.5 |
| Fight band (35–95 units) | × 1 | × 0.75 | × 1.25 | × 1 |
| Hunts a lost target (× memory) | 1 | 2 | 0.8 | 1 |
| Dodge probability | + 0 | + 0 | + 0.1 | + 0.05 |
| Mine interval | × 1 | × 2 (fewer) | × 0.7 | × 1 |
| Strafe pace / closing pace (§9.12: thrust of the keys, the closing key 0.9 × this) | 1 / 1 | 0.9 / 1.15 | 1.1 / 0.85 | 1 / 1 |
| Afterburner chase beyond | 150 | 100 | 200 | 150 |
| Heavy missile budget, chance, trade (§9.6) | 0.12, 0.2, 2.5 | 0.30, 0.4, 1.5 | 0.02, 0, 8 | 0.06, 0.1, 4 |
| Hug / standoff (§9.6) | 0.45 / 1.0 | 0.80 / 0.8 | 0.15 / 1.2 | 0.25 / 1.1 |
| Ducks within (× standoff) or closing faster than | 0.6, 5 u/s | 0.35, 15 u/s | 0.9, 3 u/s | 0.75, 5 u/s |

The *advantage* (`fight_advantage`) is the square root of the bot's
shields over its target's times the square root of its armament over
the target's (`armament_score`, the best primary in the mid band),
bounded to 0.1–10: 1 is an even fight. It is judged at each strategy
tick for the current target (the host's numbers, as a human judges an
opponent by its ship, its shots and how hurt it looks). A dodge chance
of 0 stays 0 whatever the style, so no style makes a Trainee dodge.
`test_style_goals` checks the goal choice for the same situation per
style: at full shields everyone but the collector engages (the collector
takes the powerup), at low shields the cautious and the collector retreat
while the balanced and the aggressive fight on, outgunned the cautious
bot breaks off, and behind in a fight the collector goes for a small
prize it would ignore in an even one.

**Setup UI (§6.1–§6.3).** The host setup menu's label shows `Bots: 3
(Hotshot, Balanced)...`, or `(mixed)` when the skills or styles differ
(its buffer grew to 48). The Bots screen has the count, the default skill
and style, the checkbox "Humans replace bots when full", one line per bot
with its skill and short style (`1. ravager   Ace     Aggr`; styles Bal,
Aggr, Caut, Coll), "Set all bots to default skill/style", "New random
names" and "Done". The per-bot screen (name, skill, style, team in team
modes, remove) lost B1's "every bot plays Hotshot" subtitle. The host's
console names each bot's slot with its skill and style.

**Persistence (§6.5).** The setup is saved in the pilot's `.ngp` whenever
the host setup menu closes (with the other netgame settings) and read
when it opens: `BotCount`, `BotDefault=skill,style`, `BotReplace`,
`Bot<i>=name,skill,style,team`. The pure part (`bot_profile.h`) writes
the lines and parses them; the three numbers of a bot line are read from
the right, so a name may hold a comma; a bad or missing line below the
count is ignored (the bot then gets the file's `BotDefault` skill and
style, in whatever order the lines come, and the next built-in name),
the count is bounded
to 7, lines beyond the count are dropped, names are cut to 8 characters,
unknown keys stay the profile reader's. A profile of an older build has no
bot lines and leaves the setup as it was (no bots, or `-bots N`); the
`-bots N` switch still sets the count of the first setup of the session.
`test_profile` round-trips mixed setups, all combinations and a profile
with gaps, and checks the bad values and the line length (under the
reader's 50).

**The `BOT` marker (§2.2, decision 2).** Bit 7 of each slot's `connected`
byte in `PLAYER_LIST` marks a bot (network-protocol-v2 §4.5); the
protocol is 104 on both constants. The host keeps the flag per slot
(`player_is_bot`) from the bot's allocation until a human takes the slot
(`accept_peer`), so a departed bot's line still shows it; clients take
it from every `PLAYER_LIST` and clear it at `PLAYER_JOINED`. Shown: `BOT`
instead of the ping in the kill list's ping column and in the netgame
info table (`show_netplayerinfo`, the pause key in a netgame), `[B]`
after the name on the score screen (`kmatrix`; before it when the column
has no room), and `, Bot` after a bot's name tag in the view. The session reset clears the flags.

**Humans replace bots (§2.3, decision 3).** `decide_admission` takes the
host's option. A joiner who finds the game full (no free slot below the
limit, no departed bot's slot) replaces the most recently added bot
still playing (`bot_to_replace`: the highest order of addition, below
the player limit): the host removes it as a player who quits
(`bots_remove_for_human`: a tumbling bot explodes first, else the host's
copy of its inventory is brought up to date; `multi_disconnect_player`
drops its eggs, makes the ship a ghost, prints "has left the game" and
sends `PLAYER_LEFT(quit)`), then admits the human into the slot as a new
player (scores zeroed by `new_player`; in team games the slot's team).
A bot leaves before a disconnected human's slot is handed out, so a
human who dropped can rejoin their slot while bots play; with the option
off (or no bot playing) the slot of the human disconnected the longest
is taken, as before. A closed game stays closed. A bot's slot is never
rejoined by callsign: a human with a departed bot's name is a new player
(the slot is reused as a free one, without the bot's scores); one with a
playing bot's name replaces that bot only when the game is full and the
option is on, and is otherwise refused as a duplicate (so that cycling
names cannot strip bots from a game with room). A slot handed to a new
player (a replaced bot's, a departed player's or a free one) is marked
until that player's `CLIENT_READY` (`S.awaits_entry`): a client that
restarts during its join comes back by callsign as a rejoin, and
`admission_is_new` still admits it as new (scores reset by the snapshot
and `new_player`, "joined" rather than "rejoined"), so it never inherits
the bot's or the departed player's score. *Decision:* a replaced bot stays out for the rest of the
game, even if a slot frees up (simplest, and no bot pops back in while
humans come and go); the setup keeps it, so the next game has it again.
`test_admission_with_bots` covers the order, the option, the closed game,
free and departed-bot slots first, bots before disconnected humans, the
departed bot's name, the playing bot's name (full game or not) and the
limit; `test_admission_is_new` the restart during a join.

**Unchanged:** non-bot games and human play (only the protocol number
changed; a human's `connected` byte is the same), clients' behaviour
apart from the marker, the setup menu's layout apart from the label.

**Not in B2:** changing a bot's skill or style during a game (the in-game
Bots screen is B5; `apply_config` is ready for it), per-bot parameter
overrides in the `.ngp` (§5.1 mentions them for tuning; not needed so
far), the `-botarena` ladder that would measure the kill ratios between
adjacent skills (§8.2).

### 9.8 After the v0.61-exp-16 playtest: volleys, death dump, power-up phase, turns, marker

The playtest (the user hosting alone against bots of every skill) found:
(1) the `BOT` marker did not show for the host; (2) even Insane bots
were too careful with rockets; (3) light missiles must come in volleys,
heavy ones must not be spammed; (4) bots flew past a mega missile a
short detour away; (5) humans fire their missiles off just before they
die, so the killer does not collect them; (6) the 180 degree turn was
still awkward; (7) the aim with lasers and gauss at Insane is good (no
change); (8) bots dogfight with the spawn laser instead of first
collecting weapons.  Files: `bot_weapons.h` (volleys, death dump,
intervals, risk scales), `bot_goals.h` (values, grabs, the power-up
phase, third parties), `bot_brain.h` (the turn), `bot_profile.h` (the
marker), `bot.cpp`, `gauges.cpp`, `kmatrix.cpp`.

**(1) The marker.**  The host sets its flags when it places the bots
(`bots_allocate_slots`, after the session reset) and nothing on the host
clears them but a human taking the slot; its pause table showed `BOT`.
The kill list, what one reads in play, marked a bot only in its ping
column, which is shown only with "Show Player ping" (off by default):
so neither host nor clients saw a marker there.  Now every bot's line in
the kill list says so: with the ping column, its `BOT` there; without
it, a grey `*` right after the name (the name is cut to leave it room,
`kill_list_marker_room`); not in the team view (`kill_list_marks_bot`).
The first version put ` BOT` after the name; the PR #38 review found
that the name column is narrow (about 40 units, about 22 with a kill
goal or a time limit and in the co-op left column), so a bot's name was
cut to three or four letters or to nothing, and with the ping column
the marker showed twice.  The `*` costs one narrow character in every
layout (full screen, cockpit, status bar; one or two columns of up to
eight players; kill goal and co-op score columns).  The score
screen (`kmatrix`) says `BOT` instead of `[B]`; the pause table and the
name tags (`, Bot`) are unchanged.  The host re-asserts the flag of each
bot it flies every frame (`bots_frame`), whatever might clear it.

**(2, 3) Missiles.**

*Volleys* (`volley_size`, `volley_continues`): "A single homing has no
impact.  A fleet of 3-5 of them make an opponent run."  A light missile
(concussion, homing, mercury) fired at a good target starts a volley;
its rounds follow `VOLLEY_GAP` (0.1 s) apart, and the game's own refire
limit (the weapon's `fire_wait`, `allowed_to_fire_missile` in
`bots_fire`) holds as for a human.  `missile_interval` now separates two
volleys and runs from the last round.

| | Rookie | Hotshot | Ace | Insane |
|---|---|---|---|---|
| Light missile volley (Aggressive +1, Cautious -1, at most 5 and the ammo) | 1 | 2 | 3 | 4 |
| Smart burst behind cover (seen within 1 s) / in sight at 40-120 units | - | 2 / 1 | 2 / 2 | 3 / 3 |
| Between volleys (`missile_interval`; B2) | 4 s | 2.5 s | 1.5 s (1.8) | 1.0 s (1.2) |
| Heavy missile interval / per target (B2: 5 s / 10 s for all) | - | 5 / 10 s | 4 / 8 s | 3 / 6 s |

A good target for a volley: in sight with a clear line, straight
missiles at 30-130 units crossing slower than 40 units/s, homing ones at
40-170 units at any crossing speed; otherwise one missile as before.
The volley goes on while the target stays in sight and clear (a smart
burst while it was seen within 1 s); a cloaked bot fires single shots.
Mega and earthshaker stay one at a time with their cooldowns and the
risk rule of section 9.6.  `test_volleys` models an Insane bot with ten
homing missiles and a `fire_wait` of 0.25 s: volleys of 4, 4 and 2, never
two rounds within the refire limit, the next volley a `missile_interval`
after the last round.

*Heavy-missile boldness* (`risk_profile_of`, the skill's scales of the
budget, the trade and the hug): Ace 1.5, 0.85, 1.2 (B2: 1.25, 0.9,
1.15); Insane 1.9, 0.75, 1.35 (B2: 1.5, 0.8, 1.3).  An aggressive Insane
bot accepts an expected self-damage of 57 % of its shields and a trade
of 1.13 (still above 1: more damage to the target than to itself); a
balanced one 23 % and 1.9 (Hotshot: 12 %, 2.5).  The point blank and the
lethal rules (`judge_blast`) hold for every profile
(`test_heavy_boldness`).

**(4) Pickups.**  The big missiles are worth more (`secondary_value`:
smart 2, mega 2.5, earthshaker 3; B4: 1.5, 1.5, 2); the afterburner is
worth nothing to a bot that has one.  A *high-value* powerup (value from
`GRAB_HIGH_VALUE`, 2: the big missiles, a better gun, quad, super laser,
invulnerability, cloak, the afterburner) is grabbed from 85 units
straight and 130 by path (the others: 45 and 70) with the utility
`GRAB_HIGH_UTILITY` 6.5 (the others: 4), which beats any engagement: an
aggressive bot that was just shot engages at most at 2 x 1.5 x 1.5 x
1.2 = 5.4.  Such powerups also count as worth a detour in a fight
(`collect_in_fight`: the collection falls to 0.8, not 0.35, with an
enemy in sight).  Invulnerability is grabbed in danger too, as shields
are.  In the scenario of section 9.5 (Hotshot, spawn laser, an enemy
100 units away) an earthshaker is now taken up to 85 units away (B4-B2:
45), plasma for a vulcan owner up to 85 (before: 45).  The scan's cheap
first filter (`best_grab`, before the path and the value) is the widest
radius, `grab_in_range` (the PR #38 review: it filtered on 45 units, so
the 85 of a high-value powerup was never reached); `grab_candidate` is
both steps.

**(5) The death dump** (`death_dump_wanted`, `death_dump_choice`).  A
bot about to die fires its missiles and drops its mines as fast as the
game lets it, most valuable first (earthshaker, mega, smart, smart mine,
homing, mercury, proximity, concussion, flash; guided never).  About to
die: its shields at or below the threshold while hit by an enemy within
the last 1.2 s, or enemy shots on a course that meets it within 0.7 s
(the dodge's scan, `perceive`, below 45 shields) with more damage than
its shields while it is at most at twice the threshold.  Each life draws
once whether this bot does it (the reliability):

| | Trainee | Rookie | Hotshot | Ace | Insane |
|---|---|---|---|---|---|
| Shields threshold (Collector +4, Cautious +3, Aggressive -2) | never | 10 | 14 | 17 | 20 |
| Reliability (Collector +0.1) | 0 | 0.25 | 0.6 | 0.85 | 0.97 |

No exploit: every shot goes through the normal firing as the bot
(`bots_fire`: `do_missile_firing`, `MULTI_FIRE`), one at a time as the
refire limit allows, only what the skill uses (`min_smarts`), and a
missile only with its blast clear of the bot: a bot never kills itself
to deny the kill.  The first version checked only the wall along the
nose and a target within 20 degrees; the PR #38 review found that the
attacker, a teammate or a target 25 degrees off (which a homing missile
turns to) were not counted, that the earthshaker's children bursting on
the wall behind were not either, and that the dump skipped the normal
path's friendly-fire checks.  Now each missile is weighed with the heavy
missiles' model of section 9.6 (`dump_outcome`): the wall along the nose
(`evaluate_burst`, the earthshaker's children included), the ship first
on the line along the nose (an object cast as `shot_line_clear`'s, whose
teammate, reactor, robot or clutter refuses every missile), and every
ship ahead within 400 units that it may meet (`may_meet_target`: near
the line, or in the homing cone of a homing missile; seen), the worse of
all (`merge_meet`); a teammate it may meet (friendly fire on) refuses it
(homing missiles do not track teammates, so only the line counts for
one).  The rule (`dump_blast_ok`): never at point blank, the blast
distance of `blast_safe` where it is fired and at the burst, no nominal
self-damage and an expected one below 0.5, and an earthshaker only with
the wall behind clear of its children (`shaker_behind_safe`);
invulnerable beyond the danger, the distance rules alone.  The weighing
shares the heavy missiles' fvi budget per tick.  A mine is dropped only
without a teammate behind (`teammate_behind`, `MINE_TEAMMATE_DISTANCE`,
as a chased bot drops one).  Invulnerable, it does not dump.  A bot that
survives (shields picked up, no longer hit) stops.  The log says
`dumps its missiles: about to die`, each round, and ` dump` in the
summary line.

**(6) Turning round** (`turn_round_state`, `turn_round_velocity`).  "During
the turn I switch from flying forward to flying backwards (and usually
after the turn I boost forward towards the new target)."  A target more
than 110 degrees off the nose starts a *reverse turn*: the bot wants to
fly away from the target (at least 0.8 of its top speed, with 0.4 of the
slide across the line of sight), so while the nose comes round its
thrust turns from forward to reverse by itself (`velocity_command` holds
a world velocity).  When the nose is within 26 degrees, it *boosts*
toward the target at full speed for 0.8 s, with the afterburner from
Hotshot (`afterburner_view::turn_boost`), unless the target is inside
the near edge of its fight band (+15 units) or it gets more than 60
degrees off again; a turn that has not ended after 2.5 s is given up.
It applies to the fight's own movement (not on a path, ducking, hugging
or holding clear of its blast) and to the turn to an unseen attacker
(not while collecting, retreating or refuelling); there the turn ends
when the nose is round, without the boost toward a remembered place
(the PR #38 review: the state went on to the boost and the afterburner
fired for a boost that was not flown).  Flying backwards, the bot
reverses only with `REVERSE_TURN_CLEARANCE` (25) units clear along the
velocity it wants (`reverse_turn_has_room`, a `wall_distance` cast);
else it slides round (`keep_moving_in_turn`) while the nose comes
round (`turn_velocity`).  In the flight model
(`test_reverse_turn`, Hotshot, flying at 40 units/s, a target 80 units
behind, 30-500 fps): the bot faces the target in 1.37-1.48 s (the slide
of section 9.5: 1.40-1.56 s), never below 44 units/s, always at 41
units/s or more away from the target while it turns, flies backwards at
32-37 units/s when the turn ends and reaches 40 units/s toward the
target in the boost (without the afterburner).

**(7) Aim.**  "Aim with lasers and gauss on insane is good.  In a 1:1 I
can still dodge lasers but a bot with a gauss nailed me to the wall."
Unchanged.

**(8) The power-up phase** (`in_powerup_phase`, `third_party_factor`).
A *weak* bot (`weak_armament`: its best gun scores below 1.55, the laser
levels 1-3 without quad or level 1 with quad; and no smart missile,
mega or earthshaker, fewer than 4 homing and mercury, fewer than 8 light
missiles) that knows a weapon upgrade it can reach (a laser level,
super laser, quad or gun of at least 1.2 times its armament within 600
units of path, `best_upgrade`) prefers collecting it to fighting: its
engage weight is multiplied by the style's factor and that upgrade's
collection utility by the style's weight, not reduced by an enemy in
sight:

| | Balanced | Aggressive | Cautious | Collector |
|---|---|---|---|---|
| Engage factor in the phase | 0.55 | 0.8 | 0.45 | 0.3 |
| Upgrade's collection weight | 1.3 | 1.0 | 1.4 | 1.7 |
| Engage factor, one stronger third party (each further one less) | 0.65 | 0.85 | 0.5 | 0.5 |

The phase does not apply when the bot was hit within the last 3 s from
within 70 units (it fights back), when its target is as weak and no
other enemy is known within 200 units (a fair fight), when it is
invulnerable, and on a *weapon-poor* level: fewer weapon powerups at the
level's start than half the players (rounded up) or fewer than two
(`weapon_poor_level`, counted in `bots_level_start` and logged); there
the bots fight with the laser.  *Third parties*: other enemies than the
target, seen within their memory time and within 200 units of the bot,
whose `fight_advantage` over it is above 1.25, lower its engage weight
in any phase (a dogfight with a stronger player about to swoop in is a
bad idea).  In `test_powerup_phase` a weak balanced, cautious or
collector bot with an enemy in sight goes for plasma 200 units away; an
aggressive one fights on unless it is 60 units away; without the phase
(the weapon-poor level) they all fight.  The summary line of the log
gains ` phase` and ` 3rd-party`.

**Tests.**  `test-bot-weapons`: `test_volleys` (sizes by skill, style,
ammo and target, smart bursts, continuation, the refire model),
`test_heavy_boldness`, `test_death_dump` (the trigger, reliability and
thresholds by skill, the order and the blast rule), `test_dump_blast`
(the attacker first on the line, an enemy 35 degrees off that a homing
missile turns to and a straight one passes, a teammate near the line
and off it, the earthshaker with a wall close behind, a wall close in
front, invulnerable).  `test-bot-goals`: `test_high_value_grab` (with
the scan's filter, `grab_in_range`, `grab_candidate`), `test_powerup_phase` (weakness, weapon-poor
levels, the phase and its exceptions, the goal by style, third
parties); the pickup scenarios of section 9.5 updated to the new values
and radii.  `test-bot-flight`: `test_reverse_turn` (the room behind, the state machine,
the velocities, the flight model at four frame rates).
`test-bot-presets`: `test_marker` (the `*`, not with the ping column).

**Unchanged:** the human's firing and every human path (`laser.cpp`
untouched: the bots fire through the same calls as before), clients,
the protocol (still 104), the aim and the primaries.

### 9.9 After the v0.61-exp-19 playtest: fight more, collect smarter, missiles; respawn placement, ghost kills

The playtest (the host alone against five bots on the tight level "Earth
Shaker": havoc Insane Balanced, nomad Insane Cautious, ravager Hotshot
Balanced, sparky Insane Aggressive, wraith Insane Collector; a
`-verbose` log of ten minutes).  The user: "the on-dying trigger for
missiles works well.  While alive the bots are more hesitant."  "Bots
still do not properly react to powerups in their vicinity (they ignore
them) and also do not use missiles aggressive enough, even on insane
level."  Files: `bot_goals.h` (grabs, armed, seeking), `bot_weapons.h`
(volleys, intervals, the heavy missiles' distance, the light verdict),
`bot.cpp`, `segment_depths.h` and `fireball.cpp` (respawn placement),
`multi.cpp` (ghost kills), `contrib/bot-log-replay/` (the replay).

**The log** (20:35:27-20:45:16, 2589 per-second summary lines of the
five bots; `grep "bots: '"`):

| | Seconds | Share |
|---|---|---|
| collect | 1703 | 66 % |
| of which the grab (utility 6.5 or 4) | 878 | |
| of which the power-up phase / plain | 195 / 630 | |
| engage | 504 | 19 % |
| retreat | 188 | 7 % |
| hunt | 182 | 7 % |
| roam | 12 | 0.5 % |

By what the bot knew of its target: in sight 1004 s (39 %), known but
out of sight 622 s (24 %), none 963 s (37 %).  With an enemy in sight
the bots engaged 490 s and collected 448 s, 383 of them grabs: the
grab's fixed utility (`GRAB_HIGH_UTILITY` 6.5 or `GRAB_UTILITY` 4,
`bot_goals.h:720`, taken as the collection at `bot_goals.h:791`) beat the
engagement (median 2.0) every time, for a powerup up to 85 units away,
the target at a median 74 units.  Out of sight but known: collect 359
(184 grabs at 6.5 against a hunt of median 0.7), hunt 177.  With none:
collect 896.  About the vicinity: while collecting something else, a
known and usable powerup within 20 units (the line's nearest valuable
one, `goal-lower`) was 67 times a concussion pack, 16 flash, 11 super
laser, 10 mercury, 6 smart, 4 mega missiles: the grab's ranking, value
over (straight distance + 20) (`bot.cpp:1801`), put a quad 45 units away
(3.6 / 65) before a smart missile 14 units away (2 / 34).  Farther ones
(40-85 units, 189 seconds while engaging or hunting) were rightly left.

*Heavy missiles* (the real data, printed at each shot: mega blast 80,
damage 199, homing; earthshaker blast 80, damage 220, six children blast
80, damage 100).  The bots held a mega or an earthshaker 844 s; 20 megas
and 14 earthshakers were fired (and 2 dumped), 20 of the 34 aimed at a
corner or a wall.  The verdicts of the 844 summary lines: no-target 338,
cooldown 154, not-visible 119, cloaked 64, lethal 41, aiming 32,
too-close 32, risky 31, nose-blast 22, low-value 5, too-far 5.  While
holding one a bot collected 630 s (75 %) and engaged 126 s.  The
distance needed (`heavy_min_distance`, `bot_weapons.h:1385`: 1 and 1.2
radii plus 12, times the standoff scale) was 74-130 units, beyond the
fight's 35-95.  So the heavy missiles were held mostly by bots that were
not fighting; the risk rules (section 9.6) were a minor brake.

*Light missiles*: 88 volleys (concussion 29 x 4, 24 x 2, 20 x 3, 11 x 5;
smart 3 x 2; mercury 1 x 4), about 305 concussion, 32 mercury, 60 smart
and 6 flash missiles by the ammunition counts (44, 9, 11 and 0 of them
dumped), for 810 summary lines with light missiles and an enemy in sight
with a clear line (660 at 30-200 units): a volley every 7.5 s of such
opportunity.  The crossing speeds of the heavy missile lines: 78 % at
most 40 units/s (the straight volley's limit), 14 % 40-60, 8 % above.
The log said nothing about what held a light missile back (no verdict).

*Engine errors*: 918 times `fireball.cpp:279: error: count=655xx,
skip_count=..., and no segment found at depth 23`, and 6 times
`multi.cpp:914: BUG: object ... has type 12, expected 4`.

**(1) Grabs are detours in a fight** (`grab_goal_utility`,
`grab_is_detour`).  With an enemy known (in sight or not, armed or
not), or while it seeks one, a bot that is not weak (`weak_armament`)
takes a powerup only as a short detour:
`GRAB_DETOUR_PATH` 25 units of path for any, `GRAB_DETOUR_HIGH_PATH` 60
for a high-value one (`GRAB_HIGH_VALUE`: the big missiles, a better gun,
quad, super laser, cloak, invulnerability, the afterburner, shields it
needs), a Collector 1.5 times further.  The detour is worth
`GRAB_DETOUR_FACTOR` 1.3 times the fight (it flies there shooting; above
the fight's hysteresis 1.2, so it is taken and kept).  Beyond, the grab
is worth nothing and the fight goes on.  Without an enemy (and nothing
to seek), weak, or in danger (shields, invulnerability) the grab keeps
its utility of section 9.8.  (A first version exempted an unarmed bot
whose target was out of sight: for a bot with a decent gun, fewer than
three light missiles and no heavy one, the grab then jumped between its
full utility and 0 as the target's visibility flickered, and the goal
flipped at the strategy rate with a new plan each time; PR #41 review.)
`goal_utility` now says which part made the
collect goal (`collect_source`: plain, phase, grab), and `think` goes
where that part says (before, a grab that applied but had lost to the
plain collection still redirected the bot).

**(2) The nearest reasonable powerup first** (`grab_rank`: value over
the square of (path + 20)).  The smart missile 14 units away (2 / 34²)
now comes before the quad 45 away (3.6 / 65²), a concussion pack at 20
before a mega at 80; at equal distance the more valuable.

**(3) Armed, it fights** (`armed_of`, `armed_engage_factor`,
`ARMED_COLLECT`).  A bot holding a smart missile, mega or earthshaker
(heavy; from Hotshot) or at least three light missiles (concussion,
homing, mercury; from Rookie) values the fight (engage and hunt) 1.5
(heavy) or 1.25 (light) times, and a plain collection (not a big
upgrade, not needed shields) 0.6 times, unless it is weak.

**(4) Seeking a fight** (`seek_utility`, `SEEK_MEMORY_SCALE`,
`SEEK_ARRIVED`).  An armed bot that knows of no target flies to where it
last saw an enemy, up to three times its memory time ago (Insane
Balanced 30 s, Aggressive 60 s), as a hunt without a target: worth 1
(heavy) or 0.8 (light) times the style's engage weight.  Within 50 units
of the place it has searched it (nobody there) and takes the next
freshest one; a place it finds no path to it gives up at once
(`seek_place_done`), rather than picking it again, with an A* search,
every strategy tick until the memory window expired; a grab on the way is a detour of the seek.  The log says
` seek` in the goal's brackets.

**(5) The Collector fights** (`COLLECTOR_UNDER_FIRE` 0.75).  Section 4.7
did not reduce a Collector's collection with an enemy in sight, so with
its collect weight 1.8 against its engage weight 0.7 it hardly fought
(wraith: collect 407 s, engage 83 s).  Now its collection counts 0.75 in
sight of an enemy (the others: 0.35, 0.8 for a big upgrade), and 0.6
more when armed.  It still takes a prize in an even fight and a small
one when behind (`test_style_goals`).

**(6) Missiles.**

| | Before (9.8) | Now |
|---|---|---|
| Straight volley's good target (Hotshot and below / Ace / Insane) | 30-130 units, crossing up to 40 units/s | 130 & 40 / 150 & 50 / 160 & 60 |
| Homing volley's good target | 40-170 units | Ace and Insane 40-200 |
| Other target in sight, clear, 30-200 units | 1 round | Insane a pair |
| Rounds after the first of a straight volley | the skill's cone | `volley_cone`: twice it, at least 6 degrees |
| Between volleys (`missile_interval`) | by skill | times the style's scale: Aggressive 0.75, Cautious 1.2 |
| Heavy missile while cloaked | never | from Ace at a target within 100 units (as a light one) |
| Insane risk scales (budget, trade) | 1.9, 0.75 | 2.2, 0.7 (aggressive Insane: budget 66 % of shields, trade 1.05) |
| Heavy missile's least distance (log data, full shields) | 92 mega, 108 earthshaker, times the standoff scale | where the nominal blast falls to the damage the bot accepts (budget x shields), at most 0.4 radius nearer: aggressive Insane 66 and 79, balanced Insane 81 and 97, cautious Hotshot 91 and 107 |

The least distance (`heavy_min_distance` with `accepted_damage`) sets
the standoff and the log's `min=`; the release is still the expected
outcome's (`judge_blast`: never point blank, never lethal, within the
budget and the trade), as the indirect and corner shots of section 9.6.
The heavy missiles' cooldowns stay (Insane 3 s between two, 6 s per
target): not spammed, but a bot that fights more fires them more.  A
light missile verdict (`light_check`: none-owned, no-target, cooldown,
not-visible, no-clear-shot, too-close, too-far, cloaked, heavy, chosen;
at the release aiming, nose-blast, fired) and what the bot holds are
now in the summary line (` | light=... armed=...`), so the next log
shows what holds the volleys back.

**Replay of the log** (`contrib/bot-log-replay/`: `extract.py` parses the
summary lines into the goal choice's inputs as far as the log shows
them, `replay.cpp` runs `goal_utility` and `choose_goal` of this code on
them, each bot's previous replayed goal as its current one; the
estimates are in the script's header: notably a grab's path is 1.2
times the straight distance of the nearest valuable powerup when the
grab started, a lower bound, so the grabs left are an upper bound):

| | Log | Replayed |
|---|---|---|
| collect (plain + phase / grab) | 66 % (32 / 34) | 41 % (17 / 24) |
| engage | 19 % | 26 % |
| hunt (of which seeking) | 7 % | 27 % (17 %) |
| retreat | 7 % | 5 % |
| Enemy in sight: engage / collect | 49 % / 45 % | 67 % / 28 % |
| No target: hunt (seek) / collect | 0 % / 93 % | 47 % / 53 % |
| Holding a mega or earthshaker: engage + hunt / collect | 19 % / 75 % | 62 % / 33 % |

By style (engage + hunt): Balanced 31 % to 61 %, Aggressive 40 % to
62 %, Cautious 15 % to 46 %, Collector 16 % to 35 %.  The replay cannot
show the fights that seeking and hunting find (the visible time is the
log's), nor the pickups on the way: in play the engaged share should
rise further.  Missiles: with an enemy in sight the bots now fight 67 %
instead of 49 % of the time, while holding a heavy missile they fight or
hunt 3.3 times as long, an Insane straight volley's good target covers
about 85 % of the in-range seconds (distance 613 of 660, crossing 92 %)
instead of about 67 % (563, 78 %), and an aggressive bot's interval is
0.75 s.  Expected: roughly 1.5-2 times the light volleys (about one per
bot every 15-20 s) and about twice the heavy shots; the new verdicts
will tell.

**Engine: respawn placement** (`segment_depths.h`,
`visit_segment_depths`; `fireball.cpp` `connected_segment_raw_distances`).
The network powerup drop (`choose_drop_segment`, the host's stage-3
creation of powerups) draws a segment at a random depth of 8-24 segments
from a player, scanning the builder's maximum depth first; the thief's
recreation does the same.  The builder counted a segment at a depth only
below its maximum depth (`fireball.cpp:303` returned before the count at
`:334`) but took one off the count of the old depth whenever it found a
shorter route (`:300`), also at the maximum depth, where it had never
counted it.  The 16-bit count wrapped (65511 = 25 taken off), the draw
among 0-65510 skipped past every segment at that depth and the error
followed; the drop then fell back to the next depth, so powerups never
landed at the drawn maximum depth (and the log filled up).  Now each
segment is counted at the depth it is recorded with, the maximum
included, and taken off exactly that count when it moves; the control
centre's segment is excluded at any depth (it was only below the
maximum: at the maximum it could be drawn).  The traversal is a
standard-library template over the level's accessors, so
`test-segment-depths` checks it on 400 random levels (up to 400 segments
with loops and control centres): the depths are the shortest routes not
through a control centre, and every count equals the number of segments
recorded at its depth; the old traversal's count at the maximum depth
wrapped on 389 of 399 levels.

**Engine: kills by a ghost** (`multi_compute_kill`).  A missile (or
mine) of a player that died before it hit is credited to that player,
whose object is by then its ghost (`OBJ_GHOST`, the same object with
its `player_info`): the function accepted the ghost and credited the
kill correctly, but read its player number with `get_player_id`, which
warns on anything but `OBJ_PLAYER` (`multi.cpp:914`; the six BUG lines,
all a dead bot's missile).  It now calls the getter itself after its own
type check (as `multi_do_reappear` does for a player or its ghost), for the killed
object too, and the bounty's name comes from the player number.  The
kill counts as before, as a human's post-death kill.

**Tests.**  `test-bot-goals`: `test_log_tuning_goals` (the log's fight:
no grab at 80 units, a detour at 50 worth 1.3 times the fight and kept;
a concussion pack only within 25; a Collector 1.5 times further; weak
and calm keep the grab of 9.8; hunting armed and seeking make it a
detour; `grab_rank`; `armed_of`; the armed factors; seeking against a
plain collection, roaming and grabs; the Collector under fire; the
collection's source); `test_pickup_scenarios` and `test_high_value_grab`
with the grab's path and the new limits.  `test-bot-weapons`:
`test_log_tuning_missiles` (the style's interval, the heavy missile's
least distance with the log's data by risk, the standoff, cloaked heavy
missiles, the light verdicts); `test_volleys` with the new good targets,
the Insane pair and `volley_cone`.  New `test-segment-depths`.

**Unchanged:** the protocol, clients, the human's firing, the death dump,
the aim, the heavy missiles' release rules and cooldowns.

### 9.10 After the v0.61-exp-22 playtest: pursuit round corners

The playtest (two humans, four bots).  The user: "bots do not chase a
target.  A bot can land multiple hits and hiding behind a corner makes
it forget about you."  Files: `bot_brain.h` (the target's score, the
corner approach), `bot_goals.h` (the pursuit's rules, the goal choice),
`bot_nav.h` (the prediction), `bot_weapons.h` (a homing missile round
the corner), `bot.cpp`.

**Why** (the code before this section; the exp-19 log of section 9.9,
2589 summary lines, for the numbers).

1. *The score halves at the corner* (`target_score`, `bot_brain.h:685`
   before: `visible ? 1 : confidence x 0.5`, the confidence falling
   linearly to 0 over the memory time).  The moment a target broke the
   line of sight its score halved, so its hunt (`bot_goals.h:981-985`,
   2 x score x the weights, the engagement's formula) was worth half the
   engagement and faded from there: in the log a hunt of median 1.0 one
   second after the line broke, against a plain collection or a grab
   (section 9.9: 4-6.5 before, a detour of 1.3 times the fight now).
   While a target was known but out of sight the bots collected 359 s,
   hunted 177 s and retreated 83 s.
2. *Another enemy in sight takes over at once* (`choose_target`,
   `bot.cpp:1977`): the unseen target's 0.5 x confidence x range, with
   the hysteresis 1.2 at most 0.6, lost to any visible enemy (1 x its
   range factor, 0.3-1).  Of the 34 times a target the bot engaged broke
   the line of sight, 7 ended with another target.
3. *Reaching the corner forgets the target* (`follow_path`,
   `bot.cpp:2356`): a hunt arriving at the last known place cleared the
   memory of the target.  That place is where the target was last seen:
   the corner itself.  The bot flew to the corner, found nobody there
   (the target had gone on round it) and forgot it; 12 of those 34
   out-of-sight spells ended so, the target a median 38 units away.
   Afterwards the seek of section 9.9 (`bot.cpp:2100`: armed, no target)
   had nothing to seek either, the memory being cleared.
4. *No way on*: the hunt went to the last known segment and place
   (`bot.cpp:2201`), the velocity ignored; the seek likewise.
5. *Not the cause*: the reaction delay (`bot.cpp:3562`, the percept a
   reaction time late) costs a reaction time at each reappearance before
   the bot engages, as for a human, but clears nothing; the memory stays
   while out of sight (`perceive` updates it only in sight).

In the log, one second after an engaged target broke the line of sight
the bots hunted 24 times, collected 8, retreated 1; the spell out of
sight lasted a median 2 s and ended with the target seen again 15
times, forgotten 12, another target 7.

**Pursuit** (`pursuit_start`, `pursuit_stop`, `pursuit_seconds`,
`update_pursuit` in `bot.cpp`).  At each strategy tick, before the
target choice, a bot whose target was in sight while it engaged (or
hunted) it at most `PURSUIT_ENGAGED_WITHIN` 1.5 s ago and is now out of
sight starts a pursuit if

- it landed a hit on it within `PURSUIT_HIT_WINDOW` 5 s (a bot's hit
  on a bot, recorded in `bot_take_damage`), or
- the target is damaged: shields below `PURSUIT_DAMAGED_SHIELDS` 60, or
  `PURSUIT_DAMAGE_SEEN` 15 lost since the engagement began (a human's
  hits show as its shields), or
- the bot is stronger: `fight_advantage` from 1.25 (Aggressive 1.0,
  Cautious 1.5, Collector 1.4),

and it is not weak for its style (`pursuit_weak`: shields below the
retreat threshold plus the style's margin, Balanced 5, Aggressive 0,
Cautious 20, Collector 10, plus `PURSUIT_START_MARGIN` 10 to start;
Cautious also when behind), nor flying into an obvious ambush
(`pursuit_ambush`: below 50 shields or behind, and the target is known
to hold a mega or earthshaker, or a stronger enemy is known near where
it went).  Invulnerable, neither applies.

It lasts (seconds; `pursuit_seconds`, at most the target memory it
replaces):

| | Trainee | Rookie | Hotshot | Ace | Insane |
|---|---|---|---|---|---|
| Balanced | 2 | 3 | 4.5 | 6 | 8 |
| Aggressive (x 1.75) | 3.5 | 5.25 | 7.9 | 10.5 | 14 |
| Cautious (x 0.5) | 1 | 1.5 | 2.25 | 3 | 4 |
| Collector (x 0.6) | 1.2 | 1.8 | 2.7 | 3.6 | 4.8 |

and ends when the target is seen again (it engages), is gone (dead,
disconnected), another target takes over, the time is up, the bot is
weak (without the start margin: the hysteresis) or sees an ambush, dies,
or has searched (below).  After it gave up (persistence over, weak, an
ambush, searched) the bot does not pursue nor hunt that target again
until it has seen it again (`pursuit_block`): no flying back to the
corner, no flip-flop.  Seen again, gone, died or another target block
nothing (a target that peeks and ducks back is pursued again).  While
pursuing:

- the target scores at least `PURSUIT_TARGET_SCORE` 0.9 (times its
  range factor, not faded by the confidence): it stays the target
  against a far enemy in sight, not against a close one that just hit
  the bot, and its hunt is worth about the engagement;
- a collection (plain, the phase's) and refuelling count
  `PURSUIT_COLLECT` 0.35, and a grab is taken only when high-value and
  within `PURSUIT_GRAB_PATH` 30 units of path (then 1.3 times the
  hunt); in danger (retreat) the shields are taken as before;
- the goal is where the target probably is now (`predict_pursuit`,
  `bot_nav.h`): from its last known place, a walk through the segment
  graph along its last known velocity, at each segment the passable
  exit whose centre lies best along the way (at least -0.25: a bending
  corridor is followed, never back to a segment passed), the way turning
  half toward each exit taken, for `pursuit_travel` units (its speed, at
  least 30 units/s, over the time since it was seen plus 0.5 s, at most
  2.5 s of it, plus 60 units per predicted place already reached, at
  most 240), at most `PREDICT_MAX_STEPS` 12 segments (72 edges at most
  per strategy tick); a place out of the bot's reach (the path costs of
  the tick) falls back to the last known one.  The path is planned again
  when the predicted segment changes, at most every half second (as the
  hunt), and each A* keeps its node limit;
- reaching a predicted place without finding the target, the bot goes
  on to the next (further along the way), up to `PURSUIT_MAX_ADVANCES`
  3 places or a dead end: searched, the target forgotten (the old
  arrival rule, now only then).

**Corner clearing** (`corner_approach_point`, `corner_keep`,
`plan_corner`).  From Hotshot, within `CORNER_APPROACH_RANGE` 110 units
of the corner (the last known place), once per pursuit, the bot "slices
the pie": it flies (at 0.7 of its top speed) to a point `keep` short of
the corner along its approach (Balanced 22, Aggressive 14, Cautious 32,
Collector 26 units) and swung 0.8 `keep` to the outside of the turn
(away from the side the target went to), facing the corner's exit (the
corner plus 25 units along the target's way): the view round the corner
opens from a distance, the guns on it, and the target coming back round
is in the field of view.  The point must be reachable in a straight line
(`line_clear` with the ship's radius: the full swing, half, none; at
most three probes, once).  The peek ends there or after 1.5 s; then the
path.  Trainee and Rookie fly straight at it.  A smart missile already
went round corners (`SMART_SEEN_WITHIN`: the target seen within 1 s); now from Ace a
homing missile is fired at the corner's exit too (`homing_round_corner`:
pursuing, seen within 2 s, 40-150 units from the corner; the missile
cooldown and the release's blast check as usual; one try per
`HOMING_CORNER_INTERVAL` 6 s; the bot turns to the exit and the release
aims at it, `release_aim_of`).

**The log** (`-verbose`): `pursues P#n round a corner: hits landed |
target damaged | stronger (for 8.0 s; last seen ... units away ... s
ago at ... units/s; shields ... against ..., advantage ...)`, `ends the
pursuit of P#n: seen again | persistence over | weak, breaks off |
ambush | target gone | searched, nobody | other target | died (after
... s, n predicted places reached)`, `clears the corner where P#n went:
peeks from 22 units, swing 18`, and ` pursuit` in the summary line's
goal brackets.

**Tests.**  `test-bot-goals` `test_pursuit`: the persistence table
(monotonic by skill, Aggressive > Balanced > Collector > Cautious,
within the memory), each start condition and its absence, the break-off
by style with its hysteresis, the ambush, the expiry, the predicted
distance, and the goal choice while pursuing (the hunt over a plain
collection that beat it before, the grab limit, the retreat, the goal's
hysteresis).  `test-bot-brain` `test_pursuit_target_and_corner`: the
pursued target kept against a far visible enemy, not against a close
attacker, and the corner approach (short, swung to the outside, the aim
at the exit, no peek close by, far away, for a beginner or without a
way).  `test-bot-nav` `test_pursuit_prediction`: along a corridor past
a branch, round a bend, to a dead end, into a branch, a closed door,
backwards, without velocity, the step bound.  `test-bot-weapons`
`test_homing_round_corner`.

**Unchanged:** the protocol, the reaction time, the seek of section 9.9
(no target), a hunt that is no pursuit (it still forgets on arrival).

**The PR #47 review.**

1. *The corner shot never fired.*  `choose_secondary` chose the homing
   missile round the corner only with the target out of sight, but the
   release (`missile_tick`) held every missile at an unseen target but
   the smart one and a heavy one aimed at a wall: the shot was chosen,
   held for `MISSILE_PENDING_SECONDS` and chosen again, for ever.  Now
   the release's decision is `release_aim_of` (`bot_weapons.h`: hold, at
   the target, or at the corner): a homing missile chosen as the corner
   shot (`bs.corner_shot`) goes while the pursuit of that target is under
   way, aimed at the corner's exit (the last known place plus
   `CORNER_AIM_AHEAD` along the target's way, the peek's aim); pending,
   the bot turns to the exit while flying its path.  A corner shot is
   chosen at most once per `HOMING_CORNER_INTERVAL` 6 s (counted from the
   choice: an empty corner, or an aim not reached, is not tried again at
   once).  Logged: `aims a homing missile round the corner at P#n`.
2. *A brief sighting dropped the target.*  Every end of a pursuit, "seen
   again" too, recorded the block, so a target that showed itself for a
   moment and ducked back (its memory tick that of the end) was neither
   pursued nor hunted, while it stayed the target (no seek either).  Now
   only giving up blocks (`pursuit_end_gives_up`, `pursuit_block` in
   `bot_goals.h`); the engagement the sighting renews restarts the
   pursuit.
3. *A predicted place inside a wall.*  `predict_pursuit` interpolates on
   the straight line from the last known place (or a centre) to the next
   centre; from the inner side of an L-junction that line crosses the
   wall, and the goal lay inside it (the bot pressed against the wall).
   Now the final leg is checked (`clear`, one fvi ray per strategy tick)
   and, if blocked, the point falls back to the centre of the segment it
   lies nearer, or where the leg began.  `plan_path` also replaces the
   goal segment's centre with the goal's place only if a ray reaches it
   from that centre (from the ship when already in the segment); else the
   centre is the goal (logged: `goal place in segment n out of reach`).

Tests: `test-bot-weapons` `test_release_aim` (the release's decision, the
corner shot chosen and released, the interval), `test-bot-goals`
`test_pursuit_block` (which ends block, the peek and duck),
`test-bot-nav` `test_pursuit_prediction_l_junction` (the L-junction: the
point inside the wall without the check, the fallbacks with it, an open
line unchanged).

### 9.11 B5 as implemented

**Files.** `common/main/bot_command.h` (new, pure: the `/bot` parser,
the skill and style words, which bot a name means, the add verdict, the
order of addition, the name a bot may carry), `similar/main/bot.cpp`
(add, remove, change, rename, team; the level's data prepared on
demand), `similar/main/bot_menu.cpp` (the in-game Bots screen, the
chat command, "Save as default setup"), `similar/main/net_v2.cpp` (a
player without a connection enters the game; the player list to
everyone; the slot for a bot; a bot's new name on the clients),
`similar/main/net_objects.cpp` (a slot's last inventory report is
forgotten when a player enters it), `similar/main/multi.cpp` (`/bot` in the chat, the host's notice, the
team change), `similar/main/gamecntl.cpp` (the game menu). Test:
`test-bot-commands` (new), `test-bot-presets` (the profile's bot
lines). `similar/main/playsave.cpp` writes the bot lines alone
(`write_netgame_profile_bots`). The protocol numbers stay 105: no
message changed its layout.

**The options of the game.** `Bot_game` (the skill and style of a new
bot, humans replace bots) is copied from the setup when the game starts
(`bots_allocate_slots`) and is what the game reads (`bots_replaceable`).
The in-game screen changes it, not `Bot_setup`.

**Adding a bot** (`bots_add`). The checks are `judge_add`: host of a
network game, a mode with bots, a level being played, no reactor
countdown, a slot, no join in progress. The slot is
`free_admission_slot` on the host's slot views, the same rule as for a
human who finds room (`host_free_slot_for_bot`; a slot with a connection
is never taken). Then:

1. If the level has no bot data yet (it started without bots),
   `prepare_level` builds the navigation graph, the ship limits, the
   spawn sites and the weapon count, and starts the tick.
   `bots_level_start` marks a level without bots as unprepared, so the
   graph of an earlier level is never used.
2. The name: the one given (lower case, 8 characters, letters, digits,
   `-` and `_`; not a reserved word, `name_reserved`: `/bot` and the
   screens refuse one, and `bots_add` itself would take a built-in name
   instead), else the next built-in name; made unique against every
   callsign in the game, those of departed players included (a human who
   dropped comes back by callsign).
3. The bot's state is created with the order of addition after every
   bot in the game (`next_added_order`), and the slot's bot flag is set.
   In team modes it goes to its preferred team, else to the smaller
   (blue on a tie); `multi_host_teams_changed` recolours the ships and
   sends `MULTI_GMODE_UPDATE`.
4. `net_v2::host_add_player`: the slot's inventory copy is reset
   (`net_objects_host_join`), `new_player` runs on the host (scores and
   the slot's kill-matrix row and column zeroed, as on every client
   with `PLAYER_JOINED` and in a joining human's snapshot: the kills of
   the slot's previous holder are not the new player's; "is joining", the
   kill list sorted),
   `PLAYER_JOINED` goes to everyone, and then `PLAYER_LIST`: the first
   clears the slot's bot flag on a client (a human took the slot), the
   second, behind it in the same reliable stream, sets it.
5. The first spawn is the bot's ordinary respawn: a site assigned by
   the host (`choose_bot_spawn`, with the reservations of the
   host-assigned spawns), `MULTI_REAPPEAR` as the bot, its inventory.

A client needs nothing new for this: `PLAYER_JOINED`, `PLAYER_LIST` and
`MULTI_REAPPEAR` are what a joining human causes. A human who joins
later gets the bot in the snapshot as any player (the list with the
flag, the ship object, the kill matrix, and the inventory with the
extras). While a join is being served the add is refused ("A player is
joining; try again in a moment"): the joiner's snapshot was built
without the bot, and its level start would otherwise restore the
previous holder's score in a reused slot.

**Removing a bot** (`bots_remove`) is the path of a kicked or replaced
bot (§9.7) with the reason `quit`: a bot in its death tumble explodes
first, otherwise its inventory copy is brought up to date;
`multi_disconnect_player` drops its items (once: a bot already exploded
has none), makes the ship a ghost, prints "has left the game" and sends
`PLAYER_LEFT(quit)`. The slot is a departed bot's, free for a human or
a new bot; its line stays in the kill list with its score until then.

**Changing a bot.** Skill and style: `bot_state::apply_config` at once
(the presets are read every tick, so the next tick plays them; nothing
waits for the next life), and a chat line from the host tells everyone
(`havoc now Ace Aggressive`; `multi_send_host_notice`, an ordinary
`MULTI_MESSAGE`). Clients have no other display of a bot's skill, so
the notice is the refresh. Team: the bit of `team_vector`, the ships'
colours, `MULTI_GMODE_UPDATE`, a notice. Name: the host's player and
netgame entries, then `PLAYER_LIST` to everyone; a client takes a new
callsign for a slot flagged as a bot from the list during the game
(`read_player_list`), so its kill list and name tags follow. A client
of an older build of protocol 105 keeps the old name until the next
level; nothing else differs for it. The list also goes to a peer that is
still joining (between its `JOIN_ACCEPT` and its snapshot), so a bot
renamed meanwhile does not keep its old name there; the snapshot brings
the list once more.

**Persistence.** *Decision:* in-game changes stay in the game. "Save as
default setup" and `/bot save` copy the bots playing (in the order of
addition, with their current names, skills, styles and team
preferences) and the game's options into `Bot_setup` and write the bot
lines of the pilot's `.ngp` (`replace_profile_bot_lines`: the file is
read, its bot lines are replaced, every other line stays as it is). The
whole profile is not written from the game's `Netgame`, which is not
the setup the host made: without a tracker address the tracker is
switched off in it after the setup was saved, and saving it would
switch the tracker off in the profile. Reason: adding a bot to fill an evening, or losing one
to a human, should not silently change the setup of the next game.

**Menus and the running game.** The Bots screen is opened from the game
window's key handler and runs its own event loop (`newmenu_do2`), like
the game menu and the options. In a network game the game window keeps
drawing, and its draw event runs the frame (`GameProcessFrame`), so the
game, the bots and the network go on; the menu does not sleep between
events in a network game. Each action closes the screen with a code,
is applied, and the screen is built again. Because the game runs on:

- on every idle event the screen checks that this machine still hosts a
  game with bots (else it closes) and that the bots listed are the bots
  playing (else it rebuilds); the per-bot screen closes when its bot is
  gone;
- a bot is identified by its slot *and* its order of addition, checked
  again before anything is applied, so a change never lands on a human
  or another bot that took the slot meanwhile;
- the game closes the menus in front of it when the host is hit, dies,
  the level ends or the countdown nears its end (`game_leave_menus`,
  from the game's own frame). A screen that returns without a choice
  tells the two apart (`close_watch`: the host's Escape or click is the
  screen's last event before it closes; the game's close is not): after
  the host's Escape the per-bot screen goes back to the list, after the
  game's close all Bots screens are left and nothing is applied. The
  messages of these screens ("The game is full", the saved setup, a
  refused name) are watched the same way (`notice`): a message the game
  closed does not bring the list back;
- with the deferred deletion of PR #48 a window closed while one of its
  handlers is on the stack (the game window under these nested loops,
  or a screen closed from the game's frame) stays allocated until the
  handler returns; when the game itself is over, the loop finds it
  unmanageable (`bots_manageable`) and returns without touching it.

**Tests** (`test-bot-commands`): what is no command (`/bottle`,
`/bot: hi`), help, list and save, every skill and style word and its
three-letter form, that no built-in name reads as a skill or style
(`rook` is a name), `/bot add` with each part left out and in the wrong
order (an error), the reserved words (refused as names in `/bot add`
and by `usable_name`, while names that only begin like one stay
names), the name rules, remove/skill/style with `all`, missing and
surplus words, that every error says why; the target (exact before
prefix, ambiguous, none); the add verdict; and with
`net_v2_session.h`: the slot a bot takes (lowest free, the limit for
humans and bots together, a departed bot's slot, never a disconnected
human's), and that a bot added during the game is the one a joining
human replaces, also after a removal and with the option off.

**Not in B5:** showing each bot's skill on the clients (they see `BOT`
and the notices), `/bot remove #n`, adding bots between levels, CTF
and hoard (B7).

### 9.12 After the first recordings (2026-09-30): flying like a human

The movement recordings (Documentation/movement-recording.md) of a
strong human ("EC") against five bots (one Hotshot, four Insane)
on two evenings, analysed with `movrec-analyse --bots`, put numbers on
how differently they flew. Fight distance, missile volleys and pickups
were already close; the flight itself was not:

| | human | bots |
|---|---|---|
| Mean speed; share flat out (above 85 % of the top) | 52–54 units/s; 63–70 % | 37–41; 24–36 % |
| Strafe reversals per minute of fight; vertical share | 45; 0.29–0.33 | 80–102; 0.71–1.00 |
| Thrust across while strafing; speed across (upper quartile) | 98 %; 77–79 % | 75–79 %; 45–48 % |
| 180° of a large turn; rotation rate | 1.6 s; 73 % of the top | 1.2–1.5 s; 80–99 % |
| Large turns reversing / sliding; push forward after | 11 % / 86 %; 74–78 % | 24–45 % / 55–76 %; 31–48 % |
| Afterburner overall; fleeing; no enemy in sight | 5–7 %; 16–21 %; 4–6 % | 0–2 %; 0–6 %; 0–3 % |
| Flies away turned from the enemy below | about 65 shields | about 10 |

**Root causes**, in the code, not the constants:

- *Every strafe run was a reversal*: `juke_state` gave each run a new
  direction at least 90° from the last, on a circle across the line of
  sight, with the vertical share as a part of each direction (Insane's
  1.0: every run half up or down).
- *The velocity controller jittered the strafe*: the fight asked
  `velocity_command` for a velocity (closing speed along the line of
  sight plus the strafe), which it turns into thrust with a gain of 2 on
  the error. The range keeping flips sign as the enemy moves in and out,
  and with the nose on the lead point (up to 25° off the line) that
  flip lands on the sideways axis; backing off along a path while facing
  a strafing enemy flipped the lateral thrust almost every tick. The
  analysis counted most of the 80–100 reversals there.
- *Partial thrust, slow*: a velocity controller eases off as the speed
  nears what it asked for (0.7–0.8 of the top), and the length of the
  command was capped at 1, so the bot never held two keys at once; a
  human holds forward and a strafe key together (the diagonal is 1.41
  times the top speed: the human was above the top speed a tenth of
  the time).
- *Turns at the cap*: a large turn ran at the skill's full turn cap
  (Insane 99 %), every large turn with room behind was flown backwards,
  and only those ended in a push.
- *The afterburner* needed the nose within 25° of where the bot went:
  retreating, it faced its pursuer (backwards: never), and long straight
  flights (beyond 100 units) lit it for Insane only.
- *Retreat backwards*: the retreat was always flown facing the pursuer,
  which the analysis counts as backing off, not as flying away.

**Changes** (`bot_brain.h`, `bot_goals.h`, `bot.cpp`), all on the bot's
own random numbers (deterministic on the host), no protocol change:

- **Keys, not velocities, in a fight.** `juke_state` holds a strafe key
  (left or right) for a run of the skill's length and, in the share
  `strafe_vertical` of the runs, an up or down key with it; after a run
  the keys are let go (`STRAFE_PAUSE_SHARE` 0.3, for 150–450 ms) or the
  next run follows, the other way (`STRAFE_FLIP_SHARE` 0.75) or the same
  way on. The preferred distance is drawn every 1–2.5 s. The range is a
  key too (`approach_key`): forward beyond the preferred distance plus
  15 units, reverse inside it less 15, held in between, at the style's
  closing thrust (`COMBAT_CLOSE_SPEED` 0.9 × `close_scale`); a band
  narrower than 30 units (hugging an enemy within its own blast) keeps
  the proportional thrust of before (`approach_thrust`), which does not
  overshoot into the enemy.
  `fight_keys` gives the three axes in the ship's frame; the dodge adds
  its full thrust to them, and the command is clamped per axis
  (`steer_controls`), so forward and strafe together make the diagonal.
  `skill.strafe_speed` is now the thrust of the strafe keys (0–1).
- **No flicker.** `lateral_keys` filters the sideways and vertical
  thrust of every command (keys and velocity alike) like a key: a push
  the other way than the key last held counts once the key has not been
  pushed its way for 100 ms (`KEY_FLIP_TICKS`); until then that axis is
  released. A flicker never flips the key; the strafe's own flip comes
  100 ms late. A dodge, an evasion and the stuck recovery flip at once.
- **Turns.** `turn_round_state` draws each large turn's kind from
  `turn_habits` (reverse 0.12, else a slide; push after 0.8, with the
  afterburner in 0.3 of the pushes, from Hotshot); a slide holds a
  strafe key at full thrust (`slide_state`, from 60° off the target to
  34°, the way the ship already slides), also when a wall behind forbids
  the reverse turn. The push lasts 1.1 s. The rotation cap falls from
  the skill's with the nose 60° off to 0.8 of it from 110°
  (`large_turn_cap`); aiming keeps the full cap.
- **Retreat.** A retreat is flown turned away, the nose along the path,
  in `FLEE_TURNED_SHARE` (0.4) of the time, drawn when it starts and
  every 2 s; the afterburner (from Hotshot) in `FLEE_BURN_SHARE` (0.5)
  of those draws. Shots and missiles go along `aim_dir`, the aim, so a
  bot fleeing turned away does not fire at nothing. The styles retreat
  10 shields earlier (Balanced 45, Aggressive 30, Cautious 65, Collector
  50).
- **Afterburner on long flights** from Hotshot: beyond 150 units to the
  steer point (`BOT_LONG_STRAIGHT`), lit only with 90 % charge and kept
  down to 50 % (Insane: 70 % and 35 %), so a fight still finds charge.
- **Skills**: strafe key runs Rookie 0.4–1.0 s, Hotshot 0.25–0.7,
  Ace 0.22–0.65, Insane 0.2–0.6; key thrust 0.7, 0.95, 1, 1; vertical
  0.1, 0.2, 0.2, 0.22. Trainee still does not strafe, dodge or burn, and
  turns slowest.

**Measured.** There is no game data here to fly `-botarena`, so
`test-bot-fight-sim` (new) flies the bots' movement code (the same
functions `bot_tick` calls) in the model of the ship of
`test-bot-flight` against a scripted enemy that strafes round the bot
and appears behind it, with retreats at low shields, out-of-sight and
empty stretches (16 minutes per bot), writes the flight as the
recorder writes it and reads it with the analysis of `movrec-analyse`.
With the old code the simulation reproduced the recordings' speed
(37–40), thrust across (68–79 %), vertical share (0.76–1.0) and turn
times (Hotshot 1.40 s at 83 %, Insane 1.17 s at 98 %), and doubled their
reversals (about 190 per minute, from the backward retreats). Now
(Hotshot / Insane; the test checks ranges for Hotshot, Ace and Insane,
and the same at 30 and 144 fps):

| | human | bots, recorded | simulation before | simulation now |
|---|---|---|---|---|
| Mean speed; flat out | 52–54; 63–70 % | 37–41; 24–36 % | 39–40; 35 % | 48; 52–53 % |
| Strafe reversals / min; run | 45; 0.37 s | 80–102; 0.43 s | 190; 0.23–0.40 s | 44–49; 0.47–0.57 s |
| Vertical share | 0.29–0.33 | 0.71–1.0 | 0.85–1.0 | 0.35–0.36 |
| Thrust across; speed across | 98 %; 77–79 % | 75–79 %; 45–48 % | 76–79 %; 53–58 % | 86–90 %; 73–74 % |
| 180° of a large turn; rate | 1.6 s; 73 % | 1.2–1.5 s; 80–99 % | 1.40 / 1.17 s; 83 / 98 % | 1.61 / 1.32 s; 75 / 90 % |
| Turns sliding; push after (burning) | 86 %; 74 % (26 %) | 55–76 %; 31–48 % (0–28 %) | 96–100 %; 54–58 % (71–92 %) | 82–84 %; 60–61 % (20–40 %) |
| Afterburner; fleeing; no enemy | 6 %; 21 %; 6 % | 0–2 %; 0–6 %; 0–3 % | 3–10 %; 0 %; 0–14 % | 3.7–4.3 %; 20–24 %; 3–4 % |
| Flying away (share of the fight) | 19 % | 5–13 % | 7–9 % | 18–20 % |

The simulation is open space: no walls to slide along, no pickups, no
enemy fire to dodge, and an enemy that is no human; its numbers are for
comparing the code before and after, and the next recordings of real
games will tell how close the bots came.

---

## 10. Risks

| # | Risk | Mitigation |
|---|---|---|
| R1 | The pilot refactor (B0) touches the most sensitive human code paths (controls, firing, damage) | No behaviour change in B0; review diff against the human path; play-test B0 alone before B1 |
| R2 | Host loops assume "slot playing ⇒ peer with a connection" (level ready counting, `KICK(endlevel)`, extras queue, timeouts, pings) | One `slot_is_bot(pid)` predicate; audit every `S.peers` / `connected` loop in `net_v2.cpp` in B1; `-botarena` exercises level changes |
| R3 | Custom levels with odd geometry trap bots (tiny doors, vertical shafts, secret doors) | Stuck recovery + penalised edges; level soak test; log stuck segments so levels can be reported |
| R4 | Bots too good or too dumb; "feel" is subjective | Presets are data; per-bot `.ngp` overrides for tuning; the arena ladder gives numbers |
| R5 | Host frame time spikes on huge levels (A* over 9000 segments) | Cached graph, staggered replans, A* node limit with partial path fallback (as `create_path_points` does) |
| R6 | Stage 3/4 land with APIs different from those assumed here (`powerup_apply(pid, …)`, host damage) | B3/B6 are written against the merged stages, not the design text |
| R7 | Demo recording on the host records bots as remote players only through the multiplayer records | Accept; verify in B1 that recording does not assert |

---

## 11. Decisions (user, 2026-09-27)

1. **First PR scope: menus from the start.** B1 already contains the setup
   UI (§6.1–§6.3: Bots item in the host setup menu, Bots screen, per-bot
   screen). A `-bots N` switch may exist as a developer shortcut, but the
   menus are how bots are added.
2. **Client-visible bot marker: yes.** `PLAYER_LIST` carries a bot flag
   (protocol bump); clients show `BOT` in the player list and score screens.
3. **Humans replace bots when the game is full: yes**, removing the most
   recently added bot.
4. **Powerup map knowledge: scaled by skill** (§5.1).
5. **Default skill for new bots: Hotshot.**
6. **Bots attacking the reactor: never.**
7. **Game data for testing:** the offline arena test is a local tool; CI
   covers the pure logic.
