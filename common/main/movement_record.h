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

#include <array>
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
/* A remote player's shot as FIRE carries it (raw weapon byte). */
void movement_record_fire_remote(const object &shooter, uint8_t raw_weapon, unsigned flags);
/* The weapon `weapon` hit the player ship `victim` for `damage`. */
void movement_record_hit(const object &victim, const object &weapon, fix damage);
/* The explosion of `origin` (a weapon, a dying ship; nullptr: none)
 * fired by `parent` (nullptr: none) did `damage` of splash damage to
 * the player ship `victim`.
 */
void movement_record_splash(const object &victim, const object *origin, const object *parent, fix damage);
/* A network game (protocol v2 stage 4): the host applied `damage` to
 * player `victim` from player `attacker` (255: none) of the attacker kind
 * `attacker_kind` (movement_record_format.h), with the weapon `weapon`
 * (255: none), `splash` for a blast.  Recorded where the host decides
 * and where its DAMAGE arrives, in place of the collisions seen here.
 */
void movement_record_damage(unsigned victim, unsigned attacker, std::uint8_t attacker_kind, unsigned weapon, fix damage, bool splash);
/* The player `victim` was killed by `killer` (nullptr: none). */
void movement_record_kill(const object &victim, const object *killer);
/* Player `pnum` picked up a powerup of type `powerup`. */
void movement_record_pickup(unsigned pnum, unsigned powerup);
/* -sharemoves on a client: the local player's controls this frame as the
 * recording stores them (forward, sideways, vertical, pitch, heading,
 * bank; 60 = full deflection), for its INPUT chunk.  False while there
 * are none to share (not alive, steering a guided missile, not
 * -sharemoves).
 */
[[nodiscard]]
bool movement_record_shared_controls(std::array<std::int8_t, 6> &controls);

}
#endif
