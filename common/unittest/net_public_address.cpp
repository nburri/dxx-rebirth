/*
 * This file is part of the DXX-Rebirth project <https://github.com/dxx-rebirth/dxx-rebirth/>.
 * It is copyright by its individual contributors, as recorded in the
 * project's Git history.  See COPYING.txt at the top level for license
 * terms and a link to the Git history.
 */
/*
 * Test of the host's public address (net_public_address.h): the
 * ADDRESS_SEEN layout and its checks of untrusted input, the text of an
 * address, and the choice among the players' and the tracker's reports.
 *
 * Build and run with SCons:
 *
 *	scons sdl2=1 d1x=0 d2x=1 register_runtime_test_plain_link_targets=1 test-net-public-address
 *	build/common/test-net-public-address
 */

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "net_public_address.h"

using namespace dcx;

namespace {

unsigned checks;

void check_failed(const char *const expr, const char *const file, const int line)
{
	std::fprintf(stderr, "%s:%d: check failed: %s\n", file, line, expr);
	std::exit(1);
}

#define CHECK(cond)	do { ++checks; if (!(cond)) check_failed(#cond, __FILE__, __LINE__); } while (0)

/* Documentation addresses (RFC 5737, RFC 3849) stand in for public ones
 * where the classification does not matter; classify_ipv4 calls them
 * public.
 */
seen_address v4(const uint8_t a, const uint8_t b, const uint8_t c, const uint8_t d, const uint16_t port)
{
	return make_seen_ipv4({{a, b, c, d}}, port);
}

seen_address v6(std::array<uint8_t, 16> a, const uint16_t port)
{
	return make_seen_ipv6(a.data(), port);
}

std::vector<uint8_t> encode(const seen_address &s)
{
	std::vector<uint8_t> out(NET_V2_ADDRESS_SEEN_SIZE);
	write_address_seen(s, out.data());
	return out;
}

void test_round_trip()
{
	const auto a{v4(203, 0, 113, 7, 42424)};
	const auto buf{encode(a)};
	CHECK(buf.size() == 19);
	CHECK(buf[0] == 4);
	CHECK(buf[1] == 203 && buf[2] == 0 && buf[3] == 113 && buf[4] == 7);
	CHECK(buf[17] == (42424 & 0xff) && buf[18] == (42424 >> 8));
	const auto r{read_address_seen(buf)};
	CHECK(r && *r == a);

	const auto b{v6({{0x20, 0x01, 0x0d, 0xb8, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x42}}, 50000)};
	CHECK(b.family == 6);
	const auto rb{read_address_seen(encode(b))};
	CHECK(rb && *rb == b);
}

void test_malformed()
{
	auto buf{encode(v4(203, 0, 113, 7, 42424))};
	/* Wrong sizes */
	CHECK(!read_address_seen(std::span<const uint8_t>(buf).first(18)));
	{
		auto longer{buf};
		longer.push_back(0);
		CHECK(!read_address_seen(longer));
	}
	CHECK(!read_address_seen({}));
	/* Unknown family */
	{
		auto b{buf};
		b[0] = 5;
		CHECK(!read_address_seen(b));
		b[0] = 0;
		CHECK(!read_address_seen(b));
	}
	/* IPv4 with garbage after the address */
	{
		auto b{buf};
		b[9] = 1;
		CHECK(!read_address_seen(b));
	}
	/* No port */
	{
		auto b{buf};
		b[17] = b[18] = 0;
		CHECK(!read_address_seen(b));
	}
	/* Unusable addresses */
	CHECK(!read_address_seen(encode(v4(127, 0, 0, 1, 42424))));
	CHECK(!read_address_seen(encode(v4(0, 0, 0, 0, 42424))));
	CHECK(!read_address_seen(encode(v4(169, 254, 1, 1, 42424))));
	CHECK(!read_address_seen(encode(v4(224, 0, 0, 1, 42424))));
	CHECK(!read_address_seen(encode(v4(255, 255, 255, 255, 42424))));
	CHECK(!read_address_seen(encode(v6({{0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1}}, 42424))));
	CHECK(!read_address_seen(encode(v6({{0xfe, 0x80, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1}}, 42424))));
	/* A LAN address is well formed, just not public. */
	const auto lan{read_address_seen(encode(v4(192, 168, 1, 20, 42424)))};
	CHECK(lan && !seen_address_is_public(*lan));
}

void test_mapped()
{
	/* An IPv4-mapped IPv6 address is the IPv4 address, whichever way it
	 * comes.
	 */
	const auto m{v6({{0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff, 203, 0, 113, 7}}, 42424)};
	CHECK(m.family == 4);
	CHECK(m == v4(203, 0, 113, 7, 42424));
	std::vector<uint8_t> raw(19);
	raw[0] = 6;
	raw[11] = raw[12] = 0xff;
	raw[13] = 203;
	raw[14] = 0;
	raw[15] = 113;
	raw[16] = 7;
	raw[17] = 0xb8;
	raw[18] = 0xa5;
	const auto r{read_address_seen(raw)};
	CHECK(r && *r == v4(203, 0, 113, 7, 42424));
}

void test_text()
{
	CHECK(seen_address_text(v4(203, 0, 113, 7, 42424)) == "203.0.113.7:42424");
	CHECK(seen_address_text(v6({{0x20, 0x01, 0x0d, 0xb8, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x42}}, 42424)) == "[2001:db8::42]:42424");
	CHECK(seen_address_text(v6({{0x20, 0x01, 0x0d, 0xb8, 0, 1, 0, 0, 0, 0, 0, 1, 0, 0, 0, 1}}, 1)) == "[2001:db8:1::1:0:1]:1");
	/* A single zero group stays. */
	CHECK(seen_address_text(v6({{0x20, 0x01, 0x0d, 0xb8, 0, 0, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1}}, 2)) == "[2001:db8:0:1:1:1:1:1]:2");
	/* The first of two equal runs. */
	CHECK(seen_address_text(v6({{0x20, 0x01, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 1, 0, 1}}, 3)) == "[2001::1:0:0:1:1]:3");
	/* The longer of two runs. */
	CHECK(seen_address_text(v6({{0x20, 0x01, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 1}}, 3)) == "[2001:0:0:1::1]:3");
	/* The text pastes back into "join game manually". */
	const auto p{parse_pasted_address(seen_address_text(v6({{0x20, 0x01, 0x0d, 0xb8, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x42}}, 42424)))};
	CHECK(p && p->host == "2001:db8::42" && p->port == 42424);
}

using tally = public_address_tally<8>;

void test_tally()
{
	tally t;
	constexpr int64_t interval{10};
	CHECK(!t.best());
	const auto pub{v4(203, 0, 113, 7, 42424)};
	const auto other{v4(198, 51, 100, 9, 42424)};
	/* The host (slot 0) and slots past the end never count. */
	CHECK(t.report(0, encode(pub), 0, interval) == tally::report_result::malformed);
	CHECK(t.report(8, encode(pub), 0, interval) == tally::report_result::malformed);
	CHECK(!t.best());

	CHECK(t.report(1, encode(pub), 0, interval) == tally::report_result::accepted);
	auto b{t.best()};
	CHECK(b && b->address == pub && b->players == 1 && !b->tracker);

	/* Rate limit: a second report from the slot too soon is dropped,
	 * even a different one.
	 */
	CHECK(t.report(1, encode(other), 5, interval) == tally::report_result::rate_limited);
	CHECK(t.best()->address == pub);
	CHECK(t.report(1, encode(pub), 20, interval) == tally::report_result::unchanged);

	/* Two players see `other`, one `pub`: the majority wins. */
	CHECK(t.report(2, encode(other), 20, interval) == tally::report_result::accepted);
	CHECK(t.report(3, encode(other), 20, interval) == tally::report_result::accepted);
	b = t.best();
	CHECK(b && b->address == other && b->players == 2);

	/* A player on the LAN reports a LAN address: not counted, and it
	 * replaces that slot's earlier public report.
	 */
	CHECK(t.report(3, encode(v4(192, 168, 1, 20, 42424)), 40, interval) == tally::report_result::not_public);
	b = t.best();
	/* 1:1 now, `pub` from the lower slot... both have one vote; neither
	 * is confirmed by the tracker and both are IPv4: the first found.
	 */
	CHECK(b && b->players == 1);
	CHECK(b->address == pub);

	/* The tracker breaks the tie. */
	CHECK(t.set_tracker(other));
	CHECK(!t.set_tracker(other));
	b = t.best();
	CHECK(b && b->address == other && b->players == 1 && b->tracker);

	/* A malformed report consumes the slot's rate limit too. */
	CHECK(t.report(4, std::span<const uint8_t>{}, 50, interval) == tally::report_result::malformed);
	CHECK(t.report(4, encode(pub), 55, interval) == tally::report_result::rate_limited);

	/* The player who left no longer counts; the slot may report at once
	 * for its next occupant.
	 */
	t.forget(2);
	b = t.best();
	CHECK(b && b->players + (b->tracker ? 1u : 0u) == 1);
	CHECK(b->address == other && b->tracker && b->players == 0);
	CHECK(t.report(2, encode(pub), 56, interval) == tally::report_result::accepted);
	b = t.best();
	CHECK(b && b->address == pub && b->players == 2);

	/* The tracker alone, before anyone joined. */
	tally only_tracker;
	CHECK(!only_tracker.set_tracker(v4(10, 0, 0, 1, 42424)));
	CHECK(!only_tracker.best());
	CHECK(only_tracker.set_tracker(pub));
	b = only_tracker.best();
	CHECK(b && b->address == pub && b->players == 0 && b->tracker);

	/* On a tie between IPv6 and IPv4, IPv4. */
	tally mixed;
	const auto six{v6({{0x20, 0x01, 0x0d, 0xb8, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x42}}, 42424)};
	CHECK(mixed.report(1, encode(six), 0, interval) == tally::report_result::accepted);
	CHECK(mixed.report(2, encode(pub), 0, interval) == tally::report_result::accepted);
	CHECK(mixed.best()->address == pub);

	/* Different ports at the same address (a NAT that maps per
	 * destination) are different reports.
	 */
	tally ports;
	CHECK(ports.report(1, encode(v4(203, 0, 113, 7, 1000)), 0, interval) == tally::report_result::accepted);
	CHECK(ports.report(2, encode(v4(203, 0, 113, 7, 2000)), 0, interval) == tally::report_result::accepted);
	CHECK(ports.report(3, encode(v4(203, 0, 113, 7, 2000)), 0, interval) == tally::report_result::accepted);
	CHECK(ports.best()->address.port == 2000 && ports.best()->players == 2);

	ports.clear();
	CHECK(!ports.best());
}

void test_label()
{
	CHECK(public_address_label(1, false) == "Public, seen by 1 player");
	CHECK(public_address_label(3, false) == "Public, seen by 3 players");
	CHECK(public_address_label(2, true) == "Public, seen by 2 players + tracker");
	CHECK(public_address_label(0, true) == "Public, seen by the tracker");
}

}

int main()
{
	test_round_trip();
	test_malformed();
	test_mapped();
	test_text();
	test_tally();
	test_label();
	std::printf("net_public_address: %u checks passed\n", checks);
	return 0;
}
