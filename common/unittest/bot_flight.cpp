/*
 * This file is part of the DXX-Rebirth project <https://github.com/dxx-rebirth/dxx-rebirth/>.
 * It is copyright by its individual contributors, as recorded in the
 * project's Git history.  See COPYING.txt at the top level for license
 * terms and a link to the Git history.
 */
/*
 * Test of the bots' flight (Documentation/multiplayer-bots.md sections
 * 3.4, 4.3, 4.6 and 9.1): the steering controller of bot_brain.h flies a
 * model of the player's ship at 30, 60, 144 and 500 fps.  The model is
 * the game's: the controls are quantised as bot_apply_controls and
 * apply_pilot_controls quantise them (the time an axis is held, in fix,
 * scaled by the ship's maximum thrust over FrameTime), the velocities
 * follow the frame-rate-independent drag model of physics.cpp, the
 * rotation is applied as do_physics_sim_rot applies it (the angles of
 * vm_angles_2_matrix, the turn roll taken off and put back), and the
 * ship is the Pyro-GX (mass 4, drag 0.033, maximum thrust 7.8, maximum
 * rotational thrust 0.14).
 *
 * The ship flies a path of waypoints with bends, turns to face targets
 * (behind it, straight above it) without oscillating, tracks a strafing
 * target inside the fire cone, and moves around while it fights.
 *
 * Build and run with SCons:
 *
 *	scons sdl2=1 d1x=0 d2x=1 register_runtime_test_plain_link_targets=1 test-bot-flight
 *	build/common/test-bot-flight
 */

#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "bot_brain.h"
#include "bot_nav.h"

using namespace dcx::bot;

namespace {

void check_failed(const char *const expr, const char *const file, const int line)
{
	std::fprintf(stderr, "%s:%d: check failed: %s\n", file, line, expr);
	std::exit(1);
}

#define CHECK(cond)	do { if (!(cond)) check_failed(#cond, __FILE__, __LINE__); } while (0)

using fix = int32_t;
constexpr fix F1_0{65536};

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

drag_model build_drag_model(const fix drag)
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

drag_step drag_for_frame(const drag_model &m, const fix frametime)
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

limits ship_limits()
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
frame3 rotate(const frame3 &f, const double p, const double b, const double h)
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
	/* One frame of apply_pilot_controls and do_physics_sim. */
	void step(const steer_output &c, const fix ft)
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
		const auto accel{thrust_frame.to_world({axis_accel(c.sideways), axis_accel(c.vertical), axis_accel(c.forward)})};
		vel = vel * lin.retained + accel * lin.gain;
		pos += vel * dt;
	}
};

steer_output steer(const ship &s, const vec3 &face_dir, const vec3 &face_rate, const vec3 &move_cmd, const limits &lim, const double cap, int &heading_pref)
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

/* The held time of an axis (held_axis_time, as bot_apply_controls uses
 * it): full axes are FrameTime, the result is clamped, and small axes are
 * rounded to nearest, symmetric about zero (no bias toward minus
 * infinity at 500 fps, FrameTime 131).
 */
void test_held_axis_time()
{
	CHECK(held_axis_time(1, 131) == 131);
	CHECK(held_axis_time(-1, 131) == -131);
	CHECK(held_axis_time(3, 131) == 131);
	CHECK(held_axis_time(-3, 131) == -131);
	CHECK(held_axis_time(0, 131) == 0);
	for (const double a : {0.001, 0.003, 0.004, 0.01, 0.25, 0.5, 0.77})
		CHECK(held_axis_time(a, 131) == -held_axis_time(-a, 131));
	CHECK(held_axis_time(0.003, 131) == 0);
	CHECK(held_axis_time(0.004, 131) == 1);
	CHECK(held_axis_time(-0.003, 131) == 0);
	CHECK(held_axis_time(0.5, 1092) == 546);
}

void test_limits()
{
	const auto lim{ship_limits()};
	/* Known figures of the Pyro-GX: about 58 units per second, about 150
	 * degrees per second, 0.19 s to answer a turn.
	 */
	CHECK(lim.max_speed > 55 && lim.max_speed < 62);
	CHECK(lim.turn.max_rate > radians(140) && lim.turn.max_rate < radians(160));
	CHECK(lim.turn.time_constant > 0.15 && lim.turn.time_constant < 0.22);
	/* Full forward thrust reaches the top speed at any frame rate. */
	for (const double fps : frame_rates)
	{
		const fix ft{to_fix(1 / fps)};
		ship s;
		for (double t = 0; t < 4; t += ft / 65536.0)
			s.step({.forward = 1}, ft);
		CHECK(std::abs(length(s.vel) - lim.max_speed) < 1);
	}
}

