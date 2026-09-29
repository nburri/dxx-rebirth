/*
 * Portions of this file are copyright Rebirth contributors and licensed as
 * described in COPYING.txt.
 * Portions of this file are copyright Parallax Software and licensed
 * according to the Parallax license below.
 * See COPYING.txt for license details.

THE COMPUTER CODE CONTAINED HEREIN IS THE SOLE PROPERTY OF PARALLAX
SOFTWARE CORPORATION ("PARALLAX").  PARALLAX, IN DISTRIBUTING THE CODE TO
END-USERS, AND SUBJECT TO ALL OF THE TERMS AND CONDITIONS HEREIN, GRANTS A
ROYALTY-FREE, PERPETUAL LICENSE TO SUCH END-USERS FOR USE BY SUCH END-USERS
IN USING, DISPLAYING,  AND CREATING DERIVATIVE WORKS THEREOF, SO LONG AS
SUCH USE, DISPLAY OR CREATION IS FOR NON-COMMERCIAL, ROYALTY OR REVENUE
FREE PURPOSES.  IN NO EVENT SHALL THE END-USER USE THE COMPUTER CODE
CONTAINED HEREIN FOR REVENUE-BEARING PURPOSES.  THE END-USER UNDERSTANDS
AND AGREES TO THE TERMS HEREIN AND ACCEPTS THE SAME BY USE OF THIS FILE.
COPYRIGHT 1993-1999 PARALLAX SOFTWARE CORPORATION.  ALL RIGHTS RESERVED.
*/

/*
 *
 * Lighting functions.
 *
 */

#include <algorithm>
#include <bitset>
#include <numeric>
#include <span>
#include <stdio.h>
#include <string.h>	// for memset()

#include "render_state.h"
#include "maths.h"
#include "vecmat.h"
#include "gr.h"
#include "inferno.h"
#include "segment.h"
#include "dxxerror.h"
#include "game.h"
#include "vclip.h"
#include "lighting.h"
#include "3d.h"
#include "interp.h"
#include "gameseg.h"
#include "laser.h"
#include "timer.h"
#include "player.h"
#include "playsave.h"
#include "weapon.h"
#include "powerup.h"
#include "fvi.h"
#include "physics.h"
#include "object.h"
#include "robot.h"
#include "multi.h"
#include "palette.h"
#include "bm.h"
#include "wall.h"

#include "compiler-range_for.h"
#include "d_bitset.h"
#include "d_enumerate.h"
#include "d_levelstate.h"
#include "partial_range.h"
#include "d_range.h"
#include "light_reach.h"

using std::min;

#define	HEADLIGHT_CONE_DOT	(F1_0*9/10)
#define	HEADLIGHT_SCALE		(F1_0*10)

namespace dcx {
namespace {

/* Axis-aligned bounding box of a group of rendered vertices. */
struct render_vertex_bounds
{
	vms_vector min, max;
	void include(const vms_vector &v)
	{
		min.x = std::min(min.x, v.x);
		min.y = std::min(min.y, v.y);
		min.z = std::min(min.z, v.z);
		max.x = std::max(max.x, v.x);
		max.y = std::max(max.y, v.y);
		max.z = std::max(max.z, v.z);
	}
};

/* Number of consecutive entries of the render vertex list that share one
 * bounding box.  Consecutive entries come from segments that are close in
 * the render list, so they tend to be close in space.
 */
constexpr std::size_t render_vertex_block_size{16};
/* Each rendered vertex is listed once (render_vertex_flags), so the list
 * never holds more than the vertices of the level.
 */
constexpr std::size_t max_render_vertices{MAX_VERTICES};

/* The vertices of all rendered segments, each listed once, with a copy of
 * their positions (so that apply_light reads them sequentially) and the
 * bounding boxes used to skip vertices that a light cannot reach.
 */
struct render_vertex_list
{
	unsigned n_render_vertices{0};
	render_vertex_bounds all_bounds;
	std::array<vertnum_t, max_render_vertices> vertices;
	std::array<vms_vector, max_render_vertices> positions;
	std::array<render_vertex_bounds, (max_render_vertices + render_vertex_block_size - 1) / render_vertex_block_size> block_bounds;
};

/* Same formula as vm_vec_mag_quick, applied to non-negative per-axis
 * distances and evaluated in 64 bits so that it cannot overflow.  For
 * inputs where vm_vec_mag_quick does not overflow, both return the same
 * value.  The result never decreases when any input increases: each
 * order statistic of (a, b, c) is monotonic in each input, and the
 * formula is monotonic in each order statistic.
 */
static int64_t compute_quick_magnitude(int64_t a, int64_t b, int64_t c)
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

/* Lower bound of vm_vec_dist_quick(p, v) for every v inside b. */
static int64_t quick_distance_lower_bound(const vms_vector &p, const render_vertex_bounds &b)
{
	const auto axis = [](const fix pc, const fix lo, const fix hi) -> int64_t {
		if (pc < lo)
			return int64_t{lo} - pc;
		if (pc > hi)
			return int64_t{pc} - hi;
		return 0;
	};
	return compute_quick_magnitude(axis(p.x, b.min.x, b.max.x), axis(p.y, b.min.y, b.max.y), axis(p.z, b.min.z, b.max.z));
}

/* Upper bound of vm_vec_dist_quick(p, v) for every v inside b, assuming
 * no overflow.  If the result is at most INT32_MAX, then no component of
 * p - v and no intermediate value in vm_vec_mag_quick overflows, so
 * vm_vec_dist_quick(p, v) is exact, non-negative and at least as large as
 * quick_distance_lower_bound(p, b).
 */
static int64_t quick_distance_upper_bound(const vms_vector &p, const render_vertex_bounds &b)
{
	const auto axis = [](const fix pc, const fix lo, const fix hi) -> int64_t {
		return std::max(std::abs(int64_t{pc} - lo), std::abs(int64_t{pc} - hi));
	};
	return compute_quick_magnitude(axis(p.x, b.min.x, b.max.x), axis(p.y, b.min.y, b.max.y), axis(p.z, b.min.z, b.max.z));
}

static void add_light_div(g3s_lrgb &d, const g3s_lrgb &light, const fix &scale)
{
	d.r += fixdiv(light.r, scale);
	d.g += fixdiv(light.g, scale);
	d.b += fixdiv(light.b, scale);
}

static void add_light_dot_square(g3s_lrgb &d, const g3s_lrgb &light, const fix &dot)
{
	auto square = fixmul(dot, dot);
	d.r += fixmul(square, light.r)/8;
	d.g += fixmul(square, light.g)/8;
	d.b += fixmul(square, light.b)/8;
}

static fix compute_player_light_emission_intensity(const object_base &objp)
{
	auto &phys_info = objp.mtype.phys_info;
	const fix k{compute_thrust_scale_holding_velocity(phys_info.mass, phys_info.drag)};
	// smooth thrust value like set_thrust_from_velocity()
	const auto sthrust{vm_vec_copy_scale(phys_info.velocity, k)};
	return std::max(static_cast<fix>(vm_vec_mag_quick(sthrust) / 4), F2_0) + F0_5;
}

static fix compute_fireball_light_emission_intensity(const d_vclip_array &Vclip, const object_base &objp)
{
	const auto oid = get_fireball_id(objp);
	if (!Vclip.valid_index(oid))
		return 0;
	auto &v = Vclip[oid];
	const auto light_intensity = v.light_value;
	if (objp.lifeleft < F1_0*4)
		return fixmul(fixdiv(objp.lifeleft, v.play_time), light_intensity);
	return light_intensity;
}

}
}

