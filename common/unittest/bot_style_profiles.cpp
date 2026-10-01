/*
 * This file is part of the DXX-Rebirth project <https://github.com/dxx-rebirth/dxx-rebirth/>.
 * It is copyright by its individual contributors, as recorded in the
 * project's Git history.  See COPYING.txt at the top level for license
 * terms and a link to the Git history.
 */
/*
 * Test of the bots that fly a style profile (Documentation/
 * multiplayer-bots.md section 9.13, Documentation/movement-recording.md
 * section 8.7): a real profile (the one movrec-analyse made of a strong
 * human's recordings of 2026-09-30) parsed and applied, every key
 * reaching the bot's parameters and the rules that read them; the
 * library of the `botstyles/` folder with names, words, limits and
 * untrusted files; the style profile in the pilot's netgame profile;
 * the `/bot` chat command with a profile's word.
 *
 * Build and run with SCons:
 *
 *	scons sdl2=1 d1x=0 d2x=1 register_runtime_test_plain_link_targets=1 test-bot-style-profiles
 *	build/common/test-bot-style-profiles [-f FILE.botstyle]
 *
 * `-f` also parses and applies FILE and prints the bot it makes.
 */

#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

#include "bot_brain.h"
#include "bot_command.h"
#include "bot_goals.h"
#include "bot_profile.h"
#include "bot_style_library.h"
#include "bot_style_profile.h"
#include "bot_weapons.h"

using namespace dcx::bot;

