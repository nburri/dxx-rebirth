/*
 * This file is part of the DXX-Rebirth project <https://github.com/dxx-rebirth/dxx-rebirth/>.
 * It is copyright by its individual contributors, as recorded in the
 * project's Git history.  See COPYING.txt at the top level for license
 * terms and a link to the Git history.
 */
/*
 *
 * Render-only smoothing of remote player ships.  See remote_smoothing.h
 * for an overview.
 *
 * The recorded error is split into:
 * - a world-space position offset (rendered - authoritative), and
 * - the rendered forward/up axes expressed in the local frame of the
 *   authoritative orientation.  Keeping the orientation error in the
 *   ship's local frame lets it rotate with the ship while physics
 *   extrapolates the authoritative orientation between updates.
 *
 * Both are scaled by exp(-dt / tau), where dt is the real time since
 * the update was received.  The decay is evaluated lazily whenever the
 * ship is drawn, so it is independent of the frame rate and needs no
 * per-frame bookkeeping.
 *
 * The object is drawn from the object list of its authoritative segment,
 * and lit from that segment.  To keep that consistent, the smoothed
 * position is clamped so that it never leaves obj->segnum.
 *
 */

#include <cmath>
#include <optional>
#include "remote_smoothing.h"
#include "game.h"
#include "gameseg.h"
#include "object.h"
#include "player.h"
#include "segment.h"
#include "newdemo.h"
#include "timer.h"

