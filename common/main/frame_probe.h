/*
 * This file is part of the DXX-Rebirth project <https://github.com/dxx-rebirth/dxx-rebirth/>.
 * It is copyright by its individual contributors, as recorded in the
 * project's Git history.  See COPYING.txt at the top level for license
 * terms and a link to the Git history.
 */
/*
 * Frame time probe: with -verbose, measure where the time of each game
 * frame goes, show a per-second breakdown on the HUD and log unusually
 * long frames to gamelog.txt.
 *
 * Without -verbose, a scope costs one test of a global flag, and the
 * counters are plain increments.
 */

#pragma once

#include <array>
#include <cstdint>
#include <span>

namespace dcx {
namespace frame_probe {

/* Phases of a frame.  Scopes may nest; each phase is charged only for
 * the time not spent in nested scopes, so the phases of a frame add up
 * to at most the frame time.  The remainder is shown as "other".
 */
enum class phase : uint8_t
{
	input,		// event polling and control reading
	wait,		// frame limiter (calc_frame_time)
	multi,		// network processing
	objects,	// object movement, physics, collisions, AI
	game,		// rest of GameProcessFrame
	vis,		// render list and object list construction
	light,		// dynamic lighting (runs at 60 Hz)
	world,		// rest of the 3D view (faces, objects)
	tex,		// texture page-in from the pig, merging, uploads to OpenGL
	sound,		// sound object updates and mixer calls
	hud,		// cockpit, gauges, HUD, extra views
	swap,		// buffer swap / present (gr_flip)
	count
};

inline bool enabled;

/* Kinds of SDL events counted in the input phase (see event.cpp). */
enum class input_event_kind : uint8_t
{
	key,
	text,
	mouse_motion,
	mouse_button,
	mouse_wheel,
	joy_axis,
	joy_ball,
	joy_hat,
	joy_button,
	joy_device,	// SDL_JOYDEVICEADDED/REMOVED
	pad_axis,
	pad_button,
	pad_device,	// SDL_CONTROLLERDEVICEADDED/REMOVED/REMAPPED
	window,
	other,
	count
};

class scope
{
	static inline scope *top;
	scope *parent;
	uint64_t start;
	uint64_t children;
	const phase p;
	const bool active;
public:
	explicit scope(phase);
	~scope();
	scope(const scope &) = delete;
	scope &operator=(const scope &) = delete;
	static void charge(phase, uint64_t elapsed, uint64_t children);
	static bool any_open()
	{
		return top != nullptr;
	}
};

uint64_t now();

/* Counters of the current frame.  They are written unconditionally
 * (cheaper than testing the flag), and reset by frame_mark.
 */
struct frame_counters
{
	unsigned texmerge_misses;
	unsigned texture_uploads;
	unsigned texture_pageins;
	unsigned sound_conversions;
	/* Calls into the mixer (each locks the audio device), and the
	 * longest of them, in timer ticks.
	 */
	unsigned mixer_calls;
	uint64_t mixer_max_ticks;
	/* Frame limiter: time it was asked to wait (from its entry to the
	 * deadline) and time it actually waited, in fix64 game timer units,
	 * the number of SDL_Delay(1) calls and the longest of them (timer
	 * ticks), and the number of SDL_Delay(0) yields.
	 */
	int64_t limiter_requested;
	int64_t limiter_actual;
	unsigned limiter_sleeps;
	unsigned limiter_yields;
	uint64_t limiter_max_sleep_ticks;
	/* Largest render list of this frame, and the start segment of that
	 * view.
	 */
	unsigned render_segs;
	unsigned render_segnum;
	/* Vertices of the last 60 Hz lighting pass, and whether this frame
	 * ran that pass.
	 */
	unsigned render_vertices;
	bool light_frame;
	/* Input phase (event_poll), in timer ticks: time spent in
	 * SDL_PumpEvents (the OS message pump), in SDL_JoystickUpdate
	 * (joystick polling and device detection), both summed and the
	 * longest single call, and time spent dispatching the events to
	 * the windows.  input_pumps counts the pump calls.
	 */
	uint64_t input_pump_ticks;
	uint64_t input_pump_max_ticks;
	uint64_t input_joystick_ticks;
	uint64_t input_joystick_max_ticks;
	uint64_t input_dispatch_ticks;
	unsigned input_pumps;
	std::array<unsigned, static_cast<std::size_t>(input_event_kind::count)> input_events;
};
inline frame_counters counters;

inline scope::scope(const phase p) :
	p(p), active(enabled)
{
	if (active)
	{
		parent = top;
		top = this;
		children = 0;
		start = now();
	}
}

inline scope::~scope()
{
	if (active)
	{
		const auto elapsed{now() - start};
		top = parent;
		if (parent)
			parent->children += elapsed;
		charge(p, elapsed, children);
	}
}

/* One-off work that may cause a hitch: logged individually (rate
 * limited) with its duration, so that long frames can be matched with
 * it.
 */
enum class event_kind : uint8_t
{
	texture_pagein,
	texture_upload,
	texmerge,
	sound_conversion,
};

void note_event(event_kind, unsigned id, unsigned detail, uint64_t start);

/* A scope that is also logged as an event. */
class event_scope : scope
{
	const uint64_t event_start;
	const event_kind kind;
public:
	unsigned id, detail;
	event_scope(const phase p, const event_kind kind, const unsigned id, const unsigned detail) :
		scope(p), event_start(enabled ? now() : 0), kind(kind), id(id), detail(detail)
	{
	}
	~event_scope()
	{
		if (event_start)
			note_event(kind, id, detail, event_start);
	}
};

/* A call into the sound mixer. */
class mixer_scope : scope
{
	const uint64_t call_start;
public:
	mixer_scope() :
		scope(phase::sound), call_start(enabled ? now() : 0)
	{
		++counters.mixer_calls;
	}
	~mixer_scope()
	{
		if (call_start)
		{
			const auto elapsed{now() - call_start};
			if (counters.mixer_max_ticks < elapsed)
				counters.mixer_max_ticks = elapsed;
		}
	}
};

inline void note_input_pump(const uint64_t pump_ticks, const uint64_t joystick_ticks)
{
	++counters.input_pumps;
	counters.input_pump_ticks += pump_ticks;
	counters.input_joystick_ticks += joystick_ticks;
	if (counters.input_pump_max_ticks < pump_ticks)
		counters.input_pump_max_ticks = pump_ticks;
	if (counters.input_joystick_max_ticks < joystick_ticks)
		counters.input_joystick_max_ticks = joystick_ticks;
}

inline void note_input_event(const input_event_kind k)
{
	++counters.input_events[static_cast<std::size_t>(k)];
}

inline void note_render_list(const unsigned n_render_segs, const unsigned start_segnum)
{
	if (n_render_segs > counters.render_segs)
	{
		counters.render_segs = n_render_segs;
		counters.render_segnum = start_segnum;
	}
}

/* Call once per game frame, before the frame limiter.  Closes the
 * previous frame, updates the per-second statistics and logs the frame
 * if it was unusually long.  Also enables or disables the probe
 * according to -verbose.
 */
void frame_mark();

/* True once every 10 seconds while the probe is on: time to log the
 * session growth counters (see log_session_stats in game.cpp).
 */
bool session_report_due();

/* Resident set / working set and private bytes of the process, 0 if
 * unknown.
 */
struct process_memory_info
{
	uint64_t resident_bytes;
	uint64_t private_bytes;
};
process_memory_info process_memory();

/* Counters of other subsystems for the session report, implemented next
 * to the state they read.
 */
namespace stats {
unsigned ogl_textures();		// OpenGL textures alive (ogl.cpp)
unsigned piggy_cache_used();		// bytes of the bitmap page cache in use (piggy.cpp)
unsigned piggy_cache_size();
unsigned active_sound_objects();	// digiobj.cpp
unsigned mixer_channels_busy();		// digi_mixer.cpp
unsigned mixer_sounds_converted();
unsigned texmerge_entries();		// texmerge.cpp
}

/* Text for the HUD, updated once per second.  Returns the number of
 * lines filled (0 if the probe is off or has no data yet).
 */
unsigned hud_lines(std::span<char> line0, std::span<char> line1, std::span<char> line2);

}
}
