/*
 * This file is part of the DXX-Rebirth project <https://github.com/dxx-rebirth/dxx-rebirth/>.
 * It is copyright by its individual contributors, as recorded in the
 * project's Git history.  See COPYING.txt at the top level for license
 * terms and a link to the Git history.
 */
/*
 * The bots in capture the flag (standard and Classic), hoard and team
 * hoard, the game-independent part (Documentation/multiplayer-bots.md
 * section 9.19, stage B7):
 *
 * - the roles of a team's bots (attack, defend, escort, hunt the enemy
 *   carrier, retrieve the own dropped flag; the carrier carries or
 *   waits), assigned on the host's team blackboard (assign_ctf_roles,
 *   assign_hoard_roles);
 * - what a role makes the bot want, against its other goals
 *   (objective_for: the objective goal's utility, the factors on the
 *   fight, the hunt and the collections, the path flown while shooting,
 *   the afterburner and the appetite for cover);
 * - in hoard, when to score the orbs carried (hoard_should_score), the
 *   value of an orb to collect, the weight of an enemy carrying orbs.
 *
 * Pure functions of their inputs, standard library only
 * (common/unittest/bot_modes.cpp).
 */

#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include "bot_brain.h"

namespace dcx::bot {

/* The role of a bot in a team mode.  The values are those of the
 * movement recording's role events (movement_record_format.h,
 * mode_role).
 */
enum class mode_role : uint8_t
{
	none,
	/* To the enemy flag: take it. */
	attack,
	/* Near the own flag (at home, or where it lies): keep enemies off
	 * it, intercept its taker.
	 */
	defend,
	/* With the own team's carrier. */
	escort,
	/* After the enemy who carries the own flag. */
	hunt,
	/* Carrying the enemy flag: home to the own goal. */
	carry,
	/* Carrying, but the own flag is away and the team scores only with
	 * it at home: wait near home under cover (or return the own flag).
	 */
	wait,
	/* To the own flag lying away from home: touch it to return it. */
	retrieve,
	/* Hoard: collect orbs (from the dead, by fighting). */
	collect,
	/* Hoard: to a goal, to score the orbs carried. */
	score,
};
constexpr unsigned MODE_ROLE_COUNT{10};

inline constexpr std::array<const char *, MODE_ROLE_COUNT> mode_role_names{{
	"none", "attack", "defend", "escort", "hunt", "carry", "wait", "retrieve", "collect", "score",
}};

[[nodiscard]]
constexpr const char *name_of(const mode_role r)
{
	const auto i{static_cast<unsigned>(r)};
	return i < MODE_ROLE_COUNT ? mode_role_names[i] : "?";
}

/* What a team knows of a flag (its own or the other team's).  Only what
 * a human of the team would know: the HUD tells everyone who picked up a
 * flag ("... picked up a flag!"), a capture, a return ("Red flag
 * returned"); the Classic rules put a flag home at the start and after
 * a capture.  Where a flag lies after its carrier died, or where a
 * carrier is, the team knows only from what its bots saw.
 */
enum class flag_state : uint8_t
{
	/* Not known (respawned at a random place, not seen since). */
	unknown,
	/* In its own goal (Classic). */
	home,
	/* In the level, away from home (or anywhere without Classic). */
	lying,
	/* Carried by a player. */
	carried,
};

/* A distance the role assignment takes for a place the bot does not
 * know, cannot reach, or a dead bot.
 */
constexpr double ROLE_FAR{3000};

/* What the team's blackboard knows of the team and the game. */
struct team_view
{
	/* The game's rules (net_v2_modes.h, ctf_rules). */
	bool classic{};
	bool touch_returns{};
	bool home_to_score{};
	flag_state own_flag{flag_state::unknown};
	flag_state enemy_flag{flag_state::unknown};
	/* The place of the own flag is known (lying: seen, or where its
	 * carrier was last seen).
	 */
	bool own_flag_place_known{};
	bool enemy_flag_place_known{};
	/* A player of the team (bot or human) carries the enemy flag. */
	bool team_carries{};
	/* Players of the team (humans and bots) and of the other team. */
	unsigned team_size{};
	unsigned enemies{};
	/* The team's score less the other's. */
	int score_lead{};
};

/* One player of the team, as the role assignment sees it. */
struct member_view
{
	uint8_t pid{};
	/* A bot the host flies: it takes a role.  A human is a teammate of
	 * unknown role: it counts in the team's size, but no role is given
	 * to it or counted for it.
	 */
	bool bot{};
	bool alive{true};
	/* It carries the enemy flag. */
	bool carrier{};
	bot_style style{bot_style::balanced};
	/* Its role at the last assignment (hysteresis). */
	mode_role previous{mode_role::none};
	/* Path costs (ROLE_FAR: not known, not reachable). */
	double to_own_home{ROLE_FAR};
	double to_enemy_flag{ROLE_FAR};
	double to_own_flag{ROLE_FAR};
	double to_enemy_carrier{ROLE_FAR};
	double to_own_carrier{ROLE_FAR};
};

/* The number of each role the team wants among its bots that do not
 * carry (role_quota).
 */
struct role_quota
{
	unsigned retrieve{};
	unsigned hunt{};
	unsigned escort{};
	unsigned defend{};
};

/* The defenders a team of `team_size` players wants with its flag at
 * home: none in a team of one or two (both attack), one from three, two
 * from six.  A team ahead keeps one more from four players; a team
 * behind by more than a capture sends its second defender forward.
 */
constexpr unsigned CTF_DEFEND_FROM{3};
constexpr unsigned CTF_SECOND_DEFENDER_FROM{6};
/* A capture's points (net_v2_modes.h CAPTURE_POINTS). */
constexpr int CTF_LEAD{5};

[[nodiscard]]
constexpr unsigned base_defenders(const team_view &t)
{
	unsigned d{t.team_size >= CTF_DEFEND_FROM ? 1u : 0u};
	if (t.team_size >= CTF_SECOND_DEFENDER_FROM)
		++d;
	/* Ahead by a capture or more (the team's score counts kills too: a
	 * kill traded does not flip the defence).
	 */
	if (t.score_lead >= CTF_LEAD && t.team_size >= 4 && t.team_size < CTF_SECOND_DEFENDER_FROM)
		++d;
	if (t.score_lead <= -CTF_LEAD && d > 1)
		--d;
	return d;
}

/* The roles the team wants among `bots` bots that do not carry, by the
 * state of the flags.
 */
[[nodiscard]]
constexpr role_quota ctf_quota(const team_view &t, const unsigned bots)
{
	role_quota q;
	unsigned left{bots};
	const auto take{[&left](unsigned &slot, const unsigned n) {
		const unsigned k{std::min(n, left)};
		slot += k;
		left -= k;
	}};
	/* Classic: the own flag lies away from home where the team knows it:
	 * with the touch rule the nearest bot returns it; without, a defender
	 * keeps the enemies off it (defend's place is the flag).  (In standard
	 * capture the flag a flag not carried always lies somewhere: its place
	 * is the defenders' home, as many as base_defenders says.)
	 */
	if (t.classic && t.own_flag == flag_state::lying && t.own_flag_place_known)
	{
		if (t.classic && t.touch_returns)
			take(q.retrieve, 1);
		else
			take(q.defend, 1);
	}
	/* The own flag is carried: hunt its carrier.  Half the bots (at
	 * least one); all of them when the team cannot score before it is
	 * back (Classic, "own flag home to score", the team carries the
	 * other flag already).
	 */
	if (t.own_flag == flag_state::carried)
	{
		const bool blocked{t.classic && t.home_to_score && t.team_carries};
		take(q.hunt, blocked ? left : std::max(1u, (left + 1) / 2));
	}
	/* The team carries the enemy flag: an escort from three players (two
	 * from five), the rest defend: there is nothing left to attack.
	 */
	if (t.team_carries)
	{
		take(q.escort, t.team_size >= 5 ? 2 : t.team_size >= 3 ? 1 : 0);
		take(q.defend, left);
		return q;
	}
	/* Defenders, when the own flag is at home (or lies at its start
	 * place without Classic): none while it is carried.  The bots keep
	 * one attacker among them, unless they are a single bot with two or
	 * more humans (who attack, as a rule): that bot defends.
	 */
	if (t.own_flag != flag_state::carried && q.defend == 0)
	{
		const unsigned d{base_defenders(t)};
		const bool humans_attack{t.team_size >= bots + 2};
		take(q.defend, left > 1 ? std::min(d, left - 1) : humans_attack ? std::min(d, left) : 0);
	}
	return q;
}

/* The cost of giving `role` to bot `m`: its distance to the role's place,
 * weighed by its style (Cautious defends, Aggressive attacks and hunts,
 * Collector likes carrying: attacks), less for its present role (no
 * flapping between two bots at about the same distance).
 */
constexpr double ROLE_KEEP_FACTOR{0.6};
/* A role's cost when the place is unknown: the bots are told apart by
 * their style.
 */
constexpr double ROLE_UNKNOWN_COST{1000};

[[nodiscard]]
constexpr double style_role_factor(const bot_style s, const mode_role r)
{
	switch (r)
	{
		case mode_role::defend:
			return s == bot_style::cautious ? 0.45 : s == bot_style::aggressive ? 1.7 : s == bot_style::collector ? 1.4 : 1;
		case mode_role::hunt:
			return s == bot_style::aggressive ? 0.6 : s == bot_style::cautious ? 1.3 : 1;
		case mode_role::escort:
			return s == bot_style::balanced ? 0.8 : s == bot_style::collector ? 1.2 : 1;
		case mode_role::attack:
			return s == bot_style::collector ? 0.6 : s == bot_style::aggressive ? 0.8 : s == bot_style::cautious ? 1.4 : 1;
		default:
			return 1;
	}
}

[[nodiscard]]
constexpr double role_cost(const member_view &m, const mode_role r)
{
	double d{ROLE_UNKNOWN_COST};
	switch (r)
	{
		case mode_role::retrieve:
			d = m.to_own_flag;
			break;
		case mode_role::hunt:
			d = m.to_enemy_carrier;
			break;
		case mode_role::escort:
			d = m.to_own_carrier;
			break;
		case mode_role::defend:
			d = m.to_own_home;
			break;
		case mode_role::attack:
			d = m.to_enemy_flag;
			break;
		default:
			break;
	}
	if (!(d < ROLE_FAR))
		d = ROLE_FAR;
	if (!m.alive)
		d = ROLE_FAR;
	/* A floor, so that the style and the hysteresis also tell apart two
	 * bots at the same place.
	 */
	double c{(std::max(d, 0.0) + 100) * style_role_factor(m.style, r)};
	if (m.previous == r)
		c *= ROLE_KEEP_FACTOR;
	return c;
}

/* The roles of a team's members (out[i] for members[i]): carriers carry
 * (or wait: Classic, "own flag home to score", the own flag away); the
 * other bots by the quota (ctf_quota) in the order retrieve, hunt,
 * escort, defend, each given to the bot it costs least (role_cost); the
 * rest attack.  Humans get none.
 */
inline void assign_ctf_roles(const team_view &t, const std::span<const member_view> members, const std::span<mode_role> out)
{
	const std::size_t n{std::min(members.size(), out.size())};
	std::array<bool, 16> free{};
	unsigned bots{0};
	for (std::size_t i = 0; i < n; ++i)
	{
		out[i] = mode_role::none;
		const auto &m{members[i]};
		if (!m.bot)
			continue;
		if (m.carrier)
		{
			out[i] = t.classic && t.home_to_score && t.own_flag != flag_state::home ? mode_role::wait : mode_role::carry;
			continue;
		}
		if (i < free.size())
		{
			free[i] = true;
			++bots;
		}
	}
	const auto q{ctf_quota(t, bots)};
	const auto give{[&](const mode_role r, unsigned count) {
		while (count--)
		{
			std::size_t best{n};
			double best_cost{0};
			for (std::size_t i = 0; i < n && i < free.size(); ++i)
			{
				if (!free[i])
					continue;
				const double c{role_cost(members[i], r)};
				if (best == n || c < best_cost)
				{
					best = i;
					best_cost = c;
				}
			}
			if (best == n)
				return;
			free[best] = false;
			out[best] = r;
		}
	}};
	give(mode_role::retrieve, q.retrieve);
	give(mode_role::hunt, q.hunt);
	give(mode_role::escort, q.escort);
	give(mode_role::defend, q.defend);
	for (std::size_t i = 0; i < n && i < free.size(); ++i)
		if (free[i])
			out[i] = mode_role::attack;
}

/* Hoard (net_v2_modes.h): n orbs scored at once are worth n(n+1)/2
 * points; a death drops them all and one more for the killer.  So a bot
 * gathers several before it scores, more the bolder its style, and
 * scores at once when the goal is on its way, when it is hurt, or when
 * it carries the most a ship holds.
 */
constexpr unsigned HOARD_ORBS_MAX{12};
/* The path within which a goal is "on the way": any orb is scored. */
constexpr double HOARD_GOAL_NEAR{220};
/* Hurt: below these shields, with a goal within HOARD_HURT_PATH. */
constexpr double HOARD_HURT_SHIELDS{45};
constexpr double HOARD_HURT_PATH{700};

[[nodiscard]]
constexpr unsigned hoard_score_threshold(const bot_style s)
{
	switch (s)
	{
		case bot_style::collector:
			return 6;
		case bot_style::aggressive:
			return 5;
		case bot_style::cautious:
			return 2;
		case bot_style::balanced:
		default:
			return 4;
	}
}

struct hoard_view
{
	unsigned orbs{};
	unsigned max_orbs{HOARD_ORBS_MAX};
	/* Path cost to the nearest goal (ROLE_FAR: none known). */
	double to_goal{ROLE_FAR};
	double shields{100};
	/* An enemy in sight or a hit taken lately. */
	bool threatened{};
	bot_style style{bot_style::balanced};
	/* Already on the way to score (hysteresis: it does not turn back
	 * at one orb less).
	 */
	bool scoring{};
};

[[nodiscard]]
constexpr bool hoard_should_score(const hoard_view &v)
{
	if (!v.orbs || !(v.to_goal < ROLE_FAR))
		return false;
	if (v.orbs >= v.max_orbs)
		return true;
	unsigned threshold{hoard_score_threshold(v.style)};
	/* A goal on the way: half the load is worth banking (two orbs at
	 * least: one orb is one point, worth more as a part of a load).
	 */
	if (v.to_goal < HOARD_GOAL_NEAR && v.orbs >= std::max(2u, threshold / 2))
		return true;
	if (v.shields < HOARD_HURT_SHIELDS && v.to_goal < HOARD_HURT_PATH)
		return true;
	/* Threatened with a load, the bot banks it a little earlier. */
	if (v.threatened && threshold > 2)
		--threshold;
	if (v.scoring && threshold > 1)
		--threshold;
	return v.orbs >= threshold;
}

/* The points of n orbs scored at once. */
[[nodiscard]]
constexpr unsigned hoard_points(const unsigned orbs)
{
	return orbs * (orbs + 1) / 2;
}

/* Team hoard: a teammate carrying this many orbs gets an escort. */
constexpr unsigned HOARD_ESCORT_ORBS{5};

struct hoard_member
{
	uint8_t pid{};
	bool bot{};
	bool alive{true};
	bool scoring{};
	unsigned orbs{};
	bot_style style{bot_style::balanced};
	mode_role previous{mode_role::none};
	/* Path cost to the teammate with the most orbs. */
	double to_loaded{ROLE_FAR};
};

/* The roles in hoard: score (hoard_should_score said so: `scoring`),
 * else collect; in team hoard with three or more players, the bot
 * nearest to a teammate with HOARD_ESCORT_ORBS or more escorts it.
 */
inline void assign_hoard_roles(const bool team, const unsigned team_size, const std::span<const hoard_member> members, const std::span<mode_role> out)
{
	const std::size_t n{std::min(members.size(), out.size())};
	bool loaded{false};
	for (std::size_t i = 0; i < n; ++i)
	{
		const auto &m{members[i]};
		out[i] = !m.bot ? mode_role::none : m.scoring ? mode_role::score : mode_role::collect;
		if (m.alive && m.orbs >= HOARD_ESCORT_ORBS)
			loaded = true;
	}
	if (!team || team_size < 3 || !loaded)
		return;
	std::size_t best{n};
	double best_cost{0};
	for (std::size_t i = 0; i < n; ++i)
	{
		const auto &m{members[i]};
		if (!m.bot || !m.alive || m.scoring || m.orbs >= HOARD_ESCORT_ORBS || !(m.to_loaded < ROLE_FAR))
			continue;
		double c{(m.to_loaded + 100) * style_role_factor(m.style, mode_role::escort)};
		if (m.previous == mode_role::escort)
			c *= ROLE_KEEP_FACTOR;
		if (best == n || c < best_cost)
		{
			best = i;
			best_cost = c;
		}
	}
	if (best < n)
		out[best] = mode_role::escort;
}

/* What a bot's role makes it want (objective_for). */
struct objective_view
{
	mode_role role{mode_role::none};
	/* The role's place is known (the enemy flag, the own flag, the
	 * carrier hunted or escorted, the own home, a goal), and the path
	 * cost to it (ROLE_FAR: none).
	 */
	bool place_known{};
	double path{ROLE_FAR};
	/* The carrier hunted is in sight (the fight takes it). */
	bool target_in_sight{};
	/* A hunter without a fresh sighting watches the enemy goal (`path`
	 * is to the goal): there, the fight decides.
	 */
	bool watching{};
	/* Hoard: orbs carried. */
	unsigned orbs{};
	bot_style style{bot_style::balanced};
};

struct objective
{
	/* The objective goal's utility (0: none; the bot's other goals
	 * decide).
	 */
	double utility{};
	/* Factors on the fight's utility (engage) and on the hunt of a
	 * target out of sight, and on the collections (powerups, power
	 * pickups, the power-up phase, the fuel centres).
	 */
	double engage{1};
	double hunt{1};
	double collect{1};
	/* Engaged, the bot flies the objective's path and shoots at what it
	 * sees (as when it collects), instead of the fight's keys.
	 */
	bool path_while_fighting{};
	/* The afterburner on the way (a carrier's run home). */
	bool burn{};
	/* The appetite for cover on the way (0: the bot's own). */
	double cover{};
};

/* The utilities are on the scale of goal_utility (bot_goals.h): roam
 * 0.2, a collection up to some 3-6, the engagement of a target in sight
 * 2 times its score (about 1-2.6 for one target, more for the enemy
 * carrier: carrier_priority), retreat 3-5.
 */
constexpr double OBJ_CARRY{6};
constexpr double OBJ_WAIT_FAR{4};
constexpr double OBJ_WAIT_THERE{0.3};
/* Within this path of its place a defender, a waiting carrier or an
 * escort is "there".
 */
constexpr double OBJ_THERE_PATH{160};
constexpr double OBJ_RETRIEVE{5};
constexpr double OBJ_HUNT{3};
constexpr double OBJ_WATCH_THERE{0.3};
constexpr double OBJ_DEFEND_FAR{2.6};
constexpr double OBJ_DEFEND_THERE{0.3};
constexpr double OBJ_ESCORT_FAR{3};
constexpr double OBJ_ESCORT_THERE{0.25};
constexpr double OBJ_ATTACK{2.4};
constexpr double OBJ_ATTACK_NEAR{3.4};
constexpr double OBJ_ATTACK_NEAR_PATH{300};
constexpr double OBJ_SCORE{4.5};
constexpr double OBJ_SCORE_PER_ORB{0.25};
/* A carrier's appetite for cover (bot_goals.h cover_appetite: a style's
 * own is 0-1).
 */
constexpr double CARRIER_COVER{1.2};

[[nodiscard]]
constexpr objective objective_for(const objective_view &v)
{
	objective o;
	const bool there{v.place_known && v.path < OBJ_THERE_PATH};
	switch (v.role)
	{
		case mode_role::carry:
			if (!v.place_known)
				break;
			o.utility = OBJ_CARRY;
			o.engage = 0.5;
			o.hunt = 0.2;
			o.collect = 0.25;
			o.path_while_fighting = true;
			o.burn = true;
			o.cover = CARRIER_COVER;
			break;
		case mode_role::wait:
			/* Near home under cover; there, it fights who comes (the
			 * own flag's carrier may come by) but hunts nobody far.
			 */
			if (!v.place_known)
				break;
			o.utility = there ? OBJ_WAIT_THERE : OBJ_WAIT_FAR;
			o.engage = there ? 1 : 0.6;
			o.hunt = 0.2;
			o.collect = there ? 0.6 : 0.3;
			o.path_while_fighting = !there;
			o.cover = CARRIER_COVER;
			break;
		case mode_role::retrieve:
			if (!v.place_known)
				break;
			o.utility = OBJ_RETRIEVE;
			o.engage = 0.7;
			o.hunt = 0.3;
			o.collect = 0.3;
			o.path_while_fighting = true;
			o.burn = v.path > OBJ_THERE_PATH;
			break;
		case mode_role::hunt:
			/* In sight, the carrier is the fight's target
			 * (carrier_priority); out of sight, its last known place or
			 * where it goes (the enemy goal).
			 */
			o.collect = 0.5;
			if (v.target_in_sight || !v.place_known)
				break;
			o.utility = v.watching && there ? OBJ_WATCH_THERE : OBJ_HUNT;
			o.burn = v.path > 2 * OBJ_THERE_PATH;
			break;
		case mode_role::defend:
			if (!v.place_known)
				break;
			o.utility = there ? OBJ_DEFEND_THERE : OBJ_DEFEND_FAR;
			o.hunt = 0.35;
			o.collect = 0.5;
			break;
		case mode_role::escort:
			if (!v.place_known)
				break;
			o.utility = there ? OBJ_ESCORT_THERE : OBJ_ESCORT_FAR;
			o.hunt = 0.5;
			o.collect = 0.5;
			break;
		case mode_role::attack:
			if (!v.place_known)
				break;
			o.utility = v.path < OBJ_ATTACK_NEAR_PATH ? OBJ_ATTACK_NEAR : OBJ_ATTACK;
			/* Collectors detour for powerups on the way; the others
			 * less.
			 */
			o.collect = v.style == bot_style::collector ? 0.9 : 0.7;
			break;
		case mode_role::score:
			if (!v.place_known)
				break;
			o.utility = OBJ_SCORE + OBJ_SCORE_PER_ORB * v.orbs;
			o.engage = 0.6;
			o.hunt = 0.3;
			o.collect = 0.3;
			o.path_while_fighting = true;
			o.burn = v.orbs >= 3;
			o.cover = v.orbs >= 3 ? CARRIER_COVER : 0;
			break;
		case mode_role::collect:
		case mode_role::none:
			break;
	}
	return o;
}

/* Hoard: the utility of going for a known orb at path cost `path`, for a
 * bot carrying `orbs` (each further orb is worth more points: the next
 * one adds orbs + 1).
 */
constexpr double ORB_VALUE{2.6};
constexpr double ORB_DISTANCE_SCALE{250};

[[nodiscard]]
constexpr double orb_utility(const unsigned orbs, const double path)
{
	const double value{ORB_VALUE + 0.15 * std::min(orbs, HOARD_ORBS_MAX)};
	return value * ORB_DISTANCE_SCALE / (ORB_DISTANCE_SCALE + std::max(path, 0.0));
}

/* The target weight (target_candidate::priority) of an enemy carrying
 * the bot's team's flag: every bot wants it, a hunter most.
 */
constexpr double CARRIER_PRIORITY{2.5};
constexpr double CARRIER_PRIORITY_HUNTER{4};

[[nodiscard]]
constexpr double carrier_priority(const mode_role own_role)
{
	return own_role == mode_role::hunt || own_role == mode_role::defend ? CARRIER_PRIORITY_HUNTER : CARRIER_PRIORITY;
}

/* Hoard: the target weight of an enemy carrying `orbs` orbs (its death
 * drops them and one more).
 */
[[nodiscard]]
constexpr double orb_carrier_priority(const unsigned orbs)
{
	return 1 + 0.2 * std::min(orbs, HOARD_ORBS_MAX);
}

}
