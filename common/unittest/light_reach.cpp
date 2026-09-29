/*
 * This file is part of the DXX-Rebirth project <https://github.com/dxx-rebirth/dxx-rebirth/>.
 * It is copyright by its individual contributors, as recorded in the
 * project's Git history.  See COPYING.txt at the top level for license
 * terms and a link to the Git history.
 */
/*
 * Test of the walk over the segments a dynamic light reaches
 * (light_reach.h, lighting.cpp) on synthetic levels made of cube
 * segments: a light does not pass a solid wall or a closed door, passes
 * an open side, stops at its radius, and in an open room still reaches
 * every vertex within its radius (the look of open rooms is unchanged),
 * also when the walk runs out of budget or starts outside the rendered
 * segments.  It ends with a benchmark of the walk in a big open room.
 *
 * Distances are those of the game: fixed-point coordinates and the
 * quick magnitude of vm_vec_dist_quick.
 *
 * Build and run with SCons:
 *
 *	scons sdl2=1 d1x=0 d2x=1 register_runtime_test_plain_link_targets=1 test-light-reach
 *	build/common/test-light-reach
 *
 * or directly:
 *
 *	g++ -std=gnu++23 -O2 -Wall -Wextra -Icommon/main common/unittest/light_reach.cpp -o test-light-reach
 */

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <vector>

#include "light_reach.h"