// ----------------------------------------------------------------------------------------------
namespace dsx {
namespace {

/* The cost of the walks is bounded (light_reach.h).  A light lights
 * nothing beyond the distance its walk covered: there, nothing is known
 * about walls.  Measured on 14 multiplayer levels (segments of 30-50
 * units, lights at random segment centres), the ship's glow (radius 160)
 * needs 38 segments at the median and 79 at the 90th percentile;
 * limited to 32 segments, 17% of the positions show a visible edge (more
 * than 1/32 of full light where the light stops), limited to 64 only 1%.
 *
 * Each pass has a budget of segments for all its lights
 * (light_reach_pass_budget): each light may process its share of what
 * is left, within the bounds below.  Lights are applied dimmest first,
 * so what the shots do not need goes to the ships' glow and explosions.
 * In the benchmark of test-light-reach (50 lights of radius 160 in a big
 * room of 20-unit cubes), a pass takes about 0.1 ms.
 */
constexpr std::size_t light_reach_min_segments{12};
constexpr std::size_t light_reach_max_segments{128};
constexpr std::size_t light_reach_pass_segments{768};
/* Segments that are not rendered (behind the viewer, beyond the render
 * depth) are passed only in short runs: enough for a light just behind
 * the viewer to reach the rendered segments.
 */
constexpr unsigned light_reach_max_hidden_hops{3};

/* A light of this pass, before it is applied. */
struct pending_light
{
	/* (r + g + b) / 3: lights are applied dimmest first. */
	fix intensity;
	g3s_lrgb emission;
	/* The object, or which muzzle flash. */
	objnum_t objnum;
	uint8_t muzzle;
	bool is_muzzle;
};

/* The segments reached by the light being applied (light_reach.h), and
 * the segments rendered in this lighting pass.  One instance, since
 * set_dynamic_light and apply_light are not re-entrant.
 */
struct light_reach_state
{
	generation_marks<MAX_SEGMENTS> rendered;
	generation_marks<MAX_SEGMENTS> discovered;
	std::array<light_reach_entry<segnum_t, fix>, light_reach_queue_size(light_reach_max_segments)> queue;
	/* The segments processed, nearest first. */
	std::array<segnum_t, light_reach_max_segments> segments;
	/* The light of the vertices of the processed segments, each
	 * computed once per light.
	 */
	generation_marks<MAX_VERTICES> vertex_seen;
	per_vertex_array<g3s_lrgb> vertex_light;
};

light_reach_state reach_state;

/* The level as walk_light_reach sees it: a side lets light through if
 * it can be rendered past (no wall, an open door, a grate, glass), and
 * the distance of a segment is that of the nearest point of the box
 * around its vertices.  The test of each corner against the radius is
 * left to apply_light, so a reached segment gets the same light as
 * before.
 */
class light_reach_graph
{
	light_reach_state &state;
	fvcvertptr &vcvertptr;
	fvcwallptr &vcwallptr;
	const vms_vector &light_pos;
	const fix light_reach;
public:
	light_reach_graph(light_reach_state &state, fvcvertptr &vcvertptr, fvcwallptr &vcwallptr, const vms_vector &light_pos, const fix light_reach) :
		state{state}, vcvertptr{vcvertptr}, vcwallptr{vcwallptr}, light_pos{light_pos}, light_reach{light_reach}
	{
	}
	bool rendered(const segnum_t segnum) const
	{
		return state.rendered.test(segnum);
	}
	void discover(const segnum_t segnum)
	{
		state.discovered.set(segnum);
	}
	fix distance(const segnum_t segnum) const
	{
		auto &verts{vcsegptr(segnum)->verts};
		const auto &p0{*vcvertptr(verts[segment_relative_vertnum{0}])};
		render_vertex_bounds b{p0, p0};
		for (const auto v : verts)
			b.include(*vcvertptr(v));
		return static_cast<fix>(std::min<int64_t>(quick_distance_lower_bound(light_pos, b), light_reach));
	}
	/* apply_light lights the corners of the processed segments once
	 * the walk is done, when the distance it covered is known.
	 */
	static void process(segnum_t)
	{
	}
	template <typename F>
	void for_each_lit_child(const segnum_t segnum, F &&f) const
	{
		const auto &&seg{vcsegptridx(segnum)};
		for (const auto side : MAX_SIDES_PER_SEGMENT)
		{
			const auto child{seg->shared_segment::children[side]};
			if (!IS_CHILD(child) || state.discovered.test(child))
				continue;
			if (!(WALL_IS_DOORWAY(GameBitmaps, Textures, vcwallptr, seg, side) & WALL_IS_DOORWAY_FLAG::rendpast))
				continue;
			f(child);
		}
	}
};

/* The segment that contains `pos`: obj_seg, or a neighbour of it if pos
 * already crossed one of its sides (the object's segment is updated
 * after it moves; a muzzle flash is at the gun, in front of the ship).
 */
static segnum_t light_start_segment(fvcvertptr &vcvertptr, const vcsegptridx_t obj_seg, const vms_vector &pos)
{
	const auto outside{get_seg_masks(vcvertptr, pos, obj_seg, 0).centermask};
	if (outside == sidemask_t{})
		return obj_seg;
	for (const auto side : MAX_SIDES_PER_SEGMENT)
	{
		if (!(outside & build_sidemask(side)))
			continue;
		const auto child{obj_seg->shared_segment::children[side]};
		if (IS_CHILD(child) && get_seg_masks(vcvertptr, pos, vcsegptr(child), 0).centermask == sidemask_t{})
			return child;
	}
	return obj_seg;
}

/* The corner light of `segnum` in this lighting pass, cleared on first
 * use in the pass.
 */
static per_segment_relative_vertnum_array<g3s_lrgb> &segment_corner_light(::dcx::d_level_unique_light_state &light_state, const segnum_t segnum)
{
	auto &l{light_state.Segment_dynamic_light[segnum]};
	if (l.generation != light_state.Segment_dynamic_light_generation)
	{
		l.generation = light_state.Segment_dynamic_light_generation;
		l.corners = {};
	}
	return l.corners;
}

/* Descent's rule for a dim light (and a marker): only the vertices of the
 * light's own segment are lit.  Each of those vertices is shared by the
 * segments around it; the light goes to the corners at that vertex of
 * the segments connected to the light's segment through sides that
 * contain a lit vertex and let light through, so an open neighbour
 * shows no seam and a segment behind a wall or a closed door gets
 * nothing.
 */
static void apply_light_to_own_segment(const g3s_lrgb obj_light_emission, const fix obji_64, const vcsegptridx_t obj_seg, const vms_vector &obj_pos)
{
	auto &vcvertptr{LevelSharedSegmentState.get_vertex_state().get_vertices().vcptr};
	std::array<vertnum_t, MAX_VERTICES_PER_SEGMENT> lit_vertices;
	std::array<g3s_lrgb, MAX_VERTICES_PER_SEGMENT> lit_light;
	unsigned n_lit{0};
	for (const auto vertnum : obj_seg->verts)
	{
		fix dist{vm_vec_dist_quick(obj_pos, *vcvertptr(vertnum))};
		dist = fixmul(dist/4, dist/4);
		if (dist < abs(obji_64)) {
			if (dist < MIN_LIGHT_DIST)
				dist = MIN_LIGHT_DIST;
			lit_vertices[n_lit] = vertnum;
			lit_light[n_lit] = {};
			add_light_div(lit_light[n_lit], obj_light_emission, dist);
			++n_lit;
		}
	}
	if (!n_lit)
		return;
	auto &vcwallptr{LevelUniqueWallSubsystemState.Walls.vcptr};
	auto &queue{reach_state.queue};
	reach_state.discovered.next();
	reach_state.discovered.set(obj_seg);
	std::size_t head{0}, tail{0};
	queue[tail++] = {0, obj_seg, 0};
	while (head != tail)
	{
		const auto segnum{queue[head++].segment};
		const auto &&seg{vcsegptridx(segnum)};
		unsigned lit_corners{0};
		for (const auto &&[c, v] : enumerate(seg->verts))
			for (unsigned k{0}; k != n_lit; ++k)
			{
				if (v != lit_vertices[k])
					continue;
				auto &d{segment_corner_light(LevelUniqueLightState, segnum)[c]};
				d.r += lit_light[k].r;
				d.g += lit_light[k].g;
				d.b += lit_light[k].b;
				lit_corners |= 1u << underlying_value(c);
			}
		for (const auto &&[side, sv] : enumerate(Side_to_verts))
		{
			const auto child{seg->shared_segment::children[side]};
			if (!IS_CHILD(child) || reach_state.discovered.test(child))
				continue;
			unsigned side_corners{0};
			for (const auto i : sv)
				side_corners |= 1u << underlying_value(i);
			if (!(side_corners & lit_corners))
				continue;
			if (!(WALL_IS_DOORWAY(GameBitmaps, Textures, vcwallptr, seg, side) & WALL_IS_DOORWAY_FLAG::rendpast))
				continue;
			if (tail == queue.size())
				/* Bounded; the fan around 8 vertices is far smaller. */
				break;
			reach_state.discovered.set(child);
			queue[tail++] = {0, child, 0};
		}
	}
}

static void apply_light(const g3s_lrgb obj_light_emission, const vcsegptridx_t obj_seg, const vms_vector &obj_pos, const render_vertex_list &rvl, const icobjptridx_t objnum, light_reach_pass_budget &budget)
{
	const auto limits{budget.next()};
	if (((obj_light_emission.r+obj_light_emission.g+obj_light_emission.b)/3) > 0)
	{
		fix obji_64 = ((obj_light_emission.r+obj_light_emission.g+obj_light_emission.b)/3)*64;
		sbyte is_marker{0};
#if DXX_BUILD_DESCENT == 2
		if (objnum && objnum->type == object_type::OBJ_MARKER)
				is_marker = 1;
#endif

		auto &vcvertptr{LevelSharedSegmentState.get_vertex_state().get_vertices().vcptr};
		// for pretty dim sources, only process vertices in object's own segment.
		//	12/04/95, MK, markers only cast light in own segment.
		if ((abs(obji_64) <= F1_0*8) || is_marker) {
			apply_light_to_own_segment(obj_light_emission, obji_64, obj_seg, obj_pos);
		} else {
			int headlight_shift{0};
			fix	max_headlight_dist = F1_0*200;

#if DXX_BUILD_DESCENT == 2
			if (objnum)
			{
				const object &obj = *objnum;
				if (obj.type == object_type::OBJ_PLAYER)
					if (+(obj.ctype.player_info.powerup_flags & player_flag::headlight_on)) {
						headlight_shift = 3;
						if (get_player_id(obj) != Player_num)
						{
							fvi_info		hit_data;

							const auto tvec{vm_vec_scale_add(obj.pos, obj.orient.fvec, F1_0 * 200)};
							const auto fate = find_vector_intersection(fvi_query{
								obj.pos,
								tvec,
								fvi_query::unused_ignore_obj_list,
								fvi_query::unused_LevelUniqueObjectState,
								fvi_query::unused_Robot_info,
								FQ_TRANSWALL,
								objnum,
							}, obj_seg, 0, hit_data);
							if (fate != fvi_hit_type::None)
								max_headlight_dist = vm_vec_mag_quick(vm_vec_build_sub(hit_data.hit_pnt, obj.pos)) + F1_0*4;
						}
					}
			}
#endif
			const auto n_render_vertices{rvl.n_render_vertices};
			if (!n_render_vertices)
				return;
			const fix light_reach{abs(obji_64)};
			const auto out_of_reach = [&obj_pos, headlight_shift, light_reach](const render_vertex_bounds &b) {
				return (quick_distance_lower_bound(obj_pos, b) >> headlight_shift) >= light_reach;
			};
			const bool quick_distance_may_overflow{quick_distance_upper_bound(obj_pos, rvl.all_bounds) > INT32_MAX};
			if (!quick_distance_may_overflow && out_of_reach(rvl.all_bounds))
				/* No rendered vertex is within reach. */
				return;
			if (!headlight_shift)
			{
				/* Keep the light on its side of solid walls: only the
				 * corners of the rendered segments it reaches through
				 * open sides are lit (light_reach.h), each with the
				 * same value as Descent gave its vertex.  Descent stored
				 * the light per vertex, so shots lit the walls of the
				 * next room through the wall between them: the rooms
				 * share the wall's vertices.  Beyond the distance the
				 * walk covered, nothing is lit.
				 */
				reach_state.discovered.next();
				light_reach_graph g{reach_state, vcvertptr, LevelUniqueWallSubsystemState.Walls.vcptr, obj_pos, light_reach};
				std::size_t processed;
				const auto complete{walk_light_reach(g, light_start_segment(vcvertptr, obj_seg, obj_pos), light_reach, reach_state.queue.data(), reach_state.segments.data(), limits, &processed)};
				budget.spend(processed);
				reach_state.vertex_seen.next();
				for (const auto segnum : std::span(reach_state.segments).first(processed))
				{
					if (!reach_state.rendered.test(segnum))
						continue;
					auto &corners{segment_corner_light(LevelUniqueLightState, segnum)};
					for (const auto &&[c, vertnum] : enumerate(vcsegptr(segnum)->verts))
					{
						auto &l{reach_state.vertex_light[vertnum]};
						if (reach_state.vertex_seen.set(underlying_value(vertnum)))
						{
							/* Shared by the segments around the vertex:
							 * computed once, so they all get the same.
							 */
							l = {};
							fix dist{vm_vec_dist_quick(obj_pos, *vcvertptr(vertnum))};
							if (dist < complete)
							{
								if (dist < MIN_LIGHT_DIST)
									dist = MIN_LIGHT_DIST;
								add_light_div(l, obj_light_emission, dist);
							}
						}
						auto &d{corners[c]};
						d.r += l.r;
						d.g += l.g;
						d.b += l.b;
					}
				}
				return;
			}
			/* A headlight keeps Descent's rule, per vertex: its reach
			 * (eight times the radius) would walk most of the level, and
			 * in a network game the beam of another player's headlight
			 * already stops at the first wall it hits.
			 */
			auto &Headlight_dynamic_light{LevelUniqueLightState.Headlight_dynamic_light};
			const auto apply_light_to_vertices = [&](const unsigned vv_begin, const unsigned vv_end) {
				for (unsigned vv{vv_begin}; vv != vv_end; ++vv)
				{
					const auto vertnum = rvl.vertices[vv];
					auto &vertpos = rvl.positions[vv];
					fix dist = vm_vec_dist_quick(obj_pos, vertpos);

					if ((dist >> headlight_shift) < abs(obji_64)) {

						if (dist < MIN_LIGHT_DIST)
							dist = MIN_LIGHT_DIST;

						if (objnum)
						{
							fix dot;
							// MK, Optimization note: You compute distance about 15 lines up, this is partially redundant
							const auto vec_to_point = vm_vec_normalized_quick(vm_vec_build_sub(vertpos, obj_pos));
							dot = vm_vec_build_dot(vec_to_point, objnum->orient.fvec);
							if (dot < F1_0/2)
							{
								// Do the normal thing, but darken around headlight.
								add_light_div(Headlight_dynamic_light[vertnum], obj_light_emission, fixmul(HEADLIGHT_SCALE, dist));
							}
							else
							{
								if (!(Game_mode & GM_MULTI) || dist < max_headlight_dist)
								{
									add_light_dot_square(Headlight_dynamic_light[vertnum], obj_light_emission, dot);
								}
							}
						}
						else
						{
							add_light_div(Headlight_dynamic_light[vertnum], obj_light_emission, dist);
						}
					}
				}
			};
			/* A vertex receives light only if
			 * (vm_vec_dist_quick(obj_pos, vertex) >> headlight_shift) < light_reach.
			 * If that fails for a lower bound of the distance of all
			 * vertices in a box, it fails for every vertex in that box, so
			 * those vertices can be skipped without changing any result.
			 * This is valid only if vm_vec_dist_quick cannot overflow for
			 * any rendered vertex; otherwise, process all of them.
			 */
			if (quick_distance_may_overflow)
				apply_light_to_vertices(0, n_render_vertices);
			else
			{
				for (unsigned vv_begin{0}; vv_begin < n_render_vertices; vv_begin += render_vertex_block_size)
				{
					if (out_of_reach(rvl.block_bounds[vv_begin / render_vertex_block_size]))
						continue;
					apply_light_to_vertices(vv_begin, std::min<unsigned>(vv_begin + render_vertex_block_size, n_render_vertices));
				}
			}
		}
	}
}
}
}

