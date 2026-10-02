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
 * - how seriously a homing missile that tracks the bot is dodged;
 * - section 9.6: the expected outcome of a mega or earthshaker by the
 *   bot's risk profile (skill and style), its aims at walls and
 *   corners, hugging an enemy that holds one, ducking out of sight.
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
#include <span>
#include <utility>
#include <vector>

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
 * design's table has mega from Ace and the earthshaker from Insane;
 * Hotshot uses them too (section 9.4, kept in B2, section 9.7): the risk
 * profile of each skill and style bounds them, and the cooldowns
 * (missile_interval) grow toward the lower skills.  Trainee fires no
 * secondary, Rookie only the light ones.
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
 * Section 9.8: between two volleys of the light missiles (volley_size);
 * the rounds of a volley follow at VOLLEY_GAP.  Ace 1.8 and Insane 1.2
 * before (the exp-16 playtest: "even at Insane, bots are still too
 * careful firing rockets").
 */
[[nodiscard]]
constexpr double missile_interval(const unsigned smarts)
{
	constexpr std::array<double, 5> by_smarts{{1e9, 4.0, 2.5, 1.5, 1.0}};
	return by_smarts[std::min<std::size_t>(smarts, by_smarts.size() - 1)];
}

/* Section 9.9: the style's scale of missile_interval (the exp-19
 * playtest: "do not use missiles aggressive enough, even on insane"):
 * Aggressive 0.75, Cautious 1.2.
 */
[[nodiscard]]
constexpr double missile_interval_scale(const bot_style s)
{
	switch (s)
	{
		case bot_style::aggressive:
			return 0.75;
		case bot_style::cautious:
			return 1.2;
		default:
			return 1;
	}
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
	/* The blast radius of its children (the earthshaker's), 0: none. */
	double child_blast_radius{};
	/* Section 9.6: the damage at the centre of its blast (the data's
	 * strength at the game's difficulty; it falls linearly to 0 at the
	 * blast radius), its children's, and how many children it bursts
	 * into (NUM_SMART_CHILDREN).
	 */
	double damage{};
	double child_damage{};
	unsigned children{};
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
 * seconds; and once per target until it changes or this passes (the
 * Hotshot values; section 9.8: heavy_interval and heavy_per_target by
 * weapon smarts, Ace and Insane bolder).
 */
constexpr double HEAVY_INTERVAL{5};
constexpr double HEAVY_PER_TARGET{10};

[[nodiscard]]
constexpr double heavy_interval(const unsigned smarts)
{
	constexpr std::array<double, 5> by_smarts{{1e9, 1e9, HEAVY_INTERVAL, 4.0, 3.0}};
	return by_smarts[std::min<std::size_t>(smarts, by_smarts.size() - 1)];
}

[[nodiscard]]
constexpr double heavy_per_target(const unsigned smarts)
{
	constexpr std::array<double, 5> by_smarts{{1e9, 1e9, HEAVY_PER_TARGET, 8.0, 6.0}};
	return by_smarts[std::min<std::size_t>(smarts, by_smarts.size() - 1)];
}
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
 * gets 1.2 (B4: 2; its children, which pass through the bot, are
 * bounded only by shaker_behind_safe), the mega 1 (B4: 1.5), the others 1,
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
 * wall behind it, the wall at the end of the corridor or room if not).
 * Both ends close in meanwhile: the bot flies toward the impact at
 * `closing_speed` (units/s along the nose; negative: away), and a target
 * that is the impact flies toward the bot at `target_closing` (0 for a
 * wall), so that the missile meets it sooner and nearer: at d s / (s + v)
 * for a bot that holds still (s: the missile's speed, at half its speed
 * for a thrust missile, as blast_danger_seconds).  Flying away is
 * credited up to CLOSING_AWAY_CREDIT units/s, for either.
 */
constexpr double CLOSING_AWAY_CREDIT{20};

[[nodiscard]]
constexpr double distance_at_burst(const double impact_distance, const missile_data &md, const double closing_speed, const double target_closing = 0)
{
	const double speed{std::max(md.thrust ? md.speed / 2 : md.speed, 1.0)};
	const double target{std::max(target_closing, -CLOSING_AWAY_CREDIT)};
	const double flight{std::max(impact_distance, 0.0) / std::max(speed + target, 1.0)};
	return impact_distance - (std::max(closing_speed, -CLOSING_AWAY_CREDIT) + target) * flight;
}

/* `invulnerable_left`: seconds of real invulnerability left (0 when not
 * invulnerable, or only faking it).
 */
[[nodiscard]]
constexpr bool blast_safe(const missile_role r, const double impact_distance, const missile_data &md, const double invulnerable_left, const double closing_speed = 0, const double target_closing = 0)
{
	const double f{blast_factor(r)};
	if (!(f > 0))
		return true;
	if (invulnerable_left > blast_danger_seconds(r, impact_distance, md))
		return impact_distance >= MISSILE_MIN_DISTANCE;
	/* Both where it is fired and where the bot is at the burst. */
	const double need{f * std::max(md.blast_radius, 0.0) + BLAST_MARGIN};
	return impact_distance >= need && distance_at_burst(impact_distance, md, closing_speed, target_closing) >= need;
}

/* Section 9.5: the earthshaker's children do not collide with the ship
 * that fired (their parent): one that flies back from the burst passes
 * through the bot and bursts on the wall behind it, which blast_safe
 * does not bound.  An earthshaker is released only with the wall behind
 * the bot (along the nose, backward) at least SHAKER_BEHIND_FACTOR of a
 * child's blast radius plus the margin away: a child bursting there does
 * at most half its damage.  (A partial bound, kept small for the tight
 * levels; invulnerable, as blast_safe, it is waived while the
 * invulnerability outlasts the danger.)
 */
constexpr double SHAKER_BEHIND_FACTOR{0.5};

[[nodiscard]]
constexpr bool shaker_behind_safe(const missile_role r, const double behind_distance, const double impact_distance, const missile_data &md, const double invulnerable_left)
{
	if (r != missile_role::shaker || !(md.child_blast_radius > 0))
		return true;
	if (invulnerable_left > blast_danger_seconds(r, impact_distance, md))
		return true;
	return behind_distance >= SHAKER_BEHIND_FACTOR * md.child_blast_radius + BLAST_MARGIN;
}

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
	/* An earthshaker with a wall close behind (shaker_behind_safe). */
	wall_behind,
	/* Section 9.6, the expected outcome (judge_blast): the blast would
	 * most likely kill the bot; its expected self-damage is beyond the
	 * bot's risk budget; the trade is poor; the target would hardly be
	 * hurt.
	 */
	lethal,
	risky,
	poor_trade,
	low_value,
};

inline constexpr std::array<const char *, 20> heavy_verdict_names{{
	"fire", "none-owned", "skill", "no-target", "not-visible", "no-clear-shot", "cloaked",
	"cooldown", "used-on-target", "too-fast", "too-close", "too-far", "blast", "aiming", "nose-blast",
	"wall-behind", "lethal", "risky", "poor-trade", "low-value",
}};

[[nodiscard]]
constexpr const char *name_of(const heavy_verdict v)
{
	const auto i{static_cast<unsigned>(v)};
	return i < heavy_verdict_names.size() ? heavy_verdict_names[i] : "?";
}

/* Section 9.6: heavy missile tactics.  The strict rule of sections 9.4
 * and 9.5 (never any self-damage) left the bots almost without heavy
 * missiles on tight levels, where a human fires anyway, into edges and
 * corners, and accepts a suicide now and then.  A bot now weighs the
 * expected damage to the target against the expected damage to itself,
 * for the shot at the target and for aim points near it (walls, the wall
 * behind it, the corner it hides behind), and fires the best one when
 * the trade is favourable within its risk budget (its skill and style).
 */

/* The earthshaker's children (fwd-weapon.h NUM_SMART_CHILDREN), and how
 * far from the burst they find a target they can see (laser.cpp
 * MAX_SMART_DISTANCE).  They never hit the ship that fired them.
 */
constexpr unsigned SHAKER_CHILDREN{6};
constexpr double SMART_CHILD_REACH{150};

/* A bot's appetite for risk (section 9.6), from its skill and style
 * (section 5). */
struct risk_profile
{
	/* The self-damage it accepts in expectation, a share of its shields. */
	double self_budget{0.02};
	/* The chance of any self-damage it accepts (0: none within the
	 * uncertainty of the places, the strict rule).
	 */
	double self_chance{};
	/* The expected damage to the target it wants per point of expected
	 * self-damage.
	 */
	double trade{8};
	/* The chance that it closes in on an enemy holding a heavy missile
	 * that faces it (want_hug).
	 */
	double hug{};
	/* The share of the blast distance it keeps with a heavy missile
	 * ready (heavy_standoff).
	 */
	double standoff_scale{1};
	/* It aims heavy missiles at walls and corners too. */
	bool indirect{true};
	/* Section 9.7, ducking (want_duck): a target nearer than this share
	 * of the standoff, or closing faster than duck_closing units/s, makes
	 * it break the line of sight (Cautious ducks early, Aggressive late).
	 */
	double duck_share{0.6};
	double duck_closing{5};
	/* The least value of a blast worth a heavy missile: this share of
	 * the missile's damage (or of the target's shields if fewer;
	 * judge_blast, low_value).
	 */
	double value_share{0.2};
};

