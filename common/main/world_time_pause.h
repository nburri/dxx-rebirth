/*
 * This file is part of the DXX-Rebirth project <https://github.com/dxx-rebirth/dxx-rebirth/>.
 * It is copyright by its individual contributors, as recorded in the
 * project's Git history.  See COPYING.txt at the top level for license
 * terms and a link to the Git history.
 */
/*
 * The pause count of the game world time (stop_time, start_time,
 * pause_game_world_time in game.cpp).
 *
 * While the world time is paused, the game window does not process game
 * frames, and the frame timer's last value holds the time that had
 * elapsed since the last frame instead of a point in time, so that the
 * pause does not count as frame time.
 *
 * The pauses must balance.  One path did not: in a multiplayer game, the
 * game window does not pause the time when a menu covers it, but resumes
 * it when it becomes active again if the time is paused.  A menu opened
 * while a pause_game_world_time was held (the demo recorder's "not
 * enough space" prompt, opened from inside a demo write) therefore had
 * its pause taken away by the game window, and the pause's own release
 * then decremented a count of zero.  The count wrapped to 2^32 - 1, and
 * the game never processed another frame: the game hung.  Releasing a
 * pause that is not held is now ignored.
 *
 * Standard library only (common/unittest/world_time_pause.cpp).
 */

#pragma once

#include <cstdint>

namespace dcx {

class world_time_pause_count
{
	unsigned count{};
public:
	explicit operator bool() const
	{
		return count;
	}
	unsigned get() const
	{
		return count;
	}
	/* Pause the world time at the timer value `now`.  The first pause
	 * turns `last_timer_value` into the time elapsed since it.
	 */
	void pause(const int64_t now, int64_t &last_timer_value)
	{
		if (count == 0)
		{
			last_timer_value = now - last_timer_value;
			if (last_timer_value < 0)
				last_timer_value = 0;
		}
		++count;
	}
	/* Release one pause at the timer value `now`.  The last release
	 * turns `last_timer_value` back into a point in time, so that the
	 * next frame time excludes the pause.  Returns false, and changes
	 * nothing, if the time is not paused.
	 */
	bool resume(const int64_t now, int64_t &last_timer_value)
	{
		if (count == 0)
			return false;
		if (--count == 0)
			last_timer_value = now - last_timer_value;
		return true;
	}
};

/* Whether a demo write of `nelem` elements of `elsize` bytes each wrote
 * everything, given the byte count PHYSFS_writeBytes returned.  The check
 * used to compare the byte count with `nelem`, as if it were an element
 * count (PHYSFS_write returned elements): every write of a short, an int
 * or a fix appeared to fail, so every recording stopped in its first
 * bytes with the "not enough space" prompt.
 */
constexpr bool demo_write_complete(const int64_t bytes_written, const int elsize, const int nelem)
{
	return bytes_written >= 0 && bytes_written == static_cast<int64_t>(elsize) * nelem;
}

}
