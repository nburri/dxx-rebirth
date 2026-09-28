/*
 * This file is part of the DXX-Rebirth project <https://github.com/dxx-rebirth/dxx-rebirth/>.
 * It is copyright by its individual contributors, as recorded in the
 * project's Git history.  See COPYING.txt at the top level for license
 * terms and a link to the Git history.
 */
/*
 * Multiplayer bots on the host (Documentation/multiplayer-bots.md, stages
 * B1, B3 and B4): the slots bots take, their brain on a fixed tick
 * (perception, target choice, goals, navigation, aim), the controls it
 * produces, firing and the weapon choice, missiles and mines, pickups,
 * fuel centres, the afterburner, the converter, damage, death and
 * respawn.
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
#include "bot_goals.h"
#include "bot_nav.h"
#include "bot_weapons.h"
#include "net_v2_objects.h"
#include "net_interp.h"
#include "net_v2_game.h"
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
#include "powerup.h"
#include "fuelcen.h"
#include "digi.h"
#include "sounds.h"

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
/* A roaming bot flies at least this far (section 4.3). */
constexpr double BOT_ROAM_MIN_DISTANCE{120};
/* A hunt toward a target that moves to another segment is planned again
 * at most this often (the plan also expires after 2 s).
 */
constexpr unsigned BOT_HUNT_REPLAN_TICKS{b::BOT_TICK_RATE / 2};
/* Section 4.6, dodge: projectiles within this distance are looked at,
 * their closest pass within this time, for this long a dodge.
 */
constexpr double BOT_DODGE_SCAN{150};
constexpr double BOT_DODGE_HORIZON{0.7};
constexpr unsigned BOT_DODGE_TICKS{b::ticks_from_ms(350)};
/* Section 4.3: a wall beyond the steer point is a bend only while the
 * velocity points within this angle of the steer point.
 */
constexpr double BOT_BEND_MAX_ANGLE{b::radians(40)};
/* Stage B3 (section 4.7): the path costs a bot computes at each strategy
 * tick reach this far, over at most this many segments.
 */
constexpr double BOT_DISTANCE_MAX_COST{2500};
constexpr unsigned BOT_DISTANCE_NODE_LIMIT{3000};
/* Powerups whose line of sight a bot checks per strategy tick. */
constexpr unsigned BOT_POWERUP_LOS_BUDGET{6};
/* A powerup the bot came to without taking it, or could not reach, is no
 * goal for this long.
 */
constexpr unsigned BOT_COLLECT_IGNORE_TICKS{5 * b::BOT_TICK_RATE};
constexpr unsigned BOT_COLLECT_UNREACHABLE_TICKS{10 * b::BOT_TICK_RATE};
/* Fuel and repair centres give this much per second (Fuelcen_give_amount). */
constexpr fix BOT_FUELCEN_RATE{i2f(25)};
constexpr fix BOT_FUELCEN_SOUND_DELAY{F1_0 / 4};
/* A retreat without a known shield source draws this many places. */
constexpr unsigned BOT_FLEE_TRIES{10};
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
	/* Stage B3 (section 4.7): fly to a powerup. */
	collect,
	/* Weak and threatened: away, to shields if it knows any. */
	retreat,
	/* To a fuel or repair centre, and hover there until full. */
	refuel,
};

struct bot_controls
{
	double pitch{}, heading{}, forward{}, sideways{}, vertical{};
};

struct bot_state
{
	playernum_t pid;
	bot_config cfg;
	b::bot_skill skill_level{BOT_B1_SKILL};
	const b::skill_params *skill{&b::skill_of(BOT_B1_SKILL)};
	const b::style_params *style{&b::style_of(b::bot_style::balanced)};
	pilot pl{};
	b::bot_rng rng;
	/* The stagger of the slower layers: the slot. */
	unsigned stagger{};
	/* Brain output, fixed at the last tick. */
	vec3 face_dir{0, 0, 1};
	/* The angular velocity of face_dir (the steering's feed-forward). */
	vec3 face_rate;
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
	/* Section 4.4: the lead factor, 1 on average (b::aim_lead). */
	b::aim_lead lead;
	/* Section 4.5: the range band of the weapon choice (hysteresis). */
	std::optional<b::range_band> band;
	b::juke_state juke;
	/* Section 4.6, dodge: the salt of this life's rolls (b::dodge_roll),
	 * and the dodge under way.
	 */
	uint32_t dodge_salt{};
	vec3 dodge_dir;
	uint32_t dodge_from{}, dodge_until{};
	/* Stage B3 (section 4.7): the powerups it knows, the path costs from
	 * where it is (each strategy tick), the powerup or centre it goes to.
	 */
	b::powerup_memory powerups;
	b::nav_distances dist;
	uint16_t collect_key{0xffff}, collect_sig{};
	uint32_t refuel_seg{};
	/* The afterburner is lit. */
	bool burning{};
	/* Stage B4 (section 9.4): the missile chosen and waiting for its
	 * release (until missile_until), the one to fire (and the rest of
	 * its volley), when the last missile, heavy missile and mine went,
	 * and the target of the last heavy one.
	 */
	std::optional<b::secondary> missile;
	uint32_t missile_until{};
	std::optional<b::secondary> missile_fire;
	unsigned missile_volley{};
	std::optional<uint32_t> last_missile, last_heavy, last_mine;
	uint8_t heavy_target{0xff};
	/* Section 4.7: cloak and invulnerability (b::tactics_for), from the
	 * last strategy tick.
	 */
	b::powerup_tactics tactics;
	fix64 fuel_sound_at{};
	/* Navigation. */
	bot_goal goal{bot_goal::none};
	uint32_t goal_seg{};
	std::vector<vec3> points;
	/* The edge (source segment, side) each path point lies on. */
	std::vector<std::pair<uint32_t, uint8_t>> point_edges;
	std::size_t point_index{};
	std::size_t steer_index{};
	uint32_t next_plan_tick{};
	uint32_t last_plan_tick{};
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
		face_rate = {};
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
		lead.reset();
		band.reset();
		juke.reset();
		dodge_salt = rng.next();
		dodge_dir = {};
		dodge_from = dodge_until = 0;
		collect_key = 0xffff;
		burning = false;
		missile.reset();
		missile_fire.reset();
		missile_volley = 0;
		last_missile.reset();
		last_heavy.reset();
		last_mine.reset();
		heavy_target = 0xff;
		tactics = {};
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
		stuck.restart_window();
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
	/* Stage B3: the fuel centres and repair centres of the level. */
	std::vector<uint32_t> fuel_centres;
	std::vector<uint32_t> repair_centres;
	ship_limits limits;
	/* The controls passed to apply_pilot_controls. */
	control_info controls{};
};

bots_state B;

