/*
 * This file is part of the DXX-Rebirth project <https://github.com/dxx-rebirth/dxx-rebirth/>.
 * It is copyright by its individual contributors, as recorded in the
 * project's Git history.  See COPYING.txt at the top level for license
 * terms and a link to the Git history.
 */
/*
 * Multiplayer network protocol v2: constants and wire layouts of the
 * transport layer (Documentation/network-protocol-v2.md, section 3).
 *
 * This header depends on nothing but the standard library, so that the
 * transport can be built and tested outside the game.
 */

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace dcx {

namespace net_v2 {

/* Time units.  `net_clock` is the local clock in the same units as the
 * game's `fix64` (1/65536 s); `net_time` is its low 32 bits as carried
 * on the wire (wraps after 18.2 h; compare with wrapping `int32_t`
 * differences).
 */
using net_clock = std::int64_t;
using net_time = std::uint32_t;

[[nodiscard]]
constexpr net_clock net_seconds(const net_clock s)
{
	return s * 65536;
}

[[nodiscard]]
constexpr net_clock net_milliseconds(const net_clock ms)
{
	return (ms * 65536) / 1000;
}

/* Section 3.1.  The value of MULTI_PROTO_VERSION (multi.h): 100 for
 * stage 1, bumped by one per stage that changes the wire format (101:
 * stage 2, 102: stage 3, 103: stage 3 review fixes, 104: the PLAYER_LIST
 * bot flag, 105: host-assigned spawns, SPAWN_REQUEST and SPAWN_SITE,
 * 106: stage 4, 107: the shared controls in INPUT, -sharemoves, 108:
 * ADDRESS_SEEN, 109: CAPTURE);
 * named differently so that the two never shadow each other in a
 * translation unit that sees both.
 */
constexpr std::uint16_t NET_V2_PROTO_VERSION{109};
constexpr std::size_t NET_V2_HEADER_SIZE{34};
constexpr std::size_t NET_V2_MAX_PACKET{1200};
constexpr std::size_t NET_V2_ACK_BITS{64};
constexpr std::uint8_t NET_V2_PLAYER_ID_NONE{0xff};

/* Section 3.2 */
constexpr std::size_t NET_V2_CHUNK_HEADER_SIZE{3};
constexpr std::size_t NET_V2_MAX_CHUNK_PAYLOAD{NET_V2_MAX_PACKET - NET_V2_HEADER_SIZE - NET_V2_CHUNK_HEADER_SIZE};	/* 1163 */

/* Section 3.8: a STATE or INPUT chunk payload starts with one byte, the
 * part index in the low nibble and the part count (1..NET_V2_STATE_MAX_PARTS)
 * in the high nibble, so that a bundle split over two packets is applied
 * part by part, each part latest-wins on its own.  (Addition to the
 * design text, which leaves the parts self-describing at the application
 * layer only.)
 */
constexpr std::size_t NET_V2_STATE_PART_HEADER_SIZE{1};
constexpr unsigned NET_V2_STATE_MAX_PARTS{4};
constexpr std::size_t NET_V2_MAX_STATE_PART{NET_V2_MAX_CHUNK_PAYLOAD - NET_V2_STATE_PART_HEADER_SIZE};

[[nodiscard]]
constexpr std::uint8_t net_state_part_byte(const unsigned part, const unsigned count)
{
	return static_cast<std::uint8_t>((count << 4) | (part & 0x0f));
}

/* Section 3.4 */
constexpr std::size_t NET_V2_RELIABLE_RUN_HEADER_SIZE{3};	/* first_seq, count */
constexpr std::size_t NET_V2_MESSAGE_HEADER_SIZE{3};	/* msg_type, msg_len */
constexpr std::size_t NET_V2_MAX_MESSAGE{1024};
constexpr unsigned NET_V2_MAX_RUN_COUNT{255};
constexpr std::size_t NET_V2_RESEND_BUDGET{600};
constexpr unsigned NET_V2_RECV_WINDOW{256};
constexpr unsigned NET_V2_MAX_IN_FLIGHT{NET_V2_RECV_WINDOW};
constexpr unsigned NET_V2_GAP_LOSS_THRESHOLD{3};

/* Section 3.5 */
constexpr net_clock NET_V2_RTO_MIN{net_milliseconds(50)};
constexpr net_clock NET_V2_RTO_MAX{net_milliseconds(1000)};

/* Section 3.6 */
constexpr unsigned NET_V2_QUEUE_MAX_MESSAGES{512};
constexpr std::size_t NET_V2_QUEUE_MAX_BYTES{96 * 1024};
constexpr net_clock NET_V2_UNACKED_TIMEOUT{net_seconds(10)};
constexpr net_clock NET_V2_TIMEOUT{net_seconds(5)};
constexpr net_clock NET_V2_KEEPALIVE_INTERVAL{net_milliseconds(100)};

/* Section 3.7, step 8 */
constexpr unsigned NET_V2_PROTOCOL_ERROR_LIMIT{16};
constexpr net_clock NET_V2_PROTOCOL_ERROR_WINDOW{net_seconds(10)};

/* Section 2.2 */
constexpr net_clock NET_V2_CLOCK_SAMPLE_WINDOW{net_seconds(2)};
constexpr net_clock NET_V2_CLOCK_SLEW_PER_SECOND{net_milliseconds(5)};
constexpr net_clock NET_V2_CLOCK_JUMP_THRESHOLD{net_milliseconds(100)};

enum class packet_flag : std::uint8_t
{
	has_reliable = 1 << 0,
	keepalive = 1 << 1,
	unconnected = 1 << 2,
	reserved_mask = 0xf8,
};

enum class chunk_type : std::uint8_t
{
	reliable = 0x01,
	state = 0x02,
	input = 0x03,
	event_u = 0x04,
	session = 0x05,
};

/* Little-endian accessors.  These deliberately do not use byteutil.h,
 * which needs the generated dxxsconf.h, so that this header stays
 * usable from a standalone test build.
 */
[[nodiscard]]
constexpr std::uint16_t net_get_le16(const std::uint8_t *const p)
{
	return static_cast<std::uint16_t>(p[0] | (p[1] << 8));
}

[[nodiscard]]
constexpr std::uint32_t net_get_le32(const std::uint8_t *const p)
{
	return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) | (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
}

[[nodiscard]]
constexpr std::uint64_t net_get_le64(const std::uint8_t *const p)
{
	return static_cast<std::uint64_t>(net_get_le32(p)) | (static_cast<std::uint64_t>(net_get_le32(p + 4)) << 32);
}

constexpr void net_put_le16(std::uint8_t *const p, const std::uint16_t v)
{
	p[0] = static_cast<std::uint8_t>(v);
	p[1] = static_cast<std::uint8_t>(v >> 8);
}

constexpr void net_put_le32(std::uint8_t *const p, const std::uint32_t v)
{
	p[0] = static_cast<std::uint8_t>(v);
	p[1] = static_cast<std::uint8_t>(v >> 8);
	p[2] = static_cast<std::uint8_t>(v >> 16);
	p[3] = static_cast<std::uint8_t>(v >> 24);
}

constexpr void net_put_le64(std::uint8_t *const p, const std::uint64_t v)
{
	net_put_le32(p, static_cast<std::uint32_t>(v));
	net_put_le32(p + 4, static_cast<std::uint32_t>(v >> 32));
}

/* Wrapping sequence comparison: positive if `a` is after `b`. */
[[nodiscard]]
constexpr std::int16_t seq_diff(const std::uint16_t a, const std::uint16_t b)
{
	return static_cast<std::int16_t>(static_cast<std::uint16_t>(a - b));
}

/* Wrapping net time difference: `a - b`. */
[[nodiscard]]
constexpr std::int32_t net_time_diff(const net_time a, const net_time b)
{
	return static_cast<std::int32_t>(a - b);
}

[[nodiscard]]
constexpr net_time to_net_time(const net_clock t)
{
	return static_cast<net_time>(static_cast<std::uint64_t>(t));
}

/* Section 3.1: the 34-byte header at the start of every datagram.
 *
 *	| 0 | 2 | proto        | 100
 *	| 2 | 4 | session_id   |
 *	| 6 | 4 | peer_token   |
 *	| 10 | 1 | player_id   |
 *	| 11 | 1 | flags       |
 *	| 12 | 2 | seq         |
 *	| 14 | 2 | ack         |
 *	| 16 | 8 | ack_bits    |
 *	| 24 | 4 | send_time   |
 *	| 28 | 4 | echo_time   | send_time of the newest packet received from the peer (the one `ack` names), or 0
 *	| 32 | 2 | echo_delay  | time held since receiving it, saturated at 65535
 */
struct packet_header
{
	std::uint16_t proto{NET_V2_PROTO_VERSION};
	std::uint32_t session_id{};
	std::uint32_t peer_token{};
	std::uint8_t player_id{NET_V2_PLAYER_ID_NONE};
	std::uint8_t flags{};
	std::uint16_t seq{};
	std::uint16_t ack{};
	std::uint64_t ack_bits{};
	net_time send_time{};
	net_time echo_time{};
	std::uint16_t echo_delay{};

	[[nodiscard]]
	constexpr bool has_flag(const packet_flag f) const
	{
		return (flags & static_cast<std::uint8_t>(f)) != 0;
	}

	constexpr void write(std::uint8_t *const p) const
	{
		net_put_le16(p + 0, proto);
		net_put_le32(p + 2, session_id);
		net_put_le32(p + 6, peer_token);
		p[10] = player_id;
		p[11] = flags;
		net_put_le16(p + 12, seq);
		net_put_le16(p + 14, ack);
		net_put_le64(p + 16, ack_bits);
		net_put_le32(p + 24, send_time);
		net_put_le32(p + 28, echo_time);
		net_put_le16(p + 32, echo_delay);
	}

	/* Parse the header.  Returns nothing if the buffer is too short.  No
	 * semantic checks are made here; see connection::on_receive.
	 */
	[[nodiscard]]
	static constexpr std::optional<packet_header> read(const std::span<const std::uint8_t> bytes)
	{
		if (bytes.size() < NET_V2_HEADER_SIZE)
			return std::nullopt;
		const auto p{bytes.data()};
		packet_header h;
		h.proto = net_get_le16(p + 0);
		h.session_id = net_get_le32(p + 2);
		h.peer_token = net_get_le32(p + 6);
		h.player_id = p[10];
		h.flags = p[11];
		h.seq = net_get_le16(p + 12);
		h.ack = net_get_le16(p + 14);
		h.ack_bits = net_get_le64(p + 16);
		h.send_time = net_get_le32(p + 24);
		h.echo_time = net_get_le32(p + 28);
		h.echo_delay = net_get_le16(p + 32);
		return h;
	}
};

/* Section 3.2: a chunk header, as it appears after the packet header or
 * after the previous chunk.
 */
struct chunk_header
{
	std::uint8_t type{};
	std::uint16_t length{};

	constexpr void write(std::uint8_t *const p) const
	{
		p[0] = type;
		net_put_le16(p + 1, length);
	}

	[[nodiscard]]
	static constexpr chunk_header read(const std::uint8_t *const p)
	{
		return chunk_header{
			.type = p[0],
			.length = net_get_le16(p + 1),
		};
	}
};

using packet_buffer = std::array<std::uint8_t, NET_V2_MAX_PACKET>;

}

}
