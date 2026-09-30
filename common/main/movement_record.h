/*
 * This file is part of the DXX-Rebirth project <https://github.com/dxx-rebirth/dxx-rebirth/>.
 * It is copyright by its individual contributors, as recorded in the
 * project's Git history.  See COPYING.txt at the top level for license
 * terms and a link to the Git history.
 */
/*
 * The game's side of the movement recording (-recordmoves,
 * Documentation/movement-recording.md): the frame hook samples the
 * players on the recording tick, the event hooks add fire, hits, kills
 * and pickups.  Every hook returns at once when recording is off.
 */

#pragma once

#include <cstdint>
#include "dxxsconf.h"
#include "dsx-ns.h"

#ifdef DXX_BUILD_DESCENT
#include "fwd-object.h"
#include "fwd-robot.h"
#include "fwd-player.h"
#include "maths.h"

namespace dsx {

/* Once per game frame, after the objects moved (GameProcessFrame). */
void movement_record_frame(const d_robot_info_array &Robot_info);
/* The game window closes: finish the file. */
void movement_record_end_session();
/* A shot of the player ship `shooter`: `secondary` false for a primary.
 * `weapon` is the primary or secondary weapon index.
 */
void movement_record_fire(const object &shooter, bool secondary, unsigned weapon, unsigned flags);
/* A remote player's shot as MULTI_FIRE carries it (raw weapon byte). */
void movement_record_fire_remote(const object &shooter, uint8_t raw_weapon, unsigned flags);
/* The weapon `weapon` hit the player ship `victim` for `damage`. */
void movement_record_hit(const object &victim, const object &weapon, fix damage);
/* The explosion of `origin` (a weapon, a dying ship; nullptr: none)
 * fired by `parent` (nullptr: none) did `damage` of splash damage to
 * the player ship `victim`.
 */
void movement_record_splash(const object &victim, const object *origin, const object *parent, fix damage);
/* The player `victim` was killed by `killer` (nullptr: none). */
void movement_record_kill(const object &victim, const object *killer);
/* Player `pnum` picked up a powerup of type `powerup`. */
void movement_record_pickup(unsigned pnum, unsigned powerup);

}
#endif
