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
#include "dxxsconf.h"

#if DXX_USE_SDLIMAGE
#include <SDL_image.h>
#endif

namespace dsx {

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
		/* Initialize the gamecontroller layer first, so that the mappings
		 * from gamecontrollerdb.txt are loaded before joy_init() asks
		 * SDL_IsGameController().  Otherwise, a device that is only
		 * recognized through that file is opened both as a joystick and
		 * as a gamecontroller, and every hat/D-pad press is delivered
		 * twice (SDL_JOYHATMOTION and SDL_CONTROLLERBUTTONDOWN).
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
