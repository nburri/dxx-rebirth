/*
 * This file is part of the DXX-Rebirth project <https://github.com/dxx-rebirth/dxx-rebirth/>.
 * It is copyright by its individual contributors, as recorded in the
 * project's Git history.  See COPYING.txt at the top level for license
 * terms and a link to the Git history.
 */
/*
 * Test of the analysis of the movement recordings (movement_analysis.h)
 * and of the bot style profiles (bot_style_profile.h).
 *
 * There are no recordings of real players in the repository, so the test
 * makes its own: a small flight simulation with the ship's flight model
 * flies scripted pilots with known habits (a heavy strafer, one that
 * turns round flying backwards, a long-range sniper that breaks off at
 * 50 shields, a brawler that hunts, a dodger, ...) through a fixed
 * programme of situations (a fight, the enemy behind, the enemy out of
 * sight, a flight with no enemy and a pickup), writes what the game
 * would record, as the pilot's own machine and as a host that sees the
 * pilot as a client (no controls), and checks that the analysis finds
 * the habits again, that the controls estimated from the motion agree
 * with the exact ones, that two recordings of one game merge into one
 * (by the sync records, and by the ships' paths without them), and that
 * a profile survives its file format.
 *
 * Build and run with SCons:
 *
 *	scons sdl2=1 d1x=0 d2x=1 register_runtime_test_plain_link_targets=1 test-movement-analysis
 *	build/common/test-movement-analysis [-v]
 *
 * -v prints every measured value with the range it must be in, and the
 * reports and profiles of the pilots.  `-w DIR` runs no test: it writes
 * four of the synthetic recordings into DIR, to try movrec-analyse on.
 */

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <numbers>
#include <string>
#include <vector>

#include "movement_analysis.h"

using namespace dcx::movrec;
using namespace dcx::movrec::analysis;
namespace bot = dcx::bot;

namespace {

bool verbose;

void check_failed(const char *const expr, const char *const file, const int line)
{
	std::fprintf(stderr, "%s:%d: check failed: %s\n", file, line, expr);
	std::exit(1);
}

#define CHECK(cond)	do { if (!(cond)) check_failed(#cond, __FILE__, __LINE__); } while (0)

/* `v` within [lo, hi]; says the value when not. */
void check_range(const double v, const double lo, const double hi, const char *const expr, const char *const file, const int line)
{
	if (verbose)
		std::printf("    %-46s %9.3f   [%g, %g]\n", expr, v, lo, hi);
	if (!(v >= lo && v <= hi))
	{
		std::fprintf(stderr, "%s:%d: %s = %g, expected %g to %g\n", file, line, expr, v, lo, hi);
		std::exit(1);
	}
}

#define CHECK_RANGE(v, lo, hi)	check_range((v), (lo), (hi), #v, __FILE__, __LINE__)

/*
 * Vectors.
 */

vec3 operator+(const vec3 &a, const vec3 &b)
{
	return {{a[0] + b[0], a[1] + b[1], a[2] + b[2]}};
}

vec3 operator-(const vec3 &a, const vec3 &b)
{
	return {{a[0] - b[0], a[1] - b[1], a[2] - b[2]}};
}

vec3 operator*(const vec3 &a, const double k)
{
	return {{a[0] * k, a[1] * k, a[2] * k}};
}

vec3 unit(const vec3 &a)
{
	const double l{length(a)};
	return l > 0 ? a * (1 / l) : vec3{{0, 0, 1}};
}

vec3 cross(const vec3 &a, const vec3 &b)
{
	return {{a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]}};
}

/* A small generator of its own: the same numbers on every platform. */
struct rng
{
	std::uint32_t state;
	double next()
	{
		state ^= state << 13;
		state ^= state >> 17;
		state ^= state << 5;
		return (state >> 8) / 16777216.0;
	}
};

/*
 * The simulated ship: the flight model of physics.cpp as ship_model
 * states it, in steps of a quarter of a recording tick.
 */

constexpr unsigned RATE{30};
constexpr double TICK{1.0 / RATE};

struct ship
{
	vec3 pos{}, vel{};
	vec3 right{{1, 0, 0}}, up{{0, 1, 0}}, fwd{{0, 0, 1}};
	/* pitch (nose up +), heading (nose right +), bank; revolutions/s */
	vec3 rot{};
	void step(const std::array<double, 6> &c, const ship_model &m)
	{
		constexpr unsigned substeps{4};
		const double dt{TICK / substeps};
		const double keep{std::exp(-m.linear_rate * dt)}, rkeep{std::exp(-m.turn_rate * dt)};
		for (unsigned i{}; i != substeps; ++i)
		{
			const auto thrust{fwd * c[0] + right * c[1] + up * c[2]};
			vel = vel * keep + thrust * (m.max_speed * (1 - keep));
			for (std::size_t k{}; k != 3; ++k)
				rot[k] = rot[k] * rkeep + c[3 + k] * m.max_turn * (1 - rkeep);
			const double a{2 * std::numbers::pi * dt};
			/* heading, then pitch, then bank: small angles */
			auto f{fwd + right * (rot[1] * a) + up * (rot[0] * a)};
			auto r{right + up * (rot[2] * a)};
			fwd = unit(f);
			up = unit(cross(fwd, r));
			right = cross(up, fwd);
			pos = pos + vel * dt;
		}
	}
};

/* The pitch and heading controls that turn the nose toward `dir`. */
void steer(const ship &s, const vec3 &dir, const ship_model &m, std::array<double, 6> &c)
{
	const double x{dot(dir, s.right)}, y{dot(dir, s.up)}, z{dot(dir, s.fwd)};
	const double heading{std::atan2(x, z)};
	const double pitch{std::atan2(y, std::hypot(x, z))};
	const auto axis{[&m](const double error, const double rate) {
		const double want{std::clamp(error * 4 / (2 * std::numbers::pi), -m.max_turn, m.max_turn)};
		return std::clamp(want / m.max_turn + (want - rate) / m.max_turn, -1.0, 1.0);
	}};
	c[3] = axis(pitch, s.rot[0]);
	c[4] = axis(heading, s.rot[1]);
}

/*
 * The scripted pilots.
 */

enum class turn_habit
{
	forward,
	reverse,
	slide,
};

struct habits
{
	const char *name;
	/* The distance it fights at. */
	double fight_distance{70};
	/* Strafe: thrust (0: none), the length of a run, the vertical share. */
	double strafe{};
	unsigned strafe_run_ms{600};
	double strafe_vertical{};
	/* The runs vary by this share of strafe_run_ms either way (0: the
	 * runs follow the clock exactly).
	 */
	double strafe_jitter{};
	turn_habit turn{turn_habit::forward};
	/* The push forward after turning round. */
	bool boost{};
	/* Afterburner: chasing a target further than `burn_distance`, fleeing,
	 * with no enemy in sight.
	 */
	bool burn_chase{};
	double burn_distance{150};
	bool burn_flee{};
	bool burn_cross{};
	/* Flies away below these shields (0: never). */
	double retreat_shields{};
	/* The share of the bursts it sidesteps, and how late. */
	double dodge{};
	unsigned reaction_ms{250};
	/* The sidestep turns against the strafe of the moment (else a random
	 * side).
	 */
	bool dodge_reverses{};
	/* Primaries in the close, mid and distant band. */
	std::array<std::uint8_t, 3> primary{{0, 0, 0}};
	/* Missiles per volley (0: none) and seconds between volleys. */
	unsigned volley{};
	double volley_gap_s{5};
	/* How long it follows an enemy out of sight (seconds). */
	double pursue_s{};
	/* Leaves its course for a pickup. */
	bool detour{};
};

/* The programme: a cycle of 40 s, again and again.
 *
 *	 0-14 s  fight: the enemy appears ahead and fires three bursts
 *	14-20 s  the enemy is behind
 *	20-30 s  the enemy is out of sight and moves off
 *	30-40 s  no enemy; a pickup at 35 s
 *
 * The shields at the start of a cycle go through 100, 80, 60, 45, 30, 15.
 */
constexpr unsigned CYCLE_TICKS{40 * RATE};
constexpr std::array<double, 6> CYCLE_SHIELDS{{100, 80, 60, 45, 30, 15}};
constexpr std::array<double, 3> BURST_AT{{3.0, 6.5, 10.0}};

struct moment
{
	ship pilot;
	std::array<double, 6> ctl{};
	bool burner{};
	double shields{};
	/* The enemy: none in the last part of a cycle. */
	bool enemy{};
	vec3 enemy_pos{}, enemy_vel{};
	bool sight{};
	std::uint32_t hit_until_ms{};
};

struct happening
{
	std::uint32_t ms;
	event_record e;
};

struct flight
{
	habits h;
	std::vector<moment> moments;
	std::vector<happening> events;
	unsigned primary_shots{}, missiles{}, bursts{}, dodges{}, pickups{};
};

std::uint32_t ms_of_tick(const std::uint32_t tick)
{
	return static_cast<std::uint32_t>(std::uint64_t{tick} * 1000 / RATE);
}

