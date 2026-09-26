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
static inline void timer_delay2(int fps)
{
	timer_delay_bound(1000u / fps);
}

}
#endif
