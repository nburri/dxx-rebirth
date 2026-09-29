/*
 * This file is part of the DXX-Rebirth project <https://github.com/dxx-rebirth/dxx-rebirth/>.
 * It is copyright by its individual contributors, as recorded in the
 * project's Git history.  See COPYING.txt at the top level for license
 * terms and a link to the Git history.
 */
/*
 * The file format of the movement recordings (-recordmoves,
 * Documentation/movement-recording.md): the byte layout of the file
 * header, the chunks and the records, their encoding and decoding, the
 * quantisation of the game's values and the recording tick schedule.
 *
 * The game writes with it (similar/main/movement_record.cpp), the reader
 * library (movement_record_reader.h) and the dump tool
 * (common/tools/movrec_dump.cpp) read with it.  Standard library only,
 * no allocation on the writing side (common/unittest/movement_record.cpp).
 *
 * All integers are little-endian.  A file is a header followed by chunks;
 * a chunk holds whole records and a CRC, so that a file cut short by a
 * crash loses only the chunk that was being written.
 */

#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>

namespace dcx::movrec {

/* "DXXMOVES" */
constexpr std::array<std::uint8_t, 8> FILE_MAGIC{{'D', 'X', 'X', 'M', 'O', 'V', 'E', 'S'}};
constexpr std::uint16_t FORMAT_VERSION{1};
/* "MRCK" as bytes in the file. */
constexpr std::uint32_t CHUNK_MAGIC{0x4b43524du};
/* magic, sequence, payload size, payload CRC-32 */
constexpr std::size_t CHUNK_HEADER_SIZE{16};
constexpr std::size_t CHUNK_SIZE{32 * 1024};
constexpr std::size_t MAX_CHUNK_PAYLOAD{CHUNK_SIZE - CHUNK_HEADER_SIZE};
/* A record: type, payload size, payload. */
constexpr std::size_t RECORD_HEADER_SIZE{2};
constexpr std::size_t MAX_RECORD_PAYLOAD{255};
constexpr std::size_t MAX_RECORD_SIZE{RECORD_HEADER_SIZE + MAX_RECORD_PAYLOAD};
constexpr std::size_t MAX_HEADER_SIZE{4096};
constexpr std::uint8_t PLAYER_NONE{0xff};
constexpr unsigned MAX_RECORDED_PLAYERS{8};

constexpr unsigned DEFAULT_TICK_RATE{30};
constexpr unsigned MIN_TICK_RATE{10};
constexpr unsigned MAX_TICK_RATE{60};

enum class record_type : std::uint8_t
{
	level = 1,
	player = 2,
	tick = 3,
	sample = 4,
	fire = 5,
	hit = 6,
	kill = 7,
	death = 8,
	respawn = 9,
	pickup = 10,
	weapon = 11,
	end = 12,
};

/* file_header::flags */
enum class header_flag : std::uint16_t
{
	multiplayer = 1 << 0,
	host = 1 << 1,
	bots_recorded = 1 << 2,
};

/* sample::flags */
namespace sample_flag {
constexpr std::uint8_t alive{1 << 0};
constexpr std::uint8_t dying{1 << 1};
constexpr std::uint8_t controls{1 << 2};		/* sample::controls is present */
constexpr std::uint8_t afterburner{1 << 3};
constexpr std::uint8_t fire_primary{1 << 4};	/* button held (local player only) */
constexpr std::uint8_t fire_secondary{1 << 5};	/* button held (local player only) */
constexpr std::uint8_t cloaked{1 << 6};
constexpr std::uint8_t invulnerable{1 << 7};
}

/* sample::flags2 */
namespace sample_flag2 {
constexpr std::uint8_t vitals_exact{1 << 0};	/* shields/energy are this machine's own */
constexpr std::uint8_t afterburner_known{1 << 1};
constexpr std::uint8_t bot{1 << 2};
constexpr std::uint8_t local{1 << 3};		/* flown on the recording machine */
constexpr std::uint8_t guided{1 << 4};		/* steering a guided missile */
constexpr std::uint8_t headlight{1 << 5};
constexpr std::uint8_t buttons_known{1 << 6};	/* fire_primary/fire_secondary are meaningful */
}

/* sample::context: bits 0-1 the kind of `enemy_id`, then flags. */
namespace context_flag {
constexpr std::uint8_t kind_mask{3};
constexpr std::uint8_t kind_none{0};
constexpr std::uint8_t kind_player{1};
constexpr std::uint8_t kind_robot{2};
constexpr std::uint8_t line_of_sight{1 << 2};
constexpr std::uint8_t in_my_cone{1 << 3};	/* the enemy is within 30 degrees of my nose */
constexpr std::uint8_t me_in_its_cone{1 << 4};	/* I am within 30 degrees of its nose */
constexpr std::uint8_t enemy_cloaked{1 << 5};
}

/* player_record::flags */
namespace player_flag {
constexpr std::uint8_t connected{1 << 0};
constexpr std::uint8_t bot{1 << 1};
constexpr std::uint8_t local{1 << 2};
constexpr std::uint8_t recorded{1 << 3};
}

/* event_record::kind for fire */
namespace fire_kind {
constexpr std::uint8_t primary{0};
constexpr std::uint8_t secondary{1};
constexpr std::uint8_t flare{2};
}

/* event_record::kind for hit and kill: what the attacker is */
namespace attacker_kind {
constexpr std::uint8_t none{0};
constexpr std::uint8_t player{1};
constexpr std::uint8_t robot{2};
constexpr std::uint8_t other{3};
}

/* event_record::flags for hit */
namespace hit_flag {
constexpr std::uint8_t applied_here{1 << 0};	/* this machine applies the damage */
}

/* event_record::kind for end */
namespace end_reason {
constexpr std::uint8_t closed{0};
constexpr std::uint8_t size_limit{1};
}

/* Quantisation.  The game's `fix` is 16.16 fixed point. */
namespace quant {
constexpr int POS_SHIFT{8};		/* 1/256 unit, i24 */
constexpr int VEL_SHIFT{10};		/* 1/64 unit/s, i16 */
constexpr int ROTVEL_SHIFT{4};		/* 1/4096 revolution/s, i16 */
constexpr int REL_POS_SHIFT{12};	/* 1/16 unit, i16 */
constexpr double QUAT_SCALE{32767.0};
constexpr double CONTROL_SCALE{60.0};	/* 1.0 = full deflection; up to 2.1 */
}

[[nodiscard]]
constexpr std::int16_t clamp_i16(const std::int32_t v)
{
	return static_cast<std::int16_t>(std::clamp<std::int32_t>(v, INT16_MIN, INT16_MAX));
}

[[nodiscard]]
constexpr std::int16_t quantise_shift(const std::int32_t fix_value, const int shift)
{
	return clamp_i16(fix_value >> shift);
}

[[nodiscard]]
constexpr std::int32_t quantise_pos(const std::int32_t fix_value)
{
	/* A 32 bit value shifted by 8 always fits 24 bits. */
	return fix_value >> quant::POS_SHIFT;
}

[[nodiscard]]
constexpr std::uint8_t quantise_whole_units(const std::int32_t fix_value)
{
	/* Round up, so that a ship with a fraction of a shield left shows 1. */
	if (fix_value <= 0)
		return 0;
	const std::int64_t units{(std::int64_t{fix_value} + 0xffff) >> 16};
	return static_cast<std::uint8_t>(std::min<std::int64_t>(units, 255));
}

[[nodiscard]]
inline std::int8_t quantise_control(const double v)
{
	const double q{std::round(v * quant::CONTROL_SCALE)};
	return static_cast<std::int8_t>(std::clamp(q, -127.0, 127.0));
}

/* The rotation as a unit quaternion (w, x, y, z), w >= 0, from the
 * orientation matrix whose rows are the object's right, up and forward
 * vectors in world coordinates (the game's vms_matrix rvec, uvec, fvec,
 * already converted from fix).  The quaternion rotates the object's local
 * axes (x right, y up, z forward) into those vectors.
 */
[[nodiscard]]
inline std::array<double, 4> quaternion_from_axes(const std::array<double, 3> &r, const std::array<double, 3> &u, const std::array<double, 3> &f)
{
	/* R has the columns r, u, f. */
	const double m00{r[0]}, m01{u[0]}, m02{f[0]};
	const double m10{r[1]}, m11{u[1]}, m12{f[1]};
	const double m20{r[2]}, m21{u[2]}, m22{f[2]};
	const double tr{m00 + m11 + m22};
	double w, x, y, z;
	if (tr > 0)
	{
		const double s{std::sqrt(tr + 1.0) * 2};
		w = 0.25 * s;
		x = (m21 - m12) / s;
		y = (m02 - m20) / s;
		z = (m10 - m01) / s;
	}
	else if (m00 > m11 && m00 > m22)
	{
		const double s{std::sqrt(1.0 + m00 - m11 - m22) * 2};
		w = (m21 - m12) / s;
		x = 0.25 * s;
		y = (m01 + m10) / s;
		z = (m02 + m20) / s;
	}
	else if (m11 > m22)
	{
		const double s{std::sqrt(1.0 + m11 - m00 - m22) * 2};
		w = (m02 - m20) / s;
		x = (m01 + m10) / s;
		y = 0.25 * s;
		z = (m12 + m21) / s;
	}
	else
	{
		const double s{std::sqrt(1.0 + m22 - m00 - m11) * 2};
		w = (m10 - m01) / s;
		x = (m02 + m20) / s;
		y = (m12 + m21) / s;
		z = 0.25 * s;
	}
	const double n{std::sqrt(w * w + x * x + y * y + z * z)};
	if (!(n > 0))
		return {{1, 0, 0, 0}};
	const double sign{w < 0 ? -1.0 : 1.0};
	return {{sign * w / n, sign * x / n, sign * y / n, sign * z / n}};
}

/* The inverse: the object's right, up and forward vectors. */
struct axes
{
	std::array<double, 3> right, up, forward;
};

[[nodiscard]]
inline axes axes_from_quaternion(std::array<double, 4> q)
{
	double n{std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3])};
	if (!(n > 0))
	{
		q = {{1, 0, 0, 0}};
		n = 1;
	}
	const double w{q[0] / n}, x{q[1] / n}, y{q[2] / n}, z{q[3] / n};
	return axes{
		{{1 - 2 * (y * y + z * z), 2 * (x * y + w * z), 2 * (x * z - w * y)}},
		{{2 * (x * y - w * z), 1 - 2 * (x * x + z * z), 2 * (y * z + w * x)}},
		{{2 * (x * z + w * y), 2 * (y * z - w * x), 1 - 2 * (x * x + y * y)}},
	};
}

