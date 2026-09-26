/*
 * This file is part of the DXX-Rebirth project <https://github.com/dxx-rebirth/dxx-rebirth/>.
 * It is copyright by its individual contributors, as recorded in the
 * project's Git history.  See COPYING.txt at the top level for license
 * terms and a link to the Git history.
 */
/*
 * Render-only smoothing of remote player ships.
 *
 * Every position update (PDATA / MULTI_POSITION) received for a remote
 * player hard-snaps that player's object to the authoritative state.
 * Between updates the object is extrapolated by normal physics, so the
 * snap at each update is visible as a jerk, which is very noticeable at
 * high frame rates.
 *
 * This module hides the jerk without touching the authoritative object
 * state: when an update is applied, the difference between where the
 * ship was being drawn and where it now is ("visual error") is recorded,
 * and when the ship is drawn, that error is added back and decays
 * exponentially towards zero.  Only drawing uses the smoothed values.
 * Physics, collisions, hit detection, sounds and network sends keep using
 * obj->pos / obj->orient unchanged.
 */

#pragma once

#include "dsx-ns.h"
#include "fwd-object.h"
#include "fwd-player.h"
#include "fwd-segment.h"
#include "vecmat.h"

#ifdef DXX_BUILD_DESCENT
namespace dsx {

/* Snapshot of a remote player's drawn state, taken immediately before
 * an authoritative update overwrites the object.
 */
struct remote_smoothing_pre_update
{
	vms_vector rendered_pos;
	vms_matrix rendered_orient;
	segnum_t segnum;
	/* False if the object was not in a state where smoothing applies
	 * (for example, it was a ghost).  In that case, the update snaps.
	 */
	bool valid;
};

/* Forget any pending visual error for this player, so that the next
 * frame draws the authoritative position.  Used on respawn, death, level
 * start and explicit repositioning.
 */
void remote_smoothing_reset(playernum_t pnum);

/* Call immediately before an authoritative position update is applied to
 * a remote player's object.
 */
[[nodiscard]]
remote_smoothing_pre_update remote_smoothing_begin_update(playernum_t pnum, vcobjptridx_t obj);

/* Call immediately after the update was applied.  Records the new visual
 * error, or snaps if the error is too large or the segments are not
 * adjacent.
 */
void remote_smoothing_end_update(playernum_t pnum, vcobjptridx_t obj, const remote_smoothing_pre_update &pre);

/* Position at which the object should be drawn.  For anything other than
 * a smoothed remote player ship, this is obj->pos.
 */
[[nodiscard]]
vms_vector remote_smoothing_render_pos(vcobjptridx_t obj);

/* Temporarily replace obj->pos / obj->orient with the smoothed values for
 * the lifetime of this guard, then restore the authoritative values.  Use
 * this only around pure drawing code.
 */
class remote_smoothing_render_guard
{
	object &obj;
	vms_vector saved_pos;
	vms_matrix saved_orient;
	bool active;
public:
	explicit remote_smoothing_render_guard(vmobjptridx_t obj);
	remote_smoothing_render_guard(const remote_smoothing_render_guard &) = delete;
	remote_smoothing_render_guard &operator=(const remote_smoothing_render_guard &) = delete;
	~remote_smoothing_render_guard();
};

}
#endif
