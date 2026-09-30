/*
 * This file is part of the DXX-Rebirth project <https://github.com/dxx-rebirth/dxx-rebirth/>.
 * It is copyright by its individual contributors, as recorded in the
 * project's Git history.  See COPYING.txt at the top level for license
 * terms and a link to the Git history.
 */
/*
 * The model of the player's ship the bots' flight tests fly
 * (test-bot-flight, test-bot-fight-sim): the controls quantised as
 * bot_apply_controls and apply_pilot_controls quantise them, the
 * frame-rate-independent drag model of physics.cpp, the rotation as
 * do_physics_sim_rot applies it (the turn roll taken off and put back),
 * the Pyro-GX (mass 4, drag 0.033, maximum thrust 7.8, maximum
 * rotational thrust 0.14).  Only for the tests.
 */

#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <numbers>

#include "bot_brain.h"

namespace dcx::bot::flight_model {

using fix = int32_t;
constexpr fix F1_0{65536};

[[nodiscard]]
constexpr fix fixmul(const fix a, const fix b)
{
	return static_cast<fix>((static_cast<int64_t>(a) * b) >> 16);
}

constexpr fix fixdiv(const fix a, const fix b)
{
	return static_cast<fix>((static_cast<int64_t>(a) << 16) / b);
}

constexpr fix to_fix(const double d)
{
	return static_cast<fix>(d * F1_0 + (d < 0 ? -0.5 : 0.5));
}

/* physics.cpp: the drag model of the old code at 200 fps. */
struct drag_model
{
	double decay_per_frame;
	double steady_state_per_accel;
};

constexpr fix drag_reference_frametime{F1_0 / 200};
constexpr fix drag_reference_fraction_of_step{20928};

inline drag_model build_drag_model(const fix drag)
{
	const fix reference_drag{fixmul(drag_reference_fraction_of_step, drag)};
	constexpr double accel_per_frame{static_cast<double>(drag_reference_fraction_of_step) / F1_0};
	const double drag_per_frame{static_cast<double>(reference_drag) / F1_0};
	const double retained_per_frame{1 - drag_per_frame};
	return {-std::log(retained_per_frame), accel_per_frame * retained_per_frame / drag_per_frame};
}

/* physics.cpp: v = v * retained + accel * gain, for this frame. */
struct drag_step
{
	double retained, gain;
};

inline drag_step drag_for_frame(const drag_model &m, const fix frametime)
{
	const double frames{static_cast<double>(frametime) / drag_reference_frametime};
	const double retained{std::exp(-m.decay_per_frame * frames)};
	return {retained, m.steady_state_per_accel * (1 - retained)};
}

/* The Pyro-GX. */
constexpr fix ship_mass{to_fix(4.0)};
constexpr fix ship_drag{to_fix(0.033)};
constexpr fix ship_max_thrust{to_fix(7.8)};
constexpr fix ship_max_rotthrust{to_fix(0.14)};
constexpr double rev_to_rad{2 * std::numbers::pi};
/* physics.cpp: turn roll. */
constexpr double turnroll_scale{(0x4ec4 / 2) / 65536.0};
constexpr double roll_rate{0x2000 / 65536.0};

/* The limits bot.cpp's compute_limits takes from physics.cpp. */
struct limits
{
	double max_speed;
	turn_response turn;
};

inline limits ship_limits()
{
	const auto lin{build_drag_model(ship_drag)};
	const auto rot{build_drag_model((ship_drag * 5) / 2)};
	const double accel{static_cast<double>(fixmul(ship_max_thrust, fixdiv(F1_0, ship_mass)))};
	const double rot_accel{static_cast<double>(fixmul(ship_max_rotthrust, fixdiv(F1_0, ship_mass)))};
	return {
		accel * lin.steady_state_per_accel / F1_0,
		{
			rot_accel * rot.steady_state_per_accel * rev_to_rad / F1_0,
			(static_cast<double>(drag_reference_frametime) / F1_0) / rot.decay_per_frame,
		},
	};
}

/* The rotation by pitch `p`, heading `h` and bank `b` (radians) in the
 * frame `f`: vm_matrix_x_matrix(f, vm_angles_2_matrix({p, b, h})).
 */
inline frame3 rotate(const frame3 &f, const double p, const double b, const double h)
{
	const double sp{std::sin(p)}, cp{std::cos(p)}, sh{std::sin(h)}, ch{std::cos(h)}, sb{std::sin(b)}, cb{std::cos(b)};
	const vec3 lf{sh * cp, -sp, ch * cp};
	const vec3 lr{cb * ch + sp * sb * sh, sb * cp, sp * sb * ch - cb * sh};
	const vec3 lu{sp * cb * sh - sb * ch, cb * cp, sb * sh + sp * cb * ch};
	frame3 n{f.to_world(lr), f.to_world(lu), f.to_world(lf)};
	/* check_and_fix_matrix */
	n.f = normalized(n.f);
	n.r = normalized(cross(n.u, n.f));
	n.u = cross(n.f, n.r);
	return n;
}

struct ship
{
	frame3 orient;
	vec3 pos, vel;
	/* rotvel, in revolutions per second (fix / 65536). */
	double pitch_rate{}, bank_rate{}, heading_rate{};
	double turnroll{};
	[[nodiscard]]
	frame3 unrolled() const
	{
		return turnroll ? rotate(orient, 0, -turnroll, 0) : orient;
	}
	/* One frame of apply_pilot_controls and do_physics_sim.  With the
	 * afterburner (`afterburner_scale` 1 to 2, apply_pilot_controls'
	 * scale of its charge) a forward axis is held for FrameTime times
	 * that, whatever the axis.
	 */
	void step(const steer_output &c, const fix ft, const double afterburner_scale = 0)
	{
		/* The production rounding (bot_apply_controls). */
		const auto held{[ft](const double axis) {
			return held_axis_time(axis, ft);
		}};
		const fix rot_scale{fixdiv(ship_max_rotthrust, ft)};
		const fix thrust_scale{fixdiv(ship_max_thrust, ft)};
		const fix inverse_mass{fixdiv(F1_0, ship_mass)};
		const double dt{static_cast<double>(ft) / F1_0};
		/* Rotation. */
		const auto rot{drag_for_frame(build_drag_model((ship_drag * 5) / 2), ft)};
		const double pitch_accel{static_cast<double>(fixmul(fixmul(held(c.pitch), rot_scale), inverse_mass))};
		const double heading_accel{static_cast<double>(fixmul(fixmul(held(c.heading), rot_scale), inverse_mass))};
		pitch_rate = pitch_rate * rot.retained + pitch_accel * rot.gain / F1_0;
		heading_rate = heading_rate * rot.retained + heading_accel * rot.gain / F1_0;
		bank_rate *= rot.retained;
		auto f{unrolled()};
		f = rotate(f, pitch_rate * dt * rev_to_rad, bank_rate * dt * rev_to_rad, heading_rate * dt * rev_to_rad);
		const double desired_bank{-heading_rate * turnroll_scale * rev_to_rad};
		const double max_roll{roll_rate * dt * rev_to_rad};
		turnroll += std::clamp(desired_bank - turnroll, -max_roll, max_roll);
		/* The thrust, in the rolled frame. */
		const auto thrust_frame{rotate(f, 0, turnroll, 0)};
		orient = thrust_frame;
		const auto lin{drag_for_frame(build_drag_model(ship_drag), ft)};
		const auto axis_accel{[&](const double axis) {
			return static_cast<double>(fixmul(fixmul(held(axis), thrust_scale), inverse_mass)) / F1_0;
		}};
		const double forward_accel{afterburner_scale > 0 && c.forward > 0
			? static_cast<double>(fixmul(fixmul(static_cast<fix>(std::lround(ft * afterburner_scale)), thrust_scale), inverse_mass)) / F1_0
			: axis_accel(c.forward)};
		const auto accel{thrust_frame.to_world({axis_accel(c.sideways), axis_accel(c.vertical), forward_accel})};
		vel = vel * lin.retained + accel * lin.gain;
		pos += vel * dt;
	}
};

inline steer_output steer(const ship &s, const vec3 &face_dir, const vec3 &face_rate, const vec3 &move_cmd, const limits &lim, const double cap, int &heading_pref)
{
	return steer_controls({
		.unrolled = s.unrolled(),
		.thrust_frame = s.orient,
		.face_dir = face_dir,
		.face_rate = face_rate,
		.move_cmd = move_cmd,
		.pitch_rate = s.pitch_rate * rev_to_rad,
		.heading_rate = s.heading_rate * rev_to_rad,
	}, lim.turn, cap, heading_pref);
}

constexpr std::array<double, 4> frame_rates{{30, 60, 144, 500}};

/* The 60 Hz brain tick of a frame loop: how many ticks this frame. */
struct ticker
{
	int64_t elapsed{};
	int64_t ticks{};
	unsigned advance(const fix ft)
	{
		elapsed += ft;
		const int64_t now{elapsed * BOT_TICK_RATE / F1_0};
		const auto n{static_cast<unsigned>(now - ticks)};
		ticks = now;
		return n;
	}
};

}