[[nodiscard]]
inline std::array<std::int16_t, 4> quantise_quaternion(const std::array<double, 4> &q)
{
	std::array<std::int16_t, 4> r{};
	for (std::size_t i{}; i != 4; ++i)
		r[i] = static_cast<std::int16_t>(std::clamp(std::round(q[i] * quant::QUAT_SCALE), -32767.0, 32767.0));
	return r;
}

/* CRC-32 (IEEE 802.3, reflected, as zlib's crc32). */
[[nodiscard]]
constexpr std::uint32_t crc32(const std::span<const std::uint8_t> data, std::uint32_t crc = 0)
{
	crc = ~crc;
	for (const std::uint8_t b : data)
	{
		crc ^= b;
		for (unsigned k{}; k != 8; ++k)
			crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
	}
	return ~crc;
}

/* Writes little-endian values into a fixed span; remembers an overflow
 * instead of writing past the end.
 */
class byte_writer
{
	std::span<std::uint8_t> out;
	std::size_t pos{};
	bool overflow{};
public:
	explicit byte_writer(const std::span<std::uint8_t> o) :
		out{o}
	{
	}
	void u8(const std::uint8_t v)
	{
		if (pos >= out.size())
		{
			overflow = true;
			return;
		}
		out[pos++] = v;
	}
	void i8(const std::int8_t v)
	{
		u8(static_cast<std::uint8_t>(v));
	}
	void u16(const std::uint16_t v)
	{
		u8(static_cast<std::uint8_t>(v));
		u8(static_cast<std::uint8_t>(v >> 8));
	}
	void i16(const std::int16_t v)
	{
		u16(static_cast<std::uint16_t>(v));
	}
	void i24(const std::int32_t v)
	{
		const auto uv{static_cast<std::uint32_t>(v)};
		u8(static_cast<std::uint8_t>(uv));
		u8(static_cast<std::uint8_t>(uv >> 8));
		u8(static_cast<std::uint8_t>(uv >> 16));
	}
	void u32(const std::uint32_t v)
	{
		u16(static_cast<std::uint16_t>(v));
		u16(static_cast<std::uint16_t>(v >> 16));
	}
	void i64(const std::int64_t v)
	{
		const auto uv{static_cast<std::uint64_t>(v)};
		u32(static_cast<std::uint32_t>(uv));
		u32(static_cast<std::uint32_t>(uv >> 32));
	}
	/* A string of at most 255 bytes, longer ones cut. */
	void str8(const std::string_view s)
	{
		const std::size_t n{std::min<std::size_t>(s.size(), 255)};
		u8(static_cast<std::uint8_t>(n));
		for (std::size_t i{}; i != n; ++i)
			u8(static_cast<std::uint8_t>(s[i]));
	}
	void patch_u8(const std::size_t at, const std::uint8_t v)
	{
		if (at < pos)
			out[at] = v;
	}
	void patch_u32(const std::size_t at, const std::uint32_t v)
	{
		if (at + 4 <= pos)
			for (std::size_t i{}; i != 4; ++i)
				out[at + i] = static_cast<std::uint8_t>(v >> (8 * i));
	}
	[[nodiscard]]
	std::size_t size() const
	{
		return pos;
	}
	[[nodiscard]]
	bool ok() const
	{
		return !overflow;
	}
	[[nodiscard]]
	std::span<const std::uint8_t> written() const
	{
		return std::span<const std::uint8_t>(out).first(pos);
	}
};

