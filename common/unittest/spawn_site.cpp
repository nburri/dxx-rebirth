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
#include <random>
#include <utility>

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

}

int main()
{
	test_examples();
	test_against_reference();
	std::puts("test-spawn-site: all checks passed");
	return 0;
}
