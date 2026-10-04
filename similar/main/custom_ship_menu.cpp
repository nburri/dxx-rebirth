/*
 * This file is part of the DXX-Rebirth project <https://github.com/dxx-rebirth/dxx-rebirth/>.
 * It is copyright by its individual contributors, as recorded in the
 * project's Git history.  See COPYING.txt at the top level for license
 * terms and a link to the Git history.
 */
/*
 * The ship menu (Options -> Ship...): the pilot's custom ship, chosen
 * from the ships found, with a rotating preview in the player colours
 * (Documentation/custom-ships.md section 4.2).
 */

#include "dxxsconf.h"
#include <algorithm>
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

struct ship_menu_window : window
{
	/* 0 is the Pyro-GX, then the ships found. */
	unsigned selected{};
	unsigned first_visible{};
	unsigned colour{};
	std::vector<cs::entry> ships;
	ship_menu_window(grs_canvas &src, const int x, const int y, const int w, const int h) :
		window(src, x, y, w, h)
	{
		cs::rescan();
		ships = cs::list();
		const std::string_view current{PlayerCfg.ShipName.data()};
		for (std::size_t i = 0; i < ships.size(); ++i)
			if (!current.empty() && ships[i].name == current)
				selected = static_cast<unsigned>(i + 1);
	}
	[[nodiscard]]
	unsigned count() const
	{
		return static_cast<unsigned>(ships.size() + 1);
	}
	[[nodiscard]]
	const cs::entry *entry_of(const unsigned i) const
	{
		return i ? &ships[i - 1] : nullptr;
	}
	/* The list's layout, shared by drawing and the mouse. */
	struct layout
	{
		int list_x, list_y, row_h, rows, list_w;
	};
	[[nodiscard]]
	layout get_layout(const grs_canvas &canvas, const grs_font &font) const
	{
		const int w{canvas.cv_bitmap.bm_w}, h{canvas.cv_bitmap.bm_h};
		layout l;
		l.list_x = w / 20;
		l.list_y = h / 6;
		l.row_h = std::max<int>(font.ft_h + font.ft_h / 3, 1);
		l.rows = std::max(1, (h * 3 / 4 - l.list_y) / l.row_h);
		l.list_w = w * 7 / 20;
		return l;
	}
	void choose()
	{
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

void ship_menu_window::draw(grs_canvas &canvas)
{
	gr_clear_canvas(canvas, BM_XRGB(0, 0, 0));
	const int w{canvas.cv_bitmap.bm_w}, h{canvas.cv_bitmap.bm_h};
	auto &title_font{*MEDIUM1_FONT};
	auto &font{*GAME_FONT};
	gr_set_fontcolor(canvas, BM_XRGB(28, 28, 28), -1);
	gr_string(canvas, title_font, w / 20, h / 20, "Ship");
	const auto l{get_layout(canvas, font)};
	if (selected < first_visible)
		first_visible = selected;
	if (selected >= first_visible + static_cast<unsigned>(l.rows))
		first_visible = selected - static_cast<unsigned>(l.rows) + 1;
	for (unsigned i = first_visible; i < count() && i < first_visible + static_cast<unsigned>(l.rows); ++i)
	{
		const auto e{entry_of(i)};
		const int y{l.list_y + static_cast<int>(i - first_visible) * l.row_h};
		if (i == selected)
		{
			gr_set_fontcolor(canvas, BM_XRGB(31, 31, 10), -1);
			gr_string(canvas, font, l.list_x - font.ft_w * 2, y, ">");
		}
		else
			gr_set_fontcolor(canvas, e && e->cached ? BM_XRGB(16, 20, 26) : BM_XRGB(22, 22, 22), -1);
		gr_string(canvas, font, l.list_x, y, e ? e->title.c_str() : "Pyro-GX (standard)");
	}
	/* The preview, turning. */
	{
		const int px{w * 2 / 5}, py{h / 8}, pw{w * 11 / 20}, ph{h * 3 / 5};
		auto sub{gr_create_sub_canvas(canvas, static_cast<uint16_t>(px), static_cast<uint16_t>(py), static_cast<uint16_t>(pw), static_cast<uint16_t>(ph))};
		const fix64 t{timer_query()};
		const vms_angvec angles{static_cast<fixang>(-0x1400), static_cast<fixang>(0), static_cast<fixang>((t / 3) & 0xffff)};
		custom_ship_draw_preview(*sub, entry_of(selected), angles, colour);
	}
	/* Who made it, and how to use the menu. */
	const int ty{h * 3 / 4 + font.ft_h};
	const auto line{LINE_SPACING(font, font)};
	gr_set_fontcolor(canvas, BM_XRGB(24, 24, 24), -1);
	if (const auto e{entry_of(selected)})
	{
		gr_printf(canvas, font, w / 20, ty, "%s by %s, licence %s%s", e->title.c_str(), e->author.empty() ? "unknown" : e->author.c_str(), e->licence.c_str(), e->cached ? " (received from a host)" : "");
		if (!e->source.empty())
			gr_printf(canvas, font, w / 20, static_cast<int>(ty + line), "%s", e->source.c_str());
	}
	else
		gr_string(canvas, font, w / 20, ty, "The original ship. Others see your ship; it is only its look.");
	gr_set_fontcolor(canvas, BM_XRGB(18, 18, 18), -1);
	gr_printf(canvas, font, w / 20, static_cast<int>(ty + 2 * line), "Accept ships from the host: %s (A)", PlayerCfg.AcceptShips ? "yes" : "no");
	gr_string(canvas, font, w / 20, static_cast<int>(ty + 3 * line), "Up/Down: choose, C: colour, Enter: fly it, Esc: back");
}

window_event_result ship_menu_window::event_handler(const d_event &event)
{
	switch (event.type)
	{
		case event_type::window_activated:
#if DXX_BUILD_DESCENT == 2
			gr_use_palette_table("groupa.256");
#endif
			key_toggle_repeat(1);
			break;
		case event_type::key_command:
			switch (event_key_get(event))
			{
				case KEY_ESC:
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
					PlayerCfg.AcceptShips = !PlayerCfg.AcceptShips;
					write_player_file();
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
					return window_event_result::close;
				if (b != mbtn::left)
					break;
				const auto [mx, my, mz]{mouse_get_pos()};
				(void)mz;
				const auto l{get_layout(grd_curscreen->sc_canvas, *GAME_FONT)};
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

void custom_ship_menu_picture(grs_canvas &canvas, const unsigned selected)
{
	const auto w{window_create<ship_menu_window>(grd_curscreen->sc_canvas, 0, 0, SWIDTH, SHEIGHT)};
	w->selected = std::min(selected, w->count() - 1);
	w->draw(canvas);
	window_close(w);
}

void custom_ship_menu()
{
	auto w{window_create<ship_menu_window>(grd_curscreen->sc_canvas, 0, 0, SWIDTH, SHEIGHT)};
	(void)w;
}

}
