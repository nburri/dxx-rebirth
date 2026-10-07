/*
 * This file is part of the DXX-Rebirth project <https://github.com/dxx-rebirth/dxx-rebirth/>.
 * It is copyright by its individual contributors, as recorded in the
 * project's Git history.  See COPYING.txt at the top level for license
 * terms and a link to the Git history.
 */
/*
 * Team-side spawns (Documentation/network-protocol-v2.md section 8,
 * "Team-side spawns"), the game-independent part:
 *
 * - `assign_sides`: which team's side every start position of a level is
 *   on.  No mark in the level file (the RL2 format has no free field):
 *   a start belongs to the team whose home goal is nearer by path, is
 *   neutral if the two are within 15 % of each other or a home goal is
 *   missing or unreachable; a level without goals splits into two
 *   balanced halves.
 * - `site_tier`, `partition_by_tier`: which start positions a player of a
 *   team may (re)spawn at under the host's rule ("Anywhere", "Own half",
 *   "Own half, away from the flag"), best tier first, falling back to
 *   the next tier when a tier has no free site.
 * - `spawn_score`: how a site ranks for a team player (the distance to
 *   the nearest enemy; a teammate counts only when it sits on the site;
 *   while an enemy carries the own flag, the distance to that carrier).
 * - `assign_start_locations`: the start position of every slot at level
 *   start under the rule.
 *
 * Standard library only (common/unittest/team_spawn.cpp).
 */

#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

