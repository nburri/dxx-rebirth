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

static void apply_light(const g3s_lrgb obj_light_emission, const vcsegptridx_t obj_seg, const vms_vector &obj_pos, const render_vertex_list &rvl, const icobjptridx_t objnum)
{
	auto &LevelSharedVertexState = LevelSharedSegmentState.get_vertex_state();
	auto &Vertices = LevelSharedVertexState.get_vertices();
	if (((obj_light_emission.r+obj_light_emission.g+obj_light_emission.b)/3) > 0)
	{
		fix obji_64 = ((obj_light_emission.r+obj_light_emission.g+obj_light_emission.b)/3)*64;
		sbyte is_marker{0};
#if DXX_BUILD_DESCENT == 2
		if (objnum && objnum->type == object_type::OBJ_MARKER)
				is_marker = 1;
#endif

		auto &Dynamic_light = LevelUniqueLightState.Dynamic_light;
		auto &vcvertptr = Vertices.vcptr;
		// for pretty dim sources, only process vertices in object's own segment.
		//	12/04/95, MK, markers only cast light in own segment.
		if ((abs(obji_64) <= F1_0*8) || is_marker) {
			auto &vp = obj_seg->verts;

			range_for (const auto vertnum, vp)
			{
				fix			dist;
				auto &vertpos = *vcvertptr(vertnum);
				dist = vm_vec_dist_quick(obj_pos, vertpos);
				dist = fixmul(dist/4, dist/4);
				if (dist < abs(obji_64)) {
					if (dist < MIN_LIGHT_DIST)
						dist = MIN_LIGHT_DIST;

					add_light_div(Dynamic_light[vertnum], obj_light_emission, dist);
				}
			}
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
			const auto apply_light_to_vertices = [&](const unsigned vv_begin, const unsigned vv_end) {
				for (unsigned vv{vv_begin}; vv != vv_end; ++vv)
				{
					const auto vertnum = rvl.vertices[vv];
					auto &vertpos = rvl.positions[vv];
					fix dist = vm_vec_dist_quick(obj_pos, vertpos);

					if ((dist >> headlight_shift) < abs(obji_64)) {

						if (dist < MIN_LIGHT_DIST)
							dist = MIN_LIGHT_DIST;

						if (headlight_shift && objnum)
						{
							fix dot;
							// MK, Optimization note: You compute distance about 15 lines up, this is partially redundant
							const auto vec_to_point = vm_vec_normalized_quick(vm_vec_build_sub(vertpos, obj_pos));
							dot = vm_vec_build_dot(vec_to_point, objnum->orient.fvec);
							if (dot < F1_0/2)
							{
								// Do the normal thing, but darken around headlight.
								add_light_div(Dynamic_light[vertnum], obj_light_emission, fixmul(HEADLIGHT_SCALE, dist));
							}
							else
							{
								if (!(Game_mode & GM_MULTI) || dist < max_headlight_dist)
								{
									add_light_dot_square(Dynamic_light[vertnum], obj_light_emission, dot);
								}
							}
						}
						else
						{
							add_light_div(Dynamic_light[vertnum], obj_light_emission, dist);
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
			const fix light_reach{abs(obji_64)};
			const auto out_of_reach = [&obj_pos, headlight_shift, light_reach](const render_vertex_bounds &b) {
				return (quick_distance_lower_bound(obj_pos, b) >> headlight_shift) >= light_reach;
			};
			if (quick_distance_upper_bound(obj_pos, rvl.all_bounds) > INT32_MAX)
				apply_light_to_vertices(0, n_render_vertices);
			else if (!out_of_reach(rvl.all_bounds))
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
static void cast_muzzle_flash_light(const render_vertex_list &rvl)
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
				apply_light(ml, vcsegptridx(i.segnum), i.pos, rvl, object_none);
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

	//	Create list of vertices that need to be looked at for setting of ambient light.
	auto &Dynamic_light = LevelUniqueLightState.Dynamic_light;
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
					Dynamic_light[vnum] = {};
				}
			}
		}
	}

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

	cast_muzzle_flash_light(rvl);

	range_for (const auto &&obj, vcobjptridx)
	{
		const object &objp = obj;
		if (objp.type == object_type::OBJ_NONE)
			continue;
		const auto &&obj_light_emission = compute_light_emission(Robot_info, LevelUniqueLightState, Vclip, obj);

		if (((obj_light_emission.r+obj_light_emission.g+obj_light_emission.b)/3) > 0)
			apply_light(obj_light_emission, vcsegptridx(objp.segnum), objp.pos, rvl, obj);
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

//compute the average dynamic light in a segment.  Takes the segment number
static g3s_lrgb compute_seg_dynamic_light(const per_vertex_array<g3s_lrgb> &Dynamic_light, const shared_segment &seg)
{
	const auto &&op = [&Dynamic_light](g3s_lrgb r, const vertnum_t v) {
		r.r += Dynamic_light[v].r;
		r.g += Dynamic_light[v].g;
		r.b += Dynamic_light[v].b;
		return r;
	};
	g3s_lrgb sum = std::accumulate(begin(seg.verts), end(seg.verts), g3s_lrgb{0, 0, 0}, op);
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
	auto &Dynamic_light = LevelUniqueLightState.Dynamic_light;
	const auto &&seg_dl = compute_seg_dynamic_light(Dynamic_light, objsegp);
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
