/*
 * This file is part of the DXX-Rebirth project <https://github.com/dxx-rebirth/dxx-rebirth/>.
 * It is copyright by its individual contributors, as recorded in the
 * project's Git history.  See COPYING.txt at the top level for license
 * terms and a link to the Git history.
 */
// Holds the main init and de-init functions for arch-related program parts

#include <SDL.h>
#include "songs.h"
#include "key.h"
#include "digi.h"
#include "mouse.h"
#include "joy.h"
#include "gr.h"
#include "dxxerror.h"
#include "text.h"
#include "args.h"
#include "window.h"
#include "console.h"
#include "dxxsconf.h"
#include "accessibility_keys.h"

#if DXX_USE_SDLIMAGE
#include <SDL_image.h>
#endif

namespace dsx {

#if DXX_MAX_JOYSTICKS && SDL_MAJOR_VERSION == 2
namespace {

/* Set a hint unless the player set the environment variable of the
 * same name: SDL ignores hints below SDL_HINT_OVERRIDE priority when
 * that variable exists, so every choice below can be undone from the
 * environment (e.g. SDL_JOYSTICK_HIDAPI=1).
 */
static void set_default_hint(const char *const name, const char *const value)
{
	SDL_SetHintWithPriority(name, value, SDL_HINT_DEFAULT);
}

static void log_hint(const char *const name)
{
	const auto v{SDL_GetHint(name)};
	con_printf(CON_NORMAL, "sdl-joystick: hint %s=%s%s", name, v ? v : "(SDL default)", SDL_getenv(name) ? " (from environment)" : "");
}

/* Must run before the joystick and gamecontroller subsystems start:
 * SDL reads most of these hints only in SDL_Init.
 */
static void set_joystick_hints()
{
	/* event_pump (event.cpp) calls SDL_JoystickUpdate right after
	 * SDL_PumpEvents, so that the frame probe can measure it on its
	 * own.  Without this hint, SDL_PumpEvents would also call it.
	 */
#ifdef SDL_HINT_AUTO_UPDATE_JOYSTICKS
	set_default_hint(SDL_HINT_AUTO_UPDATE_JOYSTICKS, "0");
#endif
#ifdef _WIN32
	/* Input stalls on Windows (v0.61-exp-19 gamelog: clusters of 50 to
	 * 180 ms frames, all spent in SDL_PumpEvents).  SDL 2.32 polls and
	 * detects joysticks inside SDL_PumpEvents, on the game thread, and
	 * several of its Windows backends do slow work there whenever
	 * Windows reports a device change, even for devices that are not
	 * game controllers:
	 *
	 * - HIDAPI registers for WM_DEVICECHANGE with
	 *   DEVICE_NOTIFY_ALL_INTERFACE_CLASSES, so an arrival or removal of
	 *   any device interface (USB, audio endpoints, Bluetooth, virtual
	 *   devices of vendor tools) makes the next SDL_JoystickUpdate call
	 *   SDL_hid_enumerate, which opens every HID device on the system
	 *   to read its attributes and strings.  HIDAPI only adds extended
	 *   support for some gamepads (PlayStation, Switch, Stadia, Steam:
	 *   gyro, LEDs, rumble) which Descent does not use; without it,
	 *   those pads still work through DirectInput and the
	 *   gamecontrollerdb.txt mappings, and Xbox pads through XInput.
	 *
	 * - RawInput handles Xbox-compatible pads: it opens each HID
	 *   device again on arrival, and while such a pad is connected, it
	 *   correlates its reports with XInput and Windows.Gaming.Input
	 *   readings every frame.  XInput covers these pads.
	 *
	 * - Windows.Gaming.Input delivers device arrivals and removals on
	 *   its own thread, which holds SDL's joystick lock while it looks
	 *   the device up; SDL_JoystickUpdate on the game thread waits for
	 *   that lock.  Its startup is also slow.  XInput covers the same
	 *   pads.
	 *
	 * - SDL_JOYSTICK_THREAD moves the joystick device notification
	 *   window (WM_DEVICECHANGE, raw input device messages) to a
	 *   helper thread, so that the game thread's message pump does not
	 *   process them.
	 *
	 * DirectInput (flight sticks such as the Thrustmaster T.16000M,
	 * which have no HIDAPI driver) and XInput (Xbox pads) stay enabled.
	 * DirectInput still enumerates game controllers again on the game
	 * thread after a HID device change (CM_Register_Notification and
	 * WM_DEVICECHANGE, with follow-ups 300 ms and 2 s later); the frame
	 * probe shows that as "joystick-update" time with "hid-changes".
	 */
	set_default_hint(SDL_HINT_JOYSTICK_HIDAPI, "0");
	set_default_hint(SDL_HINT_JOYSTICK_RAWINPUT, "0");
#ifdef SDL_HINT_JOYSTICK_WGI
	set_default_hint(SDL_HINT_JOYSTICK_WGI, "0");
#endif
#ifdef SDL_HINT_JOYSTICK_THREAD
	set_default_hint(SDL_HINT_JOYSTICK_THREAD, "1");
#endif
	log_hint(SDL_HINT_JOYSTICK_HIDAPI);
	log_hint(SDL_HINT_JOYSTICK_RAWINPUT);
#ifdef SDL_HINT_JOYSTICK_WGI
	log_hint(SDL_HINT_JOYSTICK_WGI);
#endif
#ifdef SDL_HINT_JOYSTICK_THREAD
	log_hint(SDL_HINT_JOYSTICK_THREAD);
#endif
#endif
#ifdef SDL_HINT_AUTO_UPDATE_JOYSTICKS
	log_hint(SDL_HINT_AUTO_UPDATE_JOYSTICKS);
#endif
}

}
#endif

static void arch_close(void)
{
	songs_uninit();

	gr_close();

#if DXX_MAX_JOYSTICKS
	if (!CGameArg.CtlNoJoystick)
	{
		joy_close();
#if SDL_MAJOR_VERSION == 2
		gamecontroller_close();
#endif
	}
#endif

	if (!CGameArg.CtlNoMouse)
		mouse_close();

	if (!CGameArg.SndNoSound)
	{
		digi_close();
	}
#if DXX_USE_SDLIMAGE
	IMG_Quit();
#endif
	SDL_Quit();
	accessibility_keys_restore();
}

arch_atexit::~arch_atexit()
{
	arch_close();
}

arch_atexit arch_init()
{
	int t;

	if (SDL_Init(SDL_INIT_VIDEO) < 0)
		Error("SDL library initialisation failed: %s.",SDL_GetError());
#if DXX_USE_SDLIMAGE
	IMG_Init(0);
#endif
#if SDL_MAJOR_VERSION == 2
	/* In SDL1, grabbing input grabbed both the keyboard and the mouse.
	 * Many game management keys assume a keyboard grab.
	 * Tell SDL2 to grab the keyboard.
	 *
	 * Unlike with SDL1, players have the option of overriding this grab
	 * by setting an environment variable.  In SDL1, the only choice was
	 * to skip both the keyboard grab and the mouse grab.  Now, players
	 * can enable grabbing in the UI, but disable keyboard grab with the
	 * environment variable.
	 */
	SDL_SetHint(SDL_HINT_GRAB_KEYBOARD, "1");
	/* Gameplay continues regardless of focus, so keep the window
	 * visible.
	 */
	SDL_SetHint(SDL_HINT_VIDEO_MINIMIZE_ON_FOCUS_LOSS, "0");
	/* Support the Alt+Shift+F4 hotkey for renaming the Guide-Bot
	 */
	SDL_SetHint(SDL_HINT_WINDOWS_NO_CLOSE_ON_ALT_F4, "1");
#endif

	/* Windows: no StickyKeys/FilterKeys/ToggleKeys prompt mid-fight.
	 * Restored when the window loses the focus and on exit.
	 */
	if (!CGameArg.CtlKeepAccessibilityKeys)
		accessibility_keys_init();

	key_init();

	digi_select_system();

	if (!CGameArg.SndNoSound)
		digi_init();

	if (!CGameArg.CtlNoMouse)
		mouse_init();

#if DXX_MAX_JOYSTICKS
	if (!CGameArg.CtlNoJoystick)
	{
#if SDL_MAJOR_VERSION == 2
		set_joystick_hints();
		/* Initialize the gamecontroller layer first, so that the mappings
		 * from gamecontrollerdb.txt are loaded before joy_init() asks
		 * SDL_IsGameController().  Otherwise, a device that is only
		 * recognized through that file is opened both as a joystick and
		 * as a gamecontroller, and every hat/D-pad press is delivered
		 * twice (SDL_JOYHATMOTION and SDL_CONTROLLERBUTTONDOWN).
		 * gamecontroller_use_for_device() decides which of the two
		 * layers opens each device.
		 */
		gamecontroller_init();
#endif
		joy_init();
	}
#endif

	if ((t = gr_init()) != 0)
		Error(TXT_CANT_INIT_GFX,t);

	return {};
}

}