namespace dcx::team_spawn {

constexpr std::size_t TEAMS{2};
/* A start position on no team's side (or a slot without a team). */
constexpr std::uint8_t SIDE_NONE{0xff};
constexpr double UNREACHABLE{std::numeric_limits<double>::infinity()};

/* The host's choice: netgame_info::TeamSpawns, GAME_SETTINGS, .ngp
 * (TeamSpawns=).  Only team modes use it.
 */
enum class rule : std::uint8_t
{
	/* As in deathmatch: every start, the most secluded ones first. */
	anywhere,
	/* The starts on the own team's side. */
	own_half,
	/* The starts on the own team's side that are at least
	 * AWAY_DISTANCE from the own home goal by path; else the farthest
	 * own-side start; never in the own flag room while another start is
	 * left.
	 */
	own_half_away,
};
constexpr std::uint8_t RULE_COUNT{3};

[[nodiscard]]
constexpr rule rule_from_byte(const std::uint8_t b)
{
	return b < RULE_COUNT ? static_cast<rule>(b) : rule::anywhere;
}

/* The rule as the menus and the game info name it. */
[[nodiscard]]
constexpr const char *rule_text(const rule r)
{
	switch (r)
	{
		case rule::own_half:
			return "own half";
		case rule::own_half_away:
			return "own half, away from flag";
		case rule::anywhere:
		default:
			return "anywhere";
	}
}

/* The rule a mode starts with when the host selects it: "Own half, away
 * from the flag" for capture the flag (Classic), "Anywhere" otherwise.
 */
[[nodiscard]]
constexpr rule default_rule(const bool ctf_classic)
{
	return ctf_classic ? rule::own_half_away : rule::anywhere;
}

/* Respawn delay (netgame_info::RespawnDelay): 0 to 3 s on top of the
 * classic death sequence.
 */
constexpr std::uint8_t RESPAWN_DELAY_LIMIT{3};

/* "Away from the flag": own starts nearer to the own home goal than this
 * (by path) are left out (the level designers' convention; some 3 s at a
 * Pyro's top speed).
 */
constexpr double AWAY_DISTANCE{200};
/* "In the flag room": closer to the own goal than this by path. */
constexpr double FLAG_ROOM_DISTANCE{60};
/* A teammate this close to a site occupies it (it ranks as if an enemy
 * were there); farther teammates do not count.
 */
constexpr double OCCUPIED_DISTANCE{30};

/* The level's segment graph: node n's edges are
 * edges[first[n] .. first[n + 1]).
 */
struct graph
{
	struct edge
	{
		std::uint32_t to{};
		double cost{};
	};
	std::vector<std::uint32_t> first;
	std::vector<edge> edges;
	[[nodiscard]]
	std::size_t size() const
	{
		return first.empty() ? 0 : first.size() - 1;
	}
	/* A graph of `n` nodes from (from, to, cost) triples in any order;
	 * those with a node out of range are dropped.
	 */
	struct triple
	{
		std::uint32_t from, to;
		double cost;
	};
	[[nodiscard]]
	static graph build(const std::size_t n, std::vector<triple> t)
	{
		std::erase_if(t, [n](const triple &e) { return e.from >= n || e.to >= n; });
		std::ranges::stable_sort(t, {}, &triple::from);
		graph g;
		g.first.assign(n + 1, 0);
		g.edges.reserve(t.size());
		std::size_t i{0};
		for (std::uint32_t node = 0; node < n; ++node)
		{
			g.first[node] = static_cast<std::uint32_t>(g.edges.size());
			for (; i < t.size() && t[i].from == node; ++i)
				g.edges.push_back({t[i].to, t[i].cost});
		}
		g.first[n] = static_cast<std::uint32_t>(g.edges.size());
		return g;
	}
};

/* The path distance from the nearest of `sources` to every node
 * (UNREACHABLE where there is no path).
 */
[[nodiscard]]
inline std::vector<double> distances(const graph &g, const std::span<const std::uint32_t> sources)
{
	const auto n{g.size()};
	std::vector<double> d(n, UNREACHABLE);
	using entry = std::pair<double, std::uint32_t>;
	std::vector<entry> open;
	const auto later = [](const entry &a, const entry &b) {
		return a.first != b.first ? a.first > b.first : a.second > b.second;
	};
	for (const auto s : sources)
		if (s < n && d[s] != 0)
		{
			d[s] = 0;
			open.push_back({0, s});
		}
	std::ranges::make_heap(open, later);
	while (!open.empty())
	{
		std::ranges::pop_heap(open, later);
		const auto [cost, node]{open.back()};
		open.pop_back();
		if (cost != d[node])
			continue;
		for (auto e{g.first[node]}; e != g.first[node + 1]; ++e)
		{
			const auto &edge{g.edges[e]};
			const double c{cost + edge.cost};
			if (c < d[edge.to])
			{
				d[edge.to] = c;
				open.push_back({c, edge.to});
				std::ranges::push_heap(open, later);
			}
		}
	}
	return d;
}

/* How the sides were decided. */
enum class method : std::uint8_t
{
	/* No start position. */
	none,
	/* The nearer home goal (the level has goals). */
	goals,
	/* No goals at all: the two halves around the two starts farthest
	 * apart by path (team anarchy on a level without goals).
	 */
	halves,
};

[[nodiscard]]
constexpr std::string_view method_name(const method m)
{
	switch (m)
	{
		case method::none: return "none";
		case method::goals: return "home goals";
		case method::halves: return "halves";
	}
	return "?";
}

/* Two path distances are "within 15 % of each other": the start is
 * neutral.
 */
constexpr double NEUTRAL_MARGIN{0.15};

[[nodiscard]]
constexpr bool within_margin(const double a, const double b)
{
	const double larger{a > b ? a : b};
	const double diff{a > b ? a - b : b - a};
	return diff <= NEUTRAL_MARGIN * larger;
}

struct sides
{
	/* Per start position: the team whose side it is on (0 blue, 1 red),
	 * SIDE_NONE if it is neutral (either team's).
	 */
	std::vector<std::uint8_t> side;
	/* Per team, per start position: the path distance to the team's
	 * home goal (UNREACHABLE without one or without a path).
	 */
	std::array<std::vector<double>, TEAMS> to_goal;
	method how{method::none};
	[[nodiscard]]
	unsigned count(const std::uint8_t team) const
	{
		return static_cast<unsigned>(std::ranges::count(side, team));
	}
};

namespace detail {

/* How much more start `i` belongs to `team` than to the other team: the
 * smaller, the better a candidate to move to `team` when balancing.
 */
[[nodiscard]]
inline double move_key(const std::array<std::vector<double>, TEAMS> &ref, const std::size_t i, const std::uint8_t team)
{
	const double to_team{ref[team][i]}, to_other{ref[team ^ 1][i]};
	if (to_team == UNREACHABLE)
		return to_other == UNREACHABLE ? 0 : UNREACHABLE;
	if (to_other == UNREACHABLE)
		return -UNREACHABLE;
	return to_team - to_other;
}

}

/* The sides of a level's start positions (the level designers'
 * convention: no mark in the level file, the geometry decides).
 *
 * `start_segments`: the segment of every start position.
 * `home`: per team (blue, red), its home goal segment (the largest goal
 * segment, net_modes.cpp `home_segment`), if the level has goals of the
 * team.
 *
 * If the level has a goal of either team, a start belongs to the team
 * whose home goal is nearer by path distance; it is neutral (either
 * team's) if the two distances are within 15 % of each other, or if a
 * home goal is missing or unreachable.  A level without any goal (team
 * anarchy) is split into two halves instead: the two starts farthest
 * apart by path seed them (the lower numbered one is blue), every start
 * goes to the nearer seed, and the sides are balanced so that each team
 * gets at least half of the starts (rounded down).  Deterministic:
 * every machine computes the same sides.
 */
[[nodiscard]]
inline sides assign_sides(const graph &g, const std::span<const std::uint32_t> start_segments, const std::array<std::optional<std::uint32_t>, TEAMS> &home)
{
	sides r;
	const auto n{start_segments.size()};
	r.side.assign(n, SIDE_NONE);
	for (auto &v : r.to_goal)
		v.assign(n, UNREACHABLE);
	if (!n)
		return r;
	const auto per_start = [&](const std::vector<double> &d) {
		std::vector<double> v(n, UNREACHABLE);
		for (std::size_t i = 0; i < n; ++i)
			if (start_segments[i] < d.size())
				v[i] = d[start_segments[i]];
		return v;
	};
	for (std::size_t t = 0; t < TEAMS; ++t)
		if (home[t])
		{
			const std::uint32_t s[1]{*home[t]};
			r.to_goal[t] = per_start(distances(g, s));
		}
	if (home[0] || home[1])
	{
		r.how = method::goals;
		for (std::size_t i = 0; i < n; ++i)
		{
			const double b{r.to_goal[0][i]}, d{r.to_goal[1][i]};
			if (b == UNREACHABLE || d == UNREACHABLE || within_margin(b, d))
				continue;
			r.side[i] = d < b ? 1 : 0;
		}
		return r;
	}
	r.how = method::halves;
	/* The two starts farthest apart (finite), lowest numbers first
	 * among equals.
	 */
	std::vector<std::vector<double>> from(n);
	for (std::size_t i = 0; i < n; ++i)
	{
		const std::uint32_t s[1]{start_segments[i]};
		from[i] = per_start(distances(g, s));
	}
	std::size_t a{0}, b{n > 1 ? 1u : 0u};
	double best{-1};
	for (std::size_t i = 0; i < n; ++i)
		for (std::size_t j = i + 1; j < n; ++j)
			if (const double d{from[i][j]}; d != UNREACHABLE && d > best)
			{
				best = d;
				a = i;
				b = j;
			}
	const std::array<std::vector<double>, TEAMS> ref{{from[a], from[b]}};
	for (std::size_t i = 0; i < n; ++i)
	{
		if (ref[0][i] == UNREACHABLE && ref[1][i] == UNREACHABLE)
			/* Reaches neither: balancing decides. */
			r.side[i] = static_cast<std::uint8_t>(i & 1);
		else
			r.side[i] = ref[1][i] < ref[0][i] ? 1 : 0;
	}
	/* While a team has fewer than half the starts, the start of the
	 * other team relatively nearest to its seed moves to it.
	 */
	const unsigned half{static_cast<unsigned>(n / 2)};
	for (std::uint8_t t = 0; t < TEAMS; ++t)
		while (r.count(t) < half && r.count(static_cast<std::uint8_t>(t ^ 1)) > half)
		{
			std::optional<std::size_t> pick;
			double pick_key{};
			for (std::size_t i = 0; i < n; ++i)
			{
				if (r.side[i] == t)
					continue;
				const double k{detail::move_key(ref, i, t)};
				if (!pick || k < pick_key)
				{
					pick = i;
					pick_key = k;
				}
			}
			if (!pick)
				break;
			r.side[*pick] = t;
		}
	return r;
}

/* The own-side start of `team` farthest (finite) from the team's home
 * goal, if there is one.
 */
[[nodiscard]]
inline std::optional<std::size_t> farthest_own_start(const sides &s, const std::uint8_t team)
{
	if (team >= TEAMS)
		return std::nullopt;
	std::optional<std::size_t> r;
	for (std::size_t i = 0; i < s.side.size(); ++i)
		if (s.side[i] == team && s.to_goal[team][i] != UNREACHABLE && (!r || s.to_goal[team][i] > s.to_goal[team][*r]))
			r = i;
	return r;
}

/* How good a start position is for a player of `team` under `r` (the
 * lowest tier with a free site wins).  `site_side`: the side of the
 * start (SIDE_NONE: neutral); `to_own_goal`: its path distance to the
 * player's own home goal (UNREACHABLE without one); `farthest_own`: it
 * is the own-side start farthest from the own home goal;
 * `own_goal_known`: the player's team has a home goal (else "away" acts
 * as "own half").
 *
 * Own half: own starts, then neutral ones, then all.  Away from the
 * flag: own starts at least AWAY_DISTANCE from the own home goal, then
 * the farthest own start, the other own starts, the neutral starts, all
 * starts; a start in the own flag room (nearer than FLAG_ROOM_DISTANCE)
 * only if nothing else is left.
 */
constexpr unsigned TIERS{6};

[[nodiscard]]
constexpr unsigned site_tier(const rule r, const std::uint8_t team, const std::uint8_t site_side, const double to_own_goal, const bool farthest_own, const bool own_goal_known)
{
	if (r == rule::anywhere || team >= TEAMS)
		return 0;
	const bool own{site_side == team};
	const bool neutral{site_side >= TEAMS};
	if (r == rule::own_half || !own_goal_known)
		return own ? 0 : neutral ? 1 : 2;
	/* Own half, away from the flag. */
	if (to_own_goal < FLAG_ROOM_DISTANCE)
		return 5;
	if (own && to_own_goal >= AWAY_DISTANCE)
		return 0;
	if (own && farthest_own)
		return 1;
	if (own)
		return 2;
	return neutral ? 3 : 4;
}

/* The tier of every site in `sites` (a span of (site index, score)
 * pairs) through `tier(site index)`; move the sites of the best tier
 * that has a free site (`reserved(site index)` false) to the front,
 * keeping their order, and return their number.  If no site is free,
 * the sites of the best tier (all reserved) are the candidates.
 */
template <typename Site, typename Tier, typename Reserved>
unsigned partition_by_tier(const std::span<Site> sites, Tier &&tier, Reserved &&reserved)
{
	if (sites.empty())
		return 0;
	unsigned best_free{TIERS}, best_any{TIERS};
	for (const auto &s : sites)
	{
		const unsigned t{tier(s.first)};
		if (t < best_any)
			best_any = t;
		if (t < best_free && !reserved(s.first))
			best_free = t;
	}
	const bool any_free{best_free < TIERS};
	const unsigned chosen{any_free ? best_free : best_any};
	const auto end{std::stable_partition(sites.begin(), sites.end(), [&](const Site &s) {
		return tier(s.first) == chosen && (!any_free || !reserved(s.first));
	})};
	return static_cast<unsigned>(std::distance(sites.begin(), end));
}

/* The score of a site for a player of a team (the higher, the better;
 * the draw prefers the highest): `nearest_enemy` and `nearest_teammate`
 * are the path distances to the nearest enemy and teammate ship
 * (UNREACHABLE: none, or no path); `carrier` the distance to the enemy
 * that carries the own flag, if one does and there is a path.
 */
[[nodiscard]]
constexpr double spawn_score(const double nearest_enemy, const double nearest_teammate, const std::optional<double> carrier)
{
	double s{carrier ? *carrier : nearest_enemy};
	if (nearest_teammate < OCCUPIED_DISTANCE && nearest_teammate < s)
		s = nearest_teammate;
	return s;
}

/* The start position of every slot at level start.
 *
 * `slot_team`: per slot, its team (SIDE_NONE: an empty slot, which gets
 * a leftover position).  `tier(team, site)`: as site_tier for a player
 * of `team`.  `sites`: the number of start positions (at least the
 * number of slots).  `shuffle(range)`: shuffles a vector<unsigned> of
 * site numbers (the game's random numbers; the test's).
 *
 * The teams take turns, one player at a time, each taking a random site
 * of its best tier that still has a free site; empty slots get the
 * leftovers in random order.
 */
template <typename Tier, typename Shuffle>
std::vector<unsigned> assign_start_locations(const std::span<const std::uint8_t> slot_team, const unsigned site_count, Tier &&tier, Shuffle &&shuffle)
{
	std::vector<unsigned> result(slot_team.size(), 0);
	std::vector<bool> taken(site_count, false);
	std::vector<bool> placed(slot_team.size(), false);
	/* Per team: its sites ordered by tier, shuffled within a tier. */
	std::array<std::vector<unsigned>, TEAMS> order;
	for (std::uint8_t t = 0; t < TEAMS; ++t)
	{
		std::array<std::vector<unsigned>, TIERS> by_tier;
		for (unsigned s = 0; s < site_count; ++s)
			by_tier[std::min<unsigned>(tier(t, s), TIERS - 1)].push_back(s);
		for (auto &v : by_tier)
		{
			shuffle(v);
			order[t].insert(order[t].end(), v.begin(), v.end());
		}
	}
	/* The members of each team, in random order. */
	std::array<std::vector<unsigned>, TEAMS> members;
	for (unsigned i = 0; i < slot_team.size(); ++i)
		if (slot_team[i] < TEAMS)
			members[slot_team[i]].push_back(i);
	for (auto &m : members)
		shuffle(m);
	std::array<std::size_t, TEAMS> next_member{}, next_site{};
	for (bool progress{true}; progress;)
	{
		progress = false;
		for (std::uint8_t t = 0; t < TEAMS; ++t)
		{
			if (next_member[t] >= members[t].size())
				continue;
			auto &k{next_site[t]};
			while (k < order[t].size() && taken[order[t][k]])
				++k;
			if (k >= order[t].size())
				continue;
			const auto slot{members[t][next_member[t]++]};
			result[slot] = order[t][k];
			taken[order[t][k]] = true;
			placed[slot] = true;
			progress = true;
		}
	}
	std::vector<unsigned> leftover;
	for (unsigned s = 0; s < site_count; ++s)
		if (!taken[s])
			leftover.push_back(s);
	shuffle(leftover);
	std::size_t l{0};
	for (unsigned i = 0; i < slot_team.size(); ++i)
		if (!placed[i] && l < leftover.size())
			result[i] = leftover[l++];
	return result;
}

}
