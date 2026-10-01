/*
 * This file is part of the DXX-Rebirth project <https://github.com/dxx-rebirth/dxx-rebirth/>.
 * It is copyright by its individual contributors, as recorded in the
 * project's Git history.  See COPYING.txt at the top level for license
 * terms and a link to the Git history.
 */
/*
 * Reader library of the movement recordings
 * (Documentation/movement-recording.md): walks a recording file, checks
 * every chunk's CRC, skips damaged chunks and a tail cut short by a
 * crash, decodes the records and converts the quantised values to game
 * units.  The base of the dump tool (common/tools/movrec_dump.cpp) and of
 * the analysis of step 2.
 *
 * Header-only, standard library only.
 */

#pragma once

#include <cmath>
#include <cstdio>
#include <optional>
#include <variant>
#include <vector>

#include "movement_record_format.h"

namespace dcx::movrec {

using record = std::variant<level_record, player_record, tick_record, sample, event_record, sync_record>;

struct read_stats
{
	std::size_t header_size{};
	std::uint32_t chunks_ok{};
	/* Chunks with a bad CRC or size, skipped; the reader resumes at the
	 * next chunk magic.
	 */
	std::uint32_t chunks_bad{};
	/* Chunks missing between two good ones (sequence numbers). */
	std::uint32_t sequence_gaps{};
	std::uint64_t records{};
	std::uint64_t unknown_records{};
	std::uint64_t malformed_records{};
	/* The file ends inside a chunk (the game stopped while writing). */
	bool truncated{};
	/* An `end` record was read: the game closed the file. */
	bool clean_end{};
};

struct read_result
{
	std::optional<file_header> header;
	read_stats stats;
};

namespace detail {

[[nodiscard]]
inline std::uint32_t le32(const std::span<const std::uint8_t> in, const std::size_t at)
{
	return std::uint32_t{in[at]} | (std::uint32_t{in[at + 1]} << 8) | (std::uint32_t{in[at + 2]} << 16) | (std::uint32_t{in[at + 3]} << 24);
}

/* The offset of the next chunk magic at or after `from`, or in.size(). */
[[nodiscard]]
inline std::size_t find_chunk_magic(const std::span<const std::uint8_t> in, std::size_t from)
{
	for (; from + 4 <= in.size(); ++from)
		if (le32(in, from) == CHUNK_MAGIC)
			return from;
	return in.size();
}

template <typename F>
void parse_payload(const std::span<const std::uint8_t> payload, read_stats &stats, F &on_record)
{
	byte_reader r{payload};
	while (r.remaining() >= RECORD_HEADER_SIZE)
	{
		const auto type{static_cast<record_type>(r.u8())};
		const std::size_t len{r.u8()};
		if (len > r.remaining())
		{
			++stats.malformed_records;
			return;
		}
		const auto body{r.take(len)};
		++stats.records;
		switch (type)
		{
			case record_type::sample:
				if (const auto s{decode_sample(body)})
					on_record(record{*s});
				else
					++stats.malformed_records;
				break;
			case record_type::tick:
				if (const auto t{decode_tick(body)})
					on_record(record{*t});
				else
					++stats.malformed_records;
				break;
			case record_type::sync:
				if (const auto y{decode_sync(body)})
					on_record(record{*y});
				else
					++stats.malformed_records;
				break;
			case record_type::level:
				if (auto l{decode_level(body)})
					on_record(record{std::move(*l)});
				else
					++stats.malformed_records;
				break;
			case record_type::player:
				if (auto p{decode_player(body)})
					on_record(record{std::move(*p)});
				else
					++stats.malformed_records;
				break;
			default:
				if (!is_event(type))
				{
					++stats.unknown_records;
					break;
				}
				if (const auto e{decode_event(type, body)})
				{
					if (type == record_type::end)
						stats.clean_end = true;
					on_record(record{*e});
				}
				else
					++stats.malformed_records;
				break;
		}
	}
	if (r.remaining())
		++stats.malformed_records;
}

}

/* Read a whole recording held in memory.  `on_record` is called with a
 * `const record &` for every record, in file order.
 */
template <typename F>
read_result read_recording(const std::span<const std::uint8_t> in, F &&on_record)
{
	read_result result;
	auto &stats{result.stats};
	auto h{decode_header(in)};
	if (!h)
		return result;
	result.header = std::move(h->first);
	stats.header_size = h->second;
	std::size_t pos{h->second};
	std::optional<std::uint32_t> expected_seq;
	while (pos < in.size())
	{
		if (pos + CHUNK_HEADER_SIZE > in.size())
		{
			stats.truncated = true;
			break;
		}
		if (detail::le32(in, pos) != CHUNK_MAGIC)
		{
			++stats.chunks_bad;
			pos = detail::find_chunk_magic(in, pos + 1);
			continue;
		}
		const auto seq{detail::le32(in, pos + 4)};
		const std::size_t len{detail::le32(in, pos + 8)};
		const auto crc{detail::le32(in, pos + 12)};
		if (len > MAX_CHUNK_PAYLOAD)
		{
			++stats.chunks_bad;
			pos = detail::find_chunk_magic(in, pos + 1);
			continue;
		}
		if (pos + CHUNK_HEADER_SIZE + len > in.size())
		{
			/* The last chunk, cut short; or a damaged size, if another
			 * chunk follows.
			 */
			const auto next{detail::find_chunk_magic(in, pos + 1)};
			if (next == in.size())
			{
				stats.truncated = true;
				break;
			}
			++stats.chunks_bad;
			pos = next;
			continue;
		}
		const auto payload{in.subspan(pos + CHUNK_HEADER_SIZE, len)};
		if (crc32(payload) != crc)
		{
			const auto next{detail::find_chunk_magic(in, pos + 1)};
			if (next == in.size() && pos + CHUNK_HEADER_SIZE + len == in.size())
				/* The last chunk with a partly written payload. */
				stats.truncated = true;
			else
				++stats.chunks_bad;
			pos = next;
			continue;
		}
		if (expected_seq && seq != *expected_seq && seq > *expected_seq)
			stats.sequence_gaps += seq - *expected_seq;
		expected_seq = seq + 1;
		++stats.chunks_ok;
		detail::parse_payload(payload, stats, on_record);
		pos += CHUNK_HEADER_SIZE + len;
	}
	return result;
}

/* The whole file, or nothing if it cannot be read. */
[[nodiscard]]
inline std::optional<std::vector<std::uint8_t>> load_file(const char *const path)
{
	std::FILE *const f{std::fopen(path, "rb")};
	if (!f)
		return std::nullopt;
	std::vector<std::uint8_t> data;
	std::array<std::uint8_t, 65536> buf;
	for (;;)
	{
		const auto n{std::fread(buf.data(), 1, buf.size(), f)};
		data.insert(data.end(), buf.begin(), buf.begin() + static_cast<std::ptrdiff_t>(n));
		if (n < buf.size())
			break;
	}
	const bool err{std::ferror(f) != 0};
	std::fclose(f);
	if (err)
		return std::nullopt;
	return data;
}

/* Conversions to game units (unit = the game's distance unit; a ship is
 * about 5 units across).
 */
using vec3 = std::array<double, 3>;

[[nodiscard]]
inline double dot(const vec3 &a, const vec3 &b)
{
	return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

[[nodiscard]]
inline double length(const vec3 &a)
{
	return std::sqrt(dot(a, a));
}

/* A vector given in world coordinates, in the ship's frame: (right, up,
 * forward).
 */
[[nodiscard]]
inline vec3 to_ship_frame(const axes &ax, const vec3 &v)
{
	return {{dot(v, ax.right), dot(v, ax.up), dot(v, ax.forward)}};
}

struct sample_units
{
	vec3 pos;			/* units */
	axes orient;
	vec3 vel;			/* world, units/s */
	vec3 vel_ship;			/* right, up, forward, units/s */
	double speed;
	vec3 rotvel;			/* pitch, heading, bank, revolutions/s */
	/* forward, sideways, vertical, pitch, heading, bank; 1 = full */
	std::array<double, 6> controls;
	bool has_enemy;
	vec3 enemy_rel_pos;		/* world */
	vec3 enemy_rel_pos_ship;	/* right, up, forward */
	vec3 enemy_rel_vel;		/* world */
	double enemy_distance;
	/* The relative position (velocity) was scaled down to fit (minor 1,
	 * context_flag::rel_pos_scaled): the direction is right, the length
	 * a lower bound (2048 units and more).
	 */
	bool enemy_distance_scaled;
	bool enemy_rel_vel_scaled;
	/* Positive when the enemy comes closer. */
	double enemy_closing_speed;
	unsigned primary, secondary;
	/* Degrees between the nose and the enemy. */
	double enemy_off_nose_deg;
};

[[nodiscard]]
inline sample_units to_units(const sample &s)
{
	sample_units u{};
	for (std::size_t i{}; i != 3; ++i)
	{
		u.pos[i] = s.pos[i] / 256.0;
		u.vel[i] = s.vel[i] / 64.0;
		u.rotvel[i] = s.rotvel[i] / 4096.0;
		u.enemy_rel_pos[i] = s.enemy_rel_pos[i] / 16.0;
		u.enemy_rel_vel[i] = s.enemy_rel_vel[i] / 64.0;
	}
	u.orient = axes_from_quaternion({{s.quat[0] / quant::QUAT_SCALE, s.quat[1] / quant::QUAT_SCALE, s.quat[2] / quant::QUAT_SCALE, s.quat[3] / quant::QUAT_SCALE}});
	u.vel_ship = to_ship_frame(u.orient, u.vel);
	u.speed = length(u.vel);
	for (std::size_t i{}; i != 6; ++i)
		u.controls[i] = s.controls[i] / quant::CONTROL_SCALE;
	u.has_enemy = (s.context & context_flag::kind_mask) != context_flag::kind_none;
	u.enemy_rel_pos_ship = to_ship_frame(u.orient, u.enemy_rel_pos);
	u.enemy_distance = length(u.enemy_rel_pos);
	u.enemy_distance_scaled = (s.context & context_flag::rel_pos_scaled) != 0;
	u.enemy_rel_vel_scaled = (s.context & context_flag::rel_vel_scaled) != 0;
	if (u.has_enemy && u.enemy_distance > 0)
	{
		u.enemy_closing_speed = -dot(u.enemy_rel_vel, u.enemy_rel_pos) / u.enemy_distance;
		const double c{std::clamp(u.enemy_rel_pos_ship[2] / u.enemy_distance, -1.0, 1.0)};
		u.enemy_off_nose_deg = std::acos(c) * 180.0 / 3.14159265358979323846;
	}
	u.primary = s.weapons & 0x0fu;
	u.secondary = static_cast<unsigned>(s.weapons >> 4);
	return u;
}

[[nodiscard]]
inline const char *record_type_name(const record_type t)
{
	switch (t)
	{
		case record_type::level: return "level";
		case record_type::player: return "player";
		case record_type::tick: return "tick";
		case record_type::sample: return "sample";
		case record_type::fire: return "fire";
		case record_type::hit: return "hit";
		case record_type::kill: return "kill";
		case record_type::death: return "death";
		case record_type::respawn: return "respawn";
		case record_type::pickup: return "pickup";
		case record_type::weapon: return "weapon";
		case record_type::end: return "end";
		case record_type::sync: return "sync";
	}
	return "unknown";
}

/* CSV of a player's samples, one row per tick. */
inline void write_sample_csv_header(std::FILE *const f)
{
	std::fputs("tick,time_s,level,pid,alive,dying,segment,x,y,z,"
		"fwd_x,fwd_y,fwd_z,up_x,up_y,up_z,"
		"vx,vy,vz,speed,v_right,v_up,v_fwd,"
		"rot_pitch,rot_heading,rot_bank,"
		"controls_valid,thrust_fwd,thrust_side,thrust_vert,ctl_pitch,ctl_heading,ctl_bank,"
		"afterburner,afterburner_known,fire_primary,fire_secondary,cloaked,invulnerable,guided,"
		"primary,secondary,shields,energy,vitals_exact,"
		"enemy_kind,enemy_id,enemy_los,enemy_in_my_cone,me_in_enemy_cone,enemy_dist,"
		"enemy_right,enemy_up,enemy_ahead,enemy_off_nose_deg,enemy_closing_speed,"
		"attacked_mask,aimed_at_mask,controls_shared\n", f);
}

inline void write_sample_csv_row(std::FILE *const f, const tick_record &t, const int level, const sample &s)
{
	const auto u{to_units(s)};
	const auto fl{[&s](const std::uint8_t bit) { return (s.flags & bit) ? 1 : 0; }};
	const auto fl2{[&s](const std::uint8_t bit) { return (s.flags2 & bit) ? 1 : 0; }};
	const auto ctx{[&s](const std::uint8_t bit) { return (s.context & bit) ? 1 : 0; }};
	const bool ctl{(s.flags & sample_flag::controls) != 0};
	std::fprintf(f, "%u,%.3f,%d,%u,%d,%d,%u,%.3f,%.3f,%.3f,"
		"%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,"
		"%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,"
		"%.4f,%.4f,%.4f,",
		t.tick, t.time_ms / 1000.0, level, s.pid, fl(sample_flag::alive), fl(sample_flag::dying), s.segment, u.pos[0], u.pos[1], u.pos[2],
		u.orient.forward[0], u.orient.forward[1], u.orient.forward[2], u.orient.up[0], u.orient.up[1], u.orient.up[2],
		u.vel[0], u.vel[1], u.vel[2], u.speed, u.vel_ship[0], u.vel_ship[1], u.vel_ship[2],
		u.rotvel[0], u.rotvel[1], u.rotvel[2]);
	if (ctl)
		std::fprintf(f, "1,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,", u.controls[0], u.controls[1], u.controls[2], u.controls[3], u.controls[4], u.controls[5]);
	else
		std::fputs("0,,,,,,,", f);
	std::fprintf(f, "%d,%d,%d,%d,%d,%d,%d,%u,%u,%u,%u,%d,",
		fl(sample_flag::afterburner), fl2(sample_flag2::afterburner_known), fl(sample_flag::fire_primary), fl(sample_flag::fire_secondary), fl(sample_flag::cloaked), fl(sample_flag::invulnerable), fl2(sample_flag2::guided),
		u.primary, u.secondary, s.shields, s.energy, fl2(sample_flag2::vitals_exact));
	if (u.has_enemy)
		std::fprintf(f, "%u,%u,%d,%d,%d,%.3f,%.3f,%.3f,%.3f,%.2f,%.3f,",
			s.context & context_flag::kind_mask, s.enemy_id, ctx(context_flag::line_of_sight), ctx(context_flag::in_my_cone), ctx(context_flag::me_in_its_cone), u.enemy_distance,
			u.enemy_rel_pos_ship[0], u.enemy_rel_pos_ship[1], u.enemy_rel_pos_ship[2], u.enemy_off_nose_deg, u.enemy_closing_speed);
	else
		std::fputs("0,,,,,,,,,,,", f);
	std::fprintf(f, "%u,%u,%d\n", s.attacked_mask, s.aimed_at_mask, ctl ? fl2(sample_flag2::controls_shared) : 0);
}

inline void write_event_csv_header(std::FILE *const f)
{
	std::fputs("time_s,level,type,pid,other,kind,id,value,flags\n", f);
}

inline void write_event_csv_row(std::FILE *const f, const int level, const event_record &e)
{
	std::fprintf(f, "%.3f,%d,%s,%u,%u,%u,%u,%u,%u\n", e.time_ms / 1000.0, level, record_type_name(e.type), e.pid, e.other, e.kind, e.id, e.value, e.flags);
}

}
