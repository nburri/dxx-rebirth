# Multiplayer bots (design)

Status: design only, nothing implemented. Target branch: `experimental-netcode`
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
  parent is not itself. For each it predicts the closest approach over the
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