[[nodiscard]]
bot_state *find_bot(const playernum_t pnum)
{
	if (pnum >= MAX_PLAYERS)
		return nullptr;
	auto &o{B.bots[pnum]};
	if (!o)
		return nullptr;
	/* A human's connection for the slot: the bot is gone (the slot was
	 * released without the hooks, which must not happen).
	 */
	if (!b::slot_flown_by_bot(true, net_v2::host_slot_has_peer(pnum)))
	{
		o.reset();
		return nullptr;
	}
	return &*o;
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
	B.fuel_centres.clear();
	B.repair_centres.clear();
	for (const auto &&segp : vcsegptridx)
	{
		const uint32_t n{segp.get_unchecked_index()};
		if (n >= count)
			continue;
		const shared_segment &ss{segp};
		if (ss.special == segment_special::fuelcen)
			B.fuel_centres.push_back(n);
		else if (ss.special == segment_special::repaircen)
			B.repair_centres.push_back(n);
	}
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

/* The speed of the shots of the ship's primary weapon, in game units,
 * as the lead needs it (section 4.4): the weapon data's speed at the
 * game's difficulty, which is the speed Laser_create_new gives the shot
 * (GameUniqueState.Difficulty_level: in multiplayer the netgame's).
 */
[[nodiscard]]
double weapon_speed(const player_info &pi)
{
	const auto &wi{Weapon_info[Primary_weapon_to_weapon_info[pi.Primary_weapon]]};
	return b::effective_shot_speed(wi.speed[GameUniqueState.Difficulty_level] / 65536.0, wi.thrust != 0);
}

/* How far its shots fly before they expire: the bot does not fire
 * beyond that (four fifths of it, with a floor for the data's odd
 * weapons).
 */
[[nodiscard]]
double weapon_range(const player_info &pi)
{
	const auto &wi{Weapon_info[Primary_weapon_to_weapon_info[pi.Primary_weapon]]};
	const double r{weapon_speed(pi) * (wi.lifetime / 65536.0)};
	return std::clamp(r * 0.8, 20.0, 400.0);
}

/* The gun a primary's shots leave from, in the ship's frame (right, up,
 * forward): the average of the guns do_laser_firing uses, since every
 * shot flies parallel to the nose from its gun.  Section 4.4: the lead
 * is solved from there, not from the ship's centre.
 */
[[nodiscard]]
vec3 gun_local(const primary_weapon_index w, const bool quad)
{
	const auto gun{[](const player_gun_number g) {
		return to_vec(Player_ship->gun_points[g]);
	}};
	switch (w)
	{
		case primary_weapon_index::laser:
			if (quad)
				return (gun(player_gun_number::_0) + gun(player_gun_number::_1) + gun(player_gun_number::_2) + gun(player_gun_number::_3)) * 0.25;
			[[fallthrough]];
		case primary_weapon_index::plasma:
		case primary_weapon_index::fusion:
#if DXX_BUILD_DESCENT == 2
		case primary_weapon_index::phoenix:
#endif
			return (gun(player_gun_number::_0) + gun(player_gun_number::_1)) * 0.5;
#if DXX_BUILD_DESCENT == 2
		case primary_weapon_index::omega:
			return gun(player_gun_number::_1);
#endif
		default:
			return gun(player_gun_number::center);
	}
}

/* The half angle (radians) of a primary's shot pattern (do_laser_firing:
 * spreadfire's outer shots 1/16 off the nose, helix's 2/16).
 */
[[nodiscard]]
double spread_half_angle(const primary_weapon_index w)
{
	switch (w)
	{
		case primary_weapon_index::spreadfire:
			return std::atan(1.0 / 16);
#if DXX_BUILD_DESCENT == 2
		case primary_weapon_index::helix:
			return std::atan(2.0 / 16);
#endif
		default:
			return 0;
	}
}

/* The energy a primary uses per second of continuous fire
 * (do_laser_firing_player: the difficulty's cost per volley, helix twice
 * in multiplayer, fire_wait between volleys); 0 for the ammunition
 * weapons and omega (charged, not fired from the energy).
 */
[[nodiscard]]
std::array<double, 10> energy_rates()
{
	std::array<double, 10> r{};
	for (unsigned i = 0; i < r.size() && i < MAX_PRIMARY_WEAPONS; ++i)
	{
		const auto w{static_cast<primary_weapon_index>(i)};
		const auto id{Primary_weapon_to_weapon_info[w]};
		if (id >= N_weapon_types)
			continue;
		const auto &wi{Weapon_info[id]};
		if (weapon_index_uses_vulcan_ammo(w))
			continue;
#if DXX_BUILD_DESCENT == 2
		if (w == primary_weapon_index::omega)
			continue;
#endif
		double e{wi.energy_usage / 65536.0};
		const auto d{GameUniqueState.Difficulty_level};
		if (d == Difficulty_level_type::_0 || d == Difficulty_level_type::_1)
			e *= (underlying_value(d) + 2) / 4.0;
#if DXX_BUILD_DESCENT == 2
		if (id == weapon_id_type::HELIX_ID && +(Game_mode & GM_MULTI))
			e *= 2;
#endif
		const double wait{std::max(wi.fire_wait / 65536.0, 0.01)};
		r[i] = e / wait;
	}
	return r;
}

/* Stage B4: a secondary's weapon data (b::missile_data). */
[[nodiscard]]
b::missile_data missile_data_of(const secondary_weapon_index w)
{
	const auto &wi{Weapon_info[Secondary_weapon_to_weapon_info[w]]};
	return {
		.speed = wi.speed[GameUniqueState.Difficulty_level] / 65536.0,
		.blast_radius = wi.damage_radius / 65536.0,
		.thrust = wi.thrust != 0,
	};
}

[[nodiscard]]
secondary_weapon_index game_secondary(const b::secondary s)
{
	return static_cast<secondary_weapon_index>(static_cast<uint8_t>(s));
}

/* The gun a secondary leaves from (do_missile_firing: guns 4 and 5 in
 * turn for the missiles), in the ship's frame.
 */
[[nodiscard]]
vec3 missile_gun_local(const secondary_weapon_index w)
{
	const auto g{Secondary_weapon_to_gun_num[w]};
	if (g == player_gun_number::_4)
		return (to_vec(Player_ship->gun_points[player_gun_number::_4]) + to_vec(Player_ship->gun_points[static_cast<player_gun_number>(5)])) * 0.5;
	return to_vec(Player_ship->gun_points[g]);
}

[[nodiscard]]
bool has_flag(const player_info &pi, const player_flag f)
{
	return +(pi.powerup_flags & f) ? true : false;
}

[[nodiscard]]
b::weapon_view weapons_of(const player_info &pi)
{
	return {
		.owned = pi.primary_weapon_flags,
		.laser_level = underlying_value(pi.laser_level),
		.quad = has_flag(pi, player_flag::quad_lasers),
		.energy = pi.energy / 65536.0,
		.vulcan_ammo = pi.vulcan_ammo,
		.energy_rate = energy_rates(),
	};
}

/* Section 4.5: the primary for the range to the target (none: no
 * target), from the weapon table (bot_goals.h); below weapon smarts 2
 * the fixed order of stage B1.  A switch costs the rearm time, as
 * select_primary_weapon's does.  No HUD, no sound.
 */
void choose_weapon(const bot_state &bs, object &obj, const std::optional<b::range_band> band)
{
	auto &pi{obj.ctype.player_info};
	const auto active{pi.Primary_weapon.get_active()};
	const auto current{static_cast<b::primary>(underlying_value(active))};
	const auto wanted{bs.skill->weapon_smarts < 2
		? b::choose_primary({
			.owned = pi.primary_weapon_flags,
			.energy = pi.energy / 65536.0,
			.vulcan_ammo = pi.vulcan_ammo,
		})
		: b::choose_primary_for(weapons_of(pi), band, current)};
	const auto w{static_cast<primary_weapon_index>(underlying_value(wanted))};
	if (active == w)
		return;
	pi.Primary_weapon = w;
	pi.Next_laser_fire_time = GameTime64 + REARM_TIME;
}

/* Section 4.7: what the bot has, for the collection values. */
[[nodiscard]]
b::resource_view resources_of(const object &obj)
{
	const auto &pi{obj.ctype.player_info};
	b::resource_view r;
	r.shields = obj.shields / 65536.0;
	r.energy = pi.energy / 65536.0;
	r.weapons = weapons_of(pi);
	r.ammo_rack = has_flag(pi, player_flag::ammo_rack);
	const double ammo_max{static_cast<double>(VULCAN_AMMO_MAX) * (r.ammo_rack ? 2 : 1)};
	r.vulcan_ammo_share = pi.vulcan_ammo / ammo_max;
	r.afterburner = has_flag(pi, player_flag::afterburner);
	r.converter = has_flag(pi, player_flag::converter);
	r.cloaked = has_flag(pi, player_flag::cloaked);
	r.invulnerable = has_flag(pi, player_flag::invulnerable) && !pi.FakingInvul;
	return r;
}

/* Section 4.7: what a powerup is to a bot. */
[[nodiscard]]
b::item_desc item_of(const powerup_type_t id)
{
	using b::item;
	const auto primary{[](const b::primary p) {
		return b::item_desc{item::primary, p, 0};
	}};
	const auto missile{[](const secondary_weapon_index w) {
		return b::item_desc{item::secondary, b::primary::laser, underlying_value(w)};
	}};
	switch (id)
	{
		case powerup_type_t::POW_ENERGY:
			return {item::energy};
		case powerup_type_t::POW_SHIELD_BOOST:
			return {item::shield};
		case powerup_type_t::POW_LASER:
			return {item::laser};
		case powerup_type_t::POW_QUAD_FIRE:
			return {item::quad};
		case powerup_type_t::POW_VULCAN_WEAPON:
			return primary(b::primary::vulcan);
		case powerup_type_t::POW_SPREADFIRE_WEAPON:
			return primary(b::primary::spreadfire);
		case powerup_type_t::POW_PLASMA_WEAPON:
			return primary(b::primary::plasma);
		case powerup_type_t::POW_FUSION_WEAPON:
			return primary(b::primary::fusion);
		case powerup_type_t::POW_VULCAN_AMMO:
			return {item::vulcan_ammo};
		case powerup_type_t::POW_MISSILE_1:
		case powerup_type_t::POW_MISSILE_4:
			return missile(secondary_weapon_index::concussion);
		case powerup_type_t::POW_HOMING_AMMO_1:
		case powerup_type_t::POW_HOMING_AMMO_4:
			return missile(secondary_weapon_index::homing);
		case powerup_type_t::POW_PROXIMITY_WEAPON:
			return missile(secondary_weapon_index::proximity);
		case powerup_type_t::POW_SMARTBOMB_WEAPON:
			return missile(secondary_weapon_index::smart);
		case powerup_type_t::POW_MEGA_WEAPON:
			return missile(secondary_weapon_index::mega);
		case powerup_type_t::POW_CLOAK:
			return {item::cloak};
		case powerup_type_t::POW_INVULNERABILITY:
			return {item::invulnerability};
#if DXX_BUILD_DESCENT == 2
		case powerup_type_t::POW_GAUSS_WEAPON:
			return primary(b::primary::gauss);
		case powerup_type_t::POW_HELIX_WEAPON:
			return primary(b::primary::helix);
		case powerup_type_t::POW_PHOENIX_WEAPON:
			return primary(b::primary::phoenix);
		case powerup_type_t::POW_OMEGA_WEAPON:
			return primary(b::primary::omega);
		case powerup_type_t::POW_SUPER_LASER:
			return {item::super_laser};
		case powerup_type_t::POW_FULL_MAP:
			return {item::full_map};
		case powerup_type_t::POW_CONVERTER:
			return {item::converter};
		case powerup_type_t::POW_AMMO_RACK:
			return {item::ammo_rack};
		case powerup_type_t::POW_AFTERBURNER:
			return {item::afterburner};
		case powerup_type_t::POW_HEADLIGHT:
			return {item::headlight};
		case powerup_type_t::POW_SMISSILE1_1:
		case powerup_type_t::POW_SMISSILE1_4:
			return missile(secondary_weapon_index::flash);
		case powerup_type_t::POW_GUIDED_MISSILE_1:
		case powerup_type_t::POW_GUIDED_MISSILE_4:
			return missile(secondary_weapon_index::guided);
		case powerup_type_t::POW_SMART_MINE:
			return missile(secondary_weapon_index::smart_mine);
		case powerup_type_t::POW_MERCURY_MISSILE_1:
		case powerup_type_t::POW_MERCURY_MISSILE_4:
			return missile(secondary_weapon_index::mercury);
		case powerup_type_t::POW_EARTHSHAKER_MISSILE:
			return missile(secondary_weapon_index::earthshaker);
#endif
		default:
			return {};
	}
}

/* Plan a path from the ship's segment to `goal_seg` (section 4.3). */
void plan_path(bot_state &bs, const object &obj, const uint32_t goal_seg, const std::optional<vec3> goal_pos, const uint32_t tick)
{
	bs.clear_path();
	bs.next_plan_tick = tick + 2 * b::BOT_TICK_RATE;
	bs.last_plan_tick = tick;
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
	if (B.graph.size() < 2)
		return;
	/* Somewhere else, not next door (bot_nav.h). */
	const uint32_t seg{b::pick_roam_goal(B.graph, obj.segnum, to_vec(obj.pos), BOT_ROAM_MIN_DISTANCE, [&bs](const uint32_t n) {
		return bs.rng.below(n);
	})};
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
	/* Section 4.3: wall avoidance, a probe along the velocity.  It takes
	 * away the speed into the wall, the more the nearer the wall, and
	 * pushes off a little.  A wall beyond the path point the bot steers
	 * at is the bend it is about to take, not an obstacle: B1 braked for
	 * every bend of a corridor, which made the bots crawl.
	 */
	const auto vel{to_vec(obj.mtype.phys_info.velocity)};
	const double speed{b::length(vel)};
	if (speed > 5)
	{
		constexpr double probe_time{0.4};
		const auto end{to_fixvec(pos + vel * probe_time)};
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
			const double hit_distance{b::distance(pos, to_vec(hit.hit_pnt))};
			/* Fighting in the open, the bot does not follow its path. */
			const bool following{!(p.visible && bs.shot_clear)};
			const bool bend{following && bs.steer_index < bs.points.size() && b::wall_hit_is_bend(pos, vel, bs.points[bs.steer_index], hit_distance, BOT_BEND_MAX_ANGLE)};
			const auto normal{b::normalized(to_vec(hit.hit_wallnorm))};
			const double into{-b::dot(vel, normal)};
			if (!bend && into > 0)
			{
				const double nearness{1 - std::clamp(hit_distance / (speed * probe_time), 0.0, 1.0)};
				bs.avoid = normal * (into * (0.5 + nearness) + B.limits.max_speed * 0.1);
				bs.avoid_until = tick + b::PERCEPTION_DIVISOR * 2;
			}
		}
	}
	/* Section 4.6, dodge: each projectile coming at the bot gets one roll
	 * (b::dodge_roll); with the skill's probability the bot thrusts across
	 * its flight for a moment, a reaction time later.  The bot's own shots
	 * and, without friendly fire, its partners' are not dodged.
	 */
	if (bs.skill->dodge_prob > 0 && tick >= bs.dodge_until)
	{
		const double radius{obj.size / 65536.0 + 3};
		const auto frame{to_frame(obj.orient)};
		const auto own_objnum{vcplayerptr(bs.pid)->objnum};
		const bool coop = +(Game_mode & GM_MULTI_COOP);
		const bool friendly_fire{!Netgame.NoFriendlyFire};
		for (const object &o : Objects.vcptr)
		{
			if (o.type != object_type::OBJ_WEAPON)
				continue;
			const auto &li{o.ctype.laser_info};
			bool own{false}, from_partner{false};
			if (li.parent_type == object_type::OBJ_PLAYER)
			{
				own = li.parent_num == own_objnum;
				const auto &parent{*Objects.vcptr(li.parent_num)};
				if (!own && parent.type == object_type::OBJ_PLAYER && laser_parent_is_matching_signature(li, parent))
				{
					const auto shooter{get_player_id(parent)};
					from_partner = coop || same_team(bs.pid, shooter);
				}
			}
			if (!b::shot_worth_dodging(own, from_partner, friendly_fire))
				continue;
			const auto rel{to_vec(o.pos) - pos};
			if (b::length(rel) > BOT_DODGE_SCAN)
				continue;
			/* Stage B4: a homing missile after this bot turns with it;
			 * it is judged with a wider pass and dodged more often.
			 */
			const bool homing_at_me{Weapon_info[get_weapon_id(o)].homing_flag && li.track_goal == own_objnum};
			const auto away{b::dodge_direction(rel, to_vec(o.mtype.phys_info.velocity) - vel, BOT_DODGE_HORIZON, b::dodge_radius(radius, homing_at_me), frame.r)};
			if (!away)
				continue;
			if (b::dodge_roll(bs.dodge_salt, static_cast<uint16_t>(o.signature)) >= b::dodge_chance(bs.skill->dodge_prob, homing_at_me))
				continue;
			bs.dodge_dir = *away;
			bs.dodge_from = tick + b::ticks_from_ms(bs.skill->reaction_ms) / 2;
			bs.dodge_until = bs.dodge_from + BOT_DODGE_TICKS;
			break;
		}
	}
	/* The host's copy of the bot's inventory, and the clients' (the
	 * energy and ammunition it spent).
	 */
	net_objects_host_own_ship_inventory(bs.pid, false);
}

