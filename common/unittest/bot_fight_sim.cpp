/*
 * This file is part of the DXX-Rebirth project <https://github.com/dxx-rebirth/dxx-rebirth/>.
 * It is copyright by its individual contributors, as recorded in the
 * project's Git history.  See COPYING.txt at the top level for license
 * terms and a link to the Git history.
 */
/*
 * How a bot flies a fight, measured as a human's flight is measured
 * (Documentation/multiplayer-bots.md section 9.12): the bot's movement
 * code (bot_brain.h, bot_goals.h: the strafe, the fight's velocity, the
 * turn round, the velocity and steering controllers, the afterburner)
 * flies the model of the ship of test-bot-flight against a scripted
 * enemy through a fixed programme, the flight is written as the game's
 * movement recorder writes it (-recordmoves-bots), and the analysis of
 * movrec-analyse (movement_analysis.h) reads it.  The numbers must lie
 * near what the analysis measured of a strong human (the playtest of
 * 2026-09-30) and away from what the bots flew before.
 *
 * The programme, a cycle of 40 s, again and again:
 *
 *	 0-14 s  a fight: the enemy strafes round the bot; at 5 s and 10 s it
 *	         is suddenly behind the bot (a large turn)
 *	14-20 s  the enemy is behind the bot again, the fight goes on
 *	20-30 s  the enemy is out of sight: the bot flies a path
 *	30-40 s  no enemy: the bot flies a path
 *
 * with the bot's shields at 100, 80, 60, 45, 30, 15 in turn; below its
 * style's retreat level it retreats from the enemy along a path while
 * the enemy follows.  No walls: the reverse turn always has room.
 *
 * Robustness.  The flight is a long floating-point simulation, and a
 * tiny difference (another compiler, -O level, fused multiply-add, the
 * maths library's sin/exp) changes a trajectory as another seed would;
 * bit-exact results across platforms would need the bots' own maths
 * (bot_brain.h) in fixed point.  So every measurement is statistical:
 * one measurement flies FLIGHT_SEEDS seeds of six cycles each (every
 * shield level once) and analyses them together, and every range lies
 * at least 5 standard deviations of that measurement from its mean.
 * The deviations come from `-survey 30` (30 disjoint sets of seeds),
 * with g++ 15 (-O0, -O2, fused multiply-add on and off) and clang++ 21
 * on 2026-10-01; the smallest margin then was 5.2 sd.  Measurements
 * that are counts by construction (a Trainee never burns, a profile's
 * turns are all reversed) have no spread.  When the bots' flying is
 * tuned, rerun `-survey 30` and keep the margins.  The ranges still
 * fail on the bots' flying before #63 (their strafe timings, vertical
 * share and retreat levels): too few strafe reversals, too much
 * vertical strafe.
 *
 * Build and run with SCons:
 *
 *	scons sdl2=1 d1x=0 d2x=1 register_runtime_test_plain_link_targets=1 test-bot-fight-sim
 *	build/common/test-bot-fight-sim [-v] [-survey N] [-f FILE]
 *
 * -v prints the analysis' report of every flight; -survey N prints each
 * measurement's spread over N sets of seeds and its margin.
 */

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <numbers>
#include <string>
#include <vector>

#include "movement_analysis.h"
#include "bot_goals.h"
#include "bot_nav.h"
#include "bot_flight_model.h"
#include "bot_style_profile.h"

namespace mr = dcx::movrec;
namespace an = dcx::movrec::analysis;
using namespace dcx::bot;
using namespace dcx::bot::flight_model;

