/*
 * This file is part of the DXX-Rebirth project <https://github.com/dxx-rebirth/dxx-rebirth/>.
 * It is copyright by its individual contributors, as recorded in the
 * project's Git history.  See COPYING.txt at the top level for license
 * terms and a link to the Git history.
 */
/*
 * Multiplayer network protocol v2: the wire layouts of the host state
 * bundle (`STATE` chunk) and of the client state (`INPUT` chunk)
 * (Documentation/network-protocol-v2.md, sections 5.1 to 5.3, and the
 * "Stage 2 as implemented" notes of section 8).
 *
 * Like net_v2.h, this header depends on nothing but the standard library,
 * so that the layouts can be tested outside the game
 * (common/unittest/net_v2_interp.cpp).  All values are in the engine's
 * units: positions and velocities are `fix` (1/65536 unit), times are net
 * time (1/65536 s), quaternions are the engine's `vms_quaternion`.
 */

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

#include "net_v2.h"

namespace dcx {

namespace net_v2 {

constexpr unsigned NET_V2_MAX_PLAYERS{8};

/* Section 5.2 */
constexpr std::size_t NET_V2_STATE_HEADER_SIZE{18};
/* The 41 bytes of section 5.2 plus `sample_age` (u16, stage 2): see
 * player_record.
 */
constexpr std::size_t NET_V2_PLAYER_RECORD_SIZE{43};
constexpr std::size_t NET_V2_GHOST_RECORD_SIZE{1};
/* The 31 bytes of section 5.2 plus `gen` (u8, stage 2): see
 * guided_record.
 */
constexpr std::size_t NET_V2_GUIDED_RECORD_SIZE{32};
constexpr std::size_t NET_V2_ROBOT_RECORD_SIZE{31};
constexpr unsigned NET_V2_MAX_GUIDED_RECORDS{NET_V2_MAX_PLAYERS};
constexpr unsigned NET_V2_MAX_ROBOT_RECORDS{12};
constexpr std::size_t NET_V2_PINGS_SIZE{NET_V2_MAX_PLAYERS};
constexpr std::size_t NET_V2_MAX_STATE_SIZE{NET_V2_STATE_HEADER_SIZE + NET_V2_MAX_PLAYERS * NET_V2_PLAYER_RECORD_SIZE + NET_V2_MAX_GUIDED_RECORDS * NET_V2_GUIDED_RECORD_SIZE + NET_V2_MAX_ROBOT_RECORDS * NET_V2_ROBOT_RECORD_SIZE + NET_V2_PINGS_SIZE};
static_assert(NET_V2_MAX_STATE_SIZE <= NET_V2_MAX_STATE_PART, "a full bundle must fit in one part");

/* Section 5.3 */
constexpr std::size_t NET_V2_INPUT_SIZE{46};
constexpr std::size_t NET_V2_MAX_INPUT_SIZE{NET_V2_INPUT_SIZE + NET_V2_GUIDED_RECORD_SIZE};

/* Section 5.1: quantisation shifts. */
constexpr unsigned NET_V2_VELOCITY_SHIFT{10};
constexpr unsigned NET_V2_ROTVEL_SHIFT{8};
constexpr unsigned NET_V2_SHIELDS_SHIFT{8};
/* Pings travel as u8 in steps of this many milliseconds. */
constexpr unsigned NET_V2_PING_STEP_MS{4};

/* `your_flags` of the bundle header. */
enum class state_flag : std::uint8_t
{
	correction = 1 << 0,
	has_pings = 1 << 1,
	countdown = 1 << 2,
};

/* `flags` of a player record. */
enum class player_record_flag : std::uint8_t
{
	alive = 1 << 0,
	cloaked = 1 << 1,
	invulnerable = 1 << 2,
	afterburner = 1 << 3,
	headlight = 1 << 4,
	flag_or_orbs = 1 << 5,
	dying = 1 << 6,
	/* The record is this one byte only. */
	ghost = 1 << 7,
};

/* `flags` of an INPUT chunk. */
enum class input_flag : std::uint8_t
{
	alive = 1 << 0,
	afterburner = 1 << 1,
	headlight = 1 << 2,
	want_respawn = 1 << 3,
	has_guided = 1 << 4,
	/* The ship exists but is in its death sequence (stage 2). */
	dying = 1 << 5,
};

[[nodiscard]]
constexpr std::uint8_t flag_bit(const auto f)
{
	return static_cast<std::uint8_t>(f);
}

/* Round `v` to the nearest multiple of 2^shift and return the quotient,
 * saturated to int16.
 */
[[nodiscard]]
constexpr std::int16_t quantise_i16(const std::int32_t v, const unsigned shift)
{
	const std::int64_t half{std::int64_t{1} << (shift - 1)};
	const std::int64_t q{(std::int64_t{v} + half) >> shift};
	return static_cast<std::int16_t>(q < -32768 ? -32768 : q > 32767 ? 32767 : q);
}

[[nodiscard]]
constexpr std::int32_t dequantise_i16(const std::int16_t q, const unsigned shift)
{
	return static_cast<std::int32_t>(q) * (std::int32_t{1} << shift);
}

/* Shields and energy: u16 in 1/256, negative values as 0. */
[[nodiscard]]
constexpr std::uint16_t quantise_u16(const std::int32_t v, const unsigned shift)
{
	if (v <= 0)
		return 0;
	const std::int64_t q{(std::int64_t{v} + (std::int64_t{1} << (shift - 1))) >> shift};
	return static_cast<std::uint16_t>(q > 65535 ? 65535 : q);
}

[[nodiscard]]
constexpr std::int32_t dequantise_u16(const std::uint16_t q, const unsigned shift)
{
	return static_cast<std::int32_t>(q) << shift;
}

[[nodiscard]]
constexpr std::uint8_t quantise_ping(const unsigned ms)
{
	const unsigned q{(ms + NET_V2_PING_STEP_MS / 2) / NET_V2_PING_STEP_MS};
	return static_cast<std::uint8_t>(q > 255 ? 255 : q);
}

struct net_vec
{
	std::int32_t x{}, y{}, z{};
	constexpr bool operator==(const net_vec &) const = default;
};

struct net_quat
{
	std::int16_t w{}, x{}, y{}, z{};
	constexpr bool operator==(const net_quat &) const = default;
};

/* A ship or missile pose as carried on the wire.  `vel` and `rotvel` hold
 * the dequantised values (multiples of 2^shift), so that re-encoding a
 * received value is exact.
 */
struct net_pose
{
	net_quat orient;
	net_vec pos;
	std::uint16_t segment{};
	net_vec vel;
	net_vec rotvel;
	constexpr bool operator==(const net_pose &) const = default;
};

/* Section 5.2, player record.  Stage 2 appends `sample_age`: the host
 * time of the bundle minus the time the record was sampled at by its
 * owner (the INPUT's `sample_time`), in net time units, saturated at
 * 65535 (one second).  A receiver therefore timestamps each record with
 * the time it describes instead of the bundle's time, so that the
 * relayed ships of other clients are interpolated between the moments
 * they were sampled rather than the moments the host happened to relay
 * them, and so that the times agree with the host's history (section
 * 6.6).  It is 0 for the host's own ship.
 */
struct player_record
{
	std::uint8_t flags{};
	net_pose pose;
	std::int32_t shields{};
	std::int32_t energy{};
	std::uint8_t weapon{};
	std::uint8_t input_age{};
	std::uint16_t sample_age{};
	[[nodiscard]]
	constexpr bool is_ghost() const
	{
		return flags & flag_bit(player_record_flag::ghost);
	}
	constexpr bool operator==(const player_record &) const = default;
};

/* Section 5.2, guided missile record.  `id` is the owner's object number
 * of the missile in stage 2 (the network object ids of section 6.1 come
 * with stage 3); a receiver maps it to its copy of the missile, which
 * was fired with that number (MULTI_FIRE_BOMB or MULTI_FIRE_TRACK).
 * The record's time is the owner's player record time (both come from the
 * same INPUT).  `gen` (stage 2, appended to the 31 bytes of section 5.2)
 * is the owner's count of guided missiles fired, modulo
 * NET_V2_GUIDED_GEN_MODULO, which the fire message carries too: a new
 * missile that reuses the object slot of the previous one has the same
 * `id`, but not the same `gen`.
 */
constexpr unsigned NET_V2_GUIDED_GEN_MODULO{128};

struct guided_record
{
	std::uint8_t pid{};
	std::uint16_t id{};
	std::uint8_t gen{};
	net_quat orient;
	net_vec pos;
	std::uint16_t segment{};
	net_vec vel;
	constexpr bool operator==(const guided_record &) const = default;
};

struct state_bundle
{
	std::uint32_t tick{};
	net_time host_time{};
	std::int32_t level_time{};
	std::uint16_t input_ack{};
	std::uint8_t your_flags{};
	/* Present records, in slot order; a ghost record has only `flags`. */
	std::array<std::optional<player_record>, NET_V2_MAX_PLAYERS> players{};
	unsigned n_guided{};
	std::array<guided_record, NET_V2_MAX_GUIDED_RECORDS> guided{};
	/* Robot records are skipped by the reader (robot games are deferred,
	 * section 10); the writer never sends any.
	 */
	unsigned n_robots{};
	std::array<std::uint8_t, NET_V2_MAX_PLAYERS> pings{};
	[[nodiscard]]
	constexpr bool has_flag(const state_flag f) const
	{
		return your_flags & flag_bit(f);
	}
};

struct input_chunk
{
	std::uint16_t seq{};
	net_time sample_time{};
	net_time view_time{};
	std::uint8_t flags{};
	net_pose pose;
	std::uint8_t weapon{};
	std::optional<guided_record> guided;
	[[nodiscard]]
	constexpr bool has_flag(const input_flag f) const
	{
		return flags & flag_bit(f);
	}
	constexpr bool operator==(const input_chunk &) const = default;
};

namespace detail {

struct wire_writer
{
	std::uint8_t *p;
	std::size_t pos{};
	constexpr void u8(const std::uint8_t v)
	{
		p[pos++] = v;
	}
	constexpr void u16(const std::uint16_t v)
	{
		net_put_le16(p + pos, v);
		pos += 2;
	}
	constexpr void i16(const std::int16_t v)
	{
		u16(static_cast<std::uint16_t>(v));
	}
	constexpr void u32(const std::uint32_t v)
	{
		net_put_le32(p + pos, v);
		pos += 4;
	}
	constexpr void i32(const std::int32_t v)
	{
		u32(static_cast<std::uint32_t>(v));
	}
	constexpr void quat(const net_quat &q)
	{
		i16(q.w);
		i16(q.x);
		i16(q.y);
		i16(q.z);
	}
	constexpr void vec(const net_vec &v)
	{
		i32(v.x);
		i32(v.y);
		i32(v.z);
	}
	constexpr void qvec(const net_vec &v, const unsigned shift)
	{
		i16(quantise_i16(v.x, shift));
		i16(quantise_i16(v.y, shift));
		i16(quantise_i16(v.z, shift));
	}
};

struct wire_reader
{
	std::span<const std::uint8_t> b;
	std::size_t pos{};
	bool ok{true};
	constexpr const std::uint8_t *take(const std::size_t n)
	{
		if (!ok || b.size() - pos < n)
		{
			ok = false;
			return nullptr;
		}
		const auto r{b.data() + pos};
		pos += n;
		return r;
	}
	constexpr std::uint8_t u8()
	{
		const auto r{take(1)};
		return r ? *r : 0;
	}
	constexpr std::uint16_t u16()
	{
		const auto r{take(2)};
		return r ? net_get_le16(r) : 0;
	}
	constexpr std::int16_t i16()
	{
		return static_cast<std::int16_t>(u16());
	}
	constexpr std::uint32_t u32()
	{
		const auto r{take(4)};
		return r ? net_get_le32(r) : 0;
	}
	constexpr std::int32_t i32()
	{
		return static_cast<std::int32_t>(u32());
	}
	constexpr net_quat quat()
	{
		net_quat q;
		q.w = i16();
		q.x = i16();
		q.y = i16();
		q.z = i16();
		return q;
	}
	constexpr net_vec vec()
	{
		net_vec v;
		v.x = i32();
		v.y = i32();
		v.z = i32();
		return v;
	}
	constexpr net_vec qvec(const unsigned shift)
	{
		net_vec v;
		v.x = dequantise_i16(i16(), shift);
		v.y = dequantise_i16(i16(), shift);
		v.z = dequantise_i16(i16(), shift);
		return v;
	}
};

constexpr void write_guided(wire_writer &w, const guided_record &g)
{
	w.u8(g.pid);
	w.u16(g.id);
	w.quat(g.orient);
	w.vec(g.pos);
	w.u16(g.segment);
	w.qvec(g.vel, NET_V2_VELOCITY_SHIFT);
	w.u8(g.gen);
}

constexpr guided_record read_guided(wire_reader &r)
{
	guided_record g;
	g.pid = r.u8();
	g.id = r.u16();
	g.orient = r.quat();
	g.pos = r.vec();
	g.segment = r.u16();
	g.vel = r.qvec(NET_V2_VELOCITY_SHIFT);
	g.gen = r.u8();
	return g;
}

}

/* The same velocity after a trip over the wire. */
[[nodiscard]]
constexpr net_vec quantised_velocity(const net_vec &v)
{
	return {dequantise_i16(quantise_i16(v.x, NET_V2_VELOCITY_SHIFT), NET_V2_VELOCITY_SHIFT), dequantise_i16(quantise_i16(v.y, NET_V2_VELOCITY_SHIFT), NET_V2_VELOCITY_SHIFT), dequantise_i16(quantise_i16(v.z, NET_V2_VELOCITY_SHIFT), NET_V2_VELOCITY_SHIFT)};
}

[[nodiscard]]
constexpr net_vec quantised_rotvel(const net_vec &v)
{
	return {dequantise_i16(quantise_i16(v.x, NET_V2_ROTVEL_SHIFT), NET_V2_ROTVEL_SHIFT), dequantise_i16(quantise_i16(v.y, NET_V2_ROTVEL_SHIFT), NET_V2_ROTVEL_SHIFT), dequantise_i16(quantise_i16(v.z, NET_V2_ROTVEL_SHIFT), NET_V2_ROTVEL_SHIFT)};
}

/* Serialise a bundle into `out` (at least NET_V2_MAX_STATE_SIZE bytes).
 * The player mask is built from `players`, and HAS_PINGS in `your_flags`
 * decides whether the ping trailer is written.  Returns the size.
 */
[[nodiscard]]
constexpr std::size_t write_state(const std::span<std::uint8_t, NET_V2_MAX_STATE_SIZE> out, const state_bundle &s)
{
	detail::wire_writer w{out.data()};
	std::uint8_t mask{};
	for (unsigned i = 0; i < NET_V2_MAX_PLAYERS; ++i)
		if (s.players[i])
			mask |= static_cast<std::uint8_t>(1u << i);
	const unsigned n_guided{s.n_guided < NET_V2_MAX_GUIDED_RECORDS ? s.n_guided : NET_V2_MAX_GUIDED_RECORDS};
	w.u32(s.tick);
	w.u32(s.host_time);
	w.i32(s.level_time);
	w.u16(s.input_ack);
	w.u8(s.your_flags);
	w.u8(mask);
	w.u8(static_cast<std::uint8_t>(n_guided));
	w.u8(0);
	for (const auto &rec : s.players)
	{
		if (!rec)
			continue;
		if (rec->is_ghost())
		{
			w.u8(flag_bit(player_record_flag::ghost));
			continue;
		}
		w.u8(rec->flags);
		w.quat(rec->pose.orient);
		w.vec(rec->pose.pos);
		w.u16(rec->pose.segment);
		w.qvec(rec->pose.vel, NET_V2_VELOCITY_SHIFT);
		w.qvec(rec->pose.rotvel, NET_V2_ROTVEL_SHIFT);
		w.u16(quantise_u16(rec->shields, NET_V2_SHIELDS_SHIFT));
		w.u16(quantise_u16(rec->energy, NET_V2_SHIELDS_SHIFT));
		w.u8(rec->weapon);
		w.u8(rec->input_age);
		w.u16(rec->sample_age);
	}
	for (unsigned i = 0; i < n_guided; ++i)
		detail::write_guided(w, s.guided[i]);
	if (s.has_flag(state_flag::has_pings))
		for (const auto p : s.pings)
			w.u8(p);
	return w.pos;
}

/* Parse a bundle.  Returns nothing unless the payload is exactly one
 * well-formed bundle: every counted record present, nothing left over,
 * at most NET_V2_MAX_GUIDED_RECORDS guided and NET_V2_MAX_ROBOT_RECORDS
 * robot records, every guided `pid` a valid slot.  Robot records are
 * skipped.
 */
[[nodiscard]]
constexpr std::optional<state_bundle> read_state(const std::span<const std::uint8_t> in)
{
	detail::wire_reader r{in};
	state_bundle s;
	s.tick = r.u32();
	s.host_time = r.u32();
	s.level_time = r.i32();
	s.input_ack = r.u16();
	s.your_flags = r.u8();
	const auto mask{r.u8()};
	s.n_guided = r.u8();
	s.n_robots = r.u8();
	if (!r.ok || s.n_guided > NET_V2_MAX_GUIDED_RECORDS || s.n_robots > NET_V2_MAX_ROBOT_RECORDS)
		return std::nullopt;
	for (unsigned i = 0; i < NET_V2_MAX_PLAYERS; ++i)
	{
		if (!(mask & (1u << i)))
			continue;
		player_record rec;
		rec.flags = r.u8();
		if (!rec.is_ghost())
		{
			rec.pose.orient = r.quat();
			rec.pose.pos = r.vec();
			rec.pose.segment = r.u16();
			rec.pose.vel = r.qvec(NET_V2_VELOCITY_SHIFT);
			rec.pose.rotvel = r.qvec(NET_V2_ROTVEL_SHIFT);
			rec.shields = dequantise_u16(r.u16(), NET_V2_SHIELDS_SHIFT);
			rec.energy = dequantise_u16(r.u16(), NET_V2_SHIELDS_SHIFT);
			rec.weapon = r.u8();
			rec.input_age = r.u8();
			rec.sample_age = r.u16();
		}
		else
			rec.flags = flag_bit(player_record_flag::ghost);
		s.players[i] = rec;
	}
	for (unsigned i = 0; i < s.n_guided; ++i)
	{
		s.guided[i] = detail::read_guided(r);
		if (s.guided[i].pid >= NET_V2_MAX_PLAYERS)
			return std::nullopt;
	}
	r.take(s.n_robots * NET_V2_ROBOT_RECORD_SIZE);
	if (s.has_flag(state_flag::has_pings))
		for (auto &p : s.pings)
			p = r.u8();
	if (!r.ok || r.pos != in.size())
		return std::nullopt;
	return s;
}

/* Serialise an INPUT chunk into `out`.  Returns the size (46, or 78 with
 * a guided record; `has_guided` in `flags` follows `guided`).
 */
[[nodiscard]]
constexpr std::size_t write_input(const std::span<std::uint8_t, NET_V2_MAX_INPUT_SIZE> out, const input_chunk &in)
{
	detail::wire_writer w{out.data()};
	w.u16(in.seq);
	w.u32(in.sample_time);
	w.u32(in.view_time);
	w.u8(static_cast<std::uint8_t>(in.guided ? (in.flags | flag_bit(input_flag::has_guided)) : (in.flags & ~flag_bit(input_flag::has_guided))));
	w.quat(in.pose.orient);
	w.vec(in.pose.pos);
	w.u16(in.pose.segment);
	w.qvec(in.pose.vel, NET_V2_VELOCITY_SHIFT);
	w.qvec(in.pose.rotvel, NET_V2_ROTVEL_SHIFT);
	w.u8(in.weapon);
	if (in.guided)
		detail::write_guided(w, *in.guided);
	return w.pos;
}

[[nodiscard]]
constexpr std::optional<input_chunk> read_input(const std::span<const std::uint8_t> bytes)
{
	detail::wire_reader r{bytes};
	input_chunk in;
	in.seq = r.u16();
	in.sample_time = r.u32();
	in.view_time = r.u32();
	in.flags = r.u8();
	in.pose.orient = r.quat();
	in.pose.pos = r.vec();
	in.pose.segment = r.u16();
	in.pose.vel = r.qvec(NET_V2_VELOCITY_SHIFT);
	in.pose.rotvel = r.qvec(NET_V2_ROTVEL_SHIFT);
	in.weapon = r.u8();
	if (in.has_flag(input_flag::has_guided))
		in.guided = detail::read_guided(r);
	if (!r.ok || r.pos != bytes.size())
		return std::nullopt;
	return in;
}

}

}
