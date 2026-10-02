/*
 * This file is part of the DXX-Rebirth project <https://github.com/dxx-rebirth/dxx-rebirth/>.
 * It is copyright by its individual contributors, as recorded in the
 * project's Git history.  See COPYING.txt at the top level for license
 * terms and a link to the Git history.
 */
/*
 * Multiplayer network protocol v2: the game-independent part of the game
 * modes the host decides (Documentation/network-protocol-v2.md, section
 * 6.8 and "Stage 6a: game modes" in section 8).
 *
 * Capture the flag: the host tests every ship (its own, its bots', the
 * clients' at their newest accepted position) against the goals; a
 * carrier of the other team's flag in its own team's goal scores
 * (`capture_due`), the host sends CAPTURE with the scores after it
 * (`capture_score`) and puts the flag back into the level at once.  Every
 * flag is always either in the level or carried by exactly one player
 * (`flag_census`).
 *
 * Capture the flag (Classic): a variant of capture the flag the host
 * chooses in the game setup.  Every flag starts in its own team's goal
 * (`home`, placed the same way on every machine, `choose_home`) and comes
 * back there after a capture; the host's options (`ctf_rules`) decide
 * whether a flag dropped by a dying carrier stays where it fell or
 * returns home at once, whether the own team returns its dropped flag by
 * touching it (`evaluate_flag_touch`), and whether a team scores only
 * while its own flag is at home (`evaluate_capture`).  A flag is at home
 * when it lies in a goal segment of its own team.
 *
 * Teams as the game numbers them: blue 0, red 1.  A flag is named by the
 * team it belongs to (the blue flag is POW_FLAG_BLUE, its home is the
 * blue goal); the other team takes it.
 *
 * Standard library only, so that the rules are tested outside the game
 * (common/unittest/net_v2_modes.cpp).  The game side is
 * similar/main/net_modes.cpp.
 */

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

#include "net_v2.h"
#include "net_v2_state.h"

