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
 * invulnerability tactics and the dodge of a homing missile; section
 * 9.6: the expected outcome of a heavy missile by risk profile, the
 * indirect aim points on synthetic corridors and corners, hugging and
 * ducking.
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
#include <span>
#include <string_view>
#include <vector>

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
	 * far enough: 80 units is beyond the earthshaker's 1.2 blast radii
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
	/* Section 9.5: 1 blast radius and the margin (B4: 1.5), at least
	 * HEAVY_MIN_DISTANCE.
	 */
	CHECK(mega_min == std::max(HEAVY_MIN_DISTANCE, blast_factor(missile_role::heavy) * MEGA_BLAST + BLAST_MARGIN));
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
	m.data[idx(secondary::mega)].blast_radius = 100;
	CHECK(choose_secondary(m) != secondary::mega);
	CHECK(heavy_check(m) == heavy_verdict::blast);
	m.target_distance = blast_factor(missile_role::heavy) * 100 + BLAST_MARGIN;
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
	/* ... unless the blast covers its crossing during the flight (a
	 * blast of 60, 80 units away at 120 units/s: 0.67 s, 33 units).
	 */
	m.data[idx(secondary::mega)].blast_radius = 60;
	m.target_distance = 80;
	CHECK(heavy_check(m) == heavy_verdict::fire);
	m.data[idx(secondary::mega)].blast_radius = MEGA_BLAST;
	m.target_distance = 100;
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
	/* The earthshaker: further (1.2 blast radii), and before mega. */
	auto e{only(armed(), {secondary::earthshaker, secondary::mega})};
	const double shaker_min{heavy_min_distance(secondary::earthshaker, blast(SHAKER_BLAST))};
	CHECK(shaker_min == std::max(SHAKER_MIN_DISTANCE, blast_factor(missile_role::shaker) * SHAKER_BLAST + BLAST_MARGIN));
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
	const double shaker_blast_min{blast_factor(missile_role::shaker) * SHAKER_BLAST + BLAST_MARGIN};
	CHECK(missile_release(secondary::earthshaker, 0, 0.1, shaker_blast_min, blast(SHAKER_BLAST), 0));
	for (double wall = 0; wall < shaker_blast_min; wall += 5)
		CHECK(!missile_release(secondary::earthshaker, 0, 0.1, wall, blast(SHAKER_BLAST), 0));
	for (double wall = 0; wall < blast_factor(missile_role::heavy) * MEGA_BLAST + BLAST_MARGIN; wall += 5)
		CHECK(!missile_release(secondary::mega, 0, 0.1, wall, blast(MEGA_BLAST), 0));
	/* The blast stays outside its radius (no damage there). */
	CHECK(shaker_blast_min > SHAKER_BLAST + 10 && blast_factor(missile_role::heavy) * MEGA_BLAST + BLAST_MARGIN > MEGA_BLAST + 10);
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
	CHECK(distance_at_burst(85, shaker, 55) < blast_factor(missile_role::shaker) * SHAKER_BLAST + BLAST_MARGIN);
	CHECK(!missile_release(secondary::earthshaker, 0, cone, 85, shaker, 0, 55));
	m.target_distance = 85;
	m.closing_speed = 55;
	CHECK(heavy_check(m) == heavy_verdict::blast);
	/* Backing off is credited, a little, at the burst; the impact itself
	 * must be outside the blast when it is fired.
	 */
	CHECK(distance_at_burst(75, shaker, -40) == 75 + CLOSING_AWAY_CREDIT * 75 / 80);
	CHECK(missile_release(secondary::earthshaker, 0, cone, 85, shaker, 0, -40));
	CHECK(!missile_release(secondary::earthshaker, 0, cone, 60, shaker, 0, -40));
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

/* Section 9.5: a target that rushes the bot meets the missile sooner and
 * nearer, at d s / (s + v) for a bot that holds still: the blast is
 * judged with both closing in, at human speeds (about 60 units/s, 90 and
 * more with the afterburner).
 */
void test_closing_target()
{
	const auto shaker{blast(SHAKER_BLAST)};
	const auto mega{blast(MEGA_BLAST)};
	const double cone{radians(6)};
	const double shaker_need{blast_factor(missile_role::shaker) * SHAKER_BLAST + BLAST_MARGIN};
	const double mega_need{blast_factor(missile_role::heavy) * MEGA_BLAST + BLAST_MARGIN};
	/* The formula: 90 units, the missile at 80 units/s, the target at
	 * 60: they meet at 90 * 80 / 140 from a bot that holds still.
	 */
	CHECK(std::fabs(distance_at_burst(90, shaker, 0, 60) - 90.0 * 80 / 140) < 1e-9);
	CHECK(distance_at_burst(90, shaker, 0, 0) == 90);
	/* A still bot, the target rushing it at 60: an earthshaker at 90
	 * units would burst inside its blast.  The bot's own speed alone
	 * (B4.1) passed it.
	 */
	CHECK(missile_release(secondary::earthshaker, 0, cone, 90, shaker, 0, 0));
	CHECK(!missile_release(secondary::earthshaker, 0, cone, 90, shaker, 0, 0, 60));
	auto m{only(armed(), {secondary::earthshaker})};
	m.target_distance = 90;
	CHECK(heavy_check(m) == heavy_verdict::fire);
	m.target_closing_speed = 60;
	CHECK(heavy_check(m) == heavy_verdict::blast);
	CHECK(!choose_secondary(m) || *choose_secondary(m) != secondary::earthshaker);
	/* Far enough, it is fired all the same. */
	m.target_distance = 150;
	CHECK(distance_at_burst(150, shaker, 0, 60) >= shaker_need);
	CHECK(heavy_check(m) == heavy_verdict::fire);
	/* A mega at 70 units with the target on its afterburner (90). */
	auto g{only(armed(), {secondary::mega})};
	g.target_distance = 70;
	CHECK(heavy_check(g) == heavy_verdict::fire);
	g.target_closing_speed = 90;
	CHECK(distance_at_burst(70, mega, 0, 90) < mega_need);
	CHECK(heavy_check(g) == heavy_verdict::blast);
	/* Both closing in at 50: nearer still. */
	CHECK(distance_at_burst(120, shaker, 50, 50) < distance_at_burst(120, shaker, 0, 50));
	/* A fleeing target is credited up to CLOSING_AWAY_CREDIT. */
	CHECK(distance_at_burst(80, shaker, 0, -60) == distance_at_burst(80, shaker, 0, -CLOSING_AWAY_CREDIT));
	CHECK(distance_at_burst(80, shaker, 0, -60) > 80);
	/* No self-kill against a rushing target: whatever the distances and
	 * speeds of both, a released heavy missile meets the target (flying
	 * straight at the missile) outside its blast radius of the bot, where
	 * the bot really is then.
	 */
	for (const auto s : {secondary::earthshaker, secondary::mega})
		for (double r = 20; r <= 90; r += 10)
			for (double d = 0; d <= 300; d += 5)
				for (double closing = -60; closing <= 60; closing += 15)
					for (double target = -60; target <= 120; target += 15)
					{
						const auto md{blast(r)};
						if (!missile_release(s, 0, cone, d, md, 0, closing, target))
							continue;
						const double speed{md.speed / 2};
						const double t{d / (speed + target)};
						CHECK(t > 0);
						CHECK(d - (closing + target) * t > r);
					}
}

