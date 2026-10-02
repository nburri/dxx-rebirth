/*
 * This file is part of the DXX-Rebirth project <https://github.com/dxx-rebirth/dxx-rebirth/>.
 * It is copyright by its individual contributors, as recorded in the
 * project's Git history.  See COPYING.txt at the top level for license
 * terms and a link to the Git history.
 */
/*
 * The system clipboard (SDL2).  Builds or platforms without a clipboard
 * (SDL 1.2, a video driver that has none) get an empty paste and a
 * failed copy, never an error.
 */

#pragma once

#include <string>
#include "key.h"

namespace dcx {

/* Ctrl+V, Shift+Insert, and Cmd+V on macOS. */
[[nodiscard]]
static inline bool key_is_paste(const int k)
{
	return k == KEY_CTRLED + KEY_V || k == KEY_SHIFTED + KEY_INSERT
#if defined(__APPLE__) || defined(macintosh)
		|| k == KEY_COMMAND + KEY_V
#endif
		;
}

/* Ctrl+C, Ctrl+Insert, and Cmd+C on macOS. */
[[nodiscard]]
static inline bool key_is_copy(const int k)
{
	return k == KEY_CTRLED + KEY_C || k == KEY_CTRLED + KEY_INSERT
#if defined(__APPLE__) || defined(macintosh)
		|| k == KEY_COMMAND + KEY_C
#endif
		;
}

/* The clipboard's text, empty when there is none or no clipboard. */
[[nodiscard]]
std::string clipboard_get_text();
/* Put `text` on the clipboard.  False when that failed. */
bool clipboard_set_text(const char *text);

}
