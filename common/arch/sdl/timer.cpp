/*
 * This file is part of the DXX-Rebirth project <https://github.com/dxx-rebirth/dxx-rebirth/>.
 * It is copyright by its individual contributors, as recorded in the
 * project's Git history.  See COPYING.txt at the top level for license
 * terms and a link to the Git history.
 */
/*
 *
 * SDL library timer functions
 *
 */

#include <SDL.h>

#include "fwd-game.h"
#include "maths.h"
#include "timer.h"
#include "config.h"
#include "game.h"
#include "multi.h"

namespace dcx {

static fix64 F64_RunTime = 0;

namespace {

#if SDL_MAJOR_VERSION == 1
/* SDL1 only offers a millisecond clock. */
static fix64 timer_read_clock()
{
	return static_cast<fix64>(SDL_GetTicks()) * F1_0 / 1000;
}
#else
/* Use the high resolution counter, so that the frame limiter can pace
 * frames with sub-millisecond precision.  At 500 fps, a millisecond
 * clock makes frames alternate between 2 ms and 3 ms.
 *
 * The counter is converted relative to its first reading, and split
 * into whole seconds and the remainder, so that the multiplication by
 * F1_0 cannot overflow, no matter how high the counter or its
 * frequency is.
 */
static fix64 timer_read_clock()
{
	static const uint64_t frequency{SDL_GetPerformanceFrequency()};
	static const uint64_t base{SDL_GetPerformanceCounter()};
	const uint64_t elapsed{SDL_GetPerformanceCounter() - base};
	constexpr uint64_t one{F1_0};
	return static_cast<fix64>((elapsed / frequency) * one + (elapsed % frequency) * one / frequency);
}
#endif

}

fix64 timer_update()
{
	static bool already_initialized;
	static fix64 last_tv;
	const fix64 cur_tv = timer_read_clock();
	const fix64 prev_tv = last_tv;
	fix64 runtime = F64_RunTime;
	last_tv = cur_tv;
	if (unlikely(!already_initialized))
	{
		already_initialized = true;
	}
	else if (likely(prev_tv < cur_tv)) // in case the clock wraps, don't update and have a little hickup
		F64_RunTime = (runtime += (cur_tv - prev_tv)); // increment! this value will overflow long after we are all dead... so why bother checking?
	return runtime;
}

fix64 timer_query(void)
{
	return (F64_RunTime);
}

void timer_delay_ms(unsigned milliseconds)
{
	SDL_Delay(milliseconds);
}

// Replacement for timer_delay which considers calc time the program needs between frames (not reentrant)
void timer_delay_bound(const unsigned caller_bound)
{
	static uint32_t FrameStart;

	uint32_t start = FrameStart;
	const auto multiplayer{+(Game_mode & GM_MULTI)};
	const auto vsync{CGameCfg.VSync};
	const auto bound = vsync ? 1000u / MAXIMUM_FPS : caller_bound;
	for (;;)
	{
		const uint32_t tv_now = SDL_GetTicks();
		if (multiplayer)
			multi_do_frame(); // during long wait, keep packets flowing
		if (!vsync)
			SDL_Delay(1);
		if (unlikely(start > tv_now))
			start = tv_now;
		if (unlikely(tv_now - start >= bound))
		{
			FrameStart = tv_now;
			break;
		}
	}
}

}
