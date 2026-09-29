/*
 * This file is part of the DXX-Rebirth project <https://github.com/dxx-rebirth/dxx-rebirth/>.
 * It is copyright by its individual contributors, as recorded in the
 * project's Git history.  See COPYING.txt at the top level for license
 * terms and a link to the Git history.
 */
/*
 * The host's bot setup in the pilot's netgame profile (<pilot>.ngp,
 * Documentation/multiplayer-bots.md sections 6.5 and 9.7): the lines
 * written and their parser, game-independent (common/unittest/
 * bot_presets.cpp).  The file is key=value, unknown keys are ignored and
 * lines are under 50 characters:
 *
 *	BotCount=3
 *	BotDefault=2,0
 *	BotReplace=1
 *	Bot0=ravager,2,0,0
 *	Bot1=havoc,3,1,0
 *	Bot2=sparky,1,2,0
 *
 * A bot line is name, skill 0-4, style 0-3, team (0 auto, 1 blue, 2 red);
 * the three numbers are read from the right, so a name may hold a comma.
 * A file without bot lines (older builds) leaves the setup as it is.
 */

#pragma once

#include <algorithm>
#include <array>
#include <charconv>
#include <cstddef>
#include <cstdio>
#include <optional>
#include <span>
#include <string_view>

#include "bot_brain.h"

