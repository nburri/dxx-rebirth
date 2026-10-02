/*
 * This file is part of the DXX-Rebirth project <https://github.com/dxx-rebirth/dxx-rebirth/>.
 * It is copyright by its individual contributors, as recorded in the
 * project's Git history.  See COPYING.txt at the top level for license
 * terms and a link to the Git history.
 */
/*
 * Switch off the Windows accessibility shortcut keys while the game has
 * the focus.  See accessibility_keys.h and Microsoft's "Disabling
 * Shortcut Keys in Games".
 */

#include <cstdlib>
#include <windows.h>
#include "accessibility_keys.h"
#include "console.h"

namespace dcx {

namespace {

/* Bits of g_modified: which settings the game changed and must put
 * back.
 */
enum : LONG
{
	modified_stickykeys = 1,
	modified_togglekeys = 2,
	modified_filterkeys = 4,
};

/* The player's settings, as read by the last suspend. */
STICKYKEYS g_saved_stickykeys{sizeof(STICKYKEYS), 0};
TOGGLEKEYS g_saved_togglekeys{sizeof(TOGGLEKEYS), 0};
FILTERKEYS g_saved_filterkeys{sizeof(FILTERKEYS), 0, 0, 0, 0, 0};
/* Written with Interlocked*, so that the crash handler (any thread)
 * restores each setting at most once.
 */
volatile LONG g_modified;
bool g_enabled;

/* Set the bit before changing the setting: a crash in between restores
 * the saved values, which is harmless.
 */
template <typename S>
static bool suspend_one(const UINT get, const UINT set, S &saved, const DWORD feature_on, const DWORD hotkey_flags, const LONG bit)
{
	S current{};
	current.cbSize = sizeof(S);
	if (!SystemParametersInfo(get, sizeof(S), &current, 0))
		return false;
	/* The player uses the feature: leave its shortcut alone, it may be
	 * how they switch it off.
	 */
	if (current.dwFlags & feature_on)
		return false;
	if (!(current.dwFlags & hotkey_flags))
		return false;
	saved = current;
	InterlockedOr(&g_modified, bit);
	current.dwFlags &= ~hotkey_flags;
	/* fWinIni 0: change the session only, never the user profile. */
	return SystemParametersInfo(set, sizeof(S), &current, 0);
}

static void restore_atexit()
{
	accessibility_keys_restore();
}

}

void accessibility_keys_suspend()
{
	if (!g_enabled || g_modified)
		return;
	const bool sk{suspend_one(SPI_GETSTICKYKEYS, SPI_SETSTICKYKEYS, g_saved_stickykeys, SKF_STICKYKEYSON, SKF_HOTKEYACTIVE | SKF_CONFIRMHOTKEY, modified_stickykeys)};
	const bool tk{suspend_one(SPI_GETTOGGLEKEYS, SPI_SETTOGGLEKEYS, g_saved_togglekeys, TKF_TOGGLEKEYSON, TKF_HOTKEYACTIVE | TKF_CONFIRMHOTKEY, modified_togglekeys)};
	const bool fk{suspend_one(SPI_GETFILTERKEYS, SPI_SETFILTERKEYS, g_saved_filterkeys, FKF_FILTERKEYSON, FKF_HOTKEYACTIVE | FKF_CONFIRMHOTKEY, modified_filterkeys)};
	static bool logged;
	if (!logged)
	{
		logged = true;
		con_printf(CON_VERBOSE, "accessibility-keys: shortcut keys switched off while focused: StickyKeys %s, ToggleKeys %s, FilterKeys %s", sk ? "yes" : "no", tk ? "yes" : "no", fk ? "yes" : "no");
	}
}

void accessibility_keys_restore()
{
	const LONG modified{InterlockedExchange(&g_modified, 0)};
	if (modified & modified_stickykeys)
		SystemParametersInfo(SPI_SETSTICKYKEYS, sizeof(STICKYKEYS), &g_saved_stickykeys, 0);
	if (modified & modified_togglekeys)
		SystemParametersInfo(SPI_SETTOGGLEKEYS, sizeof(TOGGLEKEYS), &g_saved_togglekeys, 0);
	if (modified & modified_filterkeys)
		SystemParametersInfo(SPI_SETFILTERKEYS, sizeof(FILTERKEYS), &g_saved_filterkeys, 0);
}

void accessibility_keys_init()
{
	if (g_enabled)
		return;
	g_enabled = true;
	std::atexit(&restore_atexit);
	accessibility_keys_suspend();
}

}