[[nodiscard]]
constexpr risk_profile risk_profile_of(const bot_skill k, const bot_style s)
{
	/* By style: the budget, the trade, the hug, the standoff, the
	 * chance, the duck share and closing speed.
	 */
	constexpr std::array<std::array<double, 7>, BOT_STYLE_COUNT> by_style{{
		{{0.12, 2.5, 0.45, 1.0, 0.20, 0.60, 5}},	/* balanced */
		{{0.30, 1.5, 0.80, 0.8, 0.40, 0.35, 15}},	/* aggressive */
		{{0.02, 8.0, 0.15, 1.2, 0.00, 0.90, 3}},	/* cautious: the strict rule, with the uncertainty */
		{{0.06, 4.0, 0.25, 1.1, 0.10, 0.75, 5}},	/* collector */
	}};
	/* By skill: scales of the budget, the trade and the hug.  Section
	 * 9.8: Ace and Insane clearly bolder than Hotshot (B2: 1.25, 0.9 and
	 * 1.5, 0.8; the trade of an aggressive Insane bot stays above 1); the
	 * point blank and lethal rules (judge_blast) hold for
	 * every profile, so no scale makes a suicide acceptable.  Section
	 * 9.9: Insane 2.2 and 0.7 (9.8: 1.9, 0.75; an aggressive Insane
	 * bot's trade 1.05, still above 1).
	 */
	constexpr std::array<std::array<double, 3>, BOT_SKILL_COUNT> by_skill{{
		{{0.3, 1.3, 0.0}},
		{{0.6, 1.15, 0.3}},
		{{1.0, 1.0, 1.0}},
		{{1.5, 0.85, 1.2}},
		{{2.2, 0.7, 1.35}},
	}};
	const auto si{static_cast<unsigned>(s) < BOT_STYLE_COUNT ? static_cast<unsigned>(s) : 0u};
	const auto ki{static_cast<unsigned>(k) < BOT_SKILL_COUNT ? static_cast<unsigned>(k) : static_cast<unsigned>(BOT_DEFAULT_SKILL)};
	const auto &st{by_style[si]};
	const auto &sk{by_skill[ki]};
	return {
		.self_budget = st[0] * sk[0],
		.self_chance = std::min(1.0, st[4] * sk[0]),
		.trade = st[1] * sk[1],
		.hug = std::min(1.0, st[2] * sk[2]),
		.standoff_scale = st[3],
		.indirect = ki >= static_cast<unsigned>(bot_skill::hotshot),
		.duck_share = st[5],
		.duck_closing = st[6],
	};
}

/* Section 9.17: the heavy missiles fired soon, as humans do.  The
 * recordings of exp-33 (Corona): the human fired its earthshakers 2-10 s
 * after the pickup (median about 6 over six games, mega and smart
 * alike) and seldom died with one; the bots held theirs 11-20 s and two
 * of nine died holding one.  A bot that holds a heavy missile (mega,
 * earthshaker; the smart missile is fired whenever its rules allow)
 * grows eager from half its fire delay on, fully at
 * one and a half (heavy_eagerness): its risk budget and chance rise, the
 * trade and the value it asks for fall, it keeps less of a standoff and
 * ducks less (eager_risk); its heavy cooldown halves and a target it
 * already fired one at may have another.  The point blank and lethal
 * rules hold at any eagerness (judge_blast): no suicide.  By skill the
 * delay is Hotshot 6 s, Ace 5 s, Insane 4 s (a style profile's
 * tune.heavy_fire_delay in place of it); below Hotshot no heavy missile
 * is fired at all.
 */
[[nodiscard]]
constexpr double heavy_fire_delay(const bot_skill k)
{
	constexpr std::array<double, BOT_SKILL_COUNT> by_skill{{8, 7, 6, 5, 4}};
	const auto ki{static_cast<unsigned>(k)};
	return by_skill[ki < BOT_SKILL_COUNT ? ki : static_cast<unsigned>(BOT_DEFAULT_SKILL)];
}

/* `held`: how many heavy missiles the bot holds; with several it is
 * eager at once (HEAVY_EAGER_COUNT and more: fully), as a human with a
 * stack fires them off (the arena on Earth Shaker, a level full of
 * earthshakers: bots holding four and more died with most of them).
 */
constexpr unsigned HEAVY_EAGER_COUNT{4};

[[nodiscard]]
constexpr double heavy_eagerness(const double held_s, const double delay, const unsigned held = 1)
{
	const double by_count{held > 1 ? std::min(1.0, (held - 1.0) / (HEAVY_EAGER_COUNT - 1.0)) : 0.0};
	if (!(delay > 0) || !(held_s > 0))
		return held_s > 0 ? by_count : 0;
	return std::max(by_count, std::clamp((held_s - 0.5 * delay) / delay, 0.0, 1.0));
}

/* The risk profile at eagerness `e` (0: as it is). */
[[nodiscard]]
constexpr risk_profile eager_risk(risk_profile rp, const double e)
{
	if (!(e > 0))
		return rp;
	rp.self_budget *= 1 + 1.5 * e;
	rp.self_chance = std::min(1.0, rp.self_chance + 0.25 * e);
	rp.trade *= 1 - 0.4 * e;
	rp.standoff_scale *= 1 - 0.2 * e;
	rp.duck_share *= 1 - 0.5 * e;
	rp.value_share *= 1 - 0.5 * e;
	return rp;
}

/* The game's blast (object_create_explosion_with_damage): the damage
 * falls linearly from `strength` at the centre to 0 at the radius.
 */
[[nodiscard]]
constexpr double blast_damage(const double d, const double radius, const double strength)
{
	return radius > 0 && d < radius ? strength * (1 - std::max(d, 0.0) / radius) : 0;
}

/* The expected damage at a distance known to within `spread` (units,
 * the deviation of an isotropic normal error per axis): a fixed
 * quadrature, three points along the line (Gauss-Hermite) times two
 * across (the means of the halves of the chi distribution of 2).
 */
[[nodiscard]]
inline double expected_blast_damage(const double d, const double spread, const double radius, const double strength)
{
	if (!(spread > 0.01))
		return blast_damage(d, radius, strength);
	constexpr std::array<std::array<double, 2>, 3> along{{{{-1.7320508075688772, 1.0 / 6}}, {{0, 2.0 / 3}}, {{1.7320508075688772, 1.0 / 6}}}};
	constexpr std::array<double, 2> across{{0.73, 1.78}};
	double sum{0};
	for (const auto &a : along)
		for (const double c : across)
		{
			const double x{d + a[0] * spread};
			const double y{c * spread};
			sum += a[1] * 0.5 * blast_damage(std::sqrt(x * x + y * y), radius, strength);
		}
	return sum;
}

/* The chance that a place known to within `spread` is inside the blast
 * (the same quadrature).
 */
[[nodiscard]]
inline double blast_chance(const double d, const double spread, const double radius)
{
	if (!(radius > 0))
		return 0;
	if (!(spread > 0.01))
		return d < radius ? 1 : 0;
	constexpr std::array<std::array<double, 2>, 3> along{{{{-1.7320508075688772, 1.0 / 6}}, {{0, 2.0 / 3}}, {{1.7320508075688772, 1.0 / 6}}}};
	constexpr std::array<double, 2> across{{0.73, 1.78}};
	double sum{0};
	for (const auto &a : along)
		for (const double c : across)
		{
			const double x{d + a[0] * spread};
			const double y{c * spread};
			if (x * x + y * y < radius * radius)
				sum += a[1] * 0.5;
		}
	return sum;
}

/* What the bot knows of a fight when it weighs a heavy missile. */
struct blast_scene
{
	vec3 bot;
	vec3 bot_vel;
	/* The target's place (in sight: now; else where it was last seen,
	 * `unseen_for` seconds ago) and velocity.
	 */
	vec3 target;
	vec3 target_vel;
	bool target_visible{true};
	double unseen_for{};
	/* The bot's aim error (radians, per axis). */
	double aim_sigma{0.05};
	double shields{100};
	double target_shields{100};
	/* Seconds of real invulnerability left (0: none, or faked). */
	double invulnerable_left{};
};

/* Where the missile is aimed. */
enum class aim_kind : uint8_t
{
	/* At the target (the missile meets it). */
	direct,
	/* At the wall right behind the target. */
	wall_behind,
	/* At a wall or edge next to the target in sight. */
	near_wall,
	/* At a wall near where the target hides (out of sight). */
	corner,
};

inline constexpr std::array<const char *, 4> aim_kind_names{{"direct", "behind", "wall", "corner"}};

[[nodiscard]]
constexpr const char *name_of(const aim_kind k)
{
	const auto i{static_cast<unsigned>(k)};
	return i < aim_kind_names.size() ? aim_kind_names[i] : "?";
}

/* The expected outcome of one shot. */
struct blast_outcome
{
	/* Where it bursts, and how far along the aim from the bot. */
	vec3 burst;
	double impact{};
	/* How far the burst is from the bot when it bursts: the impact less
	 * the closing of both meanwhile (as distance_at_burst; the point
	 * blank rule applies to it and to the impact).
	 */
	double burst_distance{};
	/* Expected damage to the target and to the bot (blast and
	 * children), and the bot's damage without the uncertainties.
	 */
	double target_damage{};
	double self_damage{};
	double target_nominal{};
	double self_nominal{};
	/* The chance of any self-damage. */
	double self_chance{};
	/* The earthshaker's children see the target from the burst. */
	bool children_find_target{};
};

/* The model's constants (section 9.6). */
/* A cast along the aim reaches this far. */
constexpr double BURST_CAST_LIMIT{400};
/* Uncertainties (units): the target's place, the bot's own; how much of
 * a crossing makes a missile that does not home miss (a human dodges),
 * and one that homes; how much of its flight a target out of the line
 * keeps its course; the spread a target hidden for a second adds.
 */
constexpr double TARGET_SPREAD_BASE{3};
constexpr double BOT_SPREAD_BASE{2};
constexpr double CROSSING_MISS_SHARE{0.6};
constexpr double CROSSING_MISS_HOMING{0.15};
constexpr double TARGET_TRAVEL_SHARE{0.5};
constexpr double HIDDEN_SPREAD_PER_SECOND{12};
/* The share of the children that reach a target they see. */
constexpr double CHILD_HIT_SHARE{0.5};
constexpr double CHILD_SPREAD{5};
/* A kill is worth this much damage on top. */
constexpr double KILL_BONUS{30};

