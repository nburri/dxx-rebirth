/*
 * This file is part of the DXX-Rebirth project <https://github.com/dxx-rebirth/dxx-rebirth/>.
 * It is copyright by its individual contributors, as recorded in the
 * project's Git history.  See COPYING.txt at the top level for license
 * terms and a link to the Git history.
 */
/*
 * Frame time probe.  See frame_probe.h.
 */

#include <algorithm>
#include <array>
#include <cstdio>
#include <string>
#include <SDL.h>
#if SDL_VERSION_ATLEAST(2, 0, 18)
#include <SDL_hidapi.h>
#define DXX_FRAME_PROBE_HID_CHANGES	1
#else
#define DXX_FRAME_PROBE_HID_CHANGES	0
#endif
#if SDL_MAJOR_VERSION == 1
#include <chrono>
#endif
#ifdef _WIN32
#include <windows.h>
#include <psapi.h>
#elif defined(__linux__)
#include <sched.h>
#include <unistd.h>
#endif

#include "frame_probe.h"
#include "args.h"
#include "console.h"
#include <cinttypes>

namespace dcx {
namespace frame_probe {

namespace {

constexpr std::size_t n_phases{static_cast<std::size_t>(phase::count)};

/* A frame is logged if it is longer than both of these. */
constexpr double long_frame_min_ms{4.0};
constexpr double long_frame_average_factor{2.0};
/* At most this many long frames and events are logged per second, so
 * that a slow phase of the game cannot fill the log (and slow down the
 * game with file writes).  The per-second summary still counts all of
 * them.
 */
constexpr unsigned max_logged_long_frames_per_second{30};
constexpr unsigned max_logged_events_per_second{40};
/* A gap longer than this (menus, level loading, pause) is not a frame. */
constexpr double gap_ms{1000.0};

/* Upper bounds (ms) of the frame time histogram buckets; the last
 * bucket has no upper bound.  The buckets around 2 ms show whether
 * frames stay within the 500 fps budget.
 */
constexpr std::array<double, 8> histogram_bounds{{1.9, 2.1, 2.5, 3., 4., 6., 10., 20.}};
using histogram_t = std::array<unsigned, histogram_bounds.size() + 1>;

uint64_t tick_frequency()
{
#if SDL_MAJOR_VERSION == 1
	return 1000000000u;
#else
	static const uint64_t f{SDL_GetPerformanceFrequency()};
	return f;
#endif
}

int current_cpu()
{
#ifdef _WIN32
	return static_cast<int>(GetCurrentProcessorNumber());
#elif defined(__linux__)
	return sched_getcpu();
#else
	return -1;
#endif
}

std::size_t index(const phase p)
{
	return static_cast<std::size_t>(p);
}

double ticks_to_ms(const uint64_t ticks)
{
	return static_cast<double>(ticks) * 1000. / static_cast<double>(tick_frequency());
}

double fix_to_ms(const int64_t f)
{
	return static_cast<double>(f) * 1000. / 65536.;
}

struct window_totals
{
	std::array<uint64_t, n_phases> phase_ticks{};
	histogram_t histogram{};
	uint64_t frame_ticks{};
	uint64_t max_frame_ticks{};
	uint64_t mixer_max_ticks{};
	uint64_t limiter_max_sleep_ticks{};
	int64_t limiter_overshoot{};
	int64_t limiter_max_overshoot{};
	unsigned frames{};
	unsigned long_frames{};
	unsigned logged_long_frames{};
	unsigned texmerge_misses{};
	unsigned texture_uploads{};
	unsigned texture_pageins{};
	unsigned sound_conversions{};
	unsigned mixer_calls{};
	unsigned limiter_sleeps{};
	unsigned limiter_yields{};
	unsigned cpu_migrations{};
	unsigned max_render_segs{};
	unsigned max_render_vertices{};
	unsigned logged_events{};
	unsigned suppressed_events{};
	uint64_t input_pump_max_ticks{};
	uint64_t input_joystick_max_ticks{};
	unsigned hid_changes{};
};

struct probe_state
{
	/* Phase time of the frame in progress. */
	std::array<uint64_t, n_phases> phase_ticks{};
	/* Running averages of the phases, in ms. */
	std::array<double, n_phases> phase_average_ms{};
	uint64_t first_mark{};
	uint64_t last_mark{};
	uint64_t window_start{};
	window_totals window;
	/* Running average of the frame time, in ms. */
	double average_ms{};
	int last_cpu{-1};
	uint64_t last_session_report{};
	bool session_report_pending{};
	/* Snapshot shown on the HUD. */
	std::array<std::array<char, 64>, 3> hud{};
	bool hud_valid{};
	bool announced{};
	/* Device interface arrivals and removals that Windows reported to
	 * SDL's HIDAPI layer (SDL_hid_device_change_count; on Windows it
	 * counts WM_DEVICECHANGE for all interface classes).  Each one
	 * makes SDL enumerate devices again on the main thread.
	 */
	uint32_t hid_change_count{};
	bool hid_change_count_valid{};
	unsigned frame_hid_changes{};
};

probe_state state;

constexpr std::array<const char *, n_phases> phase_names{{
	"input", "wait", "net", "obj", "game", "vis", "light", "world", "tex", "sound", "hud", "swap",
}};

constexpr std::array<const char *, static_cast<std::size_t>(input_event_kind::count)> input_event_names{{
	"key", "text", "mousemotion", "mousebutton", "mousewheel", "joyaxis", "joyball", "joyhat", "joybutton", "joydevice", "padaxis", "padbutton", "paddevice", "window", "other",
}};

/* Long frames are logged at CON_NORMAL with -frametimes, so that they
 * reach gamelog.txt without the rest of the -verbose output.
 */
con_priority long_frame_priority()
{
	return CGameArg.DbgFrameTimeHud ? CON_NORMAL : CON_VERBOSE;
}

/* Changes since the previous frame, or 0 if unknown. */
unsigned update_hid_changes()
{
#if DXX_FRAME_PROBE_HID_CHANGES
	/* SDL_hid_device_change_count would initialize hidapi if nothing
	 * else did; the joystick subsystem does, so only ask when it is up.
	 */
	if (!SDL_WasInit(SDL_INIT_JOYSTICK))
	{
		state.hid_change_count_valid = false;
		return 0;
	}
	const uint32_t c{SDL_hid_device_change_count()};
	const bool valid{state.hid_change_count_valid};
	const uint32_t previous{state.hid_change_count};
	state.hid_change_count = c;
	state.hid_change_count_valid = true;
	return valid ? c - previous : 0;
#else
	return 0;
#endif
}

double seconds_since_start(const uint64_t t)
{
	return t > state.first_mark ? ticks_to_ms(t - state.first_mark) / 1000. : 0.;
}

void reset_frame()
{
	state.phase_ticks = {};
	const auto render_vertices{counters.render_vertices};
	counters = {};
	/* Kept until the next lighting pass. */
	counters.render_vertices = render_vertices;
}

void restart(const uint64_t t, const int cpu)
{
	reset_frame();
	state.last_mark = t;
	state.window_start = t;
	state.window = {};
	state.last_cpu = cpu;
}

void log_long_frame(const uint64_t t, const double total_ms, const std::array<double, n_phases> &ms, const double other_ms, const int cpu)
{
	/* The two phases that grew most compared to their running average
	 * show where the extra time of this frame went.
	 */
	std::array<std::size_t, n_phases> order;
	for (std::size_t i{}; i != n_phases; ++i)
		order[i] = i;
	std::partial_sort(order.begin(), order.begin() + 2, order.end(), [&ms](const std::size_t a, const std::size_t b) {
		return ms[a] - state.phase_average_ms[a] > ms[b] - state.phase_average_ms[b];
	});
	const auto grew = [&ms](const std::size_t i) { return ms[i] - state.phase_average_ms[i]; };
	/* Events handled in this frame, by kind, e.g. "joyaxis 4 mousemotion 2". */
	std::array<char, 256> events;
	{
		std::size_t used{};
		events[0] = 0;
		for (std::size_t i{}; i != counters.input_events.size(); ++i)
		{
			const auto n{counters.input_events[i]};
			if (!n)
				continue;
			const auto r{std::snprintf(events.data() + used, events.size() - used, "%s%s %u", used ? " " : "", input_event_names[i], n)};
			if (r < 0 || static_cast<std::size_t>(r) >= events.size() - used)
				break;
			used += static_cast<std::size_t>(r);
		}
		if (!used)
			std::snprintf(events.data(), events.size(), "none");
	}
	con_printf(long_frame_priority(), "frame: t=%.3f long %.2f ms (avg %.2f), grew: %s %+.2f, %s %+.2f | input %.2f wait %.2f net %.2f obj %.2f game %.2f vis %.2f light %.2f world %.2f tex %.2f sound %.2f hud %.2f swap %.2f other %.2f | limiter asked %.2f waited %.2f sleeps %u (max %.2f) yields %u | seg %u vis %u verts %u%s | texmerge-miss %u uploads %u pageins %u | mixer calls %u max %.3f ms, sound conversions %u | input: pumps %u, os-pump %.2f (max %.2f), joystick-update %.2f (max %.2f), dispatch %.2f, hid-changes %u, events %s | cpu %i%s%i",
		seconds_since_start(t), total_ms, state.average_ms,
		phase_names[order[0]], grew(order[0]), phase_names[order[1]], grew(order[1]),
		ms[index(phase::input)], ms[index(phase::wait)], ms[index(phase::multi)], ms[index(phase::objects)], ms[index(phase::game)], ms[index(phase::vis)], ms[index(phase::light)], ms[index(phase::world)], ms[index(phase::tex)], ms[index(phase::sound)], ms[index(phase::hud)], ms[index(phase::swap)],
		other_ms,
		fix_to_ms(counters.limiter_requested), fix_to_ms(counters.limiter_actual), counters.limiter_sleeps, ticks_to_ms(counters.limiter_max_sleep_ticks), counters.limiter_yields,
		counters.render_segnum, counters.render_segs, counters.render_vertices,
		counters.light_frame ? " (light pass)" : "",
		counters.texmerge_misses, counters.texture_uploads, counters.texture_pageins,
		counters.mixer_calls, ticks_to_ms(counters.mixer_max_ticks), counters.sound_conversions,
		counters.input_pumps, ticks_to_ms(counters.input_pump_ticks), ticks_to_ms(counters.input_pump_max_ticks), ticks_to_ms(counters.input_joystick_ticks), ticks_to_ms(counters.input_joystick_max_ticks), ticks_to_ms(counters.input_dispatch_ticks), state.frame_hid_changes, events.data(),
		state.last_cpu, cpu == state.last_cpu ? "=" : "->", cpu);
}

void close_window(const uint64_t t)
{
	auto &w = state.window;
	const auto frames{w.frames};
	if (!frames)
		return;
	const double window_ms{ticks_to_ms(t - state.window_start)};
	std::array<double, n_phases> avg;
	double sum{};
	for (std::size_t i{}; i != n_phases; ++i)
		sum += (avg[i] = ticks_to_ms(w.phase_ticks[i]) / frames);
	const double avg_frame{ticks_to_ms(w.frame_ticks) / frames};
	const double other{std::max(0., avg_frame - sum)};
	const auto a = [&avg](const phase p) { return avg[index(p)]; };
	const auto &hg = w.histogram;
	unsigned over_2_5{0}, over_4{0};
	for (std::size_t i{}; i != hg.size(); ++i)
	{
		if (i >= 3)
			over_2_5 += hg[i];
		if (i >= 5)
			over_4 += hg[i];
	}
	auto &hud = state.hud;
	std::snprintf(hud[0].data(), hud[0].size(), "I%.2f W%.2f N%.2f O%.2f G%.2f V%.2f L%.2f", a(phase::input), a(phase::wait), a(phase::multi), a(phase::objects), a(phase::game), a(phase::vis), a(phase::light));
	std::snprintf(hud[1].data(), hud[1].size(), "R%.2f T%.2f A%.2f H%.2f S%.2f X%.2f", a(phase::world), a(phase::tex), a(phase::sound), a(phase::hud), a(phase::swap), other);
	std::snprintf(hud[2].data(), hud[2].size(), "max%.1f >2.5:%u >4:%u seg%u tm%u mx%.2f", ticks_to_ms(w.max_frame_ticks), over_2_5, over_4, w.max_render_segs, w.texmerge_misses, ticks_to_ms(w.mixer_max_ticks));
	state.hud_valid = true;
	con_printf(CON_VERBOSE, "frames: t=%.3f %u in %.0f ms, avg %.3f max %.2f ms, long %u | hist <1.9:%u <2.1:%u <2.5:%u <3:%u <4:%u <6:%u <10:%u <20:%u >=20:%u | avg ms: input %.3f wait %.3f net %.3f obj %.3f game %.3f vis %.3f light %.3f world %.3f tex %.3f sound %.3f hud %.3f swap %.3f other %.3f | limiter overshoot avg %.3f max %.3f ms, sleeps %u (max %.2f ms) yields %u | max vis segs %u verts %u | texmerge-miss %u uploads %u pageins %u | mixer calls %u max %.3f ms, sound conversions %u | events logged %u suppressed %u | input os-pump max %.2f joystick-update max %.2f ms, hid-changes %u | cpu migrations %u, cpu %i",
		seconds_since_start(t), frames, window_ms, avg_frame, ticks_to_ms(w.max_frame_ticks), w.long_frames,
		hg[0], hg[1], hg[2], hg[3], hg[4], hg[5], hg[6], hg[7], hg[8],
		a(phase::input), a(phase::wait), a(phase::multi), a(phase::objects), a(phase::game), a(phase::vis), a(phase::light), a(phase::world), a(phase::tex), a(phase::sound), a(phase::hud), a(phase::swap), other,
		fix_to_ms(w.limiter_overshoot) / frames, fix_to_ms(w.limiter_max_overshoot), w.limiter_sleeps, ticks_to_ms(w.limiter_max_sleep_ticks), w.limiter_yields,
		w.max_render_segs, w.max_render_vertices,
		w.texmerge_misses, w.texture_uploads, w.texture_pageins,
		w.mixer_calls, ticks_to_ms(w.mixer_max_ticks), w.sound_conversions,
		w.logged_events, w.suppressed_events,
		ticks_to_ms(w.input_pump_max_ticks), ticks_to_ms(w.input_joystick_max_ticks), w.hid_changes,
		w.cpu_migrations, state.last_cpu);
}

}

void note_event(const event_kind kind, const unsigned id, const unsigned detail, const uint64_t start)
{
	auto &w = state.window;
	if (w.logged_events >= max_logged_events_per_second)
	{
		++w.suppressed_events;
		return;
	}
	++w.logged_events;
	const auto t{now()};
	const char *what;
	const char *detail_name;
	switch (kind)
	{
		case event_kind::texture_pagein:
			what = "texture page-in from pig, bitmap";
			detail_name = "width";
			break;
		case event_kind::texture_upload:
			what = "texture upload to OpenGL, width";
			detail_name = "mipmaps";
			break;
		case event_kind::texmerge:
			what = "texmerge miss, base bitmap";
			detail_name = "overlay bitmap";
			break;
		case event_kind::sound_conversion:
			what = "first use of sound (conversion for the mixer)";
			detail_name = "bytes";
			break;
		default:
			return;
	}
	con_printf(CON_VERBOSE, "frame-event: t=%.3f %s %u (%s %u) took %.3f ms", seconds_since_start(t), what, id, detail_name, detail, ticks_to_ms(t - start));
}

uint64_t now()
{
#if SDL_MAJOR_VERSION == 1
	return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
#else
	return SDL_GetPerformanceCounter();
#endif
}

void scope::charge(const phase p, const uint64_t elapsed, const uint64_t children)
{
	state.phase_ticks[index(p)] += elapsed > children ? elapsed - children : 0;
}

void frame_mark()
{
	const bool want{CGameArg.DbgVerbose >= CON_VERBOSE || CGameArg.DbgFrameTimeHud};
	if (!want)
	{
		if (enabled)
		{
			enabled = false;
			state.hud_valid = false;
		}
		return;
	}
	const auto t{now()};
	const int cpu{current_cpu()};
	if (!enabled || scope::any_open())
	{
		/* Just enabled, or called from inside a measured scope (which
		 * would make the accounting inconsistent): start over.
		 */
		enabled = true;
		if (!state.announced)
		{
			state.announced = true;
			con_printf(long_frame_priority(), "frame: probe enabled; logical CPUs %i, timer frequency %" PRIu64 " Hz; logging frames longer than %.1f ms and %.1fx the running average",
#if SDL_MAJOR_VERSION == 1
				-1,
#else
				SDL_GetCPUCount(),
#endif
				tick_frequency(), long_frame_min_ms, long_frame_average_factor);
		}
		if (!state.first_mark)
			state.first_mark = t;
		update_hid_changes();
		restart(t, cpu);
		return;
	}
	const auto frame_ticks{t - state.last_mark};
	const double total_ms{ticks_to_ms(frame_ticks)};
	state.frame_hid_changes = update_hid_changes();
	if (total_ms > gap_ms)
	{
		restart(t, cpu);
		return;
	}
	std::array<double, n_phases> ms;
	double sum{};
	for (std::size_t i{}; i != n_phases; ++i)
		sum += (ms[i] = ticks_to_ms(state.phase_ticks[i]));
	const double other_ms{std::max(0., total_ms - sum)};
	auto &w = state.window;
	for (std::size_t i{}; i != n_phases; ++i)
		w.phase_ticks[i] += state.phase_ticks[i];
	w.frame_ticks += frame_ticks;
	w.max_frame_ticks = std::max(w.max_frame_ticks, frame_ticks);
	++w.histogram[std::distance(histogram_bounds.begin(), std::ranges::upper_bound(histogram_bounds, total_ms))];
	++w.frames;
	w.texmerge_misses += counters.texmerge_misses;
	w.texture_uploads += counters.texture_uploads;
	w.texture_pageins += counters.texture_pageins;
	w.sound_conversions += counters.sound_conversions;
	w.mixer_calls += counters.mixer_calls;
	w.mixer_max_ticks = std::max(w.mixer_max_ticks, counters.mixer_max_ticks);
	{
		const auto overshoot{counters.limiter_actual - counters.limiter_requested};
		if (overshoot > 0)
		{
			w.limiter_overshoot += overshoot;
			w.limiter_max_overshoot = std::max(w.limiter_max_overshoot, overshoot);
		}
	}
	w.limiter_sleeps += counters.limiter_sleeps;
	w.limiter_yields += counters.limiter_yields;
	w.limiter_max_sleep_ticks = std::max(w.limiter_max_sleep_ticks, counters.limiter_max_sleep_ticks);
	w.max_render_segs = std::max(w.max_render_segs, counters.render_segs);
	w.max_render_vertices = std::max(w.max_render_vertices, counters.render_vertices);
	w.input_pump_max_ticks = std::max(w.input_pump_max_ticks, counters.input_pump_max_ticks);
	w.input_joystick_max_ticks = std::max(w.input_joystick_max_ticks, counters.input_joystick_max_ticks);
	w.hid_changes += state.frame_hid_changes;
	if (cpu != state.last_cpu)
		++w.cpu_migrations;
	if (state.average_ms <= 0)
	{
		state.average_ms = total_ms;
		state.phase_average_ms = ms;
	}
	if (total_ms > long_frame_min_ms && total_ms > long_frame_average_factor * state.average_ms)
	{
		++w.long_frames;
		if (w.logged_long_frames < max_logged_long_frames_per_second)
		{
			++w.logged_long_frames;
			log_long_frame(t, total_ms, ms, other_ms, cpu);
		}
	}
	/* Exponential averages over about 64 frames.  Long frames count
	 * only a little, so that a burst of them is still detected.
	 */
	state.average_ms += (std::min(total_ms, 2 * state.average_ms) - state.average_ms) / 64;
	for (std::size_t i{}; i != n_phases; ++i)
		state.phase_average_ms[i] += (ms[i] - state.phase_average_ms[i]) / 64;
	state.last_mark = t;
	state.last_cpu = cpu;
	reset_frame();
	if (!state.last_session_report)
		state.last_session_report = t;
	else if (ticks_to_ms(t - state.last_session_report) >= 10000.)
	{
		state.last_session_report = t;
		state.session_report_pending = true;
	}
	if (ticks_to_ms(t - state.window_start) >= 1000.)
	{
		close_window(t);
		state.window_start = t;
		state.window = {};
	}
}

bool session_report_due()
{
	if (!enabled || !state.session_report_pending)
		return false;
	state.session_report_pending = false;
	return true;
}

process_memory_info process_memory()
{
#ifdef _WIN32
	/* K32GetProcessMemoryInfo is in kernel32 since Windows 7; older
	 * systems have GetProcessMemoryInfo only in psapi.dll, which is not
	 * linked.  Look it up at run time.
	 */
	using get_process_memory_info_t = BOOL (WINAPI *)(HANDLE, PPROCESS_MEMORY_COUNTERS, DWORD);
	static const auto get_process_memory_info = []() -> get_process_memory_info_t {
		const auto kernel32{GetModuleHandleA("kernel32.dll")};
		if (!kernel32)
			return nullptr;
		union {
			FARPROC proc;
			get_process_memory_info_t result;
		};
		proc = GetProcAddress(kernel32, "K32GetProcessMemoryInfo");
		return result;
	}();
	if (get_process_memory_info)
	{
		PROCESS_MEMORY_COUNTERS_EX pmc{};
		pmc.cb = sizeof(pmc);
		if (get_process_memory_info(GetCurrentProcess(), reinterpret_cast<PPROCESS_MEMORY_COUNTERS>(&pmc), sizeof(pmc)))
			return {pmc.WorkingSetSize, pmc.PrivateUsage};
	}
	return {};
#elif defined(__linux__)
	/* statm: size resident shared text lib data dt, in pages. */
	if (const auto f{std::fopen("/proc/self/statm", "r")})
	{
		unsigned long size{}, resident{}, shared{}, text{}, lib{}, data{};
		const auto n{std::fscanf(f, "%lu %lu %lu %lu %lu %lu", &size, &resident, &shared, &text, &lib, &data)};
		std::fclose(f);
		if (n == 6)
		{
			const uint64_t page{static_cast<uint64_t>(sysconf(_SC_PAGESIZE))};
			return {resident * page, data * page};
		}
	}
	return {};
#else
	return {};
#endif
}

unsigned hud_lines(const std::span<char> line0, const std::span<char> line1, const std::span<char> line2)
{
	if (!enabled || !state.hud_valid)
		return 0;
	const auto copy = [](const std::span<char> dst, const std::array<char, 64> &src) {
		if (dst.empty())
			return;
		const auto n{std::min(dst.size() - 1, std::char_traits<char>::length(src.data()))};
		std::copy_n(src.data(), n, dst.data());
		dst[n] = 0;
	};
	copy(line0, state.hud[0]);
	copy(line1, state.hud[1]);
	copy(line2, state.hud[2]);
	return 3;
}

}
}
