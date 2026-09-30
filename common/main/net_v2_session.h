/*
 * This file is part of the DXX-Rebirth project <https://github.com/dxx-rebirth/dxx-rebirth/>.
 * It is copyright by its individual contributors, as recorded in the
 * project's Git history.  See COPYING.txt at the top level for license
 * terms and a link to the Git history.
 */
/*
 * Multiplayer network protocol v2, stage 1: the game-independent part of
 * the session layer (Documentation/network-protocol-v2.md, section 4).
 *
 * This header holds the session message ids, the framing of UNCONNECTED
 * datagrams (discovery and join, section 3.7 step 4), the fixed layouts
 * of the handshake messages, and the pure decision logic of the join
 * handshake: the admission table of section 4.2 and the client's retry
 * schedule.  Like net_v2.h it depends on the standard library only, so
 * that common/unittest/net_v2_session.cpp can exercise it without the
 * game.  Everything that touches game state lives in
 * similar/main/net_v2.cpp.
 */

#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

#include "net_v2.h"

namespace dcx {

namespace net_v2 {

/* Section 4 message ids.  Session messages 0x01-0x1F are as designed.
 * The 0x7x block carries v1 payloads unchanged during stage 1 (section 8,
 * "Stage 1"): the gameplay layer still speaks MULTI_* records, and the
 * level-end exchange keeps its v1 layout until stage 5 replaces it.
 */
enum class session_msg : std::uint8_t
{
	game_info_lite_req = 0x01,
	game_info_lite = 0x02,
	game_info_req = 0x03,
	game_info = 0x04,
	join_request = 0x05,
	join_accept = 0x06,
	join_deny = 0x07,
	game_settings = 0x08,
	player_list = 0x09,
	player_joined = 0x0a,
	player_left = 0x0b,
	leave = 0x0c,
	kick = 0x0d,
	host_shutdown = 0x0e,
	level_start = 0x10,
	level_ready = 0x11,
	level_go = 0x12,
	client_ready = 0x13,
	snapshot_begin = 0x14,
	snapshot_objects = 0x15,
	snapshot_game = 0x1a,
	snapshot_end = 0x1c,
	/* Stage 3, object authority (section 6.1-6.4, net_v2_objects.h). */
	inventory = 0x20,
	obj_create = 0x21,
	obj_remove = 0x22,
	pickup_request = 0x24,
	pickup_grant = 0x25,
	pickup_deny = 0x26,
	/* Stage 4, firing, hits, damage, kills, respawn (section 6.5,
	 * net_v2_combat.h).
	 */
	fire = 0x27,
	weapon_hit = 0x28,
	damage = 0x29,
	player_killed = 0x2a,
	player_spawn = 0x2b,
	drop_request = 0x3b,
	obj_settle = 0x47,
	/* Host-assigned spawns (section 8, "Host-assigned spawns",
	 * net_v2_objects.h).
	 */
	spawn_request = 0x48,
	spawn_site = 0x49,
	/* v1 `endlevel_h` payload (without the upid byte), host to client. */
	legacy_endlevel_host = 0x7c,
	/* v1 `endlevel_c` payload (without upid and player number), client to
	 * host.
	 */
	legacy_endlevel_client = 0x7d,
	/* Originator player id, then a run of v1 MULTI_* records, as a
	 * reliable message.
	 */
	legacy_mdata = 0x7f,
};

/* The stage 1 EVENT_U chunk payload: one byte naming the event kind, then
 * the event.  `legacy` carries the originator player id and a run of v1
 * MULTI_* records that v1 sent without acknowledgement.
 */
enum class event_kind : std::uint8_t
{
	legacy = 0x7f,
};

/* Section 4.1: the four-character game id in discovery and join messages. */
using game_id_type = std::array<std::uint8_t, 4>;

/* Section 4.1 / 4.2: the program version as carried on the wire. */
struct program_version
{
	std::uint16_t major{};
	std::uint16_t minor{};
	std::uint16_t micro{};
	constexpr bool operator==(const program_version &) const = default;
};

/* Sizes of the fixed-layout handshake messages. */
constexpr std::size_t NET_V2_GAME_INFO_REQ_SIZE{10};
constexpr std::size_t NET_V2_JOIN_REQUEST_SIZE{32};
constexpr std::size_t NET_V2_JOIN_ACCEPT_SIZE{22};
constexpr std::size_t NET_V2_JOIN_DENY_SIZE{14};
constexpr std::size_t NET_V2_CALLSIGN_SIZE{9};

/* Section 3.6: rate limits of the discovery replies. */
constexpr net_clock NET_V2_GAME_INFO_LITE_INTERVAL{net_seconds(1) / 8};
constexpr net_clock NET_V2_GAME_INFO_INTERVAL{net_seconds(1) / 2};
/* Join requests are cheap to answer, but each one runs the admission
 * table; a flood is bounded to this rate.
 */
constexpr net_clock NET_V2_JOIN_REQUEST_INTERVAL{net_seconds(1) / 20};

/* Section 4.2: the client's schedule. */
constexpr net_clock NET_V2_JOIN_RETRY_INTERVAL{net_milliseconds(500)};
constexpr net_clock NET_V2_JOIN_TIMEOUT{net_seconds(10)};
constexpr net_clock NET_V2_JOIN_HOLEPUNCH_AFTER{net_seconds(4)};
/* Section 4.4: a join in progress, from JOIN_ACCEPT (host) or LEVEL_READY
 * (client) to LEVEL_GO, must finish within this time.  The host kicks a
 * peer still joining or syncing after it (the joins are serialised, so a
 * stuck joiner would otherwise block every later join); the client gives
 * up waiting for the snapshot with a message instead of waiting forever.
 */
constexpr net_clock NET_V2_JOIN_SYNC_TIMEOUT{net_seconds(30)};

/* Section 4.6: how long a LEAVE, KICK or HOST_SHUTDOWN is retransmitted
 * before the connection is dropped.
 */
constexpr net_clock NET_V2_CLOSE_LINGER{net_seconds(1)};

/* Section 4.1: a game list entry not refreshed for this long is removed. */
constexpr net_clock NET_V2_GAME_LIST_EXPIRY{net_seconds(30)};

/* The largest SESSION chunk payload an UNCONNECTED datagram can carry:
 * one message header and its payload inside one chunk.
 */
constexpr std::size_t NET_V2_MAX_SESSION_PAYLOAD{NET_V2_MAX_CHUNK_PAYLOAD - NET_V2_MESSAGE_HEADER_SIZE};

/* Section 3.7 step 4: how an UNCONNECTED datagram was rejected. */
enum class unconnected_status : std::uint8_t
{
	accepted,
	bad_length,
	bad_proto,
	bad_flags,
	/* flags.UNCONNECTED is clear: a connected packet. */
	connected,
	/* session_id neither 0 nor the local session. */
	bad_session,
	/* peer_token is not 0 (JOIN_ACCEPT is the exception: it carries the
	 * new token, section 4.2).
	 */
	bad_token,
	/* Not exactly one SESSION chunk filling the datagram, or a message
	 * header that does not fit its chunk.
	 */
	malformed,
};

struct unconnected_message
{
	unconnected_status status{unconnected_status::bad_length};
	packet_header header;
	session_msg type{};
	std::span<const std::uint8_t> payload;
};

/* Section 3.7 steps 1-4 for a datagram that is not for a connection.
 * `local_session` is 0 on a client that has no session yet.  A session
 * id of 0 in the header is always allowed (discovery); JOIN_ACCEPT may
 * carry a token, everything else must carry 0.
 */
[[nodiscard]]
inline unconnected_message parse_unconnected(const std::span<const std::uint8_t> datagram, const std::uint32_t local_session)
{
	unconnected_message m;
	if (datagram.size() < NET_V2_HEADER_SIZE || datagram.size() > NET_V2_MAX_PACKET)
		return m;
	const auto h{packet_header::read(datagram)};
	if (!h)
		return m;
	m.header = *h;
	if (h->proto != NET_V2_PROTO_VERSION)
	{
		m.status = unconnected_status::bad_proto;
		return m;
	}
	if (h->flags & static_cast<std::uint8_t>(packet_flag::reserved_mask))
	{
		m.status = unconnected_status::bad_flags;
		return m;
	}
	if (!h->has_flag(packet_flag::unconnected))
	{
		m.status = unconnected_status::connected;
		return m;
	}
	if (h->session_id != 0 && h->session_id != local_session)
	{
		m.status = unconnected_status::bad_session;
		return m;
	}
	const auto body{datagram.subspan(NET_V2_HEADER_SIZE)};
	if (body.size() < NET_V2_CHUNK_HEADER_SIZE + NET_V2_MESSAGE_HEADER_SIZE)
	{
		m.status = unconnected_status::malformed;
		return m;
	}
	const auto ch{chunk_header::read(body.data())};
	if (ch.type != static_cast<std::uint8_t>(chunk_type::session) || ch.length != body.size() - NET_V2_CHUNK_HEADER_SIZE)
	{
		m.status = unconnected_status::malformed;
		return m;
	}
	const auto msg{body.subspan(NET_V2_CHUNK_HEADER_SIZE)};
	const auto msg_len{net_get_le16(msg.data() + 1)};
	if (msg_len != msg.size() - NET_V2_MESSAGE_HEADER_SIZE)
	{
		m.status = unconnected_status::malformed;
		return m;
	}
	m.type = static_cast<session_msg>(msg[0]);
	if (h->peer_token != 0 && m.type != session_msg::join_accept)
	{
		m.status = unconnected_status::bad_token;
		return m;
	}
	m.payload = msg.subspan(NET_V2_MESSAGE_HEADER_SIZE);
	m.status = unconnected_status::accepted;
	return m;
}

/* Build an UNCONNECTED datagram: header (section 3.1, sequence and
 * acknowledgement fields 0) and one SESSION chunk holding one message.
 * Returns the datagram, or an empty span if the payload does not fit.
 */
[[nodiscard]]
inline std::span<const std::uint8_t> build_unconnected(packet_buffer &out, const std::uint32_t session_id, const std::uint32_t peer_token, const std::uint8_t player_id, const net_time send_time, const session_msg type, const std::span<const std::uint8_t> payload)
{
	if (payload.size() > NET_V2_MAX_SESSION_PAYLOAD)
		return {};
	packet_header h;
	h.session_id = session_id;
	h.peer_token = peer_token;
	h.player_id = player_id;
	h.flags = static_cast<std::uint8_t>(packet_flag::unconnected);
	h.send_time = send_time;
	h.write(out.data());
	std::size_t pos{NET_V2_HEADER_SIZE};
	chunk_header{
		.type = static_cast<std::uint8_t>(chunk_type::session),
		.length = static_cast<std::uint16_t>(NET_V2_MESSAGE_HEADER_SIZE + payload.size()),
	}.write(out.data() + pos);
	pos += NET_V2_CHUNK_HEADER_SIZE;
	out[pos] = static_cast<std::uint8_t>(type);
	net_put_le16(out.data() + pos + 1, static_cast<std::uint16_t>(payload.size()));
	pos += NET_V2_MESSAGE_HEADER_SIZE;
	std::copy(payload.begin(), payload.end(), out.begin() + pos);
	pos += payload.size();
	return std::span<const std::uint8_t>(out).first(pos);
}

/* Section 4.1: GAME_INFO_LITE_REQ and GAME_INFO_REQ, 10 bytes. */
struct game_info_request
{
	game_id_type game_id{};
	program_version version{};