/* The directions of an icosahedron's vertices (12), the directions in
 * which a burst is probed: the walls near a target, and where the
 * earthshaker's children fly when they find no target (they fly off at
 * random).
 */
inline constexpr std::array<vec3, 12> icosahedron_directions{{
	{0, 0.5257311121191336, 0.85065080835204}, {0, -0.5257311121191336, 0.85065080835204},
	{0, 0.5257311121191336, -0.85065080835204}, {0, -0.5257311121191336, -0.85065080835204},
	{0.5257311121191336, 0.85065080835204, 0}, {-0.5257311121191336, 0.85065080835204, 0},
	{0.5257311121191336, -0.85065080835204, 0}, {-0.5257311121191336, -0.85065080835204, 0},
	{0.85065080835204, 0, 0.5257311121191336}, {-0.85065080835204, 0, 0.5257311121191336},
	{0.85065080835204, 0, -0.5257311121191336}, {-0.85065080835204, 0, -0.5257311121191336},
}};

/* The level, as the tactics ask it (the game: fvi; the tests: boxes):
 * `geo.cast(from, unit direction, limit)` is the distance to the first
 * wall a missile meets (limit: none), `geo.sees(a, b)` a line of sight
 * from a to b (through grates: a blast reaches what it sees).
 *
 * The expected outcome of a heavy missile fired along `dir`.  `direct`:
 * aimed at the target, which it meets if the target is nearer than the
 * wall (both closing in meanwhile, as distance_at_burst).
 */
template <typename Geometry>
[[nodiscard]]
blast_outcome evaluate_burst(const Geometry &geo, const blast_scene &sc, const missile_role r, const missile_data &md, const vec3 &dir, const bool direct)
{
	blast_outcome o;
	const auto u{normalized(dir)};
	const double wall{geo.cast(sc.bot, u, BURST_CAST_LIMIT)};
	const double speed{std::max(md.thrust ? md.speed / 2 : md.speed, 1.0)};
	const double to_target{distance(sc.bot, sc.target)};
	const bool meets{direct && sc.target_visible && to_target < wall};
	const double target_closing{meets ? std::max(dot(sc.target_vel, -u), -CLOSING_AWAY_CREDIT) : 0};
	const double impact{meets ? to_target : wall};
	const double flight{impact / std::max(speed + target_closing, 1.0)};
	o.impact = impact;
	o.burst = sc.bot + u * (meets ? impact - target_closing * flight : impact);
	/* Probes from just off the wall. */
	const auto probe{meets ? o.burst : o.burst - u};
	/* The bot at the burst: its own flight, away credited up to
	 * CLOSING_AWAY_CREDIT (as distance_at_burst).
	 */
	const double closing{dot(sc.bot_vel, u)};
	const auto bot_at{sc.bot + (sc.bot_vel - u * closing) * flight + u * (std::max(closing, -CLOSING_AWAY_CREDIT) * flight)};
	const double bot_d{distance(bot_at, o.burst)};
	o.burst_distance = bot_d;
	const double bot_spread{BOT_SPREAD_BASE + 0.2 * length(sc.bot_vel) * flight};
	/* The target at the burst. */
	double target_d, target_spread;
	vec3 target_at;
	if (meets)
	{
		const auto rel{sc.target_vel - sc.bot_vel};
		const double crossing{length(rel - u * dot(rel, u))};
		target_at = o.burst;
		target_d = 0;
		target_spread = TARGET_SPREAD_BASE + crossing * flight * (md.homing ? CROSSING_MISS_HOMING : CROSSING_MISS_SHARE) + sc.aim_sigma * impact;
	}
	else
	{
		target_at = sc.target + sc.target_vel * (flight * TARGET_TRAVEL_SHARE);
		target_d = distance(target_at, o.burst);
		target_spread = TARGET_SPREAD_BASE + 1 + length(sc.target_vel) * flight * TARGET_TRAVEL_SHARE + sc.unseen_for * HIDDEN_SPREAD_PER_SECOND + 0.5 * sc.aim_sigma * impact;
	}
	const bool target_exposed{meets || geo.sees(probe, sc.target)};
	const bool bot_exposed{meets || geo.sees(probe, sc.bot)};
	const double radius{std::max(md.blast_radius, 0.0)};
	if (target_exposed)
	{
		o.target_damage = expected_blast_damage(target_d, target_spread, radius, md.damage);
		o.target_nominal = blast_damage(target_d, radius, md.damage);
	}
	/* The chance that nothing hurts the bot, factor by factor. */
	double unhurt{1};
	if (bot_exposed)
	{
		o.self_damage = expected_blast_damage(bot_d, bot_spread, radius, md.damage);
		o.self_nominal = blast_damage(bot_d, radius, md.damage);
		unhurt *= 1 - blast_chance(bot_d, bot_spread, radius);
	}
	if (r == missile_role::shaker && md.children && md.child_blast_radius > 0 && md.child_damage > 0)
	{
		const double rc{md.child_blast_radius};
		const double n{static_cast<double>(md.children)};
		o.children_find_target = target_exposed && distance(probe, sc.target) < SMART_CHILD_REACH;
		/* The children that find nothing fly off at random. */
		double scattered{n};
		if (o.children_find_target)
		{
			/* They home on the target and burst at it: next to the
			 * bot if the target hugs it.  The others miss.
			 */
			const double hits{n * CHILD_HIT_SHARE};
			scattered = n - hits;
			o.target_damage += hits * expected_blast_damage(0, CHILD_SPREAD + 0.25 * target_spread, rc, md.child_damage);
			o.target_nominal += hits * blast_damage(0, rc, md.child_damage);
			const double apart{distance(bot_at, target_at)};
			if (apart < rc + 2 * (bot_spread + target_spread) && geo.sees(sc.target, sc.bot))
			{
				const double spread{bot_spread + 0.5 * target_spread};
				o.self_damage += hits * expected_blast_damage(apart, spread, rc, md.child_damage);
				o.self_nominal += hits * blast_damage(apart, rc, md.child_damage);
				unhurt *= std::pow(1 - blast_chance(apart, spread, rc), hits);
			}
		}
		/* The scattered ones burst on the walls round the burst: the
		 * icosahedron's directions, and the cone of directions that
		 * pass within a child's blast of the bot (its share of the
		 * sphere): such a child passes through the bot (they never hit
		 * their parent's ship) and bursts on the wall behind it.
		 */
		const auto to_bot{bot_at - probe};
		const double bot_apart{std::max(length(to_bot), 1.0)};
		const double cone_share{bot_apart > rc ? (1 - std::sqrt(1 - (rc / bot_apart) * (rc / bot_apart))) / 2 : 0.5};
		double sum{0}, nominal{0}, chance{0};
		const auto probe_wall{[&](const vec3 &d, const double weight) {
			const double h{geo.cast(probe, d, SMART_CHILD_REACH * 2)};
			if (!(h < SMART_CHILD_REACH * 2))
				return;
			const auto x{probe + d * std::max(h - 1, 0.0)};
			const double apart{distance(x, bot_at)};
			if (apart >= rc + 2 * bot_spread || !geo.sees(x, sc.bot))
				return;
			sum += weight * expected_blast_damage(apart, bot_spread, rc, md.child_damage);
			nominal += weight * blast_damage(apart, rc, md.child_damage);
			chance += weight * blast_chance(apart, bot_spread, rc);
		}};
		for (const auto &d : icosahedron_directions)
			probe_wall(d, (1 - cone_share) / icosahedron_directions.size());
		probe_wall(to_bot * (1 / bot_apart), cone_share);
		o.self_damage += scattered * sum;
		o.self_nominal += scattered * nominal;
		unhurt *= std::pow(1 - std::min(chance, 1.0), scattered);
	}
	o.self_chance = 1 - unhurt;
	/* Invulnerable beyond the danger: nothing to fear. */
	if (sc.invulnerable_left > blast_danger_seconds(r, impact, md))
		o.self_damage = o.self_nominal = o.self_chance = 0;
	return o;
}

/* Section 9.6: the rule.  Never at point blank (MISSILE_MIN_DISTANCE),
 * never an almost certain suicide (the blast without the uncertainties
 * kills the bot), at least some damage to the target, the expected
 * self-damage within the budget, and the trade favourable.
 */
enum class risk_verdict : uint8_t
{
	fire,
	point_blank,
	lethal,
	low_value,
	over_budget,
	poor_trade,
};

/* What a shot is worth: the expected damage, up to what the target has
 * (plus a kill's bonus if it most likely dies).
 */
[[nodiscard]]
constexpr double blast_value(const blast_outcome &o, const blast_scene &sc)
{
	return std::min(o.target_damage, std::max(sc.target_shields, 1.0)) + (o.target_nominal >= sc.target_shields ? KILL_BONUS : 0);
}

[[nodiscard]]
constexpr risk_verdict judge_blast(const blast_outcome &o, const blast_scene &sc, const missile_data &md, const risk_profile &rp)
{
	if (std::min(o.impact, o.burst_distance) < MISSILE_MIN_DISTANCE)
		return risk_verdict::point_blank;
	if (o.self_nominal >= sc.shields)
		return risk_verdict::lethal;
	const double value{blast_value(o, sc)};
	if (value < std::max(5.0, rp.value_share * std::min(md.damage, std::max(sc.target_shields, 20.0))))
		return risk_verdict::low_value;
	if (o.self_damage > rp.self_budget * std::clamp(sc.shields, 0.0, 200.0) + 0.5 || o.self_chance > rp.self_chance + 1e-6)
		return risk_verdict::over_budget;
	if (o.self_damage * rp.trade > value)
		return risk_verdict::poor_trade;
	return risk_verdict::fire;
}