namespace {

bool verbose;

void check_failed(const char *const expr, const char *const file, const int line)
{
	std::fprintf(stderr, "%s:%d: check failed: %s\n", file, line, expr);
	std::exit(1);
}

#define CHECK(cond)	do { if (!(cond)) check_failed(#cond, __FILE__, __LINE__); } while (0)

using bvec = dcx::bot::vec3;

mr::vec3 to_mr(const vec3 &v)
{
	return {{v.x, v.y, v.z}};
}


constexpr unsigned RECORD_RATE{30};
constexpr std::size_t STYLE_FILE_MAX_BYTES_SIM{64 * 1024};
constexpr unsigned CYCLE_S{40};
constexpr std::array<double, 6> CYCLE_SHIELDS{{100, 80, 60, 45, 30, 15}};
/* How many seeds one measurement flies (see the header comment). */
constexpr unsigned FLIGHT_SEEDS{8};

/* The enemy: a ship that strafes round the bot at its own distance, as
 * a human does, with a little up and down.  Moved as a point with the
 * ship's acceleration.
 */
struct enemy
{
	vec3 pos, vel;
	vec3 strafe_dir;
	double next_change{};
	double keep{70};
};

/* One sample as the recorder writes it. */
struct moment
{
	vec3 pos, vel;
	frame3 orient;
	double pitch_rate{}, heading_rate{}, bank_rate{};
	std::array<double, 6> ctl{};
	bool burning{};
	double shields{};
	bool enemy_present{}, sight{};
	vec3 enemy_pos, enemy_vel;
};

/* A path of straight legs in the open: the next point, as follow_path
 * steers at it.
 */
struct open_path
{
	std::vector<vec3> points;
	std::size_t index{};
	void plan(bot_rng &rng, const vec3 &from, const vec3 &heading, const unsigned legs)
	{
		points.clear();
		index = 0;
		auto p{from};
		auto d{normalized(heading)};
		if (d == vec3{})
			d = {0, 0, 1};
		for (unsigned i{}; i != legs; ++i)
		{
			/* A bend of 40 to 100 degrees about a random axis. */
			const auto axis{normalized(cross(d, vec3{rng.uniform(-1, 1), rng.uniform(-1, 1), rng.uniform(-1, 1)}))};
			const double a{i ? radians(rng.uniform(40, 100)) : 0};
			d = normalized(d * std::cos(a) + cross(axis, d) * std::sin(a));
			p += d * rng.uniform(50, 200);
			points.push_back(p);
		}
	}
};

struct flight_result
{
	an::player_stats stats;
	/* Seconds with the afterburner, and all. */
	double burn_s{}, total_s{};
	/* The bot's own large turns: flown backwards, sliding. */
	unsigned turns_reversing{}, turns_sliding{};
};

/* What the bot is set to. */
struct pilot_setup
{
	bot_skill skill{bot_skill::hotshot};
	bot_style style{bot_style::balanced};
	/* Each seed's flight: CYCLE_SHIELDS.size() cycles, every shield
	 * level once.
	 */
	unsigned cycles{static_cast<unsigned>(CYCLE_SHIELDS.size())};
	/* The first of the FLIGHT_SEEDS seeds flown; their flights are
	 * analysed together, as the sessions of one player.
	 */
	uint32_t seed{1};
	double fps{60};
	/* Section 9.13: a style profile's parameters (apply_style_profile)
	 * in place of the skill's and the style's.
	 */
	const style_profile_params *params{};
};

std::vector<uint8_t> write_recording(const std::vector<moment> &moments)
{
	using namespace mr;
	std::vector<std::uint8_t> bytes;
	file_header h;
	h.tick_rate = RECORD_RATE;
	h.flags = static_cast<std::uint16_t>(header_flag::multiplayer) | static_cast<std::uint16_t>(header_flag::host);
	h.start_unix_time = 1'790'000'000;
	h.local_pid = 1;
	h.program = "test-bot-fight-sim";
	h.mission = "Open Space";
	h.level_name = "Arena";
	h.level_num = 1;
	h.num_players = 2;
	h.players[0] = {0, static_cast<std::uint8_t>(player_flag::connected | player_flag::recorded | player_flag::local), 0xff, "pilot"};
	h.players[1] = {1, static_cast<std::uint8_t>(player_flag::connected), 0xff, "enemy"};
	std::array<std::uint8_t, MAX_HEADER_SIZE> hb;
	const auto n{encode_header(hb, h)};
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
		if (!chunk->fits(r.size()))
			flush();
		chunk->append(r);
	}};
	put(encode_level(buf, 1, 100, 0x21, h.mission, "Arena"));
	for (const auto &p : std::span(h.players).first(2))
		put(encode_player(buf, p.pid, p.flags, p.team, p.callsign));
	const auto q_pos{[](const bvec &v) {
		return std::array<std::int32_t, 3>{{static_cast<std::int32_t>(std::lround(v.x * 256)), static_cast<std::int32_t>(std::lround(v.y * 256)), static_cast<std::int32_t>(std::lround(v.z * 256))}};
	}};
	const auto q16{[](const bvec &v, const double scale) {
		const auto one{[scale](const double c) {
			return static_cast<std::int16_t>(std::clamp(std::lround(c * scale), -32767L, 32767L));
		}};
		return std::array<std::int16_t, 3>{{one(v.x), one(v.y), one(v.z)}};
	}};
	for (std::uint32_t tick{}; tick != moments.size(); ++tick)
	{
		const auto &m{moments[tick]};
		const auto now{static_cast<std::uint32_t>(std::uint64_t{tick} * 1000 / RECORD_RATE)};
		put(encode(buf, tick_record{tick, now}));
		if (tick % RECORD_RATE == 0)
			put(encode(buf, sync_record{now, 0x5151u, 9'000'000 + now, static_cast<std::uint8_t>(sync_flag::clock_valid | sync_flag::host)}));
		const bvec enemy_pos{m.enemy_present ? m.enemy_pos : bvec{20000, 0, 0}};
		const auto rel{enemy_pos - m.pos};
		const double dist{length(rel)};
		{
			sample s;
			s.pid = 0;
			s.flags = sample_flag::alive | sample_flag::controls;
			if (m.burning)
				s.flags |= sample_flag::afterburner;
			s.flags2 = sample_flag2::local | sample_flag2::vitals_exact | sample_flag2::afterburner_known;
			for (std::size_t i{}; i != 6; ++i)
				s.controls[i] = quantise_control(m.ctl[i]);
			s.segment = 1;
			s.pos = q_pos(m.pos);
			s.quat = quantise_quaternion(quaternion_from_axes(to_mr(m.orient.r), to_mr(m.orient.u), to_mr(m.orient.f)));
			s.vel = q16(m.vel, 64);
			s.rotvel = q16({m.pitch_rate, m.heading_rate, m.bank_rate}, 4096);
			s.shields = static_cast<std::uint8_t>(std::clamp(m.shields, 0.0, 255.0));
			s.energy = 100;
			if (m.enemy_present)
			{
				s.context = context_flag::kind_player;
				if (m.sight)
					s.context |= context_flag::line_of_sight | context_flag::me_in_its_cone;
				if (dot(rel, m.orient.f) > 0.866 * dist)
					s.context |= context_flag::in_my_cone;
				s.enemy_id = 1;
				s.enemy_rel_pos = q16(rel, 16);
				s.enemy_rel_vel = q16(m.enemy_vel - m.vel, 64);
				if (m.sight && dist < 800)
					s.aimed_at_mask = 2;
			}
			put(encode(buf, s));
		}
		{
			const auto f{normalized(m.pos - enemy_pos)};
			auto r{normalized(cross(bvec{0, 1, 0}, f))};
			if (r == bvec{})
				r = {1, 0, 0};
			const auto u{cross(f, r)};
			sample s;
			s.pid = 1;
			s.flags = sample_flag::alive;
			s.segment = 2;
			s.pos = q_pos(enemy_pos);
			s.quat = quantise_quaternion(quaternion_from_axes(to_mr(r), to_mr(u), to_mr(f)));
			s.vel = q16(m.enemy_vel, 64);
			s.shields = 100;
			s.energy = 100;
			if (m.enemy_present)
			{
				s.context = context_flag::kind_player | context_flag::in_my_cone;
				if (m.sight)
					s.context |= context_flag::line_of_sight;
				s.enemy_id = 0;
				s.enemy_rel_pos = q16(rel * -1.0, 16);
				s.enemy_rel_vel = q16(m.vel - m.enemy_vel, 64);
			}
			put(encode(buf, s));
		}
	}
	put(encode(buf, event_record{record_type::end, static_cast<std::uint32_t>(moments.size() * 1000 / RECORD_RATE), PLAYER_NONE, PLAYER_NONE, end_reason::closed, 0, 0, 0}));
	flush();
	return bytes;
}

