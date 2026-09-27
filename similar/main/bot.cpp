/*
 * This file is part of the DXX-Rebirth project <https://github.com/dxx-rebirth/dxx-rebirth/>.
 * It is copyright by its individual contributors, as recorded in the
 * project's Git history.  See COPYING.txt at the top level for license
 * terms and a link to the Git history.
 */
/*
 * Multiplayer bots on the host (Documentation/multiplayer-bots.md, stage
 * B1): the slots bots take, their brain on a fixed tick (perception,
 * target choice, navigation, aim), the controls it produces, firing,
 * damage, death and respawn.
 *
 * A bot is a player slot the host flies.  Its ship is an ordinary remote
 * ship (control_source remote), moved by do_physics_sim from the bot's
 * controls through apply_pilot_controls, with the bot's own `pilot`.
 * Everything it does reaches the clients as the messages of a human
 * player, with the bot as originator (section 7.1).
 *
 * The decisions are taken on the brain's 60 Hz tick (perception at
 * 20 Hz, strategy at 5 Hz, staggered per bot); only the steering
 * controller runs every frame, from the current state (section 3.4).
 * The logic that does not need the game is in bot_brain.h and bot_nav.h.
 */

#include "dxxsconf.h"

#if DXX_USE_MULTIPLAYER

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <optional>
#include <span>
#include <vector>

#include "bot.h"
#include "bot_nav.h"
#include "net_interp.h"
#include "pilot.h"
#include "multi.h"
#include "object.h"
#include "player.h"
#include "game.h"
#include "gameseq.h"
#include "gameseg.h"
#include "fvi.h"
#include "physics.h"
#include "laser.h"
#include "weapon.h"
#include "collide.h"
#include "fireball.h"
#include "wall.h"
#include "segment.h"
#include "textures.h"
#include "piggy.h"
#include "kconfig.h"
#include "controls.h"
#include "endlevel.h"
#include "d_enumerate.h"
#include "newdemo.h"
#include "timer.h"
#include "strutil.h"
#include "vclip.h"
#include "bm.h"
#include "console.h"
#include "d_levelstate.h"

