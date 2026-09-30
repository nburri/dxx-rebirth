/*
 * This file is part of the DXX-Rebirth project <https://github.com/dxx-rebirth/dxx-rebirth/>.
 * It is copyright by its individual contributors, as recorded in the
 * project's Git history.  See COPYING.txt at the top level for license
 * terms and a link to the Git history.
 */
/*
 * Test of the curve of the palette effects and of the flash missile's
 * whiteout (palette_flash.h): the same wall-clock curve at 30, 60, 144
 * and 500 frames per second, and the original's shape (hold for the
 * whole of Flash_effect, then 16 units per second), and of the fusion
 * cannon's charging glow (palette_fusion_glow_frame): the same curve at
 * 30, 60, 144 and 500 frames per second.
 *
 * Build and run with SCons:
 *
 *	scons sdl2=1 d1x=0 d2x=1 register_runtime_test_plain_link_targets=1 test-palette-flash
 *	build/common/test-palette-flash
 */

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

#include "palette_flash.h"

using namespace dcx;

namespace {

void check_failed(const char *const expr, const char *const file, const int line)
{
	std::fprintf(stderr, "%s:%d: check failed: %s\n", file, line, expr);
	std::exit(1);
}

#define CHECK(cond)	do { if (!(cond)) check_failed(#cond, __FILE__, __LINE__); } while (0)

constexpr int32_t unit{65536};

/* The palette add (red channel) sampled every quarter second over eight
 * seconds, after a full flash of `flash` seconds at `fps` frames per
 * second.
 */
using samples = std::array<int, 33>;

samples simulate(const unsigned fps, const int32_t flash, const int add)
{
	int red{add}, green{add}, blue{add};
	int32_t flash_effect{flash}, remainder{};
	const palette_flash_curve s{red, green, blue, flash_effect, remainder};
	/* The game's frame time is a whole number of fixed-point units; the
	 * leftover of each second is spread like a frame timer would.
	 */
	samples out{};
	out[0] = red;
	int64_t elapsed{};
	std::size_t next{1};
	for (unsigned frame{0}; next != out.size(); ++frame)
	{
		const int64_t t0{int64_t{frame} * unit / fps};
		const int64_t t1{int64_t{frame + 1} * unit / fps};
		const auto frame_time{static_cast<int32_t>(t1 - t0)};
		palette_flash_frame(s, frame_time);
		CHECK(red == green && green == blue);
		elapsed += frame_time;
		while (next != out.size() && elapsed >= int64_t{unit} * static_cast<int64_t>(next) / 4)
			out[next++] = red;
	}
	return out;
}

void print(const unsigned fps, const samples &v)
{
	std::printf("%4u fps:", fps);
	for (const auto x : v)
		std::printf(" %2d", x);
	std::printf("\n");
}

/* A full flash (add 60, Flash_effect 2 s) holds for 2 s, then fades at
 * 16 units per second: zero 3.75 s later.  Every frame rate gives the
 * same curve, within one unit (the frame on which a unit falls).
 */
void test_same_curve_at_any_frame_rate()
{
	const auto reference{simulate(30, 2 * unit, 60)};
	print(30, reference);
	/* Hold: every sample up to and including 2 s is the full add. */
	for (std::size_t i{0}; i <= 8; ++i)
		CHECK(reference[i] == 60);
	/* Then 16 units per second: 4 per quarter second. */
	for (std::size_t i{9}; i != reference.size(); ++i)
	{
		const int expected{60 - 4 * static_cast<int>(i - 8)};
		const int clamped{expected > 0 ? expected : 0};
		CHECK(reference[i] >= clamped - 1 && reference[i] <= clamped + 1);
	}
	CHECK(reference[22] > 0);
	CHECK(reference[23] == 0);	/* 5.75 s: 2 s hold + 3.75 s fade */
	for (const unsigned fps : {60u, 144u, 500u, 1000u, 10u})
	{
		const auto v{simulate(fps, 2 * unit, 60)};
		print(fps, v);
		for (std::size_t i{0}; i != v.size(); ++i)
		{
			const int d{v[i] - reference[i]};
			CHECK(d >= -1 && d <= 1);
		}
	}
}

/* Without a flash, the palette effects diminish at 16 units per second at
 * any frame rate, including below 16 fps, where the old code truncated
 * FrameTime * 16 to whole units per frame (10 fps: 10 units per second).
 */
void test_diminish_rate()
{
	for (const unsigned fps : {5u, 10u, 12u, 30u, 60u, 144u, 500u})
	{
		int red{48}, green{-48}, blue{0};
		int32_t flash_effect{}, remainder{};
		const palette_flash_curve s{red, green, blue, flash_effect, remainder};
		int64_t elapsed{};
		for (unsigned frame{0}; elapsed < unit; ++frame)
		{
			const auto frame_time{static_cast<int32_t>(int64_t{frame + 1} * unit / fps - int64_t{frame} * unit / fps)};
			CHECK(!palette_flash_frame(s, frame_time));
			elapsed += frame_time;
		}
		CHECK(red == 32);
		CHECK(green == -32);
		CHECK(blue == 0);
	}
}

/* The flash holds on the frame on which it expires, and Flash_effect
 * never goes negative.
 */
void test_flash_expiry()
{
	int red{60}, green{60}, blue{60};
	int32_t flash_effect{100}, remainder{12345};
	const palette_flash_curve s{red, green, blue, flash_effect, remainder};
	CHECK(palette_flash_frame(s, 131));
	CHECK(flash_effect == 0);
	CHECK(red == 60);
	CHECK(remainder == 0);
	CHECK(!palette_flash_frame(s, 131));
	CHECK(red == 60);
}


/* The fusion cannon's charging glow: charge for three seconds, then
 * release, at `fps` frames per second, with the game's order within a
 * frame (diminish_palette_towards_normal, which also shows the palette,
 * then FireLaser).  The shown palette (red, green, blue) is sampled
 * every 1/20 s.  With `per_frame`, the glow is added as before: the
 * whole Fusion_charge >> 11 on every frame.
 */
struct glow_sample
{
	int red, green, blue;
};
using glow_samples = std::array<glow_sample, 101>;

glow_samples simulate_fusion(const unsigned fps, const bool per_frame)
{
	int red{}, green{}, blue{};
	int32_t flash_effect{}, remainder{};
	const palette_flash_curve s{red, green, blue, flash_effect, remainder};
	int32_t charge{}, glow_remainder{};
	glow_samples out{};
	int64_t elapsed{};
	std::size_t next{0};
	for (unsigned frame{0}; next != out.size(); ++frame)
	{
		const auto frame_time{static_cast<int32_t>(int64_t{frame + 1} * unit / fps - int64_t{frame} * unit / fps)};
		palette_flash_frame(s, frame_time);
		/* The palette shown on this frame. */
		while (next != out.size() && elapsed >= int64_t{unit} * static_cast<int64_t>(next) / 20)
			out[next++] = {red, green, blue};
		elapsed += frame_time;
		if (elapsed > 3 * unit)
			continue;	/* released */
		if (!charge)
			glow_remainder = 0;
		charge += frame_time;
		const int dr{per_frame ? charge >> 11 : palette_fusion_glow_frame(charge, frame_time, glow_remainder)};
		const bool blue_phase{charge < 2 * unit};
		palette_channel_add(red, dr, 30);
		palette_channel_add(green, blue_phase ? 0 : dr, 30);
		palette_channel_add(blue, blue_phase ? dr : 0, 30);
	}
	return out;
}

void print_fusion(const unsigned fps, const glow_samples &v)
{
	std::printf("%4u fps red: ", fps);
	for (std::size_t i{0}; i < 8; ++i)
		std::printf(" %2d", v[i].red);
	std::printf(" | blue 2.0-3.0 s:");
	for (std::size_t i{40}; i <= 60; i += 4)
		std::printf(" %2d", v[i].blue);
	std::printf(" | green 2.0-3.0 s:");
	for (std::size_t i{40}; i <= 60; i += 4)
		std::printf(" %2d", v[i].green);
	std::printf("\n");
}

/* The first sample at which the red channel is at full strength (30,
 * less the one unit that the diminish takes before it is shown).
 */
std::size_t fusion_full_red(const glow_samples &v)
{
	return static_cast<std::size_t>(std::find_if(v.begin(), v.end(), [](const glow_sample &g) { return g.red >= 28; }) - v.begin());
}

void test_fusion_glow_same_curve_at_any_frame_rate()
{
	/* At 30 fps, the new glow is the original's (within the 16/65536
	 * that the frame timer's 2184-unit frames fall short of 1/30 s).
	 */
	const auto reference{simulate_fusion(30, false)};
	const auto original{simulate_fusion(30, true)};
	print_fusion(30, reference);
	for (std::size_t i{0}; i != reference.size(); ++i)
	{
		CHECK(std::abs(reference[i].red - original[i].red) <= 1);
		CHECK(std::abs(reference[i].green - original[i].green) <= 1);
		CHECK(std::abs(reference[i].blue - original[i].blue) <= 1);
	}
	/* The original's look at 30 fps: red and blue rise to full strength
	 * over about a quarter second, blue fades at 16 units per second
	 * after two seconds while green rises, and all fade at 16 units per
	 * second after the release.
	 */
	const auto reference_full{fusion_full_red(reference)};
	CHECK(reference_full >= 4 && reference_full <= 6);
	CHECK(reference[39].blue >= 28 && reference[39].green == 0);
	CHECK(reference[41].green == 30);
	CHECK(reference[60].blue >= 13 && reference[60].blue <= 15);
	CHECK(reference[80].red >= 13 && reference[80].red <= 15);
	CHECK(reference[100].red == 0);
	/* Below 30 fps, the frames are coarser than the curve's own steps,
	 * so only the higher frame rates are compared.
	 */
	for (const unsigned fps : {60u, 144u, 500u, 1000u})
	{
		const auto v{simulate_fusion(fps, false)};
		print_fusion(fps, v);
		/* The rise is steep (3 units per 1/30 s at 0.1 s of charge,
		 * 8 at 0.25 s), and the 30 fps curve rises in steps: allow 5
		 * units while it rises, and one unit elsewhere.
		 */
		for (std::size_t i{0}; i != v.size(); ++i)
		{
			const int tolerance{i <= 8 ? 5 : 1};
			CHECK(std::abs(v[i].red - reference[i].red) <= tolerance);
			CHECK(std::abs(v[i].green - reference[i].green) <= (i >= 40 && i <= 42 ? 30 : tolerance));
			CHECK(std::abs(v[i].blue - reference[i].blue) <= tolerance);
		}
		const auto full{fusion_full_red(v)};
		CHECK(full + 1 >= reference_full && full <= reference_full + 1);	/* within 0.05 s */
	}
	/* Before, the glow grew faster the higher the frame rate. */
	const auto before_500{simulate_fusion(500, true)};
	std::printf("before:\n");
	print_fusion(500, before_500);
	CHECK(fusion_full_red(before_500) + 2 <= reference_full);
}

}

int main()
{
	test_same_curve_at_any_frame_rate();
	test_diminish_rate();
	test_flash_expiry();
	test_fusion_glow_same_curve_at_any_frame_rate();
	std::printf("palette_flash: all tests passed\n");
	return 0;
}
