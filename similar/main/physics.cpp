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
 * Code for flying through the mines
 *
 */


#include <stdio.h>
#include <stdlib.h>
#include <cmath>

#include "joy.h"
#include "dxxerror.h"

#include "inferno.h"
#include "segment.h"
#include "object.h"
#include "physics.h"
#include "robot.h"
#include "game.h"
#include "collide.h"
#include "fvi.h"
#include "gameseg.h"
#include "wall.h"
#include "laser.h"
#if DXX_BUILD_DESCENT == 2
#include "bm.h"
#include "player.h"
#define MAX_OBJECT_VEL	i2f(100)
#endif

#include "d_construct.h"
#include "d_levelstate.h"
#include "compiler-range_for.h"

//Global variables for physics system

constexpr fix ROLL_RATE{0x2000};
constexpr fixang DAMP_ANG{0x400};	//min angle to bank
constexpr fix TURNROLL_SCALE{0x4ec4/2};

namespace {

[[nodiscard]]
static fixang multiply_with_clamp_to_fixang(const fix rotvel, const fix frametime)
{
	/* Express the minimum and maximum as `fix` so that the result of `fixmul`
	 * is compared without truncation.  After `std::clamp` is applied, conversion
	 * to `fixang` cannot modify the value, because all possible values in
	 * [`min`, `max`] are representable in `fixang`, and `std::clamp` is
	 * guaranteed to return a value in [`min`, `max`].
	 */
	static constexpr fix min{std::numeric_limits<fixang>::min()};
	static constexpr fix max{std::numeric_limits<fixang>::max()};
	return std::clamp<fix>({fixmul(rotvel, frametime)}, {min}, {max});
}

static void do_physics_align_object(object_base &obj)
{
	fix largest_d{INT32_MIN};
	const shared_side *best_side{nullptr};
	// bank player according to segment orientation

	//find side of segment that player is most alligned with

	range_for (auto &i, vcsegptr(obj.segnum)->shared_segment::sides)
	{
		const auto d{vm_vec_build_dot(i.normals[0], obj.orient.uvec)};
		if (largest_d < d)
		{
			largest_d = d;
			best_side = &i;
		}
	}

	// new player leveling code: use normal of side closest to our up vec
	if (!best_side)
		return;
	const vms_vector desired_upvec{
		get_side_is_quad(*best_side)
			? vm_vec_normalized(vm_vec_build_avg(best_side->normals[0], best_side->normals[1]))
			: best_side->normals[0]
	};

	auto &levelling_remainder{obj.mtype.phys_info.angle_remainder.levelling};
	if (labs(vm_vec_build_dot(desired_upvec, obj.orient.fvec)) < f1_0 / 2)
	{
		const auto temp_matrix{vm_vector_to_matrix_u(obj.orient.fvec, desired_upvec)};

		auto delta_ang{vm_vec_delta_ang(obj.orient.uvec, temp_matrix.uvec, obj.orient.fvec)};
		delta_ang += obj.mtype.phys_info.turnroll;

		if (abs(delta_ang) > DAMP_ANG) {
			/* Carry the fraction of the rate limit that does not fit in a
			 * `fixang`, but only while the rate limit applies.
			 */
			auto remainder{levelling_remainder};
			const fixang uncapped_roll_ang{fixmul_to_fixang_with_remainder(FrameTime, ROLL_RATE, remainder)};
			const bool rate_limited{!(uncapped_roll_ang > abs(delta_ang))};
			levelling_remainder = rate_limited ? remainder : 0;
			const fixang roll_ang{
				!rate_limited
					? delta_ang
					: (delta_ang < 0
						? static_cast<fixang>(-uncapped_roll_ang)
						: static_cast<fixang>(uncapped_roll_ang))
			};
			const auto &&rotmat{vm_angles_2_matrix(
				vms_angvec{
					.p = fixang{0},
					.b = roll_ang,
					.h = fixang{0}
				})};
			obj.orient = vm_matrix_x_matrix(obj.orient, rotmat);
			return;
		}
	}
	levelling_remainder = 0;
}

[[nodiscard]]
static fixang set_object_turnroll(object_base &obj, const fix frametime)
{
	const fixang desired_bank{multiply_with_clamp_to_fixang({-obj.mtype.phys_info.rotvel.y}, {TURNROLL_SCALE})};
	auto &turnroll_remainder{obj.mtype.phys_info.angle_remainder.turnroll};
	if (const fix delta_ang{desired_bank - obj.mtype.phys_info.turnroll})
	{
		/* Carry the fraction of the rate limit that does not fit in a
		 * `fixang`, but only while the rate limit applies.
		 */
		auto remainder{turnroll_remainder};
		const fixang raw_max_roll{fixmul_to_fixang_with_remainder(ROLL_RATE, frametime, remainder)};
		turnroll_remainder = (abs(delta_ang) > raw_max_roll) ? remainder : 0;
		/* Casting to `fixang` is safe:
		 * - `raw_max_roll` is a non-negative `fixang`, since `ROLL_RATE` is
		 *   positive, `frametime` is positive and the remainder is
		 *   non-negative, so `raw_max_roll` is in the range [`0`,
		 *   `INT16_MAX`].
		 * - `-raw_max_roll` is then in the range [`-INT16_MAX`, `0`].
		 * - Therefore, [`-raw_max_roll`, `raw_max_roll`] is at worst
		 *   [`-INT16_MAX`, `INT16_MAX`].
		 * - `fixang` can represent all values in [`-INT16_MAX`, `INT16_MAX`],
		 *   so the conversion does not change the value.
		 */
		const auto max_roll{static_cast<fixang>(std::clamp<fix>(delta_ang, -raw_max_roll, raw_max_roll))};
		const auto updated_turnroll{obj.mtype.phys_info.turnroll + max_roll};
		static constexpr fix minfixang{std::numeric_limits<fixang>::min()};
		static constexpr fix maxfixang{std::numeric_limits<fixang>::max()};
		obj.mtype.phys_info.turnroll = {static_cast<fixang>(std::clamp<fix>(updated_turnroll, minfixang, maxfixang))};
	}
	else
		turnroll_remainder = 0;
	return obj.mtype.phys_info.turnroll;
}

}