namespace dsx {

namespace {

namespace b = ::dcx::bot;
using b::vec3;

/* The death tumble lasts as long as the human's (dead_player_frame). */
constexpr fix BOT_DEATH_EXPLODE_TIME{F1_0 * 2};
/* Section 4.8: a bot waits a moment before it respawns, as a human
 * takes a moment to press fire.
 */
constexpr double BOT_RESPAWN_MIN_S{1.0};
constexpr double BOT_RESPAWN_MAX_S{2.5};
/* A* expands at most this many segments per plan (section 10, R5). */
constexpr unsigned BOT_NAV_NODE_LIMIT{4000};
/* Seconds after a hit during which the attacker counts for revenge and
 * is noticed outside the field of view.
 */
constexpr unsigned BOT_REVENGE_TICKS{3 * b::BOT_TICK_RATE};
/* A cloaked player is seen only this close (section 4.2). */
constexpr double BOT_CLOAK_SEE_DISTANCE{40};
/* The engagement distance band before the style's range scale. */
constexpr double BOT_RANGE_LO{35};
constexpr double BOT_RANGE_HI{95};
/* Stage B1 plays every bot at the Hotshot preset (section 9). */
constexpr b::bot_skill BOT_B1_SKILL{b::bot_skill::hotshot};

[[nodiscard]]
vec3 to_vec(const vms_vector &v)
{
	return {v.x / 65536.0, v.y / 65536.0, v.z / 65536.0};
}

[[nodiscard]]
fix to_fix(const double d)
{
	return static_cast<fix>(std::clamp(std::lround(d * 65536), -0x7fffffffl, 0x7fffffffl));
}

[[nodiscard]]
vms_vector to_fixvec(const vec3 &v)
{
	return {to_fix(v.x), to_fix(v.y), to_fix(v.z)};
}

[[nodiscard]]
b::frame3 to_frame(const vms_matrix &m)
{
	return {to_vec(m.rvec), to_vec(m.uvec), to_vec(m.fvec)};
}

/* What the tactics layer reads, `reaction` late (section 4.2). */
struct percept
{
	uint8_t target{0xff};
	bool visible{};
	vec3 pos;
	vec3 vel;
};

enum class bot_life : uint8_t
{
	alive,
	/* Killed: its ship tumbles until it explodes. */
	dying,
	/* Exploded: a ghost until it respawns. */
	dead,
};

enum class bot_goal : uint8_t
{
	none,
	/* No target: fly to a random place. */
	roam,
	/* A target out of reach of a direct shot: fly to where it was seen. */
	hunt,
};

struct bot_controls
{
	double pitch{}, heading{}, forward{}, sideways{}, vertical{};
};

struct bot_state
{
	playernum_t pid;
	bot_config cfg;
	const b::skill_params *skill{&b::skill_of(BOT_B1_SKILL)};
	const b::style_params *style{&b::style_of(b::bot_style::balanced)};
	pilot pl{};
	b::bot_rng rng;
	/* The stagger of the slower layers: the slot. */
	unsigned stagger{};
	/* Brain output, fixed at the last tick. */
	vec3 face_dir{0, 0, 1};
	vec3 move_cmd;
	bool fire{};
	int heading_pref{1};
	/* This frame's controls. */
	bot_controls ctl;
	/* Perception. */
	per_player_array<b::target_memory> memory{};
	per_player_array<bool> visible_now{};
	std::optional<uint8_t> target;
	b::delay_line<percept, 16> seen;
	bool shot_clear{};
	uint8_t last_attacker{0xff};
	uint32_t attacked_tick{};
	b::aim_error aim;
	b::strafe_state strafe;
	/* Navigation. */
	bot_goal goal{bot_goal::none};
	uint32_t goal_seg{};
	std::vector<vec3> points;
	/* The edge (source segment, side) each path point lies on. */
	std::vector<std::pair<uint32_t, uint8_t>> point_edges;
	std::size_t point_index{};
	std::size_t steer_index{};
	uint32_t next_plan_tick{};
	b::stuck_detector stuck;
	b::edge_penalties penalties;
	vec3 recover_dir;
	vec3 avoid;
	uint32_t avoid_until{};
	/* Life. */
	bot_life life{bot_life::alive};
	bool death_pending{};
	fix64 died_at{};
	fix64 respawn_at{};
	explicit bot_state(const playernum_t p, const bot_config &c) :
		pid{p}, cfg{c}, stagger{p}
	{
	}
	void reset_for_life(const uint32_t tick)
	{
		pl = {};
		face_dir = {0, 0, 1};
		move_cmd = {};
		fire = false;
		ctl = {};
		/* Section 4.8: memory is cleared, except who killed it. */
		for (auto &&[i, m] : enumerate(memory))
			if (i != last_attacker)
				m = {};
		visible_now = {};
		target.reset();
		seen.clear();
		shot_clear = false;
		attacked_tick = tick;
		aim.reset();
		strafe.reset();
		clear_path();
		goal = bot_goal::none;
		stuck.reset();
		penalties.clear();
		avoid = {};
		avoid_until = 0;
		life = bot_life::alive;
		death_pending = false;
	}
	void clear_path()
	{
		points.clear();
		point_edges.clear();
		point_index = steer_index = 0;
	}
};

/* The ship's limits, the same for every ship (section 3.4). */
struct ship_limits
{
	double max_speed{};
	b::turn_response turn;
};

struct bots_state
{
	per_player_array<std::optional<bot_state>> bots;
	::dcx::net_interp::tick_accumulator tick{b::BOT_TICK_RATE};
	bool tick_started{};
	fix64 last_time{};
	b::nav_graph graph;
	b::astar_search search;
	b::path_result path;
	/* Side centres, 6 per segment. */
	std::vector<vec3> side_centres;
	ship_limits limits;
	/* The controls passed to apply_pilot_controls. */
	control_info controls{};
};

bots_state B;

[[nodiscard]]
bot_state *find_bot(const playernum_t pnum)
{
	if (pnum >= MAX_PLAYERS || !B.bots[pnum])
		return nullptr;
	return &*B.bots[pnum];
}

[[nodiscard]]
bool bots_running()
{
	return +(Game_mode & GM_NETWORK) && multi_i_am_master() && Newdemo_state != ND_STATE_PLAYBACK;
}

[[nodiscard]]
object &ship_of(const playernum_t pnum)
{
	auto &Objects = LevelUniqueObjectState.Objects;
	return *Objects.vmptr(vcplayerptr(pnum)->objnum);
}

/* Navigation graph (section 4.3): segment centres, the child adjacency,
 * the cost through the centre of the shared side.
 */
void build_nav_graph()
{
	auto &LevelSharedVertexState = LevelSharedSegmentState.get_vertex_state();
	auto &Vertices = LevelSharedVertexState.get_vertices();
	auto &vcvertptr = Vertices.vcptr;
	const auto count{static_cast<std::size_t>(Segments.get_count())};
	B.graph.begin(count);
	B.side_centres.assign(count * 6, vec3{});
	for (const auto &&segp : vcsegptridx)
	{
		const uint32_t n{segp.get_unchecked_index()};
		if (n >= count)
			continue;
		B.graph.set_position(n, to_vec(compute_segment_center(vcvertptr, segp)));
		for (const auto side : MAX_SIDES_PER_SEGMENT)
			B.side_centres[n * 6 + underlying_value(side)] = to_vec(compute_center_point_on_side(vcvertptr, segp, side));
	}
	for (const auto &&segp : vcsegptridx)
	{
		const uint32_t n{segp.get_unchecked_index()};
		if (n >= count)
			continue;
		const auto &from{B.graph.position(n)};
		for (const auto side : MAX_SIDES_PER_SEGMENT)
		{
			const auto child{segp->shared_segment::children[side]};
			if (!IS_CHILD(child) || child >= count)
				continue;
			const auto &mid{B.side_centres[n * 6 + underlying_value(side)]};
			const double cost{b::distance(from, mid) + b::distance(mid, B.graph.position(child))};
			B.graph.add_edge(n, {child, underlying_value(side), static_cast<float>(cost)});
		}
	}
	B.graph.finish();
	con_printf(CON_VERBOSE, "bots: navigation graph of %zu segments, %zu edges", B.graph.size(), B.graph.edge_count());
}

/* An edge a bot may take now (section 4.3): open to flying, or a door
 * it can open by flying into it.
 */
[[nodiscard]]
bool edge_passable(const uint32_t from, const b::nav_edge &e, const player_flags powerup_flags)
{
	auto &Walls = LevelUniqueWallSubsystemState.Walls;
	auto &vcwallptr = Walls.vcptr;
	const auto &&segp{vcsegptr(static_cast<segnum_t>(from))};
	const auto side{static_cast<sidenum_t>(e.side)};
	if (WALL_IS_DOORWAY(GameBitmaps, Textures, vcwallptr, segp, side) & WALL_IS_DOORWAY_FLAG::fly)
		return true;
	const auto wall_num{segp->shared_segment::sides[side].wall_num};
	if (wall_num == wall_none)
		return false;
	const auto &w{*vcwallptr(wall_num)};
	if (w.type != WALL_DOOR || +(w.flags & wall_flag::door_locked))
		return false;
	return w.keys == wall_key::none || +(powerup_flags & static_cast<player_flag>(w.keys));
}

/* A clear line from `from` to `to`: `rad` > 0 for a flight path (the
 * ship's body), 0 for sight through grates (FQ_TRANSWALL).
 */
[[nodiscard]]
bool line_clear(const object &obj, const vms_vector &from, const segnum_t from_seg, const vms_vector &to, const fix rad, const bool see_through)
{
	auto &Objects = LevelUniqueObjectState.Objects;
	fvi_info hit;
	const auto type{find_vector_intersection(fvi_query{
		from,
		to,
		fvi_query::unused_ignore_obj_list,
		fvi_query::unused_LevelUniqueObjectState,
		fvi_query::unused_Robot_info,
		see_through ? FQ_TRANSWALL : 0,
		Objects.vcptridx(&obj),
	}, from_seg, rad, hit)};
	return type == fvi_hit_type::None;
}

[[nodiscard]]
bool same_team(const playernum_t a, const playernum_t c)
{
	return +(Game_mode & GM_TEAM) && multi_get_team_from_player(Netgame, a) == multi_get_team_from_player(Netgame, c);
}

/* Section 4.4, trigger discipline: nothing but the target (or another
 * enemy) is first on the line of fire, never a teammate or the reactor
 * (decision 6 of section 11).
 */
[[nodiscard]]
bool shot_line_clear(const bot_state &bs, const object &obj, const object &target)
{
	auto &Objects = LevelUniqueObjectState.Objects;
	fvi_info hit;
	const auto type{find_vector_intersection(fvi_query{
		obj.pos,
		target.pos,
		fvi_query::unused_ignore_obj_list,
		&LevelUniqueObjectState,
		&LevelSharedRobotInfoState.Robot_info,
		FQ_IGNORE_POWERUPS,
		Objects.vcptridx(&obj),
	}, obj.segnum, F1_0 / 2, hit)};
	if (type == fvi_hit_type::None)
		return true;
	if (type != fvi_hit_type::Object || hit.hit_object == object_none)
		return false;
	const auto &o{*Objects.vcptr(hit.hit_object)};
	switch (o.type)
	{
		case object_type::OBJ_WEAPON:
			/* Another shot in flight: it does not stop this one. */
			return true;
		case object_type::OBJ_PLAYER:
		{
			const auto who{get_player_id(o)};
			return who != bs.pid && !same_team(bs.pid, who);
		}
		default:
			/* The reactor, robots, clutter. */
			return false;
	}
}

/* The primary weapon's speed and useful range, in game units. */
[[nodiscard]]
double weapon_speed(const player_info &pi)
{
	const auto &wi{Weapon_info[Primary_weapon_to_weapon_info[pi.Primary_weapon]]};
	return wi.speed[GameUniqueState.Difficulty_level] / 65536.0;
}

[[nodiscard]]
double weapon_range(const player_info &pi)
{
	const auto &wi{Weapon_info[Primary_weapon_to_weapon_info[pi.Primary_weapon]]};
	const double r{weapon_speed(pi) * (wi.lifetime / 65536.0)};
	return std::clamp(r * 0.8, 60.0, 400.0);
}

/* Stage B1: the best primary it can fire (bot_brain.h); a switch costs
 * the rearm time, as select_primary_weapon's does.  No HUD, no sound.
 */
void choose_weapon(object &obj)
{
	auto &pi{obj.ctype.player_info};
	const auto wanted{b::choose_primary({
		.owned = pi.primary_weapon_flags,
		.energy = pi.energy / 65536.0,
		.vulcan_ammo = pi.vulcan_ammo,
	})};
	const auto w{static_cast<primary_weapon_index>(underlying_value(wanted))};
	if (pi.Primary_weapon.get_active() == w)
		return;
	pi.Primary_weapon = w;
	pi.Next_laser_fire_time = GameTime64 + REARM_TIME;
}

/* Plan a path from the ship's segment to `goal_seg` (section 4.3). */
void plan_path(bot_state &bs, const object &obj, const uint32_t goal_seg, const std::optional<vec3> goal_pos, const uint32_t tick)
{
	bs.clear_path();
	bs.next_plan_tick = tick + 2 * b::BOT_TICK_RATE;
	const uint32_t start{obj.segnum};
	if (start >= B.graph.size() || goal_seg >= B.graph.size())
		return;
	const auto flags{obj.ctype.player_info.powerup_flags};
	const auto passable{[flags](const uint32_t from, const b::nav_edge &e) {
		return edge_passable(from, e, flags);
	}};
	const auto extra{[&bs](const uint32_t from, const b::nav_edge &e) {
		return bs.penalties.cost(from, e.side);
	}};
	if (!B.search.find(B.graph, start, goal_seg, passable, extra, BOT_NAV_NODE_LIMIT, B.path))
		return;
	const auto &steps{B.path.steps};
	/* Section 4.3: the centre of each side passed, then the centre of
	 * the segment entered, so a path never cuts a corner.
	 */
	for (std::size_t i = 1; i < steps.size(); ++i)
	{
		const auto from{steps[i - 1].node};
		const auto side{steps[i].side};
		bs.points.push_back(B.side_centres[from * 6 + side]);
		bs.point_edges.emplace_back(from, side);
		bs.points.push_back(B.graph.position(steps[i].node));
		bs.point_edges.emplace_back(from, side);
	}
	if (B.path.complete && goal_pos)
	{
		if (bs.points.empty())
		{
			bs.points.push_back(*goal_pos);
			bs.point_edges.emplace_back(start, b::NAV_NO_SIDE);
		}
		else
			bs.points.back() = *goal_pos;
	}
	bs.stuck.restart_window();
}

void set_goal(bot_state &bs, const object &obj, const bot_goal g, const uint32_t seg, const std::optional<vec3> pos, const uint32_t tick)
{
	if (bs.goal != g || bs.goal_seg != seg)
	{
		bs.stuck.reset();
		bs.penalties.clear();
	}
	bs.goal = g;
	bs.goal_seg = seg;
	plan_path(bs, obj, seg, pos, tick);
}

void choose_roam_goal(bot_state &bs, const object &obj, const uint32_t tick)
{
	const auto n{static_cast<unsigned>(B.graph.size())};
	if (!n)
		return;
	uint32_t seg{bs.rng.below(n)};
	/* Somewhere else, preferably not next door. */
	for (unsigned tries = 0; tries < 8; ++tries)
	{
		if (seg != obj.segnum && b::distance(B.graph.position(seg), to_vec(obj.pos)) > 80)
			break;
		seg = bs.rng.below(n);
	}
	set_goal(bs, obj, bot_goal::roam, seg, std::nullopt, tick);
}

/* Section 4.2: what the bot sees, at 20 Hz. */
void perceive(bot_state &bs, const object &obj, const uint32_t tick)
{
	auto &Objects = LevelUniqueObjectState.Objects;
	const auto &sk{*bs.skill};
	const auto pos{to_vec(obj.pos)};
	const auto frame{to_frame(obj.orient)};
	for (playernum_t i = 0; i < N_players && i < MAX_PLAYERS; ++i)
	{
		bs.visible_now[i] = false;
		if (i == bs.pid)
			continue;
		auto &plr{*vcplayerptr(i)};
		if (plr.connected != player_connection_status::playing)
		{
			bs.memory[i] = {};
			continue;
		}
		const auto &t{*Objects.vcptr(plr.objnum)};
		const bool dying{i == Player_num ? Player_dead_state != player_dead_state::no : bot_ship_dying(i)};
		if (t.type != object_type::OBJ_PLAYER || dying)
		{
			bs.memory[i] = {};
			continue;
		}
		if (same_team(bs.pid, i))
			continue;
		const auto tpos{to_vec(t.pos)};
		const auto to{tpos - pos};
		const double dist{b::length(to)};
		const bool revenge{bs.last_attacker == i && tick - bs.attacked_tick < BOT_REVENGE_TICKS};
		if (dist > sk.awareness && !revenge)
			continue;
		if (!revenge && !b::in_field_of_view(frame.f, to, sk.fov_half_deg))
			continue;
		if (+(t.ctype.player_info.powerup_flags & player_flag::cloaked) && dist > BOT_CLOAK_SEE_DISTANCE)
			continue;
		if (!line_clear(obj, obj.pos, obj.segnum, t.pos, 0, true))
			continue;
		bs.visible_now[i] = true;
		bs.memory[i] = {
			.valid = true,
			.pos = tpos,
			.vel = to_vec(t.mtype.phys_info.velocity),
			.segment = static_cast<uint16_t>(t.segnum),
			.tick = tick,
		};
	}
	percept p;
	bs.shot_clear = false;
	if (bs.target)
	{
		const auto t{*bs.target};
		p.target = t;
		p.visible = bs.visible_now[t];
		p.pos = bs.memory[t].pos;
		p.vel = bs.memory[t].vel;
		if (p.visible)
			bs.shot_clear = shot_line_clear(bs, obj, *Objects.vcptr(vcplayerptr(t)->objnum));
	}
	bs.seen.push(p);
	/* Section 4.3: string pulling, at most `lookahead` probes. */
	if (!bs.points.empty())
	{
		const fix rad{obj.size * 2 / 3};
		bs.steer_index = b::pull_string(bs.point_index, bs.points.size(), 4, [&](const std::size_t k) {
			return line_clear(obj, obj.pos, obj.segnum, to_fixvec(bs.points[k]), rad, false);
		});
	}
	/* Section 4.3: wall avoidance, a probe along the velocity. */
	const auto vel{to_vec(obj.mtype.phys_info.velocity)};
	const double speed{b::length(vel)};
	if (speed > 5)
	{
		const auto end{to_fixvec(pos + vel * 0.4)};
		fvi_info hit;
		const auto type{find_vector_intersection(fvi_query{
			obj.pos,
			end,
			fvi_query::unused_ignore_obj_list,
			fvi_query::unused_LevelUniqueObjectState,
			fvi_query::unused_Robot_info,
			0,
			Objects.vcptridx(&obj),
		}, obj.segnum, obj.size / 2, hit)};
		if (type == fvi_hit_type::Wall)
		{
			bs.avoid = b::normalized(to_vec(hit.hit_wallnorm)) * (B.limits.max_speed * 0.5);
			bs.avoid_until = tick + b::PERCEPTION_DIVISOR * 3;
		}
	}
	/* The host's copy of the bot's inventory, and the clients' (the
	 * energy and ammunition it spent).
	 */
	net_objects_host_own_ship_inventory(bs.pid, false);
}

/* Section 4.4: target choice and the goal, at 5 Hz. */
void think(bot_state &bs, object &obj, const uint32_t tick)
{
	auto &Objects = LevelUniqueObjectState.Objects;
	const auto &sk{*bs.skill};
	const auto pos{to_vec(obj.pos)};
	std::array<b::target_candidate, MAX_PLAYERS> cand{};
	unsigned n{0};
	const unsigned memory_ticks{b::ticks_from_ms(sk.memory_ms)};
	for (playernum_t i = 0; i < N_players && i < MAX_PLAYERS; ++i)
	{
		if (i == bs.pid || !bs.memory[i].valid)
			continue;
		auto &c{cand[n++]};
		c.id = i;
		c.excluded = same_team(bs.pid, i);
		c.visible = bs.visible_now[i];
		c.confidence = b::memory_confidence(bs.memory[i], tick, memory_ticks);
		c.distance = b::distance(pos, bs.memory[i].pos);
		c.damaged_me_recently = bs.last_attacker == i && tick - bs.attacked_tick < BOT_REVENGE_TICKS;
		c.low_shields = Objects.vcptr(vcplayerptr(i)->objnum)->shields < i2f(30);
		c.bounty = +(Game_mode & GM_BOUNTY) && Bounty_target == i;
	}
	bs.target = b::choose_target(std::span(cand.data(), n), bs.target, sk.awareness);
	choose_weapon(obj);
	if (bs.target)
	{
		const auto t{*bs.target};
		if (bs.visible_now[t] && bs.shot_clear)
		{
			/* A direct engagement needs no path. */
			bs.goal = bot_goal::none;
			bs.clear_path();
			return;
		}
		const auto &m{bs.memory[t]};
		if (bs.goal != bot_goal::hunt || bs.goal_seg != m.segment || bs.points.empty() || tick >= bs.next_plan_tick)
			set_goal(bs, obj, bot_goal::hunt, m.segment, m.pos, tick);
		return;
	}
	if (bs.goal == bot_goal::hunt)
		bs.goal = bot_goal::none;
	if (bs.goal == bot_goal::none || bs.points.empty())
		choose_roam_goal(bs, obj, tick);
}

/* Section 4.3: fly along the path; returns the wanted velocity. */
vec3 follow_path(bot_state &bs, object &obj, const bool engaged, const uint32_t tick)
{
	const auto pos{to_vec(obj.pos)};
	const auto frame{to_frame(obj.orient)};
	const double max_speed{B.limits.max_speed};
	if (bs.points.empty())
	{
		if (!engaged)
			bs.face_dir = frame.f;
		return {};
	}
	const double reach{std::max(obj.size / 65536.0 * 1.5, 5.0)};
	bs.point_index = b::advance_along(bs.points, bs.point_index, pos, reach);
	if (bs.steer_index < bs.point_index)
		bs.steer_index = bs.point_index;
	const auto &target{bs.points[bs.steer_index]};
	const auto to{target - pos};
	const double dist{b::length(to)};
	const bool last{bs.steer_index + 1 == bs.points.size()};
	double speed{max_speed};
	if (last)
		speed = std::min(speed, dist * 1.5);
	else
	{
		/* Slow down before a sharp turn. */
		const auto turn{b::angle_between(to, bs.points[bs.steer_index + 1] - target)};
		if (turn > b::radians(60) && dist < 40)
			speed = std::min(speed, std::max(max_speed * 0.4, dist * 1.5));
	}
	if (!engaged)
		bs.face_dir = b::normalized(to);
	/* Stuck recovery (section 4.3). */
	switch (bs.stuck.update(b::remaining_length(bs.points, bs.point_index, pos)))
	{
		case b::stuck_event::none:
			break;
		case b::stuck_event::stuck:
		{
			const auto &e{bs.point_edges[std::min(bs.point_index, bs.point_edges.size() - 1)]};
			if (e.second != b::NAV_NO_SIDE)
				bs.penalties.add(e.first, e.second, 200);
			const double side{bs.rng.uniform() < 0.5 ? -1.0 : 1.0};
			bs.recover_dir = b::normalized(frame.f * -0.7 + frame.r * (0.7 * side) + frame.u * bs.rng.uniform(-0.3, 0.3));
			con_printf(CON_VERBOSE, "bots: '%s' stuck in segment %hu", static_cast<const char *>(bs.cfg.name), static_cast<uint16_t>(obj.segnum));
			break;
		}
		case b::stuck_event::recovered:
			plan_path(bs, obj, bs.goal_seg, std::nullopt, tick);
			break;
		case b::stuck_event::give_up:
			bs.goal = bot_goal::none;
			bs.clear_path();
			return {};
	}
	if (last && dist < reach)
	{
		/* Arrived.  A hunt that finds nobody forgets the target's old
		 * position; a roam picks a new place at the next strategy tick.
		 */
		if (bs.goal == bot_goal::hunt && bs.target)
			bs.memory[*bs.target] = {};
		bs.goal = bot_goal::none;
		bs.clear_path();
		return {};
	}
	return b::normalized(to) * speed;
}

/* One tick of the brain (section 4.1). */
void bot_tick(bot_state &bs, const uint32_t tick)
{
	auto &obj{ship_of(bs.pid)};
	if (bs.life != bot_life::alive || obj.type != object_type::OBJ_PLAYER)
		return;
	const auto &sk{*bs.skill};
	if (b::layer_due(tick, b::PERCEPTION_DIVISOR, bs.stagger))
		perceive(bs, obj, tick);
	if (b::layer_due(tick, b::STRATEGY_DIVISOR, bs.stagger))
		think(bs, obj, tick);
	bs.aim.update(bs.rng, b::radians(sk.aim_sigma_deg), b::ticks_from_ms(sk.aim_drift_ms));
	const int strafe_dir{bs.strafe.update(bs.rng, b::ticks_from_ms(sk.strafe_min_ms), b::ticks_from_ms(sk.strafe_max_ms))};
	const auto pos{to_vec(obj.pos)};
	const auto vel{to_vec(obj.mtype.phys_info.velocity)};
	const auto frame{to_frame(obj.orient)};
	const double max_speed{B.limits.max_speed};
	const unsigned reaction_ticks{b::ticks_from_ms(sk.reaction_ms)};
	const auto *const p{bs.seen.delayed(reaction_ticks / b::PERCEPTION_DIVISOR)};
	bs.fire = false;
	vec3 wanted;
	if (p && p->visible && bs.target && *bs.target == p->target && bs.visible_now[p->target])
	{
		auto &pi{obj.ctype.player_info};
		/* Where the target is now, reckoned from what the bot saw a
		 * reaction time ago: a straight flight is followed, a turn is
		 * noticed late (section 4.2).
		 */
		const double delay{reaction_ticks / static_cast<double>(b::BOT_TICK_RATE)};
		const auto est{p->pos + p->vel * delay};
		const auto aim{b::aim_point(pos, est, p->vel, weapon_speed(pi), sk.lead)};
		bs.face_dir = b::apply_aim_offset(aim - pos, frame.u, bs.aim.yaw(), bs.aim.pitch());
		const auto to{est - pos};
		const double dist{b::length(to)};
		if (bs.shot_clear)
		{
			/* Section 4.6: keep the range, strafe. */
			const auto d{b::normalized(to)};
			const double scale{bs.style->range_scale};
			const double approach{b::range_keeping_speed(dist, BOT_RANGE_LO * scale, BOT_RANGE_HI * scale, max_speed * 0.8)};
			const auto lateral{b::normalized(frame.r - d * b::dot(frame.r, d))};
			const double strafe_speed{sk.strafe ? max_speed * 0.6 : 0};
			wanted = d * approach + lateral * (strafe_dir * strafe_speed);
		}
		else
			wanted = follow_path(bs, obj, true, tick);
		const double err{b::angle_between(frame.f, bs.face_dir)};
		bs.fire = b::should_fire(err, b::radians(sk.fire_cone_deg), bs.shot_clear, dist, weapon_range(pi));
	}
	else
		wanted = follow_path(bs, obj, false, tick);
	if (bs.stuck.recovering())
		wanted = bs.recover_dir * max_speed;
	if (tick < bs.avoid_until)
		wanted += bs.avoid;
	bs.move_cmd = b::velocity_command(wanted, vel, max_speed);
}

/* Section 3.4: the controls of this frame, from the direction and
 * thrust fixed at the last tick and the ship's current state.
 */
void steer(bot_state &bs, const object &obj)
{
	auto &c{bs.ctl};
	if (bs.life != bot_life::alive || obj.type != object_type::OBJ_PLAYER)
	{
		c = {};
		return;
	}
	/* The rotation applies to the orientation without the turn roll
	 * (do_physics_sim_rot).
	 */
	vms_matrix unrolled{obj.orient};
	if (const fixang roll{obj.mtype.phys_info.turnroll})
		unrolled = vm_matrix_x_matrix(obj.orient, vm_angles_2_matrix(vms_angvec{.p = 0, .b = static_cast<fixang>(-roll), .h = 0}));
	const auto frame{to_frame(unrolled)};
	const auto local{frame.to_local(bs.face_dir)};
	const auto e{b::steer_errors_local(local, bs.heading_pref)};
	if (std::abs(e.heading) > 0.2)
		bs.heading_pref = e.heading > 0 ? 1 : -1;
	constexpr double rev_to_rad{2 * std::numbers::pi / 65536.0};
	const auto &rotvel{obj.mtype.phys_info.rotvel};
	const double cap{bs.skill->turn_cap};
	c.pitch = b::rotation_axis(e.pitch, rotvel.x * rev_to_rad, B.limits.turn, cap);
	c.heading = b::rotation_axis(e.heading, rotvel.y * rev_to_rad, B.limits.turn, cap);
	const auto thrust{frame.to_local(bs.move_cmd)};
	c.sideways = std::clamp(thrust.x, -1.0, 1.0);
	c.vertical = std::clamp(thrust.y, -1.0, 1.0);
	c.forward = std::clamp(thrust.z, -1.0, 1.0);
}

void compute_limits()
{
	const auto lin{compute_thrust_response(Player_ship->mass, Player_ship->drag, Player_ship->max_thrust)};
	const auto rot{compute_rotation_response(Player_ship->mass, Player_ship->drag, Player_ship->max_rotthrust)};
	B.limits.max_speed = lin.steady_velocity / 65536.0;
	B.limits.turn = {
		.max_rate = rot.steady_velocity * 2 * std::numbers::pi / 65536.0,
		.time_constant = rot.time_constant,
	};
	if (!(B.limits.max_speed > 1))
		B.limits.max_speed = 50;
	if (!(B.limits.turn.max_rate > 0.1))
		B.limits.turn = {.max_rate = std::numbers::pi, .time_constant = 0.1};
	con_printf(CON_VERBOSE, "bots: ship top speed %.1f, turn rate %.0f deg/s, time constant %.3f s", B.limits.max_speed, B.limits.turn.max_rate * 180 / std::numbers::pi, B.limits.turn.time_constant);
}

/* Spawn invulnerability, as init_player_stats_new_ship gives the human. */
void give_spawn_invulnerability(object &obj)
{
	if (!Netgame.InvulAppear)
		return;
	auto &pi{obj.ctype.player_info};
	pi.powerup_flags |= player_flag::invulnerable;
	pi.invulnerable_time = GameTime64 - (i2f(58 - Netgame.InvulAppear) >> 1);
	pi.FakingInvul = 1;
}

/* A new ship for the bot: spawn grants, invulnerability, weapon, and
 * the announcement (MULTI_REAPPEAR as the bot, then its inventory).
 */
void new_ship(bot_state &bs, object &obj)
{
	give_spawn_invulnerability(obj);
	choose_weapon(obj);
	bs.reset_for_life(B.tick.tick());
	create_player_appearance_effect(Vclip, obj);
	multi_send_reappear(bs.pid);
	net_objects_host_own_ship_inventory(bs.pid, true);
}

/* Section 4.8, step 1: the kill (MULTI_KILL_HOST with the bot in byte
 * 1, from the host) and the tumble.
 */
void start_death(bot_state &bs, object &obj)
{
	bs.death_pending = false;
	bs.life = bot_life::dying;
	bs.pl.dead_state = player_dead_state::yes;
	bs.died_at = GameTime64;
	bs.ctl = {};
	bs.fire = false;
	obj.mtype.phys_info.thrust = {};
	obj.mtype.phys_info.rotthrust = {};
	auto &Objects = LevelUniqueObjectState.Objects;
	multi_send_kill(Objects.vmptridx(&obj));
}

/* Section 4.8, step 2: the explosion.  The deres goes out as the bot's,
 * after its inventory (multi_send_player_deres brings the host's copy up
 * to date); the host drops the eggs from that copy, as for any player.
 */
void explode(bot_state &bs, object &obj, const d_robot_info_array &Robot_info)
{
	auto &Objects = LevelUniqueObjectState.Objects;
	const auto &&objp{Objects.vmptridx(&obj)};
	multi_send_player_deres(deres_explode, bs.pid);
	drop_player_armed_bombs(objp);
	net_objects_host_drop_player_eggs(bs.pid);
	explode_badass_player(Robot_info, objp);
	objp->flags &= ~OF_SHOULD_BE_DEAD;
	multi_make_player_ghost(bs.pid);
	auto &pi{objp->ctype.player_info};
	pi.powerup_flags &= ~(player_flag::cloaked | player_flag::invulnerable);
#if DXX_BUILD_DESCENT == 2
	pi.powerup_flags &= ~player_flag::has_team_flag;
#endif
	bs.life = bot_life::dead;
	bs.respawn_at = GameTime64 + to_fix(bs.rng.uniform(BOT_RESPAWN_MIN_S, BOT_RESPAWN_MAX_S));
}

/* Section 4.8, step 3: respawn at a spawn site. */
void respawn(bot_state &bs, object &obj)
{
	auto &Objects = LevelUniqueObjectState.Objects;
	const auto spawn{choose_spawn(Objects.vmptr, bs.pid, 1)};
	if (spawn.what == spawn_choice::kind::none)
	{
		bs.respawn_at = GameTime64 + F1_0;
		return;
	}
	place_player(vmsegptridx, Objects.vmptridx(&obj), spawn);
	multi_make_ghost_player(bs.pid);
	new_ship(bs, obj);
}

void life_frame(bot_state &bs, object &obj, const d_robot_info_array &Robot_info)
{
	auto &Objects = LevelUniqueObjectState.Objects;
	switch (bs.life)
	{
		case bot_life::alive:
		{
			if (bs.death_pending)
			{
				start_death(bs, obj);
				break;
			}
			/* do_invulnerable_stuff, for the bot. */
			auto &pi{obj.ctype.player_info};
			if (+(pi.powerup_flags & player_flag::invulnerable) && GameTime64 > pi.invulnerable_time + INVULNERABLE_TIME_MAX)
			{
				pi.powerup_flags &= ~player_flag::invulnerable;
				pi.FakingInvul = 0;
			}
			break;
		}
		case bot_life::dying:
		{
			const fix64 t{GameTime64 - bs.died_at};
			const fix left{static_cast<fix>(std::max<fix64>(0, BOT_DEATH_EXPLODE_TIME - t))};
			auto &rotvel{obj.mtype.phys_info.rotvel};
			rotvel.x = left / 4;
			rotvel.y = left / 2;
			rotvel.z = left / 3;
			if (t > BOT_DEATH_EXPLODE_TIME)
			{
				explode(bs, obj, Robot_info);
				break;
			}
			if (bs.rng.uniform() * 32768 < FrameTime * 4)
			{
				multi_send_create_explosion(bs.pid);
				create_small_fireball_on_object(Objects.vmptridx(&obj), F1_0, 1);
			}
			break;
		}
		case bot_life::dead:
			/* No respawn during the countdown: dead in the mine. */
			if (GameTime64 >= bs.respawn_at && !LevelUniqueObjectState.ControlCenterState.Control_center_destroyed && vcplayerptr(bs.pid)->connected == player_connection_status::playing)
				respawn(bs, obj);
			break;
	}
}

[[nodiscard]]
bool callsign_taken(const callsign_t &name, const playernum_t except)
{
	for (playernum_t i = 0; i < MAX_PLAYERS; ++i)
	{
		if (i == except)
			continue;
		const bool in_game{i == Player_num || Netgame.players[i].connected != player_connection_status::disconnected || B.bots[i]};
		if (in_game && !d_stricmp(Netgame.players[i].callsign, name))
			return true;
	}
	return false;
}

/* Section 2.2: unique against every callsign in the game
 * ("havoc" -> "havoc2").
 */
callsign_t unique_callsign(const callsign_t &wanted, const playernum_t slot)
{
	if (!callsign_taken(wanted, slot) && wanted[0u])
		return wanted;
	const char *const base{wanted[0u] ? static_cast<const char *>(wanted) : "bot"};
	for (unsigned k = 2; k < 100; ++k)
	{
		char suffix[4];
		const auto sl{static_cast<std::size_t>(std::snprintf(suffix, sizeof(suffix), "%u", k))};
		char buf[CALLSIGN_LEN + 1]{};
		const auto keep{std::min(std::strlen(base), CALLSIGN_LEN - sl)};
		std::memcpy(buf, base, keep);
		std::memcpy(buf + keep, suffix, sl);
		callsign_t c{};
		c.copy_lower(std::span<const char>(buf, keep + sl));
		if (!callsign_taken(c, slot))
			return c;
	}
	return wanted;
}

}

bool bot_is_local(const playernum_t pnum)
{
	return find_bot(pnum) != nullptr;
}

bool bot_ship_dying(const playernum_t pnum)
{
	const auto bs{find_bot(pnum)};
	return bs && bs->life == bot_life::dying;
}

void bots_session_reset()
{
	for (auto &o : B.bots)
	{
		if (!o)
			continue;
		/* The slot is free again (a game that did not start keeps the
		 * lobby's player list).
		 */
		const auto pid{o->pid};
		vmplayerptr(pid)->connected = player_connection_status::disconnected;
		Netgame.players[pid].connected = player_connection_status::disconnected;
		Netgame.players[pid].callsign = {};
		o.reset();
	}
	B.graph.clear();
	B.side_centres.clear();
	B.tick_started = false;
}

unsigned bots_allocate_slots()
{
	bots_session_reset();
	if (!(Game_mode & GM_NETWORK) || !multi_i_am_master() || !bots_allowed_in_mode(Netgame.gamemode))
		return 0;
	bots_setup_init();
	unsigned placed{0};
	const unsigned limit{std::min<unsigned>(Netgame.max_numplayers, MAX_PLAYERS)};
	for (unsigned k = 0; k < Bot_setup.count; ++k)
	{
		/* Section 2.3: the lowest free slot below the player limit. */
		playernum_t slot{MAX_PLAYERS};
		for (playernum_t s = 1; s < limit; ++s)
		{
			if (B.bots[s] || vcplayerptr(s)->connected != player_connection_status::disconnected)
				continue;
			if (s < N_players && Netgame.players[s].callsign[0u])
				continue;
			slot = s;
			break;
		}
		if (slot >= MAX_PLAYERS)
			break;
		const auto &cfg{Bot_setup.bots[k]};
		const auto name{unique_callsign(cfg.name, slot)};
		auto &np{Netgame.players[slot]};
		np.callsign = name;
		np.rank = netplayer_info::player_rank::None;
		np.connected = player_connection_status::playing;
		np.protocol.udp.addr = {};
		np.LastPacketTime = timer_query();
		np.ping = 0;
		auto &plr{*vmplayerptr(slot)};
		plr.callsign = name;
		plr.connected = player_connection_status::playing;
		ship_of(slot).ctype.player_info.KillGoalCount = 0;
		auto &bs{B.bots[slot].emplace(slot, cfg)};
		bs.cfg.name = name;
		if (slot >= N_players)
			N_players = slot + 1;
		++placed;
		con_printf(CON_NORMAL, "bots: '%s' takes P#%u", static_cast<const char *>(name), slot);
	}
	Netgame.numplayers = N_players;
	if (placed < Bot_setup.count)
		con_printf(CON_URGENT, "bots: only %u of %u bots fit below the player limit of %u", placed, Bot_setup.count, limit);
	return placed;
}

void bots_apply_team_preferences(unsigned &team_vector, const unsigned num_players)
{
	for (playernum_t i = 0; i < num_players && i < MAX_PLAYERS; ++i)
		if (const auto bs{find_bot(i)})
		{
			if (bs->cfg.team == b::bot_team::blue)
				team_vector &= ~(1u << i);
			else if (bs->cfg.team == b::bot_team::red)
				team_vector |= 1u << i;
		}
}

void bots_level_start()
{
	if (!bots_running() || std::ranges::none_of(B.bots, [](const auto &o) { return o.has_value(); }))
		return;
	build_nav_graph();
	compute_limits();
	B.tick = ::dcx::net_interp::tick_accumulator{b::BOT_TICK_RATE};
	B.tick.reset(GameTime64);
	B.last_time = GameTime64;
	B.tick_started = true;
	for (auto &o : B.bots)
	{
		if (!o)
			continue;
		auto &bs{*o};
		bs.rng.seed(b::bot_seed(Netgame.protocol.udp.session_id, bs.pid, Current_level_num));
		bs.last_attacker = 0xff;
		bs.memory = {};
		if (vcplayerptr(bs.pid)->connected != player_connection_status::playing)
			continue;
		auto &obj{ship_of(bs.pid)};
		obj.type = object_type::OBJ_PLAYER;
		obj.control_source = object::control_type::remote;
		obj.movement_source = object::movement_type::physics;
		init_player_stats_new_ship(bs.pid);
		new_ship(bs, obj);
	}
}

void bots_level_end()
{
	for (auto &o : B.bots)
	{
		if (!o)
			continue;
		auto &plr{*vmplayerptr(o->pid)};
		/* The kill list waits for every player still in the level. */
		if (plr.connected == player_connection_status::playing)
			plr.connected = player_connection_status::end_menu;
		o->death_pending = false;
		o->ctl = {};
		o->fire = false;
	}
}

void bots_frame(const d_robot_info_array &Robot_info)
{
	if (!B.tick_started || !bots_running())
		return;
	if (Network_status != network_state::playing)
		return;
	/* The game clock went back (it restarts with each level): the tick
	 * restarts with it.
	 */
	if (GameTime64 < B.last_time)
		B.tick.reset(GameTime64);
	B.last_time = GameTime64;
	const unsigned ticks{B.tick.advance(GameTime64)};
	const uint32_t first{B.tick.tick() - ticks};
	for (auto &o : B.bots)
	{
		if (!o)
			continue;
		auto &bs{*o};
		if (vcplayerptr(bs.pid)->connected != player_connection_status::playing)
		{
			bs.ctl = {};
			continue;
		}
		auto &obj{ship_of(bs.pid)};
		life_frame(bs, obj, Robot_info);
		for (unsigned k = 0; k < ticks; ++k)
			bot_tick(bs, first + k);
		steer(bs, obj);
	}
}

void bots_fire()
{
	if (!B.tick_started || !bots_running() || Network_status != network_state::playing)
		return;
	auto &Objects = LevelUniqueObjectState.Objects;
	for (auto &o : B.bots)
	{
		if (!o || !o->fire || o->life != bot_life::alive)
			continue;
		auto &bs{*o};
		const auto &&objp{Objects.vmptridx(vcplayerptr(bs.pid)->objnum)};
		if (objp->type != object_type::OBJ_PLAYER)
			continue;
		if (!allowed_to_fire_laser(bs.pl, objp->ctype.player_info))
			continue;
		do_laser_firing_player(bs.pl, objp);
	}
}

void bot_apply_controls(object &obj)
{
	if (obj.type != object_type::OBJ_PLAYER)
		return;
	const auto bs{find_bot(get_player_id(obj))};
	if (!bs || !bots_running())
		return;
	auto &c{B.controls};
	/* The host's exit sequence moves the objects without the game frame:
	 * the bots coast.
	 */
	const bot_controls bc{Endlevel_sequence ? bot_controls{} : bs->ctl};
	const fix ft{FrameTime};
	c.pitch_time = fixmul(to_fix(bc.pitch), ft);
	c.heading_time = fixmul(to_fix(bc.heading), ft);
	c.bank_time = 0;
	c.forward_thrust_time = fixmul(to_fix(bc.forward), ft);
	c.sideways_thrust_time = fixmul(to_fix(bc.sideways), ft);
	c.vertical_thrust_time = fixmul(to_fix(bc.vertical), ft);
#if DXX_BUILD_DESCENT == 2
	c.state.afterburner = 0;
#endif
	apply_pilot_controls(obj, bs->pl, c);
}

bool bot_take_damage(object &ship, const icobjptridx_t killer, const fix damage, const bool check_friendly_fire)
{
	if (ship.type != object_type::OBJ_PLAYER)
		return false;
	const auto pid{get_player_id(ship)};
	const auto bs{find_bot(pid)};
	if (!bs)
		return false;
	if (bs->life != bot_life::alive || bs->death_pending)
		return true;
	auto &pi{ship.ctype.player_info};
	if (+(pi.powerup_flags & player_flag::invulnerable))
		return true;
	if (check_friendly_fire && multi_maybe_disable_friendly_fire(static_cast<const object *>(killer), pid))
		return true;
	if (Endlevel_sequence)
		return true;
	/* Section 4.2: a hit tells the bot roughly where the attacker is. */
	if (killer != object_none && killer->type == object_type::OBJ_PLAYER)
	{
		const auto who{get_player_id(*killer)};
		if (who != pid && who < MAX_PLAYERS)
		{
			const auto tick{B.tick.tick()};
			bs->last_attacker = who;
			bs->attacked_tick = tick;
			auto &m{bs->memory[who]};
			if (!m.valid || tick - m.tick > b::PERCEPTION_DIVISOR * 4)
			{
				const vec3 err{bs->rng.uniform(-20, 20), bs->rng.uniform(-20, 20), bs->rng.uniform(-20, 20)};
				m = {
					.valid = true,
					.pos = to_vec(killer->pos) + err,
					.vel = {},
					.segment = static_cast<uint16_t>(killer->segnum),
					.tick = tick,
				};
			}
		}
	}
	ship.shields -= damage;
	if (ship.shields < 0)
	{
		pi.killer_objnum = killer;
		/* The death starts with the next frame, as the human's does
		 * (obj_delete_all_that_should_be_dead), outside the collision
		 * handling.
		 */
		bs->death_pending = true;
	}
	return true;
}

bool bot_hit_wall(const object &ship, const vmsegptridx_t seg, const sidenum_t side)
{
	if (ship.type != object_type::OBJ_PLAYER || !bot_is_local(get_player_id(ship)))
		return false;
	/* Section 4.3: a bot opens a door by flying into it, as a human does
	 * (wall_hit_process, without its HUD messages).
	 */
	const auto wall_num{seg->shared_segment::sides[side].wall_num};
	if (wall_num == wall_none)
		return true;
	auto &Walls = LevelUniqueWallSubsystemState.Walls;
	auto &w{*Walls.vmptr(wall_num)};
	if (w.type != WALL_DOOR || +(w.flags & wall_flag::door_locked))
		return true;
	if (w.keys != wall_key::none && !(ship.ctype.player_info.powerup_flags & static_cast<player_flag>(w.keys)))
		return true;
	if (w.state != wall_state::opening)
	{
		wall_open_door(seg, side);
		multi_send_door_open(seg, side, w.flags);
	}
	return true;
}

}

#endif
