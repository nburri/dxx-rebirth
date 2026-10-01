/*
 * This file is part of the DXX-Rebirth project <https://github.com/dxx-rebirth/dxx-rebirth/>.
 * It is copyright by its individual contributors, as recorded in the
 * project's Git history.  See COPYING.txt at the top level for license
 * terms and a link to the Git history.
 */
/*
 * Managing bots during a game (Documentation/multiplayer-bots.md
 * sections 6.4 and 9.11), the game-independent part
 * (common/unittest/bot_command.cpp):
 *
 * - the host's chat command `/bot`:
 *	/bot add [skill] [style] [name]
 *	/bot remove <name|all>
 *	/bot skill <name|all> <skill>
 *	/bot style <name|all> <style>
 *	/bot list
 *	/bot save	(the bots playing become the saved setup, section 6.5)
 *   A skill is trainee, rookie, hotshot, ace or insane; a style
 *   balanced, aggressive, cautious or collector.  Each may be given by
 *   exactly its first three letters (and a style by the Bots screen's
 *   short names Aggr, Caut, Coll).  Case does not matter.  Any other
 *   word in the place of `/bot add`'s name is the name, unless it is a
 *   reserved word (name_reserved): `all`, a skill or style word, or a
 *   command word.  No bot is called so, in the chat or on the Bots
 *   screens: `/bot add col ace` (style before skill) is an error, not a
 *   bot named "ace", and `/bot remove all` never meets a bot named "all".
 *   Section 9.13: a style is also a loaded style profile's word (its
 *   player's callsign: `/bot add hot EC`), in the style's place.
 * - which bot a name means (exact, else a unique prefix);
 * - whether the host may add a bot now, and why not.
 */

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

#include "bot_brain.h"

