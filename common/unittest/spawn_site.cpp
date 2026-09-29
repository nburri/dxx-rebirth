/*
 * This file is part of the DXX-Rebirth project <https://github.com/dxx-rebirth/dxx-rebirth/>.
 * It is copyright by its individual contributors, as recorded in the
 * project's Git history.  See COPYING.txt at the top level for license
 * terms and a link to the Git history.
 */
/*
 * Test of the deathmatch spawn site choice (spawn_site.h), extracted
 * from InitPlayerPosition for the choose_spawn / place_player split
 * (Documentation/multiplayer-bots.md section 3.2).  The extraction must
 * not change the game: the test compares the extracted functions with a
 * verbatim copy of the code they replaced, over many random site
 * layouts, and checks the ranking, the chosen site and the number of
 * random numbers drawn (which decides every later d_rand result).
 *
 * It also checks the host's assignment in a network deathmatch: the
 * sites assigned recently count as ships and are left out while a free
 * one remains, so that many simultaneous requests get distinct sites
 * while there are enough, and more requests than sites get the sites
 * farthest from the others.
 *
 * Build and run with SCons:
 *
 *	scons sdl2=1 d1x=0 d2x=1 register_runtime_test_plain_link_targets=1 test-spawn-site
 *	build/common/test-spawn-site
 *
 * or directly:
 *
 *	g++ -std=gnu++23 -O2 -Wall -Wextra -Icommon/main common/unittest/spawn_site.cpp -o test-spawn-site
 */

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <climits>
#include <cstdlib>
#include <random>
#include <set>
#include <utility>
#include <vector>

#include "spawn_site.h"

using namespace dcx;