namespace dcx {

namespace net_v2 {

constexpr std::uint8_t CTF_TEAM_BLUE{0};
constexpr std::uint8_t CTF_TEAM_RED{1};
constexpr std::size_t CTF_TEAMS{2};

[[nodiscard]]
constexpr std::uint8_t other_team(const std::uint8_t team)
{
	return team == CTF_TEAM_BLUE ? CTF_TEAM_RED : CTF_TEAM_BLUE;
}

/* What the host knows of one ship when it tests the goals. */
struct capture_check
{
	/* In the game, alive (the host has not decided its death), its ship
	 * a ship.
	 */
	bool alive{};
	/* The ship's team. */
	std::uint8_t team{};
	/* It carries a flag (always the other team's: a team never takes its
	 * own).
	 */
	bool carries_flag{};
	/* The team whose goal the ship's segment is, if it is a goal. */
	std::optional<std::uint8_t> goal{};
};

/* The capture rule of fuelcen_check_for_goal: a living carrier in its own
 * team's goal scores.
 */
[[nodiscard]]
constexpr bool capture_due(const capture_check &c)
{
	return c.alive && c.carries_flag && c.goal && *c.goal == c.team;
}

/* The scores a capture changes, as multi_do_capture_bonus changed them:
 * the team's score, the carrier's kills and its count towards the kill
 * goal, five each.
 */
struct capture_scores
{
	std::int16_t team_score{};
	std::int16_t kills{};
	std::int16_t kill_goal_count{};
};

constexpr std::int16_t CAPTURE_POINTS{5};

[[nodiscard]]
constexpr capture_scores capture_score(const capture_scores &before)
{
	const auto add{[](const std::int16_t v) {
		return static_cast<std::int16_t>(v + CAPTURE_POINTS);
	}};
	return {add(before.team_score), add(before.kills), add(before.kill_goal_count)};
}

/* Whether the kill goal (`kill_goal` kills of the game setup, 0 = none)
 * is reached: a team game counts the team's score, else the player's
 * count (multi_do_capture_bonus counted the player's in either case; the
 * host now decides it as for a kill, multi_player_killed).
 */
[[nodiscard]]
constexpr bool kill_goal_reached(const unsigned kill_goal, const bool team_game, const capture_scores &after)
{
	if (!kill_goal)
		return false;
	const int goal{static_cast<int>(kill_goal) * 5};
	return (team_game ? after.team_score : after.kill_goal_count) >= goal;
}

/* Every flag is in the level or carried, never both, never neither, and
 * there is exactly as many of each as the level started with (one).
 * The host counts both every frame of a test run (the bot arena) and
 * logs a broken count in every game.
 */
struct flag_census
{
	std::array<std::uint8_t, CTF_TEAMS> in_level{};
	std::array<std::uint8_t, CTF_TEAMS> carried{};
	[[nodiscard]]
	constexpr unsigned total(const std::uint8_t flag) const
	{
		return flag < CTF_TEAMS ? unsigned{in_level[flag]} + carried[flag] : 0;
	}
	[[nodiscard]]
	constexpr bool holds(const std::array<std::uint8_t, CTF_TEAMS> &expected) const
	{
		for (std::size_t i = 0; i < CTF_TEAMS; ++i)
			if (total(static_cast<std::uint8_t>(i)) != expected[i])
				return false;
		return true;
	}
};

/* Capture the flag (Classic): the host's options, as Netgame carries them
 * (CtfClassicFlags, GAME_SETTINGS, the .ngp file).
 */
constexpr std::uint8_t CTF_RULE_CLASSIC{1 << 0};
constexpr std::uint8_t CTF_RULE_DROPPED_RETURNS{1 << 1};
constexpr std::uint8_t CTF_RULE_TOUCH_RETURNS{1 << 2};
constexpr std::uint8_t CTF_RULE_HOME_TO_SCORE{1 << 3};
constexpr std::uint8_t CTF_RULES_KNOWN{CTF_RULE_CLASSIC | CTF_RULE_DROPPED_RETURNS | CTF_RULE_TOUCH_RETURNS | CTF_RULE_HOME_TO_SCORE};
/* The options a new game starts with: a dropped flag stays, the own team
 * returns it, a team scores only with its own flag at home.
 */
constexpr std::uint8_t CTF_RULES_DEFAULT{CTF_RULE_TOUCH_RETURNS | CTF_RULE_HOME_TO_SCORE};

struct ctf_rules
{
	/* The Classic variant at all; without it the other options do
	 * nothing (standard capture the flag).
	 */
	bool classic{};
	/* A flag a dying (or departing) carrier drops returns home at once,
	 * instead of staying where it fell.
	 */
	bool dropped_returns{};
	/* A player who touches its own team's flag away from home returns it
	 * home.
	 */
	bool touch_returns{};
	/* A team scores only while its own flag is at home. */
	bool home_to_score{};
	[[nodiscard]]
	static constexpr ctf_rules from_bits(const std::uint8_t bits)
	{
		const bool c{(bits & CTF_RULE_CLASSIC) != 0};
		return {c, c && (bits & CTF_RULE_DROPPED_RETURNS), c && (bits & CTF_RULE_TOUCH_RETURNS), c && (bits & CTF_RULE_HOME_TO_SCORE)};
	}
};

/* A player touches a flag lying in the level. */
enum class flag_touch : std::uint8_t
{
	/* Nothing happens (its own flag at home, its own flag without the
	 * touch rule, or the other team's flag while it carries one).
	 */
	none,
	/* It takes the other team's flag. */
	take,
	/* Its own team's flag, away from home, goes home. */
	return_home,
};

[[nodiscard]]
constexpr flag_touch evaluate_flag_touch(const ctf_rules &r, const std::uint8_t player_team, const std::uint8_t flag_team, const bool flag_at_home, const bool carries_flag)
{
	if (player_team != flag_team)
		return carries_flag ? flag_touch::none : flag_touch::take;
	if (r.touch_returns && !flag_at_home)
		return flag_touch::return_home;
	return flag_touch::none;
}

/* A carrier in its own goal (capture_due). */
enum class capture_verdict : std::uint8_t
{
	none,
	score,
	/* Classic with home_to_score: its own team's flag is not at home. */
	own_flag_away,
};

[[nodiscard]]
constexpr capture_verdict evaluate_capture(const ctf_rules &r, const capture_check &c, const bool own_flag_at_home)
{
	if (!capture_due(c))
		return capture_verdict::none;
	if (r.home_to_score && !own_flag_at_home)
		return capture_verdict::own_flag_away;
	return capture_verdict::score;
}

/* Where the flag of a dying or departing carrier goes. */
[[nodiscard]]
constexpr bool dropped_flag_goes_home(const ctf_rules &r)
{
	return r.dropped_returns;
}

/* A flag lying away from home that nobody returns: with "score only with
 * the own flag home" and neither return rule, two dropped flags would
 * stop both teams from scoring for the rest of the level.  The host then
 * returns a flag that lay away from home this long (seconds).
 */
constexpr unsigned CTF_IDLE_RETURN_SECONDS{30};

[[nodiscard]]
constexpr bool idle_flag_returns(const ctf_rules &r)
{
	return r.home_to_score && !r.touch_returns && !r.dropped_returns;
}

/* Where a captured (or lost, or missing) flag reappears: at home in the
 * Classic variant, else at a random place.
 */
[[nodiscard]]
constexpr bool flag_respawns_home(const ctf_rules &r)
{
	return r.classic;
}

/* The home of a team's flag: one of the team's goal segments, the same
 * on every machine.  A team may have several (the goal of a level is
 * often a room of several segments): the largest (the sum of the
 * squared distances of its corners from its center, in game units
 * squared, as a measure of its size), the lowest segment number of equal
 * ones.
 */
struct goal_segment
{
	std::uint16_t segnum{};
	std::uint64_t size{};
};

[[nodiscard]]
constexpr std::optional<std::uint16_t> choose_home(const std::span<const goal_segment> goals)
{
	std::optional<goal_segment> best;
	for (const auto &g : goals)
		if (!best || g.size > best->size || (g.size == best->size && g.segnum < best->segnum))
			best = g;
	if (!best)
		return std::nullopt;
	return best->segnum;
}

/* CTF_NOTICE (0x4a): host to all, or to one player. */
enum class ctf_notice_kind : std::uint8_t
{
	/* The flag of team `team` went home: touched by player `pid`
	 * (NET_V2_PLAYER_ID_NONE: on its own, a carrier's death or
	 * departure).
	 */
	returned,
	/* Player `pid` of team `team` is in its goal with the other flag,
	 * but its own flag is not at home (to that player only).
	 */
	own_flag_away,
};

struct ctf_notice_msg
{
	static constexpr std::size_t SIZE{3};
	ctf_notice_kind kind{};
	std::uint8_t team{};
	std::uint8_t pid{NET_V2_PLAYER_ID_NONE};
	void write(std::span<std::uint8_t, SIZE> buf) const
	{
		buf[0] = static_cast<std::uint8_t>(kind);
		buf[1] = team;
		buf[2] = pid;
	}
	[[nodiscard]]
	static std::optional<ctf_notice_msg> read(const std::span<const std::uint8_t> buf)
	{
		if (buf.size() != SIZE || buf[0] > static_cast<std::uint8_t>(ctf_notice_kind::own_flag_away) || buf[1] >= CTF_TEAMS)
			return std::nullopt;
		ctf_notice_msg m;
		m.kind = static_cast<ctf_notice_kind>(buf[0]);
		m.team = buf[1];
		m.pid = buf[2];
		if (m.pid >= NET_V2_MAX_PLAYERS && (m.pid != NET_V2_PLAYER_ID_NONE || m.kind == ctf_notice_kind::own_flag_away))
			return std::nullopt;
		return m;
	}
};

/* CAPTURE (0x39): host to all.  Player `pid` of team `team` brought the
 * flag of team `flag` home; the scores are the host's after the capture,
 * which every machine takes.
 */
struct capture_msg
{
	static constexpr std::size_t SIZE{9};
	std::uint8_t pid{};
	std::uint8_t team{};
	std::uint8_t flag{};
	capture_scores scores{};
	void write(std::span<std::uint8_t, SIZE> buf) const
	{
		buf[0] = pid;
		buf[1] = team;
		buf[2] = flag;
		net_put_le16(&buf[3], static_cast<std::uint16_t>(scores.team_score));
		net_put_le16(&buf[5], static_cast<std::uint16_t>(scores.kills));
		net_put_le16(&buf[7], static_cast<std::uint16_t>(scores.kill_goal_count));
	}
	[[nodiscard]]
	static std::optional<capture_msg> read(const std::span<const std::uint8_t> buf)
	{
		if (buf.size() != SIZE)
			return std::nullopt;
		capture_msg m;
		m.pid = buf[0];
		m.team = buf[1];
		m.flag = buf[2];
		m.scores.team_score = static_cast<std::int16_t>(net_get_le16(&buf[3]));
		m.scores.kills = static_cast<std::int16_t>(net_get_le16(&buf[5]));
		m.scores.kill_goal_count = static_cast<std::int16_t>(net_get_le16(&buf[7]));
		if (m.pid >= NET_V2_MAX_PLAYERS || m.team >= CTF_TEAMS || m.flag != other_team(m.team))
			return std::nullopt;
		return m;
	}
};

}

}
