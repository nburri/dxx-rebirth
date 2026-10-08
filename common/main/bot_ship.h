/*
 * This file is part of the DXX-Rebirth project <https://github.com/dxx-rebirth/dxx-rebirth/>.
 * It is copyright by its individual contributors, as recorded in the
 * project's Git history.  See COPYING.txt at the top level for license
 * terms and a link to the Git history.
 */
/*
 * The ship a bot flies (Documentation/multiplayer-bots.md section 9.20,
 * Documentation/custom-ships.md section 11.3): Random (one of the host's
 * own ships, chosen by the bot's name; the default), the Pyro-GX, or a
 * ship the host has, by its name.  A bot that flies a style profile
 * naming a ship (`ship = <name>` in its `.botstyle`) flies that one
 * unless its own setting is not Random.
 *
 * The game-independent part (common/unittest/bot_ship.cpp): the setting,
 * its `.ngp` text (`BotShip<n>=<name>,<id>`, the id being the first
 * BOT_SHIP_ID_BYTES bytes of the file's SHA-256), which ship a setting
 * means among the ships the host has, and the ship word of
 * `/bot ship <bot|all> <ship>`.  Standard library only.
 */

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <span>
#include <string_view>

namespace dcx::bot {

/* A ship's name: [a-z0-9_-], at most 24 characters (dxship MAX_NAME). */
constexpr std::size_t BOT_SHIP_NAME_LEN{24};
/* The bytes of the SHA-256 the `.ngp` keeps (12 hex digits: the line
 * stays under 50 characters).
 */
constexpr std::size_t BOT_SHIP_ID_BYTES{6};

enum class ship_kind : uint8_t
{
	/* One of the host's own ships, by the bot's name (or its style
	 * profile's ship).
	 */
	random,
	pyro,
	named,
};

using ship_name_text = std::array<char, BOT_SHIP_NAME_LEN + 1>;
using ship_id = std::array<uint8_t, BOT_SHIP_ID_BYTES>;

struct ship_choice
{
	ship_kind kind{ship_kind::random};
	/* named: the ship's name (NUL-terminated). */
	ship_name_text name{};
	/* named: the start of the SHA-256 of the file chosen (all zero:
	 * unknown, e.g. typed by hand).
	 */
	ship_id id{};
	constexpr bool operator==(const ship_choice &) const = default;
};

namespace ship_detail {

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

[[nodiscard]]
constexpr bool iprefix(const std::string_view a, const std::string_view b)
{
	return a.size() <= b.size() && iequal(a, b.substr(0, a.size()));
}

[[nodiscard]]
constexpr int hex_value(const char c)
{
	if (c >= '0' && c <= '9')
		return c - '0';
	const char l{lower(c)};
	if (l >= 'a' && l <= 'f')
		return l - 'a' + 10;
	return -1;
}

[[nodiscard]]
constexpr std::string_view trim(std::string_view s)
{
	while (!s.empty() && (s.front() == ' ' || s.front() == '\t'))
		s.remove_prefix(1);
	while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r' || s.back() == '\n'))
		s.remove_suffix(1);
	return s;
}

}

/* A ship's name as the `.dxship` format allows it. */
[[nodiscard]]
constexpr bool valid_ship_name(const std::string_view name)
{
	if (name.empty() || name.size() > BOT_SHIP_NAME_LEN)
		return false;
	for (const char c : name)
		if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-'))
			return false;
	return true;
}

/* The words for the Pyro-GX and for Random (in the `.ngp`, a style
 * profile's `ship` line and `/bot ship`).  A ship of that name cannot be
 * chosen by name.
 */
inline constexpr std::string_view SHIP_WORD_PYRO{"pyro"};
inline constexpr std::string_view SHIP_WORD_PYRO_LONG{"pyro-gx"};
inline constexpr std::string_view SHIP_WORD_RANDOM{"random"};

[[nodiscard]]
constexpr bool ship_word_pyro(const std::string_view w)
{
	return ship_detail::iequal(w, SHIP_WORD_PYRO) || ship_detail::iequal(w, SHIP_WORD_PYRO_LONG);
}

[[nodiscard]]
constexpr ship_choice pyro_ship()
{
	ship_choice c;
	c.kind = ship_kind::pyro;
	return c;
}

/* The ship named `name` (lower-cased; nothing if it is no ship name);
 * `hash`, if given, the start of the file's SHA-256.
 */
[[nodiscard]]
constexpr std::optional<ship_choice> named_ship(const std::string_view name, const std::span<const uint8_t> hash = {})
{
	ship_choice c;
	c.kind = ship_kind::named;
	if (name.size() > BOT_SHIP_NAME_LEN)
		return std::nullopt;
	for (std::size_t i = 0; i < name.size(); ++i)
		c.name[i] = ship_detail::lower(name[i]);
	if (!valid_ship_name(std::string_view(c.name.data())) || ship_word_pyro(name) || ship_detail::iequal(name, SHIP_WORD_RANDOM))
		return std::nullopt;
	for (std::size_t i = 0; i < c.id.size() && i < hash.size(); ++i)
		c.id[i] = hash[i];
	return c;
}

[[nodiscard]]
constexpr bool ship_has_id(const ship_choice &c)
{
	for (const auto b : c.id)
		if (b)
			return true;
	return false;
}

/* `hash` (a whole SHA-256) begins with the choice's id. */
[[nodiscard]]
constexpr bool ship_id_matches(const ship_choice &c, const std::span<const uint8_t> hash)
{
	if (hash.size() < c.id.size())
		return false;
	for (std::size_t i = 0; i < c.id.size(); ++i)
		if (hash[i] != c.id[i])
			return false;
	return true;
}

/* The `.ngp` value: "pyro", "<name>,<12 hex digits>" or "<name>"
 * without an id; Random has no line (empty).
 */
using ship_choice_text = std::array<char, BOT_SHIP_NAME_LEN + 2 + 2 * BOT_SHIP_ID_BYTES + 1>;

[[nodiscard]]
inline ship_choice_text format_ship_choice(const ship_choice &c)
{
	ship_choice_text out{};
	switch (c.kind)
	{
		case ship_kind::random:
			break;
		case ship_kind::pyro:
			std::snprintf(out.data(), out.size(), "%s", SHIP_WORD_PYRO.data());
			break;
		case ship_kind::named:
		{
			const int n{std::snprintf(out.data(), out.size(), "%.24s", c.name.data())};
			if (ship_has_id(c) && n > 0)
			{
				auto p{static_cast<std::size_t>(n)};
				out[p++] = ',';
				constexpr char digits[]{"0123456789abcdef"};
				for (const auto b : c.id)
				{
					out[p++] = digits[b >> 4];
					out[p++] = digits[b & 15];
				}
				out[p] = 0;
			}
			break;
		}
	}
	return out;
}

/* The `.ngp` value, a style profile's `ship` line ("random", "pyro",
 * "pyro-gx", "<name>", "<name>,<hex>"); nothing if it is none of them.
 * A bad id is dropped (the name still counts).
 */
[[nodiscard]]
constexpr std::optional<ship_choice> parse_ship_choice(std::string_view v)
{
	v = ship_detail::trim(v);
	if (v.empty() || ship_detail::iequal(v, SHIP_WORD_RANDOM))
		return ship_choice{};
	if (ship_word_pyro(v))
		return pyro_ship();
	std::string_view name{v}, hex{};
	if (const auto comma{v.find(',')}; comma != std::string_view::npos)
	{
		name = ship_detail::trim(v.substr(0, comma));
		hex = ship_detail::trim(v.substr(comma + 1));
	}
	auto c{named_ship(name)};
	if (!c)
		return std::nullopt;
	if (hex.size() == 2 * BOT_SHIP_ID_BYTES)
	{
		ship_id id{};
		bool ok{true};
		for (std::size_t i = 0; i < id.size(); ++i)
		{
			const int hi{ship_detail::hex_value(hex[2 * i])}, lo{ship_detail::hex_value(hex[2 * i + 1])};
			if (hi < 0 || lo < 0)
			{
				ok = false;
				break;
			}
			id[i] = static_cast<uint8_t>(hi * 16 + lo);
		}
		if (ok)
			c->id = id;
	}
	return c;
}

/* A ship the host has (bundled or the user's in `ships/`, or received
 * from a host in `ships/cache/`).
 */
struct ship_candidate
{
	std::string_view name;
	/* The file's SHA-256 (or at least its start). */
	std::span<const uint8_t> hash;
	/* Received from a host: never chosen at random. */
	bool cached{};
};

/* Which ship a bot flies. */
struct ship_resolution
{
	/* The index of the ship in the candidates; nothing: the Pyro-GX. */
	std::optional<std::size_t> ship;
	/* Chosen at random (the setting, or the fallback for a missing ship). */
	bool random{};
	/* The ship wanted (by the setting or the style profile) is not
	 * there: Random instead.
	 */
	bool missing{};
	/* The ship wanted came from the style profile. */
	bool from_profile{};
};

/* Random: one of the host's own ships (not those received from a host),
 * chosen by the bot's name, so that a bot keeps its ship for the
 * session; none: the Pyro-GX.
 */
[[nodiscard]]
constexpr std::optional<std::size_t> pick_random_ship(const std::string_view callsign, const std::span<const ship_candidate> ships)
{
	std::size_t own{0};
	for (const auto &s : ships)
		if (!s.cached)
			++own;
	if (!own)
		return std::nullopt;
	uint32_t h{2166136261u};
	for (const char ch : callsign)
	{
		if (!ch)
			break;
		h ^= static_cast<uint8_t>(ch);
		h *= 16777619u;
	}
	std::size_t k{h % own};
	for (std::size_t i = 0; i < ships.size(); ++i)
		if (!ships[i].cached && !k--)
			return i;
	return std::nullopt;
}

/* The ship called `name`: an own one first, then a received one; among
 * several of that name the one whose hash begins with `c`'s id.
 */
[[nodiscard]]
constexpr std::optional<std::size_t> find_ship_named(const std::string_view name, const ship_choice *const c, const std::span<const ship_candidate> ships)
{
	std::optional<std::size_t> first;
	for (unsigned pass = 0; pass < 2; ++pass)
		for (std::size_t i = 0; i < ships.size(); ++i)
		{
			if (ships[i].cached != (pass == 1) ||!ship_detail::iequal(ships[i].name, name))
				continue;
			if (c && ship_has_id(*c) && ship_id_matches(*c, ships[i].hash))
				return i;
			if (!first)
				first = i;
		}
	if (first)
		return first;
	/* Not by its name: the very file by its id (the setting is older
	 * than a renamed ship).
	 */
	if (c && ship_has_id(*c))
		for (std::size_t i = 0; i < ships.size(); ++i)
			if (ship_id_matches(*c, ships[i].hash))
				return i;
	return std::nullopt;
}

/* The ship a bot flies: its setting `c`, else (Random) the ship its
 * style profile names (`profile_ship`, empty: none), else one at random.
 */
[[nodiscard]]
constexpr ship_resolution resolve_ship(const ship_choice &c, const std::string_view profile_ship, const std::string_view callsign, const std::span<const ship_candidate> ships)
{
	ship_resolution r;
	switch (c.kind)
	{
		case ship_kind::pyro:
			return r;
		case ship_kind::named:
			r.ship = find_ship_named(std::string_view(c.name.data()), &c, ships);
			if (r.ship)
				return r;
			r.missing = true;
			break;
		case ship_kind::random:
			if (const auto p{parse_ship_choice(profile_ship)}; p && p->kind != ship_kind::random)
			{
				r.from_profile = true;
				if (p->kind == ship_kind::pyro)
					return r;
				r.ship = find_ship_named(std::string_view(p->name.data()), &*p, ships);
				if (r.ship)
					return r;
				r.missing = true;
			}
			break;
	}
	r.random = true;
	r.ship = pick_random_ship(callsign, ships);
	return r;
}

/* The ship word of `/bot ship <bot|all> <ship>`. */
enum class ship_word_result : uint8_t
{
	found,
	random,
	pyro,
	none,
	ambiguous,
};

struct ship_word_match
{
	ship_word_result result{ship_word_result::none};
	/* found: the index in `names`. */
	std::size_t index{};
};

/* Which ship `word` means among `names` (the host's ships): "random",
 * "pyro" or "pyro-gx", a ship's name, else the only one of them all it
 * begins, ignoring case.  Two ships of one name (own and received) are
 * one: the first listed.
 */
[[nodiscard]]
constexpr ship_word_match match_ship_word(const std::string_view word, const std::span<const std::string_view> names)
{
	if (word.empty())
		return {};
	if (ship_detail::iequal(word, SHIP_WORD_RANDOM))
		return {ship_word_result::random, 0};
	if (ship_word_pyro(word))
		return {ship_word_result::pyro, 0};
	for (std::size_t i = 0; i < names.size(); ++i)
		if (ship_detail::iequal(names[i], word))
			return {ship_word_result::found, i};
	ship_word_match m;
	const auto take{[&m](const ship_word_match n) {
		if (m.result != ship_word_result::none)
			return false;
		m = n;
		return true;
	}};
	if (ship_detail::iprefix(word, SHIP_WORD_RANDOM) && !take({ship_word_result::random, 0}))
		return {ship_word_result::ambiguous, 0};
	if (ship_detail::iprefix(word, SHIP_WORD_PYRO_LONG) && !take({ship_word_result::pyro, 0}))
		return {ship_word_result::ambiguous, 0};
	for (std::size_t i = 0; i < names.size(); ++i)
	{
		if (!ship_detail::iprefix(word, names[i]))
			continue;
		/* The same name listed twice is one ship. */
		bool seen{};
		for (std::size_t j = 0; j < i; ++j)
			if (ship_detail::iequal(names[j], names[i]))
				seen = true;
		if (seen)
			continue;
		if (!take({ship_word_result::found, i}))
			return {ship_word_result::ambiguous, 0};
	}
	return m;
}

}