namespace {

// ----------------------------------------------------------------------------------------------
static void collect_muzzle_flash_lights(pending_light *const lights, std::size_t &n_lights)
{
	static constexpr fix FLASH_LEN_FIXED_SECONDS{F1_0 / 3};
	static constexpr fix FLASH_SCALE{3 * F1_0 / FLASH_LEN_FIXED_SECONDS};
	fix64 current_time;
	short time_since_flash;

	current_time = timer_query();

	range_for (auto &i, Muzzle_data)
	{
		if (i.create_time)
		{
			time_since_flash = current_time - i.create_time;
			if (time_since_flash < FLASH_LEN_FIXED_SECONDS)
			{
				g3s_lrgb ml;
				ml.r = ml.g = ml.b = ((FLASH_LEN_FIXED_SECONDS - time_since_flash) * FLASH_SCALE);
				if (ml.r > 0)
					lights[n_lights++] = {
						.intensity = ml.r,
						.emission = ml,
						.objnum = objnum_t{},
						.muzzle = static_cast<uint8_t>(&i - Muzzle_data.data()),
						.is_muzzle = true,
					};
			}
			else
			{
				i.create_time = 0; // turn off this muzzle flash
			}
		}
	}
}

// Translation table to make flares flicker at different rates
const std::array<fix, 16> Obj_light_xlate{{0x1234, 0x3321, 0x2468, 0x1735,
			    0x0123, 0x19af, 0x3f03, 0x232a,
			    0x2123, 0x39af, 0x0f03, 0x132a,
			    0x3123, 0x29af, 0x1f03, 0x032a
}};
#if DXX_BUILD_DESCENT == 1
#define compute_player_light_emission_intensity(LevelUniqueHeadlightState, obj)	compute_player_light_emission_intensity(obj)
#define compute_light_emission(Robot_info, LevelUniqueHeadlightState, Vclip, obj)	compute_light_emission(Vclip, obj)
#elif DXX_BUILD_DESCENT == 2
#undef compute_player_light_emission_intensity
#undef compute_light_emission
#endif
}