[[nodiscard]]
constexpr heavy_verdict heavy_verdict_of(const risk_verdict v)
{
	switch (v)
	{
		case risk_verdict::fire:
			return heavy_verdict::fire;
		case risk_verdict::point_blank:
			return heavy_verdict::too_close;
		case risk_verdict::lethal:
			return heavy_verdict::lethal;
		case risk_verdict::low_value:
			return heavy_verdict::low_value;
		case risk_verdict::over_budget:
			return heavy_verdict::risky;
		case risk_verdict::poor_trade:
			return heavy_verdict::poor_trade;
	}
	return heavy_verdict::blast;
}

/* One aim weighed. */
struct aim_option
{
	aim_kind kind{aim_kind::direct};
	/* The direction from the bot, and the point aimed at. */
	vec3 dir;
	vec3 point;
	blast_outcome outcome;
	risk_verdict verdict{risk_verdict::low_value};
	/* The ranking of the favourable ones: value less the weighted risk. */
	double score{};
};

/* Aim points further off the line to the target than this are left out
 * (the turn would take too long).
 */
constexpr double INDIRECT_MAX_ANGLE{50 * 3.14159265358979323846 / 180};
/* The target counts as hiding for this long after it was last seen. */
constexpr double CORNER_SEEN_WITHIN{3};

/* Section 9.6: a missile aimed at a wall near a target in sight may meet
 * the target on the way: a homing one turns to a target within its
 * homing cone (laser.h HOMING_MIN_TRACKABLE_DOT), any one meets a target
 * near its line (MEET_LATERAL units plus the target's crossing during
 * the flight).  Such an aim bears the risk of the direct shot too: the
 * worse of both (merge_meet), with the direct shot's point blank.
 */
constexpr double HOMING_CONE_COS{0.75};
constexpr double MEET_LATERAL{10};

[[nodiscard]]
inline bool may_meet_target(const blast_scene &sc, const vec3 &dir, const missile_data &md)
{
	if (!sc.target_visible)
		return false;
	const auto u{normalized(dir)};
	const auto to{sc.target - sc.bot};
	const double along{dot(to, u)};
	if (!(along > 0))
		return false;
	if (md.homing && along >= HOMING_CONE_COS * length(to))
		return true;
	const double speed{std::max(md.thrust ? md.speed / 2 : md.speed, 1.0)};
	const auto rel{sc.target_vel - sc.bot_vel};
	const double crossing{length(rel - u * dot(rel, u))};
	return length(to - u * along) < MEET_LATERAL + crossing * (along / speed);
}

/* The worse of an aim's outcome and of the missile meeting the target
 * (the value stays the aim's).
 */
[[nodiscard]]
constexpr blast_outcome merge_meet(blast_outcome o, const blast_outcome &meet)
{
	o.self_damage = std::max(o.self_damage, meet.self_damage);
	o.self_nominal = std::max(o.self_nominal, meet.self_nominal);
	o.self_chance = std::max(o.self_chance, meet.self_chance);
	o.impact = std::min(o.impact, meet.impact);
	o.burst_distance = std::min(o.burst_distance, meet.burst_distance);
	return o;
}

/* Section 9.6: the bounds of one weighing (the game's; the default:
 * everything, as the tests and the evaluation take it).  The fan's
 * spokes and the probes from the target are split into `subsets`
 * interleaved parts, one weighed per plan (`phase` rotates through
 * them); of the indirect candidates, the `max_indirect` most promising
 * by a cheap estimate (the burst near the target, away from the bot)
 * are weighed; `previous`, the last plan's best indirect aim, is weighed
 * again (the rotation does not lose it).
 */
struct aim_search
{
	unsigned subsets{1};
	unsigned phase{};
	unsigned max_indirect{~0u};
	std::optional<vec3> previous;
};

/* The candidates (section 9.6): the target itself; the wall right
 * behind it; walls near it that the bot's missile reaches, within
 * `reach` of it (the blast radius; for an earthshaker at a hidden
 * target, whose children find it round a corner, up to 60 units): where
 * a fan of rays from the bot round the line to it (or to where it was
 * last seen) meets a wall (the edges and the corner that hides it), and
 * where probes from it in the icosahedron's directions meet one the
 * missile can reach.  `visit(kind, point)`.
 */
inline constexpr std::array<double, 3> AIM_FAN_ANGLES{{8 * 3.14159265358979323846 / 180, 16 * 3.14159265358979323846 / 180, 28 * 3.14159265358979323846 / 180}};
constexpr unsigned AIM_FAN_SPOKES{8};

/* The indirect aim point `point` qualifies: not at point blank, within
 * `reach` of the target, not too far off the line to it.
 */
[[nodiscard]]
inline bool indirect_aim_ok(const blast_scene &sc, const vec3 &point, const double reach)
{
	const auto to_target{sc.target - sc.bot};
	const double dist{length(to_target)};
	const auto aim{point - sc.bot};
	const double len{length(aim)};
	return dist > 1 && len >= MISSILE_MIN_DISTANCE && distance(point, sc.target) <= reach && dot(aim, to_target) >= len * dist * std::cos(INDIRECT_MAX_ANGLE);
}

template <typename Geometry, typename Visit>
void aim_candidates(const Geometry &geo, const blast_scene &sc, const double reach, const bool indirect, Visit &&visit, const aim_search &search = {})
{
	const auto to_target{sc.target - sc.bot};
	const double dist{length(to_target)};
	if (!(dist > 1))
		return;
	const auto u{to_target * (1 / dist)};
	if (sc.target_visible)
		visit(aim_kind::direct, sc.target);
	if (!indirect || !(reach > 0))
		return;
	if (sc.target_visible)
	{
		const double b{geo.cast(sc.target, u, reach)};
		if (b < reach)
			visit(aim_kind::wall_behind, sc.target + u * std::max(b - 1, 0.0));
	}
	const auto kind{sc.target_visible ? aim_kind::near_wall : aim_kind::corner};
	const auto consider{[&](const vec3 &point) {
		if (indirect_aim_ok(sc, point, reach))
			visit(kind, point);
	}};
	const unsigned subsets{std::max(search.subsets, 1u)};
	const auto in_subset{[&](const unsigned j) {
		return j % subsets == search.phase % subsets;
	}};
	/* The fan from the bot (the line itself too, for a hidden target:
	 * where its line of sight breaks).
	 */
	const auto side{normalized(std::fabs(u.x) < 0.9 ? cross(u, vec3{1, 0, 0}) : cross(u, vec3{0, 1, 0}))};
	const auto up{cross(side, u)};
	if (!sc.target_visible)
	{
		const double h{geo.cast(sc.bot, u, dist + reach)};
		consider(sc.bot + u * std::max(h - 1, 0.0));
	}
	for (unsigned a = 0; a < AIM_FAN_ANGLES.size(); ++a)
		for (unsigned k = 0; k < AIM_FAN_SPOKES; ++k)
		{
			if (!in_subset(k + a))
				continue;
			const double angle{AIM_FAN_ANGLES[a]};
			const double phi{2 * 3.14159265358979323846 * k / AIM_FAN_SPOKES};
			const auto dir{u * std::cos(angle) + (side * std::cos(phi) + up * std::sin(phi)) * std::sin(angle)};
			const double h{geo.cast(sc.bot, dir, dist + reach)};
			if (h < dist + reach)
				consider(sc.bot + dir * std::max(h - 1, 0.0));
		}
	/* The probes from the target, where the missile reaches. */
	for (unsigned i = 0; i < icosahedron_directions.size(); ++i)
	{
		if (!in_subset(i))
			continue;
		const auto &d{icosahedron_directions[i]};
		const double h{geo.cast(sc.target, d, reach)};
		if (!(h < reach))
			continue;
		const auto point{sc.target + d * std::max(h - 1, 0.0)};
		if (!indirect_aim_ok(sc, point, reach))
			continue;
		const auto aim{point - sc.bot};
		const double len{length(aim)};
		if (geo.cast(sc.bot, aim, len + 1) < len - 2)
			continue;
		visit(kind, point);
	}
}

/* The best favourable aim, if any, and the weighing of the direct shot
 * (or, without one, of the least bad) for the log.
 */
struct aim_choice
{
	std::optional<aim_option> best;
	/* Why no aim is favourable (the direct shot's verdict, else the
	 * best candidate's).
	 */
	risk_verdict why{risk_verdict::low_value};
	/* How many aims were weighed, how many of them were favourable, the
	 * indirect ones; favourable indirect ones refused for an object on
	 * the line (`clear`).
	 */
	unsigned candidates{};
	unsigned favourable{};
	unsigned indirect_favourable{};
	unsigned blocked{};
};

/* Every line clear (the tests): the game checks the objects on the line
 * to an indirect aim point (a teammate, the reactor, a robot, a ship
 * whose burst would be near the bot).
 */
struct any_line_clear
{
	constexpr bool operator()(const aim_option &) const
	{
		return true;
	}
};

