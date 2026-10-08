/*
 * This file is part of the DXX-Rebirth project <https://github.com/dxx-rebirth/dxx-rebirth/>.
 * It is copyright by its individual contributors, as recorded in the
 * project's Git history.  See COPYING.txt at the top level for license
 * terms and a link to the Git history.
 */
/*
 * Capture the flag: the cues every machine plays for the flag events
 * the host decides (net_modes.cpp, data/sounds/README.md), told from
 * the listener's side.  The cues are this fork's own sounds
 * (data/sounds, CC0), loaded into sound ids the game's data leaves free;
 * after a cue the original game's voice plays where it has one ("Blue
 * team has the flag", "You have scored").  Depends on the standard
 * library only, so that the unit tests can use it without the game.
 */

#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace dcx {

namespace ctf_cues {

/* Teams as the game numbers them (net_v2_modes.h CTF_TEAM_*). */
constexpr std::uint8_t TEAM_BLUE{0};
constexpr std::uint8_t TEAM_RED{1};

enum class event : std::uint8_t
{
	/* A player takes the flag (of the other team). */
	flag_taken,
	/* A carrier loses the flag where it is: it died (and the flag does
	 * not go home by the rules) or dropped it by hand.
	 */
	flag_dropped,
	/* The flag went home: touched by its own team, on its own after
	 * lying away (30 s), or at once as a dying carrier's.
	 */
	flag_returned,
	/* The carrier of the flag scores in its own goal. */
	capture,
};

/* "own" is the flag of the listener's team, "enemy" the other one; "we"
 * the listener's team.  Values are indices into SOUND_IDS and FILES.
 */
enum class cue : std::uint8_t
{
	own_flag_taken,
	enemy_flag_taken,
	own_flag_dropped,
	enemy_flag_dropped,
	own_flag_returned,
	enemy_flag_returned,
	we_scored,
	they_scored,
};
constexpr std::size_t CUES{8};

/* The original game's voice after the cue (sounds.h SOUND_HUD_*). */
enum class voice : std::uint8_t
{
	none,
	you_got_flag,		/* SOUND_HUD_YOU_GOT_FLAG */
	blue_got_flag,		/* SOUND_HUD_BLUE_GOT_FLAG */
	red_got_flag,		/* SOUND_HUD_RED_GOT_FLAG */
	you_scored,		/* SOUND_HUD_YOU_GOT_GOAL */
	blue_scored,		/* SOUND_HUD_BLUE_GOT_GOAL */
	red_scored,		/* SOUND_HUD_RED_GOT_GOAL */
};

struct announcement
{
	cue sound{};
	voice then{voice::none};
	constexpr bool operator==(const announcement &) const = default;
};

[[nodiscard]]
constexpr std::uint8_t other_team(const std::uint8_t team)
{
	return team == TEAM_BLUE ? TEAM_RED : TEAM_BLUE;
}

/* What the listener hears for `e` concerning the flag of `flag_team`
 * (the flag taken, dropped, returned, or captured), the listener being
 * of `listener_team`; `listener_acted`: the listener took or captured
 * the flag itself.
 */
[[nodiscard]]
constexpr announcement announce(const event e, const std::uint8_t flag_team, const std::uint8_t listener_team, const bool listener_acted)
{
	const bool own{flag_team == listener_team};
	/* The team that took or captured the flag. */
	const bool actor_blue{other_team(flag_team) == TEAM_BLUE};
	switch (e)
	{
		case event::flag_taken:
			return {own ? cue::own_flag_taken : cue::enemy_flag_taken, listener_acted ? voice::you_got_flag : actor_blue ? voice::blue_got_flag : voice::red_got_flag};
		case event::flag_dropped:
			return {own ? cue::own_flag_dropped : cue::enemy_flag_dropped, voice::none};
		case event::flag_returned:
			return {own ? cue::own_flag_returned : cue::enemy_flag_returned, voice::none};
		case event::capture:
			return {own ? cue::they_scored : cue::we_scored, listener_acted ? voice::you_scored : actor_blue ? voice::blue_scored : voice::red_scored};
	}
	return {own ? cue::own_flag_returned : cue::enemy_flag_returned, voice::none};
}

/* The sound ids (sounds.h SOUND_CTF_*) the cues take: ids DESCENT2.HAM
 * maps to no sound (the hoard's are 84 to 87).  A mission whose own HAM
 * uses one of them keeps it; that cue then plays SOUND_HUD_MESSAGE.
 */
constexpr std::array<std::uint8_t, CUES> SOUND_IDS{{252, 253, 243, 239, 234, 229, 228, 218}};

/* The files (data/sounds, the release packages' sounds/ folder): mono,
 * 22050 Hz (or any rate the taunt decoder reads), at the loudness of the
 * game's voices.
 */
constexpr std::array<const char *, CUES> FILES{{
	"sounds/ctf-own-flag-taken.wav",
	"sounds/ctf-enemy-flag-taken.wav",
	"sounds/ctf-own-flag-dropped.wav",
	"sounds/ctf-enemy-flag-dropped.wav",
	"sounds/ctf-own-flag-returned.wav",
	"sounds/ctf-enemy-flag-returned.wav",
	"sounds/ctf-we-scored.wav",
	"sounds/ctf-they-scored.wav",
}};
/* A cue longer than this is cut (they last 0.6 to 1.5 s). */
constexpr std::size_t MAX_SECONDS{3};
/* The largest file read. */
constexpr std::size_t MAX_FILE_SIZE{1u << 20};

/* A cue at 22050 Hz (-1 to 1) as the game keeps its sounds: unsigned
 * 8-bit, 128 the silence, at 22050 Hz, or at 11025 Hz (the average of
 * each pair) when the game runs with the 11 kHz sounds; cut to
 * MAX_SECONDS.
 */
[[nodiscard]]
inline std::vector<std::uint8_t> to_game_sound(const std::span<const float> s, const bool half_rate)
{
	const std::size_t step{half_rate ? 2u : 1u};
	const std::size_t n{std::min(s.size(), MAX_SECONDS * 22050) / step};
	std::vector<std::uint8_t> out(n);
	for (std::size_t i{}; i != n; ++i)
	{
		float v{};
		for (std::size_t k{}; k != step; ++k)
			v += s[i * step + k];
		v /= static_cast<float>(step);
		if (!std::isfinite(v))
			v = 0;
		const long q{std::lround(v * 128.0f) + 128};
		out[i] = static_cast<std::uint8_t>(q < 0 ? 0 : q > 255 ? 255 : q);
	}
	return out;
}

}

}
