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

#include <algorithm>
#include <SDL.h>

#include "args.h"
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

namespace {

/* SDL_Delay(1) may sleep longer than 1 ms, so the frame wait sleeps
 * only while at least this much time remains, and yields for the rest
 * of the wait.  On Windows, SDL_Delay(1) can take up to about 2 ms.
 * Elsewhere, it is precise to about 0.1 ms, so a smaller margin
 * suffices and saves CPU time at lower frame rates.
 */
#ifdef _WIN32
constexpr fix frame_wait_sleep_margin{F1_0 * 2 / 1000};
#else
constexpr fix frame_wait_sleep_margin{F1_0 * 3 / 2 / 1000};
#endif

/* While waiting, process multiplayer packets at most this often.
 * Checking on every pass of the wait would poll the network many
 * thousand times per second.
 */
constexpr fix frame_wait_multi_interval{F1_0 / 1000};

/* With vsync, the buffer swap waits for the monitor, so it paces the
 * frames.  A software bound near the refresh interval would fight the
 * swap: if the bound is a bit longer than the refresh interval, every
 * other refresh is missed, and the frame rate is halved (a 540 Hz
 * monitor would run at 270 fps with a bound of 1/500 s).  Keep only a
 * bound of 1 ms, in case the driver does not wait for vsync.
 */
constexpr int vsync_maximum_fps{1000};

}

fix timer_get_frame_bound()
{
	return F1_0 / (CGameCfg.VSync ? vsync_maximum_fps : CGameArg.SysMaxFPS);
}

fix64 timer_wait_frame(const fix64 deadline)
{
	const auto multiplayer{+(Game_mode & GM_MULTI)};
	const auto may_sleep{!CGameArg.SysNoNiceFPS && !CGameCfg.VSync};
	auto timer_value{timer_update()};
	/* Process packets on the first pass of a wait, as before, and then
	 * at most once per frame_wait_multi_interval.
	 */
	auto next_multi_frame{timer_value};
	while (timer_value < deadline)
	{
		if (multiplayer && timer_value >= next_multi_frame)
		{
			multi_do_frame(); // during long wait, keep packets flowing
			next_multi_frame = timer_value + frame_wait_multi_interval;
		}
		if (may_sleep)
		{
			/* Sleeping close to the deadline would make the frame
			 * late, so during the last frame_wait_sleep_margin of
			 * the wait, only give up the time slice.  This keeps the
			 * frame rate steady at the cost of CPU time: at high
			 * frame rates, the wait uses most of a core.
			 * SDL_Delay(0) yields on Windows (Sleep(0)) and sleeps
			 * for the timer slack (about 50 us on Linux) elsewhere.
			 */
			SDL_Delay(deadline - timer_value >= frame_wait_sleep_margin ? 1 : 0);
		}
		timer_value = timer_update();
	}
	return timer_value;
}

// Replacement for timer_delay which considers calc time the program needs between frames (not reentrant)
void timer_delay_bound(const unsigned caller_bound)
{
	static uint32_t FrameStart;

	uint32_t start = FrameStart;
	const auto multiplayer{+(Game_mode & GM_MULTI)};
	/* With vsync, let the buffer swap pace the screen, but still respect
	 * the menu frame limit.  Screens which ask for a lower rate (such as
	 * 50 fps) run up to the menu limit instead, because a bound longer
	 * than the refresh interval would make the swap miss refreshes
	 * (50 fps on a 60 Hz monitor would become 30 fps).
	 */
	static_assert(1000u / MENU_MAXIMUM_FPS > 0);
	const auto bound{CGameCfg.VSync ? std::min(caller_bound, 1000u / MENU_MAXIMUM_FPS) : caller_bound};
	for (;;)
	{
		const uint32_t tv_now = SDL_GetTicks();
		if (multiplayer)
			multi_do_frame(); // during long wait, keep packets flowing
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
