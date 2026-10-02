/*
 * This file is part of the DXX-Rebirth project <https://github.com/dxx-rebirth/dxx-rebirth/>.
 * It is copyright by its individual contributors, as recorded in the
 * project's Git history.  See COPYING.txt at the top level for license
 * terms and a link to the Git history.
 */
#pragma once

/* Windows accessibility shortcut keys (StickyKeys: Shift 5 times,
 * FilterKeys: hold right Shift for 8 seconds, ToggleKeys: hold NumLock
 * for 5 seconds).  Their confirmation prompts take the focus away from
 * the game in the middle of a fight.  Following Microsoft's
 * "Disabling Shortcut Keys in Games", the game switches off only the
 * shortcut keys, only while it has the focus, and only for features the
 * player has not switched on.  The settings are never written to the
 * user profile, and are restored on exit, on fatal errors and in the
 * crash handler.  -keep-accessibility-keys disables all of this.
 *
 * Other platforms: no-ops.
 */

namespace dcx {

#ifdef _WIN32
/* Startup (not called with -keep-accessibility-keys): enable the
 * feature and switch off the shortcut keys.  Registers an atexit
 * restore.
 */
void accessibility_keys_init();
/* Focus gained: read the current settings again (the player may have
 * changed them while the game was in the background) and switch off the
 * shortcut keys.  Nothing until accessibility_keys_init has run.
 */
void accessibility_keys_suspend();
/* Focus lost, exit, fatal error, crash: put back the settings saved by
 * the last accessibility_keys_suspend.  Idempotent, safe to call from
 * any thread: only SystemParametersInfo with saved structs.
 */
void accessibility_keys_restore();
#else
static inline void accessibility_keys_init() {}
static inline void accessibility_keys_suspend() {}
static inline void accessibility_keys_restore() {}
#endif

}