/* The bot's movement, as bot_tick (similar/main/bot.cpp) does it, in the
 * open: the tick's decisions and then the frame's steering.  One seed's
 * flight; its counters are added to `out`, the recording returned.
 */
std::vector<uint8_t> fly_one(const pilot_setup &setup, const uint32_t seed, flight_result &out)
{
	const auto lim{ship_limits()};
	const style_profile_params prm{setup.params ? *setup.params : style_profile_params{skill_of(setup.skill), style_of(setup.style), {}}};
	const auto &sk{prm.skill};
	const auto &st{prm.style};
	const auto &tune{prm.tune};
	const double max_speed{lim.max_speed};
	const double FPS{setup.fps};
	const fix ft{to_fix(1 / FPS)};
	bot_rng rng{bot_seed(seed, 1, 1)};
	bot_rng world{bot_seed(seed, 7, 3)};
	ship s;
	enemy e;
	juke_state juke;
	turn_round_state turning;
	slide_state slide;
	lateral_keys lateral;
	approach_key approach;
	aim_error aim;
	open_path path;
	int heading_pref{1};
	vec3 face{0, 0, 1}, face_rate, move;
	bool burning{};
	double charge{1};
	std::vector<moment> moments;
	ticker tk;
	uint32_t tick{};
	const unsigned frames{static_cast<unsigned>(setup.cycles * CYCLE_S * FPS)};
	/* The recorder samples on its own schedule of game time. */
	double next_record{0};
	bool had_sight{};
	bool retreat_goal_before{}, flee_turned{}, flee_burn{};
	uint32_t flee_roll_at{};
	double shields{100};
	for (unsigned frame{}; frame != frames; ++frame)
	{
		const double t{frame / FPS};
		const unsigned cycle{static_cast<unsigned>(t / CYCLE_S)};
		const double ct{t - cycle * CYCLE_S};
		shields = CYCLE_SHIELDS[cycle % CYCLE_SHIELDS.size()];
		const bool retreating{shields < st.retreat_shields};
		const bool present{ct < 30};
		const bool sight{ct < 20};
		/* The enemy's script. */
		if (frame == 0 || static_cast<unsigned>((frame - 1) / FPS / CYCLE_S) != cycle)
		{
			if (length(s.pos) > 5000)
				s.pos = {};
			e.pos = s.pos + s.orient.f * 150 + s.orient.r * world.uniform(-40, 40);
			e.vel = {};
			e.keep = retreating ? 60 : world.uniform(50, 90);
		}
		const auto behind_at{[&](const double at) {
			return !retreating && ct >= at && ct - 1 / FPS < at;
		}};
		if (behind_at(5) || behind_at(10) || behind_at(14))
		{
			e.pos = s.pos - s.orient.f * world.uniform(70, 110) + s.orient.u * world.uniform(-20, 20) + s.orient.r * world.uniform(-20, 20);
			e.vel = s.vel;
		}
		if (sight)
		{
			const auto to_bot{s.pos - e.pos};
			const double d{length(to_bot)};
			if (t >= e.next_change)
			{
				const auto across{normalized(cross(to_bot, vec3{world.uniform(-1, 1), world.uniform(-1, 1), world.uniform(-1, 1)}))};
				e.strafe_dir = across;
				e.next_change = t + world.uniform(0.4, 1.0);
			}
			const auto want{e.strafe_dir * 50 + normalized(to_bot) * std::clamp((d - e.keep) * 2, -50.0, 60.0)};
			e.vel += (want - e.vel) * (1 - std::exp(-2.12 / FPS));
		}
		else if (present)
			e.vel += (s.orient.r * 55 - e.vel) * (1 - std::exp(-2.12 / FPS));
		e.pos += e.vel * (1 / FPS);
		/* The bot's ticks. */
		for (unsigned n{tk.advance(ft)}; n; --n)
		{
			++tick;
			aim.update(rng, radians(sk.aim_sigma_deg), ticks_from_ms(sk.aim_drift_ms));
			const double range_lo{tune.range_lo * st.range_scale}, range_hi{tune.range_hi * st.range_scale};
			juke.update(rng, ticks_from_ms(sk.strafe_min_ms), ticks_from_ms(sk.strafe_max_ms), range_lo, range_hi, sk.strafe_vertical);
			const auto to{e.pos - s.pos};
			const double dist{length(to)};
			const auto frame3_now{s.orient};
			vec3 wanted;
			bool keys{};
			thrust_keys k;
			bool long_straight{};
			bool retreat_goal{};
			if (sight)
			{
				if (!had_sight || path.points.empty())
					path.points.clear();
				had_sight = true;
				const auto aim_at{aim_point(s.pos, e.pos, e.vel, 120, 1)};
				face = apply_aim_offset(aim_at - s.pos, frame3_now.u, aim.yaw(), aim.pitch());
				face_rate = line_of_sight_rate(aim_at - s.pos, e.vel - s.vel);
				if (retreating)
				{
					/* The retreat's path: away from the enemy, flown
					 * facing it or turned away (b::FLEE_TURNED_SHARE,
					 * drawn when the retreat starts).
					 */
					if (!retreat_goal_before || tick >= flee_roll_at)
					{
						flee_turned = rng.uniform() < FLEE_TURNED_SHARE;
						flee_burn = rng.uniform() < tune.flee_burn;
						flee_roll_at = tick + FLEE_ROLL_TICKS;
					}
					retreat_goal = true;
					if (path.points.empty() || distance(path.points.back(), s.pos) < 20)
						path.plan(rng, s.pos, -to, 3);
					path.index = advance_along(path.points, path.index, s.pos, 6.6);
					wanted = normalized(path.points[std::min(path.index, path.points.size() - 1)] - s.pos) * max_speed;
					if (flee_turned)
					{
						face = normalized(wanted);
						face_rate = {};
					}
					turning.reset();
				}
				else
				{
					const double err{angle_between(frame3_now.f, face)};
					const auto before{turning.phase};
					const auto turn{turning.update(err, tick, dist, range_lo, rng, tune.turns)};
					if (turn != before)
					{
						out.turns_reversing += turn == turn_phase::reversing;
						out.turns_sliding += turn == turn_phase::sliding;
					}
					if (turn == turn_phase::reversing || turn == turn_phase::boost)
						wanted = turn_round_velocity(turn, {}, to, s.vel, frame3_now.r * static_cast<double>(heading_pref), max_speed, tune.turns.reverse_speed);
					else
					{
						keys = true;
						const int sl{slide.update(turn == turn_phase::sliding ? std::max(err, SLIDE_START + 0.01) : err, dot(s.vel, frame3_now.r), heading_pref)};
						if (sl)
							k = slide_keys(sl);
						else
							k = fight_keys(juke, approach.update(dist, juke.range(), range_hi - range_lo) * effective_close_speed(st), effective_strafe_speed(sk, st));
					}
				}
			}
			else
			{
				if (had_sight || path.points.empty() || distance(path.points.back(), s.pos) < 20)
					path.plan(rng, s.pos, s.orient.f, 12);
				had_sight = false;
				turning.reset();
				path.index = advance_along(path.points, path.index, s.pos, 6.6);
				const auto &p{path.points[std::min(path.index, path.points.size() - 1)]};
				const auto dir{normalized(p - s.pos)};
				double speed{max_speed};
				if (path.index + 1 < path.points.size())
				{
					const double dp{distance(p, s.pos)};
					if (angle_between(p - s.pos, path.points[path.index + 1] - p) > radians(60) && dp < 40)
						speed = std::min(speed, std::max(max_speed * 0.4, dp * 1.5));
				}
				wanted = dir * speed;
				face = dir;
				face_rate = {};
				long_straight = distance(p, s.pos) > BOT_LONG_STRAIGHT;
			}
			retreat_goal_before = retreat_goal;
			move = keys ? frame3_now.to_world(k.local()) : velocity_command(wanted, s.vel, max_speed);
			move = frame3_now.to_world(lateral.apply(frame3_now.to_local(move), tick));
			const bool aligned{length(wanted) > max_speed * 0.5 && angle_between(frame3_now.f, wanted) < radians(25)};
			burning = want_afterburner({
				.have = true,
				.charge = charge,
				.use = afterburner_of(setup.skill),
				.chasing_far = false,
				.retreating = retreat_goal && flee_burn,
				.dodging = false,
				.long_straight = long_straight,
				.turn_boost = turning.phase == turn_phase::boost && turning.burn,
				.aligned = aligned,
				.burning = burning,
			});
		}
		auto c{steer(s, face, face_rate, move, lim, sk.turn_cap, heading_pref)};
		const bool burn_now{burning && c.forward > 0 && charge > 0};
		const double scale{1 + 2 * std::min(0.5, charge)};
		s.step(c, ft, burn_now ? scale : 0);
		if (burn_now)
			charge = std::max(0.0, charge - 1 / (3 * FPS));
		else
			charge = std::min(1.0, charge + 1 / (8 * FPS));
		out.total_s += 1 / FPS;
		out.burn_s += burn_now ? 1 / FPS : 0;
		if (t + 1e-9 >= next_record)
		{
			next_record += 1.0 / RECORD_RATE;
			moment m;
			m.pos = s.pos;
			m.vel = s.vel;
			m.orient = s.orient;
			m.pitch_rate = s.pitch_rate;
			m.heading_rate = s.heading_rate;
			m.bank_rate = s.bank_rate;
			m.ctl = {{burn_now ? scale : c.forward, c.sideways, c.vertical, c.pitch, c.heading, 0}};
			m.burning = burn_now;
			m.shields = shields;
			m.enemy_present = present;
			m.sight = sight;
			m.enemy_pos = e.pos;
			m.enemy_vel = e.vel;
			moments.push_back(m);
		}
	}
	return write_recording(moments);
}