/* Reads little-endian values; reads past the end give 0 and set fail. */
class byte_reader
{
	std::span<const std::uint8_t> in;
	std::size_t pos{};
	bool failed{};
public:
	explicit byte_reader(const std::span<const std::uint8_t> i) :
		in{i}
	{
	}
	std::uint8_t u8()
	{
		if (pos >= in.size())
		{
			failed = true;
			return 0;
		}
		return in[pos++];
	}
	std::int8_t i8()
	{
		return static_cast<std::int8_t>(u8());
	}
	std::uint16_t u16()
	{
		const std::uint16_t lo{u8()};
		const std::uint16_t hi{u8()};
		return static_cast<std::uint16_t>(lo | (hi << 8));
	}
	std::int16_t i16()
	{
		return static_cast<std::int16_t>(u16());
	}
	std::int32_t i24()
	{
		std::uint32_t v{u8()};
		v |= std::uint32_t{u8()} << 8;
		v |= std::uint32_t{u8()} << 16;
		if (v & 0x800000u)
			v |= 0xff000000u;
		return static_cast<std::int32_t>(v);
	}
	std::uint32_t u32()
	{
		const std::uint32_t lo{u16()};
		const std::uint32_t hi{u16()};
		return lo | (hi << 16);
	}
	std::int64_t i64()
	{
		const std::uint64_t lo{u32()};
		const std::uint64_t hi{u32()};
		return static_cast<std::int64_t>(lo | (hi << 32));
	}
	std::string str8()
	{
		const std::size_t n{u8()};
		std::string s;
		if (pos + n > in.size())
		{
			failed = true;
			pos = in.size();
			return s;
		}
		s.assign(reinterpret_cast<const char *>(in.data() + pos), n);
		pos += n;
		return s;
	}
	[[nodiscard]]
	std::span<const std::uint8_t> take(const std::size_t n)
	{
		if (pos + n > in.size())
		{
			failed = true;
			pos = in.size();
			return {};
		}
		const auto r{in.subspan(pos, n)};
		pos += n;
		return r;
	}
	[[nodiscard]]
	std::size_t position() const
	{
		return pos;
	}
	[[nodiscard]]
	std::size_t remaining() const
	{
		return in.size() - pos;
	}
	[[nodiscard]]
	bool ok() const
	{
		return !failed;
	}
};

