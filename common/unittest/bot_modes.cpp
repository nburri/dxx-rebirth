/*
 * This file is part of the DXX-Rebirth project <https://github.com/dxx-rebirth/dxx-rebirth/>.
 * It is copyright by its individual contributors, as recorded in the
 * project's Git history.  See COPYING.txt at the top level for license
 * terms and a link to the Git history.
 */
/*
 * Test of the bots in capture the flag and hoard (bot_modes.h,
 * Documentation/multiplayer-bots.md section 9.19, stage B7): the team's
 * role assignment (quotas by the flags' state, the bot each role costs
 * least, styles, hysteresis, humans, dead bots), the objectives' weights
 * against the other goals (bot_goals.h), when to score orbs, the hoard
 * escort and the targets' weights.
 *
 * Build and run with SCons:
 *
 *	scons sdl2=1 d1x=0 d2x=1 register_runtime_test_plain_link_targets=1 test-bot-modes
 *	build/common/test-bot-modes
 */

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <initializer_list>
#include <span>
#include <string_view>

#include "bot_modes.h"
#include "bot_goals.h"

using namespace dcx::bot;

namespace {

void check_failed(const char *const expr, const char *const file, const int line)
{
	std::fprintf(stderr, "%s:%d: check failed: %s\n", file, line, expr);
	std::exit(1);
}

#define CHECK(cond)	do { if (!(cond)) check_failed(#cond, __FILE__, __LINE__); } while (0)

struct team_case
{
	std::array<member_view, 8> m{};
	std::array<mode_role, 8> r{};
	unsigned n{};
	member_view &add(const bool bot, const bot_style s = bot_style::balanced)
	{
		auto &v{m[n++]};
		v.pid = static_cast<uint8_t>(n);
		v.bot = bot;
		v.style = s;
		v.to_own_home = 300;
		v.to_enemy_flag = 300;
		return v;
	}
	void assign(const team_view &t)
	{
		assign_ctf_roles(t, std::span(m.data(), n), std::span(r.data(), n));
	}
	[[nodiscard]]
	unsigned count(const mode_role role) const
	{
		return static_cast<unsigned>(std::count(r.begin(), r.begin() + n, role));
	}
};

team_view classic_home(const unsigned size)
{
	team_view t;
	t.classic = true;
	t.touch_returns = true;
	t.home_to_score = true;
	t.own_flag = flag_state::home;
	t.enemy_flag = flag_state::home;
	t.own_flag_place_known = true;
	t.enemy_flag_place_known = true;
	t.team_size = size;
	t.enemies = size;
	return t;
}

/* Both flags at home: one defender from three players, two from six,
 * none in a team of two; the rest attack.  The cautious bot defends.
 */
void test_quota_home()
{
	{
		team_case c;
		c.add(true);
		c.add(true);
		c.assign(classic_home(2));
		CHECK(c.count(mode_role::attack) == 2);
		CHECK(c.count(mode_role::defend) == 0);
	}
	{
		team_case c;
		c.add(true, bot_style::aggressive);
		c.add(true, bot_style::cautious);
		c.add(true, bot_style::collector);
		c.assign(classic_home(3));
		CHECK(c.count(mode_role::defend) == 1);
		CHECK(c.count(mode_role::attack) == 2);
		CHECK(c.r[1] == mode_role::defend);
	}
	{
		team_case c;
		for (unsigned i = 0; i < 6; ++i)
			c.add(true);
		c.assign(classic_home(6));
		CHECK(c.count(mode_role::defend) == 2);
		CHECK(c.count(mode_role::attack) == 4);
	}
	/* Ahead in a team of four: a second defender; far behind in a team
	 * of six: one less.
	 */
	{
		team_case c;
		for (unsigned i = 0; i < 4; ++i)
			c.add(true);
		auto t{classic_home(4)};
		t.score_lead = 5;
		c.assign(t);
		CHECK(c.count(mode_role::defend) == 2);
		CHECK(c.count(mode_role::attack) == 2);
	}
	{
		team_case c;
		for (unsigned i = 0; i < 6; ++i)
			c.add(true);
		auto t{classic_home(6)};
		t.score_lead = -10;
		c.assign(t);
		CHECK(c.count(mode_role::defend) == 1);
	}
}

/* The nearest bot to home defends (equal styles). */
void test_nearest()
{
	team_case c;
	c.add(true).to_own_home = 900;
	c.add(true).to_own_home = 100;
	c.add(true).to_own_home = 500;
	c.assign(classic_home(3));
	CHECK(c.r[1] == mode_role::defend);
	CHECK(c.r[0] == mode_role::attack && c.r[2] == mode_role::attack);
}

/* Hysteresis: the defender keeps its role against a bot a little
 * nearer; a much nearer one takes it.
 */
void test_hysteresis()
{
	team_case c;
	auto &a{c.add(true)};
	auto &b{c.add(true)};
	c.add(true).to_own_home = 2000;
	a.to_own_home = 400;
	a.previous = mode_role::defend;
	b.to_own_home = 300;
	c.assign(classic_home(3));
	CHECK(c.r[0] == mode_role::defend);
	b.to_own_home = 50;
	c.assign(classic_home(3));
	CHECK(c.r[1] == mode_role::defend);
	CHECK(c.r[0] == mode_role::attack);
}

/* Humans have no role but count in the team's size; a lone bot with two
 * humans defends, one with one human attacks.
 */
void test_humans()
{
	{
		team_case c;
		c.add(false);
		c.add(false);
		c.add(true);
		c.assign(classic_home(3));
		CHECK(c.r[0] == mode_role::none && c.r[1] == mode_role::none);
		CHECK(c.r[2] == mode_role::defend);
	}
	{
		team_case c;
		c.add(false);
		c.add(true);
		c.assign(classic_home(2));
		CHECK(c.r[1] == mode_role::attack);
	}
	{
		team_case c;
		c.add(false);
		c.add(true);
		c.add(true);
		c.assign(classic_home(3));
		CHECK(c.count(mode_role::defend) == 1);
		CHECK(c.count(mode_role::attack) == 1);
	}
}

/* The own flag carried by an enemy: half the bots hunt (at least one);
 * all of them when the team carries the other flag and scores only with
 * its own at home.
 */
void test_hunt()
{
	{
		team_case c;
		for (unsigned i = 0; i < 4; ++i)
			c.add(true);
		c.m[2].style = bot_style::aggressive;
		auto t{classic_home(4)};
		t.own_flag = flag_state::carried;
		c.assign(t);
		CHECK(c.count(mode_role::hunt) == 2);
		CHECK(c.count(mode_role::defend) == 0);
		CHECK(c.r[2] == mode_role::hunt);
	}
	{
		team_case c;
		c.add(true);
		c.add(true);
		auto t{classic_home(2)};
		t.own_flag = flag_state::carried;
		c.assign(t);
		CHECK(c.count(mode_role::hunt) == 1);
		CHECK(c.count(mode_role::attack) == 1);
	}
	{
		team_case c;
		c.add(true).carrier = true;
		c.add(true);
		c.add(true);
		auto t{classic_home(3)};
		t.own_flag = flag_state::carried;
		t.enemy_flag = flag_state::carried;
		t.team_carries = true;
		c.assign(t);
		/* The carrier waits for its flag (Classic, home to score). */
		CHECK(c.r[0] == mode_role::wait);
		CHECK(c.r[1] == mode_role::hunt && c.r[2] == mode_role::hunt);
	}
	/* Without "home to score" the carrier carries and an escort goes
	 * along while the other hunts.
	 */
	{
		team_case c;
		c.add(true).carrier = true;
		c.add(true);
		c.add(true);
		c.m[1].to_enemy_carrier = 100;
		c.m[2].to_own_carrier = 50;
		auto t{classic_home(3)};
		t.home_to_score = false;
		t.own_flag = flag_state::carried;
		t.enemy_flag = flag_state::carried;
		t.team_carries = true;
		c.assign(t);
		CHECK(c.r[0] == mode_role::carry);
		CHECK(c.r[1] == mode_role::hunt);
		CHECK(c.r[2] == mode_role::escort);
	}
}

/* The team carries the enemy flag, its own at home: the carrier carries,
 * one escort from three players (two from five), the rest defend.
 */
void test_carry()
{
	team_case c;
	for (unsigned i = 0; i < 5; ++i)
		c.add(true);
	c.m[3].carrier = true;
	auto t{classic_home(5)};
	t.enemy_flag = flag_state::carried;
	t.team_carries = true;
	c.assign(t);
	CHECK(c.r[3] == mode_role::carry);
	CHECK(c.count(mode_role::escort) == 2);
	CHECK(c.count(mode_role::defend) == 2);
	CHECK(c.count(mode_role::attack) == 0);
}

/* The own flag dropped where the team knows it: with the touch rule the
 * nearest bot retrieves it; without, a bot defends it.
 */
void test_retrieve()
{
	{
		team_case c;
		c.add(true).to_own_flag = 800;
		c.add(true).to_own_flag = 120;
		c.add(true).to_own_flag = 400;
		auto t{classic_home(3)};
		t.own_flag = flag_state::lying;
		c.assign(t);
		CHECK(c.r[1] == mode_role::retrieve);
		CHECK(c.count(mode_role::retrieve) == 1);
	}
	{
		team_case c;
		c.add(true);
		c.add(true);
		auto t{classic_home(2)};
		t.touch_returns = false;
		t.own_flag = flag_state::lying;
		c.assign(t);
		CHECK(c.count(mode_role::defend) == 1);
		CHECK(c.count(mode_role::retrieve) == 0);
	}
	/* Standard capture the flag: no touch return. */
	{
		team_case c;
		c.add(true);
		c.add(true);
		c.add(true);
		team_view t;
		t.own_flag = flag_state::lying;
		t.own_flag_place_known = true;
		t.enemy_flag = flag_state::lying;
		t.enemy_flag_place_known = true;
		t.team_size = 3;
		c.assign(t);
		CHECK(c.count(mode_role::retrieve) == 0);
		CHECK(c.count(mode_role::defend) == 1);
		CHECK(c.count(mode_role::attack) == 2);
	}
}

/* Standard capture the flag: a flag not carried always lies somewhere;
 * the defenders follow the team's size as in Classic (the review of B7:
 * a lone bot defended for ever).  Classic with the own flag gone for a
 * moment (a capture): no retriever, no hunter.
 */
void test_standard_sizes()
{
	for (unsigned size = 1; size <= 6; ++size)
	{
		team_case c;
		for (unsigned i = 0; i < size; ++i)
			c.add(true);
		team_view t;
		t.own_flag = flag_state::lying;
		t.own_flag_place_known = true;
		t.enemy_flag = flag_state::lying;
		t.enemy_flag_place_known = true;
		t.team_size = size;
		c.assign(t);
		CHECK(c.count(mode_role::defend) == (size >= 6 ? 2u : size >= 3 ? 1u : 0u));
		CHECK(c.count(mode_role::attack) == size - c.count(mode_role::defend));
	}
	team_case c;
	c.add(true);
	c.add(true);
	c.add(true);
	auto t{classic_home(3)};
	t.own_flag = flag_state::unknown;
	t.own_flag_place_known = false;
	c.assign(t);
	CHECK(c.count(mode_role::retrieve) == 0 && c.count(mode_role::hunt) == 0);
	CHECK(c.count(mode_role::defend) == 1);
}

/* A dead bot is the last choice for a role with a place. */
void test_dead()
{
	team_case c;
	auto &a{c.add(true)};
	a.alive = false;
	a.to_own_home = 10;
	c.add(true).to_own_home = 600;
	c.add(true).to_own_home = 700;
	c.assign(classic_home(3));
	CHECK(c.r[0] == mode_role::attack);
	CHECK(c.count(mode_role::defend) == 1);
}

/* Every bot of every case gets exactly one role, each role within its
 * quota (a sweep over the flags' states and team sizes).
 */
void test_sweep()
{
	const std::array<flag_state, 4> states{{flag_state::unknown, flag_state::home, flag_state::lying, flag_state::carried}};
	for (unsigned size = 1; size <= 7; ++size)
		for (unsigned humans = 0; humans < size; ++humans)
			for (const auto own : states)
				for (const auto enemy : states)
					for (unsigned rules = 0; rules < 8; ++rules)
					{
						team_case c;
						for (unsigned i = 0; i < size; ++i)
						{
							auto &m{c.add(i >= humans, static_cast<bot_style>(i % BOT_STYLE_COUNT))};
							m.to_own_home = 100.0 * i;
						}
						team_view t;
						t.classic = rules & 1;
						t.touch_returns = rules & 2;
						t.home_to_score = rules & 4;
						t.own_flag = own;
						t.enemy_flag = enemy;
						t.own_flag_place_known = true;
						t.enemy_flag_place_known = true;
						t.team_carries = enemy == flag_state::carried;
						if (t.team_carries)
							c.m[size - 1].carrier = true;
						t.team_size = size;
						c.assign(t);
						unsigned carriers{0};
						for (unsigned i = 0; i < size; ++i)
						{
							const auto r{c.r[i]};
							if (i < humans)
								CHECK(r == mode_role::none);
							else
								CHECK(r != mode_role::none && r != mode_role::collect && r != mode_role::score);
							if (r == mode_role::carry || r == mode_role::wait)
								++carriers;
						}
						CHECK(carriers <= 1);
						CHECK(c.count(mode_role::retrieve) <= 1);
						if (own != flag_state::carried)
							CHECK(c.count(mode_role::hunt) == 0);
						else if (size > humans + (t.team_carries ? 1u : 0u))
							CHECK(c.count(mode_role::hunt) >= 1);
						if (!t.team_carries)
							CHECK(c.count(mode_role::escort) == 0);
					}
}

/* The objectives against the other goals: a carrier goes home rather
 * than fight one enemy in sight (engage about 2), a defender at home
 * lets the fight decide, an attacker goes for the flag rather than roam
 * or a small collection, a retreat at low shields beats an attack.
 */
void test_objectives()
{
	objective_view v;
	v.place_known = true;
	v.path = 600;
	v.role = mode_role::carry;
	const auto carry{objective_for(v)};
	CHECK(carry.utility > 4);
	CHECK(carry.path_while_fighting && carry.burn && carry.cover > 0);
	CHECK(carry.engage < 1);
	v.role = mode_role::defend;
	v.path = 50;
	const auto defend_there{objective_for(v)};
	CHECK(defend_there.utility > ROAM_UTILITY && defend_there.utility < 1);
	CHECK(defend_there.hunt < 1);
	v.path = 900;
	CHECK(objective_for(v).utility > 2);
	v.role = mode_role::attack;
	const auto far_attack{objective_for(v)};
	v.path = 100;
	CHECK(objective_for(v).utility > far_attack.utility);
	v.place_known = false;
	CHECK(objective_for(v).utility == 0);
	v.place_known = true;
	v.role = mode_role::hunt;
	v.target_in_sight = true;
	CHECK(objective_for(v).utility == 0);
	v.target_in_sight = false;
	CHECK(objective_for(v).utility > 2);
	/* Watching the enemy goal from round about: there, the fight
	 * decides; far from it, the hunter goes.
	 */
	v.watching = true;
	v.path = 60;
	CHECK(objective_for(v).utility < 1);
	v.path = 600;
	CHECK(objective_for(v).utility > 2);
	v.watching = false;
	v.role = mode_role::score;
	v.orbs = 5;
	const auto score5{objective_for(v)};
	v.orbs = 1;
	CHECK(score5.utility > objective_for(v).utility);
	/* In the goal choice. */
	goal_inputs in;
	in.has_target = true;
	in.target_visible = true;
	in.target_score = 1;
	in.armed = armed_level::light;
	in.objective = carry.utility;
	in.mode_engage = carry.engage;
	in.mode_hunt = carry.hunt;
	CHECK(choose_goal(in) == goal_kind::objective);
	in.objective = defend_there.utility;
	in.mode_engage = defend_there.engage;
	in.mode_hunt = defend_there.hunt;
	CHECK(choose_goal(in) == goal_kind::engage);
	in.target_visible = false;
	in.target_score = 0.5;
	in.mode_hunt = 0.35;
	in.objective = 2.6;
	CHECK(choose_goal(in) == goal_kind::objective);
	goal_inputs roam;
	roam.objective = far_attack.utility;
	CHECK(choose_goal(roam) == goal_kind::objective);
	/* Weak and threatened: the retreat beats an attack. */
	goal_inputs weak;
	weak.threatened = true;
	weak.shields = 10;
	weak.objective = far_attack.utility;
	CHECK(choose_goal(weak) == goal_kind::retreat);
	/* The carrier keeps going home even weak. */
	weak.objective = carry.utility;
	CHECK(choose_goal(weak) == goal_kind::objective);
	/* The objective's hysteresis: the current goal counts more. */
	goal_inputs close;
	close.objective = 2.4;
	close.collect = 2.6;
	close.current = goal_kind::objective;
	CHECK(choose_goal(close) == goal_kind::objective);
}

void test_hoard()
{
	hoard_view v;
	v.to_goal = 800;
	v.orbs = 0;
	CHECK(!hoard_should_score(v));
	v.orbs = 3;
	CHECK(!hoard_should_score(v));
	v.orbs = 4;
	CHECK(hoard_should_score(v));
	v.style = bot_style::collector;
	CHECK(!hoard_should_score(v));
	v.orbs = 6;
	CHECK(hoard_should_score(v));
	/* The most a ship holds. */
	v.orbs = 12;
	v.to_goal = 5000;
	CHECK(!hoard_should_score(v));
	v.to_goal = 2000;
	CHECK(hoard_should_score(v));
	/* A goal on the way: half the load. */
	v.style = bot_style::balanced;
	v.to_goal = 100;
	v.orbs = 1;
	CHECK(!hoard_should_score(v));
	v.orbs = 2;
	CHECK(hoard_should_score(v));
	/* Hurt. */
	v.orbs = 1;
	v.to_goal = 500;
	v.shields = 30;
	CHECK(hoard_should_score(v));
	v.shields = 100;
	CHECK(!hoard_should_score(v));
	/* Cautious banks early; scoring keeps going one orb below. */
	v.style = bot_style::cautious;
	v.orbs = 2;
	CHECK(hoard_should_score(v));
	v.style = bot_style::balanced;
	v.orbs = 3;
	CHECK(!hoard_should_score(v));
	v.scoring = true;
	CHECK(hoard_should_score(v));
	v.scoring = false;
	v.threatened = true;
	CHECK(hoard_should_score(v));
	/* No goal known. */
	v.to_goal = ROLE_FAR;
	v.orbs = 12;
	CHECK(!hoard_should_score(v));
	CHECK(hoard_points(1) == 1 && hoard_points(4) == 10 && hoard_points(12) == 78);
	/* An orb is worth more with a load, less far away. */
	CHECK(orb_utility(5, 100) > orb_utility(0, 100));
	CHECK(orb_utility(0, 100) > orb_utility(0, 800));
}

void test_hoard_roles()
{
	std::array<hoard_member, 4> m{};
	std::array<mode_role, 4> r{};
	for (unsigned i = 0; i < 4; ++i)
	{
		m[i].pid = static_cast<uint8_t>(i);
		m[i].bot = true;
	}
	m[0].orbs = 6;
	m[1].to_loaded = 300;
	m[2].to_loaded = 100;
	m[3].bot = false;
	m[3].to_loaded = 10;
	/* Team hoard of four: the nearest bot escorts the loaded one. */
	assign_hoard_roles(true, 4, m, r);
	CHECK(r[0] == mode_role::collect);
	CHECK(r[2] == mode_role::escort);
	CHECK(r[1] == mode_role::collect);
	CHECK(r[3] == mode_role::none);
	/* Free for all: no escort. */
	assign_hoard_roles(false, 4, m, r);
	CHECK(r[2] == mode_role::collect);
	/* Scoring bots score. */
	m[0].scoring = true;
	assign_hoard_roles(true, 4, m, r);
	CHECK(r[0] == mode_role::score);
	/* Nobody loaded: no escort. */
	m[0].orbs = 2;
	assign_hoard_roles(true, 4, m, r);
	CHECK(std::none_of(r.begin(), r.end(), [](const mode_role x) { return x == mode_role::escort; }));
}

void test_priorities()
{
	CHECK(carrier_priority(mode_role::hunt) > carrier_priority(mode_role::attack));
	CHECK(carrier_priority(mode_role::attack) > 1);
	CHECK(orb_carrier_priority(0) == 1);
	CHECK(orb_carrier_priority(6) > orb_carrier_priority(2));
	/* The target score takes the weight. */
	target_candidate c;
	c.visible = true;
	c.distance = 100;
	const double plain{target_score(c, 300)};
	c.priority = CARRIER_PRIORITY_HUNTER;
	CHECK(target_score(c, 300) > 3 * plain);
	CHECK(std::string_view{name_of(mode_role::retrieve)} == "retrieve");
}

}

int main()
{
	test_quota_home();
	test_nearest();
	test_hysteresis();
	test_humans();
	test_hunt();
	test_carry();
	test_retrieve();
	test_dead();
	test_standard_sizes();
	test_sweep();
	test_objectives();
	test_hoard();
	test_hoard_roles();
	test_priorities();
	std::puts("test-bot-modes: all checks passed");
	return 0;
}
