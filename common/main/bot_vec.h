/*
 * This file is part of the DXX-Rebirth project <https://github.com/dxx-rebirth/dxx-rebirth/>.
 * It is copyright by its individual contributors, as recorded in the
 * project's Git history.  See COPYING.txt at the top level for license
 * terms and a link to the Git history.
 */
/*
 * The vector type of the bots' pure logic (bot_nav.h, bot_brain.h):
 * game units (the game's `fix` divided by 65536) in double precision.
 * Bots run only on the host, so floating point decisions never need to
 * agree between machines; the brain is deterministic on one machine,
 * which is what the frame rate independence needs
 * (Documentation/multiplayer-bots.md section 3.4).
 *
 * Standard library only, so that the logic is tested outside the game.
 */

#pragma once

#include <cmath>

namespace dcx::bot {

struct vec3
{
	double x{}, y{}, z{};
	constexpr vec3 &operator+=(const vec3 &o)
	{
		x += o.x;
		y += o.y;
		z += o.z;
		return *this;
	}
	constexpr vec3 &operator-=(const vec3 &o)
	{
		x -= o.x;
		y -= o.y;
		z -= o.z;
		return *this;
	}
	constexpr vec3 &operator*=(const double s)
	{
		x *= s;
		y *= s;
		z *= s;
		return *this;
	}
	constexpr bool operator==(const vec3 &) const = default;
};

[[nodiscard]]
constexpr vec3 operator+(vec3 a, const vec3 &b)
{
	return a += b;
}

[[nodiscard]]
constexpr vec3 operator-(vec3 a, const vec3 &b)
{
	return a -= b;
}

[[nodiscard]]
constexpr vec3 operator-(const vec3 &a)
{
	return {-a.x, -a.y, -a.z};
}

[[nodiscard]]
constexpr vec3 operator*(vec3 a, const double s)
{
	return a *= s;
}

[[nodiscard]]
constexpr vec3 operator*(const double s, vec3 a)
{
	return a *= s;
}

[[nodiscard]]
constexpr double dot(const vec3 &a, const vec3 &b)
{
	return a.x * b.x + a.y * b.y + a.z * b.z;
}

[[nodiscard]]
constexpr vec3 cross(const vec3 &a, const vec3 &b)
{
	return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}

[[nodiscard]]
inline double length(const vec3 &a)
{
	return std::sqrt(dot(a, a));
}

[[nodiscard]]
inline double distance(const vec3 &a, const vec3 &b)
{
	return length(a - b);
}

/* The unit vector along `a`, or the zero vector for a (nearly) zero `a`. */
[[nodiscard]]
inline vec3 normalized(const vec3 &a)
{
	const double l{length(a)};
	return l > 1e-9 ? a * (1 / l) : vec3{};
}

/* An orientation as the game keeps it: right, up and forward unit
 * vectors (vms_matrix rvec, uvec, fvec).
 */
struct frame3
{
	vec3 r{1, 0, 0}, u{0, 1, 0}, f{0, 0, 1};
	/* World to local: (right, up, forward) components. */
	[[nodiscard]]
	constexpr vec3 to_local(const vec3 &w) const
	{
		return {dot(w, r), dot(w, u), dot(w, f)};
	}
	[[nodiscard]]
	constexpr vec3 to_world(const vec3 &l) const
	{
		return r * l.x + u * l.y + f * l.z;
	}
};

}