namespace {

void check_failed(const char *const expr, const char *const file, const int line)
{
	std::fprintf(stderr, "%s:%d: check failed: %s\n", file, line, expr);
	std::exit(1);
}

#define CHECK(cond)	do { if (!(cond)) check_failed(#cond, __FILE__, __LINE__); } while (0)

using fix = int32_t;
constexpr unsigned MAX_PLAYERS{8};
constexpr fix min_distance{(15 * 20) << 16};	// i2f(15*20)
using site = std::pair<int, fix>;
using site_array = std::array<site, MAX_PLAYERS>;

/* d_rand: 0 .. 0x7fff, counting the draws. */
struct counting_rand
{
	uint32_t state;
	unsigned draws{0};
	int operator()()
	{
		++draws;
		state = state * 1103515245u + 12345u;
		return (state >> 16) & 0x7fff;
	}
};

/* The code before the split (respawn_locations and InitPlayerPosition in
 * gameseq.cpp), verbatim but for the types.
 */
unsigned reference_rank(site_array &sites, const unsigned max_spawn_sites, const unsigned SecludedSpawns)
{
	if (max_spawn_sites > SecludedSpawns)
	{
		const auto &&predicate = [](const site &a, const site &b) {
			return a.second > b.second;
		};
		const auto b = sites.begin();
		const auto m = std::next(b, SecludedSpawns);
		const auto e = std::next(b, max_spawn_sites);
		std::partial_sort(b, m, e, predicate);
		return SecludedSpawns;
	}
	return max_spawn_sites;
}

int reference_pick(const site_array &locations, const unsigned usable, counting_rand &d_rand)
{
	int NewPlayer;
	uint_fast32_t trys{0};
	do {
		trys++;
		NewPlayer = d_rand() % usable;
		const auto closest_dist = locations[NewPlayer].second;
		if (closest_dist >= min_distance)
			break;
	} while (trys < MAX_PLAYERS * 2);
	return locations[NewPlayer].first;
}

void test_against_reference()
{
	std::mt19937 gen{12345};
	std::uniform_int_distribution<unsigned> sites_dist{0, MAX_PLAYERS};
	std::uniform_int_distribution<unsigned> secluded_dist{1, MAX_PLAYERS};
	std::uniform_int_distribution<fix> dist_dist{0, 700 << 16};
	std::uniform_int_distribution<int> kind{0, 9};
	unsigned picks{0};
	for (unsigned iteration = 0; iteration < 200000; ++iteration)
	{
		const unsigned max_spawn_sites{sites_dist(gen)};
		const unsigned secluded{secluded_dist(gen)};
		site_array a{}, b{};
		for (unsigned i = 0; i < max_spawn_sites; ++i)
		{
			/* Mix far, near, equal and unreachable (INT32_MAX) distances. */
			const auto k{kind(gen)};
			const fix d{k == 0 ? INT32_MAX : k == 1 ? min_distance : k == 2 ? (100 << 16) : dist_dist(gen)};
			a[i] = b[i] = {static_cast<int>(i), d};
		}
		const auto usable_ref{reference_rank(a, max_spawn_sites, secluded)};
		const auto usable{rank_secluded_spawn_sites(std::span<site>(b.data(), max_spawn_sites), secluded)};
		CHECK(usable == usable_ref);
		CHECK(a == b);
		if (!usable)
			continue;
		const uint32_t seed{static_cast<uint32_t>(gen())};
		counting_rand r_ref{seed}, r{seed};
		const auto chosen_ref{reference_pick(a, usable_ref, r_ref)};
		const auto chosen{pick_spawn_site(std::span<const site>(b.data(), usable), usable, min_distance, MAX_PLAYERS * 2, r)};
		CHECK(chosen == chosen_ref);
		CHECK(r.draws == r_ref.draws);
		CHECK(r.state == r_ref.state);
		++picks;
	}
	CHECK(picks > 100000);
}

void test_examples()
{
	/* Fewer sites than secluded: the order is kept. */
	{
		site_array s{{{0, 5}, {1, 9}, {2, 1}}};
		CHECK(rank_secluded_spawn_sites(std::span<site>(s.data(), 3), 3) == 3);
		CHECK(s[0].first == 0 && s[1].first == 1 && s[2].first == 2);
	}
	/* The two farthest go to the front, farthest first. */
	{
		site_array s{{{0, 5}, {1, 9}, {2, 1}, {3, 7}}};
		CHECK(rank_secluded_spawn_sites(std::span<site>(s.data(), 4), 2) == 2);
		CHECK(s[0].first == 1 && s[1].first == 3);
	}
	/* Every site is too close: exactly max_tries draws, the last one wins. */
	{
		const site_array s{{{4, 0}, {6, 0}}};
		unsigned draws{0};
		const auto chosen{pick_spawn_site(std::span<const site>(s.data(), 2), 2u, min_distance, 16u, [&draws] { return static_cast<int>(draws++); })};
		CHECK(draws == 16);
		CHECK(chosen == 6);	// draw 15 % 2 == 1
	}
	/* The first draw is far enough: one draw. */
	{
		const site_array s{{{4, min_distance}, {6, 0}}};
		unsigned draws{0};
		const auto chosen{pick_spawn_site(std::span<const site>(s.data(), 2), 2u, min_distance, 16u, [&draws] { ++draws; return 2; })};
		CHECK(draws == 1);
		CHECK(chosen == 4);
	}
}

/* A level for the assignment tests: the sites on a line (distance = the
 * difference of the coordinates, in fix units), some ships on it.
 */
struct line_level
{
	std::vector<fix> sites;
	std::vector<fix> ships;
	/* site_distance of two sites that have no path between them. */
	int unreachable{-1};
	fix site_distance(const int a, const int b) const
	{
		if (a == unreachable || b == unreachable)
			return -1;
		return std::abs(sites[a] - sites[b]);
	}
};

constexpr int64_t hold{5 << 15};	// 2.5 s

/* The host's assign_spawn (gameseq.cpp), on a line level: distance to
 * the nearest ship, the reserved sites counted as ships, the free ones
 * first, the ranking and the draw; the chosen site is then reserved.
 */
int assign(const line_level &level, spawn_reservations<MAX_PLAYERS> &reservations, const int64_t now, const unsigned secluded, counting_rand &r)
{
	site_array s{};
	const std::size_t n{level.sites.size()};
	for (std::size_t i = 0; i < n; ++i)
	{
		fix closest{INT32_MAX};
		for (const auto ship : level.ships)
			closest = std::min(closest, std::abs(level.sites[i] - ship));
		s[i] = {static_cast<int>(i), closest};
	}
	const auto reserved = [&](const int site) { return reservations.reserved(static_cast<unsigned>(site), now); };
	count_reserved_spawn_sites(std::span<site>(s.data(), n), reserved, [&level](const int a, const int b) { return level.site_distance(a, b); });
	const auto candidates{partition_free_spawn_sites(std::span<site>(s.data(), n), reserved)};
	const auto usable{rank_secluded_spawn_sites(std::span<site>(s.data(), candidates), secluded)};
	CHECK(usable > 0);
	const auto chosen{pick_spawn_site(std::span<const site>(s.data(), usable), usable, min_distance, MAX_PLAYERS * 2, r)};
	reservations.reserve(static_cast<unsigned>(chosen), now, hold);
	return chosen;
}

constexpr fix units(const int u)
{
	return u << 16;
}

void test_reservations()
{
	spawn_reservations<MAX_PLAYERS> r;
	CHECK(!r.reserved(0, 0));
	r.reserve(2, 1000, hold);
	CHECK(r.reserved(2, 1000));
	CHECK(r.reserved(2, 1000 + hold - 1));
	CHECK(!r.reserved(2, 1000 + hold));
	CHECK(!r.reserved(3, 1000));
	CHECK(!r.reserved(MAX_PLAYERS, 1000));
	r.reserve(MAX_PLAYERS, 1000, hold);	// ignored
	r.reset();
	CHECK(!r.reserved(2, 1000));
	/* An owner's reservations go when it asks again; others' stay. */
	r.reserve(1, 1000, hold, 4);
	r.reserve(2, 1000, hold, 5);
	r.reserve(3, 1000, hold);
	r.release(4);
	CHECK(!r.reserved(1, 1000));
	CHECK(r.reserved(2, 1000));
	CHECK(r.reserved(3, 1000));
	r.release(spawn_reservations<MAX_PLAYERS>::no_owner);
	CHECK(r.reserved(3, 1000));
}

void test_reserved_sites_count_as_ships()
{
	/* Sites at 0, 500 and 1000, a ship at 0, site 2 reserved: site 2 is
	 * left out, site 1 is 500 from both, site 0 is at the ship.
	 */
	{
		line_level level{{units(0), units(500), units(1000)}, {units(0)}};
		site_array s{{{0, 0}, {1, units(500)}, {2, units(1000)}}};
		const auto reserved = [](const int site) { return site == 2; };
		count_reserved_spawn_sites(std::span<site>(s.data(), 3), reserved, [&level](const int a, const int b) { return level.site_distance(a, b); });
		CHECK(s[0].second == 0 && s[1].second == units(500) && s[2].second == units(1000));
		CHECK(partition_free_spawn_sites(std::span<site>(s.data(), 3), reserved) == 2);
		CHECK(s[0].first == 0 && s[1].first == 1 && s[2].first == 2);
		CHECK(rank_secluded_spawn_sites(std::span<site>(s.data(), 2), 1) == 1);
		CHECK(s[0].first == 1);
	}
	/* Without the reservation the farthest site (2) wins; with it, 1. */
	{
		line_level level{{units(0), units(500), units(1000)}, {units(0)}};
		spawn_reservations<MAX_PLAYERS> reservations;
		counting_rand r{1};
		CHECK(assign(level, reservations, 100, 1, r) == 2);
		CHECK(assign(level, reservations, 100, 1, r) == 1);
		/* The last free site, although it is at the ship. */
		CHECK(assign(level, reservations, 100, 1, r) == 0);
		/* Every site taken: 0 is at the ship, 1 and 2 are 500 from each
		 * other; either of those, never 0.
		 */
		CHECK(assign(level, reservations, 100, 1, r) != 0);
		/* Once the reservations are over, the first choice again. */
		spawn_reservations<MAX_PLAYERS> later{reservations};
		CHECK(assign(level, later, 100 + hold, 1, r) == 2);
	}
	/* An unreachable reserved site does not count as a ship. */
	{
		line_level level{{units(0), units(10)}, {}, 1};
		site_array s{{{0, INT32_MAX}, {1, INT32_MAX}}};
		count_reserved_spawn_sites(std::span<site>(s.data(), 2), [](const int site) { return site == 1; }, [&level](const int a, const int b) { return level.site_distance(a, b); });
		CHECK(s[0].second == INT32_MAX);
	}
}

/* More requests than sites: the next site is the one farthest from the
 * ships and the other reserved sites.
 */
void test_more_players_than_sites()
{
	/* Sites at 0, 300, 1000, 1400, a ship at 1400, all reserved:
	 * site 0 is 300 from site 1, site 1 300 from site 0, site 2 400 from
	 * site 3 and the ship, site 3 at the ship.  Site 2 is the farthest.
	 */
	line_level level{{units(0), units(300), units(1000), units(1400)}, {units(1400)}};
	spawn_reservations<MAX_PLAYERS> reservations;
	for (unsigned i = 0; i < 4; ++i)
		reservations.reserve(i, 0, hold);
	counting_rand r{7};
	CHECK(assign(level, reservations, 10, 1, r) == 2);
	/* Random layouts: with every site taken and one usable site, the
	 * assignment is a site whose distance to the nearest ship or other
	 * reserved site is the largest.
	 */
	std::mt19937 gen{99};
	std::uniform_int_distribution<int> pos{0, 3000};
	std::uniform_int_distribution<unsigned> count{2, MAX_PLAYERS};
	std::uniform_int_distribution<unsigned> ships{0, 4};
	for (unsigned iteration = 0; iteration < 20000; ++iteration)
	{
		line_level l;
		const unsigned n{count(gen)};
		for (unsigned i = 0; i < n; ++i)
			l.sites.push_back(units(pos(gen)));
		for (unsigned i = ships(gen); i--;)
			l.ships.push_back(units(pos(gen)));
		spawn_reservations<MAX_PLAYERS> all;
		for (unsigned i = 0; i < n; ++i)
			all.reserve(i, 0, hold);
		fix best{-1};
		std::vector<fix> clearance(n);
		for (unsigned i = 0; i < n; ++i)
		{
			fix c{INT32_MAX};
			for (const auto ship : l.ships)
				c = std::min(c, std::abs(l.sites[i] - ship));
			for (unsigned j = 0; j < n; ++j)
				if (j != i)
					c = std::min(c, std::abs(l.sites[i] - l.sites[j]));
			clearance[i] = c;
			best = std::max(best, c);
		}
		counting_rand rr{static_cast<uint32_t>(gen())};
		const auto chosen{assign(l, all, 1, 1, rr)};
		CHECK(clearance[chosen] == best);
	}
}

/* Many requests at the same time (the host answers them in one frame):
 * distinct sites while free ones remain, whatever the ranking settings.
 */
void test_simultaneous_requests()
{
	std::mt19937 gen{4242};
	std::uniform_int_distribution<int> pos{0, 2000};
	std::uniform_int_distribution<unsigned> count{1, MAX_PLAYERS};
	std::uniform_int_distribution<unsigned> ships{0, 3};
	std::uniform_int_distribution<unsigned> secluded_dist{1, MAX_PLAYERS};
	for (unsigned iteration = 0; iteration < 50000; ++iteration)
	{
		line_level l;
		const unsigned n{count(gen)};
		for (unsigned i = 0; i < n; ++i)
			l.sites.push_back(units(pos(gen)));
		for (unsigned i = ships(gen); i--;)
			l.ships.push_back(units(pos(gen)));
		const unsigned secluded{secluded_dist(gen)};
		spawn_reservations<MAX_PLAYERS> reservations;
		counting_rand r{static_cast<uint32_t>(gen())};
		const int64_t now{static_cast<int64_t>(gen() % 100000) + 1};
		std::set<int> taken;
		for (unsigned request = 0; request < n; ++request)
		{
			const auto chosen{assign(l, reservations, now, secluded, r)};
			CHECK(chosen >= 0 && static_cast<unsigned>(chosen) < n);
			CHECK(taken.insert(chosen).second);
		}
		/* Requests a little later, still within the reservations: the
		 * sites are all taken, so any of them, but still a valid site.
		 */
		const auto extra{assign(l, reservations, now + hold / 2, secluded, r)};
		CHECK(extra >= 0 && static_cast<unsigned>(extra) < n);
	}
}

}

int main()
{
	test_examples();
	test_against_reference();
	test_reservations();
	test_reserved_sites_count_as_ships();
	test_more_players_than_sites();
	test_simultaneous_requests();
	std::puts("test-spawn-site: all checks passed");
	return 0;
}
