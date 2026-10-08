/*
 * This file is part of the DXX-Rebirth project <https://github.com/dxx-rebirth/dxx-rebirth/>.
 * It is copyright by its individual contributors, as recorded in the
 * project's Git history.  See COPYING.txt at the top level for license
 * terms and a link to the Git history.
 */
/*
 * The ship menu (Options -> Ship...): the pilot's custom ship, chosen
 * from the ships found, with a rotating preview in the player colours
 * (Documentation/custom-ships.md section 4.2).  The bot screens use the
 * same window to choose a bot's ship, with "Random" as the first row
 * (Documentation/multiplayer-bots.md section 9.20).
 */

#include "dxxsconf.h"
#include <algorithm>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "custom_ship.h"
#include "window.h"
#include "event.h"
#include "key.h"
#include "mouse.h"
#include "gr.h"
#include "gamefont.h"
#include "palette.h"
#include "gamepal.h"
#include "playsave.h"
#include "timer.h"
#include "game.h"

namespace dsx {

namespace {

namespace cs = ::dcx::custom_ship;

constexpr unsigned PREVIEW_COLOURS{8};

/* The bot screens' use of the window: the bot's name for the title, the
 * label of the "Random" row, and where the choice goes.
 */
struct bot_pick
{
	std::string title;
	std::string random_label;
	std::shared_ptr<std::optional<unsigned>> result{std::make_shared<std::optional<unsigned>>()};
	/* The window was closed by the pilot (a key or a click), not by the
	 * game (game_leave_menus).
	 */
	std::shared_ptr<bool> by_user{std::make_shared<bool>()};
};

struct ship_menu_window : window, mixin_trackable_window
{
	/* 0 is the Pyro-GX, then the ships found; for a bot 0 is Random,
	 * 1 the Pyro-GX.
	 */
	unsigned selected{};
	unsigned first_visible{};
	unsigned colour{};
	std::vector<cs::entry> ships;
	std::optional<bot_pick> bot;
	ship_menu_window(grs_canvas &src, const int x, const int y, const int w, const int h, std::optional<bot_pick> for_bot = std::nullopt, const unsigned start = 0) :
		window(src, x, y, w, h), bot(std::move(for_bot))
	{
		cs::rescan();
		ships = cs::list();
		if (bot)
		{
			selected = std::min(start, count() - 1);
			return;
		}
		const std::string_view current{PlayerCfg.ShipName.data()};
		for (std::size_t i = 0; i < ships.size(); ++i)
			if (!current.empty() && ships[i].name == current)
				selected = static_cast<unsigned>(i + 1);
	}
	/* The rows before the ships: Random (bots), the Pyro-GX. */
	[[nodiscard]]
	unsigned first_ship() const
	{
		return bot ? 2 : 1;
	}
	[[nodiscard]]
	unsigned count() const
	{
		return static_cast<unsigned>(ships.size() + first_ship());
	}
	[[nodiscard]]
	const cs::entry *entry_of(const unsigned i) const
	{
		return i >= first_ship() ? &ships[i - first_ship()] : nullptr;
	}
	[[nodiscard]]
	bool is_random(const unsigned i) const
	{
		return bot && i == 0;
	}
	void close_by_user()
	{
		if (bot)
			*bot->by_user = true;
	}
	/* The layout, shared by drawing and the mouse: the list on the
	 * left, the preview with the selected ship's credits on the right,
	 * three help lines at the bottom.  Every height comes from the scaled
	 * font, so nothing overlaps at any resolution.
	 */
	struct layout
	{
		int list_x, list_y, row_h, rows, list_w;
		int preview_x, preview_y, preview_w, preview_h;
		int info_y, help_y, line_h;
	};
	[[nodiscard]]
	static layout get_layout(const grs_canvas &canvas, const grs_font &font, const grs_font &title_font)
	{
		const int w{canvas.cv_bitmap.bm_w}, h{canvas.cv_bitmap.bm_h};
		layout l;
		/* The scaled height of a line of text and its spacing. */
		const int text_h{std::max(static_cast<int>(gr_get_string_size(font, "Ag").height), 1)};
		l.line_h = std::max(static_cast<int>(LINE_SPACING(font, font) + 0.5f), text_h + 1);
		l.row_h = l.line_h + std::max(text_h / 3, 2);
		const int margin{w / 20};
		l.list_x = margin + 2 * static_cast<int>(gr_get_string_size(font, "> ").width);
		l.list_y = h / 20 + static_cast<int>(gr_get_string_size(title_font, "Ship").height) + l.line_h;
		l.help_y = h - h / 30 - 3 * l.line_h;
		l.list_w = w * 2 / 5 - l.list_x;
		l.rows = std::max(1, (l.help_y - l.line_h - l.list_y) / l.row_h);
		l.preview_x = w * 9 / 20;
		l.preview_w = w - margin - l.preview_x;
		l.preview_y = l.list_y;
		/* Three lines of credits below the preview. */
		l.preview_h = std::max(1, l.help_y - l.line_h - 3 * l.line_h - l.preview_y);
		l.info_y = l.preview_y + l.preview_h + l.line_h / 2;
		return l;
	}
	void choose()
	{
		if (bot)
		{
			*bot->result = selected;
			*bot->by_user = true;
			return;
		}
		const auto e{entry_of(selected)};
		PlayerCfg.ShipName = {};
		if (e)
			PlayerCfg.ShipName.copy_if(e->name.c_str(), e->name.size() + 1);
		write_player_file();
	}
	void select(const int delta)
	{
		const int n{static_cast<int>(count())};
		selected = static_cast<unsigned>(std::clamp(static_cast<int>(selected) + delta, 0, n - 1));
	}
	void draw(grs_canvas &canvas);
	virtual window_event_result event_handler(const d_event &) override;
};

/* `text`, shortened with "..." to at most `width` pixels. */
std::string fit(const grs_font &font, std::string text, const int width)
{
	if (static_cast<int>(gr_get_string_size(font, text.c_str()).width) <= width)
		return text;
	while (!text.empty())
	{
		text.pop_back();
		const auto shortened{text + "..."};
		if (static_cast<int>(gr_get_string_size(font, shortened.c_str()).width) <= width)
			return shortened;
	}
	return {};
}

void ship_menu_window::draw(grs_canvas &canvas)
{
	gr_clear_canvas(canvas, BM_XRGB(0, 0, 0));
	const int w{canvas.cv_bitmap.bm_w};
	auto &title_font{*MEDIUM1_FONT};
	auto &font{*GAME_FONT};
	const int margin{w / 20};
	gr_set_fontcolor(canvas, BM_XRGB(28, 28, 28), -1);
	gr_string(canvas, title_font, margin, canvas.cv_bitmap.bm_h / 20, bot ? fit(title_font, bot->title, w - 2 * margin).c_str() : "Ship");
	const auto l{get_layout(canvas, font, title_font)};
	if (selected < first_visible)
		first_visible = selected;
	if (selected >= first_visible + static_cast<unsigned>(l.rows))
		first_visible = selected - static_cast<unsigned>(l.rows) + 1;
	const int text_pad{(l.row_h - static_cast<int>(gr_get_string_size(font, "Ag").height)) / 2};
	for (unsigned i = first_visible; i < count() && i < first_visible + static_cast<unsigned>(l.rows); ++i)
	{
		const auto e{entry_of(i)};
		const int y{l.list_y + static_cast<int>(i - first_visible) * l.row_h};
		if (i == selected)
		{
			/* A bar behind the selected row. */
			gr_urect(canvas, l.list_x - 3, y, l.list_x + l.list_w, y + l.row_h - 2, BM_XRGB(6, 8, 18));
			gr_set_fontcolor(canvas, BM_XRGB(31, 31, 10), -1);
			gr_string(canvas, font, margin, y + text_pad, ">");
		}
		else
			gr_set_fontcolor(canvas, e && e->cached ? BM_XRGB(16, 20, 26) : BM_XRGB(22, 22, 22), -1);
		gr_string(canvas, font, l.list_x, y + text_pad, fit(font, e ? e->title : is_random(i) ? bot->random_label : std::string{"Pyro-GX (standard)"}, l.list_w - 4).c_str());
	}
	/* More rows above or below. */
	gr_set_fontcolor(canvas, BM_XRGB(16, 16, 16), -1);
	if (first_visible)
		gr_string(canvas, font, l.list_x, l.list_y - l.line_h, "...");
	if (first_visible + static_cast<unsigned>(l.rows) < count())
		gr_string(canvas, font, l.list_x, l.list_y + l.rows * l.row_h, "...");
	/* The preview, turning, in its own area (none for Random). */
	if (!is_random(selected))
	{
		auto sub{gr_create_sub_canvas(canvas, static_cast<uint16_t>(l.preview_x), static_cast<uint16_t>(l.preview_y), static_cast<uint16_t>(l.preview_w), static_cast<uint16_t>(l.preview_h))};
		const fix64 t{timer_query()};
		const vms_angvec angles{static_cast<fixang>(-0x1400), static_cast<fixang>(0), static_cast<fixang>((t / 3) & 0xffff)};
		custom_ship_draw_preview(*sub, entry_of(selected), angles, colour);
	}
	/* Who made the selected ship, below the preview. */
	gr_set_fontcolor(canvas, BM_XRGB(24, 24, 24), -1);
	if (const auto e{entry_of(selected)})
	{
		gr_string(canvas, font, l.preview_x, l.info_y, fit(font, e->title + " by " + (e->author.empty() ? std::string{"unknown"} : e->author), l.preview_w).c_str());
		gr_string(canvas, font, l.preview_x, l.info_y + l.line_h, fit(font, "Licence " + e->licence + (e->cached ? " (received from a host)" : ""), l.preview_w).c_str());
		if (!e->source.empty())
			gr_string(canvas, font, l.preview_x, l.info_y + 2 * l.line_h, fit(font, e->source, l.preview_w).c_str());
	}
	else if (is_random(selected))
	{
		gr_string(canvas, font, l.preview_x, l.info_y, fit(font, "Its style's ship if it names one,", l.preview_w).c_str());
		gr_string(canvas, font, l.preview_x, l.info_y + l.line_h, fit(font, "else one of yours, by the bot's name.", l.preview_w).c_str());
	}
	else
	{
		gr_string(canvas, font, l.preview_x, l.info_y, fit(font, "The original ship.", l.preview_w).c_str());
		gr_string(canvas, font, l.preview_x, l.info_y + l.line_h, fit(font, "Others see your ship; it is only its look.", l.preview_w).c_str());
	}
	/* How to use the menu. */
	const int help_w{w - 2 * margin};
	gr_set_fontcolor(canvas, BM_XRGB(18, 18, 18), -1);
	if (bot)
	{
		gr_string(canvas, font, margin, l.help_y, fit(font, "The bot flies it; players who do not show custom ships see a Pyro-GX.", help_w).c_str());
		gr_string(canvas, font, margin, l.help_y + 2 * l.line_h, fit(font, "Up/Down: choose, C: colour, Enter: take it, Esc: back", help_w).c_str());
		return;
	}
	gr_string(canvas, font, margin, l.help_y, fit(font, std::string{"Accept ships from the host: "} + (PlayerCfg.AcceptShips ? "yes" : "no") + " (A)", help_w).c_str());
	gr_string(canvas, font, margin, l.help_y + l.line_h, fit(font, std::string{"Show custom ships: "} + (PlayerCfg.ShowCustomShips ? "yes" : "no (everyone is a Pyro-GX)") + " (S)", help_w).c_str());
	gr_string(canvas, font, margin, l.help_y + 2 * l.line_h, fit(font, "Up/Down: choose, C: colour, Enter: fly it, Esc: back", help_w).c_str());
}

window_event_result ship_menu_window::event_handler(const d_event &event)
{
	switch (event.type)
	{
		case event_type::window_activated:
#if DXX_BUILD_DESCENT == 2
			/* In a game the level's palette stays (the Pyro's preview
			 * looks a little off, the HUD keeps its colours).
			 */
			if (!Game_wind)
				gr_use_palette_table("groupa.256");
#endif
			key_toggle_repeat(1);
			break;
		case event_type::key_command:
			switch (event_key_get(event))
			{
				case KEY_ESC:
					close_by_user();
					return window_event_result::close;
				case KEY_UP:
				case KEY_PAD8:
					select(-1);
					return window_event_result::handled;
				case KEY_DOWN:
				case KEY_PAD2:
					select(1);
					return window_event_result::handled;
				case KEY_PAGEUP:
					select(-8);
					return window_event_result::handled;
				case KEY_PAGEDOWN:
					select(8);
					return window_event_result::handled;
				case KEY_HOME:
					select(-static_cast<int>(count()));
					return window_event_result::handled;
				case KEY_END:
					select(static_cast<int>(count()));
					return window_event_result::handled;
				case KEY_C:
					colour = (colour + 1) % PREVIEW_COLOURS;
					return window_event_result::handled;
				case KEY_A:
					if (bot)
						break;
					PlayerCfg.AcceptShips = !PlayerCfg.AcceptShips;
					write_player_file();
					return window_event_result::handled;
				case KEY_S:
					if (bot)
						break;
					PlayerCfg.ShowCustomShips = !PlayerCfg.ShowCustomShips;
					write_player_file();
					/* Pieces of a death drawn the other way go (the Pyro's
					 * debris shows again); decode the ships now, not at the
					 * first sight.
					 */
					custom_ship_level_start();
					custom_ship_preload();
					return window_event_result::handled;
				case KEY_ENTER:
				case KEY_PADENTER:
				case KEY_SPACEBAR:
					choose();
					return window_event_result::close;
				default:
					break;
			}
			break;
		case event_type::mouse_button_down:
			{
				const auto b{event_mouse_get_button(event)};
				if (b == mbtn::z_up)
				{
					select(-1);
					return window_event_result::handled;
				}
				if (b == mbtn::z_down)
				{
					select(1);
					return window_event_result::handled;
				}
				if (b == mbtn::right)
				{
					close_by_user();
					return window_event_result::close;
				}
				if (b != mbtn::left)
					break;
				const auto [mx, my, mz]{mouse_get_pos()};
				(void)mz;
				const auto l{get_layout(grd_curscreen->sc_canvas, *GAME_FONT, *MEDIUM1_FONT)};
				if (mx >= l.list_x && mx < l.list_x + l.list_w && my >= l.list_y)
				{
					const unsigned row{static_cast<unsigned>((my - l.list_y) / l.row_h)};
					if (row < static_cast<unsigned>(l.rows) && first_visible + row < count())
					{
						const unsigned i{first_visible + row};
						if (i == selected)
						{
							choose();
							return window_event_result::close;
						}
						selected = i;
					}
				}
				return window_event_result::handled;
			}
		case event_type::window_draw:
			timer_delay(F1_0 / 60);
			draw(*grd_curcanv);
			break;
		case event_type::window_close:
#if DXX_BUILD_DESCENT == 2
			if (!Game_wind)
				load_palette(MENU_PALETTE, load_palette_use::background, load_palette_change_screen::delayed);
#endif
			key_toggle_repeat(0);
			break;
		default:
			break;
	}
	return window_event_result::ignored;
}

}

void custom_ship_menu()
{
	auto w{window_create<ship_menu_window>(grd_curscreen->sc_canvas, 0, 0, SWIDTH, SHEIGHT)};
	(void)w;
}

std::optional<unsigned> custom_ship_pick_for_bot(const char *const title, const char *const random_label, const unsigned selected, bool &forced)
{
	bot_pick pick;
	pick.title = title;
	pick.random_label = random_label;
	const auto result{pick.result};
	const auto by_user{pick.by_user};
	auto w{window_create<ship_menu_window>(grd_curscreen->sc_canvas, 0, 0, SWIDTH, SHEIGHT, std::move(pick), selected)};
	/* Until it closes, as a newmenu_do2 waits. */
	for (const auto exists{w->track()}; *exists;)
		event_process();
	forced = !*by_user;
	return *result;
}

}