template <typename Geometry, typename Clear = any_line_clear>
[[nodiscard]]
aim_choice choose_heavy_aim(const Geometry &geo, const blast_scene &sc, const missile_role r, const missile_data &md, const risk_profile &rp, const aim_search &search = {}, Clear &&clear = {})
{
	aim_choice c;
	const bool children{r == missile_role::shaker && md.children > 0 && md.child_blast_radius > 0};
	const double reach_visible{std::max(md.blast_radius, 0.0) * 0.9};
	const double reach{sc.target_visible ? reach_visible : (children ? std::max(reach_visible, 60.0) : reach_visible)};
	/* The candidates: the direct shot and the wall behind always, the
	 * others the most promising first, near duplicates once.
	 */
	std::vector<std::pair<aim_kind, vec3>> fixed, others;
	const auto near_known{[&](const vec3 &p) {
		for (const auto &f : fixed)
			if (distance(f.second, p) < 3)
				return true;
		for (const auto &f : others)
			if (distance(f.second, p) < 3)
				return true;
		return false;
	}};
	aim_candidates(geo, sc, reach, rp.indirect, [&](const aim_kind kind, const vec3 &point) {
		if (kind == aim_kind::direct || kind == aim_kind::wall_behind)
			fixed.emplace_back(kind, point);
		else if (!near_known(point))
			others.emplace_back(kind, point);
	}, search);
	const auto indirect_kind{sc.target_visible ? aim_kind::near_wall : aim_kind::corner};
	if (rp.indirect && search.previous && indirect_aim_ok(sc, *search.previous, reach) && !near_known(*search.previous))
		fixed.emplace_back(indirect_kind, *search.previous);
	if (others.size() > search.max_indirect)
	{
		const double radius{std::max(md.blast_radius, 1.0)};
		std::vector<std::pair<double, std::size_t>> ranked;
		ranked.reserve(others.size());
		for (std::size_t j = 0; j < others.size(); ++j)
		{
			const auto &p{others[j].second};
			const double e{blast_damage(distance(p, sc.target), std::max(reach, 1.0), 1) - 1.5 * blast_damage(distance(p, sc.bot), radius, 1)};
			ranked.emplace_back(std::isfinite(e) ? e : -1e9, j);
		}
		std::ranges::stable_sort(ranked, [](const auto &a, const auto &b) { return a.first > b.first; });
		std::vector<std::pair<aim_kind, vec3>> kept;
		kept.reserve(search.max_indirect);
		for (std::size_t j = 0; j < search.max_indirect; ++j)
			kept.push_back(others[ranked[j].second]);
		others = std::move(kept);
	}
	std::optional<blast_outcome> direct_outcome;
	std::optional<risk_verdict> direct_verdict;
	double least_bad{-1e18};
	const auto weigh{[&](const aim_kind kind, const vec3 &point) {
		aim_option a;
		a.kind = kind;
		a.point = point;
		a.dir = normalized(point - sc.bot);
		a.outcome = evaluate_burst(geo, sc, r, md, a.dir, kind == aim_kind::direct);
		if (kind == aim_kind::direct)
			direct_outcome = a.outcome;
		else if (kind == aim_kind::wall_behind || may_meet_target(sc, a.dir, md))
		{
			/* It may meet the target on the way: the nearer burst's
			 * risk too.
			 */
			if (!direct_outcome)
				direct_outcome = evaluate_burst(geo, sc, r, md, sc.target - sc.bot, true);
			a.outcome = merge_meet(a.outcome, *direct_outcome);
		}
		a.verdict = judge_blast(a.outcome, sc, md, rp);
		a.score = blast_value(a.outcome, sc) - rp.trade * a.outcome.self_damage;
		++c.candidates;
		if (kind == aim_kind::direct)
			direct_verdict = a.verdict;
		if (a.verdict == risk_verdict::fire)
		{
			/* The direct shot is preferred when about as good: the aim
			 * is the target's, the easiest.
			 */
			const double score{a.score * (kind == aim_kind::direct ? 1.15 : 1)};
			const bool better{!c.best || score > c.best->score * (c.best->kind == aim_kind::direct ? 1.15 : 1)};
			/* An indirect aim that would win: the objects on its line. */
			if (kind != aim_kind::direct && better && !clear(a))
			{
				++c.blocked;
				return;
			}
			++c.favourable;
			if (kind != aim_kind::direct)
				++c.indirect_favourable;
			if (better)
				c.best = a;
		}
		else if (!direct_verdict && a.score > least_bad)
		{
			least_bad = a.score;
			c.why = a.verdict;
		}
	}};
	for (const auto &[kind, point] : fixed)
		weigh(kind, point);
	for (const auto &[kind, point] : others)
		weigh(kind, point);
	if (direct_verdict)
		c.why = *direct_verdict;
	return c;
}

/* Section 9.6, hugging: an enemy that holds a heavy missile (the bot saw
 * it pick one up or fire one) and faces the bot at mid range will not
 * fire it at a bot that hugs its ship, or kills itself too.  The bot
 * closes in with its profile's chance (drawn once per engagement), when
 * it has no heavy shot of its own usable soon (then it keeps its
 * standoff instead: one mode at a time) and is not about to retreat.
 * A mode holds for HUG_HOLD_SECONDS (no flapping); a hug ends when the
 * enemy is out of sight for HUG_LOST_SECONDS.
 */
constexpr double HUG_START_MIN{25};
constexpr double HUG_START_MAX{160};
constexpr double HUG_HOLD_SECONDS{1.5};
constexpr double HUG_LOST_SECONDS{1.5};
/* Hugging, it keeps within this distance (and not nearer than
 * HUG_NEAREST, the ships bump).
 */
constexpr double HUG_NEAREST{8};

struct hug_view
{
	bool enemy_heavy{};
	bool enemy_facing{};
	double distance{};
	/* Already hugging (it keeps on while the enemy is in reach), and
	 * the seconds since the hug started or ended.
	 */
	bool hugging{};
	double since_change{1e9};
	/* Seconds the enemy is out of sight (0: in sight). */
	double unseen_for{};
	/* The bot's own heavy missile usable soon (heavy_usable_soon). */
	bool own_heavy_soon{};
	bool weak{};
	/* The draw of this engagement, uniform in [0, 1). */
	double roll{1};
};

[[nodiscard]]
constexpr bool want_hug(const hug_view &v, const risk_profile &rp)
{
	if (!v.enemy_heavy || v.weak)
		return false;
	if (v.hugging)
	{
		if (v.unseen_for > HUG_LOST_SECONDS)
			return false;
		if (v.since_change < HUG_HOLD_SECONDS)
			return true;
		return !v.own_heavy_soon && v.distance <= HUG_START_MAX;
	}
	if (v.own_heavy_soon || v.unseen_for > 0 || v.since_change < HUG_HOLD_SECONDS)
		return false;
	if (!v.enemy_facing || v.distance < HUG_START_MIN || v.distance > HUG_START_MAX)
		return false;
	return v.roll < rp.hug;
}

/* What a bot knows of an enemy's heavy missiles (section 9.6): how many
 * it saw it pick up and not fire yet, and until when (in ticks) it
 * counts as holding one.  A pickup: one more, HEAVY_KNOWN_SECONDS.  A
 * shot: one less; the last known one fired, it is forgotten; a shot not
 * seen picked up, it may hold more, for HEAVY_GUESS_SECONDS.
 */
constexpr double HEAVY_KNOWN_SECONDS{25};
constexpr double HEAVY_GUESS_SECONDS{10};

struct heavy_holding
{
	unsigned count{};
	uint32_t until{};
	[[nodiscard]]
	constexpr bool held(const uint32_t tick) const
	{
		return tick < until;
	}
};

[[nodiscard]]
constexpr heavy_holding heavy_picked_up(const heavy_holding h, const uint32_t tick)
{
	return {std::min(h.count + 1, 9u), tick + static_cast<uint32_t>(HEAVY_KNOWN_SECONDS * BOT_TICK_RATE)};
}

[[nodiscard]]
constexpr heavy_holding heavy_fired(const heavy_holding h, const uint32_t tick)
{
	if (h.count > 1)
		return {h.count - 1, tick + static_cast<uint32_t>(HEAVY_KNOWN_SECONDS * BOT_TICK_RATE)};
	if (h.count == 1)
		return {};
	return {0, tick + static_cast<uint32_t>(HEAVY_GUESS_SECONDS * BOT_TICK_RATE)};
}

/* The distance to keep while hugging: well inside the enemy's own blast
 * (it will not fire), from HUG_NEAREST.
 */
[[nodiscard]]
constexpr double hug_distance(const double enemy_blast_radius)
{
	return std::clamp(0.35 * enemy_blast_radius, HUG_NEAREST + 6, 22.0);
}

/* Section 9.6, the other way round: a bot holding a heavy missile whose
 * target closes in within the distance it needs (the target hugs, or
 * rushes it) and that has no favourable aim breaks the line of sight
 * round a corner (a place the target cannot see, nearby), then fires
 * at the corner or at the target coming round it.  It also does so when
 * it cannot back off (a wall close behind it).
 */
struct duck_view
{
	bool heavy_ready{};
	/* The best aim is favourable now. */
	bool favourable{};
	double distance{};
	double standoff{};
	double target_closing{};
	bool back_blocked{};
	/* The style's tendency (risk_profile::duck_share, duck_closing). */
	double duck_share{0.6};
	double duck_closing{5};
};

[[nodiscard]]
constexpr bool want_duck(const duck_view &v)
{
	if (!v.heavy_ready || v.favourable || !(v.standoff > 0) || v.distance >= v.standoff)
		return false;
	return v.target_closing > v.duck_closing || v.back_blocked || v.distance < v.duck_share * v.standoff;
}

/* How a bot that sees its target moves (sections 4.6, 4.7 and 9.6):
 * - `combat`: the fight band, strafing, the standoff and the blast hold
 *   (combat_velocity), with a clear shot and no path goal;
 * - `path`: its path (collecting, retreating, refuelling, or without a
 *   clear shot);
 * - `duck`: to its duck point, which overrides both while it lasts.
 * One choice per tick, so no mode's velocity is overwritten by
 * another's.
 */
enum class engaged_move : uint8_t
{
	combat,
	path,
	duck,
};

[[nodiscard]]
constexpr engaged_move engaged_movement(const bool shot_clear, const bool path_goal, const bool ducking)
{
	if (ducking)
		return engaged_move::duck;
	if (shot_clear && !path_goal)
		return engaged_move::combat;
	return engaged_move::path;
}

/* Where it ducks: a place 15-90 units away, preferably far from the
 * target and near the bot.  Of the places ranked so, the first
 * `max_checks` are checked: the bot can fly there in a straight line
 * (`flyable`) and the target cannot see it (`hidden`).
 */
constexpr double DUCK_MIN_DISTANCE{15};
constexpr double DUCK_MAX_DISTANCE{90};

