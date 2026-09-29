/*
 * This file is part of the DXX-Rebirth project <https://github.com/dxx-rebirth/dxx-rebirth/>.
 * It is copyright by its individual contributors, as recorded in the
 * project's Git history.  See COPYING.txt at the top level for license
 * terms and a link to the Git history.
 */
/*
 * Test of the game world time's pause count and of the demo write check
 * (world_time_pause.h): starting a demo recording in a multiplayer game
 * must not leave the world time paused forever.
 *
 * Build and run with SCons:
 *
 *	scons sdl2=1 d1x=0 d2x=1 register_runtime_test_plain_link_targets=1 test-world-time-pause
 *	build/common/test-world-time-pause
 */

#include <cstdint>
#include <cstdio>
#include <cstdlib>

#include "world_time_pause.h"

using namespace dcx;

namespace {

void check_failed(const char *const expr, const char *const file, const int line)
{
	std::fprintf(stderr, "%s:%d: check failed: %s\n", file, line, expr);
	std::exit(1);
}

#define CHECK(cond)	do { if (!(cond)) check_failed(#cond, __FILE__, __LINE__); } while (0)

/* A pause keeps the elapsed frame time, and excludes its own duration. */
void test_pause_excludes_paused_time()
{
	world_time_pause_count p;
	int64_t last{1000};
	CHECK(!p);
	p.pause(1100, last);
	CHECK(p);
	CHECK(last == 100);
	CHECK(p.resume(5100, last));
	CHECK(!p);
	/* 100 units of frame time had elapsed; the 4000 paused do not count. */
	CHECK(last == 5000);
	CHECK(5100 - last == 100);
}

/* Nested pauses: only the outermost converts the timer value. */
void test_nested()
{
	world_time_pause_count p;
	int64_t last{0};
	p.pause(10, last);
	p.pause(20, last);
	CHECK(p.get() == 2);
	CHECK(last == 10);
	CHECK(p.resume(30, last));
	CHECK(last == 10);
	CHECK(p.resume(40, last));
	CHECK(!p);
	CHECK(last == 30);
}

/* A timer value behind the last frame does not make a negative elapsed
 * time.
 */
void test_clock_behind()
{
	world_time_pause_count p;
	int64_t last{500};
	p.pause(400, last);
	CHECK(last == 0);
	CHECK(p.resume(600, last));
	CHECK(last == 600);
}

/* The multiplayer F5 sequence that hung: the demo recorder holds a
 * pause (pause_game_world_time) and opens a menu; the game window does
 * not pause on deactivation in a multiplayer game, but resumes on
 * activation if the time is paused; then the recorder releases its
 * pause.  The count must end at zero, not wrap around.
 */
void test_multiplayer_menu_inside_pause()
{
	world_time_pause_count p;
	int64_t last{1000};
	p.pause(1010, last);			// pause_game_world_time in the recorder
	/* window_deactivated in a multiplayer game: no stop_time */
	if (p)
		CHECK(p.resume(3000, last));	// window_activated: if (time_paused) start_time()
	CHECK(!p);
	CHECK(last == 2990);
	CHECK(!p.resume(3005, last));		// ~pause_game_world_time: ignored
	CHECK(!p);
	CHECK(p.get() == 0);
	CHECK(last == 2990);
	/* The game processes frames again, and pauses still work. */
	p.pause(3010, last);
	CHECK(p.get() == 1);
	CHECK(p.resume(3020, last));
	CHECK(p.get() == 0);
}

/* The same sequence in a single player game balances by itself. */
void test_single_player_menu_inside_pause()
{
	world_time_pause_count p;
	int64_t last{1000};
	p.pause(1010, last);			// recorder
	p.pause(1020, last);			// window_deactivated: stop_time
	CHECK(p.resume(3000, last));		// window_activated: start_time
	CHECK(p.resume(3005, last));		// recorder
	CHECK(!p);
	CHECK(last == 2995);
}

/* PHYSFS_writeBytes returns bytes, not elements. */
void test_demo_write_complete()
{
	CHECK(demo_write_complete(1, 1, 1));	// byte
	CHECK(demo_write_complete(2, 2, 1));	// short
	CHECK(demo_write_complete(4, 4, 1));	// int, fix
	CHECK(demo_write_complete(7, 1, 7));	// string
	CHECK(demo_write_complete(0, 1, 0));
	/* The old check: a short "wrote" 1 element only if 1 byte was
	 * written.  Both a short write and an error are failures.
	 */
	CHECK(!demo_write_complete(1, 2, 1));
	CHECK(!demo_write_complete(3, 4, 1));
	CHECK(!demo_write_complete(-1, 2, 1));
	CHECK(!demo_write_complete(-1, 1, 0));
}

}

int main()
{
	test_pause_excludes_paused_time();
	test_nested();
	test_clock_behind();
	test_multiplayer_menu_inside_pause();
	test_single_player_menu_inside_pause();
	test_demo_write_complete();
	std::puts("test-world-time-pause: all checks passed");
	return 0;
}
