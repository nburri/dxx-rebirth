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
 * the per-bot screen (name, skill, style, team, remove), the setup in
 * the pilot's netgame profile (section 6.5) and the bot flag of each
 * player slot (section 2.2).  Each bot plays its own skill and style
 * (stage B2, section 9.7), so bots of different skills and styles play
 * in the same game.
 *
 * Stage B5 (sections 6.4 and 9.11): the host's in-game Bots screen and
 * the chat command `/bot`, which add, remove and change the bots of the
 * game being played (through bot.cpp); the setup is only touched by
 * "Save as default setup".
 *
 * Section 9.13: the style profiles (`.botstyle` files of `botstyles/`),
 * offered after the built-in styles wherever a style is chosen.
 */

#include "dxxsconf.h"

#if DXX_USE_MULTIPLAYER

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <random>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "bot.h"
#include "args.h"
#include "console.h"
#include "event.h"
#include "key.h"
#include "mouse.h"
#include "hudmsg.h"
#include "playsave.h"
#include "window.h"
#include "multi.h"
#include "newmenu.h"
#include "player.h"
#include "strutil.h"
#include "timer.h"
#include "text.h"
#include "partial_range.h"
#include "physfsx.h"
#include "physfs_list.h"

namespace dcx {

bot_setup Bot_setup;
bot_game_options Bot_game;

namespace {

std::array<bool, MAX_PLAYERS> Player_bot_flags{};

/* Section 9.13: the style profiles. */
bot::style_library Bot_styles;

}

const bot::style_library &bots_style_library()
{
	return Bot_styles;
}

void bots_load_styles(const bool report)
{
	namespace b = ::dcx::bot;
	Bot_styles.clear();
	/* The folder is made, so that a host sees where the files go. */
	PHYSFS_mkdir(b::BOT_STYLE_FOLDER);
	std::vector<std::string> files;
	if (const auto list{PHYSFSX_uncounted_list{PHYSFS_enumerateFiles(b::BOT_STYLE_FOLDER)}})
		for (const auto i : list)
		{
			const std::string_view n{i};
			constexpr std::string_view ext{b::STYLE_PROFILE_EXTENSION};
			if (n.size() > ext.size() && b::detail::iequal(n.substr(n.size() - ext.size()), ext))
				files.emplace_back(n);
		}
	/* The same order on every host: the first of two files with one name
	 * wins.
	 */
	std::ranges::sort(files);
	for (const auto &f : files)
	{
		const std::string path{std::string{b::BOT_STYLE_FOLDER} + "/" + f};
		const std::string_view stem{std::string_view{f}.substr(0, f.size() - b::STYLE_PROFILE_EXTENSION.size())};
		RAIIPHYSFS_File fp{PHYSFS_openRead(path.c_str())};
		if (!fp)
		{
			if (report)
				con_printf(CON_NORMAL, "bots: style %s: cannot be read", path.c_str());
			continue;
		}
		/* One byte more than a profile may have: a larger file is refused
		 * without reading it all.
		 */
		std::string text(b::STYLE_FILE_MAX_BYTES + 1, '\0');
		const auto got{PHYSFS_readBytes(fp, text.data(), text.size())};
		if (got < 0)
			continue;
		text.resize(static_cast<std::size_t>(got));
		const auto r{Bot_styles.add(stem, text)};
		if (r == b::style_add_result::added)
		{
			const auto &ls{Bot_styles[Bot_styles.size() - 1]};
			con_printf(report ? CON_NORMAL : CON_VERBOSE, "bots: style \"%s\" from %s (base %s%s%s)", ls.name.data(), path.c_str(), b::bot_style_names[static_cast<unsigned>(ls.profile.base_style) % b::BOT_STYLE_COUNT], ls.word[0] ? ", /bot word " : "", ls.word.data());
		}
		else if (report)
			con_printf(CON_NORMAL, "bots: style %s: %s", path.c_str(), b::style_add_text(r));
		if (r == b::style_add_result::full)
			break;
	}
}

bool player_is_bot(const unsigned pnum)
{
	return pnum < Player_bot_flags.size() && Player_bot_flags[pnum];
}

void set_player_is_bot(const unsigned pnum, const bool bot)
{
	if (pnum < Player_bot_flags.size())
		Player_bot_flags[pnum] = bot;
}

void clear_player_bot_flags()
{
	Player_bot_flags = {};
}

}

