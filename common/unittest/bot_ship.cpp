/*
 * This file is part of the DXX-Rebirth project <https://github.com/dxx-rebirth/dxx-rebirth/>.
 * It is copyright by its individual contributors, as recorded in the
 * project's Git history.  See COPYING.txt at the top level for license
 * terms and a link to the Git history.
 */
/*
 * Test of the bots' ships (Documentation/multiplayer-bots.md section
 * 9.20): the setting and its text, which ship a setting means among the
 * host's ships (a style profile's ship, a missing ship, Random), the
 * `BotShip<n>` lines of the netgame profile, a style profile's `ship`
 * line and the chat command `/bot ship`.
 *
 * Build and run with SCons:
 *
 *	scons sdl2=1 d1x=0 d2x=1 register_runtime_test_plain_link_targets=1 test-bot-ships
 *	build/common/test-bot-ships
 */

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "bot_ship.h"
#include "bot_profile.h"
#include "bot_command.h"
#include "bot_style_profile.h"

using namespace dcx::bot;

namespace {

void check_failed(const char *const expr, const char *const file, const int line)
{
	std::fprintf(stderr, "%s:%d: check failed: %s\n", file, line, expr);
	std::exit(1);
}

#define CHECK(cond)	do { if (!(cond)) check_failed(#cond, __FILE__, __LINE__); } while (0)

using digest = std::array<uint8_t, 32>;

digest hash_of(const uint8_t first)
{
	digest d{};
	for (std::size_t i = 0; i < d.size(); ++i)
		d[i] = static_cast<uint8_t>(first + i * 7);
	return d;
}

struct fleet
{
	std::vector<digest> hashes;
	std::vector<std::string> names;
	std::vector<bool> cached;
	std::vector<ship_candidate> list;
	/* The views are made again: the vectors may have moved. */
	void add(const std::string &name, const uint8_t h, const bool received = false)
	{
		names.push_back(name);
		hashes.push_back(hash_of(h));
		cached.push_back(received);
		list.clear();
		for (std::size_t i = 0; i < names.size(); ++i)
			list.push_back({names[i], hashes[i], cached[i]});
	}
};

void test_choice_text()
{
	CHECK(parse_ship_choice("")->kind == ship_kind::random);
	CHECK(parse_ship_choice("random")->kind == ship_kind::random);
	CHECK(parse_ship_choice(" Random ")->kind == ship_kind::random);
	CHECK(parse_ship_choice("pyro")->kind == ship_kind::pyro);
	CHECK(parse_ship_choice("Pyro-GX")->kind == ship_kind::pyro);
	const auto n{parse_ship_choice("Longhorn")};
	CHECK(n && n->kind == ship_kind::named);
	CHECK(std::string_view(n->name.data()) == "longhorn");
	CHECK(!ship_has_id(*n));
	/* Not a ship's name: spaces, too long, odd characters. */
	CHECK(!parse_ship_choice("tie style"));
	CHECK(!parse_ship_choice("a234567890123456789012345"));
	CHECK(parse_ship_choice("a23456789012345678901234"));
	CHECK(!parse_ship_choice("x=y"));
	/* With its id; a bad id is dropped, the name stays. */
	const auto d{hash_of(0x3f)};
	const auto c{named_ship("tie-style", d)};
	CHECK(c && ship_has_id(*c) && ship_id_matches(*c, d));
	const auto text{format_ship_choice(*c)};
	CHECK(std::string_view(text.data()) == "tie-style,3f464d545b62");
	CHECK(parse_ship_choice(text.data()) == c);
	const auto bad{parse_ship_choice("tie-style,3f46zz545b62")};
	CHECK(bad && !ship_has_id(*bad) && std::string_view(bad->name.data()) == "tie-style");
	CHECK(std::string_view(format_ship_choice(pyro_ship()).data()) == "pyro");
	CHECK(format_ship_choice(ship_choice{})[0] == 0);
	/* The reserved words are no names. */
	CHECK(!named_ship("pyro"));
	CHECK(!named_ship("random"));
	/* The longest line fits the netgame profile's reader. */
	const auto longest{named_ship("a23456789012345678901234", d)};
	char line[80];
	std::snprintf(line, sizeof(line), "BotShip6=%s", format_ship_choice(*longest).data());
	CHECK(std::strlen(line) < BOT_PROFILE_LINE_SIZE);
}

void test_resolve()
{
	fleet f;
	f.add("anvil", 1);
	f.add("longhorn", 2);
	f.add("tie-style", 3);
	f.add("received", 4, true);
	const std::span<const ship_candidate> ships{f.list};
	/* Pyro. */
	{
		const auto r{resolve_ship(pyro_ship(), {}, "havoc", ships)};
		CHECK(!r.ship && !r.random && !r.missing);
	}
	/* A named ship, also a received one. */
	{
		const auto r{resolve_ship(*named_ship("longhorn"), {}, "havoc", ships)};
		CHECK(r.ship == 1u && !r.random && !r.missing);
		const auto c{resolve_ship(*named_ship("received"), {}, "havoc", ships)};
		CHECK(c.ship == 3u && !c.random);
	}
	/* Random: an own ship, by the name, never a received one; the same
	 * for the same name.
	 */
	for (const auto *const who : {"havoc", "ravager", "sparky", "nomad", "wraith", "talon", "viper", "blitz"})
	{
		const auto r{resolve_ship({}, {}, who, ships)};
		CHECK(r.random && r.ship && *r.ship < 3u);
		CHECK(resolve_ship({}, {}, who, ships).ship == r.ship);
	}
	/* No own ship: Random is the Pyro. */
	{
		fleet g;
		g.add("received", 4, true);
		const auto r{resolve_ship({}, {}, "havoc", g.list)};
		CHECK(r.random && !r.ship);
	}
	/* The style profile's ship, unless the setting is not Random. */
	{
		const auto r{resolve_ship({}, "tie-style", "havoc", ships)};
		CHECK(r.ship == 2u && r.from_profile && !r.random);
		const auto p{resolve_ship({}, "pyro", "havoc", ships)};
		CHECK(!p.ship && p.from_profile && !p.random);
		const auto o{resolve_ship(*named_ship("anvil"), "tie-style", "havoc", ships)};
		CHECK(o.ship == 0u && !o.from_profile);
		const auto q{resolve_ship(pyro_ship(), "tie-style", "havoc", ships)};
		CHECK(!q.ship);
	}
	/* A ship that is gone: Random instead, said. */
	{
		const auto r{resolve_ship(*named_ship("gone"), {}, "havoc", ships)};
		CHECK(r.missing && r.random && r.ship && *r.ship < 3u);
		const auto p{resolve_ship({}, "gone", "havoc", ships)};
		CHECK(p.missing && p.random && p.from_profile);
	}
	/* Two ships of one name (the user's and one received): the id
	 * chooses; without it the own one.
	 */
	{
		fleet g;
		g.add("twin", 10);
		g.add("twin", 11, true);
		CHECK(resolve_ship(*named_ship("twin"), {}, "x", g.list).ship == 0u);
		CHECK(resolve_ship(*named_ship("twin", hash_of(11)), {}, "x", g.list).ship == 1u);
		CHECK(resolve_ship(*named_ship("twin", hash_of(10)), {}, "x", g.list).ship == 0u);
		/* By id when the name is not there. */
		const auto byid{resolve_ship(*named_ship("old-name", hash_of(11)), {}, "x", g.list)};
		CHECK(byid.ship == 1u && !byid.missing);
	}
}

void test_word()
{
	const std::array<std::string_view, 5> names{{"anvil", "longhorn", "locust", "tie-style", "anvil"}};
	const std::span<const std::string_view> n{names};
	CHECK(match_ship_word("random", n).result == ship_word_result::random);
	CHECK(match_ship_word("RAND", n).result == ship_word_result::random);
	CHECK(match_ship_word("pyro", n).result == ship_word_result::pyro);
	CHECK(match_ship_word("pyro-gx", n).result == ship_word_result::pyro);
	CHECK(match_ship_word("py", n).result == ship_word_result::pyro);
	const auto l{match_ship_word("Longhorn", n)};
	CHECK(l.result == ship_word_result::found && l.index == 1);
	const auto t{match_ship_word("tie", n)};
	CHECK(t.result == ship_word_result::found && t.index == 3);
	CHECK(match_ship_word("lo", n).result == ship_word_result::ambiguous);
	const auto lon{match_ship_word("lon", n)};
	CHECK(lon.result == ship_word_result::found && lon.index == 1);
	/* The same name twice is one ship. */
	const auto a{match_ship_word("an", n)};
	CHECK(a.result == ship_word_result::found && a.index == 0);
	CHECK(match_ship_word("zz", n).result == ship_word_result::none);
	CHECK(match_ship_word("", n).result == ship_word_result::none);
	/* "r" begins "random" and a ship. */
	const std::array<std::string_view, 1> r{{"raptor"}};
	CHECK(match_ship_word("r", r).result == ship_word_result::ambiguous);
	CHECK(match_ship_word("rap", r).result == ship_word_result::found);
}

void test_profile_lines()
{
	bot_profile p;
	p.count = 3;
	std::snprintf(p.bots[0].name.data(), p.bots[0].name.size(), "ravager");
	std::snprintf(p.bots[1].name.data(), p.bots[1].name.size(), "havoc");
	std::snprintf(p.bots[2].name.data(), p.bots[2].name.size(), "sparky");
	p.bots[0].ship = *named_ship("a23456789012345678901234", hash_of(0x3f));
	p.bots[2].ship = pyro_ship();
	std::array<profile_line, BOT_PROFILE_MAX_LINES> lines;
	const auto n{format_profile(p, lines)};
	CHECK(n == 3 + 3 + 2);
	bool saw0{}, saw2{};
	profile_reader r;
	for (std::size_t i = 0; i < n; ++i)
	{
		const std::string_view line{lines[i].data()};
		CHECK(line.size() < BOT_PROFILE_LINE_SIZE - 1);
		saw0 |= line == "BotShip0=a23456789012345678901234,3f464d545b62";
		saw2 |= line == "BotShip2=pyro";
		CHECK(!line.starts_with("BotShip1"));
		const auto eq{line.find('=')};
		CHECK(r.parse(line.substr(0, eq), line.substr(eq + 1)));
	}
	CHECK(saw0 && saw2);
	CHECK(r.result() == p);
	/* Every bot with a ship line. */
	{
		bot_profile full;
		full.count = BOT_PROFILE_MAX_BOTS;
		for (auto &b : full.bots)
		{
			b.ship = *named_ship("longhorn", hash_of(1));
			b.profile = make_style_name("EC style");
		}
		full.default_profile = make_style_name("EC style");
		full.taunt = true;
		std::array<profile_line, BOT_PROFILE_MAX_LINES> all;
		CHECK(format_profile(full, all) == BOT_PROFILE_MAX_LINES);
	}
	/* Lines in any order; a ship line beyond the count is dropped; a bad
	 * one is ignored; an older file without ship lines reads Random.
	 */
	{
		profile_reader q;
		CHECK(q.parse("BotShip1", "longhorn"));
		CHECK(q.parse("BotShip5", "anvil"));
		CHECK(q.parse("BotShip0", "not a ship"));
		CHECK(q.parse("BotCount", "2"));
		CHECK(q.parse("Bot0", "ravager,2,0,0"));
		CHECK(q.parse("Bot1", "havoc,2,0,0"));
		CHECK(!q.parse("BotShipX", "anvil"));
		const auto res{q.result()};
		CHECK(res.bots[0].ship.kind == ship_kind::random);
		CHECK(res.bots[1].ship.kind == ship_kind::named && std::string_view(res.bots[1].ship.name.data()) == "longhorn");
		CHECK(res.bots[5].ship.kind == ship_kind::random);
	}
	/* "Save as default setup": the ship lines are replaced with the rest. */
	{
		const std::string text{"name=x\nBotCount=1\nBot0=ravager,2,0,0\nBotShip0=anvil\nBotShip3=old\nNGPVersion=3\n"};
		bot_profile q;
		q.count = 1;
		std::snprintf(q.bots[0].name.data(), q.bots[0].name.size(), "ravager");
		q.bots[0].ship = pyro_ship();
		const auto out{replace_profile_bot_lines(text, q, "NGPVersion")};
		CHECK(out.find("BotShip0=pyro\n") != std::string::npos);
		CHECK(out.find("anvil") == std::string::npos);
		CHECK(out.find("BotShip3") == std::string::npos);
		CHECK(out.find("name=x\n") == 0);
	}
}

void test_style_profile_ship()
{
	const std::string text{"format = 1\nname = Nico style\ncallsign = Nico\nship = Longhorn\nstyle.retreat_shields = 40\n"};
	const auto p{parse_style_profile(text)};
	CHECK(p && p->ship == "longhorn");
	const auto again{parse_style_profile(write_style_profile(*p))};
	CHECK(again && again->ship == "longhorn" && *again == *p);
	CHECK(parse_style_profile("format = 1\nship = pyro-gx\n")->ship == "pyro");
	CHECK(parse_style_profile("format = 1\nship = random\n")->ship.empty());
	CHECK(parse_style_profile("format = 1\nship = no such ship!\n")->ship.empty());
	CHECK(parse_style_profile("format = 1\n")->ship.empty());
	CHECK(write_style_profile(*parse_style_profile("format = 1\n")).find("ship") == std::string::npos);
}

void test_command()
{
	const auto c{parse_command("/bot ship havoc longhorn")};
	CHECK(c.kind == command_kind::ship);
	CHECK(std::string_view(c.name.data()) == "havoc" && !c.all);
	CHECK(std::string_view(c.ship_word.data()) == "longhorn");
	const auto a{parse_command("/bot SHIP all Random")};
	CHECK(a.kind == command_kind::ship && a.all);
	CHECK(std::string_view(a.ship_word.data()) == "random");
	const auto p{parse_command("/bot ship rav py")};
	CHECK(p.kind == command_kind::ship && std::string_view(p.ship_word.data()) == "py");
	CHECK(parse_command("/bot ship").kind == command_kind::error);
	CHECK(parse_command("/bot ship havoc").kind == command_kind::error);
	CHECK(parse_command("/bot ship havoc anvil more").kind == command_kind::error);
	CHECK(parse_command("/bot ship havoc a234567890123456789012345").kind == command_kind::error);
	/* No bot is called "ship". */
	CHECK(name_reserved("ship"));
	CHECK(parse_command("/bot add ship").kind == command_kind::error);
	/* The word as the game matches it. */
	const std::array<std::string_view, 2> names{{"longhorn", "anvil"}};
	CHECK(match_ship_word(c.ship_word.data(), names).index == 0);
	CHECK(match_ship_word(p.ship_word.data(), names).result == ship_word_result::pyro);
}

}

int main()
{
	test_choice_text();
	test_resolve();
	test_word();
	test_profile_lines();
	test_style_profile_ship();
	test_command();
	std::puts("bot_ship: all tests passed");
	return 0;
}