/* Section 9.5: the earthshaker's children pass through the bot and burst
 * on the wall behind it: a wall close behind holds the release.
 */
void test_shaker_behind()
{
	auto shaker{blast(SHAKER_BLAST)};
	shaker.child_blast_radius = 30;
	const double need{SHAKER_BEHIND_FACTOR * 30 + BLAST_MARGIN};
	CHECK(!shaker_behind_safe(missile_role::shaker, need - 1, 120, shaker, 0));
	CHECK(shaker_behind_safe(missile_role::shaker, need, 120, shaker, 0));
	CHECK(shaker_behind_safe(missile_role::shaker, 400, 120, shaker, 0));
	/* The mega has no children; a missile without child data passes. */
	CHECK(shaker_behind_safe(missile_role::heavy, 0, 120, shaker, 0));
	CHECK(shaker_behind_safe(missile_role::shaker, 0, 120, blast(SHAKER_BLAST), 0));
	/* Invulnerable beyond the danger: waived. */
	CHECK(shaker_behind_safe(missile_role::shaker, 0, 120, shaker, 10));
	CHECK(!shaker_behind_safe(missile_role::shaker, 0, 120, shaker, 1));
}

/* Section 9.6: synthetic levels, a union of axis-aligned boxes (the
 * free space); casts march in small steps.
 */
struct box
{
	vec3 lo, hi;
};

struct box_level
{
	std::vector<box> boxes;
	[[nodiscard]]
	bool free(const vec3 &p) const
	{
		for (const auto &b : boxes)
			if (p.x >= b.lo.x && p.x <= b.hi.x && p.y >= b.lo.y && p.y <= b.hi.y && p.z >= b.lo.z && p.z <= b.hi.z)
				return true;
		return false;
	}
	[[nodiscard]]
	double cast(const vec3 &from, const vec3 &dir, const double limit) const
	{
		const auto u{normalized(dir)};
		constexpr double step{0.25};
		for (double t = 0; t < limit; t += step)
			if (!free(from + u * t))
				return std::max(t - step, 0.0);
		return limit;
	}
	[[nodiscard]]
	bool sees(const vec3 &a, const vec3 &b) const
	{
		const double d{distance(a, b)};
		return d < 0.01 || cast(a, b - a, d) >= d - 0.3;
	}
};

/* A mega and an earthshaker of the order of the game's (the real data
 * are not here; the log prints them at the first shot).
 */
missile_data test_mega()
{
	return {.speed = 160, .blast_radius = 50, .thrust = true, .homing = true, .damage = 150};
}

missile_data test_shaker()
{
	return {.speed = 160, .blast_radius = 60, .thrust = true, .homing = true, .child_blast_radius = 48, .damage = 200, .child_damage = 100, .children = SHAKER_CHILDREN};
}

/* A big room: nothing near anybody. */
box_level open_room()
{
	return {{{{-400, -400, -400}, {400, 400, 400}}}};
}

blast_scene still_scene(const vec3 &bot, const vec3 &target)
{
	blast_scene sc;
	sc.bot = bot;
	sc.target = target;
	sc.aim_sigma = 0.02;
	return sc;
}

void test_risk_profiles()
{
	const auto cautious{risk_profile_of(bot_skill::hotshot, bot_style::cautious)};
	const auto balanced{risk_profile_of(bot_skill::hotshot, bot_style::balanced)};
	const auto aggressive{risk_profile_of(bot_skill::hotshot, bot_style::aggressive)};
	const auto insane{risk_profile_of(bot_skill::insane, bot_style::aggressive)};
	const auto rookie{risk_profile_of(bot_skill::rookie, bot_style::balanced)};
	/* Aggressive and higher skill accept more; cautious stays strict. */
	CHECK(cautious.self_chance == 0);
	CHECK(cautious.self_budget < balanced.self_budget && balanced.self_budget < aggressive.self_budget && aggressive.self_budget < insane.self_budget);
	CHECK(cautious.self_chance < balanced.self_chance && balanced.self_chance < aggressive.self_chance && aggressive.self_chance < insane.self_chance);
	CHECK(cautious.trade > balanced.trade && balanced.trade > aggressive.trade && aggressive.trade > insane.trade);
	CHECK(rookie.self_budget < balanced.self_budget);
	CHECK(aggressive.hug > balanced.hug && balanced.hug > cautious.hug);
	CHECK(aggressive.standoff_scale < balanced.standoff_scale && balanced.standoff_scale < cautious.standoff_scale);
	/* Indirect fire from Hotshot (the heavy missiles' skill). */
	CHECK(balanced.indirect && !rookie.indirect);
	/* The blast and its expectation. */
	CHECK(blast_damage(0, 50, 150) == 150);
	CHECK(blast_damage(25, 50, 150) == 75);
	CHECK(blast_damage(50, 50, 150) == 0);
	CHECK(std::fabs(expected_blast_damage(25, 0, 50, 150) - 75) < 1e-9);
	CHECK(expected_blast_damage(45, 5, 50, 150) > blast_damage(45, 50, 150));
	CHECK(expected_blast_damage(80, 5, 50, 150) == 0);
	CHECK(blast_chance(20, 3, 50) == 1 && blast_chance(90, 3, 50) == 0);
	CHECK(blast_chance(50, 4, 50) > 0 && blast_chance(50, 4, 50) < 1);
}

/* Point blank never, whatever the style, unless invulnerable; the
 * expected outcome decides the rest.
 */
