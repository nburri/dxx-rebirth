/*
 * This file is part of the DXX-Rebirth project <https://github.com/dxx-rebirth/dxx-rebirth/>.
 * It is copyright by its individual contributors, as recorded in the
 * project's Git history.  See COPYING.txt at the top level for license
 * terms and a link to the Git history.
 */
/*
 *
 * SDL Event related stuff
 *
 *
 */

#include <ranges>
#include <SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include "event.h"
#include "key.h"
#include "mouse.h"
#include "window.h"
#include "timer.h"
#include "cmd.h"
#include "config.h"
#include "inferno.h"

#include "joy.h"
#include "args.h"
#include "partial_range.h"
#include "frame_probe.h"

namespace dcx {

namespace {

struct event_poll_state
{
	uint8_t clean_uniframe{1};
	const window *const front_window = window_get_front();
	window_event_result highest_result = window_event_result::ignored;
	void process_event_batch(std::ranges::subrange<const SDL_Event *>);
};

}

#if SDL_MAJOR_VERSION == 2
extern SDL_Window *g_pRebirthSDLMainWindow;

static void windowevent_handler(const SDL_WindowEvent &windowevent)
{
	switch (windowevent.event)
	{
		case SDL_WINDOWEVENT_SIZE_CHANGED:
			{
				const d_window_size_event e{windowevent.data1, windowevent.data2};
				event_send(e);
				break;
			}
	}
}
#endif

namespace {

static void event_notify_begin_loop()
{
	const d_event_begin_loop event;
	event_send(event);
}

static void event_notify_end_loop()
{
	event_send(d_event_end_loop{});
}

/* Run the OS message pump, then poll the joysticks.
 *
 * On SDL2, arch_init sets SDL_HINT_AUTO_UPDATE_JOYSTICKS to 0, so that
 * SDL_PumpEvents does not call SDL_JoystickUpdate itself.  Calling it
 * here, right after the pump, keeps SDL's own order (OS messages first,
 * then joystick polling and device detection), and lets the frame probe
 * tell the two apart: on Windows, a device change makes
 * SDL_JoystickUpdate enumerate devices again on this thread, which can
 * take tens of milliseconds, while the OS message pump can stall on its
 * own (hooks, message floods).
 */
static void event_joystick_update()
{
#if SDL_MAJOR_VERSION == 2 && DXX_MAX_JOYSTICKS
	/* Returns at once if the joystick subsystem is not initialized
	 * (-nojoystick).
	 */
	SDL_JoystickUpdate();
#endif
}

static void event_pump()
{
	if (!frame_probe::enabled)
	{
		SDL_PumpEvents();
		event_joystick_update();
		return;
	}
	const auto t0{frame_probe::now()};
	SDL_PumpEvents();
	const auto t1{frame_probe::now()};
	event_joystick_update();
	const auto t2{frame_probe::now()};
	frame_probe::note_input_pump(t1 - t0, t2 - t1);
}

static frame_probe::input_event_kind classify_event(const uint32_t type)
{
	using k = frame_probe::input_event_kind;
	switch (type)
	{
		case SDL_KEYDOWN:
		case SDL_KEYUP:
			return k::key;
#if SDL_MAJOR_VERSION == 2
		case SDL_TEXTINPUT:
		case SDL_TEXTEDITING:
			return k::text;
		case SDL_MOUSEWHEEL:
			return k::mouse_wheel;
		case SDL_WINDOWEVENT:
			return k::window;
		case SDL_JOYDEVICEADDED:
		case SDL_JOYDEVICEREMOVED:
			return k::joy_device;
		case SDL_CONTROLLERAXISMOTION:
			return k::pad_axis;
		case SDL_CONTROLLERBUTTONDOWN:
		case SDL_CONTROLLERBUTTONUP:
			return k::pad_button;
		case SDL_CONTROLLERDEVICEADDED:
		case SDL_CONTROLLERDEVICEREMOVED:
		case SDL_CONTROLLERDEVICEREMAPPED:
			return k::pad_device;
#endif
		case SDL_MOUSEMOTION:
			return k::mouse_motion;
		case SDL_MOUSEBUTTONDOWN:
		case SDL_MOUSEBUTTONUP:
			return k::mouse_button;
		case SDL_JOYAXISMOTION:
			return k::joy_axis;
		case SDL_JOYBALLMOTION:
			return k::joy_ball;
		case SDL_JOYHATMOTION:
			return k::joy_hat;
		case SDL_JOYBUTTONDOWN:
		case SDL_JOYBUTTONUP:
			return k::joy_button;
		default:
			return k::other;
	}
}

}

window_event_result event_poll()
{
	event_poll_state state;
	event_notify_begin_loop();

	for (;;)
	{
	// If the front window changes, exit this loop, otherwise unintended behavior can occur
	// like pressing 'Return' really fast at 'Difficulty Level' causing multiple games to be started
		if (state.front_window != window_get_front())
			break;
		std::array<SDL_Event, 128> events;

		event_pump();
#if SDL_MAJOR_VERSION == 1
		const auto peep = SDL_PeepEvents(events.data(), events.size(), SDL_GETEVENT, SDL_ALLEVENTS);
#elif SDL_MAJOR_VERSION == 2
		const auto peep = SDL_PeepEvents(events.data(), events.size(), SDL_GETEVENT, SDL_FIRSTEVENT, SDL_LASTEVENT);
#endif
		if (peep <= 0)
			break;
		const auto batch{unchecked_partial_range(events, static_cast<unsigned>(peep))};
		if (frame_probe::enabled)
		{
			for (auto &&event : batch)
				frame_probe::note_input_event(classify_event(event.type));
			const auto t0{frame_probe::now()};
			state.process_event_batch(batch);
			frame_probe::counters.input_dispatch_ticks += frame_probe::now() - t0;
		}
		else
			state.process_event_batch(batch);
		if (state.highest_result == window_event_result::deleted)
			break;
	}
	// Send the idle event if there were no other events (or they were ignored)
	if (state.highest_result == window_event_result::ignored)
	{
		const d_event ievent{event_type::idle};
		state.highest_result = std::max(event_send(ievent), state.highest_result);
	}
	else
	{
#if DXX_USE_EDITOR
		event_reset_idle_seconds();
#endif
	}
	mouse_cursor_autohide();
	event_notify_end_loop();
	return state.highest_result;
}

void event_poll_state::process_event_batch(const std::ranges::subrange<const SDL_Event *> events)
{
	for (auto &&event : events)
	{
		window_event_result result;
		switch(event.type) {
#if SDL_MAJOR_VERSION == 2
			case SDL_WINDOWEVENT:
				windowevent_handler(event.window);
				continue;
#endif
			case SDL_KEYDOWN:
			case SDL_KEYUP:
				if (clean_uniframe)
				{
					clean_uniframe=0;
					unicode_frame_buffer = {};
				}
				result = key_handler(&event.key);
				break;
			case SDL_MOUSEBUTTONDOWN:
			case SDL_MOUSEBUTTONUP:
				if (CGameArg.CtlNoMouse)
					continue;
				result = mouse_button_handler(&event.button);
				break;
			case SDL_MOUSEMOTION:
				if (CGameArg.CtlNoMouse)
					continue;
				result = mouse_motion_handler(&event.motion);
				break;
#if DXX_MAX_JOYSTICKS
#if SDL_MAJOR_VERSION == 2
#if DXX_MAX_BUTTONS_PER_JOYSTICK
			case SDL_CONTROLLERBUTTONDOWN:
			case SDL_CONTROLLERBUTTONUP:
				if (CGameArg.CtlNoJoystick)
					continue;
				result = gc_button_handler(&event.cbutton);
				break;
#endif
#if DXX_MAX_AXES_PER_JOYSTICK
			case SDL_CONTROLLERAXISMOTION:
				if (CGameArg.CtlNoJoystick)
					continue;
#if (DXX_MAX_BUTTONS_PER_JOYSTICK || DXX_MAX_HATS_PER_JOYSTICK)
				highest_result = std::max(gc_axisbutton_handler(&event.caxis), highest_result);
#endif
				result = gc_axis_handler(&event.caxis);
				break;
#endif
			case SDL_CONTROLLERDEVICEADDED:
				result = gc_device_added(&event.cdevice);
				break;
			case SDL_CONTROLLERDEVICEREMOVED:
				result = gc_device_removed(&event.cdevice);
				break;
#endif
			case SDL_JOYBUTTONDOWN:
			case SDL_JOYBUTTONUP:
				if (CGameArg.CtlNoJoystick)
					continue;
				result = joy_button_handler(&event.jbutton);
				break;
			case SDL_JOYAXISMOTION:
				if (CGameArg.CtlNoJoystick)
					continue;
#if DXX_MAX_BUTTONS_PER_JOYSTICK || DXX_MAX_HATS_PER_JOYSTICK
				highest_result = std::max(joy_axisbutton_handler(&event.jaxis), highest_result);
#endif
				result = joy_axis_handler(&event.jaxis);
				break;
			case SDL_JOYHATMOTION:
				if (CGameArg.CtlNoJoystick)
					continue;
				result = joy_hat_handler(&event.jhat);
				break;
			case SDL_JOYBALLMOTION:
				continue;
#endif
			case SDL_QUIT: {
				result = call_default_handler(d_event{event_type::quit});
				break;
			}
			default:
				continue;
		}
		highest_result = std::max(result, highest_result);
	}
}

void event_flush()
{
	std::array<SDL_Event, 128> events;
	for (;;)
	{
		event_pump();
#if SDL_MAJOR_VERSION == 1
		const auto peep = SDL_PeepEvents(events.data(), events.size(), SDL_GETEVENT, SDL_ALLEVENTS);
#elif SDL_MAJOR_VERSION == 2
		const auto peep = SDL_PeepEvents(events.data(), events.size(), SDL_GETEVENT, SDL_FIRSTEVENT, SDL_LASTEVENT);
#endif
		if (peep != events.size())
			break;
	}
}

window_event_result call_default_handler(const d_event &event)
{
	return standard_handler(event);
}

window_event_result event_send(const d_event &event)
{
	window *wind;
	window_event_result handled = window_event_result::ignored;

	for (wind = window_get_front(); wind && handled == window_event_result::ignored; wind = window_get_prev(*wind))
		if (wind->is_visible())
		{
			handled = wind->send_event(event);

			if (handled == window_event_result::deleted) // break away if necessary: window_send_event() could have closed wind by now
				break;
			if (wind->is_modal())
				break;
		}
	
	if (handled == window_event_result::ignored)
		return call_default_handler(event);

	return handled;
}

// Process the first event in queue, sending to the appropriate handler
// This is the new object-oriented system
// Uses the old system for now, but this may change
window_event_result event_process(void)
{
	window *wind = window_get_front();
	window_event_result highest_result;

	timer_update();

	{
		const frame_probe::scope probe{frame_probe::phase::input};
		highest_result = event_poll();	// send input events first
	}

	cmd_queue_process();

	// Doing this prevents problems when a draw event can create a newmenu,
	// such as some network menus when they report a problem
	// Also checking for window_event_result::deleted in case a window was created
	// with the same pointer value as the deleted one
	if ((highest_result == window_event_result::deleted) || (window_get_front() != wind))
		return highest_result;

	const d_event event{event_type::window_draw};	// then draw all visible windows
	for (wind = window_get_first(); wind != nullptr;)
	{
		if (wind->is_visible())
		{
			auto prev = window_get_prev(*wind);
			auto result = wind->send_event(event);
			highest_result = std::max(result, highest_result);
			if (result == window_event_result::deleted)
			{
				if (!prev)
				{
					wind = window_get_first();
					continue;
				}
				wind = prev;	// take the previous window and get the next one from that (if prev isn't nullptr)
			}
		}
		wind = window_get_next(*wind);
	}

	{
		const frame_probe::scope probe{frame_probe::phase::swap};
		gr_flip();
	}

	return highest_result;
}

namespace {

template <bool activate_focus>
static void event_change_focus()
{
	const auto enable_grab = activate_focus && CGameCfg.Grabinput && likely(!CGameArg.DbgForbidConsoleGrab);
#if SDL_MAJOR_VERSION == 1
	SDL_WM_GrabInput(enable_grab ? SDL_GRAB_ON : SDL_GRAB_OFF);
#elif SDL_MAJOR_VERSION == 2
	SDL_SetWindowGrab(g_pRebirthSDLMainWindow, enable_grab ? SDL_TRUE : SDL_FALSE);
	SDL_SetRelativeMouseMode(enable_grab ? SDL_TRUE : SDL_FALSE);
#endif
	if (activate_focus)
		mouse_disable_cursor();
	else
		mouse_enable_cursor();
}

}

void event_enable_focus()
{
	event_change_focus<true>();
}

void event_disable_focus()
{
	event_change_focus<false>();
}

#if DXX_USE_EDITOR
static fix64 last_event = 0;

void event_reset_idle_seconds()
{
	last_event = timer_query();
}

fix event_get_idle_seconds()
{
	return (timer_query() - last_event)/F1_0;
}
#endif

}
