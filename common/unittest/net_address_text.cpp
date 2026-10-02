/*
 * This file is part of the DXX-Rebirth project <https://github.com/dxx-rebirth/dxx-rebirth/>.
 * It is copyright by its individual contributors, as recorded in the
 * project's Git history.  See COPYING.txt at the top level for license
 * terms and a link to the Git history.
 */
/*
 * Test of the text of game addresses on the clipboard
 * (net_address_text.h): trimming and filtering a paste for a menu
 * input field, splitting a pasted "host:port", and writing and ranking
 * the host's own addresses for "copy game address".
 *
 * Build and run with SCons:
 *
 *	scons sdl2=1 d1x=0 d2x=1 register_runtime_test_plain_link_targets=1 test-net-address-text
 *	build/common/test-net-address-text
 */

#include <cstdio>
#include <cstdlib>
#include <string>

#include "net_address_text.h"

using namespace dcx;

namespace {

unsigned checks;

void check_failed(const char *const expr, const char *const file, const int line)
{
	std::fprintf(stderr, "%s:%d: check failed: %s\n", file, line, expr);
	std::exit(1);
}

#define CHECK(cond)	do { ++checks; if (!(cond)) check_failed(#cond, __FILE__, __LINE__); } while (0)

bool parses_to(const char *const text, const char *const host, const uint16_t port)
{
	const auto a{parse_pasted_address(text)};
	return a && a->host == host && a->port == port;
}

bool rejected(const char *const text)
{
	return !parse_pasted_address(text);
}

void test_trim()
{
	CHECK(trim_pasted_text("  1.2.3.4:42424\r\n") == "1.2.3.4:42424");
	CHECK(trim_pasted_text("\n\n  host \nsecond line") == "host");
	CHECK(trim_pasted_text("\t\r\n ") == "");
	CHECK(trim_pasted_text("") == "");
	CHECK(trim_pasted_text("one") == "one");
	CHECK(trim_pasted_text("a b\r\nc") == "a b");
}

void test_parse()
{
	CHECK(parses_to("example.org", "example.org", 0));
	CHECK(parses_to("192.168.1.5", "192.168.1.5", 0));
	CHECK(parses_to("192.168.1.5:42424", "192.168.1.5", 42424));
	CHECK(parses_to("  203.0.113.7:1\n", "203.0.113.7", 1));
	CHECK(parses_to("host.example:65535", "host.example", 65535));
	CHECK(parses_to("[2001:db8::1]:42424", "2001:db8::1", 42424));
	CHECK(parses_to("[2001:db8::1]", "2001:db8::1", 0));
	CHECK(parses_to("2001:db8::1", "2001:db8::1", 0));
	CHECK(parses_to("::1", "::1", 0));
	CHECK(parses_to("[fe80::1%eth0]:5000", "fe80::1%eth0", 5000));
	CHECK(parses_to("udp://1.2.3.4:42425/", "1.2.3.4", 42425));
	CHECK(parses_to("localhost", "localhost", 0));
	CHECK(parses_to("42424", "42424", 0));
	CHECK(rejected(""));
	CHECK(rejected("   \r\n"));
	CHECK(rejected("1.2.3.4:"));
	CHECK(rejected("1.2.3.4:0"));
	CHECK(rejected("1.2.3.4:65536"));
	CHECK(rejected("1.2.3.4:123456"));
	CHECK(rejected("1.2.3.4:42a"));
	CHECK(rejected(":42424"));
	CHECK(rejected("[2001:db8::1"));
	CHECK(rejected("[2001:db8::1]42424"));
	CHECK(rejected("[2001:db8::1]:"));
	CHECK(rejected("[1.2.3.4]:42424"));
	CHECK(rejected("[]:42424"));
	CHECK(rejected("join me at 1.2.3.4"));
	CHECK(rejected("host name:42424"));
}

void test_filter()
{
	/* No allowed set: printable ASCII; control and high characters go. */
	CHECK(filter_pasted_text("  abc\x01" "d\xe9" "e \n", nullptr, 100) == "abcde");
	CHECK(filter_pasted_text("a\tb", nullptr, 100) == "a b");
	/* The room limits the length. */
	CHECK(filter_pasted_text("1234567890", nullptr, 5) == "12345");
	CHECK(filter_pasted_text("1234567890", nullptr, 0) == "");
	/* Digits only. */
	CHECK(filter_pasted_text(" 42 424\n", "09", 10) == "42424");
	CHECK(filter_pasted_text("4242424", "09", 5) == "42424");
	/* A space becomes '_' when the field takes '_' but no space. */
	CHECK(filter_pasted_text("my pilot", "azAZ09__", 20) == "my_pilot");
	CHECK(filter_pasted_text("my pilot!", "azAZ", 20) == "mypilot");
	CHECK(input_char_allowed('5', "09"));
	CHECK(!input_char_allowed('a', "09"));
	CHECK(input_char_allowed('a', nullptr));
}

void test_classify()
{
	const auto v4 = [](const unsigned a, const unsigned b, const unsigned c, const unsigned d) {
		return classify_ipv4((uint32_t{a} << 24) | (uint32_t{b} << 16) | (uint32_t{c} << 8) | uint32_t{d});
	};
	CHECK(v4(127, 0, 0, 1) == host_address_kind::unusable);
	CHECK(v4(0, 0, 0, 0) == host_address_kind::unusable);
	CHECK(v4(169, 254, 3, 4) == host_address_kind::unusable);
	CHECK(v4(224, 0, 0, 1) == host_address_kind::unusable);
	CHECK(v4(255, 255, 255, 255) == host_address_kind::unusable);
	CHECK(v4(10, 1, 2, 3) == host_address_kind::lan_ipv4);
	CHECK(v4(172, 16, 0, 1) == host_address_kind::lan_ipv4);
	CHECK(v4(172, 31, 255, 1) == host_address_kind::lan_ipv4);
	CHECK(v4(172, 32, 0, 1) == host_address_kind::public_ipv4);
	CHECK(v4(192, 168, 1, 5) == host_address_kind::lan_ipv4);
	CHECK(v4(100, 64, 0, 1) == host_address_kind::lan_ipv4);
	CHECK(v4(100, 128, 0, 1) == host_address_kind::public_ipv4);
	CHECK(v4(203, 0, 113, 7) == host_address_kind::public_ipv4);
	CHECK(v4(8, 8, 8, 8) == host_address_kind::public_ipv4);

	uint8_t a[16]{};
	CHECK(classify_ipv6(a) == host_address_kind::unusable);
	a[15] = 1;
	CHECK(classify_ipv6(a) == host_address_kind::unusable);
	a[0] = 0xfe; a[1] = 0x80;
	CHECK(classify_ipv6(a) == host_address_kind::unusable);
	a[0] = 0xff; a[1] = 0x02;
	CHECK(classify_ipv6(a) == host_address_kind::unusable);
	a[0] = 0xfd; a[1] = 0x12;
	CHECK(classify_ipv6(a) == host_address_kind::lan_ipv6);
	a[0] = 0x20; a[1] = 0x01;
	CHECK(classify_ipv6(a) == host_address_kind::public_ipv6);
	const uint8_t mapped[16]{0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff, 192, 168, 0, 9};
	CHECK(classify_ipv6(mapped) == host_address_kind::lan_ipv4);
	/* The ranking is the enum order. */
	CHECK(host_address_kind::public_ipv4 < host_address_kind::lan_ipv4);
	CHECK(host_address_kind::lan_ipv4 < host_address_kind::public_ipv6);
}

void test_format_round_trip()
{
	CHECK(format_address_port("192.168.1.5", 42424) == "192.168.1.5:42424");
	CHECK(format_address_port("2001:db8::1", 42424) == "[2001:db8::1]:42424");
	CHECK(format_address_port("::ffff:203.0.113.7", 42424) == "203.0.113.7:42424");
	CHECK(format_address_port("::ffff:1:2", 42424) == "[::ffff:1:2]:42424");
	for (const char *const h : {"192.168.1.5", "2001:db8::1", "example.org", "fd12::abcd"})
	{
		const auto text{format_address_port(h, 1234)};
		CHECK(parses_to(text.c_str(), h, 1234));
	}
}

}

int main()
{
	test_trim();
	test_parse();
	test_filter();
	test_classify();
	test_format_round_trip();
	std::printf("net_address_text: %u checks passed\n", checks);
	return 0;
}
