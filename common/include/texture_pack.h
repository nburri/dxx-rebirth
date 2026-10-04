/*
 * This file is part of the DXX-Rebirth project <https://github.com/dxx-rebirth/dxx-rebirth/>.
 * It is copyright by its individual contributors, as recorded in the
 * project's Git history.  See COPYING.txt at the top level for license
 * terms and a link to the Git history.
 */
/*
 * Texture packs: replacement pictures for the game's wall and object
 * textures, loaded by the OpenGL renderer (ogl.cpp) instead of the
 * 64x64 palette bitmap of the same name.
 *
 * A replacement is a PNG file named after the bitmap as the PIG file
 * names it, searched in the PhysFS search path (the user directory and
 * the game directory):
 *
 *	textures/<mission>/<name>.png	only for the current mission
 *	textures/<name>.png		for every mission
 *
 * <mission> is the mission's file name without extension, in lower
 * case.  <name> is the bitmap name; frames of an animated bitmap are
 * called "<name>#<frame>", e.g. "textures/lava#3.png".  A bitmap that
 * the level replaced with its own art (a .POG file) uses only the
 * mission directory, so that a generic pack never covers custom art.
 *
 * Alpha 0 is transparent.  In an overlay with supertransparent pixels
 * (the holes that also hide the base texture), a pixel with alpha 0
 * and the color #FF00FF is such a hole; other transparent pixels show
 * the base texture.
 *
 * This file holds the parts that need neither OpenGL nor game data,
 * so that a unit test can check them.
 */

#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace dcx::texture_pack {

struct rgba_image
{
	unsigned w{}, h{};
	/* w * h * 4 bytes, rows from the top. */
	std::vector<uint8_t> px;
	bool empty() const
	{
		return !w || !h;
	}
};

/* The lower case directory name of a mission file name ("Corona" or
 * "Corona.mn2" -> "corona"); empty if the name is not usable as a
 * directory name.
 */
std::string mission_directory(std::string_view mission_filename);

/* The files to try for a bitmap, most specific first.  Empty if the
 * name could escape the textures directory.
 */
std::vector<std::string> candidate_paths(std::string_view bitmap_name, std::string_view mission_directory, bool mission_custom);

/* A hole pixel of a supertransparent overlay. */
static inline bool is_hole(const uint8_t *const p)
{
	return p[3] == 0 && p[0] == 255 && p[1] == 0 && p[2] == 255;
}

/* The image at power-of-two sizes no larger than max_size: bilinear,
 * wrapping around the edges (textures tile); nearest neighbour if the
 * image has holes, so that they stay holes.
 */
rgba_image to_power_of_two(const rgba_image &in, unsigned max_size);

/* The image scaled to w x h by nearest neighbour. */
rgba_image scale_nearest(const rgba_image &in, unsigned w, unsigned h);

/* An overlay over a base texture as the game's texture merge does it
 * (texmerge.cpp): the overlay rotated by orient (0-3, a quarter turn
 * each), blended by its alpha; holes are transparent in the result
 * if supertransparent.  Both are scaled to the larger size first.
 */
rgba_image composite(const rgba_image &base, const rgba_image &top, unsigned orient, bool supertransparent);

}
