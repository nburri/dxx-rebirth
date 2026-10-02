/*
 * This file is part of the DXX-Rebirth project <https://github.com/dxx-rebirth/dxx-rebirth/>.
 * It is copyright by its individual contributors, as recorded in the
 * project's Git history.  See COPYING.txt at the top level for license
 * terms and a link to the Git history.
 */
/*
 * Text of game addresses for the clipboard: what a paste into a menu
 * input field keeps, how a pasted "host", "host:port" or "[ipv6]:port"
 * splits into the address and port fields of the manual join menu, and
 * how the host's own addresses are written and ranked for "copy game
 * address".  No game state and no SDL here, so that
 * common/unittest/net_address_text.cpp can test it.
 */

#pragma once

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <string>
#include <string_view>

namespace dcx {

/* The menu's rule for allowed characters: `allowed_chars` is a list of
 * inclusive ranges as pairs of characters ("09" for digits), or null for
 * "anything".
 */
[[nodiscard]]
static inline bool input_char_allowed(const char c, const char *const allowed_chars)
{
	const char *p = allowed_chars;
	if (!p)
		return true;
	for (char a, b; (a = p[0]) && (b = p[1]); p += 2)
		if (c >= a && c <= b)
			return true;
	return false;
}

[[nodiscard]]
static inline bool pasted_space(const char c)
{
	return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\v' || c == '\f';
}

/* The first non-empty line of the clipboard text, without the
 * whitespace around it.  Text copied from a chat window or a web page
 * often carries a trailing newline or leading spaces.
 */
[[nodiscard]]
static inline std::string_view trim_pasted_text(std::string_view text)
{
	for (;;)
	{
		while (!text.empty() && pasted_space(text.front()))
			text.remove_prefix(1);
		if (text.empty())
			return text;
		const auto eol{text.find_first_of("\r\n")};
		auto line{text.substr(0, eol)};
		while (!line.empty() && pasted_space(line.back()))
			line.remove_suffix(1);
		if (!line.empty() || eol == std::string_view::npos)
			return line;
		text.remove_prefix(eol);
	}
}

/* What a paste into an input field adds: the trimmed first line,
 * without characters the field cannot hold (control characters,
 * non-ASCII, and those outside `allowed_chars`; a space becomes '_' when
 * the field takes '_' but no space, as typing does), and at most `room`
 * characters.
 */
[[nodiscard]]
static inline std::string filter_pasted_text(const std::string_view text, const char *const allowed_chars, const std::size_t room)
{
	std::string out;
	for (char c : trim_pasted_text(text))
	{
		if (out.size() >= room)
			break;
		if (c == '\t')
			c = ' ';
		else if (static_cast<unsigned char>(c) < 0x20 || static_cast<unsigned char>(c) > 0x7e)
			continue;
		if (input_char_allowed(c, allowed_chars))
			out.push_back(c);
		else if (c == ' ' && input_char_allowed('_', allowed_chars))
			out.push_back('_');
	}
	return out;
}

/* A pasted game address: the host (a name, an IPv4 address or an IPv6
 * address without brackets) and the port, 0 when the text had none.
 */
struct pasted_address
{
	std::string host;
	uint16_t port{0};
};

[[nodiscard]]
static inline std::optional<uint16_t> parse_pasted_port(const std::string_view s)
{
	if (s.empty() || s.size() > 5)
		return std::nullopt;
	unsigned v{0};
	for (const char c : s)
	{
		if (c < '0' || c > '9')
			return std::nullopt;
		v = v * 10 + static_cast<unsigned>(c - '0');
	}
	if (v == 0 || v > 65535)
		return std::nullopt;
	return static_cast<uint16_t>(v);
}

[[nodiscard]]
static inline bool pasted_host_char_ok(const char c)
{
	return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '-' || c == '_' || c == ':' || c == '%';
}

/* Split clipboard text into host and port.  Accepts "host",
 * "host:port", "[ipv6]", "[ipv6]:port" and a bare IPv6 address (more
 * than one ':', no port); a "udp://" style prefix and a trailing '/' are
 * ignored.  Anything else (spaces inside, a bad port, an empty host) is
 * not an address: nullopt.
 */
[[nodiscard]]
static inline std::optional<pasted_address> parse_pasted_address(const std::string_view text)
{
	auto s{trim_pasted_text(text)};
	if (const auto scheme{s.find("://")}; scheme != std::string_view::npos)
		s.remove_prefix(scheme + 3);
	while (!s.empty() && s.back() == '/')
		s.remove_suffix(1);
	if (s.empty())
		return std::nullopt;
	pasted_address r;
	std::string_view host;
	if (s.front() == '[')
	{
		const auto close{s.find(']')};
		if (close == std::string_view::npos)
			return std::nullopt;
		host = s.substr(1, close - 1);
		const auto rest{s.substr(close + 1)};
		if (!rest.empty())
		{
			if (rest.front() != ':')
				return std::nullopt;
			const auto port{parse_pasted_port(rest.substr(1))};
			if (!port)
				return std::nullopt;
			r.port = *port;
		}
		if (host.find(':') == std::string_view::npos)
			return std::nullopt;
	}
	else if (const auto colon{s.find(':')}; colon != std::string_view::npos && s.find(':', colon + 1) == std::string_view::npos)
	{
		host = s.substr(0, colon);
		const auto port{parse_pasted_port(s.substr(colon + 1))};
		if (!port)
			return std::nullopt;
		r.port = *port;
	}
	else
		host = s;
	if (host.empty() || !std::ranges::all_of(host, pasted_host_char_ok))
		return std::nullopt;
	r.host = host;
	return r;
}

/* How useful one of the host's own addresses is to a player who wants
 * to join, best first.
 */
enum class host_address_kind : uint8_t
{
	/* A public IPv4 address on an interface of this computer: no NAT. */
	public_ipv4,
	/* A private IPv4 address (10/8, 172.16/12, 192.168/16, 100.64/10):
	 * reachable on the LAN or a VPN; from the Internet only through the
	 * router's public address with the port forwarded.
	 */
	lan_ipv4,
	/* A global IPv6 address: reachable from the Internet if no firewall
	 * blocks the port and the joiner has IPv6.
	 */
	public_ipv6,
	/* A unique local IPv6 address (fc00::/7). */
	lan_ipv6,
	/* Loopback, link-local, unspecified, multicast: not offered. */
	unusable,
};

/* `a` in host byte order. */
[[nodiscard]]
static inline host_address_kind classify_ipv4(const uint32_t a)
{
	const unsigned b0{a >> 24}, b1{(a >> 16) & 0xffu};
	if (b0 == 0 || b0 == 127 || b0 >= 224 || (b0 == 169 && b1 == 254))
		return host_address_kind::unusable;
	if (b0 == 10 || (b0 == 172 && (b1 & 0xf0u) == 16) || (b0 == 192 && b1 == 168) || (b0 == 100 && (b1 & 0xc0u) == 64))
		return host_address_kind::lan_ipv4;
	return host_address_kind::public_ipv4;
}

/* `a` is the 16 bytes of an IPv6 address in network order. */
[[nodiscard]]
static inline host_address_kind classify_ipv6(const uint8_t *const a)
{
	if (std::all_of(a, a + 15, [](const uint8_t b) { return b == 0; }))
		/* :: and ::1 */
		return host_address_kind::unusable;
	if (a[0] == 0xfe && (a[1] & 0xc0) == 0x80)
		return host_address_kind::unusable;
	if (a[0] == 0xff)
		return host_address_kind::unusable;
	/* An IPv4-mapped address (::ffff:a.b.c.d): the IPv4 address. */
	if (std::all_of(a, a + 10, [](const uint8_t b) { return b == 0; }) && a[10] == 0xff && a[11] == 0xff)
		return classify_ipv4((uint32_t{a[12]} << 24) | (uint32_t{a[13]} << 16) | (uint32_t{a[14]} << 8) | a[15]);
	if ((a[0] & 0xfe) == 0xfc)
		return host_address_kind::lan_ipv6;
	return host_address_kind::public_ipv6;
}

/* "a.b.c.d:port", or "[v6]:port" when `host` contains ':' (an IPv6
 * address), so that the text pastes back through parse_pasted_address.
 * An IPv4-mapped IPv6 address (what a dual-stack socket reports for an
 * IPv4 peer) is written as the IPv4 address.
 */
[[nodiscard]]
static inline std::string format_address_port(std::string_view host, const uint16_t port)
{
	if (constexpr std::string_view mapped{"::ffff:"}; host.size() > mapped.size() && host.substr(0, mapped.size()) == mapped)
		if (const auto v4{host.substr(mapped.size())}; v4.find('.') != std::string_view::npos && v4.find(':') == std::string_view::npos)
			host = v4;
	std::string r;
	const bool v6{host.find(':') != std::string_view::npos};
	if (v6)
		r += '[';
	r += host;
	if (v6)
		r += ']';
	char buf[8];
	std::snprintf(buf, sizeof(buf), ":%u", static_cast<unsigned>(port));
	r += buf;
	return r;
}

}
