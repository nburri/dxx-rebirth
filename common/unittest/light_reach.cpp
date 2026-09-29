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
 * every vertex within its radius (the look of open rooms is unchanged).
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

#include <array>
#include <cmath>
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

constexpr double cell{20};
constexpr int no_child{-1};

struct vec3
{
	double x, y, z;
};

double distance(const vec3 &a, const vec3 &b)
{
	return std::sqrt((a.x - b.x) * (a.x - b.x) + (a.y - b.y) * (a.y - b.y) + (a.z - b.z) * (a.z - b.z));
}

struct box
{
	vec3 lo, hi;
};

/* Distance from p to the nearest point of b (0 inside). */
double distance(const vec3 &p, const box &b)
{
	const auto axis = [](const double c, const double lo, const double hi) {
		return c < lo ? lo - c : (c > hi ? c - hi : 0.);
	};
	const double dx{axis(p.x, b.lo.x, b.hi.x)}, dy{axis(p.y, b.lo.y, b.hi.y)}, dz{axis(p.z, b.lo.z, b.hi.z)};
	return std::sqrt(dx * dx + dy * dy + dz * dz);
}

/* Sides: 0 -x, 1 +x, 2 -y, 3 +y, 4 -z, 5 +z. */
struct segment
{
	std::array<int, 6> children;
	std::array<bool, 6> lets_light_through;
	std::array<unsigned, 8> verts;	/* corner (dx, dy, dz) at dx + 2 dy + 4 dz */
};

struct level
{
	std::vector<segment> segments;
	std::vector<vec3> vertices;

	box side_box(const unsigned s, const unsigned side) const
	{
		const auto &seg{segments[s]};
		box b{vertices[seg.verts[0]], vertices[seg.verts[7]]};
		const unsigned axis{side / 2};
		const bool high{(side & 1) != 0};
		double &lo_c{axis == 0 ? b.lo.x : axis == 1 ? b.lo.y : b.lo.z};
		double &hi_c{axis == 0 ? b.hi.x : axis == 1 ? b.hi.y : b.hi.z};
		if (high)
			lo_c = hi_c;
		else
			hi_c = lo_c;
		return b;
	}
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

constexpr std::size_t max_segments{4096};
constexpr std::size_t max_vertices{8192};

/* The game's adapter (lighting.cpp) on the synthetic level. */
struct graph
{
	const level &l;
	const vec3 light;
	const double reach;
	dcx::generation_marks<max_segments> &segments;
	dcx::generation_marks<max_vertices> &vertices;
	bool enter(const unsigned s)
	{
		if (!segments.set(s))
			return false;
		for (const auto v : l.segments[s].verts)
			vertices.set(v);
		return true;
	}
	template <typename F>
	void for_each_lit_child(const unsigned s, F &&f) const
	{
		const auto &seg{l.segments[s]};
		for (unsigned side{0}; side < 6; ++side)
		{
			const auto child{seg.children[side]};
			if (child == no_child || segments.test(static_cast<unsigned>(child)))
				continue;
			if (distance(light, l.side_box(s, side)) >= reach)
				continue;
			if (!seg.lets_light_through[side])
				continue;
			f(static_cast<unsigned>(child));
		}
	}
};

struct walker
{
	dcx::generation_marks<max_segments> segments;
	dcx::generation_marks<max_vertices> vertices;
	std::array<unsigned, max_segments> queue;
	std::size_t walk(const level &l, const unsigned start, const vec3 &light, const double reach, const std::size_t capacity = max_segments)
	{
		segments.next();
		vertices.next();
		graph g{l, light, reach, segments, vertices};
		return dcx::walk_light_reach(g, start, queue.data(), capacity);
	}
	/* The vertices apply_light lights: reached and within the radius. */
	std::vector<unsigned> lit(const level &l, const vec3 &light, const double reach) const
	{
		std::vector<unsigned> r;
		for (unsigned v{0}; v < l.vertices.size(); ++v)
			if (vertices.test(v) && distance(light, l.vertices[v]) < reach)
				r.push_back(v);
		return r;
	}
};

unsigned count_within(const level &l, const vec3 &light, const double reach, const unsigned first_vertex, const unsigned end_vertex)
{
	unsigned n{0};
	for (unsigned v{first_vertex}; v < end_vertex; ++v)
		n += distance(light, l.vertices[v]) < reach;
	return n;
}

/* Two rooms side by side, the wall between them solid, with a closed
 * door or open: shots near the wall light the other room only through an
 * open door.
 */
void test_wall_between_rooms(walker &w)
{
	for (const int door : {0, 1, 2})	/* none, closed, open */
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
		const vec3 light{55, 30, 30};
		const double reach{40};
		const unsigned start{a + 2 + 3 * (1 + 3 * 1)};
		CHECK(w.walk(l, start, light, reach) > 0);
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

/* In an open room, every vertex within the radius is reached, wherever
 * the light is: the look of open rooms does not change.
 */
void test_open_room_unchanged(walker &w)
{
	level l;
	constexpr unsigned n{8};
	add_room(l, 0, 0, 0, n, n, n);
	std::minstd_rand rng{12345};
	std::uniform_real_distribution<double> coord{0.01, n * cell - 0.01};
	std::uniform_real_distribution<double> radius{5, 120};
	for (unsigned iter{0}; iter < 2000; ++iter)
	{
		const vec3 light{coord(rng), coord(rng), coord(rng)};
		const double reach{radius(rng)};
		const auto cell_of = [](const double c) {
			return static_cast<unsigned>(c / cell);
		};
		const unsigned start{cell_of(light.x) + n * (cell_of(light.y) + n * cell_of(light.z))};
		w.walk(l, start, light, reach);
		CHECK(w.lit(l, light, reach).size() == count_within(l, light, reach, 0, l.vertices.size()));
	}
}

/* A long corridor: the walk stops at the radius. */
void test_radius_bounds_walk(walker &w)
{
	level l;
	add_room(l, 0, 0, 0, 200, 1, 1);
	const vec3 light{10, 10, 10};
	/* Sides at x = 20, 40, 60 are within 50; the one at 60 is not. */
	CHECK(w.walk(l, 0, light, 50) == 3);
	CHECK(w.walk(l, 0, light, 5) == 1);
	/* The capacity bounds the walk. */
	CHECK(w.walk(l, 0, light, 100000, 7) == 7);
	CHECK(w.walk(l, 0, light, 100000) == 200);
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

}

int main()
{
	static walker w;
	test_generation_marks();
	test_wall_between_rooms(w);
	test_open_room_unchanged(w);
	test_radius_bounds_walk(w);
	std::printf("test-light-reach: all checks passed\n");
	return 0;
}
