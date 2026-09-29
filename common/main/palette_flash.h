/*
 * This file is part of the DXX-Rebirth project <https://github.com/dxx-rebirth/dxx-rebirth/>.
 * It is copyright by its individual contributors, as recorded in the
 * project's Git history.  See COPYING.txt at the top level for license
 * terms and a link to the Git history.
 */
/*
 * The curve of the palette effects (PaletteRedAdd, PaletteGreenAdd,
 * PaletteBlueAdd) and of the flash missile's whiteout (Flash_effect),
 * one game frame at a time (diminish_palette_towards_normal in
 * game.cpp).
 *
 * The original Descent 2 (GAME.C) behaves like this:
 *
 * - While Flash_effect is running, the palette effect holds.  The
 *   original decided per frame, with "force_do || rand() > 4096".
 *   force_do is set whenever Time_flash_last_played is not zero, and
 *   the ringing sound of the flash sets Time_flash_last_played on the
 *   flash's first frame, so in practice the whiteout held on every
 *   frame for the whole of Flash_effect.
 *
 * - Afterwards, and for every other palette effect, each channel
 *   diminishes towards zero at DIMINISH_RATE (16) units per second.
 *
 * DXX-Rebirth (2013) replaced the rand() tests with timers.  The
 * replacement of "rand() > 4096" (hold on 7 frames out of 8) held the
 * flash only on the frames where a 26 Hz timer expired, so wherever
 * force_do was not set, the whiteout held on 26 frames per second and
 * decayed on all others: at 30 fps it held on most frames, at 500 fps
 * it faded at almost the full rate.  That timer also lost 1/26 s on
 * every forced frame, so it drifted far below zero during every flash.
 * The decay rate itself truncated FrameTime * 16 to whole units per
 * frame at frame rates below 16 fps (10 fps decayed at 10 units per
 * second instead of 16).
 *
 * This function holds the flash on every frame, as the original
 * effectively did, and carries the fraction of a unit from frame to
 * frame, so that the curve is the same at any frame rate.
 *
 * Standard library only (common/unittest/palette_flash.cpp).
 */

#pragma once

#include <cstdint>

namespace dcx {

/* Units per second at which the palette effects diminish (the original's
 * DIMINISH_RATE).
 */
constexpr int palette_diminish_rate{16};

/* The state that the curve carries from frame to frame. */
struct palette_flash_curve
{
	/* PaletteRedAdd, PaletteGreenAdd, PaletteBlueAdd */
	int &red, &green, &blue;
	/* Flash_effect: the whiteout's remaining time, fixed point with 16
	 * fractional bits.  Descent 1 has no flash: it passes a zero.
	 */
	int32_t &flash_effect;
	/* The fraction of a diminish unit accumulated so far, 16 fractional
	 * bits.
	 */
	int32_t &diminish_remainder;
};

namespace detail {

constexpr void palette_channel_toward_zero(int &c, const int amount)
{
	if (c > 0)
		c = (c < amount) ? 0 : c - amount;
	else if (c < 0)
		c = (c > -amount) ? 0 : c + amount;
}

}

/* Advance the curve by one frame of `frame_time` (fixed point, 16
 * fractional bits).  Returns true if the flash held the palette effect
 * on this frame.
 */
constexpr bool palette_flash_frame(const palette_flash_curve &s, const int32_t frame_time)
{
	if (s.flash_effect > 0)
	{
		/* The whiteout holds for the whole of Flash_effect, including
		 * the frame on which it expires.  The decay starts from a clean
		 * fraction afterwards.
		 */
		s.flash_effect = (s.flash_effect > frame_time) ? s.flash_effect - frame_time : 0;
		s.diminish_remainder = 0;
		return true;
	}
	s.flash_effect = 0;
	const int64_t scaled{int64_t{s.diminish_remainder} + int64_t{frame_time > 0 ? frame_time : 0} * palette_diminish_rate};
	const int amount{static_cast<int>(scaled >> 16)};
	s.diminish_remainder = static_cast<int32_t>(scaled & 0xffff);
	if (amount)
	{
		detail::palette_channel_toward_zero(s.red, amount);
		detail::palette_channel_toward_zero(s.green, amount);
		detail::palette_channel_toward_zero(s.blue, amount);
	}
	return false;
}

}
