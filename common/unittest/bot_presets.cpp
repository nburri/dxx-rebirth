/*
 * This file is part of the DXX-Rebirth project <https://github.com/dxx-rebirth/dxx-rebirth/>.
 * It is copyright by its individual contributors, as recorded in the
 * project's Git history.  See COPYING.txt at the top level for license
 * terms and a link to the Git history.
 */
/*
 * Test of the bots' skill presets and styles and of the bot setup in the
 * pilot's netgame profile (Documentation/multiplayer-bots.md sections 5,
 * 6.5 and 9.7): every parameter is monotonic in difficulty from Trainee
 * to Insane, Trainee is easy and Insane is no aimbot, the styles change
 * what they should, a beginner's trigger pauses, and the profile's bot
 * lines round-trip.
 *
 * Build and run with SCons:
 *
 *	scons sdl2=1 d1x=0 d2x=1 register_runtime_test_plain_link_targets=1 test-bot-presets
 *	build/common/test-bot-presets
 */

#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>

#include "bot_brain.h"
#include "bot_goals.h"
#include "bot_profile.h"
#include "bot_weapons.h"

using namespace dcx::bot;

namespace {

void check_failed(const char *const expr, const char *const file, const int line)
{
	std::fprintf(stderr, "%s:%d: check failed: %s\n", file, line, expr);
	std::exit(1);
}

#define CHECK(cond)	do { if (!(cond)) check_failed(#cond, __FILE__, __LINE__); } while (0)

constexpr std::array<bot_skill, BOT_SKILL_COUNT> skills{{bot_skill::trainee, bot_skill::rookie, bot_skill::hotshot, bot_skill::ace, bot_skill::insane}};
constexpr std::array<bot_style, BOT_STYLE_COUNT> styles{{bot_style::balanced, bot_style::aggressive, bot_style::cautious, bot_style::collector}};

/* Harder never means worse, for every parameter of the skill (strict
 * where the design's table changes at every step).
 */
void test_skill_monotonic()
{
	for (unsigned i = 1; i < BOT_SKILL_COUNT; ++i)
	{
		const auto &a{skill_table[i - 1]}, &b{skill_table[i]};
		CHECK(b.reaction_ms < a.reaction_ms);
		CHECK(b.aim_sigma_deg < a.aim_sigma_deg);
		CHECK(b.aim_drift_ms <= a.aim_drift_ms);
		CHECK(b.lead > a.lead);
		CHECK(b.turn_cap > a.turn_cap);
		CHECK(b.fire_cone_deg < a.fire_cone_deg);
		CHECK(b.fov_half_deg > a.fov_half_deg);
		CHECK(b.awareness > a.awareness);
		CHECK(b.hearing > a.hearing);
		CHECK(b.memory_ms > a.memory_ms);
		CHECK(b.dodge_prob > a.dodge_prob);
		CHECK(b.weapon_smarts > a.weapon_smarts);
		CHECK(b.map_knowledge > a.map_knowledge);
		CHECK(b.strafe >= a.strafe);
		CHECK(b.strafe_max_ms <= a.strafe_max_ms);
		CHECK(b.strafe_vertical >= a.strafe_vertical);
		CHECK(b.strafe_speed >= a.strafe_speed);
		CHECK(b.fire_duty >= a.fire_duty);
		/* The derived rules. */
		CHECK(static_cast<unsigned>(afterburner_of(skills[i])) > static_cast<unsigned>(afterburner_of(skills[i - 1])));
		CHECK(missile_interval(b.weapon_smarts) < missile_interval(a.weapon_smarts));
		CHECK(mine_interval(b.weapon_smarts) < mine_interval(a.weapon_smarts));
		CHECK(fusion_release_charge(b.weapon_smarts) > fusion_release_charge(a.weapon_smarts));
		CHECK(powerup_memory_ms(b) > powerup_memory_ms(a));
		for (const auto st : styles)
		{
			const auto ra{risk_profile_of(skills[i - 1], st)}, rb{risk_profile_of(skills[i], st)};
			CHECK(rb.self_budget >= ra.self_budget);
			CHECK(rb.self_chance >= ra.self_chance);
			CHECK(rb.trade <= ra.trade);
			CHECK(rb.hug >= ra.hug);
			CHECK(rb.indirect >= ra.indirect);
			const auto &sp{style_of(st)};
			CHECK(effective_dodge(b, sp) >= effective_dodge(a, sp));
			CHECK(effective_memory_ms(b, sp) > effective_memory_ms(a, sp));
			CHECK(effective_strafe_speed(b, sp) >= effective_strafe_speed(a, sp));
		}
		/* A secondary a skill fires, every harder skill fires. */
		for (unsigned s = 0; s < BOT_SECONDARY_COUNT; ++s)
			if (a.weapon_smarts >= min_smarts(static_cast<secondary>(s)))
				CHECK(b.weapon_smarts >= min_smarts(static_cast<secondary>(s)));
		/* The converter: a harder skill converts at least as early. */
		for (double shields = 0; shields < 200; shields += 5)
			if (want_convert(shields, 150, a.weapon_smarts))
				CHECK(want_convert(shields, 150, b.weapon_smarts));
	}
	/* Hotshot is the default, and what B1 to B4 played. */
	CHECK(&skill_of(BOT_DEFAULT_SKILL) == &skill_table[2]);
	CHECK(skill_of(bot_skill::hotshot).strafe_speed == 0.7);
	CHECK(skill_of(bot_skill::hotshot).fire_duty == 1.0);
	CHECK(effective_close_speed(style_of(bot_style::balanced)) == COMBAT_CLOSE_SPEED);
}

/* Trainee: a new player can win.  Insane: hard, but human. */
void test_skill_extremes()
{
	const auto &t{skill_of(bot_skill::trainee)};
	CHECK(t.reaction_ms >= 500);
	CHECK(t.aim_sigma_deg >= 6);
	CHECK(t.lead == 0);
	CHECK(t.turn_cap < 0.5);
	CHECK(t.dodge_prob == 0);
	CHECK(!t.strafe);
	CHECK(t.map_knowledge == 0);
	CHECK(t.fire_duty < 0.6);
	CHECK(afterburner_of(bot_skill::trainee) == afterburner_use::never);
	for (unsigned s = 0; s < BOT_SECONDARY_COUNT; ++s)
		CHECK(t.weapon_smarts < min_smarts(static_cast<secondary>(s)));
	CHECK(!want_convert(10, 200, t.weapon_smarts));
	/* No style makes a Trainee dodge, strafe or fire heavy missiles. */
	for (const auto st : styles)
	{
		CHECK(effective_dodge(t, style_of(st)) == 0);
		CHECK(effective_strafe_speed(t, style_of(st)) == 0);
		CHECK(risk_profile_of(bot_skill::trainee, st).hug == 0);
		CHECK(!risk_profile_of(bot_skill::trainee, st).indirect);
	}
	const auto &x{skill_of(bot_skill::insane)};
	/* A human-like reaction floor and a small aim error remain. */
	CHECK(x.reaction_ms >= 120 && x.reaction_ms <= 150);
	CHECK(x.aim_sigma_deg >= 0.8);
	CHECK(x.fire_cone_deg > 0);
	CHECK(x.turn_cap <= 1);
	for (const auto st : styles)
		CHECK(effective_dodge(x, style_of(st)) < 1);
	/* Every preset keeps a reaction time and an aim error. */
	for (const auto &s : skill_table)
	{
		CHECK(s.reaction_ms >= 120);
		CHECK(s.aim_sigma_deg > 0);
		CHECK(s.fire_duty > 0 && s.fire_duty <= 1);
		CHECK(s.strafe_speed >= 0 && s.strafe_speed < 1);
	}
}

/* What each style changes (section 5.2 and 9.7). */
void test_styles()
{
	const auto &bal{style_of(bot_style::balanced)};
	const auto &agg{style_of(bot_style::aggressive)};
	const auto &cau{style_of(bot_style::cautious)};
	const auto &col{style_of(bot_style::collector)};
	/* Retreat: aggressive late, cautious early. */
	CHECK(agg.retreat_shields < bal.retreat_shields);
	CHECK(bal.retreat_shields < col.retreat_shields);
	CHECK(col.retreat_shields < cau.retreat_shields);
	/* Engage and collect weights. */
	CHECK(agg.engage_weight > bal.engage_weight && bal.engage_weight > cau.engage_weight && cau.engage_weight > col.engage_weight);
	CHECK(col.collect_weight > cau.collect_weight && cau.collect_weight > bal.collect_weight && bal.collect_weight > agg.collect_weight);
	/* Range: aggressive closer, cautious further. */
	CHECK(agg.range_scale < bal.range_scale && bal.range_scale < cau.range_scale);
	/* Aggressive chases twice as long, drops fewer mines, closes in
	 * faster, burns sooner; cautious dodges more and keeps its pace.
	 */
	CHECK(agg.chase_memory == 2 * bal.chase_memory);
	CHECK(agg.mine_interval > bal.mine_interval && cau.mine_interval < bal.mine_interval);
	CHECK(effective_close_speed(agg) > effective_close_speed(bal) && effective_close_speed(cau) < effective_close_speed(bal));
	CHECK(agg.burn_chase_distance < bal.burn_chase_distance && cau.burn_chase_distance > bal.burn_chase_distance);
	for (const auto sk : skills)
	{
		const auto &k{skill_of(sk)};
		if (k.dodge_prob > 0)
			CHECK(std::abs(effective_dodge(k, cau) - std::min(0.95, k.dodge_prob + 0.1)) < 1e-9);
		CHECK(effective_dodge(k, bal) == k.dodge_prob);
		/* Heavy missiles: aggressive risks more and hugs more, cautious
		 * the least and ducks the earliest (section 9.6).
		 */
		const auto ra{risk_profile_of(sk, bot_style::aggressive)}, rb{risk_profile_of(sk, bot_style::balanced)}, rc{risk_profile_of(sk, bot_style::cautious)};
		CHECK(ra.self_budget >= rb.self_budget && rb.self_budget >= rc.self_budget);
		CHECK(ra.trade <= rb.trade && rb.trade <= rc.trade);
		CHECK(ra.hug >= rb.hug && rb.hug >= rc.hug);
		CHECK(ra.duck_share < rb.duck_share && rb.duck_share < rc.duck_share);
		CHECK(ra.duck_closing > rb.duck_closing && rb.duck_closing > rc.duck_closing);
		CHECK(rc.standoff_scale > rb.standoff_scale && rb.standoff_scale > ra.standoff_scale);
	}
	/* The ducking rule follows the style's share. */
	{
		const auto duck{[](const bot_style st, const double distance) {
			const auto rp{risk_profile_of(bot_skill::hotshot, st)};
			return want_duck({.heavy_ready = true, .favourable = false, .distance = distance, .standoff = 100, .target_closing = 0, .back_blocked = false, .duck_share = rp.duck_share, .duck_closing = rp.duck_closing});
		}};
		CHECK(duck(bot_style::cautious, 80));
		CHECK(!duck(bot_style::balanced, 80));
		CHECK(duck(bot_style::balanced, 50));
		CHECK(!duck(bot_style::aggressive, 50));
		CHECK(duck(bot_style::aggressive, 30));
	}
	/* Not ahead: a collector avoids the fight, a cautious bot a little;
	 * the others do not care.  Outgunned: cautious and collector break
	 * off earlier.
	 */
	for (const auto st : styles)
	{
		const auto &s{style_of(st)};
		CHECK(style_engage_factor(s, 1) == 1);
		CHECK(style_engage_factor(s, 3) == 1);
		CHECK(std::abs(style_engage_factor(s, 0.5) - s.behind_engage) < 1e-9);
		double last{0};
		for (double a = 0.1; a <= 2; a += 0.05)
		{
			const double f{style_engage_factor(s, a)};
			CHECK(f >= last - 1e-12);
			last = f;
		}
		CHECK(style_retreat_shields(s, 1) == s.retreat_shields);
		CHECK(style_retreat_shields(s, 0.3) >= s.retreat_shields);
	}
	CHECK(col.behind_engage < cau.behind_engage && cau.behind_engage < bal.behind_engage);
	CHECK(agg.behind_engage == 1 && bal.behind_engage == 1);
	CHECK(style_retreat_shields(cau, 0.3) > style_retreat_shields(cau, 1));
	CHECK(style_retreat_shields(agg, 0.3) == agg.retreat_shields);
	/* The advantage is even for equal ships and mirrors itself. */
	CHECK(std::abs(fight_advantage(100, 3, 100, 3) - 1) < 1e-9);
	CHECK(std::abs(fight_advantage(100, 3.8, 50, 1) * fight_advantage(50, 1, 100, 3.8) - 1) < 1e-9);
	CHECK(fight_advantage(150, 3, 50, 3) > 1);
	CHECK(fight_advantage(30, 1, 120, 3.8) < OUTGUNNED_ADVANTAGE);
}

/* The styles in the goal choice (bot_goals.h), with the same situation:
 * a visible target, a weapon to collect, the bot behind in the fight.
 */
void test_style_goals()
{
	const auto goal_for{[](const bot_style st, const double advantage, const double shields, const double collect = 1.0) {
		const auto &s{style_of(st)};
		goal_inputs in;
		in.has_target = true;
		in.target_visible = true;
		in.target_score = 0.9;
		in.threatened = true;
		in.shields = shields;
		in.collect = collect;
		in.collect_path = 80;
		in.retreat_shields = style_retreat_shields(s, advantage);
		in.engage_weight = s.engage_weight * style_engage_factor(s, advantage);
		in.collect_weight = s.collect_weight;
		in.collector = st == bot_style::collector;
		return choose_goal(in);
	}};
	/* Even fight at full shields: everyone but the collector engages. */
	CHECK(goal_for(bot_style::aggressive, 1, 100) == goal_kind::engage);
	CHECK(goal_for(bot_style::balanced, 1, 100) == goal_kind::engage);
	CHECK(goal_for(bot_style::cautious, 1, 100) == goal_kind::engage);
	CHECK(goal_for(bot_style::collector, 1, 100) == goal_kind::collect);
	/* A small prize: the collector fights while even, and goes for the
	 * prize when behind.
	 */
	CHECK(goal_for(bot_style::collector, 1, 100, 0.5) == goal_kind::engage);
	CHECK(goal_for(bot_style::collector, 0.5, 100, 0.5) == goal_kind::collect);
	CHECK(goal_for(bot_style::balanced, 0.5, 100, 0.5) == goal_kind::engage);
	/* Low shields: the cautious and the collector retreat, the balanced
	 * and the aggressive fight on.
	 */
	CHECK(goal_for(bot_style::cautious, 1, 45) == goal_kind::retreat);
	CHECK(goal_for(bot_style::collector, 1, 38) == goal_kind::retreat);
	CHECK(goal_for(bot_style::balanced, 1, 45) == goal_kind::engage);
	CHECK(goal_for(bot_style::aggressive, 1, 25) == goal_kind::engage);
	/* Outgunned at 70 shields: the cautious bot breaks off. */
	CHECK(goal_for(bot_style::cautious, 0.4, 70) == goal_kind::retreat);
	CHECK(goal_for(bot_style::balanced, 0.4, 70) == goal_kind::engage);
}

/* A beginner's trigger: held about its duty; a duty of 1 always and
 * without random numbers.
 */
void test_fire_burst()
{
	for (const auto &s : skill_table)
	{
		bot_rng rng{11}, ref{11};
		fire_burst b;
		unsigned on{0};
		constexpr unsigned ticks{600 * BOT_TICK_RATE};
		for (unsigned i = 0; i < ticks; ++i)
		{
			b.update(rng, s.fire_duty);
			on += b.on();
		}
		const double duty{static_cast<double>(on) / ticks};
		CHECK(std::abs(duty - s.fire_duty) < 0.04);
		if (s.fire_duty >= 1)
			CHECK(rng.next() == ref.next());
	}
	/* The pauses are short: a trainee fires again within 1.2 s. */
	{
		bot_rng rng{3};
		fire_burst b;
		unsigned off_run{0}, longest{0};
		for (unsigned i = 0; i < 120 * BOT_TICK_RATE; ++i)
		{
			b.update(rng, skill_of(bot_skill::trainee).fire_duty);
			off_run = b.on() ? 0 : off_run + 1;
			longest = std::max(longest, off_run);
		}
		CHECK(longest <= static_cast<unsigned>(1.2 * BOT_TICK_RATE));
		CHECK(longest > 0);
	}
	/* Every life starts firing: at least FIRE_BURST_MIN_S on, from a
	 * fresh state and after a reset in the middle of a pause.
	 */
	for (const std::uint32_t seed : {1u, 5u, 9u, 23u})
	{
		bot_rng rng{seed};
		fire_burst b;
		const auto duty{skill_of(bot_skill::trainee).fire_duty};
		const auto check_opening{[&] {
			for (unsigned i = 0; i < static_cast<unsigned>(FIRE_BURST_MIN_S * BOT_TICK_RATE); ++i)
			{
				b.update(rng, duty);
				CHECK(b.on());
			}
		}};
		check_opening();
		while (b.on())
			b.update(rng, duty);
		b.reset();
		check_opening();
	}
}

bot_profile sample_profile()
{
	bot_profile p;
	p.count = 3;
	p.default_skill = bot_skill::ace;
	p.default_style = bot_style::cautious;
	p.replace = false;
	std::strcpy(p.bots[0].name.data(), "ravager");
	p.bots[0].skill = bot_skill::trainee;
	p.bots[0].style = bot_style::collector;
	std::strcpy(p.bots[1].name.data(), "hav,oc");
	p.bots[1].skill = bot_skill::insane;
	p.bots[1].style = bot_style::aggressive;
	p.bots[1].team = bot_team::red;
	std::strcpy(p.bots[2].name.data(), "12345678");
	p.bots[2].skill = bot_skill::hotshot;
	p.bots[2].team = bot_team::blue;
	return p;
}

bot_profile round_trip(const bot_profile &p, const bool with_noise)
{
	std::array<profile_line, 3 + BOT_PROFILE_MAX_BOTS> lines;
	const auto n{format_profile(p, lines)};
	CHECK(n == 3 + p.count);
	profile_reader r;
	if (with_noise)
	{
		/* Other keys of the profile are not taken. */
		CHECK(!r.parse("GameName", "x"));
		CHECK(!r.parse("AllowGuidebot", "1"));
		CHECK(!r.parse("Botany", "1"));
	}
	for (std::size_t i = 0; i < n; ++i)
	{
		const std::string_view line{lines[i].data()};
		/* The .ngp reader's lines hold 49 characters. */
		CHECK(line.size() < BOT_PROFILE_LINE_SIZE - 1);
		const auto eq{line.find('=')};
		CHECK(eq != std::string_view::npos);
		CHECK(r.parse(line.substr(0, eq), line.substr(eq + 1)));
	}
	CHECK(r.seen());
	return r.result();
}

void test_profile()
{
	/* Round trip, mixed skills, styles and teams. */
	{
		const auto p{sample_profile()};
		CHECK(round_trip(p, true) == p);
	}
	/* Every combination of skill, style and team, seven bots. */
	{
		bot_profile p;
		p.count = BOT_PROFILE_MAX_BOTS;
		for (unsigned i = 0; i < BOT_PROFILE_MAX_BOTS; ++i)
		{
			std::snprintf(p.bots[i].name.data(), p.bots[i].name.size(), "bot%u", i);
			p.bots[i].skill = skills[i % BOT_SKILL_COUNT];
			p.bots[i].style = styles[(i / 2) % BOT_STYLE_COUNT];
			p.bots[i].team = bot_team{static_cast<uint8_t>(i % BOT_TEAM_COUNT)};
		}
		CHECK(round_trip(p, false) == p);
		p.count = 0;
		for (auto &b : p.bots)
			b = {};
		CHECK(round_trip(p, false) == p);
	}
	/* A profile of an older build: no bot lines. */
	{
		profile_reader r;
		CHECK(!r.parse("GameMode", "1"));
		CHECK(!r.seen());
		CHECK(r.result() == bot_profile{});
		CHECK(r.result().default_skill == bot_skill::hotshot);
		CHECK(r.result().replace);
	}
	/* Bad values keep the defaults; a bad line is ignored; lines beyond
	 * the count are dropped; the count is bounded; long names are cut.
	 */
	{
		profile_reader r;
		CHECK(r.parse("BotCount", "12"));
		CHECK(r.parse("BotDefault", "9,1"));
		CHECK(r.parse("BotReplace", "x"));
		CHECK(r.parse("Bot0", "alpha,5,0,0"));
		CHECK(r.parse("Bot1", "beta,1,2"));
		CHECK(r.parse("Bot2", "gamma,4,3,2"));
		CHECK(r.parse("Bot3", "averyverylongname,0,1,1"));
		CHECK(r.parse("Bot9", "zeta,1,1,1"));
		CHECK(r.parse("Bot4", " delta , 3 , 1 , 0 \r"));
		const auto p{r.result()};
		CHECK(p.count == BOT_PROFILE_MAX_BOTS);
		CHECK(p.default_skill == bot_skill::hotshot);
		CHECK(p.default_style == bot_style::aggressive);
		CHECK(p.replace);
		/* The rejected lines: the file's default skill and style. */
		const profile_entry fallback{.skill = bot_skill::hotshot, .style = bot_style::aggressive};
		CHECK(p.bots[0] == fallback);
		CHECK(p.bots[1] == fallback);
		CHECK(p.bots[5] == fallback);
		CHECK(p.bots[6] == fallback);
		CHECK(std::string_view{p.bots[2].name.data()} == "gamma");
		CHECK(p.bots[2].skill == bot_skill::insane && p.bots[2].style == bot_style::collector && p.bots[2].team == bot_team::red);
		CHECK(std::string_view{p.bots[3].name.data()} == "averyver");
		CHECK(p.bots[4].skill == bot_skill::ace && p.bots[4].style == bot_style::aggressive);
		CHECK(std::string_view{p.bots[4].name.data()} == " delta ");
	}
	{
		profile_reader r;
		CHECK(r.parse("BotCount", "2"));
		CHECK(r.parse("Bot0", "a,1,1,1"));
		CHECK(r.parse("Bot1", "b,2,2,2"));
		CHECK(r.parse("Bot2", "c,3,3,0"));
		const auto p{r.result()};
		CHECK(p.count == 2);
		CHECK(p.bots[2] == profile_entry{});
	}
	/* Gaps: the missing lines of a written profile take its BotDefault,
	 * not the built-in default, even when BotDefault comes last.
	 */
	{
		auto src{sample_profile()};
		src.count = 5;
		std::strcpy(src.bots[3].name.data(), "omega");
		src.bots[3].skill = bot_skill::insane;
		std::array<profile_line, 3 + BOT_PROFILE_MAX_BOTS> lines;
		const auto n{format_profile(src, lines)};
		CHECK(n == 8);
		profile_reader r;
		/* Bot1 and Bot4 lost; BotDefault read after the bot lines. */
		for (const std::size_t i : {0u, 2u, 3u, 5u, 6u, 1u})
		{
			const std::string_view line{lines[i].data()};
			const auto eq{line.find('=')};
			CHECK(r.parse(line.substr(0, eq), line.substr(eq + 1)));
		}
		const auto p{r.result()};
		CHECK(p.count == 5);
		CHECK(p.default_skill == bot_skill::ace && p.default_style == bot_style::cautious);
		const profile_entry fallback{.skill = bot_skill::ace, .style = bot_style::cautious};
		CHECK(p.bots[0] == src.bots[0]);
		CHECK(p.bots[1] == fallback);
		CHECK(p.bots[2] == src.bots[2]);
		CHECK(p.bots[3] == src.bots[3]);
		CHECK(p.bots[4] == fallback);
		CHECK(p.bots[5] == profile_entry{});
		/* Written again, the gaps are whole lines. */
		CHECK(round_trip(p, false) == p);
	}
	/* A bot line with no count line: the count stays 0 (the game keeps
	 * no bots rather than guessing).
	 */
	{
		profile_reader r;
		CHECK(r.parse("Bot0", "a,1,1,1"));
		CHECK(r.seen());
		CHECK(r.result().count == 0);
	}
	/* Too small an output: nothing written. */
	{
		std::array<profile_line, 3> lines;
		CHECK(format_profile(sample_profile(), lines) == 0);
	}
}

/* Section 6.4: "Save as default setup" during a game replaces the bot
 * lines of the pilot's profile and nothing else.
 */
void test_profile_bot_lines_only()
{
	const auto p{sample_profile()};
	std::array<profile_line, 3 + BOT_PROFILE_MAX_BOTS> lines;
	const auto n{format_profile(p, lines)};
	CHECK(n == 3 + p.count);
	std::string block;
	for (std::size_t i = 0; i < n; ++i)
		block += std::string(lines[i].data()) + "\n";
	const auto read_back{[](const std::string_view text) {
		profile_reader r;
		for (std::size_t pos{0}; pos < text.size();)
		{
			const auto nl{text.find('\n', pos)};
			const auto line{text.substr(pos, nl == std::string_view::npos ? std::string_view::npos : nl - pos)};
			pos = nl == std::string_view::npos ? text.size() : nl + 1;
			if (const auto eq{line.find('=')}; eq != std::string_view::npos)
				r.parse(line.substr(0, eq), line.substr(eq + 1));
		}
		return r.result();
	}};
	/* A profile with other bots: every other line stays as it is (the
	 * tracker the host chose, not the game's), the bot lines where they
	 * were, a bot line beyond the new count gone.
	 */
	{
		const std::string_view old{"game_name=My game\nTracker=1\ntrackernat=2\nBotCount=7\nBotDefault=0,0\nBotReplace=0\nBot0=a,0,0,0\nBot1=b,0,0,0\nBot2=c,0,0,0\nBot3=d,0,0,0\nBot4=e,0,0,0\nBot5=f,0,0,0\nBot6=g,0,0,0\nngp version=x\n"};
		const auto now{replace_profile_bot_lines(old, p, "ngp version")};
		CHECK(now == "game_name=My game\nTracker=1\ntrackernat=2\n" + block + "ngp version=x\n");
		CHECK(read_back(now) == p);
		/* Saving again changes nothing. */
		CHECK(replace_profile_bot_lines(now, p, "ngp version") == now);
	}
	/* A profile of an older build, without bot lines: before the
	 * version line.
	 */
	{
		const auto now{replace_profile_bot_lines("game_name=g\nTracker=1\nngp version=x\n", p, "ngp version")};
		CHECK(now == "game_name=g\nTracker=1\n" + block + "ngp version=x\n");
	}
	/* No version line, no final newline, a line without `=`, DOS line
	 * ends, bot lines apart from each other: all kept, the bots once.
	 */
	{
		const auto now{replace_profile_bot_lines("Bot0=x,1,1,1\r\njunk\r\nTracker=1\r\nBotCount=1\r\nKillGoal=3", p, "ngp version")};
		CHECK(now == block + "junk\r\nTracker=1\r\nKillGoal=3\n");
		CHECK(read_back(now) == p);
	}
	/* No profile yet: the bot lines alone. */
	CHECK(replace_profile_bot_lines("", p, "ngp version") == block);
	/* No bots: the three option lines, the old bots gone. */
	{
		bot_profile none;
		none.replace = false;
		const auto now{replace_profile_bot_lines("Tracker=1\nBotCount=1\nBot0=x,1,1,1\n", none, "ngp version")};
		CHECK(now == "Tracker=1\nBotCount=0\nBotDefault=2,0\nBotReplace=0\n");
		CHECK(read_back(now) == none);
	}
}

/* Section 9.8: the BOT marker in the kill list, always (a grey `*`
 * without the ping column, the ping column's `BOT` with it), never in
 * the team view; one narrow character off the name's column (the PR #38
 * review: ` BOT` cut the names to nothing).
 */
void test_marker()
{
	CHECK(kill_list_marks_bot(true, false, false));
	CHECK(!kill_list_marks_bot(false, false, false));
	CHECK(!kill_list_marks_bot(true, true, false));
	/* The ping column says BOT already: not twice. */
	CHECK(!kill_list_marks_bot(true, false, true));
	CHECK(kill_list_marker_room(true, 5) == 5);
	CHECK(kill_list_marker_room(false, 5) == 0);
	CHECK(std::string_view{BOT_KILL_LIST_MARKER} == "*");
	CHECK(std::string_view{BOT_KILL_LIST_MARKER}.size() == 1);
	CHECK(std::string_view{BOT_SCORE_MARKER} == "BOT");
}
}

int main()
{
	test_marker();
	test_skill_monotonic();
	test_skill_extremes();
	test_styles();
	test_style_goals();
	test_fire_burst();
	test_profile();
	test_profile_bot_lines_only();
	std::puts("test-bot-presets: all checks passed");
	return 0;
}