/* FLIGHT_SEEDS flights, from setup.seed on, analysed together. */
flight_result fly(const pilot_setup &setup)
{
	flight_result out;
	std::vector<an::recording> files;
	for (uint32_t i{}; i != FLIGHT_SEEDS; ++i)
	{
		const auto bytes{fly_one(setup, setup.seed + i, out)};
		auto rec{an::load_recording(bytes, "sim")};
		if (!rec)
		{
			std::fprintf(stderr, "the recording does not load\n");
			std::exit(1);
		}
		files.push_back(std::move(*rec));
	}
	const auto result{an::analyse_recordings(files, setup.skill)};
	for (const auto &p : result.players)
		if (p.stats.callsign == "pilot")
		{
			out.stats = p.stats;
			if (verbose)
				std::printf("%s\n", an::write_report(p.stats, p.profile).c_str());
		}
	return out;
}

void print_row(const char *const name, const flight_result &r)
{
	const auto &s{r.stats};
	std::printf("test-bot-fight-sim: %-16s speed %4.1f fast %3.0f%% | strafe %3.0f%% runs %4.0f ms %5.1f rev/min vert %.2f thrust %3.0f%% across %3.0f%% | turns %3u rev %3.0f%% slide %3.0f%% 180 in %4.0f ms at %3.0f%% push %3.0f%% (burn %3.0f%%) | ab %4.1f%% flee %3.0f%% roam %3.0f%% | retreat %2.0f back %3.0f%% away %3.0f%%\n",
		name, s.speed.mean, 100 * s.fast_share,
		100 * s.fight_strafe_share, s.strafe_run_ms.p50, s.strafe_reversals_per_min, s.strafe_vertical, 100 * s.strafe_thrust, 100 * s.strafe_speed,
		s.large_turns, 100 * s.reverse_turn_share, 100 * s.slide_turn_share, s.turn_180_ms.p50, 100 * s.turn_rate, 100 * s.turn_boost_share, 100 * s.turn_boost_burn_share,
		100 * s.ab_share, 100 * s.ab_situation_rate[1], 100 * s.ab_situation_rate[2],
		s.retreat_shields, 100 * s.back_off_share, 100 * s.retreat_share);
}

