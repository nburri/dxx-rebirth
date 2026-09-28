/*
 * This file is part of the DXX-Rebirth project <https://github.com/dxx-rebirth/dxx-rebirth/>.
 * It is copyright by its individual contributors, as recorded in the
 * project's Git history.  See COPYING.txt at the top level for license
 * terms and a link to the Git history.
 */
/*
 * Test of the score carry-over across a level load (net_score_carry.h,
 * Documentation/network-protocol-v2.md section 4.3): the kills, deaths,
 * kill goal count and score of every slot survive a level whose player
 * objects are at other object numbers and whose object memory holds
 * other values.
 *
 * Build and run with SCons:
 *
 *	scons sdl2=1 d1x=0 d2x=1 register_runtime_test_plain_link_targets=1 test-net-score-carry
 *	build/common/test-net-score-carry
 */

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ranges>

#include "net_score_carry.h"

using namespace dcx::net_v2;

namespace {

void check_failed(const char *const expr, const char *const file, const int line)
{
	std::fprintf(stderr, "%s:%d: check failed: %s\n", file, line, expr);
	std::exit(1);
}

#define CHECK(cond)	do { if (!(cond)) check_failed(#cond, __FILE__, __LINE__); } while (0)

/* The fields of the game's player_info that the carry touches, plus one it
 * must leave alone.
 */
struct fake_player_info
{
	int16_t net_killed_total;
	int16_t net_kills_total;
	int16_t KillGoalCount;
	struct {
		int score;
		int last_score;
	} mission;
	int energy;
};

constexpr std::size_t players{8};
constexpr std::size_t objects{32};
constexpr uint16_t none{0xffff};

struct world
{
	std::array<fake_player_info, objects> obj{};
	std::array<uint16_t, players> objnum{};
	fake_player_info *info(const std::size_t slot)
	{
		return objnum[slot] == none ? nullptr : &obj[objnum[slot]];
	}
};

/* The load of the next level: every object is rewritten with the next
 * level's contents, which never include player_info, so its memory holds
 * garbage for this test; the slots get the objects the level file
 * places, in another order.
 */
void load_level(world &w, const std::array<uint16_t, players> &placement)
{
	for (auto &&[i, o] : std::views::enumerate(w.obj))
		o = {static_cast<int16_t>(1000 + i), static_cast<int16_t>(2000 + i), 77, {static_cast<int>(-5 - i), 3}, 99};
	w.objnum = placement;
}

void test_scores_follow_the_slot()
{
	world w;
	for (std::size_t s = 0; s < players; ++s)
	{
		w.objnum[s] = static_cast<uint16_t>(s);
		w.obj[s] = {static_cast<int16_t>(s + 1), static_cast<int16_t>(10 * (s + 1)), static_cast<int16_t>(s), {static_cast<int>(100 * s), static_cast<int>(50 * s)}, 7};
	}
	/* A slot with a negative total (suicides) and one without a ship. */
	w.obj[3].net_kills_total = -4;
	w.objnum[7] = none;
	const auto before{capture_all_scores<players>([&w](const std::size_t s) -> const fake_player_info * { return w.info(s); })};
	CHECK(before[3].kills == -4);
	CHECK(before[7] == carried_scores{});
	/* The next level has its player starts after other objects and in
	 * another order.
	 */
	load_level(w, {{20, 5, 9, 0, 17, 3, 12, 28}});
	restore_all_scores(before, [&w](const std::size_t s) { return w.info(s); });
	for (std::size_t s = 0; s < players - 1; ++s)
	{
		const auto &pi{w.obj[w.objnum[s]]};
		CHECK(pi.net_killed_total == static_cast<int16_t>(s + 1));
		CHECK(pi.net_kills_total == (s == 3 ? -4 : static_cast<int16_t>(10 * (s + 1))));
		CHECK(pi.KillGoalCount == static_cast<int16_t>(s));
		CHECK(pi.mission.score == static_cast<int>(100 * s));
		CHECK(pi.mission.last_score == static_cast<int>(50 * s));
		/* Everything else is the new level's business. */
		CHECK(pi.energy == 99);
	}
	/* Slot 7 had no ship: it enters the level with zero scores. */
	CHECK(capture_scores(w.obj[w.objnum[7]]) == carried_scores{});
}

/* What the host sent before the fix: the scores read back from the new
 * objects are those of the previous level's objects at the same numbers,
 * i.e. another slot's, or no player's at all.
 */
void test_without_the_carry_scores_are_lost()
{
	world w;
	for (std::size_t s = 0; s < players; ++s)
	{
		w.objnum[s] = static_cast<uint16_t>(s);
		w.obj[s] = {0, static_cast<int16_t>(s + 1), 0, {0, 0}, 0};
	}
	/* Memory is kept (release builds do not poison), players placed one
	 * object further: every slot reads its neighbour's total.
	 */
	std::array<uint16_t, players> placement;
	for (std::size_t s = 0; s < players; ++s)
		placement[s] = static_cast<uint16_t>(s + 1);
	w.objnum = placement;
	for (std::size_t s = 0; s + 1 < players; ++s)
		CHECK(w.info(s)->net_kills_total != static_cast<int16_t>(s + 1));
}

/* Two consecutive loads keep the totals (the carry does not add). */
void test_repeated_loads()
{
	world w;
	for (std::size_t s = 0; s < players; ++s)
	{
		w.objnum[s] = static_cast<uint16_t>(s);
		w.obj[s] = {static_cast<int16_t>(s), static_cast<int16_t>(s * 3), 0, {0, 0}, 0};
	}
	const auto info{[&w](const std::size_t s) { return w.info(s); }};
	for (unsigned level = 0; level < 3; ++level)
	{
		const auto before{capture_all_scores<players>(info)};
		std::array<uint16_t, players> placement;
		for (std::size_t s = 0; s < players; ++s)
			placement[s] = static_cast<uint16_t>((s * 3 + level * 5) % objects);
		load_level(w, placement);
		restore_all_scores(before, info);
		/* A kill in the new level: slot 1 kills slot 2. */
		++w.info(1)->net_kills_total;
		++w.info(2)->net_killed_total;
	}
	CHECK(w.info(1)->net_kills_total == 3 + 3);
	CHECK(w.info(2)->net_killed_total == 2 + 3);
	CHECK(w.info(0)->net_kills_total == 0);
	CHECK(w.info(7)->net_kills_total == 21);
}

/* The level end reports: the host keeps its own counts, a client adopts
 * the host's, and a report of a level that is over changes nothing.
 */
void test_endlevel_reports()
{
	static_assert(endlevel_report_applies(true, true) == endlevel_report_use{.status = true, .scores = false});
	static_assert(endlevel_report_applies(false, true) == endlevel_report_use{.status = true, .scores = true});
	static_assert(endlevel_report_applies(true, false) == endlevel_report_use{});
	static_assert(endlevel_report_applies(false, false) == endlevel_report_use{});
	/* The race the rule removes: the host credits the client a kill made
	 * in the countdown; the client's report was sent before the relay of
	 * that kill reached it.
	 */
	int16_t host_view_of_client{10};
	++host_view_of_client;			/* MULTI_KILL_HOST relayed */
	const int16_t client_report{10};	/* in flight, older */
	if (endlevel_report_applies(true, true).scores)
		host_view_of_client = client_report;
	CHECK(host_view_of_client == 11);
	/* And the carry to the next level keeps it. */
	world w;
	for (std::size_t s = 0; s < players; ++s)
		w.objnum[s] = static_cast<uint16_t>(s);
	w.obj[1].net_kills_total = host_view_of_client;
	const auto before{capture_all_scores<players>([&w](const std::size_t s) -> const fake_player_info * { return w.info(s); })};
	load_level(w, {{7, 6, 5, 4, 3, 2, 1, 0}});
	restore_all_scores(before, [&w](const std::size_t s) { return w.info(s); });
	CHECK(w.info(1)->net_kills_total == 11);
}

}

int main()
{
	test_scores_follow_the_slot();
	test_without_the_carry_scores_are_lost();
	test_repeated_loads();
	test_endlevel_reports();
	std::puts("test-net-score-carry: all checks passed");
	return 0;
}
