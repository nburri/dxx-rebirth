#pragma once

#include <memory>
#include <utility>
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
	/* State of the segments of the current render list, indexed by
	 * segment number.  The storage is kept from frame to frame, so that
	 * building the render list does not allocate: a std::unordered_map
	 * allocated a node for every visible segment and a vector for every
	 * segment with objects on every frame.  Like the map, an entry is
	 * created in its default state when first used after clear(), and
	 * references to entries stay valid until clear().
	 */
	class segment_state_map
	{
		std::unique_ptr<per_segment_state_t[]> states{std::make_unique<per_segment_state_t[]>(MAX_SEGMENTS)};
		std::unique_ptr<bool[]> in_use{std::make_unique<bool[]>(MAX_SEGMENTS)};
		std::vector<segnum_t> used;
	public:
		/* Returns the entry, and whether it was created by this call. */
		std::pair<per_segment_state_t &, bool> try_emplace(const segnum_t segnum)
		{
			auto &e = states[segnum];
			auto &u = in_use[segnum];
			if (u)
				return {e, false};
			u = true;
			used.emplace_back(segnum);
			return {e, true};
		}
		per_segment_state_t &operator[](const segnum_t segnum)
		{
			return try_emplace(segnum).first;
		}
		void clear()
		{
			for (const auto segnum : used)
			{
				auto &e = states[segnum];
				/* Keep the buffer of the object list. */
				auto objects{std::move(e.objects)};
				objects.clear();
				e = {};
				e.objects = std::move(objects);
				in_use[segnum] = false;
			}
			used.clear();
		}
	};
	segment_state_map render_seg_map;
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
