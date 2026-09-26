/*
 * Portions of this file are copyright Rebirth contributors and licensed as
 * described in COPYING.txt.
 * Portions of this file are copyright Parallax Software and licensed
 * according to the Parallax license.
 * See COPYING.txt for license details.
 */

#pragma once
#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include "vecmat.h"
#include "pack.h"
#include "dxxsconf.h"
#include "dsx-ns.h"

namespace dcx {

/* Fractional parts, in units of 1/65536 of a `fixang`, of angles that are
 * applied to an object's orientation in per-frame increments.  Without these,
 * each frame truncates its increment to a whole `fixang`, so slow rotations
 * are lost or quantized, and the loss grows with the frame rate.
 *
 * This state is runtime-only: it is not written to savegames, level files,
 * demos or network packets.  It is reset when the object is created or read
 * from one of those.  Losing it costs less than one `fixang` per angle.
 */
struct physics_angle_remainder
{
	std::array<uint16_t, 3> rotation;	// pitch, bank, heading
	uint16_t turnroll;	// rate limit of banking caused by turning
	uint16_t levelling;	// rate limit of automatic levelling
};

/* Fractional parts, in units of 1/32768 of a `fix`, of the velocity and
 * rotational velocity, as computed by the drag and thrust integration.
 * Each has the sign of the exact velocity it belongs to, so that the
 * stored `fix` is the exact velocity truncated toward zero.  Runtime-only,
 * like `physics_angle_remainder`.
 */
struct physics_velocity_remainder
{
	std::array<int16_t, 3> velocity;
	std::array<int16_t, 3> rotvel;
};

/* Return `fixmul(a, b)` as a `fixang`, carrying the fractional part of the
 * product from one call to the next in `remainder`, so that the sum of the
 * returned angles differs from the exact sum of the products by less than
 * one `fixang`.  The result is clamped to the range of `fixang`.
 */
[[nodiscard]]
inline fixang fixmul_to_fixang_with_remainder(const fix a, const fix b, uint16_t &remainder)
{
	const int64_t product{int64_t{a} * int64_t{b} + remainder};
	remainder = static_cast<uint16_t>(product & 0xffff);
	/* Arithmetic right shift rounds toward negative infinity, which is
	 * consistent with keeping a non-negative remainder.
	 */
	return static_cast<fixang>(std::clamp<int64_t>(product >> 16, std::numeric_limits<fixang>::min(), std::numeric_limits<fixang>::max()));
}

// information for physics sim for an object
struct physics_info : prohibit_void_ptr<>
{
	vms_vector  velocity;   // velocity vector of this object
	vms_vector  thrust;     // constant force applied to this object
	fix         mass;       // the mass of this object
	fix         drag;       // how fast this slows down
	vms_vector  rotvel;     // rotational velecity (angles)
	vms_vector  rotthrust;  // rotational acceleration
	fixang      turnroll;   // rotation caused by turn banking
	uint16_t    flags;      // misc physics flags
	physics_angle_remainder angle_remainder;	// runtime only, see above
	physics_velocity_remainder velocity_remainder;	// runtime only, see above
	/* Call this when the orientation or velocities are replaced from
	 * outside the simulation, such as when an object is loaded or received.
	 */
	void reset_remainders()
	{
		angle_remainder = {};
		velocity_remainder = {};
	}
};

struct physics_info_rw
{
	vms_vector  velocity;   // velocity vector of this object
	vms_vector  thrust;     // constant force applied to this object
	fix         mass;       // the mass of this object
	fix         drag;       // how fast this slows down
	fix         obsolete_brakes;     // how much brakes applied
	vms_vector  rotvel;     // rotational velecity (angles)
	vms_vector  rotthrust;  // rotational acceleration
	fixang      turnroll;   // rotation caused by turn banking
	uint16_t    flags;      // misc physics flags
} __pack__;

}
