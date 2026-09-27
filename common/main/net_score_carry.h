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

}
