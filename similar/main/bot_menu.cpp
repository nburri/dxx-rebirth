/*
 * This file is part of the DXX-Rebirth project <https://github.com/dxx-rebirth/dxx-rebirth/>.
 * It is copyright by its individual contributors, as recorded in the
 * project's Git history.  See COPYING.txt at the top level for license
 * terms and a link to the Git history.
 */
/*
 * The host's bot setup and its menus (Documentation/multiplayer-bots.md
 * sections 6.1 to 6.3): the "Bots..." item of the host setup menu, the
 * Bots screen (count, default skill and style, the list of bots) and
 * the per-bot screen (name, skill, style, team, remove).
 *
 * Stage B1 plays every bot at the Hotshot preset; the skill and style
 * fields are kept and shown, and take effect with the presets of B2.
 */

#include "dxxsconf.h"

#if DXX_USE_MULTIPLAYER

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <random>
#include <span>

#include "bot.h"
#include "args.h"
#include "multi.h"
#include "newmenu.h"
#include "player.h"
#include "strutil.h"
#include "timer.h"
#include "text.h"
#include "partial_range.h"

namespace dcx {

bot_setup Bot_setup;

}

namespace dsx {

namespace {

namespace b = ::dcx::bot;

/* Closing codes of the Bots screen's callback (newmenu closes a
 * callback menu on a value below -1).
 */
constexpr int MENU_DONE{-2};
constexpr int MENU_REBUILD{-3};
constexpr int MENU_SET_ALL_SKILL{-4};
constexpr int MENU_NEW_NAMES{-5};
constexpr int MENU_EDIT_BOT_BASE{-100};

[[nodiscard]]
bool name_in_use(const char *const name, const unsigned except)
{
	for (unsigned i = 0; i < Bot_setup.count; ++i)
		if (i != except && !d_stricmp(Bot_setup.bots[i].name, name))
			return true;
	return false;
}

void set_name(callsign_t &c, const char *const name)
{
	c.copy_lower(std::span<const char>(name, std::min<std::size_t>(std::strlen(name), CALLSIGN_LEN)));
}

/* The next built-in name no other bot has (section 6.2). */
void assign_next_name(const unsigned i)
{
	for (const auto n : b::bot_default_names)
		if (!name_in_use(n, i))
		{
			set_name(Bot_setup.bots[i].name, n);
			return;
		}
	char buf[CALLSIGN_LEN + 1];
	std::snprintf(buf, sizeof(buf), "bot%u", i + 1);
	set_name(Bot_setup.bots[i].name, buf);
}

void set_count(const unsigned count)
{
	const auto old{Bot_setup.count};
	Bot_setup.count = std::min(count, MAX_BOTS);
	for (unsigned i = old; i < Bot_setup.count; ++i)
	{
		auto &c{Bot_setup.bots[i]};
		c.skill = Bot_setup.default_skill;
		c.style = Bot_setup.default_style;
		c.team = b::bot_team::automatic;
		assign_next_name(i);
	}
}

void remove_bot(const unsigned i)
{
	if (i >= Bot_setup.count)
		return;
	for (unsigned j = i; j + 1 < Bot_setup.count; ++j)
		Bot_setup.bots[j] = Bot_setup.bots[j + 1];
	--Bot_setup.count;
	Bot_setup.bots[Bot_setup.count] = {};
}

void new_random_names()
{
	std::array<const char *, b::bot_default_names.size()> names;
	std::ranges::copy(b::bot_default_names, names.begin());
	std::minstd_rand rng{static_cast<uint32_t>(timer_query()) | 1u};
	std::ranges::shuffle(names, rng);
	for (unsigned i = 0; i < Bot_setup.count; ++i)
		set_name(Bot_setup.bots[i].name, names[i % names.size()]);
}

[[nodiscard]]
const char *skill_name(const b::bot_skill s)
{
	const auto i{static_cast<unsigned>(s)};
	return i < b::BOT_SKILL_COUNT ? b::bot_skill_names[i] : "?";
}

[[nodiscard]]
const char *style_name(const b::bot_style s)
{
	const auto i{static_cast<unsigned>(s)};
	return i < b::BOT_STYLE_COUNT ? b::bot_style_names[i] : "?";
}

[[nodiscard]]
const char *team_name(const b::bot_team t)
{
	const auto i{static_cast<unsigned>(t)};
	return i < b::BOT_TEAM_COUNT ? b::bot_team_names[i] : "?";
}

[[nodiscard]]
bool is_team_mode(const network_game_type mode)
{
	return mode == network_game_type::team_anarchy || mode == network_game_type::capture_flag || mode == network_game_type::team_hoard;
}

/* The per-bot screen (section 6.3). */
struct bot_edit_menu
{
	enum : unsigned
	{
		label_name,
		name,
		skill,
		style,
		team,
		blank,
		remove,
		done,
		count
	};
	unsigned index;
	bool team_mode;
	std::array<newmenu_item, count> m;
	std::array<char, CALLSIGN_LEN + 1> name_text{};
	char skill_text[40]{};
	char style_text[40]{};
	char team_text[40]{};
	ntstring<NM_MAX_TEXT_LEN> skill_saved, style_saved, team_saved;
	void update_labels()
	{
		std::snprintf(skill_text, sizeof(skill_text), "Skill: %s", skill_name(b::bot_skill{static_cast<uint8_t>(m[skill].value)}));
		std::snprintf(style_text, sizeof(style_text), "Style: %s", style_name(b::bot_style{static_cast<uint8_t>(m[style].value)}));
		if (team_mode)
			std::snprintf(team_text, sizeof(team_text), "Team: %s", team_name(b::bot_team{static_cast<uint8_t>(m[team].value)}));
		else
			std::snprintf(team_text, sizeof(team_text), "Team: (team modes only)");
	}
};

int bot_edit_handler(newmenu *, const d_event &event, bot_edit_menu *const e)
{
	switch (event.type)
	{
		case event_type::newmenu_changed:
			e->update_labels();
			return 0;
		case event_type::newmenu_selected:
		{
			const auto citem{static_cast<const d_select_event &>(event).citem};
			if (citem == bot_edit_menu::remove)
				return MENU_REBUILD;
			if (citem == bot_edit_menu::done)
				return MENU_DONE;
			return 0;
		}
		default:
			return 0;
	}
}

void run_bot_edit(const unsigned i, const network_game_type mode)
{
	if (i >= Bot_setup.count)
		return;
	auto &c{Bot_setup.bots[i]};
	bot_edit_menu e{};
	e.index = i;
	e.team_mode = is_team_mode(mode);
	std::snprintf(e.name_text.data(), e.name_text.size(), "%s", static_cast<const char *>(c.name));
	char title[16];
	std::snprintf(title, sizeof(title), "BOT %u", i + 1);
	nm_set_item_text(e.m[bot_edit_menu::label_name], "Name:");
	nm_set_item_input(e.m[bot_edit_menu::name], e.name_text);
	nm_set_item_slider(e.m[bot_edit_menu::skill], e.skill_text, static_cast<unsigned>(c.skill), 0, b::BOT_SKILL_COUNT - 1, e.skill_saved);
	nm_set_item_slider(e.m[bot_edit_menu::style], e.style_text, static_cast<unsigned>(c.style), 0, b::BOT_STYLE_COUNT - 1, e.style_saved);
	if (e.team_mode)
		nm_set_item_slider(e.m[bot_edit_menu::team], e.team_text, static_cast<unsigned>(c.team), 0, b::BOT_TEAM_COUNT - 1, e.team_saved);
	else
		nm_set_item_text(e.m[bot_edit_menu::team], e.team_text);
	nm_set_item_text(e.m[bot_edit_menu::blank], "");
	nm_set_item_menu(e.m[bot_edit_menu::remove], "Remove this bot");
	nm_set_item_menu(e.m[bot_edit_menu::done], "Done");
	e.update_labels();
	const int r{newmenu_do2(menu_title{title}, menu_subtitle{"Stage B1: every bot plays Hotshot"}, e.m, bot_edit_handler, &e, bot_edit_menu::done)};
	if (r == MENU_REBUILD)
	{
		remove_bot(i);
		return;
	}
	c.skill = b::bot_skill{static_cast<uint8_t>(e.m[bot_edit_menu::skill].value)};
	c.style = b::bot_style{static_cast<uint8_t>(e.m[bot_edit_menu::style].value)};
	if (e.team_mode)
		c.team = b::bot_team{static_cast<uint8_t>(e.m[bot_edit_menu::team].value)};
	/* An empty or taken name keeps the old one. */
	if (e.name_text[0] && !name_in_use(e.name_text.data(), i))
		set_name(c.name, e.name_text.data());
}

/* The Bots screen (section 6.2).  It is rebuilt whenever the list
 * changes (count, removal, names), so its lines always match the list.
 */
struct bots_menu
{
	static constexpr unsigned first_line{5};
	unsigned max_bots;
	unsigned opt_count{0}, opt_skill{1}, opt_style{2};
	unsigned opt_set_all{}, opt_names{}, opt_done{};
	unsigned nitems{};
	std::array<newmenu_item, first_line + MAX_BOTS + 4> m;
	char count_text[40]{};
	char skill_text[40]{};
	char style_text[40]{};
	std::array<std::array<char, 64>, MAX_BOTS> lines{};
	ntstring<NM_MAX_TEXT_LEN> count_saved, skill_saved, style_saved;
	void update_labels()
	{
		std::snprintf(count_text, sizeof(count_text), "Number of bots: %u", Bot_setup.count);
		std::snprintf(skill_text, sizeof(skill_text), "Default skill: %s", skill_name(Bot_setup.default_skill));
		std::snprintf(style_text, sizeof(style_text), "Default style: %s", style_name(Bot_setup.default_style));
	}
	void build()
	{
		update_labels();
		unsigned n{0};
		nm_set_item_slider(m[n++], count_text, Bot_setup.count, 0, max_bots, count_saved);
		nm_set_item_slider(m[n++], skill_text, static_cast<unsigned>(Bot_setup.default_skill), 0, b::BOT_SKILL_COUNT - 1, skill_saved);
		nm_set_item_slider(m[n++], style_text, static_cast<unsigned>(Bot_setup.default_style), 0, b::BOT_STYLE_COUNT - 1, style_saved);
		nm_set_item_text(m[n++], "(new bots take the default skill and style)");
		nm_set_item_text(m[n++], "");
		for (unsigned i = 0; i < Bot_setup.count; ++i)
		{
			auto &c{Bot_setup.bots[i]};
			std::snprintf(lines[i].data(), lines[i].size(), "%u. %-8s  %s  %s", i + 1, static_cast<const char *>(c.name), skill_name(c.skill), style_name(c.style));
			nm_set_item_menu(m[n++], lines[i].data());
		}
		if (!Bot_setup.count)
			nm_set_item_text(m[n++], max_bots ? "(no bots)" : "(raise Maximum players to add bots)");
		nm_set_item_text(m[n++], "");
		opt_set_all = n;
		nm_set_item_menu(m[n++], "Set all bots to default skill");
		opt_names = n;
		nm_set_item_menu(m[n++], "New random names");
		opt_done = n;
		nm_set_item_menu(m[n++], "Done");
		nitems = n;
	}
};

int bots_menu_handler(newmenu *, const d_event &event, bots_menu *const bm)
{
	switch (event.type)
	{
		case event_type::newmenu_changed:
		{
			const auto citem{static_cast<const d_change_event &>(event).citem};
			if (citem == bm->opt_count)
			{
				const auto v{static_cast<unsigned>(bm->m[bm->opt_count].value)};
				if (v < Bot_setup.count)
					Bot_setup.count = v;
				else
					set_count(v);
				return MENU_REBUILD;
			}
			if (citem == bm->opt_skill)
				Bot_setup.default_skill = b::bot_skill{static_cast<uint8_t>(bm->m[bm->opt_skill].value)};
			else if (citem == bm->opt_style)
				Bot_setup.default_style = b::bot_style{static_cast<uint8_t>(bm->m[bm->opt_style].value)};
			bm->update_labels();
			return 0;
		}
		case event_type::newmenu_selected:
		{
			const auto citem{static_cast<unsigned>(static_cast<const d_select_event &>(event).citem)};
			if (citem >= bots_menu::first_line && citem < bots_menu::first_line + Bot_setup.count)
				return MENU_EDIT_BOT_BASE - static_cast<int>(citem - bots_menu::first_line);
			if (citem == bm->opt_set_all)
				return MENU_SET_ALL_SKILL;
			if (citem == bm->opt_names)
				return MENU_NEW_NAMES;
			if (citem == bm->opt_done)
				return MENU_DONE;
			return 1;
		}
		default:
			return 0;
	}
}

}

void bots_setup_init()
{
	if (Bot_setup.initialized)
		return;
	Bot_setup.initialized = true;
	Bot_setup.count = 0;
	set_count(std::min<unsigned>(CGameArg.MplBots, MAX_BOTS));
}

bool bots_allowed_in_mode(const network_game_type mode)
{
	return mode == network_game_type::anarchy || mode == network_game_type::team_anarchy || mode == network_game_type::bounty;
}

void bots_setup_label(char *const buf, const std::size_t size, const network_game_type mode)
{
	if (!bots_allowed_in_mode(mode))
	{
		std::snprintf(buf, size, "Bots: not in this mode");
		return;
	}
	if (!Bot_setup.count)
	{
		std::snprintf(buf, size, "Bots: none...");
		return;
	}
	const auto first{Bot_setup.bots[0].skill};
	const bool mixed{std::ranges::any_of(std::span(Bot_setup.bots.data(), Bot_setup.count), [first](const bot_config &c) { return c.skill != first; })};
	std::snprintf(buf, size, "Bots: %u (%s)...", Bot_setup.count, mixed ? "mixed" : skill_name(first));
}

void bots_setup_menu(const network_game_type mode, const unsigned max_players)
{
	bots_setup_init();
	if (!bots_allowed_in_mode(mode))
	{
		nm_messagebox_str(menu_title{"BOTS"}, nm_messagebox_tie(TXT_OK), menu_subtitle{"Bots play anarchy, team anarchy\nand bounty for now."});
		return;
	}
	const unsigned max_bots{std::min<unsigned>(MAX_BOTS, max_players > 1 ? max_players - 1 : 0)};
	if (Bot_setup.count > max_bots)
		Bot_setup.count = max_bots;
	int citem{0};
	for (;;)
	{
		bots_menu bm{};
		bm.max_bots = max_bots;
		bm.build();
		const int r{newmenu_do2(menu_title{"BOTS"}, menu_subtitle{nullptr}, unchecked_partial_range(bm.m, bm.nitems), bots_menu_handler, &bm, std::min<int>(citem, bm.nitems - 1))};
		if (r == MENU_REBUILD)
		{
			citem = static_cast<int>(bm.opt_count);
			continue;
		}
		if (r == MENU_SET_ALL_SKILL)
		{
			for (unsigned i = 0; i < Bot_setup.count; ++i)
			{
				Bot_setup.bots[i].skill = Bot_setup.default_skill;
				Bot_setup.bots[i].style = Bot_setup.default_style;
			}
			citem = static_cast<int>(bm.opt_set_all);
			continue;
		}
		if (r == MENU_NEW_NAMES)
		{
			new_random_names();
			citem = static_cast<int>(bm.opt_names);
			continue;
		}
		if (r <= MENU_EDIT_BOT_BASE)
		{
			const unsigned i{static_cast<unsigned>(MENU_EDIT_BOT_BASE - r)};
			run_bot_edit(i, mode);
			citem = static_cast<int>(bots_menu::first_line + std::min(i, Bot_setup.count ? Bot_setup.count - 1 : 0));
			continue;
		}
		break;
	}
}

}

#endif
