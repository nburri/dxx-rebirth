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
 * Capture the flag (Classic), a variant the host chooses: every flag
 * starts in its own team's goal and comes back there after a capture;
 * the host's options send a dropped flag home at once, let the own team
 * return a dropped flag by touching it, and let a team score only while
 * its own flag is at home (net_v2_modes.h, `ctf_rules`).  Every machine
 * puts the level's flags home the same way while the level is prepared;
 * the host does everything else and announces it (OBJ_CREATE, OBJ_REMOVE,
 * CTF_NOTICE).
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

#include <algorithm>
#include <array>
#include <cinttypes>
#include <optional>
#include <span>
#include <vector>
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
#include "hudmsg.h"
#include "digi.h"
#include "sounds.h"
#include "vclip.h"
#include "movement_record.h"
#include "movement_record_format.h"

namespace dsx {

namespace {

namespace nv = ::dcx::net_v2;
namespace mr = ::dcx::movrec;

static_assert(ctf_rule::classic == nv::CTF_RULE_CLASSIC && ctf_rule::dropped_returns == nv::CTF_RULE_DROPPED_RETURNS && ctf_rule::touch_returns == nv::CTF_RULE_TOUCH_RETURNS && ctf_rule::home_to_score == nv::CTF_RULE_HOME_TO_SCORE && ctf_rule::defaults == nv::CTF_RULES_DEFAULT);
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
	/* Capture the flag (Classic): flags returned home, captures refused
	 * for an own flag away, and when each player was last told so.
	 */
	unsigned returns{};
	unsigned refused{};
	std::array<fix64, MAX_PLAYERS> away_told{};
	/* The idle return (nv::idle_flag_returns): the flag object of each
	 * team lying away from home, and since when.
	 */
	struct idle_flag
	{
		objnum_t objnum{object_none};
		object_signature_t signature{};
		fix64 since{};
	};
	std::array<idle_flag, nv::CTF_TEAMS> idle{};
	/* The movement recording's reason of the return under way when no
	 * player returns the flag (mr::flag_return_reason).
	 */
	uint8_t return_reason{mr::flag_return_reason::dropped};
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

[[nodiscard]]
const char *team_title(const uint8_t team)
{
	return team == nv::CTF_TEAM_BLUE ? "Blue" : "Red";
}

[[nodiscard]]
nv::ctf_rules ctf_rules()
{
	if (!game_mode_capture_flag(Game_mode))
		return {};
	return nv::ctf_rules::from_bits(Netgame.CtfClassicFlags);
}

/* The flag's team (the team it belongs to), if the object is a flag. */
[[nodiscard]]
std::optional<uint8_t> flag_team(const object_base &obj)
{
	if (obj.type != object_type::OBJ_POWERUP)
		return std::nullopt;
	switch (get_powerup_id(obj))
	{
		case powerup_type_t::POW_FLAG_BLUE:
			return nv::CTF_TEAM_BLUE;
		case powerup_type_t::POW_FLAG_RED:
			return nv::CTF_TEAM_RED;
		default:
			return std::nullopt;
	}
}

/* The home of team `team`'s flag: its largest goal segment
 * (nv::choose_home), if the level has a goal for the team.
 */
[[nodiscard]]
std::optional<vmsegptridx_t> home_segment(const uint8_t team)
{
	const auto special{team == nv::CTF_TEAM_BLUE ? segment_special::goal_blue : segment_special::goal_red};
	auto &LevelSharedVertexState{LevelSharedSegmentState.get_vertex_state()};
	auto &vcvertptr{LevelSharedVertexState.get_vertices().vcptr};
	std::vector<nv::goal_segment> goals;
	auto &segments{LevelSharedSegmentState.get_segments()};
	for (auto &&seg : segments.vcptridx)
	{
		if (seg->special != special)
			continue;
		const auto center{compute_segment_center(vcvertptr, seg)};
		uint64_t size{};
		for (const auto v : seg->verts)
		{
			const auto &p{*vcvertptr(v)};
			/* In 1/256 units, so that the squares of 8 corners fit. */
			const int64_t dx{(p.x - center.x) >> 8}, dy{(p.y - center.y) >> 8}, dz{(p.z - center.z) >> 8};
			size += static_cast<uint64_t>(dx * dx + dy * dy + dz * dz);
		}
		goals.push_back({static_cast<uint16_t>(seg.get_unchecked_index()), size});
	}
	const auto home{nv::choose_home(goals)};
	if (!home)
		return std::nullopt;
	return segments.vmptridx(segnum_t{*home});
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
	movement_record_mode_event(mr::mode_event_kind::flag_capture, m.pid, m.flag, 0, static_cast<unsigned>(std::max<int>(m.scores.team_score, 0)));
	multi_apply_capture(playernum_t{m.pid}, m.scores.team_score, m.scores.kills, m.scores.kill_goal_count, true);
	if (ctf_rules().classic)
		HUD_init_message(HM_MULTI, "%s team scores!", team_title(m.team));
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
		/* Classic: home; else somewhere away from the carrier. */
		if (nv::flag_respawns_home(ctf_rules()))
			net_modes_host_flag_home(flag, MAX_PLAYERS, false);
		else if (net_drop_powerup_away_from(flag_powerup(flag), pnum) == object_none)
			con_printf(CON_URGENT, "ctf: the %s flag could not be put back; the level inventory will", team_name(flag));
	}
	else
		con_printf(CON_URGENT, "ctf: the %s flag is not put back: the level has all of them", team_name(flag));
	apply_capture(m);
}

/* A flag of team `team` lies at home. */
[[nodiscard]]
bool flag_home(const uint8_t team)
{
	auto &Objects{LevelUniqueObjectState.Objects};
	for (auto &obj : Objects.vcptr)
		if (!(obj.flags & OF_SHOULD_BE_DEAD))
			if (const auto t{flag_team(obj)}; t && *t == team && net_modes_flag_at_home(obj))
				return true;
	return false;
}

void send_notice(const nv::ctf_notice_msg &m, const std::optional<playernum_t> to);
void show_notice(const nv::ctf_notice_msg &m);

/* Host: a flag that lies away from home this long goes home, when no
 * rule would return it otherwise (nv::idle_flag_returns).
 */
void host_idle_flags()
{
	if (!nv::idle_flag_returns(ctf_rules()))
		return;
	auto &Objects{LevelUniqueObjectState.Objects};
	for (const uint8_t team : {nv::CTF_TEAM_BLUE, nv::CTF_TEAM_RED})
	{
		auto &idle{M.idle[team]};
		/* Without a goal the flag is never home: nothing to return. */
		if (!home_segment(team))
		{
			idle = {};
			continue;
		}
		std::optional<vmobjptridx_t> lying;
		for (auto &&objp : Objects.vmptridx)
			if (const auto t{flag_team(objp)}; t && *t == team && !(objp->flags & OF_SHOULD_BE_DEAD) && !net_modes_flag_at_home(objp))
			{
				lying = objp;
				break;
			}
		if (!lying)
		{
			idle = {};
			continue;
		}
		const objnum_t objnum{*lying};
		if (idle.objnum != objnum || idle.signature != (*lying)->signature)
		{
			idle = {objnum, (*lying)->signature, GameTime64};
			continue;
		}
		if (GameTime64 - idle.since < i2f(nv::CTF_IDLE_RETURN_SECONDS))
			continue;
		idle = {};
		con_printf(CON_NORMAL, "ctf: the %s flag lay away from home for %u s", team_name(team), nv::CTF_IDLE_RETURN_SECONDS);
		M.return_reason = mr::flag_return_reason::idle;
		net_modes_host_return_flag(*lying, MAX_PLAYERS);
		M.return_reason = mr::flag_return_reason::dropped;
	}
}

void apply_orb_bonus(const nv::orb_bonus_msg &m)
{
	movement_record_mode_event(mr::mode_event_kind::orb_score, m.pid, mr::PLAYER_NONE, 0, m.orbs);
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
	const auto rules{ctf_rules()};
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
		if (!nv::capture_due(c))
			continue;
		switch (nv::evaluate_capture(rules, c, !rules.home_to_score || flag_home(c.team)))
		{
			case nv::capture_verdict::none:
				break;
			case nv::capture_verdict::score:
				host_capture(i, c.team);
				break;
			case nv::capture_verdict::own_flag_away:
				/* Told once every 3 seconds while it waits there. */
				if (auto &t{M.away_told[i]}; !t || GameTime64 >= t + i2f(3) || GameTime64 < t)
				{
					t = GameTime64;
					++M.refused;
					/* The host records it (its own player's and its bots';
					 * a client records its own when the notice arrives).
					 */
					movement_record_mode_event(mr::mode_event_kind::capture_refused, i, nv::other_team(c.team), 0, 0);
					const nv::ctf_notice_msg m{nv::ctf_notice_kind::own_flag_away, c.team, static_cast<uint8_t>(i)};
					send_notice(m, i);
				}
				break;
		}
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

void show_notice(const nv::ctf_notice_msg &m)
{
	switch (m.kind)
	{
		case nv::ctf_notice_kind::returned:
			if (m.pid < N_players)
				HUD_init_message(HM_MULTI, "%s flag returned by %s", team_title(m.team), static_cast<const char *>(vcplayerptr(playernum_t{m.pid})->callsign));
			else
				HUD_init_message(HM_MULTI, "%s flag returned", team_title(m.team));
			digi_play_sample(sound_effect::SOUND_HUD_MESSAGE, F1_0);
			break;
		case nv::ctf_notice_kind::own_flag_away:
			if (m.pid == Player_num)
			{
				HUD_init_message_literal(HM_MULTI, "Your flag must be home to score");
				digi_play_sample(sound_effect::SOUND_HUD_MESSAGE, F1_0);
			}
			break;
	}
}

/* Host: a notice to everyone (`to` empty) or to one player; the host's
 * own player sees it here.
 */
void send_notice(const nv::ctf_notice_msg &m, const std::optional<playernum_t> to)
{
	std::array<uint8_t, nv::ctf_notice_msg::SIZE> buf;
	m.write(buf);
	if (!to)
		::dsx::net_v2::game_broadcast(static_cast<uint8_t>(session_msg::ctf_notice), buf);
	else if (*to != Player_num && !bot_is_local(*to))
		::dsx::net_v2::game_send_to(*to, static_cast<uint8_t>(session_msg::ctf_notice), buf);
	if (!to || *to == Player_num)
		show_notice(m);
}

void client_receive_notice(const std::span<const uint8_t> payload)
{
	const auto m{nv::ctf_notice_msg::read(payload)};
	if (!m || !game_mode_capture_flag(Game_mode))
		return;
	/* The movement recording: a return, with its reason as far as the
	 * rules tell (the notice does not carry it); a refused capture of
	 * this machine's player.
	 */
	if (m->kind == nv::ctf_notice_kind::returned)
	{
		uint8_t reason{mr::flag_return_reason::touch};
		if (m->pid >= N_players)
		{
			const auto rules{ctf_rules()};
			const bool dropped{nv::dropped_flag_goes_home(rules)}, idle{nv::idle_flag_returns(rules)};
			reason = dropped == idle ? mr::flag_return_reason::other : dropped ? mr::flag_return_reason::dropped : mr::flag_return_reason::idle;
		}
		movement_record_mode_event(mr::mode_event_kind::flag_return, m->pid, m->team, 0, 0, reason);
	}
	else if (m->kind == nv::ctf_notice_kind::own_flag_away && m->pid == Player_num)
		movement_record_mode_event(mr::mode_event_kind::capture_refused, m->pid, nv::other_team(m->team), 0, 0);
	show_notice(*m);
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
		host_idle_flags();
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
	else if (static_cast<session_msg>(type) == session_msg::ctf_notice)
		client_receive_notice(payload);
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
	if (const auto rules{ctf_rules()}; rules.classic)
		con_printf(CON_URGENT, "botarena: ctf classic (dropped flag %s, own team returns %s, score only with own flag home %s): %u flags returned home, %u captures refused for an own flag away",
			rules.dropped_returns ? "returns" : "stays", rules.touch_returns ? "yes" : "no", rules.home_to_score ? "yes" : "no", M.returns, M.refused);
#endif
}

bool net_modes_ctf_classic()
{
#if DXX_BUILD_DESCENT == 2
	return ctf_rules().classic;
#else
	return false;
#endif
}

std::optional<uint16_t> net_modes_home_segment(const uint8_t team)
{
#if DXX_BUILD_DESCENT == 2
	if (team >= nv::CTF_TEAMS)
		return std::nullopt;
	if (const auto home{home_segment(team)})
		return static_cast<uint16_t>(home->get_unchecked_index());
#else
	(void)team;
#endif
	return std::nullopt;
}

void net_modes_record_drop(const playernum_t pnum, const bool had_flag, const std::optional<bool> flag_went_home, const bool had_orbs, const unsigned orbs)
{
#if DXX_BUILD_DESCENT == 2
	if (pnum >= MAX_PLAYERS || pnum >= N_players)
		return;
	if (had_flag && game_mode_capture_flag(Game_mode))
	{
		const bool home{flag_went_home ? *flag_went_home : nv::dropped_flag_goes_home(ctf_rules())};
		movement_record_mode_event(mr::mode_event_kind::flag_drop, pnum, nv::other_team(team_of(pnum)), 0, 0, home ? mr::mode_drop_flag::went_home : 0);
	}
	if (had_orbs && game_mode_hoard(Game_mode))
		movement_record_mode_event(mr::mode_event_kind::orb_drop, pnum, mr::PLAYER_NONE, 0, orbs);
#else
	(void)pnum;
	(void)had_flag;
	(void)flag_went_home;
	(void)had_orbs;
	(void)orbs;
#endif
}

bool net_modes_flag_at_home(const object_base &flag)
{
#if DXX_BUILD_DESCENT == 2
	const auto team{flag_team(flag)};
	if (!team)
		return false;
	auto &vcsegptr{LevelSharedSegmentState.get_segments().vcptr};
	if (const auto s{vcsegptr.check_untrusted(flag.segnum)})
		return goal_team(**s) == team;
#else
	(void)flag;
#endif
	return false;
}

void net_modes_prepare_level_flags()
{
#if DXX_BUILD_DESCENT == 2
	if (!(Game_mode & GM_NETWORK) || !net_modes_ctf_classic())
		return;
	auto &LevelSharedVertexState{LevelSharedSegmentState.get_vertex_state()};
	auto &vcvertptr{LevelSharedVertexState.get_vertices().vcptr};
	auto &Objects{LevelUniqueObjectState.Objects};
	auto &vmobjptr{Objects.vmptr};
	auto &vmsegptr{LevelUniqueSegmentState.get_segments().vmptr};
	for (const uint8_t team : {nv::CTF_TEAM_BLUE, nv::CTF_TEAM_RED})
	{
		const auto home{home_segment(team)};
		if (!home)
		{
			con_printf(CON_NORMAL, "ctf: classic: the level has no %s goal; the %s flag stays where the level has it", team_name(team), team_name(team));
			continue;
		}
		const auto center{compute_segment_center(vcvertptr, *home)};
		unsigned moved{0};
		/* Every machine moves the same objects to the same place (the
		 * level's objects are the same everywhere).
		 */
		for (auto &&objp : Objects.vmptridx)
		{
			if (const auto t{flag_team(objp)}; !t || *t != team)
				continue;
			objp->pos = center;
			objp->mtype.phys_info.velocity = {};
			if (objp->segnum != *home)
				obj_relink(vmobjptr, vmsegptr, objp, *home);
			++moved;
		}
		con_printf(CON_NORMAL, "ctf: classic: %u %s flag%s home in segment %hu", moved, team_name(team), moved == 1 ? "" : "s", static_cast<uint16_t>(*home));
	}
#endif
}

void net_modes_host_flag_home(const uint8_t team, const playernum_t returned_by, const bool returned)
{
#if DXX_BUILD_DESCENT == 2
	if (!multi_i_am_master() || team >= nv::CTF_TEAMS)
		return;
	const auto powerup{flag_powerup(team)};
	const auto home{home_segment(team)};
	if (!home)
	{
		/* No goal for the team: the flag goes where the level inventory
		 * would put it.
		 */
		net_drop_powerup_away_from(powerup, Player_num);
		return;
	}
	auto &LevelSharedVertexState{LevelSharedSegmentState.get_vertex_state()};
	const auto center{compute_segment_center(LevelSharedVertexState.get_vertices().vcptr, *home)};
	Net_create_loc = 0;
	const auto &&objp{drop_powerup(LevelUniqueObjectState, LevelSharedSegmentState, LevelUniqueSegmentState, Vclip, powerup, {}, center, *home, false)};
	Net_create_loc = 0;
	if (objp == object_none)
	{
		con_printf(CON_URGENT, "ctf: the %s flag could not be put home; the level inventory will", team_name(team));
		return;
	}
	/* It stays where it is put. */
	objp->mtype.phys_info.velocity = {};
	objp->pos = center;
	net_objects_announce(objp, 0xff, true);
	object_create_explosion_without_damage(Vclip, *home, center, i2f(5), vclip_index::powerup_disappearance);
	con_printf(CON_NORMAL, "ctf: the %s flag is home%s", team_name(team), returned_by < N_players ? " (returned by a player)" : "");
	if (!returned)
		return;
	++M.returns;
	movement_record_mode_event(mr::mode_event_kind::flag_return, returned_by, team, 0, 0, returned_by < N_players ? mr::flag_return_reason::touch : M.return_reason);
	send_notice({nv::ctf_notice_kind::returned, team, returned_by < N_players ? static_cast<uint8_t>(returned_by) : nv::NET_V2_PLAYER_ID_NONE}, std::nullopt);
#else
	(void)team;
	(void)returned_by;
	(void)returned;
#endif
}

void net_modes_host_return_flag(const vmobjptridx_t flag, const playernum_t pnum)
{
#if DXX_BUILD_DESCENT == 2
	const auto team{flag_team(flag)};
	if (!team || !multi_i_am_master())
		return;
	/* The flag lying away from home goes; the one at home comes. */
	flag->flags |= OF_SHOULD_BE_DEAD;
	if (pnum < N_players)
		con_printf(CON_NORMAL, "ctf: P#%u returns the %s flag", pnum, team_name(*team));
	net_modes_host_flag_home(*team, pnum, true);
#else
	(void)flag;
	(void)pnum;
#endif
}

std::optional<uint8_t> net_modes_host_take_dropped_flag(object &ship, const playernum_t pnum)
{
#if DXX_BUILD_DESCENT == 2
	if (!game_mode_capture_flag(Game_mode) || !nv::dropped_flag_goes_home(ctf_rules()))
		return std::nullopt;
	auto &pi{ship.ctype.player_info};
	if (!(pi.powerup_flags & player_flag::has_team_flag))
		return std::nullopt;
	pi.powerup_flags &= ~player_flag::has_team_flag;
	return nv::other_team(team_of(pnum));
#else
	(void)ship;
	(void)pnum;
	return std::nullopt;
#endif
}

bool net_modes_host_respawn_flag(const powerup_type_t powerup)
{
#if DXX_BUILD_DESCENT == 2
	if (!nv::flag_respawns_home(ctf_rules()))
		return false;
	if (powerup == powerup_type_t::POW_FLAG_BLUE)
		net_modes_host_flag_home(nv::CTF_TEAM_BLUE, MAX_PLAYERS, false);
	else if (powerup == powerup_type_t::POW_FLAG_RED)
		net_modes_host_flag_home(nv::CTF_TEAM_RED, MAX_PLAYERS, false);
	else
		return false;
	return true;
#else
	(void)powerup;
	return false;
#endif
}

}

#endif