/* One player's state at one recording tick.  The fields hold the
 * quantised values as they are stored (see `quant`).
 */
struct sample
{
	std::uint8_t pid{};
	std::uint8_t flags{};		/* sample_flag */
	std::uint8_t flags2{};		/* sample_flag2 */
	std::uint16_t segment{};
	std::array<std::int32_t, 3> pos{};	/* 1/256 unit */
	std::array<std::int16_t, 4> quat{{32767, 0, 0, 0}};	/* w x y z, 1.0 = 32767 */
	std::array<std::int16_t, 3> vel{};	/* world frame, 1/64 unit/s */
	std::array<std::int16_t, 3> rotvel{};	/* ship frame (pitch, heading, bank), 1/4096 rev/s */
	std::uint8_t weapons{};		/* primary | secondary << 4 */
	std::uint8_t shields{};		/* whole units */
	std::uint8_t energy{};		/* whole units */
	std::uint8_t attacked_mask{};	/* players whose shots hit me in the last 2 s */
	std::uint8_t aimed_at_mask{};	/* players aiming at me now */
	std::uint8_t context{};		/* context_flag */
	std::uint16_t enemy_id{};	/* player number or robot object number */
	std::array<std::int16_t, 3> enemy_rel_pos{};	/* enemy - me, world frame, 1/16 unit */
	std::array<std::int16_t, 3> enemy_rel_vel{};	/* enemy - me, world frame, 1/64 unit/s */
	/* forward, sideways (right +), vertical (up +), pitch, heading, bank:
	 * 1.0 = full deflection = 60; forward reaches 2.0 with afterburner.
	 * Present when flags has sample_flag::controls.
	 */
	std::array<std::int8_t, 6> controls{};
	constexpr bool operator==(const sample &) const = default;
};

