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
 */

#include "dxxsconf.h"

#if DXX_USE_MULTIPLAYER

#include <array>
#include <cinttypes>
#include <optional>
#include <span>

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

/* What the host knows of player `pnum` for the goal test. */
[[nodiscard]]
nv::capture_check capture_view(const playernum_t pnum)
{
	nv::capture_check c;
	auto &plr{*vcplayerptr(pnum)};
	if (plr.connected != player_connection_status::playing)
		return c;
	auto &Objects{LevelUniqueObjectState.Objects};
	const auto &ship{*Objects.vcptr(plr.objnum)};
	if (ship.type != object_type::OBJ_PLAYER || !net_combat_host_player_alive(pnum))
		return c;
	if (pnum == Player_num && Player_dead_state != player_dead_state::no)
		return c;
	c.alive = true;
	c.team = team_of(pnum);
	c.carries_flag = +(ship.ctype.player_info.powerup_flags & player_flag::has_team_flag);
	if (!c.carries_flag)
		return c;
	/* Where the ship is: the host's own ship and its bots where they
	 * are, a client's ship at its newest accepted position (the ship
	 * object here is shown a little in the past).
	 */
	segnum_t segnum{ship.segnum};
	if (!flown_here(pnum))
	{
		vms_vector pos;
		if (!net_interp_newest_live_position(pnum, pos, segnum))
			return c;
	}
	auto &vcsegptr{LevelSharedSegmentState.get_segments().vcptr};
	if (const auto s{vcsegptr.check_untrusted(segnum)})
		c.goal = goal_team(**s);
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

void host_check_goals()
{
	for (playernum_t i = 0; i < N_players && i < MAX_PLAYERS; ++i)
	{
		const auto c{capture_view(i)};
		if (nv::capture_due(c))
			host_capture(i, c.team);
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
	if (!game_mode_capture_flag(Game_mode))
		return;
	host_check_goals();
	host_census();
#endif
}

void net_modes_receive(const playernum_t, const uint8_t type, const std::span<const uint8_t> payload)
{
#if DXX_BUILD_DESCENT == 2
	if (!network_game() || multi_i_am_master())
		return;
	if (static_cast<session_msg>(type) == session_msg::capture)
		client_receive_capture(payload);
#else
	(void)type;
	(void)payload;
#endif
}

void net_modes_arena_summary()
{
#if DXX_BUILD_DESCENT == 2
	if (!game_mode_capture_flag(Game_mode))
		return;
	con_printf(CON_URGENT, "botarena: ctf: %u captures; flags counted in %" PRIu64 " frames, count broken in %" PRIu64 " (level flags: blue %u, red %u)",
		M.captures, M.census_frames, M.census_broken, M.flags_expected[nv::CTF_TEAM_BLUE], M.flags_expected[nv::CTF_TEAM_RED]);
#endif
}

}

#endif