void test_expected_outcome()
{
	const auto level{open_room()};
	const auto md{test_mega()};
	for (const auto k : {bot_skill::hotshot, bot_skill::insane})
		for (const auto st : {bot_style::balanced, bot_style::aggressive, bot_style::cautious, bot_style::collector})
		{
			const auto rp{risk_profile_of(k, st)};
			for (const double d : {5.0, 12.0, 20.0, 29.0})
			{
				const auto sc{still_scene({0, 0, 0}, {d, 0, 0})};
				const auto o{evaluate_burst(level, sc, missile_role::heavy, md, {1, 0, 0}, true)};
				CHECK(judge_blast(o, sc, md, rp) == risk_verdict::point_blank);
				CHECK(!choose_heavy_aim(level, sc, missile_role::heavy, md, rp).best);
			}
			/* Inside its own blast, the blast would kill it: never. */
			auto weak{still_scene({0, 0, 0}, {32, 0, 0})};
			weak.shields = 40;
			const auto o{evaluate_burst(level, weak, missile_role::heavy, md, {1, 0, 0}, true)};
			CHECK(o.self_nominal >= weak.shields);
			CHECK(judge_blast(o, weak, md, rp) == risk_verdict::lethal);
			/* Far outside it: fire. */
			const auto far_scene{still_scene({0, 0, 0}, {120, 0, 0})};
			const auto f{evaluate_burst(level, far_scene, missile_role::heavy, md, {1, 0, 0}, true)};
			CHECK(f.self_damage == 0 && f.self_chance == 0);
			CHECK(f.target_damage > 100);
			CHECK(judge_blast(f, far_scene, md, rp) == risk_verdict::fire);
		}
	/* Invulnerable beyond the danger: point blank is still out, just
	 * beyond it the blast is harmless to the bot.
	 */
	{
		auto sc{still_scene({0, 0, 0}, {32, 0, 0})};
		sc.invulnerable_left = 20;
		const auto rp{risk_profile_of(bot_skill::hotshot, bot_style::cautious)};
		const auto o{evaluate_burst(level, sc, missile_role::heavy, md, {1, 0, 0}, true)};
		CHECK(o.self_damage == 0);
		CHECK(judge_blast(o, sc, md, rp) == risk_verdict::fire);
		sc.target = {20, 0, 0};
		CHECK(judge_blast(evaluate_burst(level, sc, missile_role::heavy, md, {1, 0, 0}, true), sc, md, rp) == risk_verdict::point_blank);
		/* Running out before the blast: the full rule. */
		sc.target = {32, 0, 0};
		sc.invulnerable_left = 0.1;
		CHECK(judge_blast(evaluate_burst(level, sc, missile_role::heavy, md, {1, 0, 0}, true), sc, md, rp) != risk_verdict::fire);
	}
	/* The edge of the blast: a favourable trade for an aggressive bot
	 * (a small expected self-damage for a likely kill), not for a
	 * cautious one (any chance of self-damage).
	 */
	{
		const auto sc{still_scene({0, 0, 0}, {50, 0, 0})};
		const auto o{evaluate_burst(level, sc, missile_role::heavy, md, {1, 0, 0}, true)};
		CHECK(o.self_chance > 0 && o.self_damage > 0 && o.self_damage < 5);
		CHECK(judge_blast(o, sc, md, risk_profile_of(bot_skill::hotshot, bot_style::aggressive)) == risk_verdict::fire);
		CHECK(judge_blast(o, sc, md, risk_profile_of(bot_skill::hotshot, bot_style::cautious)) == risk_verdict::over_budget);
		/* Well inside it: too much even for an aggressive bot. */
		const auto in{still_scene({0, 0, 0}, {38, 0, 0})};
		const auto i{evaluate_burst(level, in, missile_role::heavy, md, {1, 0, 0}, true)};
		CHECK(i.self_nominal > 0 && i.self_nominal < in.shields);
		CHECK(judge_blast(i, in, md, risk_profile_of(bot_skill::hotshot, bot_style::aggressive)) == risk_verdict::over_budget);
		CHECK(judge_blast(i, in, md, risk_profile_of(bot_skill::insane, bot_style::aggressive)) != risk_verdict::fire);
	}
	/* The poor trade: much self-damage for a target hardly hurt. */
	{
		blast_outcome o;
		o.impact = 80;
		o.burst_distance = 80;
		o.target_damage = 25;
		o.self_damage = 25;
		o.self_chance = 0.3;
		const auto sc{still_scene({0, 0, 0}, {80, 0, 0})};
		const auto rp{risk_profile_of(bot_skill::insane, bot_style::aggressive)};
		CHECK(judge_blast(o, sc, md, rp) == risk_verdict::poor_trade);
		o.target_damage = 3;
		CHECK(judge_blast(o, sc, md, rp) == risk_verdict::low_value);
	}
	/* A target rushing the bot meets the missile nearer (section 9.5). */
	{
		auto sc{still_scene({0, 0, 0}, {70, 0, 0})};
		const auto calm{evaluate_burst(level, sc, missile_role::heavy, md, {1, 0, 0}, true)};
		sc.target_vel = {-60, 0, 0};
		const auto rush{evaluate_burst(level, sc, missile_role::heavy, md, {1, 0, 0}, true)};
		CHECK(distance(rush.burst, sc.bot) < distance(calm.burst, sc.bot));
		CHECK(rush.self_damage > calm.self_damage);
	}
	/* Behind a wall the blast does not reach (it needs a line of
	 * sight): two rooms joined by a door.
	 */
	{
		const box_level two{{{{-100, -20, -20}, {0, 20, 20}}, {{0, -5, -5}, {10, 5, 5}}, {{10, -100, -20}, {100, 100, 20}}}};
		CHECK(!two.sees({-10, 15, 0}, {20, 60, 0}));
		CHECK(two.sees({-10, 0, 0}, {20, 0, 0}));
	}
}

/* The earthshaker's children: they do not hit the bot's ship, but burst
 * on the wall behind it; and at a target that hugs the bot.
 */
void test_shaker_children()
{
	const auto md{test_shaker()};
	const auto rp{risk_profile_of(bot_skill::hotshot, bot_style::cautious)};
	/* A corridor 20 wide; the bot 90 from the end wall, 6 from the wall
	 * behind it; nobody for the children to find.
	 */
	const box_level corridor{{{{-6, -10, -10}, {300, 10, 10}}}};
	auto sc{still_scene({0, 0, 0}, {150, 400, 0})};
	sc.target_visible = false;
	sc.unseen_for = 2;
	const auto near_back{evaluate_burst(corridor, sc, missile_role::shaker, md, {1, 0, 0}, false)};
	CHECK(!near_back.children_find_target);
	/* With the wall far behind, less of the children's risk. */
	const box_level long_corridor{{{{-200, -10, -10}, {300, 10, 10}}}};
	const auto far_back{evaluate_burst(long_corridor, sc, missile_role::shaker, md, {1, 0, 0}, false)};
	CHECK(far_back.self_damage <= near_back.self_damage);
	/* A target hugging the bot: the children burst next to it. */
	const auto room{open_room()};
	auto hug{still_scene({0, 0, 0}, {14, 0, 0})};
	const auto h{evaluate_burst(room, hug, missile_role::shaker, md, {1, 0, 0}, true)};
	CHECK(h.self_nominal >= hug.shields);
	CHECK(judge_blast(h, hug, md, rp) != risk_verdict::fire);
	CHECK(!choose_heavy_aim(room, hug, missile_role::shaker, md, risk_profile_of(bot_skill::insane, bot_style::aggressive)).best);
}

