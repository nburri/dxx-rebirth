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
 * Headers for physics functions and data
 *
 */


#ifndef _PHYSICS_H
#define _PHYSICS_H

#include "vecmat.h"
#include "fvi.h"
#include "fwd-window.h"

#ifdef __cplusplus

//#define FL_NORMAL  0
//#define FL_TURBO   1
//#define FL_HOVER   2
//#define FL_REVERSE 3

// Simulate a physics object for this frame
#ifdef DXX_BUILD_DESCENT
struct phys_visited_seglist
{
	unsigned nsegs;
	std::array<segnum_t, MAX_FVI_SEGS> seglist;
};

namespace dcx {

// Applies an instantaneous force on an object, resulting in an instantaneous
// change in velocity.
void phys_apply_force(object_base &obj, const vms_vector &force_vec);

}
namespace dsx {
window_event_result do_physics_sim(const d_robot_info_array &Robot_info, vmobjptridx_t obj, const vms_vector &obj_previous_position, phys_visited_seglist *phys_segs);
/* The object collisions of `obj` (a remote ship placed by the network,
 * which does not move by physics) moving in a straight line from `from`
 * in segment `from_seg` to where it is now: collide_two_objects for every
 * object the sweep touches, as do_physics_sim would, without the wall
 * collisions.
 */
void phys_sweep_objects(const d_robot_info_array &Robot_info, vmobjptridx_t obj, const vms_vector &from, segnum_t from_seg);
void phys_apply_rot(object &obj, const vms_vector &force_vec);
/* Whether a ship can fly from segment `from` into `to` within a few
 * sides, through sides open to flying (doors and walls as they are now).
 */
[[nodiscard]]
bool phys_segment_reachable_by_flying(segnum_t from, segnum_t to);
}

// this routine will set the thrust for an object to a value that will
// (hopefully) maintain the object's current velocity
namespace dcx {
void set_thrust_from_velocity(object_base &obj);
/* The response of a thrust-driven object to a constant full thrust:
 * its velocity tends to `steady_velocity` (fix units per second; for
 * rotation, fix revolutions per second) with the time constant
 * `time_constant` (seconds), at any frame rate.  The bots plan with it
 * (Documentation/multiplayer-bots.md section 3.4).  Zero if the object
 * has no drag.
 */
struct physics_thrust_response
{
	double steady_velocity;
	double time_constant;
};
[[nodiscard]]
physics_thrust_response compute_thrust_response(fix mass, fix drag, fix max_thrust);
[[nodiscard]]
physics_thrust_response compute_rotation_response(fix mass, fix drag, fix max_rotthrust);
// the factor by which set_thrust_from_velocity scales the velocity
[[nodiscard]]
fix compute_thrust_scale_holding_velocity(fix mass, fix drag);
void check_and_fix_matrix(vms_matrix &m);
void physics_turn_towards_vector(const vms_vector &goal_vector, object_base &obj, fix rate);
}
#endif

#endif

#endif /* _PHYSICS_H */