// ---------------------------------------------------------
namespace dsx {
namespace {

#if DXX_BUILD_DESCENT == 2
static fix compute_player_light_emission_intensity(d_level_unique_headlight_state &LevelUniqueHeadlightState, const object &objp)
{
	if (+(objp.ctype.player_info.powerup_flags & player_flag::headlight_on))
	{
		auto &Headlights = LevelUniqueHeadlightState.Headlights;
		auto &Num_headlights = LevelUniqueHeadlightState.Num_headlights;
		if (Num_headlights < Headlights.size())
			Headlights[Num_headlights++] = &objp;
		return HEADLIGHT_SCALE;
	}
	// If hoard game and player, add extra light based on how many orbs you have Pulse as well.
	if (game_mode_hoard(Game_mode))
	{
		if (const auto hoard_orbs{objp.ctype.player_info.hoard.orbs})
		{
			const fix hoardlight = 1 + (i2f(hoard_orbs) / 2);
			const auto s = fix_sin(static_cast<fix>(GameTime64 >> 1) & 0xFFFF); // probably a bad way to do it
			return fixmul((s + F1_0) >> 1, hoardlight);
		}
	}
	return ::dcx::compute_player_light_emission_intensity(objp);
}
#endif

static g3s_lrgb build_object_color_from_bitmap(GameBitmaps_array &GameBitmaps, const bitmap_index i, grs_bitmap &bm)
{
	if (bm.get_flag_mask(BM_FLAG_PAGED_OUT))
		piggy_bitmap_page_in(GameBitmaps, i);
	return g3s_lrgb{
		.r = bm.avg_color_rgb[0],
		.g = bm.avg_color_rgb[1],
		.b = bm.avg_color_rgb[2],
	};
}

static g3s_lrgb build_object_color_from_range(GameBitmaps_array &GameBitmaps, const bitmap_index index_begin, const bitmap_index index_end)
{
	g3s_lrgb obj_color{};
	for (auto &&[i, bm] : enumerate(partial_range(GameBitmaps, underlying_value(index_begin), underlying_value(index_end))))
	{
		const auto r = build_object_color_from_bitmap(GameBitmaps, i, bm);
		obj_color.r += r.r;
		obj_color.g += r.g;
		obj_color.b += r.b;
	}
	return obj_color;
}

static g3s_lrgb build_object_color_from_vclip(GameBitmaps_array &GameBitmaps, const d_vclip_array &Vclip, const vclip_index id)
{
	auto &v = Vclip[id];
	auto &f = v.frames;
	const auto t_idx_s = f[0];
	const auto t_idx_e = f[v.num_frames - 1];
	return build_object_color_from_range(GameBitmaps, t_idx_s, t_idx_e);
}

static g3s_lrgb build_object_color(GameBitmaps_array &GameBitmaps, const object_base &objp)
{
	switch (objp.render_type)
	{
		case render_type::RT_POLYOBJ:
		{
			auto &Polygon_models = LevelSharedPolygonModelState.Polygon_models;
			const polymodel *const po = &Polygon_models[objp.rtype.pobj_info.model_num.dsx];
			if (const auto n_textures = po->n_textures; n_textures > 0)
			{
				const bitmap_index t_idx_s = ObjBitmaps[ObjBitmapPtrs[po->first_texture]];
				const bitmap_index t_idx_e{static_cast<uint16_t>(underlying_value(t_idx_s) + n_textures)};
				return build_object_color_from_range(GameBitmaps, t_idx_s, t_idx_e);
			}
			/* If no texture, try to get a general polygon color */
			if (const auto color = g3_poly_get_color(po->model_data.get()))
			{
				auto &rgb = gr_current_pal[color];
				return {rgb.r, rgb.g, rgb.b};
			}
			/* If no color either, fall through and use a generic value */
		}
		[[fallthrough]];
		case render_type::RT_NONE:
			// no object - no light
			return {255, 255, 255};
		case render_type::RT_LASER:
		{
			const bitmap_index t_idx_s = Weapon_info[get_weapon_id(objp)].bitmap;
			return build_object_color_from_bitmap(GameBitmaps, t_idx_s, GameBitmaps[t_idx_s]);
		}
		case render_type::RT_POWERUP:
			return build_object_color_from_vclip(GameBitmaps, Vclip, objp.rtype.vclip_info.vclip_num);
		case render_type::RT_WEAPON_VCLIP:
			return build_object_color_from_vclip(GameBitmaps, Vclip, Weapon_info[get_weapon_id(objp)].weapon_vclip);
		default:
			return build_object_color_from_vclip(GameBitmaps, Vclip, vclip_index{objp.id});
	}
}

static g3s_lrgb compute_light_emission(const d_robot_info_array &Robot_info, d_level_unique_headlight_state &LevelUniqueHeadlightState, const d_vclip_array &Vclip, const vcobjptridx_t obj)
{
	int compute_color{0};
	fix light_intensity{0};
	const object &objp = obj;
	switch (objp.type)
	{
		case object_type::OBJ_PLAYER:
			light_intensity = compute_player_light_emission_intensity(LevelUniqueHeadlightState, objp);
			break;
		case object_type::OBJ_FIREBALL:
			light_intensity = compute_fireball_light_emission_intensity(Vclip, objp);
			break;
		case object_type::OBJ_ROBOT:
#if DXX_BUILD_DESCENT == 1
			light_intensity = F1_0/2;	// F1_0*Robot_info[obj->id].lightcast;
#elif DXX_BUILD_DESCENT == 2
			light_intensity = F1_0*Robot_info[get_robot_id(objp)].lightcast;
#endif
			break;
		case object_type::OBJ_WEAPON:
		{
			const auto wid = get_weapon_id(objp);
			const fix tval = Weapon_info[wid].light;
			if (wid == weapon_id_type::FLARE_ID)
				light_intensity = 2 * (min(tval, objp.lifeleft) + ((static_cast<fix>(GameTime64) ^ Obj_light_xlate[obj.get_unchecked_index() % Obj_light_xlate.size()]) & 0x3fff));
			else
				light_intensity = tval;
			break;
		}
#if DXX_BUILD_DESCENT == 2
		case object_type::OBJ_MARKER:
		{
			fix lightval = objp.lifeleft;

			lightval &= 0xffff;
			lightval = 8 * abs(F1_0/2 - lightval);

			light_intensity = lightval;
			break;
		}
#endif
		case object_type::OBJ_POWERUP:
			light_intensity = Powerup_info[get_powerup_id(objp)].light;
			break;
		case object_type::OBJ_DEBRIS:
			light_intensity = F1_0/4;
			break;
		case object_type::OBJ_LIGHT:
			light_intensity = objp.ctype.light_info.intensity;
			break;
		default:
			light_intensity = 0;
			break;
	}

	const auto &&white_light = [light_intensity] {
		return g3s_lrgb{light_intensity, light_intensity, light_intensity};
	};

	if (!PlayerCfg.DynLightColor) // colored lights not desired so use intensity only OR no intensity (== no light == no color) at all
		return white_light();

	switch (objp.type) // find out if given object should cast colored light and compute if so
	{
		default:
			break;
		case object_type::OBJ_FIREBALL:
		case object_type::OBJ_WEAPON:
#if DXX_BUILD_DESCENT == 2
		case object_type::OBJ_MARKER:
#endif
			compute_color = 1;
			break;
		case object_type::OBJ_POWERUP:
		{
			switch (get_powerup_id(objp))
			{
				case powerup_type_t::POW_EXTRA_LIFE:
				case powerup_type_t::POW_ENERGY:
				case powerup_type_t::POW_SHIELD_BOOST:
				case powerup_type_t::POW_KEY_BLUE:
				case powerup_type_t::POW_KEY_RED:
				case powerup_type_t::POW_KEY_GOLD:
				case powerup_type_t::POW_CLOAK:
				case powerup_type_t::POW_INVULNERABILITY:
#if DXX_BUILD_DESCENT == 2
				case powerup_type_t::POW_HOARD_ORB:
#endif
					compute_color = 1;
					break;
				default:
					break;
			}
			break;
		}
	}

	if (compute_color)
	{
		if (light_intensity < F1_0) // for every effect we want color, increase light_intensity so the effect becomes barely visible
			light_intensity = F1_0;

		const auto &&obj_color = build_object_color(GameBitmaps, objp);
		const fix rgbsum = obj_color.r + obj_color.g + obj_color.b;
		// obviously this object did not give us any usable color. so let's do our own but with blackjack and hookers!
		if (rgbsum <= 0)
			return white_light();
		// scale color to light intensity
		const float cscale = static_cast<float>(light_intensity * 3) / rgbsum;
		return g3s_lrgb{
			static_cast<fix>(obj_color.r * cscale),
			static_cast<fix>(obj_color.g * cscale),
			static_cast<fix>(obj_color.b * cscale)
		};
	}

	return white_light();
}

}

// ----------------------------------------------------------------------------------------------
void set_dynamic_light(const d_robot_info_array &Robot_info, render_state_t &rstate)
{
	auto &Objects = LevelUniqueObjectState.Objects;
	auto &vcobjptridx = Objects.vcptridx;
	static fix light_time; 

#if DXX_BUILD_DESCENT == 2
	LevelUniqueLightState.Num_headlights = 0;
#endif

	light_time += FrameTime;
	if (light_time < (F1_0/60)) // it's enough to stress the CPU 60 times per second
		return;
	light_time = light_time - (F1_0/60);

	enumerated_bitset<MAX_VERTICES, vertnum_t> render_vertex_flags;

	/* A new pass: the corner light of every segment is cleared. */
	if (++LevelUniqueLightState.Segment_dynamic_light_generation == 0)
	{
		/* After 2^32 - 1 passes, old generations could match again. */
		for (auto &l : LevelUniqueLightState.Segment_dynamic_light)
			l.generation = 0;
		LevelUniqueLightState.Segment_dynamic_light_generation = 1;
	}

	//	Create list of vertices that need to be looked at for setting of ambient light.
	auto &Headlight_dynamic_light = LevelUniqueLightState.Headlight_dynamic_light;
	auto &vcvertptr = LevelSharedSegmentState.get_vertex_state().get_vertices().vcptr;
	/* Sized by MAX_VERTICES, several hundred KB: too large for the stack,
	 * so keep one instance.  set_dynamic_light is not re-entrant.
	 */
	static render_vertex_list rvl;
	rvl.n_render_vertices = 0;
	auto &n_render_vertices = rvl.n_render_vertices;
	range_for (const auto segnum, partial_const_range(rstate.Render_list, rstate.N_render_segs))
	{
		if (segnum != segment_none) {
			auto &vp = Segments[segnum].verts;
			range_for (const auto vnum, vp)
			{
				auto &&b = render_vertex_flags[vnum];
				if (!b)
				{
					b = true;
					rvl.vertices[n_render_vertices] = vnum;
					rvl.positions[n_render_vertices] = *vcvertptr(vnum);
					n_render_vertices++;
					Headlight_dynamic_light[vnum] = {};
				}
			}
		}
	}

	reach_state.rendered.next();
	range_for (const auto segnum, partial_const_range(rstate.Render_list, rstate.N_render_segs))
		if (segnum != segment_none)
			reach_state.rendered.set(segnum);

	if (n_render_vertices)
	{
		rvl.all_bounds = {rvl.positions[0], rvl.positions[0]};
		for (unsigned vv_begin{0}; vv_begin < n_render_vertices; vv_begin += render_vertex_block_size)
		{
			const unsigned vv_end{std::min<unsigned>(vv_begin + render_vertex_block_size, n_render_vertices)};
			auto &b = rvl.block_bounds[vv_begin / render_vertex_block_size];
			b = {rvl.positions[vv_begin], rvl.positions[vv_begin]};
			for (unsigned vv{vv_begin + 1}; vv != vv_end; ++vv)
				b.include(rvl.positions[vv]);
			rvl.all_bounds.include(b.min);
			rvl.all_bounds.include(b.max);
		}
	}

	/* Collect the lights, then apply them dimmest first: the walks of
	 * all lights share one budget (light_reach_pass_budget).  The light
	 * of each vertex and corner is a sum, so the order changes nothing
	 * else.
	 */
	static std::array<pending_light, MAX_OBJECTS + MUZZLE_QUEUE_MAX> lights;
	std::size_t n_lights{0};
	collect_muzzle_flash_lights(lights.data(), n_lights);

	range_for (const auto &&obj, vcobjptridx)
	{
		const object &objp = obj;
		if (objp.type == object_type::OBJ_NONE)
			continue;
		const auto &&obj_light_emission = compute_light_emission(Robot_info, LevelUniqueLightState, Vclip, obj);

		if (const fix intensity{(obj_light_emission.r+obj_light_emission.g+obj_light_emission.b)/3}; intensity > 0)
			lights[n_lights++] = {
				.intensity = intensity,
				.emission = obj_light_emission,
				.objnum = obj,
				.muzzle = 0,
				.is_muzzle = false,
			};
	}
	const auto pending{std::span(lights).first(n_lights)};
	std::sort(pending.begin(), pending.end(), [](const pending_light &a, const pending_light &b) {
		return a.intensity < b.intensity;
	});
	light_reach_pass_budget budget{
		.segments = light_reach_pass_segments,
		.lights = n_lights,
		.min_segments = light_reach_min_segments,
		.max_segments = light_reach_max_segments,
		.max_hidden_hops = light_reach_max_hidden_hops,
	};
	for (const auto &l : pending)
	{
		if (l.is_muzzle)
		{
			auto &m{Muzzle_data[l.muzzle]};
			apply_light(l.emission, vcsegptridx(m.segnum), m.pos, rvl, object_none, budget);
		}
		else
		{
			const auto &&obj{vcobjptridx(l.objnum)};
			apply_light(l.emission, vcsegptridx(obj->segnum), obj->pos, rvl, obj, budget);
		}
	}
}

// ---------------------------------------------------------

#if DXX_BUILD_DESCENT == 2

void toggle_headlight_active(object &player)
{
	auto &player_info = player.ctype.player_info;
	if (+(player_info.powerup_flags & player_flag::headlight)) {
		player_info.powerup_flags ^= player_flag::headlight_on;
		if (+(Game_mode & GM_MULTI))
			multi_send_flags(player.id);
	}
}

namespace {

static fix compute_headlight_light_on_object(const d_level_unique_headlight_state &LevelUniqueHeadlightState, const object_base &objp)
{
	fix	light;

	//	Let's just illuminate players and robots for speed reasons, ok?
	if (objp.type != object_type::OBJ_ROBOT && objp.type != object_type::OBJ_PLAYER)
		return 0;

	light = 0;

	range_for (const object_base *const light_objp, partial_const_range(LevelUniqueHeadlightState.Headlights, LevelUniqueHeadlightState.Num_headlights))
	{
		const auto &&[dist, vec_to_obj] = vm_vec_normalize_quick_with_magnitude(vm_vec_build_sub(objp.pos, light_objp->pos));
		if (dist > 0) {
			const fix dot = vm_vec_build_dot(light_objp->orient.fvec, vec_to_obj);

			if (dot < F1_0/2)
				light += fixdiv(HEADLIGHT_SCALE, fixmul(HEADLIGHT_SCALE, dist));	//	Do the normal thing, but darken around headlight.
			else
				light += fixmul(fixmul(dot, dot), HEADLIGHT_SCALE)/8;
		}
	}
	return light;
}

}
#endif

}

