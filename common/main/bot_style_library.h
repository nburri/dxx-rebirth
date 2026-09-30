/*
 * This file is part of the DXX-Rebirth project <https://github.com/dxx-rebirth/dxx-rebirth/>.
 * It is copyright by its individual contributors, as recorded in the
 * project's Git history.  See COPYING.txt at the top level for license
 * terms and a link to the Git history.
 */
/*
 * The bot style profiles the host has (Documentation/multiplayer-bots.md
 * section 9.13, Documentation/movement-recording.md section 8.7): the
 * `.botstyle` files of the folder `botstyles/`, each a style a bot can
 * fly besides the four built-in ones.  The game reads the files
 * (bot_menu.cpp); this is the game-independent part: which files make a
 * style, under which name (the bot setup's, the pilot's netgame
 * profile's) and which chat word (`/bot add hot evilcow`).
 *
 * The files come from anywhere (a friend's analysis, the internet): a
 * file is read only up to STYLE_FILE_MAX_BYTES, at most
 * STYLE_LIBRARY_MAX of them are kept, the parser skips what it does not
 * understand and clamps every value (bot_style_profile.h), and the names
 * are cut to what a menu line and a profile line hold, in printable
 * ASCII.
 *
 * Header-only, standard library and the bots' pure headers only
 * (common/unittest/bot_style_profiles.cpp).
 */

#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "bot_command.h"
#include "bot_style_profile.h"

