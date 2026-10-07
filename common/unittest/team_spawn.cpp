/*
 * This file is part of the DXX-Rebirth project <https://github.com/dxx-rebirth/dxx-rebirth/>.
 * It is copyright by its individual contributors, as recorded in the
 * project's Git history.  See COPYING.txt at the top level for license
 * terms and a link to the Git history.
 */
/*
 * Test of team-side spawns (team_spawn.h): the sides of start positions
 * on synthetic levels (two bases, neutral starts, a missing or
 * unreachable goal, no goals with even and uneven start counts), the
 * tiers of the three rules (own half, away from the flag with its
 * distance filter and fallbacks), the choice among tiers with
 * reservations, the score with the flag carrier rule, and the level
 * start placement.
 *
 * Build and run with SCons:
 *
 *	scons sdl2=1 d1x=0 d2x=1 register_runtime_test_plain_link_targets=1 test-team-spawn
 *	build/common/test-team-spawn
 *
 * or directly:
 *
 *	g++ -std=gnu++23 -O2 -Wall -Wextra -Icommon/main common/unittest/team_spawn.cpp -o test-team-spawn
 */

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <set>
#include <vector>

#include "team_spawn.h"
#include "spawn_site.h"

using namespace dcx::team_spawn;

namespace {

void check_failed(const char *const expr, const char *const file, const int line)
{
	std::fprintf(stderr, "%s:%d: check failed: %s\n", file, line, expr);
	std::exit(1);
}

#define CHECK(cond)	do { if (!(cond)) check_failed(#cond, __FILE__, __LINE__); } while (0)

constexpr std::uint8_t BLUE{0}, RED{1};

using home_goals = std::array<std::optional<std::uint32_t>, TEAMS>;

home_goals homes(const std::optional<std::uint32_t> blue, const std::optional<std::uint32_t> red)
{
	return {{blue, red}};
}

/* A corridor of `n` segments 0 .. n-1, each `step` units long (both
 * directions), plus extra edges.
 */
graph corridor(const std::uint32_t n, const double step, std::vector<graph::triple> extra = {})
{
	std::vector<graph::triple> t{std::move(extra)};
	for (std::uint32_t i = 0; i + 1 < n; ++i)
	{
		t.push_back({i, i + 1, step});
		t.push_back({i + 1, i, step});
	}
	return graph::build(n, std::move(t));
}

void test_graph()
{
	/* Edges in any order; out-of-range edges are dropped. */
	const auto g{graph::build(4, {{2, 3, 1}, {0, 1, 5}, {1, 2, 2}, {9, 0, 1}, {0, 2, 20}})};
	CHECK(g.size() == 4);
	CHECK(g.edges.size() == 4);
	const std::uint32_t s[1]{0};
	const auto d{distances(g, s)};
	CHECK(d[0] == 0);
	CHECK(d[1] == 5);
	CHECK(d[2] == 7);
	CHECK(d[3] == 8);
	/* One-way: nothing leads back to 0. */
	const std::uint32_t s3[1]{3};
	const auto back{distances(g, s3)};
	CHECK(back[0] == UNREACHABLE);
	CHECK(back[3] == 0);
	/* Several sources: the nearest. */
	const auto c{corridor(10, 10)};
	const std::uint32_t two[2]{0, 9};
	const auto d2{distances(c, two)};
	CHECK(d2[4] == 40);
	CHECK(d2[5] == 40);
	CHECK(d2[8] == 10);
}

/* A symmetric two-base level: blue home at segment 0, red home at 40,
 * a corridor of 10-unit segments; starts on both sides and in the
 * middle.
 */
void test_two_bases()
{
	const auto g{corridor(41, 10)};
	const std::vector<std::uint32_t> starts{3, 5, 25, 37, 35, 15, 20, 19};
	const auto s{assign_sides(g, starts, homes(0, 40))};
	CHECK(s.how == method::goals);
	CHECK(s.side[0] == BLUE);	/* 30 vs 370 */
	CHECK(s.side[1] == BLUE);
	CHECK(s.side[2] == RED);	/* 250 vs 150 */
	CHECK(s.side[3] == RED);
	CHECK(s.side[4] == RED);
	CHECK(s.side[5] == BLUE);	/* 150 vs 250 */
	CHECK(s.side[6] == SIDE_NONE);	/* 200 vs 200: neutral */
	CHECK(s.side[7] == SIDE_NONE);	/* 190 vs 210: within 15 % */
	CHECK(s.to_goal[BLUE][0] == 30);
	CHECK(s.to_goal[RED][0] == 370);
	CHECK(s.count(BLUE) == 3);
	CHECK(s.count(RED) == 3);
	/* The 15 % margin, exactly. */
	CHECK(within_margin(85, 100));
	CHECK(!within_margin(84, 100));
	CHECK(within_margin(100, 85));
	CHECK(!within_margin(100, 84));
	/* Segment 17: 170 vs 230, 60 > 34.5: blue; 18: 180 vs 220, 40 > 33: blue;
	 * 19: 190 vs 210, 20 <= 31.5: neutral.
	 */
	const std::vector<std::uint32_t> edge{17, 18, 19, 21, 22, 23};
	const auto e{assign_sides(g, edge, homes(0, 40))};
	CHECK(e.side[0] == BLUE);
	CHECK(e.side[1] == BLUE);
	CHECK(e.side[2] == SIDE_NONE);
	CHECK(e.side[3] == SIDE_NONE);
	CHECK(e.side[4] == RED);
	CHECK(e.side[5] == RED);
	/* The farthest own start from the own home. */
	CHECK(farthest_own_start(s, BLUE) == 5u);
	CHECK(farthest_own_start(s, RED) == 2u);
}

/* Path distance, not straight distance: a start near the blue goal in
 * space but behind a long detour belongs to red.
 */
void test_path_distance()
{
	/* 0 blue home, 1..10 corridor to 11 red home (10 units each); the
	 * start segment 12 hangs off segment 10 (near red by path).
	 */
	std::vector<graph::triple> t{{10, 12, 10}, {12, 10, 10}};
	for (std::uint32_t i = 0; i + 1 < 12; ++i)
	{
		t.push_back({i, i + 1, 10});
		t.push_back({i + 1, i, 10});
	}
	const auto g{graph::build(13, std::move(t))};
	const std::vector<std::uint32_t> starts{12};
	const auto s{assign_sides(g, starts, homes(0, 11))};
	CHECK(s.side[0] == RED);
	CHECK(s.to_goal[RED][0] == 20);
	CHECK(s.to_goal[BLUE][0] == 110);
}

void test_missing_or_unreachable_goal()
{
	const auto g{corridor(41, 10)};
	const std::vector<std::uint32_t> starts{3, 37};
	/* Only blue has a goal: every start is neutral. */
	const auto s{assign_sides(g, starts, homes(0, std::nullopt))};
	CHECK(s.how == method::goals);
	CHECK(s.side[0] == SIDE_NONE);
	CHECK(s.side[1] == SIDE_NONE);
	CHECK(s.to_goal[BLUE][0] == 30);
	CHECK(s.to_goal[RED][0] == UNREACHABLE);
	/* A start in a sealed part of the level: neutral; the others as
	 * usual.
	 */
	const auto sealed{graph::build(43, [] {
		std::vector<graph::triple> v;
		for (std::uint32_t i = 0; i + 1 < 41; ++i)
		{
			v.push_back({i, i + 1, 10});
			v.push_back({i + 1, i, 10});
		}
		v.push_back({41, 42, 10});
		v.push_back({42, 41, 10});
		return v;
	}())};
	const std::vector<std::uint32_t> st{3, 42, 37};
	const auto u{assign_sides(sealed, st, homes(0, 40))};
	CHECK(u.side[0] == BLUE);
	CHECK(u.side[1] == SIDE_NONE);
	CHECK(u.side[2] == RED);
}

/* No goals at all (team anarchy): halves around the two starts
 * farthest apart, balanced.
 */
void test_no_goals()
{
	const auto g{corridor(41, 10)};
	{
		const std::vector<std::uint32_t> starts{2, 4, 6, 8, 32, 34, 36, 38};
		const auto s{assign_sides(g, starts, {})};
		CHECK(s.how == method::halves);
		for (unsigned i = 0; i < 4; ++i)
			CHECK(s.side[i] == BLUE);
		for (unsigned i = 4; i < 8; ++i)
			CHECK(s.side[i] == RED);
	}
	{
		/* Uneven: six starts near one end, one at the other.  Each team
		 * still gets at least half (3 of 7); the moved starts are those
		 * nearest the lone seed.
		 */
		const std::vector<std::uint32_t> starts{0, 2, 4, 6, 8, 10, 40};
		const auto s{assign_sides(g, starts, {})};
		CHECK(s.how == method::halves);
		CHECK(s.count(BLUE) >= 3);
		CHECK(s.count(RED) >= 3);
		CHECK(s.count(BLUE) + s.count(RED) == 7);
		CHECK(s.side[6] == RED);	/* a seed */
		CHECK(s.side[0] == BLUE);	/* the other seed */
		CHECK(s.side[5] == RED);	/* nearest to the red seed */
		CHECK(s.side[4] == RED);
		CHECK(s.side[1] == BLUE);
	}
	{
		/* Odd count, symmetric: the seeds 0 and 40, the middle start
		 * (equal distances) blue: 3 + 2.
		 */
		const std::vector<std::uint32_t> starts{0, 10, 20, 30, 40};
		const auto s{assign_sides(g, starts, {})};
		CHECK(s.side == (std::vector<std::uint8_t>{BLUE, BLUE, BLUE, RED, RED}));
	}
	{
		/* One start: nothing to split. */
		const std::vector<std::uint32_t> starts{5};
		const auto s{assign_sides(g, starts, {})};
		CHECK(s.side.size() == 1);
	}
	/* Deterministic. */
	const std::vector<std::uint32_t> starts{1, 9, 17, 25, 33, 39, 12, 28};
	const auto a{assign_sides(g, starts, {})};
	const auto b{assign_sides(g, starts, {})};
	CHECK(a.side == b.side);
}

void test_tiers()
{
	/* Anywhere and non-team players: one tier. */
	CHECK(site_tier(rule::anywhere, BLUE, RED, 10, false, true) == 0);
	CHECK(site_tier(rule::own_half, SIDE_NONE, RED, 10, false, true) == 0);
	/* Own half: own, neutral, the rest. */
	CHECK(site_tier(rule::own_half, BLUE, BLUE, 10, false, true) == 0);
	CHECK(site_tier(rule::own_half, BLUE, SIDE_NONE, 10, false, true) == 1);
	CHECK(site_tier(rule::own_half, BLUE, RED, 10, false, true) == 2);
	/* Away from the flag: the distance filter (200 u). */
	CHECK(site_tier(rule::own_half_away, BLUE, BLUE, 250, false, true) == 0);
	CHECK(site_tier(rule::own_half_away, BLUE, BLUE, 200, false, true) == 0);
	CHECK(site_tier(rule::own_half_away, BLUE, BLUE, 199, false, true) == 2);
	CHECK(site_tier(rule::own_half_away, BLUE, BLUE, 150, true, true) == 1);
	CHECK(site_tier(rule::own_half_away, BLUE, SIDE_NONE, 300, false, true) == 3);
	CHECK(site_tier(rule::own_half_away, BLUE, RED, 300, false, true) == 4);
	/* Never in the own flag room while another site is left. */
	CHECK(site_tier(rule::own_half_away, BLUE, BLUE, 30, true, true) == 5);
	CHECK(site_tier(rule::own_half_away, BLUE, RED, 30, false, true) == 5);
	/* Without an own home goal, away acts as own half. */
	CHECK(site_tier(rule::own_half_away, BLUE, BLUE, UNREACHABLE, false, false) == 0);
	CHECK(site_tier(rule::own_half_away, BLUE, RED, UNREACHABLE, false, false) == 2);
	CHECK(rule_from_byte(2) == rule::own_half_away);
	CHECK(rule_from_byte(7) == rule::anywhere);
	CHECK(default_rule(true) == rule::own_half_away);
	CHECK(default_rule(false) == rule::anywhere);
}

using site = std::pair<int, double>;

/* The distance filter and fallbacks on a level: blue home 0, red home
 * 40 (10 u segments).
 */
void test_away_choice()
{
	const auto g{corridor(41, 10)};
	/* Blue starts at 30, 100, 150, 250 u from the blue home... */
	const std::vector<std::uint32_t> starts{3, 10, 15, 25, 38, 36, 30, 20};
	const auto s{assign_sides(g, starts, homes(0, 40))};
	/* 25: 250 vs 150 -> red.  Blue: 3, 10, 15; red: 25, 38, 36, 30;
	 * 20 neutral.
	 */
	CHECK(s.side[0] == BLUE && s.side[1] == BLUE && s.side[2] == BLUE);
	CHECK(s.side[3] == RED && s.side[7] == SIDE_NONE);
	const auto tiers_for = [&](const std::uint8_t team, const rule r) {
		const auto far_own{farthest_own_start(s, team)};
		return [&s, team, r, far_own](const int i) {
			const auto u{static_cast<std::size_t>(i)};
			return site_tier(r, team, s.side[u], s.to_goal[team][u], far_own == u, true);
		};
	};
	std::vector<site> sites;
	for (unsigned i = 0; i < starts.size(); ++i)
		sites.push_back({static_cast<int>(i), 1000 - i});
	const auto no_reservation = [](int) { return false; };
	{
		/* Blue, away: no own start is 200 u from home (30, 100, 150):
		 * the farthest own start (15, 150 u).
		 */
		auto v{sites};
		const auto n{partition_by_tier(std::span<site>(v), tiers_for(BLUE, rule::own_half_away), no_reservation)};
		CHECK(n == 1);
		CHECK(v[0].first == 2);
		/* It is reserved: the other own starts outside the flag room
		 * (10, 100 u); not 3 (30 u, in the flag room).
		 */
		const auto reserved = [](const int i) { return i == 2; };
		auto w{sites};
		const auto m{partition_by_tier(std::span<site>(w), tiers_for(BLUE, rule::own_half_away), reserved)};
		CHECK(m == 1);
		CHECK(w[0].first == 1);
		/* All own starts reserved except the flag room one: neutral. */
		const auto busy = [](const int i) { return i == 1 || i == 2; };
		auto x{sites};
		const auto k{partition_by_tier(std::span<site>(x), tiers_for(BLUE, rule::own_half_away), busy)};
		CHECK(k == 1);
		CHECK(x[0].first == 7);
		/* Everything else reserved too: the enemy side, never the own
		 * flag room.
		 */
		const auto most = [](const int i) { return i != 0 && i != 3 && i != 6; };
		auto y{sites};
		const auto j{partition_by_tier(std::span<site>(y), tiers_for(BLUE, rule::own_half_away), most)};
		CHECK(j == 2);
		CHECK(y[0].first == 3 && y[1].first == 6);
		/* Every site reserved: the best tier, reserved. */
		const auto all = [](int) { return true; };
		auto z{sites};
		CHECK(partition_by_tier(std::span<site>(z), tiers_for(BLUE, rule::own_half_away), all) == 1);
		CHECK(z[0].first == 2);
	}
	{
		/* Red, away: own starts 250 (25: 150 u from red home? no:
		 * red home is 40, so 25 is 150 u, 30 is 100 u, 36 is 40 u,
		 * 38 is 20 u).  None is 200 u away: the farthest (25).
		 */
		auto v{sites};
		const auto n{partition_by_tier(std::span<site>(v), tiers_for(RED, rule::own_half_away), no_reservation)};
		CHECK(n == 1);
		CHECK(v[0].first == 3);
	}
	{
		/* Own half: every own start, flag room included. */
		auto v{sites};
		const auto n{partition_by_tier(std::span<site>(v), tiers_for(BLUE, rule::own_half), no_reservation)};
		CHECK(n == 3);
		std::set<int> got;
		for (unsigned i = 0; i < n; ++i)
			got.insert(v[i].first);
		CHECK(got == (std::set<int>{0, 1, 2}));
		/* Order kept within the tier. */
		CHECK(v[0].first == 0 && v[1].first == 1 && v[2].first == 2);
	}
	{
		/* A level where blue has starts 250 u away: those only. */
		const auto h{corridor(61, 10)};
		const std::vector<std::uint32_t> st{25, 22, 5, 55};
		const auto t{assign_sides(h, st, homes(0, 60))};
		CHECK(t.side[0] == BLUE && t.side[1] == BLUE && t.side[2] == BLUE && t.side[3] == RED);
		const auto far_own{farthest_own_start(t, BLUE)};
		std::vector<site> v{{0, 1}, {1, 1}, {2, 1}, {3, 1}};
		const auto n{partition_by_tier(std::span<site>(v), [&](const int i) {
			const auto u{static_cast<std::size_t>(i)};
			return site_tier(rule::own_half_away, BLUE, t.side[u], t.to_goal[BLUE][u], far_own == u, true);
		}, no_reservation)};
		CHECK(n == 2);
		CHECK(v[0].first == 0 && v[1].first == 1);
	}
	/* A team without own starts: the neutral ones, then all. */
	{
		std::vector<std::uint8_t> side{RED, RED, SIDE_NONE, RED};
		std::vector<site> v{{0, 1}, {1, 1}, {2, 1}, {3, 1}};
		const auto n{partition_by_tier(std::span<site>(v), [&](const int i) {
			return site_tier(rule::own_half, BLUE, side[static_cast<std::size_t>(i)], 500, false, true);
		}, no_reservation)};
		CHECK(n == 1 && v[0].first == 2);
		std::vector<site> w{{0, 1}, {1, 1}, {2, 1}, {3, 1}};
		const auto m{partition_by_tier(std::span<site>(w), [&](const int i) {
			return site_tier(rule::own_half, BLUE, side[static_cast<std::size_t>(i)], 500, false, true);
		}, [](const int i) { return i == 2; })};
		CHECK(m == 3);
	}
}

/* The score and the carrier rule. */
void test_score()
{
	/* Nearest enemy, teammates ignored unless on the site. */
	CHECK(spawn_score(300, 100, std::nullopt) == 300);
	CHECK(spawn_score(300, 10, std::nullopt) == 10);
	CHECK(spawn_score(UNREACHABLE, UNREACHABLE, std::nullopt) == UNREACHABLE);
	/* An enemy carries the own flag: the distance to the carrier, not
	 * to the nearest enemy.
	 */
	CHECK(spawn_score(50, 500, 400.0) == 400);
	CHECK(spawn_score(500, 500, 40.0) == 40);
	/* The ranking then puts the sites farthest from the carrier first. */
	const double carrier_at{120};
	const std::array<double, 5> site_pos{{0, 100, 150, 300, 400}};
	std::vector<std::pair<int, double>> sites;
	for (unsigned i = 0; i < site_pos.size(); ++i)
	{
		const double d{site_pos[i] > carrier_at ? site_pos[i] - carrier_at : carrier_at - site_pos[i]};
		/* The nearest enemy (another one) sits at 380. */
		const double e{site_pos[i] > 380 ? site_pos[i] - 380 : 380 - site_pos[i]};
		sites.push_back({static_cast<int>(i), spawn_score(e, UNREACHABLE, d)});
	}
	const auto usable{dcx::rank_secluded_spawn_sites(std::span<std::pair<int, double>>(sites), 2)};
	CHECK(usable == 2);
	CHECK(sites[0].first == 4);	/* 280 from the carrier */
	CHECK(sites[1].first == 3);	/* 180 */
	/* Without the carrier rule both enemies count (the carrier at 120,
	 * the other at 380): site 0 (120 from the nearer) first.
	 */
	std::vector<std::pair<int, double>> plain;
	for (unsigned i = 0; i < site_pos.size(); ++i)
	{
		const double e{site_pos[i] > 380 ? site_pos[i] - 380 : 380 - site_pos[i]};
		const double c{site_pos[i] > carrier_at ? site_pos[i] - carrier_at : carrier_at - site_pos[i]};
		plain.push_back({static_cast<int>(i), spawn_score(e < c ? e : c, UNREACHABLE, std::nullopt)});
	}
	(void)dcx::rank_secluded_spawn_sites(std::span<std::pair<int, double>>(plain), 2);
	CHECK(plain[0].first == 0);
}

/* Level start: each team on its own side, all sites distinct. */
void test_start_locations()
{
	const std::vector<std::uint8_t> site_side{BLUE, BLUE, BLUE, BLUE, RED, RED, RED, RED};
	const auto tier = [&](const std::uint8_t team, const unsigned s) {
		return site_tier(rule::own_half, team, site_side[s], 500, false, true);
	};
	for (unsigned seed = 1; seed <= 200; ++seed)
	{
		std::minstd_rand rng(seed);
		const auto shuffle = [&rng](std::vector<unsigned> &v) { std::shuffle(v.begin(), v.end(), rng); };
		/* 4 v 4. */
		const std::vector<std::uint8_t> teams{BLUE, RED, BLUE, RED, BLUE, RED, BLUE, RED};
		const auto loc{assign_start_locations(teams, 8, tier, shuffle)};
		std::set<unsigned> used(loc.begin(), loc.end());
		CHECK(used.size() == 8);
		for (unsigned i = 0; i < 8; ++i)
			CHECK(site_side[loc[i]] == teams[i]);
		/* 5 v 2 with one empty slot: the fifth blue player takes a red
		 * site (no own site left), every site distinct.
		 */
		const std::vector<std::uint8_t> uneven{BLUE, BLUE, RED, BLUE, BLUE, RED, BLUE, SIDE_NONE};
		const auto u{assign_start_locations(uneven, 8, tier, shuffle)};
		std::set<unsigned> uu(u.begin(), u.end());
		CHECK(uu.size() == 8);
		unsigned blue_on_red{0};
		for (unsigned i = 0; i < 8; ++i)
		{
			if (uneven[i] == RED)
				CHECK(site_side[u[i]] == RED);
			if (uneven[i] == BLUE && site_side[u[i]] == RED)
				++blue_on_red;
		}
		CHECK(blue_on_red == 1);
	}
	/* Anywhere: a plain permutation. */
	std::minstd_rand rng(7);
	const auto shuffle = [&rng](std::vector<unsigned> &v) { std::shuffle(v.begin(), v.end(), rng); };
	const std::vector<std::uint8_t> teams{BLUE, RED, BLUE, RED, SIDE_NONE, SIDE_NONE};
	const auto loc{assign_start_locations(teams, 6, [](std::uint8_t, unsigned) { return 0u; }, shuffle)};
	std::set<unsigned> used(loc.begin(), loc.end());
	CHECK(used.size() == 6);
	for (const auto l : loc)
		CHECK(l < 6);
}

}

int main()
{
	test_graph();
	test_two_bases();
	test_path_distance();
	test_missing_or_unreachable_goal();
	test_no_goals();
	test_tiers();
	test_away_choice();
	test_score();
	test_start_locations();
	std::puts("test-team-spawn: all checks passed");
	return 0;
}
