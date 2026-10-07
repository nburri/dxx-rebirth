/*
 * This file is part of the DXX-Rebirth project <https://github.com/dxx-rebirth/dxx-rebirth/>.
 * It is copyright by its individual contributors, as recorded in the
 * project's Git history.  See COPYING.txt at the top level for license
 * terms and a link to the Git history.
 */
/*
 * Mission transfer (Documentation/network-protocol-v2.md section 8,
 * "Mission transfer", protocol 115): the host sends the files of its
 * mission to a client that lacks them, or has another version of them,
 * before the client loads the level.
 *
 * A mission is a "bundle": its .mn2 and, if it has one, its .hog (a HOG
 * carries the levels and whatever custom data the mission has: .pog,
 * .hxm, .ham, .s11/.s22, briefings).  The bundle is named by the SHA-256
 * of its manifest (the files' names, sizes and SHA-256s), announced in
 * GAME_SETTINGS; a client that has a mission of that file name with that
 * bundle hash plays it, otherwise it gets the manifest (MISSION_MANIFEST,
 * from the host on every new connection) and the files as asset kind 3
 * (ASSET_REQUEST with a resume offset, ASSET_DATA in chunks of up to
 * MISSION_DATA_CHUNK bytes, ASSET_UNAVAILABLE).
 *
 * Message layouts, the checks of every untrusted field (names, sizes,
 * HOG and MN2 structure), the pacing, and both ends as state machines
 * over an interface to the game and the transport, so that
 * common/unittest/net_v2_mission.cpp runs them over simulated links.
 * Depends on the standard library, sha256.h and net_v2.h only.
 */

#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "net_v2.h"
#include "sha256.h"