constexpr std::size_t SAMPLE_BASE_SIZE{54};
constexpr std::size_t SAMPLE_CONTROLS_SIZE{6};

/* Events: one layout for all, the meaning of the fields by type
 * (Documentation/movement-recording.md section 3.4).
 */
struct event_record
{
	record_type type{};
	std::uint32_t time_ms{};
	std::uint8_t pid{PLAYER_NONE};
	std::uint8_t other{PLAYER_NONE};
	std::uint8_t kind{};
	std::uint8_t id{};
	std::uint16_t value{};
	std::uint8_t flags{};
	constexpr bool operator==(const event_record &) const = default;
};

constexpr std::size_t EVENT_SIZE{11};

struct tick_record
{
	std::uint32_t tick{};
	std::uint32_t time_ms{};
	constexpr bool operator==(const tick_record &) const = default;
};

struct level_record
{
	std::int8_t level_num{};
	std::uint16_t segments{};
	std::uint32_t game_mode{};
	std::string mission;
	std::string level_name;
	bool operator==(const level_record &) const = default;
};

struct player_record
{
	std::uint8_t pid{};
	std::uint8_t flags{};		/* player_flag */
	std::uint8_t team{};
	std::string callsign;
	bool operator==(const player_record &) const = default;
};

[[nodiscard]]
constexpr bool is_event(const record_type t)
{
	switch (t)
	{
		case record_type::fire:
		case record_type::hit:
		case record_type::kill:
		case record_type::death:
		case record_type::respawn:
		case record_type::pickup:
		case record_type::weapon:
		case record_type::end:
			return true;
		default:
			return false;
	}
}

/* A buffer for one record of any type. */
using record_buffer = std::array<std::uint8_t, MAX_RECORD_SIZE>;

namespace detail {

/* Write the type and a placeholder size; `finish_record` patches it. */
inline void begin_record(byte_writer &w, const record_type t)
{
	w.u8(static_cast<std::uint8_t>(t));
	w.u8(0);
}

[[nodiscard]]
inline std::span<const std::uint8_t> finish_record(byte_writer &w)
{
	if (!w.ok() || w.size() > MAX_RECORD_SIZE)
		return {};
	w.patch_u8(1, static_cast<std::uint8_t>(w.size() - RECORD_HEADER_SIZE));
	return w.written();
}

}

[[nodiscard]]
inline std::span<const std::uint8_t> encode(record_buffer &buf, const sample &s)
{
	byte_writer w{buf};
	detail::begin_record(w, record_type::sample);
	w.u8(s.pid);
	w.u8(s.flags);
	w.u8(s.flags2);
	w.u16(s.segment);
	for (const auto v : s.pos)
		w.i24(v);
	for (const auto v : s.quat)
		w.i16(v);
	for (const auto v : s.vel)
		w.i16(v);
	for (const auto v : s.rotvel)
		w.i16(v);
	w.u8(s.weapons);
	w.u8(s.shields);
	w.u8(s.energy);
	w.u8(s.attacked_mask);
	w.u8(s.aimed_at_mask);
	w.u8(s.context);
	w.u16(s.enemy_id);
	for (const auto v : s.enemy_rel_pos)
		w.i16(v);
	for (const auto v : s.enemy_rel_vel)
		w.i16(v);
	if (s.flags & sample_flag::controls)
		for (const auto v : s.controls)
			w.i8(v);
	return detail::finish_record(w);
}

