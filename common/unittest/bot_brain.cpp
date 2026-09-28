/*
 * This file is part of the DXX-Rebirth project <https://github.com/dxx-rebirth/dxx-rebirth/>.
 * It is copyright by its individual contributors, as recorded in the
 * project's Git history.  See COPYING.txt at the top level for license
 * terms and a link to the Git history.
 */
/*
 * Test of the bots' brain logic (bot_brain.h,
 * Documentation/multiplayer-bots.md sections 3.4, 4, 5 and 8.1): the
 * intercept solver and lead, the aim error model, the reaction delay,
 * target memory and scoring, the steering and velocity controllers at
 * different frame lengths, the tick schedule (the decisions are the same
 * for frame times from 2 ms to 100 ms), the skill and style tables and
 * the primary choice, and the slot a bot takes (section 2.3).
 *
 * Build and run with SCons:
 *
 *	scons sdl2=1 d1x=0 d2x=1 register_runtime_test_plain_link_targets=1 test-bot-brain
 *	build/common/test-bot-brain
 */

#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <numbers>
#include <vector>

#include "bot_brain.h"
#include "net_interp.h"

using namespace dcx::bot;

namespace {

void check_failed(const char *const expr, const char *const file, const int line)
{
	std::fprintf(stderr, "%s:%d: check failed: %s\n", file, line, expr);
	std::exit(1);
}

#define CHECK(cond)	do { if (!(cond)) check_failed(#cond, __FILE__, __LINE__); } while (0)

bool near(const double a, const double b, const double eps)
{
	return std::abs(a - b) <= eps;
}

void test_intercept()
{
	/* Straight-line targets: the shot and the target meet. */
	bot_rng rng{7};
	for (unsigned i = 0; i < 1000; ++i)
	{
		const vec3 shooter{rng.uniform(-100, 100), rng.uniform(-100, 100), rng.uniform(-100, 100)};
		const vec3 pos{rng.uniform(-300, 300), rng.uniform(-300, 300), rng.uniform(-300, 300)};
		const vec3 vel{rng.uniform(-40, 40), rng.uniform(-40, 40), rng.uniform(-40, 40)};
		const double speed{rng.uniform(80, 200)};
		const auto t{intercept_time(pos - shooter, vel, speed)};
		CHECK(t.has_value());
		CHECK(*t > 0);
		const auto aim{aim_point(shooter, pos, vel, speed, 1.0)};
		const auto dir{normalized(aim - shooter)};
		const auto shot_at_t{shooter + dir * (speed * *t)};
		const auto target_at_t{pos + vel * *t};
		CHECK(distance(shot_at_t, target_at_t) < 1e-6 * (1 + length(pos - shooter)));
	}
	/* A target that outruns the shot, straight away from it: none. */
	CHECK(!intercept_time({100, 0, 0}, {50, 0, 0}, 40).has_value());
	/* Coming toward it faster than the shot: it still meets. */
	const auto t{intercept_time({100, 0, 0}, {-50, 0, 0}, 40)};
	CHECK(t.has_value() && near(*t, 100.0 / 90, 1e-9));
	/* Lead 0: at the target. */
	const vec3 p{100, 20, 0}, v{0, 30, 0};
	CHECK(distance(aim_point({}, p, v, 100, 0), p) < 1e-12);
	/* Lead scales the velocity: lead 0.5 is the intercept of v / 2. */
	const auto half{aim_point({}, p, v, 100, 0.5)};
	const auto th{intercept_time(p, v * 0.5, 100)};
	CHECK(th && distance(half, p + v * 0.5 * *th) < 1e-9);
	/* Without an intercept: led by the flight time to the current
	 * position.
	 */
	const auto out{aim_point({}, {100, 0, 0}, {0, 500, 0}, 100, 1.0)};
	CHECK(distance(out, vec3{100, 500, 0}) < 1e-9);
	CHECK(intercept_time({}, {1, 0, 0}, 10) == 0.0);
	CHECK(!intercept_time({1, 0, 0}, {}, 0).has_value());
}

void test_aim_error()
{
	for (const auto &sk : skill_table)
	{
		const double sigma{radians(sk.aim_sigma_deg)};
		const unsigned drift{ticks_from_ms(sk.aim_drift_ms)};
		bot_rng rng{12345};
		aim_error e;
		double sum{0}, sum2{0};
		unsigned samples{0}, changes{0};
		double last_target{0};
		unsigned since_change{0};
		for (unsigned t = 0; t < 100000 * drift; t += 1)
		{
			e.update(rng, sigma, drift);
			++since_change;
			if (e.target_yaw() != last_target)
			{
				/* A new sample exactly every drift period. */
				if (changes)
					CHECK(since_change == drift);
				since_change = 0;
				last_target = e.target_yaw();
				++changes;
				sum += e.target_yaw();
				sum2 += e.target_yaw() * e.target_yaw();
				++samples;
			}
			/* The aim eases toward the sample, never beyond it. */
			CHECK(std::abs(e.yaw()) <= 8 * sigma);
			if (samples >= 100000)
				break;
		}
		const double mean{sum / samples};
		const double sd{std::sqrt(sum2 / samples - mean * mean)};
		CHECK(std::abs(mean) < 0.02 * sigma);
		CHECK(std::abs(sd / sigma - 1) < 0.02);
	}
	/* Deterministic for a seed. */
	bot_rng a{99}, b{99};
	aim_error ea, eb;
	for (unsigned i = 0; i < 1000; ++i)
	{
		ea.update(a, 0.05, 24);
		eb.update(b, 0.05, 24);
		CHECK(ea.yaw() == eb.yaw() && ea.pitch() == eb.pitch());
	}
	/* The offset turns the direction by the given angles. */
	const vec3 dir{0, 0, 1};
	const auto turned{apply_aim_offset(dir, {0, 1, 0}, 0.1, 0)};
	CHECK(near(angle_between(dir, turned), 0.1, 1e-12));
	CHECK(turned.x > 0);
	const auto pitched{apply_aim_offset(dir, {0, 1, 0}, 0, 0.1)};
	CHECK(near(angle_between(dir, pitched), 0.1, 1e-12));
	CHECK(pitched.y > 0);
	/* Straight up with an up hint along it: still a valid result. */
	const auto up{apply_aim_offset({0, 1, 0}, {0, 1, 0}, 0.1, 0)};
	CHECK(near(length(up), 1, 1e-12));
}

/* Section 4.4: the lead factor is 1 on average, its spread shrinks
 * with the skill, Trainee does not lead, Insane leads exactly.
 */
void test_aim_lead()
{
	double last_sd{1e9};
	for (const auto &sk : skill_table)
	{
		bot_rng rng{4242};
		aim_lead l;
		const unsigned drift{ticks_from_ms(sk.aim_drift_ms)};
		double sum{0}, sum2{0};
		const unsigned n{200000};
		for (unsigned i = 0; i < n; ++i)
		{
			l.update(rng, sk.lead, drift);
			sum += l.factor();
			sum2 += l.factor() * l.factor();
			CHECK(l.factor() >= 0);
		}
		const double mean{sum / n};
		const double sd{std::sqrt(std::max(0.0, sum2 / n - mean * mean))};
		if (sk.lead <= 0)
			CHECK(mean == 0 && sd == 0);
		else
		{
			/* Unbiased: B1 used `lead` itself (Hotshot 0.7). */
			CHECK(std::abs(mean - 1) < 0.02);
			CHECK(sd <= last_sd);
			last_sd = sd;
		}
		if (sk.lead >= 1)
			CHECK(sd < 1e-9);
	}
	/* Spread weapons: a slightly wider cone; missiles: slower on
	 * average than their top speed.
	 */
	CHECK(fire_cone_with_spread(0.1, 0) == 0.1);
	CHECK(fire_cone_with_spread(0.1, 0.12) > 0.1 && fire_cone_with_spread(0.1, 0.12) < 0.1 + 0.12);
	CHECK(fire_cone_with_spread(0.1, -1) == 0.1);
	CHECK(effective_shot_speed(200, false) == 200);
	CHECK(effective_shot_speed(200, true) < 200 && effective_shot_speed(200, true) > 100);
}

/* A shooting range for the aim (section 9.3, (c)): a bot with the
 * Hotshot preset fires 4 shots a second at a ship at `distance` that
 * strafes across the line of fire at 35-58 units/s, turning to a new
 * direction every `run_lo`-`run_hi` seconds.  The bot sees it a reaction
 * time late (dead reckoning from what it saw), leads with its lead
 * factor (`fixed_lead` > 0: B1's fixed fraction instead), and aims with
 * its drifting aim error; its nose follows the aim exactly (the flight
 * test covers the tracking).  A shot hits when it passes within the
 * ship's radius plus its own.  Returns the share of shots that hit.
 */
double shooting_range(const double shot_speed, const double distance, const double run_lo, const double run_hi, const double fixed_lead, const uint32_t seed)
{
	const auto &sk{skill_of(bot_skill::hotshot)};
	bot_rng rng{seed};
	constexpr double dt{1.0 / 240};
	constexpr double radius{4.6 + 1};
	vec3 tp{0, 0, distance}, tv;
	double next_run{0};
	aim_error aim;
	aim_lead lead;
	struct seen
	{
		vec3 pos, vel;
	};
	std::vector<seen> ring;
	struct shot
	{
		vec3 pos, vel;
		double age;
	};
	std::vector<shot> shots;
	unsigned fired{0}, hits{0};
	double t{0}, next_fire{1}, next_seen{0}, next_tick{0};
	vec3 face{0, 0, 1};
	const unsigned reaction_ticks{ticks_from_ms(sk.reaction_ms)};
	const double delay{reaction_ticks / static_cast<double>(BOT_TICK_RATE)};
	while (t < 60)
	{
		if (t >= next_run)
		{
			const double a{rng.uniform(0, 2 * std::numbers::pi)};
			const double v{rng.uniform(35, 58)};
			tv = {std::cos(a) * v, std::sin(a) * v, 0};
			next_run = t + rng.uniform(run_lo, run_hi);
		}
		/* The ship stays in front of the bot. */
		if (length(vec3{tp.x, tp.y, 0}) > 60)
			tv = normalized(vec3{-tp.x, -tp.y, 0}) * 50;
		tp += tv * dt;
		if (t >= next_seen)
		{
			ring.push_back({tp, tv});
			next_seen += 1.0 / (BOT_TICK_RATE / PERCEPTION_DIVISOR);
		}
		if (t >= next_tick)
		{
			next_tick += 1.0 / BOT_TICK_RATE;
			aim.update(rng, radians(sk.aim_sigma_deg), ticks_from_ms(sk.aim_drift_ms));
			lead.update(rng, sk.lead, ticks_from_ms(sk.aim_drift_ms));
			const std::size_t k{reaction_ticks / PERCEPTION_DIVISOR};
			const auto &p{ring[ring.size() > k ? ring.size() - 1 - k : 0]};
			const auto est{p.pos + p.vel * delay};
			const auto at{aim_point({}, est, p.vel, shot_speed, fixed_lead > 0 ? fixed_lead : lead.factor())};
			face = apply_aim_offset(at, {0, 1, 0}, aim.yaw(), aim.pitch());
		}
		if (t >= next_fire)
		{
			shots.push_back({{}, face * shot_speed, 0});
			++fired;
			next_fire += 0.25;
		}
		for (auto it{shots.begin()}; it != shots.end();)
		{
			it->pos += it->vel * dt;
			it->age += dt;
			if (dcx::bot::distance(it->pos, tp) < radius)
			{
				++hits;
				it = shots.erase(it);
			}
			else if (it->pos.z > distance + 30 || it->age > 4)
				it = shots.erase(it);
			else
				++it;
		}
		t += dt;
	}
	return fired ? static_cast<double>(hits) / fired : 0;
}

double range_average(const double shot_speed, const double distance, const double run_lo, const double run_hi, const double fixed_lead)
{
	double sum{0};
	constexpr unsigned seeds{6};
	for (uint32_t s = 1; s <= seeds; ++s)
		sum += shooting_range(shot_speed, distance, run_lo, run_hi, fixed_lead, s * 7919);
	return sum / seeds;
}

/* Section 9.3, (c): shots at a ship that flies straight across the
 * line of fire at 35-58 units/s (in any direction across it), at
 * `distance`: only the aim error and the lead error make a shot miss.
 * Each shot takes the aim and lead errors of a random moment.  Returns
 * the share that pass within the ship's radius plus the shot's.
 */
double steady_target_hits(const double shot_speed, const double distance, const double fixed_lead)
{
	const auto &sk{skill_of(bot_skill::hotshot)};
	bot_rng rng{31337};
	aim_error aim;
	aim_lead lead;
	constexpr double radius{4.6 + 1};
	constexpr unsigned shots{20000};
	unsigned hits{0};
	for (unsigned i = 0; i < shots; ++i)
	{
		for (unsigned k{1 + rng.below(30)}; k--;)
		{
			aim.update(rng, radians(sk.aim_sigma_deg), ticks_from_ms(sk.aim_drift_ms));
			lead.update(rng, sk.lead, ticks_from_ms(sk.aim_drift_ms));
		}
		const double a{rng.uniform(0, 2 * std::numbers::pi)};
		const double v{rng.uniform(35, 58)};
		const vec3 pos{0, 0, distance}, vel{std::cos(a) * v, std::sin(a) * v, 0};
		const auto at{aim_point({}, pos, vel, shot_speed, fixed_lead > 0 ? fixed_lead : lead.factor())};
		const auto dir{apply_aim_offset(at, {0, 1, 0}, aim.yaw(), aim.pitch())};
		/* The closest pass of the shot by the ship. */
		const auto rel_vel{vel - dir * shot_speed};
		const double t{std::max(0.0, -dot(pos, rel_vel) / dot(rel_vel, rel_vel))};
		if (length(pos + rel_vel * t) < radius)
			++hits;
	}
	return static_cast<double>(hits) / shots;
}

/* Section 9.3, (c): hits by shot speed.  The speeds span the game's
 * primaries (the slow blobs to vulcan and gauss); the laser's 120 is
 * the reference.
 */
void test_hit_rates_by_speed()
{
	constexpr std::array<double, 6> speeds{{80, 100, 120, 160, 200, 300}};
	constexpr std::size_t laser{2};
	for (const double distance : {40.0, 60.0, 90.0})
	{
		std::array<double, speeds.size()> now{}, b1{};
		for (std::size_t i = 0; i < speeds.size(); ++i)
		{
			now[i] = steady_target_hits(speeds[i], distance, 0);
			b1[i] = steady_target_hits(speeds[i], distance, skill_of(bot_skill::hotshot).lead);
			std::printf("test-bot-brain: speed %3.0f, a ship crossing at %2.0f: %4.1f %% of the shots hit (B1 %4.1f %%)\n", speeds[i], distance, 100 * now[i], 100 * b1[i]);
		}
		for (std::size_t i = 0; i < speeds.size(); ++i)
		{
			/* Comparable to the laser whatever the speed: the lead is
			 * right on average, so only its spread (which grows with the
			 * flight time) and the aim error miss.
			 */
			CHECK(now[i] >= (speeds[i] < 100 ? 0.7 : 0.8) * now[laser]);
			CHECK(now[i] <= 1.25 * now[laser]);
			/* B1's fixed lead (0.7) fell with the flight time. */
			if (speeds[i] <= 120)
				CHECK(now[i] > b1[i]);
		}
		CHECK(now[laser] > 0.2);
	}
	/* B1 at 60 units: the slow shots hit far less than the laser. */
	CHECK(steady_target_hits(80, 60, 0.7) < 0.6 * steady_target_hits(120, 60, 0.7));
	/* Against a strafing and a juking ship the slow shots stay behind
	 * (the ship has more time to turn away, as it has from a human's
	 * shots); these only must not collapse.
	 */
	std::array<double, speeds.size()> strafer{}, juker{};
	for (std::size_t i = 0; i < speeds.size(); ++i)
	{
		strafer[i] = range_average(speeds[i], 60, 1.5, 3, 0);
		juker[i] = range_average(speeds[i], 45, 0.6, 1.4, 0);
		std::printf("test-bot-brain: speed %3.0f: strafing ship at 60 %4.1f %%, juking at 45 %4.1f %%\n", speeds[i], 100 * strafer[i], 100 * juker[i]);
	}
	for (std::size_t i = 0; i < speeds.size(); ++i)
	{
		CHECK(strafer[i] >= (speeds[i] < 100 ? 0.4 : 0.6) * strafer[laser]);
		CHECK(juker[i] >= (speeds[i] < 100 ? 0.25 : 0.5) * juker[laser]);
	}
}

void test_reaction_and_memory()
{
	delay_line<int, 40> d;
	CHECK(d.delayed(0) == nullptr);
	for (int i = 0; i < 100; ++i)
	{
		d.push(i);
		const unsigned k{ticks_from_ms(280)};
		/* Exactly k pushes old, once there are enough. */
		if (i >= static_cast<int>(k))
			CHECK(*d.delayed(k) == i - static_cast<int>(k));
		else
			CHECK(*d.delayed(k) == 0);
		CHECK(*d.delayed(0) == i);
	}
	CHECK(d.size() == 40);
	CHECK(*d.delayed(1000) == 60);
	d.clear();
	CHECK(d.delayed(0) == nullptr);
	/* Every preset's reaction fits the line used by the game. */
	for (const auto &sk : skill_table)
		CHECK(ticks_from_ms(sk.reaction_ms) < 40);
	CHECK(ticks_from_ms(280) == 17);
	CHECK(ticks_from_ms(1000) == 60);

	target_memory m;
	CHECK(memory_confidence(m, 10, 300) == 0);
	m.valid = true;
	m.tick = 100;
	CHECK(memory_confidence(m, 100, 300) == 1);
	CHECK(near(memory_confidence(m, 250, 300), 0.5, 1e-12));
	CHECK(memory_confidence(m, 400, 300) == 0);
	CHECK(memory_confidence(m, 1000, 300) == 0);

	CHECK(in_field_of_view({0, 0, 1}, {0, 0, 5}, 70));
	CHECK(in_field_of_view({0, 0, 1}, {1, 0, 1}, 70));
	CHECK(!in_field_of_view({0, 0, 1}, {1, 0, 0.2}, 70));
	CHECK(!in_field_of_view({0, 0, 1}, {0, 0, -1}, 90));
	CHECK(in_field_of_view({0, 0, 1}, {1, 0, 0}, 90));
}

void test_target_choice()
{
	const double aw{350};
	target_candidate a{.id = 1, .visible = true, .distance = 50};
	target_candidate b{.id = 2, .visible = true, .distance = 60};
	std::vector<target_candidate> c{a, b};
	CHECK(choose_target(c, std::nullopt, aw) == 1);
	/* Hysteresis: the current target keeps it against a slightly better
	 * one...
	 */
	c[0].distance = 100;
	CHECK(target_score(c[1], aw) > target_score(c[0], aw));
	CHECK(choose_target(c, uint8_t{1}, aw) == 1);
	CHECK(choose_target(c, std::nullopt, aw) == 2);
	/* ...but not against a much better one. */
	c[0].distance = 340;
	CHECK(choose_target(c, uint8_t{1}, aw) == 2);
	/* Excluded (teammates) never. */
	c[1].excluded = true;
	CHECK(choose_target(c, std::nullopt, aw) == 1);
	c[0].excluded = true;
	CHECK(!choose_target(c, std::nullopt, aw).has_value());
	/* Remembered only: half, scaled by confidence; forgotten: none. */
	target_candidate m{.id = 3, .visible = false, .confidence = 0.5, .distance = 10};
	CHECK(near(target_score(m, aw), 0.25, 1e-12));
	m.confidence = 0;
	CHECK(target_score(m, aw) == 0);
	/* The bounty doubles, revenge and a weak target add. */
	target_candidate v{.id = 4, .visible = true, .distance = 10};
	const double base{target_score(v, aw)};
	CHECK(base == 1);
	v.bounty = true;
	CHECK(near(target_score(v, aw), 2, 1e-12));
	v.damaged_me_recently = true;
	v.low_shields = true;
	CHECK(near(target_score(v, aw), 2 * 1.5 * 1.3, 1e-12));
	/* The range factor falls to 0.3 at the awareness radius. */
	target_candidate far{.id = 5, .visible = true, .distance = aw};
	CHECK(near(target_score(far, aw), 0.3, 1e-12));
	/* Ties go to the lower id. */
	std::vector<target_candidate> tie{{.id = 6, .visible = true, .distance = 10}, {.id = 5, .visible = true, .distance = 10}};
	CHECK(choose_target(tie, std::nullopt, aw) == 5);
}

/* The rotation of a ship under the controller: the exact first-order
 * response over each frame.
 */
struct turn_sim
{
	turn_response ship;
	double angle{}, rate{};
	void step(const double axis, const double dt)
	{
		const double target{axis * ship.max_rate};
		const double k{std::exp(-dt / ship.time_constant)};
		/* Exact integral of rate over the frame. */
		angle += target * dt + (rate - target) * ship.time_constant * (1 - k);
		rate = target + (rate - target) * k;
	}
};

void test_steering()
{
	/* Directions in the ship frame. */
	auto e{steer_errors_local({0, 0, 1})};
	CHECK(e.pitch == 0 && e.heading == 0);
	e = steer_errors_local({1, 0, 1});
	CHECK(near(e.heading, std::numbers::pi / 4, 1e-12) && e.pitch == 0);
	e = steer_errors_local({0, 1, 1});
	CHECK(near(e.pitch, -std::numbers::pi / 4, 1e-12));
	e = steer_errors_local({0, 1, 0});
	CHECK(near(e.pitch, -std::numbers::pi / 2, 1e-12));
	/* Straight behind: keeps the preferred side (B1's shortest rotation). */
	e = steer_errors_shortest({0.01, 0, -1}, -1);
	CHECK(e.heading < -3);
	e = steer_errors_shortest({-0.01, 0, -1}, 1);
	CHECK(e.heading > 3);
	e = steer_errors_shortest({-0.01, 0, -1}, 0);
	CHECK(e.heading < -3);
	/* Section 9.5: combined, the same side, on both axes at once. */
	e = steer_errors_local({0.01, 0, -1}, -1);
	CHECK(e.heading < -2 && std::abs(e.pitch) > 2);
	e = steer_errors_local({-0.01, 0, -1}, 1);
	CHECK(e.heading > 2 && std::abs(e.pitch) > 2);
	e = steer_errors_local({-0.01, 0, -1}, 0);
	CHECK(e.heading < -2);

	/* The controller settles a 90 degree turn without overshoot beyond a
	 * few degrees, in about the same time at any frame length.
	 */
	const turn_response ship{radians(180), 0.12};
	std::vector<double> settle_times;
	for (const double dt : {0.002, 0.004, 0.007, 1.0 / 60, 1.0 / 30, 0.05})
	{
		turn_sim s{ship};
		const double goal{std::numbers::pi / 2};
		double t{0}, max_angle{0}, settled_at{-1};
		while (t < 3)
		{
			const double axis{rotation_axis(goal - s.angle, s.rate, ship, 1.0)};
			CHECK(axis >= -1 && axis <= 1);
			s.step(axis, dt);
			t += dt;
			max_angle = std::max(max_angle, s.angle);
			if (settled_at < 0 && std::abs(goal - s.angle) < radians(1))
				settled_at = t;
		}
		CHECK(settled_at > 0);
		CHECK(max_angle - goal < radians(3));
		CHECK(std::abs(goal - s.angle) < radians(0.2));
		settle_times.push_back(settled_at);
	}
	for (const auto st : settle_times)
		CHECK(std::abs(st - settle_times.front()) < 0.1);
	/* The cap limits the axis. */
	CHECK(rotation_axis(10, 0, ship, 0.45) == 0.45);
	CHECK(rotation_axis(-10, 0, ship, 0.45) == -0.45);
	CHECK(rotation_axis(1, 0, turn_response{}, 1) == 0);

	/* Velocity command: bounded, and zero at the wanted velocity. */
	const auto c{velocity_command({50, 0, 0}, {0, 0, 0}, 60)};
	CHECK(length(c) <= 1 + 1e-12 && c.x > 0);
	const auto hold{velocity_command({30, 0, 0}, {30, 0, 0}, 60)};
	CHECK(near(hold.x, 0.5, 1e-12));
	CHECK(velocity_command({1, 0, 0}, {}, 0) == vec3{});
	CHECK(range_keeping_speed(200, 40, 90, 60) == 60);
	CHECK(range_keeping_speed(95, 40, 90, 60) == 10);
	CHECK(range_keeping_speed(60, 40, 90, 60) == 0);
	CHECK(range_keeping_speed(30, 40, 90, 60) == -20);

	/* Strafe flips within its range. */
	bot_rng rng{3};
	strafe_state st;
	int dir{st.direction()};
	unsigned run{0};
	for (unsigned i = 0; i < 10000; ++i)
	{
		const int d{st.update(rng, 24, 72)};
		if (i && d != dir)
		{
			CHECK(run >= 24 && run <= 72);
			run = 0;
		}
		dir = d;
		++run;
	}
	CHECK(should_fire(radians(3), radians(6), true, 100, 300));
	CHECK(!should_fire(radians(7), radians(6), true, 100, 300));
	CHECK(!should_fire(radians(3), radians(6), false, 100, 300));
	CHECK(!should_fire(radians(3), radians(6), true, 400, 300));
}

/* The pieces of the fight (sections 3.4 and 4.6; the flight itself is
 * test-bot-flight): shortest-rotation errors near the vertical, the
 * feed-forward, the line of sight's rate, the jukes, the combat velocity,
 * the dodge and the trigger.
 */
void test_fight_pieces()
{
	/* Near straight up the heading error stays small whichever side the
	 * direction leans (B1's heading angle swung by 180 degrees).
	 */
	for (const double x : {-1e-3, -1e-5, 0.0, 1e-5, 1e-3})
		for (const double z : {-0.01, 0.0, 0.01})
		{
			const auto e{steer_errors_local({x, 1, z})};
			CHECK(std::abs(e.heading) < 0.01);
			CHECK(near(e.pitch, -std::numbers::pi / 2 + z, 0.02));
		}
	/* The errors are the rotation: their size is the angle to go. */
	{
		const vec3 d{0.3, -0.5, 0.4};
		const auto e{steer_errors_local(d)};
		CHECK(near(std::hypot(e.pitch, e.heading), angle_between({0, 0, 1}, d), 1e-12));
		CHECK(e.pitch > 0 && e.heading > 0);
	}
	/* Feed-forward: on target and turning with it, the axis holds the
	 * turn; without it, the axis would stop the turn.
	 */
	const turn_response ship{radians(150), 0.19};
	const double w{0.8};
	CHECK(near(rotation_axis(0, w, ship, 1, w), w / ship.max_rate, 1e-12));
	CHECK(rotation_axis(0, w, ship, 1) < 0);
	CHECK(rotation_axis(0, 0, ship, 0.75, 10) == 0.75);
	/* Line of sight: a target ahead moving right turns the line to the
	 * right (about up); moving up, the nose goes up (negative pitch rate
	 * about right).
	 */
	const auto right{line_of_sight_rate({0, 0, 60}, {50, 0, 0})};
	CHECK(near(right.y, 50.0 / 60, 1e-12) && right.x == 0 && right.z == 0);
	const auto up{line_of_sight_rate({0, 0, 60}, {0, 50, 0})};
	CHECK(near(up.x, -50.0 / 60, 1e-12));
	CHECK(line_of_sight_rate({0, 0, 60}, {0, 0, -50}) == vec3{});
	CHECK(line_of_sight_rate({}, {1, 0, 0}) == vec3{});

	/* Jukes: each run a new direction at least 90 degrees from the last,
	 * a new distance in the band, and a run length in range.
	 */
	{
		bot_rng rng{11};
		juke_state j;
		j.update(rng, 36, 84, 35, 95);
		double angle{j.angle()};
		unsigned run{1}, runs{0};
		for (unsigned i = 0; i < 20000; ++i)
		{
			j.update(rng, 36, 84, 35, 95);
			CHECK(j.range() >= 35 && j.range() <= 95);
			if (j.angle() != angle)
			{
				const double turn{std::abs(std::remainder(j.angle() - angle, 2 * std::numbers::pi))};
				CHECK(turn >= radians(90) - 1e-9);
				CHECK(run >= 36 && run <= 85);
				angle = j.angle();
				run = 0;
				++runs;
			}
			++run;
		}
		CHECK(runs > 20000 / 85 && runs < 20000 / 36);
		/* A band that moves (the style's scale) starts a new run. */
		j.update(rng, 36, 84, 200, 300);
		CHECK(j.range() >= 200 && j.range() <= 300);
	}
	/* Combat velocity: closes in when far, backs off when near, the
	 * strafe across the line of sight.
	 */
	{
		bot_rng rng{2};
		juke_state j;
		j.update(rng, 36, 84, 60, 60);
		const vec3 r{1, 0, 0}, u{0, 1, 0};
		const auto far{combat_velocity({0, 0, 150}, r, u, j, 0.5, 40, 0)};
		CHECK(near(far.z, 40, 1e-9) && near(far.x, 0, 1e-9));
		const auto close{combat_velocity({0, 0, 30}, r, u, j, 0.5, 40, 0)};
		CHECK(close.z < -30);
		const auto there{combat_velocity({0, 0, 60}, r, u, j, 0.5, 40, 35)};
		CHECK(near(there.z, 0, 1e-9));
		CHECK(near(length(there), 35, 1e-9));
		const auto flat{combat_velocity({0, 0, 60}, r, u, j, 0, 40, 35)};
		CHECK(near(flat.y, 0, 1e-9));
		CHECK(combat_velocity({}, r, u, j, 0.5, 40, 35) == vec3{});
	}
	/* Dodge: a shot that will pass within the radius makes the bot move
	 * away from where it passes; one that misses, or flies away, not.
	 */
	{
		const vec3 side{1, 0, 0};
		const auto near_miss{dodge_direction({2, 0, 60}, {0, 0, -120}, 0.7, 7, side)};
		CHECK(near_miss && near_miss->x < -0.99);
		const auto straight{dodge_direction({0, 0, 60}, {0, 0, -120}, 0.7, 7, side)};
		CHECK(straight && near(dot(*straight, {0, 0, 1}), 0, 1e-9) && near(length(*straight), 1, 1e-9));
		CHECK(!dodge_direction({20, 0, 60}, {0, 0, -120}, 0.7, 7, side));
		CHECK(!dodge_direction({0, 0, 60}, {0, 0, 120}, 0.7, 7, side));
		/* Too far to arrive within the horizon. */
		CHECK(!dodge_direction({0, 0, 200}, {0, 0, -120}, 0.7, 7, side));
	}
	/* The trigger: aligned within the cone, clear and in range. */
	{
		const vec3 aim{normalized({0.05, 0, 1})};
		const double cone{radians(skill_of(bot_skill::hotshot).fire_cone_deg)};
		CHECK(should_fire(angle_between({0, 0, 1}, aim), cone, true, 60, 300));
		CHECK(!should_fire(angle_between(normalized({0.2, 0, 1}), aim), cone, true, 60, 300));
	}
}

/* A toy brain that uses every tick-driven part: the perception layer,
 * the reaction delay, the aim error, the strafe and the random numbers.
 * Its decision on each tick depends only on the tick number.
 */
struct toy_brain
{
	bot_rng rng{bot_seed(0x1234, 3, 2)};
	aim_error aim;
	strafe_state strafe;
	delay_line<vec3, 40> seen;
	std::vector<double> log;
	void tick(const uint32_t t)
	{
		const double time{t / static_cast<double>(BOT_TICK_RATE)};
		if (layer_due(t, PERCEPTION_DIVISOR, 3))
			seen.push({std::sin(time) * 100, std::cos(time * 0.7) * 50, 200});
		aim.update(rng, radians(2.8), ticks_from_ms(400));
		const int s{strafe.update(rng, 24, 72)};
		const auto p{seen.delayed(ticks_from_ms(280) / PERCEPTION_DIVISOR)};
		const double target{p ? p->x : 0};
		log.push_back(target + aim.yaw() * 1000 + s + (layer_due(t, STRATEGY_DIVISOR, 3) ? rng.uniform() : 0));
	}
};

void test_tick_schedule()
{
	using dcx::net_interp::tick_accumulator;
	constexpr int64_t second{65536};
	std::vector<std::vector<double>> logs;
	std::vector<uint32_t> tick_counts;
	for (const double frame_ms : {2.0, 3.3, 7.0, 1000.0 / 60, 1000.0 / 144, 20.0, 1000.0 / 30, 50.0, 100.0})
	{
		tick_accumulator acc{BOT_TICK_RATE};
		toy_brain b;
		const int64_t frame{static_cast<int64_t>(frame_ms * second / 1000)};
		int64_t now{1000 * second};
		acc.reset(now);
		uint32_t tick{0};
		while (now < 1010 * second)
		{
			now += frame;
			const unsigned n{acc.advance(now)};
			for (unsigned i = 0; i < n; ++i)
				b.tick(tick++);
		}
		CHECK(tick == acc.tick());
		/* 10 s at 60 Hz: 600 ticks, within one frame's worth. */
		CHECK(tick + 60 * frame_ms / 1000 + 1 >= 600 && tick <= 601 + 60 * frame_ms / 1000);
		logs.push_back(std::move(b.log));
		tick_counts.push_back(tick);
	}
	/* The same decision on every tick, whatever the frame length. */
	const auto common{*std::ranges::min_element(tick_counts)};
	for (const auto &l : logs)
		for (uint32_t i = 0; i < common; ++i)
			CHECK(l[i] == logs.front()[i]);
	/* Staggering: each layer runs once per period for each bot, and not
	 * for all bots on the same tick.
	 */
	for (unsigned k = 0; k < 8; ++k)
	{
		unsigned runs{0};
		for (uint32_t t = 0; t < 120; ++t)
			runs += layer_due(t, STRATEGY_DIVISOR, k);
		CHECK(runs == 10);
	}
	unsigned same{0};
	for (uint32_t t = 0; t < 120; ++t)
		same += layer_due(t, PERCEPTION_DIVISOR, 0) && layer_due(t, PERCEPTION_DIVISOR, 1);
	CHECK(same == 0);
	/* Seeds differ per session, slot and level. */
	CHECK(bot_seed(1, 1, 1) != bot_seed(1, 2, 1));
	CHECK(bot_seed(1, 1, 1) != bot_seed(1, 1, 2));
	CHECK(bot_seed(1, 1, 1) != bot_seed(2, 1, 1));
	CHECK(bot_seed(1, 1, 1) == bot_seed(1, 1, 1));
	CHECK(bot_seed(0, 0, 0) != 0);
}

void test_tables()
{
	/* Better skills are better in every respect, and no preset is
	 * perfect (section 5.1).
	 */
	for (unsigned i = 1; i < BOT_SKILL_COUNT; ++i)
	{
		const auto &a{skill_table[i - 1]}, &b{skill_table[i]};
		CHECK(b.reaction_ms < a.reaction_ms);
		CHECK(b.aim_sigma_deg < a.aim_sigma_deg);
		CHECK(b.aim_drift_ms <= a.aim_drift_ms);
		CHECK(b.lead > a.lead);
		CHECK(b.turn_cap > a.turn_cap);
		CHECK(b.fire_cone_deg < a.fire_cone_deg);
		CHECK(b.fov_half_deg > a.fov_half_deg);
		CHECK(b.awareness > a.awareness);
		CHECK(b.hearing > a.hearing);
		CHECK(b.memory_ms > a.memory_ms);
		CHECK(b.dodge_prob > a.dodge_prob);
		CHECK(b.weapon_smarts > a.weapon_smarts);
		CHECK(b.map_knowledge > a.map_knowledge);
	}
	for (const auto &s : skill_table)
	{
		CHECK(s.reaction_ms > 0);
		CHECK(s.aim_sigma_deg > 0);
		CHECK(s.turn_cap <= 1);
		CHECK(s.strafe_min_ms <= s.strafe_max_ms);
		CHECK(s.strafe_vertical >= 0 && s.strafe_vertical <= 1);
	}
	CHECK(&skill_of(BOT_DEFAULT_SKILL) == &skill_table[2]);
	CHECK(skill_of(bot_skill::hotshot).reaction_ms == 280);
	CHECK(&skill_of(static_cast<bot_skill>(99)) == &skill_table[2]);
	/* Retreat thresholds: aggressive < balanced < collector < cautious. */
	CHECK(style_of(bot_style::aggressive).retreat_shields < style_of(bot_style::balanced).retreat_shields);
	CHECK(style_of(bot_style::balanced).retreat_shields < style_of(bot_style::collector).retreat_shields);
	CHECK(style_of(bot_style::collector).retreat_shields < style_of(bot_style::cautious).retreat_shields);
	for (const auto n : bot_default_names)
	{
		CHECK(std::char_traits<char>::length(n) <= 8);
		CHECK(std::char_traits<char>::length(n) > 0);
	}
}

void test_primary_choice()
{
	constexpr auto bit{[](const primary p) { return static_cast<uint16_t>(1u << static_cast<unsigned>(p)); }};
	/* The laser alone. */
	CHECK(choose_primary({.owned = bit(primary::laser), .energy = 100}) == primary::laser);
	/* Helix beats plasma beats spreadfire. */
	CHECK(choose_primary({.owned = static_cast<uint16_t>(bit(primary::laser) | bit(primary::plasma) | bit(primary::helix)), .energy = 100}) == primary::helix);
	CHECK(choose_primary({.owned = static_cast<uint16_t>(bit(primary::laser) | bit(primary::plasma) | bit(primary::spreadfire)), .energy = 100}) == primary::plasma);
	/* Low energy prefers the ammunition cannons. */
	CHECK(choose_primary({.owned = static_cast<uint16_t>(bit(primary::laser) | bit(primary::plasma) | bit(primary::vulcan)), .energy = 5, .vulcan_ammo = 100}) == primary::vulcan);
	CHECK(choose_primary({.owned = static_cast<uint16_t>(bit(primary::laser) | bit(primary::gauss) | bit(primary::vulcan)), .energy = 5, .vulcan_ammo = 100}) == primary::gauss);
	/* A cannon without ammunition is not chosen. */
	CHECK(choose_primary({.owned = static_cast<uint16_t>(bit(primary::laser) | bit(primary::vulcan)), .energy = 100, .vulcan_ammo = 0}) == primary::laser);
	/* Fusion and omega are never chosen in stage B1. */
	CHECK(choose_primary({.owned = static_cast<uint16_t>(bit(primary::laser) | bit(primary::fusion) | bit(primary::omega)), .energy = 100}) == primary::laser);
	/* Every combination returns an owned weapon (or the laser, which
	 * every ship has).
	 */
	for (unsigned owned = 0; owned < 1024; ++owned)
		for (const double energy : {0.0, 5.0, 50.0})
			for (const unsigned ammo : {0u, 100u})
			{
				const auto p{choose_primary({.owned = static_cast<uint16_t>(owned | 1), .energy = energy, .vulcan_ammo = ammo})};
				CHECK(((owned | 1) >> static_cast<unsigned>(p)) & 1);
				CHECK(p != primary::fusion && p != primary::omega && p != primary::super_laser);
				if ((p == primary::vulcan || p == primary::gauss))
					CHECK(ammo > 0);
			}
}

/* Section 2.3: the slot a bot takes, and who flies a slot. */
void test_slot_allocation()
{
	std::array<slot_view, 8> v{};
	v[0].occupied = true;
	/* The lowest free slot, above the host's. */
	CHECK(choose_bot_slot(v, 8) == 1u);
	v[1].occupied = true;
	CHECK(choose_bot_slot(v, 8) == 2u);
	/* A lobby player left out of the game: its connection lingers, the
	 * slot is not free although it is disconnected.
	 */
	v[2].has_peer = true;
	CHECK(choose_bot_slot(v, 8) == 3u);
	/* A lobby player's callsign holds its slot. */
	v[3].reserved = true;
	CHECK(choose_bot_slot(v, 8) == 4u);
	/* The player limit. */
	CHECK(choose_bot_slot(v, 4) == std::nullopt);
	CHECK(choose_bot_slot(v, 5) == 4u);
	CHECK(choose_bot_slot(v, 1) == std::nullopt);
	CHECK(choose_bot_slot(v, 0) == std::nullopt);
	/* A limit above the table is the table. */
	for (auto &x : v)
		x.occupied = true;
	CHECK(choose_bot_slot(v, 16) == std::nullopt);
	/* The linger over: free again. */
	v[2] = {};
	CHECK(choose_bot_slot(v, 16) == 2u);
	/* A slot with a connection is never a bot's. */
	CHECK(slot_flown_by_bot(true, false));
	CHECK(!slot_flown_by_bot(true, true));
	CHECK(!slot_flown_by_bot(false, false));
	CHECK(!slot_flown_by_bot(false, true));
}

void test_dodge_and_bend_rules()
{
	/* Which projectiles are dodged: never the bot's own; a partner's only
	 * when friendly fire is on; anyone else's always.
	 */
	CHECK(!shot_worth_dodging(true, false, true));
	CHECK(!shot_worth_dodging(true, false, false));
	CHECK(!shot_worth_dodging(false, true, false));
	CHECK(shot_worth_dodging(false, true, true));
	CHECK(shot_worth_dodging(false, false, false));
	CHECK(shot_worth_dodging(false, false, true));
	/* One roll per projectile: the same (salt, signature) always gives the
	 * same roll, however many projectiles are judged in between; the rolls
	 * are uniform over the signatures, and another life (salt) rolls anew.
	 */
	{
		constexpr uint32_t salt{0x12345678};
		unsigned below{0}, differ{0};
		double sum{0};
		for (unsigned sig = 0; sig < 65536; ++sig)
		{
			const double r{dodge_roll(salt, static_cast<uint16_t>(sig))};
			CHECK(r >= 0 && r < 1);
			CHECK(r == dodge_roll(salt, static_cast<uint16_t>(sig)));
			sum += r;
			below += r < 0.45;
			differ += r != dodge_roll(salt + 1, static_cast<uint16_t>(sig));
		}
		CHECK(std::fabs(sum / 65536 - 0.5) < 0.01);
		CHECK(std::fabs(below / 65536.0 - 0.45) < 0.01);
		CHECK(differ > 65000);
		static_assert(dodge_roll(1, 2) == dodge_roll(1, 2));
	}
	/* The bend exception of the wall probe: the steer point nearer than
	 * the wall and the flight toward it.
	 */
	constexpr double max_angle{radians(40)};
	const vec3 pos{0, 0, 0};
	/* Flying at the steer point 20 units ahead, wall at 30: a bend. */
	CHECK(wall_hit_is_bend(pos, {0, 0, 50}, {0, 0, 20}, 30, max_angle));
	/* 30 degrees off: still a bend. */
	CHECK(wall_hit_is_bend(pos, {0, 0, 50}, {10, 0, 17.32}, 30, max_angle));
	/* The wall nearer than the steer point: an obstacle. */
	CHECK(!wall_hit_is_bend(pos, {0, 0, 50}, {0, 0, 40}, 30, max_angle));
	/* The steer point is near but to the side (or behind): the wall along
	 * the velocity is not its bend.
	 */
	CHECK(!wall_hit_is_bend(pos, {0, 0, 50}, {20, 0, 0}, 30, max_angle));
	CHECK(!wall_hit_is_bend(pos, {0, 0, 50}, {0, 0, -10}, 30, max_angle));
	CHECK(!wall_hit_is_bend(pos, {0, 0, 50}, {15, 0, 15}, 30, max_angle));
	/* Degenerate: at the steer point, or not moving. */
	CHECK(!wall_hit_is_bend(pos, {0, 0, 50}, pos, 30, max_angle));
	CHECK(!wall_hit_is_bend(pos, {0, 0, 0}, {0, 0, 20}, 30, max_angle));
}


/* Section 9.5: a hit from an attacker the bot does not see. */
void test_unseen_hit()
{
	/* Seen: nothing new (it fights it). */
	CHECK(react_to_hit({.attacker_seen = true}) == hit_reaction::none);
	/* Unseen: evade and turn to fight; weak: evade and flee. */
	CHECK(react_to_hit({.attacker_seen = false, .shields = 80}) == hit_reaction::evade_turn);
	CHECK(react_to_hit({.attacker_seen = false, .shields = 20, .retreat_shields = 35}) == hit_reaction::evade_flee);
	CHECK(react_to_hit({.attacker_seen = false, .shields = 20, .retreat_shields = 35, .invulnerable = true}) == hit_reaction::evade_turn);
	/* Already evading that attacker: no new evasion at every hit. */
	CHECK(react_to_hit({.attacker_seen = false, .shields = 80, .evading = true}) == hit_reaction::none);
	/* The evasion goes across the line of fire (a little away), the way
	 * the ship already moves, else the side given.
	 */
	const vec3 from_attacker{0, 0, 1};	// the attacker is behind (-z)
	const auto still{evade_direction(from_attacker, {}, {1, 0, 0})};
	CHECK(std::abs(length(still) - 1) < 1e-9);
	CHECK(still.x > 0.9 && still.z > 0.1 && still.z < 0.5);
	const auto moving{evade_direction(from_attacker, {0, -30, 10}, {1, 0, 0})};
	CHECK(moving.y < -0.9);
	/* Degenerate inputs still give a direction. */
	CHECK(length(evade_direction({}, {}, {0, 1, 0})) > 0.99);
	CHECK(length(evade_direction({0, 1, 0}, {}, {0, 1, 0})) > 0.99);
}

/* Section 9.5: turning far round, the bot keeps moving. */
void test_keep_moving_in_turn()
{
	const double top{58};
	const vec3 behind{0, 0, -60};
	/* Facing the target or wanting speed already: unchanged. */
	const vec3 fast{0, 40, 0};
	CHECK(keep_moving_in_turn(fast, radians(30), behind, {}, {1, 0, 0}, top) == fast);
	CHECK(keep_moving_in_turn({}, radians(30), behind, {}, {1, 0, 0}, top) == vec3{});
	const vec3 quick{top * 0.7, 0, 0};
	CHECK(keep_moving_in_turn(quick, radians(170), behind, {}, {1, 0, 0}, top) == quick);
	/* Turning round, standing still: slides across the line of sight at
	 * TURN_MOVE_SPEED, the way the hint says...
	 */
	const auto slide{keep_moving_in_turn({}, radians(170), behind, {}, {1, 0, 0}, top)};
	CHECK(std::abs(length(slide) - TURN_MOVE_SPEED * top) < 1e-6);
	CHECK(std::abs(slide.z) < 1e-9 && slide.x > 0);
	/* ... or the way it already slides (momentum). */
	const auto keep{keep_moving_in_turn({}, radians(170), behind, {0, -20, 5}, {1, 0, 0}, top)};
	CHECK(keep.y < -0.99 * TURN_MOVE_SPEED * top);
	/* A small wish (backing off) is kept, topped up across. */
	const vec3 back{0, 0, 10};
	const auto both{keep_moving_in_turn(back, radians(120), behind, {}, {1, 0, 0}, top)};
	CHECK(std::abs(length(both) - TURN_MOVE_SPEED * top) < 1e-6);
	CHECK(std::abs(both.z - 10) < 1e-9);
}

}

int main()
{
	test_intercept();
	test_aim_error();
	test_aim_lead();
	test_hit_rates_by_speed();
	test_reaction_and_memory();
	test_target_choice();
	test_steering();
	test_fight_pieces();
	test_tick_schedule();
	test_tables();
	test_primary_choice();
	test_slot_allocation();
	test_dodge_and_bend_rules();
	test_unseen_hit();
	test_keep_moving_in_turn();
	std::puts("test-bot-brain: all checks passed");
	return 0;
}
