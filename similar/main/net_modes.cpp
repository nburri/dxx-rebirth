/*
 * This file is part of the DXX-Rebirth project <https://github.com/dxx-rebirth/dxx-rebirth/>.
 * It is copyright by its individual contributors, as recorded in the
 * project's Git history.  See COPYING.txt at the top level for license
 * terms and a link to the Git history.
 */
/*
 * Game modes the host decides in a network game
 * (Documentation/network-protocol-v2.md, section 6.8 and "Stage 6a: game
 * modes" in section 8; the rules are in net_v2_modes.h).
 *
 * Capture the flag.  Every machine used to test its own ship against the
 * goals and announce its capture (MULTI_CAPTURE_BONUS); the captured flag
 * came back only when the host's level inventory missed it, up to two
 * seconds later; a bot never scored, since only the local player was
 * tested; and a capture that crossed the carrier's death on the way to
 * the host could leave a flag twice or not at all.  Now the host tests
 * every ship each frame: its own and its bots' where they are, a client's
 * at its newest accepted position.  A capture takes the flag from the
 * carrier on the host (no report of the client brings it back), credits
 * the scores, announces CAPTURE with them and puts the flag back into the
 * level at once (OBJ_CREATE).  Captures, pickups, drops and deaths happen
 * in one order, the host's.  The host also counts the flags every frame:
 * each is in the level or carried, once (`flag_census`).
 *
 * Hoard and team hoard.  The same way, a client announced its own scored
 * orbs (MULTI_ORB_BONUS), bots never scored, and the extra orb a death
 * drops was decided by the dying player's machine, for humans only (and
 * the host flagged the next report of the dead player as suspicious).
 * Now the host tests every ship for orbs in a goal (ORB_BONUS) and decides
 * the extra orb from its own verdict on the kill, for every player.  The
 * orbs carried are the host's: a report never changes them.  The host
 * counts the orbs every frame (in the level and carried = the deaths'
 * extra orbs less those scored, `orb_census`).
 */

#include "dxxsconf.h"

#if DXX_USE_MULTIPLAYER

#include <array>
#include <cinttypes>
#include <optional>
#include <span>
#include <utility>

#include "net_v2_modes.h"
#include "net_v2_session.h"
#include "net_v2_game.h"
#include "multi.h"
#include "bot.h"
#include "fireball.h"
#include "gameseg.h"
#include "object.h"
#include "player.h"
#include "powerup.h"
#include "segment.h"
#include "newdemo.h"
#include "timer.h"
#include "console.h"
#include "d_levelstate.h"

