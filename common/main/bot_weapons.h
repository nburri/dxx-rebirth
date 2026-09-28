/*
 * This file is part of the DXX-Rebirth project <https://github.com/dxx-rebirth/dxx-rebirth/>.
 * It is copyright by its individual contributors, as recorded in the
 * project's Git history.  See COPYING.txt at the top level for license
 * terms and a link to the Git history.
 */
/*
 * The bots' secondary weapons and the items they use, the
 * game-independent part (Documentation/multiplayer-bots.md sections 4.5,
 * 4.7 and 9.4, stage B4):
 *
 * - which missile or mine a bot fires, by its skill, the target, the
 *   range and the safety of the blast (never its own mega or
 *   earthshaker at point blank or into a wall next to it);
 * - when a chosen missile is released (the aim, the blast);
 * - the energy to shield converter;
 * - how cloak and invulnerability change the bot's tactics;
 * - how seriously a homing missile that tracks the bot is dodged.
 *
 * Pure functions of their inputs, standard library only
 * (common/unittest/bot_weapons.cpp).
 */

#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <optional>

#include "bot_goals.h"

namespace dcx::bot {

/* What a secondary is for a bot. */
enum class missile_role : uint8_t
{
	/* Flies straight where the nose points: concussion, mercury. */
	straight,
	/* Homes on what it sees in front at launch: homing. */
	homing,
	/* Bursts into homing children near the target: smart. */
	smart,
	/* A big blast: mega. */
	heavy,
	/* The biggest blast, and children that home: earthshaker. */
	shaker,
	/* Blinds the one it hits: flash. */
	flash,
	/* Dropped behind: proximity bomb, smart mine. */
	mine,
	/* Not used: the guided missile (its steering and camera are the
	 * human's; section 9.4).
	 */
	none,
};

[[nodiscard]]
constexpr missile_role role_of(const secondary s)
{
	switch (s)
	{
		case secondary::concussion:
		case secondary::mercury:
			return missile_role::straight;
		case secondary::homing:
			return missile_role::homing;
		case secondary::smart:
			return missile_role::smart;
		case secondary::mega:
			return missile_role::heavy;
		case secondary::earthshaker:
			return missile_role::shaker;
		case secondary::flash:
			return missile_role::flash;
		case secondary::proximity:
		case secondary::smart_mine:
			return missile_role::mine;
		case secondary::guided:
			return missile_role::none;
	}
	return missile_role::none;
}

/* The least weapon smarts (section 5.1) that uses a secondary.  The
 * design's table has mega from Ace and the earthshaker from Insane; the
 * presets are not applied before B2 and every bot plays Hotshot, so
 * Hotshot uses them too, under the strict rules below (section 9.4).
 */
[[nodiscard]]
constexpr unsigned min_smarts(const secondary s)
{
	switch (role_of(s))
	{
		case missile_role::straight:
		case missile_role::homing:
		case missile_role::flash:
			return 1;
		case missile_role::smart:
		case missile_role::mine:
		case missile_role::heavy:
		case missile_role::shaker:
			return 2;
		case missile_role::none:
			break;
	}
	return 99;
}

/* Seconds between two missiles, and between two mines, by weapon smarts
 * (a Rookie fires one now and then, an Insane bot whenever it may).
 */
[[nodiscard]]
constexpr double missile_interval(const unsigned smarts)
{
	constexpr std::array<double, 5> by_smarts{{1e9, 4.0, 2.5, 1.8, 1.2}};
	return by_smarts[std::min<std::size_t>(smarts, by_smarts.size() - 1)];
}

[[nodiscard]]
constexpr double mine_interval(const unsigned smarts)
{
	constexpr std::array<double, 5> by_smarts{{1e9, 5.0, 3.0, 2.5, 2.0}};
	return by_smarts[std::min<std::size_t>(smarts, by_smarts.size() - 1)];
}

/* The weapon data of a secondary, as the game has it (Weapon_info). */
struct missile_data
{
	/* Top speed, units/s. */
	double speed{160};
	/* Radius of the blast's damage (damage_radius), units. */
	double blast_radius{};
	/* It accelerates (thrust): it starts at half its speed. */
	bool thrust{true};
	/* It homes (the weapon data's homing_flag): it turns after its
	 * target, so a crossing target does not dodge it by moving.
	 */
	bool homing{};
};

/* Section 9.4: the distances of the rules, in game units. */
constexpr double MISSILE_MIN_DISTANCE{30};
constexpr double MISSILE_MAX_DISTANCE{200};
constexpr double HOMING_MIN_DISTANCE{40};
constexpr double SMART_MAX_DISTANCE{120};
/* Section 9.5: B4 fired the earthshaker only from 110 units and the mega
 * from 70, at a target crossing slower than 30 units/s: a fight keeps
 * 35-95 units (bot.cpp) and a human strafes faster, so the heavy missiles
 * were practically never fired.  The distances are now those of the
 * blast (blast_safe), and the bot backs off to them (heavy_standoff).
 */
constexpr double HEAVY_MIN_DISTANCE{45};
constexpr double HEAVY_MAX_DISTANCE{220};
constexpr double SHAKER_MIN_DISTANCE{55};
constexpr double SHAKER_MAX_DISTANCE{260};
constexpr double FLASH_MAX_DISTANCE{100};
/* A mine is dropped for a pursuer this close behind. */
constexpr double MINE_PURSUER_DISTANCE{100};
/* A teammate this close behind, or further but flying the bot's way
 * (up to MINE_TEAMMATE_DISTANCE), would meet a mine dropped now.
 */
constexpr double MINE_TEAMMATE_CLOSE{40};
constexpr double MINE_TEAMMATE_DISTANCE{150};
/* While cloaked, no missile beyond this (the launch shows where the bot
 * is), and no heavy one at all.
 */
constexpr double CLOAKED_MISSILE_DISTANCE{100};
/* A heavy missile (mega, earthshaker) at most once per this many
 * seconds; and once per target until it changes or this passes.
 */
constexpr double HEAVY_INTERVAL{5};
constexpr double HEAVY_PER_TARGET{10};
/* A target crossing faster than this dodges a slow heavy missile that
 * does not home; below Ace (weapon smarts 3) the bot waits for a slower
 * moment.  A homing one (the data's homing_flag) turns after it.
 */
constexpr double HEAVY_MAX_LATERAL{45};
/* A smart missile fired at a target that was seen this recently (its
 * children find it round a corner).
 */
constexpr double SMART_SEEN_WITHIN{1};
/* A chosen missile that the aim cannot release in this time is dropped. */
constexpr double MISSILE_PENDING_SECONDS{1.5};

/* Blast safety: the first thing a missile fired along the nose meets
 * (a wall, the target, anything) must be at least `factor` blast radii
 * away, plus a margin for the ship and the blast's rounding.  The blast
 * does no damage beyond its radius (object_create_explosion_with_damage:
 * the damage falls linearly to 0 at damage_radius).  The earthshaker
 * gets 1.2 (its children spread from the blast and explode too, and do
 * not home on their shooter; B4: 2), the mega 1 (B4: 1.5), the others 1,
 * each plus the margin.  (Measured on the user's tight level "Earth
 * Shaker", section 9.5: with 2 and 1.5 no engagement at the fight's
 * 35-95 units allowed an earthshaker.)  Invulnerable, only point blank is
 * avoided, but only if the invulnerability outlasts the danger: it must
 * be real (not the faked respawn one, which a hit ends) and last beyond
 * the missile's flight, plus the earthshaker's children
 * (SHAKER_CHILDREN_SECONDS) and a spare.
 */
constexpr double BLAST_MARGIN{12};
constexpr double SHAKER_CHILDREN_SECONDS{2};
constexpr double INVULNERABLE_SPARE{0.5};

[[nodiscard]]
constexpr double blast_factor(const missile_role r)
{
	switch (r)
	{
		case missile_role::shaker:
			return 1.2;
		case missile_role::heavy:
			return 1;
		case missile_role::mine:
		case missile_role::none:
			return 0;
		default:
			return 1;
	}
}

/* Seconds until a missile fired now can no longer hurt the bot: its
 * flight to the impact (a thrust missile starts at half its speed: that
 * speed is taken throughout), then the earthshaker's children.
 */
[[nodiscard]]
constexpr double blast_danger_seconds(const missile_role r, const double impact_distance, const missile_data &md)
{
	const double speed{std::max(md.thrust ? md.speed / 2 : md.speed, 1.0)};
	return std::max(impact_distance, 0.0) / speed + (r == missile_role::shaker ? SHAKER_CHILDREN_SECONDS : 0) + INVULNERABLE_SPARE;
}

/* Section 9.5: where the bot is when the missile bursts.  The impact is
 * the first thing along the nose (the target if it is nearer than the
 * wall behind it, the wall at the end of the corridor or room if not);
 * the bot flies on meanwhile: toward the impact at `closing_speed`
 * (units/s along the nose; negative: away) for the missile's flight (at
 * half its speed for a thrust missile, as blast_danger_seconds).  Flying
 * away is credited up to CLOSING_AWAY_CREDIT units/s.
 */
constexpr double CLOSING_AWAY_CREDIT{20};

[[nodiscard]]
constexpr double distance_at_burst(const double impact_distance, const missile_data &md, const double closing_speed)
{
	const double speed{std::max(md.thrust ? md.speed / 2 : md.speed, 1.0)};
	const double flight{std::max(impact_distance, 0.0) / speed};
	return impact_distance - std::max(closing_speed, -CLOSING_AWAY_CREDIT) * flight;
}

/* `invulnerable_left`: seconds of real invulnerability left (0 when not
 * invulnerable, or only faking it).
 */
[[nodiscard]]
constexpr bool blast_safe(const missile_role r, const double impact_distance, const missile_data &md, const double invulnerable_left, const double closing_speed = 0)
{
	const double f{blast_factor(r)};
	if (!(f > 0))
		return true;
	if (invulnerable_left > blast_danger_seconds(r, impact_distance, md))
		return impact_distance >= MISSILE_MIN_DISTANCE;
	/* Both where it is fired and where the bot is at the burst. */
	const double need{f * std::max(md.blast_radius, 0.0) + BLAST_MARGIN};
	return impact_distance >= need && distance_at_burst(impact_distance, md, closing_speed) >= need;
}

/* What the missile choice looks at (the tactics layer fills it). */
struct missile_situation
{
	/* Rounds of each secondary, in the game's order. */
	std::array<uint8_t, BOT_SECONDARY_COUNT> ammo{};
	std::array<missile_data, BOT_SECONDARY_COUNT> data{};
	unsigned smarts{2};
	/* The target. */
	bool has_target{};
	bool target_visible{};
	/* The line of fire to it is clear (section 4.4). */
	bool shot_clear{};
	double target_distance{};
	/* Its speed across the line of fire. */
	double target_lateral_speed{};
	/* It faces the bot (its nose within 30 degrees). */
	bool target_facing{};
	double target_seen_ago{1e9};
	/* The same target already had its heavy missile (HEAVY_PER_TARGET). */
	bool heavy_used_on_target{};
	/* Seconds since the last missile, heavy missile and mine. */
	double since_missile{1e9};
	double since_heavy{1e9};
	double since_mine{1e9};
	/* Seconds of real invulnerability left (0: none, or faked). */
	double invulnerable_left{};
	bool cloaked{};
	/* Flying away from an enemy (retreat, collect, refuel) with that
	 * enemy behind, at this distance; at a doorway on the path.
	 */
	bool chased{};
	double pursuer_distance{1e9};
	bool at_doorway{};
	/* A teammate follows the bot (team game, friendly fire on), close
	 * enough behind to fly into a mine dropped now.
	 */
	bool teammate_behind{};
	/* The bot's speed toward the target (units/s; negative: away): the
	 * blast is judged where the bot is when the missile bursts.
	 */
	double closing_speed{};
};

/* Section 9.5: why a heavy missile (mega, earthshaker) is or is not
 * fired now, for the log (-verbose): the first rule that fails.
 */
enum class heavy_verdict : uint8_t
{
	fire,
	none_owned,
	skill,
	no_target,
	not_visible,
	no_clear_shot,
	cloaked,
	cooldown,
	used_on_target,
	too_fast,
	too_close,
	too_far,
	blast,
	/* Chosen, waiting for the aim or for the blast along the nose. */
	aiming,
	nose_blast,
};

inline constexpr std::array<const char *, 15> heavy_verdict_names{{
	"fire", "none-owned", "skill", "no-target", "not-visible", "no-clear-shot", "cloaked",
	"cooldown", "used-on-target", "too-fast", "too-close", "too-far", "blast", "aiming", "nose-blast",
}};

[[nodiscard]]
constexpr const char *name_of(const heavy_verdict v)
{
	const auto i{static_cast<unsigned>(v)};
	return i < heavy_verdict_names.size() ? heavy_verdict_names[i] : "?";
}

/* The least distance at which heavy missile `s` may be fired, by its
 * range and its blast (without invulnerability).
 */
[[nodiscard]]
constexpr double heavy_min_distance(const secondary s, const missile_data &md)
{
	const auto r{role_of(s)};
	const double range_min{r == missile_role::shaker ? SHAKER_MIN_DISTANCE : HEAVY_MIN_DISTANCE};
	return std::max(range_min, blast_factor(r) * std::max(md.blast_radius, 0.0) + BLAST_MARGIN);
}

/* The rules of one heavy missile (mega or earthshaker) now. */
[[nodiscard]]
constexpr heavy_verdict heavy_check(const missile_situation &m, const secondary s)
{
	const auto i{static_cast<unsigned>(s)};
	if (!m.ammo[i])
		return heavy_verdict::none_owned;
	if (m.smarts < min_smarts(s))
		return heavy_verdict::skill;
	if (!m.has_target)
		return heavy_verdict::no_target;
	if (m.since_missile < missile_interval(m.smarts) || m.since_heavy < HEAVY_INTERVAL)
		return heavy_verdict::cooldown;
	if (!m.target_visible)
		return heavy_verdict::not_visible;
	if (!m.shot_clear)
		return heavy_verdict::no_clear_shot;
	if (m.cloaked)
		return heavy_verdict::cloaked;
	if (m.heavy_used_on_target)
		return heavy_verdict::used_on_target;
	const auto &md{m.data[i]};
	const double d{m.target_distance};
	/* A missile that does not home misses a crossing target by its
	 * crossing during the flight; within most of the blast radius the
	 * blast still hits it.  Below Ace the bot waits for a better moment
	 * beyond that.
	 */
	if (!md.homing && m.smarts < 3 && m.target_lateral_speed >= HEAVY_MAX_LATERAL &&
		m.target_lateral_speed * d / std::max(md.thrust ? md.speed * 0.75 : md.speed, 1.0) > 0.8 * std::max(md.blast_radius, 0.0))
		return heavy_verdict::too_fast;
	const auto r{role_of(s)};
	if (d < (r == missile_role::shaker ? SHAKER_MIN_DISTANCE : HEAVY_MIN_DISTANCE))
		return heavy_verdict::too_close;
	if (d > (r == missile_role::shaker ? SHAKER_MAX_DISTANCE : HEAVY_MAX_DISTANCE))
		return heavy_verdict::too_far;
	if (!blast_safe(r, d, md, m.invulnerable_left, m.closing_speed))
		return heavy_verdict::blast;
	return heavy_verdict::fire;
}

/* The verdict on the heavy missiles together: the earthshaker's if the
 * bot has one, else the mega's.
 */
[[nodiscard]]
constexpr heavy_verdict heavy_check(const missile_situation &m)
{
	const auto e{heavy_check(m, secondary::earthshaker)};
	if (e == heavy_verdict::fire)
		return e;
	const auto g{heavy_check(m, secondary::mega)};
	if (g == heavy_verdict::fire || e == heavy_verdict::none_owned)
		return g;
	return e;
}

/* Section 9.5: the distance a fighting bot keeps while it has a heavy
 * missile ready to fire (the rules but the distance hold): far enough
 * for the blast, so that it gets its shot.  0: none.
 */
[[nodiscard]]
constexpr double heavy_standoff(const missile_situation &m)
{
	double best{0};
	for (const auto s : {secondary::earthshaker, secondary::mega})
	{
		const auto v{heavy_check(m, s)};
		if (v == heavy_verdict::too_close || v == heavy_verdict::blast || v == heavy_verdict::fire)
		{
			const double d{heavy_min_distance(s, m.data[static_cast<unsigned>(s)]) + 8};
			if (!best || d < best)
				best = d;
		}
	}
	return best;
}

/* Section 4.5 and 9.4: the secondary the bot wants to fire now, if any.
 * Whether it is released (the aim, the blast along the nose) is
 * missile_release's.
 */
[[nodiscard]]
constexpr std::optional<secondary> choose_secondary(const missile_situation &m)
{
	const auto usable{[&](const secondary s) {
		return m.ammo[static_cast<unsigned>(s)] > 0 && m.smarts >= min_smarts(s);
	}};
	/* Mines: dropped for a pursuer close behind, further at a doorway
	 * (it must pass there); the smart mine (its children home) first.
	 * Never with a teammate following on the same route.
	 */
	if (m.chased && !m.teammate_behind && m.since_mine >= mine_interval(m.smarts) && m.pursuer_distance < MINE_PURSUER_DISTANCE * (m.at_doorway ? 1.5 : 1))
	{
		if (usable(secondary::smart_mine))
			return secondary::smart_mine;
		if (usable(secondary::proximity))
			return secondary::proximity;
	}
	if (!m.has_target || m.since_missile < missile_interval(m.smarts))
		return std::nullopt;
	const double d{m.target_distance};
	if (m.cloaked && d > CLOAKED_MISSILE_DISTANCE)
		return std::nullopt;
	const bool open_shot{m.target_visible && m.shot_clear};
	/* The heavy ones (heavy_check): a clear shot, far enough for the
	 * blast (the release checks the blast along the nose again), not
	 * too often, once per target, and below Ace only at a target that
	 * does not cross fast, unless the missile homes.
	 */
	if (heavy_check(m, secondary::earthshaker) == heavy_verdict::fire)
		return secondary::earthshaker;
	if (heavy_check(m, secondary::mega) == heavy_verdict::fire)
		return secondary::mega;
	/* Smart: its children find the target, even round a corner seen a
	 * moment ago.
	 */
	if (usable(secondary::smart) && d <= SMART_MAX_DISTANCE && d >= MISSILE_MIN_DISTANCE &&
		(open_shot || (!m.target_visible && m.target_seen_ago <= SMART_SEEN_WITHIN)))
		return secondary::smart;
	if (!open_shot || d < MISSILE_MIN_DISTANCE || d > MISSILE_MAX_DISTANCE)
		return std::nullopt;
	/* Flash: at a target that faces the bot. */
	if (usable(secondary::flash) && m.target_facing && d <= FLASH_MAX_DISTANCE)
		return secondary::flash;
	/* Homing against a fast crosser or far away; straight otherwise
	 * (mercury, the faster, first).
	 */
	const bool homing_better{m.target_lateral_speed > 25 || d > 90};
	if (homing_better && usable(secondary::homing) && d >= HOMING_MIN_DISTANCE)
		return secondary::homing;
	if (usable(secondary::mercury))
		return secondary::mercury;
	if (usable(secondary::concussion))
		return secondary::concussion;
	if (usable(secondary::homing) && d >= HOMING_MIN_DISTANCE)
		return secondary::homing;
	return std::nullopt;
}

/* The cone within which a chosen missile is released: the homing ones
 * find their target within a wide cone (the game's homing takes what is
 * in front), the straight ones need the primary's.
 */
[[nodiscard]]
constexpr double missile_cone(const missile_role r, const double fire_cone, const bool homing = false)
{
	constexpr double pi{3.14159265358979323846};
	switch (r)
	{
		case missile_role::heavy:
		case missile_role::shaker:
			/* A homing mega or earthshaker finds its target itself. */
			return homing ? std::max(fire_cone, 15 * pi / 180) : fire_cone;
		case missile_role::homing:
			return std::max(fire_cone, 20 * pi / 180);
		case missile_role::smart:
			return std::max(fire_cone, 30 * pi / 180);
		case missile_role::mine:
		case missile_role::none:
			return 2 * pi;
		default:
			return fire_cone;
	}
}

/* The release of a chosen missile: the nose within its cone, and the
 * first thing along the nose far enough for its blast.  A mine needs
 * neither.
 */
[[nodiscard]]
constexpr bool missile_release(const secondary s, const double aim_error, const double fire_cone, const double impact_distance, const missile_data &md, const double invulnerable_left, const double closing_speed = 0)
{
	const auto r{role_of(s)};
	if (r == missile_role::none)
		return false;
	if (r == missile_role::mine)
		return true;
	return aim_error <= missile_cone(r, fire_cone, md.homing) && blast_safe(r, impact_distance, md, invulnerable_left, closing_speed);
}

/* Section 9.5: the fusion cannon, charged by the bot's own trigger as
 * FireLaser charges the human's (2 energy to start, then 1 a second,
 * the shot's damage growing with the charge), released when the aim is
 * on the target and the charge is the skill's, at the latest at
 * FUSION_MAX_CHARGE: from 2 s on the charge hurts the ship itself.
 */
constexpr double FUSION_MAX_CHARGE{1.8};

[[nodiscard]]
constexpr double fusion_release_charge(const unsigned smarts)
{
	constexpr std::array<double, 5> by_smarts{{0.5, 0.6, 1.0, 1.3, 1.5}};
	return by_smarts[std::min<std::size_t>(smarts, by_smarts.size() - 1)];
}

enum class fusion_action : uint8_t
{
	idle,
	charge,
	release,
};

struct fusion_view
{
	/* Fusion is the bot's primary. */
	bool selected{};
	bool charging{};
	/* Seconds of charge. */
	double charge{};
	double energy{};
	bool target_visible{};
	bool shot_clear{};
	/* The aim is within the fire cone (should_fire). */
	bool aimed{};
	double distance{};
	double range{};
};

[[nodiscard]]
constexpr fusion_action fusion_step(const fusion_view &v, const double release_charge)
{
	if (v.charging)
	{
		/* Switched away, full, out of energy: what is charged goes (the
		 * human's cannon fires by itself too).
		 */
		if (!v.selected || v.charge >= FUSION_MAX_CHARGE || v.energy <= 0)
			return fusion_action::release;
		if (v.aimed && v.charge >= release_charge)
			return fusion_action::release;
		return fusion_action::charge;
	}
	if (!v.selected || v.energy < FUSION_MIN_ENERGY)
		return fusion_action::idle;
	/* Charge while the shot comes: in sight, a clear line, in range. */
	if (v.target_visible && v.shot_clear && v.distance <= v.range)
		return fusion_action::charge;
	return fusion_action::idle;
}

/* Section 9.5: the omega cannon locks on what lies within about 20
 * degrees of the nose (OMEGA_MIN_TRACKABLE_DOT, 15/16) within
 * MAX_OMEGA_DIST: its fire cone is that wide.
 */
[[nodiscard]]
constexpr double omega_fire_cone(const double fire_cone)
{
	constexpr double pi{3.14159265358979323846};
	return std::max(fire_cone, 18 * pi / 180);
}

/* Whether the aim uses the missile's speed while it waits for the
 * release: the ones that fly straight (the homing ones turn by
 * themselves; a mine is dropped).
 */
[[nodiscard]]
constexpr bool missile_aimed(const secondary s)
{
	switch (role_of(s))
	{
		case missile_role::straight:
		case missile_role::heavy:
		case missile_role::shaker:
		case missile_role::flash:
			return true;
		default:
			return false;
	}
}

/* Section 4.7: the energy to shield converter (the game converts only
 * the energy above 100, two for one): a bot converts when its shields
 * are below this for its weapon smarts.
 */
[[nodiscard]]
constexpr bool want_convert(const double shields, const double energy, const unsigned smarts)
{
	if (energy <= 100 || shields >= 200 || !smarts)
		return false;
	constexpr std::array<double, 5> below{{0, 50, 80, 100, 110}};
	return shields < below[std::min<std::size_t>(smarts, below.size() - 1)];
}

/* Section 4.7: cloak and invulnerability.  Invulnerable, the bot is
 * aggressive: it engages more, collects less and closes in.  Cloaked, it
 * sneaks: it closes in to where it is hard to dodge, and holds its fire
 * (and missiles) at long range, where the shots would show it.
 */
struct powerup_tactics
{
	double engage_weight{1};
	double collect_weight{1};
	double range_scale{1};
	double max_fire_distance{1e9};
};

constexpr double CLOAKED_FIRE_DISTANCE{90};

[[nodiscard]]
constexpr powerup_tactics tactics_for(const bool cloaked, const bool invulnerable)
{
	powerup_tactics t;
	if (invulnerable)
	{
		t.engage_weight = 1.6;
		t.collect_weight = 0.5;
		t.range_scale = 0.6;
	}
	if (cloaked)
	{
		t.engage_weight = std::max(t.engage_weight, 1.3);
		t.range_scale = std::min(t.range_scale, 0.7);
		t.max_fire_distance = CLOAKED_FIRE_DISTANCE;
	}
	return t;
}

/* Section 4.6: a homing missile that tracks the bot turns after it, so
 * its straight flight is only a rough prediction: it is judged with a
 * wider pass and dodged more often.
 */
constexpr double HOMING_DODGE_RADIUS_SCALE{3};
constexpr double HOMING_DODGE_BONUS{0.25};

[[nodiscard]]
constexpr double dodge_radius(const double radius, const bool homing_at_me)
{
	return homing_at_me ? radius * HOMING_DODGE_RADIUS_SCALE : radius;
}

[[nodiscard]]
constexpr double dodge_chance(const double dodge_prob, const bool homing_at_me)
{
	if (!(dodge_prob > 0))
		return 0;
	return homing_at_me ? std::min(1.0, dodge_prob + HOMING_DODGE_BONUS) : dodge_prob;
}

}