[[nodiscard]]
inline double duck_score(const vec3 &bot, const vec3 &target, const vec3 &place)
{
	return distance(place, target) - 0.5 * distance(place, bot);
}

template <typename Flyable, typename Hidden>
[[nodiscard]]
std::optional<vec3> pick_duck_point(const std::span<const vec3> places, const vec3 &bot, const vec3 &target, const unsigned max_checks, Flyable &&flyable, Hidden &&hidden)
{
	std::vector<std::pair<double, vec3>> ranked;
	for (const auto &p : places)
	{
		const double d{distance(p, bot)};
		if (d >= DUCK_MIN_DISTANCE && d <= DUCK_MAX_DISTANCE)
			ranked.emplace_back(duck_score(bot, target, p), p);
	}
	std::ranges::sort(ranked, [](const auto &a, const auto &b) { return a.first > b.first; });
	unsigned checked{0};
	for (const auto &[score, p] : ranked)
	{
		if (checked++ >= max_checks)
			break;
		if (hidden(p) && flyable(p))
			return p;
	}
	return std::nullopt;
}


/* What the missile choice looks at (the tactics layer fills it). */
struct missile_situation
{
	/* Rounds of each secondary, in the game's order. */
	std::array<uint8_t, BOT_SECONDARY_COUNT> ammo{};
	std::array<missile_data, BOT_SECONDARY_COUNT> data{};
	unsigned smarts{2};
	/* Section 9.7: the style's scale of the time between two mines. */
	double mine_interval_scale{1};
	/* Section 9.9: the style's scale of the time between two missiles
	 * (missile_interval_scale), and the self-damage the bot accepts (its
	 * risk budget times its shields: heavy_min_distance).
	 */
	double missile_interval_scale{1};
	double accepted_damage{};
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
	/* Section 9.10: the bot pursues the target round a corner (a homing
	 * missile at the corner's exit: homing_round_corner).
	 */
	bool pursuing{};
	/* Seconds since the last corner homing shot was chosen (the PR #47
	 * review: HOMING_CORNER_INTERVAL).
	 */
	double since_corner_homing{1e9};
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
	/* The bot's speed toward the target, and the target's toward the
	 * bot (units/s; negative: away): the blast is judged where the bot
	 * is when the missile meets the target.
	 */
	double closing_speed{};
	double target_closing_speed{};
	/* Section 9.6: the expected-outcome verdict of each heavy missile
	 * (choose_heavy_aim; unset: the strict distance rules of section
	 * 9.5), the kind of its best aim, and the bot's standoff scale.
	 */
	std::array<std::optional<heavy_verdict>, BOT_SECONDARY_COUNT> heavy_risk{};
	std::array<aim_kind, BOT_SECONDARY_COUNT> heavy_aim{};
	double standoff_scale{1};
	/* Section 9.17: the bot's eagerness to use its heavy missile
	 * (heavy_eagerness): from one half on, the heavy cooldown is shorter
	 * and a target fired at may have another.
	 */
	double heavy_eagerness{};
};


/* The least distance at which heavy missile `s` may be fired, by its
 * range and its blast (without invulnerability).  Section 9.9: with the
 * self-damage the bot accepts (`accepted_damage`, its risk budget times
 * its shields), where the blast's nominal damage falls to that, at most
 * HEAVY_ACCEPT_SHARE of the radius nearer.  The exp-19 log (the real
 * data: mega and earthshaker blast 80, damage 199 and 220) had 74-130
 * units (1 and 1.2 radii plus the margin, times the standoff scale),
 * beyond the fight's 35-95; an aggressive Insane bot (budget 0.66 x 100
 * shields) now needs 66 units for a mega, 79 for an earthshaker.  The
 * release is still the expected outcome's (judge_blast).
 */
constexpr double HEAVY_ACCEPT_SHARE{0.4};

[[nodiscard]]
constexpr double heavy_min_distance(const secondary s, const missile_data &md, const double accepted_damage = 0)
{
	const auto r{role_of(s)};
	const double range_min{r == missile_role::shaker ? SHAKER_MIN_DISTANCE : HEAVY_MIN_DISTANCE};
	const double share{md.damage > 0 ? std::clamp(accepted_damage / md.damage, 0.0, HEAVY_ACCEPT_SHARE) : 0.0};
	return std::max(range_min, blast_factor(r) * std::max(md.blast_radius, 0.0) * (1 - share) + BLAST_MARGIN);
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
	if (m.since_missile < missile_interval(m.smarts) * m.missile_interval_scale || m.since_heavy < heavy_interval(m.smarts) * (1 - 0.5 * m.heavy_eagerness))
		return heavy_verdict::cooldown;
	/* Section 9.6: a favourable aim at a wall or a corner needs no clear
	 * line to the target (a hidden one seen lately).
	 */
	const auto &risk{m.heavy_risk[i]};
	const bool indirect{risk && *risk == heavy_verdict::fire && m.heavy_aim[i] != aim_kind::direct};
	if (!m.target_visible && !(indirect && m.target_seen_ago <= CORNER_SEEN_WITHIN))
		return heavy_verdict::not_visible;
	if (!m.shot_clear && !indirect)
		return heavy_verdict::no_clear_shot;
	/* Section 9.9: cloaked, from Ace a heavy missile at a target as near
	 * as the light ones may be (CLOAKED_MISSILE_DISTANCE: the launch
	 * shows where the bot is, but a close target has little time to
	 * use that; 64 of the exp-19 log's seconds with a heavy missile
	 * were "cloaked").
	 */
	if (m.cloaked && (m.smarts < 3 || m.target_distance > CLOAKED_MISSILE_DISTANCE))
		return heavy_verdict::cloaked;
	if (m.heavy_used_on_target && m.heavy_eagerness < 0.5)
		return heavy_verdict::used_on_target;
	const auto &md{m.data[i]};
	const double d{m.target_distance};
	if (risk)
	{
		/* Section 9.6: the expected outcome of the best aim replaces
		 * the distance rules below.
		 */
		if (d > (role_of(s) == missile_role::shaker ? SHAKER_MAX_DISTANCE : HEAVY_MAX_DISTANCE))
			return heavy_verdict::too_far;
		return *risk;
	}
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
	if (!blast_safe(r, d, md, m.invulnerable_left, m.closing_speed, m.target_closing_speed))
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
		switch (v)
		{
			case heavy_verdict::too_close:
			case heavy_verdict::blast:
			case heavy_verdict::fire:
			/* Section 9.6: the rejections a distance mends. */
			case heavy_verdict::lethal:
			case heavy_verdict::risky:
			case heavy_verdict::poor_trade:
				break;
			default:
				continue;
		}
		{
			const double d{heavy_min_distance(s, m.data[static_cast<unsigned>(s)], m.accepted_damage) * m.standoff_scale + 8};
			if (!best || d < best)
				best = d;
		}
	}
	return best;
}

/* Section 9.6: the bot's own heavy missile is usable within
 * HEAVY_SOON_SECONDS (held, its skill uses it, the cooldowns end by
 * then, not cloaked, not used on this target yet): it keeps its standoff
 * rather than hug.
 */
constexpr double HEAVY_SOON_SECONDS{2};

[[nodiscard]]
constexpr bool heavy_usable_soon(const missile_situation &m)
{
	if (m.cloaked || (m.heavy_used_on_target && m.heavy_eagerness < 0.5))
		return false;
	for (const auto s : {secondary::earthshaker, secondary::mega})
	{
		if (!m.ammo[static_cast<unsigned>(s)] || m.smarts < min_smarts(s))
			continue;
		const double wait{std::max(missile_interval(m.smarts) * m.missile_interval_scale - m.since_missile, heavy_interval(m.smarts) * (1 - 0.5 * m.heavy_eagerness) - m.since_heavy)};
		if (wait <= HEAVY_SOON_SECONDS)
			return true;
	}
	return false;
}

/* Section 9.10: pursuing a target that went round a corner a moment ago
 * (HOMING_CORNER_SEEN_WITHIN), from Ace a homing missile goes at the
 * corner's exit (the bot faces it: corner_approach_point) and finds the
 * target behind it (the game's homing takes what it sees ahead), as
 * the smart missile's children do.  At the distance of a homing shot
 * (HOMING_MIN_DISTANCE to HOMING_CORNER_MAX_DISTANCE from the place it
 * was last seen); the release checks the blast along the nose.
 */
constexpr double HOMING_CORNER_SEEN_WITHIN{2};
constexpr double HOMING_CORNER_MAX_DISTANCE{150};
/* The PR #47 review: one try at a corner per this many seconds (from its
 * choice: an empty corner, or an aim never reached, is not shot at, nor
 * chosen, over and over).
 */
constexpr double HOMING_CORNER_INTERVAL{6};

[[nodiscard]]
constexpr bool homing_round_corner(const missile_situation &m)
{
	return m.pursuing && m.has_target && !m.target_visible && !m.cloaked && m.smarts >= 3 &&
		m.ammo[static_cast<unsigned>(secondary::homing)] > 0 && m.smarts >= min_smarts(secondary::homing) &&
		m.since_corner_homing >= HOMING_CORNER_INTERVAL &&
		m.target_seen_ago <= HOMING_CORNER_SEEN_WITHIN &&
		m.target_distance >= HOMING_MIN_DISTANCE && m.target_distance <= HOMING_CORNER_MAX_DISTANCE;
}

/* The release of a chosen missile (not a mine): what it is aimed at, or
 * held.  In sight, at the target with a clear line (a heavy one aimed at
 * a wall or corner also without); out of sight, a smart missile (its
 * children find the target) and a heavy one aimed at a wall or corner,
 * at the remembered target; and (the PR #47 review) a homing missile
 * chosen round the corner of a pursuit still under way, at the corner's
 * exit (release_aim::corner).  Before the review the release held every
 * unseen shot but the smart and the indirect heavy one, so the corner
 * shot was chosen, held and chosen again every MISSILE_PENDING_SECONDS
 * and never fired.
 */