/* Turn to face a fixed direction: settles, no oscillation. */
void test_face(const vec3 &target_local, const double cap, const double settle_limit)
{
	const auto lim{ship_limits()};
	std::vector<double> settle_times;
	for (const double fps : frame_rates)
	{
		const fix ft{to_fix(1 / fps)};
		ship s;
		const auto goal{normalized(target_local)};
		int pref{1};
		double t{0}, settled{-1};
		unsigned reversals{0};
		double last_err{angle_between(s.orient.f, goal)};
		bool closing{true};
		while (t < 4)
		{
			const auto c{steer(s, goal, {}, {}, lim, cap, pref)};
			CHECK(std::abs(c.pitch) <= cap + 1e-12 && std::abs(c.heading) <= cap + 1e-12);
			s.step(c, ft);
			t += ft / 65536.0;
			const double err{angle_between(s.orient.f, goal)};
			/* The error only shrinks, up to noise, until it settles. */
			if (err > last_err + radians(0.05))
			{
				if (closing && err > radians(1))
					++reversals;
				closing = false;
			}
			else if (err < last_err - radians(0.05))
				closing = true;
			last_err = err;
			if (settled < 0 && err < radians(1))
				settled = t;
			if (settled >= 0)
				/* Stays on target: no overshoot, no hunting. */
				CHECK(err < radians(2));
		}
		CHECK(settled > 0 && settled < settle_limit);
		CHECK(reversals == 0);
		settle_times.push_back(settled);
	}
	for (const auto st : settle_times)
		CHECK(std::abs(st - settle_times.front()) < 0.1);
}

void test_turns()
{
	/* 90 degrees right, 150 degrees round, straight behind, straight up,
	 * and a direction just above and behind (the heading of a direction
	 * near the vertical was unstable before the shortest-rotation errors).
	 */
	test_face({1, 0, 0}, 0.75, 1.4);
	test_face({0.5, -0.1, -0.86}, 0.75, 1.8);
	test_face({0.001, 0, -1}, 0.75, 2.3);
	test_face({0, 1, 0}, 0.75, 1.4);
	test_face({0.02, 1, -0.05}, 0.75, 1.4);
	test_face({-0.3, -0.9, 0.3}, 1.0, 1.1);
}

/* A strafing target, circling the bot at 60 units at 50 units per second
 * and bobbing: with the feed-forward the bot keeps it inside the Hotshot
 * fire cone; without it (B1) never.
 */
void test_tracking()
{
	const auto lim{ship_limits()};
	const double cone{radians(skill_of(bot_skill::hotshot).fire_cone_deg)};
	for (const bool feed_forward : {true, false})
		for (const double fps : frame_rates)
		{
			const fix ft{to_fix(1 / fps)};
			ship s;
			int pref{1};
			double t{0};
			unsigned in_cone{0}, samples{0}, fired{0};
			const double w{50.0 / 60};
			const auto target{[w](const double time) {
				return vec3{60 * std::sin(w * time), 20 * std::sin(1.3 * time), 60 * std::cos(w * time)};
			}};
			const auto target_vel{[w](const double time) {
				return vec3{60 * w * std::cos(w * time), 26 * std::cos(1.3 * time), -60 * w * std::sin(w * time)};
			}};
			ticker tk;
			vec3 face{0, 0, 1}, rate;
			while (t < 8)
			{
				for (unsigned n{tk.advance(ft)}; n; --n)
				{
					const double tt{static_cast<double>(tk.ticks) / BOT_TICK_RATE};
					const auto rel{target(tt) - s.pos};
					face = normalized(rel);
					rate = feed_forward ? line_of_sight_rate(rel, target_vel(tt) - s.vel) : vec3{};
					if (tt > 1)
					{
						const double err{angle_between(s.orient.f, face)};
						++samples;
						in_cone += err < cone;
						fired += should_fire(err, cone, true, length(rel), 300);
					}
				}
				s.step(steer(s, face, rate, {}, lim, 0.75, pref), ft);
				t += ft / 65536.0;
			}
			if (feed_forward)
			{
				CHECK(in_cone > samples * 95 / 100);
				CHECK(fired == in_cone);
			}
			else
				CHECK(in_cone < samples / 5);
		}
}

/* A path with bends in three dimensions, flown as follow_path flies it:
 * all the way, at the same pace at any frame rate, without weaving.
 */
