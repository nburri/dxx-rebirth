/*
 * This file is part of the DXX-Rebirth project <https://github.com/dxx-rebirth/dxx-rebirth/>.
 * It is copyright by its individual contributors, as recorded in the
 * project's Git history.  See COPYING.txt at the top level for license
 * terms and a link to the Git history.
 */
/*
 * Test of the Omega cannon's fire rule (omega_fire.h) and of the damage
 * a held Omega does, at 30 to 500 fps.
 *
 * The frame loop below is a copy of the parts of the game that decide
 * when the Omega fires: do_laser_firing_player (one pull per
 * OMEGA_BASE_TIME, with the frame overhead carried), do_omega_stuff
 * (the charge taken by a shot and the recharge delay) and
 * omega_charge_frame (the recharge with the delay ending inside a frame
 * and the remainder carried), all in laser.cpp.
 *
 * What it checks:
 * - single player keeps its rule: the 18 shots of a full charge, then
 *   one shot per recharge (about 1.9 a second);
 * - in a network game, a held Omega does its damage with every pull,
 *   20 a second, from any charge, as the victims' machines did in v1
 *   (the classic game); the shots that take a charge are the same as in
 *   single player;
 * - the damage per second does not depend on the frame rate.
 *
 * Build and run with SCons:
 *
 *	scons sdl2=1 d1x=0 d2x=1 register_runtime_test_plain_link_targets=1 test-omega-fire
 *	build/common/test-omega-fire
 *
 * or directly:
 *
 *	g++ -std=gnu++23 -O2 -Wall -Wextra -Icommon/main common/unittest/omega_fire.cpp -o test-omega-fire
 */

#include <cinttypes>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <initializer_list>

#include "omega_fire.h"

