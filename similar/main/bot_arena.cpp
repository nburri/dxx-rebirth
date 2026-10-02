/*
 * This file is part of the DXX-Rebirth project <https://github.com/dxx-rebirth/dxx-rebirth/>.
 * It is copyright by its individual contributors, as recorded in the
 * project's Git history.  See COPYING.txt at the top level for license
 * terms and a link to the Git history.
 */
/*
 * -botarena (bot_arena.h, Documentation/multiplayer-bots.md section 8.2).
 */

#include <algorithm>
#include <array>
#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <string>
#include <string_view>

#include "bot_arena.h"
#include "bot.h"
#include "bot_command.h"
#include "console.h"
#include "game.h"
#include "gameseq.h"
#include "mission.h"
#include "multi.h"
#if DXX_USE_UDP
#include "net_udp.h"
#endif
#include "object.h"
#include "d_levelstate.h"
#include "player.h"
#include "playsave.h"
#include "timer.h"

namespace dcx {

namespace {

struct arena_player_stats
{
	unsigned primary_shots{};
	unsigned secondary_shots{};
	unsigned direct_hits{};
	unsigned splash_hits{};
	double damage_dealt{};
	unsigned stuck{};
	unsigned paths{};
	double path_length{};
};

struct arena_state
{
	std::array<arena_player_stats, MAX_PLAYERS> players{};
	std::chrono::steady_clock::duration bot_cpu{};
	std::chrono::steady_clock::time_point wall_start{};
	fix64 game_start{};
	fix64 next_progress{};
	bool started{};
	bool parked{};
	bool done{};
};

arena_state A;

[[nodiscard]]
bool counting(const unsigned pnum)
{
	return bot_arena_active() && A.parked && !A.done && pnum < MAX_PLAYERS;
}

}

void bot_arena_note_fire(const unsigned pnum, const bool secondary)
{
	if (!counting(pnum))
		return;
	auto &p{A.players[pnum]};
	++(secondary ? p.secondary_shots : p.primary_shots);
}

void bot_arena_note_damage(const unsigned victim, const unsigned attacker, const fix damage, const bool splash)
{
	if (!counting(attacker) || attacker == victim)
		return;
	auto &p{A.players[attacker]};
	++(splash ? p.splash_hits : p.direct_hits);
	p.damage_dealt += f2fl(damage);
}

void bot_arena_note_stuck(const unsigned pnum)
{
	if (counting(pnum))
		++A.players[pnum].stuck;
}

void bot_arena_note_path(const unsigned pnum, const double length)
{
	if (!counting(pnum))
		return;
	auto &p{A.players[pnum]};
	++p.paths;
	p.path_length += length;
}

bot_arena_cpu_scope::~bot_arena_cpu_scope()
{
	if (bot_arena_active())
		A.bot_cpu += std::chrono::steady_clock::now() - start;
}

}