namespace dsx {

namespace {

namespace nv = ::dcx::net_v2;
using nv::session_msg;

struct modes_state
{
	/* The flags of each team the level started with (counted at the
	 * first census of the level).
	 */
	std::array<uint8_t, nv::CTF_TEAMS> flags_expected{};
	bool have_expected{};
	unsigned captures{};
	/* Census: frames counted, frames with a broken count, and whether
	 * the last one was broken (logged when that changes).
	 */
	uint64_t census_frames{};
	uint64_t census_broken{};
	bool broken{};
	/* Hoard: a death the host decided earns its extra orb (cleared when
	 * the death's orbs are dropped).
	 */
	std::array<bool, MAX_PLAYERS> death_orb{};
	unsigned orb_scores{};
	unsigned orbs_scored{};
	unsigned orbs_created{};
	/* Orbs of deaths that found no room in the level. */
	unsigned orbs_lost{};
	/* The orbs of the level at the first census (none, as a rule). */
	unsigned orbs_at_start{};
	bool have_orbs_at_start{};
	uint64_t orb_frames{};
	uint64_t orb_broken{};
	bool orbs_broken{};
};

modes_state M;

#if DXX_BUILD_DESCENT == 2
[[nodiscard]]
bool network_game()
{
	return +(Game_mode & GM_NETWORK) && Newdemo_state != ND_STATE_PLAYBACK;
}

[[nodiscard]]
uint8_t team_of(const playernum_t pnum)
{
	return underlying_value(multi_get_team_from_player(Netgame, pnum));
}

[[nodiscard]]
std::optional<uint8_t> goal_team(const shared_segment &seg)
{
	switch (seg.special)
	{
		case segment_special::goal_blue:
			return nv::CTF_TEAM_BLUE;
		case segment_special::goal_red:
			return nv::CTF_TEAM_RED;
		default:
			return std::nullopt;
	}
}

[[nodiscard]]
powerup_type_t flag_powerup(const uint8_t flag)
{
	return flag == nv::CTF_TEAM_BLUE ? powerup_type_t::POW_FLAG_BLUE : powerup_type_t::POW_FLAG_RED;
}

[[nodiscard]]
const char *team_name(const uint8_t team)
{
	return team == nv::CTF_TEAM_BLUE ? "blue" : "red";
}

/* The ship of player `pnum` is flown on this machine: the local player,
 * or on the host one of its bots.
 */
[[nodiscard]]
bool flown_here(const playernum_t pnum)
{
	return pnum == Player_num || (multi_i_am_master() && bot_is_local(pnum));
}

/* The ship of player `pnum`, if it is in the game and alive as the host
 * sees it (not after the host decided its death).
 */
[[nodiscard]]
const object *live_ship(const playernum_t pnum)
{
	auto &plr{*vcplayerptr(pnum)};
	if (plr.connected != player_connection_status::playing)
		return nullptr;
	auto &Objects{LevelUniqueObjectState.Objects};
	const auto &ship{*Objects.vcptr(plr.objnum)};
	if (ship.type != object_type::OBJ_PLAYER || !net_combat_host_player_alive(pnum))
		return nullptr;
	if (pnum == Player_num && Player_dead_state != player_dead_state::no)
		return nullptr;
	return &ship;
}

/* The team whose goal the ship of player `pnum` is in, if a goal: the
 * host's own ship and its bots where they are, a client's ship at its
 * newest accepted position (the ship object here is shown a little in
 * the past).
 */
[[nodiscard]]
std::optional<uint8_t> goal_of_ship(const playernum_t pnum, const object &ship)
{
	segnum_t segnum{ship.segnum};
	if (!flown_here(pnum))
	{
		vms_vector pos;
		if (!net_interp_newest_live_position(pnum, pos, segnum))
			return std::nullopt;
	}
	auto &vcsegptr{LevelSharedSegmentState.get_segments().vcptr};
	if (const auto s{vcsegptr.check_untrusted(segnum)})
		return goal_team(**s);
	return std::nullopt;
}

/* What the host knows of player `pnum` for the goal test. */
[[nodiscard]]
nv::capture_check capture_view(const playernum_t pnum)
{
	nv::capture_check c;
	const auto ship{live_ship(pnum)};
	if (!ship)
		return c;
	c.alive = true;
	c.team = team_of(pnum);
	c.carries_flag = +(ship->ctype.player_info.powerup_flags & player_flag::has_team_flag);
	if (c.carries_flag)
		c.goal = goal_of_ship(pnum, *ship);
	return c;
}

[[nodiscard]]
nv::orb_check orb_view(const playernum_t pnum)
{
	nv::orb_check c;
	const auto ship{live_ship(pnum)};
	if (!ship)
		return c;
	c.alive = true;
	c.orbs = ship->ctype.player_info.hoard.orbs;
	if (c.orbs)
		c.in_goal = goal_of_ship(pnum, *ship).has_value();
	return c;
}

void apply_capture(const nv::capture_msg &m)
{
	multi_apply_capture(playernum_t{m.pid}, m.scores.team_score, m.scores.kills, m.scores.kill_goal_count, true);
}

nv::flag_census count_flags();

/* Host: player `pnum` of team `team` scores. */
void host_capture(const playernum_t pnum, const uint8_t team)
{
	const auto flag{nv::other_team(team)};
	net_objects_host_take_team_flag(pnum);
	auto &Objects{LevelUniqueObjectState.Objects};
	const auto &player_info{Objects.vcptr(vcplayerptr(pnum)->objnum)->ctype.player_info};
	nv::capture_msg m;
	m.pid = static_cast<uint8_t>(pnum);
	m.team = team;
	m.flag = flag;
	m.scores = nv::capture_score({team_kills[multi_get_team_from_player(Netgame, pnum)], player_info.net_kills_total, player_info.KillGoalCount});
	std::array<uint8_t, nv::capture_msg::SIZE> buf;
	m.write(buf);
	::dsx::net_v2::game_broadcast(static_cast<uint8_t>(session_msg::capture), buf);
	++M.captures;
	con_printf(CON_NORMAL, "ctf: P#%u (%s) captured the %s flag; %s team %i", pnum, team_name(team), team_name(flag), team_name(team), m.scores.team_score);
	/* The flag goes back into the level now (after CAPTURE, so that
	 * every machine has taken it from the carrier first), unless the
	 * level has all its flags of the team without it.
	 */
	if (!M.have_expected || count_flags().total(flag) < M.flags_expected[flag])
	{
		if (net_drop_powerup_away_from(flag_powerup(flag), pnum) == object_none)
			con_printf(CON_URGENT, "ctf: the %s flag could not be put back; the level inventory will", team_name(flag));
	}
	else
		con_printf(CON_URGENT, "ctf: the %s flag is not put back: the level has all of them", team_name(flag));
	apply_capture(m);
}

void apply_orb_bonus(const nv::orb_bonus_msg &m)
{
	multi_apply_orb_bonus(playernum_t{m.pid}, m.orbs, m.scores.team_score, m.scores.kills, m.scores.kill_goal_count, +(Game_mode & GM_TEAM));
}

/* Host: player `pnum` scores `orbs` orbs. */
void host_orb_bonus(const playernum_t pnum, const uint8_t orbs)
{
	net_objects_host_take_orbs(pnum);
	auto &Objects{LevelUniqueObjectState.Objects};
	const auto &player_info{Objects.vcptr(vcplayerptr(pnum)->objnum)->ctype.player_info};
	nv::orb_bonus_msg m;
	m.pid = static_cast<uint8_t>(pnum);
	m.orbs = orbs;
	m.scores = nv::orb_score({team_kills[multi_get_team_from_player(Netgame, pnum)], player_info.net_kills_total, player_info.KillGoalCount}, orbs);
	std::array<uint8_t, nv::orb_bonus_msg::SIZE> buf;
	m.write(buf);
	::dsx::net_v2::game_broadcast(static_cast<uint8_t>(session_msg::orb_bonus), buf);
	++M.orb_scores;
	M.orbs_scored += orbs;
	con_printf(CON_NORMAL, "hoard: P#%u scored %u orbs (%i points); kills %i", pnum, orbs, nv::orb_points(orbs), m.scores.kills);
	apply_orb_bonus(m);
}

void host_check_goals()
{
	const bool hoard = game_mode_hoard(Game_mode);
	for (playernum_t i = 0; i < N_players && i < MAX_PLAYERS; ++i)
	{
		if (hoard)
		{
			const auto c{orb_view(i)};
			if (nv::orb_bonus_due(c))
				host_orb_bonus(i, c.orbs);
			continue;
		}
		const auto c{capture_view(i)};
		if (nv::capture_due(c))
			host_capture(i, c.team);
	}
}

/* Host: every orb is in the level or carried, as many as the deaths made
 * less those scored.
 */
void host_orb_census()
{
	nv::orb_census census;
	auto &Objects{LevelUniqueObjectState.Objects};
	for (auto &obj : Objects.vcptr)
		if (obj.type == object_type::OBJ_POWERUP && !(obj.flags & OF_SHOULD_BE_DEAD) && get_powerup_id(obj) == powerup_type_t::POW_HOARD_ORB)
			++census.in_level;
	for (playernum_t i = 0; i < N_players && i < MAX_PLAYERS; ++i)
	{
		auto &plr{*vcplayerptr(i)};
		if (plr.connected == player_connection_status::disconnected)
			continue;
		const auto &ship{*Objects.vcptr(plr.objnum)};
		if (ship.type == object_type::OBJ_PLAYER)
			census.carried += ship.ctype.player_info.hoard.orbs;
	}
	if (!M.have_orbs_at_start)
	{
		M.orbs_at_start = census.in_level;
		M.have_orbs_at_start = true;
	}
	++M.orb_frames;
	const bool broken{!census.holds(M.orbs_at_start, M.orbs_created, M.orbs_scored + M.orbs_lost)};
	if (broken)
		++M.orb_broken;
	if (broken != M.orbs_broken)
	{
		M.orbs_broken = broken;
		con_printf(broken ? CON_URGENT : CON_NORMAL, "hoard: orb count %s: %u in the level + %u carried; level start %u + %u made by deaths - %u scored - %u lost", broken ? "BROKEN" : "restored",
			census.in_level, census.carried, M.orbs_at_start, M.orbs_created, M.orbs_scored, M.orbs_lost);
	}
}

/* The flags in the level and carried, as the host sees them. */
nv::flag_census count_flags()
{
	nv::flag_census census;
	auto &Objects{LevelUniqueObjectState.Objects};
	for (auto &obj : Objects.vcptr)
	{
		if (obj.type != object_type::OBJ_POWERUP || (obj.flags & OF_SHOULD_BE_DEAD))
			continue;
		const auto id{get_powerup_id(obj)};
		if (id == powerup_type_t::POW_FLAG_BLUE)
			++census.in_level[nv::CTF_TEAM_BLUE];
		else if (id == powerup_type_t::POW_FLAG_RED)
			++census.in_level[nv::CTF_TEAM_RED];
	}
	for (playernum_t i = 0; i < N_players && i < MAX_PLAYERS; ++i)
	{
		auto &plr{*vcplayerptr(i)};
		if (plr.connected == player_connection_status::disconnected)
			continue;
		const auto &ship{*Objects.vcptr(plr.objnum)};
		if (ship.type == object_type::OBJ_PLAYER && +(ship.ctype.player_info.powerup_flags & player_flag::has_team_flag))
			++census.carried[nv::other_team(team_of(i))];
	}
	return census;
}

/* Host: every flag is in the level or carried, once. */
void host_census()
{
	const auto census{count_flags()};
	if (!M.have_expected)
	{
		/* The level's flags, before anyone could take one. */
		M.flags_expected = census.in_level;
		M.have_expected = true;
		con_printf(CON_VERBOSE, "ctf: level flags: blue %u, red %u", M.flags_expected[nv::CTF_TEAM_BLUE], M.flags_expected[nv::CTF_TEAM_RED]);
	}
	++M.census_frames;
	const bool broken{!census.holds(M.flags_expected)};
	if (broken)
		++M.census_broken;
	if (broken != M.broken)
	{
		M.broken = broken;
		con_printf(broken ? CON_URGENT : CON_NORMAL, "ctf: flag count %s: blue flag %u in the level + %u carried, red flag %u + %u (level start: %u, %u)", broken ? "BROKEN" : "restored",
			census.in_level[nv::CTF_TEAM_BLUE], census.carried[nv::CTF_TEAM_BLUE],
			census.in_level[nv::CTF_TEAM_RED], census.carried[nv::CTF_TEAM_RED],
			M.flags_expected[nv::CTF_TEAM_BLUE], M.flags_expected[nv::CTF_TEAM_RED]);
	}
}

void client_receive_capture(const std::span<const uint8_t> payload)
{
	const auto m{nv::capture_msg::read(payload)};
	if (!m || m->pid >= N_players || !game_mode_capture_flag(Game_mode))
		return;
	apply_capture(*m);
}

void client_receive_orb_bonus(const std::span<const uint8_t> payload)
{
	const auto m{nv::orb_bonus_msg::read(payload)};
	if (!m || m->pid >= N_players || !game_mode_hoard(Game_mode))
		return;
	apply_orb_bonus(*m);
}
#endif

}

void net_modes_level_start()
{
	M = {};
}

void net_modes_frame()
{
#if DXX_BUILD_DESCENT == 2
	if (!network_game() || !multi_i_am_master() || Network_status != network_state::playing)
		return;
	if (game_mode_capture_flag(Game_mode))
	{
		host_check_goals();
		host_census();
	}
	else if (game_mode_hoard(Game_mode))
	{
		host_check_goals();
		host_orb_census();
	}
#endif
}

void net_modes_host_player_killed(const playernum_t victim, const bool by_player, const uint8_t killer)
{
#if DXX_BUILD_DESCENT == 2
	if (!network_game() || !multi_i_am_master() || victim >= MAX_PLAYERS || !game_mode_hoard(Game_mode))
		return;
	nv::death_view d;
	d.by_player = by_player && killer < N_players;
	d.killer = killer;
	d.victim = static_cast<uint8_t>(victim);
	d.team_game = +(Game_mode & GM_TEAM);
	d.victim_team = team_of(victim);
	d.killer_team = d.by_player ? team_of(killer) : d.victim_team;
	M.death_orb[victim] = nv::death_orb_due(d);
#else
	(void)victim;
	(void)by_player;
	(void)killer;
#endif
}

uint8_t net_modes_host_death_orbs(const playernum_t pnum, const uint8_t orbs)
{
#if DXX_BUILD_DESCENT == 2
	if (pnum >= MAX_PLAYERS || !std::exchange(M.death_orb[pnum], false) || orbs >= nv::HOARD_MAX_ORBS)
		return orbs;
	++M.orbs_created;
	con_printf(CON_VERBOSE, "hoard: P#%u drops an extra orb (%u)", pnum, orbs + 1u);
	return static_cast<uint8_t>(orbs + 1);
#else
	(void)pnum;
	return orbs;
#endif
}

void net_modes_forget_death(const playernum_t pnum)
{
#if DXX_BUILD_DESCENT == 2
	if (pnum < MAX_PLAYERS)
		M.death_orb[pnum] = false;
#else
	(void)pnum;
#endif
}

void net_modes_host_orbs_lost(const unsigned orbs)
{
	M.orbs_lost += orbs;
	con_printf(CON_URGENT, "hoard: %u orbs of a death found no room in the level (%u lost so far)", orbs, M.orbs_lost);
}

void net_modes_receive(const playernum_t, const uint8_t type, const std::span<const uint8_t> payload)
{
#if DXX_BUILD_DESCENT == 2
	if (!network_game() || multi_i_am_master())
		return;
	if (static_cast<session_msg>(type) == session_msg::capture)
		client_receive_capture(payload);
	else if (static_cast<session_msg>(type) == session_msg::orb_bonus)
		client_receive_orb_bonus(payload);
#else
	(void)type;
	(void)payload;
#endif
}

void net_modes_arena_summary()
{
#if DXX_BUILD_DESCENT == 2
	if (game_mode_hoard(Game_mode))
		con_printf(CON_URGENT, "botarena: hoard: %u scores, %u orbs scored, %u extra orbs of deaths, %u lost for want of room; orbs counted in %" PRIu64 " frames, count broken in %" PRIu64,
			M.orb_scores, M.orbs_scored, M.orbs_created, M.orbs_lost, M.orb_frames, M.orb_broken);
	if (!game_mode_capture_flag(Game_mode))
		return;
	con_printf(CON_URGENT, "botarena: ctf: %u captures; flags counted in %" PRIu64 " frames, count broken in %" PRIu64 " (level flags: blue %u, red %u)",
		M.captures, M.census_frames, M.census_broken, M.flags_expected[nv::CTF_TEAM_BLUE], M.flags_expected[nv::CTF_TEAM_RED]);
#endif
}

}

#endif