flight fly(const habits &h, const unsigned cycles, const std::uint32_t seed = 12345)
{
	const auto m{pyro_gx()};
	flight out;
	out.h = h;
	rng random{seed};
	ship s;
	vec3 enemy_pos{}, enemy_vel{};
	double shields{100};
	std::uint32_t hit_until{};
	bool turned{};
	std::uint32_t boost_until{};
	std::uint32_t dodge_from{~0u}, dodge_until{};
	double dodge_dir{1};
	/* The strafe's direction, and (jittered runs) when it turns next. */
	double strafe_sign{1};
	std::uint32_t strafe_flip{};
	std::uint32_t next_shot{}, next_volley{}, volley_left{}, next_missile{};
	const auto add_event{[&out](const std::uint32_t ms, const record_type type, const unsigned pid, const unsigned other, const unsigned kind, const unsigned id, const unsigned value, const unsigned flags) {
		out.events.push_back({ms, {type, ms, static_cast<std::uint8_t>(pid), static_cast<std::uint8_t>(other), static_cast<std::uint8_t>(kind), static_cast<std::uint8_t>(id), static_cast<std::uint16_t>(value), static_cast<std::uint8_t>(flags)}});
	}};
	for (std::uint32_t tick{}; tick != cycles * CYCLE_TICKS; ++tick)
	{
		const std::uint32_t now{ms_of_tick(tick)};
		const unsigned cycle{tick / CYCLE_TICKS};
		const unsigned in_cycle{tick % CYCLE_TICKS};
		const double t{in_cycle * TICK};
		const std::uint32_t cycle_ms{now - ms_of_tick(cycle * CYCLE_TICKS)};
		std::array<double, 6> c{};
		bool burner{}, enemy{true}, sight{true};
		if (in_cycle == 0)
		{
			/* Far from the origin: back to it (the positions are 24 bit). */
			if (length(s.pos) > 3000)
				s.pos = {};
			enemy_pos = s.pos + s.fwd * std::min(h.fight_distance + 130, 390.0);
			enemy_vel = {};
			shields = CYCLE_SHIELDS[cycle % CYCLE_SHIELDS.size()];
			turned = false;
			next_shot = now;
			next_volley = now + 2000;
		}
		const bool retreating{h.retreat_shields > shields};
		/* 14 s: the enemy is behind.  20 s: it moves off, out of sight. */
		if (in_cycle == 14 * RATE && !retreating)
			enemy_pos = s.pos - s.fwd * 120;
		if (in_cycle == 20 * RATE)
			enemy_vel = s.right * 55;
		enemy_pos = enemy_pos + enemy_vel * TICK;
		const auto to_enemy{enemy_pos - s.pos};
		const double dist{length(to_enemy)};
		const double off_nose{std::acos(std::clamp(dot(unit(to_enemy), s.fwd), -1.0, 1.0))};
		/* The fight: keep the distance, strafe, sidestep, fire. */
		const auto fight{[&] {
			steer(s, to_enemy, m, c);
			const double error{dist - h.fight_distance};
			c[0] = std::clamp(error / 25, -1.0, 1.0);
			if (h.burn_chase && dist > h.burn_distance && error > 0)
			{
				c[0] = 2;
				burner = true;
			}
			if (h.strafe > 0 && error < 60)
			{
				if (h.strafe_jitter > 0)
				{
					if (now >= strafe_flip)
					{
						strafe_sign = -strafe_sign;
						strafe_flip = now + static_cast<std::uint32_t>(h.strafe_run_ms * (1 + h.strafe_jitter * (2 * random.next() - 1)));
					}
				}
				else
					strafe_sign = (cycle_ms / h.strafe_run_ms) % 2 ? -1.0 : 1.0;
				const double sign{strafe_sign};
				c[1] = sign * h.strafe;
				c[2] = sign * h.strafe * h.strafe_vertical;
			}
			if (now >= dodge_from && now < dodge_until)
			{
				c[1] = dodge_dir;
				c[2] = 0;
			}
			if (off_nose < 0.17 && now >= next_shot)
			{
				next_shot = now + 250;
				add_event(now, record_type::fire, 0, PLAYER_NONE, fire_kind::primary, h.primary[static_cast<std::size_t>(bot::band_of(dist))], 0, 0);
				++out.primary_shots;
			}
			if (h.volley && off_nose < 0.17)
			{
				if (!volley_left && now >= next_volley)
				{
					volley_left = h.volley;
					next_volley = now + static_cast<std::uint32_t>(h.volley_gap_s * 1000);
					next_missile = now;
				}
				if (volley_left && now >= next_missile)
				{
					--volley_left;
					next_missile = now + 200;
					add_event(now, record_type::fire, 0, PLAYER_NONE, fire_kind::secondary, 0, 0, 0);
					++out.missiles;
				}
			}
		}};
		const auto flee{[&] {
			steer(s, to_enemy * -1.0, m, c);
			c[0] = h.burn_flee ? 2 : 1;
			burner = h.burn_flee;
		}};
		if (t < 14)
		{
			if (retreating)
				flee();
			else
				fight();
			for (const double at : BURST_AT)
			{
				const auto burst_tick{static_cast<unsigned>(at * RATE + 0.5)};
				/* Four shots of the enemy, 200 ms apart. */
				for (unsigned k{}; k != 4; ++k)
					if (in_cycle == burst_tick + k * (RATE / 5))
						add_event(now, record_type::fire, 1, PLAYER_NONE, fire_kind::primary, 0, 0, 0);
				if (in_cycle == burst_tick)
				{
					++out.bursts;
					if (!retreating && random.next() < h.dodge)
					{
						++out.dodges;
						dodge_from = now + h.reaction_ms;
						dodge_until = dodge_from + 600;
						const double side{random.next() < 0.5 ? -1.0 : 1.0};
						dodge_dir = h.dodge_reverses ? -strafe_sign : side;
					}
					else
					{
						/* It is hit. */
						add_event(now + 300, record_type::hit, 0, 1, attacker_kind::player, 0, 2 * 256, hit_flag::applied_here);
						shields -= 2;
						hit_until = now + 2300;
					}
				}
			}
		}
		else if (t < 20)
		{
			if (retreating)
				flee();
			else if (!turned && off_nose > 0.45)
			{
				/* Turning round. */
				steer(s, enemy_pos - s.pos, m, c);
				switch (h.turn)
				{
					case turn_habit::forward:
						c[0] = 1;
						break;
					case turn_habit::reverse:
						c[0] = -1;
						break;
					case turn_habit::slide:
						c[1] = 1;
						break;
				}
			}
			else
			{
				if (!turned)
				{
					turned = true;
					boost_until = h.boost ? now + 800 : now;
				}
				fight();
				if (now < boost_until)
				{
					c[0] = h.burn_chase ? 2 : 1;
					burner = h.burn_chase;
					c[1] = c[2] = 0;
				}
			}
		}
		else if (t < 30)
		{
			sight = false;
			if (t - 20 < h.pursue_s)
			{
				steer(s, to_enemy, m, c);
				c[0] = 1;
				if (h.burn_chase && dist > h.burn_distance)
				{
					c[0] = 2;
					burner = true;
				}
			}
		}
		else
		{
			enemy = sight = false;
			c[0] = h.burn_cross ? 2 : 1;
			burner = h.burn_cross;
			if (h.detour && t >= 33.5 && t < 35)
			{
				c[0] = 0;
				c[1] = 1;
				burner = false;
			}
			if (in_cycle == 35 * RATE)
			{
				add_event(now, record_type::pickup, 0, PLAYER_NONE, 0, 3, 0, 0);
				++out.pickups;
			}
		}
		s.step(c, m);
		moment mo;
		mo.pilot = s;
		mo.ctl = c;
		mo.burner = burner;
		mo.shields = shields;
		mo.enemy = enemy;
		mo.enemy_pos = enemy_pos;
		mo.enemy_vel = enemy_vel;
		mo.sight = sight;
		mo.hit_until_ms = hit_until;
		out.moments.push_back(mo);
	}
	std::stable_sort(out.events.begin(), out.events.end(), [](const happening &a, const happening &b) { return a.ms < b.ms; });
	return out;
}

/*
 * What a machine records of a flight.
 */

