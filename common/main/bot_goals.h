/*
 * This file is part of the DXX-Rebirth project <https://github.com/dxx-rebirth/dxx-rebirth/>.
 * It is copyright by its individual contributors, as recorded in the
 * project's Git history.  See COPYING.txt at the top level for license
 * terms and a link to the Git history.
 */
/*
 * The bots' resources and goals, the game-independent part
 * (Documentation/multiplayer-bots.md sections 4.5, 4.7 and 5, stage B3):
 *
 * - what a powerup is worth to a bot, by its need (shields, energy, a
 *   better weapon);
 * - the goal choice: engage, hunt, collect, retreat, refuel, roam;
 * - what a bot knows of the level's powerups (decision 4 of section 11:
 *   by skill), and its memory of the powerups it saw;
 * - the primary weapon table by range band, energy and ammunition;
 * - the long range trigger discipline and the afterburner rule.
 *
 * Pure functions of their inputs, standard library only
 * (common/unittest/bot_goals.cpp).
 */

#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include "bot_brain.h"

namespace dcx::bot {

/* Section 4.7: what a powerup is to a bot.  The game maps its powerup
 * types onto these (bot.cpp); `primary` and `secondary` name the weapon.
 */
enum class item : uint8_t
{
	/* Nothing a bot seeks (keys, extra life, the unused types). */
	none,
	energy,
	shield,
	laser,
	super_laser,
	quad,
	primary,
	vulcan_ammo,
	secondary,
	afterburner,
	converter,
	ammo_rack,
	cloak,
	invulnerability,
	/* Carried if touched (as a human does), never sought: a bot has no
	 * use for them.
	 */
	headlight,
	full_map,
};

struct item_desc
{
	item kind{item::none};
	primary weapon{primary::laser};
	/* Secondary weapon index, as the game numbers them. */
	uint8_t secondary{};
};

/* The secondary weapons, in the game's order (secondary_weapon_index). */
enum class secondary : uint8_t
{
	concussion,
	homing,
	proximity,
	smart,
	mega,
	flash,
	guided,
	smart_mine,
	mercury,
	earthshaker,
};
constexpr unsigned BOT_SECONDARY_COUNT{10};

/* The primaries a bot fires (stage B1): fusion needs the human's charge
 * trigger and omega the human's charge model.  A bot still picks them up
 * (it denies them to the others and drops them when it dies), for little.
 */
[[nodiscard]]
constexpr bool bot_fires_primary(const primary p)
{
	return p != primary::fusion && p != primary::omega && p != primary::super_laser;
}

[[nodiscard]]
constexpr bool is_ammo_primary(const primary p)
{
	return p == primary::vulcan || p == primary::gauss;
}

/* Section 4.5: the range bands of the weapon table. */
enum class range_band : uint8_t
{
	close,	// < 60
	mid,	// 60 - 150
	far,	// > 150
};
constexpr unsigned BOT_RANGE_BANDS{3};

[[nodiscard]]
constexpr range_band band_of(const double distance)
{
	return distance < 60 ? range_band::close : distance < 150 ? range_band::mid : range_band::far;
}

/* What the weapon choice looks at. */
struct weapon_view
{
	/* Bit n: primary n owned (HAS_PRIMARY_FLAG). */
	uint16_t owned{1};
	/* 0-based, as the game counts it: 0-3 lasers, 4-5 super lasers. */
	unsigned laser_level{};
	bool quad{};
	double energy{100};
	/* Rounds of vulcan (and gauss) ammunition. */
	unsigned vulcan_ammo{};
};

[[nodiscard]]
constexpr bool owns(const weapon_view &v, const primary p)
{
	return (v.owned >> static_cast<unsigned>(p)) & 1u;
}

/* Section 4.5: the preference of each primary in each range band, before
 * energy.  Close: helix, spreadfire, super laser with quad, plasma, gauss.
 * Mid: plasma, helix, super laser with quad, gauss, vulcan, phoenix.
 * Far: gauss, vulcan, the lasers (the fast shots; slow blobs miss).
 * The laser's row is its level's (laser_score).
 */
inline constexpr std::array<std::array<double, BOT_RANGE_BANDS>, 10> primary_table{{
	{{1.0, 1.0, 0.8}},	// laser (scaled by laser_score)
	{{2.4, 2.6, 2.8}},	// vulcan
	{{3.5, 2.2, 0.6}},	// spreadfire
	{{3.2, 3.8, 2.0}},	// plasma
	{{0.0, 0.0, 0.0}},	// fusion: not fired by bots
	{{0.0, 0.0, 0.0}},	// super laser: fired as the laser
	{{3.0, 3.3, 3.6}},	// gauss
	{{3.7, 3.5, 1.2}},	// helix
	{{2.0, 2.4, 0.8}},	// phoenix
	{{0.0, 0.0, 0.0}},	// omega: not fired by bots
}};

/* The laser's strength by level (0-based; 4 and 5 are the super lasers),
 * times 1.3 with the quad lasers.
 */
[[nodiscard]]
constexpr double laser_score(const unsigned level, const bool quad)
{
	constexpr std::array<double, 6> by_level{{1.0, 1.25, 1.5, 1.75, 2.3, 2.6}};
	return by_level[std::min<std::size_t>(level, by_level.size() - 1)] * (quad ? 1.3 : 1.0);
}

/* The factor energy puts on a primary: nothing fires without energy
 * but the ammunition weapons; low on energy (below 20, section 4.5) the
 * energy weapons lose against the ammunition ones, the laser (the
 * cheapest shot) less.  Between 20 and 50 the energy-hungry weapons are
 * spared a little (conserve energy).
 */
[[nodiscard]]
constexpr double energy_factor(const primary p, const double energy)
{
	if (is_ammo_primary(p))
		return 1;
	if (energy < 1)
		return 0;
	if (p == primary::laser)
		return energy < 20 ? 0.8 : 1;
	if (energy < 20)
		return 0.5;
	if (energy < 50)
		return 0.75 + 0.25 * (energy - 20) / 30;
	return 1;
}

/* The score of primary `p` at range `band` (0: not usable now). */
[[nodiscard]]
constexpr double weapon_score(const primary p, const range_band band, const weapon_view &v)
{
	if (!owns(v, p) || !bot_fires_primary(p))
		return 0;
	if (is_ammo_primary(p) && !v.vulcan_ammo)
		return 0;
	double s{primary_table[static_cast<unsigned>(p)][static_cast<unsigned>(band)]};
	if (p == primary::laser)
		s *= laser_score(v.laser_level, v.quad);
	return s * energy_factor(p, v.energy);
}

/* A switch costs REARM_TIME: the current weapon counts this much more. */
constexpr double WEAPON_SWITCH_HYSTERESIS{1.15};

/* Section 4.5: the primary for a target at range `band` (none: no
 * target; the mid band, the all-round choice), from the table, keeping
 * `current` unless another is clearly better.  The laser if nothing
 * else can fire (it always is owned).
 */
[[nodiscard]]
constexpr primary choose_primary_for(const weapon_view &v, const std::optional<range_band> band, const primary current)
{
	const auto b{band.value_or(range_band::mid)};
	primary best{primary::laser};
	double best_score{-1};
	for (unsigned i = 0; i < primary_table.size(); ++i)
	{
		const auto p{static_cast<primary>(i)};
		double s{weapon_score(p, b, v)};
		if (s <= 0)
			continue;
		if (p == current)
			s *= WEAPON_SWITCH_HYSTERESIS;
		if (s > best_score)
		{
			best = p;
			best_score = s;
		}
	}
	return best;
}

/* The strength of a bot's armament: its best primary in the mid band. */
[[nodiscard]]
constexpr double armament_score(const weapon_view &v)
{
	double best{0};
	for (unsigned i = 0; i < primary_table.size(); ++i)
		best = std::max(best, weapon_score(static_cast<primary>(i), range_band::mid, v));
	return best;
}

/* Section 4.5, the far band: the bot does not spam at long range.  The
 * chance that a shot hits, roughly: the target's radius against the
 * spread of where it may be when the shot arrives, from the aim error
 * (`sigma` radians) and from the target's speed across the line of
 * fire (it may change its mind during half the flight).
 */
[[nodiscard]]
inline double long_shot_hit_chance(const double distance, const double shot_speed, const double lateral_speed, const double sigma, const double target_radius)
{
	if (distance <= 0)
		return 1;
	const double flight{shot_speed > 0 ? distance / shot_speed : 10.0};
	const double aim_spread{sigma * distance};
	const double move_spread{0.5 * lateral_speed * flight};
	const double spread{std::hypot(aim_spread, move_spread)};
	if (spread <= target_radius)
		return 1;
	return std::clamp(target_radius / spread, 0.0, 1.0);
}

/* Hold fire beyond the mid band when a shot hits less than 10 % of the
 * time.
 */
constexpr double LONG_SHOT_MIN_CHANCE{0.1};

[[nodiscard]]
inline bool long_shot_worthwhile(const double distance, const double shot_speed, const double lateral_speed, const double sigma, const double target_radius)
{
	return band_of(distance) != range_band::far || long_shot_hit_chance(distance, shot_speed, lateral_speed, sigma, target_radius) >= LONG_SHOT_MIN_CHANCE;
}

/* Section 4.7: what the bot has, as the collection values see it. */
struct resource_view
{
	double shields{100};
	double energy{100};
	weapon_view weapons;
	/* Vulcan ammunition as a share of what the bot can carry. */
	double vulcan_ammo_share{};
	bool afterburner{};
	bool converter{};
	bool ammo_rack{};
	bool cloaked{};
	bool invulnerable{};
};

/* The needs, 0 (none) to 1 (desperate).  Shields below 100 are a need
 * (a pickup takes them up to 200, which is worth little); energy too,
 * but half as much when the bot has an ammunition weapon with rounds
 * left.
 */
[[nodiscard]]
constexpr double shield_need(const double shields)
{
	return std::clamp((100 - shields) / 80, 0.0, 1.0);
}

[[nodiscard]]
constexpr double energy_need(const resource_view &r)
{
	const double n{std::clamp((100 - r.energy) / 80, 0.0, 1.0)};
	const bool ammo_weapon{(owns(r.weapons, primary::vulcan) || owns(r.weapons, primary::gauss)) && r.weapons.vulcan_ammo > 0};
	return ammo_weapon ? n * 0.5 : n;
}

/* A weapon that makes the bot stronger than what it has is worth this
 * much (section 4.7), one that does not this much (a spare, and it is
 * dropped when the bot dies).
 */
constexpr double VALUE_BETTER_PRIMARY{5};
constexpr double VALUE_SPARE_PRIMARY{1};
constexpr double BETTER_MARGIN{0.2};

[[nodiscard]]
constexpr double secondary_value(const uint8_t index)
{
	switch (static_cast<secondary>(index))
	{
		case secondary::smart:
		case secondary::mega:
			return 1.5;
		case secondary::earthshaker:
			return 2;
		case secondary::proximity:
		case secondary::smart_mine:
			return 0.8;
		default:
			return 1;
	}
}

/* Section 4.7: the value of a powerup to a bot (0: not worth a detour).
 * The rules of the game (evaluate_pickup) decide whether the bot can
 * take it at all; this says how much it wants it.
 */
[[nodiscard]]
constexpr double item_value(const item_desc &d, const resource_view &r)
{
	const auto &w{r.weapons};
	const double have{armament_score(w)};
	const auto better{[&](const weapon_view &after) {
		return armament_score(after) > have + BETTER_MARGIN;
	}};
	switch (d.kind)
	{
		case item::none:
		case item::headlight:
		case item::full_map:
			return 0;
		case item::shield:
			return 0.5 + 3.5 * shield_need(r.shields);
		case item::energy:
			return 0.3 + 3 * energy_need(r);
		case item::laser:
		case item::super_laser:
			{
				auto after{w};
				after.laser_level = d.kind == item::laser ? std::min(after.laser_level + 1, 3u) : std::min(std::max(after.laser_level + 1, 4u), 5u);
				return better(after) ? 3 : VALUE_SPARE_PRIMARY;
			}
		case item::quad:
			{
				auto after{w};
				after.quad = true;
				return better(after) ? 4 : 2;
			}
		case item::primary:
			{
				if (owns(w, d.weapon))
				{
					/* A vulcan or gauss cannon the bot has: its ammunition. */
					if (is_ammo_primary(d.weapon))
						return 1 + 2 * (1 - std::clamp(r.vulcan_ammo_share, 0.0, 1.0));
					return 0;
				}
				auto after{w};
				after.owned = static_cast<uint16_t>(after.owned | (1u << static_cast<unsigned>(d.weapon)));
				/* A new cannon comes with rounds. */
				if (is_ammo_primary(d.weapon) && !after.vulcan_ammo)
					after.vulcan_ammo = 1;
				return better(after) ? VALUE_BETTER_PRIMARY : VALUE_SPARE_PRIMARY;
			}
		case item::vulcan_ammo:
			if (owns(w, primary::vulcan) || owns(w, primary::gauss))
				return 1 + 2 * (1 - std::clamp(r.vulcan_ammo_share, 0.0, 1.0));
			return 0.3;
		case item::secondary:
			return secondary_value(d.secondary);
		case item::afterburner:
			return 2;
		case item::converter:
			return 0.8;
		case item::ammo_rack:
			return 0.6;
		case item::cloak:
			return r.cloaked ? 0 : 2.5;
		case item::invulnerability:
			return r.invulnerable ? 0 : 4;
	}
	return 0;
}

/* Section 4.7: the collection score, value x need / path distance (the
 * need is in the value); 60 units away halves it.
 */
constexpr double COLLECT_DISTANCE_SCALE{60};

[[nodiscard]]
constexpr double collect_utility(const double value, const double path_cost)
{
	if (value <= 0)
		return 0;
	return value * COLLECT_DISTANCE_SCALE / (COLLECT_DISTANCE_SCALE + std::max(path_cost, 0.0));
}

/* Section 4.1: the goals of the strategy layer. */
enum class goal_kind : uint8_t
{
	roam,
	/* A target known but not seen: fly to where it was. */
	hunt,
	/* A target in sight. */
	engage,
	/* Fly to a powerup. */
	collect,
	/* Weak and threatened: away from the threat, to shields if known. */
	retreat,
	/* To a fuel (or repair) centre, and hover in it. */
	refuel,
};
constexpr unsigned BOT_GOAL_COUNT{6};

struct goal_inputs
{
	/* The chosen target: its score (target_score) and whether it is in
	 * sight now.
	 */
	bool has_target{};
	bool target_visible{};
	double target_score{};
	/* Someone is after the bot: a target is known, or it was hit in the
	 * last seconds.
	 */
	bool threatened{};
	double shields{100};
	bool invulnerable{};
	/* The best powerup to collect (collect_utility) and its path cost. */
	double collect{};
	double collect_path{};
	/* The best fuel or repair centre (collect_utility of the need). */
	double refuel{};
	/* The style (section 5.2). */
	double retreat_shields{35};
	double engage_weight{1};
	double collect_weight{1};
	bool collector{};
	/* The goal of the last strategy tick (hysteresis). */
	std::optional<goal_kind> current;
};

struct goal_utilities
{
	std::array<double, BOT_GOAL_COUNT> u{};
	[[nodiscard]]
	double operator[](const goal_kind g) const
	{
		return u[static_cast<unsigned>(g)];
	}
};

/* A roaming bot does anything else that is worth something. */
constexpr double ROAM_UTILITY{0.2};
/* A goal kept from the last tick counts this much more. */
constexpr double GOAL_HYSTERESIS{1.2};
/* An enemy in sight: collection (and refuelling) counts this much, unless
 * the style is Collector or the powerup is right there (section 4.7).
 */
constexpr double COLLECT_UNDER_FIRE{0.35};
constexpr double GRAB_DISTANCE{40};

[[nodiscard]]
inline goal_utilities goal_utility(const goal_inputs &in)
{
	goal_utilities r;
	auto &u{r.u};
	const auto at{[&](const goal_kind g) -> double & {
		return u[static_cast<unsigned>(g)];
	}};
	at(goal_kind::roam) = ROAM_UTILITY;
	if (in.has_target)
	{
		if (in.target_visible)
			at(goal_kind::engage) = 2 * in.target_score * in.engage_weight;
		else
			at(goal_kind::hunt) = 2 * in.target_score * in.engage_weight;
	}
	const bool under_fire{in.target_visible && !in.collector};
	double collect{in.collect * in.collect_weight};
	if (under_fire && in.collect_path > GRAB_DISTANCE)
		collect *= COLLECT_UNDER_FIRE;
	at(goal_kind::collect) = collect;
	double refuel{in.refuel * in.collect_weight};
	if (under_fire)
		refuel *= COLLECT_UNDER_FIRE;
	at(goal_kind::refuel) = refuel;
	/* Section 4.7: below the style's threshold, a threatened bot strongly
	 * prefers to retreat, the more the weaker; invulnerable, never.
	 */
	if (in.threatened && !in.invulnerable && in.shields < in.retreat_shields && in.retreat_shields > 0)
		at(goal_kind::retreat) = 3 + 2 * (in.retreat_shields - in.shields) / in.retreat_shields;
	return r;
}

[[nodiscard]]
inline goal_kind choose_goal(const goal_inputs &in)
{
	const auto u{goal_utility(in)};
	goal_kind best{goal_kind::roam};
	double best_u{-1};
	for (unsigned i = 0; i < BOT_GOAL_COUNT; ++i)
	{
		const auto g{static_cast<goal_kind>(i)};
		double v{u[g]};
		if (v <= 0)
			continue;
		if (in.current && *in.current == g)
			v *= GOAL_HYSTERESIS;
		if (v > best_u)
		{
			best = g;
			best_u = v;
		}
	}
	return best;
}

/* The refuel value of a fuel centre (energy) or a repair centre
 * (shields): they give up to 100.
 */
[[nodiscard]]
constexpr double fuel_centre_value(const bool repair, const resource_view &r)
{
	if (repair)
		return r.shields < 100 ? 0.5 + 3.5 * shield_need(r.shields) : 0;
	return r.energy < 100 ? 0.3 + 3 * energy_need(r) : 0;
}

/* A bot hovering in a fuel centre stays until it is full (the centre
 * gives up to 100), or an enemy comes into sight while it has at least
 * `fight_energy`.
 */
[[nodiscard]]
constexpr bool keep_refuelling(const double energy, const bool enemy_in_sight, const double fight_energy = 40)
{
	if (energy >= 99.5)
		return false;
	return !enemy_in_sight || energy < fight_energy;
}

/* Section 4.7, retreat without a known shield source: of the places
 * drawn, the one furthest from the threat for the least flying.
 */
[[nodiscard]]
inline double flee_score(const vec3 &place, const vec3 &threat, const double path_cost)
{
	return distance(place, threat) - 0.3 * path_cost;
}

/* A shield source (or any retreat goal) that lies toward the threat is
 * worth less: flying past the attacker is not retreating.
 */
[[nodiscard]]
inline double retreat_direction_factor(const vec3 &from, const vec3 &place, const vec3 &threat)
{
	const auto to_place{normalized(place - from)};
	const auto to_threat{normalized(threat - from)};
	if (to_place == vec3{} || to_threat == vec3{})
		return 1;
	const double d{dot(to_place, to_threat)};
	return d > 0 ? 1 - 0.8 * d : 1;
}

/* Section 4.7 and decision 4 of section 11: a bot knows the level's
 * initial powerup layout within `map_knowledge` path segments of where
 * it is (0: nothing; 0xffff: the whole level).  Anything else it must
 * have seen or heard.
 */
constexpr unsigned MAP_KNOWLEDGE_WHOLE_LEVEL{0xffff};

[[nodiscard]]
constexpr bool knows_from_map(const bool initial_layout, const std::optional<unsigned> hops, const unsigned map_knowledge)
{
	if (!initial_layout || !map_knowledge)
		return false;
	if (map_knowledge >= MAP_KNOWLEDGE_WHOLE_LEVEL)
		return true;
	return hops && *hops <= map_knowledge;
}

/* A powerup that just appeared (a respawn, with its effect and sound) is
 * heard within the hearing radius for this long.
 */
constexpr double APPEAR_HEARD_SECONDS{1.0};

[[nodiscard]]
constexpr bool hears_appearance(const double age_seconds, const double distance_to, const double hearing_radius)
{
	return age_seconds >= 0 && age_seconds <= APPEAR_HEARD_SECONDS && distance_to <= hearing_radius;
}

/* Section 4.7: a bot's memory of the powerups it knows: where each was
 * and what it was.  An entry stays until the bot sees the place again
 * without it (someone took it), comes to it, or forgets it with time; a
 * human, too, may fly to a powerup that is no longer there.
 */
struct known_powerup
{
	/* The object slot and its signature. */
	uint16_t key{};
	uint16_t signature{};
	/* The game's powerup type, and what it carries (a cannon's rounds,
	 * the omega charge).
	 */
	uint8_t type{};
	uint32_t count{};
	/* Placed at level load (the map knowledge's layout). */
	bool initial{};
	vec3 pos;
	uint32_t segment{};
	uint32_t learned_tick{};
	/* Not a goal before this tick (it could not be taken on arrival). */
	uint32_t ignore_until{};
};

class powerup_memory
{
	std::vector<known_powerup> m_items;
	std::size_t m_capacity;
public:
	explicit powerup_memory(const std::size_t capacity = 96) :
		m_capacity{std::max<std::size_t>(capacity, 1)}
	{
	}
	void clear()
	{
		m_items.clear();
	}
	[[nodiscard]]
	std::size_t size() const
	{
		return m_items.size();
	}
	[[nodiscard]]
	std::span<const known_powerup> items() const
	{
		return m_items;
	}
	[[nodiscard]]
	known_powerup *find(const uint16_t key, const uint16_t signature)
	{
		for (auto &k : m_items)
			if (k.key == key && k.signature == signature)
				return &k;
		return nullptr;
	}
	[[nodiscard]]
	bool knows(const uint16_t key, const uint16_t signature) const
	{
		return std::ranges::any_of(m_items, [&](const known_powerup &k) {
			return k.key == key && k.signature == signature;
		});
	}
	[[nodiscard]]
	bool full() const
	{
		return m_items.size() >= m_capacity;
	}
	/* The bot learned of a powerup (saw it, heard it appear, knows the
	 * map): remember it, or refresh it.  A slot that now holds another
	 * object replaces the old entry.  Full: a new powerup the bot only
	 * knows from the map is not learned (else, on a level with more
	 * powerups than the memory holds, each strategy tick would push out
	 * another and the set would churn); one it perceived pushes out the
	 * oldest entry that is not being ignored (the ignore keeps the bot
	 * from going back to a powerup it could not reach).  Returns whether
	 * the powerup is remembered now.
	 */
	bool learn(const known_powerup &p, const uint32_t tick, const bool may_evict = true)
	{
		for (auto &k : m_items)
			if (k.key == p.key)
			{
				const auto ignore{k.signature == p.signature ? k.ignore_until : 0};
				k = p;
				k.learned_tick = tick;
				k.ignore_until = ignore;
				return true;
			}
		if (full())
		{
			if (!may_evict)
				return false;
			auto victim{m_items.end()};
			for (auto i{m_items.begin()}; i != m_items.end(); ++i)
			{
				if (tick < i->ignore_until)
					continue;
				if (victim == m_items.end() || static_cast<int32_t>(i->learned_tick - victim->learned_tick) < 0)
					victim = i;
			}
			if (victim == m_items.end())
				return false;
			m_items.erase(victim);
		}
		auto &k{m_items.emplace_back(p)};
		k.learned_tick = tick;
		k.ignore_until = 0;
		return true;
	}
	void forget(const uint16_t key)
	{
		std::erase_if(m_items, [key](const known_powerup &k) {
			return k.key == key;
		});
	}
	/* Forget what was learned more than `max_age` ticks ago. */
	void expire(const uint32_t tick, const uint32_t max_age)
	{
		std::erase_if(m_items, [&](const known_powerup &k) {
			return tick - k.learned_tick > max_age;
		});
	}
	void ignore_for(const uint16_t key, const uint32_t until)
	{
		for (auto &k : m_items)
			if (k.key == key)
				k.ignore_until = until;
	}
};

/* Section 4.7: the afterburner, by skill (section 5.1): Trainee never,
 * Rookie when chasing, Hotshot also when retreating, Ace also to dodge,
 * Insane also on long straight flights.  It is lit only above 30 % of
 * its charge and kept while the reason holds and some charge is left,
 * and only while the bot wants to go where its nose points (the
 * afterburner is full forward thrust).
 */
enum class afterburner_use : uint8_t
{
	never,
	chase,
	retreat,
	dodge,
	roam,
};

struct afterburner_view
{
	bool have{};
	/* 0 to 1. */
	double charge{};
	afterburner_use use{afterburner_use::never};
	/* The reasons. */
	bool chasing_far{};
	bool retreating{};
	bool dodging{};
	bool long_straight{};
	/* The wanted velocity lies within the burn cone of the nose. */
	bool aligned{};
	/* Burning now. */
	bool burning{};
};

constexpr double AFTERBURNER_LIGHT_CHARGE{0.3};
constexpr double AFTERBURNER_KEEP_CHARGE{0.05};
/* Chasing a target further than this lights it (section 4.7). */
constexpr double AFTERBURNER_CHASE_DISTANCE{150};

[[nodiscard]]
constexpr bool want_afterburner(const afterburner_view &v)
{
	if (!v.have || v.use == afterburner_use::never || !v.aligned)
		return false;
	const auto level{static_cast<unsigned>(v.use)};
	const bool reason{
		v.chasing_far ||
		(v.retreating && level >= static_cast<unsigned>(afterburner_use::retreat)) ||
		(v.dodging && level >= static_cast<unsigned>(afterburner_use::dodge)) ||
		(v.long_straight && level >= static_cast<unsigned>(afterburner_use::roam))
	};
	if (!reason)
		return false;
	return v.charge > (v.burning ? AFTERBURNER_KEEP_CHARGE : AFTERBURNER_LIGHT_CHARGE);
}

inline constexpr std::array<afterburner_use, BOT_SKILL_COUNT> afterburner_by_skill{{
	afterburner_use::never,
	afterburner_use::chase,
	afterburner_use::retreat,
	afterburner_use::dodge,
	afterburner_use::roam,
}};

[[nodiscard]]
constexpr afterburner_use afterburner_of(const bot_skill s)
{
	const auto i{static_cast<unsigned>(s)};
	return afterburner_by_skill[i < BOT_SKILL_COUNT ? i : static_cast<unsigned>(BOT_DEFAULT_SKILL)];
}

/* How long a bot remembers a powerup it saw (section 4.7), by skill:
 * the memory time of section 5.1 is for targets; places are kept longer.
 */
[[nodiscard]]
constexpr unsigned powerup_memory_ms(const skill_params &s)
{
	return 30000 + s.memory_ms * 3;
}

}
