/*
 * This file is part of the DXX-Rebirth project <https://github.com/dxx-rebirth/dxx-rebirth/>.
 * It is copyright by its individual contributors, as recorded in the
 * project's Git history.  See COPYING.txt at the top level for license
 * terms and a link to the Git history.
 */
/*
 * -botarena (Documentation/multiplayer-bots.md section 8.2): a headless
 * anarchy game of bots on this machine, for testing the bots on real
 * levels.  The host hosts it without the menus and without the network
 * (the game socket is on the loopback address only), its own ship is a
 * ghost nobody sees, and the game runs on a simulated clock (timer.h)
 * at a fixed frame rate, as fast as the machine can, without drawing.
 * After the given game time it writes a summary to the log (and
 * stdout) and the program ends.
 */

#pragma once

#include <chrono>
#include "dxxsconf.h"
#include "dsx-ns.h"
#include "args.h"
#include "maths.h"

namespace dcx {

/* This run is a -botarena run. */
[[nodiscard]]
static inline bool bot_arena_active()
{
	return CGameArg.DbgBotArenaSeconds != 0;
}

/* The arena's counters, written by the game wherever it sees the event.
 * They do nothing outside an arena run.
 */
void bot_arena_note_fire(unsigned pnum, bool secondary);
void bot_arena_note_damage(unsigned victim, unsigned attacker, fix damage, bool splash);
void bot_arena_note_stuck(unsigned pnum);
void bot_arena_note_path(unsigned pnum, double length);
/* Section 9.19: capture the flag and hoard (net_modes.cpp, on the host):
 * a capture, a flag returned by touch, orbs scored.
 */
void bot_arena_note_capture(unsigned pnum);
void bot_arena_note_flag_return(unsigned pnum);
void bot_arena_note_orb_score(unsigned pnum, unsigned orbs);
/* The host decided a kill by a player (net_combat.cpp): the arena tells
 * the kills of teammates by shots from those by a dying ship's blast
 * (`weapon` 255: no weapon).
 */
void bot_arena_note_kill(unsigned victim, unsigned killer, unsigned weapon);

/* The time the bots' code takes: bots_frame and bots_fire (game.cpp). */
class bot_arena_cpu_scope
{
	/* Outside an arena run, no clock is read. */
	const std::chrono::steady_clock::time_point start{bot_arena_active() ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{}};
public:
	bot_arena_cpu_scope() = default;
	bot_arena_cpu_scope(const bot_arena_cpu_scope &) = delete;
	bot_arena_cpu_scope &operator=(const bot_arena_cpu_scope &) = delete;
	~bot_arena_cpu_scope();
};

}

#ifdef DXX_BUILD_DESCENT
#include "window.h"

namespace dsx {

/* Instead of the main menu: load the mission, host the game, start the
 * level.  False if it could not (the reason is in the log); the program
 * then ends.
 */
[[nodiscard]]
bool bot_arena_start();
/* Once per frame of the game, after everything moved: the first frame
 * parks the host's ship; at the end of the time, the summary, and the
 * game window closes (window_event_result::close).
 */
[[nodiscard]]
window_event_result bot_arena_frame();
/* The program's exit status: 0 if the arena ran its time. */
[[nodiscard]]
int bot_arena_exit_status();

}
#endif
