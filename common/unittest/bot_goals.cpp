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
#include <string_view>

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
	CHECK(band_of(150) == range_band::distant);
	CHECK(band_of(1000) == range_band::distant);
	/* With hysteresis: the band changes only BAND_HYSTERESIS past a
	 * border, both ways.
	 */
	CHECK(band_of(62, range_band::close) == range_band::close);
	CHECK(band_of(68.5, range_band::close) == range_band::mid);
	CHECK(band_of(55, range_band::mid) == range_band::mid);
	CHECK(band_of(51, range_band::mid) == range_band::close);
	CHECK(band_of(155, range_band::mid) == range_band::mid);
	CHECK(band_of(160, range_band::mid) == range_band::distant);
	CHECK(band_of(145, range_band::distant) == range_band::distant);
	CHECK(band_of(140, range_band::distant) == range_band::mid);
	CHECK(band_of(62, std::nullopt) == range_band::mid);
	/* A jump across two bands is taken at once. */
	CHECK(band_of(200, range_band::close) == range_band::distant);
	CHECK(band_of(10, range_band::distant) == range_band::close);
}

/* Section 4.5: the table's preferences per band. */
void test_weapon_table()
{
	weapon_view all;
	all.owned = bits({primary::vulcan, primary::spreadfire, primary::plasma, primary::fusion, primary::gauss, primary::helix, primary::phoenix, primary::omega});
	all.vulcan_ammo = 1000;
	all.energy = 150;
	/* Close: omega (charged) first, then helix; mid: plasma; far: gauss. */
	CHECK(choose_primary_for(all, range_band::close, primary::laser) == primary::omega);
	CHECK(choose_primary_for(all, range_band::mid, primary::laser) == primary::plasma);
	CHECK(choose_primary_for(all, range_band::distant, primary::laser) == primary::gauss);
	/* Without a target: the mid band's choice. */
	CHECK(choose_primary_for(all, std::nullopt, primary::laser) == primary::plasma);
	/* Section 9.5: fusion and omega are fired (the bot's own charge);
	 * the super laser is the laser.  Omega does not reach the far band.
	 */
	for (const auto b : {range_band::close, range_band::mid, range_band::distant})
	{
		CHECK(weapon_score(primary::fusion, b, all) > 0);
		CHECK(weapon_score(primary::super_laser, b, all) == 0);
	}
	CHECK(weapon_score(primary::omega, range_band::close, all) > weapon_score(primary::helix, range_band::close, all));
	CHECK(weapon_score(primary::omega, range_band::mid, all) < weapon_score(primary::plasma, range_band::mid, all));
	CHECK(weapon_score(primary::omega, range_band::distant, all) == 0);
	/* An empty omega: helix at close range; an omega recharging from
	 * little energy counts less.
	 */
	weapon_view drained{all};
	drained.omega_charge = 0.1;
	CHECK(choose_primary_for(drained, range_band::close, primary::laser) == primary::helix);
	drained.omega_charge = 0.05;
	drained.energy = 0.5;
	CHECK(omega_factor(drained) == 0);
	CHECK(omega_factor(all) == 1);
	/* Section 9.5: below an eighth of its charge omega does not fire
	 * (do_omega_stuff), unless the energy is gone and some charge is
	 * left: the bot neither chooses nor fires it then.
	 */
	CHECK(omega_can_fire(0.125, 100));
	CHECK(!omega_can_fire(0.12, 100));
	CHECK(!omega_can_fire(0.1, 0.5));
	CHECK(omega_can_fire(0.05, 0));
	CHECK(!omega_can_fire(0, 0));
	weapon_view low{all};
	low.omega_charge = 0.1;
	CHECK(omega_factor(low) == 0);
	CHECK(weapon_score(primary::omega, range_band::close, low) == 0);
	low.energy = 0;
	low.omega_charge = 0.05;
	CHECK(omega_factor(low) > 0);
	low.omega_charge = 0.2;
	low.energy = 150;
	CHECK(omega_factor(low) > 0);
	/* Fusion needs the energy for a charge. */
	weapon_view fusion_only{};
	fusion_only.owned = bits({primary::fusion});
	fusion_only.energy = 100;
	CHECK(choose_primary_for(fusion_only, range_band::close, primary::laser) == primary::fusion);
	CHECK(choose_primary_for(fusion_only, range_band::mid, primary::laser) == primary::fusion);
	fusion_only.energy = FUSION_MIN_ENERGY - 1;
	CHECK(weapon_score(primary::fusion, range_band::close, fusion_only) == 0);
	CHECK(choose_primary_for(fusion_only, range_band::close, primary::fusion) == primary::laser);
	/* Without omega and fusion, the order of B3: helix at close range. */
	weapon_view no_charge{all};
	no_charge.owned = bits({primary::vulcan, primary::spreadfire, primary::plasma, primary::gauss, primary::helix, primary::phoenix});
	CHECK(choose_primary_for(no_charge, range_band::close, primary::laser) == primary::helix);
	/* Close range without helix: spreadfire. */
	weapon_view close{all};
	close.owned = bits({primary::spreadfire, primary::plasma, primary::gauss, primary::vulcan});
	CHECK(choose_primary_for(close, range_band::close, primary::laser) == primary::spreadfire);
	/* Far: the fast shots (gauss, vulcan, the lasers), not the blobs. */
	weapon_view far_view{all};
	far_view.owned = bits({primary::vulcan, primary::spreadfire, primary::helix, primary::phoenix});
	CHECK(choose_primary_for(far_view, range_band::distant, primary::laser) == primary::vulcan);
	far_view.owned = bits({primary::spreadfire, primary::helix, primary::phoenix});
	far_view.laser_level = 3;
	CHECK(choose_primary_for(far_view, range_band::distant, primary::spreadfire) == primary::laser);
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
	CHECK(weapon_score(primary::gauss, range_band::distant, v) == 0);
	CHECK(choose_primary_for(v, range_band::distant, primary::gauss) == primary::laser);
	/* Energy: each weapon by the seconds of fire it still buys, the
	 * laser too (it has no bonus of its own any more).
	 */
	weapon_view e;
	e.energy_rate = default_energy_rate;
	e.energy = default_energy_rate[3] * ENERGY_COMFORT_SECONDS;
	CHECK(energy_factor(primary::plasma, e) == 1);
	e.energy /= 2;
	CHECK(energy_factor(primary::plasma, e) == 0.75);
	e.energy = 0.5;
	CHECK(energy_factor(primary::plasma, e) == 0);
	e.energy = 10;
	/* The cheaper shot lasts longer. */
	CHECK(energy_factor(primary::laser, e) > energy_factor(primary::plasma, e));
	e.energy_rate[0] = e.energy_rate[3];
	CHECK(energy_factor(primary::laser, e) == energy_factor(primary::plasma, e));
	CHECK(energy_factor(primary::vulcan, e) == 1);
	e.energy = 0;
	CHECK(energy_factor(primary::vulcan, e) == 1);
	/* Section 9.3 (a): with 40 energy a bot with the quad super laser
	 * still takes plasma and helix where the table says so (B3 kept the
	 * laser below 50 energy).
	 */
	weapon_view w;
	w.owned = bits({primary::plasma, primary::helix, primary::spreadfire});
	w.laser_level = 5;
	w.quad = true;
	w.energy = 40;
	CHECK(choose_primary_for(w, range_band::close, primary::laser) == primary::helix);
	CHECK(choose_primary_for(w, range_band::mid, primary::laser) == primary::plasma);
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
	/* Mid: plasma 3.8 against helix 3.5 (within 12 %): keep either. */
	CHECK(choose_primary_for(v, range_band::mid, primary::helix) == primary::helix);
	CHECK(choose_primary_for(v, range_band::mid, primary::plasma) == primary::plasma);
	CHECK(choose_primary_for(v, range_band::mid, primary::laser) == primary::plasma);
	/* Close: helix 3.7 against plasma 3.2: switch to helix, and back in
	 * the mid band it stays: a fight across the 60 unit border does not
	 * switch back and forth.
	 */
	CHECK(choose_primary_for(v, range_band::close, primary::plasma) == primary::helix);
	CHECK(choose_primary_for(v, range_band::mid, primary::helix) == primary::helix);
	/* Far: plasma 2.0 against helix 1.2: switch. */
	CHECK(choose_primary_for(v, range_band::distant, primary::helix) == primary::plasma);
	/* Section 9.3 (a): the laser held since the spawn does not keep
	 * the close range choices away.  The quad super laser against
	 * spreadfire and helix at close range: they win, whatever is held;
	 * in the mid band the quad super laser and helix are even, and the
	 * one held stays.
	 */
	weapon_view l;
	l.owned = bits({primary::spreadfire});
	l.laser_level = 5;
	l.quad = true;
	CHECK(choose_primary_for(l, range_band::close, primary::laser) == primary::spreadfire);
	l.owned = bits({primary::helix});
	CHECK(choose_primary_for(l, range_band::close, primary::laser) == primary::helix);
	CHECK(choose_primary_for(l, range_band::mid, primary::helix) == primary::helix);
	CHECK(choose_primary_for(l, range_band::mid, primary::laser) == primary::laser);
	/* Every pair of weapons: a switch in one band is never undone by
	 * the next band's choice with the same weapons (no flipping at a
	 * border), for the close-mid border the fights straddle.
	 */
	for (unsigned a = 0; a < 10; ++a)
		for (unsigned c = 0; c < 10; ++c)
		{
			weapon_view pv;
			pv.owned = static_cast<uint16_t>(bit(primary::laser) | (1u << a) | (1u << c));
			pv.vulcan_ammo = 1000;
			const auto in_close{choose_primary_for(pv, range_band::close, static_cast<primary>(a))};
			const auto in_mid{choose_primary_for(pv, range_band::mid, in_close)};
			CHECK(choose_primary_for(pv, range_band::close, in_mid) == in_mid || choose_primary_for(pv, range_band::close, in_mid) == in_close);
			const auto back{choose_primary_for(pv, range_band::close, in_mid)};
			CHECK(choose_primary_for(pv, range_band::mid, back) == in_mid);
		}
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
	/* A better primary is worth 3-7 by how much better, a spare 1,
	 * one it has nothing.
	 */
	const auto plasma{item_desc{item::primary, primary::plasma, 0}};
	CHECK(upgrade_ratio(plasma, r.weapons) > 3);
	CHECK(item_value(plasma, r) == 7);
	CHECK(upgrade_value(1) == 3 && upgrade_value(2) == 5 && upgrade_value(5) == 7);
	CHECK(item_value({item::primary, primary::phoenix, 0}, r) > 5 && item_value({item::primary, primary::phoenix, 0}, r) < 7);
	r.weapons.owned = bits({primary::helix, primary::plasma});
	CHECK(item_value(plasma, r) == 0);
	CHECK(item_value({item::primary, primary::spreadfire, 0}, r) == VALUE_SPARE_PRIMARY);
	CHECK(upgrade_ratio({item::primary, primary::spreadfire, 0}, r.weapons) == 1);
	/* Section 9.5: fusion and omega are fired now: an upgrade like the
	 * others (B3: a spare, 1).
	 */
	r.weapons.owned = bits({});
	CHECK(item_value({item::primary, primary::fusion, 0}, r) == 7);
	CHECK(item_value({item::primary, primary::omega, 0}, r) > 3);
	/* A new cannon is better than the laser (it comes with rounds). */
	CHECK(item_value({item::primary, primary::gauss, 0}, r) == 7);
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
	CHECK(item_value({item::laser}, r) == upgrade_value(1.25));
	CHECK(std::abs(item_value({item::super_laser}, r) - 5.6) < 1e-9);
	CHECK(item_value({item::quad}, r) > 3 && item_value({item::quad}, r) < 4);
	r.weapons.owned = bits({primary::plasma, primary::helix});
	CHECK(item_value({item::laser}, r) == VALUE_SPARE_PRIMARY);
	CHECK(item_value({item::quad}, r) == 2);
	/* Section 5.1 values, and the items a bot never seeks. */
	CHECK(item_value({item::secondary, primary::laser, 0}, r) == 1);
	/* Section 9.8: the big missiles are high-value grabs. */
	CHECK(item_value({item::secondary, primary::laser, 9}, r) == 3);
	CHECK(item_value({item::secondary, primary::laser, 4}, r) == 2.5);
	CHECK(item_value({item::secondary, primary::laser, 3}, r) == 2);
	for (const uint8_t i : {3, 4, 9})
		CHECK(item_value({item::secondary, primary::laser, i}, r) >= GRAB_HIGH_VALUE);
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
	CHECK(collect_utility(5.0, 300) > collect_utility(0.5, 20));
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
	in.collect = collect_utility(5.0, 80);
	in.collect_path = 80;
	CHECK(choose_goal(in) == goal_kind::collect);
	/* ... but not with an enemy in sight (collection stops), unless it is
	 * right there, or the style is Collector.
	 */
	in.target_visible = true;
	in.target_score = 0.8;
	CHECK(choose_goal(in) == goal_kind::engage);
	in.collect = collect_utility(5.0, 30);
	in.collect_path = 30;
	CHECK(choose_goal(in) == goal_kind::collect);
	in.collect = collect_utility(5.0, 80);
	in.collect_path = 80;
	/* Section 9.3 (a): a much better weapon (the spawn laser against
	 * plasma: value 7) is worth the detour in a fight too; the bot
	 * shoots on its way.
	 */
	CHECK(choose_goal(in) == goal_kind::engage);
	in.collect = collect_utility(7, 80);
	in.collect_upgrade = true;
	CHECK(choose_goal(in) == goal_kind::collect);
	in.collect_upgrade = false;
	CHECK(choose_goal(in) == goal_kind::engage);
	in.collect = collect_utility(5.0, 80);
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
	/* The style's threshold: an Aggressive bot fights on at 30, a
	 * Cautious one retreats at 60.
	 */
	goal_inputs fight;
	fight.has_target = fight.target_visible = fight.threatened = true;
	fight.target_score = 1;
	fight.shields = 30;
	fight.retreat_shields = style_of(bot_style::aggressive).retreat_shields;
	fight.engage_weight = style_of(bot_style::aggressive).engage_weight;
	CHECK(choose_goal(fight) == goal_kind::engage);
	fight.shields = 60;
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

/* Section 9.5: the playtest's situations with the real numbers, and the
 * opportunistic pickup.  A Hotshot bot (awareness 350) with the spawn
 * laser and full shields fights a visible enemy 100 units away.
 */
void test_pickup_scenarios()
{
	const double awareness{skill_of(bot_skill::hotshot).awareness};
	const double enemy_score{target_score(target_candidate{.id = 0, .visible = true, .distance = 100}, awareness)};
	CHECK(enemy_score > 0.9 && enemy_score < 0.95);
	resource_view spawn;
	spawn.weapons.energy = spawn.energy;
	const auto fight_with{[&](const item_desc &d, const resource_view &r, const double distance_to) {
		goal_inputs in;
		in.has_target = in.target_visible = in.threatened = true;
		in.target_score = enemy_score;
		in.shields = r.shields;
		in.collect = collect_utility(item_value(d, r), distance_to);
		in.collect_path = distance_to;
		in.collect_upgrade = collect_in_fight(d, r);
		in.grab = grab_worthwhile(item_value(d, r), distance_to, distance_to);
		in.grab_value = item_value(d, r);
		in.grab_shields = d.kind == item::shield;
		in.grab_path = distance_to;
		in.current = goal_kind::engage;
		return in;
	}};
	/* Plasma 20 units away: worth 7, taken (it was, if known). */
	const item_desc plasma{item::primary, primary::plasma, 0};
	CHECK(item_value(plasma, spawn) == 7);
	CHECK(choose_goal(fight_with(plasma, spawn, 20)) == goal_kind::collect);
	/* ... and 80 units away (a much better weapon, section 9.3). */
	CHECK(choose_goal(fight_with(plasma, spawn, 80)) == goal_kind::collect);
	/* An earthshaker 20 units away: B3 fought on past it (worth 2: 1.5
	 * against the fight's 1.85, times 1.2 for the current goal).  The
	 * grab takes it.
	 */
	const item_desc shaker{item::secondary, primary::laser, static_cast<uint8_t>(secondary::earthshaker)};
	auto in{fight_with(shaker, spawn, 20)};
	CHECK(goal_utility(in)[goal_kind::engage] > collect_utility(2, 20));
	/* Section 9.8: now worth 3, it is taken even without the grab. */
	in.grab = false;
	CHECK(choose_goal(in) == goal_kind::collect);
	in.grab = true;
	CHECK(choose_goal(in) == goal_kind::collect);
	/* Section 9.8: a high-value grab reaches 85 units (the old radius:
	 * 45); beyond it the fight goes on.  Section 9.9: in a fight (not
	 * weak) only as a detour of GRAB_DETOUR_HIGH_PATH; weak, as before.
	 */
	CHECK(choose_goal(fight_with(shaker, spawn, 60)) == goal_kind::collect);
	CHECK(choose_goal(fight_with(shaker, spawn, 80)) == goal_kind::engage);
	CHECK(choose_goal(fight_with(shaker, spawn, 100)) == goal_kind::engage);
	{
		auto w{fight_with(shaker, spawn, 80)};
		w.weak = true;
		CHECK(choose_goal(w) == goal_kind::collect);
		w = fight_with(shaker, spawn, 100);
		w.weak = true;
		CHECK(choose_goal(w) == goal_kind::engage);
	}
	/* A concussion pack 30 units away: grabbed; energy at full energy:
	 * not worth it.
	 */
	const item_desc concussion{item::secondary, primary::laser, static_cast<uint8_t>(secondary::concussion)};
	CHECK(choose_goal(fight_with(concussion, spawn, 20)) == goal_kind::collect);
	CHECK(choose_goal(fight_with(concussion, spawn, 30)) == goal_kind::engage);
	CHECK(choose_goal(fight_with({item::energy}, spawn, 10)) == goal_kind::engage);
	CHECK(choose_goal(fight_with({item::shield}, spawn, 10)) == goal_kind::engage);
	/* Energy it needs is grabbed. */
	resource_view drained{spawn};
	drained.energy = drained.weapons.energy = 30;
	CHECK(choose_goal(fight_with({item::energy}, drained, 25)) == goal_kind::collect);
	/* A vulcan owner and plasma 100 units away: not a big upgrade
	 * (x1.46), so not in a fight; 80 units away (section 9.8: a better
	 * gun is a high-value grab) and 30: grabbed.
	 */
	resource_view vulcan{spawn};
	vulcan.weapons.owned = bits({primary::vulcan});
	vulcan.weapons.vulcan_ammo = 1000;
	CHECK(upgrade_ratio(plasma, vulcan.weapons) < BIG_UPGRADE_RATIO);
	CHECK(choose_goal(fight_with(plasma, vulcan, 100)) == goal_kind::engage);
	CHECK(choose_goal(fight_with(plasma, vulcan, 80)) == goal_kind::engage);
	CHECK(choose_goal(fight_with(plasma, vulcan, 55)) == goal_kind::collect);
	CHECK(choose_goal(fight_with(plasma, vulcan, 30)) == goal_kind::collect);
	/* Shields: at 50 a shield powerup is worth a detour in a fight (0.8,
	 * not 0.35); close by it is grabbed.
	 */
	resource_view hurt{spawn};
	hurt.shields = 50;
	CHECK(collect_in_fight({item::shield}, hurt));
	CHECK(!collect_in_fight({item::shield}, spawn));
	CHECK(!collect_in_fight(concussion, hurt));
	CHECK(collect_in_fight(plasma, spawn));
	CHECK(choose_goal(fight_with({item::shield}, hurt, 40)) == goal_kind::collect);
	/* In danger (weak and threatened) no grab, retreat; but shields are
	 * grabbed.
	 */
	resource_view weak{spawn};
	weak.shields = 20;
	auto d{fight_with(shaker, weak, 20)};
	d.retreat_shields = 35;
	CHECK(in_danger(d));
	CHECK(!grab_applies(d));
	CHECK(choose_goal(d) == goal_kind::retreat);
	auto s{fight_with({item::shield}, weak, 20)};
	s.retreat_shields = 35;
	CHECK(grab_applies(s));
	CHECK(choose_goal(s) == goal_kind::collect);
	/* Invulnerable: no danger. */
	d.invulnerable = true;
	CHECK(!in_danger(d) && grab_applies(d));
	/* The grab's limits. */
	CHECK(grab_worthwhile(GRAB_MIN_VALUE, GRAB_RADIUS, GRAB_PATH));
	CHECK(!grab_worthwhile(GRAB_MIN_VALUE - 0.01, 10, 10));
	CHECK(!grab_worthwhile(1.5, GRAB_RADIUS + 1, 50));
	CHECK(!grab_worthwhile(1.5, 30, GRAB_PATH + 1));
	/* Section 9.8: the high-value ones from further. */
	CHECK(grab_worthwhile(GRAB_HIGH_VALUE, GRAB_HIGH_RADIUS, GRAB_HIGH_PATH));
	CHECK(!grab_worthwhile(GRAB_HIGH_VALUE, GRAB_HIGH_RADIUS + 1, 50));
	CHECK(!grab_worthwhile(GRAB_HIGH_VALUE, 30, GRAB_HIGH_PATH + 1));
	/* Without an enemy the grab wins over a far collection and roaming. */
	goal_inputs calm;
	calm.collect = collect_utility(7, 300);
	calm.grab = true;
	CHECK(choose_goal(calm) == goal_kind::collect);
	CHECK(goal_utility(calm)[goal_kind::collect] == GRAB_UTILITY);
}

/* Section 9.5, the laser preference: a bot holding the spawn laser that
 * owns one other gun takes it in every band where the table ranks it
 * above the laser, at full energy and at little, from the laser as the
 * current weapon (its hysteresis).
 */
void test_owned_weapon_selection()
{
	for (const double energy : {100.0, 60.0, 15.0})
		for (const auto w : {primary::vulcan, primary::spreadfire, primary::plasma, primary::helix, primary::gauss, primary::phoenix, primary::fusion, primary::omega})
		{
			weapon_view v;
			v.owned = bits({w});
			v.energy = energy;
			v.vulcan_ammo = 1000;
			for (const auto band : {range_band::close, range_band::mid, range_band::distant})
			{
				/* Far: the slow blobs and the short omega are worse
				 * than the laser; so is helix, low on energy.
				 */
				const bool laser_better{
					band == range_band::distant && (w == primary::spreadfire || w == primary::phoenix || w == primary::omega || (w == primary::helix && energy < 20))
				};
				const auto chosen{choose_primary_for(v, band, primary::laser)};
				CHECK(chosen == (laser_better ? primary::laser : w));
			}
		}
	/* With a laser of level 4 and quad (3.4 at close range), the close
	 * range guns still win over it.
	 */
	weapon_view strong;
	strong.laser_level = 3;
	strong.quad = true;
	for (const auto w : {primary::spreadfire, primary::helix, primary::omega, primary::fusion})
	{
		strong.owned = bits({w});
		CHECK(choose_primary_for(strong, range_band::close, primary::laser) == w);
	}
}

/* Section 9.5: a powerup close by is noticed outside the field of view,
 * any within the awareness inside it.
 */
void test_noticing()
{
	CHECK(notices_powerup(30, false, 350));
	CHECK(notices_powerup(POWERUP_NOTICE_DISTANCE, false, 350));
	CHECK(!notices_powerup(POWERUP_NOTICE_DISTANCE + 1, false, 350));
	CHECK(notices_powerup(300, true, 350));
	CHECK(!notices_powerup(400, true, 350));
	/* A Trainee's small awareness still limits. */
	CHECK(!notices_powerup(50, false, 40));
	/* Section 9.5: the map knowledge also reaches its segments' worth of
	 * path: a Hotshot (8 segments) in a spawn area of small segments
	 * knows the powerups 300 units away through 20 of them.
	 */
	const unsigned hotshot{skill_of(bot_skill::hotshot).map_knowledge};
	CHECK(!knows_from_map(true, 20u, hotshot));
	CHECK(knows_from_map(true, 20u, hotshot, 300.0));
	CHECK(!knows_from_map(true, 20u, hotshot, hotshot * MAP_KNOWLEDGE_UNITS_PER_SEGMENT + 1));
	CHECK(knows_from_map(true, 5u, hotshot, 1000.0));
	CHECK(!knows_from_map(false, 1u, hotshot, 10.0));
	CHECK(!knows_from_map(true, 1u, 0, 10.0));
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
	/* Retreat from Hotshot, dodge from Ace. */
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
	/* Section 9.12: long straight flights from Hotshot, only with most
	 * of the charge and down to a reserve (Insane spends more).
	 */
	v.long_straight = true;
	v.use = afterburner_of(bot_skill::rookie);
	CHECK(!want_afterburner(v));
	v.use = afterburner_of(bot_skill::hotshot);
	CHECK(want_afterburner(v));
	v.charge = AFTERBURNER_ROAM_LIGHT - 0.01;
	CHECK(!want_afterburner(v));
	v.burning = true;
	CHECK(want_afterburner(v));
	v.charge = AFTERBURNER_ROAM_KEEP - 0.01;
	CHECK(!want_afterburner(v));
	v.use = afterburner_of(bot_skill::insane);
	CHECK(want_afterburner(v));
	v.charge = AFTERBURNER_ROAM_KEEP_INSANE - 0.01;
	CHECK(!want_afterburner(v));
	v.burning = false;
	v.charge = AFTERBURNER_ROAM_LIGHT_INSANE + 0.01;
	CHECK(want_afterburner(v));
	v.charge = 1;
	v.long_straight = false;
	v.chasing_far = true;
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
	v.chasing_far = false;
	CHECK(!want_afterburner(v));
	/* An out-of-range skill: the default's rule. */
	CHECK(afterburner_of(static_cast<bot_skill>(99)) == afterburner_of(BOT_DEFAULT_SKILL));
}

/* Section 9.8: a high-value powerup close by is worth a detour, in a
 * fight too ("several bots fly close by a mega missile and not take a
 * minor detour").
 */
void test_high_value_grab()
{
	resource_view r;
	const item_desc mega{item::secondary, primary::laser, 4};
	const item_desc conc{item::secondary, primary::laser, 0};
	const double v_mega{item_value(mega, r)}, v_conc{item_value(conc, r)};
	/* A mega 70 units away, 100 by path: a detour; a concussion there is
	 * not, but still one within the old radius.
	 */
	CHECK(grab_worthwhile(v_mega, 70, 100));
	CHECK(!grab_worthwhile(v_conc, 70, 100));
	CHECK(grab_worthwhile(v_conc, 40, 60));
	CHECK(!grab_worthwhile(v_mega, 100, 100));
	CHECK(!grab_worthwhile(v_mega, 70, 150));
	CHECK(grab_utility(v_mega) == GRAB_HIGH_UTILITY && grab_utility(v_conc) == GRAB_UTILITY);
	/* The scan's first filter (best_grab) lets the high-value radius
	 * through: a mega 70 units away is a candidate, a concussion there
	 * is not, nothing beyond GRAB_HIGH_RADIUS is.
	 */
	CHECK(grab_in_range(70) && grab_in_range(GRAB_HIGH_RADIUS) && !grab_in_range(GRAB_HIGH_RADIUS + 1));
	CHECK(grab_candidate(v_mega, 70, 100));
	CHECK(grab_candidate(v_mega, 84, 120));
	CHECK(!grab_candidate(v_conc, 70, 100));
	CHECK(grab_candidate(v_conc, 40, 60));
	CHECK(!grab_candidate(v_mega, 90, 100));
	/* The high-value items, with the spawn laser. */
	for (const item_desc d : {item_desc{item::secondary, primary::laser, 3}, item_desc{item::secondary, primary::laser, 9}, item_desc{item::invulnerability}, item_desc{item::cloak}, item_desc{item::afterburner}, item_desc{item::quad}, item_desc{item::super_laser}, item_desc{item::primary, primary::plasma, 0}})
	{
		CHECK(item_value(d, r) >= GRAB_HIGH_VALUE);
		CHECK(collect_in_fight(d, r));
	}
	CHECK(!collect_in_fight(conc, r));
	/* The afterburner the bot has is worth nothing. */
	r.afterburner = true;
	CHECK(item_value({item::afterburner}, r) == 0);
	/* An aggressive bot that was just shot (target score 1.5, engage
	 * weight 1.5, engaged already): it takes the mega on its way, not a
	 * concussion pack.
	 */
	goal_inputs in;
	in.has_target = true;
	in.target_visible = true;
	in.target_score = 1.5;
	in.threatened = true;
	in.engage_weight = 1.5;
	in.collect_weight = 0.6;
	in.retreat_shields = 20;
	in.current = goal_kind::engage;
	in.grab = true;
	in.grab_value = v_mega;
	in.grab_path = 50;
	CHECK(choose_goal(in) == goal_kind::collect);
	in.grab_value = v_conc;
	CHECK(choose_goal(in) == goal_kind::engage);
	/* Section 9.9: on its way (a short detour) the pack too. */
	in.grab_path = 20;
	CHECK(choose_goal(in) == goal_kind::collect);
	in.grab_path = 50;
	/* In danger it retreats, but invulnerability is taken. */
	in.shields = 10;
	in.target_score = 0.8;
	in.current.reset();
	in.grab_value = v_mega;
	CHECK(choose_goal(in) == goal_kind::retreat);
	in.grab_value = item_value({item::invulnerability}, resource_view{});
	in.grab_invulnerability = true;
	CHECK(choose_goal(in) == goal_kind::collect);
}

/* Section 9.8: the early-life power-up phase and third parties. */
void test_powerup_phase()
{
	std::array<uint8_t, BOT_SECONDARY_COUNT> none{};
	weapon_view spawn;
	CHECK(weak_armament(spawn, none));
	/* Laser 3 is still weak, laser 4 and quad laser 2 are not. */
	spawn.laser_level = 2;
	CHECK(weak_armament(spawn, none));
	spawn.laser_level = 3;
	CHECK(!weak_armament(spawn, none));
	spawn.laser_level = 1;
	spawn.quad = true;
	CHECK(!weak_armament(spawn, none));
	spawn = {};
	/* A gun, or strong secondaries: not weak. */
	{
		auto w{spawn};
		w.owned = bits({primary::plasma});
		CHECK(!weak_armament(w, none));
		auto mega{none};
		mega[static_cast<unsigned>(secondary::mega)] = 1;
		CHECK(!weak_armament(spawn, mega));
		auto homing{none};
		homing[static_cast<unsigned>(secondary::homing)] = 4;
		CHECK(!weak_armament(spawn, homing));
		auto conc{none};
		conc[static_cast<unsigned>(secondary::concussion)] = 4;
		CHECK(weak_armament(spawn, conc));
	}
	/* Weapon-poor levels: fewer weapon powerups than half the players,
	 * or fewer than two.
	 */
	CHECK(weapon_poor_level(0, 2));
	CHECK(weapon_poor_level(1, 2));
	CHECK(!weapon_poor_level(2, 2));
	CHECK(weapon_poor_level(3, 8));
	CHECK(!weapon_poor_level(4, 8));
	CHECK(!weapon_poor_level(12, 8));
	CHECK(is_weapon_item(item::primary) && is_weapon_item(item::laser) && is_weapon_item(item::quad) && is_weapon_item(item::super_laser));
	CHECK(!is_weapon_item(item::secondary) && !is_weapon_item(item::shield));
	/* The phase. */
	const powerup_phase_view weak{
		.weak = true,
		.upgrade_known = true,
	};
	CHECK(in_powerup_phase(weak));
	for (const auto change : {0, 1, 2, 3, 4, 5})
	{
		auto v{weak};
		switch (change)
		{
			case 0: v.weak = false; break;
			case 1: v.upgrade_known = false; break;
			/* Few weapons on the level: fight with laser 1. */
			case 2: v.weapon_poor_level = true; break;
			/* Attacked at close range: fight back. */
			case 3: v.attacked_close = true; break;
			case 4: v.invulnerable = true; break;
			/* The target as weak and alone: a fair fight. */
			default: v.target_weak = v.target_alone = true; break;
		}
		CHECK(!in_powerup_phase(v));
	}
	{
		auto v{weak};
		v.target_weak = true;
		CHECK(in_powerup_phase(v));
		v.target_weak = false;
		v.target_alone = true;
		CHECK(in_powerup_phase(v));
	}
	/* By style: Collector strongest, Aggressive least. */
	CHECK(powerup_phase_engage(bot_style::collector) < powerup_phase_engage(bot_style::cautious));
	CHECK(powerup_phase_engage(bot_style::cautious) < powerup_phase_engage(bot_style::balanced));
	CHECK(powerup_phase_engage(bot_style::balanced) < powerup_phase_engage(bot_style::aggressive));
	CHECK(powerup_phase_collect(bot_style::collector) > powerup_phase_collect(bot_style::balanced));
	CHECK(powerup_phase_collect(bot_style::aggressive) <= powerup_phase_collect(bot_style::balanced));
	/* The goal: a weak balanced bot with a target in sight and plasma
	 * 200 units away collects in the phase, fights without it (the
	 * weapon-poor level).
	 */
	resource_view r;
	const double plasma{item_value({item::primary, primary::plasma, 0}, r)};
	goal_inputs in;
	in.has_target = true;
	in.target_visible = true;
	in.target_score = 1;
	in.threatened = true;
	in.collect = collect_utility(plasma, 200);
	in.collect_path = 200;
	in.collect_upgrade = true;
	CHECK(choose_goal(in) == goal_kind::engage);
	for (const auto st : {bot_style::balanced, bot_style::cautious, bot_style::collector})
	{
		auto p{in};
		p.phase_engage = powerup_phase_engage(st);
		p.phase_collect = collect_utility(plasma, 200) * powerup_phase_collect(st);
		CHECK(choose_goal(p) == goal_kind::collect);
	}
	/* An aggressive bot fights on unless the upgrade is nearer. */
	{
		auto p{in};
		p.engage_weight = 1.5;
		p.collect_weight = 0.6;
		p.phase_engage = powerup_phase_engage(bot_style::aggressive);
		p.phase_collect = collect_utility(plasma, 200) * powerup_phase_collect(bot_style::aggressive);
		CHECK(choose_goal(p) == goal_kind::engage);
		p.phase_collect = collect_utility(plasma, 60) * powerup_phase_collect(bot_style::aggressive);
		CHECK(choose_goal(p) == goal_kind::collect);
	}
	/* Third parties: stronger enemies near make the dogfight less
	 * attractive, the more of them the less; the aggressive bot minds
	 * least.
	 */
	for (const auto st : {bot_style::balanced, bot_style::aggressive, bot_style::cautious, bot_style::collector})
	{
		CHECK(third_party_factor(st, 0) == 1);
		CHECK(third_party_factor(st, 1) < 1);
		CHECK(third_party_factor(st, 2) < third_party_factor(st, 1));
		CHECK(third_party_factor(st, 2) > 0.1);
		CHECK(third_party_factor(bot_style::aggressive, 1) >= third_party_factor(st, 1));
	}
	{
		auto p{in};
		p.collect = 0;
		p.collect_upgrade = false;
		p.target_score = 0.6;
		p.phase_collect = 0.8;
		CHECK(choose_goal(p) == goal_kind::engage);
		p.third_party = third_party_factor(bot_style::balanced, 1);
		CHECK(choose_goal(p) == goal_kind::collect);
	}
}

/* Section 9.9: after the exp-19 playtest ("while alive the bots are
 * more hesitant"; "bots still do not properly react to powerups in
 * their vicinity"): grabs only as detours in a fight, the nearest
 * reasonable powerup first, armed bots fight and seek fights.
 */
void test_log_tuning_goals()
{
	const double v_mega{item_value({item::secondary, primary::laser, static_cast<uint8_t>(secondary::mega)}, resource_view{})};
	const double v_conc{item_value({item::secondary, primary::laser, static_cast<uint8_t>(secondary::concussion)}, resource_view{})};
	/* The log's typical fight: an Insane balanced bot, an enemy in sight
	 * 74 units away (engage 2.0), a mega within the grab's radius.
	 */
	goal_inputs in;
	in.has_target = in.target_visible = in.threatened = true;
	in.target_score = 1;
	in.current = goal_kind::engage;
	in.grab = true;
	in.grab_value = v_mega;
	in.grab_path = 80;
	/* Section 9.8 left the fight for it (6.5 against 2.4); now not. */
	CHECK(goal_utility(in)[goal_kind::engage] == 2);
	CHECK(choose_goal(in) == goal_kind::engage);
	CHECK(goal_utility(in)[goal_kind::collect] == 0);
	/* A detour: taken, worth a bit more than the fight. */
	in.grab_path = 50;
	CHECK(choose_goal(in) == goal_kind::collect);
	{
		const auto u{goal_utility(in)};
		CHECK(u.collect_from == collect_source::grab);
		CHECK(std::abs(u[goal_kind::collect] - GRAB_DETOUR_FACTOR * 2) < 1e-9);
		/* Kept against the fight's hysteresis, and once collecting. */
		CHECK(u[goal_kind::collect] > GOAL_HYSTERESIS * u[goal_kind::engage]);
	}
	in.current = goal_kind::collect;
	CHECK(choose_goal(in) == goal_kind::collect);
	in.current = goal_kind::engage;
	/* A concussion pack: only right on the way. */
	in.grab_value = v_conc;
	CHECK(choose_goal(in) == goal_kind::engage);
	in.grab_path = 30;
	CHECK(choose_goal(in) == goal_kind::engage);
	in.grab_path = 20;
	CHECK(choose_goal(in) == goal_kind::collect);
	/* A Collector goes further for it. */
	in.grab_path = 35;
	in.collector = true;
	CHECK(choose_goal(in) == goal_kind::collect);
	in.grab_path = 45;
	CHECK(choose_goal(in) == goal_kind::engage);
	in.collector = false;
	/* Weak (it needs weapons): the grab of section 9.8. */
	in.grab_value = v_mega;
	in.grab_path = 120;
	in.weak = true;
	CHECK(choose_goal(in) == goal_kind::collect);
	CHECK(goal_utility(in)[goal_kind::collect] == GRAB_HIGH_UTILITY);
	in.weak = false;
	/* No enemy: the grab of section 9.8. */
	{
		goal_inputs calm;
		calm.grab = true;
		calm.grab_value = v_mega;
		calm.grab_path = 120;
		calm.armed = armed_level::heavy;
		CHECK(goal_utility(calm)[goal_kind::collect] == GRAB_HIGH_UTILITY);
	}
	/* The enemy known, out of sight: a detour only (it hunts), unarmed
	 * (a gun, no missiles) or armed.
	 */
	in.target_visible = false;
	in.target_score = 0.4;
	CHECK(choose_goal(in) == goal_kind::hunt);
	CHECK(goal_utility(in)[goal_kind::collect] == 0);
	/* PR #41 review: with a decent gun, no missiles, the mega 120 units
	 * away and the target's visibility flickering (5 Hz), the choice
	 * stays: neither leg gives the grab its full utility.
	 */
	{
		goal_inputs f{in};
		for (unsigned i{0}; i != 10; ++i)
		{
			f.target_visible = i % 2 == 0;
			const goal_kind g{choose_goal(f)};
			CHECK(g != goal_kind::collect);
			CHECK(goal_utility(f)[goal_kind::collect] == 0);
			f.current = g;
		}
		/* Within the detour's reach, the grab both ways. */
		f.grab_path = 50;
		for (unsigned i{0}; i != 10; ++i)
		{
			f.target_visible = i % 2 == 0;
			CHECK(choose_goal(f) == goal_kind::collect);
			f.current = goal_kind::collect;
		}
	}
	in.armed = armed_level::light;
	CHECK(choose_goal(in) == goal_kind::hunt);
	in.grab_path = 40;
	CHECK(choose_goal(in) == goal_kind::collect);
	CHECK(goal_utility(in)[goal_kind::collect] > goal_utility(in)[goal_kind::hunt]);
	/* The nearest reasonable powerup first: the log's smart missile 14
	 * units away before a quad 45 away (value 3.6); a mega 80 units away
	 * after a concussion pack at 20.
	 */
	CHECK(grab_rank(2, 14) > grab_rank(3.6, 45));
	CHECK(grab_rank(1, 20) > grab_rank(2.5, 80));
	CHECK(grab_rank(3.6, 30) > grab_rank(2, 30));
	/* What the bot holds: heavy (smart, mega, earthshaker) from Hotshot,
	 * light (3 concussion, homing, mercury) from Rookie.
	 */
	{
		std::array<uint8_t, BOT_SECONDARY_COUNT> ammo{};
		CHECK(armed_of(ammo, 4) == armed_level::none);
		ammo[static_cast<unsigned>(secondary::concussion)] = 2;
		CHECK(armed_of(ammo, 4) == armed_level::none);
		ammo[static_cast<unsigned>(secondary::homing)] = 1;
		CHECK(armed_of(ammo, 4) == armed_level::light);
		CHECK(armed_of(ammo, 0) == armed_level::none);
		ammo[static_cast<unsigned>(secondary::mega)] = 1;
		CHECK(armed_of(ammo, 2) == armed_level::heavy);
		CHECK(armed_of(ammo, 1) == armed_level::light);
		ammo = {};
		ammo[static_cast<unsigned>(secondary::guided)] = 4;
		ammo[static_cast<unsigned>(secondary::flash)] = 4;
		CHECK(armed_of(ammo, 4) == armed_level::none);
		CHECK(std::string_view{name_of(armed_level::heavy)} == "heavy");
	}
	/* Armed: the fight counts more, a plain collection less (not a big
	 * upgrade, not weak).
	 */
	{
		goal_inputs a;
		a.has_target = a.target_visible = a.threatened = true;
		a.target_score = 0.8;
		a.collect = 2;
		a.collect_path = 30;
		const auto u0{goal_utility(a)};
		a.armed = armed_level::heavy;
		const auto u1{goal_utility(a)};
		CHECK(std::abs(u1[goal_kind::engage] - 1.5 * u0[goal_kind::engage]) < 1e-9);
		CHECK(std::abs(u1[goal_kind::collect] - ARMED_COLLECT * u0[goal_kind::collect]) < 1e-9);
		CHECK(choose_goal(a) == goal_kind::engage);
		a.armed = armed_level::none;
		CHECK(choose_goal(a) == goal_kind::collect);
		a.armed = armed_level::light;
		a.collect_upgrade = true;
		CHECK(goal_utility(a)[goal_kind::collect] == u0[goal_kind::collect]);
	}
	/* No target, armed: it seeks the last enemy seen, above a plain
	 * collection (the log's median 0.78) and roaming, below a grab.
	 */
	{
		goal_inputs s;
		s.collect = 0.78;
		s.collect_path = 150;
		CHECK(choose_goal(s) == goal_kind::collect);
		s.armed = armed_level::light;
		s.seek = seek_utility(s.armed, 1);
		CHECK(choose_goal(s) == goal_kind::hunt);
		s.armed = armed_level::heavy;
		s.seek = seek_utility(s.armed, 1);
		CHECK(choose_goal(s) == goal_kind::hunt);
		CHECK(seek_utility(armed_level::heavy, 1) > seek_utility(armed_level::light, 1));
		CHECK(seek_utility(armed_level::none, 1.5) == 0);
		CHECK(seek_utility(armed_level::light, 0.7) > ROAM_UTILITY);
		/* A grab while seeking: a detour, as while hunting. */
		s.grab = true;
		s.grab_value = v_conc;
		s.grab_path = 40;
		CHECK(choose_goal(s) == goal_kind::hunt);
		s.grab_path = 20;
		CHECK(choose_goal(s) == goal_kind::collect);
		s.grab_value = v_mega;
		s.grab_path = 55;
		CHECK(choose_goal(s) == goal_kind::collect);
		/* Nothing to seek: the grab of section 9.8. */
		s.seek = 0;
		s.grab_path = 120;
		CHECK(goal_utility(s)[goal_kind::collect] == GRAB_HIGH_UTILITY);
		/* A known target replaces the seek. */
		s.grab = false;
		s.has_target = true;
		s.target_score = 0.1;
		CHECK(std::abs(goal_utility(s)[goal_kind::hunt] - 2 * 0.1 * 1.5) < 1e-9);
	}
	/* A Collector in sight of an enemy: its collection counts less
	 * (section 4.7: not at all), so an even fight goes on for a small
	 * prize.
	 */
	{
		goal_inputs c;
		c.has_target = c.target_visible = c.threatened = true;
		c.target_score = 0.9;
		c.engage_weight = style_of(bot_style::collector).engage_weight;
		c.collect_weight = style_of(bot_style::collector).collect_weight;
		c.collector = true;
		c.collect = 0.8;
		c.collect_path = 80;
		CHECK(std::abs(goal_utility(c)[goal_kind::collect] - 0.8 * 1.8 * COLLECTOR_UNDER_FIRE) < 1e-9);
		CHECK(choose_goal(c) == goal_kind::engage);
		c.collect = 2;
		CHECK(choose_goal(c) == goal_kind::collect);
		c.armed = armed_level::heavy;
		CHECK(choose_goal(c) == goal_kind::engage);
	}
	/* PR #41 review: a place sought is done once reached, or when no
	 * path to it is found (not planned for again every tick).
	 */
	CHECK(seek_place_done(SEEK_ARRIVED, true));
	CHECK(!seek_place_done(SEEK_ARRIVED + 1, true));
	CHECK(seek_place_done(SEEK_ARRIVED + 100, false));
	/* The phase's upgrade is reported as the collection's source. */
	{
		goal_inputs p;
		p.collect = 0.5;
		p.collect_path = 100;
		p.phase_collect = 1.2;
		CHECK(goal_utility(p).collect_from == collect_source::phase);
		p.phase_collect = 0.1;
		CHECK(goal_utility(p).collect_from == collect_source::plain);
	}
}

bool close_to(const double a, const double b)
{
	return std::abs(a - b) <= 1e-9 * std::max(1.0, std::abs(b));
}

/* Section 9.10: pursuit.  The persistence by skill and style, the
 * conditions to start (an engagement just broken off, hits landed, the
 * target damaged, the bot stronger), the break-off (weak for the style,
 * an obvious ambush) and its hysteresis, the prediction's distance, and
 * the goal choice while pursuing (the hunt beats collections; only a
 * very close high-value grab).
 */
void test_pursuit()
{
	/* Persistence: longer with the skill; Aggressive long, Cautious
	 * short; never beyond the target memory it replaces.
	 */
	for (unsigned k = 0; k < BOT_SKILL_COUNT; ++k)
	{
		const auto skill{static_cast<bot_skill>(k)};
		if (k)
			CHECK(pursuit_seconds(skill, bot_style::balanced) > pursuit_seconds(static_cast<bot_skill>(k - 1), bot_style::balanced));
		CHECK(pursuit_seconds(skill, bot_style::aggressive) > pursuit_seconds(skill, bot_style::balanced));
		CHECK(pursuit_seconds(skill, bot_style::balanced) > pursuit_seconds(skill, bot_style::collector));
		CHECK(pursuit_seconds(skill, bot_style::collector) > pursuit_seconds(skill, bot_style::cautious));
		for (unsigned s = 0; s < BOT_STYLE_COUNT; ++s)
		{
			const auto style{static_cast<bot_style>(s)};
			CHECK(pursuit_seconds(skill, style) * 1000 <= effective_memory_ms(skill_of(skill), style_of(style)) + 0.5);
		}
	}
	CHECK(pursuit_seconds(bot_skill::insane, bot_style::aggressive) == 14);
	CHECK(pursuit_seconds(bot_skill::insane, bot_style::balanced) == 8);
	CHECK(pursuit_seconds(bot_skill::insane, bot_style::cautious) == 4);
	CHECK(pursuit_seconds(bot_skill::trainee, bot_style::balanced) == 2);

	/* The start: just engaged, a hit landed. */
	pursuit_view v;
	v.skill = bot_skill::insane;
	v.since_engaged = 0.4;
	v.since_hit = 1;
	CHECK(pursuit_start(v) == pursuit_reason::hits);
	/* Not after a pause: the engagement is over. */
	v.since_engaged = PURSUIT_ENGAGED_WITHIN + 0.1;
	CHECK(pursuit_start(v) == pursuit_reason::none);
	v.since_engaged = 0.4;
	/* No hit: the target damaged (low, or hurt in this engagement). */
	v.since_hit = PURSUIT_HIT_WINDOW + 1;
	CHECK(pursuit_start(v) == pursuit_reason::none);
	v.target_shields = 40;
	CHECK(pursuit_start(v) == pursuit_reason::damaged);
	v.target_shields = 90;
	v.damage_seen = 20;
	CHECK(pursuit_start(v) == pursuit_reason::damaged);
	v.damage_seen = 5;
	/* Stronger: by the style's threshold. */
	v.advantage = 1.3;
	CHECK(pursuit_start(v) == pursuit_reason::stronger);
	v.advantage = 1.1;
	CHECK(pursuit_start(v) == pursuit_reason::none);
	v.style = bot_style::aggressive;
	v.retreat_shields = 20;
	CHECK(pursuit_start(v) == pursuit_reason::stronger);
	v.style = bot_style::cautious;
	v.retreat_shields = 55;
	v.advantage = 1.4;
	CHECK(pursuit_start(v) == pursuit_reason::none);
	v.advantage = 1.6;
	CHECK(pursuit_start(v) == pursuit_reason::stronger);

	/* Weak for its style: Balanced (retreat 35, margin 5) does not start
	 * below 50 but goes on down to 40 (hysteresis); Cautious (retreat
	 * 55, margin 20) not below 85, breaks off below 75, and when behind.
	 */
	pursuit_view w;
	w.since_engaged = 0.2;
	w.since_hit = 0.5;
	w.shields = 45;
	CHECK(pursuit_start(w) == pursuit_reason::none);
	CHECK(!pursuit_stop(w, 1));
	w.shields = 38;
	CHECK(pursuit_stop(w, 1) == pursuit_end::weak);
	w.shields = 55;
	CHECK(pursuit_start(w) == pursuit_reason::hits);
	w.style = bot_style::cautious;
	w.retreat_shields = 55;
	w.shields = 80;
	CHECK(pursuit_start(w) == pursuit_reason::none);
	CHECK(!pursuit_stop(w, 1));
	w.shields = 70;
	CHECK(pursuit_stop(w, 1) == pursuit_end::weak);
	w.shields = 95;
	w.advantage = 0.9;
	CHECK(pursuit_stop(w, 1) == pursuit_end::weak);
	w.invulnerable = true;
	w.shields = 10;
	CHECK(!pursuit_stop(w, 1));
	/* Aggressive: at its retreat threshold only. */
	w = {};
	w.style = bot_style::aggressive;
	w.retreat_shields = 20;
	w.since_engaged = 0.2;
	w.since_hit = 0.5;
	w.shields = 32;
	CHECK(pursuit_start(w) == pursuit_reason::hits);
	w.shields = 21;
	CHECK(!pursuit_stop(w, 1));

	/* An obvious ambush for a weakened bot: the target holds a heavy
	 * missile, or a stronger enemy is near where it went.  At full
	 * shields and ahead, it goes on.
	 */
	pursuit_view a;
	a.since_engaged = 0.2;
	a.since_hit = 0.5;
	a.shields = 45;
	a.retreat_shields = 20;
	a.style = bot_style::aggressive;
	a.target_heavy = true;
	CHECK(pursuit_ambush(a));
	CHECK(pursuit_start(a) == pursuit_reason::none);
	CHECK(pursuit_stop(a, 1) == pursuit_end::ambush);
	a.shields = 100;
	a.advantage = 1.2;
	CHECK(!pursuit_ambush(a));
	CHECK(pursuit_start(a) == pursuit_reason::hits);
	a.target_heavy = false;
	a.stronger_near = 1;
	a.advantage = 0.8;
	CHECK(pursuit_ambush(a));
	a.invulnerable = true;
	CHECK(!pursuit_ambush(a));

	/* Persistence over. */
	pursuit_view p;
	p.skill = bot_skill::hotshot;
	CHECK(!pursuit_stop(p, 4.4));
	CHECK(pursuit_stop(p, 4.6) == pursuit_end::expired);
	p.style = bot_style::aggressive;
	p.retreat_shields = 20;
	CHECK(!pursuit_stop(p, 7.8));
	CHECK(pursuit_stop(p, 7.9) == pursuit_end::expired);

	/* How far along its way the target is predicted. */
	CHECK(close_to(pursuit_travel(0, 0, 0), PURSUIT_MIN_SPEED * PURSUIT_LEAD_SECONDS));
	CHECK(close_to(pursuit_travel(80, 1, 0), 120));
	CHECK(close_to(pursuit_travel(80, 10, 0), 80 * PURSUIT_PREDICT_SECONDS));
	CHECK(close_to(pursuit_travel(80, 1, 1), 120 + PURSUIT_ADVANCE));
	CHECK(close_to(pursuit_travel(200, 10, 3), PURSUIT_PREDICT_MAX));

	/* The goal while pursuing.  The target (range factor 0.8) broke the
	 * line of sight: before, its score halved (0.5 x confidence) and the
	 * hunt (0.8) lost to a plain collection (1.5 x 60 / 160 = 0.56 ... a
	 * shield pack of 1.2 value) or any grab; now it scores 0.9.
	 */
	goal_inputs g;
	g.has_target = true;
	g.target_visible = false;
	g.threatened = true;
	g.shields = 80;
	g.collect = 1.2;
	g.collect_path = 100;
	g.current = goal_kind::engage;
	g.target_score = 0.5 * 0.8;
	CHECK(choose_goal(g) == goal_kind::collect);
	target_candidate tc{.id = 1, .visible = false, .confidence = 0.95, .distance = 80, .pursued = true};
	g.target_score = target_score(tc, 350);
	CHECK(g.target_score > 0.7);
	g.pursuing = true;
	CHECK(choose_goal(g) == goal_kind::hunt);
	CHECK(close_to(goal_utility(g)[goal_kind::collect], 1.2 * PURSUIT_COLLECT));
	/* A grab: only a high-value one very close. */
	g.grab = true;
	g.grab_value = 1;
	g.grab_path = 15;
	CHECK(goal_utility(g).collect_from != collect_source::grab);
	CHECK(choose_goal(g) == goal_kind::hunt);
	g.grab_value = GRAB_HIGH_VALUE;
	g.grab_path = PURSUIT_GRAB_PATH + 10;
	CHECK(choose_goal(g) == goal_kind::hunt);
	g.grab_path = PURSUIT_GRAB_PATH - 5;
	CHECK(goal_utility(g).collect_from == collect_source::grab);
	CHECK(choose_goal(g) == goal_kind::collect);
	/* Without the pursuit that grab was a detour of 60 units too. */
	g.grab_path = 50;
	g.pursuing = false;
	CHECK(goal_utility(g).collect_from == collect_source::grab);
	/* In danger the retreat (and shields) still win. */
	g.pursuing = true;
	g.grab = false;
	g.shields = 20;
	CHECK(choose_goal(g) == goal_kind::retreat);
	/* The hunt, once chosen, is kept against a slightly better
	 * collection (the goal's hysteresis).
	 */
	g.shields = 80;
	g.current = goal_kind::hunt;
	const double hunt{goal_utility(g)[goal_kind::hunt]};
	g.pursuing = false;
	g.collect = hunt * 1.1 / g.collect_weight;
	CHECK(choose_goal(g) == goal_kind::hunt);
}

/* The PR #47 review: only a pursuit given up blocks the next one (and
 * the hunt) until the target is seen again; a brief sighting (a peek
 * and a duck back) lets the pursuit start again.
 */
void test_pursuit_block()
{
	CHECK(pursuit_end_gives_up(pursuit_end::expired));
	CHECK(pursuit_end_gives_up(pursuit_end::weak));
	CHECK(pursuit_end_gives_up(pursuit_end::ambush));
	CHECK(pursuit_end_gives_up(pursuit_end::searched));
	CHECK(!pursuit_end_gives_up(pursuit_end::seen));
	CHECK(!pursuit_end_gives_up(pursuit_end::lost));
	CHECK(!pursuit_end_gives_up(pursuit_end::other_target));
	CHECK(!pursuit_end_gives_up(pursuit_end::died));
	pursuit_block b;
	CHECK(!b.blocks(3, 0));
	/* Given up at the sighting of tick 100: blocked until seen after. */
	b.on_end(pursuit_end::expired, 3, 100);
	CHECK(b.blocks(3, 100));
	CHECK(!b.blocks(3, 101));
	CHECK(!b.blocks(4, 100));
	/* Peek and duck: seen at 120 (a new pursuit), which ends "seen
	 * again" at that tick; ducked back, the memory stays at 120: the
	 * pursuit may start again (before the review it was blocked).
	 */
	CHECK(!b.blocks(3, 120));
	b.on_end(pursuit_end::seen, 3, 120);
	CHECK(!b.blocks(3, 120));
	/* The block lifted for that target, not for another given up. */
	b.on_end(pursuit_end::weak, 5, 130);
	b.on_end(pursuit_end::seen, 3, 131);
	CHECK(b.blocks(5, 130));
	/* Gone, died, another target: no block. */
	for (const auto e : {pursuit_end::lost, pursuit_end::died, pursuit_end::other_target})
	{
		pursuit_block c;
		c.on_end(e, 2, 50);
		CHECK(!c.blocks(2, 50));
	}
	/* Searched (a dead end) and an ambush block. */
	for (const auto e : {pursuit_end::searched, pursuit_end::ambush})
	{
		pursuit_block c;
		c.on_end(e, 2, 50);
		CHECK(c.blocks(2, 50));
	}
}

}

int main()
{
	test_pursuit();
	test_pursuit_block();
	test_log_tuning_goals();
	test_high_value_grab();
	test_powerup_phase();
	test_bands();
	test_weapon_table();
	test_weapon_resources();
	test_weapon_hysteresis();
	test_long_shots();
	test_values();
	test_collect_utility();
	test_goal_choice();
	test_pickup_scenarios();
	test_owned_weapon_selection();
	test_noticing();
	test_fuel_centres();
	test_retreat();
	test_map_knowledge();
	test_powerup_memory();
	test_afterburner();
	std::puts("test-bot-goals: all checks passed");
	return 0;
}
