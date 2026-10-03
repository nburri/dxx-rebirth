/*
 * This file is part of the DXX-Rebirth project <https://github.com/dxx-rebirth/dxx-rebirth/>.
 * It is copyright by its individual contributors, as recorded in the
 * project's Git history.  See COPYING.txt at the top level for license
 * terms and a link to the Git history.
 */
/*
 * -visshot <mission> <level> <dir>: a debug tool for comparing the
 * visual quality options (ogl_effects.h).  It starts the level as a
 * single player game without menus, places the camera at the player
 * start and at a few long straight views of the level (with an
 * explosion, a flare and some shots in view), renders every viewpoint
 * with every preset of options, writes each picture as a PPM file
 * (<dir>/vpN-<preset>.ppm) and the time per frame of each preset to
 * <dir>/timing.txt, and quits.  The pictures show the game's art: keep
 * them local.
 */

#pragma once

#include "dxxsconf.h"
#include "dsx-ns.h"
#include "args.h"
#include "window.h"

namespace dcx {

[[nodiscard]]
static inline bool vis_shot_active()
{
	return !CGameArg.DbgVisShotDir.empty();
}

}

#ifdef DXX_BUILD_DESCENT
namespace dsx {

/* inferno.cpp: load the mission and start the level; false on failure. */
bool vis_shot_start();
/* game.cpp, every frame: after a few frames, take the pictures and
 * return window_event_result::close.
 */
window_event_result vis_shot_frame();
int vis_shot_exit_status();

}
#endif