/* What a strong human flew (the recordings of 2026-09-30, both games,
 * EC.report.txt) and what the bots flew then (five bots, Hotshot
 * and Insane), for the table in the output.
 */
void print_reference()
{
	std::puts("test-bot-fight-sim: human (EC)  speed 51.8 fast  63% | strafe  67% runs  367 ms  44.8 rev/min vert 0.33 thrust  98% across  77% | turns 180 in 1633 ms at 73%, rev 11% slide 86% push 74% (burn 26%) | ab 6.2% flee 21% roam 6%");
	std::puts("test-bot-fight-sim: bots before      speed 37-41 fast 24-36% | strafe 75-85% runs 367-433 ms 80-102 rev/min vert 0.71-1.0 thrust 75-79% across 45-48% | turns 180 in 1175-1457 ms at 80-99%, rev 24-45% slide 55-76% push 31-48% (burn 0-28%) | ab 0.1-2.3% flee 0-6% roam 0-3%");
}


/* One measurement and the range it must lie in. */
struct check
{
	std::string name;
	double value, lo, hi;
};

constexpr double ANY{std::numeric_limits<double>::infinity()};

/* Every measurement of the test, flown from seed `seed` on. */
std::vector<check> measure(const uint32_t seed, const bool print)
{
	std::vector<check> c;
	const auto add{[&c](std::string name, const double v, const double lo, const double hi) {
		c.push_back({std::move(name), v, lo, hi});
	}};
	const auto row{[print](const char *const name, const flight_result &r) {
		if (print)
			print_row(name, r);
	}};
	std::array<flight_result, BOT_SKILL_COUNT> by_skill;
	for (const auto skill : {bot_skill::trainee, bot_skill::rookie, bot_skill::hotshot, bot_skill::ace, bot_skill::insane})
	{
		auto &r{by_skill[static_cast<unsigned>(skill)]};
		r = fly({.skill = skill, .seed = seed});
		row(bot_skill_names[static_cast<unsigned>(skill)], r);
	}
	for (const auto style : {bot_style::aggressive, bot_style::cautious, bot_style::collector})
	{
		const auto r{fly({.skill = bot_skill::hotshot, .style = style, .seed = seed})};
		char name[32];
		std::snprintf(name, sizeof(name), "Hotshot %s", bot_style_names[static_cast<unsigned>(style)]);
		row(name, r);
	}
	/* Hotshot (the default) to Insane: near the human, away from the
	 * bots before.  Section 9.16: in the open the strafe reverses less
	 * than the human's (12-20 a minute: the juke pauses with forward
	 * flight and goes on the same way more often); on real levels the
	 * walls, the dodges and the paths bring it to his (the -botarena
	 * games of Corona and Earth Shaker: 50-52 a minute, EC 36-52).  The
	 * afterburner burns from nearer in a chase (the styles' distance) and
	 * on shorter straights (BOT_LONG_STRAIGHT 100).
	 */
	for (const auto skill : {bot_skill::hotshot, bot_skill::ace, bot_skill::insane})
	{
		const auto &s{by_skill[static_cast<unsigned>(skill)].stats};
		const std::string n{bot_skill_names[static_cast<unsigned>(skill)]};
		add(n + " speed", s.speed.mean, 45, 60);
		add(n + " fast_share", s.fast_share, 0.45, 0.8);
		add(n + " fight_strafe_share", s.fight_strafe_share, 0.55, 0.9);
		add(n + " strafe_reversals_per_min", s.strafe_reversals_per_min, 10, 65);
		add(n + " strafe_vertical", s.strafe_vertical, 0.15, 0.5);
		add(n + " strafe_thrust", s.strafe_thrust, 0.8, 1);
		add(n + " strafe_speed", s.strafe_speed, 0.62, 1);
		add(n + " slide_turn_share", s.slide_turn_share, 0.65, 1);
		add(n + " reverse_turn_share", s.reverse_turn_share, 0, 0.33);
		add(n + " turn_rate", s.turn_rate, 0.6, 0.94);
		add(n + " turn_180_ms", s.turn_180_ms.p50, 1200, 1800);
		add(n + " turn_boost_share", s.turn_boost_share, 0.35, 1);
		add(n + " ab_share", s.ab_share, 0.02, 0.15);
		add(n + " ab_flee_rate", s.ab_situation_rate[1], 0.065, 0.45);
		add(n + " ab_roam_rate", s.ab_situation_rate[2], 0.01, 0.2);
		add(n + " retreat_share", s.retreat_share, 0.1, 0.35);
	}
	/* The skills stay apart: a Trainee does not strafe or burn and turns
	 * slowest, an Insane bot turns fastest.
	 */
	{
		const auto &t{by_skill[0].stats}, &h{by_skill[2].stats}, &i{by_skill[4].stats};
		add("Trainee strafe_reversals_per_min", t.strafe_reversals_per_min, 0, 20);
		add("Trainee ab_share", t.ab_share, 0, 0);
		add("Trainee - Hotshot turn_180_ms", t.turn_180_ms.p50 - h.turn_180_ms.p50, 0, ANY);
		add("Hotshot - Insane turn_180_ms", h.turn_180_ms.p50 - i.turn_180_ms.p50, 0, ANY);
		add("Insane - Trainee speed", i.speed.mean - t.speed.mean, 0, ANY);
	}
	/* The same at another frame rate. */
	{
		const auto &ref{by_skill[2].stats};
		for (const double fps : {30.0, 144.0})
		{
			const auto r{fly({.skill = bot_skill::hotshot, .seed = seed, .fps = fps})};
			char name[32];
			std::snprintf(name, sizeof(name), "Hotshot %.0f fps", fps);
			row(name, r);
			const auto &s{r.stats};
			const std::string n{name};
			add(n + " - 60 fps speed", s.speed.mean - ref.speed.mean, -3, 3);
			add(n + " / 60 fps strafe_reversals_per_min", s.strafe_reversals_per_min / ref.strafe_reversals_per_min, 0.75, 1.25);
			add(n + " / 60 fps turn_180_ms", s.turn_180_ms.p50 / ref.turn_180_ms.p50, 0.9, 1.1);
		}
	}
	/* Section 9.13: a style profile flies as it says.  A profile of a
	 * pilot who turns backwards, bobs as much as he strafes and never
	 * burns fleeing, and one who fights far off, on a Hotshot bot.
	 */
	{
		const auto profile{parse_style_profile(
			"format = 1\n"
			"name = test style\n"
			"base_style = Balanced\n"
			"skill.strafe_vertical = 1\n"
			"tune.reverse_turn = 1\n"
			"tune.burn_retreat = 0\n")};
		CHECK(profile);
		const auto params{apply_style_profile(*profile, bot_skill::hotshot)};
		const auto r{fly({.skill = bot_skill::hotshot, .seed = seed, .params = &params})};
		row("test style", r);
		const auto &s{r.stats}, &h{by_skill[2].stats};
		add("test style strafe_vertical", s.strafe_vertical, 0.7, ANY);
		add("test style - Hotshot strafe_vertical", s.strafe_vertical - h.strafe_vertical, 0.3, ANY);
		/* Every large turn of its own backwards.  (The analysis counts a
		 * turn as reverse only with reverse thrust in 40 % of it; the
		 * bot's reverse turn holds its momentum away from the target,
		 * forward thrust while the nose points away, reverse only in
		 * its second half, so the analysis' share hardly moves:
		 * Documentation/multiplayer-bots.md section 9.13.)
		 */
		add("test style reversing turns per minute", r.turns_reversing * 60 / r.total_s, 1.25, ANY);
		add("test style sliding turns", r.turns_sliding, 0, 0);
		const auto &hr{by_skill[2]};
		/* A Hotshot's own large turns: mostly sliding. */
		add("Hotshot reversing share of own turns", hr.turns_reversing / std::max(1.0, static_cast<double>(hr.turns_reversing + hr.turns_sliding)), -ANY, 0.4);
		add("test style ab_flee_rate", s.ab_situation_rate[1], 0, 0.02);
	}
	{
		const auto profile{parse_style_profile(
			"format = 1\n"
			"name = far style\n"
			"tune.range_lo = 110\n"
			"tune.range_hi = 170\n")};
		CHECK(profile);
		const auto params{apply_style_profile(*profile, bot_skill::hotshot)};
		const auto r{fly({.skill = bot_skill::hotshot, .seed = seed, .params = &params})};
		row("far style", r);
		/* The scripted enemy closes in faster than a bot backs off:
		 * the bot keeps backing off.
		 */
		add("far style - Hotshot back_off_share", r.stats.back_off_share - by_skill[2].stats.back_off_share, 0.2, ANY);
	}
	return c;
}

