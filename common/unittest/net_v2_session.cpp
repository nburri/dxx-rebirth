/*
 * This file is part of the DXX-Rebirth project <https://github.com/dxx-rebirth/dxx-rebirth/>.
 * It is copyright by its individual contributors, as recorded in the
 * project's Git history.  See COPYING.txt at the top level for license
 * terms and a link to the Git history.
 */
/*
 * Test of the game-independent session layer (net_v2_session.h): the
 * UNCONNECTED datagram framing and its validation order, the handshake
 * message layouts, the admission table of section 4.2, the lobby slot
 * choice, the player count a joining player sees, the recognition of
 * retried join requests and of join denials, the serialisation of joins
 * in progress and their bounds, the client's join schedule and the rate
 * limiter.
 *
 * Build and run with SCons:
 *
 *	scons sdl2=1 d1x=0 d2x=1 register_runtime_test_plain_link_targets=1 test-net-v2-session
 *	build/common/test-net-v2-session
 *
 * or directly:
 *
 *	g++ -std=gnu++23 -O2 -Wall -Wextra -Icommon/main common/unittest/net_v2_session.cpp -o test-net-v2-session
 */

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "net_v2_session.h"

using namespace dcx::net_v2;

namespace {

void check_failed(const char *const expr, const char *const file, const int line)
{
	std::fprintf(stderr, "%s:%d: check failed: %s\n", file, line, expr);
	std::exit(1);
}

#define CHECK(cond)	do { if (!(cond)) check_failed(#cond, __FILE__, __LINE__); } while (0)

constexpr game_id_type test_game_id{{'D', '2', 'X', 'R'}};
constexpr program_version test_version{1, 2, 3};

void test_unconnected_framing()
{
	packet_buffer buf;
	const join_request req{
		.game_id = test_game_id,
		.version = test_version,
		.client_nonce = 0x12345678,
		.callsign = {{'p', 'i', 'l', 'o', 't', 0, 0, 0, 0}},
		.rank = 3,
		.current_level = -2,
		.client_time = 0xdeadbeef,
	};
	std::array<std::uint8_t, NET_V2_JOIN_REQUEST_SIZE> payload;
	req.write(payload.data());
	const auto dg{build_unconnected(buf, 0xaabbccdd, 0, NET_V2_PLAYER_ID_NONE, 777, session_msg::join_request, payload)};
	CHECK(dg.size() == NET_V2_HEADER_SIZE + NET_V2_CHUNK_HEADER_SIZE + NET_V2_MESSAGE_HEADER_SIZE + NET_V2_JOIN_REQUEST_SIZE);
	/* The first two bytes are the protocol, so a v1 build reads upid 100
	 * and up (an unknown packet type).
	 */
	CHECK(dg[0] == NET_V2_PROTO_VERSION && dg[1] == 0);

	{
		const auto m{parse_unconnected(dg, 0xaabbccdd)};
		CHECK(m.status == unconnected_status::accepted);
		CHECK(m.type == session_msg::join_request);
		CHECK(m.header.session_id == 0xaabbccdd);
		CHECK(m.header.send_time == 777);
		CHECK(m.header.player_id == NET_V2_PLAYER_ID_NONE);
		const auto r{join_request::read(m.payload)};
		CHECK(r.has_value());
		CHECK(r->game_id == test_game_id);
		CHECK(r->version == test_version);
		CHECK(r->client_nonce == 0x12345678);
		CHECK(r->callsign == req.callsign);
		CHECK(r->rank == 3);
		CHECK(r->current_level == -2);
		CHECK(r->client_time == 0xdeadbeef);
	}
	/* Wrong local session: rejected before the chunk is looked at. */
	CHECK(parse_unconnected(dg, 0x11111111).status == unconnected_status::bad_session);
	/* Session 0 is discovery and always allowed. */
	{
		packet_buffer b2;
		const auto d2{build_unconnected(b2, 0, 0, NET_V2_PLAYER_ID_NONE, 1, session_msg::game_info_lite_req, std::array<std::uint8_t, 10>{})};
		CHECK(parse_unconnected(d2, 0x11111111).status == unconnected_status::accepted);
	}
	/* Validation order: length, proto, flags, connected, token, chunk. */
	{
		std::vector<std::uint8_t> d(dg.begin(), dg.end());
		CHECK(parse_unconnected(std::span(d).first(NET_V2_HEADER_SIZE - 1), 0xaabbccdd).status == unconnected_status::bad_length);
		std::vector<std::uint8_t> big(NET_V2_MAX_PACKET + 1, 0);
		CHECK(parse_unconnected(big, 0).status == unconnected_status::bad_length);
		d[0] = 16;
		CHECK(parse_unconnected(d, 0xaabbccdd).status == unconnected_status::bad_proto);
		d[0] = NET_V2_PROTO_VERSION;
		d[11] |= 0x80;
		CHECK(parse_unconnected(d, 0xaabbccdd).status == unconnected_status::bad_flags);
		d[11] = 0;
		CHECK(parse_unconnected(d, 0xaabbccdd).status == unconnected_status::connected);
		d[11] = static_cast<std::uint8_t>(packet_flag::unconnected);
		net_put_le32(d.data() + 6, 5);
		CHECK(parse_unconnected(d, 0xaabbccdd).status == unconnected_status::bad_token);
		net_put_le32(d.data() + 6, 0);
		CHECK(parse_unconnected(d, 0xaabbccdd).status == unconnected_status::accepted);
		/* A second chunk, a wrong chunk type, a wrong chunk length and a
		 * wrong message length are all malformed.
		 */
		auto d2{d};
		d2.push_back(0);
		CHECK(parse_unconnected(d2, 0xaabbccdd).status == unconnected_status::malformed);
		d2 = d;
		d2[NET_V2_HEADER_SIZE] = static_cast<std::uint8_t>(chunk_type::reliable);
		CHECK(parse_unconnected(d2, 0xaabbccdd).status == unconnected_status::malformed);
		d2 = d;
		net_put_le16(d2.data() + NET_V2_HEADER_SIZE + 1, 3);
		CHECK(parse_unconnected(d2, 0xaabbccdd).status == unconnected_status::malformed);
		d2 = d;
		net_put_le16(d2.data() + NET_V2_HEADER_SIZE + NET_V2_CHUNK_HEADER_SIZE + 1, 5);
		CHECK(parse_unconnected(d2, 0xaabbccdd).status == unconnected_status::malformed);
		/* A header-only datagram with the flag is malformed, not a crash. */
		CHECK(parse_unconnected(std::span(d).first(NET_V2_HEADER_SIZE), 0xaabbccdd).status == unconnected_status::malformed);
	}
	/* JOIN_ACCEPT is the one message allowed to carry a token. */
	{
		packet_buffer b2;
		const join_accept acc{.client_nonce = 9, .player_id = 3, .tick_rate = 60, .host_time = 100, .tick = 5, .client_time = 42, .session_id = 0xaabbccdd};
		std::array<std::uint8_t, NET_V2_JOIN_ACCEPT_SIZE> p;
		acc.write(p.data());
		const auto d2{build_unconnected(b2, 0xaabbccdd, 0x5555, 0, 1, session_msg::join_accept, p)};
		const auto m{parse_unconnected(d2, 0xaabbccdd)};
		CHECK(m.status == unconnected_status::accepted);
		CHECK(m.header.peer_token == 0x5555);
		const auto r{join_accept::read(m.payload)};
		CHECK(r && r->client_nonce == 9 && r->player_id == 3 && r->tick_rate == 60 && r->host_time == 100 && r->tick == 5 && r->client_time == 42 && r->session_id == 0xaabbccdd);
	}
	/* An oversized payload is refused. */
	{
		packet_buffer b2;
		std::vector<std::uint8_t> p(NET_V2_MAX_SESSION_PAYLOAD + 1);
		CHECK(build_unconnected(b2, 0, 0, 0, 0, session_msg::game_info, p).empty());
		p.pop_back();
		const auto d2{build_unconnected(b2, 0, 0, 0, 0, session_msg::game_info, p)};
		CHECK(d2.size() == NET_V2_MAX_PACKET);
		CHECK(parse_unconnected(d2, 0).status == unconnected_status::accepted);
	}
}

void test_message_layouts()
{
	{
		const game_info_request r{.game_id = test_game_id, .version = test_version};
		std::array<std::uint8_t, NET_V2_GAME_INFO_REQ_SIZE> p;
		r.write(p.data());
		const auto b{game_info_request::read(p)};
		CHECK(b && b->game_id == test_game_id && b->version == test_version);
		CHECK(!game_info_request::read(std::span(p).first(9)));
	}
	{
		const join_deny d{.client_nonce = 77, .reason = 4, .version = test_version, .proto = 100};
		std::array<std::uint8_t, NET_V2_JOIN_DENY_SIZE> p;
		d.write(p.data());
		CHECK(p[13] == 0);
		const auto b{join_deny::read(p)};
		CHECK(b && b->client_nonce == 77 && b->reason == 4 && b->version == test_version && b->proto == 100);
		CHECK(!join_deny::read(std::span(p).first(13)));
	}
	{
		/* A callsign without terminator is terminated by the reader. */
		join_request r{};
		r.callsign.fill('x');
		std::array<std::uint8_t, NET_V2_JOIN_REQUEST_SIZE> p;
		r.write(p.data());
		const auto b{join_request::read(p)};
		CHECK(b && b->callsign.back() == 0 && b->callsign[7] == 'x');
	}
}

void test_admission()
{
	std::vector<slot_view> slots;
	/* Empty game, not closed: the first slot. */
	{
		const auto d{decide_admission(slots, 8, false)};
		CHECK(d.result == admission_result::accept_new && d.slot == 0);
	}
	/* Host in slot 0, a free slot at the end. */
	slots.push_back({.occupied = true, .connected = true});
	{
		const auto d{decide_admission(slots, 8, false)};
		CHECK(d.result == admission_result::accept_new && d.slot == 1);
	}
	/* Closed game: denied. */
	CHECK(decide_admission(slots, 8, true).result == admission_result::deny_closed);
	/* A returning player is accepted even in a closed game. */
	slots.push_back({.occupied = true, .connected = false, .callsign_matches = true, .last_packet_time = 50});
	{
		const auto d{decide_admission(slots, 8, true)};
		CHECK(d.result == admission_result::accept_rejoin && d.slot == 1);
	}
	/* Same callsign, still connected, different address: duplicate. */
	slots[1].connected = true;
	CHECK(decide_admission(slots, 8, false).result == admission_result::deny_duplicate_callsign);
	/* Same callsign, connected, same address: a restarted client. */
	slots[1].address_matches = true;
	{
		const auto d{decide_admission(slots, 8, false)};
		CHECK(d.result == admission_result::accept_replace && d.slot == 1);
	}
	slots[1].callsign_matches = false;
	slots[1].address_matches = false;
	/* A vacated slot in the middle is taken before a new slot at the end. */
	slots.push_back({.occupied = false});
	slots.push_back({.occupied = true, .connected = true});
	{
		const auto d{decide_admission(slots, 8, false)};
		CHECK(d.result == admission_result::accept_new && d.slot == 2);
	}
	slots[2].occupied = true;
	slots[2].connected = true;
	/* Four occupied slots, limit 4, everybody connected: full. */
	CHECK(decide_admission(slots, 4, false).result == admission_result::deny_full);
	/* Two disconnected: the one silent the longest is replaced. */
	slots[2].connected = false;
	slots[2].last_packet_time = 200;
	slots[3].connected = false;
	slots[3].last_packet_time = 100;
	{
		const auto d{decide_admission(slots, 4, false)};
		CHECK(d.result == admission_result::accept_new && d.slot == 3);
	}
	/* Below the limit with all slots occupied: a new slot is opened. */
	slots[2].connected = true;
	slots[3].connected = true;
	{
		const auto d{decide_admission(slots, 8, false)};
		CHECK(d.result == admission_result::accept_new && d.slot == 4);
	}
	/* The host's view is always MAX_PLAYERS wide.  With the slots below
	 * the limit connected, the empty slots above it are neither free nor
	 * taken over: the game is full.
	 */
	{
		std::vector<slot_view> wide(8);
		for (unsigned i = 0; i < 4; ++i)
			wide[i] = {.occupied = true, .connected = true, .last_packet_time = 1000};
		CHECK(decide_admission(wide, 4, false).result == admission_result::deny_full);
		/* A disconnected slot above the limit (stale) is not taken over
		 * either; one below it is.
		 */
		wide[5] = {.occupied = true, .connected = false, .last_packet_time = 1};
		CHECK(decide_admission(wide, 4, false).result == admission_result::deny_full);
		wide[2].connected = false;
		const auto d{decide_admission(wide, 4, false)};
		CHECK(d.result == admission_result::accept_new && d.slot == 2);
	}
}

/* Documentation/multiplayer-bots.md section 2.3: humans replace bots,
 * and a bot's slot is never rejoined by callsign.
 */
void test_admission_with_bots()
{
	/* Host, a human, three bots added in the order 1, 2, 3; limit 5. */
	std::vector<slot_view> slots(8);
	slots[0] = {.occupied = true, .connected = true};
	slots[1] = {.occupied = true, .connected = true, .last_packet_time = 10};
	slots[2] = {.occupied = true, .connected = true, .bot = true, .bot_order = 1};
	slots[3] = {.occupied = true, .connected = true, .bot = true, .bot_order = 3};
	slots[4] = {.occupied = true, .connected = true, .bot = true, .bot_order = 2};
	/* Full, the option off: denied as before. */
	CHECK(decide_admission(slots, 5, false).result == admission_result::deny_full);
	CHECK(decide_admission(slots, 5, false, false).result == admission_result::deny_full);
	/* The option on: the most recently added bot leaves. */
	{
		const auto d{decide_admission(slots, 5, false, true)};
		CHECK(d.result == admission_result::accept_replace_bot && d.slot == 3);
		CHECK(admission_accepts(d.result));
		CHECK(bot_to_replace(slots, 5) == 3u);
	}
	/* A closed game stays closed. */
	CHECK(decide_admission(slots, 5, true, true).result == admission_result::deny_closed);
	/* A free slot (below the limit) is taken before any bot leaves. */
	{
		const auto d{decide_admission(slots, 6, false, true)};
		CHECK(d.result == admission_result::accept_new && d.slot == 5);
	}
	/* A bot leaves before a disconnected human's slot is handed out: the
	 * human may come back to it.
	 */
	slots[1].connected = false;
	{
		const auto d{decide_admission(slots, 5, false, true)};
		CHECK(d.result == admission_result::accept_replace_bot && d.slot == 3);
	}
	/* The option off: the disconnected human's slot, as before. */
	{
		const auto d{decide_admission(slots, 5, false, false)};
		CHECK(d.result == admission_result::accept_new && d.slot == 1);
	}
	/* The human returns to its slot while the bots play. */
	slots[1].callsign_matches = true;
	{
		const auto d{decide_admission(slots, 5, false, true)};
		CHECK(d.result == admission_result::accept_rejoin && d.slot == 1);
	}
	slots[1].callsign_matches = false;
	slots[1].connected = true;
	/* After the last bot left (disconnected, still listed as a bot), its
	 * slot is the free one: the next bot is not removed.
	 */
	slots[3].connected = false;
	{
		const auto d{decide_admission(slots, 5, false, true)};
		CHECK(d.result == admission_result::accept_new && d.slot == 3);
		CHECK(bot_to_replace(slots, 5) == 4u);
	}
	/* A departed bot's name never rejoins its slot (no score carried);
	 * the slot is only reused as a new player's.
	 */
	slots[3].callsign_matches = true;
	{
		const auto d{decide_admission(slots, 5, false, true)};
		CHECK(d.result == admission_result::accept_new && d.slot == 3);
	}
	/* Even in a closed game a departed bot's name is no rejoin. */
	CHECK(decide_admission(slots, 5, true, true).result == admission_result::deny_closed);
	slots[3].callsign_matches = false;
	slots[3].connected = true;
	/* A playing bot with the joiner's name, the game full: that bot is
	 * replaced (option on), or the name is a duplicate (option off).
	 */
	slots[2].callsign_matches = true;
	{
		const auto d{decide_admission(slots, 5, false, true)};
		CHECK(d.result == admission_result::accept_replace_bot && d.slot == 2);
	}
	CHECK(decide_admission(slots, 5, false, false).result == admission_result::deny_duplicate_callsign);
	/* With room in the game the name is a duplicate: cycling names must
	 * not strip bots.  A free slot, or a departed bot's slot, is room.
	 */
	{
		const auto d{decide_admission(slots, 8, false, true)};
		CHECK(d.result == admission_result::deny_duplicate_callsign && d.slot == 2);
	}
	slots[4].connected = false;
	CHECK(decide_admission(slots, 5, false, true).result == admission_result::deny_duplicate_callsign);
	slots[4].connected = true;
	slots[2].callsign_matches = false;
	/* Bots at or above the limit (a lowered limit) are not replaced. */
	CHECK(!bot_to_replace(slots, 2).has_value());
	CHECK(decide_admission(slots, 2, false, true).result == admission_result::deny_full);
	/* No bots at all: nothing to replace. */
	std::vector<slot_view> humans(4, slot_view{.occupied = true, .connected = true});
	CHECK(!bot_to_replace(humans, 4).has_value());
	CHECK(decide_admission(humans, 4, false, true).result == admission_result::deny_full);
}

/* A slot handed to a new player stays new until its CLIENT_READY: a
 * restarted client's rejoin by callsign does not inherit the score of the
 * bot (or departed player) that held the slot.
 */
void test_admission_is_new()
{
	CHECK(admission_is_new(admission_result::accept_new, false));
	CHECK(admission_is_new(admission_result::accept_replace_bot, false));
	CHECK(!admission_is_new(admission_result::accept_rejoin, false));
	CHECK(!admission_is_new(admission_result::accept_replace, false));
	CHECK(admission_is_new(admission_result::accept_rejoin, true));
	CHECK(admission_is_new(admission_result::accept_replace, true));
	CHECK(!admission_is_new(admission_result::deny_full, true));
	CHECK(!admission_is_new(admission_result::deny_duplicate_callsign, true));
	/* The scenario: a human replaces the bot in slot 3, then restarts
	 * during its join.  The slot now carries the human's callsign and is
	 * disconnected: the admission is a rejoin, made new by the mark.
	 */
	std::vector<slot_view> slots(4);
	slots[0] = {.occupied = true, .connected = true};
	slots[1] = {.occupied = true, .connected = true};
	slots[2] = {.occupied = true, .connected = true, .bot = true, .bot_order = 1};
	slots[3] = {.occupied = true, .connected = true, .bot = true, .bot_order = 2};
	auto d{decide_admission(slots, 4, false, true)};
	CHECK(d.result == admission_result::accept_replace_bot && d.slot == 3);
	bool awaits_entry{admission_is_new(d.result, false)};
	slots[3] = {.occupied = true, .connected = false, .callsign_matches = true};
	d = decide_admission(slots, 4, false, true);
	CHECK(d.result == admission_result::accept_rejoin && d.slot == 3);
	CHECK(admission_is_new(d.result, awaits_entry));
	/* After its CLIENT_READY the mark is gone: a later drop and return is
	 * a real rejoin.
	 */
	awaits_entry = false;
	CHECK(!admission_is_new(d.result, awaits_entry));
}

/* Section 4.5: the PLAYER_LIST bot flag in the `connected` byte. */
void test_player_list_bot_flag()
{
	for (std::uint8_t status = 0; status < 8; ++status)
		for (const bool bot : {false, true})
		{
			const auto b{encode_list_connected(status, bot)};
			CHECK(((b & PLAYER_LIST_BOT_FLAG) != 0) == bot);
			const auto d{decode_list_connected(b)};
			CHECK(d.status == status);
			CHECK(d.bot == bot);
		}
	/* A human's byte is unchanged by the encoding (older layouts). */
	CHECK(encode_list_connected(1, false) == 1);
	CHECK(encode_list_connected(1, true) == 0x81);
	/* A stray high bit in the status never survives as status. */
	CHECK(decode_list_connected(encode_list_connected(0x81, false)).status == 1);
	CHECK(!decode_list_connected(encode_list_connected(0x81, false)).bot);
}

void test_lobby_slot()
{
	/* Host only: slot 1. */
	{
		const std::array<bool, 8> occ{{true}};
		const auto s{choose_lobby_slot(occ, 4)};
		CHECK(s && *s == 1);
	}
	/* A hole is filled first. */
	{
		const std::array<bool, 8> occ{{true, true, false, true}};
		const auto s{choose_lobby_slot(occ, 4)};
		CHECK(s && *s == 2);
	}
	/* Full at the limit, even though slots above it are free. */
	{
		const std::array<bool, 8> occ{{true, true, true, true}};
		CHECK(!choose_lobby_slot(occ, 4));
		const auto s{choose_lobby_slot(occ, 8)};
		CHECK(s && *s == 4);
	}
	/* Every slot taken. */
	{
		std::array<bool, 8> occ;
		occ.fill(true);
		CHECK(!choose_lobby_slot(occ, 8));
	}
	/* A short view: slots past its end are free. */
	{
		const std::array<bool, 2> occ{{true, true}};
		const auto s{choose_lobby_slot(occ, 8)};
		CHECK(s && *s == 2);
	}
	/* Never slot 0, never at or above the limit. */
	{
		const std::array<bool, 8> occ{};
		const auto s{choose_lobby_slot(occ, 2)};
		CHECK(s && *s == 1);
		CHECK(!choose_lobby_slot(occ, 1));
	}
}

void test_player_count_including()
{
	/* A player joining in progress in the next slot: counted. */
	CHECK(player_count_including(1, 1) == 2);
	CHECK(player_count_including(2, 3) == 4);
	/* Already counted (lobby player, rejoin, lower hole). */
	CHECK(player_count_including(2, 1) == 2);
	CHECK(player_count_including(4, 1) == 4);
	CHECK(player_count_including(1, 0) == 1);
	/* Nobody counted yet. */
	CHECK(player_count_including(0, 0) == 1);
}

void test_duplicate_join()
{
	/* Other address or other nonce: a different attempt. */
	CHECK(classify_duplicate_join(false, 7, 7, true) == duplicate_join::none);
	CHECK(classify_duplicate_join(true, 7, 8, true) == duplicate_join::none);
	CHECK(classify_duplicate_join(true, 7, 8, false) == duplicate_join::none);
	/* The accept was lost: send it again. */
	CHECK(classify_duplicate_join(true, 7, 7, true) == duplicate_join::resend_accept);
	/* A late retry after the connection was established: ignored, never a
	 * second admission.
	 */
	CHECK(classify_duplicate_join(true, 7, 7, false) == duplicate_join::ignore);
	/* A closing connection (the peer left or was kicked) never answers its
	 * old attempt again, not even with the accept it may have lost.
	 */
	CHECK(classify_duplicate_join(true, 7, 7, true, true) == duplicate_join::ignore);
	CHECK(classify_duplicate_join(true, 7, 7, false, true) == duplicate_join::ignore);
	/* Rejoin: the client left (or was killed and restarted) and asks again
	 * from the same address, with a new nonce.  Whatever state the old
	 * connection is in - still connecting, established, or closing - the
	 * request is a new join (admitted or replacing by the admission
	 * table), never a duplicate to be ignored.
	 */
	CHECK(classify_duplicate_join(true, 7, 8, true, false) == duplicate_join::none);
	CHECK(classify_duplicate_join(true, 7, 8, false, false) == duplicate_join::none);
	CHECK(classify_duplicate_join(true, 7, 8, false, true) == duplicate_join::none);
	CHECK(classify_duplicate_join(true, 7, 8, true, true) == duplicate_join::none);
}

void test_join_serialisation()
{
	using p = peer_phase;
	const auto view{[](const peer_phase ph, const bool same) {
		return join_peer_view{.phase = ph, .same_address = same};
	}};
	/* Nobody joining. */
	{
		const std::array<join_peer_view, 3> v{{view(p::none, false), view(p::playing, false), view(p::none, true)}};
		CHECK(!join_in_progress(v, false));
		/* An extras run (or a queued one) is part of the join. */
		CHECK(join_in_progress(v, true));
	}
	/* Another client joining or syncing blocks. */
	{
		const std::array<join_peer_view, 2> v{{view(p::playing, false), view(p::joining, false)}};
		CHECK(join_in_progress(v, false));
	}
	{
		const std::array<join_peer_view, 2> v{{view(p::syncing, false), view(p::none, false)}};
		CHECK(join_in_progress(v, false));
	}
	/* The requester's own stale attempt does not block it: the request
	 * replaces it.
	 */
	{
		const std::array<join_peer_view, 2> v{{view(p::syncing, true), view(p::joining, true)}};
		CHECK(!join_in_progress(v, false));
	}
	/* The serialisation ends with the joining peer: once it is closing
	 * (left, kicked, or removed after the join timeout) or gone (its
	 * connection timed out), it blocks nobody.
	 */
	{
		const std::array<join_peer_view, 2> v{{view(p::closing, false), view(p::none, false)}};
		CHECK(!join_in_progress(v, false));
	}
	/* The host's bound on a join in progress. */
	CHECK(!join_stalled(p::joining, net_seconds(5), net_seconds(5) + NET_V2_JOIN_SYNC_TIMEOUT - 1));
	CHECK(join_stalled(p::joining, net_seconds(5), net_seconds(5) + NET_V2_JOIN_SYNC_TIMEOUT));
	CHECK(join_stalled(p::syncing, 0, NET_V2_JOIN_SYNC_TIMEOUT + net_seconds(1)));
	/* Nobody else is ever removed by it. */
	CHECK(!join_stalled(p::playing, 0, NET_V2_JOIN_SYNC_TIMEOUT * 10));
	CHECK(!join_stalled(p::closing, 0, NET_V2_JOIN_SYNC_TIMEOUT * 10));
	CHECK(!join_stalled(p::none, 0, NET_V2_JOIN_SYNC_TIMEOUT * 10));
}

void test_join_sync_wait()
{
	join_sync_wait w;
	/* Not waiting (a fresh level start is not bounded). */
	CHECK(!w.active());
	CHECK(!w.expired(NET_V2_JOIN_SYNC_TIMEOUT * 10));
	w.begin(net_seconds(100));
	CHECK(w.active());
	CHECK(!w.expired(net_seconds(100) + NET_V2_JOIN_SYNC_TIMEOUT - 1));
	CHECK(w.expired(net_seconds(100) + NET_V2_JOIN_SYNC_TIMEOUT));
	/* LEVEL_GO ends the wait. */
	w.end();
	CHECK(!w.active());
	CHECK(!w.expired(net_seconds(1000)));
	/* A new join starts a fresh bound. */
	w.begin(net_seconds(1000));
	CHECK(!w.expired(net_seconds(1001)));
}

void test_join_deny()
{
	/* The answer to our join attempt. */
	CHECK(classify_join_deny(5, false, true, 5, true, false) == join_deny_match::join);
	CHECK(classify_join_deny(5, true, true, 5, true, false) == join_deny_match::join);
	/* Wrong nonce, wrong sender, or no attempt running: ignored. */
	CHECK(classify_join_deny(6, false, true, 5, true, false) == join_deny_match::ignore);
	CHECK(classify_join_deny(5, false, true, 5, false, true) == join_deny_match::ignore);
	CHECK(classify_join_deny(5, false, false, 5, true, false) == join_deny_match::ignore);
	/* A version mismatch without a nonce or sender check is not accepted:
	 * forged or stray denials cannot abort a join.
	 */
	CHECK(classify_join_deny(0, true, true, 5, false, false) == join_deny_match::ignore);
	CHECK(classify_join_deny(9, true, true, 5, true, true) == join_deny_match::ignore);
	/* The version answer to our GAME_INFO_REQ: nonce 0, from the host we
	 * asked.
	 */
	CHECK(classify_join_deny(0, true, false, 0, false, true) == join_deny_match::discovery);
	CHECK(classify_join_deny(0, true, true, 5, false, true) == join_deny_match::discovery);
	/* Nonce 0 of another reason is not discovery. */
	CHECK(classify_join_deny(0, false, false, 0, false, true) == join_deny_match::ignore);
}

void test_join_attempt()
{
	join_attempt j;
	CHECK(!j.active());
	CHECK(!j.due(0));
	CHECK(!j.timed_out(net_seconds(100)));
	j.begin(net_seconds(10), 0xabc);
	CHECK(j.active() && j.nonce() == 0xabc);
	/* Sent at once, then every 500 ms. */
	CHECK(j.due(net_seconds(10)));
	CHECK(!j.due(net_seconds(10) + net_milliseconds(499)));
	CHECK(j.due(net_seconds(10) + net_milliseconds(500)));
	CHECK(!j.due(net_seconds(10) + net_milliseconds(600)));
	/* Hole punch once, from 4 s on. */
	CHECK(!j.holepunch_due(net_seconds(13)));
	CHECK(j.holepunch_due(net_seconds(14)));
	CHECK(!j.holepunch_due(net_seconds(15)));
	/* Timeout at 10 s. */
	CHECK(!j.timed_out(net_seconds(19)));
	CHECK(j.timed_out(net_seconds(20)));
	j.end();
	CHECK(!j.active());
	CHECK(!j.due(net_seconds(21)));
	CHECK(!j.timed_out(net_seconds(30)));
	/* A new attempt starts a fresh schedule. */
	j.begin(net_seconds(30), 1);
	CHECK(j.due(net_seconds(30)));
	CHECK(!j.holepunch_due(net_seconds(31)));
	CHECK(j.holepunch_due(net_seconds(34)));
}

void test_rate_limiter()
{
	rate_limiter r{NET_V2_GAME_INFO_LITE_INTERVAL};
	CHECK(r.allow(0));
	CHECK(!r.allow(NET_V2_GAME_INFO_LITE_INTERVAL - 1));
	CHECK(r.allow(NET_V2_GAME_INFO_LITE_INTERVAL));
	CHECK(!r.allow(NET_V2_GAME_INFO_LITE_INTERVAL + 1));
	r.reset();
	CHECK(r.allow(NET_V2_GAME_INFO_LITE_INTERVAL + 1));
}

void test_crc32()
{
	/* The well-known check value of "123456789". */
	const std::array<std::uint8_t, 9> s{{'1', '2', '3', '4', '5', '6', '7', '8', '9'}};
	CHECK(crc32_update(0, s) == 0xcbf43926u);
	/* Incremental over two parts equals the whole. */
	CHECK(crc32_update(crc32_update(0, std::span(s).first(4)), std::span(s).subspan(4)) == 0xcbf43926u);
	CHECK(crc32_update(0, {}) == 0);
}

}

int main()
{
	test_unconnected_framing();
	test_message_layouts();
	test_admission();
	test_admission_with_bots();
	test_admission_is_new();
	test_player_list_bot_flag();
	test_lobby_slot();
	test_player_count_including();
	test_duplicate_join();
	test_join_serialisation();
	test_join_sync_wait();
	test_join_deny();
	test_join_attempt();
	test_rate_limiter();
	test_crc32();
	std::puts("all tests passed");
	return 0;
}
