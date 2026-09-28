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

/* A missile of this blast radius at the speed of the game's. */
constexpr missile_data blast(const double radius)
{
	return {.speed = 160, .blast_radius = radius, .thrust = true};
}

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
	CHECK(!missile_release(secondary::guided, 0, 1, 1000, blast(0), 0));
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
	 * far enough: 80 units is beyond the earthshaker's 1.5 blast radii
	 * and the margin, section 9.5), then smart.
	 */
	m = armed();
	CHECK(choose_secondary(m) == secondary::earthshaker);
	m.ammo[idx(secondary::earthshaker)] = 0;
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
	const double mega_min{heavy_min_distance(secondary::mega, blast(MEGA_BLAST))};
	/* Section 9.5: 1.2 blast radii and the margin (B4: 1.5), at least
	 * HEAVY_MIN_DISTANCE.
	 */
	CHECK(mega_min == std::max(HEAVY_MIN_DISTANCE, 1.2 * MEGA_BLAST + BLAST_MARGIN));
	/* Point blank: not mega (concussion instead). */
	for (double d = 0; d < mega_min; d += 5)
	{
		m.target_distance = d;
		CHECK(choose_secondary(m) != secondary::mega);
		CHECK(heavy_check(m) == (d < HEAVY_MIN_DISTANCE ? heavy_verdict::too_close : heavy_verdict::blast));
	}
	m.target_distance = mega_min;
	CHECK(choose_secondary(m) == secondary::mega);
	CHECK(heavy_check(m) == heavy_verdict::fire);
	/* In a fight's band (35-95 units) the mega is fired now (B4: from 70
	 * units, at a crosser below 30 units/s).
	 */
	m.target_distance = 70;
	m.target_lateral_speed = 40;
	CHECK(choose_secondary(m) == secondary::mega);
	/* A big blast needs more room. */
	m.target_distance = 100;
	m.data[idx(secondary::mega)].blast_radius = 80;
	CHECK(choose_secondary(m) != secondary::mega);
	CHECK(heavy_check(m) == heavy_verdict::blast);
	m.target_distance = 1.2 * 80 + BLAST_MARGIN;
	CHECK(choose_secondary(m) == secondary::mega);
	m.data[idx(secondary::mega)].blast_radius = MEGA_BLAST;
	/* A fast crosser dodges one that does not home: Hotshot waits, Ace
	 * fires; a homing one is fired at it.
	 */
	m.target_distance = 100;
	m.target_lateral_speed = HEAVY_MAX_LATERAL + 5;
	CHECK(choose_secondary(m) != secondary::mega);
	CHECK(heavy_check(m) == heavy_verdict::too_fast);
	m.smarts = 3;
	CHECK(choose_secondary(m) == secondary::mega);
	m.smarts = 2;
	m.data[idx(secondary::mega)].homing = true;
	CHECK(choose_secondary(m) == secondary::mega);
	m.data[idx(secondary::mega)].homing = false;
	m.target_lateral_speed = 10;
	/* Once per target, and not too often. */
	m.heavy_used_on_target = true;
	CHECK(choose_secondary(m) != secondary::mega);
	CHECK(heavy_check(m) == heavy_verdict::used_on_target);
	m.heavy_used_on_target = false;
	m.since_heavy = HEAVY_INTERVAL - 1;
	CHECK(choose_secondary(m) != secondary::mega);
	CHECK(heavy_check(m) == heavy_verdict::cooldown);
	m.since_heavy = 1e9;
	/* The verdicts of the other rules. */
	{
		auto v{m};
		v.target_visible = false;
		CHECK(heavy_check(v) == heavy_verdict::not_visible);
		v = m;
		v.shot_clear = false;
		CHECK(heavy_check(v) == heavy_verdict::no_clear_shot);
		v = m;
		v.has_target = false;
		CHECK(heavy_check(v) == heavy_verdict::no_target);
		v = m;
		v.smarts = 1;
		CHECK(heavy_check(v) == heavy_verdict::skill);
		v = m;
		v.ammo[idx(secondary::mega)] = 0;
		CHECK(heavy_check(v) == heavy_verdict::none_owned);
		v = m;
		v.target_distance = HEAVY_MAX_DISTANCE + 1;
		CHECK(heavy_check(v) == heavy_verdict::too_far);
		for (unsigned i = 0; i < heavy_verdict_names.size(); ++i)
			CHECK(name_of(static_cast<heavy_verdict>(i)) != nullptr);
	}
	/* The earthshaker: further (1.5 blast radii), and before mega. */
	auto e{only(armed(), {secondary::earthshaker, secondary::mega})};
	const double shaker_min{heavy_min_distance(secondary::earthshaker, blast(SHAKER_BLAST))};
	CHECK(shaker_min == std::max(SHAKER_MIN_DISTANCE, 1.5 * SHAKER_BLAST + BLAST_MARGIN));
	CHECK(shaker_min < 95);
	e.target_distance = shaker_min - 1;
	CHECK(choose_secondary(e) == secondary::mega);
	e.target_distance = shaker_min + 1;
	CHECK(choose_secondary(e) == secondary::earthshaker);
	e.data[idx(secondary::earthshaker)].blast_radius = 80;
	CHECK(choose_secondary(e) == secondary::mega);
	e.data[idx(secondary::earthshaker)].blast_radius = SHAKER_BLAST;
	/* The playtest: a bot with an earthshaker in a close fight (60
	 * units): not yet; it backs off to the distance it needs
	 * (heavy_standoff), then fires.
	 */
	auto close{only(armed(), {secondary::earthshaker})};
	close.target_distance = 60;
	close.target_lateral_speed = 40;
	CHECK(!choose_secondary(close));
	CHECK(heavy_check(close) == heavy_verdict::blast);
	const double keep{heavy_standoff(close)};
	CHECK(keep > shaker_min && keep < shaker_min + 10);
	close.target_distance = keep;
	CHECK(choose_secondary(close) == secondary::earthshaker);
	CHECK(heavy_standoff(close) == keep);
	/* No standoff for a missile it may not fire anyway. */
	close.heavy_used_on_target = true;
	CHECK(heavy_standoff(close) == 0);
	close.heavy_used_on_target = false;
	close.ammo[idx(secondary::earthshaker)] = 0;
	CHECK(heavy_standoff(close) == 0);
	/* Release: the wall along the nose counts, not only the target.  The
	 * user's own death: an earthshaker fired at a far target with a wall
	 * right in front.
	 */
	const double shaker_blast_min{1.5 * SHAKER_BLAST + BLAST_MARGIN};
	CHECK(missile_release(secondary::earthshaker, 0, 0.1, shaker_blast_min, blast(SHAKER_BLAST), 0));
	for (double wall = 0; wall < shaker_blast_min; wall += 5)
		CHECK(!missile_release(secondary::earthshaker, 0, 0.1, wall, blast(SHAKER_BLAST), 0));
	for (double wall = 0; wall < 1.2 * MEGA_BLAST + BLAST_MARGIN; wall += 5)
		CHECK(!missile_release(secondary::mega, 0, 0.1, wall, blast(MEGA_BLAST), 0));
	/* The blast stays outside its radius (no damage there). */
	CHECK(shaker_blast_min > SHAKER_BLAST + 10 && 1.2 * MEGA_BLAST + BLAST_MARGIN > MEGA_BLAST + 10);
	CHECK(missile_release(secondary::mega, 0, 0.1, 200, blast(MEGA_BLAST), 0));
	/* A homing heavy missile is released within a wider cone. */
	auto homing_mega{blast(MEGA_BLAST)};
	homing_mega.homing = true;
	CHECK(!missile_release(secondary::mega, radians(12), radians(6), 200, blast(MEGA_BLAST), 0));
	CHECK(missile_release(secondary::mega, radians(12), radians(6), 200, homing_mega, 0));
	/* Every missile keeps its own blast off the bot. */
	CHECK(!missile_release(secondary::concussion, 0, 0.1, 5, blast(10), 0));
	CHECK(missile_release(secondary::concussion, 0, 0.1, 30, blast(10), 0));
	/* Invulnerable: only point blank is avoided. */
	CHECK(missile_release(secondary::mega, 0, 0.1, 40, blast(MEGA_BLAST), 30));
	CHECK(!missile_release(secondary::mega, 0, 0.1, 10, blast(MEGA_BLAST), 30));
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

