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
 */

#include <algorithm>
#include <cmath>
#include "remote_smoothing.h"
#include "game.h"
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

/* Map a vector from the local frame of m to world space. */
vms_vector remote_smoothing_local_to_world(const vms_matrix &m, const vms_vector &l)
{
	auto v{vm_vec_copy_scale(m.rvec, l.x)};
	vm_vec_scale_add2(v, m.uvec, l.y);
	vm_vec_scale_add2(v, m.fvec, l.z);
	return v;
}

vms_vector remote_smoothing_pos(const vms_vector &pos, const remote_smoothing_state &s, const fix w)
{
	return vm_vec_scale_add(pos, s.pos_error, w);
}

vms_matrix remote_smoothing_orient(const vms_matrix &m, const remote_smoothing_state &s, const fix w)
{
	/* Blend the local error axes towards the identity axes, then
	 * re-orthonormalize.  For the small angles admitted by
	 * remote_smoothing_min_axis_dot, this is a good approximation of a
	 * spherical interpolation.
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
	return vm_vector_to_matrix_u(remote_smoothing_local_to_world(m, lf), remote_smoothing_local_to_world(m, lu));
}

bool remote_smoothing_segments_adjacent(const vcobjptridx_t obj, const segnum_t old_segnum)
{
	if (old_segnum == obj->segnum)
		return true;
	auto &children = vcsegptr(obj->segnum)->children;
	return std::ranges::find(children, old_segnum) != children.end();
}

}

void remote_smoothing_reset(const playernum_t pnum)
{
	if (pnum < MAX_PLAYERS)
		remote_smoothing_states[pnum].active = false;
}

remote_smoothing_pre_update remote_smoothing_begin_update(const playernum_t pnum, const vcobjptridx_t obj)
{
	remote_smoothing_pre_update pre{
		.rendered_pos = obj->pos,
		.rendered_orient = obj->orient,
		.segnum = obj->segnum,
		.valid = remote_smoothing_enabled() && remote_smoothing_eligible(pnum, obj),
	};
	if (pre.valid)
	{
		auto &s = remote_smoothing_states[pnum];
		if (const auto w{remote_smoothing_weight(s)})
		{
			pre.rendered_pos = remote_smoothing_pos(obj->pos, s, w);
			pre.rendered_orient = remote_smoothing_orient(obj->orient, s, w);
		}
	}
	return pre;
}

void remote_smoothing_end_update(const playernum_t pnum, const vcobjptridx_t obj, const remote_smoothing_pre_update &pre)
{
	if (pnum >= MAX_PLAYERS)
		return;
	auto &s = remote_smoothing_states[pnum];
	/* Snap unless every check below passes. */
	s.active = false;
	if (!pre.valid || !remote_smoothing_enabled() || !remote_smoothing_eligible(pnum, obj))
		return;
	/* Only smooth across the same or a directly connected segment.
	 * Anything else is a jump (or the ship would be drawn through a
	 * wall).
	 */
	if (!remote_smoothing_segments_adjacent(obj, pre.segnum))
		return;
	const auto pos_error{vm_vec_build_sub(pre.rendered_pos, obj->pos)};
	if (vm_vec_mag_quick(pos_error) > remote_smoothing_max_pos_error)
		return;
	const auto local_fvec{vm_vec_build_rotated(pre.rendered_orient.fvec, obj->orient)};
	const auto local_uvec{vm_vec_build_rotated(pre.rendered_orient.uvec, obj->orient)};
	if (local_fvec.z < remote_smoothing_min_axis_dot || local_uvec.y < remote_smoothing_min_axis_dot)
		return;
	s.pos_error = pos_error;
	s.local_fvec = local_fvec;
	s.local_uvec = local_uvec;
	s.error_time = timer_query();
	s.active = true;
}

vms_vector remote_smoothing_render_pos(const vcobjptridx_t obj)
{
	const auto pnum{remote_smoothing_player_for_render(obj)};
	if (pnum >= MAX_PLAYERS)
		return obj->pos;
	auto &s = remote_smoothing_states[pnum];
	const auto w{remote_smoothing_weight(s)};
	return w ? remote_smoothing_pos(obj->pos, s, w) : obj->pos;
}

remote_smoothing_render_guard::remote_smoothing_render_guard(const vmobjptridx_t o) :
	obj(*o), saved_pos(o->pos), saved_orient(o->orient), active(false)
{
	const auto pnum{remote_smoothing_player_for_render(o)};
	if (pnum >= MAX_PLAYERS)
		return;
	auto &s = remote_smoothing_states[pnum];
	const auto w{remote_smoothing_weight(s)};
	if (!w)
		return;
	obj.pos = remote_smoothing_pos(saved_pos, s, w);
	obj.orient = remote_smoothing_orient(saved_orient, s, w);
	active = true;
}

remote_smoothing_render_guard::~remote_smoothing_render_guard()
{
	if (!active)
		return;
	obj.pos = saved_pos;
	obj.orient = saved_orient;
}

}
