/*
 * This file is part of the DXX-Rebirth project <https://github.com/dxx-rebirth/dxx-rebirth/>.
 * It is copyright by its individual contributors, as recorded in the
 * project's Git history.  See COPYING.txt at the top level for license
 * terms and a link to the Git history.
 */
/*
 * Bot navigation, the game-independent part
 * (Documentation/multiplayer-bots.md section 4.3):
 *
 * - `nav_graph`: nodes with positions and directed edges with a cost and
 *   the side they leave through (the game's segment graph, built by
 *   similar/main/bot.cpp at level start);
 * - `astar_search`: A* with a pluggable passability test and extra edge
 *   cost (doors and walls change during the game, so they are asked for
 *   at search time), deterministic tie-breaking, a node limit and a
 *   partial path to the explored node nearest the goal;
 * - `pull_string` and `advance_along`: which path point the bot steers
 *   at, and when it has reached one;
 * - `nav_distances`: the path cost and segment count to every node
 *   within reach (stage B3: collection, map knowledge);
 * - `stuck_detector` and `edge_penalties`: stuck recovery.
 *
 * Standard library only (common/unittest/bot_nav.cpp).
 */

#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <vector>

#include "bot_vec.h"

namespace dcx::bot {

constexpr uint8_t NAV_NO_SIDE{0xff};

struct nav_edge
{
	uint32_t to{};
	/* The side of the edge's source node it leaves through. */
	uint8_t side{NAV_NO_SIDE};
	/* At least the distance between the two node positions, so that the
	 * straight-line heuristic is admissible.
	 */
	float cost{};
};

/* Compressed adjacency: the edges of node n are
 * edges[first[n] .. first[n + 1]).
 */
class nav_graph
{
	std::vector<vec3> m_pos;
	std::vector<uint32_t> m_first;
	std::vector<nav_edge> m_edges;
	uint32_t m_next_node{};
public:
	void clear()
	{
		m_pos.clear();
		m_first.clear();
		m_edges.clear();
		m_next_node = 0;
	}
	/* Start a graph of `n` nodes; add edges in ascending source order,
	 * then call finish().
	 */
	void begin(const std::size_t n)
	{
		m_pos.assign(n, vec3{});
		m_first.assign(n + 1, 0);
		m_edges.clear();
		m_next_node = 0;
	}
	void set_position(const uint32_t n, const vec3 &p)
	{
		m_pos[n] = p;
	}
	void add_edge(const uint32_t from, const nav_edge &e)
	{
		while (m_next_node <= from)
			m_first[m_next_node++] = static_cast<uint32_t>(m_edges.size());
		m_edges.push_back(e);
	}
	void finish()
	{
		while (m_next_node < m_first.size())
			m_first[m_next_node++] = static_cast<uint32_t>(m_edges.size());
	}
	[[nodiscard]]
	std::size_t size() const
	{
		return m_pos.size();
	}
	[[nodiscard]]
	std::size_t edge_count() const
	{
		return m_edges.size();
	}
	[[nodiscard]]
	const vec3 &position(const uint32_t n) const
	{
		return m_pos[n];
	}
	[[nodiscard]]
	std::span<const nav_edge> neighbours(const uint32_t n) const
	{
		return std::span<const nav_edge>(m_edges).subspan(m_first[n], m_first[n + 1] - m_first[n]);
	}
};

struct path_step
{
	uint32_t node{};
	/* The side of the previous node this step entered through
	 * (NAV_NO_SIDE for the start).
	 */
	uint8_t side{NAV_NO_SIDE};
	constexpr bool operator==(const path_step &) const = default;
};

struct path_result
{
	std::vector<path_step> steps;
	/* The path reaches the goal; otherwise it ends at the explored node
	 * nearest the goal (no path, or the node limit was reached).
	 */
	bool complete{};
	double cost{};
	unsigned expanded{};
};

class astar_search
{
	struct open_entry
	{
		double f, h, g;
		uint32_t node;
	};
	/* The heap keeps the best entry at the front: lowest f, then lowest
	 * h (nearer the goal), then lowest node number.  The order is total,
	 * so the result does not depend on the insertion order.
	 */
	static bool worse(const open_entry &a, const open_entry &b)
	{
		if (a.f != b.f)
			return a.f > b.f;
		if (a.h != b.h)
			return a.h > b.h;
		return a.node > b.node;
	}
	std::vector<double> m_g;
	std::vector<uint32_t> m_parent;
	std::vector<uint8_t> m_parent_side;
	std::vector<uint32_t> m_seen;
	std::vector<uint32_t> m_closed;
	std::vector<open_entry> m_open;
	uint32_t m_generation{};
	void prepare(const std::size_t n)
	{
		if (m_g.size() != n)
		{
			m_g.assign(n, 0);
			m_parent.assign(n, 0);
			m_parent_side.assign(n, NAV_NO_SIDE);
			m_seen.assign(n, 0);
			m_closed.assign(n, 0);
			m_generation = 0;
		}
		if (++m_generation == 0)
		{
			std::ranges::fill(m_seen, 0);
			std::ranges::fill(m_closed, 0);
			m_generation = 1;
		}
		m_open.clear();
	}
public:
	/* Search `graph` from `start` to `goal`.  `passable(from, edge)`
	 * says whether an edge may be taken now; `extra_cost(from, edge)`
	 * (>= 0) adds to its cost.  At most `node_limit` nodes are expanded.
	 * Returns whether `out` holds a path (complete, or partial toward the
	 * goal); false only for invalid nodes.
	 */
	template <typename Passable, typename ExtraCost>
	bool find(const nav_graph &graph, const uint32_t start, const uint32_t goal, Passable &&passable, ExtraCost &&extra_cost, const unsigned node_limit, path_result &out)
	{
		out.steps.clear();
		out.complete = false;
		out.cost = 0;
		out.expanded = 0;
		const auto n{graph.size()};
		if (start >= n || goal >= n)
			return false;
		prepare(n);
		const auto &goal_pos{graph.position(goal)};
		const auto heuristic{[&](const uint32_t node) {
			return distance(graph.position(node), goal_pos);
		}};
		const auto gen{m_generation};
		m_g[start] = 0;
		m_parent[start] = start;
		m_parent_side[start] = NAV_NO_SIDE;
		m_seen[start] = gen;
		const double h0{heuristic(start)};
		m_open.push_back({h0, h0, 0, start});
		uint32_t best{start};
		double best_h{h0};
		double best_g{0};
		uint32_t reached{start};
		bool found{false};
		while (!m_open.empty())
		{
			std::ranges::pop_heap(m_open, worse);
			const auto e{m_open.back()};
			m_open.pop_back();
			if (m_closed[e.node] == gen || e.g != m_g[e.node])
				continue;
			m_closed[e.node] = gen;
			if (e.h < best_h || (e.h == best_h && e.g < best_g))
			{
				best = e.node;
				best_h = e.h;
				best_g = e.g;
			}
			if (e.node == goal)
			{
				found = true;
				reached = goal;
				break;
			}
			if (out.expanded >= node_limit)
				break;
			++out.expanded;
			for (const auto &edge : graph.neighbours(e.node))
			{
				if (edge.to >= n || m_closed[edge.to] == gen)
					continue;
				if (!passable(e.node, edge))
					continue;
				const double g{e.g + static_cast<double>(edge.cost) + extra_cost(e.node, edge)};
				if (m_seen[edge.to] == gen && g >= m_g[edge.to])
					continue;
				m_seen[edge.to] = gen;
				m_g[edge.to] = g;
				m_parent[edge.to] = e.node;
				m_parent_side[edge.to] = edge.side;
				const double h{heuristic(edge.to)};
				m_open.push_back({g + h, h, g, edge.to});
				std::ranges::push_heap(m_open, worse);
			}
		}
		if (!found)
			reached = best;
		out.complete = found;
		out.cost = m_g[reached];
		for (uint32_t node{reached};;)
		{
			out.steps.push_back({node, m_parent_side[node]});
			if (node == start)
				break;
			node = m_parent[node];
		}
		std::ranges::reverse(out.steps);
		out.steps.front().side = NAV_NO_SIDE;
		return true;
	}
};

/* Section 4.7 (stage B3): the path cost and the number of segments from
 * one node to every node within reach, for the collection scores and the
 * bot's map knowledge.  Dijkstra with the same passability and extra cost
 * as the A*, stopped at `max_cost` or after `node_limit` expansions (the
 * nodes left out count as unreachable).
 */
class nav_distances
{
	struct open_entry
	{
		double g;
		uint32_t node;
	};
	static bool worse(const open_entry &a, const open_entry &b)
	{
		if (a.g != b.g)
			return a.g > b.g;
		return a.node > b.node;
	}
	std::vector<double> m_cost;
	std::vector<uint16_t> m_hops;
	std::vector<uint32_t> m_done;
	std::vector<uint32_t> m_seen;
	std::vector<open_entry> m_open;
	uint32_t m_generation{};
	uint32_t m_start{};
	unsigned m_expanded{};
public:
	template <typename Passable, typename ExtraCost>
	void compute(const nav_graph &graph, const uint32_t start, Passable &&passable, ExtraCost &&extra_cost, const double max_cost, const unsigned node_limit)
	{
		const auto n{graph.size()};
		if (m_cost.size() != n)
		{
			m_cost.assign(n, 0);
			m_hops.assign(n, 0);
			m_done.assign(n, 0);
			m_seen.assign(n, 0);
			m_generation = 0;
		}
		if (++m_generation == 0)
		{
			std::ranges::fill(m_done, 0);
			std::ranges::fill(m_seen, 0);
			m_generation = 1;
		}
		m_open.clear();
		m_expanded = 0;
		m_start = start;
		if (start >= n)
			return;
		const auto gen{m_generation};
		m_cost[start] = 0;
		m_hops[start] = 0;
		m_seen[start] = gen;
		m_open.push_back({0, start});
		while (!m_open.empty())
		{
			std::ranges::pop_heap(m_open, worse);
			const auto e{m_open.back()};
			m_open.pop_back();
			if (m_done[e.node] == gen || e.g != m_cost[e.node])
				continue;
			if (m_expanded >= node_limit)
				break;
			m_done[e.node] = gen;
			++m_expanded;
			for (const auto &edge : graph.neighbours(e.node))
			{
				if (edge.to >= n || m_done[edge.to] == gen)
					continue;
				if (!passable(e.node, edge))
					continue;
				const double g{e.g + static_cast<double>(edge.cost) + extra_cost(e.node, edge)};
				if (g > max_cost)
					continue;
				if (m_seen[edge.to] == gen && g >= m_cost[edge.to])
					continue;
				m_seen[edge.to] = gen;
				m_cost[edge.to] = g;
				m_hops[edge.to] = static_cast<uint16_t>(std::min<unsigned>(m_hops[e.node] + 1u, 0xffffu));
				m_open.push_back({g, edge.to});
				std::ranges::push_heap(m_open, worse);
			}
		}
	}
	/* Whether node `n` was reached (its cost is final). */
	[[nodiscard]]
	bool reached(const uint32_t n) const
	{
		return n < m_done.size() && m_generation && m_done[n] == m_generation;
	}
	/* The path cost to `n`, if reached. */
	[[nodiscard]]
	std::optional<double> cost(const uint32_t n) const
	{
		if (!reached(n))
			return std::nullopt;
		return m_cost[n];
	}
	/* The segments on that path, if reached. */
	[[nodiscard]]
	std::optional<unsigned> hops(const uint32_t n) const
	{
		if (!reached(n))
			return std::nullopt;
		return m_hops[n];
	}
	[[nodiscard]]
	uint32_t start() const
	{
		return m_start;
	}
	[[nodiscard]]
	unsigned expanded() const
	{
		return m_expanded;
	}
};

/* Section 4.3, roam: where a bot with nobody to fight flies to.  Of up
 * to `tries` segments drawn with `below(n)` (uniform in [0, n)), the
 * first one at least `min_distance` from `pos` that has a neighbour (a
 * segment nothing connects to cannot be reached), else the furthest
 * drawn; never `from` unless the graph has nothing else.
 */
template <typename Below>
[[nodiscard]]
uint32_t pick_roam_goal(const nav_graph &graph, const uint32_t from, const vec3 &pos, const double min_distance, Below &&below, const unsigned tries = 12)
{
	const auto n{static_cast<uint32_t>(graph.size())};
	if (n < 2)
		return from;
	uint32_t best{from};
	double best_distance{-1};
	for (unsigned i = 0; i < tries; ++i)
	{
		const uint32_t seg{below(n)};
		if (seg >= n || seg == from || graph.neighbours(seg).empty())
			continue;
		const double d{distance(graph.position(seg), pos)};
		if (d >= min_distance)
			return seg;
		if (d > best_distance)
		{
			best = seg;
			best_distance = d;
		}
	}
	return best;
}

/* The furthest of the path points `from` .. `from + lookahead - 1` (not
 * beyond `count - 1`) that `reachable(i)` accepts, tried from the
 * furthest down; `from` if none is (the next point is steered at
 * anyway).
 */
template <typename Reachable>
std::size_t pull_string(const std::size_t from, const std::size_t count, const unsigned lookahead, Reachable &&reachable)
{
	if (from >= count || !lookahead)
		return from;
	const std::size_t last{std::min(count - 1, from + lookahead - 1)};
	for (std::size_t i{last}; i > from; --i)
		if (reachable(i))
			return i;
	return from;
}

/* The path point to steer at next, given the current one: a point is
 * reached when the bot is within `reach` of it, or has passed it (it is
 * beyond the point along the path, within 3 `reach`).  The last point is
 * never passed.
 */
[[nodiscard]]
inline std::size_t advance_along(const std::span<const vec3> points, std::size_t index, const vec3 &pos, const double reach)
{
	while (index + 1 < points.size())
	{
		const auto &p{points[index]};
		const auto rel{pos - p};
		const double d{length(rel)};
		if (d < reach || (d < 3 * reach && dot(rel, points[index + 1] - p) > 0))
			++index;
		else
			break;
	}
	return index;
}

/* The length left to fly: to the current point, then along the path. */
[[nodiscard]]
inline double remaining_length(const std::span<const vec3> points, const std::size_t index, const vec3 &pos)
{
	if (index >= points.size())
		return 0;
	double r{distance(pos, points[index])};
	for (std::size_t i{index}; i + 1 < points.size(); ++i)
		r += distance(points[i], points[i + 1]);
	return r;
}

/* Section 4.3: stuck when the remaining path length did not shrink by
 * `min_progress` within `window_ticks` (2 units per second over 1.5 s at
 * 60 Hz); then `recover_ticks` of recovery manoeuvre, after which the
 * path is planned again; after `max_failures` failures for one goal,
 * give it up.
 *
 * The recovery is timed by `tick_recovery`, called once per tick
 * whatever the bot does, not by `update`, which only runs while the bot
 * follows its path: a recovery must not outlive the moment it was meant
 * for (a fight in the open, where the path is not followed, would
 * otherwise leave it on and the bot flying along the recovery direction
 * into walls).  A new path or combat movement ends it
 * (`restart_window`, `cancel_recovery`).
 */
struct stuck_params
{
	unsigned window_ticks{90};
	double min_progress{3.0};
	unsigned recover_ticks{30};
	unsigned max_failures{3};
};

enum class stuck_event : uint8_t
{
	none,
	/* Start the recovery manoeuvre. */
	stuck,
	/* Too many failures for this goal: choose another. */
	give_up,
};

class stuck_detector
{
	stuck_params m_params;
	double m_best{};
	unsigned m_ticks{};
	unsigned m_recover{};
	unsigned m_failures{};
	bool m_have{};
public:
	constexpr stuck_detector() = default;
	constexpr explicit stuck_detector(const stuck_params &p) :
		m_params{p}
	{
	}
	/* A new goal. */
	void reset()
	{
		m_have = false;
		m_ticks = 0;
		m_recover = 0;
		m_failures = 0;
	}
	/* A new path to the same goal: its remaining length starts anew, and
	 * a recovery under way is over (the new path avoids the stuck edge).
	 */
	void restart_window()
	{
		m_have = false;
		m_ticks = 0;
		m_recover = 0;
	}
	/* Something else moves the bot (combat): no recovery manoeuvre, and
	 * the progress is measured anew when the path is followed again.
	 */
	void cancel_recovery()
	{
		restart_window();
	}
	[[nodiscard]]
	bool recovering() const
	{
		return m_recover != 0;
	}
	[[nodiscard]]
	unsigned failures() const
	{
		return m_failures;
	}
	/* Once per tick, whatever the bot does: true when a recovery has just
	 * run out (plan again, with the stuck edge penalised).
	 */
	bool tick_recovery()
	{
		if (!m_recover || --m_recover)
			return false;
		restart_window();
		return true;
	}
	/* Once per tick while the path is followed, with the remaining path
	 * length.
	 */
	stuck_event update(const double remaining)
	{
		if (m_recover)
			return stuck_event::none;
		if (!m_have || remaining <= m_best - m_params.min_progress)
		{
			m_have = true;
			m_best = remaining;
			m_ticks = 0;
			return stuck_event::none;
		}
		if (++m_ticks < m_params.window_ticks)
			return stuck_event::none;
		if (++m_failures >= m_params.max_failures)
		{
			reset();
			return stuck_event::give_up;
		}
		m_recover = m_params.recover_ticks;
		return stuck_event::stuck;
	}
};

/* Extra costs for edges a bot got stuck on, for its next plans. */
class edge_penalties
{
	struct entry
	{
		uint32_t node{};
		uint8_t side{NAV_NO_SIDE};
		double cost{};
	};
	std::array<entry, 8> m_entries{};
	unsigned m_next{};
public:
	void clear()
	{
		m_entries = {};
		m_next = 0;
	}
	void add(const uint32_t node, const uint8_t side, const double cost)
	{
		for (auto &e : m_entries)
			if (e.side != NAV_NO_SIDE && e.node == node && e.side == side)
			{
				e.cost += cost;
				return;
			}
		m_entries[m_next] = {node, side, cost};
		m_next = (m_next + 1) % m_entries.size();
	}
	[[nodiscard]]
	double cost(const uint32_t node, const uint8_t side) const
	{
		for (const auto &e : m_entries)
			if (e.side != NAV_NO_SIDE && e.node == node && e.side == side)
				return e.cost;
		return 0;
	}
};

}