/* Indirect aim points on synthetic geometry. */
void test_indirect_aims()
{
	const auto rp{risk_profile_of(bot_skill::hotshot, bot_style::balanced)};
	/* A straight corridor: the target 8 units before its end wall.  The
	 * wall behind it and the corridor's walls round it are candidates;
	 * the bot fires (at it, or at the wall behind it).
	 */
	{
		const box_level corridor{{{{-20, -12, -12}, {208, 12, 12}}}};
		const auto sc{still_scene({0, 0, 0}, {200, 0, 0})};
		unsigned direct{0}, behind{0}, near_wall{0};
		aim_candidates(corridor, sc, 50, true, [&](const aim_kind k, const vec3 &p) {
			direct += k == aim_kind::direct;
			behind += k == aim_kind::wall_behind;
			near_wall += k == aim_kind::near_wall;
			CHECK(distance(p, sc.target) <= 50);
		});
		CHECK(direct == 1 && behind == 1 && near_wall > 0);
		const auto c{choose_heavy_aim(corridor, sc, missile_role::heavy, test_mega(), rp)};
		CHECK(c.best && c.favourable > 1 && c.indirect_favourable > 0);
		/* Without indirect fire (below Hotshot) only the target. */
		unsigned n{0};
		aim_candidates(corridor, sc, 50, false, [&](aim_kind, const vec3 &) { ++n; });
		CHECK(n == 1);
	}
	/* A corner: an L of two corridors.  The target hides round it, 40
	 * units up the second leg, last seen a second ago.  An earthshaker
	 * at the end wall of the first leg bursts where its children see the
	 * target: a corner shot.  A mega's blast does not reach round it.
	 */
	{
		const box_level l_shape{{{{-20, -10, -10}, {200, 10, 10}}, {{180, -10, -10}, {200, 150, 10}}}};
		auto sc{still_scene({20, 0, 0}, {190, 40, 0})};
		CHECK(!l_shape.sees(sc.bot, sc.target));
		sc.target_visible = false;
		sc.unseen_for = 1;
		unsigned corner{0};
		aim_candidates(l_shape, sc, 60, true, [&](const aim_kind k, const vec3 &) {
			CHECK(k == aim_kind::corner);
			++corner;
		});
		CHECK(corner > 0);
		const auto shaker{choose_heavy_aim(l_shape, sc, missile_role::shaker, test_shaker(), rp)};
		CHECK(shaker.best && shaker.best->kind == aim_kind::corner);
		CHECK(shaker.best->outcome.children_find_target);
		CHECK(shaker.best->outcome.self_damage < 3);
		CHECK(!choose_heavy_aim(l_shape, sc, missile_role::heavy, test_mega(), rp).best);
		/* A child flying back passes through the bot and bursts on the
		 * wall 40 units behind it: a small chance, too much for a
		 * cautious bot; with the wall far behind it, none.
		 */
		const auto cautious{risk_profile_of(bot_skill::hotshot, bot_style::cautious)};
		CHECK(shaker.best->outcome.self_chance > 0 && shaker.best->outcome.self_chance < 0.1);
		CHECK(!choose_heavy_aim(l_shape, sc, missile_role::shaker, test_shaker(), cautious).best);
		const box_level long_l{{{{-200, -10, -10}, {200, 10, 10}}, {{180, -10, -10}, {200, 150, 10}}}};
		const auto safe{choose_heavy_aim(long_l, sc, missile_role::shaker, test_shaker(), cautious)};
		CHECK(safe.best && safe.best->kind == aim_kind::corner && safe.best->outcome.self_chance == 0);
		/* Hidden too long ago: nowhere to aim (the tactics layer stops
		 * after CORNER_SEEN_WITHIN; the spread grows meanwhile).
		 */
		sc.unseen_for = 10;
		const auto stale{choose_heavy_aim(l_shape, sc, missile_role::shaker, test_shaker(), rp)};
		CHECK(!stale.best || stale.best->outcome.target_damage < shaker.best->outcome.target_damage);
	}
	/* The target hugs the bot (12 units) in a room with a wall 40 units
	 * off: never the direct shot; invulnerable, not into the wall next
	 * to them both either: the missile would meet the target on the way
	 * (it homes, or passes within reach of it), at point blank.
	 */
	{
		const box_level room{{{{-100, -100, -100}, {45, 100, 100}}}};
		auto sc{still_scene({0, 0, 0}, {12, 0, 0})};
		const auto rp_cautious{risk_profile_of(bot_skill::hotshot, bot_style::cautious)};
		CHECK(!choose_heavy_aim(room, sc, missile_role::heavy, test_mega(), rp_cautious).best);
		sc.invulnerable_left = 20;
		const auto c{choose_heavy_aim(room, sc, missile_role::heavy, test_mega(), rp_cautious)};
		CHECK(!c.best && c.why == risk_verdict::point_blank);
		auto straight{test_mega()};
		straight.homing = false;
		CHECK(!choose_heavy_aim(room, sc, missile_role::heavy, straight, rp_cautious).best);
	}
}

/* Section 9.6: the heavy_check with the expected outcome. */
void test_heavy_check_risk()
{
	auto m{armed(2)};
	const auto e{idx(secondary::earthshaker)};
	m.heavy_risk[e] = heavy_verdict::fire;
	m.heavy_aim[e] = aim_kind::direct;
	/* The distance rules give way to the outcome: 40 units is fine if
	 * the weighing says so.
	 */
	m.target_distance = 40;
	CHECK(heavy_check(m, secondary::earthshaker) == heavy_verdict::fire);
	m.heavy_risk[e] = heavy_verdict::risky;
	CHECK(heavy_check(m, secondary::earthshaker) == heavy_verdict::risky);
	CHECK(heavy_standoff(m) > 0);
	/* A corner shot needs no sight of the target, if seen lately. */
	m.heavy_risk[e] = heavy_verdict::fire;
	m.heavy_aim[e] = aim_kind::corner;
	m.target_visible = false;
	m.shot_clear = false;
	m.target_seen_ago = 1;
	CHECK(heavy_check(m, secondary::earthshaker) == heavy_verdict::fire);
	CHECK(choose_secondary(m) == secondary::earthshaker);
	m.target_seen_ago = CORNER_SEEN_WITHIN + 1;
	CHECK(heavy_check(m, secondary::earthshaker) == heavy_verdict::not_visible);
	/* A direct aim still needs the clear shot. */
	m.target_seen_ago = 0;
	m.target_visible = true;
	m.heavy_aim[e] = aim_kind::direct;
	CHECK(heavy_check(m, secondary::earthshaker) == heavy_verdict::no_clear_shot);
	/* Cooldown and cloak still come first. */
	m.shot_clear = true;
	m.cloaked = true;
	CHECK(heavy_check(m, secondary::earthshaker) == heavy_verdict::cloaked);
	CHECK(std::string_view{name_of(heavy_verdict::poor_trade)} == "poor-trade");
	CHECK(std::string_view{name_of(aim_kind::corner)} == "corner");
}