namespace {

//compute the average dynamic light in a segment: the light of its corners
//(only lights that reach the segment) and the headlights on its vertices.
static g3s_lrgb compute_seg_dynamic_light(const ::dcx::d_level_unique_light_state &LevelUniqueLightState, const segnum_t segnum, const shared_segment &seg)
{
	auto &Headlight_dynamic_light = LevelUniqueLightState.Headlight_dynamic_light;
	const auto &&op = [&Headlight_dynamic_light](g3s_lrgb r, const vertnum_t v) {
		r.r += Headlight_dynamic_light[v].r;
		r.g += Headlight_dynamic_light[v].g;
		r.b += Headlight_dynamic_light[v].b;
		return r;
	};
	g3s_lrgb sum = std::accumulate(begin(seg.verts), end(seg.verts), g3s_lrgb{0, 0, 0}, op);
	if (const auto corners{LevelUniqueLightState.get_segment_dynamic_light(segnum)})
		for (const auto &c : *corners)
		{
			sum.r += c.r;
			sum.g += c.g;
			sum.b += c.b;
		}
	sum.r >>= 3;
	sum.g >>= 3;
	sum.b >>= 3;
	return sum;
}

static std::array<g3s_lrgb, MAX_OBJECTS> object_light;
static std::array<object_signature_t, MAX_OBJECTS> object_sig;
static int reset_lighting_hack;
}
const object *old_viewer;
#define LIGHT_RATE i2f(4) //how fast the light ramps up

