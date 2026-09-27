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
 * Header for controls.c
 *
 */

#pragma once

#ifdef __cplusplus
#include "fwd-object.h"

#ifdef DXX_BUILD_DESCENT
#include "kconfig.h"
#include "pilot.h"
namespace dsx {
/* The local player's ship from the local input devices: returns unless
 * `obj` is the ship of `Player_num` (D2), then applies `Controls` with
 * `Local_pilot`.
 */
void read_flying_controls(object &obj, control_info &Controls);
/* Turn one frame of `Controls` into thrust and rotational thrust for the
 * player ship `obj`, flown by `p`: afterburner charge and drain, blob
 * drops, wiggle, and in D2 the steering of the ship's guided missile.
 * Reads no input device and no "local player" state.
 */
void apply_pilot_controls(object &obj, pilot &p, const control_info &Controls);
}
#if DXX_BUILD_DESCENT == 2
namespace dsx {
/* Local_pilot.afterburner_charge */
extern fix &Afterburner_charge;
/* Local_pilot.rate_dividers */
extern pilot_rate_dividers &Local_player_rate_dividers;
}
#endif
#endif

#endif