/* Section 4.7: the path costs from where the bot is to everything within
 * reach, with the passability and penalties of its paths.
 */
void compute_distances(bot_state &bs, const object &obj)
{
	const auto flags{obj.ctype.player_info.powerup_flags};
	bs.dist.compute(B.graph, obj.segnum, [flags](const uint32_t from, const b::nav_edge &e) {
		return edge_passable(from, e, flags);
	}, [&bs](const uint32_t from, const b::nav_edge &e) {
		return bs.penalties.cost(from, e.side);
	}, BOT_DISTANCE_MAX_COST, BOT_DISTANCE_NODE_LIMIT);
}

/* The live powerup a memory entry names, if it is still there. */
[[nodiscard]]
const object *live_powerup(const b::known_powerup &k)
{
	auto &Objects = LevelUniqueObjectState.Objects;
	if (k.key > Highest_object_index)
		return nullptr;
	const auto &o{*Objects.vcptr(objnum_t{k.key})};
	if (o.type != object_type::OBJ_POWERUP || underlying_value(o.signature) != k.signature || (o.flags & OF_SHOULD_BE_DEAD))
		return nullptr;
	return &o;
}

/* Section 4.7 and decision 4 of section 11: what the bot learns of the
 * powerups at this strategy tick.  It knows the level's initial layout
 * within its skill's map knowledge (path segments from where it is),
 * sees powerups (awareness, field of view, line of sight; a few checks
 * per tick) and hears a new one appear within its hearing radius (a
 * respawn, a death's drop).  A remembered powerup whose place it sees
 * empty, or comes to, is forgotten; so is one remembered too long.
 */