void test_waypoints()
{
	const auto lim{ship_limits()};
	const std::vector<vec3> points{
		{0, 0, 40}, {0, 0, 100}, {60, 0, 100}, {60, 50, 110}, {60, 50, 170}, {0, 60, 170}, {-40, 20, 140},
	};
	double course{length(points[0])};
	for (std::size_t i = 1; i < points.size(); ++i)
		course += distance(points[i - 1], points[i]);
	std::vector<double> arrival;
	for (const double fps : frame_rates)
	{
		const fix ft{to_fix(1 / fps)};
		ship s;
		int pref{1};
		std::size_t index{0};
		double t{0}, flown{0}, arrived{-1};
		const double reach{6.6};
		ticker tk;
		vec3 face{0, 0, 1}, move;
		unsigned sign_changes{0};
		int last_sign{0};
		while (t < 20 && arrived < 0)
		{
			for (unsigned n{tk.advance(ft)}; n; --n)
			{
				index = advance_along(points, index, s.pos, reach);
				const auto &p{points[index]};
				const auto to{p - s.pos};
				const double dist{length(to)};
				const bool last{index + 1 == points.size()};
				double speed{lim.max_speed};
				if (last)
					speed = std::min(speed, dist * 1.5);
				else if (angle_between(to, points[index + 1] - p) > radians(60) && dist < 40)
					speed = std::min(speed, std::max(lim.max_speed * 0.4, dist * 1.5));
				if (last && dist < reach)
				{
					arrived = t;
					break;
				}
				face = normalized(to);
				move = velocity_command(face * speed, s.vel, lim.max_speed);
			}
			const auto c{steer(s, face, {}, move, lim, 0.75, pref)};
			/* Weaving: the heading axis swinging from one side to the
			 * other at more than a quarter of its range.
			 */
			const int sign{c.heading > 0.25 ? 1 : c.heading < -0.25 ? -1 : 0};
			if (sign && last_sign && sign != last_sign)
				++sign_changes;
			if (sign)
				last_sign = sign;
			const auto before{s.pos};
			s.step(c, ft);
			flown += distance(before, s.pos);
			t += ft / 65536.0;
		}
		CHECK(arrived > 0);
		/* About the top speed on average, along about the course. */
		CHECK(arrived < course / lim.max_speed * 1.8);
		CHECK(flown < course * 1.25);
		/* One swing per bend at most. */
		CHECK(sign_changes <= points.size());
		arrival.push_back(arrived);
	}
	for (const auto a : arrival)
		CHECK(std::abs(a - arrival.front()) < arrival.front() * 0.08);
}

/* A fight in the open against a target that keeps still: the bot goes
 * round it, in and out, without shaking on one spot, and keeps it inside
 * the fire cone most of the time.
 */
void test_combat_movement()
{
	const auto lim{ship_limits()};
	const auto &sk{skill_of(bot_skill::hotshot)};
	for (const double fps : frame_rates)
	{
		const fix ft{to_fix(1 / fps)};
		ship s;
		s.pos = {0, 0, -70};
		int pref{1};
		bot_rng rng{bot_seed(7, 2, 1)};
		juke_state juke;
		const vec3 target{};
		ticker tk;
		vec3 face{0, 0, 1}, rate, move;
		double t{0}, min_d{1e9}, max_d{0}, speed_sum{0};
		unsigned ticks{0}, reversals{0}, in_cone{0};
		vec3 lo{1e9, 1e9, 1e9}, hi{-1e9, -1e9, -1e9};
		std::vector<vec3> vels;
		while (t < 30)
		{
			for (unsigned n{tk.advance(ft)}; n; --n)
			{
				juke.update(rng, ticks_from_ms(sk.strafe_min_ms), ticks_from_ms(sk.strafe_max_ms), 35, 95);
				const auto to{target - s.pos};
				face = normalized(to);
				rate = line_of_sight_rate(to, -s.vel);
				const auto wanted{combat_velocity(to, s.orient.r, s.orient.u, juke, sk.strafe_vertical, lim.max_speed * 0.8, lim.max_speed * 0.7)};
				move = velocity_command(wanted, s.vel, lim.max_speed);
				++ticks;
				if (ticks > 60)
				{
					const double d{length(to)};
					min_d = std::min(min_d, d);
					max_d = std::max(max_d, d);
					speed_sum += length(s.vel);
					in_cone += angle_between(s.orient.f, face) < radians(sk.fire_cone_deg);
					lo = {std::min(lo.x, s.pos.x), std::min(lo.y, s.pos.y), std::min(lo.z, s.pos.z)};
					hi = {std::max(hi.x, s.pos.x), std::max(hi.y, s.pos.y), std::max(hi.z, s.pos.z)};
					vels.push_back(s.vel);
				}
			}
			s.step(steer(s, face, rate, move, lim, sk.turn_cap, pref), ft);
			t += ft / 65536.0;
		}
		const unsigned samples{ticks - 60};
		/* Keeps the band, roughly. */
		CHECK(min_d > 20 && max_d < 120);
		/* Moves: at half the top speed on average, over a wide area. */
		CHECK(speed_sum / samples > lim.max_speed * 0.45);
		const auto span{hi - lo};
		CHECK(span.x > 60 || span.z > 60);
		CHECK(span.y > 15);
		/* No shaking: the velocity turns round (against its direction of
		 * a quarter second before) at most about once a second.
		 */
		for (std::size_t i = 15; i < vels.size(); i += 15)
			reversals += dot(vels[i], vels[i - 15]) < 0;
		CHECK(reversals < 30 * 0.8);
		/* And it can shoot while it does. */
		CHECK(in_cone > samples * 80 / 100);
	}
}

}

int main()
{
	test_held_axis_time();
	test_limits();
	test_turns();
	test_tracking();
	test_waypoints();
	test_combat_movement();
	std::puts("test-bot-flight: all checks passed");
	return 0;
}
