/*
 * This file is part of the DXX-Rebirth project <https://github.com/dxx-rebirth/dxx-rebirth/>.
 * It is copyright by its individual contributors, as recorded in the
 * project's Git history.  See COPYING.txt at the top level for license
 * terms and a link to the Git history.
 */
/*
 * Team-side spawns (Documentation/network-protocol-v2.md section 8,
 * "Team-side spawns"), the game's part: the sides of the level's start
 * positions (team_spawn.h, assign_sides) from its segment graph and the
 * teams' home goals, computed on every machine alike when a team game
 * needs them; the tier of a start for a player under the host's rule
 * (Netgame.TeamSpawns); the start positions at level start; the spawn
 * statistics the arena prints.
 */

#include "dxxsconf.h"

#if DXX_USE_MULTIPLAYER

#include <algorithm>
#include <array>
#include <cinttypes>
#include <optional>
#include <random>
#include <span>
#include <vector>

#include "team_spawn.h"
#include "multi.h"
#include "gameseq.h"
#include "gameseg.h"
#include "game.h"
#include "object.h"
#include "player.h"
#include "segment.h"
#include "wall.h"
#include "mission.h"
#include "console.h"
#include "newdemo.h"
#include "d_levelstate.h"

namespace dsx {

namespace {

namespace ts = ::dcx::team_spawn;

struct level_sides
{
	bool valid{};
	int level{};
	const void *mission{};
	std::size_t segments{};
	unsigned starts{};
	per_player_array<uint32_t> start_segment{};
	std::array<std::optional<uint32_t>, ts::TEAMS> home;
	ts::sides sides;
	std::array<std::optional<std::size_t>, ts::TEAMS> farthest_own;
};

level_sides L;

/* Arena statistics (team_spawn_print_stats). */
struct spawn_stats
{
	std::array<std::array<unsigned, 3>, ts::TEAMS> where{};	/* own, neutral, enemy side */
	std::array<double, ts::TEAMS> own_goal_sum{}, enemy_goal_sum{};
	std::array<unsigned, ts::TEAMS> own_goal_n{}, enemy_goal_n{};
	std::array<double, ts::TEAMS> own_goal_least{{ts::UNREACHABLE, ts::UNREACHABLE}};
};

spawn_stats Stats;

/* A ship passes a side: there is a segment behind it and no wall, or an
 * open side, an illusion, a door (any: it opens, or a switch opens it)
 * or a blastable wall (it is shot open).  Solid and cloaked walls block.
 */
[[nodiscard]]
bool side_passable(const shared_segment &seg, const sidenum_t side)
{
	const auto wall_num{seg.sides[side].wall_num};
	if (wall_num == wall_none)
		return true;
	auto &Walls = LevelUniqueWallSubsystemState.Walls;
	const auto type{Walls.vcptr(wall_num)->type};
	return type == WALL_OPEN || type == WALL_ILLUSION || type == WALL_DOOR || type == WALL_BLASTABLE;
}

/* The level's segment graph: centre to side centre to centre. */
[[nodiscard]]
ts::graph level_graph()
{
	auto &LevelSharedVertexState = LevelSharedSegmentState.get_vertex_state();
	auto &vcvertptr = LevelSharedVertexState.get_vertices().vcptr;
	const auto count{static_cast<std::size_t>(Segments.get_count())};
	std::vector<vms_vector> centre(count);
	for (const auto &&segp : vcsegptridx)
		if (const std::size_t n{segp.get_unchecked_index()}; n < count)
			centre[n] = compute_segment_center(vcvertptr, segp);
	std::vector<ts::graph::triple> edges;
	edges.reserve(count * 3);
	for (const auto &&segp : vcsegptridx)
	{
		const uint32_t n{segp.get_unchecked_index()};
		if (n >= count)
			continue;
		for (const auto side : MAX_SIDES_PER_SEGMENT)
		{
			const auto child{segp->shared_segment::children[side]};
			if (!IS_CHILD(child) || child >= count || !side_passable(segp, side))
				continue;
			const auto mid{compute_center_point_on_side(vcvertptr, segp, side)};
			const double cost{f2fl(vm_vec_dist(centre[n], mid)) + f2fl(vm_vec_dist(mid, centre[child]))};
			edges.push_back({n, static_cast<uint32_t>(child), cost});
		}
	}
	return ts::graph::build(count, std::move(edges));
}

[[nodiscard]]
const char *team_word(const uint8_t s)
{
	return s == 0 ? "blue" : s == 1 ? "red" : "neutral";
}

/* The sides of the current level's starts, computed when first needed
 * (and again whenever the level, its starts or the mission change).
 */
const level_sides &sides_now()
{
	const auto starts{std::min<unsigned>(NumNetPlayerPositions, MAX_PLAYERS)};
	const auto segments{static_cast<std::size_t>(Segments.get_count())};
	bool same{L.valid && L.level == Current_level_num && L.mission == Current_mission.get() && L.segments == segments && L.starts == starts};
	for (unsigned i = 0; same && i < starts; ++i)
		same = L.start_segment[i] == static_cast<uint32_t>(Player_init[i].segnum);
	if (same)
		return L;
	L = {};
	L.valid = true;
	L.level = Current_level_num;
	L.mission = Current_mission.get();
	L.segments = segments;
	L.starts = starts;
	for (unsigned i = 0; i < starts; ++i)
		L.start_segment[i] = static_cast<uint32_t>(Player_init[i].segnum);
	for (uint8_t t = 0; t < ts::TEAMS; ++t)
		if (const auto h{net_modes_home_segment(t)})
			L.home[t] = *h;
	const auto g{level_graph()};
	L.sides = ts::assign_sides(g, std::span<const uint32_t>(L.start_segment.data(), starts), L.home);
	for (uint8_t t = 0; t < ts::TEAMS; ++t)
		L.farthest_own[t] = ts::farthest_own_start(L.sides, t);
	con_printf(CON_NORMAL, "team spawns: level %i, %u starts by %s (blue home %d, red home %d): %u blue, %u red, %u neutral",
		Current_level_num, starts, ts::method_name(L.sides.how).data(),
		L.home[0] ? static_cast<int>(*L.home[0]) : -1, L.home[1] ? static_cast<int>(*L.home[1]) : -1,
		L.sides.count(0), L.sides.count(1), starts - L.sides.count(0) - L.sides.count(1));
	for (unsigned i = 0; i < starts; ++i)
	{
		const auto &p{Player_init[i].pos};
		con_printf(CON_NORMAL, "team spawns: start %u (segment %u, x %.0f): %s, %.0f u to the blue home, %.0f u to the red home",
			i, L.start_segment[i], f2fl(p.x), team_word(L.sides.side[i]), L.sides.to_goal[0][i], L.sides.to_goal[1][i]);
	}
	return L;
}

}

ts::rule team_spawn_rule()
{
	if (!(Game_mode & GM_NETWORK) || !(Game_mode & GM_TEAM) || +(Game_mode & GM_MULTI_COOP) || Newdemo_state == ND_STATE_PLAYBACK)
		return ts::rule::anywhere;
	return ts::rule_from_byte(Netgame.TeamSpawns);
}

std::optional<uint8_t> team_spawn_team_of(const playernum_t pnum)
{
	if (!(Game_mode & GM_TEAM) || pnum >= MAX_PLAYERS)
		return std::nullopt;
	return static_cast<uint8_t>(multi_get_team_from_player(Netgame, pnum));
}

unsigned team_spawn_tier(const playernum_t pnum, const unsigned site)
{
	const auto r{team_spawn_rule()};
	if (r == ts::rule::anywhere)
		return 0;
	const auto team{team_spawn_team_of(pnum)};
	if (!team || *team >= ts::TEAMS)
		return 0;
	const auto &l{sides_now()};
	if (site >= l.starts)
		return ts::TIERS - 1;
	return ts::site_tier(r, *team, l.sides.side[site], l.sides.to_goal[*team][site], l.farthest_own[*team] == site, l.home[*team].has_value());
}

bool team_spawn_assign_locations(per_player_array<uint32_t> &locations, const unsigned site_count, const uint32_t seed)
{
	if (team_spawn_rule() == ts::rule::anywhere || site_count > MAX_PLAYERS)
		return false;
	(void)sides_now();
	std::array<uint8_t, MAX_PLAYERS> slot_team;
	slot_team.fill(ts::SIDE_NONE);
	for (playernum_t i = 0; i < N_players && i < site_count; ++i)
		if (vcplayerptr(i)->connected != player_connection_status::disconnected)
			slot_team[i] = *team_spawn_team_of(i);
	std::minstd_rand rng(seed ? seed : 1);
	const auto result{ts::assign_start_locations(std::span<const uint8_t>(slot_team.data(), site_count), site_count, [](const uint8_t team, const unsigned site) {
		const auto &l{L};
		return ts::site_tier(team_spawn_rule(), team, l.sides.side[site], l.sides.to_goal[team][site], l.farthest_own[team] == site, l.home[team].has_value());
	}, [&rng](std::vector<unsigned> &v) {
		std::shuffle(v.begin(), v.end(), rng);
	})};
	for (unsigned i = 0; i < site_count; ++i)
	{
		locations[i] = result[i];
		if (slot_team[i] < ts::TEAMS)
			con_printf(CON_NORMAL, "team spawns: P#%u (%s) starts at %u (%s)", i, team_word(slot_team[i]), result[i], team_word(L.sides.side[result[i]]));
	}
	return true;
}

void team_spawn_note(const playernum_t pnum, const unsigned site)
{
	if (!(Game_mode & GM_TEAM) || !(Game_mode & GM_NETWORK))
		return;
	const auto team{team_spawn_team_of(pnum)};
	if (!team || *team >= ts::TEAMS)
		return;
	const auto &l{sides_now()};
	if (site >= l.starts)
		return;
	const auto t{*team};
	const auto s{l.sides.side[site]};
	++Stats.where[t][s == t ? 0 : s >= ts::TEAMS ? 1 : 2];
	if (const double d{l.sides.to_goal[t][site]}; d != ts::UNREACHABLE)
	{
		Stats.own_goal_sum[t] += d;
		++Stats.own_goal_n[t];
		if (d < Stats.own_goal_least[t])
			Stats.own_goal_least[t] = d;
	}
	if (const double d{l.sides.to_goal[t ^ 1][site]}; d != ts::UNREACHABLE)
	{
		Stats.enemy_goal_sum[t] += d;
		++Stats.enemy_goal_n[t];
	}
	con_printf(CON_VERBOSE, "team spawns: P#%u (%s) spawns at %u (%s side, %.0f u to its home)", pnum, team_word(t), site, team_word(s), l.sides.to_goal[t][site]);
}

void team_spawn_level_start()
{
	L.valid = false;
}

void team_spawn_print_stats()
{
	if (!(Game_mode & GM_TEAM))
		return;
	/* "Time to own base": the path to the own home goal at a Pyro's top
	 * speed (some 60 units per second).
	 */
	constexpr double pyro_speed{60};
	con_printf(CON_URGENT, "botarena: spawns: rule %u, respawn delay %u s", static_cast<unsigned>(Netgame.TeamSpawns), static_cast<unsigned>(Netgame.RespawnDelay));
	for (uint8_t t = 0; t < ts::TEAMS; ++t)
	{
		const auto &w{Stats.where[t]};
		const unsigned n{w[0] + w[1] + w[2]};
		const double own{Stats.own_goal_n[t] ? Stats.own_goal_sum[t] / Stats.own_goal_n[t] : 0};
		const double enemy{Stats.enemy_goal_n[t] ? Stats.enemy_goal_sum[t] / Stats.enemy_goal_n[t] : 0};
		con_printf(CON_URGENT, "botarena: spawns: %-4s %u: %u own side, %u neutral, %u enemy side; own home %.0f u mean (%.1f s), %.0f u nearest; enemy home %.0f u mean (%.1f s)",
			team_word(t), n, w[0], w[1], w[2], own, own / pyro_speed, Stats.own_goal_least[t] == ts::UNREACHABLE ? 0 : Stats.own_goal_least[t], enemy, enemy / pyro_speed);
	}
}

}

#endif