void learn_powerups(bot_state &bs, const object &obj, const uint32_t tick)
{
	auto &Objects = LevelUniqueObjectState.Objects;
	const auto &sk{*bs.skill};
	const auto pos{to_vec(obj.pos)};
	const auto frame{to_frame(obj.orient)};
	unsigned budget{BOT_POWERUP_LOS_BUDGET};
	const auto sees{[&](const vms_vector &where, const vec3 &to) {
		if (b::length(to) > sk.awareness || !b::in_field_of_view(frame.f, to, sk.fov_half_deg) || !budget)
			return false;
		--budget;
		return line_clear(obj, obj.pos, obj.segnum, where, 0, true);
	}};
	/* First the places it remembers: still there? */
	std::array<uint16_t, 128> gone{};
	std::size_t n_gone{0};
	for (const auto &k : bs.powerups.items())
	{
		if (const auto o{live_powerup(k)})
		{
			/* It rolled (a drop settling): the bot follows it. */
			if (o->pos != to_fixvec(k.pos))
			{
				auto u{k};
				u.pos = to_vec(o->pos);
				u.segment = o->segnum;
				u.count = static_cast<uint32_t>(std::max(o->ctype.powerup_info.count, 0));
				bs.powerups.learn(u, k.learned_tick);
			}
			continue;
		}
		const auto to{k.pos - pos};
		if ((b::length(to) < 15 || sees(to_fixvec(k.pos), to)) && n_gone < gone.size())
			gone[n_gone++] = k.key;
	}
	for (std::size_t i = 0; i < n_gone; ++i)
		bs.powerups.forget(gone[i]);
	bs.powerups.expire(tick, b::ticks_from_ms(b::powerup_memory_ms(sk)));
	/* Then the powerups it does not know yet. */
	for (const auto &&o : Objects.vcptridx)
	{
		if (o->type != object_type::OBJ_POWERUP || (o->flags & OF_SHOULD_BE_DEAD))
			continue;
		const uint16_t key = o.get_unchecked_index();
		const uint16_t sig = underlying_value(o->signature);
		if (bs.powerups.knows(key, sig))
			continue;
		if (item_of(get_powerup_id(o)).kind == b::item::none)
			continue;
		const auto netid{net_objects_netid_of(o)};
		const bool initial{netid != 0xffff && ::dcx::net_v2::is_level_netid(netid)};
		const auto ppos{to_vec(o->pos)};
		const auto to{ppos - pos};
		const bool from_map{b::knows_from_map(initial, bs.dist.hops(o->segnum), sk.map_knowledge)};
		/* Only a perceived powerup may push another out of a full
		 * memory (powerup_memory::learn); the map alone need not be
		 * checked further while there is room.
		 */
		bool perceived{false};
		if (!from_map || bs.powerups.full())
		{
			if (!initial)
			{
				const double age{(GameTime64 - o->ctype.powerup_info.creation_time) / 65536.0};
				perceived = b::hears_appearance(age, b::length(to), sk.hearing);
			}
			if (!perceived)
				perceived = sees(o->pos, to);
		}
		if (!from_map && !perceived)
			continue;
		bs.powerups.learn({
			.key = key,
			.signature = sig,
			.type = static_cast<uint8_t>(get_powerup_id(o)),
			.count = static_cast<uint32_t>(std::max(o->ctype.powerup_info.count, 0)),
			.initial = initial,
			.pos = ppos,
			.segment = o->segnum,
		}, tick, perceived);
	}
}

/* A goal place: a powerup or a centre. */
struct goal_place
{
	double utility{};
	double path{};
	uint32_t segment{};
	vec3 pos;
	uint16_t key{0xffff}, sig{};
	bool shields{};
	/* A much stronger armament (b::BIG_UPGRADE_RATIO). */
	bool upgrade{};
};

/* Section 4.7: the best powerup to collect, by value (its need) over the
 * path cost.  Only what the rules let the bot take (as a human's "already
 * have") and what it can reach.
 */
[[nodiscard]]
goal_place best_collect(const bot_state &bs, const b::resource_view &res, const uint32_t tick, const bool shields_only = false)
{
	goal_place best;
	for (const auto &k : bs.powerups.items())
	{
		if (tick < k.ignore_until)
			continue;
		const auto type{static_cast<powerup_type_t>(k.type)};
		const auto desc{item_of(type)};
		if (shields_only && desc.kind != b::item::shield)
			continue;
		const auto cost{bs.dist.cost(k.segment)};
		if (!cost)
			continue;
		const double value{b::item_value(desc, res)};
		if (value <= 0)
			continue;
		if (!net_objects_bot_can_use(bs.pid, type, k.count))
			continue;
		const double path{*cost + b::distance(B.graph.position(k.segment), k.pos)};
		const double u{b::collect_utility(value, path)};
		if (u > best.utility)
			best = {u, path, k.segment, k.pos, k.key, k.signature, desc.kind == b::item::shield, b::upgrade_ratio(desc, res.weapons) >= b::BIG_UPGRADE_RATIO};
	}
	return best;
}

/* Section 4.7: the best fuel centre (energy) or repair centre (shields). */
[[nodiscard]]
goal_place best_centre(const bot_state &bs, const b::resource_view &res, const bool repair_only = false)
{
	goal_place best;
	const auto consider{[&](const std::vector<uint32_t> &centres, const bool repair) {
		const double value{b::fuel_centre_value(repair, res)};
		if (value <= 0)
			return;
		for (const auto seg : centres)
		{
			const auto cost{bs.dist.cost(seg)};
			if (!cost)
				continue;
			const double u{b::collect_utility(value, *cost)};
			if (u > best.utility)
				best = {u, *cost, seg, B.graph.position(seg), 0xffff, 0, repair};
		}
	}};
	if (!repair_only)
		consider(B.fuel_centres, false);
	consider(B.repair_centres, true);
	return best;
}

[[nodiscard]]
bool in_centre(const object &obj, const bool repair)
{
	const shared_segment &seg{*vcsegptr(obj.segnum)};
	return seg.special == (repair ? segment_special::repaircen : segment_special::fuelcen);
}

/* Section 4.7, retreat: a shield source away from the threat (a shield
 * powerup it knows, a repair centre), else the place of a few drawn that
 * is furthest from the threat for the least flying.
 */
[[nodiscard]]
std::optional<goal_place> retreat_place(bot_state &bs, const object &obj, const b::resource_view &res, const vec3 &threat, const uint32_t tick)
{
	const auto pos{to_vec(obj.pos)};
	std::optional<goal_place> best;
	double best_u{0};
	for (const auto &p : {best_collect(bs, res, tick, true), best_centre(bs, res, true)})
	{
		if (p.utility <= 0)
			continue;
		const double u{p.utility * b::retreat_direction_factor(pos, p.pos, threat)};
		if (u > best_u)
		{
			best_u = u;
			best = p;
		}
	}
	if (best && best_u > 0.3)
		return best;
	best.reset();
	double best_flee{-1e9};
	const auto n{static_cast<unsigned>(B.graph.size())};
	for (unsigned i = 0; i < BOT_FLEE_TRIES; ++i)
	{
		const uint32_t seg{bs.rng.below(n)};
		const auto cost{bs.dist.cost(seg)};
		if (!cost || *cost < 80)
			continue;
		const auto &place{B.graph.position(seg)};
		const double f{b::flee_score(place, threat, *cost)};
		if (f > best_flee)
		{
			best_flee = f;
			best = goal_place{0, *cost, seg, place, 0xffff, 0, false};
		}
	}
	return best;
}

/* The goal of the last strategy tick, as the goal choice sees it. */
[[nodiscard]]
std::optional<b::goal_kind> current_goal(const bot_state &bs, const bool target_visible)
{
	switch (bs.goal)
	{
		case bot_goal::none:
			return std::nullopt;
		case bot_goal::roam:
			return b::goal_kind::roam;
		case bot_goal::hunt:
			return target_visible ? b::goal_kind::engage : b::goal_kind::hunt;
		case bot_goal::collect:
			return b::goal_kind::collect;
		case bot_goal::retreat:
			return b::goal_kind::retreat;
		case bot_goal::refuel:
			return b::goal_kind::refuel;
	}
	return std::nullopt;
}