/* Section 9.6: hugging, both ways. */
void test_hugging()
{
	const auto aggressive{risk_profile_of(bot_skill::hotshot, bot_style::aggressive)};
	const auto cautious{risk_profile_of(bot_skill::hotshot, bot_style::cautious)};
	hug_view v{.enemy_heavy = true, .enemy_facing = true, .distance = 80, .roll = 0.5};
	CHECK(want_hug(v, aggressive));
	CHECK(!want_hug(v, cautious));
	/* Not without the knowledge, not unless it faces the bot, not out
	 * of mid range, not with its own heavy shot ready, not weak.
	 */
	for (const auto change : {0, 1, 2, 3, 4, 5})
	{
		auto w{v};
		switch (change)
		{
			case 0: w.enemy_heavy = false; break;
			case 1: w.enemy_facing = false; break;
			case 2: w.distance = 200; break;
			case 3: w.distance = 15; break;
			case 4: w.own_heavy_soon = true; break;
			default: w.weak = true; break;
		}
		CHECK(!want_hug(w, aggressive));
	}
	/* Hugging, it keeps on (facing or not) while in reach. */
	v.hugging = true;
	v.enemy_facing = false;
	v.distance = 12;
	CHECK(want_hug(v, aggressive));
	v.distance = 170;
	CHECK(!want_hug(v, aggressive));
	/* The draw: an aggressive bot hugs more often. */
	unsigned a{0}, c{0};
	for (unsigned i = 0; i < 100; ++i)
	{
		hug_view r{.enemy_heavy = true, .enemy_facing = true, .distance = 80, .roll = (i + 0.5) / 100};
		a += want_hug(r, aggressive);
		c += want_hug(r, cautious);
	}
	CHECK(a > 70 && c < 20 && c > 0);
	/* Inside the enemy's own blast, not bumping. */
	CHECK(hug_distance(50) < 50 * 0.5 && hug_distance(50) > HUG_NEAREST);
	CHECK(hug_distance(1000) <= 22 && hug_distance(0) >= HUG_NEAREST);
	/* The other way: the target closes in within the standoff. */
	duck_view d{.heavy_ready = true, .favourable = false, .distance = 50, .standoff = 80, .target_closing = 20};
	CHECK(want_duck(d));
	d.target_closing = 0;
	CHECK(!want_duck(d));
	d.back_blocked = true;
	CHECK(want_duck(d));
	d.back_blocked = false;
	d.distance = 30;
	CHECK(want_duck(d));
	d.favourable = true;
	CHECK(!want_duck(d));
	d.favourable = false;
	d.distance = 90;
	CHECK(!want_duck(d));
	d.heavy_ready = false;
	d.distance = 30;
	CHECK(!want_duck(d));
	/* Where: out of the target's sight, reachable, far from it. */
	const box_level l_shape{{{{-20, -10, -10}, {200, 10, 10}}, {{180, -10, -10}, {200, 150, 10}}}};
	const vec3 bot{188, 3, 0}, target{100, 0, 0};
	const std::array<vec3, 5> places{{{150, 0, 0}, {190, 40, 0}, {190, 80, 0}, {190, 140, 0}, {185, 0, 0}}};
	const auto p{pick_duck_point(std::span<const vec3>(places), bot, target, 6,
		[&](const vec3 &q) { return l_shape.sees(bot, q); },
		[&](const vec3 &q) { return !l_shape.sees(target, q); })};
	CHECK(p && !l_shape.sees(target, *p) && l_shape.sees(bot, *p));
	CHECK(distance(*p, bot) >= DUCK_MIN_DISTANCE && distance(*p, bot) <= DUCK_MAX_DISTANCE);
	/* In the open: nowhere. */
	const auto room{open_room()};
	CHECK(!pick_duck_point(std::span<const vec3>(places), bot, target, 6,
		[&](const vec3 &q) { return room.sees(bot, q); },
		[&](const vec3 &q) { return !room.sees(target, q); }));
}

/* Section 9.6, review: one movement per tick; the duck overrides the
 * fight and the path, the fight needs a clear shot and no path goal.
 */
void test_engaged_movement()
{
	CHECK(engaged_movement(true, false, false) == engaged_move::combat);
	CHECK(engaged_movement(false, false, false) == engaged_move::path);
	CHECK(engaged_movement(true, true, false) == engaged_move::path);
	CHECK(engaged_movement(false, true, false) == engaged_move::path);
	for (const bool clear : {false, true})
		for (const bool goal : {false, true})
			CHECK(engaged_movement(clear, goal, true) == engaged_move::duck);
}

/* Section 9.6, review: the point blank rule on the burst distance, the
 * meeting of the target on the way to a wall aim.
 */
void test_burst_distance_and_meeting()
{
	const auto level{open_room()};
	const auto md{test_mega()};
	const auto rp{risk_profile_of(bot_skill::insane, bot_style::aggressive)};
	/* 40 units off (beyond point blank), both rushing: the burst comes
	 * within 30 units of the bot.
	 */
	auto sc{still_scene({0, 0, 0}, {40, 0, 0})};
	sc.invulnerable_left = 20;
	const auto calm{evaluate_burst(level, sc, missile_role::heavy, md, {1, 0, 0}, true)};
	CHECK(calm.impact >= MISSILE_MIN_DISTANCE && calm.burst_distance >= MISSILE_MIN_DISTANCE);
	CHECK(judge_blast(calm, sc, md, rp) == risk_verdict::fire);
	sc.bot_vel = {40, 0, 0};
	sc.target_vel = {-50, 0, 0};
	const auto rush{evaluate_burst(level, sc, missile_role::heavy, md, {1, 0, 0}, true)};
	CHECK(rush.impact >= MISSILE_MIN_DISTANCE && rush.burst_distance < MISSILE_MIN_DISTANCE);
	CHECK(judge_blast(rush, sc, md, rp) == risk_verdict::point_blank);
	/* A homing missile aimed at a wall 20 degrees off a target in sight
	 * meets it; 70 degrees off, or a target out of sight, not; a
	 * missile that does not home only near its line.
	 */
	const auto t{still_scene({0, 0, 0}, {60, 0, 0})};
	CHECK(may_meet_target(t, {std::cos(0.35), std::sin(0.35), 0}, md));
	CHECK(!may_meet_target(t, {std::cos(1.2), std::sin(1.2), 0}, md));
	auto hidden{t};
	hidden.target_visible = false;
	CHECK(!may_meet_target(hidden, {1, 0, 0}, md));
	auto straight{md};
	straight.homing = false;
	CHECK(may_meet_target(t, {1, 0.05, 0}, straight));
	CHECK(!may_meet_target(t, {std::cos(0.35), std::sin(0.35), 0}, straight));
	/* The worse of both, the aim's value. */
	blast_outcome wall, meet;
	wall.impact = 90;
	wall.burst_distance = 90;
	wall.target_damage = 100;
	meet.impact = 35;
	meet.burst_distance = 20;
	meet.self_damage = 30;
	meet.self_nominal = 40;
	meet.self_chance = 0.5;
	const auto w{merge_meet(wall, meet)};
	CHECK(w.impact == 35 && w.burst_distance == 20 && w.self_damage == 30 && w.self_nominal == 40 && w.self_chance == 0.5 && w.target_damage == 100);
	CHECK(judge_blast(w, t, md, rp) == risk_verdict::point_blank);
	/* A target rushing the bot in a corridor: every wall aim ahead meets
	 * it first, so none is taken at point blank.
	 */
	const box_level corridor{{{{-20, -12, -12}, {208, 12, 12}}}};
	auto rushing{still_scene({0, 0, 0}, {45, 0, 0})};
	rushing.target_vel = {-60, 0, 0};
	rushing.bot_vel = {30, 0, 0};
	const auto c{choose_heavy_aim(corridor, rushing, missile_role::heavy, md, rp)};
	CHECK(!c.best || c.best->outcome.burst_distance >= MISSILE_MIN_DISTANCE);
}

