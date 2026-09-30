/*
 * This file is part of the DXX-Rebirth project <https://github.com/dxx-rebirth/dxx-rebirth/>.
 * It is copyright by its individual contributors, as recorded in the
 * project's Git history.  See COPYING.txt at the top level for license
 * terms and a link to the Git history.
 */
/*
 * Test of managing bots during a game (Documentation/multiplayer-bots.md
 * sections 6.4 and 9.11): the host's chat command `/bot`, which bot a
 * name means, when a bot may be added, and how a bot added during the
 * game fits the admission of humans (the slot it takes, the player
 * limit, humans replace bots).
 *
 * Build and run with SCons:
 *
 *	scons sdl2=1 d1x=0 d2x=1 register_runtime_test_plain_link_targets=1 test-bot-commands
 *	build/common/test-bot-commands
 */

#include <array>
#include <cstdio>
#include <cstdlib>
#include <span>
#include <string_view>

#include "bot_command.h"
#include "net_v2_session.h"

using namespace dcx::bot;
namespace nv = dcx::net_v2;

namespace {

void check_failed(const char *const expr, const char *const file, const int line)
{
	std::fprintf(stderr, "%s:%d: check failed: %s\n", file, line, expr);
	std::exit(1);
}

#define CHECK(cond)	do { if (!(cond)) check_failed(#cond, __FILE__, __LINE__); } while (0)

[[nodiscard]]
std::string_view name_of(const command &c)
{
	return c.name.data();
}

void test_not_a_command()
{
	CHECK(parse_command("").kind == command_kind::none);
	CHECK(parse_command("hello there").kind == command_kind::none);
	CHECK(parse_command("/kick: havoc").kind == command_kind::none);
	CHECK(parse_command("/bottle add").kind == command_kind::none);
	CHECK(parse_command("/botadd").kind == command_kind::none);
	CHECK(parse_command("bot add").kind == command_kind::none);
	/* A message to a player named "/bot" is still no command. */
	CHECK(parse_command("/bot: hi").kind == command_kind::none);
}

void test_help_and_list()
{
	CHECK(parse_command("/bot").kind == command_kind::help);
	CHECK(parse_command("/bot   ").kind == command_kind::help);
	CHECK(parse_command("/BOT Help").kind == command_kind::help);
	CHECK(parse_command("/bots ?").kind == command_kind::help);
	CHECK(parse_command("/bot list").kind == command_kind::list);
	CHECK(parse_command("  /bot   LIST  ").kind == command_kind::list);
	CHECK(parse_command("/bot list all").kind == command_kind::error);
	CHECK(parse_command("/bot save").kind == command_kind::save);
	CHECK(parse_command("/bot save now").kind == command_kind::error);
	const auto bad{parse_command("/bot dance")};
	CHECK(bad.kind == command_kind::error);
	CHECK(bad.error && bad.error[0]);
}

void test_skill_and_style_words()
{
	CHECK(parse_skill("trainee") == bot_skill::trainee);
	CHECK(parse_skill("Rookie") == bot_skill::rookie);
	CHECK(parse_skill("HOTSHOT") == bot_skill::hotshot);
	CHECK(parse_skill("ace") == bot_skill::ace);
	CHECK(parse_skill("insane") == bot_skill::insane);
	CHECK(parse_skill("hot") == bot_skill::hotshot);
	CHECK(parse_skill("ins") == bot_skill::insane);
	CHECK(parse_skill("roo") == bot_skill::rookie);
	/* "rook" is a built-in bot name, not a skill. */
	CHECK(!parse_skill("rook"));
	CHECK(!parse_skill("ro"));
	CHECK(!parse_skill(""));
	CHECK(!parse_skill("insaner"));
	CHECK(!parse_skill("balanced"));
	CHECK(parse_style("balanced") == bot_style::balanced);
	CHECK(parse_style("Aggressive") == bot_style::aggressive);
	CHECK(parse_style("cautious") == bot_style::cautious);
	CHECK(parse_style("collector") == bot_style::collector);
	CHECK(parse_style("bal") == bot_style::balanced);
	CHECK(parse_style("agg") == bot_style::aggressive);
	CHECK(parse_style("Aggr") == bot_style::aggressive);
	CHECK(parse_style("caut") == bot_style::cautious);
	CHECK(parse_style("col") == bot_style::collector);
	CHECK(parse_style("coll") == bot_style::collector);
	CHECK(!parse_style("c"));
	CHECK(!parse_style("ace"));
	CHECK(!parse_style("collect"));
	/* No word is both. */
	for (const auto w : {"trainee", "rookie", "hotshot", "ace", "insane", "tra", "roo", "hot", "ins"})
		CHECK(!parse_style(w));
	for (const auto w : {"balanced", "aggressive", "cautious", "collector", "bal", "agg", "cau", "col", "aggr", "caut", "coll"})
		CHECK(!parse_skill(w));
	/* No built-in name is a skill or a style: `/bot add <name>` names. */
	for (const auto n : bot_default_names)
	{
		CHECK(!parse_skill(n));
		CHECK(!parse_style(n));
	}
}

void test_add()
{
	{
		const auto c{parse_command("/bot add")};
		CHECK(c.kind == command_kind::add);
		CHECK(!c.skill && !c.style && name_of(c).empty());
	}
	{
		const auto c{parse_command("/bot add ace")};
		CHECK(c.kind == command_kind::add);
		CHECK(c.skill == bot_skill::ace && !c.style && name_of(c).empty());
	}
	{
		const auto c{parse_command("/bot add Insane Aggressive Killer")};
		CHECK(c.kind == command_kind::add);
		CHECK(c.skill == bot_skill::insane && c.style == bot_style::aggressive);
		CHECK(name_of(c) == "killer");
	}
	{
		/* The style alone, the name alone, skill and name. */
		const auto st{parse_command("/bot add cautious")};
		CHECK(st.kind == command_kind::add && !st.skill && st.style == bot_style::cautious && name_of(st).empty());
		const auto nm{parse_command("/bot add rook")};
		CHECK(nm.kind == command_kind::add && !nm.skill && !nm.style && name_of(nm) == "rook");
		const auto sn{parse_command("/bot add trainee dummy")};
		CHECK(sn.kind == command_kind::add && sn.skill == bot_skill::trainee && !sn.style && name_of(sn) == "dummy");
	}
	{
		/* The order is skill, style, name: a style before a skill puts
		 * the skill word in the name's place, and no bot is called by a
		 * skill or style word: an error that says the order, not a bot
		 * named "ace".
		 */
		for (const auto line : {"/bot add aggressive ace", "/bot add col ace", "/bot add Coll INSANE", "/bot add ace ace", "/bot add ace col hot", "/bot add col bal", "/bot add cautious caut", "/bot add aggressive ace havoc"})
		{
			const auto c{parse_command(line)};
			CHECK(c.kind == command_kind::error);
			CHECK(c.error && std::string_view(c.error).find("Skill, then style") != std::string_view::npos);
			CHECK(name_of(c).empty());
		}
		/* Nor by `all` or a command word. */
		for (const auto line : {"/bot add all", "/bot add ALL", "/bot add ace all", "/bot add ace col all", "/bot add add", "/bot add remove", "/bot add rm", "/bot add kick", "/bot add skill", "/bot add style", "/bot add list", "/bot add save", "/bot add help", "/bot add bot"})
		{
			const auto c{parse_command(line)};
			CHECK(c.kind == command_kind::error);
			CHECK(c.error && std::string_view(c.error) == BOT_NAME_RESERVED_TEXT);
		}
		/* A name that only begins like one is a name. */
		CHECK(name_of(parse_command("/bot add acer")) == "acer");
		CHECK(name_of(parse_command("/bot add ace col ally")) == "ally");
		CHECK(name_of(parse_command("/bot add hotdog")) == "hotdog");
		CHECK(name_of(parse_command("/bot add adder")) == "adder");
		CHECK(name_of(parse_command("/bot add bots")) == "bots");
		CHECK(parse_command("/bot add ace bal one two").kind == command_kind::error);
	}
	/* Names: 8 characters, one word of letters, digits, - and _. */
	CHECK(name_of(parse_command("/bot add 12345678")) == "12345678");
	CHECK(parse_command("/bot add 123456789").kind == command_kind::error);
	CHECK(parse_command("/bot add a:b").kind == command_kind::error);
	CHECK(parse_command("/bot add <x>").kind == command_kind::error);
	CHECK(name_of(parse_command("/bot add My_Bot-1")) == "my_bot-1");
}

void test_sanitize()
{
	CHECK(std::string_view(sanitize_name("Havoc").data()) == "havoc");
	CHECK(std::string_view(sanitize_name("a b: c").data()) == "abc");
	CHECK(std::string_view(sanitize_name("123456789abc").data()) == "12345678");
	CHECK(std::string_view(sanitize_name(": ").data()).empty());
	CHECK(std::string_view(sanitize_name("").data()).empty());
}

void test_reserved_names()
{
	/* `all`, the skills and styles as /bot reads them, the commands. */
	for (const auto w : {"all", "All", "trainee", "rookie", "hotshot", "ace", "insane", "tra", "roo", "hot", "ins", "INS", "balanced", "aggressive", "cautious", "collector", "bal", "agg", "cau", "col", "aggr", "caut", "coll", "Coll", "add", "remove", "rm", "kick", "skill", "style", "list", "save", "help", "bot"})
	{
		CHECK(name_reserved(w));
		CHECK(!usable_name(w)[0]);
	}
	for (const auto w : {"", "rook", "acer", "ally", "al", "colt", "balance", "insaner", "kicker", "bots", "x"})
		CHECK(!name_reserved(w));
	/* No built-in name is reserved. */
	for (const auto n : bot_default_names)
	{
		CHECK(!name_reserved(n));
		CHECK(usable_name(n)[0]);
	}
	/* What the Bots screen's name field gives: cleaned first, so a
	 * reserved word with other characters around it is caught too.
	 */
	CHECK(std::string_view(usable_name("Havoc").data()) == "havoc");
	CHECK(std::string_view(usable_name("my bot").data()) == "mybot");
	CHECK(std::string_view(usable_name("a l l").data()).empty());
	CHECK(std::string_view(usable_name("A.C.E").data()).empty());
	CHECK(std::string_view(usable_name(": ").data()).empty());
	/* A reserved word in the place of a target is what it says: `all`
	 * is every bot, anything else names a bot by its beginning.
	 */
	CHECK(parse_command("/bot remove all").all);
	CHECK(!parse_command("/bot remove ace").all);
	CHECK(name_of(parse_command("/bot remove ace")) == "ace");
}

void test_remove_skill_style()
{
	{
		const auto c{parse_command("/bot remove Havoc")};
		CHECK(c.kind == command_kind::remove && !c.all && name_of(c) == "havoc");
	}
	{
		const auto c{parse_command("/bot remove all")};
		CHECK(c.kind == command_kind::remove && c.all);
		CHECK(parse_command("/bot rm ALL").all);
	}
	CHECK(parse_command("/bot remove").kind == command_kind::error);
	CHECK(parse_command("/bot remove havoc now").kind == command_kind::error);
	{
		const auto c{parse_command("/bot skill havoc insane")};
		CHECK(c.kind == command_kind::skill && !c.all && name_of(c) == "havoc" && c.skill == bot_skill::insane && !c.style);
	}
	{
		const auto c{parse_command("/bot skill all tra")};
		CHECK(c.kind == command_kind::skill && c.all && c.skill == bot_skill::trainee);
	}
	CHECK(parse_command("/bot skill havoc").kind == command_kind::error);
	CHECK(parse_command("/bot skill havoc godlike").kind == command_kind::error);
	CHECK(parse_command("/bot skill havoc aggressive").kind == command_kind::error);
	CHECK(parse_command("/bot skill").kind == command_kind::error);
	CHECK(parse_command("/bot skill all ace now").kind == command_kind::error);
	{
		const auto c{parse_command("/bot style all collector")};
		CHECK(c.kind == command_kind::style && c.all && c.style == bot_style::collector && !c.skill);
	}
	{
		const auto c{parse_command("/bot style sparky Caut")};
		CHECK(c.kind == command_kind::style && name_of(c) == "sparky" && c.style == bot_style::cautious);
	}
	CHECK(parse_command("/bot style sparky ace").kind == command_kind::error);
	CHECK(parse_command("/bot style sparky").kind == command_kind::error);
	/* Every error says why. */
	for (const auto line : {"/bot remove", "/bot skill x", "/bot style x y", "/bot skill x y", "/bot nonsense", "/bot add 123456789"})
	{
		const auto c{parse_command(line)};
		CHECK(c.kind == command_kind::error && c.error && c.error[0]);
	}
}

void test_target()
{
	constexpr std::array<std::string_view, 4> names{{"havoc", "havoc2", "sparky", "rook"}};
	const std::span<const std::string_view> all{names};
	/* The exact name wins over the prefix it is of another. */
	CHECK(find_target(all, "havoc").result == target_result::found);
	CHECK(find_target(all, "havoc").index == 0);
	CHECK(find_target(all, "HAVOC2").index == 1);
	CHECK(find_target(all, "hav").result == target_result::ambiguous);
	CHECK(find_target(all, "sp").result == target_result::found);
	CHECK(find_target(all, "sp").index == 2);
	CHECK(find_target(all, "r").index == 3);
	CHECK(find_target(all, "viper").result == target_result::none);
	CHECK(find_target(all, "").result == target_result::none);
	CHECK(find_target(all, "sparky1").result == target_result::none);
	CHECK(find_target(all.first(0), "havoc").result == target_result::none);
}

void test_add_verdict()
{
	const add_situation ok{.host = true, .mode_allowed = true, .playing = true, .countdown = false, .join_in_progress = false, .free_slot = true};
	CHECK(judge_add(ok) == add_verdict::ok);
	auto s{ok};
	s.host = false;
	CHECK(judge_add(s) == add_verdict::not_host);
	s = ok;
	s.mode_allowed = false;
	CHECK(judge_add(s) == add_verdict::wrong_mode);
	s = ok;
	s.playing = false;
	CHECK(judge_add(s) == add_verdict::not_playing);
	s = ok;
	s.countdown = true;
	CHECK(judge_add(s) == add_verdict::countdown);
	s = ok;
	s.free_slot = false;
	CHECK(judge_add(s) == add_verdict::full);
	s = ok;
	s.join_in_progress = true;
	CHECK(judge_add(s) == add_verdict::join_in_progress);
	/* A full game is full, whoever is joining. */
	s.free_slot = false;
	CHECK(judge_add(s) == add_verdict::full);
	/* A client is told it is not the host whatever else holds. */
	CHECK(judge_add({}) == add_verdict::not_host);
	for (const auto v : {add_verdict::not_host, add_verdict::wrong_mode, add_verdict::not_playing, add_verdict::countdown, add_verdict::join_in_progress, add_verdict::full})
		CHECK(add_verdict_text(v)[0]);
	CHECK(!add_verdict_text(add_verdict::ok)[0]);
}

/* The host's slots as the admission sees them (net_v2.cpp
 * build_slot_views): the rule a bot added during the game shares with a
 * joining human.
 */
using views = std::array<nv::slot_view, 8>;

[[nodiscard]]
nv::slot_view human(const bool connected, const nv::net_clock last = 0)
{
	nv::slot_view v{};
	v.occupied = true;
	v.connected = connected;
	v.last_packet_time = last;
	return v;
}

[[nodiscard]]
nv::slot_view bot(const bool playing, const unsigned order)
{
	nv::slot_view v{};
	v.occupied = true;
	v.connected = playing;
	v.bot = true;
	v.bot_order = order;
	return v;
}

void test_slot_rules()
{
	/* Host and one human, limit 4: a bot takes the lowest free slot. */
	{
		views v{};
		v[0] = human(true);
		v[1] = human(true);
		CHECK(nv::free_admission_slot(v, 4) == 2u);
		v[2] = bot(true, 1);
		CHECK(nv::free_admission_slot(v, 4) == 3u);
		v[3] = bot(true, 2);
		/* The limit holds for humans and bots together. */
		CHECK(!nv::free_admission_slot(v, 4));
		CHECK(nv::free_admission_slot(v, 5) == 4u);
	}
	/* A departed bot's slot is as good as free; a departed human's is
	 * kept for their return, so a bot is not added into it.
	 */
	{
		views v{};
		v[0] = human(true);
		v[1] = human(false, 100);
		v[2] = bot(false, 1);
		v[3] = human(true);
		CHECK(nv::free_admission_slot(v, 4) == 2u);
		v[2] = bot(true, 2);
		CHECK(!nv::free_admission_slot(v, 4));
	}
	/* A slot with a connection (a human joining) is connected: not
	 * taken by a bot added at that moment.
	 */
	{
		views v{};
		v[0] = human(true);
		v[1] = bot(false, 1);
		v[1].connected = true;	/* a joiner's peer in the departed bot's slot */
		v[1].bot = false;
		CHECK(nv::free_admission_slot(v, 2) == std::nullopt);
	}
}

void test_added_order_and_replacement()
{
	/* The game started with two bots (orders 1, 2); the host adds one
	 * during the game.
	 */
	constexpr std::array<unsigned, 8> orders{{0, 0, 1, 2, 0, 0, 0, 0}};
	const auto added{next_added_order(orders)};
	CHECK(added == 3);
	CHECK(next_added_order(std::span<const unsigned>{}) == 1);
	/* After the first bot was removed, a new one is still the newest. */
	constexpr std::array<unsigned, 8> after_removal{{0, 0, 0, 2, 3, 0, 0, 0}};
	CHECK(next_added_order(after_removal) == 4);

	views v{};
	v[0] = human(true);
	v[1] = human(true);
	v[2] = bot(true, 1);
	v[3] = bot(true, 2);
	/* The new bot went into the lowest free slot... */
	const auto slot{nv::free_admission_slot(v, 5)};
	CHECK(slot == 4u);
	v[*slot] = bot(true, added);
	/* ...and the game is full: no further bot... */
	CHECK(!nv::free_admission_slot(v, 5));
	/* ...and a human who joins replaces the bot added last, */
	{
		const auto d{nv::decide_admission(v, 5, false, true)};
		CHECK(d.result == nv::admission_result::accept_replace_bot);
		CHECK(d.slot == 4);
	}
	/* unless the host turned the option off in the game: full. */
	CHECK(nv::decide_admission(v, 5, false, false).result == nv::admission_result::deny_full);
	/* The host removed the first bot: its slot is free for a human
	 * without anyone leaving, and for a new bot.
	 */
	v[2] = bot(false, 1);
	{
		const auto d{nv::decide_admission(v, 5, false, true)};
		CHECK(d.result == nv::admission_result::accept_new);
		CHECK(d.slot == 2);
	}
	CHECK(nv::free_admission_slot(v, 5) == 2u);
	/* A bot added into it is the newest again. */
	v[2] = bot(true, 4);
	{
		const auto d{nv::decide_admission(v, 5, false, true)};
		CHECK(d.result == nv::admission_result::accept_replace_bot);
		CHECK(d.slot == 2);
	}
	/* With a disconnected human and bots in a full game, the bot goes
	 * first (the human can still come back), and no bot can be added.
	 */
	v[1] = human(false, 50);
	CHECK(!nv::free_admission_slot(v, 5));
	CHECK(nv::decide_admission(v, 5, false, true).result == nv::admission_result::accept_replace_bot);
}

}

int main()
{
	test_not_a_command();
	test_help_and_list();
	test_skill_and_style_words();
	test_add();
	test_sanitize();
	test_reserved_names();
	test_remove_skill_style();
	test_target();
	test_add_verdict();
	test_slot_rules();
	test_added_order_and_replacement();
	std::puts("bot_command: all tests passed");
	return 0;
}