void start_lighting_frame(const object &viewer)
{
	reset_lighting_hack = (&viewer != old_viewer);
	old_viewer = &viewer;
}

namespace dsx {

//compute the lighting for an object.  Takes a pointer to the object,
//and possibly a rotated 3d point.  If the point isn't specified, the
//object's center point is rotated.
g3s_lrgb compute_object_light(const d_level_unique_light_state &LevelUniqueLightState, const vcobjptridx_t obj)
{
	g3s_lrgb light;
	const vcobjidx_t objnum = obj;

	//First, get static (mono) light for this segment
	const cscusegment objsegp = vcsegptr(obj->segnum);
	light.r = light.g = light.b = objsegp.u.static_light;

	auto &os = object_sig[objnum];
	auto &ol = object_light[objnum];
	//Now, maybe return different value to smooth transitions
	if (!reset_lighting_hack && os == obj->signature)
	{
		fix frame_delta;
		g3s_lrgb delta_light;

		delta_light.r = light.r - ol.r;
		delta_light.g = light.g - ol.g;
		delta_light.b = light.b - ol.b;

		frame_delta = fixmul(LIGHT_RATE,FrameTime);

		if (abs(((delta_light.r+delta_light.g+delta_light.b)/3)) <= frame_delta)
		{
			ol = light;		//we've hit the goal
		}
		else
		{
			if (((delta_light.r+delta_light.g+delta_light.b)/3) < 0)
				frame_delta = -frame_delta;
			ol.r += frame_delta;
			ol.g += frame_delta;
			ol.b += frame_delta;
			light = ol;
		}

	}
	else //new object, initialize 
	{
		os = obj->signature;
		ol = light;
	}

	//Finally, add in dynamic light for this segment
	const auto &&seg_dl = compute_seg_dynamic_light(LevelUniqueLightState, obj->segnum, objsegp);
#if DXX_BUILD_DESCENT == 2
	//Next, add in (NOTE: WHITE) headlight on this object
	const fix mlight = compute_headlight_light_on_object(LevelUniqueLightState, obj);
	light.r += mlight;
	light.g += mlight;
	light.b += mlight;
#endif
 
	light.r += seg_dl.r;
	light.g += seg_dl.g;
	light.b += seg_dl.b;

	return light;
}

}
