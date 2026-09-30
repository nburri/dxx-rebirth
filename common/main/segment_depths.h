/*
 * This file is part of the DXX-Rebirth project <https://github.com/dxx-rebirth/dxx-rebirth/>.
 * It is copyright by its individual contributors, as recorded in the
 * project's Git history.  See COPYING.txt at the top level for license
 * terms and a link to the Git history.
 */
/*
 * The traversal of connected_segment_raw_distances (fireball.cpp): the
 * depth (in segments) of every segment within `max_depth` of a start
 * segment, and how many segments lie at each depth.  The network powerup
 * drop (choose_drop_segment) and the thief's recreation draw a segment
 * at a given depth by those counts (scan_segment_depths).  Standard
 * library only.
 *
 * The traversal used to count a segment only below `max_depth`, but
 * took one off the count of its old depth whenever it found a shorter
 * route to it, also at `max_depth`, where it had never been counted.
 * The 16-bit count wrapped (count=655xx in the log), and the drop, which
 * scans `max_depth` first, skipped past every segment and reported an
 * error each time.  Now every segment is counted at the depth it is
 * recorded with, `max_depth` included, and taken off exactly that count
 * when it moves.  The control centre's segment is excluded at any depth
 * (it was only below `max_depth`).
 */

#pragma once

#include <cstdint>
#include <optional>

namespace dcx {

/* `Graph` provides:
 *	segment_index: the segment type;
 *	std::optional<uint8_t> depth_of(segment_index): the recorded depth;
 *	bool excluded(segment_index): never a result (the control centre);
 *	void exclude(segment_index): record it at depth 0, uncounted, so
 *		that no later route changes it;
 *	void record(segment_index, uint8_t depth);
 *	void count(uint8_t depth, int delta): the count at `depth`, if it
 *		keeps one for that depth;
 *	uint8_t max_depth_of();
 *	for_each_passable_child(segment_index, F): F(child) for each child
 *		a ship can fly to.
 *
 * Invariant: a segment recorded at depth d (not excluded) is in the
 * count of d, so each count is the number of segments recorded at its
 * depth.
 */
template <typename Graph>
void visit_segment_depths(const Graph &g, const typename Graph::segment_index seg, const uint8_t depth)
{
	if (const auto known{g.depth_of(seg)})
	{
		/* Found already by a route as short or shorter. */
		if (*known <= depth)
			return;
		/* A shorter route: off the count of its old depth. */
		g.count(*known, -1);
	}
	if (g.excluded(seg))
	{
		/* Not a result, and not traversed: a segment behind it is
		 * reached by another route, if any.
		 */
		g.exclude(seg);
		return;
	}
	g.record(seg, depth);
	g.count(depth, 1);
	if (depth >= g.max_depth_of())
		return;
	const uint8_t next{static_cast<uint8_t>(depth + 1u)};
	g.for_each_passable_child(seg, [&g, next](const typename Graph::segment_index child) {
		visit_segment_depths(g, child, next);
	});
}

}
