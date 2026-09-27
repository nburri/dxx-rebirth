/*
 * This file is part of the DXX-Rebirth project <https://github.com/dxx-rebirth/dxx-rebirth/>.
 * It is copyright by its individual contributors, as recorded in the
 * project's Git history.  See COPYING.txt at the top level for license
 * terms and a link to the Git history.
 */
/*
 * The state of one pilot: whoever flies a player ship through the
 * controls -> physics -> firing path (Documentation/multiplayer-bots.md
 * section 3.2).  Today the only pilot is the human at this machine,
 * `Local_pilot`.  The host's bots get their own `pilot` later.
 *
 * Everything here used to be a global about "the local player".  The
 * old names (`Player_dead_state`, `Afterburner_charge`,
 * `Local_player_rate_dividers`, `Drop_afterburner_blob_flag`,
 * `Global_missile_firing_count`) remain as references to the members of
 * `Local_pilot`, for the code that is only ever about the human: HUD,
 * cockpit, death camera, demo, savegame.  Code that a bot will also run
 * takes a `pilot &` instead.
 *
 * Per-player state that other machines need already lives in
 * `player_info` (fire timers, weapons, energy) and does not move here.
 */

#pragma once

#include <cstdint>
#include "maths.h"
#include "fwd-object.h"

#ifdef DXX_BUILD_DESCENT
namespace dsx {

#if DXX_BUILD_DESCENT == 2
/* Remainders of the per-frame divisions of FrameTime for a pilot's
 * ship.  They only exist on the machine that flies the ship, so
 * player_info, savegames and the network protocol are unchanged.
 * reset() is called where the ship is (re)initialized: new ship, level
 * start, savegame load and demo playback start.  Each remainder is less
 * than its divisor in fix units, so resets elsewhere are not needed.
 */
struct pilot_rate_dividers
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
#endif

struct pilot
{
	/* Not `no` while the ship is dying or dead: no thrust, no firing,
	 * no omega recharge.
	 */
	player_dead_state dead_state{player_dead_state::no};
	/* Missiles still to fire from one press of the secondary trigger
	 * (a weapon with fire_count > 1 fires over several frames).
	 */
	int missile_firing_count{};
#if DXX_BUILD_DESCENT == 2
	fix afterburner_charge{};
	pilot_rate_dividers rate_dividers{};
	/* Set by apply_pilot_controls when the afterburner charge crosses a
	 * blob step; the blob is dropped after the physics move.
	 */
	int drop_afterburner_blob_flag{};
#endif
};

/* The human at this machine, who flies the ship of `Player_num`. */
extern pilot Local_pilot;

}
#endif