namespace dcx::bot {

/* A style's name: the menus and the `.ngp` line `BotStyle<n>=<name>`
 * (under 50 characters) hold it.
 */
constexpr std::size_t BOT_STYLE_NAME_LEN{31};
/* A style's chat word (`/bot add hot evilcow`). */
constexpr std::size_t BOT_STYLE_WORD_LEN{16};
constexpr std::size_t STYLE_LIBRARY_MAX{24};
constexpr std::size_t STYLE_FILE_MAX_BYTES{64 * 1024};
/* The folder, in the PhysFS write directory (next to the pilot files). */
inline constexpr char BOT_STYLE_FOLDER[]{"botstyles"};

using style_name = std::array<char, BOT_STYLE_NAME_LEN + 1>;
using style_word = std::array<char, BOT_STYLE_WORD_LEN + 1>;

/* `s` as a style's name: printable ASCII ('?' for anything else, the
 * menus' fonts have nothing more), no '=' or ',' (the profile's line),
 * spaces trimmed, at most BOT_STYLE_NAME_LEN characters.
 */
[[nodiscard]]
inline style_name make_style_name(std::string_view s)
{
	style_name out{};
	std::size_t n{0};
	while (!s.empty() && (s.front() == ' ' || s.front() == '\t'))
		s.remove_prefix(1);
	for (std::size_t i{0}; i < s.size() && n < BOT_STYLE_NAME_LEN; ++i)
	{
		const auto c{static_cast<unsigned char>(s[i])};
		if (c >= 0x80)
		{
			/* One '?' per UTF-8 character: skip its continuation bytes. */
			while (i + 1 < s.size() && (static_cast<unsigned char>(s[i + 1]) & 0xc0) == 0x80)
				++i;
			out[n++] = '?';
		}
		else if (c < 0x20 || c == 0x7f)
			out[n++] = ' ';
		else if (c == '=' || c == ',')
			out[n++] = ' ';
		else
			out[n++] = static_cast<char>(c);
	}
	while (n && out[n - 1] == ' ')
		out[--n] = 0;
	return out;
}

/* `s` as a chat word: lower case letters, digits, '-' and '_' only, at
 * most BOT_STYLE_WORD_LEN; empty if nothing is left or it is a word
 * `/bot` reads otherwise (a skill, a built-in style, a command word).
 */
[[nodiscard]]
inline style_word make_style_word(const std::string_view s)
{
	style_word out{};
	std::size_t n{0};
	for (const char ch : s)
	{
		if (n >= BOT_STYLE_WORD_LEN)
			break;
		const char c{detail::lower(ch)};
		if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_')
			out[n++] = c;
	}
	if (name_reserved(std::string_view(out.data())))
		out = {};
	return out;
}

struct loaded_style
{
	/* The name the setup shows and the netgame profile stores. */
	style_name name{};
	/* The chat word (empty: none). */
	style_word word{};
	style_profile profile;
};

enum class style_add_result : uint8_t
{
	added,
	/* No `format` line, or a newer format. */
	not_a_profile,
	/* Larger than STYLE_FILE_MAX_BYTES. */
	too_large,
	/* A style of that name is there already (the first file wins). */
	duplicate,
	/* STYLE_LIBRARY_MAX styles already. */
	full,
};

[[nodiscard]]
constexpr const char *style_add_text(const style_add_result r)
{
	switch (r)
	{
		case style_add_result::added:
			return "loaded";
		case style_add_result::not_a_profile:
			return "not a bot style profile";
		case style_add_result::too_large:
			return "too large";
		case style_add_result::duplicate:
			return "a style of that name is loaded already";
		case style_add_result::full:
			return "too many styles";
	}
	return "";
}

class style_library
{
	std::vector<loaded_style> m_styles;
public:
	void clear()
	{
		m_styles.clear();
	}
	[[nodiscard]]
	std::size_t size() const
	{
		return m_styles.size();
	}
	[[nodiscard]]
	const loaded_style &operator[](const std::size_t i) const
	{
		return m_styles[i];
	}
	/* The style of file `file_stem` (its name without `.botstyle`) with
	 * the text `text`.  Its name is the profile's `name`, else
	 * "<callsign> style", else "<file_stem> style"; its word the
	 * callsign's, else the file stem's.
	 */
	style_add_result add(const std::string_view file_stem, const std::string_view text)
	{
		if (text.size() > STYLE_FILE_MAX_BYTES)
			return style_add_result::too_large;
		if (m_styles.size() >= STYLE_LIBRARY_MAX)
			return style_add_result::full;
		auto parsed{parse_style_profile(text)};
		if (!parsed)
			return style_add_result::not_a_profile;
		loaded_style ls;
		const std::string who{!parsed->callsign.empty() ? parsed->callsign : std::string{file_stem}};
		ls.name = make_style_name(!parsed->name.empty() ? parsed->name : who + " style");
		if (!ls.name[0])
			ls.name = make_style_name(std::string{file_stem} + " style");
		if (!ls.name[0])
			return style_add_result::not_a_profile;
		if (find(ls.name.data()))
			return style_add_result::duplicate;
		ls.word = make_style_word(who);
		if (ls.word[0] && find_word(ls.word.data()))
			ls.word = {};
		ls.profile = std::move(*parsed);
		m_styles.push_back(std::move(ls));
		return style_add_result::added;
	}
	/* The index of the style called `name` (case does not matter), or
	 * size().
	 */
	[[nodiscard]]
	std::size_t index_of(const std::string_view name) const
	{
		if (name.empty())
			return m_styles.size();
		for (std::size_t i{0}; i != m_styles.size(); ++i)
			if (detail::iequal(std::string_view(m_styles[i].name.data()), name))
				return i;
		return m_styles.size();
	}
	[[nodiscard]]
	const loaded_style *find(const std::string_view name) const
	{
		const auto i{index_of(name)};
		return i < m_styles.size() ? &m_styles[i] : nullptr;
	}
	[[nodiscard]]
	const loaded_style *find_word(const std::string_view word) const
	{
		if (word.empty())
			return nullptr;
		for (const auto &s : m_styles)
			if (s.word[0] && detail::iequal(std::string_view(s.word.data()), word))
				return &s;
		return nullptr;
	}
	/* The chat words, in the library's order (empty for a style without
	 * one), for parse_command.
	 */
	[[nodiscard]]
	std::vector<std::string_view> words() const
	{
		std::vector<std::string_view> r;
		r.reserve(m_styles.size());
		for (const auto &s : m_styles)
			r.emplace_back(s.word.data());
		return r;
	}
};

/* Section 9.13: a bot's style as the setup's sliders offer it: the four
 * built-in styles, then the library's.
 */
[[nodiscard]]
constexpr unsigned style_choice_count(const std::size_t library_size)
{
	return BOT_STYLE_COUNT + static_cast<unsigned>(std::min(library_size, STYLE_LIBRARY_MAX));
}

}