namespace {

using fix = std::int32_t;
constexpr fix F1_0{65536};
constexpr fix MAX_OMEGA_CHARGE{F1_0};
constexpr fix MIN_OMEGA_CHARGE{MAX_OMEGA_CHARGE / 8};
constexpr fix OMEGA_BASE_TIME{F1_0 / 20};
constexpr fix OMEGA_CHARGE_SCALE{4};

unsigned failures;

void check(const bool ok, const char *const what)
{
	if (!ok)
	{
		++failures;
		std::printf("FAIL: %s\n", what);
	}
}

struct held_result
{
	unsigned lightning{};	/* pulls that made lightning (damage) */
	unsigned charged{};		/* of them, those that took a charge */
	unsigned pulls{};
	/* Game time of the first lightning and of the n-th (n = needed). */
	std::int64_t first{-1}, nth{-1};
};

/* Hold the trigger for `seconds` from `start_charge`. */
held_result hold(const unsigned fps, const bool network_game, const fix start_charge, const double seconds, const unsigned needed = 0)
{
	const fix frame_time{F1_0 / static_cast<fix>(fps)};
	std::int64_t game_time{0};
	std::int64_t next_fire{0};
	fix charge{start_charge};
	fix delay{0};
	fix remainder{0};
	fix energy{F1_0 * 200};
	held_result r;
	const std::int64_t end{static_cast<std::int64_t>(seconds * F1_0)};
	while (game_time < end)
	{
		game_time += frame_time;
		/* do_laser_firing_player */
		while (next_fire <= game_time)
		{
			const fix overhead{game_time - next_fire <= frame_time ? static_cast<fix>(game_time - next_fire) : 0};
			++r.pulls;
			switch (dcx::omega_shot_kind(charge, MIN_OMEGA_CHARGE, energy, network_game))
			{
				case dcx::omega_shot::none:
					break;
				case dcx::omega_shot::charged:
					++r.charged;
					charge = charge > OMEGA_BASE_TIME ? charge - OMEGA_BASE_TIME : 0;
					delay = F1_0 / 3 + frame_time;
					[[fallthrough]];
				case dcx::omega_shot::uncharged:
					if (!r.lightning++)
						r.first = game_time;
					if (r.lightning == needed)
						r.nth = game_time;
					break;
			}
			next_fire = game_time - overhead + OMEGA_BASE_TIME;
		}
		/* omega_charge_frame */
		if (charge >= MAX_OMEGA_CHARGE)
		{
			remainder = 0;
			continue;
		}
		fix charge_time{frame_time};
		if (delay)
		{
			if (delay > frame_time)
			{
				delay -= frame_time;
				continue;
			}
			charge_time -= delay;
			delay = 0;
		}
		const fix old_charge{charge};
		const fix total{charge_time + remainder};
		charge += total / OMEGA_CHARGE_SCALE;
		remainder = total % OMEGA_CHARGE_SCALE;
		if (charge >= MAX_OMEGA_CHARGE)
		{
			charge = MAX_OMEGA_CHARGE;
			remainder = 0;
		}
		energy -= static_cast<fix>((static_cast<std::int64_t>(charge - old_charge) * 190) / 17);
	}
	return r;
}

void test_rule()
{
	using dcx::omega_shot;
	using dcx::omega_shot_kind;
	for (const bool net : {false, true})
	{
		check(omega_shot_kind(MAX_OMEGA_CHARGE, MIN_OMEGA_CHARGE, F1_0 * 100, net) == omega_shot::charged, "full charge fires");
		check(omega_shot_kind(MIN_OMEGA_CHARGE, MIN_OMEGA_CHARGE, F1_0 * 100, net) == omega_shot::charged, "minimum charge fires");
		check(omega_shot_kind(1, MIN_OMEGA_CHARGE, 0, net) == omega_shot::charged, "some charge and no energy fires");
		check(omega_shot_kind(0, MIN_OMEGA_CHARGE, 0, net) == omega_shot::none, "no charge, no energy: nothing");
	}
	check(omega_shot_kind(MIN_OMEGA_CHARGE - 1, MIN_OMEGA_CHARGE, F1_0 * 100, false) == omega_shot::none, "single player: low charge does not fire");
	check(omega_shot_kind(0, MIN_OMEGA_CHARGE, F1_0, false) == omega_shot::none, "single player: empty charge does not fire");
	check(omega_shot_kind(MIN_OMEGA_CHARGE - 1, MIN_OMEGA_CHARGE, F1_0 * 100, true) == omega_shot::uncharged, "network: low charge fires uncharged");
	check(omega_shot_kind(0, MIN_OMEGA_CHARGE, 1, true) == omega_shot::uncharged, "network: empty charge with energy fires uncharged");
}

void test_held(const unsigned fps)
{
	char what[160];
	/* Single player: 18 shots of a full charge, then about 1.9 a second. */
	{
		const auto burst{hold(fps, false, MAX_OMEGA_CHARGE, 1.0)};
		std::snprintf(what, sizeof(what), "%u fps single player: 18 shots in the first second (got %u)", fps, burst.lightning);
		check(burst.lightning == 18, what);
		const auto a{hold(fps, false, MAX_OMEGA_CHARGE, 5.0)};
		const auto b{hold(fps, false, MAX_OMEGA_CHARGE, 25.0)};
		const double rate{(b.lightning - a.lightning) / 20.0};
		std::snprintf(what, sizeof(what), "%u fps single player: sustained %.3f shots/s, expected 1.7 to 1.95", fps, rate);
		check(rate > 1.7 && rate < 1.95, what);
		check(b.lightning == b.charged, "single player: every shot takes a charge");
	}
	/* Network game: every pull, 20 a second, from any charge. */
	for (const fix start : {MAX_OMEGA_CHARGE, MAX_OMEGA_CHARGE / 2, MIN_OMEGA_CHARGE - 1, fix{0}})
	{
		const auto r{hold(fps, true, start, 10.0)};
		std::snprintf(what, sizeof(what), "%u fps network, charge %.3f: %u shots of %u pulls in 10 s, expected every pull, 199 to 201", fps, start / 65536.0, r.lightning, r.pulls);
		check(r.lightning == r.pulls && r.lightning >= 199 && r.lightning <= 201, what);
		/* The charge itself is used as in single player. */
		const auto s{hold(fps, false, start, 10.0)};
		std::snprintf(what, sizeof(what), "%u fps network, charge %.3f: charged shots %u, single player %u", fps, start / 65536.0, r.charged, s.charged);
		check(r.charged == s.charged, what);
	}
	/* Time to kill 100 shields at 5.6 per hit (18 hits) from an empty
	 * charge: 17 pulls apart, as v1's victims took it.
	 */
	{
		const auto r{hold(fps, true, 0, 5.0, 18)};
		const double ttk{static_cast<double>(r.nth - r.first) / 65536.0};
		std::snprintf(what, sizeof(what), "%u fps network, empty charge: 18 hits in %.3f s, expected 0.85 (+-0.03)", fps, ttk);
		/* The pulls fall on frames: at 30 and 60 fps a little earlier. */
		check(std::fabs(ttk - 0.85) < 0.03, what);
	}
}

}

int main()
{
	test_rule();
	for (const unsigned fps : {30u, 60u, 120u, 200u, 500u})
		test_held(fps);
	if (failures)
	{
		std::printf("%u failure(s)\n", failures);
		return EXIT_FAILURE;
	}
	std::printf("omega_fire: all tests passed\n");
	return EXIT_SUCCESS;
}