#define MAX_IGNORE_OBJS 100

#ifndef NDEBUG
int Total_retries{0}, Total_sims=0;
int Dont_move_ai_objects{0};
#endif

#define FT (f1_0/64)

namespace {

/* Thrust and drag used to be applied in whole steps of `FT` (1/64 s), and
 * the rest of the frame was scaled linearly.  Every frame shorter than `FT`
 * took only the linear path, so top speed and turn rate depended on the
 * frame rate.  The reference behavior is that of the old code at a steady
 * 200 fps (`FrameTime` = F1_0 / 200 = 327), the long-time default frame
 * rate cap, where each frame did
 *
 *     v = (v + accel * k) * R
 *     k = fixdiv(327, FT)
 *     R = 1 - fixmul(k, drag)
 *
 * Applying that recurrence `n` times gives the closed form
 *
 *     v = v * R ** n + steady * (1 - R ** n)
 *     steady = accel * k * R / (1 - R)
 *
 * which is also defined for fractional `n`.  Using it with
 * `n = FrameTime / 327` gives the same result for any frame rate, such that
 * N short frames are equivalent to one long frame, and reproduces the old
 * code at 200 fps up to rounding.
 */
constexpr fix drag_reference_frametime{F1_0 / 200};
/* `fixdiv(drag_reference_frametime, FT)`, which is exact */
constexpr fix drag_reference_fraction_of_step{drag_reference_frametime * (F1_0 / FT)};
static_assert(drag_reference_fraction_of_step == 20928);

/* Frame rate independent description of the drag model for one drag
 * value: over `n` reference frames, velocity decays by
 * `exp(-decay_per_frame * n)` toward `accel * steady_state_per_accel`.
 */
struct drag_model
{
	double decay_per_frame;
	double steady_state_per_accel;
};

[[nodiscard]]
static drag_model build_drag_model(const fix drag)
{
	/* Compute these as the old code did at 200 fps, including its
	 * truncation, so that 200 fps is unchanged.
	 */
	const fix reference_drag{fixmul(drag_reference_fraction_of_step, drag)};
	constexpr double accel_per_frame{static_cast<double>(drag_reference_fraction_of_step) / F1_0};
	if (reference_drag <= 0)
		/* Drag too small to have any effect at the reference frame time:
		 * no decay, and no steady state under thrust.
		 */
		return {0, 0};
	const double drag_per_frame{static_cast<double>(reference_drag) / F1_0};
	if (drag_per_frame < 1)
	{
		const double retained_per_frame{1 - drag_per_frame};
		return {-std::log(retained_per_frame), accel_per_frame * retained_per_frame / drag_per_frame};
	}
	/* For a drag this high, the old code zeroed or negated the velocity
	 * every frame, which cannot be made frame rate independent.  This is
	 * only possible for rotational drag, which is 5/2 of the object's drag,
	 * and only if that drag is 1.25 or more.  Use a continuous decay with
	 * the same drag per reference frame instead.  It lets thrust act, with
	 * a steady state of `accel * k / drag_per_frame`.
	 */
	return {drag_per_frame, accel_per_frame / drag_per_frame};
}

struct drag_integration
{
	double retained;	// fraction of the velocity that remains after the frame
	double accel_gain;	// velocity gained per unit of per-step acceleration
};

[[nodiscard]]
static drag_integration build_drag_integration(const fix drag, const fix frametime)
{
	const double frames{static_cast<double>(frametime) / drag_reference_frametime};
	const auto model{build_drag_model(drag)};
	if (!model.decay_per_frame)
		return {1, frames * (static_cast<double>(drag_reference_fraction_of_step) / F1_0)};
	const double retained{std::exp(-model.decay_per_frame * frames)};
	return {retained, model.steady_state_per_accel * (1 - retained)};
}

/* Few distinct drag values are in use at a time, and `FrameTime` is the
 * same for all objects in a frame, so cache the result of
 * `build_drag_integration`.  This avoids calling `std::exp` and `std::log`
 * for almost every object, and makes all objects with the same drag use
 * the same factors.
 */
[[nodiscard]]
static const drag_integration &get_drag_integration(const fix drag, const fix frametime)
{
	struct cache_entry
	{
		fix drag, frametime;
		drag_integration integration;
	};
	static std::array<cache_entry, 32> cache{};
	auto &e{cache[(static_cast<uint32_t>(drag) * UINT32_C(2654435761)) >> 27]};
	if (e.drag != drag || e.frametime != frametime)
		e = {drag, frametime, build_drag_integration(drag, frametime)};
	return e.integration;
}

[[nodiscard]]
static fix apply_drag_integration(const drag_integration &di, const fix v, const fix accel, int16_t &remainder)
{
	/* Carry the fractional part of the velocity to the next frame, instead
	 * of rounding it away every frame.  That keeps the result independent
	 * of how the time is split into frames: there is no rounding bias of
	 * the top speed, no jitter at the steady state, and a coasting object
	 * comes to rest (the exact velocity falls below one unit, which
	 * truncates to zero) after the same time at any frame rate.
	 */
	constexpr double remainder_scale{32768};
	const double exact{std::clamp<double>((v + remainder / remainder_scale) * di.retained + accel * di.accel_gain, std::numeric_limits<fix>::min(), std::numeric_limits<fix>::max())};
	const double whole{std::trunc(exact)};
	remainder = static_cast<int16_t>(std::clamp<double>(std::round((exact - whole) * remainder_scale), -INT16_MAX, INT16_MAX));
	return static_cast<fix>(whole);
}

static void apply_drag_integration(const drag_integration &di, vms_vector &v, const vms_vector &accel, std::array<int16_t, 3> &remainder)
{
	v.x = apply_drag_integration(di, v.x, accel.x, remainder[0]);
	v.y = apply_drag_integration(di, v.y, accel.y, remainder[1]);
	v.z = apply_drag_integration(di, v.z, accel.z, remainder[2]);
}

}