[[nodiscard]]
inline std::span<const std::uint8_t> encode(record_buffer &buf, const event_record &e)
{
	byte_writer w{buf};
	detail::begin_record(w, e.type);
	w.u32(e.time_ms);
	w.u8(e.pid);
	w.u8(e.other);
	w.u8(e.kind);
	w.u8(e.id);
	w.u16(e.value);
	w.u8(e.flags);
	return detail::finish_record(w);
}

[[nodiscard]]
inline std::span<const std::uint8_t> encode(record_buffer &buf, const tick_record &t)
{
	byte_writer w{buf};
	detail::begin_record(w, record_type::tick);
	w.u32(t.tick);
	w.u32(t.time_ms);
	return detail::finish_record(w);
}

/* Strings are cut to fit the record (callsigns are short; mission and
 * level names at most 100 bytes each).
 */
[[nodiscard]]
inline std::span<const std::uint8_t> encode_level(record_buffer &buf, const std::int8_t level_num, const std::uint16_t segments, const std::uint32_t game_mode, const std::string_view mission, const std::string_view level_name)
{
	byte_writer w{buf};
	detail::begin_record(w, record_type::level);
	w.i8(level_num);
	w.u16(segments);
	w.u32(game_mode);
	w.str8(mission.substr(0, 100));
	w.str8(level_name.substr(0, 100));
	return detail::finish_record(w);
}

[[nodiscard]]
inline std::span<const std::uint8_t> encode_player(record_buffer &buf, const std::uint8_t pid, const std::uint8_t flags, const std::uint8_t team, const std::string_view callsign)
{
	byte_writer w{buf};
	detail::begin_record(w, record_type::player);
	w.u8(pid);
	w.u8(flags);
	w.u8(team);
	w.str8(callsign.substr(0, 32));
	return detail::finish_record(w);
}

/* Decoders take the record's payload.  They read the fields they know and
 * ignore any trailing bytes, which a later format version may add.
 */
[[nodiscard]]
inline std::optional<sample> decode_sample(const std::span<const std::uint8_t> payload)
{
	byte_reader r{payload};
	sample s;
	s.pid = r.u8();
	s.flags = r.u8();
	s.flags2 = r.u8();
	s.segment = r.u16();
	for (auto &v : s.pos)
		v = r.i24();
	for (auto &v : s.quat)
		v = r.i16();
	for (auto &v : s.vel)
		v = r.i16();
	for (auto &v : s.rotvel)
		v = r.i16();
	s.weapons = r.u8();
	s.shields = r.u8();
	s.energy = r.u8();
	s.attacked_mask = r.u8();
	s.aimed_at_mask = r.u8();
	s.context = r.u8();
	s.enemy_id = r.u16();
	for (auto &v : s.enemy_rel_pos)
		v = r.i16();
	for (auto &v : s.enemy_rel_vel)
		v = r.i16();
	if (s.flags & sample_flag::controls)
		for (auto &v : s.controls)
			v = r.i8();
	if (!r.ok())
		return std::nullopt;
	return s;
}

[[nodiscard]]
inline std::optional<event_record> decode_event(const record_type t, const std::span<const std::uint8_t> payload)
{
	byte_reader r{payload};
	event_record e;
	e.type = t;
	e.time_ms = r.u32();
	e.pid = r.u8();
	e.other = r.u8();
	e.kind = r.u8();
	e.id = r.u8();
	e.value = r.u16();
	e.flags = r.u8();
	if (!r.ok())
		return std::nullopt;
	return e;
}

[[nodiscard]]
inline std::optional<tick_record> decode_tick(const std::span<const std::uint8_t> payload)
{
	byte_reader r{payload};
	tick_record t;
	t.tick = r.u32();
	t.time_ms = r.u32();
	if (!r.ok())
		return std::nullopt;
	return t;
}