/* Section 9.6, review: the bounded search and the objects on the line. */
void test_aim_search_bounds()
{
	const auto rp{risk_profile_of(bot_skill::hotshot, bot_style::balanced)};
	const box_level corridor{{{{-20, -12, -12}, {208, 12, 12}}}};
	const auto sc{still_scene({0, 0, 0}, {200, 0, 0})};
	const auto full{choose_heavy_aim(corridor, sc, missile_role::heavy, test_mega(), rp)};
	CHECK(full.best && full.candidates > 10);
	/* Half the fan per plan, at most 4 indirect aims (plus the direct
	 * shot, the wall behind, the last best).
	 */
	aim_search search{.subsets = 2, .phase = 0, .max_indirect = 4, .previous = std::nullopt};
	const auto first{choose_heavy_aim(corridor, sc, missile_role::heavy, test_mega(), rp, search)};
	CHECK(first.best && first.candidates <= 6);
	/* Both phases see different spokes. */
	unsigned n0{0}, n1{0};
	aim_candidates(corridor, sc, 45, true, [&](aim_kind, const vec3 &) { ++n0; }, aim_search{.subsets = 2, .phase = 0, .max_indirect = ~0u, .previous = std::nullopt});
	aim_candidates(corridor, sc, 45, true, [&](aim_kind, const vec3 &) { ++n1; }, aim_search{.subsets = 2, .phase = 1, .max_indirect = ~0u, .previous = std::nullopt});
	unsigned all{0};
	aim_candidates(corridor, sc, 45, true, [&](aim_kind, const vec3 &) { ++all; });
	CHECK(n0 < all && n1 < all && n0 + n1 >= all);
	/* The last best one is weighed again. */
	search.phase = 1;
	search.max_indirect = 0;
	const vec3 previous{190, 11, 0};
	search.previous = previous;
	const auto again{choose_heavy_aim(corridor, sc, missile_role::heavy, test_mega(), rp, search)};
	/* The direct shot, the wall behind, the last best. */
	CHECK(again.candidates == 3);
	/* An object on the line refuses the indirect aims (a teammate): the
	 * direct shot remains.
	 */
	const auto blocked{choose_heavy_aim(corridor, sc, missile_role::heavy, test_mega(), rp, {}, [](const aim_option &) { return false; })};
	CHECK(!blocked.best || blocked.best->kind == aim_kind::direct);
	/* A corner shot (no direct one) refused: no aim at all. */
	const box_level l_shape{{{{-20, -10, -10}, {200, 10, 10}}, {{180, -10, -10}, {200, 150, 10}}}};
	auto hidden{still_scene({20, 0, 0}, {190, 40, 0})};
	hidden.target_visible = false;
	hidden.unseen_for = 1;
	CHECK(choose_heavy_aim(l_shape, hidden, missile_role::shaker, test_shaker(), rp).best);
	const auto refused{choose_heavy_aim(l_shape, hidden, missile_role::shaker, test_shaker(), rp, {}, [](const aim_option &) { return false; })};
	CHECK(!refused.best && refused.blocked > 0);
}

/* Section 9.6, review: the enemy's heavy missiles counted, the modes
 * held, the hug dropped out of sight.
 */
void test_heavy_knowledge_and_modes()
{
	constexpr uint32_t tick{1000};
	heavy_holding h;
	CHECK(!h.held(tick));
	h = heavy_picked_up(h, tick);
	h = heavy_picked_up(h, tick);
	CHECK(h.count == 2 && h.held(tick + 60));
	h = heavy_fired(h, tick + 60);
	CHECK(h.count == 1 && h.held(tick + 120));
	/* Its last known one fired: forgotten. */
	h = heavy_fired(h, tick + 120);
	CHECK(!h.held(tick + 121));
	/* A shot not seen picked up: it may hold more, for a short while. */
	h = heavy_fired(h, tick + 200);
	CHECK(h.held(tick + 201) && !h.held(tick + 200 + static_cast<uint32_t>(HEAVY_GUESS_SECONDS * BOT_TICK_RATE)));
	CHECK(HEAVY_GUESS_SECONDS < HEAVY_KNOWN_SECONDS && HEAVY_KNOWN_SECONDS < 45);
	/* The bot's own heavy missile usable soon: no hug (its standoff). */
	auto m{armed(2)};
	m.since_missile = 10;
	m.since_heavy = 10;
	CHECK(heavy_usable_soon(m));
	m.since_heavy = 1;
	CHECK(!heavy_usable_soon(m));
	m.since_heavy = HEAVY_INTERVAL - 1;
	CHECK(heavy_usable_soon(m));
	m.heavy_used_on_target = true;
	CHECK(!heavy_usable_soon(m));
	m.heavy_used_on_target = false;
	m.ammo[idx(secondary::mega)] = 0;
	m.ammo[idx(secondary::earthshaker)] = 0;
	CHECK(!heavy_usable_soon(m));
	const auto aggressive{risk_profile_of(bot_skill::hotshot, bot_style::aggressive)};
	hug_view v{.enemy_heavy = true, .enemy_facing = true, .distance = 80, .roll = 0.1};
	CHECK(want_hug(v, aggressive));
	v.own_heavy_soon = true;
	CHECK(!want_hug(v, aggressive));
	/* Hugging, a moment held even if its own missile comes ready... */
	v.hugging = true;
	v.since_change = 0.5;
	CHECK(want_hug(v, aggressive));
	/* ...then the standoff. */
	v.since_change = 3;
	CHECK(!want_hug(v, aggressive));
	/* Just stopped: not again at once. */
	v.hugging = false;
	v.own_heavy_soon = false;
	v.since_change = 0.5;
	CHECK(!want_hug(v, aggressive));
	/* Out of sight: kept a moment, then dropped; never started. */
	v.hugging = true;
	v.since_change = 3;
	v.unseen_for = 1;
	CHECK(want_hug(v, aggressive));
	v.unseen_for = HUG_LOST_SECONDS + 0.5;
	CHECK(!want_hug(v, aggressive));
	v.since_change = 0.5;
	CHECK(!want_hug(v, aggressive));
	v.hugging = false;
	v.since_change = 3;
	v.unseen_for = 0.2;
	CHECK(!want_hug(v, aggressive));
	/* The enemy's known missiles gone: no hug. */
	v.unseen_for = 0;
	v.hugging = true;
	v.enemy_heavy = false;
	CHECK(!want_hug(v, aggressive));
}