	constexpr void write(std::uint8_t *const p) const
	{
		std::copy(game_id.begin(), game_id.end(), p);
		net_put_le16(p + 4, version.major);
		net_put_le16(p + 6, version.minor);
		net_put_le16(p + 8, version.micro);
	}
	[[nodiscard]]
	static constexpr std::optional<game_info_request> read(const std::span<const std::uint8_t> b)
	{
		if (b.size() != NET_V2_GAME_INFO_REQ_SIZE)
			return std::nullopt;
		game_info_request r;
		std::copy(b.begin(), b.begin() + 4, r.game_id.begin());
		r.version = {net_get_le16(b.data() + 4), net_get_le16(b.data() + 6), net_get_le16(b.data() + 8)};
		return r;
	}
};

/* Section 4.2: JOIN_REQUEST, 32 bytes. */
struct join_request
{
	game_id_type game_id{};
	program_version version{};
	std::uint32_t client_nonce{};
	/* NUL-padded; the receiver lower-cases it. */
	std::array<std::uint8_t, NET_V2_CALLSIGN_SIZE> callsign{};
	std::uint8_t rank{};
	std::int32_t current_level{};
	net_time client_time{};

	constexpr void write(std::uint8_t *const p) const
	{
		std::copy(game_id.begin(), game_id.end(), p);
		net_put_le16(p + 4, version.major);
		net_put_le16(p + 6, version.minor);
		net_put_le16(p + 8, version.micro);
		net_put_le32(p + 10, client_nonce);
		std::copy(callsign.begin(), callsign.end(), p + 14);
		p[23] = rank;
		net_put_le32(p + 24, static_cast<std::uint32_t>(current_level));
		net_put_le32(p + 28, client_time);
	}
	[[nodiscard]]
	static constexpr std::optional<join_request> read(const std::span<const std::uint8_t> b)
	{
		if (b.size() != NET_V2_JOIN_REQUEST_SIZE)
			return std::nullopt;
		join_request r;
		std::copy(b.begin(), b.begin() + 4, r.game_id.begin());
		r.version = {net_get_le16(b.data() + 4), net_get_le16(b.data() + 6), net_get_le16(b.data() + 8)};
		r.client_nonce = net_get_le32(b.data() + 10);
		std::copy(b.begin() + 14, b.begin() + 14 + NET_V2_CALLSIGN_SIZE, r.callsign.begin());
		/* The last byte is always a terminator, whatever the sender put
		 * there.
		 */
		r.callsign.back() = 0;
		r.rank = b[23];
		r.current_level = static_cast<std::int32_t>(net_get_le32(b.data() + 24));
		r.client_time = net_get_le32(b.data() + 28);
		return r;
	}
};

/* Section 4.2: JOIN_ACCEPT, 22 bytes.  The new token travels in the header. */
struct join_accept
{
	std::uint32_t client_nonce{};
	std::uint8_t player_id{};
	std::uint8_t tick_rate{};
	net_time host_time{};
	std::uint32_t tick{};
	net_time client_time{};
	std::uint32_t session_id{};