[[nodiscard]]
inline std::optional<level_record> decode_level(const std::span<const std::uint8_t> payload)
{
	byte_reader r{payload};
	level_record l;
	l.level_num = r.i8();
	l.segments = r.u16();
	l.game_mode = r.u32();
	l.mission = r.str8();
	l.level_name = r.str8();
	if (!r.ok())
		return std::nullopt;
	return l;
}

[[nodiscard]]
inline std::optional<player_record> decode_player(const std::span<const std::uint8_t> payload)
{
	byte_reader r{payload};
	player_record p;
	p.pid = r.u8();
	p.flags = r.u8();
	p.team = r.u8();
	p.callsign = r.str8();
	if (!r.ok())
		return std::nullopt;
	return p;
}

/* The file header.  The strings are copied once per session. */
struct file_header
{
	std::uint16_t version{FORMAT_VERSION};
	std::uint16_t tick_rate{DEFAULT_TICK_RATE};
	std::uint16_t flags{};		/* header_flag */
	std::int64_t start_unix_time{};
	std::uint32_t game_mode{};
	std::uint8_t local_pid{};
	std::string program;
	std::string mission;
	std::string level_name;
	std::int8_t level_num{};
	struct player
	{
		std::uint8_t pid{};
		std::uint8_t flags{};
		std::uint8_t team{};
		std::string callsign;
		bool operator==(const player &) const = default;
	};
	std::array<player, MAX_RECORDED_PLAYERS> players{};
	std::uint8_t num_players{};
	bool operator==(const file_header &) const = default;
};

/* Layout: magic (8), version u16, header size u16 (all bytes, CRC
 * included), tick rate u16, flags u16, start time i64, game mode u32,
 * local player u8, program str8, mission str8, level name str8, level
 * number i8, player count u8, per player {pid u8, flags u8, team u8,
 * callsign str8}, CRC-32 u32 of everything before it.  Returns the size,
 * 0 if `out` is too small.
 */
[[nodiscard]]
inline std::size_t encode_header(const std::span<std::uint8_t> out, const file_header &h)
{
	byte_writer w{out};
	for (const auto b : FILE_MAGIC)
		w.u8(b);
	w.u16(h.version);
	w.u16(0);
	w.u16(h.tick_rate);
	w.u16(h.flags);
	w.i64(h.start_unix_time);
	w.u32(h.game_mode);
	w.u8(h.local_pid);
	w.str8(h.program);
	w.str8(h.mission);
	w.str8(h.level_name);
	w.i8(h.level_num);
	const std::uint8_t n{std::min<std::uint8_t>(h.num_players, MAX_RECORDED_PLAYERS)};
	w.u8(n);
	for (std::size_t i{}; i != n; ++i)
	{
		const auto &p{h.players[i]};
		w.u8(p.pid);
		w.u8(p.flags);
		w.u8(p.team);
		w.str8(p.callsign);
	}
	const std::size_t total{w.size() + 4};
	if (!w.ok() || total > MAX_HEADER_SIZE || total > out.size())
		return 0;
	out[10] = static_cast<std::uint8_t>(total);
	out[11] = static_cast<std::uint8_t>(total >> 8);
	w.u32(crc32(out.first(total - 4)));
	return w.ok() ? total : 0;
}

/* The header at the start of `in` and its size, or nothing. */
[[nodiscard]]
inline std::optional<std::pair<file_header, std::size_t>> decode_header(const std::span<const std::uint8_t> in)
{
	if (in.size() < 16 || !std::equal(FILE_MAGIC.begin(), FILE_MAGIC.end(), in.begin()))
		return std::nullopt;
	byte_reader r{in};
	std::ignore = r.take(FILE_MAGIC.size());
	file_header h;
	h.version = r.u16();
	const std::size_t total{r.u16()};
	if (total < 16 || total > in.size())
		return std::nullopt;
	byte_reader crc_reader{in.subspan(total - 4, 4)};
	if (crc32(in.first(total - 4)) != crc_reader.u32())
		return std::nullopt;
	byte_reader b{in.first(total - 4)};
	std::ignore = b.take(12);
	h.tick_rate = b.u16();
	h.flags = b.u16();
	h.start_unix_time = b.i64();
	h.game_mode = b.u32();
	h.local_pid = b.u8();
	h.program = b.str8();
	h.mission = b.str8();
	h.level_name = b.str8();
	h.level_num = b.i8();
	const std::uint8_t n{b.u8()};
	if (n > MAX_RECORDED_PLAYERS)
		return std::nullopt;
	h.num_players = n;
	for (std::size_t i{}; i != n; ++i)
	{
		auto &p{h.players[i]};
		p.pid = b.u8();
		p.flags = b.u8();
		p.team = b.u8();
		p.callsign = b.str8();
	}
	if (!b.ok() || h.tick_rate == 0)
		return std::nullopt;
	return std::pair{std::move(h), total};
}

