/*
 * This file is part of the DXX-Rebirth project <https://github.com/dxx-rebirth/dxx-rebirth/>.
 * It is copyright by its individual contributors, as recorded in the
 * project's Git history.  See COPYING.txt at the top level for license
 * terms and a link to the Git history.
 */
/*
 * The system clipboard through SDL2 (clipboard.h).
 */

#include "dxxsconf.h"
#include <SDL.h>
#include <SDL_version.h>
#include "clipboard.h"
#include "console.h"

namespace dcx {

std::string clipboard_get_text()
{
#if SDL_MAJOR_VERSION == 2
	if (!SDL_HasClipboardText())
		return {};
	char *const text{SDL_GetClipboardText()};
	if (!text)
		return {};
	std::string r{text};
	SDL_free(text);
	return r;
#else
	return {};
#endif
}

bool clipboard_set_text(const char *const text)
{
#if SDL_MAJOR_VERSION == 2
	if (SDL_SetClipboardText(text) == 0)
		return true;
	con_printf(CON_VERBOSE, "Clipboard: cannot copy: %s", SDL_GetError());
	return false;
#else
	(void)text;
	return false;
#endif
}

}