/* Section 9.8: volleys of the light missiles, bursts of smart missiles,
 * the heavy ones one at a time.
 */
void test_volleys()
{
	const auto view{[](const secondary s, const unsigned smarts, const bot_style st, const unsigned ammo, const double d) {
		return volley_view{
			.s = s,
			.smarts = smarts,
			.style = st,
			.ammo = ammo,
			.target_visible = true,
			.shot_clear = true,
			.target_distance = d,
			.target_lateral_speed = 10,
			.target_seen_ago = 0,
		};
	}};
	/* Homing at a good target: Rookie 1, Hotshot 2, Ace 3, Insane 4. */
	for (unsigned smarts = 1; smarts <= 4; ++smarts)
		CHECK(volley_size(view(secondary::homing, smarts, bot_style::balanced, 10, 80)) == std::max(1u, smarts));
	/* "A fleet of 3-5 of them make an opponent run." */
	CHECK(volley_size(view(secondary::homing, 4, bot_style::aggressive, 10, 80)) == 5);
	CHECK(volley_size(view(secondary::concussion, 3, bot_style::balanced, 10, 80)) == 3);
	CHECK(volley_size(view(secondary::mercury, 4, bot_style::balanced, 10, 80)) == 4);
	CHECK(volley_size(view(secondary::homing, 2, bot_style::cautious, 10, 80)) == 1);
	/* Never more than it holds. */
	CHECK(volley_size(view(secondary::homing, 4, bot_style::aggressive, 2, 80)) == 2);
	CHECK(volley_size(view(secondary::homing, 4, bot_style::aggressive, 0, 80)) == 0);
	/* Not a good target: one. */
	CHECK(volley_size(view(secondary::concussion, 4, bot_style::balanced, 10, 170)) == 1);
	{
		auto v{view(secondary::concussion, 4, bot_style::balanced, 10, 80)};
		v.target_lateral_speed = 60;
		CHECK(volley_size(v) == 1);
		/* A homing volley follows a crosser. */
		v.s = secondary::homing;
		CHECK(volley_size(v) == 4);
		v.shot_clear = false;
		CHECK(volley_size(v) == 1);
		v.shot_clear = true;
		v.cloaked = true;
		CHECK(volley_size(v) == 1);
	}
	/* Smart missiles: a burst at a target behind cover (seen a moment
	 * ago), from Ace also at one in sight at mid range.
	 */
	{
		auto v{view(secondary::smart, 2, bot_style::balanced, 5, 80)};
		CHECK(volley_size(v) == 1);
		v.smarts = 3;
		CHECK(volley_size(v) == 2);
		v.target_visible = false;
		v.target_seen_ago = 0.5;
		v.smarts = 2;
		CHECK(volley_size(v) == 2);
		v.smarts = 4;
		CHECK(volley_size(v) == 3);
		v.style = bot_style::aggressive;
		CHECK(volley_size(v) == 3);
		v.target_seen_ago = 3;
		CHECK(volley_size(v) == 1);
	}
	/* The heavy ones, flash and mines: one at a time. */
	for (const auto s : {secondary::mega, secondary::earthshaker, secondary::flash, secondary::proximity, secondary::smart_mine})
		CHECK(volley_size(view(s, 4, bot_style::aggressive, 10, 120)) == 1);
	/* The rounds follow while the target stays good, after the gap. */
	{
		const auto v{view(secondary::homing, 4, bot_style::balanced, 5, 80)};
		CHECK(volley_continues(v, 3, VOLLEY_GAP));
		CHECK(!volley_continues(v, 3, VOLLEY_GAP / 2));
		CHECK(!volley_continues(v, 0, 1));
		auto lost{v};
		lost.target_visible = false;
		CHECK(!volley_continues(lost, 3, 1));
		auto empty{v};
		empty.ammo = 0;
		CHECK(!volley_continues(empty, 3, 1));
		auto smart{view(secondary::smart, 4, bot_style::balanced, 5, 80)};
		smart.target_visible = false;
		smart.target_seen_ago = 0.4;
		CHECK(volley_continues(smart, 2, 1));
		CHECK(!volley_continues(view(secondary::mega, 4, bot_style::balanced, 5, 80), 2, 1));
	}
	/* The fire rate stays within the weapon's refire limit: the rounds
	 * go when both the volley's gap and the game's fire_wait allow
	 * (allowed_to_fire_missile), the next volley a missile_interval after
	 * the last round.  A model of an Insane bot with 10 homing missiles
	 * and a fire_wait of 0.25 s, at 60 ticks a second.
	 */
	{
		constexpr double fire_wait{0.25};
		constexpr double dt{1.0 / 60};
		unsigned ammo{10}, left{0};
		double last{-1e9}, next_allowed{0};
		std::vector<double> shots;
		for (double t = 0; t < 6 && ammo; t += dt)
		{
			auto v{view(secondary::homing, 4, bot_style::balanced, ammo, 80)};
			bool go{false};
			if (left)
			{
				if (volley_continues(v, left, t - last))
					go = true;
				else if (!(t - last < VOLLEY_GAP))
					left = 0;
			}
			if (!go && !left && t - last >= missile_interval(4))
				go = true;
			if (!go || t < next_allowed)
				continue;
			if (left)
				--left;
			else
				left = volley_size(v) - 1;
			shots.push_back(t);
			last = t;
			next_allowed = t + fire_wait;
			--ammo;
		}
		CHECK(shots.size() == 10);
		for (std::size_t i = 1; i < shots.size(); ++i)
			CHECK(shots[i] - shots[i - 1] >= fire_wait - 1e-9);
		/* Volleys of four: the fifth round waits the interval. */
		CHECK(shots[3] - shots[0] < 4 * fire_wait);
		CHECK(shots[4] - shots[3] >= missile_interval(4) - 1e-9);
	}
}

/* Section 9.8: Ace and Insane are clearly bolder with the heavy missiles
 * than Hotshot, within the no-suicide rules.
 */