namespace dcx {

namespace net_v2 {

/* The asset kind of a mission file (net_v2_ships.h has 1 ships, 2
 * taunts); its messages use the ASSET_* ids with this kind byte.
 */
constexpr std::uint8_t MISSION_ASSET_KIND{3};
constexpr std::uint8_t MISSION_MSG_REQUEST{0x4c};
constexpr std::uint8_t MISSION_MSG_DATA{0x4d};
constexpr std::uint8_t MISSION_MSG_UNAVAILABLE{0x4e};
/* MISSION_MANIFEST, host to client. */
constexpr std::uint8_t MISSION_MSG_MANIFEST{0x52};

using mission_hash = std::array<std::uint8_t, 32>;

/* Caps.  The group's largest mission (The Enemy Within, 27.4 MiB HOG)
 * fits with margin.
 */
constexpr std::uint32_t MISSION_FILE_MAX{48u << 20};
constexpr std::uint32_t MISSION_BUNDLE_MAX{48u << 20};
constexpr std::uint32_t MISSION_MN2_MAX{64u << 10};
constexpr std::size_t MISSION_MAX_FILES{2};
/* HOG entries: name (13 bytes), size (4). */
constexpr std::size_t MISSION_HOG_MAX_ENTRIES{4096};
/* The mission's file name without extension: as in GAME_SETTINGS, 8
 * characters at most.
 */
constexpr std::size_t MISSION_BASENAME_MAX{8};
/* A file name: basename, '.', extension, NUL padded. */
constexpr std::size_t MISSION_FILE_NAME_FIELD{13};
constexpr std::size_t MISSION_TITLE_FIELD{26};

/* ASSET_REQUEST (kind 3): kind, file hash, resume offset. */
constexpr std::size_t MISSION_REQUEST_SIZE{1 + 32 + 4};
/* ASSET_DATA (kind 3): kind, file hash, file size, offset, the bytes. */
constexpr std::size_t MISSION_DATA_HEADER{1 + 32 + 4 + 4};
constexpr std::size_t MISSION_DATA_CHUNK{NET_V2_MAX_MESSAGE - MISSION_DATA_HEADER};
/* ASSET_UNAVAILABLE (kind 3): kind, file hash, reason. */
constexpr std::size_t MISSION_UNAVAILABLE_SIZE{1 + 32 + 1};

/* Pacing (bytes per second).  In the lobby the transfer may use the
 * link: it starts at MISSION_RATE_START and grows while the round trip
 * stays near the lowest seen (no queue builds up anywhere on the path),
 * and shrinks when the round trip rises (a queue builds: the uplink is
 * full, and the other players' packets would wait behind ours) or when
 * much is lost.  Random loss alone does not slow it down: the
 * retransmissions cost what they cost.  During a level (a player joining
 * a game in progress) it stays at or below MISSION_RATE_LEVEL so that
 * the players in the level do not suffer.
 */
constexpr double MISSION_RATE_START{512.0 * 1024};
constexpr double MISSION_RATE_MIN{64.0 * 1024};
constexpr double MISSION_RATE_MAX{4.0 * 1024 * 1024};
constexpr double MISSION_RATE_LEVEL{192.0 * 1024};
constexpr double MISSION_RATE_ADJUST_INTERVAL{0.25};
/* Retransmitted share of the messages sent in an interval above which
 * the rate shrinks whatever the round trip.
 */
constexpr double MISSION_LOSS_HIGH{0.15};
/* The round trip above which a queue is building: the lowest seen during
 * the transfer, times MISSION_RTT_RISE_FACTOR, plus MISSION_RTT_RISE.
 */
constexpr double MISSION_RTT_RISE_FACTOR{1.25};
constexpr double MISSION_RTT_RISE{0.015};
/* Bytes the transfer keeps queued or in flight on the connection: the
 * window.  The lobby's is large enough for 2 MB/s at 100 ms; during a
 * level the window stays small, so that gameplay messages never wait
 * long behind it.
 */
constexpr std::size_t MISSION_WINDOW_LOBBY{224 * 1024};
constexpr std::size_t MISSION_WINDOW_LEVEL{32 * 1024};
/* Packets per tick on a connection that carries a transfer
 * (connection::set_max_packets_per_tick): in the lobby 32 at 60 Hz, up to
 * some 1.9 MB/s; during a level 4, enough for MISSION_RATE_LEVEL.
 */
constexpr unsigned MISSION_BULK_PACKETS_PER_TICK{32};
constexpr unsigned MISSION_LEVEL_PACKETS_PER_TICK{4};
/* A client gives up when the manifest does not come, or when no data
 * comes for this long while it waits for some.
 */
constexpr double MISSION_MANIFEST_TIMEOUT{15};
constexpr double MISSION_STALL_TIMEOUT{20};

[[nodiscard]]
constexpr char mission_lower(const char c)
{
	return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
}

[[nodiscard]]
inline std::string mission_lower(const std::string_view s)
{
	std::string r;
	r.reserve(s.size());
	for (const auto c : s)
		r.push_back(mission_lower(c));
	return r;
}

[[nodiscard]]
constexpr bool mission_name_char(const char c)
{
	return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
}

/* A mission's file name without folder or extension: 1..8 of
 * [A-Za-z0-9_-] (no path, no dot, nothing a file system could misread).
 */
[[nodiscard]]
constexpr bool mission_basename_valid(const std::string_view s)
{
	if (s.empty() || s.size() > MISSION_BASENAME_MAX)
		return false;
	for (const auto c : s)
		if (!mission_name_char(c))
			return false;
	return true;
}

enum class mission_file_type : std::uint8_t
{
	mn2,
	hog,
};

/* The type of a bundle file name ("<basename>.mn2" or ".hog", any case),
 * or nothing: anything else is not a file this transfer moves.
 */
[[nodiscard]]
inline std::optional<mission_file_type> mission_file_type_of(const std::string_view name)
{
	const auto dot{name.find('.')};
	if (dot == std::string_view::npos || !mission_basename_valid(name.substr(0, dot)))
		return std::nullopt;
	const auto ext{mission_lower(name.substr(dot + 1))};
	if (ext == "mn2")
		return mission_file_type::mn2;
	if (ext == "hog")
		return mission_file_type::hog;
	return std::nullopt;
}

/* A HOG: "DHF", then entries of a name (13 bytes, NUL terminated) and a
 * size (u32), each followed by its data, to the exact end of the file.
 * Every entry within the file, every name a plain file name, at most
 * MISSION_HOG_MAX_ENTRIES of them.
 */
[[nodiscard]]
inline bool mission_hog_valid(const std::span<const std::uint8_t> b)
{
	if (b.size() < 3 || b[0] != 'D' || b[1] != 'H' || b[2] != 'F')
		return false;
	std::size_t at{3}, entries{};
	while (at != b.size())
	{
		if (b.size() - at < 17 || ++entries > MISSION_HOG_MAX_ENTRIES)
			return false;
		/* A name: 1..12 printable characters, no path separators, then
		 * NUL within the 13 bytes.
		 */
		std::size_t n{};
		while (n != 13 && b[at + n])
		{
			const auto c{b[at + n]};
			if (c < 0x21 || c > 0x7e || c == '/' || c == '\\' || c == ':')
				return false;
			++n;
		}
		if (!n || n == 13)
			return false;
		const std::size_t size{net_get_le32(&b[at + 13])};
		at += 17;
		if (size > b.size() - at)
			return false;
		at += size;
	}
	return true;
}

/* An MN2: text without NUL bytes whose first line names the mission
 * ("name", "xname", "zname" or "!name", as mission.cpp reads it).
 */
[[nodiscard]]
inline bool mission_mn2_valid(const std::span<const std::uint8_t> b)
{
	if (b.empty() || b.size() > MISSION_MN2_MAX || std::ranges::find(b, std::uint8_t{0}) != b.end())
		return false;
	std::size_t i{};
	while (i != b.size() && (b[i] == ' ' || b[i] == '\t' || b[i] == '\r' || b[i] == '\n'))
		++i;
	const auto starts{[&](const std::string_view t) {
		if (b.size() - i < t.size())
			return false;
		for (std::size_t k{}; k != t.size(); ++k)
			if (mission_lower(static_cast<char>(b[i + k])) != t[k])
				return false;
		return true;
	}};
	return starts("name") || starts("xname") || starts("zname") || starts("!name");
}

/* A file's own checks, by its type. */
[[nodiscard]]
inline bool mission_file_content_valid(const mission_file_type t, const std::span<const std::uint8_t> b)
{
	return t == mission_file_type::hog ? mission_hog_valid(b) : mission_mn2_valid(b);
}

struct mission_file
{
	std::string name;
	std::uint32_t size{};
	mission_hash hash{};
	[[nodiscard]]
	mission_file_type type() const
	{
		return mission_file_type_of(name).value_or(mission_file_type::mn2);
	}
};

/* The bundle: the mission's file name (without extension), its title,
 * and its files.  The bundle hash covers the files only, in name order
 * (case ignored): their names, sizes and contents.
 */
struct mission_manifest
{
	std::string basename;
	std::string title;
	std::vector<mission_file> files;
	[[nodiscard]]
	std::uint64_t total_size() const
	{
		std::uint64_t t{};
		for (const auto &f : files)
			t += f.size;
		return t;
	}
	/* Sort the files into the canonical order (case-insensitive names). */
	void normalise()
	{
		std::ranges::sort(files, [](const mission_file &a, const mission_file &b) {
			return mission_lower(a.name) < mission_lower(b.name);
		});
	}
	[[nodiscard]]
	mission_hash bundle_hash() const
	{
		auto sorted{*this};
		sorted.normalise();
		sha256 h;
		static constexpr char tag[]{"DXX-MISSION-BUNDLE-1"};
		h.update(std::span(reinterpret_cast<const std::uint8_t *>(tag), sizeof(tag)));
		for (const auto &f : sorted.files)
		{
			const auto n{mission_lower(f.name)};
			h.update(std::span(reinterpret_cast<const std::uint8_t *>(n.data()), n.size() + 1));
			std::array<std::uint8_t, 4> s;
			net_put_le32(s.data(), f.size);
			h.update(s);
			h.update(f.hash);
		}
		return h.finish();
	}
	/* Every rule for a bundle: a valid basename, a title, one .mn2 and at
	 * most one .hog, both of that basename, no file empty or over its
	 * cap, the total within MISSION_BUNDLE_MAX.
	 */
	[[nodiscard]]
	bool valid() const
	{
		if (!mission_basename_valid(basename) || title.size() >= MISSION_TITLE_FIELD || files.empty() || files.size() > MISSION_MAX_FILES)
			return false;
		for (const auto c : title)
			if (static_cast<unsigned char>(c) < 0x20 || c == 0x7f)
				return false;
		unsigned mn2{}, hog{};
		const auto base{mission_lower(basename)};
		for (const auto &f : files)
		{
			const auto t{mission_file_type_of(f.name)};
			if (!t || mission_lower(f.name.substr(0, f.name.find('.'))) != base || !f.size || f.size > MISSION_FILE_MAX)
				return false;
			if (*t == mission_file_type::mn2)
			{
				if (f.size > MISSION_MN2_MAX)
					return false;
				++mn2;
			}
			else
				++hog;
		}
		return mn2 == 1 && hog <= 1 && total_size() <= MISSION_BUNDLE_MAX;
	}
	[[nodiscard]]
	const mission_file *find(const mission_hash &h) const
	{
		for (const auto &f : files)
			if (f.hash == h)
				return &f;
		return nullptr;
	}
};

/* MISSION_MANIFEST: bundle hash, basename (9, NUL padded), title (26),
 * file count, then per file its name (13), size (u32) and hash (32).
 * The receiver recomputes the bundle hash: a manifest that does not
 * hash to what it claims is refused.
 */
struct mission_manifest_msg
{
	static constexpr std::size_t FIXED{32 + (MISSION_BASENAME_MAX + 1) + MISSION_TITLE_FIELD + 1};
	static constexpr std::size_t PER_FILE{MISSION_FILE_NAME_FIELD + 4 + 32};
	static constexpr std::size_t MAX_SIZE{FIXED + MISSION_MAX_FILES * PER_FILE};
	static_assert(MAX_SIZE <= NET_V2_MAX_MESSAGE);
	[[nodiscard]]
	static std::vector<std::uint8_t> write(const mission_manifest &m)
	{
		std::vector<std::uint8_t> b(FIXED + m.files.size() * PER_FILE);
		const auto h{m.bundle_hash()};
		std::ranges::copy(h, b.begin());
		std::size_t at{32};
		const auto put_text{[&](const std::string &s, const std::size_t field) {
			std::copy_n(s.data(), std::min(s.size(), field - 1), b.begin() + at);
			at += field;
		}};
		put_text(m.basename, MISSION_BASENAME_MAX + 1);
		put_text(m.title, MISSION_TITLE_FIELD);
		b[at++] = static_cast<std::uint8_t>(m.files.size());
		for (const auto &f : m.files)
		{
			put_text(f.name, MISSION_FILE_NAME_FIELD);
			net_put_le32(&b[at], f.size);
			at += 4;
			std::ranges::copy(f.hash, b.begin() + at);
			at += 32;
		}
		return b;
	}
	[[nodiscard]]
	static std::optional<mission_manifest> read(const std::span<const std::uint8_t> b)
	{
		if (b.size() < FIXED)
			return std::nullopt;
		const auto count{b[FIXED - 1]};
		if (!count || count > MISSION_MAX_FILES || b.size() != FIXED + count * PER_FILE)
			return std::nullopt;
		std::size_t at{32};
		bool ok{true};
		const auto get_text{[&](const std::size_t field) {
			const auto f{b.subspan(at, field)};
			at += field;
			const auto nul{std::ranges::find(f, std::uint8_t{0})};
			if (nul == f.end())
			{
				ok = false;
				return std::string{};
			}
			/* Nothing after the NUL: one encoding per manifest. */
			if (std::any_of(nul, f.end(), [](const std::uint8_t c) { return c != 0; }))
				ok = false;
			return std::string(f.begin(), nul);
		}};
		mission_manifest m;
		m.basename = get_text(MISSION_BASENAME_MAX + 1);
		m.title = get_text(MISSION_TITLE_FIELD);
		++at;
		for (unsigned i{}; i != count; ++i)
		{
			mission_file f;
			f.name = get_text(MISSION_FILE_NAME_FIELD);
			f.size = net_get_le32(&b[at]);
			at += 4;
			std::copy_n(&b[at], 32, f.hash.begin());
			at += 32;
			m.files.push_back(std::move(f));
		}
		if (!ok || !m.valid())
			return std::nullopt;
		/* No two files of the same name or the same content. */
		for (std::size_t i{}; i != m.files.size(); ++i)
			for (std::size_t j{i + 1}; j != m.files.size(); ++j)
				if (mission_lower(m.files[i].name) == mission_lower(m.files[j].name) || m.files[i].hash == m.files[j].hash)
					return std::nullopt;
		mission_hash claimed;
		std::copy_n(b.begin(), 32, claimed.begin());
		if (m.bundle_hash() != claimed)
			return std::nullopt;
		return m;
	}
};

struct mission_request_msg
{
	mission_hash hash{};
	std::uint32_t offset{};
	void write(std::uint8_t *const p) const
	{
		p[0] = MISSION_ASSET_KIND;
		std::ranges::copy(hash, p + 1);
		net_put_le32(p + 33, offset);
	}
	[[nodiscard]]
	static std::optional<mission_request_msg> read(const std::span<const std::uint8_t> b)
	{
		if (b.size() != MISSION_REQUEST_SIZE || b[0] != MISSION_ASSET_KIND)
			return std::nullopt;
		mission_request_msg m;
		std::copy_n(&b[1], 32, m.hash.begin());
		m.offset = net_get_le32(&b[33]);
		if (m.offset >= MISSION_FILE_MAX)
			return std::nullopt;
		return m;
	}
};

struct mission_data_msg
{
	mission_hash hash{};
	std::uint32_t total{}, offset{};
	std::span<const std::uint8_t> data;
	[[nodiscard]]
	std::size_t size() const
	{
		return MISSION_DATA_HEADER + data.size();
	}
	void write(std::uint8_t *const p) const
	{
		p[0] = MISSION_ASSET_KIND;
		std::ranges::copy(hash, p + 1);
		net_put_le32(p + 33, total);
		net_put_le32(p + 37, offset);
		std::ranges::copy(data, p + MISSION_DATA_HEADER);
	}
	[[nodiscard]]
	static std::optional<mission_data_msg> read(const std::span<const std::uint8_t> b)
	{
		if (b.size() <= MISSION_DATA_HEADER || b.size() > MISSION_DATA_HEADER + MISSION_DATA_CHUNK || b[0] != MISSION_ASSET_KIND)
			return std::nullopt;
		mission_data_msg m;
		std::copy_n(&b[1], 32, m.hash.begin());
		m.total = net_get_le32(&b[33]);
		m.offset = net_get_le32(&b[37]);
		m.data = b.subspan(MISSION_DATA_HEADER);
		if (!m.total || m.total > MISSION_FILE_MAX || m.offset >= m.total || m.data.size() > m.total - m.offset)
			return std::nullopt;
		return m;
	}
};

enum class mission_unavailable_reason : std::uint8_t
{
	/* Not a file of the host's mission. */
	unknown = 0,
	/* The host does not send missions ("Send missions to players" off). */
	refused = 1,
	/* The host's own copy no longer reads, or no longer has its hash. */
	unreadable = 2,
};

struct mission_unavailable_msg
{
	mission_hash hash{};
	mission_unavailable_reason reason{};
	void write(std::uint8_t *const p) const
	{
		p[0] = MISSION_ASSET_KIND;
		std::ranges::copy(hash, p + 1);
		p[33] = static_cast<std::uint8_t>(reason);
	}
	[[nodiscard]]
	static std::optional<mission_unavailable_msg> read(const std::span<const std::uint8_t> b)
	{
		if (b.size() != MISSION_UNAVAILABLE_SIZE || b[0] != MISSION_ASSET_KIND || b[33] > static_cast<std::uint8_t>(mission_unavailable_reason::unreadable))
			return std::nullopt;
		mission_unavailable_msg m;
		std::copy_n(&b[1], 32, m.hash.begin());
		m.reason = static_cast<mission_unavailable_reason>(b[33]);
		return m;
	}
};

/* What GAME_SETTINGS says about the host's mission (protocol 115): its
 * bundle hash (zeros: none, a built-in mission or one that cannot be
 * sent), the bundle's size, and whether the host sends it.
 */
struct mission_announcement
{
	static constexpr std::size_t SIZE{32 + 4 + 1};
	static constexpr std::uint8_t FLAG_SENDS{1};
	mission_hash bundle{};
	std::uint32_t size{};
	bool sends{};
	[[nodiscard]]
	bool known() const
	{
		return bundle != mission_hash{};
	}
	void write(std::uint8_t *const p) const
	{
		std::ranges::copy(bundle, p);
		net_put_le32(p + 32, size);
		p[36] = sends ? FLAG_SENDS : 0;
	}
	/* Unknown flags are ignored; a size over the cap or a hash without a
	 * size reads as "no mission announced".
	 */
	[[nodiscard]]
	static mission_announcement read(const std::uint8_t *const p)
	{
		mission_announcement a;
		std::copy_n(p, 32, a.bundle.begin());
		a.size = net_get_le32(p + 32);
		a.sends = p[36] & FLAG_SENDS;
		if (!a.known() || !a.size || a.size > MISSION_BUNDLE_MAX)
			return {};
		return a;
	}
};

/* A mission this machine has with the host's file name: where it is (the
 * caller's handle) and its bundle hash.
 */
struct mission_candidate
{
	std::size_t index{};
	mission_hash bundle{};
};

enum class mission_decision : std::uint8_t
{
	/* Play the local candidate `index`. */
	use_local,
	/* No bundle announced (an older host, a built-in mission): the first
	 * mission of that file name, as before protocol 115.
	 */
	use_by_name,
	download,
	/* None, or only other versions; and the download cannot happen: */
	missing_host_does_not_send,
	missing_player_refuses,
	missing_too_large,
};

struct mission_choice
{
	mission_decision decision{};
	std::size_t index{};
	/* A mission of that name exists here, but not this version. */
	bool other_version{};
};

/* Which mission a joining client plays: the one with the host's bundle
 * hash; else a download if the host sends and the player accepts; else
 * why not.  The candidates are this machine's missions of the host's
 * file name (its own and earlier downloads).
 */
[[nodiscard]]
inline mission_choice choose_mission(const mission_announcement &a, const std::span<const mission_candidate> candidates, const bool player_accepts)
{
	if (!a.known())
		return {mission_decision::use_by_name, 0, false};
	for (const auto &c : candidates)
		if (c.bundle == a.bundle)
			return {mission_decision::use_local, c.index, false};
	const bool other{!candidates.empty()};
	if (a.sends && player_accepts && a.size <= MISSION_BUNDLE_MAX)
		return {mission_decision::download, 0, other};
	/* No download: another version of it is still worth a try (the host
	 * refuses it only if its levels differ, by their checksum).
	 */
	if (other)
		return {mission_decision::use_local, candidates.front().index, true};
	if (!a.sends)
		return {mission_decision::missing_host_does_not_send, 0, false};
	if (!player_accepts)
		return {mission_decision::missing_player_refuses, 0, false};
	return {mission_decision::missing_too_large, 0, false};
}

/* The folder a downloaded bundle is kept in, under missions/: by the
 * first 12 hex digits of its hash, so that another version never
 * overwrites the player's own files or another download.
 */
[[nodiscard]]
inline std::string mission_download_dir(const mission_hash &bundle)
{
	static constexpr char hex[]{"0123456789abcdef"};
	std::string r{"missions/downloaded/"};
	for (std::size_t i{}; i != 6; ++i)
	{
		r.push_back(hex[bundle[i] >> 4]);
		r.push_back(hex[bundle[i] & 15]);
	}
	return r;
}

/* The first byte of an ASSET_* payload names its kind: the mission's go
 * here, the others to the ship exchange.
 */
[[nodiscard]]
constexpr bool is_mission_asset_message(const std::uint8_t type, const std::span<const std::uint8_t> payload)
{
	return (type == MISSION_MSG_REQUEST || type == MISSION_MSG_DATA || type == MISSION_MSG_UNAVAILABLE) && !payload.empty() && payload[0] == MISSION_ASSET_KIND;
}

/* The rate of one transfer: grows by a quarter per interval while the
 * round trip stays low, shrinks by 30 % when it rises or when more than
 * MISSION_LOSS_HIGH is retransmitted, within [MISSION_RATE_MIN, cap];
 * the cap is MISSION_RATE_LEVEL during a level.  `allowance` is what may
 * be sent now (a quarter second's worth at most, so that a pause never
 * ends in a burst).  `rtt` is the connection's smoothed round trip in
 * seconds (0: not known).
 */
struct mission_pacer
{
	double rate{MISSION_RATE_START};
	double allowance{};
	double since_adjust{};
	double min_rtt{};
	std::uint64_t base_sends{}, base_resends{};
	bool started{};
	void advance(const double seconds, const bool in_level, const std::uint64_t sends, const std::uint64_t resends, const double rtt = 0)
	{
		const double cap{in_level ? MISSION_RATE_LEVEL : MISSION_RATE_MAX};
		if (!started)
		{
			started = true;
			base_sends = sends;
			base_resends = resends;
		}
		since_adjust += seconds;
		if (since_adjust >= MISSION_RATE_ADJUST_INTERVAL)
		{
			since_adjust = 0;
			const auto ds{sends - base_sends}, dr{resends - base_resends};
			base_sends = sends;
			base_resends = resends;
			if (rtt > 0 && (min_rtt <= 0 || rtt < min_rtt))
				min_rtt = rtt;
			if (ds >= 8)
			{
				const double loss{static_cast<double>(dr) / static_cast<double>(ds)};
				const bool queueing{rtt > 0 && rtt > min_rtt * MISSION_RTT_RISE_FACTOR + MISSION_RTT_RISE};
				if (loss > MISSION_LOSS_HIGH || queueing)
					rate *= 0.7;
				else
					rate *= 1.25;
			}
		}
		rate = std::clamp(rate, MISSION_RATE_MIN, cap);
		allowance = std::min(allowance + rate * seconds, rate / 4);
	}
};

/* What the transfer needs from the game and the transport.  Slots are
 * player numbers; a client talks to slot 0, the host.
 */
class mission_env
{
public:
	virtual ~mission_env() = default;
	virtual void send(std::uint8_t slot, std::uint8_t type, std::span<const std::uint8_t> payload) = 0;
	/* Bytes queued or in flight on the connection to that slot. */
	virtual std::size_t queued_bytes(std::uint8_t slot) = 0;
	/* The connection's reliable messages sent and retransmitted so far. */
	struct link_counters
	{
		std::uint64_t sends{}, resends{};
		/* The smoothed round trip in seconds, 0 if not known yet. */
		double rtt{};
	};
	virtual link_counters link(std::uint8_t slot) = 0;
	/* The packets per tick the connection to that slot may send while a
	 * transfer runs on it (MISSION_BULK_PACKETS_PER_TICK in the lobby,
	 * with a window of MISSION_WINDOW_LOBBY; MISSION_LEVEL_PACKETS_PER_TICK
	 * during a level), or 0: back to normal.
	 */
	virtual void set_bulk(std::uint8_t slot, unsigned packets_per_tick) = 0;
	/* Host: the whole file, read and checked against its manifest entry
	 * (size and SHA-256), or nullptr.
	 */
	virtual std::shared_ptr<const std::vector<std::uint8_t>> host_file(const mission_manifest &, const mission_file &) = 0;
	/* Client: the file is already where the bundle's files are kept. */
	virtual bool client_has_file(const mission_manifest &, const mission_file &) = 0;
	/* Client: keep a received, checked file; false if it cannot. */
	virtual bool client_store(const mission_manifest &, const mission_file &, std::span<const std::uint8_t>) = 0;
	virtual void note(std::string_view what) = 0;
};

/* The host's side: the manifest it announces, and one send queue per
 * client.
 */
class mission_host
{
public:
	static constexpr unsigned MAX_SLOTS{8};
private:
	struct outgoing
	{
		bool active{};
		mission_file file;
		std::shared_ptr<const std::vector<std::uint8_t>> bytes;
		std::uint32_t offset{};
		mission_pacer pacer;
		/* Bundle bytes this client has had (finished files), for its
		 * progress.
		 */
		std::uint64_t done{};
		std::vector<mission_hash> finished;
		unsigned bulk{};
	};
	mission_env &env;
	std::optional<mission_manifest> manifest;
	std::array<outgoing, MAX_SLOTS> out{};
	void unavailable(const std::uint8_t slot, const mission_hash &h, const mission_unavailable_reason r)
	{
		std::array<std::uint8_t, MISSION_UNAVAILABLE_SIZE> b;
		mission_unavailable_msg{h, r}.write(b.data());
		env.send(slot, MISSION_MSG_UNAVAILABLE, b);
	}
	void set_bulk(const std::uint8_t slot, const unsigned packets)
	{
		if (out[slot].bulk != packets)
		{
			out[slot].bulk = packets;
			env.set_bulk(slot, packets);
		}
	}
	void receive_request(const std::uint8_t slot, const mission_request_msg &m)
	{
		if (!enabled)
		{
			unavailable(slot, m.hash, mission_unavailable_reason::refused);
			return;
		}
		const auto f{manifest ? manifest->find(m.hash) : nullptr};
		if (!f || m.offset >= f->size)
		{
			unavailable(slot, m.hash, mission_unavailable_reason::unknown);
			return;
		}
		auto &o{out[slot]};
		/* The file the client is getting already: from where it asks. */
		if (!o.active || o.file.hash != f->hash)
		{
			auto bytes{env.host_file(*manifest, *f)};
			if (!bytes || bytes->size() != f->size)
			{
				unavailable(slot, m.hash, mission_unavailable_reason::unreadable);
				return;
			}
			o.bytes = std::move(bytes);
			o.file = *f;
			if (!o.active)
				o.pacer = {};
			o.active = true;
			env.note("sending " + f->name);
		}
		o.offset = m.offset;
	}
public:
	/* "Send missions to players". */
	bool enabled{true};
	explicit mission_host(mission_env &e) :
		env{e}
	{
	}
	/* The mission of this game (nothing: none can be sent, a built-in
	 * mission or one that breaks a rule).
	 */
	void set_manifest(std::optional<mission_manifest> m)
	{
		if (m && !m->valid())
			m.reset();
		manifest = std::move(m);
		for (std::uint8_t s{}; s != MAX_SLOTS; ++s)
			slot_cleared(s);
	}
	[[nodiscard]]
	const std::optional<mission_manifest> &current() const
	{
		return manifest;
	}
	/* A client connected: it gets the manifest, if the host sends
	 * missions.
	 */
	void client_joined(const std::uint8_t slot)
	{
		if (slot >= MAX_SLOTS)
			return;
		slot_cleared(slot);
		if (!enabled || !manifest)
			return;
		const auto b{mission_manifest_msg::write(*manifest)};
		env.send(slot, MISSION_MSG_MANIFEST, b);
	}
	void slot_cleared(const std::uint8_t slot)
	{
		if (slot >= MAX_SLOTS)
			return;
		set_bulk(slot, 0);
		out[slot] = {};
	}
	void receive(const std::uint8_t from, const std::uint8_t type, const std::span<const std::uint8_t> payload)
	{
		if (!from || from >= MAX_SLOTS || type != MISSION_MSG_REQUEST)
			return;
		if (const auto m{mission_request_msg::read(payload)})
			receive_request(from, *m);
	}
	/* Send what the pacing and the window allow. */
	void pump(const double seconds, const bool in_level)
	{
		for (std::uint8_t slot{1}; slot != MAX_SLOTS; ++slot)
		{
			auto &o{out[slot]};
			if (!o.active)
			{
				set_bulk(slot, 0);
				continue;
			}
			set_bulk(slot, in_level ? MISSION_LEVEL_PACKETS_PER_TICK : MISSION_BULK_PACKETS_PER_TICK);
			const auto l{env.link(slot)};
			o.pacer.advance(std::max(seconds, 0.0), in_level, l.sends, l.resends, l.rtt);
			const auto window{in_level ? MISSION_WINDOW_LEVEL : MISSION_WINDOW_LOBBY};
			std::array<std::uint8_t, MISSION_DATA_HEADER + MISSION_DATA_CHUNK> buf;
			while (o.active && o.pacer.allowance > 0 && env.queued_bytes(slot) < window)
			{
				const auto n{std::min<std::size_t>(MISSION_DATA_CHUNK, o.file.size - o.offset)};
				const mission_data_msg m{o.file.hash, o.file.size, o.offset, std::span(*o.bytes).subspan(o.offset, n)};
				m.write(buf.data());
				env.send(slot, MISSION_MSG_DATA, std::span(buf).first(m.size()));
				o.offset += static_cast<std::uint32_t>(n);
				o.pacer.allowance -= static_cast<double>(m.size());
				if (o.offset >= o.file.size)
				{
					env.note("sent " + o.file.name);
					if (std::ranges::find(o.finished, o.file.hash) == o.finished.end())
					{
						o.finished.push_back(o.file.hash);
						o.done += o.file.size;
					}
					o.active = false;
					o.bytes.reset();
				}
			}
		}
	}
	[[nodiscard]]
	bool busy(const std::uint8_t slot) const
	{
		return slot < MAX_SLOTS && out[slot].active;
	}
	/* Percent of the bundle sent to that slot, while it gets files. */
	[[nodiscard]]
	std::optional<unsigned> progress(const std::uint8_t slot) const
	{
		if (slot >= MAX_SLOTS || !out[slot].active || !manifest)
			return std::nullopt;
		const auto total{manifest->total_size()};
		const auto &o{out[slot]};
		return total ? static_cast<unsigned>(std::min<std::uint64_t>(100, (o.done + o.offset) * 100 / total)) : 0;
	}
};

enum class mission_client_state : std::uint8_t
{
	idle,
	/* Waits for the manifest of the announced bundle. */
	waiting_manifest,
	downloading,
	done,
	failed,
};

/* The client's side: get the files of the bundle announced in
 * GAME_SETTINGS that it lacks, one after the other, the .hog first and
 * the .mn2 last (a bundle without its .mn2 is not a mission to the
 * mission list, so an interrupted download never shows up half).
 */
class mission_client
{
	mission_env &env;
	mission_client_state m_state{mission_client_state::idle};
	mission_hash expected{};
	std::optional<mission_manifest> manifest;
	std::vector<mission_file> todo;
	/* The file being received, kept across failures and new connections
	 * so that a new attempt resumes it (its hash names it).
	 */
	struct partial_file
	{
		mission_hash hash{};
		std::vector<std::uint8_t> bytes;
	};
	partial_file partial;
	double clock{}, last_progress{};
	std::uint64_t done_bytes{}, total_bytes{};
	std::string m_error;
	void fail(std::string why)
	{
		m_state = mission_client_state::failed;
		m_error = std::move(why);
		env.set_bulk(0, 0);
		env.note("download failed: " + m_error);
	}
	void request_next()
	{
		while (!todo.empty() && env.client_has_file(*manifest, todo.front()))
		{
			done_bytes += todo.front().size;
			todo.erase(todo.begin());
		}
		if (todo.empty())
		{
			m_state = mission_client_state::done;
			env.set_bulk(0, 0);
			env.note("mission complete");
			return;
		}
		const auto &f{todo.front()};
		if (partial.hash != f.hash)
		{
			partial.hash = f.hash;
			partial.bytes.clear();
		}
		partial.bytes.reserve(f.size);
		std::array<std::uint8_t, MISSION_REQUEST_SIZE> b;
		mission_request_msg{f.hash, static_cast<std::uint32_t>(partial.bytes.size())}.write(b.data());
		env.send(0, MISSION_MSG_REQUEST, b);
		env.note("asking for " + f.name + (partial.bytes.empty() ? std::string{} : " from byte " + std::to_string(partial.bytes.size())));
		last_progress = clock;
	}
	void receive_manifest(const std::span<const std::uint8_t> payload)
	{
		if (m_state != mission_client_state::waiting_manifest)
			return;
		const auto m{mission_manifest_msg::read(payload)};
		if (!m)
		{
			fail("the host sent an invalid mission description");
			return;
		}
		if (m->bundle_hash() != expected)
		{
			fail("the host described another mission than it announced");
			return;
		}
		manifest = *m;
		todo = m->files;
		/* The .hog first, the .mn2 last. */
		std::ranges::stable_sort(todo, [](const mission_file &a, const mission_file &b) {
			return a.type() == mission_file_type::hog && b.type() != mission_file_type::hog;
		});
		total_bytes = m->total_size();
		done_bytes = 0;
		m_state = mission_client_state::downloading;
		env.set_bulk(0, MISSION_BULK_PACKETS_PER_TICK);
		request_next();
	}
	void receive_data(const mission_data_msg &m)
	{
		if (m_state != mission_client_state::downloading || todo.empty())
			return;
		const auto &f{todo.front()};
		/* Anything but the next part of the file asked for: a duplicate
		 * from an earlier request (ignored), or a host that does not
		 * follow the protocol.
		 */
		if (m.hash != f.hash)
			return;
		if (m.total != f.size)
		{
			fail("the host sent a file of the wrong size");
			return;
		}
		if (m.offset != partial.bytes.size())
		{
			if (m.offset < partial.bytes.size())
				/* Sent again after a resumed request: the part we have. */
				return;
			fail("the host skipped part of a file");
			return;
		}
		partial.bytes.insert(partial.bytes.end(), m.data.begin(), m.data.end());
		last_progress = clock;
		if (partial.bytes.size() < f.size)
			return;
		const auto bytes{std::move(partial.bytes)};
		partial = {};
		if (sha256_of(bytes) != f.hash)
		{
			fail(f.name + " does not match its SHA-256");
			return;
		}
		if (!mission_file_content_valid(f.type(), bytes))
		{
			fail(f.name + " is not a valid " + (f.type() == mission_file_type::hog ? "HOG" : "MN2") + " file");
			return;
		}
		if (!env.client_store(*manifest, f, bytes))
		{
			fail("cannot write " + f.name);
			return;
		}
		env.note("received " + f.name);
		done_bytes += f.size;
		todo.erase(todo.begin());
		request_next();
	}
public:
	explicit mission_client(mission_env &e) :
		env{e}
	{
	}
	/* Get the bundle of that hash from the host (whose manifest is on its
	 * way, or arrived already: pass it).
	 */
	void begin(const mission_hash &bundle, const std::optional<std::span<const std::uint8_t>> manifest_payload = std::nullopt)
	{
		expected = bundle;
		manifest.reset();
		todo.clear();
		m_error.clear();
		done_bytes = total_bytes = 0;
		m_state = mission_client_state::waiting_manifest;
		last_progress = clock;
		if (manifest_payload)
			receive_manifest(*manifest_payload);
	}
	/* Stop (the player cancelled, the connection is gone); what was
	 * received of the current file is kept for the next attempt.
	 */
	void stop()
	{
		if (m_state == mission_client_state::waiting_manifest || m_state == mission_client_state::downloading)
			env.set_bulk(0, 0);
		m_state = mission_client_state::idle;
	}
	void receive(const std::uint8_t from, const std::uint8_t type, const std::span<const std::uint8_t> payload)
	{
		if (from != 0)
			return;
		switch (type)
		{
			case MISSION_MSG_MANIFEST:
				receive_manifest(payload);
				break;
			case MISSION_MSG_DATA:
				if (const auto m{mission_data_msg::read(payload)})
					receive_data(*m);
				break;
			case MISSION_MSG_UNAVAILABLE:
				if (const auto m{mission_unavailable_msg::read(payload)}; m && m_state == mission_client_state::downloading && !todo.empty() && m->hash == todo.front().hash)
					fail(m->reason == mission_unavailable_reason::refused ? "the host does not send missions" :
						m->reason == mission_unavailable_reason::unreadable ? "the host cannot read its mission files" :
						"the host does not have that mission file");
				break;
			default:
				break;
		}
	}
	/* Time passes: the timeouts. */
	void tick(const double seconds)
	{
		clock += std::max(seconds, 0.0);
		if (m_state == mission_client_state::waiting_manifest && clock - last_progress >= MISSION_MANIFEST_TIMEOUT)
			fail("the host did not describe its mission (does it send missions?)");
		else if (m_state == mission_client_state::downloading && clock - last_progress >= MISSION_STALL_TIMEOUT)
			fail("the host stopped sending the mission");
	}
	[[nodiscard]]
	mission_client_state state() const
	{
		return m_state;
	}
	[[nodiscard]]
	const std::string &error() const
	{
		return m_error;
	}
	[[nodiscard]]
	const std::optional<mission_manifest> &current() const
	{
		return manifest;
	}
	[[nodiscard]]
	std::uint64_t received() const
	{
		return done_bytes + (todo.empty() || partial.hash != todo.front().hash ? 0 : partial.bytes.size());
	}
	[[nodiscard]]
	std::uint64_t total() const
	{
		return total_bytes;
	}
	[[nodiscard]]
	unsigned percent() const
	{
		return total_bytes ? static_cast<unsigned>(std::min<std::uint64_t>(100, received() * 100 / total_bytes)) : 0;
	}
};

}

}
