/*
 * This file is part of the DXX-Rebirth project <https://github.com/dxx-rebirth/dxx-rebirth/>.
 * It is copyright by its individual contributors, as recorded in the
 * project's Git history.  See COPYING.txt at the top level for license
 * terms and a link to the Git history.
 */
/*
 * Where a dynamic light can reach (lighting.cpp).  Descent's dynamic
 * lights have no occlusion: an object's light is added to every rendered
 * vertex within its radius, so the shots in one room light the walls of
 * the next room through the solid wall between them.  apply_light now
 * lights only the vertices of the segments the light reaches: the
 * segments connected to the light's segment through sides that can be
 * seen through (open sides, open doors, grates, glass), where each side
 * passed lies within the light's radius.  In an open room every vertex
 * within the radius is still reached (the straight line from the light
 * to it crosses only sides within the radius), so the look of open rooms
 * does not change.  The walk's cost is bounded (walk_light_reach): where
 * it stops early, the old rule applies beyond the distance it covered.
 *
 * Standard library only, so that test-light-reach can check the walk on
 * synthetic levels.
 */

#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>

namespace dcx {

/* One mark per element, cleared for all elements at once by next():
 * an element is marked if its stamp equals the current generation.
 */
template <std::size_t N>
class generation_marks
{
	std::array<uint32_t, N> stamps{};
	uint32_t current{1};
public:
	/* Clears every mark. */
	void next()
	{
		if (++current == 0)
		{
			/* After 2^32 - 1 generations, old stamps could match again. */
			stamps.fill(0);
			current = 1;
		}
	}
	/* Marks `i`; returns whether it was not marked yet. */
	bool set(const std::size_t i)
	{
		auto &s{stamps[i]};
		if (s == current)
			return false;
		s = current;
		return true;
	}
	bool test(const std::size_t i) const
	{
		return stamps[i] == current;
	}
	/* For the test: jump close to the wrap of the generation. */
	void set_generation_for_test(const uint32_t g)
	{
		current = g;
	}
};

/* One entry of the walk's queue: a segment to process, and how many
 * segments in a row that are not rendered lead to it.
 */
template <typename Index>
struct light_reach_entry
{
	Index segment;
	uint8_t hidden_hops;
};

struct light_reach_limits
{
	/* At most this many segments are processed per light. */
	std::size_t max_segments;
	/* A path passes at most this many segments in a row that are not
	 * rendered (the light's own segment may be one of them).
	 */
	unsigned max_hidden_hops;
};

/* The queue needs at most this many entries: each processed segment adds
 * at most one entry per side.
 */
constexpr std::size_t light_reach_queue_size(const std::size_t max_segments)
{
	return 6 * max_segments + 1;
}

/* Breadth-first walk from `start` over the segments a light reaches:
 * those behind sides that let light through and lie within `reach` of
 * the light.  In an open room, the straight line from the light to a
 * vertex at distance d crosses only sides nearer than d, so every vertex
 * within reach is in a segment the walk reaches.
 *
 * The cost is bounded: at most `limits.max_segments` segments are
 * processed, and a path through segments that are not rendered stops
 * after `limits.max_hidden_hops` of them.  Where the walk stops early, it
 * lowers the distance it returns, `complete`, to the distance of the
 * nearest side it did not pass or of the nearest segment it did not
 * process.  Any segment reachable through sides all nearer than
 * `complete` was processed, so the straight-line argument
 * holds within `complete`: a vertex nearer than that which was not
 * reached is behind a wall.  The caller keeps the old rule (no
 * occlusion) for vertices at `complete` or beyond.  If nothing stopped
 * the walk early, `complete` is `reach`.
 *
 * `Graph` provides:
 *	bool rendered(Index): whether the segment is in the render list;
 *	bool discover(Index): marks the segment as found; true the first
 *		time;
 *	Key distance(Index): a lower bound of the distance from the light
 *		to any point of the segment;
 *	void process(Index): marks the segment's vertices as reached;
 *	void for_each_lit_child(Index, F): F(child, distance) for each
 *		neighbour not found yet behind a side that lets light through,
 *		with a lower bound of the distance to that side, if below
 *		`reach`.
 *
 * `queue` holds `light_reach_queue_size(limits.max_segments)` entries.
 * `processed`, if given, receives the number of segments processed.
 */
template <typename Graph, typename Index, typename Key>
Key walk_light_reach(Graph &g, const Index start, const Key reach, light_reach_entry<Index> *const queue, const light_reach_limits limits, std::size_t *const processed = nullptr)
{
	Key complete{reach};
	std::size_t head{0}, tail{0};
	const uint8_t start_hops{g.rendered(start) ? uint8_t{0} : uint8_t{1}};
	if (limits.max_segments && start_hops <= limits.max_hidden_hops)
	{
		g.discover(start);
		queue[tail++] = {start, start_hops};
	}
	else
		complete = Key{0};
	while (head != tail)
	{
		if (head == limits.max_segments)
		{
			/* Out of budget: the segments left in the queue were not
			 * processed.
			 */
			for (std::size_t i{head}; i != tail; ++i)
				complete = std::min(complete, g.distance(queue[i].segment));
			break;
		}
		const auto e{queue[head++]};
		g.process(e.segment);
		g.for_each_lit_child(e.segment, [&](const Index child, const Key distance) {
			if (distance >= complete)
				return;
			const uint8_t hops{g.rendered(child) ? uint8_t{0} : static_cast<uint8_t>(e.hidden_hops + 1)};
			if (hops > limits.max_hidden_hops)
			{
				complete = distance;
				return;
			}
			if (g.discover(child))
				queue[tail++] = {child, hops};
		});
	}
	if (processed)
		*processed = head;
	return complete;
}

}
