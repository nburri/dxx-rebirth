/*
 * This file is part of the DXX-Rebirth project <https://github.com/dxx-rebirth/dxx-rebirth/>.
 * It is copyright by its individual contributors, as recorded in the
 * project's Git history.  See COPYING.txt at the top level for license
 * terms and a link to the Git history.
 */

/*
 *
 * Header for timer functions
 *
 */

#pragma once

#include "maths.h"

#ifdef __cplusplus
namespace dcx {

fix64 timer_update();

[[nodiscard]]
fix64 timer_query();

void timer_delay_ms(unsigned milliseconds);
static inline void timer_delay(fix seconds)
{
	timer_delay_ms(f2i(seconds * 1000));
}
void timer_delay_bound(unsigned bound);
/* Wait until the game timer reaches `deadline`, and return the timer
 * value.  Used by the frame limiters of the game and the automap.
 * While waiting, keep multiplayer packets flowing.
 */
fix64 timer_wait_frame(fix64 deadline);
/* Return the minimum time between two frames of the game or the
 * automap.
 */
[[nodiscard]]
fix timer_get_frame_bound();
static inline void timer_delay2(int fps)
{
	timer_delay_bound(1000u / fps);
}

/* The simulated clock of -botarena (Documentation/multiplayer-bots.md
 * section 8.2): from now on the game timer no longer follows the wall
 * clock.  It starts at `start` and moves only when a frame waits for it
 * (timer_wait_frame jumps to the deadline at once), by `step` per frame
 * of the game (timer_get_frame_bound).  There is no way back.  Nobody
 * watches a simulated game: the game draws no frames then (game.cpp,
 * event.cpp).
 */
void timer_use_simulated_clock(fix64 start, fix step);
/* The game timer is the simulated clock. */
[[nodiscard]]
bool timer_simulated();

}
#endif
