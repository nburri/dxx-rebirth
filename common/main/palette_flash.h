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
 * The fusion cannon's charging glow (palette_fusion_glow_frame) was
 * the one palette effect added on every frame: the original added
 * Fusion_charge >> 11 units per frame to red and to blue (green after
 * two seconds of charge), so the glow grew faster the higher the frame
 * rate: at 30 fps it reached full strength after about 0.25 s of
 * charge, at 500 fps after about 0.1 s.  The other palette effects are
 * added once per event (a hit, a pickup, a force field bounce, a lava
 * scrape, which is already rate limited), or set from a timer (the
 * reactor countdown, the final boss), so they do not depend on the
 * frame rate.
 *
 * Standard library only (common/unittest/palette_flash.cpp).
 */

#pragma once

#include <algorithm>
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

/* PALETTE_FLASH_ADD for one channel: add, then clamp to +/- maxval
 * (30, or 60 during a flash missile's whiteout).
 */
constexpr void palette_channel_add(int &c, const int d, const int maxval)
{
	c = std::clamp(c + d, -maxval, maxval);
}

/* The frame rate at which the per-frame palette adds of the original
 * look as intended.
 */
constexpr int palette_reference_fps{30};

/* The fusion cannon's charging glow for one frame of `frame_time` (fixed
 * point, 16 fractional bits) at the charge `fusion_charge` (fixed point,
 * after this frame's charge was added).  Returns the units to add to
 * the glowing channels on this frame.
 *
 * The original added Fusion_charge >> 11 units on every frame.  This
 * adds Fusion_charge / 2048 units per 1/30 s, as a rate, and carries the
 * fraction of a unit in `remainder` (27 fractional bits) from frame to
 * frame.  At 30 fps, that is the original's add (the carried fraction
 * stands in for the truncation); at higher frame rates, it follows the
 * charge more smoothly, and reaches full strength about one 30 fps
 * frame later than at 30 fps.
 */
constexpr int palette_fusion_glow_frame(const int32_t fusion_charge, const int32_t frame_time, int32_t &remainder)
{
	constexpr int fraction_bits{16 + 11};
	const int64_t scaled{int64_t{remainder} + int64_t{fusion_charge > 0 ? fusion_charge : 0} * int64_t{frame_time > 0 ? frame_time : 0} * palette_reference_fps};
	remainder = static_cast<int32_t>(scaled & ((int64_t{1} << fraction_bits) - 1));
	return static_cast<int>(scaled >> fraction_bits);
}

}