	constexpr void write(std::uint8_t *const p) const
	{
		net_put_le32(p + 0, client_nonce);
		p[4] = player_id;
		p[5] = tick_rate;
		net_put_le32(p + 6, host_time);
		net_put_le32(p + 10, tick);
		net_put_le32(p + 14, client_time);
		net_put_le32(p + 18, session_id);
	}
	[[nodiscard]]
	static constexpr std::optional<join_accept> read(const std::span<const std::uint8_t> b)
	{
		if (b.size() != NET_V2_JOIN_ACCEPT_SIZE)
			return std::nullopt;
		join_accept r;
		r.client_nonce = net_get_le32(b.data() + 0);
		r.player_id = b[4];
		r.tick_rate = b[5];
		r.host_time = net_get_le32(b.data() + 6);
		r.tick = net_get_le32(b.data() + 10);
		r.client_time = net_get_le32(b.data() + 14);
		r.session_id = net_get_le32(b.data() + 18);
		return r;
	}
};

/* Section 4.2: JOIN_DENY, 14 bytes.  `reason` is a kick_player_reason
 * value; this header does not know the enum.
 */
struct join_deny
{
	std::uint32_t client_nonce{};
	std::uint8_t reason{};
	program_version version{};
	std::uint16_t proto{NET_V2_PROTO_VERSION};

