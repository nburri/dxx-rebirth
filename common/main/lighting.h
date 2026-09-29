/*
 * Portions of this file are copyright Rebirth contributors and licensed as
 * described in COPYING.txt.
 * Portions of this file are copyright Parallax Software and licensed
 * according to the Parallax license below.
 * See COPYING.txt for license details.

THE COMPUTER CODE CONTAINED HEREIN IS THE SOLE PROPERTY OF PARALLAX
SOFTWARE CORPORATION ("PARALLAX").  PARALLAX, IN DISTRIBUTING THE CODE TO
END-USERS, AND SUBJECT TO ALL OF THE TERMS AND CONDITIONS HEREIN, GRANTS A
ROYALTY-FREE, PERPETUAL LICENSE TO SUCH END-USERS FOR USE BY SUCH END-USERS
IN USING, DISPLAYING,  AND CREATING DERIVATIVE WORKS THEREOF, SO LONG AS
SUCH USE, DISPLAY OR CREATION IS FOR NON-COMMERCIAL, ROYALTY OR REVENUE
FREE PURPOSES.  IN NO EVENT SHALL THE END-USER USE THE COMPUTER CODE
CONTAINED HEREIN FOR REVENUE-BEARING PURPOSES.  THE END-USER UNDERSTANDS
AND AGREES TO THE TERMS HEREIN AND ACCEPTS THE SAME BY USE OF THIS FILE.
COPYRIGHT 1993-1999 PARALLAX SOFTWARE CORPORATION.  ALL RIGHTS RESERVED.
*/

/*
 *
 * Lighting system prototypes, structures, etc.
 *
 */

#pragma once

#include "maths.h"
#include "vecmat.h"

#include "fwd-object.h"
#include "fwd-segment.h"
#include "3d.h"
#include "d_array.h"
#include <cstdint>

#define MIN_LIGHT_DIST  (F1_0*4)

namespace dcx {

/* The dynamic light of one segment, per corner (segment_relative_vertnum).
 * Valid only if `generation` is the current lighting pass.
 */
struct segment_dynamic_light
{
	uint32_t generation;
	per_segment_relative_vertnum_array<g3s_lrgb> corners;
};

struct d_level_unique_light_state
{
	/* The light of objects (shots, fireballs, flares, the ship's glow,
	 * muzzle flashes) is stored per segment corner, not per vertex: two
	 * segments on either side of a solid wall or a closed door share the
	 * wall's vertices, and light stored per vertex would reach the faces
	 * of both.  apply_light writes only the corners of the segments the
	 * light reaches (light_reach.h).
	 */
	per_segment_array<segment_dynamic_light> Segment_dynamic_light;
	/* The current lighting pass; entries of Segment_dynamic_light from
	 * earlier passes hold no light.
	 */
	uint32_t Segment_dynamic_light_generation{1};
	/* Headlights only, per vertex (they keep Descent's rule, see
	 * apply_light), for the vertices rendered by the last pass (the
	 * others are cleared).
	 */
	per_vertex_array<g3s_lrgb> Headlight_dynamic_light;
	/* The corner light of `segnum` from the current pass, or nullptr if
	 * no light reached it.
	 */
	const per_segment_relative_vertnum_array<g3s_lrgb> *get_segment_dynamic_light(const segnum_t segnum) const
	{
		auto &l{Segment_dynamic_light[segnum]};
		return l.generation == Segment_dynamic_light_generation ? &l.corners : nullptr;
	}
};

}

#ifdef DXX_BUILD_DESCENT
namespace dsx {

#if DXX_BUILD_DESCENT == 2
struct d_level_unique_headlight_state
{
	unsigned Num_headlights{};
	std::array<const object_base *, 8> Headlights{};
};

struct d_level_unique_light_state :
	d_level_unique_headlight_state,
	::dcx::d_level_unique_light_state
{
};
#endif

extern d_level_unique_light_state LevelUniqueLightState;
}
extern const object *old_viewer;

namespace dsx {
// compute the lighting for an object.  Takes a pointer to the object,
// and possibly a rotated 3d point.  If the point isn't specified, the
// object's center point is rotated.
g3s_lrgb compute_object_light(const d_level_unique_light_state &LevelUniqueLightState, vcobjptridx_t obj);

// turn headlight boost on & off
#if DXX_BUILD_DESCENT == 2
void toggle_headlight_active(object &);
#endif
}
void start_lighting_frame(const object &viewer);
#endif
