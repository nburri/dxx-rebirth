/*
 * This file is part of the DXX-Rebirth project <https://github.com/dxx-rebirth/dxx-rebirth/>.
 * It is copyright by its individual contributors, as recorded in the
 * project's Git history.  See COPYING.txt at the top level for license
 * terms and a link to the Git history.
 */
/*
 * Multiplayer network protocol v2, stage 4: firing, hits, damage, kills
 * and respawn (Documentation/network-protocol-v2.md, sections 6.5 and 6.6
 * and "Stage 4 as implemented").
 *
 * - FIRE: the shooter's machine (a client for its player, the host for
 *   its own player and its bots) numbers the projectiles of a shot and
 *   seeds their random values; every other machine creates the shot at
 *   the shooter's origin with the same seed and advances it by the time
 *   it took to arrive (catch-up, with the weapon's own physics).
 * - WEAPON_HIT: only the shooter's machine reports what its projectiles
 *   hit (a direct hit or a blast), with the host time at which the target
 *   was shown there.  The host checks the report against its history of
 *   the target's positions (rewind) and applies the damage.  Damage
 *   without a player's weapon (walls, lava, bumps, robots) is reported by
 *   the machine that flies the ship (`self`).
 * - DAMAGE: the host's damage, to everyone; the victim takes it off its
 *   shields and counts it like a grant (the INVENTORY `seq`).
 * - PLAYER_KILLED: the host's kill and the scores after it.
 * - PLAYER_SPAWN: a new ship (replaces MULTI_REAPPEAR).
 *
 * The rules and wire layouts are game-independent and tested on their
 * own: common/main/net_v2_combat.h, common/unittest/net_v2_combat.cpp.
 */

#include "dxxsconf.h"

#if DXX_USE_MULTIPLAYER

#include <algorithm>
#include <array>
#include <cstdio>
#include <optional>
#include <span>

#include "net_v2_combat.h"
#include "net_v2_session.h"
#include "net_v2_game.h"
#include "net_interp.h"
#include "multi.h"
#include "multibot.h"
#include "bot.h"
#include "object.h"
#include "player.h"
#include "weapon.h"
#include "laser.h"
#include "collide.h"
#include "physics.h"
#include "gameseg.h"
#include "segment.h"
#include "game.h"
#include "newdemo.h"
#include "timer.h"
#include "console.h"
#include "d_levelstate.h"
#include "endlevel.h"
#include "movement_record.h"
#include "movement_record_format.h"

