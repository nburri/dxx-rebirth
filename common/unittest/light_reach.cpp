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
 * segments.
 *
 * The light is stored per segment corner (lighting.h): two rooms on
 * either side of a wall share the wall's vertices, and light stored per
 * vertex would reach the faces of both.  The corner model of apply_light
 * is checked against Descent's per-vertex model: the room behind a wall
 * that shares its vertices gets nothing, the lit room gets the same
 * values, and in an open room segments sharing a vertex get the same
 * value for it (no seams).  It ends with a benchmark of a lighting pass
 * (walk and corners) in a big open room.
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
#include <span>
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

/* As apply_light: fixdiv(emission, max(dist, MIN_LIGHT_DIST)). */
constexpr fix emission{4 * unit};
fix light_at(const int64_t d)
{
	const int64_t scale{std::max<int64_t>(d, 4 * unit)};
	return static_cast<fix>((int64_t{emission} << 16) / scale);
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
	std::vector<unsigned> segments;
	std::array<unsigned, max_segments> bfs_queue;
};

/* The game's adapter (lighting.cpp) on the synthetic level. */
struct graph
{
	const level &l;
	const vec3 light;
	const fix reach;
	walk_state &state;
	/* The benchmark does not mark vertices: the game does not. */
	bool mark_vertices{true};
	bool rendered(const unsigned s) const
	{
		return l.rendered[s];
	}
	void discover(const unsigned s)
	{
		state.discovered.set(s);
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
		if (mark_vertices)
			for (const auto v : l.segments[s].verts)
				state.vertices.set(v);
	}
	template <typename F>
	void for_each_lit_child(const unsigned s, F &&f) const
	{
		const auto &seg{l.segments[s]};
		for (unsigned side{0}; side < 6; ++side)
		{
			const auto child{seg.children[side]};
			if (child == no_child || state.discovered.test(static_cast<unsigned>(child)))
				continue;
			if (!seg.lets_light_through[side])
				continue;
			f(static_cast<unsigned>(child));
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
			for_each_lit_child(s, [this, &tail](const unsigned child) {
				if (distance(child) >= reach || !state.discovered.set(child))
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
/* A light's limits within the game's bounds (lighting.cpp). */
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
		state.segments.resize(limits.max_segments);
		graph g{l, light, reach, state};
		complete = dcx::walk_light_reach(g, start, reach, state.queue.data(), state.segments.data(), limits, &processed);
		CHECK(complete <= reach);
		CHECK(processed <= limits.max_segments);
	}
	/* As apply_light: the corners of the processed segments that are
	 * rendered, nearer than the distance the walk covered, get the light
	 * Descent gave their vertex.
	 */
	void light_corners(const level &l, const vec3 &light, std::vector<std::array<fix, 8>> &corners) const
	{
		corners.assign(l.segments.size(), {});
		for (std::size_t i{0}; i != processed; ++i)
		{
			const auto s{state.segments[i]};
			if (!l.rendered[s])
				continue;
			for (unsigned c{0}; c < 8; ++c)
			{
				const auto d{distance(light, l.vertices[l.segments[s].verts[c]])};
				if (d < complete)
					corners[s][c] += light_at(d);
			}
		}
	}
	/* The rendered vertices within the radius that are reached, or
	 * beyond what the walk covered (the property of the walk that
	 * light_corners relies on: every vertex nearer than `complete` in
	 * an open room is reached).
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

/* The per-vertex model of Descent: every rendered vertex within reach
 * gets the light, whatever lies between.
 */
std::vector<fix> vertex_light(const level &l, const vec3 &light, const fix reach)
{
	std::vector<fix> r(l.vertices.size());
	for (unsigned s{0}; s < l.segments.size(); ++s)
		if (l.rendered[s])
			for (const auto v : l.segments[s].verts)
			{
				const auto d{distance(light, l.vertices[v])};
				r[v] = d < reach ? light_at(d) : 0;
			}
	return r;
}

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

/* In an open room, every rendered vertex within the radius is reached
 * or beyond what the walk covered, wherever the light is.  Also when the
 * budget stops the walk early, and when only part of the room is
 * rendered.
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

/* Two rooms on either side of a solid wall (or a closed door) that
 * share the wall's vertices, as rooms next to each other usually do.
 * Stored per vertex, a light near the wall reached the faces of the far
 * room that use the wall's vertices; stored per corner, the far room gets
 * nothing and the near room gets what it got before.  With the wall
 * open, both rooms get what they got before (within the distance the
 * walk covered).
 */
void test_wall_shares_vertices(walker &w)
{
	constexpr unsigned nx{6}, ny{3}, nz{3};
	const auto index = [](const unsigned i, const unsigned j, const unsigned k) {
		return i + nx * (j + ny * k);
	};
	for (const bool wall : {true, false})
	for (const auto limits : {unlimited, game_budget})
	for (const vec3 light : {vec3{55 * unit, 30 * unit, 30 * unit}, vec3{59 * unit, 5 * unit, 55 * unit}, vec3{45 * unit, 20 * unit, 40 * unit}})
	{
		level l;
		add_room(l, 0, 0, 0, nx, ny, nz);
		/* The wall at x = 60, between cells i = 2 and i = 3. */
		if (wall)
			for (unsigned k{0}; k < nz; ++k)
				for (unsigned j{0}; j < ny; ++j)
				{
					l.segments[index(2, j, k)].lets_light_through[1] = false;
					l.segments[index(3, j, k)].lets_light_through[0] = false;
				}
		const fix reach{40 * unit};
		const unsigned light_segment{index(static_cast<unsigned>(light.x / cell), static_cast<unsigned>(light.y / cell), static_cast<unsigned>(light.z / cell))};
		w.walk(l, light_segment, light, reach, limits);
		/* Behind the wall, the budget is enough.  Open, the two rooms
		 * may exceed it: nothing is lit beyond what the walk covered.
		 */
		if (wall || limits.max_segments == unlimited.max_segments)
			CHECK(w.complete == reach);
		std::vector<std::array<fix, 8>> corners;
		w.light_corners(l, light, corners);
		const auto per_vertex{vertex_light(l, light, reach)};
		unsigned far_lit_per_vertex{0}, near_lit{0};
		for (unsigned s{0}; s < l.segments.size(); ++s)
		{
			const bool far{s % nx >= 3};
			for (unsigned c{0}; c < 8; ++c)
			{
				const auto v{l.segments[s].verts[c]};
				const auto old{distance(light, l.vertices[v]) < w.complete ? per_vertex[v] : 0};
				if (far && wall)
				{
					far_lit_per_vertex += old != 0;
					CHECK(corners[s][c] == 0);
				}
				else
				{
					near_lit += old != 0;
					CHECK(corners[s][c] == old);
				}
			}
		}
		CHECK(near_lit > 0);
		/* Per vertex, the far room's faces on the wall were lit. */
		if (wall)
			CHECK(far_lit_per_vertex > 0);
	}
}

/* In an open room, a corner gets the value Descent gave its vertex if it
 * is nearer than the distance the walk covered, and nothing beyond (no
 * wall-ignoring fallback): the segments that share a vertex all get the
 * same value for it, so faces show no seams.  With no budget, that is
 * every vertex within the radius.
 */
void test_open_room_corners(walker &w)
{
	constexpr unsigned n{12};
	for (const bool half_rendered : {false, true})
	for (const auto limits : {unlimited, game_budget, dcx::light_reach_limits{8, 1}})
	{
		level l;
		add_room(l, 0, 0, 0, n, n, n);
		if (half_rendered)
			for (unsigned s{0}; s < l.segments.size(); ++s)
				l.rendered[s] = s % n >= n / 2;
		std::minstd_rand rng{777};
		std::uniform_int_distribution<fix> coord{unit / 64, static_cast<fix>(n) * cell - unit / 64};
		std::uniform_int_distribution<fix> radius{5 * unit, 160 * unit};
		std::vector<std::array<fix, 8>> corners;
		std::vector<fix> seen(l.vertices.size());
		std::vector<uint8_t> has_seen(l.vertices.size());
		for (unsigned iter{0}; iter < 300; ++iter)
		{
			const vec3 light{coord(rng), coord(rng), coord(rng)};
			const fix reach{radius(rng)};
			w.walk(l, cell_index(n, light), light, reach, limits);
			if (limits.max_segments == unlimited.max_segments)
				CHECK(w.complete == reach);
			w.light_corners(l, light, corners);
			const auto per_vertex{vertex_light(l, light, reach)};
			std::fill(has_seen.begin(), has_seen.end(), 0);
			for (unsigned s{0}; s < l.segments.size(); ++s)
			{
				if (!l.rendered[s])
					continue;
				for (unsigned c{0}; c < 8; ++c)
				{
					const auto v{l.segments[s].verts[c]};
					const auto value{corners[s][c]};
					CHECK(value == (distance(light, l.vertices[v]) < w.complete ? per_vertex[v] : 0));
					if (has_seen[v])
						CHECK(seen[v] == value);
					else
					{
						has_seen[v] = 1;
						seen[v] = value;
					}
				}
			}
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

/* As lighting.cpp. */
dcx::light_reach_pass_budget game_pass_budget(const std::size_t n_lights)
{
	return {
		.segments = 768,
		.lights = n_lights,
		.min_segments = 12,
		.max_segments = 128,
		.max_hidden_hops = 3,
	};
}

/* Each light gets its share of the segments left, within the bounds;
 * what a light does not use goes to the next ones.
 */
void test_pass_budget()
{
	{
		/* 50 big lights share the budget. */
		auto b{game_pass_budget(50)};
		std::size_t total{0};
		for (unsigned i{0}; i < 50; ++i)
		{
			const auto limits{b.next()};
			CHECK(limits.max_segments >= 12 && limits.max_segments <= 16);
			CHECK(limits.max_hidden_hops == 3);
			b.spend(limits.max_segments);
			total += limits.max_segments;
		}
		CHECK(total <= 768 + 12 * 50 / 4);
		/* Out of budget: the minimum. */
		CHECK(b.next().max_segments == 12);
	}
	{
		/* A few big lights get up to the maximum. */
		auto b{game_pass_budget(4)};
		CHECK(b.next().max_segments == 128);
	}
	{
		/* 30 shots that need 10 segments each, then 8 ships. */
		auto b{game_pass_budget(38)};
		for (unsigned i{0}; i < 30; ++i)
		{
			CHECK(b.next().max_segments >= 12);
			b.spend(10);
		}
		CHECK(b.segments == 468);
		CHECK(b.next().max_segments == 58);
	}
}

/* Cost of one lighting pass in a big open room of 20-unit cubes (a worst
 * case: real levels have bigger segments and walls) with lights of radius
 * 160 (the ship's glow): the walk before the budget (every segment within
 * reach), then the game's pass budget with the corners lit as apply_light
 * does (per segment, cleared on first use in the pass), for 50 lights
 * (about 15 segments each) and for 8 lights (96 each).
 */
void benchmark()
{
	static walker w;
	constexpr unsigned n{24};
	constexpr unsigned max_lights{50};
	/* The walk before the budget is slow: fewer passes. */
	constexpr unsigned passes_before{20}, passes{500};
	level l;
	add_room(l, 0, 0, 0, n, n, n);
	std::minstd_rand rng{4242};
	std::uniform_int_distribution<fix> coord{unit, static_cast<fix>(n) * cell - unit};
	std::array<vec3, max_lights> lights;
	for (auto &p : lights)
		p = {coord(rng), coord(rng), coord(rng)};
	const fix reach{160 * unit};
	using clock = std::chrono::steady_clock;
	const auto per_pass_ms = [](const clock::duration d, const unsigned n) {
		return std::chrono::duration<double, std::milli>(d).count() / n;
	};
	std::size_t visited_before{0};
	auto t0{clock::now()};
	for (unsigned pass{0}; pass < passes_before; ++pass)
		for (const auto &p : lights)
		{
			w.reset();
			graph g{l, p, reach, w.state};
			visited_before += g.walk_unbounded(cell_index(n, p));
		}
	const auto before{per_pass_ms(clock::now() - t0, passes_before)};
	std::printf("test-light-reach: %u lights, radius 160, %ux%ux%u room of 20-unit cubes: before the budget %.3f ms/pass (%zu segments/light)\n",
		max_lights, n, n, n,
		before, visited_before / (passes_before * max_lights));
	struct segment_light
	{
		uint32_t generation;
		std::array<std::array<fix, 3>, 8> corners;
	};
	std::vector<segment_light> seg_light(l.segments.size());
	uint32_t generation{0};
	/* As apply_light: the light of each vertex is computed once per
	 * light, then added to the corners of the processed segments that
	 * use it.
	 */
	dcx::generation_marks<max_vertices> vertex_seen;
	std::vector<fix> vertex_value(l.vertices.size());
	fix sink{0};
	w.state.queue.resize(dcx::light_reach_queue_size(128));
	w.state.segments.resize(128);
	for (const unsigned n_lights : {max_lights, 8u})
	{
		std::size_t visited{0};
		t0 = clock::now();
		for (unsigned pass{0}; pass < passes; ++pass)
		{
			++generation;
			auto budget{game_pass_budget(n_lights)};
			for (const auto &p : std::span(lights).first(n_lights))
			{
				const auto limits{budget.next()};
				w.reset();
				graph g{l, p, reach, w.state, false};
				std::size_t processed;
				const auto complete{dcx::walk_light_reach(g, cell_index(n, p), reach, w.state.queue.data(), w.state.segments.data(), limits, &processed)};
				budget.spend(processed);
				visited += processed;
				vertex_seen.next();
				for (std::size_t i{0}; i != processed; ++i)
				{
					const auto s{w.state.segments[i]};
					auto &sl{seg_light[s]};
					if (sl.generation != generation)
					{
						sl.generation = generation;
						sl.corners = {};
					}
					for (unsigned c{0}; c < 8; ++c)
					{
						const auto vertex{l.segments[s].verts[c]};
						auto &v{vertex_value[vertex]};
						if (vertex_seen.set(vertex))
						{
							const auto d{distance(p, l.vertices[vertex])};
							v = d < complete ? light_at(d) : 0;
						}
						if (!v)
							continue;
						for (auto &ch : sl.corners[c])
							ch += v;
					}
				}
			}
			sink += seg_light[pass % seg_light.size()].corners[0][0];
		}
		const auto t{per_pass_ms(clock::now() - t0, passes)};
		std::printf("test-light-reach: %u lights, radius 160, pass budget, walk and corners: %.3f ms/pass (%zu segments/light)%s\n",
			n_lights, t, visited / (passes * n_lights), sink == 1 ? " " : "");
		CHECK(visited <= passes * std::max<std::size_t>(50 * 32, 32 * n_lights));
	}
}

}

int main()
{
	static walker w;
	test_generation_marks();
	test_pass_budget();
	test_wall_between_rooms(w);
	test_open_room_unchanged(w);
	test_wall_shares_vertices(w);
	test_open_room_corners(w);
	test_radius_bounds_walk(w);
	benchmark();
	std::printf("test-light-reach: all checks passed\n");
	return 0;
}