/* Collects records into one chunk: header, then the records.  Records
 * never span chunks.  No allocation.
 */
class chunk_builder
{
	std::array<std::uint8_t, CHUNK_SIZE> buf{};
	std::size_t used{CHUNK_HEADER_SIZE};
	std::uint32_t seq{};
public:
	[[nodiscard]]
	bool empty() const
	{
		return used == CHUNK_HEADER_SIZE;
	}
	[[nodiscard]]
	std::size_t payload_size() const
	{
		return used - CHUNK_HEADER_SIZE;
	}
	[[nodiscard]]
	bool fits(const std::size_t record_size) const
	{
		return used + record_size <= CHUNK_SIZE;
	}
	/* False if the record does not fit (flush first) or is empty. */
	bool append(const std::span<const std::uint8_t> record)
	{
		if (record.empty() || !fits(record.size()))
			return false;
		std::copy(record.begin(), record.end(), buf.begin() + static_cast<std::ptrdiff_t>(used));
		used += record.size();
		return true;
	}
	/* The finished chunk, header filled.  Call `reset` once written. */
	[[nodiscard]]
	std::span<const std::uint8_t> finish()
	{
		const auto payload{std::span<const std::uint8_t>(buf).subspan(CHUNK_HEADER_SIZE, payload_size())};
		byte_writer w{std::span<std::uint8_t>(buf).first(CHUNK_HEADER_SIZE)};
		w.u32(CHUNK_MAGIC);
		w.u32(seq);
		const std::size_t payload_bytes{payload.size()};
		w.u32(static_cast<std::uint32_t>(payload_bytes));
		w.u32(crc32(payload));
		return std::span<const std::uint8_t>(buf).first(used);
	}
	void reset()
	{
		used = CHUNK_HEADER_SIZE;
		++seq;
	}
	/* Start a new file: empty, sequence 0. */
	void restart()
	{
		used = CHUNK_HEADER_SIZE;
		seq = 0;
	}
	[[nodiscard]]
	std::uint32_t sequence() const
	{
		return seq;
	}
};

/* The recording tick schedule: tick k is due at game time k / rate
 * seconds after the session start.  The game calls `advance` once per
 * frame with the frame's duration (16.16 fixed point); it returns the
 * tick to sample when a new tick became due, at most one per frame.  A
 * frame longer than a tick period skips ticks rather than recording
 * duplicates, so the samples stay one per tick at most and the file size
 * does not depend on the frame rate.  Paused time (no frames, or frames
 * of duration 0) does not advance the clock.
 */
class tick_scheduler
{
	std::int64_t time_fix{};	/* 16.16 */
	std::uint32_t rate{DEFAULT_TICK_RATE};
	std::uint32_t next_tick{};
	std::uint32_t skipped{};
public:
	explicit tick_scheduler(const unsigned r = DEFAULT_TICK_RATE) :
		rate{std::clamp(r, MIN_TICK_RATE, MAX_TICK_RATE)}
	{
	}
	std::optional<std::uint32_t> advance(const std::int32_t frame_fix)
	{
		if (frame_fix <= 0)
			return std::nullopt;
		time_fix += frame_fix;
		const auto t{static_cast<std::uint32_t>((time_fix * rate) >> 16)};
		if (t < next_tick)
			return std::nullopt;
		skipped += t - next_tick;
		next_tick = t + 1;
		return t;
	}
	/* Milliseconds of game time since the session start. */
	[[nodiscard]]
	std::uint32_t time_ms() const
	{
		return static_cast<std::uint32_t>((time_fix * 1000) >> 16);
	}
	[[nodiscard]]
	std::uint32_t tick_rate() const
	{
		return rate;
	}
	[[nodiscard]]
	std::uint32_t skipped_ticks() const
	{
		return skipped;
	}
};

}