namespace dsx {

namespace {

namespace nv = ::dcx::net_v2;
using nv::netid_t;
using nv::NETID_NONE;
using nv::net_clock;
using nv::session_msg;

/* A client's own damage (walls, lava, bumps) is summed and reported at
 * most this often.
 */
constexpr net_clock SELF_DAMAGE_INTERVAL{nv::net_milliseconds(50)};
/* The ids reserved for the mines of one death (at most three of each
 * kind are armed).
 */
constexpr unsigned MINE_IDS_PER_DEATH{8};
/* No single report of own damage is taken for more than this. */
constexpr fix SELF_DAMAGE_MAX{F1_0 * 400};
/* A bump is credited to the other ship only if it is this close. */
constexpr fix BUMP_RANGE{F1_0 * 40};
/* Where the host has the projectile relative to the target: the target's
 * size, the tolerance of the rules, and this for the projectile's own
 * size.
 */
constexpr std::int64_t PROJECTILE_SLACK{3 * nv::NET_V2_UNIT};
/* The host keeps a position of its own ship and its bots at most this
 * often: the history's ring must span the rewind window at any frame
 * rate (64 entries of 5 ms: 320 ms).
 */
constexpr net_clock HOST_HISTORY_STEP{nv::net_milliseconds(5)};
/* The own damage of different causes is summed apart. */
constexpr std::size_t SELF_DAMAGE_SLOTS{4};
#if DXX_BUILD_DESCENT == 2
/* laser.cpp: MAX_OMEGA_DIST, OMEGA_DAMAGE_SCALE * OMEGA_BASE_TIME. */
constexpr std::int64_t OMEGA_REACH{80 * nv::NET_V2_UNIT};
constexpr fix OMEGA_DAMAGE_FACTOR{32 * (F1_0 / 20)};
#endif

struct weapon_netid
{
	netid_t netid{NETID_NONE};
	uint16_t signature{};
};

/* The weapons of one shot being created: they take consecutive ids. */
struct fire_scope
{
	bool open{};
	bool mines{};
	uint8_t pid{};
	netid_t first{NETID_NONE};
	unsigned limit{};
	unsigned used{};
	uint16_t seed{};
	unsigned n_created{};
	std::array<objnum_t, nv::NET_V2_FIRE_MAX_PROJECTILES> created{};
};

struct host_player
{
	nv::position_history<> history;
	bool alive{true};
	net_clock death_time{};
	nv::fire_limiter primary;
	nv::fire_limiter secondary;
	nv::fire_limiter flare;
	nv::token_bucket bucket{nv::NET_V2_COMBAT_RATE, nv::NET_V2_COMBAT_BURST};
	/* The time of the newest entry the host pushed for its own ship or
	 * a bot (net_combat_frame).
	 */
	net_clock last_push{};
	bool pushed{};
};

struct self_damage
{
	nv::damage_accumulator pending;
	nv::attacker_kind cause{nv::attacker_kind::none};
	uint8_t target{};
	bool have{};
};

struct combat_state
{
	std::array<weapon_netid, MAX_OBJECTS> ids{};
	std::array<uint16_t, nv::NETID_CREATORS> counter{};
	fire_scope scope;
	std::array<netid_t, MAX_PLAYERS> expected_mines{};
	std::array<host_player, MAX_PLAYERS> host{};
	nv::shot_registry registry;
	std::array<self_damage, SELF_DAMAGE_SLOTS> self{};
	std::array<unsigned, nv::NET_V2_HIT_VERDICTS> hit_verdicts{};
	std::array<unsigned, nv::NET_V2_FIRE_VERDICTS> fire_verdicts{};
};

combat_state C;

template <typename M>
void send_msg(const session_msg type, const M &m, const playernum_t exclude = MAX_PLAYERS)
{
	std::array<uint8_t, M::SIZE> buf;
	m.write(buf);
	::dsx::net_v2::game_broadcast(static_cast<uint8_t>(type), buf, exclude);
}

[[nodiscard]]
nv::net_vec to_net_vec(const vms_vector &v)
{
	return {v.x, v.y, v.z};
}

[[nodiscard]]
vms_vector to_vms(const nv::net_vec &v)
{
	return {v.x, v.y, v.z};
}

[[nodiscard]]
bool segment_valid(const uint16_t s)
{
	return s < LevelSharedSegmentState.get_segments().get_count();
}

/* The host clock now (the estimate on a client). */
[[nodiscard]]
net_clock clock_now()
{
	int64_t t;
	if (net_interp_host_clock(t))
		return t;
	return timer_query();
}

/* The ship of player `pnum` is flown on this machine: the local player,
 * or on the host one of its bots.
 */
[[nodiscard]]
bool flown_here(const playernum_t pnum)
{
	return pnum == Player_num || (multi_i_am_master() && bot_is_local(pnum));
}

[[nodiscard]]
bool player_in_game(const uint8_t pnum)
{
	return pnum < N_players && pnum < MAX_PLAYERS && vcplayerptr(playernum_t{pnum})->connected != player_connection_status::disconnected;
}

[[nodiscard]]
uint8_t team_of(const playernum_t pnum)
{
	return underlying_value(multi_get_team_from_player(Netgame, pnum));
}

/* The player whose weapon this is (its highest parent), if a player. */
[[nodiscard]]
std::optional<playernum_t> shooter_of(const object &weapon)
{
	if (weapon.type != object_type::OBJ_WEAPON)
		return std::nullopt;
	auto &li{weapon.ctype.laser_info};
	if (li.parent_type != object_type::OBJ_PLAYER && li.parent_type != object_type::OBJ_GHOST)
		return std::nullopt;
	auto &Objects{LevelUniqueObjectState.Objects};
	if (li.parent_num > Highest_object_index)
		return std::nullopt;
	auto &parent{*Objects.vcptr(li.parent_num)};
	if (!laser_parent_is_matching_signature(li, parent))
		return std::nullopt;
	if (parent.type != object_type::OBJ_PLAYER && parent.type != object_type::OBJ_GHOST)
		return std::nullopt;
	/* Override macro, call only the getter: a ghost is a player's own
	 * object too.
	 */
	const auto pnum{(get_player_id)(parent)};
	if (pnum >= N_players || pnum >= MAX_PLAYERS)
		return std::nullopt;
	return pnum;
}

/* Who a damage without a weapon of a player's is credited to. */
struct credit
{
	nv::attacker_kind kind{nv::attacker_kind::none};
	uint8_t pid{nv::NET_V2_PLAYER_ID_NONE};
};

[[nodiscard]]
credit credit_of(const icobjptridx_t killer)
{
	if (killer == object_none)
		return {};
	switch (killer->type)
	{
		case object_type::OBJ_PLAYER:
		case object_type::OBJ_GHOST:
			{
				const auto pnum{(get_player_id)(*killer)};
				if (pnum < N_players && pnum < MAX_PLAYERS)
					return {nv::attacker_kind::player, static_cast<uint8_t>(pnum)};
				return {};
			}
		case object_type::OBJ_ROBOT:
			return {nv::attacker_kind::robot, nv::NET_V2_PLAYER_ID_NONE};
		case object_type::OBJ_CNTRLCEN:
			return {nv::attacker_kind::reactor, nv::NET_V2_PLAYER_ID_NONE};
		case object_type::OBJ_WEAPON:
			if (const auto s{shooter_of(*killer)})
				return {nv::attacker_kind::player, static_cast<uint8_t>(*s)};
#if DXX_BUILD_DESCENT == 2
			if (get_weapon_id(*killer) == weapon_id_type::PMINE_ID)
				return {nv::attacker_kind::mine, nv::NET_V2_PLAYER_ID_NONE};
#endif
			return {nv::attacker_kind::robot, nv::NET_V2_PLAYER_ID_NONE};
		default:
			return {};
	}
}

/* Section 6.5: the weapon a FIRE names. */
[[nodiscard]]
std::optional<weapon_id_type> fired_weapon_id(const uint8_t weapon, const uint8_t level)
{
	if (weapon == FLARE_ADJUST)
		return weapon_id_type::FLARE_ID;
	if (weapon >= MISSILE_ADJUST)
	{
		const unsigned i = weapon - MISSILE_ADJUST;
		if (i >= MAX_SECONDARY_WEAPONS)
			return std::nullopt;
		return Secondary_weapon_to_weapon_info[secondary_weapon_index{static_cast<uint8_t>(i)}];
	}
	if (weapon >= MAX_PRIMARY_WEAPONS)
		return std::nullopt;
	const primary_weapon_index w{weapon};
	/* D1's table has the cheap spreadfire; do_laser_firing fires this. */
	if (w == primary_weapon_index::spreadfire)
		return weapon_id_type::SPREADFIRE_ID;
#if DXX_BUILD_DESCENT == 2
	/* Fired as the laser, never under its own number. */
	if (w == primary_weapon_index::super_laser)
		return std::nullopt;
#endif
	if (w == primary_weapon_index::laser)
		switch (laser_level{level})
		{
			case laser_level::_1:
				return weapon_id_type::LASER_ID_L1;
			case laser_level::_2:
				return weapon_id_type::LASER_ID_L2;
			case laser_level::_3:
				return weapon_id_type::LASER_ID_L3;
			case laser_level::_4:
				return weapon_id_type::LASER_ID_L4;
#if DXX_BUILD_DESCENT == 2
			case laser_level::_5:
				return weapon_id_type::LASER_ID_L5;
			case laser_level::_6:
				return weapon_id_type::LASER_ID_L6;
#endif
			default:
				return std::nullopt;
		}
	return Primary_weapon_to_weapon_info[w];
}

[[nodiscard]]
bool valid_weapon(const unsigned id)
{
	return id < N_weapon_types;
}

[[nodiscard]]
weapon_id_type children_of(const weapon_id_type id)
{
#if DXX_BUILD_DESCENT == 2
	return Weapon_info[id].children;
#else
	return id == weapon_id_type::SMART_ID ? weapon_id_type::PLAYER_SMART_HOMING_ID : weapon_none;
#endif
}

/* The most projectiles one FIRE of this weapon creates (children are
 * not counted: they take their parent's id).
 */
[[nodiscard]]
unsigned max_projectiles(const uint8_t weapon, const uint8_t flags)
{
	if (weapon >= MISSILE_ADJUST)
		return 1;
	switch (primary_weapon_index{weapon})
	{
		case primary_weapon_index::laser:
			return (flags & LASER_QUAD) ? 4 : 2;
		case primary_weapon_index::spreadfire:
			return 3;
		case primary_weapon_index::plasma:
		case primary_weapon_index::fusion:
			return 2;
#if DXX_BUILD_DESCENT == 2
		case primary_weapon_index::helix:
			return 5;
		case primary_weapon_index::phoenix:
			return 2;
		case primary_weapon_index::omega:
			/* The placeholder and MAX_OMEGA_BLOBS (laser.cpp) blobs. */
			return 2 + 16;
#endif
		case primary_weapon_index::vulcan:
		default:
			return 1;
	}
}

/* The host's record of one projectile of an accepted shot. */
[[nodiscard]]
nv::shot_record shot_for(const playernum_t owner, const netid_t id, const std::optional<weapon_id_type> wid, const net_clock fire_time, const nv::net_vec &origin)
{
	nv::shot_record r;
	r.id = id;
	r.owner = static_cast<uint8_t>(owner);
	r.fire_time = fire_time;
	r.origin = origin;
	if (!wid || !valid_weapon(underlying_value(*wid)))
	{
		/* Nothing known: any weapon, far and long. */
		r.max_speed = 1000 * nv::NET_V2_UNIT;
		r.lifetime = nv::net_seconds(40);
		r.per_target_limit = static_cast<uint8_t>(1 + NUM_SMART_CHILDREN);
		r.total_limit = static_cast<uint8_t>(r.per_target_limit * nv::NET_V2_MAX_PLAYERS);
		return r;
	}
	const auto Difficulty_level{GameUniqueState.Difficulty_level};
	const auto &wi{Weapon_info[*wid]};
	r.weapon_id = underlying_value(*wid);
	std::int64_t speed{wi.speed[Difficulty_level]};
	net_clock life{wi.lifetime};
	bool splash{wi.damage_radius > 0};
	const bool persistent{wi.persistent == weapon_info::persistence_flag::persistent};
	unsigned children{0};
	if (const auto child{children_of(*wid)}; child != weapon_none && valid_weapon(underlying_value(child)))
	{
		const auto &ci{Weapon_info[child]};
		r.child_weapon_id = underlying_value(child);
		speed = std::max<std::int64_t>(speed, ci.speed[Difficulty_level]);
		life += ci.lifetime;
		splash = splash || ci.damage_radius > 0;
		children = NUM_SMART_CHILDREN;
	}
	else
		r.child_weapon_id = r.weapon_id;
	/* Thrust and homing: twice the nominal speed bounds the path; a mine
	 * inherits its ship's speed.
	 */
	r.max_speed = 2 * speed + nv::NET_V2_MAX_SHIP_SPEED;
	/* Flares live up to two seconds longer. */
	r.lifetime = life + nv::net_seconds(3);
#if DXX_BUILD_DESCENT == 2
	if (*wid == weapon_id_type::OMEGA_ID)
		r.reach = OMEGA_REACH;
#endif
	r.per_target_limit = static_cast<uint8_t>(1 + children);
	r.total_limit = static_cast<uint8_t>((splash || persistent || children) ? r.per_target_limit * nv::NET_V2_MAX_PLAYERS : 1);
	return r;
}

/* The most a direct hit of this weapon can do in a network game. */
[[nodiscard]]
fix max_direct_damage(const uint8_t weapon_id)
{
	if (!valid_weapon(weapon_id))
		return F1_0 * 200;
	const auto wid{weapon_id_type{weapon_id}};
	const auto &wi{Weapon_info[wid]};
	fix base{wi.strength[GameUniqueState.Difficulty_level]};
#if DXX_BUILD_DESCENT == 2
	if (wid == weapon_id_type::OMEGA_ID)
		base = fixmul(OMEGA_DAMAGE_FACTOR, base);
#endif
	/* A full fusion charge. */
	if (wid == weapon_id_type::FUSION_ID)
		base *= 4;
#if DXX_BUILD_DESCENT == 2
	base = fixmul(base, wi.multi_damage_scale);
#endif
	return base;
}

[[nodiscard]]
net_clock rewind_window()
{
	return nv::rewind_window(0);
}

/* The local copy of a weapon number, while it is the object it was. */
void bind_weapon(const vcobjptridx_t weapon, const netid_t id)
{
	C.ids[weapon.get_unchecked_index()] = {id, underlying_value(weapon->signature)};
}

[[nodiscard]]
netid_t netid_of(const vcobjptridx_t weapon)
{
	const auto &e{C.ids[weapon.get_unchecked_index()]};
	if (weapon->type != object_type::OBJ_WEAPON || e.signature != underlying_value(weapon->signature))
		return NETID_NONE;
	return e.netid;
}

void open_scope(const uint8_t pid, const netid_t first, const unsigned limit, const bool mines)
{
	auto &s{C.scope};
	s = {};
	s.open = true;
	s.mines = mines;
	s.pid = pid;
	s.first = first;
	s.limit = first == NETID_NONE ? 0 : std::min<unsigned>(limit, nv::NET_V2_FIRE_MAX_PROJECTILES);
}

/* ---- The host: damage and kills ---- */

/* The movement recording's hit, from the host's decision (the host) or
 * its DAMAGE (a client), so that every machine records the damage that
 * was done, not the collisions it saw.  As before stage 4, damage without
 * a weapon or a blast (walls, lava, bumps, the fusion overcharge) is not
 * a hit.
 */
void record_damage(const nv::damage_msg &d)
{
	namespace mr = ::dcx::movrec;
	std::uint8_t akind;
	switch (d.cause)
	{
		case nv::attacker_kind::player:
			if (d.kind == nv::hit_kind::self)
				return;
			akind = mr::attacker_kind::player;
			break;
		case nv::attacker_kind::robot:
			akind = mr::attacker_kind::robot;
			break;
		case nv::attacker_kind::reactor:
		case nv::attacker_kind::mine:
			akind = mr::attacker_kind::other;
			break;
		case nv::attacker_kind::none:
		default:
			return;
	}
	movement_record_damage(d.victim, d.attacker, akind, d.weapon_id, d.amount, d.kind == nv::hit_kind::splash);
}

[[nodiscard]]
nv::victim_view view_of(const playernum_t victim, const object &ship)
{
	nv::victim_view v;
	v.playing = vcplayerptr(victim)->connected == player_connection_status::playing && ship.type == object_type::OBJ_PLAYER;
	bool alive{C.host[victim].alive};
	if (victim == Player_num)
		alive = alive && Player_dead_state == player_dead_state::no;
	else if (bot_is_local(victim))
		alive = alive && !bot_ship_dying(victim);
	else
		/* Before its first report of this life the host has no shields
		 * for it.
		 */
		alive = alive && net_objects_host_has_report(victim);
	v.alive = alive;
	auto &pf{ship.ctype.player_info.powerup_flags};
	v.invulnerable = +(pf & player_flag::invulnerable);
	v.cloaked = +(pf & player_flag::cloaked);
	v.team = team_of(victim);
	return v;
}

void kill_local_ship(const nv::kill_attribution &k)
{
	auto &Objects{LevelUniqueObjectState.Objects};
	auto &plrobj{*Objects.vmptr(get_local_player().objnum)};
	/* An escaped ship does not die (start_player_death_sequence would
	 * not start, leaving it marked dead); the kill still counts.
	 */
	if (plrobj.type != object_type::OBJ_PLAYER || Player_dead_state != player_dead_state::no || get_local_player().connected != player_connection_status::playing)
		return;
	plrobj.ctype.player_info.killer_objnum = k.kind == nv::attacker_kind::player && k.pid < MAX_PLAYERS ? vcplayerptr(playernum_t{k.pid})->objnum : object_none;
	if (plrobj.shields >= 0)
		plrobj.shields = -1;
	/* The death sequence starts with the next removal of dead objects
	 * (obj_delete_all_that_should_be_dead), as a local death did.
	 */
	plrobj.flags |= OF_SHOULD_BE_DEAD;
}

/* The scores after a kill, as PLAYER_KILLED carries them. */
void fill_counts(nv::player_killed_msg &m)
{
	auto &Objects{LevelUniqueObjectState.Objects};
	auto &victim{Objects.vcptr(vcplayerptr(playernum_t{m.victim})->objnum)->ctype.player_info};
	m.victim_deaths = victim.net_killed_total;
	m.victim_kills = victim.net_kills_total;
	if (m.kind == nv::attacker_kind::player && m.killer < MAX_PLAYERS)
	{
		m.matrix = kill_matrix[m.killer][m.victim];
		m.killer_kills = Objects.vcptr(vcplayerptr(playernum_t{m.killer})->objnum)->ctype.player_info.net_kills_total;
	}
}

void host_kill(const playernum_t victim, const nv::kill_attribution killer, const uint8_t weapon_id)
{
	auto &hp{C.host[victim]};
	if (!hp.alive)
		return;
	hp.alive = false;
	hp.death_time = timer_query();
	nv::player_killed_msg m;
	m.victim = static_cast<uint8_t>(victim);
	m.killer = killer.pid;
	m.kind = killer.kind;
	m.weapon_id = weapon_id;
	m.team_vector = static_cast<uint8_t>(Netgame.team_vector);
	m.bounty_target = static_cast<uint8_t>(Bounty_target);
	multi_player_killed(victim, static_cast<uint8_t>(killer.kind), killer.pid);
	fill_counts(m);
	send_msg(session_msg::player_killed, m);
	con_printf(CON_VERBOSE, "net: P#%u killed (by kind %u, P#%u, weapon %u)", victim, static_cast<unsigned>(killer.kind), killer.pid, weapon_id);
	if (victim == Player_num)
		kill_local_ship(killer);
	/* A bot: bot_take_damage marked its death; it starts with its next
	 * frame.
	 */
	if (+(Game_mode & GM_BOUNTY))
		multi_send_bounty();
}

/* The host applies damage to player `victim`, whoever reported it. */
void host_apply(const playernum_t victim, const nv::hit_kind how, const nv::attacker_kind kind, const uint8_t attacker, const uint8_t weapon_id, const fix amount, const vms_vector &point, const bool always)
{
	if (victim >= N_players || victim >= MAX_PLAYERS)
		return;
	auto &Objects{LevelUniqueObjectState.Objects};
	const auto &&ship{Objects.vmptridx(vcplayerptr(victim)->objnum)};
	const auto view{view_of(victim, *ship)};
	const bool by_player{kind == nv::attacker_kind::player && attacker < N_players && attacker < MAX_PLAYERS};
	const nv::damage_context ctx{
		.team_game = +(Game_mode & GM_TEAM) != 0,
		.coop = +(Game_mode & GM_MULTI_COOP) != 0,
		.no_friendly_fire = Netgame.NoFriendlyFire != 0,
		/* The host's own exit: the others still in the mine are
		 * playing (an escaped client is not, view_of).
		 */
		.endlevel = victim == Player_num && Endlevel_sequence != 0,
	};
	const auto verdict{nv::evaluate_damage(view, kind, by_player ? team_of(attacker) : 0, ctx, always, amount)};
	if (verdict != nv::damage_verdict::apply)
		return;
	fix shields;
	if (victim == Player_num)
	{
		PALETTE_FLASH_ADD(f2i(amount) * 4, -f2i(amount / 2), -f2i(amount / 2));
		shields = (ship->shields -= amount);
	}
	else if (bot_is_local(victim))
	{
		icobjptridx_t killer{object_none};
		if (by_player)
			killer = Objects.imptridx(vcplayerptr(playernum_t{attacker})->objnum);
		/* The bot's reaction to the hit, its shields and, if they are
		 * gone, its death.
		 */
		const fix before{ship->shields};
		if (!bot_take_damage(ship, killer, amount, false))
			return;
		/* It may have ignored the damage (its death is pending). */
		if (ship->shields == before)
			return;
		shields = ship->shields;
	}
	else
		shields = net_objects_host_damage(victim, amount);
	nv::damage_msg d;
	d.victim = static_cast<uint8_t>(victim);
	d.attacker = by_player ? attacker : nv::NET_V2_PLAYER_ID_NONE;
	d.weapon_id = weapon_id;
	d.kind = how;
	d.cause = kind;
	d.amount = amount;
	d.shields = shields;
	d.point = to_net_vec(point);
	send_msg(session_msg::damage, d);
	record_damage(d);
	if (shields < 0)
		host_kill(victim, nv::kill_credit(kind, attacker, player_in_game(attacker)), weapon_id);
}

/* ---- Reports of the shooter's machine ---- */

[[nodiscard]]
net_clock display_time(const playernum_t target)
{
	int64_t t;
	if (target != Player_num && net_interp_display_time(target, t))
		return t;
	return clock_now();
}

void client_report(const nv::weapon_hit_msg &m)
{
	send_msg(session_msg::weapon_hit, m);
}

void report_weapon_hit(const nv::hit_kind kind, const playernum_t shooter, const playernum_t victim, const vcobjptridx_t weapon, const vms_vector &point, const fix damage)
{
	const uint8_t wid{underlying_value(get_weapon_id(weapon))};
	if (multi_i_am_master())
	{
		/* The host's own shots and its bots' hit where the host shows the
		 * target: nothing to check.
		 */
		host_apply(victim, kind, nv::attacker_kind::player, static_cast<uint8_t>(shooter), wid, damage, point, false);
		return;
	}
	nv::weapon_hit_msg m;
	m.netid = netid_of(weapon);
	if (m.netid == NETID_NONE)
	{
		con_printf(CON_VERBOSE, "net: hit of weapon %u without an id not reported", wid);
		return;
	}
	m.weapon_id = wid;
	m.kind = kind;
	m.target = static_cast<uint8_t>(victim);
	m.target_time = nv::to_net_time(display_time(victim));
	m.point = to_net_vec(point);
	m.segment = static_cast<uint16_t>(weapon->segnum);
	m.damage = damage;
	client_report(m);
}

void flush_self_slot(self_damage &s, const bool force)
{
	if (!s.have)
		return;
	const auto amount{s.pending.take(clock_now(), force ? 0 : SELF_DAMAGE_INTERVAL)};
	if (amount <= 0)
		return;
	nv::weapon_hit_msg m;
	m.netid = NETID_NONE;
	m.weapon_id = 0xff;
	m.kind = nv::hit_kind::self;
	m.cause = s.cause;
	m.target = s.target;
	m.target_time = nv::to_net_time(clock_now());
	auto &Objects{LevelUniqueObjectState.Objects};
	auto &plrobj{*Objects.vcptr(get_local_player().objnum)};
	m.point = to_net_vec(plrobj.pos);
	m.segment = static_cast<uint16_t>(plrobj.segnum);
	m.damage = amount;
	client_report(m);
}

void flush_self(const bool force)
{
	for (auto &s : C.self)
		flush_self_slot(s, force);
}

/* Damage of the ship `victim`, flown here, without a player's weapon. */
void own_damage(const playernum_t victim, const credit c, const fix amount, const vms_vector &point)
{
	if (amount <= 0)
		return;
	const uint8_t target{c.kind == nv::attacker_kind::player && c.pid < MAX_PLAYERS ? c.pid : static_cast<uint8_t>(victim)};
	if (multi_i_am_master())
	{
		host_apply(victim, nv::hit_kind::self, c.kind, target, 0xff, amount, point, true);
		return;
	}
	if (victim != Player_num)
		return;
	/* Each cause (a robot and a wall at once) is summed on its own, so
	 * that two of them do not send a report every frame.
	 */
	self_damage *slot{};
	for (auto &s : C.self)
		if (s.have && s.cause == c.kind && s.target == target)
		{
			slot = &s;
			break;
		}
	if (!slot)
	{
		for (auto &s : C.self)
			if (!s.have || s.pending.empty())
			{
				slot = &s;
				break;
			}
		if (!slot)
		{
			/* More causes at once than slots: send the oldest. */
			slot = &C.self[0];
			flush_self_slot(*slot, true);
		}
		/* A slot keeps its cause while it has one; its interval runs on. */
		if (!slot->have || slot->cause != c.kind || slot->target != target)
		{
			*slot = {};
			slot->have = true;
			slot->cause = c.kind;
			slot->target = target;
		}
	}
	slot->pending.add(amount);
}

/* ---- The host: messages from clients ---- */

void host_receive_self(const playernum_t from, const nv::weapon_hit_msg &m)
{
	const fix amount{std::clamp<fix>(m.damage, 0, SELF_DAMAGE_MAX)};
	uint8_t attacker{static_cast<uint8_t>(from)};
	if (m.cause == nv::attacker_kind::player && m.target != from && m.target < N_players)
	{
		/* A bump: credited to the other ship if it is near. */
		auto &Objects{LevelUniqueObjectState.Objects};
		auto &a{*Objects.vcptr(vcplayerptr(from)->objnum)};
		auto &b{*Objects.vcptr(vcplayerptr(playernum_t{m.target})->objnum)};
		/* Never a teammate's: a suicide next to it would cost it a kill. */
		const bool teammate{+(Game_mode & GM_TEAM) && team_of(from) == team_of(playernum_t{m.target})};
		if (!teammate && b.type == object_type::OBJ_PLAYER && vm_vec_dist_quick(a.pos, b.pos) < BUMP_RANGE)
			attacker = m.target;
	}
	host_apply(from, nv::hit_kind::self, m.cause, attacker, 0xff, amount, to_vms(m.point), true);
}

void host_receive_hit(const playernum_t from, const std::span<const uint8_t> payload)
{
	const auto count{[](const nv::hit_verdict v) { ++C.hit_verdicts[static_cast<std::size_t>(v)]; }};
	const auto m{nv::weapon_hit_msg::read(payload)};
	if (!m)
		return count(nv::hit_verdict::malformed);
	const net_clock now{timer_query()};
	if (!C.host[from].bucket.take(now))
		return count(nv::hit_verdict::rate_limited);
	if (m->kind == nv::hit_kind::self)
		return host_receive_self(from, *m);
	if (m->target >= N_players)
		return count(nv::hit_verdict::no_target);
	const auto target_time{::dcx::net_interp::unwrap(m->target_time, now)};
	const auto at{nv::limit_rewind(now, target_time, rewind_window())};
	const auto shot{C.registry.find(m->netid)};
	const auto target_at{C.host[m->target].history.rewind(at.time)};
	auto &Objects{LevelUniqueObjectState.Objects};
	auto &target{*Objects.vcptr(vcplayerptr(playernum_t{m->target})->objnum)};
	nv::hit_verdict verdict;
	fix amount{};
	if (m->kind == nv::hit_kind::direct)
	{
		verdict = nv::judge_direct_hit(shot, static_cast<uint8_t>(from), m->weapon_id, m->target, now, target_time, m->point, target_at, target.size, PROJECTILE_SLACK);
		amount = nv::clamp_claim(m->damage, max_direct_damage(m->weapon_id));
	}
	else
	{
		if (!valid_weapon(m->weapon_id))
			return count(nv::hit_verdict::wrong_weapon);
		const auto &wi{Weapon_info[weapon_id_type{m->weapon_id}]};
		fix max_damage{wi.strength[GameUniqueState.Difficulty_level]};
#if DXX_BUILD_DESCENT == 2
		if (GameUniqueState.Difficulty_level == Difficulty_level_type::_0)
			max_damage /= 4;
#endif
		const auto s{nv::judge_splash_hit(shot, static_cast<uint8_t>(from), m->weapon_id, m->target, now, target_time, m->point, target_at, max_damage, wi.damage_radius)};
		verdict = s.verdict;
		amount = s.damage;
	}
	count(verdict);
	if (verdict != nv::hit_verdict::accept)
	{
		con_printf(CON_VERBOSE, "net: hit of P#%u on P#%u (id %04x, weapon %u) refused: %s", from, m->target, m->netid, m->weapon_id, nv::hit_verdict_name(verdict));
		return;
	}
	nv::consume_shot(*shot, m->target);
	host_apply(playernum_t{m->target}, m->kind, nv::attacker_kind::player, static_cast<uint8_t>(from), m->weapon_id, amount, to_vms(m->point), false);
}

/* ---- FIRE ---- */

/* Advance a shot that arrived late by `total` with its own physics: each
 * step sweeps its path, so it stops at the first wall or object.
 */
void catch_up(const vmobjptridx_t w, const net_clock total)
{
	if (total <= 0 || w->movement_source != object::movement_type::physics)
		return;
#if DXX_BUILD_DESCENT == 2
	/* A guided missile is driven by its owner's records. */
	if (get_weapon_id(w) == weapon_id_type::GUIDEDMISS_ID)
		return;
#endif
	const auto signature{w->signature};
	const fix saved{FrameTime};
	nv::catch_up(total, nv::NET_V2_CATCH_UP_STEP, [&](const net_clock dt) {
		if (w->type != object_type::OBJ_WEAPON || w->signature != signature || (w->flags & OF_SHOULD_BE_DEAD))
			return false;
		FrameTime = static_cast<fix>(dt);
		const auto previous{w->pos};
		do_physics_sim(LevelSharedRobotInfoState.Robot_info, w, previous, nullptr);
		w->lifeleft -= FrameTime;
		return w->type == object_type::OBJ_WEAPON && w->signature == signature && !(w->flags & OF_SHOULD_BE_DEAD) && w->lifeleft > 0;
	});
	FrameTime = saved;
}

/* Every machine but the shooter's: act out the shot from the shooter's
 * origin, with its seed and ids, and advance it by its age.
 */
void act_fire(const nv::fire_msg &m, const net_clock fire_time)
{
	if (m.pid >= N_players || m.pid == Player_num)
		return;
	auto &Objects{LevelUniqueObjectState.Objects};
	const auto &&ship{Objects.vmptridx(vcplayerptr(playernum_t{m.pid})->objnum)};
	if (ship->type != object_type::OBJ_PLAYER && ship->type != object_type::OBJ_GHOST)
		return;
	const auto saved_pos{ship->pos};
	const auto saved_orient{ship->orient};
	const auto saved_segment{ship->segnum};
	const bool placed{segment_valid(m.segment)};
	if (placed)
	{
		ship->pos = to_vms(m.origin);
		ship->orient = vms_matrix_from_quaternion(vms_quaternion{m.orient.w, m.orient.x, m.orient.y, m.orient.z});
		if (ship->segnum != segnum_t{m.segment})
			obj_relink(Objects.vmptr, vmsegptr, ship, vmsegptridx(segnum_t{m.segment}));
	}
	open_scope(m.pid, m.netid, m.count, false);
	d_srand(m.seed);
	const icobjidx_t track{m.track < N_players ? icobjidx_t{vcplayerptr(playernum_t{m.track})->objnum} : icobjidx_t{object_none}};
	multi_do_fire(playernum_t{m.pid}, m.weapon, m.level, m.flags, ship->orient.fvec, track);
	const auto created{C.scope.created};
	const auto n_created{C.scope.n_created};
	C.scope.open = false;
	if (placed)
	{
		ship->pos = saved_pos;
		ship->orient = saved_orient;
		if (ship->segnum != saved_segment)
			obj_relink(Objects.vmptr, vmsegptr, ship, vmsegptridx(saved_segment));
	}
	const auto age{nv::catch_up_time(clock_now(), fire_time)};
	for (unsigned i = 0; i < n_created; ++i)
		if (created[i] <= Highest_object_index)
			catch_up(Objects.vmptridx(created[i]), age);
}

void host_register(const playernum_t owner, const nv::fire_msg &m, const net_clock fire_time)
{
	const auto wid{fired_weapon_id(m.weapon, m.level)};
	for (unsigned i = 0; i < m.count; ++i)
		C.registry.add(shot_for(owner, nv::netid_advance(m.netid, i), wid, fire_time, m.origin));
}

[[nodiscard]]
net_clock fire_wait_of(const uint8_t weapon, const uint8_t level)
{
	/* allowed_to_fire_flare: a quarter of a second. */
	if (weapon == FLARE_ADJUST)
		return F1_0 / 4;
#if DXX_BUILD_DESCENT == 2
	if (weapon == underlying_value(primary_weapon_index::omega))
		return F1_0 / 20;
#endif
	if (const auto wid{fired_weapon_id(weapon, level)}; wid && valid_weapon(underlying_value(*wid)))
		return Weapon_info[*wid].fire_wait;
	return 0;
}

void host_receive_fire(const playernum_t from, const std::span<const uint8_t> payload)
{
	const auto count{[](const nv::fire_verdict v) { ++C.fire_verdicts[static_cast<std::size_t>(v)]; }};
	auto m{nv::fire_msg::read(payload)};
	if (!m || (m->count && nv::netid_creator(m->netid) != from))
		return count(nv::fire_verdict::malformed);
	/* A weapon the game has, with no more projectiles than it fires: an
	 * unknown weapon would be registered as a shot of any weapon.
	 */
	if (const auto wid{fired_weapon_id(m->weapon, m->level)}; !wid || !valid_weapon(underlying_value(*wid)) || m->count > max_projectiles(m->weapon, m->flags))
		return count(nv::fire_verdict::malformed);
	/* The shooter is the sender, whatever it wrote. */
	m->pid = static_cast<uint8_t>(from);
	auto &hp{C.host[from]};
	const net_clock now{timer_query()};
	if (!hp.bucket.take(now))
		return count(nv::fire_verdict::rate_limited);
	const auto fire_time{::dcx::net_interp::unwrap(m->fire_time, now)};
	const bool alive{hp.alive || fire_time <= hp.death_time};
	const bool owned{net_objects_host_owns_weapon(from, m->weapon, m->level)};
	const auto shooter_at{hp.history.empty() ? std::nullopt : hp.history.rewind(fire_time)};
	auto verdict{nv::judge_fire(now, fire_time, alive, owned, m->origin, shooter_at)};
	if (verdict == nv::fire_verdict::accept)
	{
		/* The game keeps a time for each: primaries, missiles and
		 * bombs, flares.
		 */
		auto &limiter{m->weapon == FLARE_ADJUST ? hp.flare : m->weapon >= MISSILE_ADJUST ? hp.secondary : hp.primary};
		/* A secondary's volley (fire_count) comes one round per frame. */
		unsigned rounds{1};
		if (m->weapon != FLARE_ADJUST && m->weapon >= MISSILE_ADJUST)
			if (const auto wid{fired_weapon_id(m->weapon, m->level)}; wid && valid_weapon(underlying_value(*wid)))
				rounds = static_cast<unsigned>(std::max<int>(Weapon_info[*wid].fire_count, 1));
		if (!limiter.allow(fire_time, fire_wait_of(m->weapon, m->level), rounds))
			verdict = nv::fire_verdict::too_fast;
	}
	count(verdict);
	if (verdict != nv::fire_verdict::accept)
	{
		con_printf(CON_VERBOSE, "net: FIRE of P#%u (weapon %u) refused: %s", from, m->weapon, nv::fire_verdict_name(verdict));
		return;
	}
	host_register(from, *m, fire_time);
	send_msg(session_msg::fire, *m, from);
	act_fire(*m, fire_time);
}

void client_receive_fire(const std::span<const uint8_t> payload)
{
	const auto m{nv::fire_msg::read(payload)};
	if (!m || m->pid == Player_num)
		return;
	act_fire(*m, ::dcx::net_interp::unwrap(m->fire_time, clock_now()));
}

/* ---- DAMAGE, PLAYER_KILLED, PLAYER_SPAWN on a client ---- */

void client_receive_damage(const std::span<const uint8_t> payload)
{
	const auto m{nv::damage_msg::read(payload)};
	if (!m || m->victim >= N_players)
		return;
	record_damage(*m);
	if (m->victim == Player_num)
	{
		if (Player_dead_state == player_dead_state::no)
			PALETTE_FLASH_ADD(f2i(m->amount) * 4, -f2i(m->amount / 2), -f2i(m->amount / 2));
		net_objects_own_damage(m->amount);
		return;
	}
	/* The others' shields, as the host has them. */
	auto &Objects{LevelUniqueObjectState.Objects};
	auto &obj{*Objects.vmptr(vcplayerptr(playernum_t{m->victim})->objnum)};
	if (obj.type == object_type::OBJ_PLAYER)
		obj.shields = m->shields;
}

void client_receive_killed(const std::span<const uint8_t> payload)
{
	auto m{nv::player_killed_msg::read(payload)};
	if (!m || m->victim >= N_players)
		return;
	/* A killer this machine does not know yet (a join in progress): no
	 * killer, but the victim dies all the same.
	 */
	if (m->kind == nv::attacker_kind::player && m->killer >= N_players)
	{
		m->kind = nv::attacker_kind::none;
		m->killer = nv::NET_V2_PLAYER_ID_NONE;
	}
	Netgame.team_vector = m->team_vector;
	Bounty_target = m->bounty_target;
	multi_player_killed(playernum_t{m->victim}, static_cast<uint8_t>(m->kind), m->killer);
	/* The host's counts, which the rules gave here too unless an earlier
	 * message was missed.
	 */
	auto &Objects{LevelUniqueObjectState.Objects};
	auto &victim{Objects.vmptr(vcplayerptr(playernum_t{m->victim})->objnum)->ctype.player_info};
	victim.net_killed_total = m->victim_deaths;
	victim.net_kills_total = m->victim_kills;
	if (m->kind == nv::attacker_kind::player)
	{
		kill_matrix[m->killer][m->victim] = m->matrix;
		Objects.vmptr(vcplayerptr(playernum_t{m->killer})->objnum)->ctype.player_info.net_kills_total = m->killer_kills;
	}
	if (m->victim == Player_num)
		kill_local_ship({m->kind, m->killer});
}

void apply_spawn(const nv::player_spawn_msg &m)
{
	if (m.pid >= N_players || m.pid == Player_num)
		return;
	auto &Objects{LevelUniqueObjectState.Objects};
	const auto &&ship{Objects.vmptridx(vcplayerptr(playernum_t{m.pid})->objnum)};
	if (ship->type != object_type::OBJ_PLAYER && ship->type != object_type::OBJ_GHOST)
		return;
	/* The appearance where the ship appears (its records follow). */
	if (segment_valid(m.segment))
	{
		ship->pos = to_vms(m.pos);
		ship->orient = vms_matrix_from_quaternion(vms_quaternion{m.orient.w, m.orient.x, m.orient.y, m.orient.z});
		if (ship->segnum != segnum_t{m.segment})
			obj_relink(Objects.vmptr, vmsegptr, ship, vmsegptridx(segnum_t{m.segment}));
	}
	multi_player_spawned(playernum_t{m.pid});
}

void host_spawned(const playernum_t pnum)
{
	auto &hp{C.host[pnum]};
	hp.alive = true;
	hp.history.clear();
	hp.pushed = false;
	hp.primary.reset();
	hp.secondary.reset();
	hp.flare.reset();
}

void host_receive_spawn(const playernum_t from, const std::span<const uint8_t> payload)
{
	auto m{nv::player_spawn_msg::read(payload)};
	if (!m || m->pid != from || from >= N_players)
		return;
	host_spawned(from);
	send_msg(session_msg::player_spawn, *m, from);
	apply_spawn(*m);
}

void client_receive_spawn(const std::span<const uint8_t> payload)
{
	if (const auto m{nv::player_spawn_msg::read(payload)})
		apply_spawn(*m);
}

}

bool net_combat_active()
{
	return +(Game_mode & GM_NETWORK) && Newdemo_state != ND_STATE_PLAYBACK;
}

void net_combat_level_start()
{
	C = {};
	C.expected_mines.fill(NETID_NONE);
	const net_clock now{timer_query()};
	for (auto &h : C.host)
		h.bucket.reset(now);
}

void net_combat_host_join(const playernum_t pnum)
{
	if (pnum >= MAX_PLAYERS)
		return;
	C.host[pnum] = {};
	C.host[pnum].bucket.reset(timer_query());
	/* The slot's previous player: its shots and mines are not the new
	 * player's.
	 */
	C.registry.forget_owner(static_cast<uint8_t>(pnum));
	C.expected_mines[pnum] = NETID_NONE;
}

void net_combat_frame()
{
	if (!net_combat_active() || (Network_status != network_state::playing && Network_status != network_state::endlevel))
		return;
	/* A shot is created within one call; nothing is left open. */
	C.scope.open = false;
	if (!multi_i_am_master())
	{
		flush_self(false);
		return;
	}
	/* The host's own ship and its bots are where they are now. */
	const net_clock now{timer_query()};
	auto &Objects{LevelUniqueObjectState.Objects};
	for (playernum_t i = 0; i < N_players && i < MAX_PLAYERS; ++i)
	{
		if (!flown_here(i) || vcplayerptr(i)->connected != player_connection_status::playing)
			continue;
		auto &ship{*Objects.vcptr(vcplayerptr(i)->objnum)};
		auto &hp{C.host[i]};
		if (ship.type == object_type::OBJ_PLAYER && (!hp.pushed || now - hp.last_push >= HOST_HISTORY_STEP))
		{
			hp.history.push(now, to_net_vec(ship.pos), static_cast<uint16_t>(ship.segnum));
			hp.last_push = now;
			hp.pushed = true;
		}
	}
	C.registry.expire(now);
}

void net_combat_receive(const playernum_t from, const uint8_t type, const std::span<const uint8_t> payload)
{
	if (!net_combat_active() || from >= MAX_PLAYERS)
		return;
	const auto t{static_cast<session_msg>(type)};
	if (multi_i_am_master())
	{
		if (from == Player_num || from >= N_players)
			return;
		switch (t)
		{
			case session_msg::fire:
				host_receive_fire(from, payload);
				break;
			case session_msg::weapon_hit:
				host_receive_hit(from, payload);
				break;
			case session_msg::player_spawn:
				host_receive_spawn(from, payload);
				break;
			default:
				con_printf(CON_VERBOSE, "net: combat message %u from client P#%u ignored", type, from);
				break;
		}
		return;
	}
	switch (t)
	{
		case session_msg::fire:
			client_receive_fire(payload);
			break;
		case session_msg::damage:
			client_receive_damage(payload);
			break;
		case session_msg::player_killed:
			client_receive_killed(payload);
			break;
		case session_msg::player_spawn:
			client_receive_spawn(payload);
			break;
		default:
			con_printf(CON_VERBOSE, "net: combat message %u from the host ignored", type);
			break;
	}
}

void net_combat_weapon_created(const vcobjptridx_t weapon, const vcobjptridx_t parent)
{
	if (!net_combat_active())
		return;
	/* A child (smart blobs, the earthshaker's missiles): its parent's id. */
	if (parent->type == object_type::OBJ_WEAPON)
	{
		bind_weapon(weapon, netid_of(parent));
		return;
	}
	auto &s{C.scope};
	if (s.open && (parent->type == object_type::OBJ_PLAYER || parent->type == object_type::OBJ_GHOST) && (get_player_id)(*parent) == s.pid && s.used < s.limit)
	{
		bind_weapon(weapon, nv::netid_advance(s.first, s.used++));
		if (s.n_created < s.created.size())
			s.created[s.n_created++] = weapon.get_unchecked_index();
		return;
	}
	bind_weapon(weapon, NETID_NONE);
}

void net_combat_begin_fire(const object_base &shooter)
{
	if (!net_combat_active() || shooter.type != object_type::OBJ_PLAYER)
		return;
	const auto pid{get_player_id(shooter)};
	if (pid >= MAX_PLAYERS)
		return;
	open_scope(static_cast<uint8_t>(pid), nv::dynamic_netid(static_cast<uint8_t>(pid), C.counter[pid & (nv::NETID_CREATORS - 1)]), nv::NET_V2_FIRE_MAX_PROJECTILES, false);
	/* The shot's random values (spread, speed variance) come out the same
	 * on every machine.
	 */
	const auto seed{static_cast<uint16_t>(d_rand())};
	C.scope.seed = seed;
	d_srand(seed);
}

void net_combat_abort_fire()
{
	if (!C.scope.open || C.scope.mines)
		return;
	/* The ids given stay used: they were given to objects. */
	auto &c{C.counter[C.scope.pid & (nv::NETID_CREATORS - 1)]};
	c = static_cast<uint16_t>((c + C.scope.used) & nv::NETID_COUNTER_MASK);
	C.scope.open = false;
}

void net_combat_send_fire(const playernum_t pnum, const uint8_t weapon, const uint8_t level, const uint8_t flags, const objnum_t track)
{
	auto &s{C.scope};
	if (!net_combat_active() || pnum >= MAX_PLAYERS)
	{
		s.open = false;
		return;
	}
	nv::fire_msg m;
	m.pid = static_cast<uint8_t>(pnum);
	const net_clock fire_time{clock_now()};
	m.fire_time = nv::to_net_time(fire_time);
	m.weapon = weapon;
	m.level = level;
	m.flags = flags;
	auto &Objects{LevelUniqueObjectState.Objects};
	auto &ship{*Objects.vcptr(vcplayerptr(pnum)->objnum)};
	const auto q{build_quaternionpos(ship)};
	m.origin = to_net_vec(q.pos);
	m.segment = static_cast<uint16_t>(q.segment);
	m.orient = {q.orient.w, q.orient.x, q.orient.y, q.orient.z};
	if (s.open && s.pid == pnum && !s.mines)
	{
		m.seed = s.seed;
		m.count = static_cast<uint8_t>(s.used);
		m.netid = s.used ? s.first : NETID_NONE;
		auto &c{C.counter[pnum & (nv::NETID_CREATORS - 1)]};
		c = static_cast<uint16_t>((c + s.used) & nv::NETID_COUNTER_MASK);
	}
	s.open = false;
	if (track != object_none && track <= Highest_object_index)
	{
		auto &t{*Objects.vcptr(track)};
		if (t.type == object_type::OBJ_PLAYER)
			m.track = static_cast<uint8_t>(get_player_id(t));
	}
	if (multi_i_am_master())
	{
		host_register(pnum, m, fire_time);
		send_msg(session_msg::fire, m);
	}
	else
		send_msg(session_msg::fire, m);
}

bool net_combat_weapon_hit_player(const vmobjptridx_t playerobj, const vcobjptridx_t weapon, const vms_vector &point, const fix damage)
{
	if (!net_combat_active() || playerobj->type != object_type::OBJ_PLAYER)
		return false;
	const auto victim{get_player_id(playerobj)};
	if (const auto shooter{shooter_of(weapon)})
	{
		if (flown_here(*shooter))
			report_weapon_hit(nv::hit_kind::direct, *shooter, victim, weapon, point, damage);
		return true;
	}
	/* A robot's or the reactor's weapon: the victim's machine. */
	if (flown_here(victim))
		own_damage(victim, credit_of(weapon), damage, point);
	return true;
}

bool net_combat_splash_player(const vmobjptridx_t playerobj, const icobjptridx_t origin, const icobjptridx_t killer, const fix damage, const object_base &fireball)
{
	if (!net_combat_active() || playerobj->type != object_type::OBJ_PLAYER)
		return false;
	const auto victim{get_player_id(playerobj)};
	if (origin != object_none && origin->type == object_type::OBJ_WEAPON)
	{
		if (const auto shooter{shooter_of(origin)})
		{
			if (flown_here(*shooter))
				report_weapon_hit(nv::hit_kind::splash, *shooter, victim, origin, fireball.pos, damage);
			return true;
		}
	}
	else if (origin != object_none && (origin->type == object_type::OBJ_PLAYER || origin->type == object_type::OBJ_GHOST))
	{
		/* A ship's explosion (its deres): the host's view decides. */
		if (multi_i_am_master())
			if (const auto c{credit_of(origin)}; c.kind == nv::attacker_kind::player)
				host_apply(victim, nv::hit_kind::splash, c.kind, c.pid, 0xff, damage, fireball.pos, false);
		return true;
	}
	if (flown_here(victim))
		own_damage(victim, credit_of(killer), damage, fireball.pos);
	return true;
}

bool net_combat_damage_player(object &playerobj, const icobjptridx_t killer, const fix damage, const bool always)
{
	if (!net_combat_active())
		return false;
	(void)always;
	if (playerobj.type != object_type::OBJ_PLAYER)
		return true;
	const auto victim{get_player_id(playerobj)};
	if (flown_here(victim))
		own_damage(victim, credit_of(killer), damage, playerobj.pos);
	return true;
}

void net_combat_local_death_started()
{
	if (!net_combat_active())
		return;
	C.self = {};
	multi_strip_robots(Player_num);
}

void net_combat_send_spawn(const playernum_t pnum)
{
	if (!net_combat_active() || pnum >= MAX_PLAYERS)
		return;
	auto &Objects{LevelUniqueObjectState.Objects};
	auto &ship{*Objects.vcptr(vcplayerptr(pnum)->objnum)};
	nv::player_spawn_msg m;
	m.pid = static_cast<uint8_t>(pnum);
	if (+(ship.ctype.player_info.powerup_flags & player_flag::invulnerable))
		m.flags |= underlying_value(nv::spawn_flag::invulnerable);
	const auto q{build_quaternionpos(ship)};
	m.pos = to_net_vec(q.pos);
	m.segment = static_cast<uint16_t>(q.segment);
	m.orient = {q.orient.w, q.orient.x, q.orient.y, q.orient.z};
	if (multi_i_am_master())
		host_spawned(pnum);
	if (pnum == Player_num)
		C.self = {};
	send_msg(session_msg::player_spawn, m);
	net_objects_player_reappeared(pnum);
}

bool net_combat_host_player_alive(const playernum_t pnum)
{
	return pnum < MAX_PLAYERS && C.host[pnum].alive;
}

void net_combat_host_position(const playernum_t pnum, const int64_t time, const vms_vector &pos, const segnum_t segnum)
{
	if (pnum >= MAX_PLAYERS)
		return;
	C.host[pnum].history.push(time, to_net_vec(pos), static_cast<uint16_t>(segnum));
}

uint16_t net_combat_reserve_mine_ids(const playernum_t pnum)
{
	if (!net_combat_active() || pnum >= MAX_PLAYERS)
		return NETID_NONE;
	auto &c{C.counter[pnum & (nv::NETID_CREATORS - 1)]};
	const auto first{nv::dynamic_netid(static_cast<uint8_t>(pnum), c)};
	c = static_cast<uint16_t>((c + MINE_IDS_PER_DEATH) & nv::NETID_COUNTER_MASK);
	C.expected_mines[pnum] = first;
	return first;
}

void net_combat_expect_mines(const playernum_t pnum, const uint16_t first)
{
	if (pnum >= MAX_PLAYERS)
		return;
	C.expected_mines[pnum] = first != NETID_NONE && !nv::is_level_netid(first) && nv::netid_creator(first) == pnum ? first : NETID_NONE;
}

void net_combat_begin_mines(const vcobjptridx_t playerobj)
{
	if (!net_combat_active())
		return;
	const auto pnum{(get_player_id)(*playerobj)};
	if (pnum >= MAX_PLAYERS)
		return;
	open_scope(static_cast<uint8_t>(pnum), C.expected_mines[pnum], MINE_IDS_PER_DEATH, true);
	C.expected_mines[pnum] = NETID_NONE;
}

void net_combat_end_mines()
{
	auto &s{C.scope};
	if (!s.open || !s.mines)
		return;
	s.open = false;
	if (!multi_i_am_master() || s.first == NETID_NONE)
		return;
	/* The host: the mines are shots of their owner. */
	auto &Objects{LevelUniqueObjectState.Objects};
	const net_clock now{timer_query()};
	for (unsigned i = 0; i < s.n_created; ++i)
	{
		const auto &&w{Objects.vcptridx(s.created[i])};
		if (w->type != object_type::OBJ_WEAPON)
			continue;
		C.registry.add(shot_for(s.pid, nv::netid_advance(s.first, i), get_weapon_id(w), now, to_net_vec(w->pos)));
	}
}

uint16_t net_combat_netid_of(const vcobjptridx_t weapon)
{
	return netid_of(weapon);
}

imobjptridx_t net_combat_object_of(const uint16_t netid)
{
	if (netid == NETID_NONE)
		return object_none;
	auto &Objects{LevelUniqueObjectState.Objects};
	for (objnum_t i = 0; i <= Highest_object_index; ++i)
	{
		const auto &e{C.ids[i]};
		if (e.netid != netid)
			continue;
		const auto &&o{Objects.vmptridx(i)};
		if (o->type == object_type::OBJ_WEAPON && underlying_value(o->signature) == e.signature)
			return o;
	}
	return object_none;
}

}

#endif
