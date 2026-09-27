/*
 * This file is part of the DXX-Rebirth project <https://github.com/dxx-rebirth/dxx-rebirth/>.
 * It is copyright by its individual contributors, as recorded in the
 * project's Git history.  See COPYING.txt at the top level for license
 * terms and a link to the Git history.
 */
/*
 * Test of the game-independent session layer (net_v2_session.h): the
 * UNCONNECTED datagram framing and its validation order, the handshake
 * message layouts, the admission table of section 4.2, the client's join
 * schedule and the rate limiter.
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
	/* The first two bytes are the protocol, so a v1 build reads upid 100. */
	CHECK(dg[0] == 100 && dg[1] == 0);

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
		d[0] = 100;
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
	test_join_attempt();
	test_rate_limiter();
	test_crc32();
	std::puts("all tests passed");
	return 0;
}