struct view
{
	/* The player flown on the recording machine (exact controls): 0 the
	 * pilot, 1 the enemy.
	 */
	unsigned local_pid{};
	bool host{true};
	/* The part of the flight it records. */
	std::uint32_t first_tick{};
	std::uint32_t last_tick{~0u};
	/* The session's clock: sync records, or none (format minor 0). */
	bool sync{true};
	std::uint32_t session_id{0x1234abcdu};
	std::int64_t host_epoch_ms{7'000'000};
	std::int64_t start_unix{1'790'000'000};
	/* It sees the other ship this many ticks late. */
	unsigned remote_lag{};
	/* The client's afterburner bit reaches the host. */
	bool remote_afterburner{true};
	/* The pilot, flown elsewhere, shares its controls (-sharemoves). */
	bool shared_controls{};
	const char *pilot_name{"Pilot"};
	const char *enemy_name{"Target"};
	const char *mission{"Test Mission"};
};

std::array<std::int32_t, 3> q_pos(const vec3 &v)
{
	return {{static_cast<std::int32_t>(std::lround(v[0] * 256)), static_cast<std::int32_t>(std::lround(v[1] * 256)), static_cast<std::int32_t>(std::lround(v[2] * 256))}};
}

std::array<std::int16_t, 3> q16(const vec3 &v, const double scale)
{
	std::array<std::int16_t, 3> r{};
	for (std::size_t i{}; i != 3; ++i)
		r[i] = static_cast<std::int16_t>(std::clamp(std::lround(v[i] * scale), -32767L, 32767L));
	return r;
}

std::vector<std::uint8_t> record(const flight &f, const view &v)
{
	std::vector<std::uint8_t> bytes;
	file_header h;
	h.tick_rate = RATE;
	h.flags = static_cast<std::uint16_t>(header_flag::multiplayer) | (v.host ? static_cast<std::uint16_t>(header_flag::host) : std::uint16_t{0});
	h.start_unix_time = v.start_unix;
	h.local_pid = static_cast<std::uint8_t>(v.local_pid);
	h.program = "test";
	h.mission = v.mission;
	h.level_name = "Arena";
	h.level_num = 1;
	h.num_players = 2;
	const std::uint8_t recorded{player_flag::connected | player_flag::recorded};
	h.players[0] = {0, static_cast<std::uint8_t>(recorded | (v.local_pid == 0 ? player_flag::local : v.shared_controls ? player_flag::shares_controls : 0)), 0xff, v.pilot_name};
	h.players[1] = {1, static_cast<std::uint8_t>(recorded | (v.local_pid == 1 ? player_flag::local : 0)), 0xff, v.enemy_name};
	if (!v.sync)
		h.minor = 0;
	std::array<std::uint8_t, MAX_HEADER_SIZE> hb;
	const auto n{encode_header(hb, h)};
	CHECK(n);
	bytes.assign(hb.begin(), hb.begin() + static_cast<std::ptrdiff_t>(n));
	const auto chunk{std::make_unique<chunk_builder>()};
	const auto flush{[&] {
		if (chunk->empty())
			return;
		const auto c{chunk->finish()};
		bytes.insert(bytes.end(), c.begin(), c.end());
		chunk->reset();
	}};
	record_buffer buf;
	const auto put{[&](const std::span<const std::uint8_t> r) {
		CHECK(!r.empty());
		if (!chunk->fits(r.size()))
			flush();
		CHECK(chunk->append(r));
	}};
	put(encode_level(buf, 1, 100, 0x21, v.mission, "Arena"));
	for (const auto &p : std::span(h.players).first(2))
		put(encode_player(buf, p.pid, p.flags, p.team, p.callsign));
	const std::uint32_t first{v.first_tick}, last{std::min<std::uint32_t>(v.last_tick, static_cast<std::uint32_t>(f.moments.size()) - 1)};
	const std::uint32_t origin{ms_of_tick(first)};
	std::size_t next_event{};
	while (next_event != f.events.size() && f.events[next_event].ms < origin)
		++next_event;
	for (std::uint32_t tick{first}; tick <= last; ++tick)
	{
		const std::uint32_t now{ms_of_tick(tick)};
		const std::uint32_t file_ms{now - origin};
		put(encode(buf, tick_record{tick - first, file_ms}));
		if (v.sync && (tick - first) % RATE == 0)
			put(encode(buf, sync_record{file_ms, v.session_id, v.host_epoch_ms + now, static_cast<std::uint8_t>(sync_flag::clock_valid | (v.host ? sync_flag::host : 0))}));
		/* Each ship as this machine sees it: the remote one late. */
		const auto seen{[&](const unsigned pid) -> const moment & {
			const std::uint32_t lag{pid == v.local_pid ? 0 : v.remote_lag};
			return f.moments[tick >= lag ? tick - lag : 0];
		}};
		const auto &mp{seen(0)};
		const auto &me{seen(1)};
		/* The enemy is a plain target: it hangs where the script puts
		 * it (far away when there is none) and faces the pilot.
		 */
		const vec3 enemy_pos{me.enemy ? me.enemy_pos : vec3{{20000, 0, 0}}};
		const auto enemy_fwd{unit(mp.pilot.pos - enemy_pos)};
		const auto enemy_right{unit(cross(vec3{{0, 1, 0}}, enemy_fwd))};
		const auto enemy_up{cross(enemy_fwd, enemy_right)};
		const auto rel{enemy_pos - mp.pilot.pos};
		const double dist{length(rel)};
		const bool enemy{mp.enemy && me.enemy};
		{
			sample s;
			s.pid = 0;
			s.flags = sample_flag::alive;
			s.flags2 = 0;
			if (v.local_pid == 0)
			{
				s.flags |= sample_flag::controls;
				s.flags2 |= sample_flag2::local | sample_flag2::vitals_exact | sample_flag2::afterburner_known | sample_flag2::buttons_known;
				for (std::size_t i{}; i != 6; ++i)
					s.controls[i] = quantise_control(mp.ctl[i]);
			}
			else if (v.shared_controls)
			{
				/* What the host records of a client with -sharemoves. */
				control_array c{};
				for (std::size_t i{}; i != 6; ++i)
					c[i] = quantise_control(mp.ctl[i]);
				set_shared_controls(s, c);
			}
			else if (v.remote_afterburner)
				s.flags2 |= sample_flag2::afterburner_known;
			if (mp.burner && (s.flags2 & sample_flag2::afterburner_known) && !(s.flags2 & sample_flag2::controls_shared))
				s.flags |= sample_flag::afterburner;
			s.segment = 1;
			s.pos = q_pos(mp.pilot.pos);
			s.quat = quantise_quaternion(quaternion_from_axes(mp.pilot.right, mp.pilot.up, mp.pilot.fwd));
			s.vel = q16(mp.pilot.vel, 64);
			s.rotvel = q16(mp.pilot.rot, 4096);
			s.weapons = 0x00;
			s.shields = static_cast<std::uint8_t>(std::clamp(mp.shields, 0.0, 255.0));
			s.energy = 100;
			if (now < mp.hit_until_ms)
				s.attacked_mask = 2;
			if (enemy)
			{
				s.context = context_flag::kind_player;
				if (mp.sight)
					s.context |= context_flag::line_of_sight | context_flag::me_in_its_cone;
				if (dot(rel, mp.pilot.fwd) > 0.866 * dist)
					s.context |= context_flag::in_my_cone;
				s.enemy_id = 1;
				s.enemy_rel_pos = q16(rel, 16);
				s.enemy_rel_vel = q16(me.enemy_vel - mp.pilot.vel, 64);
				if (mp.sight && dist < 800)
					s.aimed_at_mask = 2;
			}
			put(encode(buf, s));
		}
		{
			sample s;
			s.pid = 1;
			s.flags = sample_flag::alive;
			if (v.local_pid == 1)
			{
				s.flags |= sample_flag::controls;
				s.flags2 |= sample_flag2::local | sample_flag2::vitals_exact | sample_flag2::afterburner_known;
				s.controls = {{0, 30, 0, 0, 0, 0}};
			}
			s.segment = 2;
			s.pos = q_pos(enemy_pos);
			s.quat = quantise_quaternion(quaternion_from_axes(enemy_right, enemy_up, enemy_fwd));
			s.vel = q16(me.enemy_vel, 64);
			s.shields = 100;
			s.energy = 100;
			if (enemy)
			{
				s.context = context_flag::kind_player | context_flag::in_my_cone;
				if (mp.sight)
					s.context |= context_flag::line_of_sight;
				s.enemy_id = 0;
				s.enemy_rel_pos = q16(rel * -1.0, 16);
				s.enemy_rel_vel = q16(mp.pilot.vel - me.enemy_vel, 64);
			}
			put(encode(buf, s));
		}
		for (; next_event != f.events.size() && f.events[next_event].ms <= now; ++next_event)
		{
			auto e{f.events[next_event].e};
			e.time_ms -= origin;
			put(encode(buf, e));
		}
	}
	put(encode(buf, event_record{record_type::end, ms_of_tick(last) - origin, PLAYER_NONE, PLAYER_NONE, end_reason::closed, 0, 0, 0}));
	flush();
	return bytes;
}

recording load(const std::vector<std::uint8_t> &bytes, const char *const name)
{
	auto r{load_recording(bytes, name)};
	CHECK(r);
	return std::move(*r);
}

/* The analysis of one flight recorded by `v`: the pilot's result. */
player_result analyse_flight(const flight &f, const view &v = {})
{
	const std::array<recording, 1> files{{load(record(f, v), f.h.name)}};
	const auto result{analyse_recordings(files)};
	CHECK(result.sessions.size() == 1);
	for (const auto &p : result.players)
		if (p.stats.callsign == v.pilot_name)
		{
			if (verbose)
				std::printf("\n--- %s (%s) ---\n%s\n%s", f.h.name, v.local_pid == 0 ? "own recording" : "recorded by another machine", write_report(p.stats, p.profile).c_str(), bot::write_style_profile(p.profile).c_str());
			return p;
		}
	CHECK(!"the pilot is in the result");
	return {};
}

double value(const bot::style_profile &p, const char *const key)
{
	const auto v{p.value(key)};
	if (!v)
	{
		std::fprintf(stderr, "profile has no %s\n", key);
		std::exit(1);
	}
	return *v;
}

/*
 * The pilots.
 */

constexpr unsigned CYCLES{18};	/* 12 minutes */

habits strafer()
{
	habits h{};
	h.name = "strafer";
	h.fight_distance = 70;
	h.strafe = 1;
	h.strafe_run_ms = 500;
	return h;
}

habits bobber()
{
	habits h{};
	h.name = "bobber";
	h.fight_distance = 80;
	h.strafe = 0.8;
	h.strafe_run_ms = 1200;
	h.strafe_vertical = 1;
	h.turn = turn_habit::slide;
	return h;
}

habits reverse_turner()
{
	habits h{};
	h.name = "reverse turner";
	h.fight_distance = 60;
	h.turn = turn_habit::reverse;
	h.boost = true;
	h.burn_chase = true;
	h.burn_distance = 150;
	h.burn_cross = true;
	return h;
}

habits sniper()
{
	habits h{};
	h.name = "sniper";
	h.fight_distance = 250;
	h.retreat_shields = 50;
	h.burn_flee = true;
	h.primary = {{0, 3, 6}};	/* laser, plasma, gauss */
	h.volley = 1;
	h.volley_gap_s = 6;
	return h;
}

habits brawler()
{
	habits h{};
	h.name = "brawler";
	h.fight_distance = 40;
	h.primary = {{2, 3, 0}};	/* spreadfire, plasma, laser */
	h.volley = 3;
	h.volley_gap_s = 4;
	h.pursue_s = 6;
	h.detour = true;
	return h;
}

habits dodger()
{
	habits h{};
	h.name = "dodger";
	h.fight_distance = 90;
	h.dodge = 0.8;
	h.reaction_ms = 250;
	return h;
}

/* Weaves all the time (runs of 225 to 675 ms) and answers 70 % of the
 * bursts by turning its strafe round, 250 ms late: the dodge hides in
 * the weave.
 */
habits weaving_dodger()
{
	habits h{};
	h.name = "weaving dodger";
	h.fight_distance = 80;
	h.strafe = 1;
	h.strafe_run_ms = 450;
	h.strafe_jitter = 0.5;
	h.dodge = 0.7;
	h.dodge_reverses = true;
	h.reaction_ms = 250;
	return h;
}

/* The same weave without a dodge. */
habits weaver()
{
	auto h{weaving_dodger()};
	h.name = "weaver";
	h.dodge = 0;
	return h;
}

const ship_model M{pyro_gx()};

void test_ship_model()
{
	/* The Pyro-GX: bot_flight.cpp's limits from the same numbers. */
	CHECK_RANGE(M.max_speed, 57.5, 59.5);
	CHECK_RANGE(M.max_turn, 0.39, 0.43);
	CHECK_RANGE(M.linear_rate, 2.0, 2.2);
	CHECK_RANGE(M.turn_rate, 5.2, 5.5);
	/* The simulated ship reaches them. */
	ship s;
	for (unsigned i{}; i != 5 * RATE; ++i)
		s.step({{1, 0, 0, 0, 0, 0}}, M);
	CHECK_RANGE(length(s.vel), M.max_speed * 0.99, M.max_speed * 1.01);
	for (unsigned i{}; i != 2 * RATE; ++i)
		s.step({{0, 0, 0, 0, 1, 0}}, M);
	CHECK_RANGE(s.rot[1], M.max_turn * 0.99, M.max_turn * 1.01);
}

void test_summary()
{
	const auto s{summarise({5, 1, 3, 2, 4})};
	CHECK(s.n == 5 && s.mean == 3 && s.p50 == 3);
	CHECK_RANGE(s.p25, 2, 2);
	CHECK_RANGE(s.p90, 4.6, 4.6);
	CHECK(summarise({}).n == 0);
}

/* The heavy strafer against a pilot that does not strafe. */
void test_strafer()
{
	const auto r{analyse_flight(fly(strafer(), CYCLES))};
	const auto &s{r.stats};
	CHECK(s.estimated_s == 0 && s.exact_s > 600);
	CHECK_RANGE(s.fight_strafe_share, 0.7, 1.0);
	CHECK_RANGE(s.strafe_run_ms.p50, 430, 570);
	CHECK_RANGE(s.strafe_reversals_per_min, 80, 125);
	CHECK_RANGE(s.strafe_vertical, 0, 0.1);
	CHECK(value(r.profile, "skill.strafe") == 1);
	CHECK_RANGE(value(r.profile, "skill.strafe_min_ms"), 400, 570);
	CHECK_RANGE(value(r.profile, "skill.strafe_max_ms"), 430, 700);
	CHECK_RANGE(value(r.profile, "skill.strafe_vertical"), 0, 0.1);
	CHECK(r.profile.find("skill.strafe")->confidence == bot::style_confidence::high);
	const auto traits{describe_traits(s)};
	CHECK(!traits.empty() && traits.front().starts_with("Heavy strafer"));

	const auto other{analyse_flight(fly(reverse_turner(), CYCLES))};
	CHECK_RANGE(other.stats.fight_strafe_share, 0, 0.1);
	CHECK(value(other.profile, "skill.strafe") == 0);
	CHECK(!other.profile.find("skill.strafe_min_ms"));

	/* Up and down as much as left and right, in long runs. */
	const auto bob{analyse_flight(fly(bobber(), CYCLES))};
	CHECK_RANGE(bob.stats.strafe_vertical, 0.8, 1.0);
	CHECK_RANGE(bob.stats.strafe_run_ms.p50, 1050, 1300);
	CHECK_RANGE(value(bob.profile, "skill.strafe_vertical"), 0.8, 1.0);
	CHECK(value(bob.profile, "skill.strafe_min_ms") > value(r.profile, "skill.strafe_max_ms"));
}

/* Turning round: backwards, pushing forward, sliding. */
void test_turns()
{
	const auto back{analyse_flight(fly(reverse_turner(), CYCLES))};
	const auto &s{back.stats};
	/* One turn per cycle. */
	CHECK_RANGE(s.large_turns, CYCLES - 2, CYCLES + 2);
	CHECK_RANGE(s.reverse_turn_share, 0.85, 1.0);
	CHECK_RANGE(s.turn_180_ms.p50, 900, 1900);
	CHECK_RANGE(s.reverse_turn_speed, 0.3, 1.0);
	CHECK_RANGE(s.turn_boost_share, 0.8, 1.0);
	CHECK_RANGE(s.turn_boost_burn_share, 0.8, 1.0);
	CHECK_RANGE(value(back.profile, "tune.reverse_turn"), 0.85, 1.0);
	CHECK_RANGE(value(back.profile, "tune.turn_boost"), 0.8, 1.0);

	const auto fwd{analyse_flight(fly(strafer(), CYCLES))};
	CHECK_RANGE(fwd.stats.large_turns, CYCLES - 2, CYCLES + 2);
	CHECK_RANGE(fwd.stats.reverse_turn_share, 0, 0.1);
	CHECK_RANGE(fwd.stats.forward_turn_share, 0.85, 1.0);
	CHECK_RANGE(value(fwd.profile, "tune.reverse_turn"), 0, 0.1);

	const auto slide{analyse_flight(fly(bobber(), CYCLES))};
	CHECK_RANGE(slide.stats.slide_turn_share, 0.85, 1.0);
	CHECK_RANGE(slide.stats.reverse_turn_share, 0, 0.1);
}

/* The sniper: far away, gauss at range, off below 50 shields. */
void test_sniper()
{
	const auto r{analyse_flight(fly(sniper(), CYCLES))};
	const auto &s{r.stats};
	CHECK_RANGE(s.fire_distance.p50, 230, 275);
	CHECK_RANGE(value(r.profile, "style.range_scale"), 2.5, 3.0);
	CHECK_RANGE(value(r.profile, "tune.range_lo"), 200, 260);
	CHECK_RANGE(value(r.profile, "tune.range_hi"), 240, 330);
	/* Gauss (6) beyond 150 units. */
	const auto &distant{s.primary_by_band[static_cast<std::size_t>(bot::range_band::distant)]};
	CHECK(distant[6] > 100 && distant[0] == 0 && distant[3] == 0);
	/* Breaks off at 50: the cycles that start with 45, 30 and 15. */
	CHECK_RANGE(s.retreat_contrast, 0.35, 1.0);
	CHECK_RANGE(s.retreat_shields, 50, 50);
	CHECK_RANGE(value(r.profile, "style.retreat_shields"), 50, 50);
	/* Flees with the afterburner, and with nothing else. */
	CHECK_RANGE(s.ab_situation_rate[1], 0.9, 1.0);
	CHECK_RANGE(s.ab_situation_rate[0], 0, 0.05);
	CHECK_RANGE(value(r.profile, "tune.burn_retreat"), 0.9, 1.0);
	CHECK(value(r.profile, "style.burn_chase_distance") == 1000);
	/* Single missiles, 6 s apart. */
	CHECK_RANGE(s.volley_size, 1, 1.05);
	CHECK_RANGE(s.volley_gap_s.p50, 5.8, 8.5);
	const auto traits{describe_traits(s)};
	CHECK(std::any_of(traits.begin(), traits.end(), [](const std::string &t) { return t.starts_with("Long-range fighter"); }));
}

/* The brawler: close, never breaks off, hunts, volleys, detours. */
void test_brawler()
{
	const auto f{fly(brawler(), CYCLES)};
	const auto r{analyse_flight(f)};
	const auto &s{r.stats};
	CHECK_RANGE(s.fire_distance.p50, 35, 50);
	CHECK_RANGE(value(r.profile, "style.range_scale"), 0.5, 0.8);
	const auto &close_band{s.primary_by_band[static_cast<std::size_t>(bot::range_band::close)]};
	CHECK(close_band[2] > 100 && close_band[0] == 0);
	CHECK(s.primary_shots == f.primary_shots);
	CHECK(s.secondary_shots == f.missiles);
	CHECK_RANGE(s.retreat_share, 0, 0.05);
	CHECK(value(r.profile, "style.retreat_shields") == 10);
	/* Follows the enemy out of sight for 6 s, every time. */
	CHECK_RANGE(s.sight_losses, CYCLES - 1, CYCLES);
	CHECK_RANGE(s.pursue_share, 0.9, 1.0);
	CHECK_RANGE(s.pursuit_s.p50, 5.3, 6.7);
	CHECK_RANGE(value(r.profile, "tune.pursuit_seconds"), 5.3, 6.7);
	CHECK_RANGE(value(r.profile, "style.chase_memory"), 1.1, 1.6);
	/* Volleys of three, 4 s apart. */
	CHECK_RANGE(s.volley_size, 2.8, 3.0);
	CHECK(s.volley_max == 3);
	CHECK_RANGE(s.volley_gap_s.p50, 3.8, 4.3);
	CHECK_RANGE(value(r.profile, "tune.volley_size"), 2.8, 3.0);
	/* Leaves its course for the pickup. */
	CHECK(s.pickups == f.pickups);
	CHECK_RANGE(s.pickup_detour_share, 0.9, 1.0);
	CHECK_RANGE(value(r.profile, "tune.grab_detour"), 0.9, 1.0);
	/* Took the hits of every burst. */
	CHECK(s.hits_taken == f.bursts);

	/* The sniper lets the enemy go and flies past no pickup. */
	const auto sn{analyse_flight(fly(sniper(), CYCLES))};
	CHECK_RANGE(sn.stats.pursue_share, 0, 0.1);
	CHECK(value(sn.profile, "tune.pursuit_seconds") == 0);
	CHECK_RANGE(sn.stats.pickup_detour_share, 0, 0.1);
	CHECK(value(r.profile, "style.engage_weight") > value(sn.profile, "style.engage_weight") + 0.15);
	CHECK(value(r.profile, "style.collect_weight") > value(sn.profile, "style.collect_weight") + 0.5);
	CHECK(value(r.profile, "style.chase_memory") > value(sn.profile, "style.chase_memory") + 0.5);
	/* The nearest built-in styles. */
	CHECK(r.profile.base_style == bot::bot_style::aggressive);
	CHECK(sn.profile.base_style == bot::bot_style::cautious);
}

/* The dodger sidesteps 80 % of the bursts, 250 ms late; pilots that
 * weave all the time (on the clock, or in runs of varying length) and
 * never dodge must not look like dodgers; one that weaves and dodges by
 * turning its strafe round must.
 */
void test_dodger()
{
	const auto f{fly(dodger(), CYCLES)};
	const auto r{analyse_flight(f)};
	const auto &s{r.stats};
	const double truth{static_cast<double>(f.dodges) / f.bursts};
	CHECK_RANGE(truth, 0.65, 0.95);
	CHECK_RANGE(s.dodge_triggers, f.bursts - 3, f.bursts);
	CHECK(s.dodge_measurable);
	CHECK_RANGE(s.dodge_rate, truth - 0.08, truth + 0.08);
	CHECK_RANGE(s.dodge_baseline, 0, 0.1);
	CHECK_RANGE(s.dodge_prob, truth - 0.1, truth + 0.08);
	CHECK_RANGE(s.dodge_se, 0, 0.1);
	CHECK_RANGE(s.dodge_reaction_ms, 240, 330);
	CHECK_RANGE(value(r.profile, "skill.dodge_prob"), truth - 0.1, truth + 0.08);
	CHECK(r.profile.find("skill.dodge_prob")->confidence != bot::style_confidence::low);
	CHECK_RANGE(value(r.profile, "measured.dodge_reaction_ms"), 240, 330);
	/* Its sidesteps cost the shooter its hits. */
	CHECK(s.dodge_switch_n > 30 && s.dodge_none_n >= 5);
	CHECK_RANGE(s.dodge_hit_after_switch, 0, 0.1);
	CHECK_RANGE(s.dodge_hit_after_none, 0.9, 1.0);
	const auto traits{describe_traits(s)};
	CHECK(std::any_of(traits.begin(), traits.end(), [](const std::string &t) { return t.starts_with("Dodges incoming fire"); }));

	/* Recorded by the host (the switches from the estimated thrust):
	 * the same answer, no more than medium sure.
	 */
	{
		view remote;
		remote.local_pid = 1;
		const auto e{analyse_flight(f, remote)};
		if (verbose)
			std::printf("dodger, estimated: prob %.2f se %.2f reaction %.0f\n", e.stats.dodge_prob, e.stats.dodge_se, e.stats.dodge_reaction_ms);
		CHECK(e.stats.dodge_measurable);
		CHECK_RANGE(e.stats.dodge_prob, truth - 0.15, truth + 0.08);
		CHECK_RANGE(e.stats.dodge_reaction_ms, 220, 380);
		CHECK(e.profile.find("skill.dodge_prob")->confidence == bot::style_confidence::medium);
	}

	const auto still{analyse_flight(fly(brawler(), CYCLES))};
	CHECK_RANGE(still.stats.dodge_triggers, f.bursts - 3, f.bursts);
	CHECK(still.stats.dodge_measurable);
	CHECK_RANGE(still.stats.dodge_prob, 0, 0.05);
	CHECK_RANGE(value(still.profile, "skill.dodge_prob"), 0, 0.05);

	/* The strafer switches every 500 ms on the clock, and the bursts
	 * come right at its switches: the old measure saw a sidestep after
	 * nearly every burst and every quiet moment alike.  At the same
	 * phase of its rhythm the quiet moments switch just as often.
	 */
	for (const auto &h : {strafer(), weaver()})
	{
		const auto w{analyse_flight(fly(h, CYCLES))};
		const auto &ws{w.stats};
		if (verbose)
			std::printf("%s: rate %.2f baseline %.2f room %.1f prob %.2f se %.2f\n", h.name, ws.dodge_rate, ws.dodge_baseline, ws.dodge_room, ws.dodge_prob, ws.dodge_se);
		CHECK_RANGE(ws.dodge_weave_rate, 0.5, 1.0);
		CHECK(ws.dodge_triggers >= f.bursts - 3);
		/* Nothing beyond chance: within two standard errors of 0, and
		 * a profile that does not trust it.
		 */
		CHECK(ws.dodge_prob - 2 * ws.dodge_se <= 0);
		CHECK_RANGE(ws.dodge_prob, 0, 0.3);
		if (const auto e{w.profile.find("skill.dodge_prob")}; e && e->value > 0.1)
			CHECK(e->confidence == bot::style_confidence::low);
		CHECK(!w.profile.find("measured.dodge_reaction_ms"));
		const auto wt{describe_traits(ws)};
		CHECK(std::none_of(wt.begin(), wt.end(), [](const std::string &t) { return t.starts_with("Dodges") || t.starts_with("Sometimes dodges"); }));
	}

	/* Weaves and dodges: found, though less sure than the plain dodger. */
	{
		const auto wf{fly(weaving_dodger(), CYCLES)};
		const auto w{analyse_flight(wf)};
		const auto &ws{w.stats};
		const double wtruth{static_cast<double>(wf.dodges) / wf.bursts};
		if (verbose)
			std::printf("weaving dodger: truth %.2f rate %.2f baseline %.2f room %.1f prob %.2f se %.2f reaction %.0f\n", wtruth, ws.dodge_rate, ws.dodge_baseline, ws.dodge_room, ws.dodge_prob, ws.dodge_se, ws.dodge_reaction_ms);
		CHECK(ws.dodge_measurable);
		CHECK_RANGE(ws.dodge_prob, wtruth - 0.3, wtruth + 0.15);
		CHECK(ws.dodge_prob - 2 * ws.dodge_se > 0);
		CHECK_RANGE(ws.dodge_reaction_ms, 200, 360);
		CHECK_RANGE(value(w.profile, "skill.dodge_prob"), wtruth - 0.3, wtruth + 0.15);
	}

	/* The enemy's shots: counted; no warning while they are there. */
	CHECK(s.enemy_shots == 4 * f.bursts);
	CHECK(s.hits_taken_from_players == f.bursts - f.dodges);
	CHECK(write_report(s, r.profile).find("warning: took") == std::string::npos);
	/* A recording without the enemy's shots (an older recorder left out
	 * the shots of the bots not recorded): hits but no shot, a warning.
	 */
	auto blind{f};
	std::erase_if(blind.events, [](const happening &h) { return h.e.type == record_type::fire && h.e.pid != 0; });
	const auto b{analyse_flight(blind)};
	CHECK(b.stats.enemy_shots == 0 && b.stats.dodge_triggers == 0 && !b.stats.dodge_measurable);
	CHECK(!b.profile.find("skill.dodge_prob"));
	CHECK(b.stats.hits_taken_from_players == s.hits_taken_from_players);
	CHECK(write_report(b.stats, b.profile).find("warning: took") != std::string::npos);
}

/* The afterburner: chasing from 150 units, and with no enemy about. */
void test_afterburner()
{
	/* afterburner_known_s etc. index the situations: 0 chasing, 1
	 * fleeing, 2 no enemy in sight, 3 the rest.
	 */
	const auto r{analyse_flight(fly(reverse_turner(), CYCLES))};
	const auto &s{r.stats};
	CHECK(s.ab_estimated_s == 0);
	/* With no enemy in sight: the 10 s of flight, not the 10 s it hangs
	 * about after the enemy left its sight.
	 */
	CHECK_RANGE(s.ab_situation_rate[2], 0.45, 0.55);
	CHECK_RANGE(s.ab_situation_rate[0], 0.2, 1.0);	/* chasing */
	CHECK_RANGE(s.ab_chase_distance.p10, 140, 185);
	CHECK_RANGE(value(r.profile, "style.burn_chase_distance"), 140, 185);
	CHECK_RANGE(value(r.profile, "tune.burn_roam"), 0.45, 0.55);
	const auto none{analyse_flight(fly(strafer(), CYCLES))};
	CHECK_RANGE(none.stats.ab_share, 0, 0.001);
	CHECK(value(none.profile, "style.burn_chase_distance") == 1000);
	CHECK_RANGE(value(none.profile, "tune.burn_roam"), 0, 0.01);
}

/* The same flights as a host records a client: no controls.  The
 * thrust estimated from the motion gives the same picture, marked as
 * estimated.
 */
void test_estimated_controls()
{
	view remote;
	remote.local_pid = 1;
	for (const auto &h : {strafer(), reverse_turner(), bobber()})
	{
		const auto f{fly(h, CYCLES)};
		const auto own{analyse_flight(f)};
		const auto est{analyse_flight(f, remote)};
		const auto &a{own.stats};
		const auto &b{est.stats};
		/* Where both exist, the estimate is close to the controls. */
		CHECK(a.estimator_n > 10000);
		CHECK_RANGE(a.estimator_rms, 0, 0.2);
		CHECK(b.exact_s == 0 && b.estimated_s > 600);
		CHECK_RANGE(b.estimated_share(), 1, 1);
		CHECK_RANGE(b.forward_share, a.forward_share - 0.06, a.forward_share + 0.06);
		CHECK_RANGE(b.reverse_share, a.reverse_share - 0.06, a.reverse_share + 0.06);
		CHECK_RANGE(b.fight_strafe_share, a.fight_strafe_share - 0.08, a.fight_strafe_share + 0.08);
		CHECK_RANGE(b.strafe_vertical, a.strafe_vertical - 0.1, a.strafe_vertical + 0.1);
		CHECK_RANGE(b.reverse_turn_share, a.reverse_turn_share - 0.1, a.reverse_turn_share + 0.1);
		CHECK_RANGE(b.slide_turn_share, a.slide_turn_share - 0.15, a.slide_turn_share + 0.15);
		CHECK_RANGE(b.large_turns, a.large_turns, a.large_turns);
		if (a.strafe_run_ms.n)
		{
			CHECK_RANGE(b.strafe_run_ms.p50, a.strafe_run_ms.p50 - 120, a.strafe_run_ms.p50 + 120);
			CHECK_RANGE(b.strafe_reversals_per_min, a.strafe_reversals_per_min * 0.8, a.strafe_reversals_per_min * 1.2);
		}
		/* The profile says how sure it is. */
		CHECK(own.profile.find("skill.strafe")->confidence == bot::style_confidence::high);
		CHECK(est.profile.find("skill.strafe")->confidence == bot::style_confidence::medium);
		CHECK(value(est.profile, "skill.strafe") == value(own.profile, "skill.strafe"));
		CHECK_RANGE(value(est.profile, "measured.estimated_controls_share"), 1, 1);
	}
	/* The afterburner of a client that does not report it (before
	 * exp-25): unknown in the file, estimated from the thrust.
	 */
	view old{remote};
	old.remote_afterburner = false;
	const auto f{fly(reverse_turner(), CYCLES)};
	const auto known{analyse_flight(f, remote)};
	const auto guessed{analyse_flight(f, old)};
	CHECK(known.stats.ab_estimated_s == 0);
	CHECK(guessed.stats.ab_estimated_s > 600);
	CHECK_RANGE(guessed.stats.ab_share, known.stats.ab_share - 0.05, known.stats.ab_share + 0.05);
	CHECK_RANGE(guessed.stats.ab_situation_rate[2], 0.4, 0.55);
	CHECK(guessed.profile.find("tune.burn_roam")->confidence == bot::style_confidence::medium);
}

/* The same flights as a host records a client that shares its controls
 * (-sharemoves): exact, marked shared, nothing estimated, the same
 * picture as the pilot's own recording.
 */
void test_shared_controls()
{
	view shared;
	shared.local_pid = 1;
	shared.shared_controls = true;
	for (const auto &h : {strafer(), reverse_turner()})
	{
		const auto f{fly(h, CYCLES)};
		const auto own{analyse_flight(f)};
		const auto sh{analyse_flight(f, shared)};
		const auto &a{own.stats};
		const auto &b{sh.stats};
		CHECK(a.shared_s == 0);
		CHECK(b.estimated_s == 0 && b.exact_s > 600);
		CHECK_RANGE(b.shared_s, b.exact_s, b.exact_s);
		CHECK_RANGE(b.exact_s, a.exact_s - 0.1, a.exact_s + 0.1);
		CHECK_RANGE(b.forward_share, a.forward_share, a.forward_share);
		CHECK_RANGE(b.reverse_share, a.reverse_share, a.reverse_share);
		CHECK_RANGE(b.fight_strafe_share, a.fight_strafe_share, a.fight_strafe_share);
		CHECK_RANGE(b.reverse_turn_share, a.reverse_turn_share, a.reverse_turn_share);
		CHECK_RANGE(b.ab_share, a.ab_share - 0.02, a.ab_share + 0.02);
		CHECK(b.ab_estimated_s == 0);
		/* As sure as from the pilot's own machine. */
		CHECK(sh.profile.find("skill.strafe")->confidence == bot::style_confidence::high);
		CHECK(value(sh.profile, "skill.strafe") == value(own.profile, "skill.strafe"));
		CHECK_RANGE(value(sh.profile, "measured.shared_controls_share"), 1, 1);
		CHECK(!own.profile.find("measured.shared_controls_share"));
		CHECK(write_report(b, sh.profile).find("shared by the player's machine") != std::string::npos);
	}
}

/* Two recordings of one game: the host's (the pilot flown there) and
 * the client's (the enemy flown there, joined later, left earlier).
 */
struct two_files
{
	flight f;
	std::vector<recording> files;
	view host, client;
};

two_files game_of_two(const bool sync, const char *const mission = "Test Mission", const std::uint32_t seed = 12345)
{
	two_files g;
	g.f = fly(strafer(), 6, seed);
	g.host.sync = g.client.sync = sync;
	g.host.pilot_name = g.client.pilot_name = "Alice";
	g.host.enemy_name = g.client.enemy_name = "Bob";
	g.host.mission = g.client.mission = mission;
	g.host.remote_lag = g.client.remote_lag = 1;
	g.client.local_pid = 1;
	g.client.host = false;
	/* 50.5 s after the host, until 3 min 20 s. */
	g.client.first_tick = 1515;
	g.client.last_tick = 6000;
	g.client.start_unix = g.host.start_unix + 53;
	g.files.push_back(load(record(g.f, g.host), "host.dmr"));
	g.files.push_back(load(record(g.f, g.client), "client.dmr"));
	return g;
}

void check_merged(const two_files &g, const session &ses)
{
	const auto ms{merge_session(g.files, ses)};
	CHECK(ms.players.size() == 2);
	const auto &alice{ms.players[0]};
	const auto &bob{ms.players[1]};
	CHECK(alice.callsign == "Alice" && bob.callsign == "Bob");
	const std::size_t ticks{g.f.moments.size()};
	const std::size_t client_ticks{g.client.last_tick - g.client.first_tick + 1};
	/* Alice: every tick once, from the host (her machine). */
	CHECK(alice.samples.size() == ticks);
	for (const auto &s : alice.samples)
		CHECK(s.file == 0 && (s.s.flags & sample_flag::controls));
	/* Bob: his own recording where there is one, the host's before and
	 * after; no tick twice, none missing (but for one at each seam).
	 */
	std::size_t own{}, from_host{};
	for (std::size_t i{}; i != bob.samples.size(); ++i)
	{
		const auto &s{bob.samples[i]};
		++(s.file == 1 ? own : from_host);
		CHECK((s.file == 1) == ((s.s.flags & sample_flag::controls) != 0));
		/* A tick apart; at a seam up to two, or a fraction of one. */
		if (i)
			CHECK(s.t - bob.samples[i - 1].t >= 10 && s.t - bob.samples[i - 1].t <= 70);
	}
	CHECK(own == client_ticks);
	CHECK_RANGE(static_cast<double>(from_host), static_cast<double>(ticks - client_ticks) - 3, static_cast<double>(ticks - client_ticks) + 1);
	/* Every event once, though both files have it. */
	unsigned shots{}, pickups{}, hits{}, bob_shots{};
	for (const auto &e : ms.events)
	{
		if (e.e.type == record_type::fire)
			++(e.who == 0 ? shots : bob_shots);
		else if (e.e.type == record_type::pickup)
			++pickups;
		else if (e.e.type == record_type::hit)
		{
			++hits;
			CHECK(e.who == 0 && e.other == 1);
		}
	}
	CHECK(shots == g.f.primary_shots + g.f.missiles);
	CHECK(pickups == g.f.pickups);
	CHECK(hits == g.f.bursts - g.f.dodges);
	CHECK_RANGE(bob_shots, g.f.bursts * 4 - 2, g.f.bursts * 4);
	/* The tracks: Bob's controls are exact where his file has him. */
	const auto tr{build_track(ms, 1)};
	const std::array<track, 1> tracks{{tr}};
	const auto st{analyse(tracks)};
	CHECK_RANGE(st.exact_s, client_ticks * TICK - 0.1, client_ticks * TICK + 0.1);
	CHECK_RANGE(st.alive_s, ticks * TICK - 0.2, ticks * TICK + 0.1);
}

void test_merge_by_sync()
{
	const auto g{game_of_two(true)};
	CHECK(g.files[0].session_id() == 0x1234abcdu && g.files[1].session_id() == 0x1234abcdu);
	CHECK(g.files[0].header.minor == FORMAT_MINOR);
	std::vector<std::string> notes;
	const auto sessions{group_sessions(g.files, &notes)};
	CHECK(sessions.size() == 1 && notes.empty());
	const auto &ses{sessions.front()};
	CHECK(ses.files.size() == 2);
	CHECK(ses.files[0].file == 0 && ses.files[0].source == clock_source::sync);
	CHECK(ses.files[1].file == 1 && ses.files[1].source == clock_source::sync);
	/* The client's time 0 is the host's tick 1515. */
	CHECK(ses.files[1].clock.to_session(0) == ses.files[0].clock.to_session(ms_of_tick(1515)));
	CHECK(ses.files[1].clock.to_session(12345) == ses.files[0].clock.to_session(ms_of_tick(1515) + 12345));
	check_merged(g, ses);
	/* The order of the files does not matter. */
	const std::array<recording, 2> swapped{{g.files[1], g.files[0]}};
	const auto again{group_sessions(swapped)};
	CHECK(again.size() == 1 && again.front().files.size() == 2 && again.front().files[0].file == 1);
	/* A game with another session id is another game. */
	auto other{game_of_two(true)};
	view elsewhere{other.host};
	elsewhere.session_id = 0x999;
	const std::array<recording, 2> two_games{{g.files[0], load(record(other.f, elsewhere), "other.dmr")}};
	CHECK(group_sessions(two_games).size() == 2);
	/* The whole way: each player once, Alice's numbers as from her own
	 * file alone.
	 */
	const auto result{analyse_recordings(g.files)};
	CHECK(result.players.size() == 2);
	const std::array<recording, 1> alone{{g.files[0]}};
	const auto single{analyse_recordings(alone)};
	const auto find{[](const analysis_result &r, const char *const name) -> const player_stats & {
		for (const auto &p : r.players)
			if (p.stats.callsign == name)
				return p.stats;
		CHECK(!"player found");
		std::abort();
	}};
	CHECK(find(result, "Alice").primary_shots == find(single, "Alice").primary_shots);
	CHECK(find(result, "Alice").alive_s == find(single, "Alice").alive_s);
	CHECK(find(result, "Alice").fight_strafe_share == find(single, "Alice").fight_strafe_share);
	CHECK(find(result, "Bob").exact_s > 100 && find(single, "Bob").exact_s == 0);
}

/* The same without sync records (files of format minor 0): the clocks
 * are aligned by the ships' paths.
 */
void test_merge_by_trajectory()
{
	const auto g{game_of_two(false)};
	CHECK(g.files[0].session_id() == 0 && g.files[0].syncs.empty());
	CHECK(g.files[0].header.minor == 0);
	std::vector<std::string> notes;
	const auto sessions{group_sessions(g.files, &notes)};
	CHECK(sessions.size() == 1 && notes.empty());
	const auto &ses{sessions.front()};
	CHECK(ses.files.size() == 2);
	CHECK(ses.files[0].file == 0 && ses.files[0].source == clock_source::own);
	CHECK(ses.files[1].file == 1 && ses.files[1].source == clock_source::trajectory);
	/* Right within the lag with which each sees the other (a tick), and
	 * nowhere near the 53 s between the headers' start times.
	 */
	const auto error{ses.files[1].clock.to_session(0) - ses.files[0].clock.to_session(ms_of_tick(1515))};
	CHECK_RANGE(static_cast<double>(error), -45, 45);
	check_merged(g, ses);
	/* Two games that look alike (same mission, same names, minutes
	 * apart) but are not the same: nothing aligns, two games, a note.
	 */
	auto a{game_of_two(false)};
	view b{a.client};
	b.first_tick = 0;
	b.last_tick = ~0u;
	const std::array<recording, 2> unlike{{a.files[0], load(record(fly(brawler(), 6, 999), b), "unlike.dmr")}};
	notes.clear();
	const auto apart{group_sessions(unlike, &notes)};
	CHECK(apart.size() == 2 && notes.size() == 1);
	CHECK(notes.front().find("unlike.dmr") != std::string::npos);
	/* Another mission is never the same game. */
	const auto c{game_of_two(false, "Other Mission")};
	const std::array<recording, 2> missions{{g.files[0], c.files[1]}};
	notes.clear();
	CHECK(group_sessions(missions, &notes).size() == 2 && notes.empty());
}

/* Several games of a player add up. */
void test_several_games()
{
	view first, second;
	second.session_id = 77;
	second.start_unix += 7200;
	const auto fa{fly(sniper(), 9, 1)};
	const auto fb{fly(sniper(), 9, 2)};
	const std::array<recording, 2> files{{load(record(fa, first), "a.dmr"), load(record(fb, second), "b.dmr")}};
	const auto r{analyse_recordings(files)};
	CHECK(r.sessions.size() == 2);
	const auto &p{r.players.front()};
	CHECK(p.stats.callsign == "Pilot" && p.stats.sessions == 2);
	CHECK_RANGE(p.stats.alive_s, 719, 721);
	CHECK(p.stats.primary_shots == fa.primary_shots + fb.primary_shots);
	CHECK_RANGE(p.stats.retreat_shields, 50, 50);
	CHECK(p.profile.source.find("2 games") != std::string::npos);

	/* A file without a session (minor 0) that looks like two games of
	 * different sessions does not make them one: first in the list, it
	 * joins one of them, and the other stays apart.
	 */
	view old, host_a, client_b;
	old.sync = false;
	old.host = false;
	old.local_pid = 1;
	host_a.session_id = 1;
	client_b.session_id = 2;
	client_b.host = false;
	client_b.local_pid = 1;
	client_b.host_epoch_ms += 3'600'000;
	const std::array<recording, 3> bridged{{load(record(fa, old), "old.dmr"), load(record(fa, host_a), "a.dmr"), load(record(fb, client_b), "b.dmr")}};
	const auto sessions{group_sessions(bridged)};
	for (const auto &ses : sessions)
		for (const auto &x : ses.files)
			for (const auto &y : ses.files)
				CHECK(bridged[x.file].session_id() == 0 || bridged[y.file].session_id() == 0 || bridged[x.file].session_id() == bridged[y.file].session_id());
	CHECK(sessions.size() == 2);
}

/* A recording cut short by a crash, and a slot that changes hands. */
void test_reader_cases()
{
	const auto f{fly(strafer(), 6)};
	auto bytes{record(f, {})};
	const auto whole{load(bytes, "whole")};
	CHECK(whole.stats.clean_end && whole.samples.size() == 2 * f.moments.size());
	bytes.resize(bytes.size() * 6 / 10);
	const auto cut{load(bytes, "cut")};
	CHECK(cut.stats.truncated && !cut.stats.clean_end);
	CHECK_RANGE(static_cast<double>(cut.samples.size()) / static_cast<double>(whole.samples.size()), 0.5, 0.61);
	const std::array<recording, 1> files{{cut}};
	const auto r{analyse_recordings(files)};
	CHECK_RANGE(r.players.front().stats.alive_s, 115, 147);
	CHECK_RANGE(r.players.front().stats.fight_strafe_share, 0.7, 1.0);
	/* Not a recording. */
	CHECK(!load_recording(std::span<const std::uint8_t>(bytes).first(10), "short"));

	/* Slot 1 goes from Bob to Carol: the samples and events after the
	 * player record are Carol's.
	 */
	std::array<std::uint8_t, MAX_HEADER_SIZE> hb;
	file_header h;
	h.flags = static_cast<std::uint16_t>(header_flag::multiplayer);
	h.num_players = 2;
	h.players[0] = {0, player_flag::connected | player_flag::local | player_flag::recorded, 0xff, "Alice"};
	h.players[1] = {1, player_flag::connected | player_flag::recorded, 0xff, "Bob"};
	const auto n{encode_header(hb, h)};
	std::vector<std::uint8_t> file(hb.begin(), hb.begin() + static_cast<std::ptrdiff_t>(n));
	const auto cb{std::make_unique<chunk_builder>()};
	record_buffer buf;
	sample s;
	s.pid = 1;
	s.flags = sample_flag::alive;
	s.context = context_flag::kind_player;
	s.enemy_id = 0;
	CHECK(cb->append(encode_level(buf, 1, 10, 0, "M", "L")));
	CHECK(cb->append(encode(buf, tick_record{0, 0})));
	CHECK(cb->append(encode(buf, s)));
	CHECK(cb->append(encode(buf, event_record{record_type::fire, 10, 1, PLAYER_NONE, 0, 0, 0, 0})));
	CHECK(cb->append(encode_player(buf, 1, player_flag::connected | player_flag::recorded, 0xff, "Carol")));
	CHECK(cb->append(encode(buf, tick_record{1, 33})));
	CHECK(cb->append(encode(buf, s)));
	CHECK(cb->append(encode(buf, event_record{record_type::hit, 40, 0, 1, attacker_kind::player, 0, 256, 0})));
	/* A sample of no player (a damaged or hostile file) is skipped. */
	sample nobody{s};
	nobody.pid = PLAYER_NONE;
	CHECK(cb->append(encode(buf, nobody)));
	const auto chunk{cb->finish()};
	file.insert(file.end(), chunk.begin(), chunk.end());
	const auto rec{load(file, "slots")};
	CHECK(rec.players.size() == 3);
	CHECK(rec.players[0].callsign == "Alice" && rec.players[0].local);
	CHECK(rec.players[1].callsign == "Bob" && rec.players[2].callsign == "Carol");
	CHECK(rec.samples.size() == 2 && rec.samples[0].who == 1 && rec.samples[1].who == 2);
	CHECK(rec.samples[0].enemy == 0 && rec.samples[0].time_ms == 0 && rec.samples[1].time_ms == 33);
	CHECK(rec.events.size() == 2 && rec.events[0].who == 1 && rec.events[1].who == 0 && rec.events[1].other == 2);
}

/* The profile's file format. */
void test_profile_format()
{
	const auto r{analyse_flight(fly(brawler(), CYCLES))};
	const auto &p{r.profile};
	CHECK(p.name == "Pilot style" && p.callsign == "Pilot");
	const auto text{bot::write_style_profile(p)};
	const auto back{bot::parse_style_profile(text)};
	CHECK(back);
	CHECK(back->name == p.name && back->callsign == p.callsign && back->source == p.source);
	CHECK(back->base_skill == p.base_skill && back->base_style == p.base_style);
	CHECK(back->entries.size() == p.entries.size());
	for (std::size_t i{}; i != p.entries.size(); ++i)
	{
		CHECK(back->entries[i].key == p.entries[i].key);
		CHECK(back->entries[i].confidence == p.entries[i].confidence);
		CHECK(std::abs(back->entries[i].value - p.entries[i].value) <= 1e-4 * std::abs(p.entries[i].value));
	}
	/* Written again, the text is the same. */
	CHECK(bot::write_style_profile(*back) == text);
	/* Every key of the table is a field a profile can set. */
	for (const auto &k : bot::style_profile_keys)
		CHECK(k.lo < k.hi && (k.key.starts_with("style.") || k.key.starts_with("skill.") || k.key.starts_with("tune.")));

	/* By hand: comments, spaces, unknown keys, values out of range. */
	const auto hand{bot::parse_style_profile(
		"# a profile\r\n"
		"\n"
		"  format=1\n"
		"name =  Nico style  \n"
		"callsign = Nico\n"
		"base_skill = ace\n"
		"base_style = AGGRESSIVE\n"
		"style.retreat_shields = 500\n"
		"style.engage_weight = 1.25\n"
		"confidence.style.engage_weight = low\n"
		"skill.strafe_min_ms = 300\n"
		"skill.strafe_max_ms = 200\n"
		"skill.dodge_prob = nonsense\n"
		"future.thing = 12\n"
		"confidence.future.thing = medium\n"
		"a line without the sign\n"
		" = 5\n"
	)};
	CHECK(hand);
	CHECK(hand->name == "Nico style" && hand->callsign == "Nico");
	CHECK(hand->base_skill == bot::bot_skill::ace && hand->base_style == bot::bot_style::aggressive);
	CHECK(hand->value("style.retreat_shields") == 90.0);
	CHECK(hand->find("style.engage_weight")->confidence == bot::style_confidence::low);
	CHECK(hand->find("style.retreat_shields")->confidence == bot::style_confidence::high);
	CHECK(!hand->find("skill.dodge_prob"));
	CHECK(hand->value("future.thing") == 12.0 && hand->find("future.thing")->confidence == bot::style_confidence::medium);
	CHECK(hand->entries.size() == 5);
	/* Not profiles. */
	CHECK(!bot::parse_style_profile(""));
	CHECK(!bot::parse_style_profile("name = x\nstyle.engage_weight = 1\n"));
	CHECK(!bot::parse_style_profile("format = 2\nname = from the future\n"));

	/* Onto the bot's parameters. */
	const auto ace{bot::skill_of(bot::bot_skill::ace)};
	const auto aggressive{bot::style_of(bot::bot_style::aggressive)};
	const auto applied{bot::apply_style_profile(*hand, bot::bot_skill::ace)};
	CHECK(applied.style.retreat_shields == 90);
	/* Low confidence: a quarter of the way from the base style. */
	CHECK_RANGE(applied.style.engage_weight, aggressive.engage_weight + 0.25 * (1.25 - aggressive.engage_weight) - 1e-9, aggressive.engage_weight + 0.25 * (1.25 - aggressive.engage_weight) + 1e-9);
	/* Not in the profile: the base style's. */
	CHECK(applied.style.collect_weight == aggressive.collect_weight && applied.style.burn_chase_distance == aggressive.burn_chase_distance);
	CHECK(applied.skill.strafe_min_ms == 300 && applied.skill.strafe_max_ms == 300);
	/* The skill keeps what is its own. */
	CHECK(applied.skill.aim_sigma_deg == ace.aim_sigma_deg && applied.skill.reaction_ms == ace.reaction_ms && applied.skill.dodge_prob == ace.dodge_prob);
	/* A skill that neither strafes nor dodges still does not. */
	const auto trainee{bot::apply_style_profile(p, bot::bot_skill::trainee)};
	CHECK(!trainee.skill.strafe && trainee.skill.dodge_prob == 0);
	/* Medium confidence: most of the way from Aggressive's 30 to 10. */
	CHECK_RANGE(trainee.style.retreat_shields, 15.9, 16.1);
	/* The brawler's profile on an Insane bot. */
	const auto insane{bot::apply_style_profile(p, bot::bot_skill::insane)};
	CHECK(insane.style.retreat_shields < 17 && insane.style.chase_memory > 1.1);
	/* Section 9.13 of the bots' document: its band is its own (the
	 * quartiles of its firing distance), not the base band scaled.
	 */
	CHECK(insane.style.range_scale == 1 && insane.tune.range_hi < 70 && insane.tune.range_lo < 50);
	CHECK(!insane.skill.strafe);
	/* It never dodged (medium confidence: 54 bursts): most of the way
	 * down from Insane's 0.85.
	 */
	CHECK_RANGE(insane.skill.dodge_prob, 0.2, 0.3);
}

/* Too little data gives no opinion. */
void test_little_data()
{
	view v;
	v.last_tick = 10 * RATE;
	const auto r{analyse_flight(fly(strafer(), 1), v)};
	CHECK_RANGE(r.stats.alive_s, 10, 10.1);
	const auto traits{describe_traits(r.stats)};
	CHECK(traits.size() == 1 && traits.front().starts_with("Too little data"));
	for (const auto &e : r.profile.entries)
		CHECK(e.key.starts_with("measured.") || e.confidence == bot::style_confidence::low);
}

/* -w DIR: the synthetic recordings as files, to try movrec-analyse and
 * movrec-dump on (a game of two recorded on both machines, and one
 * recording each of the sniper and the brawler).
 */
int write_examples(const std::string &dir)
{
	const auto write{[&dir](const char *const name, const std::vector<std::uint8_t> &bytes) {
		const std::string path{dir + "/" + name};
		std::FILE *const f{std::fopen(path.c_str(), "wb")};
		if (!f)
		{
			std::fprintf(stderr, "%s: cannot write\n", path.c_str());
			return false;
		}
		const bool ok{std::fwrite(bytes.data(), 1, bytes.size(), f) == bytes.size()};
		std::printf("%s\n", path.c_str());
		return !std::fclose(f) && ok;
	}};
	const auto g{game_of_two(true)};
	view sniper_view, brawler_view;
	sniper_view.pilot_name = "Sniper";
	sniper_view.session_id = 0x51;
	brawler_view.pilot_name = "Brawler";
	brawler_view.session_id = 0xb2;
	brawler_view.start_unix += 3600;
	return write("example-host.dmr", record(g.f, g.host))
		&& write("example-client.dmr", record(g.f, g.client))
		&& write("example-sniper.dmr", record(fly(sniper(), CYCLES), sniper_view))
		&& write("example-brawler.dmr", record(fly(brawler(), CYCLES), brawler_view)) ? 0 : 1;
}

}

int main(const int argc, char **const argv)
{
	if (argc > 2 && !std::strcmp(argv[1], "-w"))
		return write_examples(argv[2]);
	verbose = argc > 1 && !std::strcmp(argv[1], "-v");
	test_ship_model();
	test_summary();
	test_strafer();
	test_turns();
	test_sniper();
	test_brawler();
	test_dodger();
	test_afterburner();
	test_estimated_controls();
	test_shared_controls();
	test_merge_by_sync();
	test_merge_by_trajectory();
	test_several_games();
	test_reader_cases();
	test_profile_format();
	test_little_data();
	std::puts("test-movement-analysis: all checks passed");
	return 0;
}