/* Section 4.4: target choice; section 4.7: the goal, at 5 Hz. */
void think(bot_state &bs, object &obj, const uint32_t tick)
{
	auto &Objects = LevelUniqueObjectState.Objects;
	const auto &sk{*bs.skill};
	const auto &st{*bs.style};
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
	double target_score{0};
	bool target_visible{false};
	std::optional<double> target_distance;
	if (bs.target)
	{
		for (unsigned k = 0; k < n; ++k)
			if (cand[k].id == *bs.target)
			{
				target_score = b::target_score(cand[k], sk.awareness);
				target_distance = cand[k].distance;
			}
		target_visible = bs.visible_now[*bs.target];
	}
	/* Section 4.5: the primary for the range to the target; the band
	 * changes only a little past its border.
	 */
	bs.band = target_distance ? std::optional<b::range_band>{b::band_of(*target_distance, bs.band)} : std::nullopt;
	choose_weapon(bs, obj, bs.band);
	/* Section 4.7: what it knows and what it needs. */
	compute_distances(bs, obj);
	learn_powerups(bs, obj, tick);
	const auto res{resources_of(obj)};
	const auto collect{best_collect(bs, res, tick)};
	auto centre{best_centre(bs, res)};
	/* A bot hovering in a centre stays until it is full, unless an enemy
	 * comes into sight while it has enough to fight.
	 */
	const bool refuelling{bs.goal == bot_goal::refuel && bs.points.empty()};
	if (refuelling && (in_centre(obj, false) || in_centre(obj, true)))
	{
		const bool repair{in_centre(obj, true)};
		const double level{repair ? res.shields : res.energy};
		if (b::keep_refuelling(level, target_visible))
			centre = {std::max(centre.utility, 3.0), 0, static_cast<uint32_t>(obj.segnum), pos, 0xffff, 0, repair};
	}
	const bool attacked{bs.last_attacker < MAX_PLAYERS && tick - bs.attacked_tick < BOT_REVENGE_TICKS};
	/* Section 4.7: cloaked it sneaks, invulnerable it attacks. */
	bs.tactics = b::tactics_for(res.cloaked, res.invulnerable);
	const auto goal{b::choose_goal({
		.has_target = bs.target.has_value(),
		.target_visible = target_visible,
		.target_score = target_score,
		.threatened = bs.target.has_value() || attacked,
		.shields = res.shields,
		.invulnerable = res.invulnerable,
		.collect = collect.utility,
		.collect_path = collect.path,
		.collect_upgrade = collect.upgrade,
		.refuel = centre.utility,
		.retreat_shields = st.retreat_shields,
		.engage_weight = st.engage_weight * bs.tactics.engage_weight,
		.collect_weight = st.collect_weight * bs.tactics.collect_weight,
		.collector = bs.cfg.style == b::bot_style::collector,
		.current = current_goal(bs, target_visible),
	})};
	const bool replan_due{bs.points.empty() || tick >= bs.next_plan_tick};
	switch (goal)
	{
		case b::goal_kind::engage:
		case b::goal_kind::hunt:
		{
			/* Hunt: a path to where the target is (or was last seen),
			 * also while the bot fights it in the open.  B1 dropped the
			 * path for a direct engagement, so until the bot reacted to
			 * the target (a reaction time), whenever the line of fire
			 * closed and whenever the target left the field of view for
			 * a moment, the bot had nowhere to go and stopped dead: it
			 * fought on one spot.  Now it chases along the path in those
			 * moments.  A target that moves on is planned for again at
			 * most every half second.
			 */
			const auto &m{bs.memory[*bs.target]};
			const bool moved{bs.goal != bot_goal::hunt || bs.goal_seg != m.segment};
			if (replan_due || (moved && tick - bs.last_plan_tick >= BOT_HUNT_REPLAN_TICKS))
				set_goal(bs, obj, bot_goal::hunt, m.segment, m.pos, tick);
			return;
		}
		case b::goal_kind::collect:
		{
			const bool other{bs.goal != bot_goal::collect || bs.collect_key != collect.key || bs.collect_sig != collect.sig};
			bs.collect_key = collect.key;
			bs.collect_sig = collect.sig;
			if (other || replan_due)
				set_goal(bs, obj, bot_goal::collect, collect.segment, collect.pos, tick);
			return;
		}
		case b::goal_kind::refuel:
			if (obj.segnum == centre.segment)
			{
				/* In the centre: hover (follow_path holds the place). */
				if (bs.goal != bot_goal::refuel || !bs.points.empty())
				{
					bs.goal = bot_goal::refuel;
					bs.goal_seg = centre.segment;
					bs.clear_path();
				}
				bs.refuel_seg = centre.segment;
				return;
			}
			if (bs.goal != bot_goal::refuel || bs.refuel_seg != centre.segment || replan_due)
			{
				bs.refuel_seg = centre.segment;
				set_goal(bs, obj, bot_goal::refuel, centre.segment, centre.pos, tick);
			}
			return;
		case b::goal_kind::retreat:
		{
			/* On its way: keep the place (planned again every 2 s, as
			 * doors and penalties change); a new one only on arrival.
			 */
			if (bs.goal == bot_goal::retreat && !bs.points.empty())
			{
				if (tick >= bs.next_plan_tick)
					plan_path(bs, obj, bs.goal_seg, std::nullopt, tick);
				return;
			}
			vec3 threat{pos};
			if (bs.target)
				threat = bs.memory[*bs.target].pos;
			else if (attacked && bs.memory[bs.last_attacker].valid)
				threat = bs.memory[bs.last_attacker].pos;
			if (const auto place{retreat_place(bs, obj, res, threat, tick)})
			{
				set_goal(bs, obj, bot_goal::retreat, place->segment, place->pos, tick);
				return;
			}
			break;
		}
		case b::goal_kind::roam:
			break;
	}
	if (bs.goal != bot_goal::roam)
		bs.goal = bot_goal::none;
	if (bs.goal == bot_goal::none || bs.points.empty())
		choose_roam_goal(bs, obj, tick);
}

/* Section 4.3: fly along the path; returns the wanted velocity. */
vec3 follow_path(bot_state &bs, object &obj, const bool engaged)
{
	const auto pos{to_vec(obj.pos)};
	const auto frame{to_frame(obj.orient)};
	const double max_speed{B.limits.max_speed};
	if (bs.points.empty())
	{
		if (!engaged)
		{
			bs.face_dir = frame.f;
			bs.face_rate = {};
		}
		/* Refuelling (section 4.7): hover in the centre. */
		if (bs.goal == bot_goal::refuel && bs.goal_seg < B.graph.size())
		{
			const auto hold{(B.graph.position(bs.goal_seg) - pos) * 0.5};
			const double l{b::length(hold)};
			const double cap{max_speed * 0.3};
			return l > cap ? hold * (cap / l) : hold;
		}
		return {};
	}
	const double reach{std::max(obj.size / 65536.0 * 1.5, 5.0)};
	/* A powerup is taken by touching it: the bot flies through its
	 * centre rather than stopping next to it.
	 */
	const bool collecting{bs.goal == bot_goal::collect};
	bs.point_index = b::advance_along(bs.points, bs.point_index, pos, reach);
	if (bs.steer_index < bs.point_index)
		bs.steer_index = bs.point_index;
	const auto &target{bs.points[bs.steer_index]};
	const auto to{target - pos};
	const double dist{b::length(to)};
	const bool last{bs.steer_index + 1 == bs.points.size()};
	double speed{max_speed};
	if (last)
		speed = std::min(speed, collecting ? dist * 3 + 8 : dist * 1.5);
	else
	{
		/* Slow down before a sharp turn. */
		const auto turn{b::angle_between(to, bs.points[bs.steer_index + 1] - target)};
		if (turn > b::radians(60) && dist < 40)
			speed = std::min(speed, std::max(max_speed * 0.4, dist * 1.5));
	}
	if (!engaged)
	{
		bs.face_dir = b::normalized(to);
		bs.face_rate = {};
	}
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
		case b::stuck_event::give_up:
			/* A powerup it cannot reach is no goal for a while. */
			if (collecting)
				bs.powerups.ignore_for(bs.collect_key, B.tick.tick() + BOT_COLLECT_UNREACHABLE_TICKS);
			bs.goal = bot_goal::none;
			bs.clear_path();
			return {};
	}
	if (last && dist < (collecting ? 2.0 : reach))
	{
		/* Arrived.  A hunt that finds nobody forgets the target's old
		 * position; a roam picks a new place at the next strategy tick.
		 * At a powerup that is still there (it could not be taken) the
		 * bot does not come back for a while; a refuelling bot hovers.
		 */
		if (bs.goal == bot_goal::hunt && bs.target)
			bs.memory[*bs.target] = {};
		if (collecting)
			bs.powerups.ignore_for(bs.collect_key, B.tick.tick() + BOT_COLLECT_IGNORE_TICKS);
		if (bs.goal != bot_goal::refuel)
			bs.goal = bot_goal::none;
		bs.clear_path();
		return {};
	}
	return b::normalized(to) * speed;
}

/* Section 4.7: the afterburner, by the skill's rule (bot_goals.h): when
 * chasing a target far away, retreating, dodging, or on a long straight
 * flight, with enough charge, and only while the bot wants to go where
 * its nose points (the afterburner is full forward thrust).  The others
 * hear it as they hear a human's (MULTI_SOUND_FUNCTION as the bot).
 */
