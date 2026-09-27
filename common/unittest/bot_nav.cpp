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
 * following and stuck recovery.
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
#include <functional>
#include <limits>
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
	/* Recovery lasts its ticks, then asks for a new plan. */
	unsigned rec{1};
	while ((ev = s.update(100)) == stuck_event::none)
		++rec;
	CHECK(ev == stuck_event::recovered);
	CHECK(rec == 30);
	CHECK(!s.recovering());
	/* Second failure, then the third gives the goal up. */
	while ((ev = s.update(100)) == stuck_event::none)
		;
	CHECK(ev == stuck_event::stuck);
	while ((ev = s.update(100)) == stuck_event::none)
		;
	CHECK(ev == stuck_event::recovered);
	while ((ev = s.update(100)) == stuck_event::none)
		;
	CHECK(ev == stuck_event::give_up);
	CHECK(s.failures() == 0);
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

}

int main()
{
	test_grid_against_dijkstra();
	test_random_graphs();
	test_passable_and_extra_cost();
	test_node_limit_and_determinism();
	test_large_graph_budget();
	test_string_pulling_and_following();
	test_stuck_detector();
	std::puts("test-bot-nav: all checks passed");
	return 0;
}