//	-----------------------------------------------------------------------------------------------------------
// add rotational velocity & acceleration
namespace dsx {
namespace {
static void do_physics_sim_rot(object_base &obj)
{
	Assert(FrameTime > 0);	//Get MATT if hit this!

	if (const physics_info &pi{obj.mtype.phys_info}; !(pi.rotvel.x || pi.rotvel.y || pi.rotvel.z || pi.rotthrust.x || pi.rotthrust.y || pi.rotthrust.z))
		return;

	if (obj.mtype.phys_info.drag)
	{
		const fix drag{(obj.mtype.phys_info.drag * 5) / 2};

		if (obj.mtype.phys_info.flags & PF_USES_THRUST)
		{
			const auto accel{vm_vec_copy_scale(obj.mtype.phys_info.rotthrust, fixdiv(f1_0, obj.mtype.phys_info.mass))};
			apply_drag_integration(get_drag_integration(drag, FrameTime), obj.mtype.phys_info.rotvel, accel, obj.mtype.phys_info.velocity_remainder.rotvel);
		}
		else
#if DXX_BUILD_DESCENT == 2
			if (! (obj.mtype.phys_info.flags & PF_FREE_SPINNING))
#endif
		{
			apply_drag_integration(get_drag_integration(drag, FrameTime), obj.mtype.phys_info.rotvel, vms_vector{}, obj.mtype.phys_info.velocity_remainder.rotvel);
		}

	}

	//now rotate object

	//unrotate object for bank caused by turn
	const fixang old_turnroll{obj.mtype.phys_info.turnroll};
	if (old_turnroll)
	{
		const auto &&rotmat{vm_angles_2_matrix(
			vms_angvec{
				.p = fixang{0},
				.b = static_cast<fixang>(-old_turnroll),
				.h = fixang{0}
			})};
		obj.orient = vm_matrix_x_matrix(obj.orient, rotmat);
	}

	const auto frametime{FrameTime};

	/* Carry the part of each angle that does not fit in a `fixang` to the
	 * next frame.  Otherwise, up to one `fixang` per axis would be lost every
	 * frame, so slow rotations would be lost entirely, and more so at higher
	 * frame rates.
	 */
	auto &rotation_remainder{obj.mtype.phys_info.angle_remainder.rotation};
	obj.orient = vm_matrix_x_matrix(obj.orient, vm_angles_2_matrix(
			vms_angvec{
				.p = fixmul_to_fixang_with_remainder(obj.mtype.phys_info.rotvel.x, frametime, rotation_remainder[0]),
				.b = fixmul_to_fixang_with_remainder(obj.mtype.phys_info.rotvel.z, frametime, rotation_remainder[1]),
				.h = fixmul_to_fixang_with_remainder(obj.mtype.phys_info.rotvel.y, frametime, rotation_remainder[2])
			}));

	//re-rotate object for bank caused by turn
	/* The call to `set_object_turnroll` may have changed
	 * `.phys_info.turnroll`, so use the returned turnroll, which may differ
	 * from `old_turnroll`.
	 */
	if (const fixang turnroll{(obj.mtype.phys_info.flags & PF_TURNROLL) ? set_object_turnroll(obj, frametime) : old_turnroll})
	{
		obj.orient = vm_matrix_x_matrix(obj.orient, vm_angles_2_matrix(
				vms_angvec{
					.p = fixang{0},
					.b = turnroll,
					.h = fixang{0}
				}));
	}
	check_and_fix_matrix(obj.orient);
}

// On joining edges fvi tends to get inaccurate as hell. Approach is to check if the object interects with the wall and if so, move away from it.
static void fix_illegal_wall_intersection(const vmobjptridx_t obj)
{
	auto &LevelSharedVertexState = LevelSharedSegmentState.get_vertex_state();
	auto &Objects = LevelUniqueObjectState.Objects;
	auto &Vertices = LevelSharedVertexState.get_vertices();
	auto &vmobjptr = Objects.vmptr;
	if (!(obj->type == object_type::OBJ_PLAYER || obj->type == object_type::OBJ_ROBOT))
		return;

	auto &vcvertptr = Vertices.vcptr;
	const auto &&hresult{sphere_intersects_wall(vcsegptridx, vcvertptr, obj->pos, vcsegptridx(obj->segnum), obj->size)};
	if (hresult.seg)
	{
		vm_vec_scale_add2(obj->pos, hresult.seg->sides[hresult.side].normals[0], FrameTime*10);
		update_object_seg(vmobjptr, LevelSharedSegmentState, LevelUniqueSegmentState, obj);
	}
}

class ignore_objects_array_t
{
	using object_index_array = std::array<vcobjidx_t, MAX_IGNORE_OBJS>;
	object_index_array::iterator iter_first_uninitialized;
	std::array<std::byte, sizeof(object_index_array)> storage;
	object_index_array &array()
	{
		return *reinterpret_cast<object_index_array *>(&storage);
	}
	const object_index_array &array() const
	{
		return *reinterpret_cast<const object_index_array *>(&storage);
	}
public:
	ignore_objects_array_t() :
		iter_first_uninitialized(array().begin())
	{
	}
	vcobjidx_t *push_back(const vcobjidx_t o)
	{
		if (likely(iter_first_uninitialized != array().end()))
			return std::construct_at(std::to_address(iter_first_uninitialized++), o);
		return nullptr;
	}
	operator std::ranges::subrange<const vcobjidx_t *>() const
	{
		return {array().begin(), iter_first_uninitialized};
	}
};

}

//	-----------------------------------------------------------------------------------------------------------
//Simulate a physics object for this frame
window_event_result do_physics_sim(const d_robot_info_array &Robot_info, const vmobjptridx_t obj, const vms_vector &obj_previous_position, phys_visited_seglist *const phys_segs)
{
	auto &LevelSharedVertexState = LevelSharedSegmentState.get_vertex_state();
	auto &Objects = LevelUniqueObjectState.Objects;
	auto &Vertices = LevelSharedVertexState.get_vertices();
	auto &vcobjptr = Objects.vcptr;
	auto &vmobjptr = Objects.vmptr;
	ignore_objects_array_t ignore_obj_list;
	fvi_info hit_info;
	fix moved_time;			//how long objected moved before hit something
	const auto orig_segnum{obj->segnum};
	bool Player_ScrapeFrame=false;
	auto result = window_event_result::handled;
#if DXX_BUILD_DESCENT == 2
	bool bounced{};
	auto &TmapInfo = LevelUniqueTmapInfoState.TmapInfo;
#endif

	Assert(obj->movement_source == object::movement_type::physics);

#ifndef NDEBUG
	if (Dont_move_ai_objects)
		if (obj->control_source == object::control_type::ai)
			return window_event_result::ignored;
#endif

	do_physics_sim_rot(obj);

	if (const physics_info &pi{obj->mtype.phys_info}; !(pi.velocity.x || pi.velocity.y || pi.velocity.z || pi.thrust.x || pi.thrust.y || pi.thrust.z))
		return window_event_result::ignored;

	fix sim_time{FrameTime};
	const vms_vector start_pos{obj->pos};

		//if uses thrust, cannot have zero drag
	Assert(!(obj->mtype.phys_info.flags&PF_USES_THRUST) || obj->mtype.phys_info.drag!=0);

	//do thrust & drag
	if (const fix drag{obj->mtype.phys_info.drag})
	{
		auto &di{get_drag_integration(drag, FrameTime)};

		if (obj->mtype.phys_info.flags & PF_USES_THRUST) {

			const auto accel{vm_vec_copy_scale(obj->mtype.phys_info.thrust,fixdiv(f1_0,obj->mtype.phys_info.mass))};
			apply_drag_integration(di, obj->mtype.phys_info.velocity, accel, obj->mtype.phys_info.velocity_remainder.velocity);
		}
		else
			apply_drag_integration(di, obj->mtype.phys_info.velocity, vms_vector{}, obj->mtype.phys_info.velocity_remainder.velocity);
	}

	int count{0};
	auto &vcvertptr = Vertices.vcptr;
	auto &Walls = LevelUniqueWallSubsystemState.Walls;
	auto &vcwallptr = Walls.vcptr;
	auto fate{fvi_hit_type::None};
	bool try_again{};
	bool obj_stopped{};
	do {
		try_again = 0;

		//Move the object
		const auto frame_vec{vm_vec_copy_scale(obj->mtype.phys_info.velocity, sim_time)};

		if (frame_vec == vms_vector{})
			break;

		count++;

		//	If retry count is getting large, then we are trying to do something stupid.
		if (count > 8) break; // in original code this was 3 for all non-player objects. still leave us some limit in case fvi goes apeshit.

		const auto new_pos = vm_vec_build_add(obj->pos,frame_vec);
		int flags{0};
		if (obj->type == object_type::OBJ_WEAPON)
			flags |= FQ_TRANSPOINT;

		if (phys_segs)
			flags |= FQ_GET_SEGLIST;

		fate = find_vector_intersection(fvi_query{
			obj->pos,
			new_pos,
			ignore_obj_list,
			&LevelUniqueObjectState,
			&Robot_info,
			flags,
			obj,
		}, obj->segnum, obj->size, hit_info);
		//	Matt: Mike's hack.
		if (fate == fvi_hit_type::Object)
		{
			auto &objp = *vcobjptr(hit_info.hit_object);

			if ((objp.type == object_type::OBJ_WEAPON && is_proximity_bomb_or_player_smart_mine(get_weapon_id(objp))) ||
				objp.type == object_type::OBJ_POWERUP) // do not increase count for powerups since they *should* not change our movement
				count--;
		}

#ifndef NDEBUG
		if (fate == fvi_hit_type::BadP0)
			Int3();
#endif

		if (phys_segs && !hit_info.seglist.empty())
		{
			auto n_phys_segs{phys_segs->nsegs};
			if (n_phys_segs && phys_segs->seglist[n_phys_segs - 1] == hit_info.seglist[0])
				n_phys_segs--;
			range_for (const auto &hs, hit_info.seglist)
			{
				if (!(n_phys_segs < phys_segs->seglist.size() - 1))
					break;
				phys_segs->seglist[n_phys_segs++] = hs;
			}
			phys_segs->nsegs = n_phys_segs;
		}

		const vms_vector ipos{hit_info.hit_pnt};		//position after this frame
		const auto iseg{hit_info.hit_seg};
		const sidenum_t WallHitSide{hit_info.hit_side};
		const segnum_t WallHitSeg{hit_info.hit_side_seg};

		if (iseg==segment_none) {		//some sort of horrible error
			if (obj->type == object_type::OBJ_WEAPON)
				obj->flags |= OF_SHOULD_BE_DEAD;
			break;
		}

		assert(!(fate == fvi_hit_type::Wall && (WallHitSeg == segment_none || WallHitSeg > Highest_segment_index)));

		const vms_vector save_pos{obj->pos};			//save the object's position
		const auto save_seg{obj->segnum};

		// update object's position and segment number
		obj->pos = ipos;

		const auto &&obj_segp = Segments.vmptridx(iseg);
		if ( iseg != obj->segnum )
			obj_relink(vmobjptr, Segments.vmptr, obj, obj_segp);

		//if start point not in segment, move object to center of segment
		if (get_seg_masks(vcvertptr, obj->pos, Segments.vcptr(obj->segnum), 0).centermask != sidemask_t{})
		{
			auto n{find_object_seg(LevelSharedSegmentState, LevelUniqueSegmentState, obj)};
			if (n == segment_none)
			{
				//Int3();
				if (obj->type == object_type::OBJ_PLAYER && (n = find_point_seg(LevelSharedSegmentState, LevelUniqueSegmentState, obj_previous_position, obj_segp DXX_lighting_hack_pass_parameter)) != segment_none)
				{
					obj->pos = obj_previous_position;
					obj_relink(vmobjptr, Segments.vmptr, obj, n);
				}
				else {
					obj->pos = compute_segment_center(vcvertptr, obj_segp);
					obj->pos.x += obj;
				}
				if (obj->type == object_type::OBJ_WEAPON)
					obj->flags |= OF_SHOULD_BE_DEAD;
			}
			return window_event_result::ignored;
		}

		//calulate new sim time
		{
			//vms_vector moved_vec;
			vms_vector moved_vec_n;
			const auto actual_dist{vm_vec_normalized_dir(moved_vec_n, obj->pos, save_pos)};

			if (fate == fvi_hit_type::Wall && vm_vec_build_dot(moved_vec_n,frame_vec) < 0)
			{		//moved backwards

				//don't change position or sim_time

				obj->pos = save_pos;
		
				//iseg = obj->segnum;		//don't change segment

				obj_relink(vmobjptr, Segments.vmptr, obj, Segments.vmptridx(save_seg));

				moved_time = 0;
			}
			else {
				const auto attempted_dist{vm_vec_mag(frame_vec)};
				const auto old_sim_time{sim_time};
				sim_time = fixmuldiv(sim_time,attempted_dist-actual_dist,attempted_dist);

				moved_time = old_sim_time - sim_time;

				if (sim_time < 0 || sim_time>old_sim_time) {
					sim_time = old_sim_time;
					//WHY DOES THIS HAPPEN??

					moved_time = 0;
				}
			}
		}


		switch( fate )		{

			case fvi_hit_type::Wall:		{
				fix hit_speed{0};

				// Find hit speed	

				const auto moved_v{vm_vec_build_sub(obj->pos, save_pos)};
				auto wall_part{vm_vec_build_dot(moved_v,hit_info.hit_wallnorm)};

				if ((wall_part != 0 && moved_time>0 && (hit_speed=-fixdiv(wall_part,moved_time))>0) || obj->type == object_type::OBJ_WEAPON || obj->type == object_type::OBJ_DEBRIS)
					result = collide_object_with_wall(
#if DXX_BUILD_DESCENT == 2
						LevelSharedSegmentState.DestructibleLights,
#endif
						Robot_info, obj, hit_speed, Segments.vmptridx(WallHitSeg), WallHitSide, hit_info.hit_pnt);
				/*
				 * Due to the nature of this loop, it's possible that a local player may receive scrape damage multiple times in one frame.
				 * Check if we received damage and do not apply more damage (nor produce damage sounds/flashes/bumps, etc) for the rest of the loop.
				 * It's possible that other walls later in the loop would still be valid for scraping but due to the generalized outcome, this should be negligible (practical wall sliding is handled below).
				 * NOTE: Remote players will return false and never receive damage. But since we handle only one object (remote or local) per loop, this is no problem. 
				 */
				if (obj->type == object_type::OBJ_PLAYER && Player_ScrapeFrame == false)
					Player_ScrapeFrame = scrape_player_on_wall(obj, Segments.vmptridx(WallHitSeg), WallHitSide, hit_info.hit_pnt);

				Assert( WallHitSeg != segment_none );

				if ( !(obj->flags&OF_SHOULD_BE_DEAD) )	{
#if DXX_BUILD_DESCENT == 2
					if (!cheats.bouncyfire)
#endif
					Assert(!(obj->mtype.phys_info.flags & PF_STICK && obj->mtype.phys_info.flags & PF_BOUNCE));	//can't be bounce and stick

#if DXX_BUILD_DESCENT == 1
					/*
					 * Force fields are not supported in Descent 1.  Use
					 * this as a placeholder to make the code match the
					 * force field handling in Descent 2.
					 */
					const auto forcefield_bounce{false};
#elif DXX_BUILD_DESCENT == 2
					const unique_segment &useg = vcsegptr(WallHitSeg);
					auto &uside = useg.sides[WallHitSide];
					const auto texture1_index{get_texture_index(uside.tmap_num)};
					if (texture1_index >= TmapInfo.size()) [[unlikely]]
						break;
					const auto forcefield_bounce{(TmapInfo[texture1_index].flags & tmapinfo_flag::force_field)};		//bounce off a forcefield
					bool check_vel{};
#endif

					if (!forcefield_bounce && (obj->mtype.phys_info.flags & PF_STICK)) {		//stop moving

						LevelUniqueStuckObjectState.add_stuck_object(vcwallptr, obj, Segments.vmptr(WallHitSeg), WallHitSide);

						obj->mtype.phys_info.velocity = {};
						obj_stopped = 1;
						try_again = 0;
					}
					else {					// Slide object along wall

						wall_part = vm_vec_build_dot(hit_info.hit_wallnorm,obj->mtype.phys_info.velocity);

						// if wall_part, make sure the value is sane enough to get usable velocity computed
						if (wall_part < 0 && wall_part > -f1_0) wall_part = -f1_0;
						if (wall_part > 0 && wall_part < f1_0) wall_part = f1_0;

						if (+forcefield_bounce || (obj->mtype.phys_info.flags & PF_BOUNCE)) {		//bounce off wall
							wall_part *= 2;	//Subtract out wall part twice to achieve bounce

#if DXX_BUILD_DESCENT == 2
							if (+forcefield_bounce) {
								check_vel = 1;				//check for max velocity
								if (obj->type == object_type::OBJ_PLAYER)
									wall_part *= 2;		//player bounce twice as much
							}

							if ( obj->mtype.phys_info.flags & PF_BOUNCES_TWICE) {
								Assert(obj->mtype.phys_info.flags & PF_BOUNCE);
								if (obj->mtype.phys_info.flags & PF_BOUNCED_ONCE)
									obj->mtype.phys_info.flags &= ~(PF_BOUNCE+PF_BOUNCED_ONCE+PF_BOUNCES_TWICE);
								else
									obj->mtype.phys_info.flags |= PF_BOUNCED_ONCE;
							}
							bounced = 1;		//this object bounced
#endif
						}

						vm_vec_scale_add2(obj->mtype.phys_info.velocity,hit_info.hit_wallnorm,-wall_part);

#if DXX_BUILD_DESCENT == 2
						if (check_vel) {
							fix vel = vm_vec_mag_quick(obj->mtype.phys_info.velocity);

							if (vel > MAX_OBJECT_VEL)
								vm_vec_scale(obj->mtype.phys_info.velocity,fixdiv(MAX_OBJECT_VEL,vel));
						}

						if (bounced && obj->type == object_type::OBJ_WEAPON)
						{
							/* Copy `objp.orient.uvec` into a temporary and pass the temporary,
							 * since it is undefined behavior to access `objp.orient` after the old
							 * object is destroyed until the new value is constructed.
							 */
							auto old_uvec{obj->orient.uvec};
							reconstruct_at(obj->orient, vm_vector_to_matrix_u, obj->mtype.phys_info.velocity, std::move(old_uvec));
						}
#endif

						try_again = 1;
					}
				}

				break;
			}

			case fvi_hit_type::Object:		{
				// Mark the hit object so that on a retry the fvi code
				// ignores this object.

				Assert(hit_info.hit_object != object_none);
				const auto old_vel{obj->mtype.phys_info.velocity};
				//	Calculcate the hit point between the two objects.
				{
					const auto &&hit = obj.absolute_sibling(hit_info.hit_object);
					const auto &ppos0 = hit->pos;
					const auto &ppos1 = obj->pos;
					const fix size0{hit->size};
					const fix size1{obj->size};
					Assert(size0+size1 != 0);	// Error, both sizes are 0, so how did they collide, anyway?!?
					auto pos_hit{vm_vec_scale_add(ppos0, vm_vec_build_sub(ppos1, ppos0), fixdiv(size0, size0 + size1))};

					collide_two_objects(Robot_info, obj, hit, pos_hit);
				}

				// Let object continue its movement
				if ( !(obj->flags&OF_SHOULD_BE_DEAD)  )	{
					//obj->pos = save_pos;

					if ((obj->mtype.phys_info.flags & PF_PERSISTENT) || old_vel == obj->mtype.phys_info.velocity)
					{
						//if (Objects[hit_info.hit_object].type == object_type::OBJ_POWERUP)
						if (ignore_obj_list.push_back(hit_info.hit_object))
						try_again = 1;
					}
				}

				break;
			}	
			case fvi_hit_type::None:
				break;

			case fvi_hit_type::BadP0:
#ifndef NDEBUG
				Int3();		// Unexpected collision type: start point not in specified segment.
#endif
				break;
		}
	} while ( try_again );

	//	Pass retry count info to AI.
	if (obj->control_source == object::control_type::ai)
	{
		if (count > 0) {
			obj->ctype.ai_info.ail.retry_count = count-1;
#ifndef NDEBUG
			Total_retries += count-1;
			Total_sims++;
#endif
		}
	}

	// After collision with objects and walls, set velocity from actual movement
	if (!obj_stopped
#if DXX_BUILD_DESCENT == 2
		&& !bounced 
#endif
		&& ((obj->type == object_type::OBJ_PLAYER) || (obj->type == object_type::OBJ_ROBOT) || (obj->type == object_type::OBJ_DEBRIS)) 
		&& (fate == fvi_hit_type::Wall || fate == fvi_hit_type::Object || fate == fvi_hit_type::BadP0)
		)
	{	
		const auto moved_vec{vm_vec_build_sub(obj->pos, start_pos)};
		obj->mtype.phys_info.velocity = vm_vec_copy_scale(moved_vec, fixdiv(F1_0, FrameTime));
	}

	fix_illegal_wall_intersection(obj);

	//Assert(check_point_in_seg(&obj->pos,obj->segnum,0).centermask==0);

	//if (obj->control_source == object::control_type::flying)
	if (obj->mtype.phys_info.flags & PF_LEVELLING)
		do_physics_align_object( obj );

	//hack to keep player from going through closed doors
	if (obj->type==object_type::OBJ_PLAYER && obj->segnum!=orig_segnum && (!cheats.ghostphysics) ) {

		const cscusegment orig_segp{vcsegptr(orig_segnum)};
		const auto &&sidenum = find_connect_side(vcsegptridx(obj->segnum), orig_segp);
		if (sidenum != side_none)
		{

			if (! (WALL_IS_DOORWAY(GameBitmaps, Textures, vcwallptr, orig_segp, sidenum) & WALL_IS_DOORWAY_FLAG::fly))
			{
				//bump object back

				auto &s = orig_segp.s.sides[sidenum];
				const auto &&vertex_list{create_abs_vertex_lists(orig_segp, s, sidenum).vertex_list};

				//let's pretend this wall is not triangulated
				const auto b{begin(vertex_list)};
				const auto vertnum{*std::min_element(b, std::next(b, 4))};

				const fix dist{vm_dist_to_plane(start_pos, s.normals[0], vcvertptr(vertnum))};
				obj->pos = vm_vec_scale_add(start_pos, s.normals[0], obj->size - dist);
				update_object_seg(vmobjptr, LevelSharedSegmentState, LevelUniqueSegmentState, obj);
			}
		}
	}

//--WE ALWYS WANT THIS IN, MATT AND MIKE DECISION ON 12/10/94, TWO MONTHS AFTER FINAL 	#ifndef NDEBUG
	//if end point not in segment, move object to last pos, or segment center
	if (get_seg_masks(vcvertptr, obj->pos, vcsegptr(obj->segnum), 0).centermask != sidemask_t{})
	{
		if (find_object_seg(LevelSharedSegmentState, LevelUniqueSegmentState, obj) == segment_none)
		{
			segnum_t n;
			const auto &&obj_segp = Segments.vmptridx(obj->segnum);
			if (obj->type == object_type::OBJ_PLAYER && (n = find_point_seg(LevelSharedSegmentState, LevelUniqueSegmentState, obj_previous_position, obj_segp DXX_lighting_hack_pass_parameter)) != segment_none)
			{
				obj->pos = obj_previous_position;
				obj_relink(vmobjptr, Segments.vmptr, obj, Segments.vmptridx(n));
			}
			else {
				obj->pos = compute_segment_center(vcvertptr, obj_segp);
				obj->pos.x += obj;
			}
			if (obj->type == object_type::OBJ_WEAPON)
				obj->flags |= OF_SHOULD_BE_DEAD;
		}
	}

	return result;
//--WE ALWYS WANT THIS IN, MATT AND MIKE DECISION ON 12/10/94, TWO MONTHS AFTER FINAL 	#endif
}
}