/* `-survey N`: the test's measurements from N disjoint sets of seeds,
 * with each one's spread and its distance from its range in standard
 * deviations: how the ranges were checked (header comment).
 */
int survey(const unsigned n)
{
	std::vector<std::vector<check>> runs;
	for (unsigned k{}; k != n; ++k)
		runs.push_back(measure(1 + k * FLIGHT_SEEDS, false));
	unsigned outside{};
	double worst_margin{ANY};
	std::printf("%-48s %10s %9s %10s %10s %10s %10s %7s\n", "measurement", "mean", "sd", "min", "max", "lo", "hi", "margin");
	for (std::size_t i{}; i != runs.front().size(); ++i)
	{
		double sum{}, sq{}, lo_seen{ANY}, hi_seen{-ANY};
		for (const auto &r : runs)
		{
			const auto &m{r[i]};
			sum += m.value;
			sq += m.value * m.value;
			lo_seen = std::min(lo_seen, m.value);
			hi_seen = std::max(hi_seen, m.value);
			outside += !(m.value >= m.lo && m.value <= m.hi);
		}
		const auto &first{runs.front()[i]};
		const double mean{sum / n};
		const double sd{n > 1 ? std::sqrt(std::max(0.0, (sq - sum * mean) / (n - 1))) : 0};
		const double room{std::min(mean - first.lo, first.hi - mean)};
		const double margin{sd > 0 ? room / sd : (room >= 0 ? ANY : -ANY)};
		worst_margin = std::min(worst_margin, margin);
		std::printf("%-48s %10.4g %9.3g %10.4g %10.4g %10.4g %10.4g %7.1f\n", first.name.c_str(), mean, sd, lo_seen, hi_seen, first.lo, first.hi, margin);
	}
	std::printf("test-bot-fight-sim: survey of %u x %u seeds: %u measurements outside their range, smallest margin %.1f sd\n", n, FLIGHT_SEEDS, outside, worst_margin);
	return outside != 0;
}

}