	constexpr void write(std::uint8_t *const p) const
	{
		net_put_le32(p + 0, client_nonce);
		p[4] = reason;
		net_put_le16(p + 5, version.major);
		net_put_le16(p + 7, version.minor);
		net_put_le16(p + 9, version.micro);
		net_put_le16(p + 11, proto);
		p[13] = 0;
	}
	[[nodiscard]]
	static constexpr std::optional<join_deny> read(const std::span<const std::uint8_t> b)
	{
		if (b.size() != NET_V2_JOIN_DENY_SIZE)
			return std::nullopt;
		join_deny r;
		r.client_nonce = net_get_le32(b.data() + 0);
		r.reason = b[4];
		r.version = {net_get_le16(b.data() + 5), net_get_le16(b.data() + 7), net_get_le16(b.data() + 9)};
		r.proto = net_get_le16(b.data() + 11);
		return r;
	}
};

/* A minimum interval between accepted events, for the discovery replies
 * and join requests of section 3.6.
 */
class rate_limiter
{
	net_clock m_interval;
	std::optional<net_clock> m_last;
public:
	explicit constexpr rate_limiter(const net_clock interval) :
		m_interval{interval}
	{
	}
	/* True, and the event is counted, if at least the interval has passed
	 * since the last accepted event.
	 */
	[[nodiscard]]
	constexpr bool allow(const net_clock now)
	{
		if (m_last && now - *m_last < m_interval)
			return false;
		m_last = now;
		return true;
	}
	constexpr void reset()
	{
		m_last.reset();
	}
};

/* Section 4.2, the client side: JOIN_REQUEST every 500 ms for at most
 * 10 s, a hole punch request through the tracker once after 4 s.
 */
class join_attempt
{
	bool m_active{};
	net_clock m_started{};
	std::optional<net_clock> m_last_sent;
	bool m_holepunch_sent{};
	std::uint32_t m_nonce{};
public:
	constexpr void begin(const net_clock now, const std::uint32_t nonce)
	{
		m_active = true;
		m_started = now;
		m_last_sent.reset();
		m_holepunch_sent = false;
		m_nonce = nonce;
	}
	constexpr void end()
	{
		m_active = false;
	}
	[[nodiscard]]
	constexpr bool active() const
	{
		return m_active;
	}
	[[nodiscard]]
	constexpr std::uint32_t nonce() const
	{
		return m_nonce;
	}
	/* True once per retry interval; the caller sends the request. */
	[[nodiscard]]
	constexpr bool due(const net_clock now)
	{
		if (!m_active)
			return false;
		if (m_last_sent && now - *m_last_sent < NET_V2_JOIN_RETRY_INTERVAL)
			return false;
		m_last_sent = now;
		return true;
	}
	[[nodiscard]]
	constexpr bool timed_out(const net_clock now) const
	{
		return m_active && now - m_started >= NET_V2_JOIN_TIMEOUT;
	}
	/* True once, 4 s into the attempt. */
	[[nodiscard]]
	constexpr bool holepunch_due(const net_clock now)
	{
		if (!m_active || m_holepunch_sent || now - m_started < NET_V2_JOIN_HOLEPUNCH_AFTER)
			return false;
		m_holepunch_sent = true;
		return true;
	}
};

/* Section 4.5, PLAYER_LIST: bit 7 of a slot's `connected` byte marks a
 * bot (Documentation/multiplayer-bots.md section 2.2); the low bits are
 * the connection status.
 */
constexpr std::uint8_t PLAYER_LIST_BOT_FLAG{0x80};

[[nodiscard]]
constexpr std::uint8_t encode_list_connected(const std::uint8_t status, const bool bot)
{
	return static_cast<std::uint8_t>((status & ~PLAYER_LIST_BOT_FLAG) | (bot ? PLAYER_LIST_BOT_FLAG : 0));
}

struct list_connected
{
	std::uint8_t status;
	bool bot;
};

[[nodiscard]]
constexpr list_connected decode_list_connected(const std::uint8_t b)
{
	return {static_cast<std::uint8_t>(b & ~PLAYER_LIST_BOT_FLAG), (b & PLAYER_LIST_BOT_FLAG) != 0};
}

/* Section 4.2: the admission table, on a view of the host's player slots.
 * The caller has already handled version mismatch, the endlevel state, the
 * refuse prompt and a retry of a pending accept.
 */
struct slot_view
{
	/* The slot has a player (a callsign), connected or not. */
	bool occupied{};
	/* The player in the slot is connected (any state but disconnected). */
	bool connected{};
	/* The requester's callsign equals the slot's. */
	bool callsign_matches{};
	/* The requester's address equals the slot's last known address. */
	bool address_matches{};
	/* Local time of the last packet from the slot's player; the oldest
	 * disconnected slot is taken over first.
	 */
	net_clock last_packet_time{};
	/* Documentation/multiplayer-bots.md section 2.3: the slot's player is
	 * a bot (flown by the host; `connected` while it plays, not after it
	 * left), added as the `bot_order`th bot of the game.
	 */
	bool bot{};
	unsigned bot_order{};
};

enum class admission_result : std::uint8_t
{
	/* A new player takes `slot`. */
	accept_new,
	/* The player in `slot` returns (same callsign, was disconnected). */
	accept_rejoin,
	/* The player in `slot` is connected from the same address: a client
	 * that restarted before its old connection timed out.  The caller
	 * drops the old connection and accepts as a rejoin.
	 */
	accept_replace,
	deny_closed,
	deny_full,
	deny_duplicate_callsign,
	/* The bot playing in `slot` leaves and a new player takes the slot
	 * (humans replace bots: the game is full, or the bot has the
	 * requester's callsign).
	 */
	accept_replace_bot,
};

/* The results that give the requester `slot`. */
[[nodiscard]]
constexpr bool admission_accepts(const admission_result r)
{
	return r == admission_result::accept_new || r == admission_result::accept_rejoin || r == admission_result::accept_replace || r == admission_result::accept_replace_bot;
}

struct admission_decision
{
	admission_result result;
	unsigned slot;
};

/* The bot a joining human replaces (bots section 2.3): the most recently
 * added bot still playing below the player limit.
 */
[[nodiscard]]
constexpr std::optional<unsigned> bot_to_replace(const std::span<const slot_view> slots, const unsigned max_players)
{
	std::optional<unsigned> r;
	for (unsigned i = 1; i < slots.size() && i < max_players; ++i)
	{
		const auto &s{slots[i]};
		if (s.bot && s.connected && (!r || s.bot_order > slots[*r].bot_order))
			r = i;
	}
	return r;
}

/* A slot a new player may take without anyone leaving: a free slot below
 * the player limit, else the slot of a bot that left (a bot's slot is
 * never rejoined, so it is as good as free).
 */
[[nodiscard]]
constexpr std::optional<unsigned> free_admission_slot(const std::span<const slot_view> slots, const unsigned max_players)
{
	for (unsigned i = 0; i < slots.size() && i < max_players; ++i)
		if (!slots[i].occupied)
			return i;
	if (slots.size() < max_players)
		return static_cast<unsigned>(slots.size());
	for (unsigned i = 1; i < slots.size() && i < max_players; ++i)
	{
		const auto &s{slots[i]};
		if (s.bot && !s.connected)
			return i;
	}
	return std::nullopt;
}

/* `bots_replaceable`: the host lets humans replace bots (the Bots
 * screen's option).  A bot's slot is never rejoined by callsign: a human
 * with a departed bot's name is a new player; one with a playing bot's
 * name replaces that bot only when the game is full (option on),
 * otherwise it is refused as a duplicate, so that cycling names cannot
 * strip bots from a game with room.  In a full game the most recently
 * added bot leaves before a disconnected human's slot is handed out: a
 * human who dropped keeps a slot to rejoin while bots play.
 */
[[nodiscard]]
constexpr admission_decision decide_admission(const std::span<const slot_view> slots, const unsigned max_players, const bool game_closed, const bool bots_replaceable = false)
{
	const auto free_slot{free_admission_slot(slots, max_players)};
	/* A returning player first: the callsign names its slot. */
	for (unsigned i = 0; i < slots.size(); ++i)
	{
		const auto &s{slots[i]};
		if (!s.occupied || !s.callsign_matches)
			continue;
		if (s.bot)
		{
			if (!s.connected)
				continue;
			if (game_closed)
				return {admission_result::deny_closed, 0};
			if (bots_replaceable && i < max_players && !free_slot)
				return {admission_result::accept_replace_bot, i};
			return {admission_result::deny_duplicate_callsign, i};
		}
		if (!s.connected)
			return {admission_result::accept_rejoin, i};
		if (s.address_matches)
			return {admission_result::accept_replace, i};
		return {admission_result::deny_duplicate_callsign, i};
	}
	if (game_closed)
		return {admission_result::deny_closed, 0};
	if (free_slot)
		return {admission_result::accept_new, *free_slot};
	/* Every slot below the limit is taken.  Slots at or above the limit
	 * are never handed out: the level has start positions (and the game
	 * per-player tables) only for the first `max_players` slots.  A bot
	 * leaves first, then the player disconnected the longest is replaced.
	 */
	if (bots_replaceable)
		if (const auto b{bot_to_replace(slots, max_players)})
			return {admission_result::accept_replace_bot, *b};
	std::optional<unsigned> oldest;
	for (unsigned i = 0; i < slots.size() && i < max_players; ++i)
	{
		const auto &s{slots[i]};
		if (s.connected)
			continue;
		if (!oldest || s.last_packet_time < slots[*oldest].last_packet_time)
			oldest = i;
	}
	if (oldest)
		return {admission_result::accept_new, *oldest};
	return {admission_result::deny_full, 0};
}

/* Whether an accepted player enters `slot` as a new player (scores and
 * kill-matrix row reset, "joined" rather than "rejoined").  A slot handed
 * to a new player (a free or departed player's slot, or a replaced bot's)
 * `awaits_entry` until that player's CLIENT_READY: a client that restarts
 * during its join comes back by callsign as a rejoin, but it never
 * entered the game and must not inherit the previous holder's score.
 */
[[nodiscard]]
constexpr bool admission_is_new(const admission_result r, const bool awaits_entry)
{
	switch (r)
	{
		case admission_result::accept_new:
		case admission_result::accept_replace_bot:
			return true;
		case admission_result::accept_rejoin:
		case admission_result::accept_replace:
			return awaits_entry;
		default:
			return false;
	}
}

/* Section 4.3, the lobby: the slot a new player gets while the host forms
 * the game.  `occupied[i]` is true for a slot that has a player or a
 * connection; slot 0 is the host.  Returns the lowest free slot below
 * `max_players`, or nothing if the game is full.  Keeping every slot below
 * the player limit means the slots are always valid indices into the
 * start positions and the per-player tables, without renumbering (every
 * peer's player id is fixed by its JOIN_ACCEPT).
 */
[[nodiscard]]
constexpr std::optional<unsigned> choose_lobby_slot(const std::span<const bool> occupied, const unsigned max_players)
{
	for (unsigned i = 1; i < max_players; ++i)
		if (i >= occupied.size() || !occupied[i])
			return i;
	return std::nullopt;
}

/* The player count a player in `slot` must see: the game it is in
 * includes its own slot.  The host counts a player joining a level in
 * progress only when it has applied the snapshot (CLIENT_READY), so the
 * count at the time the snapshot is sent does not include it yet; a
 * client whose N_players left out its own slot would drop every kill
 * message about itself and leave itself out of the kill list.
 */
[[nodiscard]]
constexpr unsigned player_count_including(const unsigned numplayers, const unsigned slot)
{
	return numplayers > slot ? numplayers : slot + 1;
}

/* The session phase of a peer (net_v2.cpp keeps one per player slot). */
enum class peer_phase : std::uint8_t
{
	none,
	/* JOIN_ACCEPT sent (host) or received (client); a join in progress
	 * awaits the client's LEVEL_READY.
	 */
	joining,
	/* The level snapshot was queued; awaiting CLIENT_READY. */
	syncing,
	playing,
	/* LEAVE, KICK or HOST_SHUTDOWN queued; the connection lingers for
	 * NET_V2_CLOSE_LINGER so that it can be retransmitted.
	 */
	closing,
};

/* Section 4.2: a JOIN_REQUEST compared with an existing connection.  A
 * request carrying the nonce of the connection's own join attempt, from
 * its address, is a retry: while the connection is still `connecting` the
 * JOIN_ACCEPT was lost and is sent again; once the connection is
 * established the retry was merely delayed or reordered and is ignored.
 * It must never be admitted as a new join, which would give the same
 * client a second slot or replace its live connection.  A connection that
 * is closing (the peer left or was kicked) never answers its old attempt
 * again: the client retries, and is judged afresh once the linger is over.
 *
 * Anything else is not a retry of this connection, whatever state the
 * connection is in: a client that left (or was killed) and joins again
 * from the same address draws a new nonce, and that request is a new join
 * (or replaces the stale connection by the admission table), never a
 * duplicate to be ignored.
 */
enum class duplicate_join : std::uint8_t
{
	/* Not a retry of this connection's attempt. */
	none,
	resend_accept,
	ignore,
};

[[nodiscard]]
constexpr duplicate_join classify_duplicate_join(const bool same_address, const std::uint32_t connection_nonce, const std::uint32_t request_nonce, const bool connecting, const bool closing = false)
{
	if (!same_address || connection_nonce != request_nonce)
		return duplicate_join::none;
	if (closing)
		return duplicate_join::ignore;
	return connecting ? duplicate_join::resend_accept : duplicate_join::ignore;
}

/* Section 4.2: the host serves one join in progress at a time.  A peer
 * joining or syncing blocks the others, except when it is at the address
 * of the requester (the same client asking again; the request replaces
 * the stale attempt).  A peer that is gone, closing or playing never
 * blocks: the serialisation ends with the peer, however it ends.
 * `extras_pending` is the host's extras run (or a queued one), which is
 * part of the join as in v1.
 */
struct join_peer_view
{
	peer_phase phase{peer_phase::none};
	bool same_address{};
};

[[nodiscard]]
constexpr bool join_in_progress(const std::span<const join_peer_view> peers, const bool extras_pending)
{
	if (extras_pending)
		return true;
	for (const auto &p : peers)
		if ((p.phase == peer_phase::joining || p.phase == peer_phase::syncing) && !p.same_address)
			return true;
	return false;
}

/* Section 4.4: the host's bound on a join in progress.  `since` is when
 * the peer entered its current phase.
 */
[[nodiscard]]
constexpr bool join_stalled(const peer_phase phase, const net_clock since, const net_clock now)
{
	return (phase == peer_phase::joining || phase == peer_phase::syncing) && now - since >= NET_V2_JOIN_SYNC_TIMEOUT;
}

/* Section 4.4, the client: the wait for the level snapshot of a join in
 * progress, from LEVEL_READY to LEVEL_GO.  It is bounded, so that a host
 * that never completes the join cannot hold the client forever.  A client
 * waiting for a fresh level start (lobby, next level) is not bounded: it
 * waits for the host and the other players, as in v1.
 */
class join_sync_wait
{
	std::optional<net_clock> m_started;
public:
	constexpr void begin(const net_clock now)
	{
		m_started = now;
	}
	constexpr void end()
	{
		m_started.reset();
	}
	[[nodiscard]]
	constexpr bool active() const
	{
		return m_started.has_value();
	}
	[[nodiscard]]
	constexpr bool expired(const net_clock now) const
	{
		return m_started && now - *m_started >= NET_V2_JOIN_SYNC_TIMEOUT;
	}
};

/* Section 4.2, the client: whether a JOIN_DENY is an answer to us.  A
 * denial of the running join attempt carries its nonce and comes from the
 * host it was sent to.  A version denial with nonce 0 answers a
 * GAME_INFO_REQ (discovery) and must come from the host that was asked.
 * Anything else is ignored, so that a stray or forged datagram cannot
 * abort a join.
 */
enum class join_deny_match : std::uint8_t
{
	ignore,
	/* Ends the join attempt. */
	join,
	/* A version mismatch reported to the game info request. */
	discovery,
};

[[nodiscard]]
constexpr join_deny_match classify_join_deny(const std::uint32_t deny_nonce, const bool version_mismatch, const bool join_active, const std::uint32_t join_nonce, const bool from_join_host, const bool from_info_host)
{
	if (join_active && deny_nonce == join_nonce && from_join_host)
		return join_deny_match::join;
	if (version_mismatch && deny_nonce == 0 && from_info_host)
		return join_deny_match::discovery;
	return join_deny_match::ignore;
}

/* CRC-32 (IEEE 802.3, as zlib) over the snapshot payloads (section 4.4).
 * Bitwise: the snapshot is some tens of kilobytes per join, so a table
 * is not worth its footprint here.
 */
[[nodiscard]]
constexpr std::uint32_t crc32_update(std::uint32_t crc, const std::span<const std::uint8_t> bytes)
{
	crc = ~crc;
	for (const auto b : bytes)
	{
		crc ^= b;
		for (unsigned k = 0; k < 8; ++k)
			crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
	}
	return ~crc;
}

}

}
