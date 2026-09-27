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
	/* Straight behind: keeps the preferred side. */
	e = steer_errors_local({0.01, 0, -1}, -1);
	CHECK(e.heading < -3);
	e = steer_errors_local({-0.01, 0, -1}, 1);
	CHECK(e.heading > 3);
	e = steer_errors_local({-0.01, 0, -1}, 0);
	CHECK(e.heading < -3);

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

}

int main()
{
	test_intercept();
	test_aim_error();
	test_reaction_and_memory();
	test_target_choice();
	test_steering();
	test_tick_schedule();
	test_tables();
	test_primary_choice();
	test_slot_allocation();
	std::puts("test-bot-brain: all checks passed");
	return 0;
}
