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
};

/* Section 9.4: the distances of the rules, in game units. */
constexpr double MISSILE_MIN_DISTANCE{30};
constexpr double MISSILE_MAX_DISTANCE{200};
constexpr double HOMING_MIN_DISTANCE{40};
constexpr double SMART_MAX_DISTANCE{120};
constexpr double HEAVY_MIN_DISTANCE{70};
constexpr double HEAVY_MAX_DISTANCE{220};
constexpr double SHAKER_MIN_DISTANCE{110};
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
constexpr double HEAVY_INTERVAL{8};
constexpr double HEAVY_PER_TARGET{25};
/* A target crossing faster than this dodges a slow heavy missile; below
 * Ace (weapon smarts 3) the bot waits for a slower moment.
 */
constexpr double HEAVY_MAX_LATERAL{30};
/* A smart missile fired at a target that was seen this recently (its
 * children find it round a corner).
 */
constexpr double SMART_SEEN_WITHIN{1};
/* A chosen missile that the aim cannot release in this time is dropped. */
constexpr double MISSILE_PENDING_SECONDS{1.5};

/* Blast safety: the first thing a missile fired along the nose meets
 * (a wall, the target, anything) must be at least `factor` blast radii
 * away, plus a margin for the ship and the blast's rounding.  The
 * earthshaker gets 2 (its children spread from the blast and explode
 * too), the mega 1.5, the others 1.  Invulnerable, only point blank is
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
			return 2;
		case missile_role::heavy:
			return 1.5;
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

/* `invulnerable_left`: seconds of real invulnerability left (0 when not
 * invulnerable, or only faking it).
 */
[[nodiscard]]
constexpr bool blast_safe(const missile_role r, const double impact_distance, const missile_data &md, const double invulnerable_left)
{
	const double f{blast_factor(r)};
	if (!(f > 0))
		return true;
	if (invulnerable_left > blast_danger_seconds(r, impact_distance, md))
		return impact_distance >= MISSILE_MIN_DISTANCE;
	return impact_distance >= f * std::max(md.blast_radius, 0.0) + BLAST_MARGIN;
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
};

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
	/* The heavy ones: a clear shot, far enough (the release checks the
	 * blast along the nose again), not too often, once per target, and
	 * below Ace only at a target that does not cross fast.
	 */
	const bool heavy_ok{open_shot && !m.cloaked && !m.heavy_used_on_target && m.since_heavy >= HEAVY_INTERVAL && (m.smarts >= 3 || m.target_lateral_speed < HEAVY_MAX_LATERAL)};
	if (heavy_ok)
	{
		if (usable(secondary::earthshaker) && d >= SHAKER_MIN_DISTANCE && d <= SHAKER_MAX_DISTANCE &&
			blast_safe(missile_role::shaker, d, m.data[static_cast<unsigned>(secondary::earthshaker)], m.invulnerable_left))
			return secondary::earthshaker;
		if (usable(secondary::mega) && d >= HEAVY_MIN_DISTANCE && d <= HEAVY_MAX_DISTANCE &&
			blast_safe(missile_role::heavy, d, m.data[static_cast<unsigned>(secondary::mega)], m.invulnerable_left))
			return secondary::mega;
	}
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
constexpr double missile_cone(const missile_role r, const double fire_cone)
{
	constexpr double pi{3.14159265358979323846};
	switch (r)
	{
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
constexpr bool missile_release(const secondary s, const double aim_error, const double fire_cone, const double impact_distance, const missile_data &md, const double invulnerable_left)
{
	const auto r{role_of(s)};
	if (r == missile_role::none)
		return false;
	if (r == missile_role::mine)
		return true;
	return aim_error <= missile_cone(r, fire_cone) && blast_safe(r, impact_distance, md, invulnerable_left);
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