namespace dcx {

//make sure matrix is orthogonal
void check_and_fix_matrix(vms_matrix &m)
{
	/* Copy `m.fvec`, `m.uvec` into temporaries and pass the temporaries, since
	 * it is undefined behavior to access `m` after the old object is destroyed
	 * until the new value is constructed.
	 */
	auto old_fvec{m.fvec};
	auto old_uvec{m.uvec};
	reconstruct_at(m, vm_vector_to_matrix_u, std::move(old_fvec), std::move(old_uvec));
}

//Applies an instantaneous force on an object, resulting in an instantaneous
//change in velocity.
void phys_apply_force(object_base &obj, const vms_vector &force_vec)
{
	if (obj.movement_source != object::movement_type::physics)
		return;

	//	Put in by MK on 2/13/96 for force getting applied to Omega blobs, which have 0 mass,
	//	in collision with crazy reactor robot thing on d2levf-s.
	if (obj.mtype.phys_info.mass == 0)
		return;

	//Add in acceleration due to force
	vm_vec_scale_add2(obj.mtype.phys_info.velocity, force_vec, fixdiv(f1_0, obj.mtype.phys_info.mass));
}

namespace {

//	----------------------------------------------------------------
//	Do *dest = *delta unless:
//				*delta is pretty small
//		and	they are of different signs.
static void physics_set_rotvel_and_saturate(fix &dest, fix delta)
{
	if ((delta ^ dest) < 0) {
		if (abs(delta) < F1_0/8) {
			dest = delta/4;
		} else
			dest = delta;
	} else {
		dest = delta;
	}
}

}

//	------------------------------------------------------------------------------------------------------
//	Note: This is the old ai_turn_towards_vector code.
//	phys_apply_rot used to call ai_turn_towards_vector until I fixed it, which broke phys_apply_rot.
void physics_turn_towards_vector(const vms_vector &goal_vector, object_base &obj, fix rate)
{
	// Make this object turn towards the goal_vector.  Changes orientation, doesn't change direction of movement.
	// If no one moves, will be facing goal_vector in 1 second.

	//	Detect null vector.
	if (goal_vector == vms_vector{})
		return;

	//	Make morph objects turn more slowly.
	if (obj.control_source == object::control_type::morph)
		rate *= 2;

	const auto dest_angles{vm_extract_angles_vector(goal_vector)};
	const auto cur_angles{vm_extract_angles_vector(obj.orient.fvec)};

	fix delta_p{dest_angles.p - cur_angles.p};
	fix delta_h{dest_angles.h - cur_angles.h};

	if (delta_p > F1_0/2) delta_p = dest_angles.p - cur_angles.p - F1_0;
	if (delta_p < -F1_0/2) delta_p = dest_angles.p - cur_angles.p + F1_0;
	if (delta_h > F1_0/2) delta_h = dest_angles.h - cur_angles.h - F1_0;
	if (delta_h < -F1_0/2) delta_h = dest_angles.h - cur_angles.h + F1_0;

	delta_p = fixdiv(delta_p, rate);
	delta_h = fixdiv(delta_h, rate);

	if (abs(delta_p) < F1_0/16) delta_p *= 4;
	if (abs(delta_h) < F1_0/16) delta_h *= 4;

	auto &rotvel_ptr = obj.mtype.phys_info.rotvel;
	physics_set_rotvel_and_saturate(rotvel_ptr.x, delta_p);
	physics_set_rotvel_and_saturate(rotvel_ptr.y, delta_h);
	rotvel_ptr.z = 0;
}

}