namespace dsx {

namespace {

namespace b = ::dcx::bot;

/* Closing codes of the Bots screen's callback (newmenu closes a
 * callback menu on a value below -1, for a change event too).
 */
constexpr int MENU_DONE{-2};
constexpr int MENU_REBUILD{-3};
constexpr int MENU_SET_ALL_SKILL{-4};
constexpr int MENU_NEW_NAMES{-5};
constexpr int MENU_EDIT_BOT_BASE{-100};

/* The Bots list's short style names. */
constexpr std::array<const char *, b::BOT_STYLE_COUNT> style_short_names{{"Bal", "Aggr", "Caut", "Coll"}};

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
		c.profile = Bot_setup.default_profile;
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
const char *style_short_name(const b::bot_style s)
{
	const auto i{static_cast<unsigned>(s)};
	return i < b::BOT_STYLE_COUNT ? style_short_names[i] : "?";
}

/* Section 9.13: a style as the sliders offer it (style_choice_count):
 * the built-in styles, then the loaded profiles.  A profile that is not
 * loaded shows as its base style.
 */
[[nodiscard]]
unsigned style_choice(const b::bot_style style, const b::style_name &profile)
{
	if (profile[0])
		if (const auto i{bots_style_library().index_of(profile.data())}; i < bots_style_library().size())
			return b::BOT_STYLE_COUNT + static_cast<unsigned>(i);
	return static_cast<unsigned>(style) % b::BOT_STYLE_COUNT;
}

[[nodiscard]]
unsigned style_choice_max()
{
	return b::style_choice_count(bots_style_library().size()) - 1;
}

void set_style_choice(const unsigned v, b::bot_style &style, b::style_name &profile)
{
	const auto &lib{bots_style_library()};
	if (v >= b::BOT_STYLE_COUNT && v - b::BOT_STYLE_COUNT < lib.size())
	{
		const auto &ls{lib[v - b::BOT_STYLE_COUNT]};
		style = ls.profile.base_style;
		profile = ls.name;
	}
	else
	{
		style = b::bot_style{static_cast<uint8_t>(std::min(v, b::BOT_STYLE_COUNT - 1))};
		profile = {};
	}
}

[[nodiscard]]
const char *style_choice_name(const unsigned v)
{
	const auto &lib{bots_style_library()};
	if (v >= b::BOT_STYLE_COUNT && v - b::BOT_STYLE_COUNT < lib.size())
		return lib[v - b::BOT_STYLE_COUNT].name.data();
	return style_name(b::bot_style{static_cast<uint8_t>(std::min(v, b::BOT_STYLE_COUNT - 1))});
}

/* A bot's style for a label: the profile's name (also when its file is
 * gone), else the built-in style's.
 */
[[nodiscard]]
const char *config_style_name(const b::bot_style style, const b::style_name &profile)
{
	return profile[0] ? profile.data() : style_name(style);
}

/* The same in the lists' short column (the profile's name cut). */
void config_style_short(const b::bot_style style, const b::style_name &profile, std::array<char, 8> &out)
{
	std::snprintf(out.data(), out.size(), "%s", profile[0] ? profile.data() : style_short_name(style));
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
	char style_text[56]{};
	char team_text[40]{};
	ntstring<NM_MAX_TEXT_LEN> skill_saved, style_saved, team_saved;
	/* Section 9.13: the bot's style profile is not loaded (its file is
	 * gone): the slider's first place stands for it.
	 */
	const char *missing_profile{};
	unsigned missing_choice{};
	void note_missing(const b::bot_style st, const b::style_name &profile)
	{
		missing_profile = nullptr;
		if (profile[0] && !bots_style_library().find(profile.data()))
		{
			missing_profile = profile.data();
			missing_choice = style_choice(st, profile);
		}
	}
	void update_labels()
	{
		std::snprintf(skill_text, sizeof(skill_text), "Skill: %s", skill_name(b::bot_skill{static_cast<uint8_t>(m[skill].value)}));
		if (missing_profile && static_cast<unsigned>(m[style].value) == missing_choice)
			std::snprintf(style_text, sizeof(style_text), "Style: %s (no file)", missing_profile);
		else
			std::snprintf(style_text, sizeof(style_text), "Style: %s", style_choice_name(static_cast<unsigned>(m[style].value)));
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
	nm_set_item_slider(e.m[bot_edit_menu::style], e.style_text, style_choice(c.style, c.profile), 0, style_choice_max(), e.style_saved);
	e.note_missing(c.style, c.profile);
	if (e.team_mode)
		nm_set_item_slider(e.m[bot_edit_menu::team], e.team_text, static_cast<unsigned>(c.team), 0, b::BOT_TEAM_COUNT - 1, e.team_saved);
	else
		nm_set_item_text(e.m[bot_edit_menu::team], e.team_text);
	nm_set_item_text(e.m[bot_edit_menu::blank], "");
	nm_set_item_menu(e.m[bot_edit_menu::remove], "Remove this bot");
	nm_set_item_menu(e.m[bot_edit_menu::done], "Done");
	e.update_labels();
	const int r{newmenu_do2(menu_title{title}, menu_subtitle{nullptr}, e.m, bot_edit_handler, &e, bot_edit_menu::done)};
	if (r == MENU_REBUILD)
	{
		remove_bot(i);
		return;
	}
	c.skill = b::bot_skill{static_cast<uint8_t>(e.m[bot_edit_menu::skill].value)};
	/* Section 9.13: a style that is not loaded (its file is gone) stays
	 * as it was unless the slider was moved.
	 */
	if (static_cast<unsigned>(e.m[bot_edit_menu::style].value) != style_choice(c.style, c.profile))
		set_style_choice(static_cast<unsigned>(e.m[bot_edit_menu::style].value), c.style, c.profile);
	if (e.team_mode)
		c.team = b::bot_team{static_cast<uint8_t>(e.m[bot_edit_menu::team].value)};
	/* An empty or taken name keeps the old one, and so does a word
	 * `/bot` reads as something else (section 6.4: `all`, a skill, a
	 * style, a command).
	 */
	if (e.name_text[0] && !name_in_use(e.name_text.data(), i) && !b::name_reserved(e.name_text.data()))
		set_name(c.name, e.name_text.data());
}

/* The Bots screen (section 6.2).  It is rebuilt whenever the list
 * changes (count, removal, names), so its lines always match the list.
 */
struct bots_menu
{
	static constexpr unsigned first_line{6};
	unsigned max_bots;
	unsigned opt_count{0}, opt_skill{1}, opt_style{2}, opt_replace{3};
	unsigned opt_set_all{}, opt_names{}, opt_done{};
	unsigned nitems{};
	/* The bots listed on this screen: the item indices are taken from
	 * it, not from Bot_setup, which a change may alter before the screen
	 * is rebuilt.
	 */
	unsigned listed{};
	std::array<newmenu_item, first_line + MAX_BOTS + 4> m;
	char count_text[40]{};
	char skill_text[40]{};
	char style_text[56]{};
	std::array<std::array<char, 64>, MAX_BOTS> lines{};
	ntstring<NM_MAX_TEXT_LEN> count_saved, skill_saved, style_saved;
	void update_labels()
	{
		std::snprintf(count_text, sizeof(count_text), "Number of bots: %u", Bot_setup.count);
		std::snprintf(skill_text, sizeof(skill_text), "Default skill: %s", skill_name(Bot_setup.default_skill));
		std::snprintf(style_text, sizeof(style_text), "Default style: %s", config_style_name(Bot_setup.default_style, Bot_setup.default_profile));
	}
	void build()
	{
		update_labels();
		unsigned n{0};
		nm_set_item_slider(m[n++], count_text, Bot_setup.count, 0, max_bots, count_saved);
		nm_set_item_slider(m[n++], skill_text, static_cast<unsigned>(Bot_setup.default_skill), 0, b::BOT_SKILL_COUNT - 1, skill_saved);
		nm_set_item_slider(m[n++], style_text, style_choice(Bot_setup.default_style, Bot_setup.default_profile), 0, style_choice_max(), style_saved);
		nm_set_item_checkbox(m[n++], "Humans replace bots when full", Bot_setup.replace);
		nm_set_item_text(m[n++], "(new bots take the default skill and style)");
		nm_set_item_text(m[n++], "");
		listed = Bot_setup.count;
		for (unsigned i = 0; i < listed; ++i)
		{
			auto &c{Bot_setup.bots[i]};
			std::array<char, 8> st;
			config_style_short(c.style, c.profile, st);
			std::snprintf(lines[i].data(), lines[i].size(), "%u. %-8s  %-7s %s", i + 1, static_cast<const char *>(c.name), skill_name(c.skill), st.data());
			nm_set_item_menu(m[n++], lines[i].data());
		}
		if (!Bot_setup.count)
			nm_set_item_text(m[n++], max_bots ? "(no bots)" : "(raise Maximum players to add bots)");
		nm_set_item_text(m[n++], "");
		opt_set_all = n;
		nm_set_item_menu(m[n++], "Set all bots to default skill/style");
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
				/* The list has a different length: close the screen so
				 * that bots_setup_menu rebuilds it (newmenu honours a
				 * close requested by a change).
				 */
				if (Bot_setup.count != bm->listed)
					return MENU_REBUILD;
				bm->update_labels();
				return 0;
			}
			if (citem == bm->opt_skill)
				Bot_setup.default_skill = b::bot_skill{static_cast<uint8_t>(bm->m[bm->opt_skill].value)};
			else if (citem == bm->opt_style)
				set_style_choice(static_cast<unsigned>(bm->m[bm->opt_style].value), Bot_setup.default_style, Bot_setup.default_profile);
			else if (citem == bm->opt_replace)
				Bot_setup.replace = bm->m[bm->opt_replace].value != 0;
			bm->update_labels();
			return 0;
		}
		case event_type::newmenu_selected:
		{
			const auto citem{static_cast<unsigned>(static_cast<const d_select_event &>(event).citem)};
			if (citem >= bots_menu::first_line && citem < bots_menu::first_line + bm->listed && citem - bots_menu::first_line < Bot_setup.count)
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
	bots_load_styles(true);
	Bot_setup.count = 0;
	set_count(std::min<unsigned>(CGameArg.MplBots, MAX_BOTS));
}

void bots_setup_load(const b::bot_profile &p)
{
	const bool first{!Bot_setup.initialized};
	Bot_setup.initialized = true;
	Bot_setup.default_skill = p.default_skill;
	Bot_setup.default_style = p.default_style;
	Bot_setup.default_profile = p.default_profile;
	Bot_setup.replace = p.replace;
	Bot_setup.count = std::min<unsigned>(p.count, MAX_BOTS);
	for (unsigned i = 0; i < MAX_BOTS; ++i)
	{
		auto &c{Bot_setup.bots[i]};
		c = {};
		if (i >= Bot_setup.count)
			continue;
		const auto &e{p.bots[i]};
		c.skill = e.skill;
		c.style = e.style;
		c.team = e.team;
		c.profile = e.profile;
	}
	/* Names after every line is in place, so that a missing or taken
	 * one gets the next built-in name no other bot has.
	 */
	for (unsigned i = 0; i < Bot_setup.count; ++i)
	{
		const char *const n{p.bots[i].name.data()};
		/* A reserved word (section 6.4) from a profile written by hand
		 * or by an older build: the next built-in name instead.
		 */
		if (n[0] && !name_in_use(n, i) && !b::name_reserved(n))
			set_name(Bot_setup.bots[i].name, n);
	}
	for (unsigned i = 0; i < Bot_setup.count; ++i)
		if (!Bot_setup.bots[i].name[0u])
			assign_next_name(i);
	/* The developer switch wins for the first setup of the session. */
	if (first && CGameArg.MplBots)
	{
		const unsigned n{std::min<unsigned>(CGameArg.MplBots, MAX_BOTS)};
		if (n < Bot_setup.count)
			Bot_setup.count = n;
		else
			set_count(n);
	}
}

b::bot_profile bots_setup_profile()
{
	b::bot_profile p;
	p.count = std::min<unsigned>(Bot_setup.count, b::BOT_PROFILE_MAX_BOTS);
	p.default_skill = Bot_setup.default_skill;
	p.default_style = Bot_setup.default_style;
	p.default_profile = Bot_setup.default_profile;
	p.replace = Bot_setup.replace;
	for (unsigned i = 0; i < p.count; ++i)
	{
		const auto &c{Bot_setup.bots[i]};
		auto &e{p.bots[i]};
		std::snprintf(e.name.data(), e.name.size(), "%s", static_cast<const char *>(c.name));
		e.skill = c.skill;
		e.style = c.style;
		e.team = c.team;
		e.profile = c.profile;
	}
	return p;
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
	const auto &first{Bot_setup.bots[0]};
	const bool mixed{std::ranges::any_of(std::span(Bot_setup.bots.data(), Bot_setup.count), [&first](const bot_config &c) { return c.skill != first.skill || c.style != first.style || c.profile != first.profile; })};
	if (mixed)
		std::snprintf(buf, size, "Bots: %u (mixed)...", Bot_setup.count);
	else
		std::snprintf(buf, size, "Bots: %u (%s, %s)...", Bot_setup.count, skill_name(first.skill), config_style_name(first.style, first.profile));
}

void bots_setup_menu(const network_game_type mode, const unsigned max_players)
{
	bots_setup_init();
	/* Section 9.13: files added or changed since. */
	bots_load_styles(false);
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
				Bot_setup.bots[i].profile = Bot_setup.default_profile;
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

/* Section 6.4: managing the bots of the game being played. */

namespace {

constexpr int MENU_ADD_BOT{-6};
constexpr int MENU_ADD_BOT_CHOOSE{-7};
constexpr int MENU_SAVE_DEFAULT{-8};
/* The bot being edited left the game, or the game is over. */
constexpr int MENU_GONE{-9};

using bot_list = std::array<bot_in_game, MAX_BOTS>;

/* Who closed a screen that returns -1: the host (Escape, the mouse), or
 * the game, which closes the menus in front of it when the host is hit,
 * dies or the level ends (game_leave_menus).  The game closes a window
 * from its own frame, so the screen's last event then was not the key
 * or the click that closes it.
 */
struct close_watch
{
	bool by_user{};
	bool forced{};
	void see(const d_event &event)
	{
		switch (event.type)
		{
			case event_type::key_command:
				by_user = event_key_get(event) == KEY_ESC;
				break;
			case event_type::mouse_button_down:
			case event_type::mouse_button_up:
				by_user = true;
				break;
			case event_type::window_close:
				forced = !by_user;
				break;
			default:
				by_user = false;
				break;
		}
	}
};

/* The bot a screen shows is still that bot (its slot may have gone to a
 * human, or to another bot, while the screen was open).
 */
[[nodiscard]]
bool still_playing(const bot_in_game &bot)
{
	return bots_manageable() && bot_is_local(bot.pid) && bot_added_order(bot.pid) == bot.added;
}

[[nodiscard]]
const char *team_label(const playernum_t pid)
{
	if (!(Game_mode & GM_TEAM))
		return "";
	return multi_get_team_from_player(Netgame, pid) == team_number::red ? "Red" : "Blue";
}

/* A message with "OK" in front of the running game.  False if the game
 * closed it (game_leave_menus: the host was hit, died, the level
 * ended): the Bots screens then stay closed instead of coming back in
 * front of a game that wants the host's attention.
 */
int notice_handler(newmenu *, const d_event &event, close_watch *const watch)
{
	watch->see(event);
	return 0;
}

[[nodiscard]]
bool notice(const char *const text)
{
	close_watch watch;
	std::array<newmenu_item, 1> m;
	nm_set_item_menu(m[0], TXT_OK);
	const int r{newmenu_do2(menu_title{"BOTS"}, menu_subtitle{text}, m, notice_handler, &watch, 0)};
	return !(r == -1 && watch.forced) && bots_manageable();
}

void say_add_failure(const b::add_verdict why, char *const buf, const std::size_t size)
{
	if (why == b::add_verdict::full)
		std::snprintf(buf, size, "The game is full (%u of %u players)", bots_players_in_game(), static_cast<unsigned>(Netgame.max_numplayers));
	else
		std::snprintf(buf, size, "%s", b::add_verdict_text(why));
}

/* The per-bot screen in the game (section 6.3): an existing bot, or the
 * bot about to be added.
 */
struct ingame_edit : bot_edit_menu
{
	std::optional<bot_in_game> existing;
	close_watch watch;
};

int ingame_edit_handler(newmenu *, const d_event &event, ingame_edit *const e)
{
	e->watch.see(event);
	if (event.type == event_type::idle && (e->existing ? !still_playing(*e->existing) : !bots_manageable()))
		return MENU_GONE;
	const auto r{bot_edit_handler(nullptr, event, e)};
	/* Enter on the name or a slider does not close the screen. */
	if (!r && event.type == event_type::newmenu_selected)
		return 1;
	return r;
}

/* Returns false if the Bots screens are to close: the bot or the game
 * was gone, or the game closed the screen.  Only "Done" (or "Add this
 * bot", "Remove this bot") changes anything: Escape discards, and so
 * does a screen the game closed under the host's hands.
 */
bool run_ingame_edit(const bot_in_game *const existing)
{
	ingame_edit e{};
	if (existing)
		e.existing = *existing;
	const bot_config start{existing ? existing->cfg : bot_config{{}, Bot_game.default_skill, Bot_game.default_style, b::bot_team::automatic, Bot_game.default_profile}};
	e.team_mode = (Game_mode & GM_TEAM) != game_mode_flags{};
	std::snprintf(e.name_text.data(), e.name_text.size(), "%s", static_cast<const char *>(start.name));
	/* A playing bot shows the team it is on. */
	const auto team{existing && e.team_mode ? (multi_get_team_from_player(Netgame, existing->pid) == team_number::red ? b::bot_team::red : b::bot_team::blue) : start.team};
	nm_set_item_text(e.m[bot_edit_menu::label_name], existing ? "Name:" : "Name (empty: the next built-in name):");
	nm_set_item_input(e.m[bot_edit_menu::name], e.name_text);
	nm_set_item_slider(e.m[bot_edit_menu::skill], e.skill_text, static_cast<unsigned>(start.skill), 0, b::BOT_SKILL_COUNT - 1, e.skill_saved);
	nm_set_item_slider(e.m[bot_edit_menu::style], e.style_text, style_choice(start.style, start.profile), 0, style_choice_max(), e.style_saved);
	e.note_missing(start.style, start.profile);
	if (e.team_mode)
		nm_set_item_slider(e.m[bot_edit_menu::team], e.team_text, static_cast<unsigned>(team), 0, b::BOT_TEAM_COUNT - 1, e.team_saved);
	else
		nm_set_item_text(e.m[bot_edit_menu::team], e.team_text);
	nm_set_item_text(e.m[bot_edit_menu::blank], "");
	/* `remove` closes with MENU_REBUILD, `done` with MENU_DONE. */
	nm_set_item_menu(e.m[bot_edit_menu::remove], existing ? "Remove this bot" : "Add this bot");
	nm_set_item_menu(e.m[bot_edit_menu::done], existing ? "Done" : "Cancel");
	e.update_labels();
	const int r{newmenu_do2(menu_title{existing ? "BOT" : "NEW BOT"}, menu_subtitle{existing ? static_cast<const char *>(start.name) : nullptr}, e.m, ingame_edit_handler, &e, existing ? bot_edit_menu::done : bot_edit_menu::remove)};
	if (r == MENU_GONE || !bots_manageable() || (r == -1 && e.watch.forced))
		return false;
	if (r != MENU_DONE && r != MENU_REBUILD)
		return true;
	bot_config c{start};
	c.skill = b::bot_skill{static_cast<uint8_t>(e.m[bot_edit_menu::skill].value)};
	if (static_cast<unsigned>(e.m[bot_edit_menu::style].value) != style_choice(start.style, start.profile))
		set_style_choice(static_cast<unsigned>(e.m[bot_edit_menu::style].value), c.style, c.profile);
	if (e.team_mode)
		c.team = b::bot_team{static_cast<uint8_t>(e.m[bot_edit_menu::team].value)};
	if (!existing)
	{
		/* Only "Add this bot" adds ("Cancel" is the screen's `done`). */
		if (r != MENU_REBUILD)
			return true;
		const auto clean{b::sanitize_name(e.name_text.data())};
		/* No bot is called by a word `/bot` reads as something else:
		 * said here, not answered with a built-in name.
		 */
		if (b::name_reserved(clean.data()))
		{
			char msg[96];
			std::snprintf(msg, sizeof(msg), "A bot cannot be called '%s':\n'all', skills, styles and\n/bot commands are not names.", clean.data());
			return notice(msg);
		}
		set_name(c.name, clean.data());
		b::add_verdict why;
		if (!bots_add(c, why))
		{
			char msg[64];
			say_add_failure(why, msg, sizeof(msg));
			return notice(msg);
		}
		return true;
	}
	if (!still_playing(*existing))
		return false;
	if (r == MENU_REBUILD)
	{
		bots_remove(existing->pid);
		return true;
	}
	bots_set_skill_style(existing->pid, c.skill, c.style, c.profile);
	if (e.team_mode && c.team != team)
		bots_set_team(existing->pid, c.team);
	/* An empty name keeps the old one, as does an untouched one (a name
	 * from the setup may hold characters a rename would drop).
	 */
	if (e.name_text[0] && d_stricmp(e.name_text.data(), static_cast<const char *>(start.name)))
	{
		const auto clean{b::sanitize_name(e.name_text.data())};
		if (b::name_reserved(clean.data()))
		{
			char msg[112];
			std::snprintf(msg, sizeof(msg), "'%s' keeps its name: 'all',\nskills, styles and /bot commands\nare not names.", static_cast<const char *>(start.name));
			return notice(msg);
		}
		bots_rename(existing->pid, e.name_text.data());
	}
	return true;
}

/* The in-game Bots screen: the options of this game, the bots playing,
 * add, save.  Rebuilt whenever the bots in the game change (by this
 * screen, by `/kick`, or by a human who replaces a bot).
 */
struct ingame_menu
{
	static constexpr unsigned first_line{5};
	static constexpr unsigned opt_skill{0}, opt_style{1}, opt_replace{2};
	unsigned opt_add{}, opt_add_choose{}, opt_save{}, opt_done{};
	unsigned nitems{};
	bot_list bots{};
	unsigned listed{};
	std::array<newmenu_item, first_line + MAX_BOTS + 6> m;
	char skill_text[48]{};
	char style_text[56]{};
	char players_text[48]{};
	std::array<std::array<char, 64>, MAX_BOTS> lines{};
	ntstring<NM_MAX_TEXT_LEN> skill_saved, style_saved;
	close_watch watch;
	void update_labels()
	{
		std::snprintf(skill_text, sizeof(skill_text), "New bots' skill: %s", skill_name(Bot_game.default_skill));
		std::snprintf(style_text, sizeof(style_text), "New bots' style: %s", config_style_name(Bot_game.default_style, Bot_game.default_profile));
	}
	void build()
	{
		update_labels();
		listed = bots_in_game(bots);
		std::snprintf(players_text, sizeof(players_text), "Players: %u of %u, %u bot%s", bots_players_in_game(), static_cast<unsigned>(Netgame.max_numplayers), listed, listed == 1 ? "" : "s");
		unsigned n{0};
		nm_set_item_slider(m[n++], skill_text, static_cast<unsigned>(Bot_game.default_skill), 0, b::BOT_SKILL_COUNT - 1, skill_saved);
		nm_set_item_slider(m[n++], style_text, style_choice(Bot_game.default_style, Bot_game.default_profile), 0, style_choice_max(), style_saved);
		nm_set_item_checkbox(m[n++], "Humans replace bots when full", Bot_game.replace);
		nm_set_item_text(m[n++], players_text);
		nm_set_item_text(m[n++], "");
		for (unsigned i = 0; i < listed; ++i)
		{
			const auto &bot{bots[i]};
			std::array<char, 8> st;
			config_style_short(bot.cfg.style, bot.cfg.profile, st);
			std::snprintf(lines[i].data(), lines[i].size(), "%u. %-8s  %-7s %-7s %s", i + 1, static_cast<const char *>(bot.cfg.name), skill_name(bot.cfg.skill), st.data(), team_label(bot.pid));
			nm_set_item_menu(m[n++], lines[i].data());
		}
		if (!listed)
			nm_set_item_text(m[n++], "(no bots in the game)");
		nm_set_item_text(m[n++], "");
		opt_add = n;
		nm_set_item_menu(m[n++], "Add a bot");
		opt_add_choose = n;
		nm_set_item_menu(m[n++], "Add a bot: name, skill, style...");
		opt_save = n;
		nm_set_item_menu(m[n++], "Save as default setup");
		opt_done = n;
		nm_set_item_menu(m[n++], "Done");
		nitems = n;
	}
	/* The bots in the game are no longer the ones listed. */
	[[nodiscard]]
	bool stale() const
	{
		bot_list now{};
		const auto n{bots_in_game(now)};
		if (n != listed)
			return true;
		for (unsigned i = 0; i < n; ++i)
			if (now[i].pid != bots[i].pid || now[i].added != bots[i].added)
				return true;
		return false;
	}
};

int ingame_menu_handler(newmenu *, const d_event &event, ingame_menu *const im)
{
	im->watch.see(event);
	switch (event.type)
	{
		case event_type::idle:
			if (!bots_manageable())
				return MENU_GONE;
			if (im->stale())
				return MENU_REBUILD;
			return 0;
		case event_type::newmenu_changed:
		{
			const auto citem{static_cast<unsigned>(static_cast<const d_change_event &>(event).citem)};
			if (citem == ingame_menu::opt_skill)
				Bot_game.default_skill = b::bot_skill{static_cast<uint8_t>(im->m[ingame_menu::opt_skill].value)};
			else if (citem == ingame_menu::opt_style)
				set_style_choice(static_cast<unsigned>(im->m[ingame_menu::opt_style].value), Bot_game.default_style, Bot_game.default_profile);
			else if (citem == ingame_menu::opt_replace)
				Bot_game.replace = im->m[ingame_menu::opt_replace].value != 0;
			im->update_labels();
			return 0;
		}
		case event_type::newmenu_selected:
		{
			const auto citem{static_cast<unsigned>(static_cast<const d_select_event &>(event).citem)};
			if (citem >= ingame_menu::first_line && citem < ingame_menu::first_line + im->listed)
				return MENU_EDIT_BOT_BASE - static_cast<int>(citem - ingame_menu::first_line);
			if (citem == im->opt_add)
				return MENU_ADD_BOT;
			if (citem == im->opt_add_choose)
				return MENU_ADD_BOT_CHOOSE;
			if (citem == im->opt_save)
				return MENU_SAVE_DEFAULT;
			if (citem == im->opt_done)
				return MENU_DONE;
			return 1;
		}
		default:
			return 0;
	}
}

/* The host's HUD and console: the answer to a /bot command. */
void reply(const char *const text)
{
	HUD_init_message(HM_MULTI, "%s", text);
}

void command_list()
{
	bot_list bots{};
	const auto n{bots_in_game(bots)};
	if (!n)
	{
		reply("No bots in the game");
		return;
	}
	for (unsigned i = 0; i < n; ++i)
	{
		const auto &bot{bots[i]};
		char line[80];
		const auto team{team_label(bot.pid)};
		std::snprintf(line, sizeof(line), "%u. %s: %s, %s%s%s", i + 1, static_cast<const char *>(bot.cfg.name), skill_name(bot.cfg.skill), config_style_name(bot.cfg.style, bot.cfg.profile), team[0] ? ", " : "", team);
		reply(line);
	}
}

/* The bots a command means: all, or the one named. */
[[nodiscard]]
unsigned command_targets(const b::command &c, const bot_list &bots, const unsigned n, std::array<unsigned, MAX_BOTS> &out)
{
	if (!n)
	{
		reply("No bots in the game");
		return 0;
	}
	if (c.all)
	{
		for (unsigned i = 0; i < n; ++i)
			out[i] = i;
		return n;
	}
	std::array<std::string_view, MAX_BOTS> names;
	for (unsigned i = 0; i < n; ++i)
		names[i] = static_cast<const char *>(bots[i].cfg.name);
	const auto m{b::find_target(std::span<const std::string_view>(names).first(n), c.name.data())};
	char line[80];
	switch (m.result)
	{
		case b::target_result::found:
			out[0] = static_cast<unsigned>(m.index);
			return 1;
		case b::target_result::ambiguous:
			std::snprintf(line, sizeof(line), "Several bots begin with '%s'", c.name.data());
			break;
		case b::target_result::none:
			std::snprintf(line, sizeof(line), "No bot named '%s' (/bot list)", c.name.data());
			break;
	}
	reply(line);
	return 0;
}

}

bool bots_chat_command(const char *const text)
{
	/* Section 9.13: a loaded style profile's word is a style. */
	const auto words{bots_style_library().words()};
	const auto c{b::parse_command(text, words)};
	if (c.kind == b::command_kind::none)
		return false;
	if (!(Game_mode & GM_NETWORK) || !multi_i_am_master())
	{
		reply("Only the host can manage bots");
		return true;
	}
	if (!bots_manageable())
	{
		reply(b::add_verdict_text(b::add_verdict::wrong_mode));
		return true;
	}
	bot_list bots{};
	const auto n{bots_in_game(bots)};
	std::array<unsigned, MAX_BOTS> targets{};
	char line[96];
	switch (c.kind)
	{
		case b::command_kind::none:
			break;
		case b::command_kind::help:
			reply("/bot add [skill] [style] [name], /bot remove <name|all>");
			reply("/bot skill <name|all> <skill>, /bot style <name|all> <style>");
			reply("/bot list, /bot save (as the default setup)");
			con_printf(CON_NORMAL, "bots: %s", b::BOT_COMMAND_USAGE);
			break;
		case b::command_kind::error:
			reply(c.error);
			break;
		case b::command_kind::list:
			command_list();
			break;
		case b::command_kind::save:
			bots_save_as_default();
			std::snprintf(line, sizeof(line), "Bot setup saved as default: %u bot%s", n, n == 1 ? "" : "s");
			reply(line);
			break;
		case b::command_kind::add:
		{
			bot_config cfg{{}, c.skill.value_or(Bot_game.default_skill), c.style.value_or(Bot_game.default_style), b::bot_team::automatic, c.style ? b::style_name{} : Bot_game.default_profile};
			if (c.profile)
				set_style_choice(b::BOT_STYLE_COUNT + *c.profile, cfg.style, cfg.profile);
			set_name(cfg.name, c.name.data());
			b::add_verdict why;
			if (!bots_add(cfg, why))
			{
				say_add_failure(why, line, sizeof(line));
				reply(line);
			}
			break;
		}
		case b::command_kind::remove:
		{
			const auto count{command_targets(c, bots, n, targets)};
			for (unsigned i = 0; i < count; ++i)
				bots_remove(bots[targets[i]].pid);
			break;
		}
		case b::command_kind::skill:
		case b::command_kind::style:
		{
			const auto count{command_targets(c, bots, n, targets)};
			for (unsigned i = 0; i < count; ++i)
			{
				const auto &bot{bots[targets[i]]};
				/* The skill command keeps the style (and its profile). */
				auto style{bot.cfg.style};
				auto profile{bot.cfg.profile};
				if (c.style)
				{
					style = *c.style;
					profile = {};
				}
				else if (c.profile)
					set_style_choice(b::BOT_STYLE_COUNT + *c.profile, style, profile);
				bots_set_skill_style(bot.pid, c.skill.value_or(bot.cfg.skill), style, profile);
			}
			break;
		}
	}
	return true;
}

void bots_save_as_default()
{
	bot_list bots{};
	const auto n{bots_in_game(bots)};
	Bot_setup.initialized = true;
	Bot_setup.count = n;
	Bot_setup.default_skill = Bot_game.default_skill;
	Bot_setup.default_style = Bot_game.default_style;
	Bot_setup.default_profile = Bot_game.default_profile;
	Bot_setup.replace = Bot_game.replace;
	for (unsigned i = 0; i < MAX_BOTS; ++i)
		Bot_setup.bots[i] = i < n ? bots[i].cfg : bot_config{};
	/* Only the bot lines: every other line of the profile stays as the
	 * host's setup wrote it (the game's Netgame differs from it: the
	 * tracker is forced off in it without a tracker address).
	 */
	write_netgame_profile_bots();
	con_printf(CON_NORMAL, "bots: the %u bots of this game are the default setup now", n);
}

void bots_ingame_menu()
{
	/* Section 9.13: files added or changed since. */
	bots_load_styles(false);
	int citem{-1};
	for (;;)
	{
		if (!bots_manageable())
			return;
		ingame_menu im{};
		im.build();
		if (citem < 0)
			citem = static_cast<int>(im.opt_add);
		const int r{newmenu_do2(menu_title{"BOTS"}, menu_subtitle{"in this game"}, unchecked_partial_range(im.m, im.nitems), ingame_menu_handler, &im, std::min<int>(citem, im.nitems - 1))};
		/* The game kept running while the screen was open: it may be
		 * over, or no longer ours to manage.
		 */
		if (r == MENU_GONE || !bots_manageable())
			return;
		if (r == MENU_REBUILD)
			continue;
		if (r == MENU_ADD_BOT)
		{
			b::add_verdict why;
			if (!bots_add(bot_config{{}, Bot_game.default_skill, Bot_game.default_style, b::bot_team::automatic, Bot_game.default_profile}, why))
			{
				char msg[64];
				say_add_failure(why, msg, sizeof(msg));
				/* The game closed the message: the list stays closed. */
				if (!notice(msg))
					return;
			}
			citem = -1;
			continue;
		}
		if (r == MENU_ADD_BOT_CHOOSE)
		{
			if (!run_ingame_edit(nullptr))
				return;
			citem = -1;
			continue;
		}
		if (r == MENU_SAVE_DEFAULT)
		{
			bots_save_as_default();
			char msg[96];
			std::snprintf(msg, sizeof(msg), "The %u bot%s of this game\nare the default setup now.", im.listed, im.listed == 1 ? "" : "s");
			if (!notice(msg))
				return;
			citem = static_cast<int>(im.opt_save);
			continue;
		}
		if (r <= MENU_EDIT_BOT_BASE)
		{
			const unsigned i{static_cast<unsigned>(MENU_EDIT_BOT_BASE - r)};
			if (i < im.listed && still_playing(im.bots[i]) && !run_ingame_edit(&im.bots[i]))
			{
				/* The game closed the screen, or is over: leave.  A bot
				 * that left while its screen was open: back to the list.
				 */
				if (!bots_manageable() || still_playing(im.bots[i]))
					return;
			}
			citem = static_cast<int>(ingame_menu::first_line + i);
			continue;
		}
		break;
	}
}

}

#endif
