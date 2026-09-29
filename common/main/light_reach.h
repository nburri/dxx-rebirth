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
 * does not change.
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

/* Breadth-first walk from `start` over the segments a light reaches.
 *
 * `Graph` provides:
 *	bool enter(Index): mark the segment as reached (and whatever goes
 *		with it, such as its vertices); false if it was reached already;
 *	void for_each_lit_child(Index, F): F(child) for each neighbour
 *		behind a side that lets light through and lies within the
 *		light's radius.
 *
 * `queue` holds at least `capacity` entries; the walk never reaches more
 * than `capacity` segments (the nearest ones, in steps, come first).
 * Returns the number of segments reached, `start` included.
 */
template <typename Graph, typename Index>
std::size_t walk_light_reach(Graph &g, const Index start, Index *const queue, const std::size_t capacity)
{
	if (!capacity || !g.enter(start))
		return 0;
	std::size_t head{0}, tail{0};
	queue[tail++] = start;
	while (head != tail)
	{
		const Index seg{queue[head++]};
		g.for_each_lit_child(seg, [&g, queue, capacity, &tail](const Index child) {
			if (tail != capacity && g.enter(child))
				queue[tail++] = child;
		});
	}
	return tail;
}

}