namespace dsx {

//	-----------------------------------------------------------------------------
//	Applies an instantaneous whack on an object, resulting in an instantaneous
//	change in orientation.
void phys_apply_rot(object &obj, const vms_vector &force_vec)
{
	fix	rate;

	if (obj.movement_source != object::movement_type::physics)
		return;

#if DXX_BUILD_DESCENT == 2
	auto &Robot_info = LevelSharedRobotInfoState.Robot_info;
#endif
	const auto vecmag{vm_vec_mag(force_vec)};
	if (vecmag < F1_0/32)
		rate = 4*F1_0;
	else if (vecmag < obj.mtype.phys_info.mass >> 11)
		rate = 4*F1_0;
	else {
		rate = fixdiv(obj.mtype.phys_info.mass, vecmag / 8);
		if (obj.type == object_type::OBJ_ROBOT) {
			if (rate < F1_0/4)
				rate = F1_0/4;
#if DXX_BUILD_DESCENT == 1
			obj.ctype.ai_info.SKIP_AI_COUNT = 2;
#elif DXX_BUILD_DESCENT == 2
			//	Changed by mk, 10/24/95, claw guys should not slow down when attacking!
			if (!Robot_info[get_robot_id(obj)].thief && !Robot_info[get_robot_id(obj)].attack_type) {
				if (obj.ctype.ai_info.SKIP_AI_COUNT * FrameTime < 3*F1_0/4) {
					fix tval{fixdiv(F1_0, 8*FrameTime)};
					auto addval{f2i(tval)};

					if ( (d_rand() * 2) < (tval & 0xffff))
						addval++;
					obj.ctype.ai_info.SKIP_AI_COUNT += addval;
				}
			}
#endif
		} else {
			if (rate < F1_0/2)
				rate = F1_0/2;
		}
	}

	//	Turn amount inversely proportional to mass.  Third parameter is seconds to do 360 turn.
	physics_turn_towards_vector(force_vec, obj, rate);
}

}