namespace {

void check_failed(const char *const expr, const char *const file, const int line)
{
	std::fprintf(stderr, "%s:%d: check failed: %s\n", file, line, expr);
	std::exit(1);
}

#define CHECK(cond)	do { if (!(cond)) check_failed(#cond, __FILE__, __LINE__); } while (0)

using fix = int32_t;
constexpr fix unit{1 << 16};
constexpr fix cell{20 * unit};
constexpr int no_child{-1};

struct vec3
{
	fix x, y, z;
};

/* As lighting.cpp: the quick magnitude of vm_vec_mag_quick on
 * non-negative per-axis distances, in 64 bits.
 */
int64_t quick_magnitude(int64_t a, int64_t b, int64_t c)
{
	if (a < b)
		std::swap(a, b);
	if (b < c)
	{
		std::swap(b, c);
		if (a < b)
			std::swap(a, b);
	}
	const int64_t bc{(b >> 2) + (c >> 3)};
	return a + bc + (bc >> 1);
}

int64_t distance(const vec3 &a, const vec3 &b)
{
	return quick_magnitude(std::abs(int64_t{a.x} - b.x), std::abs(int64_t{a.y} - b.y), std::abs(int64_t{a.z} - b.z));
}

struct box
{
	vec3 lo, hi;
	void include(const vec3 &v)
	{
		lo = {std::min(lo.x, v.x), std::min(lo.y, v.y), std::min(lo.z, v.z)};
		hi = {std::max(hi.x, v.x), std::max(hi.y, v.y), std::max(hi.z, v.z)};
	}
};

/* Lower bound of the distance from p to any point of b. */
int64_t distance(const vec3 &p, const box &b)
{
	const auto axis = [](const fix c, const fix lo, const fix hi) -> int64_t {
		return c < lo ? int64_t{lo} - c : (c > hi ? int64_t{c} - hi : 0);
	};
	return quick_magnitude(axis(p.x, b.lo.x, b.hi.x), axis(p.y, b.lo.y, b.hi.y), axis(p.z, b.lo.z, b.hi.z));
}

/* Sides: 0 -x, 1 +x, 2 -y, 3 +y, 4 -z, 5 +z.  Corner (dx, dy, dz) is
 * vertex dx + 2 dy + 4 dz.
 */
constexpr std::array<std::array<unsigned, 4>, 6> side_to_verts{{
	{{0, 2, 4, 6}}, {{1, 3, 5, 7}},
	{{0, 1, 4, 5}}, {{2, 3, 6, 7}},
	{{0, 1, 2, 3}}, {{4, 5, 6, 7}},
}};

struct segment
{
	std::array<int, 6> children;
	std::array<bool, 6> lets_light_through;
	std::array<unsigned, 8> verts;
};

struct level
{
	std::vector<segment> segments;
	std::vector<vec3> vertices;
	std::vector<uint8_t> rendered;
};

/* A room of nx * ny * nz open cells at cell position (ox, oy, oz), with
 * its own vertices (rooms are not joined).  Returns the index of its
 * first segment; cell (i, j, k) is first + i + nx (j + ny k).
 */
unsigned add_room(level &l, const int ox, const int oy, const int oz, const unsigned nx, const unsigned ny, const unsigned nz)
{
	const unsigned first_vertex(l.vertices.size());
	const auto vertex_index = [=](const unsigned i, const unsigned j, const unsigned k) {
		return first_vertex + i + (nx + 1) * (j + (ny + 1) * k);
	};
	for (unsigned k{0}; k <= nz; ++k)
		for (unsigned j{0}; j <= ny; ++j)
			for (unsigned i{0}; i <= nx; ++i)
				l.vertices.push_back({(ox + static_cast<int>(i)) * cell, (oy + static_cast<int>(j)) * cell, (oz + static_cast<int>(k)) * cell});
	const unsigned first(l.segments.size());
	const auto segment_index = [=](const unsigned i, const unsigned j, const unsigned k) {
		return static_cast<int>(first + i + nx * (j + ny * k));
	};
	for (unsigned k{0}; k < nz; ++k)
		for (unsigned j{0}; j < ny; ++j)
			for (unsigned i{0}; i < nx; ++i)
			{
				segment s;
				for (unsigned c{0}; c < 8; ++c)
					s.verts[c] = vertex_index(i + (c & 1), j + ((c >> 1) & 1), k + ((c >> 2) & 1));
				s.children = {{
					i ? segment_index(i - 1, j, k) : no_child,
					i + 1 < nx ? segment_index(i + 1, j, k) : no_child,
					j ? segment_index(i, j - 1, k) : no_child,
					j + 1 < ny ? segment_index(i, j + 1, k) : no_child,
					k ? segment_index(i, j, k - 1) : no_child,
					k + 1 < nz ? segment_index(i, j, k + 1) : no_child,
				}};
				for (unsigned side{0}; side < 6; ++side)
					s.lets_light_through[side] = s.children[side] != no_child;
				l.segments.push_back(s);
				l.rendered.push_back(true);
			}
	return first;
}

/* Joins side `side_a` of a with the opposite side of b (a door). */
void join(level &l, const unsigned a, const unsigned side_a, const unsigned b, const bool open)
{
	const unsigned side_b{side_a ^ 1};
	l.segments[a].children[side_a] = static_cast<int>(b);
	l.segments[b].children[side_b] = static_cast<int>(a);
	l.segments[a].lets_light_through[side_a] = open;
	l.segments[b].lets_light_through[side_b] = open;
}

constexpr std::size_t max_segments{16384};
constexpr std::size_t max_vertices{16384};

struct walk_state
{
	dcx::generation_marks<max_segments> discovered;
	dcx::generation_marks<max_vertices> vertices;
	std::vector<dcx::light_reach_entry<unsigned>> queue;
	std::array<unsigned, max_segments> bfs_queue;
};

/* The game's adapter (lighting.cpp) on the synthetic level. */
struct graph
{
	const level &l;
	const vec3 light;
	const fix reach;
	walk_state &state;
	bool rendered(const unsigned s) const
	{
		return l.rendered[s];
	}
	bool discover(const unsigned s)
	{
		return state.discovered.set(s);
	}
	fix distance(const unsigned s) const
	{
		const auto &seg{l.segments[s]};
		box b{l.vertices[seg.verts[0]], l.vertices[seg.verts[0]]};
		for (const auto v : seg.verts)
			b.include(l.vertices[v]);
		return static_cast<fix>(std::min<int64_t>(::distance(light, b), reach));
	}
	void process(const unsigned s)
	{
		for (const auto v : l.segments[s].verts)
			state.vertices.set(v);
	}
	template <typename F>
	void for_each_lit_child(const unsigned s, F &&f) const
	{
		const auto &seg{l.segments[s]};
		/* As lighting.cpp: each vertex fetched once. */
		std::array<const vec3 *, 8> p;
		for (unsigned i{0}; i < 8; ++i)
			p[i] = &l.vertices[seg.verts[i]];
		for (unsigned side{0}; side < 6; ++side)
		{
			const auto child{seg.children[side]};
			if (child == no_child || state.discovered.test(static_cast<unsigned>(child)))
				continue;
			if (!seg.lets_light_through[side])
				continue;
			const auto &sv{side_to_verts[side]};
			box portal{*p[sv[0]], *p[sv[0]]};
			for (const auto i : sv)
				portal.include(*p[i]);
			const auto d{::distance(light, portal)};
			if (d >= reach)
				continue;
			f(static_cast<unsigned>(child), static_cast<fix>(d));
		}
	}
	/* The walk before the budget: breadth-first over every segment
	 * within reach, rendered or not.
	 */
	std::size_t walk_unbounded(const unsigned start)
	{
		std::size_t head{0}, tail{0};
		state.discovered.set(start);
		for (const auto v : l.segments[start].verts)
			state.vertices.set(v);
		state.bfs_queue[tail++] = start;
		while (head != tail)
		{
			const unsigned s{state.bfs_queue[head++]};
			for_each_lit_child(s, [this, &tail](const unsigned child, fix) {
				if (!state.discovered.set(child))
					return;
				for (const auto v : l.segments[child].verts)
					state.vertices.set(v);
				state.bfs_queue[tail++] = child;
			});
		}
		return tail;
	}
};

constexpr dcx::light_reach_limits unlimited{max_segments, 255};
/* As lighting.cpp. */
constexpr dcx::light_reach_limits game_budget{32, 3};

struct walker
{
	walk_state state;
	fix complete{0};
	std::size_t processed{0};
	void reset()
	{
		state.discovered.next();
		state.vertices.next();
	}
	void walk(const level &l, const unsigned start, const vec3 &light, const fix reach, const dcx::light_reach_limits limits = unlimited)
	{
		reset();
		state.queue.resize(dcx::light_reach_queue_size(limits.max_segments));
		graph g{l, light, reach, state};
		complete = dcx::walk_light_reach(g, start, reach, state.queue.data(), limits, &processed);
		CHECK(complete <= reach);
		CHECK(processed <= limits.max_segments);
	}
	/* The rendered vertices apply_light lights: within the radius, and
	 * reached or beyond what the walk checked.
	 */
	std::vector<unsigned> lit(const level &l, const vec3 &light, const fix reach) const
	{
		std::vector<bool> rendered_vertex(l.vertices.size());
		for (unsigned s{0}; s < l.segments.size(); ++s)
			if (l.rendered[s])
				for (const auto v : l.segments[s].verts)
					rendered_vertex[v] = true;
		std::vector<unsigned> r;
		for (unsigned v{0}; v < l.vertices.size(); ++v)
		{
			if (!rendered_vertex[v])
				continue;
			const auto d{distance(light, l.vertices[v])};
			if (d < reach && (d >= complete || state.vertices.test(v)))
				r.push_back(v);
		}
		return r;
	}
};

unsigned count_within(const level &l, const vec3 &light, const fix reach, const unsigned first_vertex, const unsigned end_vertex)
{
	std::vector<bool> rendered_vertex(l.vertices.size());
	for (unsigned s{0}; s < l.segments.size(); ++s)
		if (l.rendered[s])
			for (const auto v : l.segments[s].verts)
				rendered_vertex[v] = true;
	unsigned n{0};
	for (unsigned v{first_vertex}; v < end_vertex; ++v)
		n += rendered_vertex[v] && distance(light, l.vertices[v]) < reach;
	return n;
}

unsigned cell_index(const unsigned n, const vec3 &p)
{
	const auto c = [](const fix x) {
		return static_cast<unsigned>(x / cell);
	};
	return c(p.x) + n * (c(p.y) + n * c(p.z));
}

/* Two rooms side by side, the wall between them solid, with a closed
 * door or open: shots near the wall light the other room only through an
 * open door.  Also with the game's budget, and with the light's own
 * segment and its neighbours not rendered (a shot just behind the
 * viewer).
 */
void test_wall_between_rooms(walker &w)
{
	for (const bool hide_start : {false, true})
	for (const int door : {0, 1, 2})	/* none, closed, open */
	for (const auto limits : {unlimited, game_budget})
	{
		level l;
		const unsigned a{add_room(l, 0, 0, 0, 3, 3, 3)};
		const unsigned a_vertices_end(l.vertices.size());
		/* Room b starts one cell further: the wall between is one cell thick. */
		const unsigned b{add_room(l, 4, 0, 0, 3, 3, 3)};
		const unsigned b_vertices_end(l.vertices.size());
		if (door)
		{
			/* A one-cell tunnel through the wall at (3, 1, 1). */
			const unsigned t{add_room(l, 3, 1, 1, 1, 1, 1)};
			join(l, a + 2 + 3 * (1 + 3 * 1), 1, t, true);
			join(l, t, 1, b + 0 + 3 * (1 + 3 * 1), door == 2);
		}
		/* A shot in room a, near the wall at x = 60. */
		const vec3 light{55 * unit, 30 * unit, 30 * unit};
		const fix reach{40 * unit};
		const unsigned start{a + 2 + 3 * (1 + 3 * 1)};
		if (hide_start)
		{
			l.rendered[start] = false;
			l.rendered[start - 1] = false;
		}
		w.walk(l, start, light, reach, limits);
		CHECK(w.processed > 0);
		/* The budget is enough behind a wall: the walk is complete. */
		if (door != 2)
			CHECK(w.complete == reach);
		const auto lit{w.lit(l, light, reach)};
		unsigned lit_a{0}, lit_b{0};
		for (const auto v : lit)
		{
			lit_a += v < a_vertices_end;
			lit_b += v >= a_vertices_end && v < b_vertices_end;
		}
		/* Without occlusion, room b's wall is lit. */
		CHECK(count_within(l, light, reach, a_vertices_end, b_vertices_end) > 0);
		/* Room a is lit as before. */
		CHECK(lit_a == count_within(l, light, reach, 0, a_vertices_end));
		if (door == 2)
			CHECK(lit_b > 0);
		else
			CHECK(lit_b == 0);
	}
}

/* In an open room, every rendered vertex within the radius is lit,
 * wherever the light is: the look of open rooms does not change.  Also
 * when the budget stops the walk early, and when only part of the room
 * is rendered.
 */
void test_open_room_unchanged(walker &w)
{
	constexpr unsigned n{12};
	for (const bool half_rendered : {false, true})
	for (const auto limits : {unlimited, game_budget, dcx::light_reach_limits{8, 1}, dcx::light_reach_limits{1, 0}})
	{
		level l;
		add_room(l, 0, 0, 0, n, n, n);
		if (half_rendered)
			/* The viewer looks at +x from the middle of the room. */
			for (unsigned s{0}; s < l.segments.size(); ++s)
				l.rendered[s] = s % n >= n / 2;
		std::minstd_rand rng{12345};
		std::uniform_int_distribution<fix> coord{unit / 64, static_cast<fix>(n) * cell - unit / 64};
		std::uniform_int_distribution<fix> radius{5 * unit, 160 * unit};
		for (unsigned iter{0}; iter < 1000; ++iter)
		{
			const vec3 light{coord(rng), coord(rng), coord(rng)};
			const fix reach{radius(rng)};
			w.walk(l, cell_index(n, light), light, reach, limits);
			CHECK(w.lit(l, light, reach).size() == count_within(l, light, reach, 0, l.vertices.size()));
		}
	}
}

/* A long corridor: the walk stops at the radius, or at its budget. */
void test_radius_bounds_walk(walker &w)
{
	level l;
	add_room(l, 0, 0, 0, 200, 1, 1);
	const vec3 light{10 * unit, 10 * unit, 10 * unit};
	/* Sides at x = 20, 40, 60 are within 50; the one at 60 is not. */
	w.walk(l, 0, light, 50 * unit);
	CHECK(w.processed == 3);
	CHECK(w.complete == 50 * unit);
	w.walk(l, 0, light, 5 * unit);
	CHECK(w.processed == 1);
	/* The budget bounds the walk; beyond, the old rule applies. */
	w.walk(l, 0, light, 10000 * unit, dcx::light_reach_limits{7, 0});
	CHECK(w.processed == 7);
	CHECK(w.complete == 130 * unit);
	w.walk(l, 0, light, 10000 * unit);
	CHECK(w.processed == 200);
	/* Segments not rendered are passed only in short runs. */
	for (unsigned s{3}; s < 200; ++s)
		l.rendered[s] = false;
	w.walk(l, 0, light, 10000 * unit, dcx::light_reach_limits{100, 2});
	CHECK(w.processed == 5);
	CHECK(w.complete == 90 * unit);
	/* A light in a segment that is not rendered, with no hops allowed:
	 * nothing is checked, the old rule applies.
	 */
	w.walk(l, 199, light, 50 * unit, dcx::light_reach_limits{100, 0});
	CHECK(w.processed == 0);
	CHECK(w.complete == 0);
}

/* The marks are cleared at every generation, also across the wrap. */
void test_generation_marks()
{
	dcx::generation_marks<4> m;
	CHECK(m.set(1));
	CHECK(!m.set(1));
	CHECK(m.test(1));
	m.next();
	CHECK(!m.test(1));
	m.set_generation_for_test(UINT32_MAX - 1);
	CHECK(m.set(2));
	m.next();	/* UINT32_MAX */
	CHECK(!m.test(2));
	CHECK(m.set(3));
	m.next();	/* wraps: all cleared, generation 1 */
	CHECK(!m.test(3));
	CHECK(!m.test(2));
	for (unsigned i{0}; i < 4; ++i)
		CHECK(!m.test(i));
	CHECK(m.set(0));
	CHECK(m.test(0));
}

/* Cost of one lighting pass: 50 lights of radius 160 (the ship's glow,
 * big fireballs) in a big open room of 20-unit cubes, before (every
 * segment within reach) and with the game's budget.
 */
void benchmark()
{
	static walker w;
	constexpr unsigned n{24};
	constexpr unsigned n_lights{50};
	/* The walk before the budget is slow: fewer passes. */
	constexpr unsigned passes_before{20}, passes{500};
	level l;
	add_room(l, 0, 0, 0, n, n, n);
	std::minstd_rand rng{4242};
	std::uniform_int_distribution<fix> coord{unit, static_cast<fix>(n) * cell - unit};
	std::array<vec3, n_lights> lights;
	for (auto &p : lights)
		p = {coord(rng), coord(rng), coord(rng)};
	const fix reach{160 * unit};
	using clock = std::chrono::steady_clock;
	const auto per_pass_ms = [](const clock::duration d, const unsigned n) {
		return std::chrono::duration<double, std::milli>(d).count() / n;
	};
	std::size_t visited_before{0}, visited_after{0};
	auto t0{clock::now()};
	for (unsigned pass{0}; pass < passes_before; ++pass)
		for (const auto &p : lights)
		{
			w.reset();
			graph g{l, p, reach, w.state};
			visited_before += g.walk_unbounded(cell_index(n, p));
		}
	const auto before{per_pass_ms(clock::now() - t0, passes_before)};
	w.state.queue.resize(dcx::light_reach_queue_size(game_budget.max_segments));
	t0 = clock::now();
	for (unsigned pass{0}; pass < passes; ++pass)
		for (const auto &p : lights)
		{
			w.reset();
			graph g{l, p, reach, w.state};
			std::size_t processed;
			dcx::walk_light_reach(g, cell_index(n, p), reach, w.state.queue.data(), game_budget, &processed);
			visited_after += processed;
		}
	const auto after{per_pass_ms(clock::now() - t0, passes)};
	std::printf("test-light-reach: %u lights, radius 160, %ux%ux%u room of 20-unit cubes: before %.3f ms/pass (%zu segments/light), after %.3f ms/pass (%zu segments/light)\n",
		n_lights, n, n, n,
		before, visited_before / (passes_before * n_lights),
		after, visited_after / (passes * n_lights));
	CHECK(visited_after <= passes * n_lights * game_budget.max_segments);
}

}

int main()
{
	static walker w;
	test_generation_marks();
	test_wall_between_rooms(w);
	test_open_room_unchanged(w);
	test_radius_bounds_walk(w);
	benchmark();
	std::printf("test-light-reach: all checks passed\n");
	return 0;
}