void test_heavy_boldness()
{
	for (const auto st : {bot_style::balanced, bot_style::aggressive, bot_style::cautious, bot_style::collector})
	{
		const auto h{risk_profile_of(bot_skill::hotshot, st)}, a{risk_profile_of(bot_skill::ace, st)}, i{risk_profile_of(bot_skill::insane, st)};
		CHECK(a.self_budget >= 1.4 * h.self_budget && i.self_budget >= 1.8 * h.self_budget);
		CHECK(a.trade <= 0.9 * h.trade && i.trade <= 0.8 * h.trade);
		CHECK(a.self_chance >= h.self_chance && i.self_chance >= a.self_chance);
	}
	/* The aggressive Insane bot still wants more damage to the target
	 * than to itself.
	 */
	CHECK(risk_profile_of(bot_skill::insane, bot_style::aggressive).trade > 1);
	/* Heavy missiles more often at the higher skills. */
	CHECK(heavy_interval(2) == HEAVY_INTERVAL && heavy_per_target(2) == HEAVY_PER_TARGET);
	CHECK(heavy_interval(3) < heavy_interval(2) && heavy_interval(4) < heavy_interval(3));
	CHECK(heavy_per_target(3) < heavy_per_target(2) && heavy_per_target(4) < heavy_per_target(3));
	CHECK(missile_interval(4) <= 1.0 && missile_interval(3) <= 1.5);
	/* A shot at the edge of its blast: an Insane balanced bot takes it, a
	 * Hotshot balanced one does not.
	 */
	const auto md{[] {
		auto m{blast(40)};
		m.damage = 150;
		return m;
	}()};
	blast_outcome o;
	o.impact = 80;
	o.burst_distance = 80;
	o.target_damage = 60;
	o.self_damage = 15;
	o.self_chance = 0.3;
	const auto sc{still_scene({0, 0, 0}, {80, 0, 0})};
	CHECK(judge_blast(o, sc, md, risk_profile_of(bot_skill::hotshot, bot_style::balanced)) == risk_verdict::over_budget);
	CHECK(judge_blast(o, sc, md, risk_profile_of(bot_skill::insane, bot_style::balanced)) == risk_verdict::fire);
	CHECK(judge_blast(o, sc, md, risk_profile_of(bot_skill::ace, bot_style::aggressive)) == risk_verdict::fire);
	/* Never at point blank, never a lethal blast, whatever the skill. */
	auto close{o};
	close.impact = close.burst_distance = 20;
	auto lethal{o};
	lethal.self_nominal = sc.shields;
	for (const auto k : {bot_skill::hotshot, bot_skill::ace, bot_skill::insane})
		for (const auto st : {bot_style::balanced, bot_style::aggressive})
		{
			CHECK(judge_blast(close, sc, md, risk_profile_of(k, st)) == risk_verdict::point_blank);
			CHECK(judge_blast(lethal, sc, md, risk_profile_of(k, st)) == risk_verdict::lethal);
		}
}

/* Section 9.8: the death dump. */
void test_death_dump()
{
	const death_dump_view hurt{
		.shields = 12,
		.since_hit = 0.4,
		.incoming_damage = 0,
		.invulnerable = false,
		.has_ammo = true,
		.roll = 0.5,
	};
	CHECK(death_dump_wanted(hurt, bot_skill::insane, bot_style::balanced));
	CHECK(death_dump_wanted(hurt, bot_skill::hotshot, bot_style::balanced));
	/* Trainee never. */
	CHECK(!death_dump_wanted(hurt, bot_skill::trainee, bot_style::balanced));
	/* Not under fire, or not low: no dump. */
	{
		auto v{hurt};
		v.since_hit = 3;
		CHECK(!death_dump_wanted(v, bot_skill::insane, bot_style::balanced));
		v = hurt;
		v.shields = 40;
		CHECK(!death_dump_wanted(v, bot_skill::insane, bot_style::balanced));
		v = hurt;
		v.invulnerable = true;
		CHECK(!death_dump_wanted(v, bot_skill::insane, bot_style::balanced));
		v = hurt;
		v.has_ammo = false;
		CHECK(!death_dump_wanted(v, bot_skill::insane, bot_style::balanced));
	}
	/* A lethal hit on its way: dump, even before the threshold. */
	{
		auto v{hurt};
		v.shields = 30;
		v.since_hit = 5;
		CHECK(!death_dump_wanted(v, bot_skill::insane, bot_style::balanced));
		v.incoming_damage = 32;
		CHECK(death_dump_wanted(v, bot_skill::insane, bot_style::balanced));
		/* A full-shield bot is not about to die. */
		v.shields = 100;
		v.incoming_damage = 120;
		CHECK(!death_dump_wanted(v, bot_skill::insane, bot_style::balanced));
	}
	/* Higher skill does it more reliably, with more shields left. */
	for (unsigned k = 1; k < BOT_SKILL_COUNT; ++k)
		for (const auto st : {bot_style::balanced, bot_style::aggressive, bot_style::cautious, bot_style::collector})
		{
			const auto a{static_cast<bot_skill>(k - 1)}, b{static_cast<bot_skill>(k)};
			CHECK(death_dump_reliability(b, st) > death_dump_reliability(a, st));
			CHECK(death_dump_shields(b, st) > death_dump_shields(a, st));
		}
	CHECK(death_dump_reliability(bot_skill::insane, bot_style::balanced) >= 0.95);
	{
		auto v{hurt};
		v.roll = 0.7;
		CHECK(!death_dump_wanted(v, bot_skill::hotshot, bot_style::balanced));
		CHECK(death_dump_wanted(v, bot_skill::ace, bot_style::balanced));
	}
	/* The order: the most valuable first; a missile only with its blast
	 * clear of the bot, a mine always; only what the skill uses.
	 */
	std::array<uint8_t, BOT_SECONDARY_COUNT> ammo{};
	ammo[idx(secondary::earthshaker)] = 1;
	ammo[idx(secondary::homing)] = 3;
	ammo[idx(secondary::proximity)] = 4;
	const auto all_ok{[](secondary) { return true; }};
	const auto none_ok{[](secondary) { return false; }};
	CHECK(death_dump_choice(ammo, 4, all_ok) == secondary::earthshaker);
	CHECK(death_dump_choice(ammo, 4, none_ok) == secondary::proximity);
	CHECK(death_dump_choice(ammo, 4, [](const secondary s) { return s != secondary::earthshaker; }) == secondary::homing);
	CHECK(death_dump_choice(ammo, 1, all_ok) == secondary::homing);
	ammo = {};
	ammo[idx(secondary::guided)] = 2;
	CHECK(!death_dump_choice(ammo, 4, all_ok));
}

}

int main()
{
	test_volleys();
	test_heavy_boldness();
	test_death_dump();
	test_risk_profiles();
	test_expected_outcome();
	test_shaker_children();
	test_indirect_aims();
	test_heavy_check_risk();
	test_hugging();
	test_engaged_movement();
	test_burst_distance_and_meeting();
	test_aim_search_bounds();
	test_heavy_knowledge_and_modes();
	test_closing_target();
	test_shaker_behind();
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
