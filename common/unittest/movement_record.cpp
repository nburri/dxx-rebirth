/*
 * This file is part of the DXX-Rebirth project <https://github.com/dxx-rebirth/dxx-rebirth/>.
 * It is copyright by its individual contributors, as recorded in the
 * project's Git history.  See COPYING.txt at the top level for license
 * terms and a link to the Git history.
 */
/*
 * Test of the movement recording format (movement_record_format.h) and
 * reader (movement_record_reader.h): record round trips, the chunks, a
 * file cut short at any byte or damaged in a chunk, the tick schedule at
 * several frame rates, the quantisation.
 *
 * Build and run with SCons:
 *
 *	scons sdl2=1 d1x=0 d2x=1 register_runtime_test_plain_link_targets=1 test-movement-record
 *	build/common/test-movement-record
 */

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <vector>

#include "movement_record_reader.h"

using namespace dcx::movrec;

namespace {

void check_failed(const char *const expr, const char *const file, const int line)
{
	std::fprintf(stderr, "%s:%d: check failed: %s\n", file, line, expr);
	std::exit(1);
}

#define CHECK(cond)	do { if (!(cond)) check_failed(#cond, __FILE__, __LINE__); } while (0)

sample make_sample(const unsigned i, const bool with_controls)
{
	sample s;
	s.pid = static_cast<std::uint8_t>(i % 8);
	s.flags = sample_flag::alive | sample_flag::afterburner | (with_controls ? sample_flag::controls : 0);
	s.flags2 = sample_flag2::local | sample_flag2::afterburner_known;
	s.segment = static_cast<std::uint16_t>(900 + i);
	s.pos = {{-8388608 + static_cast<std::int32_t>(i), 8388607, -12345}};
	s.quat = {{32767, -1, 2, -32767}};
	s.vel = {{-32768, 32767, static_cast<std::int16_t>(i)}};
	s.rotvel = {{1, -2, 3}};
	s.weapons = 0x53;
	s.shields = 200;
	s.energy = 1;
	s.attacked_mask = 0x81;
	s.aimed_at_mask = 0x02;
	s.context = context_flag::kind_player | context_flag::line_of_sight | context_flag::in_my_cone;
	s.enemy_id = 3;
	s.enemy_rel_pos = {{-100, 200, -300}};
	s.enemy_rel_vel = {{4, -5, 6}};
	if (with_controls)
		s.controls = {{120, -60, 0, 127, -127, 1}};
	return s;
}

/* Encoded records decode to the same values. */
void test_record_round_trip()
{
	record_buffer buf;
	for (const bool ctl : {false, true})
	{
		const auto s{make_sample(5, ctl)};
		const auto bytes{encode(buf, s)};
		CHECK(bytes.size() == RECORD_HEADER_SIZE + SAMPLE_BASE_SIZE + (ctl ? SAMPLE_CONTROLS_SIZE : 0));
		CHECK(bytes[0] == static_cast<std::uint8_t>(record_type::sample));
		CHECK(bytes[1] == bytes.size() - RECORD_HEADER_SIZE);
		const auto d{decode_sample(bytes.subspan(RECORD_HEADER_SIZE))};
		CHECK(d);
		CHECK(*d == s);
		/* One byte short: rejected. */
		CHECK(!decode_sample(bytes.subspan(RECORD_HEADER_SIZE, bytes.size() - RECORD_HEADER_SIZE - 1)));
	}
	{
		const event_record e{record_type::hit, 123456789, 2, 5, attacker_kind::player, 9, 65535, hit_flag::applied_here};
		const auto bytes{encode(buf, e)};
		CHECK(bytes.size() == RECORD_HEADER_SIZE + EVENT_SIZE);
		const auto d{decode_event(record_type::hit, bytes.subspan(RECORD_HEADER_SIZE))};
		CHECK(d && *d == e);
	}
	{
		const tick_record t{4000000000u, 77};
		const auto bytes{encode(buf, t)};
		const auto d{decode_tick(bytes.subspan(RECORD_HEADER_SIZE))};
		CHECK(d && *d == t);
	}
	{
		const auto bytes{encode_level(buf, -3, 800, 0x1234, "Vertigo", "Level Name")};
		const auto d{decode_level(bytes.subspan(RECORD_HEADER_SIZE))};
		CHECK(d);
		CHECK(d->level_num == -3 && d->segments == 800 && d->game_mode == 0x1234);
		CHECK(d->mission == "Vertigo" && d->level_name == "Level Name");
	}
	{
		/* Long strings are cut to fit a record. */
		const std::string long_name(300, 'x');
		const auto bytes{encode_level(buf, 1, 1, 1, long_name, long_name)};
		CHECK(!bytes.empty());
		const auto d{decode_level(bytes.subspan(RECORD_HEADER_SIZE))};
		CHECK(d && d->mission.size() == 100 && d->level_name.size() == 100);
	}
	{
		const auto bytes{encode_player(buf, 4, player_flag::connected | player_flag::bot, 1, "Nico")};
		const auto d{decode_player(bytes.subspan(RECORD_HEADER_SIZE))};
		CHECK(d && d->pid == 4 && d->flags == (player_flag::connected | player_flag::bot) && d->team == 1 && d->callsign == "Nico");
	}
	{
		/* Trailing bytes of a later version are ignored. */
		const auto s{make_sample(1, false)};
		const auto bytes{encode(buf, s)};
		std::vector<std::uint8_t> longer(bytes.begin() + RECORD_HEADER_SIZE, bytes.end());
		longer.push_back(0xaa);
		longer.push_back(0xbb);
		const auto d{decode_sample(longer)};
		CHECK(d && *d == s);
	}
}

file_header make_header()
{
	file_header h;
	h.tick_rate = 30;
	h.flags = static_cast<std::uint16_t>(header_flag::multiplayer) | static_cast<std::uint16_t>(header_flag::host);
	h.start_unix_time = 1790000000;
	h.game_mode = 0x21;
	h.local_pid = 0;
	h.program = "D2X-Rebirth test";
	h.mission = "Descent 2: Counterstrike!";
	h.level_name = "Ascent";
	h.level_num = 1;
	h.num_players = 2;
	h.players[0] = {0, player_flag::connected | player_flag::local | player_flag::recorded, 0xff, "host"};
	h.players[1] = {1, player_flag::connected | player_flag::recorded, 0xff, "client"};
	return h;
}

void test_header_round_trip()
{
	std::array<std::uint8_t, MAX_HEADER_SIZE> buf;
	const auto h{make_header()};
	const auto n{encode_header(buf, h)};
	CHECK(n > 16);
	const auto d{decode_header(std::span<const std::uint8_t>(buf).first(n))};
	CHECK(d);
	CHECK(d->second == n);
	CHECK(d->first == h);
	/* A damaged header is rejected. */
	auto bad{buf};
	bad[20] ^= 1;
	CHECK(!decode_header(std::span<const std::uint8_t>(bad).first(n)));
	CHECK(!decode_header(std::span<const std::uint8_t>(buf).first(n - 1)));
	/* Too small an output buffer. */
	std::array<std::uint8_t, 20> small;
	CHECK(encode_header(small, h) == 0);
}

/* A file as the game writes it: header, then chunks of `per_chunk`
 * ticks, each tick a tick record, `players` samples and an event.
 */
struct test_file
{
	std::vector<std::uint8_t> bytes;
	std::vector<std::size_t> chunk_ends;
	unsigned ticks{};
	unsigned samples{};
	unsigned events{};
};

test_file build_file(const unsigned ticks, const unsigned players, const unsigned per_chunk, const bool clean_end)
{
	test_file f;
	std::array<std::uint8_t, MAX_HEADER_SIZE> hb;
	const auto n{encode_header(hb, make_header())};
	f.bytes.assign(hb.begin(), hb.begin() + static_cast<std::ptrdiff_t>(n));
	/* Heap: the builder's buffer is 32 KiB. */
	auto cb{std::make_unique<chunk_builder>()};
	record_buffer rb;
	const auto flush{[&]() {
		if (cb->empty())
			return;
		const auto c{cb->finish()};
		f.bytes.insert(f.bytes.end(), c.begin(), c.end());
		f.chunk_ends.push_back(f.bytes.size());
		cb->reset();
	}};
	const auto put{[&](const std::span<const std::uint8_t> r) {
		if (!cb->fits(r.size()))
			flush();
		CHECK(cb->append(r));
	}};
	put(encode_level(rb, 1, 100, 0, "m", "l"));
	for (unsigned t{}; t != ticks; ++t)
	{
		put(encode(rb, tick_record{t, t * 33}));
		++f.ticks;
		for (unsigned p{}; p != players; ++p)
		{
			auto s{make_sample(t, p == 0)};
			s.pid = static_cast<std::uint8_t>(p);
			put(encode(rb, s));
			++f.samples;
		}
		put(encode(rb, event_record{record_type::fire, t * 33, 0, PLAYER_NONE, fire_kind::primary, 1, 0, 0}));
		++f.events;
		if ((t + 1) % per_chunk == 0)
			flush();
	}
	if (clean_end)
	{
		put(encode(rb, event_record{record_type::end, ticks * 33, PLAYER_NONE, PLAYER_NONE, end_reason::closed, 0, 0, 0}));
		++f.events;
	}
	flush();
	return f;
}

struct counts
{
	unsigned ticks{}, samples{}, events{}, levels{};
	std::uint32_t last_tick{};
	bool ordered{true};
};

counts count_records(const std::span<const std::uint8_t> bytes, read_result &res)
{
	counts c;
	bool first{true};
	res = read_recording(bytes, [&c, &first](const record &r) {
		if (const auto t{std::get_if<tick_record>(&r)})
		{
			if (!first && t->tick != c.last_tick + 1)
				c.ordered = false;
			first = false;
			c.last_tick = t->tick;
			++c.ticks;
		}
		else if (std::holds_alternative<sample>(r))
			++c.samples;
		else if (std::holds_alternative<event_record>(r))
			++c.events;
		else if (std::holds_alternative<level_record>(r))
			++c.levels;
	});
	return c;
}

void test_whole_file()
{
	const auto f{build_file(300, 4, 30, true)};
	read_result res;
	const auto c{count_records(f.bytes, res)};
	CHECK(res.header);
	CHECK(res.header->mission == "Descent 2: Counterstrike!");
	CHECK(c.ticks == f.ticks && c.samples == f.samples && c.events == f.events && c.levels == 1);
	CHECK(c.ordered);
	CHECK(res.stats.clean_end);
	CHECK(!res.stats.truncated);
	CHECK(res.stats.chunks_bad == 0 && res.stats.sequence_gaps == 0 && res.stats.malformed_records == 0);
	CHECK(res.stats.chunks_ok == f.chunk_ends.size());
}

/* A big chunk: the builder flushes when full, records never span. */
void test_full_chunks()
{
	const auto f{build_file(2000, 8, 100000, false)};
	CHECK(f.chunk_ends.size() > 2);
	read_result res;
	const auto c{count_records(f.bytes, res)};
	CHECK(c.samples == f.samples && c.ticks == f.ticks && c.ordered);
	CHECK(!res.stats.clean_end);
	CHECK(res.stats.chunks_ok == f.chunk_ends.size());
}

/* The file cut at every byte of its last two chunks (a crash while
 * writing): everything in the complete chunks is read, nothing else, and
 * the reader reports the truncation.
 */
void test_truncation()
{
	const auto f{build_file(40, 2, 10, true)};
	/* Four chunks of 10 ticks, then one with the end record. */
	CHECK(f.chunk_ends.size() == 5);
	const auto header_end{decode_header(f.bytes)->second};
	for (std::size_t cut{header_end}; cut <= f.bytes.size(); ++cut)
	{
		read_result res;
		const auto c{count_records(std::span<const std::uint8_t>(f.bytes).first(cut), res)};
		CHECK(res.header);
		std::size_t whole{};
		while (whole < f.chunk_ends.size() && f.chunk_ends[whole] <= cut)
			++whole;
		CHECK(res.stats.chunks_ok == whole);
		CHECK(res.stats.chunks_bad == 0);
		const bool at_boundary{cut == header_end || (whole && f.chunk_ends[whole - 1] == cut)};
		CHECK(res.stats.truncated == !at_boundary);
		/* 10 ticks per chunk. */
		CHECK(c.ticks == std::min<std::size_t>(whole * 10, 40));
		CHECK(c.ordered);
		CHECK(res.stats.clean_end == (cut == f.bytes.size()));
	}
	/* A cut inside the header: nothing. */
	read_result res;
	count_records(std::span<const std::uint8_t>(f.bytes).first(header_end - 1), res);
	CHECK(!res.header);
}

/* A damaged byte in a chunk: that chunk is skipped, the rest is read. */
void test_damaged_chunk()
{
	auto f{build_file(40, 2, 10, true)};
	/* Inside the second chunk's payload. */
	f.bytes[f.chunk_ends[0] + CHUNK_HEADER_SIZE + 50] ^= 0x40;
	read_result res;
	const auto c{count_records(f.bytes, res)};
	CHECK(res.stats.chunks_ok == 4);
	CHECK(res.stats.chunks_bad == 1);
	CHECK(res.stats.sequence_gaps == 1);
	CHECK(c.ticks == 30);
	CHECK(res.stats.clean_end);
	CHECK(!res.stats.truncated);
	/* A damaged size field of the first chunk. */
	auto g{build_file(40, 2, 10, true)};
	const auto h_end{decode_header(g.bytes)->second};
	g.bytes[h_end + 9] ^= 0x7f;
	read_result res2;
	const auto c2{count_records(g.bytes, res2)};
	CHECK(res2.stats.chunks_ok == 4);
	CHECK(res2.stats.chunks_bad >= 1);
	CHECK(c2.ticks == 30);
	/* Garbage appended after a clean file: reported, records intact. */
	auto h{build_file(10, 1, 10, true)};
	h.bytes.insert(h.bytes.end(), {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20});
	read_result res3;
	const auto c3{count_records(h.bytes, res3)};
	CHECK(c3.ticks == 10 && res3.stats.chunks_ok == 2 && res3.stats.clean_end);
}

/* Unknown record types are skipped by their size. */
void test_unknown_records()
{
	std::array<std::uint8_t, MAX_HEADER_SIZE> hb;
	const auto n{encode_header(hb, make_header())};
	std::vector<std::uint8_t> bytes(hb.begin(), hb.begin() + static_cast<std::ptrdiff_t>(n));
	auto cb{std::make_unique<chunk_builder>()};
	record_buffer rb;
	const std::array<std::uint8_t, 5> unknown{{200, 3, 1, 2, 3}};
	CHECK(cb->append(unknown));
	CHECK(cb->append(encode(rb, tick_record{7, 8})));
	const auto c{cb->finish()};
	bytes.insert(bytes.end(), c.begin(), c.end());
	unsigned ticks{};
	const auto res{read_recording(bytes, [&ticks](const record &r) {
		if (std::holds_alternative<tick_record>(r))
			++ticks;
	})};
	CHECK(ticks == 1);
	CHECK(res.stats.unknown_records == 1);
	CHECK(res.stats.malformed_records == 0);
}

/* The tick schedule: one sample per tick at most, the same number of
 * samples over the same game time whatever the frame rate.
 */
void test_tick_scheduler()
{
	for (const unsigned fps : {20u, 30u, 60u, 144u, 200u, 500u, 1000u})
	{
		tick_scheduler sched{30};
		unsigned samples{};
		std::uint32_t last{};
		bool first{true};
		/* 10 seconds of frames. */
		const std::int32_t frame{static_cast<std::int32_t>(65536 / fps)};
		std::int64_t elapsed{};
		while (elapsed < std::int64_t{10} * 65536)
		{
			if (const auto t{sched.advance(frame)})
			{
				CHECK(first || *t > last);
				first = false;
				last = *t;
				++samples;
			}
			elapsed += frame;
		}
		if (fps >= 30)
			/* Every tick: 300 in 10 s (the frame length is rounded down). */
			CHECK(samples >= 299 && samples <= 301);
		else
		{
			/* Slower than the tick: one per frame, ticks skipped. */
			CHECK(samples >= 199 && samples <= 201);
			CHECK(sched.skipped_ticks() >= 95);
		}
		CHECK(last >= 298 && last <= 301);
	}
	{
		/* Paused: no time, no tick. */
		tick_scheduler sched{30};
		CHECK(sched.advance(65536 / 60));
		for (unsigned i{}; i != 100; ++i)
			CHECK(!sched.advance(0));
		CHECK(!sched.advance(-5));
		CHECK(sched.time_ms() == 16);
	}
	{
		/* The rate is clamped. */
		CHECK(tick_scheduler{1}.tick_rate() == MIN_TICK_RATE);
		CHECK(tick_scheduler{1000}.tick_rate() == MAX_TICK_RATE);
	}
	{
		/* Irregular frames: the ticks are those of the elapsed time. */
		tick_scheduler sched{60};
		const std::array<std::int32_t, 6> frames{{100, 65536 / 3, 7, 65536 / 500, 65536, 1}};
		std::int64_t time{};
		for (const auto f : frames)
		{
			time += f;
			const auto t{sched.advance(f)};
			if (t)
				CHECK(*t == static_cast<std::uint32_t>((time * 60) >> 16));
		}
	}
}

void test_quantisation()
{
	/* Positions: exact to 1/256 unit over the whole fix range. */
	CHECK(quantise_pos(65536) == 256);
	CHECK(quantise_pos(-65536) == -256);
	CHECK(quantise_pos(INT32_MAX) == 8388607);
	CHECK(quantise_pos(INT32_MIN) == -8388608);
	/* Velocities clamp. */
	CHECK(quantise_shift(100 * 65536, quant::VEL_SHIFT) == 6400);
	CHECK(quantise_shift(1000 * 65536, quant::VEL_SHIFT) == INT16_MAX);
	CHECK(quantise_shift(-1000 * 65536, quant::VEL_SHIFT) == INT16_MIN);
	/* Shields round up and clamp. */
	CHECK(quantise_whole_units(0) == 0);
	CHECK(quantise_whole_units(-5) == 0);
	CHECK(quantise_whole_units(1) == 1);
	CHECK(quantise_whole_units(100 * 65536) == 100);
	CHECK(quantise_whole_units(400 * 65536) == 255);
	CHECK(quantise_control(1.0) == 60);
	CHECK(quantise_control(-1.0) == -60);
	CHECK(quantise_control(2.0) == 120);
	CHECK(quantise_control(5.0) == 127);
	/* Orientation: the axes come back within the quantisation error,
	 * for rotations about every axis including the half turns.
	 */
	for (int i{}; i != 64; ++i)
	{
		const double a{i * 0.1}, b{i * 0.37 - 3}, c{i * 0.73};
		/* Rotation from three angles: R = Rz(c) Ry(b) Rx(a). */
		const double ca{std::cos(a)}, sa{std::sin(a)}, cb{std::cos(b)}, sb{std::sin(b)}, cc{std::cos(c)}, sc{std::sin(c)};
		const std::array<std::array<double, 3>, 3> m{{
			{{cc * cb, cc * sb * sa - sc * ca, cc * sb * ca + sc * sa}},
			{{sc * cb, sc * sb * sa + cc * ca, sc * sb * ca - cc * sa}},
			{{-sb, cb * sa, cb * ca}},
		}};
		const vec3 r{{m[0][0], m[1][0], m[2][0]}}, u{{m[0][1], m[1][1], m[2][1]}}, f{{m[0][2], m[1][2], m[2][2]}};
		const auto q{quantise_quaternion(quaternion_from_axes(r, u, f))};
		CHECK(q[0] >= 0);
		const auto ax{axes_from_quaternion({{q[0] / quant::QUAT_SCALE, q[1] / quant::QUAT_SCALE, q[2] / quant::QUAT_SCALE, q[3] / quant::QUAT_SCALE}})};
		for (std::size_t k{}; k != 3; ++k)
		{
			CHECK(std::fabs(ax.right[k] - r[k]) < 1e-3);
			CHECK(std::fabs(ax.up[k] - u[k]) < 1e-3);
			CHECK(std::fabs(ax.forward[k] - f[k]) < 1e-3);
		}
	}
	{
		/* Identity and a half turn about the up axis. */
		const auto q{quaternion_from_axes({{1, 0, 0}}, {{0, 1, 0}}, {{0, 0, 1}})};
		CHECK(std::fabs(q[0] - 1) < 1e-12);
		const auto h{quaternion_from_axes({{-1, 0, 0}}, {{0, 1, 0}}, {{0, 0, -1}})};
		const auto ax{axes_from_quaternion(h)};
		CHECK(std::fabs(ax.forward[2] + 1) < 1e-9 && std::fabs(ax.right[0] + 1) < 1e-9);
	}
	{
		/* The ship frame of the reader: moving backwards while strafing
		 * right, with the enemy ahead and to the left.
		 */
		sample s;
		s.quat = quantise_quaternion(quaternion_from_axes({{-1, 0, 0}}, {{0, 1, 0}}, {{0, 0, -1}}));
		s.vel = {{-64 * 10, 0, 64 * 20}};	/* world -x: right; world +z: backwards */
		s.context = context_flag::kind_player;
		s.enemy_rel_pos = {{16 * 30, 0, -16 * 40}};	/* world: ship's left, ahead */
		s.enemy_rel_vel = {{0, 0, 64 * 5}};		/* moving away along world +z: toward me? */
		const auto u{to_units(s)};
		CHECK(std::fabs(u.vel_ship[0] - 10) < 0.01);
		CHECK(std::fabs(u.vel_ship[2] + 20) < 0.01);
		CHECK(std::fabs(u.enemy_rel_pos_ship[0] + 30) < 0.01);
		CHECK(std::fabs(u.enemy_rel_pos_ship[2] - 40) < 0.01);
		CHECK(std::fabs(u.enemy_distance - 50) < 0.01);
		/* The enemy at -40 z relative, moving +z: closing. */
		CHECK(std::fabs(u.enemy_closing_speed - 4) < 0.01);
		CHECK(std::fabs(u.enemy_off_nose_deg - std::acos(0.8) * 180 / 3.14159265358979323846) < 0.01);
	}
}

void test_crc()
{
	/* The standard check value. */
	const std::array<std::uint8_t, 9> s{{'1', '2', '3', '4', '5', '6', '7', '8', '9'}};
	CHECK(crc32(s) == 0xcbf43926u);
	static_assert(crc32(std::span<const std::uint8_t>{}) == 0);
}

}

int main()
{
	test_crc();
	test_record_round_trip();
	test_header_round_trip();
	test_whole_file();
	test_full_chunks();
	test_truncation();
	test_damaged_chunk();
	test_unknown_records();
	test_tick_scheduler();
	test_quantisation();
	std::puts("test-movement-record: all checks passed");
	return 0;
}
