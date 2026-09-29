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
 * lights only the corners of the segments the light reaches: the
 * segments connected to the light's segment through sides that can be
 * seen through (open sides, open doors, grates, glass), where each
 * segment passed lies within the light's radius.  The light is stored per
 * segment corner, not per vertex (lighting.h), since the segments on
 * either side of a wall share its vertices.  In an open room every
 * vertex within the radius is still reached (the straight line from the
 * light to it passes only through segments within the radius), so the
 * look of open rooms does not change.  The walk's cost is bounded (walk_light_reach):
 * where it stops early, nothing is lit beyond the distance it covered.
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

/* One entry of the walk's queue: a segment to process, a lower bound of
 * its distance to the light, and how many segments in a row that are not
 * rendered lead to it.
 */
template <typename Index, typename Key = int32_t>
struct light_reach_entry
{
	Key distance;
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

/* The segments that all the lights of a lighting pass may process:
 * each light gets its share of what is left (the segments left over the
 * lights left), no fewer than `min_segments` and no more than
 * `max_segments`.  Applying the lights dimmest first, what the small
 * lights do not need goes to the big ones.  A pass processes at most
 * about max(`segments`, `min_segments` per light).
 */
struct light_reach_pass_budget
{
	std::size_t segments;
	std::size_t lights;
	std::size_t min_segments;
	std::size_t max_segments;
	unsigned max_hidden_hops;
	/* The limits of the next light, which is counted as applied. */
	light_reach_limits next()
	{
		const std::size_t share{lights ? segments / lights : segments};
		if (lights)
			--lights;
		return {
			.max_segments = std::clamp(share, min_segments, max_segments),
			.max_hidden_hops = max_hidden_hops,
		};
	}
	void spend(const std::size_t processed)
	{
		segments -= std::min(segments, processed);
	}
};

/* The queue needs at most this many entries: each processed segment adds
 * at most one entry per side.
 */
constexpr std::size_t light_reach_queue_size(const std::size_t max_segments)
{
	return 6 * max_segments + 1;
}

/* Walk from `start` over the segments a light reaches: those behind
 * sides that let light through and within `reach` of the light, nearest
 * first.  In an open room, the straight line from the light to a vertex
 * at distance d passes only through segments nearer than d, so every
 * vertex within reach is in a segment the walk reaches.
 *
 * The cost is bounded: at most `limits.max_segments` segments are
 * processed, and a path through segments that are not rendered stops
 * after `limits.max_hidden_hops` of them.  Where the walk stops early, it
 * lowers the distance it returns, `complete`, to the distance of the
 * nearest segment it found but did not process, or where a path of
 * hidden segments stopped.  Any segment reachable through segments all
 * nearer than `complete` was processed (on such a path, the first
 * segment not processed would have been found), so the straight-line
 * argument holds within `complete`: a vertex nearer than that which was
 * not reached is behind a wall.  The caller lights nothing at `complete` or beyond.  If
 * nothing stopped the walk early, `complete` is `reach`.  Processing the
 * nearest segments first makes `complete` as large as the budget allows.
 *
 * `Graph` provides:
 *	bool rendered(Index): whether the segment is in the render list;
 *	void discover(Index): marks the segment as found;
 *	Key distance(Index): a lower bound of the distance from the light
 *		to any point of the segment;
 *	void process(Index): called for each segment processed, in order;
 *	void for_each_lit_child(Index, F): F(child) for each neighbour
 *		not found yet behind a side that lets light through.
 *
 * `queue` holds `light_reach_queue_size(limits.max_segments)` entries,
 * `segments` receives the segments processed, nearest first (at most
 * `limits.max_segments`), and `processed`, if given, their number.
 */
template <typename Graph, typename Index, typename Key>
Key walk_light_reach(Graph &g, const Index start, const Key reach, light_reach_entry<Index, Key> *const queue, Index *const segments, const light_reach_limits limits, std::size_t *const processed = nullptr)
{
	Key complete{reach};
	std::size_t n_processed{0}, tail{0};
	/* A heap, nearest first. */
	const auto nearer_first = [](const light_reach_entry<Index, Key> &a, const light_reach_entry<Index, Key> &b) {
		return a.distance > b.distance;
	};
	const uint8_t start_hops{g.rendered(start) ? uint8_t{0} : uint8_t{1}};
	if (limits.max_segments && start_hops <= limits.max_hidden_hops)
	{
		g.discover(start);
		queue[tail++] = {Key{0}, start, start_hops};
	}
	else
		complete = Key{0};
	while (tail)
	{
		if (n_processed == limits.max_segments)
		{
			/* Out of budget: queue[0] is the nearest segment found but
			 * not processed.
			 */
			complete = std::min(complete, queue[0].distance);
			break;
		}
		std::pop_heap(queue, queue + tail, nearer_first);
		const auto e{queue[--tail]};
		segments[n_processed++] = e.segment;
		g.process(e.segment);
		g.for_each_lit_child(e.segment, [&](const Index child) {
			const Key distance{g.distance(child)};
			if (distance >= complete)
				return;
			const uint8_t hops{g.rendered(child) ? uint8_t{0} : static_cast<uint8_t>(e.hidden_hops + 1)};
			if (hops > limits.max_hidden_hops)
			{
				complete = distance;
				return;
			}
			g.discover(child);
			queue[tail++] = {distance, child, hops};
			std::push_heap(queue, queue + tail, nearer_first);
		});
	}
	if (processed)
		*processed = n_processed;
	return complete;
}

}