void decide_afterburner(bot_state &bs, object &obj, const vec3 &wanted, const uint32_t tick)
{
#if DXX_BUILD_DESCENT == 2
	const auto &pi{obj.ctype.player_info};
	const auto frame{to_frame(obj.orient)};
	const auto pos{to_vec(obj.pos)};
	const double max_speed{B.limits.max_speed};
	bool chasing_far{false};
	if (bs.target && (bs.goal == bot_goal::hunt) && bs.memory[*bs.target].valid)
		chasing_far = b::distance(pos, bs.memory[*bs.target].pos) > b::AFTERBURNER_CHASE_DISTANCE;
	bool long_straight{false};
	if ((bs.goal == bot_goal::roam || bs.goal == bot_goal::collect) && bs.steer_index < bs.points.size())
		long_straight = b::distance(pos, bs.points[bs.steer_index]) > 100;
	const bool dodging{tick >= bs.dodge_from && tick < bs.dodge_until};
	const bool aligned{b::length(wanted) > max_speed * 0.5 && b::angle_between(frame.f, wanted) < b::radians(25)};
	const bool burn{!bs.stuck.recovering() && b::want_afterburner({
		.have = has_flag(pi, player_flag::afterburner),
		.charge = bs.pl.afterburner_charge / 65536.0,
		.use = b::afterburner_of(bs.skill_level),
		.chasing_far = chasing_far,
		.retreating = bs.goal == bot_goal::retreat,
		.dodging = dodging,
		.long_straight = long_straight,
		.aligned = aligned,
		.burning = bs.burning,
	})};
	if (burn == bs.burning)
		return;
	bs.burning = burn;
	auto &Objects = LevelUniqueObjectState.Objects;
	const auto &&objp{Objects.vcptridx(&obj)};
	if (burn)
	{
		digi_link_sound_to_object3(sound_effect::SOUND_AFTERBURNER_IGNITE, objp, 1, F1_0, sound_stack::allow_stacking, vm_distance{i2f(256)}, 20098, 25776);
		multi_send_sound_function(3, sound_effect::SOUND_AFTERBURNER_IGNITE, bs.pid);
	}
	else
	{
		digi_kill_sound_linked_to_object(objp);
		multi_send_sound_function(0, 0, bs.pid);
	}
#else
	(void)bs;
	(void)obj;
	(void)wanted;
	(void)tick;
#endif
}

/* The afterburner goes out (death, level end). */
void stop_afterburner(bot_state &bs, const object &obj)
{
	if (!bs.burning)
		return;
	bs.burning = false;
#if DXX_BUILD_DESCENT == 2
	auto &Objects = LevelUniqueObjectState.Objects;
	digi_kill_sound_linked_to_object(Objects.vcptridx(&obj));
	multi_send_sound_function(0, 0, bs.pid);
#else
	(void)obj;
#endif
}

/* The distance to the wall a shot fired from the ship along `dir` meets,
 * up to `limit`: where a missile that misses would burst.  (Objects are
 * left out: the bot's own shots in flight would stop the line; the
 * target is taken into account by the caller.)
 */
[[nodiscard]]
double wall_distance(const object &obj, const vec3 &dir, const double limit)
{
	auto &Objects = LevelUniqueObjectState.Objects;
	const auto pos{to_vec(obj.pos)};
	fvi_info hit;
	const auto type{find_vector_intersection(fvi_query{
		obj.pos,
		to_fixvec(pos + dir * limit),
		fvi_query::unused_ignore_obj_list,
		fvi_query::unused_LevelUniqueObjectState,
		fvi_query::unused_Robot_info,
		0,
		Objects.vcptridx(&obj),
	}, obj.segnum, F1_0, hit)};
	if (type == fvi_hit_type::None)
		return limit;
	return b::distance(pos, to_vec(hit.hit_pnt));
}

/* Stage B4 (section 9.4): missiles and mines.  At each tick the bot
 * either waits for the release of the missile it chose (the aim within
 * its cone, the blast along the nose far enough away) or chooses one
 * (b::choose_secondary); a mine is dropped at once.  bots_fire fires it
 * through do_missile_firing as the bot, as a human's.
 */
