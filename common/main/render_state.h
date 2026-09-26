#pragma once

#include <unordered_map>
#include <vector>
#include "dxxsconf.h"
#include "fwd-segment.h"
#include "fwd-robot.h"
#include "objnum.h"
#include <array>
#include <limits>


struct rect
{
	short left,top,right,bot;
};

namespace dcx {

struct render_state_t
{
	struct per_segment_state_t
	{
		struct distant_object
		{
			objnum_t objnum;
		};
		std::vector<distant_object> objects;
		uint16_t Seg_depth{0};		//depth for this seg in Render_list
		bool processed = false;		//whether this entry has been processed
		rect render_window;
	};
	unsigned N_render_segs{0};
	/* Every segment is added to the list at most once (render_pos), so the
	 * list can hold all segments of a level and never has to stop early.
	 * The former limit of 500 was too small for the large rooms of some
	 * custom levels: walls beyond it were missing or flickered.
	 */
	std::array<segnum_t, MAX_SEGMENTS> Render_list;
	std::array<short, MAX_SEGMENTS> render_pos;	//where in render_list does this segment appear?
	static_assert(MAX_SEGMENTS <= std::numeric_limits<short>::max(), "render_pos must be able to hold every list position");
	std::unordered_map<segnum_t, per_segment_state_t> render_seg_map;
};

}

#ifdef DXX_BUILD_DESCENT
namespace dsx {
#if DXX_BUILD_DESCENT == 1
#define set_dynamic_light(Robot_info, render)	set_dynamic_light(render)
#elif DXX_BUILD_DESCENT == 2
#undef set_dynamic_light
#endif
void set_dynamic_light(const d_robot_info_array &, render_state_t &);
}
#endif