/* Invulnerability relaxes the blast safety only while it outlasts the
 * danger: the missile's flight (at half its speed, a thrust missile),
 * the earthshaker's children (2 s) and a spare.  None left (not
 * invulnerable, or the faked respawn invulnerability, which the caller
 * passes as 0): the full rule.
 */
void test_invulnerable_blast()
{
	const auto mega{blast(MEGA_BLAST)};
	const auto shaker{blast(SHAKER_BLAST)};
	/* 40 units at 80 units/s: 0.5 s, plus the spare. */
	CHECK(blast_danger_seconds(missile_role::heavy, 40, mega) == 40.0 / 80 + INVULNERABLE_SPARE);
	CHECK(blast_danger_seconds(missile_role::shaker, 40, shaker) == 40.0 / 80 + SHAKER_CHILDREN_SECONDS + INVULNERABLE_SPARE);
	/* A missile without thrust flies at its full speed. */
	CHECK(blast_danger_seconds(missile_role::heavy, 40, missile_data{.speed = 160, .blast_radius = MEGA_BLAST, .thrust = false}) == 40.0 / 160 + INVULNERABLE_SPARE);
	/* Faked (0): never at 30-40 units. */
	CHECK(!missile_release(secondary::mega, 0, 0.1, 40, mega, 0));
	CHECK(!missile_release(secondary::earthshaker, 0, 0.1, 40, shaker, 0));
	/* Nearly expired: the mega's blast would outlast it. */
	CHECK(!missile_release(secondary::mega, 0, 0.1, 40, mega, 0.9));
	CHECK(missile_release(secondary::mega, 0, 0.1, 40, mega, 1.1));
	/* The earthshaker needs 2 s more for its children. */
	CHECK(!missile_release(secondary::earthshaker, 0, 0.1, 40, shaker, 1.1));
	CHECK(!missile_release(secondary::earthshaker, 0, 0.1, 40, shaker, 2.9));
	CHECK(missile_release(secondary::earthshaker, 0, 0.1, 40, shaker, 3.1));
	/* Point blank stays out whatever is left. */
	CHECK(!missile_release(secondary::earthshaker, 0, 0.1, 20, shaker, 30));
	/* The choice follows: a mega of blast 60 at 70 units is short of
	 * its normal 1.5 blast radii + margin; invulnerable, it needs
	 * 70 / 80 s + the spare of cover.
	 */
	auto m{only(armed(), {secondary::mega})};
	m.target_distance = HEAVY_MIN_DISTANCE;
	m.data[idx(secondary::mega)] = blast(60);
	CHECK(!choose_secondary(m));
	m.invulnerable_left = 1;
	CHECK(!choose_secondary(m));
	m.invulnerable_left = 30;
	CHECK(choose_secondary(m) == secondary::mega);
}

