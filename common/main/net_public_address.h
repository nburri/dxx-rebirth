/*
 * This file is part of the DXX-Rebirth project <https://github.com/dxx-rebirth/dxx-rebirth/>.
 * It is copyright by its individual contributors, as recorded in the
 * project's Git history.  See COPYING.txt at the top level for license
 * terms and a link to the Git history.
 */
/*
 * The host's public address (Documentation/network-protocol-v2.md,
 * section 4.6, "ADDRESS_SEEN", protocol 108).  Behind a NAT router the
 * host only knows its LAN addresses; the address a player on the
 * Internet must type is the router's, which only the outside sees.
 * Each client tells the host the address and port it reaches the host
 * at (ADDRESS_SEEN, once per join), the tracker reveals the address it
 * sees the host's game socket at, and the host shows the address most
 * of them agree on.
 *
 * The wire layout of ADDRESS_SEEN, its checks, the text of an address
 * and the choice among the reports live here, without game state or
 * sockets, so that common/unittest/net_public_address.cpp can test them.
 */

#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <span>
#include <string>

#include "net_address_text.h"

namespace dcx {

/* An address and port of the host as someone outside sees it.  IPv4 in
 * bytes[0..3] (network order), the rest zero; an IPv4-mapped IPv6
 * address is stored as IPv4.
 */
struct seen_address
{
	uint8_t family{};	/* 4 or 6 */
	std::array<uint8_t, 16> bytes{};
	uint16_t port{};
	constexpr bool operator==(const seen_address &) const = default;
};

[[nodiscard]]
static inline seen_address make_seen_ipv4(const std::array<uint8_t, 4> &a, const uint16_t port)
{
	seen_address r;
	r.family = 4;
	std::copy(a.begin(), a.end(), r.bytes.begin());
	r.port = port;
	return r;
}

/* `a` is the 16 bytes of an IPv6 address in network order. */
[[nodiscard]]
static inline seen_address make_seen_ipv6(const uint8_t *const a, const uint16_t port)
{
	if (std::all_of(a, a + 10, [](const uint8_t b) { return b == 0; }) && a[10] == 0xff && a[11] == 0xff)
		return make_seen_ipv4({{a[12], a[13], a[14], a[15]}}, port);
	seen_address r;
	r.family = 6;
	std::copy(a, a + 16, r.bytes.begin());
	r.port = port;
	return r;
}

[[nodiscard]]
static inline host_address_kind seen_address_kind(const seen_address &s)
{
	if (s.family == 4)
		return classify_ipv4((uint32_t{s.bytes[0]} << 24) | (uint32_t{s.bytes[1]} << 16) | (uint32_t{s.bytes[2]} << 8) | s.bytes[3]);
	if (s.family == 6)
		return classify_ipv6(s.bytes.data());
	return host_address_kind::unusable;
}

/* Only an Internet address is worth showing as "public": a player on the
 * LAN reports the LAN address the host already knows.
 */
[[nodiscard]]
static inline bool seen_address_is_public(const seen_address &s)
{
	const auto k{seen_address_kind(s)};
	return s.port != 0 && (k == host_address_kind::public_ipv4 || k == host_address_kind::public_ipv6);
}

/* "a.b.c.d:port" or "[v6]:port", IPv6 in the RFC 5952 short form, so
 * that it pastes back through parse_pasted_address.
 */
[[nodiscard]]
static inline std::string seen_address_text(const seen_address &s)
{
	char buf[48];
	if (s.family == 4)
	{
		std::snprintf(buf, sizeof(buf), "%u.%u.%u.%u", s.bytes[0], s.bytes[1], s.bytes[2], s.bytes[3]);
		return format_address_port(buf, s.port);
	}
	std::array<unsigned, 8> w;
	for (std::size_t i = 0; i < 8; ++i)
		w[i] = (unsigned{s.bytes[2 * i]} << 8) | s.bytes[2 * i + 1];
	/* The longest run of two or more zero groups becomes "::". */
	std::size_t best_at{8}, best_len{0};
	for (std::size_t i = 0; i < 8;)
	{
		if (w[i])
		{
			++i;
			continue;
		}
		std::size_t j{i};
		while (j < 8 && !w[j])
			++j;
		if (j - i > best_len && j - i >= 2)
		{
			best_at = i;
			best_len = j - i;
		}
		i = j;
	}
	std::string text;
	for (std::size_t i = 0; i < 8;)
	{
		if (i == best_at)
		{
			text += "::";
			i += best_len;
			continue;
		}
		if (!text.empty() && text.back() != ':')
			text += ':';
		std::snprintf(buf, sizeof(buf), "%x", w[i]);
		text += buf;
		++i;
	}
	return format_address_port(text, s.port);
}

/* ADDRESS_SEEN (session message 0x0F, reliable, client to host, once
 * after JOIN_ACCEPT): family u8 (4 or 6), the address (16 bytes; IPv4 in
 * the first 4, the rest zero), the port u16 little endian.
 */
constexpr std::size_t NET_V2_ADDRESS_SEEN_SIZE{19};

static inline void write_address_seen(const seen_address &s, uint8_t *const out)
{
	out[0] = s.family;
	std::copy(s.bytes.begin(), s.bytes.end(), out + 1);
	out[17] = static_cast<uint8_t>(s.port & 0xffu);
	out[18] = static_cast<uint8_t>(s.port >> 8);
}

/* Untrusted input: the exact size, a known family, IPv4 padded with
 * zeros, a port, and an address that can be reached at all (not
 * loopback, link-local, multicast or unspecified).
 */
[[nodiscard]]
static inline std::optional<seen_address> read_address_seen(const std::span<const uint8_t> in)
{
	if (in.size() != NET_V2_ADDRESS_SEEN_SIZE)
		return std::nullopt;
	seen_address s;
	s.family = in[0];
	std::copy(in.begin() + 1, in.begin() + 17, s.bytes.begin());
	s.port = static_cast<uint16_t>(in[17] | (in[18] << 8));
	if (s.family == 4)
	{
		if (!std::all_of(s.bytes.begin() + 4, s.bytes.end(), [](const uint8_t b) { return b == 0; }))
			return std::nullopt;
	}
	else if (s.family == 6)
		/* An IPv4-mapped address sent as IPv6: the same address. */
		s = make_seen_ipv6(s.bytes.data(), s.port);
	else
		return std::nullopt;
	if (!s.port || seen_address_kind(s) == host_address_kind::unusable)
		return std::nullopt;
	return s;
}

/* The host's tally of the reports: the newest public report of each
 * connected player and the tracker's.  `slots` is MAX_PLAYERS; slot 0
 * (the host) never reports.  Times are in the caller's units (fix64).
 */
template <std::size_t slots>
class public_address_tally
{
	std::array<std::optional<seen_address>, slots> peer_report{};
	std::array<int64_t, slots> last_report{};
	std::array<bool, slots> reported{};
	std::optional<seen_address> tracker_report;
public:
	enum class report_result : uint8_t
	{
		accepted,
		/* Same as the slot's last report: nothing changed. */
		unchanged,
		/* A LAN address (a player on the same network): kept out of the
		 * tally, which is about the Internet address.
		 */
		not_public,
		malformed,
		/* Too soon after the slot's last report. */
		rate_limited,
	};
	/* A report from the player in `slot`, whose connection the caller
	 * has authenticated.  At most one per `min_interval`.
	 */
	report_result report(const std::size_t slot, const std::span<const uint8_t> payload, const int64_t now, const int64_t min_interval)
	{
		if (slot == 0 || slot >= slots)
			return report_result::malformed;
		if (reported[slot] && now - last_report[slot] < min_interval)
			return report_result::rate_limited;
		reported[slot] = true;
		last_report[slot] = now;
		const auto s{read_address_seen(payload)};
		if (!s)
			return report_result::malformed;
		if (!seen_address_is_public(*s))
		{
			peer_report[slot].reset();
			return report_result::not_public;
		}
		if (peer_report[slot] == *s)
			return report_result::unchanged;
		peer_report[slot] = *s;
		return report_result::accepted;
	}
	/* The player left: its report no longer counts, and a new player in
	 * the slot may report at once.
	 */
	void forget(const std::size_t slot)
	{
		if (slot >= slots)
			return;
		peer_report[slot].reset();
		reported[slot] = false;
	}
	/* The tracker listed this game at `s`; false if it is not public or
	 * not new.
	 */
	bool set_tracker(const seen_address &s)
	{
		if (!seen_address_is_public(s) || tracker_report == s)
			return false;
		tracker_report = s;
		return true;
	}
	void clear()
	{
		*this = {};
	}
	struct choice
	{
		seen_address address;
		/* Players whose report is this address. */
		unsigned players;
		/* The tracker sees the game there too. */
		bool tracker;
	};
	/* The address most reports agree on (the tracker counts as one
	 * report): on a tie the one the tracker confirms, then IPv4 (what
	 * most players can reach), then the lowest slot's.
	 */
	[[nodiscard]]
	std::optional<choice> best() const
	{
		std::optional<choice> r;
		const auto consider = [&](const seen_address &a) {
			choice c{a, 0, tracker_report == a};
			for (const auto &p : peer_report)
				if (p == a)
					++c.players;
			if (!r)
			{
				r = c;
				return;
			}
			const auto votes = [](const choice &x) { return x.players + (x.tracker ? 1u : 0u); };
			if (votes(c) != votes(*r))
			{
				if (votes(c) > votes(*r))
					r = c;
				return;
			}
			if (c.tracker != r->tracker)
			{
				if (c.tracker)
					r = c;
				return;
			}
			if (c.address.family == 4 && r->address.family != 4)
				r = c;
		};
		for (const auto &p : peer_report)
			if (p)
				consider(*p);
		if (tracker_report)
			consider(*tracker_report);
		return r;
	}
};

/* "Public, seen by 2 players and the tracker" and the like. */
[[nodiscard]]
static inline std::string public_address_label(const unsigned players, const bool tracker)
{
	char buf[64];
	if (players && tracker)
		std::snprintf(buf, sizeof(buf), "Public, seen by %u player%s + tracker", players, players == 1 ? "" : "s");
	else if (players)
		std::snprintf(buf, sizeof(buf), "Public, seen by %u player%s", players, players == 1 ? "" : "s");
	else
		std::snprintf(buf, sizeof(buf), "Public, seen by the tracker");
	return buf;
}

}