namespace dcx::bot {

constexpr std::size_t BOT_PROFILE_MAX_BOTS{7};
constexpr std::size_t BOT_PROFILE_NAME_LEN{8};
/* The .ngp reader's line buffer (PHYSFSX_gets_line_t<50>). */
constexpr std::size_t BOT_PROFILE_LINE_SIZE{50};

struct profile_entry
{
	/* NUL-terminated; empty: the game gives it the next built-in name. */
	std::array<char, BOT_PROFILE_NAME_LEN + 1> name{};
	bot_skill skill{BOT_DEFAULT_SKILL};
	bot_style style{bot_style::balanced};
	bot_team team{bot_team::automatic};
	constexpr bool operator==(const profile_entry &) const = default;
};

struct bot_profile
{
	unsigned count{};
	bot_skill default_skill{BOT_DEFAULT_SKILL};
	bot_style default_style{bot_style::balanced};
	bool replace{true};
	std::array<profile_entry, BOT_PROFILE_MAX_BOTS> bots{};
	constexpr bool operator==(const bot_profile &) const = default;
};

using profile_line = std::array<char, BOT_PROFILE_LINE_SIZE>;

/* The lines of `p` (without newlines) into `out`; returns how many (0
 * if `out` is too small: it needs 3 + p.count lines).
 */
inline std::size_t format_profile(const bot_profile &p, const std::span<profile_line> out)
{
	const unsigned count{std::min<unsigned>(p.count, BOT_PROFILE_MAX_BOTS)};
	if (out.size() < 3 + count)
		return 0;
	std::snprintf(out[0].data(), BOT_PROFILE_LINE_SIZE, "BotCount=%u", count);
	std::snprintf(out[1].data(), BOT_PROFILE_LINE_SIZE, "BotDefault=%u,%u", static_cast<unsigned>(p.default_skill), static_cast<unsigned>(p.default_style));
	std::snprintf(out[2].data(), BOT_PROFILE_LINE_SIZE, "BotReplace=%u", p.replace ? 1u : 0u);
	for (unsigned i = 0; i < count; ++i)
	{
		const auto &b{p.bots[i]};
		std::snprintf(out[3 + i].data(), BOT_PROFILE_LINE_SIZE, "Bot%u=%.8s,%u,%u,%u", i, b.name.data(), static_cast<unsigned>(b.skill), static_cast<unsigned>(b.style), static_cast<unsigned>(b.team));
	}
	return 3 + count;
}

namespace detail {

[[nodiscard]]
inline std::optional<unsigned> parse_unsigned(std::string_view v)
{
	while (!v.empty() && (v.back() == ' ' || v.back() == '\r' || v.back() == '\n' || v.back() == '\t'))
		v.remove_suffix(1);
	while (!v.empty() && (v.front() == ' ' || v.front() == '\t'))
		v.remove_prefix(1);
	unsigned r{};
	const auto [ptr, ec]{std::from_chars(v.data(), v.data() + v.size(), r)};
	if (ec != std::errc{} || ptr != v.data() + v.size())
		return std::nullopt;
	return r;
}

/* The value `v` as a skill, a style or a team (out of range: nothing). */
template <typename E, unsigned N>
[[nodiscard]]
inline std::optional<E> parse_enum(const std::string_view v)
{
	const auto u{parse_unsigned(v)};
	if (!u || *u >= N)
		return std::nullopt;
	return E{static_cast<uint8_t>(*u)};
}

/* Split off the last comma field of `v`. */
[[nodiscard]]
inline std::optional<std::string_view> pop_field(std::string_view &v)
{
	const auto c{v.rfind(',')};
	if (c == std::string_view::npos)
		return std::nullopt;
	const auto f{v.substr(c + 1)};
	v = v.substr(0, c);
	return f;
}

}

/* Reads the bot lines of a profile, line by line; other keys are left
 * to the caller.  Bad values are ignored (the default stays).
 */
class profile_reader
{
	bot_profile m_profile;
	/* Bot line `i` was read and accepted. */
	std::array<bool, BOT_PROFILE_MAX_BOTS> m_read{};
	bool m_seen{};
public:
	/* Line `key`=`value`: true if it is a bot key (taken, or ignored as
	 * bad).
	 */
	bool parse(const std::string_view key, const std::string_view value)
	{
		if (!key.starts_with("Bot"))
			return false;
		const auto rest{key.substr(3)};
		if (rest == "Count")
		{
			m_seen = true;
			if (const auto u{detail::parse_unsigned(value)})
				m_profile.count = std::min<unsigned>(*u, BOT_PROFILE_MAX_BOTS);
			return true;
		}
		if (rest == "Default")
		{
			m_seen = true;
			auto v{value};
			const auto style_field{detail::pop_field(v)};
			if (!style_field)
				return true;
			if (const auto k{detail::parse_enum<bot_skill, BOT_SKILL_COUNT>(v)})
				m_profile.default_skill = *k;
			if (const auto s{detail::parse_enum<bot_style, BOT_STYLE_COUNT>(*style_field)})
				m_profile.default_style = *s;
			return true;
		}
		if (rest == "Replace")
		{
			m_seen = true;
			if (const auto u{detail::parse_unsigned(value)})
				m_profile.replace = *u != 0;
			return true;
		}
		const auto index{detail::parse_unsigned(rest)};
		if (!index)
			return false;
		m_seen = true;
		if (*index >= BOT_PROFILE_MAX_BOTS)
			return true;
		auto v{value};
		const auto team_f{detail::pop_field(v)};
		const auto style_f{team_f ? detail::pop_field(v) : std::nullopt};
		const auto skill_f{style_f ? detail::pop_field(v) : std::nullopt};
		if (!skill_f)
			return true;
		const auto skill{detail::parse_enum<bot_skill, BOT_SKILL_COUNT>(*skill_f)};
		const auto style{detail::parse_enum<bot_style, BOT_STYLE_COUNT>(*style_f)};
		const auto team{detail::parse_enum<bot_team, BOT_TEAM_COUNT>(*team_f)};
		if (!skill || !style || !team)
			return true;
		profile_entry e{};
		e.skill = *skill;
		e.style = *style;
		e.team = *team;
		const auto len{std::min(v.size(), BOT_PROFILE_NAME_LEN)};
		std::ranges::copy(v.substr(0, len), e.name.begin());
		m_profile.bots[*index] = e;
		m_read[*index] = true;
		return true;
	}
	/* The file had bot lines. */
	[[nodiscard]]
	bool seen() const
	{
		return m_seen;
	}
	/* The setup read; the lines beyond the count are dropped.  A bot
	 * below the count whose line is missing or bad gets the file's
	 * default skill and style (whatever order the lines came in) and the
	 * next built-in name.
	 */
	[[nodiscard]]
	bot_profile result() const
	{
		auto p{m_profile};
		for (std::size_t i = 0; i < p.bots.size(); ++i)
		{
			if (i >= p.count)
				p.bots[i] = {};
			else if (!m_read[i])
				p.bots[i] = {.skill = p.default_skill, .style = p.default_style};
		}
		return p;
	}
};


/* Section 9.8: the BOT marker in the HUD's kill list.  The exp-16
 * playtest: "Bots do not show up as 'BOT' in scoring for me."  The kill
 * list marked a bot only in its ping column, shown with the ping display
 * option alone; now a bot's line always carries the marker after its
 * name (the ping column keeps its own), on the host and the clients
 * alike (player_is_bot).  The team view lists teams, not players.
 */
inline constexpr char BOT_KILL_LIST_MARKER[]{" BOT"};
inline constexpr char BOT_SCORE_MARKER[]{"BOT"};

[[nodiscard]]
constexpr bool kill_list_marks_bot(const bool is_bot, const bool team_view)
{
	return is_bot && !team_view;
}

/* The room the marker takes from the name's column (the name is cut to
 * what is left, as the kill list cuts a long name).
 */
[[nodiscard]]
constexpr unsigned long kill_list_marker_room(const bool marked, const unsigned long marker_width)
{
	return marked ? marker_width : 0;
}

}