namespace dsx {

namespace {

/* Time constant of the exponential decay of the visual error.  After one
 * time constant, 37% of the error remains; after three, 5%.
 */
constexpr fix remote_smoothing_time_constant{F1_0 * 75 / 1000};

/* Beyond this many time constants, the remaining error (< 0.04%) is
 * dropped.  This also guarantees that a stale error can never resurface.
 */
constexpr fix64 remote_smoothing_expiry{fix64{remote_smoothing_time_constant} * 8};

/* Position errors larger than this are not smoothed, but snapped.  A
 * player ship has a radius of about 4.6 units, and a correction this
 * large is no longer a prediction error, but a teleport (respawn, lost
 * packets, lag spike).
 */
constexpr fix remote_smoothing_max_pos_error{F1_0 * 20};

/* Orientation errors where the rendered forward or up axis deviates by
 * more than 60 degrees (cos 60 = 0.5) from the authoritative one are
 * snapped.  This also keeps the blended axes well away from zero length.
 */
constexpr fix remote_smoothing_min_axis_dot{F1_0 / 2};

struct remote_smoothing_state
{
	/* World-space offset: rendered position - authoritative position,
	 * at the time of the update.
	 */
	vms_vector pos_error;
	/* Rendered forward and up axes, in the local frame of the
	 * authoritative orientation, at the time of the update.  With no
	 * error, these are (0, 0, 1) and (0, 1, 0).
	 */
	vms_vector local_fvec, local_uvec;
	fix64 error_time;
	bool active;
};

per_player_array<remote_smoothing_state> remote_smoothing_states;

bool remote_smoothing_enabled()
{
	/* Demo playback has no network updates, and the player numbers of
	 * objects need not match the live Players array.
	 */
	return +(Game_mode & GM_NETWORK) && Newdemo_state != ND_STATE_PLAYBACK;
}

/* True if obj is the ship of remote player pnum, drawn as a polygon
 * model.  The local player's ship is never smoothed.
 */
bool remote_smoothing_eligible(const playernum_t pnum, const vcobjptridx_t obj)
{
	return pnum < MAX_PLAYERS &&
		pnum != Player_num &&
		obj->type == object_type::OBJ_PLAYER &&
		obj->render_type == render_type::RT_POLYOBJ &&
		vcplayerptr(pnum)->objnum == obj;
}

/* Returns the player number if obj should be drawn smoothed, otherwise
 * MAX_PLAYERS.
 */
playernum_t remote_smoothing_player_for_render(const vcobjptridx_t obj)
{
	if (!remote_smoothing_enabled() || obj->type != object_type::OBJ_PLAYER)
		return MAX_PLAYERS;
	const playernum_t pnum = get_player_id(obj);
	return remote_smoothing_eligible(pnum, obj) ? pnum : MAX_PLAYERS;
}

/* Fraction (F1_0 = all) of the recorded error that remains now. */
fix remote_smoothing_weight(const remote_smoothing_state &s)
{
	if (!s.active)
		return 0;
	const fix64 dt{timer_query() - s.error_time};
	if (dt <= 0)
		return F1_0;
	if (dt >= remote_smoothing_expiry)
		return 0;
	return static_cast<fix>(F1_0 * std::exp(-static_cast<double>(dt) / static_cast<double>(remote_smoothing_time_constant)));
}

/* Steps of the bisection that clamps the smoothed position into the
 * object's segment.  Six steps resolve the offset to 1/64 of its length,
 * which is at most 0.3 units for the largest admitted error.
 */
constexpr unsigned remote_smoothing_clamp_steps{6};

bool remote_smoothing_point_in_segment(fvcvertptr &vcvertptr, const shared_segment &seg, const vms_vector &p)
{
	return get_seg_masks(vcvertptr, p, seg, 0).centermask == sidemask_t{};
}

/* Return pos + offset, shortened as little as necessary so that it stays
 * inside the segment of obj.  The segment is convex enough that the
 * points along the offset are inside up to some fraction and outside
 * beyond it, so a bisection finds the largest usable fraction.  pos
 * itself is the authoritative position, which is the fallback.
 */
vms_vector remote_smoothing_clamp_to_segment(const vcobjptridx_t obj, const vms_vector &offset)
{
	auto &vcvertptr = LevelSharedSegmentState.get_vertex_state().get_vertices().vcptr;
	const shared_segment &seg = *vcsegptr(obj->segnum);
	const auto &pos = obj->pos;
	if (const auto p{vm_vec_build_add(pos, offset)}; remote_smoothing_point_in_segment(vcvertptr, seg, p))
		return p;
	fix inside{0}, outside{F1_0};
	for (unsigned i = remote_smoothing_clamp_steps; i--;)
	{
		const fix mid{(inside + outside) / 2};
		if (remote_smoothing_point_in_segment(vcvertptr, seg, vm_vec_scale_add(pos, offset, mid)))
			inside = mid;
		else
			outside = mid;
	}
	return vm_vec_scale_add(pos, offset, inside);
}

/* The smoothed pose of remote player pnum, or nothing if there is no
 * remaining error.  The caller must have checked that obj is eligible.
 */
std::optional<remote_smoothing_pose> remote_smoothing_smoothed_pose(const playernum_t pnum, const vcobjptridx_t obj)
{
	auto &s = remote_smoothing_states[pnum];
	const auto w{remote_smoothing_weight(s)};
	if (!w)
		return std::nullopt;
	/* Blend the local error axes towards the identity axes, then map them
	 * to world space and re-orthonormalize.  For the small angles
	 * admitted by remote_smoothing_min_axis_dot, this is a good
	 * approximation of a spherical interpolation.
	 */
	const fix rest{F1_0 - w};
	const vms_vector lf{
		.x = fixmul(s.local_fvec.x, w),
		.y = fixmul(s.local_fvec.y, w),
		.z = fixmul(s.local_fvec.z, w) + rest,
	};
	const vms_vector lu{
		.x = fixmul(s.local_uvec.x, w),
		.y = fixmul(s.local_uvec.y, w) + rest,
		.z = fixmul(s.local_uvec.z, w),
	};
	const auto local_to_world{vm_transposed_matrix(obj->orient)};
	return remote_smoothing_pose{
		.pos = remote_smoothing_clamp_to_segment(obj, vm_vec_copy_scale(s.pos_error, w)),
		.orient = vm_vector_to_matrix_u(vm_vec_build_rotated(lf, local_to_world), vm_vec_build_rotated(lu, local_to_world)),
	};
}

remote_smoothing_pose remote_smoothing_authoritative_pose(const object_base &obj)
{
	return {
		.pos = obj.pos,
		.orient = obj.orient,
	};
}

}

void remote_smoothing_reset(const playernum_t pnum)
{
	if (pnum < MAX_PLAYERS)
		remote_smoothing_states[pnum].active = false;
}

remote_smoothing_pre_update remote_smoothing_begin_update(const playernum_t pnum, const vcobjptridx_t obj)
{
	const bool valid{remote_smoothing_enabled() && remote_smoothing_eligible(pnum, obj)};
	const auto smoothed{valid ? remote_smoothing_smoothed_pose(pnum, obj) : std::nullopt};
	return {
		.rendered = smoothed ? *smoothed : remote_smoothing_authoritative_pose(obj),
		.segnum = obj->segnum,
		.valid = valid,
	};
}

void remote_smoothing_end_update(const playernum_t pnum, const vcobjptridx_t obj, const remote_smoothing_pre_update &pre)
{
	if (pnum >= MAX_PLAYERS)
		return;
	auto &s = remote_smoothing_states[pnum];
	/* Snap unless every check below passes.  pre.valid implies that
	 * smoothing was enabled.
	 */
	s.active = false;
	if (!pre.valid || !remote_smoothing_eligible(pnum, obj))
		return;
	/* Only smooth across the same or a directly connected segment.
	 * Anything else is a jump.
	 */
	if (pre.segnum != obj->segnum && find_connect_side(pre.segnum, *vcsegptr(obj->segnum)) == side_none)
		return;
	const auto pos_error{vm_vec_build_sub(pre.rendered.pos, obj->pos)};
	if (vm_vec_mag_quick(pos_error) > remote_smoothing_max_pos_error)
		return;
	const auto local_fvec{vm_vec_build_rotated(pre.rendered.orient.fvec, obj->orient)};
	const auto local_uvec{vm_vec_build_rotated(pre.rendered.orient.uvec, obj->orient)};
	if (local_fvec.z < remote_smoothing_min_axis_dot || local_uvec.y < remote_smoothing_min_axis_dot)
		return;
	s.pos_error = pos_error;
	s.local_fvec = local_fvec;
	s.local_uvec = local_uvec;
	s.error_time = timer_query();
	s.active = true;
}

remote_smoothing_pose remote_smoothing_render_pose(const vcobjptridx_t obj)
{
	if (const auto pnum{remote_smoothing_player_for_render(obj)}; pnum < MAX_PLAYERS)
		if (const auto smoothed{remote_smoothing_smoothed_pose(pnum, obj)})
			return *smoothed;
	return remote_smoothing_authoritative_pose(obj);
}

}
