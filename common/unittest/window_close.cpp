/*
 * This file is part of the DXX-Rebirth project <https://github.com/dxx-rebirth/dxx-rebirth/>.
 * It is copyright by its individual contributors, as recorded in the
 * project's Git history.  See COPYING.txt at the top level for license
 * terms and a link to the Git history.
 */
/*
 * Test of closing a window from inside one of its own event handlers
 * (common/arch/sdl/window.cpp): the window must stay allocated until the
 * handler returns, receive no more events, and be deleted exactly once.
 *
 * This is what happens on a multiplayer client when the host leaves: the
 * "Host left the game!" message runs a nested event loop deep inside the
 * network code, and game_leave_menus then closes the windows in front of
 * the game, among them the score screen whose draw handler polled the
 * network; or the game window closes while its key handler waits for
 * the in-game menu.  Before, the window was deleted at once and the
 * handler returned into freed memory (v0.61-exp-22 crash, faulting IP
 * read from a reused allocation).
 *
 * Build and run with SCons:
 *
 *	scons sdl2=1 d1x=0 d2x=1 register_runtime_test_plain_link_targets=1 test-window-close
 *	build/common/test-window-close
 */

#include <cstdio>
#include <cstdlib>
#include <functional>

#include "window.h"

namespace dcx {

grs_canvas *grd_curcanv;

void gr_init_sub_canvas(grs_subcanvas &, grs_canvas &, uint16_t, uint16_t, uint16_t, uint16_t)
{
}

void (con_printf)(con_priority_wrapper, const char *, ...)
{
}

void con_puts(con_priority_wrapper, std::span<const char>)
{
}

void con_puts(con_priority_wrapper, std::span<char>)
{
}

void menu_destroy_hook(window *)
{
}

}

using namespace dcx;

namespace {

void check_failed(const char *const expr, const char *const file, const int line)
{
	std::fprintf(stderr, "%s:%d: check failed: %s\n", file, line, expr);
	std::exit(1);
}

#define CHECK(cond)	do { if (!(cond)) check_failed(#cond, __FILE__, __LINE__); } while (0)

grs_canvas &test_canvas()
{
	alignas(grs_canvas) static unsigned char storage[sizeof(grs_canvas)];
	return *reinterpret_cast<grs_canvas *>(storage);
}

unsigned destroyed;
unsigned close_events;

/* A window whose draw handler runs `on_draw`, and which records what it
 * receives after the draw returns into it.
 */
struct test_window final : window
{
	std::function<window_event_result(test_window &)> on_draw;
	unsigned draws_after_close{};
	bool closed{};
	test_window() :
		window(test_canvas(), 0, 0, 10, 10)
	{
	}
	~test_window() override
	{
		++destroyed;
	}
	window_event_result event_handler(const d_event &event) override
	{
		switch (event.type)
		{
			case event_type::window_draw:
				if (closed)
					++draws_after_close;
				if (on_draw)
				{
					const auto r{on_draw(*this)};
					/* The handler touches its own object after the nested
					 * code closed it: this must still be valid memory.
					 */
					if (closed)
						++draws_after_close;
					return r;
				}
				break;
			case event_type::window_close:
				++close_events;
				closed = true;
				break;
			default:
				break;
		}
		return window_event_result::ignored;
	}
};

void reset_counters()
{
	destroyed = 0;
	close_events = 0;
}

/* Closed from inside its own draw handler (the score screen polled the
 * network; the host-left path closed the menus): deleted on return.
 */
void test_close_inside_own_handler()
{
	reset_counters();
	const auto w{window_create<test_window>()};
	w->on_draw = [](test_window &self) {
		CHECK(window_close(&self));
		CHECK(close_events == 1);
		CHECK(!destroyed);
		CHECK(!self.is_visible());
		CHECK(window_get_front() != &self);
		/* Events to a closed window are not delivered. */
		const auto r{self.send_event(d_event{event_type::window_draw})};
		CHECK(r == window_event_result::deleted);
		CHECK(self.draws_after_close == 0);
		/* Closing again does nothing more. */
		CHECK(window_close(&self));
		CHECK(close_events == 1);
		/* It must not come back. */
		self.set_visible(1);
		CHECK(!self.is_visible());
		return window_event_result::ignored;
	};
	const auto r{w->send_event(d_event{event_type::window_draw})};
	CHECK(r == window_event_result::deleted);
	CHECK(destroyed == 1);
	CHECK(close_events == 1);
	CHECK(window_get_front() == nullptr);
	CHECK(window_get_first() == nullptr);
}

/* The handler asks to be closed after something nested already closed
 * it: one close event, one deletion.
 */
void test_close_then_return_close()
{
	reset_counters();
	const auto w{window_create<test_window>()};
	w->on_draw = [](test_window &self) {
		window_close(&self);
		return window_event_result::close;
	};
	CHECK(w->send_event(d_event{event_type::window_draw}) == window_event_result::deleted);
	CHECK(destroyed == 1);
	CHECK(close_events == 1);
}

/* The in-game menu case: the game window (below) waits in its handler
 * for a nested loop, and inside that loop the game window is closed from
 * the outside.  It is deleted when its own handler returns, and the
 * window list stays consistent for the menu above it.
 */
void test_close_from_nested_loop()
{
	reset_counters();
	const auto game{window_create<test_window>()};
	const auto menu{window_create<test_window>()};
	CHECK(window_get_front() == menu);
	game->on_draw = [menu](test_window &self) {
		/* Nested loop: the game closes (as from multi_quit_game), then
		 * the menu (game_leave_menus).
		 */
		CHECK(window_close(&self));
		CHECK(!destroyed);
		CHECK(window_get_front() == menu);
		CHECK(window_close(menu));
		CHECK(destroyed == 1);
		CHECK(window_get_front() == nullptr);
		return window_event_result::handled;
	};
	CHECK(game->send_event(d_event{event_type::window_draw}) == window_event_result::deleted);
	CHECK(destroyed == 2);
	CHECK(close_events == 2);
	CHECK(window_get_first() == nullptr);
}

/* A window closed while not in a handler is deleted at once, as before. */
void test_close_outside_handler()
{
	reset_counters();
	const auto w{window_create<test_window>()};
	CHECK(window_close(w));
	CHECK(destroyed == 1);
	CHECK(close_events == 1);
	CHECK(window_get_first() == nullptr);
}

/* A handler returning close still closes and deletes. */
void test_return_close()
{
	reset_counters();
	const auto w{window_create<test_window>()};
	w->on_draw = [](test_window &) {
		return window_event_result::close;
	};
	CHECK(w->send_event(d_event{event_type::window_draw}) == window_event_result::deleted);
	CHECK(destroyed == 1);
	CHECK(close_events == 1);
}

}

int main()
{
	test_close_inside_own_handler();
	test_close_then_return_close();
	test_close_from_nested_loop();
	test_close_outside_handler();
	test_return_close();
	std::puts("window close tests passed");
	return 0;
}
