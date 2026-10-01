/*
 * This file is part of the DXX-Rebirth project <https://github.com/dxx-rebirth/dxx-rebirth/>.
 * It is copyright by its individual contributors, as recorded in the
 * project's Git history.  See COPYING.txt at the top level for license
 * terms and a link to the Git history.
 */
/*
 * Test of the v2 state layouts (net_v2_state.h) and of the
 * game-independent interpolation, clock and tick math (net_interp.h, and
 * the transport's clock_sync and tick grants): quantisation, the STATE and
 * INPUT layouts, the snapshot ring under reordering, duplication and late
 * arrival, Hermite and nlerp interpolation, the extrapolation cap, the
 * discontinuity snaps, the interpolation delay estimator, the lag marker,
 * the tick accumulator at 30/60/120 Hz with frame times from 2 ms to
 * 100 ms, an end-to-end simulation of a remote ship shown at 500 fps
 * and at 30 fps over a lossy, jittery, reordering link, and the muzzle
 * flashes a remote ship carries along.
 *
 * Build and run with SCons:
 *
 *	scons sdl2=1 d1x=0 d2x=1 register_runtime_test_plain_link_targets=1 test-net-v2-interp
 *	build/common/test-net-v2-interp
 *
 * or directly:
 *
 *	g++ -std=gnu++23 -O2 -Wall -Wextra -Icommon/main common/unittest/net_v2_interp.cpp common/main/net_v2_transport.cpp -o test-net-v2-interp
 */

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <numbers>
#include <random>
#include <vector>

#include "net_interp.h"
#include "net_v2_transport.h"

using namespace dcx::net_v2;
using namespace dcx::net_interp;