namespace dcx {

namespace {

/* Return the thrust per unit of velocity that holds a velocity steady under
 * the drag integration in `do_physics_sim`, at any frame rate, or 0 if no
 * thrust can hold a velocity steady.  The steady state of that integration
 * is `accel * steady_state_per_accel`, where `accel` is computed from the
 * thrust as `fixmul(thrust, fixdiv(F1_0, mass))`.
 */
[[nodiscard]]
static double compute_thrust_per_velocity(const fix mass, const fix drag)
{
	if (drag <= 0 || mass <= 0)
		return 0;
	const double steady_state_per_accel{build_drag_model(drag).steady_state_per_accel};
	const fix inverse_mass{fixdiv(F1_0, mass)};
	if (!(steady_state_per_accel > 0) || inverse_mass <= 0)
		return 0;
	return F1_0 / (steady_state_per_accel * inverse_mass);
}

}

fix compute_thrust_scale_holding_velocity(const fix mass, const fix drag)
{
	return static_cast<fix>(std::min<double>(std::round(F1_0 * compute_thrust_per_velocity(mass, drag)), std::numeric_limits<fix>::max()));
}

//this routine will set the thrust for an object to a value that will
//maintain the object's current velocity
void set_thrust_from_velocity(object_base &obj)
{
	Assert(obj.movement_source == object::movement_type::physics);
	auto &phys_info = obj.mtype.phys_info;
	const double thrust_per_velocity{compute_thrust_per_velocity(phys_info.mass, phys_info.drag)};
	const auto scale{[thrust_per_velocity](const fix v) {
		return static_cast<fix>(std::clamp<double>(std::round(v * thrust_per_velocity), std::numeric_limits<fix>::min(), std::numeric_limits<fix>::max()));
	}};
	phys_info.thrust = {
		.x = scale(phys_info.velocity.x),
		.y = scale(phys_info.velocity.y),
		.z = scale(phys_info.velocity.z),
	};
}

}
