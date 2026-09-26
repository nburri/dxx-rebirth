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
namespace dsx {
void read_flying_controls(object &obj, control_info &Controls);
}
#if DXX_BUILD_DESCENT == 2
#include "maths.h"
namespace dsx {
extern fix Afterburner_charge;

/* Remainders of the per-frame divisions of FrameTime for the local
 * player's ship.  They only exist for the local player, so player_info,
 * savegames and the network protocol are unchanged.  Call reset()
 * wherever the per-ship state is (re)initialized.
 */
struct local_player_rate_dividers
{
	fix_rate_divider<4> omega_charge;			// OMEGA_CHARGE_SCALE
	fix_rate_divider<3> afterburner_drain;		// AFTERBURNER_USE_SECS
	fix_rate_divider<8> afterburner_recharge;	// AFTERBURNER_RECHARGE_SECS
	fix_rate_divider<8> headlight_drain;		// FrameTime*3/8
	void reset()
	{
		*this = {};
	}
};

extern local_player_rate_dividers Local_player_rate_dividers;
}
#endif
#endif

#endif