enum class release_aim : uint8_t
{
	hold,
	target,
	corner,
};

struct release_view
{
	missile_role role{missile_role::none};
	/* A place of the target, seen or remembered. */
	bool has_target_pos{};
	bool target_visible{};
	bool shot_clear{};
	/* A heavy missile aimed at a wall or a corner (section 9.6). */
	bool indirect{};
	/* Chosen as the corner shot (homing_round_corner), and the pursuit
	 * of that target still under way.
	 */
	bool corner_shot{};
	bool pursuing{};
};

[[nodiscard]]
constexpr release_aim release_aim_of(const release_view &v)
{
	if (!v.has_target_pos)
		return release_aim::hold;
	if (v.target_visible)
		return v.shot_clear || v.indirect ? release_aim::target : release_aim::hold;
	if (v.role == missile_role::smart || v.indirect)
		return release_aim::target;
	if (v.role == missile_role::homing && v.corner_shot && v.pursuing)
		return release_aim::corner;
	return release_aim::hold;
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
	if (m.chased && !m.teammate_behind && m.since_mine >= mine_interval(m.smarts) * m.mine_interval_scale && m.pursuer_distance < MINE_PURSUER_DISTANCE * (m.at_doorway ? 1.5 : 1))
	{
		if (usable(secondary::smart_mine))
			return secondary::smart_mine;
		if (usable(secondary::proximity))
			return secondary::proximity;
	}
	if (!m.has_target || m.since_missile < missile_interval(m.smarts) * m.missile_interval_scale)
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
	/* Section 9.10: a homing missile round the corner of a pursuit. */
	if (homing_round_corner(m))
		return secondary::homing;
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

/* Section 9.9: why a light missile (concussion, homing, mercury; smart
 * and flash count as light here) is or is not fired now, for the log
 * (-verbose): the first rule of choose_secondary that fails, or, for a
 * missile chosen, the release (bot.cpp: aiming, nose-blast), or fired.
 * The exp-19 log had no light verdict, so what held the volleys back
 * could not be read from it.
 */
enum class light_verdict : uint8_t
{
	none_owned,
	no_target,
	cooldown,
	not_visible,
	no_clear_shot,
	too_close,
	too_far,
	cloaked,
	heavy,
	chosen,
	aiming,
	nose_blast,
	fired,
};

inline constexpr std::array<const char *, 13> light_verdict_names{{
	"none-owned", "no-target", "cooldown", "not-visible", "no-clear-shot", "too-close", "too-far", "cloaked", "heavy", "chosen", "aiming", "nose-blast", "fired",
}};

[[nodiscard]]
constexpr const char *name_of(const light_verdict v)
{
	const auto i{static_cast<unsigned>(v)};
	return i < light_verdict_names.size() ? light_verdict_names[i] : "?";
}

/* The verdict of choose_secondary on the light missiles (`chosen`: the
 * secondary it chose, if any).
 */
[[nodiscard]]
constexpr light_verdict light_check(const missile_situation &m, const std::optional<secondary> chosen)
{
	if (chosen)
	{
		const auto r{role_of(*chosen)};
		if (r == missile_role::heavy || r == missile_role::shaker)
			return light_verdict::heavy;
		if (r != missile_role::mine)
			return light_verdict::chosen;
	}
	bool owned{false};
	for (const auto s : {secondary::concussion, secondary::homing, secondary::mercury, secondary::smart, secondary::flash})
		if (m.ammo[static_cast<unsigned>(s)] && m.smarts >= min_smarts(s))
			owned = true;
	if (!owned)
		return light_verdict::none_owned;
	if (!m.has_target)
		return light_verdict::no_target;
	if (m.since_missile < missile_interval(m.smarts) * m.missile_interval_scale)
		return light_verdict::cooldown;
	if (m.cloaked && m.target_distance > CLOAKED_MISSILE_DISTANCE)
		return light_verdict::cloaked;
	if (!m.target_visible)
		return light_verdict::not_visible;
	if (!m.shot_clear)
		return light_verdict::no_clear_shot;
	if (m.target_distance < MISSILE_MIN_DISTANCE || (m.target_distance < HOMING_MIN_DISTANCE && !m.ammo[static_cast<unsigned>(secondary::concussion)] && !m.ammo[static_cast<unsigned>(secondary::mercury)] && !m.ammo[static_cast<unsigned>(secondary::flash)]))
		return light_verdict::too_close;
	return light_verdict::too_far;
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
constexpr bool missile_release(const secondary s, const double aim_error, const double fire_cone, const double impact_distance, const missile_data &md, const double invulnerable_left, const double closing_speed = 0, const double target_closing = 0)
{
	const auto r{role_of(s)};
	if (r == missile_role::none)
		return false;
	if (r == missile_role::mine)
		return true;
	return aim_error <= missile_cone(r, fire_cone, md.homing) && blast_safe(r, impact_distance, md, invulnerable_left, closing_speed, target_closing);
}

/* Section 9.8: volleys.  "A single homing has no impact.  A fleet of 3-5
 * of them make an opponent run."  The light missiles (concussion,
 * homing, mercury) go in volleys at a good target, smart missiles in
 * bursts at a target behind cover (or, from Ace, in sight at mid
 * range), the heavy ones (mega, earthshaker) one at a time as before.
 * The rounds of a volley follow each other at VOLLEY_GAP (the game's own
 * refire limit, the weapon's fire_wait, holds as for a human:
 * allowed_to_fire_missile); missile_interval separates two volleys.
 */
constexpr double VOLLEY_GAP{0.1};
constexpr unsigned VOLLEY_MAX{5};
/* A good target for a volley of straight missiles: near enough that a
 * crossing target does not simply fly out of the stream.
 */
constexpr double VOLLEY_STRAIGHT_MAX_DISTANCE{130};
constexpr double VOLLEY_STRAIGHT_MAX_LATERAL{40};
constexpr double VOLLEY_HOMING_MAX_DISTANCE{170};
constexpr double SMART_BURST_MIN_DISTANCE{40};

/* Section 9.9: the exp-19 log had 88 volleys in ten minutes for five
 * bots (one per bot every 30 s) on a level full of missiles.  From Ace
 * a good target reaches further and crosses faster (the values above
 * are Hotshot's and below): straight missiles to 150 units (Insane 160)
 * at up to 50 units/s across (Insane 60), homing ones to 200 units; and
 * an Insane bot fires a pair at any other target in sight with a clear
 * line in the missiles' range (below: one).
 */
[[nodiscard]]
constexpr double volley_straight_max_distance(const unsigned smarts)
{
	return smarts >= 4 ? 160 : smarts >= 3 ? 150 : VOLLEY_STRAIGHT_MAX_DISTANCE;
}

[[nodiscard]]
constexpr double volley_straight_max_lateral(const unsigned smarts)
{
	return smarts >= 4 ? 60 : smarts >= 3 ? 50 : VOLLEY_STRAIGHT_MAX_LATERAL;
}

[[nodiscard]]
constexpr double volley_homing_max_distance(const unsigned smarts)
{
	return smarts >= 3 ? 200 : VOLLEY_HOMING_MAX_DISTANCE;
}

/* Section 9.9: the rounds after the first of a straight volley go
 * within this wider cone (twice the skill's, at least 6 degrees): the
 * stream spreads round the target, which is what makes it run.  The
 * first round keeps the skill's cone.
 */
[[nodiscard]]
constexpr double volley_cone(const double fire_cone)
{
	constexpr double pi{3.14159265358979323846};
	return std::max(2 * fire_cone, 6 * pi / 180);
}

struct volley_view
{
	secondary s{secondary::concussion};
	unsigned smarts{2};
	bot_style style{bot_style::balanced};
	/* Rounds of `s` left. */
	unsigned ammo{};
	bool target_visible{};
	bool shot_clear{};
	double target_distance{};
	double target_lateral_speed{};
	double target_seen_ago{1e9};
	bool cloaked{};
	/* Section 9.13: a style profile's missiles per volley
	 * (tune.volley_size): the size of a good volley, in place of the
	 * table; negative: the table.
	 */
	double mean{-1};
};

/* The rounds of a good volley of light missiles by weapon smarts and
 * style (volley_size's table).
 */
[[nodiscard]]
constexpr int volley_table_size(const unsigned smarts, const bot_style style)
{
	constexpr std::array<int, 5> by_smarts{{1, 1, 2, 3, 4}};
	const int style_step{style == bot_style::aggressive ? 1 : style == bot_style::cautious ? -1 : 0};
	return by_smarts[std::min<std::size_t>(smarts, by_smarts.size() - 1)] + style_step;
}

/* The rounds of the volley that starts with `v.s` now (1: a single
 * shot).  By weapon smarts (Rookie 1, Hotshot 2, Ace 3, Insane 4 light
 * missiles), Aggressive one more, Cautious one fewer; never more than
 * the bot holds.
 */
[[nodiscard]]
constexpr unsigned volley_size(const volley_view &v)
{
	if (!v.ammo)
		return 0;
	const auto r{role_of(v.s)};
	int n{1};
	const int style_step{v.style == bot_style::aggressive ? 1 : v.style == bot_style::cautious ? -1 : 0};
	switch (r)
	{
		case missile_role::straight:
		case missile_role::homing:
		{
			if (!v.target_visible || !v.shot_clear || v.cloaked)
				break;
			const bool good{r == missile_role::homing
				? v.target_distance >= HOMING_MIN_DISTANCE && v.target_distance <= volley_homing_max_distance(v.smarts)
				: v.target_distance >= MISSILE_MIN_DISTANCE && v.target_distance <= volley_straight_max_distance(v.smarts) && v.target_lateral_speed <= volley_straight_max_lateral(v.smarts)};
			if (!good)
			{
				/* Section 9.9: an Insane bot's pair. */
				if (v.smarts >= 4 && v.target_distance >= MISSILE_MIN_DISTANCE && v.target_distance <= MISSILE_MAX_DISTANCE)
					n = 2;
				break;
			}
			n = v.mean >= 1 ? static_cast<int>(std::min(v.mean, 8.0) + 0.5) : volley_table_size(v.smarts, v.style);
			break;
		}
		case missile_role::smart:
		{
			/* Behind cover (its children find it round the corner),
			 * or from Ace in sight at mid range.
			 */
			const bool cover{!v.target_visible && v.target_seen_ago <= SMART_SEEN_WITHIN};
			const bool in_sight{v.target_visible && v.shot_clear && v.smarts >= 3 && v.target_distance >= SMART_BURST_MIN_DISTANCE && v.target_distance <= SMART_MAX_DISTANCE};
			if (!(cover || in_sight) || v.cloaked)
				break;
			constexpr std::array<int, 5> by_smarts{{1, 1, 2, 2, 3}};
			n = std::min(v.mean >= 1 ? static_cast<int>(std::min(v.mean, 8.0) + 0.5) : by_smarts[std::min<std::size_t>(v.smarts, by_smarts.size() - 1)] + style_step, 3);
			break;
		}
		default:
			/* Heavy missiles one at a time; flash, mines: one. */
			break;
	}
	return static_cast<unsigned>(std::clamp<int>(n, 1, std::min<int>(static_cast<int>(VOLLEY_MAX), static_cast<int>(v.ammo))));
}

/* The next round of a volley under way goes now: rounds left, the
 * missile held, the gap passed, and the target still one the volley's
 * rule accepts (a straight or homing volley needs the target in sight
 * and a clear line; a smart burst one seen a moment ago).
 */
[[nodiscard]]
constexpr bool volley_continues(const volley_view &v, const unsigned rounds_left, const double since_missile)
{
	if (!rounds_left || !v.ammo || since_missile < VOLLEY_GAP || v.cloaked)
		return false;
	switch (role_of(v.s))
	{
		case missile_role::straight:
		case missile_role::homing:
			return v.target_visible && v.shot_clear && v.target_distance >= MISSILE_MIN_DISTANCE && v.target_distance <= MISSILE_MAX_DISTANCE;
		case missile_role::smart:
			return (v.target_visible && v.shot_clear) || v.target_seen_ago <= SMART_SEEN_WITHIN;
		default:
			return false;
	}
}

/* Section 9.8: the death dump.  "We tend to push the rocket fire button
 * when we are about to die... to not give the player who killed you all
 * your inventory."  A bot about to die (its shields very low while it is
 * under fire, or more damage coming at it than it has shields) fires its
 * missiles and drops its mines as fast as the game lets it, most
 * valuable first.  The shots go through the normal firing as the bot,
 * one at a time as the game's refire limit allows; a missile only with
 * its blast clear of the bot (blast_safe along the nose), so a bot never
 * kills itself to deny the kill.  Higher skill does it more
 * reliably (a roll drawn once per life).
 */
struct death_dump_view
{
	double shields{100};
	/* Seconds since an enemy last hit the bot. */
	double since_hit{1e9};
	/* Damage of the enemy shots on a course that meets the bot within
	 * the next moment (the dodge's scan).
	 */
	double incoming_damage{};
	bool invulnerable{};
	/* Any secondary the bot may fire held. */
	bool has_ammo{};
	/* This life's roll, uniform in [0, 1). */
	double roll{1};
};

/* Under fire: hit within this many seconds. */
constexpr double DEATH_DUMP_UNDER_FIRE{1.2};

[[nodiscard]]
constexpr double death_dump_shields(const bot_skill k, const bot_style s)
{
	constexpr std::array<double, BOT_SKILL_COUNT> by_skill{{0, 10, 14, 17, 20}};
	const auto ki{static_cast<unsigned>(k) < BOT_SKILL_COUNT ? static_cast<unsigned>(k) : static_cast<unsigned>(BOT_DEFAULT_SKILL)};
	if (!by_skill[ki])
		return 0;
	/* A collector minds its inventory most, an aggressive bot fights to
	 * the last.
	 */
	return by_skill[ki] + (s == bot_style::collector ? 4 : s == bot_style::cautious ? 3 : s == bot_style::aggressive ? -2 : 0);
}

[[nodiscard]]
constexpr double death_dump_reliability(const bot_skill k, const bot_style s)
{
	constexpr std::array<double, BOT_SKILL_COUNT> by_skill{{0, 0.25, 0.6, 0.85, 0.97}};
	const auto ki{static_cast<unsigned>(k) < BOT_SKILL_COUNT ? static_cast<unsigned>(k) : static_cast<unsigned>(BOT_DEFAULT_SKILL)};
	if (!by_skill[ki])
		return 0;
	return std::min(1.0, by_skill[ki] + (s == bot_style::collector ? 0.1 : 0));
}

[[nodiscard]]
constexpr bool death_dump_wanted(const death_dump_view &v, const bot_skill k, const bot_style s)
{
	if (v.invulnerable || !v.has_ammo || !(v.shields > 0))
		return false;
	if (!(v.roll < death_dump_reliability(k, s)))
		return false;
	const bool low_under_fire{v.shields <= death_dump_shields(k, s) && v.since_hit <= DEATH_DUMP_UNDER_FIRE};
	/* A lethal hit on its way, to a bot already hurt (at most twice
	 * its threshold: a full-shield bot is not one shot from death).
	 */
	const bool lethal_incoming{v.incoming_damage >= v.shields && v.shields <= 2 * death_dump_shields(k, s)};
	return low_under_fire || lethal_incoming;
}

/* The dump's order: what the killer would value most first. */
inline constexpr std::array<secondary, 9> death_dump_order{{
	secondary::earthshaker,
	secondary::mega,
	secondary::smart,
	secondary::smart_mine,
	secondary::homing,
	secondary::mercury,
	secondary::proximity,
	secondary::concussion,
	secondary::flash,
}};

/* The secondary to dump now: the first of death_dump_order held that the
 * skill uses and that may go (`blast_ok`: a missile's blast clear of the
 * bot, dump_outcome and dump_blast_ok; a mine only without a teammate
 * behind, as the normal path's).
 */
template <typename BlastOk>
[[nodiscard]]
constexpr std::optional<secondary> death_dump_choice(const std::array<uint8_t, BOT_SECONDARY_COUNT> &ammo, const unsigned smarts, BlastOk &&blast_ok)
{
	for (const auto s : death_dump_order)
	{
		if (!ammo[static_cast<unsigned>(s)] || smarts < min_smarts(s))
			continue;
		if (!blast_ok(s))
			continue;
		return s;
	}
	return std::nullopt;
}

/* The PR #38 review: the dump's missile is weighed with the heavy
 * missiles' model (evaluate_burst, section 9.6), not only against the
 * wall along the nose: it bursts on the first ship on the line, and it
 * may meet any ship near the line or, homing, within its homing cone
 * (may_meet_target) -- the attacker, another enemy, a teammate.  The
 * worse of all those (merge_meet) counts.  A teammate it may meet
 * refuses the shot (the normal path's shot_clear: never a teammate on
 * the line; homing missiles do not track teammates, so only the line
 * counts for one).
 */
struct dump_ship
{
	vec3 pos{};
	vec3 vel{};
	bool teammate{};
	/* First on the line along the nose (the game's object cast). */
	bool on_line{};
};

template <typename Geometry>
[[nodiscard]]
std::optional<blast_outcome> dump_outcome(const Geometry &geo, blast_scene sc, const missile_role r, const missile_data &md, const vec3 &dir, const std::span<const dump_ship> ships)
{
	const auto u{normalized(dir)};
	/* Along the nose to the wall: no target (a point far behind, out of
	 * sight, that the children do not find).
	 */
	sc.target = sc.bot - u * (4 * BURST_CAST_LIMIT);
	sc.target_vel = {};
	sc.target_visible = false;
	sc.unseen_for = 0;
	auto o{evaluate_burst(geo, sc, r, md, u, false)};
	for (const auto &ship : ships)
	{
		auto meet_sc{sc};
		meet_sc.target = ship.pos;
		meet_sc.target_vel = ship.vel;
		meet_sc.target_visible = true;
		auto line_md{md};
		if (ship.teammate)
			line_md.homing = false;
		if (!ship.on_line && !(may_meet_target(meet_sc, u, line_md) && geo.sees(sc.bot, ship.pos)))
			continue;
		if (ship.teammate)
			return std::nullopt;
		o = merge_meet(o, evaluate_burst(geo, meet_sc, r, md, ship.pos - sc.bot, true));
	}
	return o;
}

/* The rule for a dump: never any self-damage (a bot about to die would
 * kill itself: no budget, no trade), never at point blank, the blast
 * clear of the bot where it is fired and at the burst (blast_safe's
 * distance), and an earthshaker only with the wall behind clear of its
 * children (shaker_behind_safe; evaluate_burst weighs them too).
 * Invulnerable beyond the danger, the distance rules alone.
 */
constexpr double DUMP_SELF_DAMAGE_MAX{0.5};

[[nodiscard]]
constexpr bool dump_blast_ok(const blast_outcome &o, const missile_role r, const missile_data &md, const double invulnerable_left, const double behind_distance)
{
	if (!(blast_factor(r) > 0))
		return true;
	const double nearest{std::min(o.impact, o.burst_distance)};
	if (nearest < MISSILE_MIN_DISTANCE)
		return false;
	if (!shaker_behind_safe(r, behind_distance, o.impact, md, invulnerable_left))
		return false;
	if (invulnerable_left > blast_danger_seconds(r, o.impact, md))
		return true;
	const double need{blast_factor(r) * std::max(md.blast_radius, 0.0) + BLAST_MARGIN};
	return nearest >= need && !(o.self_nominal > 0) && o.self_damage < DUMP_SELF_DAMAGE_MAX;
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
