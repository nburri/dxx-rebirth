/*
 * This file is part of the DXX-Rebirth project <https://github.com/dxx-rebirth/dxx-rebirth/>.
 * It is copyright by its individual contributors, as recorded in the
 * project's Git history.  See COPYING.txt at the top level for license
 * terms and a link to the Git history.
 */
/*
 * Test of the bots' resources and goals (bot_goals.h,
 * Documentation/multiplayer-bots.md sections 4.5, 4.7 and 5, stage B3):
 * the weapon table by range band, energy and ammunition, the long range
 * trigger discipline, the needs and the value of each powerup, the
 * collection score, the goal choice, the fuel centres, the retreat, the
 * map knowledge by skill, the memory of powerups and the afterburner.
 *
 * Build and run with SCons:
 *
 *	scons sdl2=1 d1x=0 d2x=1 register_runtime_test_plain_link_targets=1 test-bot-goals
 *	build/common/test-bot-goals
 */

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <initializer_list>

#include "bot_goals.h"

using namespace dcx::bot;

namespace {

void check_failed(const char *const expr, const char *const file, const int line)
{
	std::fprintf(stderr, "%s:%d: check failed: %s\n", file, line, expr);
	std::exit(1);
}

#define CHECK(cond)	do { if (!(cond)) check_failed(#cond, __FILE__, __LINE__); } while (0)

resource_view with_shields(const double shields)
{
	resource_view r;
	r.shields = shields;
	return r;
}

resource_view with_energy(const double energy)
{
	resource_view r;
	r.energy = energy;
	return r;
}

known_powerup kp(const uint16_t key, const uint16_t signature, const uint8_t type = 0, const vec3 &pos = {}, const uint32_t segment = 0)
{
	known_powerup k;
	k.key = key;
	k.signature = signature;
	k.type = type;
	k.pos = pos;
	k.segment = segment;
	return k;
}

constexpr uint16_t bit(const primary p)
{
	return static_cast<uint16_t>(1u << static_cast<unsigned>(p));
}

constexpr uint16_t bits(const std::initializer_list<primary> ps)
{
	uint16_t r{bit(primary::laser)};
	for (const auto p : ps)
		r = static_cast<uint16_t>(r | bit(p));
	return r;
}

void test_bands()
{
	CHECK(band_of(0) == range_band::close);
	CHECK(band_of(59.9) == range_band::close);
	CHECK(band_of(60) == range_band::mid);
	CHECK(band_of(149.9) == range_band::mid);
	CHECK(band_of(150) == range_band::far);
	CHECK(band_of(1000) == range_band::far);
}

/* Section 4.5: the table's preferences per band. */
void test_weapon_table()
{
	weapon_view all;
	all.owned = bits({primary::vulcan, primary::spreadfire, primary::plasma, primary::fusion, primary::gauss, primary::helix, primary::phoenix, primary::omega});
	all.vulcan_ammo = 1000;
	all.energy = 150;
	/* Close: helix first; mid: plasma; far: gauss. */
	CHECK(choose_primary_for(all, range_band::close, primary::laser) == primary::helix);
	CHECK(choose_primary_for(all, range_band::mid, primary::laser) == primary::plasma);
	CHECK(choose_primary_for(all, range_band::far, primary::laser) == primary::gauss);
	/* Without a target: the mid band's choice. */
	CHECK(choose_primary_for(all, std::nullopt, primary::laser) == primary::plasma);
	/* Never fusion or omega (stage B1: the human's charge). */
	for (const auto b : {range_band::close, range_band::mid, range_band::far})
	{
		const auto p{choose_primary_for(all, b, primary::fusion)};
		CHECK(p != primary::fusion && p != primary::omega);
		CHECK(weapon_score(primary::fusion, b, all) == 0);
		CHECK(weapon_score(primary::omega, b, all) == 0);
		CHECK(weapon_score(primary::super_laser, b, all) == 0);
	}
	/* Close range without helix: spreadfire. */
	weapon_view close{all};
	close.owned = bits({primary::spreadfire, primary::plasma, primary::gauss, primary::vulcan});
	CHECK(choose_primary_for(close, range_band::close, primary::laser) == primary::spreadfire);
	/* Far: the fast shots (gauss, vulcan, the lasers), not the blobs. */
	weapon_view far{all};
	far.owned = bits({primary::vulcan, primary::spreadfire, primary::helix, primary::phoenix});
	CHECK(choose_primary_for(far, range_band::far, primary::laser) == primary::vulcan);
	far.owned = bits({primary::spreadfire, primary::helix, primary::phoenix});
	far.laser_level = 3;
	CHECK(choose_primary_for(far, range_band::far, primary::spreadfire) == primary::laser);
	/* Mid order: plasma, helix, super laser with quad, gauss, vulcan,
	 * phoenix.
	 */
	weapon_view mid{all};
	mid.laser_level = 5;
	mid.quad = true;
	mid.owned = bits({primary::helix, primary::gauss, primary::vulcan, primary::phoenix});
	CHECK(choose_primary_for(mid, range_band::mid, primary::phoenix) == primary::helix);
	mid.owned = bits({primary::gauss, primary::vulcan, primary::phoenix});
	CHECK(choose_primary_for(mid, range_band::mid, primary::vulcan) == primary::laser);
	mid.quad = false;
	mid.laser_level = 0;
	CHECK(choose_primary_for(mid, range_band::mid, primary::laser) == primary::gauss);
	mid.owned = bits({primary::vulcan, primary::phoenix});
	CHECK(choose_primary_for(mid, range_band::mid, primary::laser) == primary::vulcan);
	mid.owned = bits({primary::phoenix});
	CHECK(choose_primary_for(mid, range_band::mid, primary::laser) == primary::phoenix);
	/* The laser grows with its level and the quad lasers. */
	for (unsigned l = 0; l < 5; ++l)
		CHECK(laser_score(l + 1, false) > laser_score(l, false));
	CHECK(laser_score(3, true) > laser_score(3, false));
	CHECK(laser_score(9, false) == laser_score(5, false));
}

void test_weapon_resources()
{
	weapon_view v;
	v.owned = bits({primary::plasma, primary::gauss});
	v.vulcan_ammo = 500;
	v.energy = 100;
	CHECK(choose_primary_for(v, range_band::mid, primary::laser) == primary::plasma);
	/* Low on energy (below 20): the ammunition weapon. */
	v.energy = 15;
	CHECK(choose_primary_for(v, range_band::mid, primary::plasma) == primary::gauss);
	/* No energy: nothing but the ammunition weapons fires. */
	v.energy = 0.5;
	CHECK(weapon_score(primary::plasma, range_band::mid, v) == 0);
	CHECK(weapon_score(primary::laser, range_band::mid, v) == 0);
	CHECK(choose_primary_for(v, range_band::close, primary::laser) == primary::gauss);
	/* No rounds: no gauss; nothing fires: the laser. */
	v.vulcan_ammo = 0;
	CHECK(weapon_score(primary::gauss, range_band::far, v) == 0);
	CHECK(choose_primary_for(v, range_band::far, primary::gauss) == primary::laser);
	/* Energy between 20 and 50 spares the hungry weapons, smoothly. */
	CHECK(energy_factor(primary::plasma, 20) == 0.75);
	CHECK(energy_factor(primary::plasma, 35) > 0.75 && energy_factor(primary::plasma, 35) < 1);
	CHECK(energy_factor(primary::plasma, 50) == 1);
	CHECK(energy_factor(primary::laser, 10) > energy_factor(primary::plasma, 10));
	CHECK(energy_factor(primary::vulcan, 0) == 1);
	/* Not owned: no score. */
	CHECK(weapon_score(primary::helix, range_band::close, v) == 0);
}

/* A switch costs the rearm time: a slightly better weapon does not make
 * the bot switch back and forth.
 */
void test_weapon_hysteresis()
{
	weapon_view v;
	v.owned = bits({primary::plasma, primary::helix});
	v.energy = 100;
	/* Mid: plasma 3.8 against helix 3.5 (within 15 %): keep either. */
	CHECK(choose_primary_for(v, range_band::mid, primary::helix) == primary::helix);
	CHECK(choose_primary_for(v, range_band::mid, primary::plasma) == primary::plasma);
	CHECK(choose_primary_for(v, range_band::mid, primary::laser) == primary::plasma);
	/* Far: plasma 2.0 against helix 1.2: switch. */
	CHECK(choose_primary_for(v, range_band::far, primary::helix) == primary::plasma);
	/* The current weapon cannot fire: switch whatever the margin. */
	v.owned = bits({primary::gauss});
	v.vulcan_ammo = 0;
	CHECK(choose_primary_for(v, range_band::mid, primary::gauss) == primary::laser);
}

void test_long_shots()
{
	const double sigma{radians(2.8)};
	/* Within the mid band: always worth a shot. */
	CHECK(long_shot_worthwhile(100, 120, 60, sigma, 5));
	/* Far, a still target: fire. */
	CHECK(long_shot_worthwhile(200, 120, 0, sigma, 5));
	CHECK(long_shot_hit_chance(200, 120, 0, sigma, 5) > 0.4);
	/* Far, a fast strafer against a slow shot: hold. */
	CHECK(!long_shot_worthwhile(300, 80, 60, sigma, 5));
	/* The chance falls with the distance and the target's speed. */
	CHECK(long_shot_hit_chance(300, 120, 30, sigma, 5) < long_shot_hit_chance(200, 120, 30, sigma, 5));
	CHECK(long_shot_hit_chance(200, 120, 50, sigma, 5) < long_shot_hit_chance(200, 120, 10, sigma, 5));
	/* A faster shot keeps its chance longer. */
	CHECK(long_shot_hit_chance(250, 400, 40, sigma, 5) > long_shot_hit_chance(250, 100, 40, sigma, 5));
	CHECK(long_shot_hit_chance(0, 100, 40, sigma, 5) == 1);
}

/* Section 4.7: needs and values. */
void test_values()
{
	resource_view r;
	r.weapons.energy = r.energy;
	/* Shields: 1-4 by need, more as they drop. */
	CHECK(shield_need(100) == 0 && shield_need(150) == 0);
	CHECK(shield_need(20) == 1 && shield_need(0) == 1);
	double last{0};
	for (double s = 200; s >= 0; s -= 10)
	{
		r.shields = s;
		const double v{item_value({item::shield}, r)};
		CHECK(v >= last && v >= 0.5 && v <= 4);
		last = v;
	}
	r.shields = 10;
	CHECK(item_value({item::shield}, r) == 4);
	r.shields = 100;
	/* Energy: its need, halved with an ammunition weapon that has
	 * rounds.
	 */
	r.energy = 20;
	r.weapons.energy = 20;
	const double e{item_value({item::energy}, r)};
	CHECK(e > 2.5);
	r.weapons.owned = bits({primary::vulcan});
	r.weapons.vulcan_ammo = 300;
	CHECK(energy_need(r) == 0.5);
	CHECK(item_value({item::energy}, r) < e);
	r.weapons.vulcan_ammo = 0;
	CHECK(energy_need(r) == 1);
	r.weapons = {};
	r.energy = r.weapons.energy = 100;
	/* A better primary is worth 5, a spare 1, one it has nothing. */
	const auto plasma{item_desc{item::primary, primary::plasma, 0}};
	CHECK(item_value(plasma, r) == VALUE_BETTER_PRIMARY);
	r.weapons.owned = bits({primary::helix, primary::plasma});
	CHECK(item_value(plasma, r) == 0);
	CHECK(item_value({item::primary, primary::spreadfire, 0}, r) == VALUE_SPARE_PRIMARY);
	/* Fusion and omega: taken (denied to others, dropped at death), for
	 * little.
	 */
	r.weapons.owned = bits({});
	CHECK(item_value({item::primary, primary::fusion, 0}, r) == VALUE_SPARE_PRIMARY);
	CHECK(item_value({item::primary, primary::omega, 0}, r) == VALUE_SPARE_PRIMARY);
	/* A new cannon is better than the laser (it comes with rounds). */
	CHECK(item_value({item::primary, primary::gauss, 0}, r) == VALUE_BETTER_PRIMARY);
	/* A cannon it has: its rounds, the more the emptier. */
	r.weapons.owned = bits({primary::gauss});
	r.weapons.vulcan_ammo = 100;
	r.vulcan_ammo_share = 0.9;
	const double some{item_value({item::primary, primary::gauss, 0}, r)};
	r.vulcan_ammo_share = 0.1;
	CHECK(item_value({item::primary, primary::gauss, 0}, r) > some);
	CHECK(item_value({item::vulcan_ammo}, r) == item_value({item::primary, primary::gauss, 0}, r));
	r.weapons.owned = bits({});
	CHECK(item_value({item::vulcan_ammo}, r) < 0.5);
	/* Laser upgrades matter while the laser is the best weapon. */
	CHECK(item_value({item::laser}, r) == 3);
	CHECK(item_value({item::super_laser}, r) == 3);
	CHECK(item_value({item::quad}, r) == 4);
	r.weapons.owned = bits({primary::plasma, primary::helix});
	CHECK(item_value({item::laser}, r) == VALUE_SPARE_PRIMARY);
	CHECK(item_value({item::quad}, r) == 2);
	/* Section 5.1 values, and the items a bot never seeks. */
	CHECK(item_value({item::secondary, primary::laser, 0}, r) == 1);
	CHECK(item_value({item::secondary, primary::laser, 9}, r) == 2);
	CHECK(item_value({item::headlight}, r) == 0);
	CHECK(item_value({item::full_map}, r) == 0);
	CHECK(item_value({item::none}, r) == 0);
	CHECK(item_value({item::afterburner}, r) > 0);
	CHECK(item_value({item::cloak}, r) > 0);
	r.cloaked = true;
	CHECK(item_value({item::cloak}, r) == 0);
	CHECK(item_value({item::invulnerability}, r) == 4);
	r.invulnerable = true;
	CHECK(item_value({item::invulnerability}, r) == 0);
}

void test_collect_utility()
{
	/* Value over distance: halved 60 units away, falling. */
	CHECK(collect_utility(4, 0) == 4);
	CHECK(collect_utility(4, 60) == 2);
	CHECK(collect_utility(4, 100) < collect_utility(4, 50));
	CHECK(collect_utility(0, 10) == 0);
	CHECK(collect_utility(-1, 10) == 0);
	CHECK(collect_utility(2, -5) == 2);
	/* A better weapon far away against a shield orb nearby at full
	 * shields.
	 */
	CHECK(collect_utility(VALUE_BETTER_PRIMARY, 300) > collect_utility(0.5, 20));
}

/* Section 4.1: the goal choice. */
void test_goal_choice()
{
	/* Nothing to do: roam. */
	goal_inputs in;
	CHECK(choose_goal(in) == goal_kind::roam);
	/* A target in sight: engage. */
	in.has_target = true;
	in.target_visible = true;
	in.target_score = 0.8;
	in.threatened = true;
	CHECK(choose_goal(in) == goal_kind::engage);
	/* Known but not seen: hunt. */
	in.target_visible = false;
	in.target_score = 0.4;
	CHECK(choose_goal(in) == goal_kind::hunt);
	/* A better weapon near, while hunting: collect it first. */
	in.collect = collect_utility(VALUE_BETTER_PRIMARY, 80);
	in.collect_path = 80;
	CHECK(choose_goal(in) == goal_kind::collect);
	/* ... but not with an enemy in sight (collection stops), unless it is
	 * right there, or the style is Collector.
	 */
	in.target_visible = true;
	in.target_score = 0.8;
	CHECK(choose_goal(in) == goal_kind::engage);
	in.collect = collect_utility(VALUE_BETTER_PRIMARY, 30);
	in.collect_path = 30;
	CHECK(choose_goal(in) == goal_kind::collect);
	in.collect = collect_utility(VALUE_BETTER_PRIMARY, 80);
	in.collect_path = 80;
	in.collector = true;
	in.collect_weight = style_of(bot_style::collector).collect_weight;
	in.engage_weight = style_of(bot_style::collector).engage_weight;
	CHECK(choose_goal(in) == goal_kind::collect);
	in.collector = false;
	in.collect_weight = in.engage_weight = 1;
	/* Weak and threatened: retreat; invulnerable, never. */
	in.shields = 20;
	in.retreat_shields = style_of(bot_style::balanced).retreat_shields;
	CHECK(choose_goal(in) == goal_kind::retreat);
	in.invulnerable = true;
	CHECK(choose_goal(in) != goal_kind::retreat);
	in.invulnerable = false;
	/* Not threatened: no retreat (it collects shields instead). */
	in.has_target = in.target_visible = in.threatened = false;
	in.collect = collect_utility(item_value({item::shield}, with_shields(20)), 100);
	in.collect_path = 100;
	CHECK(choose_goal(in) == goal_kind::collect);
	/* The style's threshold: an Aggressive bot fights on at 25, a
	 * Cautious one retreats at 50.
	 */
	goal_inputs fight;
	fight.has_target = fight.target_visible = fight.threatened = true;
	fight.target_score = 1;
	fight.shields = 25;
	fight.retreat_shields = style_of(bot_style::aggressive).retreat_shields;
	fight.engage_weight = style_of(bot_style::aggressive).engage_weight;
	CHECK(choose_goal(fight) == goal_kind::engage);
	fight.shields = 50;
	fight.retreat_shields = style_of(bot_style::cautious).retreat_shields;
	fight.engage_weight = style_of(bot_style::cautious).engage_weight;
	CHECK(choose_goal(fight) == goal_kind::retreat);
	/* The retreat grows the weaker the bot. */
	fight.shields = 10;
	const double weak{goal_utility(fight)[goal_kind::retreat]};
	fight.shields = 40;
	CHECK(goal_utility(fight)[goal_kind::retreat] < weak);
	/* Out of energy with a fuel centre near: refuel. */
	goal_inputs dry;
	dry.refuel = collect_utility(fuel_centre_value(false, with_energy(5)), 150);
	CHECK(choose_goal(dry) == goal_kind::refuel);
	/* Hysteresis: two close goals do not flip. */
	goal_inputs close;
	close.collect = 1.0;
	close.collect_path = 100;
	close.refuel = 1.1;
	CHECK(choose_goal(close) == goal_kind::refuel);
	close.current = goal_kind::collect;
	CHECK(choose_goal(close) == goal_kind::collect);
	close.refuel = 1.3;
	CHECK(choose_goal(close) == goal_kind::refuel);
}

void test_fuel_centres()
{
	resource_view r;
	r.energy = 100;
	CHECK(fuel_centre_value(false, r) == 0);
	r.energy = 30;
	CHECK(fuel_centre_value(false, r) > 2);
	r.shields = 100;
	CHECK(fuel_centre_value(true, r) == 0);
	r.shields = 30;
	CHECK(fuel_centre_value(true, r) > 2);
	/* The bot stays until full; an enemy in sight makes it leave only if
	 * it has enough to fight.
	 */
	CHECK(keep_refuelling(50, false));
	CHECK(!keep_refuelling(99.6, false));
	CHECK(!keep_refuelling(50, true));
	CHECK(keep_refuelling(20, true));
}

void test_retreat()
{
	/* Away from the threat is better than toward it. */
	const vec3 from{0, 0, 0}, threat{100, 0, 0};
	CHECK(retreat_direction_factor(from, {-100, 0, 0}, threat) == 1);
	CHECK(retreat_direction_factor(from, {0, 100, 0}, threat) == 1);
	CHECK(retreat_direction_factor(from, {100, 0, 0}, threat) < 0.25);
	CHECK(retreat_direction_factor(from, from, threat) == 1);
	/* The flee score: far from the threat, for little flying. */
	CHECK(flee_score({-200, 0, 0}, threat, 200) > flee_score({150, 0, 0}, threat, 150));
	CHECK(flee_score({-200, 0, 0}, threat, 200) > flee_score({-200, 0, 0}, threat, 600));
}

/* Decision 4: the map knowledge by skill. */
void test_map_knowledge()
{
	const auto &trainee{skill_of(bot_skill::trainee)};
	const auto &rookie{skill_of(bot_skill::rookie)};
	const auto &hotshot{skill_of(bot_skill::hotshot)};
	const auto &insane{skill_of(bot_skill::insane)};
	/* A Trainee knows nothing of the map. */
	CHECK(!knows_from_map(true, 0u, trainee.map_knowledge));
	/* A Rookie the layout within 3 segments. */
	CHECK(knows_from_map(true, 3u, rookie.map_knowledge));
	CHECK(!knows_from_map(true, 4u, rookie.map_knowledge));
	CHECK(knows_from_map(true, 8u, hotshot.map_knowledge));
	CHECK(!knows_from_map(true, 9u, hotshot.map_knowledge));
	/* Out of reach of its path search: only the whole-level knowledge. */
	CHECK(!knows_from_map(true, std::nullopt, hotshot.map_knowledge));
	CHECK(knows_from_map(true, std::nullopt, insane.map_knowledge));
	CHECK(knows_from_map(true, 5000u, insane.map_knowledge));
	/* A powerup that was not there at level start (a respawn, a drop)
	 * must be seen or heard, whatever the skill.
	 */
	for (const auto s : {bot_skill::trainee, bot_skill::rookie, bot_skill::hotshot, bot_skill::ace, bot_skill::insane})
		CHECK(!knows_from_map(false, 0u, skill_of(s).map_knowledge));
	/* The knowledge grows with the skill. */
	for (unsigned s = 1; s < BOT_SKILL_COUNT; ++s)
		CHECK(skill_table[s].map_knowledge > skill_table[s - 1].map_knowledge);
	/* A respawn is heard within the hearing radius, for a moment. */
	CHECK(hears_appearance(0.2, 100, hotshot.hearing));
	CHECK(!hears_appearance(0.2, 200, hotshot.hearing));
	CHECK(!hears_appearance(2, 50, hotshot.hearing));
	CHECK(!hears_appearance(0.2, 1, trainee.hearing) || trainee.hearing >= 1);
	CHECK(!hears_appearance(-1, 1, 100));
	/* Places are remembered longer than targets, longer with skill. */
	for (unsigned s = 0; s < BOT_SKILL_COUNT; ++s)
	{
		CHECK(powerup_memory_ms(skill_table[s]) > skill_table[s].memory_ms);
		if (s)
			CHECK(powerup_memory_ms(skill_table[s]) > powerup_memory_ms(skill_table[s - 1]));
	}
}

void test_powerup_memory()
{
	powerup_memory m{4};
	CHECK(m.size() == 0 && !m.knows(1, 10));
	m.learn(kp(1, 10, 2, {1, 0, 0}, 5), 100);
	CHECK(m.knows(1, 10) && !m.knows(1, 11) && !m.knows(2, 10));
	CHECK(m.find(1, 10)->learned_tick == 100);
	/* Refreshing keeps one entry and its ignore time. */
	m.ignore_for(1, 500);
	m.learn(kp(1, 10, 2, {2, 0, 0}, 6), 200);
	CHECK(m.size() == 1 && m.find(1, 10)->segment == 6 && m.find(1, 10)->ignore_until == 500);
	/* The slot holds another object now: it replaces the old entry. */
	m.learn(kp(1, 11, 3), 300);
	CHECK(m.size() == 1 && !m.knows(1, 10) && m.knows(1, 11) && m.find(1, 11)->ignore_until == 0);
	/* Full: the oldest goes. */
	m.learn(kp(2, 1), 310);
	m.learn(kp(3, 1), 320);
	m.learn(kp(4, 1), 330);
	CHECK(m.size() == 4);
	m.learn(kp(5, 1), 340);
	CHECK(m.size() == 4 && !m.knows(1, 11) && m.knows(5, 1));
	/* Forget one; forget the old ones. */
	m.forget(3);
	CHECK(m.size() == 3 && !m.knows(3, 1));
	m.expire(400, 70);
	CHECK(m.size() == 2 && !m.knows(2, 1) && m.knows(4, 1) && m.knows(5, 1));
	/* The tick wraps: the age is still right. */
	powerup_memory w;
	w.learn(kp(7, 7), 0xfffffff0u);
	w.expire(0x10u, 0x40u);
	CHECK(w.knows(7, 7));
	w.expire(0x40u, 0x40u);
	CHECK(!w.knows(7, 7));
	m.clear();
	CHECK(m.size() == 0);
	/* Full: the map alone does not push anything out (no churn on a
	 * level with more powerups than the memory holds), and a perceived
	 * powerup never pushes out one being ignored.
	 */
	powerup_memory f{3};
	CHECK(f.learn(kp(1, 1), 100) && f.learn(kp(2, 1), 110) && f.learn(kp(3, 1), 120));
	CHECK(f.full());
	CHECK(!f.learn(kp(4, 1), 130, false));
	CHECK(f.size() == 3 && !f.knows(4, 1) && f.knows(1, 1));
	/* Refreshing a known one still works when full. */
	CHECK(f.learn(kp(2, 1), 135, false) && f.find(2, 1)->learned_tick == 135);
	f.ignore_for(1, 1000);
	CHECK(f.learn(kp(5, 1), 140));
	CHECK(f.knows(1, 1) && f.find(1, 1)->ignore_until == 1000 && !f.knows(3, 1) && f.knows(5, 1));
	f.ignore_for(2, 1000);
	f.ignore_for(5, 1000);
	CHECK(!f.learn(kp(6, 1), 150));
	CHECK(f.size() == 3 && !f.knows(6, 1));
	/* The ignores run out: the oldest goes again. */
	CHECK(f.learn(kp(6, 1), 1000));
	CHECK(!f.knows(1, 1) && f.knows(2, 1) && f.knows(5, 1) && f.knows(6, 1));
}

/* Section 4.7 and 5.1: the afterburner. */
void test_afterburner()
{
	afterburner_view v;
	v.have = true;
	v.charge = 1;
	v.aligned = true;
	v.chasing_far = true;
	/* Trainee never; Rookie and up when chasing. */
	v.use = afterburner_of(bot_skill::trainee);
	CHECK(!want_afterburner(v));
	for (const auto s : {bot_skill::rookie, bot_skill::hotshot, bot_skill::ace, bot_skill::insane})
	{
		v.use = afterburner_of(s);
		CHECK(want_afterburner(v));
	}
	v.chasing_far = false;
	/* Retreat from Hotshot, dodge from Ace, roaming from Insane. */
	v.retreating = true;
	v.use = afterburner_of(bot_skill::rookie);
	CHECK(!want_afterburner(v));
	v.use = afterburner_of(bot_skill::hotshot);
	CHECK(want_afterburner(v));
	v.retreating = false;
	v.dodging = true;
	CHECK(!want_afterburner(v));
	v.use = afterburner_of(bot_skill::ace);
	CHECK(want_afterburner(v));
	v.dodging = false;
	v.long_straight = true;
	CHECK(!want_afterburner(v));
	v.use = afterburner_of(bot_skill::insane);
	CHECK(want_afterburner(v));
	/* Lit above 30 %, kept down to 5 %. */
	v.charge = 0.25;
	CHECK(!want_afterburner(v));
	v.burning = true;
	CHECK(want_afterburner(v));
	v.charge = 0.04;
	CHECK(!want_afterburner(v));
	v.charge = 1;
	/* Not without one, not when the nose points elsewhere, not without
	 * a reason.
	 */
	v.aligned = false;
	CHECK(!want_afterburner(v));
	v.aligned = true;
	v.have = false;
	CHECK(!want_afterburner(v));
	v.have = true;
	v.long_straight = false;
	CHECK(!want_afterburner(v));
	/* An out-of-range skill: the default's rule. */
	CHECK(afterburner_of(static_cast<bot_skill>(99)) == afterburner_of(BOT_DEFAULT_SKILL));
}

}

int main()
{
	test_bands();
	test_weapon_table();
	test_weapon_resources();
	test_weapon_hysteresis();
	test_long_shots();
	test_values();
	test_collect_utility();
	test_goal_choice();
	test_fuel_centres();
	test_retreat();
	test_map_knowledge();
	test_powerup_memory();
	test_afterburner();
	std::puts("test-bot-goals: all checks passed");
	return 0;
}
