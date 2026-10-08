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
 *
 * Section 9.13: a bot (and the default) that flies a style profile has
 * a line with the profile's name besides, its style number being the
 * profile's base style (what the bot flies when the file is gone, and
 * what an older build reads):
 *
 *	BotDefaultStyle=EC style
 *	BotStyle1=EC style
 *
 * Section 9.20: a bot whose ship is not Random has a line with it, the
 * ship's name and the start of its SHA-256 (bot_ship.h), or `pyro`:
 *
 *	BotShip0=longhorn,3fa94c01d2e7
 *	BotShip2=pyro
 */

#pragma once

#include <algorithm>
#include <array>
#include <charconv>
#include <cstddef>
#include <cstdio>
#include <optional>
#include <span>
#include <string>
#include <string_view>

#include "bot_brain.h"
#include "bot_style_library.h"
#include "bot_ship.h"

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
	/* Section 9.13: the style profile it flies (empty: `style`). */
	style_name profile{};
	/* Section 9.20: the ship it flies. */
	ship_choice ship{};
	constexpr bool operator==(const profile_entry &) const = default;
};

struct bot_profile
{
	unsigned count{};
	bot_skill default_skill{BOT_DEFAULT_SKILL};
	bot_style default_style{bot_style::balanced};
	bool replace{true};
	/* Bots sound their horn after some kills (Documentation/taunts.md). */
	bool taunt{false};
	std::array<profile_entry, BOT_PROFILE_MAX_BOTS> bots{};
	/* Section 9.13: the default's style profile (empty: default_style). */
	style_name default_profile{};
	constexpr bool operator==(const bot_profile &) const = default;
};

/* The most lines format_profile writes. */
constexpr std::size_t BOT_PROFILE_MAX_LINES{5 + 3 * BOT_PROFILE_MAX_BOTS};

using profile_line = std::array<char, BOT_PROFILE_LINE_SIZE>;

/* The lines of `p` (without newlines) into `out`; returns how many (0
 * if `out` is too small: it needs BOT_PROFILE_MAX_LINES lines, or 3 +
 * p.count and one per style profile and per ship that is not Random).
 */
inline std::size_t format_profile(const bot_profile &p, const std::span<profile_line> out)
{
	const unsigned count{std::min<unsigned>(p.count, BOT_PROFILE_MAX_BOTS)};
	std::size_t need{3 + count + (p.default_profile[0] ? 1u : 0u) + (p.taunt ? 1u : 0u)};
	for (unsigned i = 0; i < count; ++i)
		need += (p.bots[i].profile[0] ? 1 : 0) + (p.bots[i].ship.kind != ship_kind::random ? 1 : 0);
	if (out.size() < need)
		return 0;
	std::size_t n{0};
	std::snprintf(out[n++].data(), BOT_PROFILE_LINE_SIZE, "BotCount=%u", count);
	std::snprintf(out[n++].data(), BOT_PROFILE_LINE_SIZE, "BotDefault=%u,%u", static_cast<unsigned>(p.default_skill), static_cast<unsigned>(p.default_style));
	if (p.default_profile[0])
		std::snprintf(out[n++].data(), BOT_PROFILE_LINE_SIZE, "BotDefaultStyle=%.31s", p.default_profile.data());
	std::snprintf(out[n++].data(), BOT_PROFILE_LINE_SIZE, "BotReplace=%u", p.replace ? 1u : 0u);
	/* Written only when on: a profile without it reads as off. */
	if (p.taunt)
		std::snprintf(out[n++].data(), BOT_PROFILE_LINE_SIZE, "BotTaunt=1");
	for (unsigned i = 0; i < count; ++i)
	{
		const auto &b{p.bots[i]};
		std::snprintf(out[n++].data(), BOT_PROFILE_LINE_SIZE, "Bot%u=%.8s,%u,%u,%u", i, b.name.data(), static_cast<unsigned>(b.skill), static_cast<unsigned>(b.style), static_cast<unsigned>(b.team));
		if (b.profile[0])
			std::snprintf(out[n++].data(), BOT_PROFILE_LINE_SIZE, "BotStyle%u=%.31s", i, b.profile.data());
		if (b.ship.kind != ship_kind::random)
			std::snprintf(out[n++].data(), BOT_PROFILE_LINE_SIZE, "BotShip%u=%s", i, format_ship_choice(b.ship).data());
	}
	return n;
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
	/* Section 9.13: the style profile lines (in any order with the bot
	 * lines).
	 */
	std::array<style_name, BOT_PROFILE_MAX_BOTS> m_profiles{};
	/* Section 9.20: the ship lines. */
	std::array<ship_choice, BOT_PROFILE_MAX_BOTS> m_ships{};
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
		if (rest == "DefaultStyle")
		{
			m_seen = true;
			m_profile.default_profile = make_style_name(value);
			return true;
		}
		if (rest.starts_with("Style"))
		{
			const auto i{detail::parse_unsigned(rest.substr(5))};
			if (!i)
				return false;
			m_seen = true;
			if (*i < BOT_PROFILE_MAX_BOTS)
				m_profiles[*i] = make_style_name(value);
			return true;
		}
		if (rest.starts_with("Ship"))
		{
			const auto i{detail::parse_unsigned(rest.substr(4))};
			if (!i)
				return false;
			m_seen = true;
			if (*i < BOT_PROFILE_MAX_BOTS)
				if (const auto c{parse_ship_choice(value)})
					m_ships[*i] = *c;
			return true;
		}
		if (rest == "Taunt")
		{
			m_seen = true;
			if (const auto u{detail::parse_unsigned(value)})
				m_profile.taunt = *u != 0;
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
				p.bots[i] = {.skill = p.default_skill, .style = p.default_style, .profile = p.default_profile};
			else
				p.bots[i].profile = m_profiles[i];
			/* The ship goes with the bot's position, also when its
			 * bot line is missing.
			 */
			if (i < p.count)
				p.bots[i].ship = m_ships[i];
		}
		return p;
	}
};