namespace {

void check_failed(const char *const expr, const char *const file, const int line)
{
	std::fprintf(stderr, "%s:%d: check failed: %s\n", file, line, expr);
	std::exit(1);
}

#define CHECK(cond)	do { if (!(cond)) check_failed(#cond, __FILE__, __LINE__); } while (0)

[[nodiscard]]
bool close_to(const double a, const double b, const double eps = 1e-4)
{
	return std::abs(a - b) <= eps;
}

/* the profile of a strong human player (callsign EC), as movrec-analyse wrote it:
 * two evenings, 27.7 minutes alive, controls exact.
 */
constexpr std::string_view ec{
	"# D2X-Rebirth bot style profile (Documentation/movement-recording.md, section 8.5)\n"
	"format = 1\n"
	"name = EC style\n"
	"callsign = EC\n"
	"source = 2 games, 27.7 min alive, 13.9 min in fights, controls 100% exact\n"
	"base_skill = Hotshot\n"
	"base_style = Balanced\n"
	"\n"
	"# weight of fighting against everything else\n"
	"style.engage_weight = 0.94701\n"
	"# weight of collecting powerups\n"
	"style.collect_weight = 1.3803\n"
	"# scale of the fight distance (1 = 35 to 95 units)\n"
	"style.range_scale = 0.97369\n"
	"# scale of how long a lost target is hunted\n"
	"style.chase_memory = 0.4\n"
	"# scale of the speed closing in and backing off\n"
	"style.close_scale = 0.86991\n"
	"# afterburner when chasing a target further than this\n"
	"style.burn_chase_distance = 1000\n"
	"\n"
	"# 1: strafes while fighting\n"
	"skill.strafe = 1\n"
	"# shortest run of the strafe in one direction\n"
	"skill.strafe_min_ms = 200\n"
	"# longest run of the strafe in one direction\n"
	"skill.strafe_max_ms = 700\n"
	"# vertical share of the strafe (0 flat, 1 as much as sideways)\n"
	"skill.strafe_vertical = 0.33086\n"
	"# strafe speed, share of the top speed\n"
	"skill.strafe_speed = 0.9\n"
	"\n"
	"# near edge of the fight band (BOT_RANGE_LO, 35)\n"
	"tune.range_lo = 41.322\n"
	"# far edge of the fight band (BOT_RANGE_HI, 95)\n"
	"tune.range_hi = 96.668\n"
	"# how long a target is followed after it left the sight (pursuit_seconds)\n"
	"tune.pursuit_seconds = 0\n"
	"# share of the time fleeing with the afterburner\n"
	"tune.burn_retreat = 0.21436\n"
	"# share of the time with no enemy in sight with the afterburner\n"
	"tune.burn_roam = 0.057\n"
	"# share of the large turns flown backwards (bots: every one with room)\n"
	"tune.reverse_turn = 0.1087\n"
	"# backward speed in such a turn, share of the top speed (REVERSE_TURN_SPEED, 0.8)\n"
	"tune.reverse_turn_speed = 0.71203\n"
	"# share of the large turns followed by a push forward (TURN_BOOST_TICKS)\n"
	"tune.turn_boost = 0.74157\n"
	"# share of those pushes with the afterburner\n"
	"tune.turn_boost_burn = 0.25758\n"
	"# scale of the time between two missile volleys (missile_interval_scale)\n"
	"tune.missile_interval_scale = 1.3932\n"
	"# missiles per volley\n"
	"tune.volley_size = 1.9259\n"
	"# share of the pickups taken by leaving the course (grab_is_detour)\n"
	"tune.grab_detour = 0.35807\n"
	"\n"
	"measured.alive_minutes = 27.651\n"
	"measured.fight_minutes = 13.913\n"
	"measured.estimated_controls_share = 0\n"
	"measured.speed_mean = 51.788\n"
	"measured.enemy_distance_median = 78.372\n"
	"measured.turn_180_ms = 1632.9\n"
	"measured.afterburner_share = 0.061964\n"
	"measured.hits_per_shot = 0.13531\n"
};

void print_bot(const char *const what, const style_profile_params &b)
{
	std::printf("test-bot-style-profiles: %s: strafe %d %u-%u ms vertical %.2f thrust %.2f dodge %.2f | retreat %.0f engage %.2f collect %.2f range x%.2f chase x%.2f close x%.2f burn chase %.0f | band %.1f-%.1f turns reverse %.2f at %.2f push %.2f burn %.2f | flee burn %.2f roam burn %.2f missiles x%.2f volley %.2f pursuit %.1f s detour x%.2f\n",
		what, b.skill.strafe, b.skill.strafe_min_ms, b.skill.strafe_max_ms, b.skill.strafe_vertical, b.skill.strafe_speed, b.skill.dodge_prob,
		b.style.retreat_shields, b.style.engage_weight, b.style.collect_weight, b.style.range_scale, b.style.chase_memory, b.style.close_scale, b.style.burn_chase_distance,
		b.tune.range_lo, b.tune.range_hi, b.tune.turns.reverse, b.tune.turns.reverse_speed, b.tune.turns.boost, b.tune.turns.boost_burn,
		b.tune.flee_burn, b.tune.roam_burn, b.tune.missile_interval_scale, b.tune.volley_size, b.tune.pursuit_seconds, b.tune.grab_detour_scale);
}

/* The real profile, parsed, and a Hotshot bot that flies it. */
void test_real_profile()
{
	const auto p{parse_style_profile(ec)};
	CHECK(p);
	CHECK(p->name == "EC style" && p->callsign == "EC");
	CHECK(p->base_skill == bot_skill::hotshot && p->base_style == bot_style::balanced);
	/* 6 style, 5 skill, 12 tune, 8 measured keys. */
	CHECK(p->entries.size() == 31);
	CHECK(close_to(*p->value("tune.range_lo"), 41.322) && close_to(*p->value("skill.strafe_vertical"), 0.33086));
	/* The file's own text survives a round trip. */
	CHECK(parse_style_profile(write_style_profile(*p)) == p);

	const auto base_skill{skill_of(bot_skill::hotshot)};
	const auto balanced{style_of(bot_style::balanced)};
	const auto b{apply_style_profile(*p, bot_skill::hotshot)};
	print_bot("EC style, Hotshot", b);
	/* skill.*: the strafe. */
	CHECK(b.skill.strafe);
	CHECK(b.skill.strafe_min_ms == 200 && b.skill.strafe_max_ms == 700);
	CHECK(close_to(b.skill.strafe_vertical, 0.33086) && close_to(b.skill.strafe_speed, 0.9));
	/* Not in the profile: the skill's own. */
	CHECK(b.skill.dodge_prob == base_skill.dodge_prob && b.skill.aim_sigma_deg == base_skill.aim_sigma_deg && b.skill.turn_cap == base_skill.turn_cap);
	/* style.* */
	CHECK(close_to(b.style.engage_weight, 0.94701) && close_to(b.style.collect_weight, 1.3803));
	CHECK(close_to(b.style.chase_memory, 0.4) && close_to(b.style.close_scale, 0.86991) && b.style.burn_chase_distance == 1000);
	CHECK(b.style.retreat_shields == balanced.retreat_shields && b.style.dodge_bonus == balanced.dodge_bonus && b.style.mine_interval == balanced.mine_interval);
	/* The band is the profile's own: the range scale is not applied on
	 * top of it.
	 */
	CHECK(b.style.range_scale == 1);
	/* tune.* */
	CHECK(close_to(b.tune.range_lo, 41.322) && close_to(b.tune.range_hi, 96.668));
	CHECK(close_to(b.tune.turns.reverse, 0.1087) && close_to(b.tune.turns.reverse_speed, 0.71203));
	CHECK(close_to(b.tune.turns.boost, 0.74157) && close_to(b.tune.turns.boost_burn, 0.25758));
	CHECK(close_to(b.tune.flee_burn, 0.21436 / FLEE_BURN_FULL_TIME));
	CHECK(b.tune.roam_burn == 1);
	CHECK(close_to(b.tune.missile_interval_scale, 1.3932) && close_to(b.tune.volley_size, 1.9259));
	CHECK(b.tune.pursuit_seconds == 0);
	CHECK(close_to(b.tune.grab_detour_scale, 0.35807 / GRAB_DETOUR_BASE_SHARE));

	/* And the rules that read them. */
	/* The strafe's thrust, the closing thrust, the hunt's memory. */
	CHECK(close_to(effective_strafe_speed(b.skill, b.style), 0.9));
	CHECK(close_to(effective_close_speed(b.style), COMBAT_CLOSE_SPEED * 0.86991));
	CHECK(effective_memory_ms(b.skill, b.style) == static_cast<unsigned>(base_skill.memory_ms * 0.4 + 0.5));
	/* It lets a lost enemy go (tune.pursuit_seconds 0), where a Hotshot
	 * Balanced bot pursues one it just hit.
	 */
	{
		pursuit_view v{.skill = bot_skill::hotshot, .style = bot_style::balanced, .since_engaged = 0.2, .since_hit = 1};
		CHECK(pursuit_start(v) == pursuit_reason::hits);
		v.seconds = b.tune.pursuit_seconds;
		CHECK(pursuit_start(v) == pursuit_reason::none);
		v.seconds = 3;
		CHECK(pursuit_start(v) == pursuit_reason::hits);
		CHECK(!pursuit_stop(v, 2.9) && pursuit_stop(v, 3.1) == pursuit_end::expired);
	}
	/* Volleys of two (1.93 rounded), where Hotshot Balanced's table
	 * says 2 too; and a profile of 4 gets 4.
	 */
	{
		volley_view v{.s = secondary::concussion, .smarts = 2, .ammo = 8, .target_visible = true, .shot_clear = true, .target_distance = 80};
		CHECK(volley_size(v) == 2);
		v.mean = b.tune.volley_size;
		CHECK(volley_size(v) == 2);
		v.mean = 4;
		CHECK(volley_size(v) == 4);
		v.ammo = 3;
		CHECK(volley_size(v) == 3);
	}
	/* The time between volleys: 1.39 times the skill's. */
	CHECK(close_to(missile_interval(base_skill.weapon_smarts) * b.tune.missile_interval_scale, 2.5 * 1.3932));
	/* A grab in a fight: a shorter detour than the default's. */
	CHECK(grab_is_detour(1, GRAB_DETOUR_PATH, false));
	CHECK(!grab_is_detour(1, GRAB_DETOUR_PATH, false, b.tune.grab_detour_scale));
	CHECK(grab_is_detour(1, GRAB_DETOUR_PATH * 0.75, false, b.tune.grab_detour_scale));
	/* The turn habits drive the turns (their shares). */
	{
		turn_round_state tr;
		bot_rng rng{9};
		unsigned reverse{0}, boost{0};
		constexpr unsigned n{20000};
		for (unsigned i = 0; i < n; ++i)
		{
			tr.reset();
			reverse += tr.update(radians(170), 1, 100, 41, rng, b.tune.turns) == turn_phase::reversing;
			boost += tr.update(radians(10), 60, 100, 41, rng, b.tune.turns) == turn_phase::boost;
		}
		CHECK(std::abs(reverse / double{n} - 0.1087) < 0.02);
		CHECK(std::abs(boost / double{n} - 0.74157) < 0.02);
	}

	/* The same profile on the other skills: the skill keeps its aim and
	 * senses; a Trainee still does not strafe or dodge.
	 */
	for (unsigned i = 0; i < BOT_SKILL_COUNT; ++i)
	{
		const auto k{static_cast<bot_skill>(i)};
		const auto a{apply_style_profile(*p, k)};
		CHECK(a.skill.aim_sigma_deg == skill_of(k).aim_sigma_deg && a.skill.reaction_ms == skill_of(k).reaction_ms);
		CHECK(a.skill.strafe == skill_of(k).strafe);
		CHECK(close_to(a.tune.range_lo, 41.322));
	}
	CHECK(!apply_style_profile(*p, bot_skill::trainee).skill.strafe);

	/* Lower confidence goes part of the way. */
	{
		auto q{*p};
		for (auto &e : q.entries)
			if (e.key == "tune.turn_boost")
				e.confidence = style_confidence::low;
		const auto c{apply_style_profile(q, bot_skill::hotshot)};
		const turn_habits d{};
		CHECK(close_to(c.tune.turns.boost, d.boost + 0.25 * (0.74157 - d.boost)));
	}
	/* A built-in style's bot: the code's constants. */
	{
		style_profile empty;
		const auto c{apply_style_profile(empty, bot_skill::hotshot)};
		const tune_params d{};
		CHECK(c.tune.range_lo == d.range_lo && c.tune.range_hi == d.range_hi && c.tune.flee_burn == d.flee_burn && c.tune.roam_burn == 1);
		CHECK(c.tune.missile_interval_scale < 0 && c.tune.volley_size < 0 && c.tune.pursuit_seconds < 0 && c.tune.grab_detour_scale == 1);
		CHECK(c.style.range_scale == balanced.range_scale);
	}
	/* One edge alone: the band is the profile's too, never scaled. */
	{
		const auto q{parse_style_profile("format = 1\nstyle.range_scale = 3\ntune.range_lo = 400\n")};
		CHECK(q);
		const auto c{apply_style_profile(*q, bot_skill::hotshot)};
		CHECK(c.style.range_scale == 1 && c.tune.range_lo == 400 && c.tune.range_hi == 410);
		const auto r{parse_style_profile("format = 1\nstyle.range_scale = 3\n")};
		CHECK(apply_style_profile(*r, bot_skill::hotshot).style.range_scale == 3);
	}
}

/* Every value a file can give stays in its range, however bad the file. */
void check_sane(const style_profile_params &b)
{
	const auto in{[](const double v, const double lo, const double hi) {
		return std::isfinite(v) && v >= lo && v <= hi;
	}};
	CHECK(b.skill.strafe_min_ms <= b.skill.strafe_max_ms && b.skill.strafe_max_ms <= 5000);
	CHECK(in(b.skill.strafe_vertical, 0, 1) && in(b.skill.strafe_speed, 0, 1) && in(b.skill.dodge_prob, 0, 0.95));
	CHECK(in(b.style.retreat_shields, 5, 90) && in(b.style.engage_weight, 0.5, 1.8) && in(b.style.range_scale, 0.5, 3));
	CHECK(in(b.style.burn_chase_distance, 40, 1000) && in(b.style.chase_memory, 0.4, 2.5));
	CHECK(in(b.tune.range_lo, 15, 400) && in(b.tune.range_hi, b.tune.range_lo + 10, 810));
	/* A band of the profile's is never scaled. */
	CHECK(b.style.range_scale == 1 || (b.tune.range_lo == 35 && b.tune.range_hi == 95));
	CHECK(in(b.tune.turns.reverse, 0, 1) && in(b.tune.turns.reverse_speed, 0, 1) && in(b.tune.turns.boost, 0, 1) && in(b.tune.turns.boost_burn, 0, 1));
	CHECK(in(b.tune.flee_burn, 0, 1) && in(b.tune.roam_burn, 0, 1) && in(b.tune.grab_detour_scale, 0.25, 3));
	CHECK(b.tune.missile_interval_scale < 0 || in(b.tune.missile_interval_scale, 0.3, 4));
	CHECK(b.tune.volley_size < 0 || in(b.tune.volley_size, 1, 8));
	CHECK(b.tune.pursuit_seconds < 0 || in(b.tune.pursuit_seconds, 0, 30));
}

/* Files from anywhere: the parser and the library take anything. */
void test_untrusted()
{
	/* The review of PR #64: a byte order mark before `format` (a
	 * hand-edited file), and a backward turn at no speed (a bot that
	 * stood still while its nose came round).
	 */
	{
		const auto p{parse_style_profile(
			"\xef\xbb\xbf" "format = 1\r\n"
			"tune.reverse_turn_speed = 0\r\n")};
		CHECK(p);
		const auto b{apply_style_profile(*p, bot_skill::ace)};
		CHECK(close_to(b.tune.turns.reverse_speed, 0.3));
	}
	/* Out of range, not numbers, not finite. */
	{
		const auto p{parse_style_profile(
			"format = 1\n"
			"style.retreat_shields = 5000\n"
			"style.engage_weight = nan\n"
			"style.collect_weight = inf\n"
			"style.range_scale = 1e999\n"
			"skill.strafe_min_ms = 99999\n"
			"skill.strafe_max_ms = -5\n"
			"tune.range_lo = 390\n"
			"tune.range_hi = 20\n"
			"tune.volley_size = 0\n"
			"tune.pursuit_seconds = -3\n"
			"tune.burn_roam = 12\n"
			"tune.grab_detour = 0\n"
			"confidence.tune.burn_roam = bogus\n"
			"base_style = nonsense\n"
			"= 3\n"
			"style. = 4\n"
			"no equals sign\n")};
		CHECK(p);
		CHECK(!p->find("style.engage_weight") && !p->find("style.collect_weight") && !p->find("style.range_scale"));
		CHECK(*p->value("style.retreat_shields") == 90);
		CHECK(p->base_style == bot_style::balanced);
		for (unsigned i = 0; i < BOT_SKILL_COUNT; ++i)
		{
			const auto b{apply_style_profile(*p, static_cast<bot_skill>(i))};
			check_sane(b);
			CHECK(b.tune.range_hi >= b.tune.range_lo + 10);
			CHECK(b.tune.volley_size >= 1 && b.tune.pursuit_seconds == 0 && b.tune.roam_burn == 1 && b.tune.grab_detour_scale == 0.25);
		}
	}
	/* Random bytes, random lines of known keys with random values. */
	{
		uint32_t x{2463534242u};
		const auto next{[&x] {
			x ^= x << 13;
			x ^= x >> 17;
			x ^= x << 5;
			return x;
		}};
		for (unsigned round = 0; round < 3000; ++round)
		{
			std::string text{round % 3 ? "format = 1\n" : ""};
			const unsigned lines{next() % 40};
			for (unsigned l = 0; l < lines; ++l)
			{
				if (next() % 4 == 0)
				{
					/* Noise, NULs and high bytes included. */
					const unsigned len{next() % 80};
					for (unsigned c = 0; c < len; ++c)
						text += static_cast<char>(next() & 0xff);
				}
				else
				{
					const auto &k{style_profile_keys[next() % style_profile_keys.size()]};
					text += std::string{k.key} + " = ";
					char v[32];
					std::snprintf(v, sizeof(v), "%.6g", (static_cast<double>(next() % 20001) - 10000) / (1 + next() % 100));
					text += v;
					if (next() % 5 == 0)
						text += "\nconfidence." + std::string{k.key} + " = " + style_confidence_names[next() % 3];
				}
				text += '\n';
			}
			const auto p{parse_style_profile(text)};
			if (round % 3 == 0)
				continue;
			if (!p)
				continue;
			for (const auto &e : p->entries)
				if (const auto k{find_style_profile_key(e.key)})
					CHECK(e.value >= k->lo && e.value <= k->hi);
			check_sane(apply_style_profile(*p, static_cast<bot_skill>(next() % BOT_SKILL_COUNT)));
			style_library lib;
			const auto r{lib.add("random", text)};
			CHECK(r == style_add_result::added || r == style_add_result::not_a_profile || r == style_add_result::too_large);
			if (r == style_add_result::added)
			{
				const std::string_view name{lib[0].name.data()};
				CHECK(!name.empty() && name.size() <= BOT_STYLE_NAME_LEN);
				for (const char c : name)
					CHECK(c >= 0x20 && c < 0x7f && c != '=' && c != ',');
			}
		}
	}
}

void test_library()
{
	style_library lib;
	CHECK(lib.add("EC", ec) == style_add_result::added);
	CHECK(lib.size() == 1);
	CHECK(std::string_view(lib[0].name.data()) == "EC style");
	CHECK(std::string_view(lib[0].word.data()) == "ec");
	CHECK(lib.find("Ec Style") == &lib[0] && lib.index_of("EC style") == 0);
	CHECK(lib.find_word("ec") == &lib[0]);
	CHECK(!lib.find("nobody style") && lib.index_of("") == lib.size());
	/* The same name again: the first file wins. */
	CHECK(lib.add("EC2", ec) == style_add_result::duplicate);
	/* Not a profile; too large. */
	CHECK(lib.add("x", "name = x\n") == style_add_result::not_a_profile);
	CHECK(lib.add("x", std::string(STYLE_FILE_MAX_BYTES + 1, '#')) == style_add_result::too_large);
	/* Without a name: "<callsign> style"; without a callsign: the file's. */
	CHECK(lib.add("nico-file", "format = 1\ncallsign = Nico\n") == style_add_result::added);
	CHECK(std::string_view(lib[1].name.data()) == "Nico style" && std::string_view(lib[1].word.data()) == "nico");
	CHECK(lib.add("brick", "format = 1\n") == style_add_result::added);
	CHECK(std::string_view(lib[2].name.data()) == "brick style" && std::string_view(lib[2].word.data()) == "brick");
	/* A name cut, cleaned to printable ASCII (one '?' per UTF-8 letter,
	 * no '=' or ','); a callsign that is a /bot word gives no word.
	 */
	CHECK(lib.add("a", "format = 1\nname = J\xc3\xbcrgen, the = \x01 very long name of a style beyond\ncallsign = ace\n") == style_add_result::added);
	CHECK(std::string_view(lib[3].name.data()) == "J?rgen  the     very long name");
	CHECK(!lib[3].word[0]);
	/* A word taken already: none. */
	CHECK(lib.add("b", "format = 1\nname = other\ncallsign = EC\n") == style_add_result::added);
	CHECK(!lib[4].word[0]);
	const auto words{lib.words()};
	CHECK(words.size() == 5 && words[0] == "ec" && words[3].empty());
	/* At most STYLE_LIBRARY_MAX. */
	for (unsigned i = lib.size(); i < STYLE_LIBRARY_MAX; ++i)
	{
		const auto text{"format = 1\nname = s" + std::to_string(i) + "\n"};
		CHECK(lib.add("s", text) == style_add_result::added);
	}
	CHECK(lib.add("one-more", "format = 1\nname = one more\n") == style_add_result::full);
	CHECK(style_choice_count(lib.size()) == BOT_STYLE_COUNT + STYLE_LIBRARY_MAX);
	lib.clear();
	CHECK(!lib.size() && style_choice_count(0) == BOT_STYLE_COUNT);
}

/* The pilot's netgame profile keeps a bot's style profile by name. */
void test_netgame_profile()
{
	bot_profile p;
	p.count = 3;
	p.default_skill = bot_skill::ace;
	p.default_style = bot_style::balanced;
	p.default_profile = make_style_name("EC style");
	std::strcpy(p.bots[0].name.data(), "ravager");
	p.bots[0].profile = make_style_name("EC style");
	std::strcpy(p.bots[1].name.data(), "havoc");
	p.bots[1].style = bot_style::cautious;
	std::strcpy(p.bots[2].name.data(), "sparky");
	p.bots[2].profile = make_style_name("a style with the longest name ok");
	std::array<profile_line, BOT_PROFILE_MAX_LINES> lines;
	const auto n{format_profile(p, lines)};
	CHECK(n == 3 + 3 + 3);
	profile_reader r;
	for (std::size_t i = 0; i < n; ++i)
	{
		const std::string_view line{lines[i].data()};
		/* The .ngp reader's lines hold 49 characters. */
		CHECK(line.size() < BOT_PROFILE_LINE_SIZE - 1);
		const auto eq{line.find('=')};
		CHECK(eq != std::string_view::npos);
		CHECK(r.parse(line.substr(0, eq), line.substr(eq + 1)));
	}
	const auto back{r.result()};
	CHECK(back == p);
	CHECK(std::string_view(back.bots[2].profile.data()) == "a style with the longest name o");
	/* The style lines before the bot lines, and for a bot beyond the
	 * count; an older file without them; a bot with no line takes the
	 * default's style profile.
	 */
	{
		profile_reader q;
		CHECK(q.parse("BotStyle1", "Nico style"));
		CHECK(q.parse("BotStyle9", "far away"));
		CHECK(q.parse("BotCount", "3"));
		CHECK(q.parse("BotDefault", "2,1"));
		CHECK(q.parse("BotDefaultStyle", " EC style "));
		CHECK(q.parse("Bot0", "a,2,0,0"));
		CHECK(q.parse("Bot1", "b,2,0,0"));
		CHECK(!q.parse("BotStylex", "x"));
		const auto g{q.result()};
		CHECK(!g.bots[0].profile[0]);
		CHECK(std::string_view(g.bots[1].profile.data()) == "Nico style");
		CHECK(std::string_view(g.bots[2].profile.data()) == "EC style" && g.bots[2].style == bot_style::aggressive);
		CHECK(std::string_view(g.default_profile.data()) == "EC style");
	}
	{
		profile_reader q;
		CHECK(q.parse("BotCount", "1"));
		CHECK(q.parse("Bot0", "a,2,3,0"));
		const auto g{q.result()};
		CHECK(!g.bots[0].profile[0] && !g.default_profile[0] && g.bots[0].style == bot_style::collector);
	}
	/* Save as default during a game: the style lines are bot lines. */
	{
		const auto text{replace_profile_bot_lines("Foo=1\nBotCount=1\nBot0=x,1,1,1\nBotStyle0=old\nNGPVersion=1\n", p, "NGPVersion")};
		CHECK(text.find("BotStyle0=EC style\n") != std::string::npos);
		CHECK(text.find("BotStyle0=old") == std::string::npos);
		CHECK(text.starts_with("Foo=1\nBotCount=3\n") && text.ends_with("NGPVersion=1\n"));
	}
}

/* `/bot` with a loaded profile's word in a style's place. */
void test_chat()
{
	const std::array<std::string_view, 3> words{{"EC", "", "nico"}};
	{
		const auto c{parse_command("/bot add hot EC", words)};
		CHECK(c.kind == command_kind::add && c.skill == bot_skill::hotshot && !c.style && c.profile == 0u && !c.name[0]);
	}
	{
		const auto c{parse_command("/bot add Nico brick", words)};
		CHECK(c.kind == command_kind::add && !c.skill && c.profile == 2u && std::string_view(c.name.data()) == "brick");
	}
	{
		/* After a built-in style, the word is a name. */
		const auto c{parse_command("/bot add ace bal EC", words)};
		CHECK(c.kind == command_kind::add && c.style == bot_style::balanced && !c.profile && std::string_view(c.name.data()) == "ec");
	}
	{
		const auto c{parse_command("/bot style all EC", words)};
		CHECK(c.kind == command_kind::style && c.all && c.profile == 0u && !c.style);
	}
	{
		const auto c{parse_command("/bot style havoc caut", words)};
		CHECK(c.kind == command_kind::style && c.style == bot_style::cautious && !c.profile);
	}
	CHECK(parse_command("/bot style all nobody", words).kind == command_kind::error);
	/* Without profiles, as before. */
	CHECK(parse_command("/bot add hot EC").kind == command_kind::add && std::string_view(parse_command("/bot add hot EC").name.data()) == "ec");
	CHECK(parse_command("/bot style all EC").kind == command_kind::error);
}

int test_file(const char *const path)
{
	std::ifstream in{path, std::ios::binary};
	if (!in)
	{
		std::fprintf(stderr, "cannot read %s\n", path);
		return 1;
	}
	const std::string text{std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
	style_library lib;
	const auto r{lib.add("file", text)};
	std::printf("test-bot-style-profiles: %s: %s\n", path, style_add_text(r));
	if (r != style_add_result::added)
		return 1;
	std::printf("test-bot-style-profiles: name \"%s\", word \"%s\", base %s\n", lib[0].name.data(), lib[0].word.data(), bot_style_names[static_cast<unsigned>(lib[0].profile.base_style)]);
	for (unsigned i = 0; i < BOT_SKILL_COUNT; ++i)
	{
		const auto b{apply_style_profile(lib[0].profile, static_cast<bot_skill>(i))};
		check_sane(b);
		print_bot(bot_skill_names[i], b);
	}
	return 0;
}

}

int main(const int argc, char **const argv)
{
	test_real_profile();
	test_untrusted();
	test_library();
	test_netgame_profile();
	test_chat();
	for (int i = 1; i + 1 < argc; ++i)
		if (!std::strcmp(argv[i], "-f") && test_file(argv[i + 1]))
			return 1;
	std::puts("test-bot-style-profiles: all checks passed");
	return 0;
}