namespace dcx::bot {

/* A callsign: at most 8 characters (CALLSIGN_LEN). */
constexpr std::size_t BOT_COMMAND_NAME_LEN{8};

enum class command_kind : uint8_t
{
	/* Not a /bot command (the chat message goes out as typed). */
	none,
	/* `/bot` alone or `/bot help`: the usage. */
	help,
	list,
	add,
	remove,
	skill,
	style,
	save,
	/* A /bot command that could not be read: `error` says why. */
	error,
};

struct command
{
	command_kind kind{command_kind::none};
	std::optional<bot_skill> skill;
	std::optional<bot_style> style;
	/* Section 9.13: the style is the style profile of this index in the
	 * words parse_command was given (and `style` is empty).
	 */
	std::optional<unsigned> profile;
	/* add: the new bot's name (empty: the next built-in name);
	 * remove, skill, style: the bot meant (unless `all`).  NUL-terminated,
	 * cut to 8 characters.
	 */
	std::array<char, BOT_COMMAND_NAME_LEN + 1> name{};
	bool all{};
	/* kind == error: the reason, for the host's HUD. */
	const char *error{};
};

inline constexpr const char *BOT_COMMAND_USAGE{"/bot add [skill] [style] [name], remove <name|all>, skill <name|all> <skill>, style <name|all> <style>, list, save"};

namespace detail {

[[nodiscard]]
constexpr char lower(const char c)
{
	return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
}

[[nodiscard]]
constexpr bool iequal(const std::string_view a, const std::string_view b)
{
	if (a.size() != b.size())
		return false;
	for (std::size_t i = 0; i < a.size(); ++i)
		if (lower(a[i]) != lower(b[i]))
			return false;
	return true;
}

/* `a` is a prefix of `b`, ignoring case. */
[[nodiscard]]
constexpr bool iprefix(const std::string_view a, const std::string_view b)
{
	return a.size() <= b.size() && iequal(a, b.substr(0, a.size()));
}

[[nodiscard]]
constexpr bool is_space(const char c)
{
	return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

/* The next word of `s` (skipping spaces); empty at the end. */
[[nodiscard]]
constexpr std::string_view next_word(std::string_view &s)
{
	std::size_t i{0};
	while (i < s.size() && is_space(s[i]))
		++i;
	std::size_t j{i};
	while (j < s.size() && !is_space(s[j]) && s[j])
		++j;
	const auto w{s.substr(i, j - i)};
	s.remove_prefix(j);
	return w;
}

/* The full name, or exactly its first three letters: a longer part is
 * a bot's name ("rook" is a built-in name, not "rookie").
 */
[[nodiscard]]
constexpr bool names(const std::string_view word, const std::string_view full)
{
	return iequal(word, full) || (word.size() == 3 && iprefix(word, full));
}

constexpr void set_name(std::array<char, BOT_COMMAND_NAME_LEN + 1> &out, const std::string_view w)
{
	out = {};
	const auto n{w.size() < BOT_COMMAND_NAME_LEN ? w.size() : BOT_COMMAND_NAME_LEN};
	for (std::size_t i = 0; i < n; ++i)
		out[i] = lower(w[i]);
}

[[nodiscard]]
constexpr command error(const char *const why)
{
	command c;
	c.kind = command_kind::error;
	c.error = why;
	return c;
}

}

/* A name as a bot may carry it in the game: lower case, at most 8
 * characters, only letters, digits, '-' and '_' (no space and no colon:
 * a chat line "name: text" is addressed to a player, and `/bot` names a
 * bot by one word).
 */
[[nodiscard]]
constexpr std::array<char, BOT_COMMAND_NAME_LEN + 1> sanitize_name(const std::string_view w)
{
	std::array<char, BOT_COMMAND_NAME_LEN + 1> out{};
	std::size_t n{0};
	for (const char ch : w)
	{
		if (!ch || n >= BOT_COMMAND_NAME_LEN)
			break;
		const char c{detail::lower(ch)};
		if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_')
			out[n++] = c;
	}
	return out;
}

/* A skill by name ("ace", "Insane", "hot"...). */
[[nodiscard]]
constexpr std::optional<bot_skill> parse_skill(const std::string_view w)
{
	for (unsigned i = 0; i < BOT_SKILL_COUNT; ++i)
		if (detail::names(w, bot_skill_names[i]))
			return bot_skill{static_cast<uint8_t>(i)};
	return std::nullopt;
}

/* A style by name ("aggressive", "Caut", "col"...). */
[[nodiscard]]
constexpr std::optional<bot_style> parse_style(const std::string_view w)
{
	/* The Bots screen's short names. */
	constexpr std::array<std::string_view, BOT_STYLE_COUNT> short_names{{"bal", "aggr", "caut", "coll"}};
	for (unsigned i = 0; i < BOT_STYLE_COUNT; ++i)
		if (detail::names(w, bot_style_names[i]) || detail::iequal(w, short_names[i]))
			return bot_style{static_cast<uint8_t>(i)};
	return std::nullopt;
}

/* A word no bot is called: `all` (every bot, in remove, skill and
 * style), a skill or a style as `/bot` reads it (the full word, its
 * first three letters, the Bots screen's short names), and the command
 * words.  As a bot's name it would be read as something else in a
 * command, or hide a mistake in one (`/bot add col ace`).
 */
[[nodiscard]]
constexpr bool name_reserved(const std::string_view w)
{
	constexpr std::array<std::string_view, 11> words{{"all", "add", "remove", "rm", "kick", "skill", "style", "list", "save", "help", "bot"}};
	for (const auto r : words)
		if (detail::iequal(w, r))
			return true;
	return parse_skill(w) || parse_style(w);
}

/* sanitize_name, and nothing if what is left is a reserved word (the
 * bot then gets a built-in name, or keeps the name it has).
 */
[[nodiscard]]
constexpr std::array<char, BOT_COMMAND_NAME_LEN + 1> usable_name(const std::string_view w)
{
	auto out{sanitize_name(w)};
	/* Also the word as given: "aggressive" is longer than a name. */
	if (name_reserved(w) || name_reserved(std::string_view(out.data())))
		out = {};
	return out;
}

inline constexpr const char *BOT_NAME_RESERVED_TEXT{"Not a name: a skill, style or command word"};

/* Section 9.13: `w` as the word of a loaded style profile (the index in
 * `words`).
 */
[[nodiscard]]
constexpr std::optional<unsigned> parse_style_word(const std::string_view w, const std::span<const std::string_view> words)
{
	if (w.empty())
		return std::nullopt;
	for (std::size_t i = 0; i < words.size(); ++i)
		if (!words[i].empty() && detail::iequal(w, words[i]))
			return static_cast<unsigned>(i);
	return std::nullopt;
}

/* The chat line `s` as a /bot command (`none` if it is not one);
 * `style_words`: the words of the loaded style profiles.
 */
[[nodiscard]]
constexpr command parse_command(std::string_view s, const std::span<const std::string_view> style_words = {})
{
	auto rest{s};
	const auto head{detail::next_word(rest)};
	if (!detail::iequal(head, "/bot") && !detail::iequal(head, "/bots"))
		return {};
	const auto verb{detail::next_word(rest)};
	command c;
	if (verb.empty() || detail::iequal(verb, "help") || verb == "?")
	{
		c.kind = command_kind::help;
		return c;
	}
	if (detail::iequal(verb, "list") || detail::iequal(verb, "save"))
	{
		c.kind = detail::iequal(verb, "list") ? command_kind::list : command_kind::save;
		if (!detail::next_word(rest).empty())
			return detail::error(c.kind == command_kind::list ? "Usage: /bot list" : "Usage: /bot save");
		return c;
	}
	if (detail::iequal(verb, "add"))
	{
		c.kind = command_kind::add;
		/* In the order skill, style, name; each may be left out. */
		auto w{detail::next_word(rest)};
		if (!w.empty())
			if (const auto k{parse_skill(w)})
			{
				c.skill = k;
				w = detail::next_word(rest);
			}
		if (!w.empty())
		{
			if (const auto st{parse_style(w)})
			{
				c.style = st;
				w = detail::next_word(rest);
			}
			else if (const auto pr{parse_style_word(w, style_words)})
			{
				c.profile = pr;
				w = detail::next_word(rest);
			}
		}
		if (!w.empty())
		{
			if (w.size() > BOT_COMMAND_NAME_LEN)
				return detail::error("A name has at most 8 characters");
			c.name = sanitize_name(w);
			if (!c.name[0] || std::string_view(c.name.data()).size() != w.size())
				return detail::error("A name has letters, digits, - and _");
			/* `/bot add col ace`: the skill comes before the style. */
			if (name_reserved(w))
				return detail::error(parse_skill(w) || parse_style(w) ? "Skill, then style, then name: /bot add ace col name" : BOT_NAME_RESERVED_TEXT);
		}
		if (!detail::next_word(rest).empty())
			return detail::error("Usage: /bot add [skill] [style] [name]");
		return c;
	}
	const bool is_remove{detail::iequal(verb, "remove") || detail::iequal(verb, "rm") || detail::iequal(verb, "kick")};
	const bool is_skill{detail::iequal(verb, "skill")};
	const bool is_style{detail::iequal(verb, "style")};
	if (!is_remove && !is_skill && !is_style)
		return detail::error("Unknown /bot command; /bot help");
	const auto target{detail::next_word(rest)};
	const char *const usage{is_remove ? "Usage: /bot remove <name|all>" : is_skill ? "Usage: /bot skill <name|all> <trainee..insane>" : "Usage: /bot style <name|all> <balanced|aggressive|cautious|collector>"};
	if (target.empty())
		return detail::error(usage);
	if (detail::iequal(target, "all"))
		c.all = true;
	else
		detail::set_name(c.name, target);
	if (is_remove)
	{
		c.kind = command_kind::remove;
		if (!detail::next_word(rest).empty())
			return detail::error(usage);
		return c;
	}
	const auto value{detail::next_word(rest)};
	if (value.empty() || !detail::next_word(rest).empty())
		return detail::error(usage);
	if (is_skill)
	{
		c.kind = command_kind::skill;
		c.skill = parse_skill(value);
		if (!c.skill)
			return detail::error("Skills: trainee, rookie, hotshot, ace, insane");
	}
	else
	{
		c.kind = command_kind::style;
		c.style = parse_style(value);
		if (!c.style)
		{
			c.profile = parse_style_word(value, style_words);
			if (!c.profile)
				return detail::error("Styles: balanced, aggressive, cautious, collector, or a loaded style's word");
		}
	}
	return c;
}

enum class target_result : uint8_t
{
	found,
	none,
	ambiguous,
};

struct target_match
{
	target_result result{target_result::none};
	std::size_t index{};
};

/* Which of `names` (the playing bots' callsigns) `wanted` means: the one
 * with that name, else the only one it begins, ignoring case.
 */
[[nodiscard]]
constexpr target_match find_target(const std::span<const std::string_view> names, const std::string_view wanted)
{
	if (wanted.empty())
		return {};
	for (std::size_t i = 0; i < names.size(); ++i)
		if (detail::iequal(names[i], wanted))
			return {target_result::found, i};
	target_match m;
	for (std::size_t i = 0; i < names.size(); ++i)
		if (detail::iprefix(wanted, names[i]))
		{
			if (m.result == target_result::found)
				return {target_result::ambiguous, 0};
			m = {target_result::found, i};
		}
	return m;
}

/* Whether the host may add a bot now (section 6.4). */
enum class add_verdict : uint8_t
{
	ok,
	/* Not a network game this machine hosts. */
	not_host,
	/* Bots play anarchy, team anarchy and bounty until stage B7. */
	wrong_mode,
	/* Between levels, or the level is not running yet. */
	not_playing,
	/* The reactor is destroyed: nobody spawns until the next level. */
	countdown,
	/* The host is serving a human's join (its snapshot describes the
	 * game without the new bot): the host asks again in a moment.
	 */
	join_in_progress,
	/* No slot below the player limit a new player could take without
	 * anyone leaving (a disconnected human keeps a slot to rejoin).
	 */
	full,
};

struct add_situation
{
	bool host{};
	bool mode_allowed{};
	bool playing{};
	bool countdown{};
	bool join_in_progress{};
	bool free_slot{};
};

[[nodiscard]]
constexpr add_verdict judge_add(const add_situation &s)
{
	if (!s.host)
		return add_verdict::not_host;
	if (!s.mode_allowed)
		return add_verdict::wrong_mode;
	if (!s.playing)
		return add_verdict::not_playing;
	if (s.countdown)
		return add_verdict::countdown;
	if (!s.free_slot)
		return add_verdict::full;
	if (s.join_in_progress)
		return add_verdict::join_in_progress;
	return add_verdict::ok;
}

[[nodiscard]]
constexpr const char *add_verdict_text(const add_verdict v)
{
	switch (v)
	{
		case add_verdict::ok:
			return "";
		case add_verdict::not_host:
			return "Only the host can manage bots";
		case add_verdict::wrong_mode:
			return "Bots play anarchy, team anarchy and bounty for now";
		case add_verdict::not_playing:
			return "Bots can be added while a level is played";
		case add_verdict::countdown:
			return "No new bots during the countdown";
		case add_verdict::join_in_progress:
			return "A player is joining; try again in a moment";
		case add_verdict::full:
			return "The game is full";
	}
	return "";
}

/* The order of addition for a bot added now: after every bot in the
 * game, so that a joining human who finds the game full replaces it
 * first (section 2.3, net_v2_session.h bot_to_replace).
 */
[[nodiscard]]
constexpr unsigned next_added_order(const std::span<const unsigned> orders)
{
	unsigned m{0};
	for (const auto o : orders)
		if (o > m)
			m = o;
	return m + 1;
}

}