void test_release()
{
	const double cone{radians(6)};
	/* Straight ones need the fire cone, homing and smart a wider one. */
	CHECK(missile_release(secondary::concussion, radians(5), cone, 100, blast(10), 0));
	CHECK(!missile_release(secondary::concussion, radians(8), cone, 100, blast(10), 0));
	CHECK(missile_release(secondary::homing, radians(15), cone, 100, blast(10), 0));
	CHECK(!missile_release(secondary::homing, radians(25), cone, 100, blast(10), 0));
	CHECK(missile_release(secondary::smart, radians(25), cone, 100, blast(20), 0));
	CHECK(missile_cone(missile_role::straight, cone) == cone);
	CHECK(missile_cone(missile_role::homing, radians(30)) == radians(30));
	/* Mines: at once. */
	CHECK(missile_release(secondary::proximity, 3, cone, 0, blast(0), 0));
	CHECK(missile_release(secondary::smart_mine, 3, cone, 0, blast(0), 0));
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
	/* A teammate following on the route: no mine. */
	auto t{armed()};
	t.has_target = false;
	t.chased = true;
	t.pursuer_distance = 50;
	CHECK(choose_secondary(t) == secondary::smart_mine);
	t.teammate_behind = true;
	CHECK(!choose_secondary(t));
	t.at_doorway = true;
	CHECK(!choose_secondary(t));
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

namespace {

/* Section 9.5: the fusion cannon's charge and release, and omega's cone. */
void test_fusion_and_omega()
{
	const double release{fusion_release_charge(2)};
	CHECK(release > 0.5 && release < FUSION_MAX_CHARGE);
	CHECK(FUSION_MAX_CHARGE < 2);	// from 2 s on the charge hurts the ship
	for (unsigned s = 0; s < 4; ++s)
		CHECK(fusion_release_charge(s + 1) >= fusion_release_charge(s));
	fusion_view v{
		.selected = true,
		.charging = false,
		.charge = 0,
		.energy = 100,
		.target_visible = true,
		.shot_clear = true,
		.aimed = false,
		.distance = 60,
		.range = 200,
	};
	/* A target in sight, in range: start charging, even before the aim is on it. */
	CHECK(fusion_step(v, release) == fusion_action::charge);
	/* Not selected, too little energy, no target, out of range: idle. */
	auto w{v};
	w.selected = false;
	CHECK(fusion_step(w, release) == fusion_action::idle);
	w = v;
	w.energy = FUSION_MIN_ENERGY - 1;
	CHECK(fusion_step(w, release) == fusion_action::idle);
	w = v;
	w.target_visible = false;
	CHECK(fusion_step(w, release) == fusion_action::idle);
	w = v;
	w.shot_clear = false;
	CHECK(fusion_step(w, release) == fusion_action::idle);
	w = v;
	w.distance = 300;
	CHECK(fusion_step(w, release) == fusion_action::idle);
	/* Charging: hold until the skill's charge with the aim on target. */
	v.charging = true;
	v.charge = release - 0.1;
	v.aimed = true;
	CHECK(fusion_step(v, release) == fusion_action::charge);
	v.charge = release;
	CHECK(fusion_step(v, release) == fusion_action::release);
	v.aimed = false;
	CHECK(fusion_step(v, release) == fusion_action::charge);
	/* The target lost: keep the charge while it may come back, release
	 * at the limit (never into the self-damage).
	 */
	v.target_visible = false;
	CHECK(fusion_step(v, release) == fusion_action::charge);
	v.charge = FUSION_MAX_CHARGE;
	CHECK(fusion_step(v, release) == fusion_action::release);
	/* Out of energy or switched away: release what is charged. */
	v.charge = 0.5;
	v.energy = 0;
	CHECK(fusion_step(v, release) == fusion_action::release);
	v.energy = 50;
	v.selected = false;
	CHECK(fusion_step(v, release) == fusion_action::release);
	/* Simulated: charging at 60 Hz with the aim on target from 0.3 s,
	 * the shot goes at the release charge, within a tick.
	 */
	fusion_view sim{v};
	sim.selected = true;
	sim.charging = false;
	sim.charge = 0;
	sim.target_visible = true;
	double t{0};
	for (unsigned k = 0; k < 600; ++k, t += 1.0 / 60)
	{
		sim.aimed = t >= 0.3;
		const auto a{fusion_step(sim, release)};
		if (a == fusion_action::release)
			break;
		if (a == fusion_action::charge)
		{
			sim.charging = true;
			sim.charge += 1.0 / 60;
		}
	}
	CHECK(sim.charge >= release && sim.charge < release + 2.0 / 60);
	/* Omega: the cone of its lock, at least 18 degrees. */
	CHECK(omega_fire_cone(radians(6)) >= radians(18) - 1e-9);
	CHECK(omega_fire_cone(radians(25)) == radians(25));
}

/* Section 9.5: a narrow corridor.  The blast is judged at the impact
 * along the nose (the target if nearer than the wall behind it) and
 * where the bot is when the missile bursts; never at a wall in front.
 * The same rules as the game's: no burst within its radius of the bot.
 */
void test_corridor_shaker()
{
	const auto shaker{blast(SHAKER_BLAST)};
	const double cone{radians(6)};
	/* A target 120 units down a corridor, the wall behind it at 300:
	 * the impact is the target.  A valid shot, and chosen.
	 */
	const double impact{std::min(300.0, 120.0)};
	CHECK(missile_release(secondary::earthshaker, 0, cone, impact, shaker, 0));
	auto m{only(armed(), {secondary::earthshaker})};
	m.target_distance = 120;
	m.target_lateral_speed = 20;
	CHECK(choose_secondary(m) == secondary::earthshaker);
	/* A wall 30 units ahead (the target round a bend beyond it): never. */
	CHECK(!missile_release(secondary::earthshaker, 0, cone, std::min(30.0, 120.0), shaker, 0));
	/* The bot flying fast toward the impact: judged where it is at the
	 * burst.  At 85 units closing at 55 units/s it would be inside the
	 * blast when the missile (80 units/s at first) bursts.
	 */
	CHECK(missile_release(secondary::earthshaker, 0, cone, 85, shaker, 0));
	CHECK(distance_at_burst(85, shaker, 55) < 1.5 * SHAKER_BLAST + BLAST_MARGIN);
	CHECK(!missile_release(secondary::earthshaker, 0, cone, 85, shaker, 0, 55));
	m.target_distance = 85;
	m.closing_speed = 55;
	CHECK(heavy_check(m) == heavy_verdict::blast);
	/* Backing off is credited, a little, at the burst; the impact itself
	 * must be outside the blast when it is fired.
	 */
	CHECK(distance_at_burst(75, shaker, -40) == 75 + CLOSING_AWAY_CREDIT * 75 / 80);
	CHECK(missile_release(secondary::earthshaker, 0, cone, 85, shaker, 0, -40));
	CHECK(!missile_release(secondary::earthshaker, 0, cone, 75, shaker, 0, -40));
	/* No self-kill: whatever the distances and speeds, a released heavy
	 * missile bursts outside its blast radius of the bot, where the bot
	 * is at the burst (up to the credited 20 units/s away).
	 */
	for (const auto s : {secondary::earthshaker, secondary::mega})
		for (double r = 20; r <= 90; r += 10)
			for (double d = 0; d <= 300; d += 5)
				for (double closing = -60; closing <= 60; closing += 10)
				{
					const auto md{blast(r)};
					if (missile_release(s, 0, cone, d, md, 0, closing))
					{
						CHECK(d > r);
						CHECK(distance_at_burst(d, md, std::max(closing, 0.0)) > r);
					}
				}
}

}

int main()
{
	test_roles_and_skills();
	test_missile_choice();
	test_heavy_safety();
	test_invulnerable_blast();
	test_fusion_and_omega();
	test_corridor_shaker();
	test_release();
	test_mines();
	test_converter_and_tactics();
	std::puts("test-bot-weapons: all checks passed");
	return 0;
}
