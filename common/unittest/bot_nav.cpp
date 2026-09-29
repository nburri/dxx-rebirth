/*
 * This file is part of the DXX-Rebirth project <https://github.com/dxx-rebirth/dxx-rebirth/>.
 * It is copyright by its individual contributors, as recorded in the
 * project's Git history.  See COPYING.txt at the top level for license
 * terms and a link to the Git history.
 */
/*
 * Test of the bots' navigation logic (bot_nav.h,
 * Documentation/multiplayer-bots.md sections 4.3 and 8.1): A* against
 * Dijkstra on grids and random graphs, passability, extra costs, the
 * node limit and partial paths, deterministic tie-breaking, a graph of
 * MAX_SEGMENTS nodes within the time budget, string pulling, path
 * following, stuck recovery and the roam goals.
 *
 * Build and run with SCons:
 *
 *	scons sdl2=1 d1x=0 d2x=1 register_runtime_test_plain_link_targets=1 test-bot-nav
 *	build/common/test-bot-nav
 */

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <algorithm>
#include <functional>
#include <limits>
#include <numbers>
#include <optional>
#include <queue>
#include <random>
#include <vector>

#include "bot_nav.h"

using namespace dcx::bot;

namespace {

void check_failed(const char *const expr, const char *const file, const int line)
{
	std::fprintf(stderr, "%s:%d: check failed: %s\n", file, line, expr);
	std::exit(1);
}

#define CHECK(cond)	do { if (!(cond)) check_failed(#cond, __FILE__, __LINE__); } while (0)

bool near(const double a, const double b, const double eps = 1e-6)
{
	return std::abs(a - b) <= eps * std::max(1.0, std::abs(b));
}

/* A 3D grid of w x h x d nodes 20 units apart, 6-connected like
 * segments; side numbers 0..5 for -x, +x, -y, +y, -z, +z.  Blocked
 * nodes have no edges in or out.
 */
struct grid
{
	unsigned w, h, d;
	std::vector<bool> blocked;
	nav_graph g;
	uint32_t id(const unsigned x, const unsigned y, const unsigned z) const
	{
		return (z * h + y) * w + x;
	}
	grid(const unsigned w, const unsigned h, const unsigned d, const double block_fraction, const uint32_t seed) :
		w{w}, h{h}, d{d}, blocked(w * h * d, false)
	{
		std::minstd_rand rng{seed};
		for (auto &&b : blocked)
			b = (rng() % 1000) < block_fraction * 1000;
		blocked[0] = false;
		blocked.back() = false;
		g.begin(w * h * d);
		for (unsigned z = 0; z < d; ++z)
			for (unsigned y = 0; y < h; ++y)
				for (unsigned x = 0; x < w; ++x)
				{
					const auto n{id(x, y, z)};
					g.set_position(n, {x * 20.0, y * 20.0, z * 20.0});
					if (blocked[n])
						continue;
					const auto add{[&](const int dx, const int dy, const int dz, const uint8_t side) {
						const int nx{static_cast<int>(x) + dx}, ny{static_cast<int>(y) + dy}, nz{static_cast<int>(z) + dz};
						if (nx < 0 || ny < 0 || nz < 0 || nx >= static_cast<int>(w) || ny >= static_cast<int>(h) || nz >= static_cast<int>(d))
							return;
						const auto m{id(nx, ny, nz)};
						if (blocked[m])
							return;
						g.add_edge(n, {m, side, 20.0f});
					}};
					add(-1, 0, 0, 0);
					add(1, 0, 0, 1);
					add(0, -1, 0, 2);
					add(0, 1, 0, 3);
					add(0, 0, -1, 4);
					add(0, 0, 1, 5);
				}
		g.finish();
	}
};

/* Reference shortest path costs. */
template <typename Passable, typename Extra>
std::vector<double> dijkstra(const nav_graph &g, const uint32_t start, Passable &&passable, Extra &&extra)
{
	std::vector<double> dist(g.size(), std::numeric_limits<double>::infinity());
	using item = std::pair<double, uint32_t>;
	std::priority_queue<item, std::vector<item>, std::greater<item>> q;
	dist[start] = 0;
	q.push({0, start});
	while (!q.empty())
	{
		const auto [dd, n]{q.top()};
		q.pop();
		if (dd > dist[n])
			continue;
		for (const auto &e : g.neighbours(n))
		{
			if (!passable(n, e))
				continue;
			const double nd{dd + e.cost + extra(n, e)};
			if (nd < dist[e.to])
			{
				dist[e.to] = nd;
				q.push({nd, e.to});
			}
		}
	}
	return dist;
}

const auto all_passable{[](uint32_t, const nav_edge &) { return true; }};
const auto no_extra{[](uint32_t, const nav_edge &) { return 0.0; }};

/* The path is a chain of edges of the graph from start, each step naming
 * the side it came through, and its cost is the sum of their costs.
 */
template <typename Extra>
void check_path_valid(const nav_graph &g, const path_result &r, const uint32_t start, Extra &&extra)
{
	CHECK(!r.steps.empty());
	CHECK(r.steps.front().node == start);
	CHECK(r.steps.front().side == NAV_NO_SIDE);
	double cost{0};
	for (std::size_t i = 1; i < r.steps.size(); ++i)
	{
		bool found{false};
		for (const auto &e : g.neighbours(r.steps[i - 1].node))
			if (e.to == r.steps[i].node && e.side == r.steps[i].side)
			{
				found = true;
				cost += e.cost + extra(r.steps[i - 1].node, e);
				break;
			}
		CHECK(found);
	}
	CHECK(near(cost, r.cost));
}

void test_grid_against_dijkstra()
{
	astar_search search;
	for (uint32_t seed = 1; seed <= 20; ++seed)
	{
		grid gr{12, 9, 4, 0.25, seed};
		const auto &g{gr.g};
		const auto ref{dijkstra(g, 0, all_passable, no_extra)};
		std::minstd_rand rng{seed * 7};
		for (unsigned k = 0; k < 20; ++k)
		{
			const uint32_t goal{static_cast<uint32_t>(rng() % g.size())};
			path_result r;
			CHECK(search.find(g, 0, goal, all_passable, no_extra, 100000, r));
			if (std::isinf(ref[goal]))
			{
				CHECK(!r.complete);
				continue;
			}
			CHECK(r.complete);
			CHECK(r.steps.back().node == goal);
			CHECK(near(r.cost, ref[goal]));
			check_path_valid(g, r, 0, no_extra);
		}
	}
}

void test_random_graphs()
{
	astar_search search;
	for (uint32_t seed = 1; seed <= 30; ++seed)
	{
		std::minstd_rand rng{seed};
		const unsigned n{50 + static_cast<unsigned>(rng() % 150)};
		nav_graph g;
		g.begin(n);
		std::vector<vec3> pos(n);
		for (auto &p : pos)
			p = {static_cast<double>(rng() % 1000), static_cast<double>(rng() % 1000), static_cast<double>(rng() % 1000)};
		for (uint32_t i = 0; i < n; ++i)
			g.set_position(i, pos[i]);
		for (uint32_t i = 0; i < n; ++i)
		{
			const unsigned deg{1 + static_cast<unsigned>(rng() % 5)};
			for (unsigned k = 0; k < deg; ++k)
			{
				const uint32_t j{static_cast<uint32_t>(rng() % n)};
				if (j == i)
					continue;
				/* Cost at least the straight distance. */
				const double c{distance(pos[i], pos[j]) * (1 + (rng() % 100) / 100.0)};
				g.add_edge(i, {j, static_cast<uint8_t>(k), static_cast<float>(c + 1)});
			}
		}
		g.finish();
		for (unsigned q = 0; q < 10; ++q)
		{
			const uint32_t s{static_cast<uint32_t>(rng() % n)}, t{static_cast<uint32_t>(rng() % n)};
			const auto ref{dijkstra(g, s, all_passable, no_extra)};
			path_result r;
			CHECK(search.find(g, s, t, all_passable, no_extra, 100000, r));
			CHECK(r.complete == !std::isinf(ref[t]));
			if (r.complete)
			{
				/* float edge costs, summed in double. */
				CHECK(near(r.cost, ref[t], 1e-5));
				check_path_valid(g, r, s, no_extra);
			}
		}
	}
}

void test_passable_and_extra_cost()
{
	/* A 10 x 10 x 1 grid; a wall at x = 5 except at y = 9. */
	grid gr{10, 10, 1, 0, 1};
	const auto &g{gr.g};
	const auto wall{[&](const uint32_t from, const nav_edge &e) {
		const auto fx{from % 10}, tx{e.to % 10}, ty{e.to / 10};
		const bool crosses{(fx == 4 && tx == 5) || (fx == 5 && tx == 4)};
		return !crosses || ty == 9;
	}};
	astar_search search;
	path_result r;
	const uint32_t start{gr.id(0, 0, 0)}, goal{gr.id(9, 0, 0)};
	CHECK(search.find(g, start, goal, wall, no_extra, 100000, r));
	CHECK(r.complete);
	const auto ref{dijkstra(g, start, wall, no_extra)};
	CHECK(near(r.cost, ref[goal]));
	/* It must go up to y = 9 and back: 9 + 9 + 9 steps of 20. */
	CHECK(near(r.cost, 27 * 20));
	bool through_gap{false};
	for (const auto &s : r.steps)
		if (s.node == gr.id(5, 9, 0))
			through_gap = true;
	CHECK(through_gap);
	/* Closed completely: partial path to the explored node nearest the
	 * goal (x = 4, y = 0).
	 */
	const auto closed{[&](const uint32_t from, const nav_edge &e) {
		const auto fx{from % 10}, tx{e.to % 10};
		return !((fx == 4 && tx == 5) || (fx == 5 && tx == 4));
	}};
	CHECK(search.find(g, start, goal, closed, no_extra, 100000, r));
	CHECK(!r.complete);
	CHECK(r.steps.back().node == gr.id(4, 0, 0));
	check_path_valid(g, r, start, no_extra);
	/* An extra cost on the direct row makes the path leave it. */
	const auto penalty{[&](const uint32_t from, const nav_edge &e) {
		return (from / 10 == 0 && e.to / 10 == 0 && from == gr.id(3, 0, 0)) ? 1000.0 : 0.0;
	}};
	CHECK(search.find(g, start, goal, all_passable, penalty, 100000, r));
	CHECK(r.complete);
	const auto ref2{dijkstra(g, start, all_passable, penalty)};
	CHECK(near(r.cost, ref2[goal]));
	CHECK(near(r.cost, 11 * 20));
	check_path_valid(g, r, start, penalty);
	/* edge_penalties as the extra cost. */
	edge_penalties pen;
	pen.add(gr.id(3, 0, 0), 1, 500);
	CHECK(pen.cost(gr.id(3, 0, 0), 1) == 500);
	pen.add(gr.id(3, 0, 0), 1, 500);
	CHECK(pen.cost(gr.id(3, 0, 0), 1) == 1000);
	CHECK(pen.cost(gr.id(3, 0, 0), 0) == 0);
	const auto from_pen{[&](const uint32_t from, const nav_edge &e) { return pen.cost(from, e.side); }};
	CHECK(search.find(g, start, goal, all_passable, from_pen, 100000, r));
	CHECK(near(r.cost, 11 * 20));
	for (unsigned i = 0; i < 10; ++i)
		pen.add(100 + i, 0, 1);
	/* The ring of 8 forgot the oldest entries. */
	CHECK(pen.cost(gr.id(3, 0, 0), 1) == 0);
	pen.clear();
	CHECK(pen.cost(109, 0) == 0);
}

void test_node_limit_and_determinism()
{
	grid gr{20, 20, 1, 0, 1};
	const auto &g{gr.g};
	astar_search search;
	path_result r;
	const uint32_t start{gr.id(0, 0, 0)}, goal{gr.id(19, 19, 0)};
	CHECK(search.find(g, start, goal, all_passable, no_extra, 10, r));
	CHECK(!r.complete);
	CHECK(r.expanded == 10);
	check_path_valid(g, r, start, no_extra);
	/* The partial path gets nearer the goal. */
	CHECK(distance(g.position(r.steps.back().node), g.position(goal)) < distance(g.position(start), g.position(goal)));
	/* Many equally short paths: the same one every time, whether the
	 * workspace is fresh or reused.
	 */
	path_result a, b;
	CHECK(search.find(g, start, goal, all_passable, no_extra, 100000, a));
	for (unsigned i = 0; i < 5; ++i)
		CHECK(search.find(g, gr.id(3, 7, 0), gr.id(15, 2, 0), all_passable, no_extra, 100000, r));
	CHECK(search.find(g, start, goal, all_passable, no_extra, 100000, b));
	CHECK(a.complete && b.complete);
	CHECK(a.steps == b.steps);
	astar_search other;
	path_result c;
	CHECK(other.find(g, start, goal, all_passable, no_extra, 100000, c));
	CHECK(a.steps == c.steps);
	/* Start == goal. */
	CHECK(search.find(g, start, start, all_passable, no_extra, 100000, r));
	CHECK(r.complete && r.steps.size() == 1 && r.steps[0].node == start);
	/* Invalid nodes. */
	CHECK(!search.find(g, start, static_cast<uint32_t>(g.size()), all_passable, no_extra, 100000, r));
}

void test_large_graph_budget()
{
	/* 9000 nodes: the size of the largest levels (MAX_SEGMENTS). */
	grid gr{30, 30, 10, 0.15, 3};
	const auto &g{gr.g};
	CHECK(g.size() == 9000);
	astar_search search;
	path_result r;
	std::minstd_rand rng{5};
	unsigned searches{0};
	const auto t0{std::chrono::steady_clock::now()};
	for (unsigned i = 0; i < 50; ++i)
	{
		const uint32_t s{static_cast<uint32_t>(rng() % g.size())}, t{static_cast<uint32_t>(rng() % g.size())};
		CHECK(search.find(g, s, t, all_passable, no_extra, 9000, r));
		++searches;
	}
	const auto elapsed{std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count()};
	const double per_search{elapsed / searches};
	std::printf("test-bot-nav: %u searches on 9000 nodes, %.3f ms each\n", searches, per_search);
	/* Section 3.5 budgets 0.2 - 1 ms; allow for slow, unoptimised test
	 * builds.
	 */
	CHECK(per_search < 20);
	/* Against Dijkstra on this graph as well. */
	const auto ref{dijkstra(g, 0, all_passable, no_extra)};
	for (unsigned i = 0; i < 20; ++i)
	{
		const uint32_t t{static_cast<uint32_t>(rng() % g.size())};
		CHECK(search.find(g, 0, t, all_passable, no_extra, 100000, r));
		CHECK(r.complete == !std::isinf(ref[t]));
		if (r.complete)
			CHECK(near(r.cost, ref[t]));
	}
}

void test_string_pulling_and_following()
{
	/* Points along an L: (0,0,0) (10,0,0) (20,0,0) (20,10,0) (20,20,0). */
	const std::vector<vec3> pts{{0, 0, 0}, {10, 0, 0}, {20, 0, 0}, {20, 10, 0}, {20, 20, 0}};
	/* Visibility: everything up to the corner. */
	const auto visible_to_corner{[&](const std::size_t i) { return i <= 2; }};
	CHECK(pull_string(0, pts.size(), 4, visible_to_corner) == 2);
	/* All visible: at most `lookahead - 1` ahead. */
	CHECK(pull_string(0, pts.size(), 4, [](std::size_t) { return true; }) == 3);
	CHECK(pull_string(2, pts.size(), 4, [](std::size_t) { return true; }) == 4);
	/* Nothing visible: the next point anyway. */
	CHECK(pull_string(1, pts.size(), 4, [](std::size_t) { return false; }) == 1);
	CHECK(pull_string(5, pts.size(), 4, [](std::size_t) { return true; }) == 5);
	/* The furthest point is tried first. */
	std::vector<std::size_t> tried;
	(void)pull_string(0, pts.size(), 4, [&](const std::size_t i) { tried.push_back(i); return false; });
	CHECK((tried == std::vector<std::size_t>{3, 2, 1}));
	/* Reached within `reach`. */
	CHECK(advance_along(pts, 0, {0.5, 0, 0}, 2) == 1);
	/* Passed point 1 (beyond it toward point 2) and near. */
	CHECK(advance_along(pts, 1, {14, 0, 0}, 2) == 2);
	/* Beyond the point but far: not yet (it may be a detour). */
	CHECK(advance_along(pts, 1, {10, 15, 0}, 2) == 1);
	/* Several at once. */
	CHECK(advance_along(pts, 0, {10.5, 0, 0}, 12) == 3);
	/* The last point is never passed. */
	CHECK(advance_along(pts, 4, {20, 20, 0}, 2) == 4);
	CHECK(near(remaining_length(pts, 0, {0, 0, 0}), 40));
	CHECK(near(remaining_length(pts, 3, {20, 5, 0}), 15));
	CHECK(remaining_length(pts, 9, {0, 0, 0}) == 0);
}

void test_stuck_detector()
{
	stuck_detector s{stuck_params{.window_ticks = 90, .min_progress = 3, .recover_ticks = 30, .max_failures = 3}};
	/* Steady progress of 2.5 units per second (above 2): never stuck. */
	double remaining{500};
	for (unsigned t = 0; t < 1200; ++t)
	{
		CHECK(s.update(remaining) == stuck_event::none);
		remaining -= 2.5 / 60;
	}
	/* Stopped: stuck after exactly the window. */
	s.reset();
	unsigned t{0};
	stuck_event ev{stuck_event::none};
	while ((ev = s.update(100)) == stuck_event::none)
		++t;
	CHECK(ev == stuck_event::stuck);
	CHECK(t == 90);
	CHECK(s.recovering());
	CHECK(s.failures() == 1);
	/* Recovery lasts its ticks, then asks for a new plan; it is timed by
	 * tick_recovery alone (the path need not be followed meanwhile), and
	 * update does not count progress while it runs.
	 */
	unsigned rec{1};
	CHECK(s.update(100) == stuck_event::none);
	while (!s.tick_recovery())
		++rec;
	CHECK(rec == 30);
	CHECK(!s.recovering());
	CHECK(!s.tick_recovery());
	/* Second failure, then the third gives the goal up. */
	while ((ev = s.update(100)) == stuck_event::none)
		;
	CHECK(ev == stuck_event::stuck);
	while (!s.tick_recovery())
		;
	while ((ev = s.update(100)) == stuck_event::none)
		;
	CHECK(ev == stuck_event::give_up);
	CHECK(s.failures() == 0);
	/* A new path (a replan to the same segment) or combat movement ends
	 * a recovery at once, keeping the failure count.
	 */
	s.reset();
	while ((ev = s.update(100)) == stuck_event::none)
		;
	CHECK(s.recovering());
	s.restart_window();
	CHECK(!s.recovering());
	CHECK(!s.tick_recovery());
	CHECK(s.failures() == 1);
	while ((ev = s.update(100)) == stuck_event::none)
		;
	CHECK(s.recovering());
	s.cancel_recovery();
	CHECK(!s.recovering());
	CHECK(s.failures() == 2);
	/* Without a path to follow (update never called), a recovery still
	 * expires after its ticks.
	 */
	s.reset();
	while ((ev = s.update(100)) == stuck_event::none)
		;
	for (unsigned k = 1; k < 30; ++k)
		CHECK(!s.tick_recovery());
	CHECK(s.tick_recovery());
	CHECK(!s.recovering());
	/* A slow crawl (1 unit per second) is stuck too. */
	s.reset();
	remaining = 100;
	t = 0;
	while ((ev = s.update(remaining)) == stuck_event::none)
	{
		remaining -= 1.0 / 60;
		++t;
	}
	CHECK(ev == stuck_event::stuck);
	CHECK(t == 90);
}

/* Roam goals (section 4.3): far away, reachable, never the segment the
 * bot is in, spread over the level; and a path to them that A* finds.
 */
void test_roam_goals()
{
	grid gr{12, 4, 12, 0.15, 99};
	std::minstd_rand rng{5};
	const auto below{[&rng](const uint32_t n) -> uint32_t {
		/* uint_fast32_t is uint32_t on Windows: no cast. */
		const uint32_t r = rng() % n;
		return r;
	}};
	astar_search search;
	path_result path;
	const auto open{[](uint32_t, const nav_edge &) { return true; }};
	const auto none{[](uint32_t, const nav_edge &) { return 0.0; }};
	std::vector<unsigned> chosen(gr.g.size(), 0);
	unsigned far{0}, reached{0};
	for (unsigned i = 0; i < 400; ++i)
	{
		uint32_t from;
		do
			from = below(static_cast<uint32_t>(gr.g.size()));
		while (gr.blocked[from]);
		const auto &pos{gr.g.position(from)};
		const uint32_t goal{pick_roam_goal(gr.g, from, pos, 120, below)};
		CHECK(goal != from);
		CHECK(goal < gr.g.size());
		CHECK(!gr.blocked[goal]);
		++chosen[goal];
		far += distance(gr.g.position(goal), pos) >= 120;
		CHECK(search.find(gr.g, from, goal, open, none, 4000, path));
		reached += path.complete;
		/* A real journey: many segments, not the next one. */
		if (path.complete)
			CHECK(path.steps.size() >= 3);
	}
	/* Almost always far; reachable unless the blocks cut it off. */
	CHECK(far > 400 * 95 / 100);
	CHECK(reached > 400 * 80 / 100);
	/* Spread over the level, not a few favourite places. */
	unsigned distinct{0}, most{0};
	for (const auto c : chosen)
	{
		distinct += c != 0;
		most = std::max(most, c);
	}
	CHECK(distinct > 150);
	CHECK(most < 10);
	/* A graph of one node: stays. */
	nav_graph one;
	one.begin(1);
	one.finish();
	CHECK(pick_roam_goal(one, 0, {}, 120, below) == 0);
	/* Everything near: the furthest drawn. */
	grid small{3, 1, 1, 0, 1};
	for (unsigned i = 0; i < 20; ++i)
	{
		const auto g{pick_roam_goal(small.g, 0, small.g.position(0), 1000, below)};
		CHECK(g == 1 || g == 2);
	}
}

/* Stage B3: the path costs to every node within reach equal the
 * reference Dijkstra's, with passability and extra costs; the segment
 * counts are those of a shortest path; the cost bound and the node limit
 * leave nodes out, never a wrong cost in.
 */
void test_nav_distances()
{
	nav_distances nd;
	/* Not computed yet: nothing reached. */
	CHECK(!nd.reached(0));
	for (uint32_t seed = 1; seed <= 20; ++seed)
	{
		grid gr{12, 9, 3, 0.2, seed};
		std::minstd_rand rng{seed};
		const auto extra{[seed](const uint32_t from, const nav_edge &e) {
			return ((from * 7 + e.side + seed) % 5 == 0) ? 15.0 : 0.0;
		}};
		const auto passable{[seed](const uint32_t from, const nav_edge &e) {
			return (from + e.to + seed) % 11 != 0;
		}};
		for (unsigned q = 0; q < 5; ++q)
		{
			uint32_t s{static_cast<uint32_t>(rng() % gr.g.size())};
			if (gr.blocked[s])
				s = 0;
			const auto ref{dijkstra(gr.g, s, passable, extra)};
			nd.compute(gr.g, s, passable, extra, 1e9, 100000);
			CHECK(nd.start() == s);
			for (uint32_t n = 0; n < gr.g.size(); ++n)
			{
				CHECK(nd.reached(n) == !std::isinf(ref[n]));
				if (!nd.reached(n))
				{
					CHECK(!nd.cost(n) && !nd.hops(n));
					continue;
				}
				CHECK(near(*nd.cost(n), ref[n], 1e-5));
				/* Every edge costs at least 20: a path of k segments costs
				 * at least 20 k, and no more than cost / 20 segments.
				 */
				CHECK(*nd.hops(n) * 20.0 <= *nd.cost(n) + 1e-6);
			}
			CHECK(*nd.hops(s) == 0 && *nd.cost(s) == 0);
			/* The bound: exactly the nodes within it (their cost final). */
			nd.compute(gr.g, s, passable, extra, 100, 100000);
			for (uint32_t n = 0; n < gr.g.size(); ++n)
			{
				CHECK(nd.reached(n) == (ref[n] <= 100));
				if (nd.reached(n))
					CHECK(near(*nd.cost(n), ref[n], 1e-5));
			}
			/* The node limit: at most that many, the nearest, exact. */
			nd.compute(gr.g, s, passable, extra, 1e9, 10);
			unsigned reached{0};
			double worst{0};
			for (uint32_t n = 0; n < gr.g.size(); ++n)
				if (nd.reached(n))
				{
					++reached;
					CHECK(near(*nd.cost(n), ref[n], 1e-5));
					worst = std::max(worst, ref[n]);
				}
			CHECK(reached <= 10 && nd.expanded() == reached);
			for (uint32_t n = 0; n < gr.g.size(); ++n)
				if (!nd.reached(n))
					CHECK(ref[n] >= worst);
		}
	}
	/* Hops on a plain grid: the Manhattan distance. */
	grid open{6, 6, 1, 0, 1};
	nd.compute(open.g, open.id(0, 0, 0), all_passable, no_extra, 1e9, 100000);
	for (unsigned y = 0; y < 6; ++y)
		for (unsigned x = 0; x < 6; ++x)
			CHECK(*nd.hops(open.id(x, y, 0)) == x + y);
	/* An invalid start: nothing. */
	nd.compute(open.g, 1000, all_passable, no_extra, 1e9, 100000);
	CHECK(!nd.reached(0));
}


/* Section 9.5: a level like the playtest's, a dense core of small
 * segments (the spawn area) inside a large ring reached by two
 * corridors.  B1's roam (the first of up to 12 uniform draws 120 units
 * away) almost never left the core; exploring (far and unvisited places)
 * goes round the ring.
 */
struct core_and_ring
{
	nav_graph g;
	uint32_t core_nodes{};
	uint32_t ring_first{}, ring_nodes{32};
	core_and_ring()
	{
		constexpr unsigned cw{12}, ch{4};
		constexpr double step{20};
		core_nodes = cw * cw * ch;
		constexpr unsigned corridor{4};
		ring_first = core_nodes + 2 * corridor;
		const uint32_t n{ring_first + ring_nodes};
		std::vector<vec3> pos(n);
		std::vector<std::vector<uint32_t>> adj(n);
		const auto link{[&](const uint32_t a, const uint32_t b) {
			adj[a].push_back(b);
			adj[b].push_back(a);
		}};
		const auto core_id{[&](const unsigned x, const unsigned y, const unsigned z) {
			return (y * cw + z) * cw + x;
		}};
		const double half{(cw - 1) * step / 2};
		for (unsigned y = 0; y < ch; ++y)
			for (unsigned z = 0; z < cw; ++z)
				for (unsigned x = 0; x < cw; ++x)
				{
					const auto id{core_id(x, y, z)};
					pos[id] = {x * step - half, y * step, z * step - half};
					if (x + 1 < cw)
						link(id, core_id(x + 1, y, z));
					if (z + 1 < cw)
						link(id, core_id(x, y, z + 1));
					if (y + 1 < ch)
						link(id, core_id(x, y + 1, z));
				}
		constexpr double radius{400};
		for (uint32_t i = 0; i < ring_nodes; ++i)
		{
			const double a{2 * std::numbers::pi * i / ring_nodes};
			pos[ring_first + i] = {radius * std::cos(a), 0, radius * std::sin(a)};
			link(ring_first + i, ring_first + (i + 1) % ring_nodes);
		}
		/* Two corridors: +x and -x, from the core's edge to the ring. */
		for (unsigned c = 0; c < 2; ++c)
		{
			const double sign{c ? -1.0 : 1.0};
			uint32_t prev{core_id(c ? 0 : cw - 1, 0, cw / 2)};
			for (unsigned k = 0; k < corridor; ++k)
			{
				const uint32_t id{core_nodes + c * corridor + k};
				pos[id] = {sign * (half + (radius - half) * (k + 1) / (corridor + 1)), 0, 0};
				link(prev, id);
				prev = id;
			}
			link(prev, ring_first + (c ? ring_nodes / 2 : 0));
		}
		g.begin(n);
		for (uint32_t i = 0; i < n; ++i)
			g.set_position(i, pos[i]);
		for (uint32_t i = 0; i < n; ++i)
		{
			std::sort(adj[i].begin(), adj[i].end());
			uint8_t side{0};
			for (const auto j : adj[i])
				g.add_edge(i, {j, side++, static_cast<float>(distance(pos[i], pos[j]))});
		}
		g.finish();
	}
	[[nodiscard]]
	bool in_core(const uint32_t n) const
	{
		return n < core_nodes;
	}
};

void test_explore_core_and_ring()
{
	const core_and_ring level;
	const auto open{[](uint32_t, const nav_edge &) { return true; }};
	const auto none{[](uint32_t, const nav_edge &) { return 0.0; }};
	std::minstd_rand rng{11};
	const auto below{[&rng](const uint32_t n) -> uint32_t {
		const uint32_t r = rng() % n;
		return r;
	}};
	/* The path costs of a strategy tick (2500 units, 3000 nodes) reach
	 * the whole ring from the core's centre.
	 */
	nav_distances nd;
	const uint32_t centre{(2 * 12 + 6) * 12 + 6};
	nd.compute(level.g, centre, open, none, 2500, 3000);
	for (uint32_t i = 0; i < level.ring_nodes; ++i)
		CHECK(nd.reached(level.ring_first + i));
	/* B1's roam: mostly the core. */
	unsigned b1_out{0};
	for (unsigned i = 0; i < 400; ++i)
		b1_out += !level.in_core(pick_roam_goal(level.g, centre, level.g.position(centre), 120, below));
	CHECK(b1_out < 400 * 3 / 10);
	/* Exploring: a bot flies 40 roams, from goal to goal, marking what
	 * it passes; time goes by the flight (50 units/s).
	 */
	astar_search search;
	path_result path;
	std::vector<double> visited(level.g.size(), -1);
	double now{0};
	uint32_t at{centre};
	unsigned out{0};
	for (unsigned i = 0; i < 40; ++i)
	{
		nd.compute(level.g, at, open, none, 2500, 3000);
		const auto goal{pick_explore_goal(level.g, at, below, [&nd](const uint32_t n) -> std::optional<double> {
			return nd.cost(n);
		}, [&](const uint32_t n) {
			return visited[n] < 0 ? 1e9 : now - visited[n];
		})};
		CHECK(goal.has_value());
		CHECK(*goal != at);
		CHECK(search.find(level.g, at, *goal, open, none, 4000, path));
		CHECK(path.complete);
		for (const auto &st : path.steps)
		{
			visited[st.node] = now;
			if (&st != &path.steps.front())
				now += 0.5;
		}
		now += *nd.cost(*goal) / 50;
		out += !level.in_core(*goal);
		at = *goal;
	}
	unsigned ring_seen{0};
	for (uint32_t i = 0; i < level.ring_nodes; ++i)
		ring_seen += visited[level.ring_first + i] >= 0;
	std::printf("test-bot-nav: core and ring: B1 roams left the core %u of 400 times; exploring %u of 40, %u of %u ring segments visited\n", b1_out, out, ring_seen, level.ring_nodes);
	CHECK(out >= 40 / 3 && out > b1_out * 40 / 400);
	CHECK(ring_seen >= level.ring_nodes * 3 / 4);
	/* The score: far and long unvisited first; capped. */
	CHECK(explore_score(600, 1e9) > explore_score(300, 1e9));
	CHECK(explore_score(600, 1e9) > explore_score(600, 10));
	CHECK(explore_score(5000, 1e9) == explore_score(EXPLORE_COST_CAP, 1e9));
	CHECK(explore_score(600, 0) > 0);
	/* Section 9.5: a spawn site sealed off (here the ring, its corridors
	 * closed: 32 of 616 segments) is not open; the core is.  The real
	 * case: Schwarzbrenner Outpost's two cells of 9 of 272 segments,
	 * against 243 for the other sites.
	 */
	const auto sealed{[&](const uint32_t from, const nav_edge &e) {
		return level.in_core(from) == level.in_core(e.to) && (from < level.ring_first) == (e.to < level.ring_first);
	}};
	const auto count_from{[&](const uint32_t start) {
		nd.compute(level.g, start, sealed, none, 1e12, static_cast<unsigned>(level.g.size()));
		std::size_t k{0};
		for (uint32_t i = 0; i < level.g.size(); ++i)
			k += nd.reached(i);
		return k;
	}};
	CHECK(count_from(level.ring_first) == level.ring_nodes);
	CHECK(!spawn_site_open(count_from(level.ring_first), level.g.size()));
	CHECK(spawn_site_open(count_from(centre), level.g.size()));
	CHECK(!spawn_site_open(9, 272) && spawn_site_open(243, 272));
	CHECK(spawn_site_open(150, 9000) && !spawn_site_open(149, 9000));
	/* Nothing within reach: none (the caller falls back to B1's roam). */
	CHECK(!pick_explore_goal(level.g, centre, below, [](uint32_t) -> std::optional<double> { return std::nullopt; }, [](uint32_t) { return 0.0; }));
}

/* Section 9.10: where a pursued target probably is.  A corridor along +x
 * (nodes 0-4, 20 units apart) that bends to +y at node 4 (nodes 5-7, a
 * dead end), with a side branch at node 2 to -y (node 8).
 */
void test_pursuit_prediction()
{
	nav_graph g;
	g.begin(9);
	const std::array<vec3, 9> at{{{0, 0, 0}, {20, 0, 0}, {40, 0, 0}, {60, 0, 0}, {80, 0, 0}, {80, 20, 0}, {80, 40, 0}, {80, 60, 0}, {40, -20, 0}}};
	for (uint32_t i = 0; i < at.size(); ++i)
		g.set_position(i, at[i]);
	const std::array<std::pair<uint32_t, uint32_t>, 8> links{{{0, 1}, {1, 2}, {2, 3}, {3, 4}, {4, 5}, {5, 6}, {6, 7}, {2, 8}}};
	for (uint32_t n = 0; n < at.size(); ++n)
		for (const auto &[x, y] : links)
		{
			if (x == n)
				g.add_edge(n, {y, 0, 20.0f});
			else if (y == n)
				g.add_edge(n, {x, 0, 20.0f});
		}
	g.finish();
	const auto all{[](uint32_t, const nav_edge &) { return true; }};
	const vec3 fast{60, 0, 0};
	/* Along the corridor, past the branch. */
	auto r{predict_pursuit(g, 1, at[1], fast, 55, all)};
	CHECK(r.segment == 4 && !r.dead_end);
	CHECK(near(r.point.x, 75) && near(r.point.y, 0));
	r = predict_pursuit(g, 1, at[1], fast, 45, all);
	CHECK(r.segment == 3 && near(r.point.x, 65));
	/* Round the bend: the exit the corridor takes. */
	r = predict_pursuit(g, 1, at[1], fast, 100, all);
	CHECK(r.segment == 6 && near(r.point.x, 80) && near(r.point.y, 40));
	/* To the dead end, not back. */
	r = predict_pursuit(g, 1, at[1], fast, 1000, all);
	CHECK(r.segment == 7 && r.dead_end);
	/* Heading -y at node 2: the branch. */
	r = predict_pursuit(g, 2, at[2], {0, -50, 0}, 30, all);
	CHECK(r.segment == 8 && r.dead_end);
	/* A closed door (3 to 4): the way ends at node 3. */
	const auto door{[](const uint32_t from, const nav_edge &e) { return !(from == 3 && e.to == 4); }};
	r = predict_pursuit(g, 1, at[1], fast, 100, door);
	CHECK(r.segment == 3 && r.dead_end);
	/* Heading back (-x): to node 0, a dead end. */
	r = predict_pursuit(g, 1, at[1], {-40, 0, 0}, 100, all);
	CHECK(r.segment == 0 && r.dead_end);
	/* No velocity: the last known place. */
	r = predict_pursuit(g, 3, {61, 2, 0}, {}, 100, all);
	CHECK(r.segment == 3 && r.point == (vec3{61, 2, 0}) && r.steps == 0);
	/* The walk is bounded: a corridor of 40 nodes. */
	nav_graph line;
	line.begin(40);
	for (uint32_t i = 0; i < 40; ++i)
	{
		line.set_position(i, {i * 20.0, 0, 0});
		if (i)
			line.add_edge(i, {i - 1, 0, 20.0f});
		if (i + 1 < 40)
			line.add_edge(i, {i + 1, 1, 20.0f});
	}
	line.finish();
	r = predict_pursuit(line, 0, {0, 0, 0}, fast, 1e6, all);
	CHECK(r.steps == PREDICT_MAX_STEPS && r.segment == PREDICT_MAX_STEPS && !r.dead_end);
}

/* The PR #47 review: an L-junction.  Segment 0 is the horizontal arm
 * (x -30..10, y -10..10, centre (-10, 0)), segment 1 the vertical arm
 * (x 10..30, y -10..50, centre (20, 20)); outside both is wall.  From a
 * last known place high in the arm, the straight line to the next
 * centre crosses the wall at the inner corner (x < 10, y > 10): the
 * predicted point falls back to a place inside.
 */
void test_pursuit_prediction_l_junction()
{
	nav_graph g;
	g.begin(2);
	g.set_position(0, {-10, 0, 0});
	g.set_position(1, {20, 20, 0});
	g.add_edge(0, {1, 0, 36.0f});
	g.add_edge(1, {0, 1, 36.0f});
	g.finish();
	const auto all{[](uint32_t, const nav_edge &) { return true; }};
	const auto inside{[](const vec3 &p) {
		return (p.x >= -30 && p.x <= 10 && p.y >= -10 && p.y <= 10) || (p.x >= 10 && p.x <= 30 && p.y >= -10 && p.y <= 50);
	}};
	const auto clear{[&inside](uint32_t, const vec3 &a, const vec3 &b) {
		for (unsigned i = 0; i <= 64; ++i)
			if (!inside(a + (b - a) * (i / 64.0)))
				return false;
		return true;
	}};
	const vec3 high{-25, 8, 0};
	const vec3 east{60, 0, 0};
	/* Without the check: a point inside the wall (the bug). */
	auto r{predict_pursuit(g, 0, high, east, 20, all)};
	CHECK(r.segment == 0 && !inside(r.point));
	/* Short of half the leg: the last known place. */
	r = predict_pursuit(g, 0, high, east, 20, all, clear);
	CHECK(r.segment == 0 && r.point == high && inside(r.point));
	/* Past half the leg: the point lies in the arm, but its line crosses
	 * the corner: the arm's centre.
	 */
	r = predict_pursuit(g, 0, high, east, 40, all, clear);
	CHECK(r.segment == 1 && r.point == (vec3{20, 20, 0}));
	/* Low in the arm the line is open: the point along it, unchanged. */
	const vec3 low{-25, -5, 0};
	const auto open{predict_pursuit(g, 0, low, east, 20, all)};
	r = predict_pursuit(g, 0, low, east, 20, all, clear);
	CHECK(inside(open.point) && r.point == open.point && r.segment == open.segment);
}
}

int main()
{
	test_pursuit_prediction();
	test_pursuit_prediction_l_junction();
	test_grid_against_dijkstra();
	test_random_graphs();
	test_passable_and_extra_cost();
	test_node_limit_and_determinism();
	test_large_graph_budget();
	test_string_pulling_and_following();
	test_stuck_detector();
	test_roam_goals();
	test_nav_distances();
	test_explore_core_and_ring();
	std::puts("test-bot-nav: all checks passed");
	return 0;
}