int main(const int argc, char **const argv)
{
	for (int i{1}; i < argc; ++i)
		if (!std::strcmp(argv[i], "-v"))
			verbose = true;
	for (int i{1}; i + 1 < argc; ++i)
		if (!std::strcmp(argv[i], "-survey"))
			return survey(static_cast<unsigned>(std::max(2L, std::strtol(argv[i + 1], nullptr, 10))));
	print_reference();
	unsigned failed{};
	for (const auto &m : measure(1, true))
		if (!(m.value >= m.lo && m.value <= m.hi))
		{
			std::fprintf(stderr, "test-bot-fight-sim: %s = %g, expected %g to %g\n", m.name.c_str(), m.value, m.lo, m.hi);
			++failed;
		}
	if (failed)
		return 1;
	/* `-f FILE`: fly a style profile on a Hotshot bot. */
	for (int i{1}; i + 1 < argc; ++i)
		if (!std::strcmp(argv[i], "-f"))
		{
			std::FILE *const f{std::fopen(argv[i + 1], "rb")};
			if (!f)
				return 1;
			std::string text;
			for (int c; (c = std::fgetc(f)) != EOF && text.size() < STYLE_FILE_MAX_BYTES_SIM;)
				text += static_cast<char>(c);
			std::fclose(f);
			const auto profile{parse_style_profile(text)};
			if (!profile)
				return 1;
			for (const auto skill : {bot_skill::hotshot, bot_skill::insane})
			{
				const auto params{apply_style_profile(*profile, skill)};
				char name[48];
				std::snprintf(name, sizeof(name), "%.24s %s", profile->name.c_str(), bot_skill_names[static_cast<unsigned>(skill)]);
				print_row(name, fly({.skill = skill, .params = &params}));
			}
		}
	std::puts("test-bot-fight-sim: all checks passed");
	return 0;
}
