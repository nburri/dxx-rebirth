/*
 * This file is part of the DXX-Rebirth project <https://github.com/dxx-rebirth/dxx-rebirth/>.
 * It is copyright by its individual contributors, as recorded in the
 * project's Git history.  See COPYING.txt at the top level for license
 * terms and a link to the Git history.
 */
/*
 * Test of the bots' secondary weapons and items (bot_weapons.h,
 * Documentation/multiplayer-bots.md sections 4.5, 4.7 and 9.4, stage
 * B4): which missile or mine a bot fires by skill, target and range, the
 * blast safety of mega and earthshaker (never at point blank, never into
 * a wall next to the bot), the release, the converter, the cloak and
 * invulnerability tactics and the dodge of a homing missile.
 *
 * Build and run with SCons:
 *
 *	scons sdl2=1 d1x=0 d2x=1 register_runtime_test_plain_link_targets=1 test-bot-weapons
 *	build/common/test-bot-weapons
 */

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <initializer_list>

#include "bot_weapons.h"

using namespace dcx::bot;

namespace {

void check_failed(const char *const expr, const char *const file, const int line)
{
	std::fprintf(stderr, "%s:%d: check failed: %s\n", file, line, expr);
	std::exit(1);
}

#define CHECK(cond)	do { if (!(cond)) check_failed(#cond, __FILE__, __LINE__); } while (0)

constexpr unsigned idx(const secondary s)
{
	return static_cast<unsigned>(s);
}

/* Blast radii of the order of the game's (the game passes its own). */
constexpr double MEGA_BLAST{40};
constexpr double SHAKER_BLAST{45};

/* A bot with one of each, a visible target 80 units away crossing
 * slowly, nothing fired yet.
 */
missile_situation armed(const unsigned smarts = 2)
{
	missile_situation m;
	for (auto &a : m.ammo)
		a = 2;
	m.data[idx(secondary::mega)].blast_radius = MEGA_BLAST;
	m.data[idx(secondary::earthshaker)].blast_radius = SHAKER_BLAST;
	m.data[idx(secondary::smart)].blast_radius = 20;
	m.data[idx(secondary::concussion)].blast_radius = 10;
	m.smarts = smarts;
	m.has_target = true;
	m.target_visible = true;
	m.shot_clear = true;
	m.target_distance = 80;
	m.target_lateral_speed = 10;
	m.target_seen_ago = 0;
	return m;
}

missile_situation only(missile_situation m, const std::initializer_list<secondary> keep)
{
	for (auto &a : m.ammo)
		a = 0;
	for (const auto s : keep)
		m.ammo[idx(s)] = 1;
	return m;
}

void test_roles_and_skills()
{
	CHECK(role_of(secondary::concussion) == missile_role::straight);
	CHECK(role_of(secondary::mercury) == missile_role::straight);
	CHECK(role_of(secondary::homing) == missile_role::homing);
	CHECK(role_of(secondary::smart) == missile_role::smart);
	CHECK(role_of(secondary::mega) == missile_role::heavy);
	CHECK(role_of(secondary::earthshaker) == missile_role::shaker);
	CHECK(role_of(secondary::flash) == missile_role::flash);
	CHECK(role_of(secondary::proximity) == missile_role::mine);
	CHECK(role_of(secondary::smart_mine) == missile_role::mine);
	/* The guided missile is never fired (its steering and camera are the
	 * human's), and worth little to collect.
	 */
	CHECK(role_of(secondary::guided) == missile_role::none);
	CHECK(min_smarts(secondary::guided) > 4);
	CHECK(!missile_release(secondary::guided, 0, 1, 1000, 0, false));
	CHECK(secondary_value(idx(secondary::guided)) < 0.5);
	/* Section 5.1: Trainee fires none; Rookie concussion, homing, flash,
	 * mercury; Hotshot all the rest.
	 */
	for (unsigned i = 0; i < BOT_SECONDARY_COUNT; ++i)
		CHECK(min_smarts(static_cast<secondary>(i)) >= 1);
	CHECK(min_smarts(secondary::concussion) == 1 && min_smarts(secondary::homing) == 1);
	CHECK(min_smarts(secondary::smart) == 2 && min_smarts(secondary::proximity) == 2 && min_smarts(secondary::mega) == 2 && min_smarts(secondary::earthshaker) == 2);
	CHECK(!choose_secondary(armed(0)).has_value());
	/* The better the skill, the more often. */
	for (unsigned s = 1; s < 4; ++s)
	{
		CHECK(missile_interval(s + 1) < missile_interval(s));
		CHECK(mine_interval(s + 1) < mine_interval(s));
	}
	CHECK(missile_interval(0) > 1e6);
}

void test_missile_choice()
{
	/* No target: nothing (mines aside). */
	auto m{armed()};
	m.has_target = false;
	CHECK(!choose_secondary(m));
	/* Not too often. */
	m = armed();
	m.since_missile = missile_interval(2) - 0.1;
	CHECK(!choose_secondary(m));
	/* Mid range, a slow crosser: the heavy missile first (a clear shot,
	 * far enough), then smart.
	 */
	m = armed();
	CHECK(choose_secondary(m) == secondary::mega);
	m.heavy_used_on_target = true;
	CHECK(choose_secondary(m) == secondary::smart);
	/* Straight missiles: mercury before concussion; homing against a
	 * fast crosser or far away.
	 */
	auto s{only(armed(), {secondary::concussion, secondary::mercury, secondary::homing})};
	CHECK(choose_secondary(s) == secondary::mercury);
	s.ammo[idx(secondary::mercury)] = 0;
	CHECK(choose_secondary(s) == secondary::concussion);
	s.target_lateral_speed = 45;
	CHECK(choose_secondary(s) == secondary::homing);
	s.target_lateral_speed = 10;
	s.target_distance = 150;
	CHECK(choose_secondary(s) == secondary::homing);
	/* Homing needs some distance (it must turn), straight ones too. */
	s.target_distance = 35;
	s.target_lateral_speed = 45;
	CHECK(choose_secondary(s) == secondary::concussion);
	s.target_distance = 20;
	CHECK(!choose_secondary(s));
	/* Beyond reach: nothing. */
	s.target_distance = 300;
	CHECK(!choose_secondary(s));
	/* No clear shot, not visible: nothing straight. */
	s.target_distance = 80;
	s.shot_clear = false;
	CHECK(!choose_secondary(s));
	/* Flash: at a target that faces the bot, within 100. */
	auto f{only(armed(), {secondary::flash, secondary::concussion})};
	CHECK(choose_secondary(f) == secondary::concussion);
	f.target_facing = true;
	CHECK(choose_secondary(f) == secondary::flash);
	f.target_distance = 120;
	CHECK(choose_secondary(f) == secondary::concussion);
	/* Smart: within 120, also round a corner seen within a second. */
	auto sm{only(armed(), {secondary::smart})};
	CHECK(choose_secondary(sm) == secondary::smart);
	sm.target_distance = 130;
	CHECK(!choose_secondary(sm));
	sm.target_distance = 90;
	sm.target_visible = sm.shot_clear = false;
	sm.target_seen_ago = 0.5;
	CHECK(choose_secondary(sm) == secondary::smart);
	sm.target_seen_ago = 2;
	CHECK(!choose_secondary(sm));
	/* Rookie: concussion and homing, never smart or heavy ones. */
	auto r{armed(1)};
	r.since_missile = 1e9;
	const auto rc{choose_secondary(r)};
	CHECK(rc && min_smarts(*rc) <= 1);
}

/* Section 9.4: never a heavy missile at point blank or with its blast
 * reaching the bot.
 */
void test_heavy_safety()
{
	auto m{only(armed(), {secondary::mega, secondary::concussion})};
	/* Point blank: not mega (concussion instead). */
	for (double d = 0; d < HEAVY_MIN_DISTANCE; d += 5)
	{
		m.target_distance = d;
		CHECK(choose_secondary(m) != secondary::mega);
	}
	m.target_distance = 100;
	CHECK(choose_secondary(m) == secondary::mega);
	/* A big blast needs more room: 1.5 radii and the margin. */
	m.data[idx(secondary::mega)].blast_radius = 70;
	CHECK(choose_secondary(m) != secondary::mega);
	m.target_distance = 1.5 * 70 + BLAST_MARGIN;
	CHECK(choose_secondary(m) == secondary::mega);
	m.data[idx(secondary::mega)].blast_radius = MEGA_BLAST;
	/* A fast crosser dodges it: Hotshot waits, Ace fires. */
	m.target_distance = 100;
	m.target_lateral_speed = 45;
	CHECK(choose_secondary(m) != secondary::mega);
	m.smarts = 3;
	CHECK(choose_secondary(m) == secondary::mega);
	m.smarts = 2;
	m.target_lateral_speed = 10;
	/* Once per target, and not too often. */
	m.heavy_used_on_target = true;
	CHECK(choose_secondary(m) != secondary::mega);
	m.heavy_used_on_target = false;
	m.since_heavy = HEAVY_INTERVAL - 1;
	CHECK(choose_secondary(m) != secondary::mega);
	m.since_heavy = 1e9;
	/* The earthshaker: further still (2 blast radii), and before mega. */
	auto e{only(armed(), {secondary::earthshaker, secondary::mega})};
	e.target_distance = 100;
	CHECK(choose_secondary(e) == secondary::mega);
	e.target_distance = std::max(SHAKER_MIN_DISTANCE, 2 * SHAKER_BLAST + BLAST_MARGIN) + 1;
	CHECK(choose_secondary(e) == secondary::earthshaker);
	e.data[idx(secondary::earthshaker)].blast_radius = 80;
	CHECK(choose_secondary(e) == secondary::mega);
	/* Release: the wall along the nose counts, not only the target.  The
	 * user's own death: an earthshaker fired at a far target with a wall
	 * right in front.
	 */
	CHECK(missile_release(secondary::earthshaker, 0, 0.1, 2 * SHAKER_BLAST + BLAST_MARGIN, SHAKER_BLAST, false));
	for (double wall = 0; wall < 2 * SHAKER_BLAST + BLAST_MARGIN; wall += 5)
		CHECK(!missile_release(secondary::earthshaker, 0, 0.1, wall, SHAKER_BLAST, false));
	for (double wall = 0; wall < 1.5 * MEGA_BLAST + BLAST_MARGIN; wall += 5)
		CHECK(!missile_release(secondary::mega, 0, 0.1, wall, MEGA_BLAST, false));
	CHECK(missile_release(secondary::mega, 0, 0.1, 200, MEGA_BLAST, false));
	/* Every missile keeps its own blast off the bot. */
	CHECK(!missile_release(secondary::concussion, 0, 0.1, 5, 10, false));
	CHECK(missile_release(secondary::concussion, 0, 0.1, 30, 10, false));
	/* Invulnerable: only point blank is avoided. */
	CHECK(missile_release(secondary::mega, 0, 0.1, 40, MEGA_BLAST, true));
	CHECK(!missile_release(secondary::mega, 0, 0.1, 10, MEGA_BLAST, true));
	/* Cloaked: no heavy missile (it shows where the bot is), nothing
	 * beyond 100.
	 */
	auto c{armed()};
	c.cloaked = true;
	const auto cc{choose_secondary(c)};
	CHECK(cc && *cc != secondary::mega && *cc != secondary::earthshaker);
	c.target_distance = 150;
	CHECK(!choose_secondary(c));
}

void test_release()
{
	const double cone{radians(6)};
	/* Straight ones need the fire cone, homing and smart a wider one. */
	CHECK(missile_release(secondary::concussion, radians(5), cone, 100, 10, false));
	CHECK(!missile_release(secondary::concussion, radians(8), cone, 100, 10, false));
	CHECK(missile_release(secondary::homing, radians(15), cone, 100, 10, false));
	CHECK(!missile_release(secondary::homing, radians(25), cone, 100, 10, false));
	CHECK(missile_release(secondary::smart, radians(25), cone, 100, 20, false));
	CHECK(missile_cone(missile_role::straight, cone) == cone);
	CHECK(missile_cone(missile_role::homing, radians(30)) == radians(30));
	/* Mines: at once. */
	CHECK(missile_release(secondary::proximity, 3, cone, 0, 0, false));
	CHECK(missile_release(secondary::smart_mine, 3, cone, 0, 0, false));
	/* Aimed with the missile's speed: the ones that fly straight. */
	CHECK(missile_aimed(secondary::concussion) && missile_aimed(secondary::mercury) && missile_aimed(secondary::mega) && missile_aimed(secondary::earthshaker) && missile_aimed(secondary::flash));
	CHECK(!missile_aimed(secondary::homing) && !missile_aimed(secondary::smart) && !missile_aimed(secondary::proximity) && !missile_aimed(secondary::guided));
}

void test_mines()
{
	auto m{armed()};
	m.has_target = false;
	m.chased = true;
	m.pursuer_distance = 60;
	/* Chased: a mine, the smart mine first. */
	CHECK(choose_secondary(m) == secondary::smart_mine);
	m.ammo[idx(secondary::smart_mine)] = 0;
	CHECK(choose_secondary(m) == secondary::proximity);
	/* Not too often; not for a pursuer far behind (except at a
	 * doorway, which it must pass).
	 */
	m.since_mine = mine_interval(2) - 0.5;
	CHECK(!choose_secondary(m));
	m.since_mine = 1e9;
	m.pursuer_distance = 130;
	CHECK(!choose_secondary(m));
	m.at_doorway = true;
	CHECK(choose_secondary(m) == secondary::proximity);
	m.pursuer_distance = 200;
	CHECK(!choose_secondary(m));
	/* Not chased: no mine. */
	m.pursuer_distance = 50;
	m.chased = false;
	CHECK(!choose_secondary(m));
	/* Rookie: no mines. */
	auto r{armed(1)};
	r = only(r, {secondary::proximity});
	r.has_target = false;
	r.chased = true;
	r.pursuer_distance = 50;
	CHECK(!choose_secondary(r));
}

void test_converter_and_tactics()
{
	/* Only the energy above 100 converts; by skill below which shields. */
	CHECK(!want_convert(40, 100, 4));
	CHECK(want_convert(40, 150, 2));
	CHECK(!want_convert(90, 150, 2));
	CHECK(want_convert(90, 150, 3));
	CHECK(!want_convert(40, 150, 0));
	CHECK(!want_convert(200, 200, 4));
	for (unsigned s = 1; s < 4; ++s)
		for (double sh = 0; sh < 200; sh += 5)
			CHECK(!want_convert(sh, 150, s) || want_convert(sh, 150, s + 1));
	/* Invulnerable: aggressive; cloaked: sneaks close, holds long fire. */
	const auto none{tactics_for(false, false)};
	CHECK(none.engage_weight == 1 && none.collect_weight == 1 && none.range_scale == 1 && none.max_fire_distance > 1e6);
	const auto inv{tactics_for(false, true)};
	CHECK(inv.engage_weight > 1 && inv.collect_weight < 1 && inv.range_scale < 1);
	const auto cl{tactics_for(true, false)};
	CHECK(cl.engage_weight > 1 && cl.range_scale < 1 && cl.max_fire_distance == CLOAKED_FIRE_DISTANCE);
	const auto both{tactics_for(true, true)};
	CHECK(both.engage_weight >= inv.engage_weight && both.range_scale <= cl.range_scale && both.max_fire_distance == CLOAKED_FIRE_DISTANCE);
	/* A homing missile after the bot: a wider pass, dodged more often,
	 * never for a bot that does not dodge.
	 */
	CHECK(dodge_radius(7, true) > dodge_radius(7, false));
	CHECK(dodge_radius(7, false) == 7);
	CHECK(dodge_chance(0.45, true) > dodge_chance(0.45, false));
	CHECK(dodge_chance(0.9, true) <= 1);
	CHECK(dodge_chance(0, true) == 0);
}

}

int main()
{
	test_roles_and_skills();
	test_missile_choice();
	test_heavy_safety();
	test_release();
	test_mines();
	test_converter_and_tactics();
	std::puts("test-bot-weapons: all checks passed");
	return 0;
}
