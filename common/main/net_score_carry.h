/*
 * This file is part of the DXX-Rebirth project <https://github.com/dxx-rebirth/dxx-rebirth/>.
 * It is copyright by its individual contributors, as recorded in the
 * project's Git history.  See COPYING.txt at the top level for license
 * terms and a link to the Git history.
 */
/*
 * Carrying the players' scores across a level load
 * (Documentation/network-protocol-v2.md section 4.3, "Stage 1 as
 * implemented", level start).
 *
 * A netgame's kills, deaths, kill goal count and score live in each
 * player's ship object (object::ctype.player_info) and persist for the
 * whole game.  Loading the next level replaces the object array: the
 * level file places new player objects, possibly at other object
 * numbers, and never writes their player_info.  Only the local player's
 * player_info was preserved; every other slot (clients, the host's bots)
 * kept whatever the previous level had left in that memory: its own
 * values when the level has its player starts at the same object numbers,
 * another slot's or another object's bytes otherwise.  The host then built
 * the level start scores (GAME_SETTINGS/SNAPSHOT_GAME) from those objects,
 * and every machine adopted them.
 *
 * The scores belong to the slot, not to the object: capture them per slot
 * before the load and write them into each slot's new object after it.
 * Standard library only (common/unittest/net_score_carry.cpp).
 */

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace dcx::net_v2 {

/* The fields of player_info that persist across the levels of a game. */
struct carried_scores
{
	int16_t kills{};
	int16_t killed{};
	int16_t kill_goal{};
	int32_t score{};
	int32_t last_score{};
	constexpr bool operator==(const carried_scores &) const = default;
};

template <typename player_info_type>
[[nodiscard]]
constexpr carried_scores capture_scores(const player_info_type &pi)
{
	return {
		.kills = pi.net_kills_total,
		.killed = pi.net_killed_total,
		.kill_goal = pi.KillGoalCount,
		.score = pi.mission.score,
		.last_score = pi.mission.last_score,
	};
}

template <typename player_info_type>
constexpr void restore_scores(player_info_type &pi, const carried_scores &s)
{
	pi.net_kills_total = s.kills;
	pi.net_killed_total = s.killed;
	pi.KillGoalCount = s.kill_goal;
	pi.mission.score = s.score;
	pi.mission.last_score = s.last_score;
}

/* `info(slot)` returns a pointer to the player_info of the slot's ship, or
 * nullptr when the slot has no ship object.  A slot without a ship before
 * the load carries zero scores.
 */
template <std::size_t N, typename info_of_slot>
[[nodiscard]]
constexpr std::array<carried_scores, N> capture_all_scores(info_of_slot &&info)
{
	std::array<carried_scores, N> r{};
	for (std::size_t i = 0; i < N; ++i)
		if (const auto pi{info(i)})
			r[i] = capture_scores(*pi);
	return r;
}

template <std::size_t N, typename info_of_slot>
constexpr void restore_all_scores(const std::array<carried_scores, N> &scores, info_of_slot &&info)
{
	for (std::size_t i = 0; i < N; ++i)
		if (const auto pi{info(i)})
			restore_scores(*pi, scores[i]);
}

/* The level end reports (Documentation/network-protocol-v2.md section 4.7,
 * stage 1: LEGACY_ENDLEVEL_HOST and LEGACY_ENDLEVEL_CLIENT, once per
 * second while the reactor countdown runs and at the score screen).
 *
 * The host computes every kill (a client's own death goes to the host as
 * MULTI_KILL_CLIENT, the host credits it and relays MULTI_KILL_HOST), so
 * the host's kills, deaths and kill matrix are the game's.  A client's
 * report holds the client's view of its own row, which lags the host by
 * the kills still on their way to it: a report sent before the relay of
 * a kill arrives after the host credited it.  The host adopted that row,
 * reverting the kill, and sent the reverted scores to everybody with the
 * next level start.  So the host takes only the connection state and the
 * countdown from a client's report, and a client takes the host's scores
 * for every other slot (its own row arrives with the next level start).
 *
 * A report belongs to the end of the level being played: once the next
 * level has started (playing, countdown not running) a report still in
 * flight describes the previous level and is dropped.
 */
struct endlevel_report_use
{
	bool status;	/* connection state and countdown */
	bool scores;	/* kills, deaths and kill matrix */
	constexpr bool operator==(const endlevel_report_use &) const = default;
};

[[nodiscard]]
constexpr endlevel_report_use endlevel_report_applies(const bool receiver_is_host, const bool level_ending)
{
	if (!level_ending)
		return {};
	return {
		.status = true,
		.scores = !receiver_is_host,
	};
}

}