void missile_tick(bot_state &bs, object &obj, const uint32_t tick, const percept *const p)
{
	const auto &sk{*bs.skill};
	auto &pi{obj.ctype.player_info};
	if (bs.missile_fire)
		return;
	if (bs.missile && tick >= bs.missile_until)
		bs.missile.reset();
	const auto seconds_since{[tick](const std::optional<uint32_t> &t) {
		return t ? (tick - *t) / static_cast<double>(b::BOT_TICK_RATE) : 1e9;
	}};
	const auto pos{to_vec(obj.pos)};
	const auto frame{to_frame(obj.orient)};
	const auto vel{to_vec(obj.mtype.phys_info.velocity)};
	const auto res_cloaked{has_flag(pi, player_flag::cloaked)};
	const bool invulnerable{has_flag(pi, player_flag::invulnerable)};
	/* The target, as the tactics layer sees it (a reaction time late),
	 * or as remembered.
	 */
	const bool target_visible{p && p->visible && bs.target && *bs.target == p->target && bs.visible_now[p->target]};
	std::optional<vec3> target_pos;
	if (target_visible)
		target_pos = p->pos;
	else if (bs.target && bs.memory[*bs.target].valid)
		target_pos = bs.memory[*bs.target].pos;
	if (!bs.missile)
	{
		b::missile_situation m;
		for (unsigned i = 0; i < b::BOT_SECONDARY_COUNT && i < MAX_SECONDARY_WEAPONS; ++i)
		{
			const auto w{static_cast<secondary_weapon_index>(i)};
			m.ammo[i] = pi.secondary_ammo[w];
			if (m.ammo[i])
				m.data[i] = missile_data_of(w);
		}
		m.smarts = sk.weapon_smarts;
		m.has_target = target_pos.has_value();
		m.target_visible = target_visible;
		m.shot_clear = target_visible && bs.shot_clear;
		m.invulnerable = invulnerable;
		m.cloaked = res_cloaked;
		m.since_missile = seconds_since(bs.last_missile);
		m.since_heavy = seconds_since(bs.last_heavy);
		m.since_mine = seconds_since(bs.last_mine);
		if (bs.target)
		{
			const auto t{*bs.target};
			const auto &mem{bs.memory[t]};
			m.target_seen_ago = mem.valid ? (tick - mem.tick) / static_cast<double>(b::BOT_TICK_RATE) : 1e9;
			m.heavy_used_on_target = bs.heavy_target == t && m.since_heavy < b::HEAVY_PER_TARGET;
			if (target_pos)
			{
				const auto to{*target_pos - pos};
				m.target_distance = b::length(to);
				const auto los{b::normalized(to)};
				const auto rel_vel{mem.vel - vel};
				m.target_lateral_speed = b::length(rel_vel - los * b::dot(rel_vel, los));
				const auto &ship{ship_of(t)};
				m.target_facing = b::dot(to_vec(ship.orient.fvec), -los) > std::cos(b::radians(30));
			}
			/* Section 4.7: flying away from it (retreat, collect,
			 * refuel) with it close behind.
			 */
			const bool flying_away{bs.goal == bot_goal::retreat || bs.goal == bot_goal::collect || bs.goal == bot_goal::refuel};
			if (flying_away && mem.valid && m.target_seen_ago <= 1)
			{
				const auto to{mem.pos - pos};
				const double speed{b::length(vel)};
				if (speed > 15 && b::dot(b::normalized(vel), b::normalized(to)) < -0.3)
				{
					m.chased = true;
					m.pursuer_distance = b::length(to);
				}
			}
		}
		/* A doorway: the next path point is the centre of a side. */
		m.at_doorway = bs.point_index < bs.points.size() && !(bs.point_index & 1) && b::distance(pos, bs.points[bs.point_index]) < 12;
		const auto chosen{b::choose_secondary(m)};
		if (!chosen)
			return;
		bs.missile = chosen;
		bs.missile_until = tick + static_cast<uint32_t>(b::MISSILE_PENDING_SECONDS * b::BOT_TICK_RATE);
	}
	const auto s{*bs.missile};
	const auto w{game_secondary(s)};
	if (!pi.secondary_ammo[w])
	{
		bs.missile.reset();
		return;
	}
	const auto role{b::role_of(s)};
	if (role != b::missile_role::mine)
	{
		/* The target: in sight, or a smart missile's seen a moment ago. */
		if (!target_pos || (!target_visible && role != b::missile_role::smart))
			return;
		if (target_visible && !bs.shot_clear && role != b::missile_role::smart)
			return;
	}
	double err{0}, impact{1e9};
	const auto md{missile_data_of(w)};
	if (role != b::missile_role::mine)
	{
		err = b::angle_between(frame.f, target_visible ? bs.face_dir : *target_pos - pos);
		/* Where it bursts: the wall along the nose, or the target on
		 * the way.
		 */
		impact = wall_distance(obj, frame.f, 400);
		if (target_visible)
			impact = std::min(impact, b::distance(pos, *target_pos));
	}
	if (!b::missile_release(s, err, b::radians(sk.fire_cone_deg), impact, md.blast_radius, invulnerable))
		return;
	bs.missile.reset();
	bs.missile_fire = s;
	bs.missile_volley = std::max<unsigned>(1, Weapon_info[Secondary_weapon_to_weapon_info[w]].fire_count);
	if (role == b::missile_role::mine)
		bs.last_mine = tick;
	else
	{
		bs.last_missile = tick;
		if (role == b::missile_role::heavy || role == b::missile_role::shaker)
		{
			bs.last_heavy = tick;
			bs.heavy_target = bs.target ? *bs.target : 0xff;
		}
	}
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
	bs.lead.update(bs.rng, sk.lead, b::ticks_from_ms(sk.aim_drift_ms));
	const double range_scale{bs.style->range_scale * bs.tactics.range_scale};
	bs.juke.update(bs.rng, b::ticks_from_ms(sk.strafe_min_ms), b::ticks_from_ms(sk.strafe_max_ms), BOT_RANGE_LO * range_scale, BOT_RANGE_HI * range_scale);
	const auto pos{to_vec(obj.pos)};
	const auto vel{to_vec(obj.mtype.phys_info.velocity)};
	const auto frame{to_frame(obj.orient)};
	const double max_speed{B.limits.max_speed};
	const unsigned reaction_ticks{b::ticks_from_ms(sk.reaction_ms)};
	const auto *const p{bs.seen.delayed(reaction_ticks / b::PERCEPTION_DIVISOR)};
	bs.fire = false;
	/* Section 4.3: the stuck recovery runs out on time, whatever moves
	 * the bot meanwhile; then the path is planned again.
	 */
	if (bs.stuck.tick_recovery() && bs.goal != bot_goal::none)
		plan_path(bs, obj, bs.goal_seg, std::nullopt, tick);
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
		/* Section 4.4: the lead from the gun the shots leave from, with
		 * the current weapon's speed and the bot's lead factor.
		 */
		const auto primary_now{pi.Primary_weapon.get_active()};
		/* Stage B4: a missile that flies straight is aimed with its
		 * own speed from its own gun while it waits for its release.
		 */
		const bool aim_missile{bs.missile && b::missile_aimed(*bs.missile)};
		const auto gun{aim_missile ? missile_gun_local(game_secondary(*bs.missile)) : gun_local(primary_now, has_flag(pi, player_flag::quad_lasers))};
		double shot_speed{weapon_speed(pi)};
		if (aim_missile)
		{
			const auto md{missile_data_of(game_secondary(*bs.missile))};
			shot_speed = b::effective_shot_speed(md.speed, md.thrust);
		}
		const auto shooter{pos + frame.to_world(gun)};
		const auto aim{b::aim_point(shooter, est, p->vel, shot_speed, bs.lead.factor())};
		bs.face_dir = b::apply_aim_offset(aim - shooter, frame.u, bs.aim.yaw(), bs.aim.pitch());
		/* The steering's feed-forward: how fast the line to the target
		 * turns, as the bot reckons it.
		 */
		bs.face_rate = b::line_of_sight_rate(aim - pos, p->vel - vel);
		const auto to{est - pos};
		const double dist{b::length(to)};
		/* Collecting, retreating or refuelling (section 4.7), the bot
		 * shoots at what it sees but flies its path: backward when it
		 * retreats facing its pursuer.
		 */
		const bool path_goal{bs.goal == bot_goal::collect || bs.goal == bot_goal::retreat || bs.goal == bot_goal::refuel};
		if (bs.shot_clear && !path_goal)
		{
			/* Section 4.6: close in and back off inside the band, strafe
			 * across the line of sight in changing directions.
			 */
			const double strafe_speed{sk.strafe ? max_speed * 0.7 : 0};
			/* Combat movement takes over: no recovery manoeuvre. */
			bs.stuck.cancel_recovery();
			wanted = b::combat_velocity(to, frame.r, frame.u, bs.juke, sk.strafe_vertical, max_speed * 0.8, strafe_speed);
		}
		else
			wanted = follow_path(bs, obj, true);
		const double err{b::angle_between(frame.f, bs.face_dir)};
		/* Section 4.5: beyond the mid band, no shot that hits less than
		 * one time in ten.
		 */
		const auto rel_vel{p->vel - vel};
		const auto los{b::normalized(to)};
		const double lateral{b::length(rel_vel - los * b::dot(rel_vel, los))};
		const auto &target_obj{ship_of(p->target)};
		const double cone{b::fire_cone_with_spread(b::radians(sk.fire_cone_deg), spread_half_angle(primary_now))};
		bs.fire = b::should_fire(err, cone, bs.shot_clear, dist, std::min(weapon_range(pi), bs.tactics.max_fire_distance)) &&
			b::long_shot_worthwhile(dist, weapon_speed(pi), lateral, b::radians(sk.aim_sigma_deg), target_obj.size / 65536.0);
	}
	else
		wanted = follow_path(bs, obj, false);
	if (bs.stuck.recovering())
		wanted = bs.recover_dir * max_speed;
	else if (tick >= bs.dodge_from && tick < bs.dodge_until)
		wanted += bs.dodge_dir * max_speed;
	if (tick < bs.avoid_until)
		wanted += bs.avoid;
	bs.move_cmd = b::velocity_command(wanted, vel, max_speed);
	decide_afterburner(bs, obj, wanted, tick);
	missile_tick(bs, obj, tick, p);
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
	 * (do_physics_sim_rot); the thrust to the orientation with it
	 * (apply_pilot_controls).
	 */
	vms_matrix unrolled{obj.orient};
	if (const fixang roll{obj.mtype.phys_info.turnroll})
		unrolled = vm_matrix_x_matrix(obj.orient, vm_angles_2_matrix(vms_angvec{.p = 0, .b = static_cast<fixang>(-roll), .h = 0}));
	constexpr double rev_to_rad{2 * std::numbers::pi / 65536.0};
	const auto &rotvel{obj.mtype.phys_info.rotvel};
	const auto out{b::steer_controls({
		.unrolled = to_frame(unrolled),
		.thrust_frame = to_frame(obj.orient),
		.face_dir = bs.face_dir,
		.face_rate = bs.face_rate,
		.move_cmd = bs.move_cmd,
		.pitch_rate = rotvel.x * rev_to_rad,
		.heading_rate = rotvel.y * rev_to_rad,
	}, B.limits.turn, bs.skill->turn_cap, bs.heading_pref)};
	c = {out.pitch, out.heading, out.forward, out.sideways, out.vertical};
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
	choose_weapon(bs, obj, std::nullopt);
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
	stop_afterburner(bs, obj);
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

/* Section 4.7: a fuel centre gives energy, a repair centre shields, to
 * a ship in it, as object_move_one gives the local player's (up to 100,
 * Fuelcen_give_amount per second); the sound is heard on the host.
 */
void refuel_frame(bot_state &bs, object &obj)
{
	const shared_segment &seg{*vcsegptr(obj.segnum)};
	fix *level{nullptr};
	if (seg.special == segment_special::fuelcen)
		level = &obj.ctype.player_info.energy;
	else if (seg.special == segment_special::repaircen)
		level = &obj.shields;
	else
		return;
	const fix full{seg.special == segment_special::fuelcen ? INITIAL_ENERGY : INITIAL_SHIELDS};
	if (*level >= full)
		return;
	*level += std::min(fixmul(FrameTime, BOT_FUELCEN_RATE), full - *level);
	if (GameTime64 >= bs.fuel_sound_at || bs.fuel_sound_at > GameTime64 + F1_0)
	{
		bs.fuel_sound_at = GameTime64 + BOT_FUELCEN_SOUND_DELAY;
		auto &Objects = LevelUniqueObjectState.Objects;
		digi_link_sound_to_object(sound_effect::SOUND_REFUEL_STATION_GIVING_FUEL, Objects.vcptridx(&obj), 0, F1_0 / 2, sound_stack::allow_stacking);
	}
}

/* Section 4.7: the energy to shield converter, as the human's key
 * works it (transfer_energy_to_shield: only the energy above 100, 20 a
 * second, two for one), without its HUD text and local sound.
 */
void convert_frame(const bot_state &bs, object &obj)
{
#if DXX_BUILD_DESCENT == 2
	auto &pi{obj.ctype.player_info};
	if (!has_flag(pi, player_flag::converter) || !b::want_convert(obj.shields / 65536.0, pi.energy / 65536.0, bs.skill->weapon_smarts))
		return;
	constexpr fix converter_rate{i2f(20)};
	constexpr fix converter_scale{2};
	const fix e{std::min({fixmul(FrameTime, converter_rate), pi.energy - INITIAL_ENERGY, (MAX_SHIELDS - obj.shields) * converter_scale})};
	if (e <= 0)
		return;
	pi.energy -= e;
	obj.shields += e / converter_scale;
#else
	(void)bs;
	(void)obj;
#endif
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
			/* do_invulnerable_stuff, for the bot: an invulnerability
			 * powerup that runs out is replaced in the level, as the
			 * human's is.
			 */
			auto &pi{obj.ctype.player_info};
			if (+(pi.powerup_flags & player_flag::invulnerable) && GameTime64 > pi.invulnerable_time + INVULNERABLE_TIME_MAX)
			{
				pi.powerup_flags &= ~player_flag::invulnerable;
				if (pi.FakingInvul)
					pi.FakingInvul = 0;
				else
					maybe_drop_net_powerup(powerup_type_t::POW_INVULNERABILITY, 1, 0);
			}
			refuel_frame(bs, obj);
			convert_frame(bs, obj);
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

/* A bot leaving (kicked, or its slot released) puts its afterburner out
 * first: the loop is linked to its ship object, which stays behind as a
 * ghost, so on the host and the clients it would otherwise play on
 * until the level ends, into the tenure of whoever takes the slot next.
 */
void put_out_afterburner(bot_state &bs)
{
	if (bs.burning && vcplayerptr(bs.pid)->objnum != object_none)
		stop_afterburner(bs, ship_of(bs.pid));
	bs.burning = false;
}

}