namespace dsx {

namespace {

int exit_status{1};

}

#if DXX_USE_UDP
namespace {

namespace b = ::dcx::bot;

[[nodiscard]]
std::string_view trim(std::string_view s)
{
	while (!s.empty() && (s.front() == ' ' || s.front() == '\t'))
		s.remove_prefix(1);
	while (!s.empty() && (s.back() == ' ' || s.back() == '\t'))
		s.remove_suffix(1);
	return s;
}

/* -botarena-bots "skill:style[:name],...": the bots, in order; a bot the
 * list leaves out (or a field it leaves empty) plays the setup's default.
 * False (and a log line) if a word is no skill or style.
 */
[[nodiscard]]
bool parse_bot_list(const std::string_view list, const unsigned count, b::bot_profile &p)
{
	p.count = count;
	for (unsigned i = 0; i < count; ++i)
	{
		auto &e{p.bots[i]};
		e = {};
		e.skill = p.default_skill;
		e.style = p.default_style;
		e.profile = p.default_profile;
	}
	std::string_view rest{list};
	for (unsigned i = 0; i < count && !rest.empty(); ++i)
	{
		const auto comma{rest.find(',')};
		const auto item{trim(rest.substr(0, comma))};
		rest = comma == std::string_view::npos ? std::string_view{} : rest.substr(comma + 1);
		auto &e{p.bots[i]};
		if (item.empty())
			continue;
		std::array<std::string_view, 3> field{};
		std::string_view f{item};
		for (auto &w : field)
		{
			const auto colon{f.find(':')};
			w = trim(f.substr(0, colon));
			if (colon == std::string_view::npos)
				break;
			f = f.substr(colon + 1);
		}
		if (!field[0].empty())
		{
			const auto k{b::parse_skill(field[0])};
			if (!k)
			{
				con_printf(CON_URGENT, "botarena: \"%.*s\" is no skill (trainee, rookie, hotshot, ace, insane)", static_cast<int>(field[0].size()), field[0].data());
				return false;
			}
			e.skill = *k;
		}
		if (!field[1].empty())
		{
			const auto st{b::parse_style(field[1])};
			if (!st)
			{
				con_printf(CON_URGENT, "botarena: \"%.*s\" is no style (balanced, aggressive, cautious, collector)", static_cast<int>(field[1].size()), field[1].data());
				return false;
			}
			e.style = *st;
			e.profile = {};
		}
		const auto n{std::min(field[2].size(), e.name.size() - 1)};
		std::copy_n(field[2].data(), n, e.name.data());
		e.name[n] = 0;
	}
	return true;
}

/* The bot setup the arena plays: the list of -botarena-bots, else the
 * pilot's netgame profile (read by net_udp_arena_prepare), each with
 * `count` bots.
 */
[[nodiscard]]
bool setup_bots(const unsigned count)
{
	bots_load_styles(true);
	auto p{bots_setup_profile()};
	if (!CGameArg.DbgBotArenaSpec.empty())
	{
		if (!parse_bot_list(CGameArg.DbgBotArenaSpec, count, p))
			return false;
	}
	else
	{
		for (unsigned i = p.count; i < count; ++i)
		{
			auto &e{p.bots[i]};
			e = {};
			e.skill = p.default_skill;
			e.style = p.default_style;
			e.profile = p.default_profile;
		}
		p.count = count;
	}
	bots_setup_load(p);
	return true;
}

/* The host's ship is a ghost nobody sees, hits or shoots at: as a
 * player who left (multi_make_player_ghost), and it does not move.
 */
void park_host_ship()
{
	auto &Objects = LevelUniqueObjectState.Objects;
	auto &obj{*Objects.vmptr(get_local_player().objnum)};
	obj.type = object_type::OBJ_GHOST;
	obj.render_type = render_type::RT_NONE;
	obj.movement_source = object::movement_type::None;
	obj.control_source = object::control_type::None;
	multi_reset_player_object(obj);
}

[[nodiscard]]
double seconds_of(const std::chrono::steady_clock::duration d)
{
	return std::chrono::duration<double>(d).count();
}

[[nodiscard]]
const char *style_label(const bot_config &c, std::array<char, 32> &buf)
{
	if (c.profile[0])
		std::snprintf(buf.data(), buf.size(), "%s", c.profile.data());
	else
		std::snprintf(buf.data(), buf.size(), "%s", b::bot_style_names[static_cast<unsigned>(c.style)]);
	return buf.data();
}

struct skill_totals
{
	unsigned bots{};
	unsigned kills{};
	unsigned deaths{};
	unsigned shots{};
	unsigned hits{};
};

/* The summary.  As movrec-analyse counts them: "hits" are the direct
 * hits (each bolt of a shot of several, and the missiles' direct hits)
 * on other players, "shots" the primary shots (a volley of bolts is one
 * shot), "splash" the blast hits, "damage" the shields they took.
 */
void print_summary(const double game_seconds)
{
	auto &Objects = LevelUniqueObjectState.Objects;
	const double wall{seconds_of(std::chrono::steady_clock::now() - A.wall_start)};
	const double cpu{seconds_of(A.bot_cpu)};
	std::array<bot_in_game, MAX_BOTS> list;
	const unsigned n{bots_in_game(list)};
	con_printf(CON_URGENT, "botarena: summary: %.0f s of game time on level %u of \"%s\" (%s), %u bots, %u fps, seed %" PRIu32,
		game_seconds, static_cast<unsigned>(Current_level_num), Current_mission->mission_name.data(), &*Current_mission->filename, n, static_cast<unsigned>(CGameArg.DbgBotArenaFps), CGameArg.DbgBotArenaSeed);
	con_printf(CON_URGENT, "botarena: wall time %.1f s: %.2f game seconds per wall second; bot code %.2f ms per game second (%.2f ms per bot)",
		wall, wall > 0 ? game_seconds / wall : 0.0, cpu * 1000 / game_seconds, n ? cpu * 1000 / game_seconds / n : 0.0);
	con_printf(CON_URGENT, "botarena: %-8s %-7s %-10s %5s %6s %4s %6s %5s %6s %6s %6s %5s %8s %6s %8s",
		"bot", "skill", "style", "kills", "deaths", "self", "shots", "hits", "hit/sh", "splash", "damage", "miss.", "stuck", "/min", "path avg");
	std::array<skill_totals, b::BOT_SKILL_COUNT> by_skill{};
	const double minutes{game_seconds / 60};
	for (unsigned i = 0; i < n; ++i)
	{
		const auto &g{list[i]};
		const auto pid{g.pid};
		const auto &pi{Objects.vcptr(vcplayerptr(pid)->objnum)->ctype.player_info};
		unsigned kills{0};
		for (unsigned v = 0; v < MAX_PLAYERS; ++v)
			if (v != pid)
				kills += kill_matrix[pid][v];
		const unsigned self{kill_matrix[pid][pid]};
		const unsigned deaths{static_cast<unsigned>(std::max<int>(pi.net_killed_total, 0))};
		const auto &s{A.players[pid]};
		std::array<char, 32> style;
		con_printf(CON_URGENT, "botarena: %-8s %-7s %-10s %5u %6u %4u %6u %5u %6.2f %6u %6.0f %5u %8u %6.2f %8.0f",
			static_cast<const char *>(g.cfg.name), b::bot_skill_names[static_cast<unsigned>(g.cfg.skill)], style_label(g.cfg, style),
			kills, deaths, self, s.primary_shots, s.direct_hits, s.primary_shots ? static_cast<double>(s.direct_hits) / s.primary_shots : 0.0,
			s.splash_hits, s.damage_dealt, s.secondary_shots, s.stuck, minutes > 0 ? s.stuck / minutes : 0.0, s.paths ? s.path_length / s.paths : 0.0);
		auto &t{by_skill[static_cast<unsigned>(g.cfg.skill)]};
		++t.bots;
		t.kills += kills;
		t.deaths += deaths;
		t.shots += s.primary_shots;
		t.hits += s.direct_hits;
	}
	for (unsigned k = 0; k < by_skill.size(); ++k)
		if (const auto &t{by_skill[k]}; t.bots)
			con_printf(CON_URGENT, "botarena: skill %-7s %u bot%s: kills %u, deaths %u (%.2f kills per death), %.2f direct hits per primary shot (%u hits, %u shots)",
				b::bot_skill_names[k], t.bots, t.bots == 1 ? "" : "s", t.kills, t.deaths, t.deaths ? static_cast<double>(t.kills) / t.deaths : static_cast<double>(t.kills), t.shots ? static_cast<double>(t.hits) / t.shots : 0.0, t.hits, t.shots);
}

}

bool bot_arena_start()
{
	if (!InterfaceUniqueState.PilotName[0u])
	{
		/* No -pilot: a pilot of defaults, written nowhere. */
		new_player_config();
		InterfaceUniqueState.PilotName.copy(std::span<const char>("arena", 5));
	}
	const auto &mission{CGameArg.DbgBotArenaMission};
	if (const auto err{load_mission_by_file_or_title(mission.c_str())})
	{
		con_printf(CON_URGENT, "botarena: mission \"%s\": %s", mission.c_str(), err);
		return false;
	}
	const unsigned level{CGameArg.DbgBotArenaLevel};
	if (level > static_cast<unsigned>(Current_mission->last_level))
	{
		con_printf(CON_URGENT, "botarena: mission \"%s\" has %u levels, not %u", Current_mission->mission_name.data(), static_cast<unsigned>(Current_mission->last_level), level);
		return false;
	}
	/* From here on the game time is simulated: the same seed plays the
	 * same game (as long as the bots' code does not change).
	 */
	timer_use_simulated_clock(F1_0, F1_0 / CGameArg.DbgBotArenaFps);
	const unsigned bots{CGameArg.DbgBotArenaBots};
	net_udp_arena_prepare(level, bots);
	if (!setup_bots(bots))
		return false;
	con_printf(CON_URGENT, "botarena: level %u of \"%s\" (%s), %u bots, %" PRIu32 " s at %u fps, seed %" PRIu32,
		level, Current_mission->mission_name.data(), &*Current_mission->filename, bots, CGameArg.DbgBotArenaSeconds, static_cast<unsigned>(CGameArg.DbgBotArenaFps), CGameArg.DbgBotArenaSeed);
	A.started = true;
	if (!net_udp_arena_start(CGameArg.DbgBotArenaSeed))
	{
		con_printf(CON_URGENT, "botarena: the game did not start");
		return false;
	}
	return true;
}

window_event_result bot_arena_frame()
{
	if (!A.started || A.done)
		return window_event_result::ignored;
	if (!A.parked)
	{
		park_host_ship();
		A.parked = true;
		A.game_start = GameTime64;
		A.next_progress = GameTime64 + i2f(60);
		A.wall_start = std::chrono::steady_clock::now();
		std::array<bot_in_game, MAX_BOTS> list;
		const unsigned n{bots_in_game(list)};
		for (unsigned i = 0; i < n; ++i)
		{
			std::array<char, 32> style;
			con_printf(CON_URGENT, "botarena: P#%u '%s' %s %s", static_cast<unsigned>(list[i].pid), static_cast<const char *>(list[i].cfg.name), b::bot_skill_names[static_cast<unsigned>(list[i].cfg.skill)], style_label(list[i].cfg, style));
		}
		return window_event_result::ignored;
	}
	const double game_seconds{static_cast<double>(GameTime64 - A.game_start) / F1_0};
	if (GameTime64 >= A.next_progress)
	{
		A.next_progress += i2f(60);
		const double wall{seconds_of(std::chrono::steady_clock::now() - A.wall_start)};
		con_printf(CON_URGENT, "botarena: %.0f of %" PRIu32 " s (%.1f game seconds per wall second)", game_seconds, CGameArg.DbgBotArenaSeconds, wall > 0 ? game_seconds / wall : 0.0);
	}
	/* Section 9.16: the level ends (the reactor destroyed, the time or
	 * kill limit): the end-of-level screens wait for players, and the
	 * arena hung there.  It ends now, with the summary of the time
	 * played.
	 */
	if (LevelUniqueObjectState.ControlCenterState.Control_center_destroyed)
		con_printf(CON_URGENT, "botarena: the level ends after %.0f of %" PRIu32 " s (the reactor was destroyed); the arena ends here", game_seconds, CGameArg.DbgBotArenaSeconds);
	else if (game_seconds < CGameArg.DbgBotArenaSeconds)
		return window_event_result::ignored;
	print_summary(game_seconds);
	A.done = true;
	exit_status = 0;
	return window_event_result::close;
}

#else
bool bot_arena_start()
{
	con_puts(CON_URGENT, "botarena: this build has no network games");
	return false;
}

window_event_result bot_arena_frame()
{
	return window_event_result::ignored;
}
#endif

int bot_arena_exit_status()
{
	return exit_status;
}

}
