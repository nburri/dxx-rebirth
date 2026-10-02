/*
 * This file is part of the DXX-Rebirth project <https://github.com/dxx-rebirth/dxx-rebirth/>.
 * It is copyright by its individual contributors, as recorded in the
 * project's Git history.  See COPYING.txt at the top level for license
 * terms and a link to the Git history.
 */
/*
 * The reactor countdown in a network game
 * (Documentation/network-protocol-v2.md section 4.7, "Reactor countdown
 * as implemented").
 *
 * Every machine destroys the reactor (its own shot or MULTI_CONTROLCEN),
 * opens the exit and runs the countdown with its own frame time
 * (cntrlcen.cpp, do_countdown_frame).  A machine's countdown runs only
 * while its own player is `playing`; the exit trigger, too, works only
 * for a `playing` player.
 *
 * Two rules keep the countdown and the exit the same on every machine:
 *
 * - A kill during the countdown marks the victim "died in the mine"
 *   (D2, multi_compute_kill) on the other machines only.  The victim's
 *   own machine marks itself when its death sequence ends (DoPlayerDead):
 *   marking it at the kill made the host-judged death (PLAYER_KILLED,
 *   kill_local_ship) refuse to start, so the ship flew on with its
 *   countdown stopped and the exit closed to it.
 *
 * - The host's countdown is the game's: the level end status the host
 *   sends every second carries it, and a playing client sets its own
 *   timer to it when they differ by more than a second.  While the host
 *   does not play the level itself (it escaped, died in the mine or
 *   looks at the score screen), its value is the lowest of the clients'
 *   reports and only ever moves a client's countdown down.
 *
 * Standard library only (common/unittest/net_countdown.cpp).
 */

#pragma once

#include <cstdint>
#include <optional>

namespace dcx::net_v2 {

/* Whether a kill during the countdown marks the victim's slot "died in
 * the mine" at once (the remote victims), or only when its death sequence
 * ends (the local player).
 */
[[nodiscard]]
constexpr bool kill_marks_died_in_mine(const bool countdown_running, const bool victim_is_local)
{
	return countdown_running && !victim_is_local;
}

/* 16.16 fixed point, as the game's fix. */
inline constexpr std::int32_t COUNTDOWN_F1_0{0x10000};

/* The seconds the HUD shows for a timer (cntrlcen.cpp: f2i(timer + 7/8)). */
[[nodiscard]]
constexpr std::int32_t countdown_seconds_of_timer(const std::int32_t timer)
{
	return (timer + COUNTDOWN_F1_0 * 7 / 8) >> 16;
}

/* A timer that shows `seconds`, in the middle of that second. */
[[nodiscard]]
constexpr std::int32_t countdown_timer_of_seconds(const std::int32_t seconds)
{
	return seconds * COUNTDOWN_F1_0 - COUNTDOWN_F1_0 * 3 / 8;
}

/* The status byte of "no countdown" (Countdown_seconds_left -1 as u8). */
inline constexpr std::uint8_t COUNTDOWN_NONE{0xff};

/* A playing client's countdown timer after the host's level end status:
 * the new timer, or nothing to change.  `host_seconds` is the host's
 * Countdown_seconds_left (u8), `host_live` whether the host plays the
 * level itself (its countdown runs).  A difference of one second is the
 * status's age and is left alone.
 */
[[nodiscard]]
constexpr std::optional<std::int32_t> countdown_correction(const std::int32_t local_timer, const std::uint8_t host_seconds, const bool host_live)
{
	/* The mine blew up here already (the whiteout runs): too late. */
	if (host_seconds == COUNTDOWN_NONE || local_timer <= 0)
		return std::nullopt;
	const std::int32_t local_seconds{countdown_seconds_of_timer(local_timer)};
	const std::int32_t host{host_seconds};
	if (host < local_seconds - 1 || (host_live && host > local_seconds + 1))
		return countdown_timer_of_seconds(host);
	return std::nullopt;
}

/* The host's backstop: the countdown runs with the game's frame time,
 * which stands still on a machine whose countdown stopped (its player not
 * `playing`, or the score screen of a host that escaped and takes the
 * lowest of the clients' reports).  Once the real time since the reactor
 * died exceeds the countdown by this much, the host ends it (sends 0), so
 * every machine still in the level blows up and the level ends for all.
 */
inline constexpr std::int32_t COUNTDOWN_OVERDUE_SECONDS{5};

[[nodiscard]]
constexpr bool countdown_overdue(const std::int64_t elapsed_real_time, const std::int32_t total_seconds)
{
	return elapsed_real_time > static_cast<std::int64_t>(total_seconds + COUNTDOWN_OVERDUE_SECONDS) * COUNTDOWN_F1_0;
}

}