/* Section 6.4, "Save as default setup" during a game: the text of a
 * profile with its bot lines replaced by those of `p`, and every other
 * line as the file has it.  The game being played is not the setup the
 * host made (the command line may have switched the tracker off in it,
 * a level may have changed its options), so only the bots are written.
 * The bot lines go where the file's first one was, else before its
 * version line (`version_key`, the last line the game writes), else at
 * the end; a file that does not exist (empty `text`) gets them alone,
 * and its reader leaves every other option at its default.
 */
[[nodiscard]]
inline std::string replace_profile_bot_lines(const std::string_view text, const bot_profile &p, const std::string_view version_key)
{
	std::array<profile_line, BOT_PROFILE_MAX_LINES> lines;
	const auto n{format_profile(p, lines)};
	std::string block;
	for (std::size_t i = 0; i < n; ++i)
	{
		block += lines[i].data();
		block += '\n';
	}
	std::string out;
	out.reserve(text.size() + block.size());
	bool placed{};
	const auto place{[&] {
		if (!placed)
			out += block;
		placed = true;
	}};
	profile_reader scratch;
	for (std::size_t pos{0}; pos < text.size();)
	{
		const auto nl{text.find('\n', pos)};
		const auto end{nl == std::string_view::npos ? text.size() : nl};
		const auto line{text.substr(pos, end - pos)};
		pos = end + (nl == std::string_view::npos ? 0 : 1);
		const auto eq{line.find('=')};
		if (eq != std::string_view::npos)
		{
			const auto key{line.substr(0, eq)};
			if (scratch.parse(key, line.substr(eq + 1)))
			{
				place();
				continue;
			}
			if (key == version_key)
				place();
		}
		out += line;
		out += '\n';
	}
	place();
	return out;
}

/* Section 9.8: the BOT marker in the HUD's kill list.  The exp-16
 * playtest: "Bots do not show up as 'BOT' in scoring for me."  The kill
 * list marked a bot only in its ping column, shown with the ping display
 * option alone.  A bot's line now always says so, on the host and the
 * clients alike (player_is_bot): with the ping column its `BOT` there,
 * without it a grey `*` after the name.  The name column is narrow
 * (about 40 units, 22 with a kill goal or a time limit): the PR #38
 * review found that ` BOT` after the name cut a bot's name to three or
 * four letters or to nothing, and showed the marker twice with the ping
 * column.  The `*` costs one narrow character.  The team view lists
 * teams, not players.
 */
inline constexpr char BOT_KILL_LIST_MARKER[]{"*"};
inline constexpr char BOT_SCORE_MARKER[]{"BOT"};

/* `ping_column`: the kill list shows the ping column, which marks a bot
 * with its `BOT` already.
 */
[[nodiscard]]
constexpr bool kill_list_marks_bot(const bool is_bot, const bool team_view, const bool ping_column)
{
	return is_bot && !team_view && !ping_column;
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
