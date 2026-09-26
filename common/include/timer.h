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
/* Wait a short moment in a frame limiter loop, which still has to wait
 * for `remaining` time before the next frame.
 */
void timer_delay_frame_step(fix64 remaining);
static inline void timer_delay2(int fps)
{
	timer_delay_bound(1000u / fps);
}

}
#endif
