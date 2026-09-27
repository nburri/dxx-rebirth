# Multiplayer bots (design)

Status: design; stage B0 (the pilot refactor, §3.2.1) and stage B1 (the
first bot with its setup menus, §9.1) are implemented. Target branch: `experimental-netcode`
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
  that finds the game full removes the most recently added bot first. The
  removal is broadcast as `PLAYER_LEFT(quit)`, the same as a quitting player.
  The human then gets that slot.
- **Removal**: the host treats a removed bot like a player who sent `LEAVE`
  (v2 §4.6): ghost the ship, drop its eggs, `PLAYER_LEFT(quit)`, slot becomes
  `disconnected`. Its score stays in the kill list like any departed player's.
  A later joiner may reuse the slot, and `new_player` zeroes the scores.
- **Host leaves**: the game ends (`HOST_SHUTDOWN`) and the bots go with it.
  There is no host migration in v2, so nothing more is needed.
- **Level change**: bots persist across levels with their scores, like
  humans (`LEVEL_START` → `SNAPSHOT_GAME`).

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

- **Lead accuracy** `ℓ`: the bot uses `ℓ × V`. At `ℓ = 0` it aims at where
  the target is.
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
| Proximity bomb, smart mine | dropped while retreating with a pursuer behind within 80 units, or at a doorway on its own path while being chased; Hotshot+ |
| Flash missile | when the target is facing the bot within 100 units (cosmetic advantage only) |
| Guided missile | fired as an unguided missile that is released immediately; steering is a non-goal |

### 4.6 Movement in combat and evasion

- **Range keeping**: each weapon has a preferred distance band. The bot
  thrusts forward or back to stay in it and sidesteps while firing.
- **Strafe pattern**: lateral strafing whose direction flips every
  0.4–1.2 s (random within the style's range), plus vertical bobbing for
  Ace+. Circle-strafing happens when the target is in the band.
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
| Balanced (default) | 35 | 1.0 / 1.0 | weapon default | — |
| Aggressive | 20 | 1.5 / 0.6 | −25 % | chases 2× longer, fewer mines, uses mega more readily |
| Cautious | 55 | 0.8 / 1.2 | +25 % | dodge prob. +0.1, drops mines when retreating, breaks off when outgunned |
| Collector | 40 | 0.7 / 1.8 | weapon default | roams powerup-rich areas and hoards before fighting |

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
   Name:   [Havoc___]            (8 chars; not editable in game)
   Skill:        Ace   [===|=]
   Style: Aggressive   [=|==]
   Team:        Auto   [|==]     (Auto / Blue / Red; team modes only)
   Remove this bot
   Done
```

### 6.4 In game

- **ESC "Game Menu"** (`HandleSystemKey(KEY_ESC)`, `gamecntl.cpp`) gets a
  "Bots..." choice for the host in network anarchy modes: `Abort Game`,
  `Options...`, `Bots...`. It opens §6.2 in in-game mode. The menu does not
  pause the game, as today. Skill and style changes apply at the next
  strategy tick. Add and remove work as described in §2.3.
- **Chat commands** for quick use, next to the existing `/kick:` and
  `/move:` in `multi_send_message_end`:
  `/bot add [skill] [style]`, `/bot remove <name|#n>`,
  `/bot skill <name|all> <trainee..insane>`. `/kick: <botname>` removes a bot.
- **Kill list**: bots show `BOT` in the ping column (with the flag of §2.2).

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
  The file is written when the setup menu closes and after in-game changes.

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
| **B5** | In-game bot menu, chat commands, add/remove during play, join in progress with bots (extras inventory until stage 5). | B2 | S |
| **B6** | Move to stage 4 authority: bots in the history ring, robot-style hit detection for bot shots, generic `PLAYER_KILLED`/`PLAYER_SPAWN`; delete the bot-specific kill path. | v2 stage 4 | S |
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
`choose_spawn` / `place_player`, `multi_make_ghost_player` (spawn grants),
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