namespace {

void check_failed(const char *const expr, const char *const file, const int line)
{
	std::fprintf(stderr, "%s:%d: check failed: %s\n", file, line, expr);
	std::exit(1);
}

#define CHECK(cond)	do { if (!(cond)) check_failed(#cond, __FILE__, __LINE__); } while (0)

constexpr std::int32_t F1{65536};

[[nodiscard]]
double units(const std::int64_t fixv)
{
	return static_cast<double>(fixv) / F1;
}

[[nodiscard]]
double dist(const net_vec &a, const net_vec &b)
{
	const double dx{units(a.x) - units(b.x)}, dy{units(a.y) - units(b.y)}, dz{units(a.z) - units(b.z)};
	return std::sqrt(dx * dx + dy * dy + dz * dz);
}

[[nodiscard]]
net_vec vec_units(const double x, const double y, const double z)
{
	return {static_cast<std::int32_t>(std::lround(x * F1)), static_cast<std::int32_t>(std::lround(y * F1)), static_cast<std::int32_t>(std::lround(z * F1))};
}

/* An engine quaternion (unit = 32767) for a rotation of `angle` about z. */
[[nodiscard]]
net_quat quat_z(const double angle)
{
	return {static_cast<std::int16_t>(std::lround(std::cos(angle / 2) * 32767)), 0, 0, static_cast<std::int16_t>(std::lround(std::sin(angle / 2) * 32767))};
}

[[nodiscard]]
double quat_angle_z(const net_quat &q)
{
	return 2 * std::atan2(static_cast<double>(q.z), static_cast<double>(q.w));
}

void test_quantisation()
{
	CHECK(quantise_i16(0, 10) == 0);
	CHECK(quantise_i16(511, 10) == 0);
	CHECK(quantise_i16(512, 10) == 1);
	CHECK(quantise_i16(-512, 10) == 0);
	CHECK(quantise_i16(-513, 10) == -1);
	CHECK(quantise_i16(1 << 30, 10) == 32767);
	CHECK(quantise_i16(-(1 << 30), 10) == -32768);
	/* Every velocity within range round-trips within half a step. */
	std::mt19937 rng{1};
	std::uniform_int_distribution<std::int32_t> d{-500 * F1, 500 * F1};
	for (unsigned i = 0; i < 100000; ++i)
	{
		const auto v{d(rng)};
		const auto r{dequantise_i16(quantise_i16(v, NET_V2_VELOCITY_SHIFT), NET_V2_VELOCITY_SHIFT)};
		CHECK(std::abs(r - v) <= 512);
		/* Re-encoding a received value is exact (the host relays). */
		CHECK(dequantise_i16(quantise_i16(r, NET_V2_VELOCITY_SHIFT), NET_V2_VELOCITY_SHIFT) == r);
	}
	CHECK(quantise_u16(-5, 8) == 0);
	CHECK(quantise_u16(200 * F1, 8) == 51200);
	CHECK(dequantise_u16(51200, 8) == 200 * F1);
	CHECK(quantise_u16(1 << 30, 8) == 65535);
	CHECK(quantise_ping(0) == 0);
	CHECK(quantise_ping(41) == 10);
	CHECK(quantise_ping(5000) == 255);
}

[[nodiscard]]
player_record make_record(const std::int32_t seed)
{
	player_record r;
	r.flags = flag_bit(player_record_flag::alive) | flag_bit(player_record_flag::afterburner);
	r.pose.orient = {1000, -2000, 3000, -4000};
	r.pose.pos = {seed * F1 + 17, -seed * 3 * F1 - 5, 123456789};
	r.pose.segment = static_cast<std::uint16_t>(seed + 100);
	r.pose.vel = quantised_velocity({seed * 40 * F1 + 999, -7 * F1, 3});
	r.pose.rotvel = quantised_rotvel({F1 / 3, -F1 / 7, 11});
	r.shields = dequantise_u16(quantise_u16(137 * F1 + 300, 8), 8);
	r.energy = dequantise_u16(quantise_u16(88 * F1, 8), 8);
	r.weapon = 0x35;
	r.input_age = 3;
	r.sample_age = static_cast<std::uint16_t>(1000 + seed);
	return r;
}

void test_state_layout()
{
	state_bundle s;
	s.tick = 0x01020304;
	s.host_time = 0xfffffff0u;
	s.level_time = 1234567;
	s.input_ack = 777;
	s.your_flags = flag_bit(state_flag::has_pings) | flag_bit(state_flag::countdown);
	s.players[0] = make_record(1);
	player_record ghost;
	ghost.flags = flag_bit(player_record_flag::ghost);
	s.players[3] = ghost;
	s.players[7] = make_record(7);
	s.n_guided = 2;
	s.guided[0] = {.pid = 0, .id = 55, .gen = 127, .orient = {1, 2, 3, 4}, .pos = {5, 6, 7}, .segment = 8, .vel = quantised_velocity({100 * F1, 0, -100 * F1})};
	s.guided[1] = {.pid = 7, .id = 9000, .gen = 3, .orient = {-1, -2, -3, -4}, .pos = {-5, -6, -7}, .segment = 9, .vel = {}};
	s.pings = {{0, 1, 2, 3, 4, 5, 6, 255}};
	std::array<std::uint8_t, NET_V2_MAX_STATE_SIZE> buf{};
	const auto n{write_state(buf, s)};
	CHECK(n == NET_V2_STATE_HEADER_SIZE + 2 * NET_V2_PLAYER_RECORD_SIZE + NET_V2_GHOST_RECORD_SIZE + 2 * NET_V2_GUIDED_RECORD_SIZE + NET_V2_PINGS_SIZE);
	const auto r{read_state(std::span(buf).first(n))};
	CHECK(r.has_value());
	CHECK(r->tick == s.tick && r->host_time == s.host_time && r->level_time == s.level_time && r->input_ack == s.input_ack && r->your_flags == s.your_flags);
	CHECK(r->players[0] == s.players[0]);
	CHECK(r->players[3] && r->players[3]->is_ghost());
	CHECK(r->players[7] == s.players[7]);
	CHECK(!r->players[1] && !r->players[2] && !r->players[4]);
	CHECK(r->n_guided == 2 && r->guided[0] == s.guided[0] && r->guided[1] == s.guided[1]);
	CHECK(r->pings == s.pings);
	/* Truncated or padded: rejected. */
	CHECK(!read_state(std::span(buf).first(n - 1)));
	CHECK(!read_state(std::span(buf).first(n + 1)));
	CHECK(!read_state(std::span(buf).first(NET_V2_STATE_HEADER_SIZE - 1)));
	/* Without pings, the trailer is gone. */
	s.your_flags = 0;
	const auto n2{write_state(buf, s)};
	CHECK(n2 == n - NET_V2_PINGS_SIZE);
	CHECK(read_state(std::span(buf).first(n2)).has_value());
	/* Counts beyond the limits and a guided pid beyond the slots. */
	{
		auto b2{buf};
		b2[16] = NET_V2_MAX_GUIDED_RECORDS + 1;
		CHECK(!read_state(std::span(b2).first(n2)));
		b2 = buf;
		b2[17] = NET_V2_MAX_ROBOT_RECORDS + 1;
		CHECK(!read_state(std::span(b2).first(n2)));
		b2 = buf;
		b2[NET_V2_STATE_HEADER_SIZE + 2 * NET_V2_PLAYER_RECORD_SIZE + NET_V2_GHOST_RECORD_SIZE] = NET_V2_MAX_PLAYERS;
		CHECK(!read_state(std::span(b2).first(n2)));
	}
	/* Robot records are skipped. */
	{
		std::vector<std::uint8_t> b3(buf.begin(), buf.begin() + n2);
		b3[17] = 2;
		b3.insert(b3.end(), 2 * NET_V2_ROBOT_RECORD_SIZE, 0xaa);
		const auto r3{read_state(b3)};
		CHECK(r3 && r3->n_robots == 2 && r3->players[7] == s.players[7]);
	}
	/* The largest bundle fits in one part. */
	{
		state_bundle big;
		for (unsigned i = 0; i < NET_V2_MAX_PLAYERS; ++i)
			big.players[i] = make_record(static_cast<std::int32_t>(i));
		big.n_guided = NET_V2_MAX_GUIDED_RECORDS;
		big.your_flags = flag_bit(state_flag::has_pings);
		const auto nb{write_state(buf, big)};
		CHECK(nb <= NET_V2_MAX_STATE_PART);
		CHECK(read_state(std::span(buf).first(nb)).has_value());
	}

	/* INPUT */
	input_chunk in;
	in.seq = 65535;
	in.sample_time = 0x80000001u;
	in.view_time = 0x7fffffffu;
	/* A ship in its death sequence is still alive (it exists), and dying. */
	in.flags = flag_bit(input_flag::alive) | flag_bit(input_flag::headlight) | flag_bit(input_flag::dying);
	in.pose = make_record(4).pose;
	in.weapon = 0x42;
	std::array<std::uint8_t, NET_V2_MAX_INPUT_SIZE> ib{};
	const auto ni{write_input(ib, in)};
	CHECK(ni == NET_V2_INPUT_SIZE);
	CHECK(read_input(std::span(ib).first(ni)) == in);
	CHECK(read_input(std::span(ib).first(ni))->has_flag(input_flag::dying));
	CHECK(!read_input(std::span(ib).first(ni - 1)));
	in.guided = guided_record{.pid = 2, .id = 300, .gen = 42, .orient = {7, 7, 7, 7}, .pos = {1, 2, 3}, .segment = 4, .vel = quantised_velocity({F1, F1, F1})};
	const auto ng{write_input(ib, in)};
	CHECK(ng == NET_V2_INPUT_SIZE + NET_V2_GUIDED_RECORD_SIZE);
	CHECK(NET_V2_GUIDED_RECORD_SIZE == 32 && ng == 78);
	/* The generation is the record's last byte. */
	CHECK(ib[ng - 1] == 42);
	const auto rg{read_input(std::span(ib).first(ng))};
	CHECK(rg && rg->has_flag(input_flag::has_guided) && rg->guided == in.guided && rg->pose == in.pose);
	/* The flag without the record, or the record without the flag. */
	CHECK(!read_input(std::span(ib).first(NET_V2_INPUT_SIZE)));
	ib[10] &= static_cast<std::uint8_t>(~flag_bit(input_flag::has_guided));
	CHECK(!read_input(std::span(ib).first(ng)));

	/* Protocol 107: the pilot's controls (-sharemoves), 6 bytes at the
	 * end, after the guided record if there is one.
	 */
	{
		input_chunk c;
		c.seq = 7;
		c.flags = flag_bit(input_flag::alive) | flag_bit(input_flag::afterburner);
		c.pose = make_record(1).pose;
		c.controls = ::dcx::movrec::control_array{{120, -60, 60, -1, 0, 59}};
		const auto nc{write_input(ib, c)};
		CHECK(nc == NET_V2_INPUT_SIZE + NET_V2_INPUT_CONTROLS_SIZE);
		CHECK(ib[10] & flag_bit(input_flag::controls));
		CHECK(static_cast<std::int8_t>(ib[NET_V2_INPUT_SIZE]) == 120 && static_cast<std::int8_t>(ib[nc - 1]) == 59);
		const auto rc{read_input(std::span(ib).first(nc))};
		CHECK(rc && rc->controls == c.controls && rc->pose == c.pose && !rc->guided);
		/* Cut short, or without the flag (6 bytes too many). */
		CHECK(!read_input(std::span(ib).first(nc - 1)));
		ib[10] &= static_cast<std::uint8_t>(~flag_bit(input_flag::controls));
		CHECK(!read_input(std::span(ib).first(nc)));
		/* The flag in `flags` follows `controls`. */
		c.flags |= flag_bit(input_flag::controls);
		c.controls.reset();
		CHECK(write_input(ib, c) == NET_V2_INPUT_SIZE && !(ib[10] & flag_bit(input_flag::controls)));
		/* With a guided record: the controls come last, and the largest
		 * INPUT fits the buffer.
		 */
		c.controls = ::dcx::movrec::control_array{{1, 2, 3, 4, 5, 6}};
		c.guided = in.guided;
		const auto nb{write_input(ib, c)};
		CHECK(nb == NET_V2_MAX_INPUT_SIZE && ib[nb - 1] == 6 && ib[nb - 7] == 42);
		const auto rb{read_input(std::span(ib).first(nb))};
		CHECK(rb && rb->guided == c.guided && rb->controls == c.controls);
		/* Untrusted values are clamped to what a pilot can give: forward
		 * -60..120, the others -60..60.
		 */
		for (std::size_t i{}; i != 6; ++i)
			ib[nb - 6 + i] = static_cast<std::uint8_t>(i % 2 ? 0x80 : 0x7f);
		const auto rx{read_input(std::span(ib).first(nb))};
		CHECK(rx && rx->controls);
		const ::dcx::movrec::control_array clamped{{120, -60, 60, -60, 60, -60}};
		CHECK(*rx->controls == clamped);
	}
}

[[nodiscard]]
snapshot snap(const host_clock t, const net_vec &pos, const net_vec &vel = {}, const std::uint16_t seg = 0)
{
	snapshot s;
	s.time = t;
	s.pos = pos;
	s.vel = vel;
	s.orient = quat_z(0);
	s.segment = seg;
	return s;
}

/* A guided missile whose object slot is reused by the owner's next
 * guided missile: the new missile's records (same `id`, next `gen`)
 * arrive before its fire message and must not drive the old copy; once
 * the fire message made the new copy, they drive it.
 */
void test_guided_identity()
{
	guided_identity who;
	snapshot_ring ring;
	CHECK(!who.describes(200, 1));
	/* Missile 1 in slot 200. */
	CHECK(who.receive(200, 1));
	ring.insert(snap(net_milliseconds(0), {0, 0, 0}));
	CHECK(!who.receive(200, 1));
	ring.insert(snap(net_milliseconds(16), {F1, 0, 0}));
	CHECK(who.describes(200, 1));
	CHECK(ring.size() == 2);
	/* Missile 2 reuses slot 200: its first record clears the ring... */
	if (who.receive(200, 2))
		ring.clear();
	ring.insert(snap(net_milliseconds(33), {100 * F1, 0, 0}));
	CHECK(ring.size() == 1);
	/* ...and does not describe the old copy (generation 1). */
	CHECK(!who.describes(200, 1));
	/* The fire message made the new copy, generation 2. */
	CHECK(who.describes(200, 2));
	/* Another slot is another missile too. */
	CHECK(who.receive(201, 2));
	CHECK(!who.describes(200, 2));
	/* The generation wraps with the modulo, still telling neighbours apart. */
	guided_identity w2;
	CHECK(w2.receive(5, static_cast<std::uint8_t>(NET_V2_GUIDED_GEN_MODULO - 1)));
	CHECK(w2.receive(5, 0));
	CHECK(NET_V2_GUIDED_GEN_MODULO <= 128);
	/* Inactive (the copy is gone): nothing is described, the next record
	 * starts afresh.
	 */
	w2.active = false;
	CHECK(!w2.describes(5, 0));
	CHECK(w2.receive(5, 0));
}

/* A remote ship's object collisions are swept along its path of the
 * frame, never across a jump.
 */
void test_sweepable_move()
{
	CHECK(is_sweepable_move({0, 0, 0}, {0, 0, 0}));
	CHECK(is_sweepable_move({0, 0, 0}, {3 * F1, 4 * F1, 0}));
	CHECK(is_sweepable_move({0, 0, 0}, {20 * F1, 0, 0}));
	CHECK(!is_sweepable_move({0, 0, 0}, {20 * F1, F1, 0}));
	CHECK(!is_sweepable_move({-1000 * F1, 0, 0}, {1000 * F1, 0, 0}));
}

/* The death sequence: the owner's records stay live (alive) while the
 * ship tumbles, so the receivers interpolate the tumble up to the newest
 * record, which is where the ship explodes (a ghost record or the deres
 * put it there); `alive` never changes during it, so nothing snaps.
 */
void test_death_tumble()
{
	snapshot_ring ring;
	const net_vec v{60 * F1, 0, 0};
	for (int k = 0; k <= 60; ++k)
	{
		auto s{snap(net_milliseconds(k * 50), {static_cast<std::int32_t>(k) * 3 * F1, 0, 0}, v)};
		s.orient = quat_z(k * 0.1);
		ring.insert(s);
	}
	const auto p{sample(ring, net_milliseconds(49 * 50 + 25))};
	CHECK(p && p->kind == pose_kind::interpolated && p->alive);
	CHECK(std::abs(units(p->pos.x) - 148.5) < 0.5);
	/* The ship's final pose is the newest snapshot. */
	CHECK(ring.newest().pos.x == 180 * F1);
}

void test_unwrap()
{
	CHECK(unwrap(10, 5) == 10);
	CHECK(unwrap(0xfffffff0u, 0x100000005ll) == 0xfffffff0ll);
	CHECK(unwrap(5, 0xfffffff0ll) == 0x100000005ll);
	CHECK(unwrap(0, 0) == 0);
}

void test_ring()
{
	snapshot_ring r;
	CHECK(r.empty());
	CHECK(!sample(r, 0));
	CHECK(r.insert(snap(100, {})) == insert_result::newest);
	CHECK(r.insert(snap(300, {})) == insert_result::newest);
	/* Reordered: goes in between. */
	CHECK(r.insert(snap(200, {})) == insert_result::inserted);
	/* Duplicate (a record the host relayed again unchanged). */
	CHECK(r.insert(snap(200, {1, 1, 1})) == insert_result::duplicate);
	CHECK(r.size() == 3 && r[0].time == 100 && r[1].time == 200 && r[2].time == 300 && r[1].pos.x == 0);
	/* Older than all, while not full: kept. */
	CHECK(r.insert(snap(50, {})) == insert_result::inserted);
	CHECK(r.oldest().time == 50);
	/* Fill: the oldest are dropped, in order. */
	for (host_clock t = 400; r.size() < NET_INTERP_RING_SIZE; t += 100)
		CHECK(r.insert(snap(t, {})) == insert_result::newest);
	CHECK(r.insert(snap(10000, {})) == insert_result::newest);
	CHECK(r.size() == NET_INTERP_RING_SIZE && r.oldest().time == 100 && r.newest().time == 10000);
	/* Older than everything in a full ring. */
	CHECK(r.insert(snap(60, {})) == insert_result::too_old);
	/* Late but inside: inserted, the oldest goes. */
	CHECK(r.insert(snap(150, {})) == insert_result::inserted);
	CHECK(r.oldest().time == 150 && r.size() == NET_INTERP_RING_SIZE);
	for (std::size_t i = 1; i < r.size(); ++i)
		CHECK(r[i - 1].time < r[i].time);
	r.clear();
	CHECK(r.empty());
}

void test_interpolation()
{
	const host_clock t0{net_seconds(10)};
	const net_clock tick{net_seconds(1) / 60};
	/* Straight line at 30 unit/s: Hermite with matching velocities is the
	 * line itself.
	 */
	{
		snapshot_ring r;
		const auto v{vec_units(30, 0, 0)};
		r.insert(snap(t0, vec_units(0, 0, 0), v, 1));
		r.insert(snap(t0 + tick, vec_units(0.5, 0, 0), v, 2));
		const auto p{sample(r, t0 + tick / 2)};
		CHECK(p && p->kind == pose_kind::interpolated);
		CHECK(dist(p->pos, vec_units(0.25, 0, 0)) < 1e-4);
		/* Segment: the nearer snapshot's first. */
		CHECK(p->segment == 2 && p->other_segment == 1);
		const auto q{sample(r, t0 + tick / 4)};
		CHECK(q->segment == 1 && q->other_segment == 2);
		/* Exactly on a snapshot. */
		CHECK(sample(r, t0)->pos == r[0].pos);
		CHECK(sample(r, t0 + tick)->pos == r[1].pos);
		/* Before the oldest: held there. */
		const auto h{sample(r, t0 - tick)};
		CHECK(h->kind == pose_kind::early && h->pos == r[0].pos);
	}
	/* A tight circle, 60 Hz snapshots: Hermite follows the arc far more
	 * closely than a straight line between the snapshots.
	 */
	{
		const double radius{10}, speed{60};
		const double omega{speed / radius};
		const auto truth{[&](const double t) {
			return vec_units(radius * std::cos(omega * t), radius * std::sin(omega * t), 0);
		}};
		const auto truth_vel{[&](const double t) {
			return vec_units(-radius * omega * std::sin(omega * t), radius * omega * std::cos(omega * t), 0);
		}};
		snapshot_ring r;
		for (unsigned i = 0; i < 16; ++i)
		{
			const double t{static_cast<double>(i) / 60};
			r.insert(snap(t0 + static_cast<host_clock>(i) * net_seconds(1) / 60, truth(t), quantised_velocity(truth_vel(t))));
		}
		double max_hermite{}, max_linear{};
		for (unsigned k = 0; k < 1000; ++k)
		{
			const double t{static_cast<double>(k) / 1000 * 15 / 60};
			const host_clock rt{t0 + static_cast<host_clock>(std::llround(t * F1))};
			const auto p{sample(r, rt)};
			CHECK(p && p->kind == pose_kind::interpolated);
			max_hermite = std::max(max_hermite, dist(p->pos, truth(units(rt - t0))));
			/* The linear alternative, for comparison. */
			std::size_t i{1};
			while (r[i].time < rt)
				++i;
			const double s{static_cast<double>(rt - r[i - 1].time) / static_cast<double>(r[i].time - r[i - 1].time)};
			const net_vec lin{
				static_cast<std::int32_t>(r[i - 1].pos.x + (r[i].pos.x - r[i - 1].pos.x) * s),
				static_cast<std::int32_t>(r[i - 1].pos.y + (r[i].pos.y - r[i - 1].pos.y) * s),
				0};
			max_linear = std::max(max_linear, dist(lin, truth(units(rt - t0))));
		}
		CHECK(max_hermite < 0.002);
		CHECK(max_linear > 5 * max_hermite);
	}
	/* Orientation: halfway between 0 and 20 degrees is 10; the shorter arc
	 * is taken when the second quaternion has the opposite sign.
	 */
	{
		snapshot_ring r;
		auto a{snap(t0, {})};
		auto b{snap(t0 + tick, {})};
		const double deg{std::numbers::pi / 180};
		a.orient = quat_z(0);
		b.orient = quat_z(20 * deg);
		r.insert(a);
		r.insert(b);
		CHECK(std::abs(quat_angle_z(sample(r, t0 + tick / 2)->orient) - 10 * deg) < 0.01 * deg);
		CHECK(std::abs(quat_angle_z(sample(r, t0 + tick / 4)->orient) - 5 * deg) < 0.2 * deg);
		snapshot_ring r2;
		b.orient = {static_cast<std::int16_t>(-b.orient.w), static_cast<std::int16_t>(-b.orient.x), static_cast<std::int16_t>(-b.orient.y), static_cast<std::int16_t>(-b.orient.z)};
		r2.insert(a);
		r2.insert(b);
		const auto q{sample(r2, t0 + tick / 2)->orient};
		CHECK(std::abs(quat_angle_z(q) - 10 * deg) < 0.01 * deg);
		/* Unit magnitude in engine units. */
		const double m{std::sqrt(static_cast<double>(q.w) * q.w + static_cast<double>(q.x) * q.x + static_cast<double>(q.y) * q.y + static_cast<double>(q.z) * q.z)};
		CHECK(std::abs(m - 32767) < 2);
	}
}

void test_extrapolation_and_loss()
{
	const host_clock t0{net_seconds(100)};
	const net_clock tick{net_seconds(1) / 60};
	const auto v{vec_units(40, -20, 10)};
	snapshot_ring r;
	r.insert(snap(t0, vec_units(0, 0, 0), v, 5));
	r.insert(snap(t0 + tick, vec_units(40.0 / 60, -20.0 / 60, 10.0 / 60), v, 6));
	/* 50 ms past the newest: constant velocity. */
	{
		const auto p{sample(r, t0 + tick + net_milliseconds(50))};
		CHECK(p->kind == pose_kind::extrapolated);
		const double t{units(tick + net_milliseconds(50))};
		CHECK(dist(p->pos, vec_units(40 * t, -20 * t, 10 * t)) < 1e-3);
		CHECK(p->vel == v && p->segment == 6);
	}
	/* Beyond the 100 ms cap: held where the cap put it, velocity zero. */
	for (const auto late : {net_milliseconds(101), net_milliseconds(150), net_seconds(5)})
	{
		const auto p{sample(r, t0 + tick + late)};
		CHECK(p->kind == pose_kind::stale);
		const double t{units(tick + NET_INTERP_EXTRAPOLATE_MAX)};
		CHECK(dist(p->pos, vec_units(40 * t, -20 * t, 10 * t)) < 1e-3);
		CHECK(p->vel == net_vec{});
	}
	/* A late snapshot arrives: interpolation resumes where it belongs. */
	r.insert(snap(t0 + 3 * tick, vec_units(40.0 * 3 / 60, -20.0 * 3 / 60, 10.0 * 3 / 60), v, 7));
	{
		const auto p{sample(r, t0 + 2 * tick)};
		CHECK(p->kind == pose_kind::interpolated);
		CHECK(dist(p->pos, vec_units(40.0 * 2 / 60, -20.0 * 2 / 60, 10.0 * 2 / 60)) < 1e-3);
	}
	/* A gap past NET_INTERP_HERMITE_MAX_GAP is interpolated linearly. */
	{
		snapshot_ring g;
		g.insert(snap(t0, vec_units(0, 0, 0), vec_units(0, 100, 0)));
		g.insert(snap(t0 + net_milliseconds(400), vec_units(10, 0, 0), vec_units(0, -100, 0)));
		const auto p{sample(g, t0 + net_milliseconds(200))};
		CHECK(dist(p->pos, vec_units(5, 0, 0)) < 1e-3);
	}
	/* Discontinuity: 25 units apart (respawn) — hold `a`, then `b` at its
	 * time, never anything in between.
	 */
	{
		snapshot_ring d;
		d.insert(snap(t0, vec_units(0, 0, 0)));
		d.insert(snap(t0 + tick, vec_units(25, 0, 0)));
		const auto p{sample(d, t0 + tick / 2)};
		CHECK(p->kind == pose_kind::held && p->pos == d[0].pos);
		CHECK(sample(d, t0 + tick)->pos == d[1].pos);
		CHECK(sample(d, t0 + tick + 1)->pos == d[1].pos);
	}
	/* A change of `alive` snaps the same way even without a jump. */
	{
		snapshot_ring d;
		auto a{snap(t0, vec_units(0, 0, 0))};
		auto b{snap(t0 + tick, vec_units(1, 0, 0))};
		b.alive = false;
		d.insert(a);
		d.insert(b);
		const auto p{sample(d, t0 + tick / 2)};
		CHECK(p->pos == a.pos && p->alive);
		CHECK(!sample(d, t0 + tick)->alive);
	}
}

/* Reordered, duplicated and late snapshots give the same poses as the
 * same snapshots delivered in order.
 */
void test_reorder_equivalence()
{
	std::mt19937 rng{7};
	const net_clock tick{net_seconds(1) / 60};
	std::vector<snapshot> all;
	for (unsigned i = 0; i < 12; ++i)
	{
		const double t{static_cast<double>(i) / 60};
		all.push_back(snap(static_cast<host_clock>(i) * tick, vec_units(std::sin(t * 3) * 20, t * 50, 0), quantised_velocity(vec_units(std::cos(t * 3) * 60, 50, 0))));
	}
	snapshot_ring ordered;
	for (const auto &s : all)
		ordered.insert(s);
	for (unsigned trial = 0; trial < 50; ++trial)
	{
		auto shuffled{all};
		std::shuffle(shuffled.begin(), shuffled.end(), rng);
		/* Duplicates of some. */
		shuffled.push_back(all[3]);
		shuffled.push_back(all[7]);
		snapshot_ring r;
		for (const auto &s : shuffled)
			r.insert(s);
		CHECK(r.size() == all.size());
		for (net_clock t = 0; t < 12 * tick; t += 97)
			CHECK(sample(r, t)->pos == sample(ordered, t)->pos);
	}
}

void test_delay_estimator()
{
	const net_clock tick{net_seconds(1) / 60};
	const net_clock base{2 * tick};
	/* A clean link: the base delay, 2 ticks (33 ms at 60 Hz). */
	{
		delay_estimator e{base};
		for (net_clock t = 0; t < net_seconds(5); t += tick)
		{
			e.add_sample(t, net_milliseconds(1));
			e.update(t);
		}
		CHECK(std::abs(e.delay() - (base + net_milliseconds(1))) <= 2);
	}
	/* 20 ms of uniform jitter: base + p90 (18 ms). */
	{
		std::mt19937 rng{3};
		std::uniform_int_distribution<net_clock> j{0, net_milliseconds(20)};
		delay_estimator e{base};
		for (net_clock t = 0; t < net_seconds(6); t += tick)
		{
			e.add_sample(t, j(rng));
			e.update(t);
		}
		CHECK(std::abs(e.delay() - (base + net_milliseconds(18))) < net_milliseconds(2));
	}
	/* Negative lateness (a snapshot ahead of the estimated host time)
	 * counts as none; a huge one is capped.
	 */
	{
		delay_estimator e{base};
		e.add_sample(0, -net_milliseconds(50));
		CHECK(e.delay() == base);
		delay_estimator big{base};
		big.add_sample(0, net_seconds(3));
		CHECK(big.delay() == base + NET_INTERP_LATENESS_MAX);
	}
	/* Adaptation: the jitter grows from 0 to 60 ms at t = 5 s.  The delay
	 * climbs at most NET_INTERP_DELAY_SLEW_PER_SECOND, and reaches the new
	 * target within a few seconds, the same at 500 fps and at 10 fps.
	 */
	{
		const auto run{[&](const net_clock frame) {
			std::mt19937 rng{11};
			std::uniform_int_distribution<net_clock> j{0, net_milliseconds(60)};
			delay_estimator e{base};
			std::vector<net_clock> at_100ms;
			net_clock next_sample{0};
			net_clock prev_delay{base};
			net_clock prev_t{0};
			for (net_clock t = 0; t <= net_seconds(12); t += frame)
			{
				for (; next_sample <= t; next_sample += tick)
					e.add_sample(next_sample, next_sample < net_seconds(5) ? 0 : j(rng));
				e.update(t);
				if (t > 0)
				{
					/* Never faster than the slew rate (plus rounding). */
					const auto step{std::abs(e.delay() - prev_delay)};
					CHECK(step <= (t - prev_t) * NET_INTERP_DELAY_SLEW_PER_SECOND / net_seconds(1) + 1);
				}
				prev_delay = e.delay();
				prev_t = t;
				if (t % net_milliseconds(100) < frame)
					at_100ms.push_back(e.delay());
			}
			CHECK(std::abs(e.delay() - (base + net_milliseconds(54))) < net_milliseconds(3));
			return at_100ms;
		}};
		const auto fast{run(net_milliseconds(2))};
		const auto slow{run(net_milliseconds(100))};
		const auto n{std::min(fast.size(), slow.size())};
		CHECK(n >= 100);
		for (std::size_t i = 0; i < n; ++i)
			/* The recomputation happens at the first frame after each
			 * second, up to one slow frame late: at most one frame of
			 * slew apart.
			 */
			CHECK(std::abs(fast[i] - slow[i]) <= net_milliseconds(7));
	}
	/* A tick rate change moves the base, not the margin. */
	{
		delay_estimator e{base};
		e.add_sample(0, net_milliseconds(10));
		const auto margin{e.delay() - base};
		e.set_base(2 * (net_seconds(1) / 120));
		CHECK(e.delay() == 2 * (net_seconds(1) / 120) + margin);
		e.reset(base);
		CHECK(e.delay() == base && !e.started());
	}
}

void test_lag_indicator()
{
	lag_indicator l;
	CHECK(!l.update(net_milliseconds(100)));
	CHECK(!l.update(NET_INTERP_LAG_ON));
	CHECK(l.update(NET_INTERP_LAG_ON + 1));
	/* Hysteresis: stays on between OFF and ON. */
	CHECK(l.update(net_milliseconds(220)));
	CHECK(l.update(NET_INTERP_LAG_OFF));
	CHECK(!l.update(NET_INTERP_LAG_OFF - 1));
	CHECK(!l.update(net_milliseconds(240)));
}

void test_tick_accumulator()
{
	std::mt19937 rng{5};
	for (const unsigned rate : {30u, 60u, 120u})
	{
		const std::vector<net_clock> frames{
			net_milliseconds(2), 131 /* 500 fps, rounded down */, 164, net_milliseconds(7),
			net_seconds(1) / 60, net_seconds(1) / 60 + 1, net_milliseconds(33), net_milliseconds(50), net_milliseconds(100),
		};
		for (const auto frame : frames)
		{
			tick_accumulator a{rate};
			a.reset(0);
			net_clock t{0};
			unsigned total{0};
			unsigned max_per_frame{0};
			for (; t < net_seconds(60); )
			{
				t += frame;
				const auto n{a.advance(t)};
				total += n;
				max_per_frame = std::max(max_per_frame, n);
			}
			/* Exact over a minute: no drift, no lost or extra tick (the
			 * one-unit tolerance may count the last one a unit early).
			 */
			const auto expected{static_cast<unsigned>((t * rate) / net_seconds(1))};
			CHECK(total >= expected && total <= expected + 1);
			CHECK(a.tick() == total);
			/* Never more ticks in a frame than it spans (rounded up). */
			CHECK(max_per_frame <= static_cast<unsigned>((frame * rate + net_seconds(1) - 1) / net_seconds(1)) + 1);
		}
		/* A true 60 Hz caller with an integer clock (floor(k / 60 s)) gets
		 * exactly one tick per call at 60 Hz, never 0 then 2.
		 */
		if (rate == 60)
		{
			tick_accumulator a{rate};
			a.reset(0);
			for (net_clock k = 1; k < 6000; ++k)
				CHECK(a.advance(k * net_seconds(1) / 60) == 1);
		}
		/* Random frame times between 2 and 100 ms. */
		{
			std::uniform_int_distribution<net_clock> d{net_milliseconds(2), net_milliseconds(100)};
			tick_accumulator a{rate};
			a.reset(0);
			net_clock t{0};
			unsigned total{0};
			while (t < net_seconds(120))
			{
				t += d(rng);
				total += a.advance(t);
			}
			const auto expected{static_cast<unsigned>((t * rate) / net_seconds(1))};
			CHECK(total >= expected && total <= expected + 1);
		}
	}
	/* A rate change keeps the phase. */
	{
		tick_accumulator a{60};
		a.reset(0);
		CHECK(a.advance(net_seconds(1) / 120) == 0);
		a.set_rate(120);
		CHECK(a.advance(net_seconds(1) / 120 + 1) == 1);
		CHECK(a.rate() == 120 && a.period() == (net_seconds(1) + 119) / 120);
	}
	/* Backwards or repeated times count nothing. */
	{
		tick_accumulator a{60};
		a.reset(net_seconds(5));
		CHECK(a.advance(net_seconds(4)) == 0);
		CHECK(a.advance(net_seconds(5)) == 0);
	}
}

/* The transport's per-connection tick grants (the packet pacing of each
 * connection) at the same rates: one grant per period at any frame rate
 * up to the period, at most two per call beyond it.
 */
void test_connection_ticks()
{
	for (const unsigned rate : {30u, 60u, 120u})
		for (const auto frame : {net_milliseconds(2), net_milliseconds(5), net_seconds(1) / 60, net_milliseconds(33), net_milliseconds(100)})
		{
			connection_config cfg;
			cfg.session_id = 1;
			cfg.peer_token = 2;
			cfg.local_player_id = 0;
			cfg.remote_player_id = 1;
			cfg.tick = {net_seconds(1), rate};
			connection c{cfg, 0};
			unsigned total{0};
			net_clock t{0};
			for (; t < net_seconds(10); )
			{
				t += frame;
				const auto n{c.begin_tick(t)};
				CHECK(n <= 2);
				total += n;
			}
			const auto expected{static_cast<unsigned>((t * rate) / net_seconds(1))};
			if (frame * rate <= net_seconds(1))
				CHECK(total + 1 >= expected && total <= expected + 1);
			else
				/* Frames longer than a period: at most two per frame. */
				CHECK(total <= 2 * static_cast<unsigned>(t / frame));
		}
}

/* §2.2: the estimated host clock under jitter, from the transport's
 * clock_sync fed the (rtt, offset) samples of a simulated link.
 */
void test_clock_offset()
{
	std::mt19937 rng{9};
	std::uniform_int_distribution<net_clock> jitter{0, net_milliseconds(20)};
	const net_clock base{net_milliseconds(40)};
	clock_sync cs;
	net_clock true_offset{net_seconds(12345)};
	for (net_clock now = 0; now < net_seconds(10); now += net_seconds(1) / 60)
	{
		if (now == net_seconds(6))
			/* The host's clock steps by 300 ms (a route change looks the
			 * same): followed within one window.
			 */
			true_offset += net_milliseconds(300);
		const auto up{base + jitter(rng)};
		const auto down{base + jitter(rng)};
		cs.add_sample(now, up + down, true_offset + (up - down) / 2);
		cs.update(now);
		if (now >= net_seconds(2) && now < net_seconds(6))
			CHECK(std::abs(cs.offset() - true_offset) < net_milliseconds(5));
		if (now >= net_seconds(8) + net_milliseconds(100))
			CHECK(std::abs(cs.offset() - true_offset) < net_milliseconds(5));
	}
}

/* The whole receiver pipeline: a host ship on a curved path, sampled at
 * the tick rate, sent over a link with 40 ms latency, 25 ms jitter, 5 %
 * loss, 5 % duplication and 10 % reordering; shown by a client at 500 fps
 * and at 30 fps from its own clock through the estimated offset and the
 * adaptive delay.  After the warm-up the shown ship is where the host's
 * ship was at the render time, render time never goes backwards, and at
 * 500 fps the ship never jumps further in a frame than it can fly.
 * A relayed ship (another client's, 80 ms older at the host) adapts its
 * own delay without touching the host ship's.
 */
void test_end_to_end()
{
	for (const unsigned rate : {30u, 60u, 120u})
		for (const auto frame : {net_milliseconds(2), net_seconds(1) / 30})
		{
			std::mt19937 rng{rate * 1000 + static_cast<unsigned>(frame)};
			std::uniform_real_distribution<double> u{0, 1};
			std::uniform_int_distribution<net_clock> jitter{0, net_milliseconds(25)};
			const net_clock latency{net_milliseconds(40)};
			const net_clock relay_age{net_milliseconds(80)};
			/* host clock = client clock + true_offset */
			const net_clock true_offset{net_seconds(1000) + 12345};
			const double radius{25}, speed{70};
			const auto truth{[&](const double t) {
				const double a{speed / radius * t};
				return vec_units(radius * std::cos(a), radius * std::sin(a), 3 * std::sin(t));
			}};
			const auto truth_vel{[&](const double t) {
				const double w{speed / radius};
				return vec_units(-radius * w * std::sin(w * t), radius * w * std::cos(w * t), 3 * std::cos(t));
			}};
			struct in_flight
			{
				net_clock arrive_local;
				host_clock time;
				bool relayed;
			};
			std::vector<in_flight> link;
			const net_clock tick_period{net_seconds(1) / rate};
			entity_track host_ship, relay_ship;
			host_ship.reset(tick_period);
			relay_ship.reset(tick_period);
			clock_sync cs;
			tick_accumulator ticks{rate};
			ticks.reset(true_offset);
			host_clock prev_render{0};
			net_vec prev_pos{};
			bool have_prev{};
			unsigned frames{0}, interpolated{0}, relay_interpolated{0};
			double max_err{0}, max_relay_err{0};
			const auto make{[&](const host_clock t) {
				const double s{units(t)};
				return snap(t, truth(s), quantised_velocity(truth_vel(s)));
			}};
			for (net_clock local = 0; local < net_seconds(20); local += frame)
			{
				const host_clock host_now{local + true_offset};
				/* The host sends one bundle per tick passed (the newest). */
				if (ticks.advance(host_now))
				{
					if (u(rng) >= 0.05)
					{
						const auto delay{latency + jitter(rng) + (u(rng) < 0.10 ? net_milliseconds(30) : 0)};
						link.push_back({local + delay, host_now, false});
						link.push_back({local + delay, host_now - relay_age - jitter(rng), true});
						if (u(rng) < 0.05)
						{
							link.push_back({local + delay + net_milliseconds(5), host_now, false});
						}
					}
					/* The clock sample of that packet. */
					const auto up{latency + jitter(rng)};
					const auto down{latency + jitter(rng)};
					cs.add_sample(local, up + down, true_offset + (up - down) / 2);
				}
				cs.update(local);
				const host_clock est_now{local + cs.offset()};
				for (auto it = link.begin(); it != link.end(); )
				{
					if (it->arrive_local > local)
					{
						++it;
						continue;
					}
					(it->relayed ? relay_ship : host_ship).receive(make(it->time), local, est_now);
					it = link.erase(it);
				}
				host_ship.delay.update(local);
				relay_ship.delay.update(local);
				if (host_ship.ring.empty() || relay_ship.ring.empty())
					continue;
				const host_clock render{host_ship.render_time(est_now)};
				const host_clock relay_render{relay_ship.render_time(est_now)};
				const auto p{sample(host_ship.ring, render)};
				const auto pr{sample(relay_ship.ring, relay_render)};
				CHECK(p && pr);
				if (local < net_seconds(4))
					continue;
				++frames;
				CHECK(render >= prev_render);
				prev_render = render;
				if (p->kind == pose_kind::interpolated)
					++interpolated;
				if (pr->kind == pose_kind::interpolated)
					++relay_interpolated;
				max_err = std::max(max_err, dist(p->pos, truth(units(render))));
				max_relay_err = std::max(max_relay_err, dist(pr->pos, truth(units(relay_render))));
				if (have_prev && frame == net_milliseconds(2))
					/* 70 unit/s is 0.14 unit per 2 ms frame; allow for
					 * the delay slew (6 %) and clock slew.
					 */
					CHECK(dist(p->pos, prev_pos) < 0.4);
				prev_pos = p->pos;
				have_prev = true;
			}
			CHECK(frames > 100);
			/* Interpolating nearly all the time: only a burst of loss
			 * forces a short extrapolation.
			 */
			CHECK(interpolated * 100 >= frames * 97);
			CHECK(relay_interpolated * 100 >= frames * 95);
			/* Within a small fraction of a unit of where the ship was. */
			CHECK(max_err < 0.35);
			CHECK(max_relay_err < 0.35);
			/* The relayed ship's delay covers its extra age; the host ship's
			 * does not carry it.
			 */
			CHECK(relay_ship.delay.delay() > host_ship.delay.delay() + net_milliseconds(60));
			/* The newest snapshot's age before the next arrives is one
			 * snapshot interval (a tick, or a 30 fps host frame) plus the
			 * one-way latency and jitter (40 to 65 ms, up to 95 ms for the
			 * 10 % reordered, which puts the 90th percentile among them):
			 * the host ship's delay covers that, without the relayed
			 * ship's extra age.
			 */
			const net_clock interval{std::max(tick_period, frame)};
			CHECK(host_ship.delay.delay() > tick_period + interval + latency);
			CHECK(host_ship.delay.delay() < tick_period + interval + latency + net_milliseconds(65));
		}
}

/* The axes (rows of the engine's orientation matrix) of a ship turned by
 * `yaw` about its up axis and then by `pitch` about its right axis.
 */
[[nodiscard]]
ship_axes axes_yaw_pitch(const double yaw, const double pitch)
{
	const double cy{std::cos(yaw)}, sy{std::sin(yaw)}, cp{std::cos(pitch)}, sp{std::sin(pitch)};
	/* Forward and right as given, up = forward x right, as the engine
	 * builds its matrices.
	 */
	const double f[3]{sy * cp, -sp, cy * cp};
	const double r[3]{cy, 0, -sy};
	const double u[3]{f[1] * r[2] - f[2] * r[1], f[2] * r[0] - f[0] * r[2], f[0] * r[1] - f[1] * r[0]};
	return {vec_units(r[0], r[1], r[2]), vec_units(u[0], u[1], u[2]), vec_units(f[0], f[1], f[2])};
}

[[nodiscard]]
net_vec add(const net_vec &a, const net_vec &b)
{
	return {a.x + b.x, a.y + b.y, a.z + b.z};
}

[[nodiscard]]
net_vec sub(const net_vec &a, const net_vec &b)
{
	return {a.x - b.x, a.y - b.y, a.z - b.z};
}

void test_carried_effects()
{
	/* The frame conversion: a gun point in the ship's frame goes to the
	 * world and back, and the axes map to the unit vectors.
	 */
	{
		const auto axes{axes_yaw_pitch(0.7, -0.3)};
		const auto gun{vec_units(2.2, -1.5, 4.8)};
		const auto world{from_ship_frame(axes, gun)};
		CHECK(std::abs(units(world.x) * units(world.x) + units(world.y) * units(world.y) + units(world.z) * units(world.z) - (2.2 * 2.2 + 1.5 * 1.5 + 4.8 * 4.8)) < 0.001);
		CHECK(dist(to_ship_frame(axes, world), gun) < 0.0005);
		CHECK(dist(from_ship_frame(axes, vec_units(0, 0, 1)), axes.fvec) < 0.0001);
		CHECK(dist(to_ship_frame(axes, axes.rvec), vec_units(1, 0, 0)) < 0.0001);
		const ship_axes identity{vec_units(1, 0, 0), vec_units(0, 1, 0), vec_units(0, 0, 1)};
		CHECK(from_ship_frame(identity, gun) == gun);
		CHECK(to_ship_frame(identity, gun) == gun);
	}
	/* The playtest case: a ship flying forward at 60 unit/s and turning,
	 * drawn at 500 fps.  Its flash is made at the gun where the ship was
	 * in the previous frame (the fire message is read before the frame's
	 * pose is written) and is carried each frame: it stays on the gun for
	 * its whole life, where a flash that stays put ends up far behind.
	 */
	{
		constexpr double speed{60}, frame{0.002}, life{0.3};
		const auto gun{vec_units(3.0, -1.0, 5.0)};
		const auto pose_at{[&](const double t) {
			const double yaw{0.5 * t};
			const auto axes{axes_yaw_pitch(yaw, 0.1)};
			const net_vec pos{add(vec_units(100, 20, -50), vec_units(speed * t * std::sin(yaw), 0, speed * t * std::cos(yaw)))};
			return std::pair{pos, axes};
		}};
		const auto [pos0, axes0] = pose_at(0);
		const net_vec flash0{add(pos0, from_ship_frame(axes0, gun))};
		carried_effects c;
		c.add({.object = 7, .signature = 42, .local = to_ship_frame(axes0, sub(flash0, pos0))});
		CHECK(c.size() == 1);
		net_vec flash{flash0};
		double worst{};
		for (double t = frame; t <= life; t += frame)
		{
			const auto [pos, axes] = pose_at(t);
			c.remove_if([&](const carried_effects::entry &e) {
				flash = add(pos, from_ship_frame(axes, e.local));
				return false;
			});
			worst = std::max(worst, dist(flash, add(pos, from_ship_frame(axes, gun))));
		}
		CHECK(worst < 0.002);
		const auto [pos_end, axes_end] = pose_at(life);
		CHECK(dist(flash0, add(pos_end, from_ship_frame(axes_end, gun))) > 15);
	}
	/* The list: a new object in a slot replaces the old entry, a full list
	 * drops its oldest entry, remove_if keeps the order.
	 */
	{
		carried_effects c;
		CHECK(c.empty());
		for (std::uint16_t i = 0; i < NET_INTERP_CARRIED_MAX; ++i)
			c.add({.object = i, .signature = 1, .local = {}});
		CHECK(c.size() == NET_INTERP_CARRIED_MAX);
		c.add({.object = 3, .signature = 2, .local = {}});
		CHECK(c.size() == NET_INTERP_CARRIED_MAX);
		CHECK(c[NET_INTERP_CARRIED_MAX - 1].object == 3 && c[NET_INTERP_CARRIED_MAX - 1].signature == 2);
		c.add({.object = 100, .signature = 1, .local = {}});
		CHECK(c.size() == NET_INTERP_CARRIED_MAX);
		CHECK(c[0].object == 1);
		CHECK(c[NET_INTERP_CARRIED_MAX - 1].object == 100);
		c.remove_if([](const carried_effects::entry &e) { return e.object % 2 == 0; });
		/* 0 was dropped, 3 moved to the end when its slot was reused. */
		const std::vector<std::uint16_t> expected{1, 5, 7, 9, 11, 13, 15, 3};
		CHECK(c.size() == expected.size());
		for (std::size_t i = 0; i < c.size(); ++i)
			CHECK(c[i].object == expected[i]);
		c.clear();
		CHECK(c.empty());
	}
}

}

int main()
{
	test_quantisation();
	test_state_layout();
	test_guided_identity();
	test_sweepable_move();
	test_death_tumble();
	test_unwrap();
	test_ring();
	test_interpolation();
	test_extrapolation_and_loss();
	test_reorder_equivalence();
	test_delay_estimator();
	test_lag_indicator();
	test_tick_accumulator();
	test_connection_ticks();
	test_clock_offset();
	test_end_to_end();
	test_carried_effects();
	std::puts("all tests passed");
	return 0;
}