bool bot_is_local(const playernum_t pnum)
{
	return find_bot(pnum) != nullptr;
}

void bot_slot_released(const playernum_t pnum)
{
	if (pnum < MAX_PLAYERS && B.bots[pnum])
	{
		con_printf(CON_VERBOSE, "bots: P#%u is no longer a bot", pnum);
		put_out_afterburner(*B.bots[pnum]);
		B.bots[pnum].reset();
	}
}

bool bots_kick(const playernum_t pnum)
{
	const auto bs{find_bot(pnum)};
	if (!bs || !multi_i_am_master())
		return false;
	put_out_afterburner(*bs);
	auto &obj{ship_of(pnum)};
	if (obj.type == object_type::OBJ_PLAYER)
	{
		/* Killed and still tumbling: it explodes now (the deres, its
		 * eggs), as it would have.  Otherwise the host's copy of its
		 * inventory is brought up to date, and multi_disconnect_player
		 * drops the eggs from it.
		 */
		if (bs->life == bot_life::dying && Network_status == network_state::playing)
			explode(*bs, obj, LevelSharedRobotInfoState.Robot_info);
		else
			net_objects_host_own_ship_inventory(pnum, true);
	}
	net_v2::host_remove_player(pnum, kick_player_reason::kicked);
	B.bots[pnum].reset();
	return true;
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
		/* Section 2.3: the lowest free slot below the player limit.  A
		 * slot whose connection still exists (a lobby player left out of
		 * the game, whose kick lingers) is not free: the end of that
		 * connection would disconnect the slot.
		 */
		per_player_array<b::slot_view> views{};
		for (auto &&[s, v] : enumerate(views))
		{
			const auto pn{static_cast<playernum_t>(s)};
			v.occupied = B.bots[pn].has_value() || vcplayerptr(pn)->connected != player_connection_status::disconnected;
			v.has_peer = net_v2::host_slot_has_peer(pn);
			v.reserved = pn < N_players && Netgame.players[pn].callsign[0u];
		}
		const auto chosen{b::choose_bot_slot(views, limit)};
		if (!chosen)
			break;
		const playernum_t slot{static_cast<playernum_t>(*chosen)};
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
		bs.powerups.clear();
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
		if (plr.objnum != object_none)
			stop_afterburner(*o, ship_of(o->pid));
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
		const auto connected{vcplayerptr(bs.pid)->connected};
		if (connected != player_connection_status::playing)
		{
			bs.ctl = {};
			bs.fire = false;
			/* Killed during the countdown (D2 marks it died in the mine
			 * with the kill): the death sequence still runs to the
			 * explosion, as the human's does; it does not respawn.
			 */
			if (connected == player_connection_status::died_in_mine && bs.life != bot_life::alive)
				life_frame(bs, ship_of(bs.pid), Robot_info);
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
		if (!o || o->life != bot_life::alive)
			continue;
		auto &bs{*o};
		const auto &&objp{Objects.vmptridx(vcplayerptr(bs.pid)->objnum)};
		if (objp->type != object_type::OBJ_PLAYER)
			continue;
#if DXX_BUILD_DESCENT == 2
		/* The afterburner's trail, after the move, as the human's
		 * (object_move_one).
		 */
		if (bs.pl.drop_afterburner_blob_flag)
		{
			bs.pl.drop_afterburner_blob_flag = 0;
			drop_afterburner_blobs(*objp, 2, i2f(5) / 2, -1);
			multi_send_drop_blobs(bs.pid);
		}
#endif
		/* Stage B4: the missile or mine the brain released, as the
		 * human fires one (do_missile_firing: MULTI_FIRE as the bot);
		 * a volley's further rounds follow frame by frame, as the
		 * human's Global_missile_firing_count does.
		 */
		if (bs.missile_fire)
		{
			const auto w{game_secondary(*bs.missile_fire)};
			auto &pi{objp->ctype.player_info};
			const bool volley_on{bs.missile_volley < std::max<unsigned>(1, Weapon_info[Secondary_weapon_to_weapon_info[w]].fire_count)};
			if (!pi.secondary_ammo[w])
				bs.missile_fire.reset();
			else if (volley_on || allowed_to_fire_missile(pi))
			{
				do_missile_firing(bs.pl, w, objp);
				if (!bs.missile_volley || !--bs.missile_volley)
					bs.missile_fire.reset();
			}
		}
		if (!bs.fire)
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
	/* An axis held for the whole frame is FrameTime (the human's keys);
	 * rounded, not truncated toward minus infinity as fixmul would, so
	 * that at 500 fps (FrameTime 131) a small axis is not biased.
	 */
	const auto held{[](const double axis) {
		return b::held_axis_time(axis, FrameTime);
	}};
	c.pitch_time = held(bc.pitch);
	c.heading_time = held(bc.heading);
	c.bank_time = 0;
	c.forward_thrust_time = held(bc.forward);
	c.sideways_thrust_time = held(bc.sideways);
	c.vertical_thrust_time = held(bc.vertical);
#if DXX_BUILD_DESCENT == 2
	c.state.afterburner = bc.forward > 0 && bs->burning && !Endlevel_sequence;
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

bool bot_touch_powerup(object &ship, const vmobjptridx_t powerup)
{
	if (ship.type != object_type::OBJ_PLAYER)
		return false;
	const auto pid{get_player_id(ship)};
	const auto bs{find_bot(pid)};
	if (!bs || !bots_running())
		return false;
	if (bs->life != bot_life::alive || bs->death_pending || Endlevel_sequence || Network_status != network_state::playing)
		return true;
	auto &pi{ship.ctype.player_info};
	const auto id{get_powerup_id(powerup)};
	switch (id)
	{
		/* Keys stay in a multiplayer level, and every player may take
		 * one (do_powerup): the bot can then open those doors.
		 */
		case powerup_type_t::POW_KEY_BLUE:
			pi.powerup_flags |= player_flag::blue_key;
			return true;
		case powerup_type_t::POW_KEY_RED:
			pi.powerup_flags |= player_flag::red_key;
			return true;
		case powerup_type_t::POW_KEY_GOLD:
			pi.powerup_flags |= player_flag::gold_key;
			return true;
		default:
			break;
	}
	/* Stage B3 (section 4.7): the host's decision, as for every player's
	 * pickup (net_objects.cpp), granted at once: the host is the
	 * authority and the bot is its own ship.
	 */
	if (!net_objects_bot_touch(pid, powerup))
		return true;
	/* What do_powerup does besides the inventory. */
	switch (id)
	{
		case powerup_type_t::POW_CLOAK:
			pi.cloak_time = GameTime64;
			multi_send_cloak(pid);
			break;
		case powerup_type_t::POW_INVULNERABILITY:
			pi.FakingInvul = 0;
			pi.invulnerable_time = GameTime64;
			break;
#if DXX_BUILD_DESCENT == 2
		case powerup_type_t::POW_AFTERBURNER:
			bs->pl.afterburner_charge = F1_0;
			break;
#endif
		default:
			break;
	}
	/* Only now do the clients see the new inventory (the grant only
	 * names the powerup): after MULTI_CLOAK, with the real
	 * invulnerability, and the host's copy taken from the ship as it is.
	 */
	net_objects_host_own_ship_inventory(pid, true);
	/* The powerup is no goal any more; the weapon choice sees what it got
	 * at the next strategy tick.
	 */
	bs->powerups.forget(powerup.get_unchecked_index());
	con_printf(CON_VERBOSE, "bots: '%s' takes powerup %u", static_cast<const char *>(bs->cfg.name), underlying_value(id));
	return true;
}

void bot_cloak_expired(const playernum_t pnum)
{
	if (!find_bot(pnum) || !bots_running())
		return;
	/* As the human's game does for its own cloak: the others see it, and
	 * the cloak comes back into the level.
	 */
	maybe_drop_net_powerup(powerup_type_t::POW_CLOAK, 1, 0);
	multi_send_decloak(pnum);
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
