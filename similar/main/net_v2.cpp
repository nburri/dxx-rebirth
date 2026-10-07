/*
 * This file is part of the DXX-Rebirth project <https://github.com/dxx-rebirth/dxx-rebirth/>.
 * It is copyright by its individual contributors, as recorded in the
 * project's Git history.  See COPYING.txt at the top level for license
 * terms and a link to the Git history.
 */
/*
 * Multiplayer network protocol v2, stages 1 and 2
 * (Documentation/network-protocol-v2.md, section 8): the UDP sockets, the
 * session layer of section 4 (discovery, join handshake, one transport
 * connection per peer, level start, level snapshot for a join in
 * progress, leave and kick), the network tick, the client's INPUT and the
 * host's state bundle of section 5, and the mapping of the remaining v1
 * gameplay messages onto the transport (section 6.10, "stage 1" column).
 *
 * The menus and the level start flow stayed in net_udp.cpp; the two files
 * talk through net_v2_game.h.  Nothing else in the game knows about this
 * file: the gameplay layer uses multi::dispatch.
 */

#include "dxxsconf.h"
#include <algorithm>
#include <deque>
#include <optional>
#include <random>
#include <ranges>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <vector>

#include "pstypes.h"
#include "digi.h"
#include "window.h"
#include "strutil.h"
#include "args.h"
#include "timer.h"
#include "newmenu.h"
#include "object.h"
#include "dxxerror.h"
#include "player.h"
#include "gameseq.h"
#include "net_udp.h"
#include "net_v2.h"
#include "net_v2_transport.h"
#include "net_v2_session.h"
#include "net_v2_modes.h"
#include "net_v2_game.h"
#include "net_v2_state.h"
#include "net_interp.h"
#include "net_score_carry.h"
#include "net_countdown.h"
#include "net_public_address.h"
#include "game.h"
#include "multi.h"
#include "bot.h"
#include "taunt.h"
#include "net_mission.h"
#include "net_v2_mission.h"
#include "multiinternal.h"
#include "powerup.h"
#include "gameseg.h"
#include "sounds.h"
#include "text.h"
#include "newdemo.h"
#include "multibot.h"
#include "wall.h"
#include "bm.h"
#include "effects.h"
#include "physics.h"
#include "hudmsg.h"
#include "switch.h"
#include "textures.h"
#include "event.h"
#include "playsave.h"
#include "vers_id.h"
#include "weapon.h"
#include "console.h"
#include "byteutil.h"
#include "kconfig.h"
#include "controls.h"
#include "movement_record.h"
#include "movement_record_format.h"

#include "compiler-range_for.h"
#include "d_enumerate.h"
#include "d_levelstate.h"
#include "d_range.h"
#include "partial_range.h"

#if !defined(WIN32)
#include <arpa/inet.h>
#include <netdb.h>
#include <sys/socket.h>
#include <ifaddrs.h>
#include <net/if.h>
#endif

#ifndef _WIN32
constexpr std::integral_constant<int, -1> INVALID_SOCKET{};
#endif

/* Sockets.  Unchanged from the v1 code except that they are private to
 * this file; net_udp.cpp reaches them through net_v2_game.h.
 */
namespace dcx {

namespace {

constexpr sockaddr_in GBcast = { // global Broadcast address clients and hosts will use for lite_info exchange over LAN
	.sin_family = AF_INET,
	.sin_port = words_bigendian ? UDP_PORT_DEFAULT : SWAPSHORT(UDP_PORT_DEFAULT),
	.sin_addr = {
#ifdef _WIN32
		.S_un = {
			.S_addr = 0xffffffff
		}
#else
		.s_addr = 0xffffffff
#endif
	},
	.sin_zero = {}
};

#if DXX_USE_IPv6
constexpr sockaddr_in6 GMcast_v6 = { // same for IPv6-only
	.sin6_family = AF_INET6,
	.sin6_port = GBcast.sin_port,
	.sin6_flowinfo = 0,
	.sin6_addr = {
		{{0xff, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1}}
	},
	.sin6_scope_id = 0,
};
#endif

class RAIIsocket
{
#ifndef _WIN32
	typedef int SOCKET;
	static int closesocket(SOCKET fd)
	{
		return close(fd);
	}
#endif
	SOCKET s{INVALID_SOCKET};
public:
	constexpr RAIIsocket() = default;
	RAIIsocket(int domain, int type, int protocol) : s(socket(domain, type, protocol))
	{
	}
	RAIIsocket(const RAIIsocket &) = delete;
	RAIIsocket &operator=(const RAIIsocket &) = delete;
	RAIIsocket(RAIIsocket &&r) :
		s{std::exchange(r.s, INVALID_SOCKET)}
	{
	}
	~RAIIsocket()
	{
		reset();
	}
	RAIIsocket &operator=(RAIIsocket &&r)
	{
		std::swap(s, r.s);
		return *this;
	}
	void reset()
	{
		if (s != INVALID_SOCKET)
			closesocket(std::exchange(s, INVALID_SOCKET));
	}
	[[nodiscard]]
	explicit operator bool() const { return s != INVALID_SOCKET; }
	[[nodiscard]]
	explicit operator bool() { return static_cast<bool>(*const_cast<const RAIIsocket *>(this)); }
	[[nodiscard]]
	operator SOCKET() { return s; }
	bool operator<(auto &&) const = delete;
	bool operator<=(auto &&) const = delete;
	bool operator>(auto &&) const = delete;
	bool operator>=(auto &&) const = delete;
	bool operator==(auto &&) const = delete;
	bool operator!=(auto &&) const = delete;
};

struct addrinfo_deleter
{
	static void operator()(addrinfo *p)
	{
		freeaddrinfo(p);
	}
};

class RAIIaddrinfo : std::unique_ptr<addrinfo, addrinfo_deleter>
{
	using base_type = std::unique_ptr<addrinfo, addrinfo_deleter>;
public:
	int getaddrinfo(const char *node, const char *service, const addrinfo *hints)
	{
		addrinfo *p = nullptr;
		int r = ::getaddrinfo(node, service, hints, &p);
		reset(p);
		return r;
	}
	using base_type::get;
	using base_type::operator->;
};

struct sockaddr_ref
{
	sockaddr &sa;
	socklen_t len;
	/* For bug compatibility with earlier versions, the returned socklen_t from
	 * the kernel is ignored, and the data structure is assumed to be filled
	 * with a value of the correct size.
	 */
#if DXX_USE_IPv6
	sockaddr_ref(sockaddr_in6 &sai6) :
		sa(reinterpret_cast<sockaddr &>(sai6)), len(sizeof(sai6))
	{
	}
#else
	sockaddr_ref(sockaddr_in &sai) :
		sa(reinterpret_cast<sockaddr &>(sai)), len(sizeof(sai))
	{
	}
#endif
	sockaddr_ref(_sockaddr &sai_) :
#if DXX_USE_IPv6
		sockaddr_ref(sai_.sin6)
#else
		sockaddr_ref(sai_.sin)
#endif
	{
	}
};

struct csockaddr_ref
{
	const sockaddr &sa;
	const socklen_t len;
	csockaddr_ref(const sockaddr_in &sai) :
		sa(reinterpret_cast<const sockaddr &>(sai)), len(sizeof(sai))
	{
	}
#if DXX_USE_IPv6
	csockaddr_ref(const sockaddr_in6 &sai6) :
		sa(reinterpret_cast<const sockaddr &>(sai6)),
		len(
#if defined(__linux__)
			/* Known to work */
#else
			/* Default case: not known.  Add a runtime check. */
			sai6.sin6_family == AF_INET ? sizeof(sockaddr_in) :
#endif
			sizeof(sai6))
	{
	}
#endif
	csockaddr_ref(const _sockaddr &sai_) :
#if DXX_USE_IPv6
		csockaddr_ref(sai_.sin6)
#else
		csockaddr_ref(sai_.sin)
#endif
	{
	}
};

using csocket_data_buffer = std::span<const uint8_t>;
using socket_data_buffer = std::span<uint8_t>;

std::array<RAIIsocket, 2> UDP_Socket;
int UDP_num_sendto, UDP_len_sendto, UDP_num_recvfrom, UDP_len_recvfrom;

ssize_t dxx_sendto(const int sockfd, const csocket_data_buffer msg, const int flags, const csockaddr_ref to)
{
	ssize_t rv = sendto(sockfd, reinterpret_cast<const char *>(msg.data()), msg.size(), flags, &to.sa, to.len);
	UDP_num_sendto++;
	if (rv > 0)
		UDP_len_sendto += rv;
	return rv;
}

ssize_t dxx_recvfrom(const int sockfd, const socket_data_buffer msg, const int flags, sockaddr_ref from)
{
	ssize_t rv = recvfrom(sockfd, reinterpret_cast<char *>(msg.data()), msg.size(), flags, &from.sa, &from.len);
	UDP_num_recvfrom++;
	UDP_len_recvfrom += rv;
	return rv;
}

void udp_traffic_stat()
{
	static fix64 last_traf_time = 0;

	if (timer_query() >= last_traf_time + F1_0)
	{
		last_traf_time = timer_query();
		con_printf(CON_DEBUG, "P#%u TRAFFIC - OUT: %fKB/s %iPPS IN: %fKB/s %iPPS",Player_num, static_cast<float>(UDP_len_sendto)/1024, UDP_num_sendto, static_cast<float>(UDP_len_recvfrom)/1024, UDP_num_recvfrom);
		UDP_num_sendto = UDP_len_sendto = UDP_num_recvfrom = UDP_len_recvfrom = 0;
	}
}

// Open socket
int udp_open_socket(RAIIsocket &sock, int port, const bool loopback = false)
{
	int bcast{1};

	// close stale socket
	struct _sockaddr sAddr;   // my address information

	sock = RAIIsocket(sAddr.address_family, SOCK_DGRAM, 0);
	if (!sock)
	{
		con_printf(CON_URGENT,"udp_open_socket: socket creation failed (port %i)", port);
		nm_messagebox(menu_title{TXT_ERROR}, {TXT_OK}, "Port: %i\nCould not create socket.", port);
		return -1;
	}
	sAddr = {};
	sAddr.sa.sa_family = sAddr.address_family;
#if DXX_USE_IPv6
	sAddr.sin6.sin6_port = htons (port); // short, network byte order
	if (loopback)
		sAddr.sin6.sin6_addr = in6addr_loopback;
	else
		sAddr.sin6.sin6_addr = IN6ADDR_ANY_INIT; // automatically fill with my IP
	{
		/* Accept and send IPv4 traffic on the IPv6 socket, using
		 * IPv4-mapped addresses (::ffff:a.b.c.d), which udp_dns_filladdr
		 * produces for IPv4 hosts.  Linux defaults to this, but Windows
		 * defaults to IPV6_V6ONLY=1, which made IPv4 hosts unreachable
		 * ("No response by host") and IPv4 clients unable to reach a
		 * Windows host.
		 */
		int v6only{0};
#ifdef _WIN32
		setsockopt(sock, IPPROTO_IPV6, IPV6_V6ONLY, reinterpret_cast<const char *>(&v6only), sizeof(v6only));
#else
		setsockopt(sock, IPPROTO_IPV6, IPV6_V6ONLY, &v6only, sizeof(v6only));
#endif
	}
#else
	sAddr.sin.sin_port = htons (port); // short, network byte order
	sAddr.sin.sin_addr.s_addr = htonl(loopback ? INADDR_LOOPBACK : INADDR_ANY); // automatically fill with my IP
#endif

	if (bind(sock, &sAddr.sa, sizeof(sAddr)) < 0)
	{
		con_printf(CON_URGENT,"udp_open_socket: bind name to socket failed (port %i)", port);
		nm_messagebox(menu_title{TXT_ERROR}, {TXT_OK}, "Port: %i\nCould not bind name to socket.", port);
		sock.reset();
		return -1;
	}
	if (loopback)
		return 0;
#ifdef _WIN32
	setsockopt(sock, SOL_SOCKET, SO_BROADCAST, reinterpret_cast<const char *>(&bcast), sizeof(bcast));
#else
	setsockopt(sock, SOL_SOCKET, SO_BROADCAST, &bcast, sizeof(bcast));
#endif
	return 0;
}

#ifndef MSG_DONTWAIT
int udp_general_packet_ready(const SOCKET sock)
{
	fd_set set;
	struct timeval tv;

	FD_ZERO(&set);
	FD_SET(sock, &set);
	tv.tv_sec = tv.tv_usec = 0;
	if (select(sock + 1, &set, NULL, NULL, &tv) > 0)
		return 1;
	else
		return 0;
}
#endif

// Gets some text. Returns 0 if nothing on there.
ssize_t udp_receive_packet(RAIIsocket &sock, const socket_data_buffer msg, const sockaddr_ref sender_addr)
{
	if (!sock)
		return -1;
#ifndef MSG_DONTWAIT
	if (!udp_general_packet_ready(sock))
		return 0;
#endif
	ssize_t msglen;
		int flags{0};
#ifdef MSG_DONTWAIT
		flags |= MSG_DONTWAIT;
#endif
		msglen = dxx_recvfrom(sock, msg, flags, sender_addr);

		if (msglen < 0)
			return 0;

	return msglen;
}

void net_udp_flush(RAIIsocket &s)
{
	if (!s)
		return;
	unsigned i{0};
	struct _sockaddr sender_addr;
	std::array<uint8_t, 1500> packet;
	while (udp_receive_packet(s, packet, sender_addr) > 0)
		++i;
	if (i)
		con_printf(CON_VERBOSE, "Flushed %u UDP packets from socket %i", i, static_cast<int>(s));
}

}

const char *dxx_ntop(const _sockaddr &sa, typename _sockaddr::presentation_buffer &dbuf)
{
#ifdef WIN32
#ifdef DXX_HAVE_INET_NTOP
	/*
	 * Windows and inet_ntop: copy the in_addr/in6_addr to local
	 * variables because the Microsoft prototype lacks a const
	 * qualifier.
	 */
	union {
		in_addr ia;
#if DXX_USE_IPv6
		in6_addr ia6;
#endif
	};
	const auto addr =
#if DXX_USE_IPv6
		(sa.sa.sa_family == AF_INET6)
		? &(ia6 = sa.sin6.sin6_addr)
		:
#endif
		static_cast<void *>(&(ia = sa.sin.sin_addr));
#else
	/*
	 * Windows and not inet_ntop: only inet_ntoa available.
	 *
	 * SConf check_inet_ntop_present enforces that Windows without
	 * inet_ntop cannot enable IPv6, so the IPv4 branch must be correct
	 * here.
	 *
	 * The reverse is not true.  Windows with inet_ntop might not enable
	 * IPv6.
	 */
#if DXX_USE_IPv6
#error "IPv6 requires inet_ntop; SConf should prevent this path"
#endif
	dbuf.back() = 0;
	/*
	 * Copy the formatted string to the local buffer `dbuf` to guard
	 * against concurrent uses of `dxx_ntop`.
	 */
	return reinterpret_cast<const char *>(memcpy(dbuf.data(), inet_ntoa(sa.sin.sin_addr), dbuf.size() - 1));
#endif
#else
	/*
	 * Not Windows; assume inet_ntop present.  Non-Windows platforms
	 * declare inet_ntop with a const qualifier, so take a pointer to
	 * the underlying data.
	 */
	const auto addr =
#if DXX_USE_IPv6
		(sa.sa.sa_family == AF_INET6)
		? &sa.sin6.sin6_addr
		:
#endif
		static_cast<const void *>(&sa.sin.sin_addr);
#endif
#if !defined(WIN32) || defined(DXX_HAVE_INET_NTOP)
	if (const auto r = inet_ntop(sa.sa.sa_family, addr, dbuf.data(), dbuf.size()))
		return r;
	return "address";
#endif
}

uint16_t dxx_sockaddr_port(const _sockaddr &sa)
{
	return ntohs(
#if DXX_USE_IPv6
		sa.sa.sa_family == AF_INET6
		? sa.sin6.sin6_port
		:
#endif
		sa.sin.sin_port);
}

namespace {

void add_host_address_candidate(std::vector<host_address_candidate> &out, const sockaddr *const sa, const uint16_t port)
{
	_sockaddr a{};
	host_address_kind kind;
	if (sa->sa_family == AF_INET)
	{
		memcpy(&a.sin, sa, sizeof(a.sin));
		kind = classify_ipv4(ntohl(a.sin.sin_addr.s_addr));
	}
#if DXX_USE_IPv6
	else if (sa->sa_family == AF_INET6)
	{
		memcpy(&a.sin6, sa, sizeof(a.sin6));
		kind = classify_ipv6(a.sin6.sin6_addr.s6_addr);
	}
#endif
	else
		return;
	if (kind == host_address_kind::unusable)
		return;
	typename _sockaddr::presentation_buffer dbuf;
	auto text{format_address_port(dxx_ntop(a, dbuf), port)};
	if (std::ranges::any_of(out, [&text](const host_address_candidate &c) { return c.text == text; }))
		return;
	out.push_back({std::move(text), kind});
}

}

std::vector<host_address_candidate> host_address_candidates(const uint16_t port)
{
	std::vector<host_address_candidate> out;
#ifdef WIN32
	/* No getifaddrs: the addresses the host name resolves to, which on
	 * Windows are those of the interfaces.
	 */
	char name[256];
	if (gethostname(name, sizeof(name)) == 0)
	{
		name[sizeof(name) - 1] = 0;
		addrinfo hints{};
		hints.ai_family = AF_UNSPEC;
		hints.ai_socktype = SOCK_DGRAM;
		RAIIaddrinfo result;
		if (result.getaddrinfo(name, nullptr, &hints) == 0)
			for (auto p{result.get()}; p; p = p->ai_next)
				if (p->ai_addr)
					add_host_address_candidate(out, p->ai_addr, port);
	}
#else
	ifaddrs *list;
	if (getifaddrs(&list) == 0)
	{
		for (auto p{list}; p; p = p->ifa_next)
			if (p->ifa_addr && (p->ifa_flags & IFF_UP) && !(p->ifa_flags & IFF_LOOPBACK))
				add_host_address_candidate(out, p->ifa_addr, port);
		freeifaddrs(list);
	}
#endif
	std::ranges::stable_sort(out, {}, &host_address_candidate::kind);
	return out;
}

// Resolve address
int udp_dns_filladdr(_sockaddr &out, const char *host, uint16_t port, bool numeric_only, bool silent)
{
	sockaddr_ref addr{out};
	// Variables
	addrinfo hints{};
	char sPort[6];

	// Build the port
	snprintf(sPort, 6, "%hu", port);

	// Uncomment the following if we want ONLY what we compile for
	hints.ai_family = _sockaddr::address_family;
	// We are always UDP
	hints.ai_socktype = SOCK_DGRAM;
#ifdef AI_NUMERICSERV
	hints.ai_flags |= AI_NUMERICSERV;
#endif
#if DXX_USE_IPv6
	hints.ai_flags |= AI_V4MAPPED | AI_ALL;
#endif
	// Numeric address only?
	if (numeric_only)
		hints.ai_flags |= AI_NUMERICHOST;

	// Resolve the domain name
	RAIIaddrinfo result;
	if (result.getaddrinfo(host, sPort, &hints) != 0)
	{
		con_printf( CON_URGENT, "udp_dns_filladdr (getaddrinfo) failed for host %s", host );
		if (!silent)
			nm_messagebox(menu_title{TXT_ERROR}, {TXT_OK}, "Could not resolve address\n%s", host);
		addr.sa.sa_family = AF_UNSPEC;
		return -1;
	}

	if (result->ai_addrlen > addr.len)
	{
		con_printf(CON_URGENT, "Address too big for host %s", host);
		if (!silent)
			nm_messagebox(menu_title{TXT_ERROR}, {TXT_OK}, "Address too big for host\n%s", host);
		addr.sa.sa_family = AF_UNSPEC;
		return -1;
	}
	// Now copy it over
	memcpy(&addr.sa, result->ai_addr, addr.len = result->ai_addrlen);
	return 0;
}

}

namespace dsx {

namespace net_v2 {

namespace {

using ::dcx::net_v2::connection;
using ::dcx::net_v2::connection_config;
using ::dcx::net_v2::connection_state;
using ::dcx::net_v2::close_reason;
using ::dcx::net_v2::chunk_type;
using ::dcx::net_v2::session_msg;
using ::dcx::net_v2::event_kind;
using ::dcx::net_v2::packet_header;
using ::dcx::net_v2::packet_buffer;
using ::dcx::net_v2::receive_status;
using ::dcx::net_v2::tick_period;
using ::dcx::net_v2::game_id_type;
using ::dcx::net_v2::program_version;
using ::dcx::net_v2::rate_limiter;
using ::dcx::net_v2::join_attempt;
using ::dcx::net_v2::slot_view;
using ::dcx::net_v2::admission_result;
using ::dcx::net_v2::NET_V2_PLAYER_ID_NONE;
using ::dcx::net_v2::NET_V2_MAX_PACKET;
using ::dcx::net_v2::NET_V2_MAX_MESSAGE;
using ::dcx::net_v2::NET_V2_MAX_EVENT;
using ::dcx::net_v2::NET_V2_CALLSIGN_SIZE;

#if DXX_BUILD_DESCENT == 1
constexpr game_id_type Game_id{{'D', '1', 'X', 'R'}};
#elif DXX_BUILD_DESCENT == 2
constexpr game_id_type Game_id{{'D', '2', 'X', 'R'}};
#endif
constexpr program_version Program_version{DXX_VERSION_MAJORi, DXX_VERSION_MINORi, DXX_VERSION_MICROi};

/* Stage 1 wire sizes (Documentation/network-protocol-v2.md, sections 4.1,
 * 4.3, 4.4, 4.5 and the stage 1 notes of section 6.10).
 */
/* Protocol 114: 66 (TeamSpawns, RespawnDelay). */
constexpr std::size_t GAME_SETTINGS_FIXED_SIZE{66 + ::dcx::net_v2::mission_announcement::SIZE};
constexpr std::size_t PLAYER_LIST_ENTRY_SIZE{12};
constexpr std::size_t PLAYER_LIST_SIZE{MAX_PLAYERS * PLAYER_LIST_ENTRY_SIZE};
constexpr std::size_t PLAYER_JOINED_SIZE{12};
constexpr std::size_t LEVEL_START_SIZE{29};
constexpr std::size_t LEVEL_READY_SIZE{6};
constexpr std::size_t LEVEL_GO_SIZE{8};
constexpr std::size_t SNAPSHOT_BEGIN_SIZE{4};
constexpr std::size_t SNAPSHOT_END_SIZE{8};
/* Object number, owner, remote object number, net id (stage 3, 0xffff
 * for none), the object.
 */
constexpr std::size_t SNAPSHOT_OBJECT_ENTRY_OBJECT_OFFSET{4 + 1 + 4 + 2};
constexpr std::size_t SNAPSHOT_OBJECT_ENTRY_SIZE{SNAPSHOT_OBJECT_ENTRY_OBJECT_OFFSET + sizeof(object_rw)};
constexpr unsigned SNAPSHOT_OBJECTS_PER_MESSAGE{3};
constexpr std::size_t SNAPSHOT_GAME_SIZE{2 + (MAX_PLAYERS * MAX_PLAYERS * 2) + (MAX_PLAYERS * 2) + (MAX_PLAYERS * 2) + (MAX_PLAYERS * 4) + 4 + 1 + 1 + MAX_PLAYERS + 4 + 2 + 10 + 4 + MAX_PLAYERS};
/* v1 endlevel_h without the upid byte; endlevel_c without upid and player
 * number.
 */
constexpr std::size_t LEGACY_ENDLEVEL_HOST_SIZE{1 + (MAX_PLAYERS * 5) + (MAX_PLAYERS * MAX_PLAYERS * 2)};
constexpr std::size_t LEGACY_ENDLEVEL_CLIENT_SIZE{1 + 1 + 2 + 2 + (MAX_PLAYERS * 2)};

/* Reliable messages beyond what the connection should hold wait in a
 * per-peer backlog (a level snapshot is larger than the connection's
 * queue bound); the backlog is fed to the connection while it is below
 * these marks, and a peer whose backlog grows past BACKLOG_MAX_BYTES is
 * too slow to keep.
 */
constexpr std::size_t BACKLOG_PUMP_BYTES{48 * 1024};
constexpr std::size_t BACKLOG_PUMP_MESSAGES{256};
constexpr std::size_t BACKLOG_MAX_BYTES{4u << 20};

constexpr fix64 GAME_INFO_BROADCAST_INTERVAL{F1_0 * 10};
/* A transport statistics line per connection on the console. */
constexpr fix64 STATS_INTERVAL{F1_0 * 5};
constexpr fix64 ENDLEVEL_INTERVAL{F1_0};
constexpr fix64 EXTRAS_INTERVAL{F1_0 / 50};
/* menu_pump runs a frame only if none ran for this long (a menu with a
 * polling handler runs one every event loop).
 */
constexpr fix64 MENU_PUMP_INTERVAL{F1_0 / 100};

struct queued_message
{
	session_msg type;
	std::vector<uint8_t> payload;
};

struct peer
{
	/* none, joining, syncing, playing, closing (net_v2_session.h). */
	using phase = ::dcx::net_v2::peer_phase;
	phase ph{phase::none};
	/* Host: when the peer entered `joining` or `syncing` (section 4.4:
	 * a join in progress is bounded by NET_V2_JOIN_SYNC_TIMEOUT).
	 */
	fix64 phase_since{};
	std::optional<connection> conn;
	_sockaddr addr{};
	uint32_t token{};
	uint32_t nonce{};
	std::array<uint8_t, ::dcx::net_v2::NET_V2_JOIN_ACCEPT_SIZE> accept_payload{};
	/* The player is new to the game (v1 Network_player_added). */
	bool is_new{};
	/* The peer reported the level loaded (LEVEL_READY). */
	bool has_ready{};
	/* Host: the peer was sent LEVEL_GO for the current level (at the level
	 * start or after its snapshot).  A peer without it at the level end
	 * was still joining and would be stranded (host_end_level).
	 */
	bool in_level{};
	int32_t ready_level{};
	uint16_t ready_checksum{};
	fix64 close_at{};
	std::deque<queued_message> backlog;
	std::size_t backlog_bytes{};
	/* How full the connection's queue may be before messages wait in the
	 * backlog: larger while a mission transfer runs in the lobby
	 * (game_set_bulk).
	 */
	std::size_t pump_bytes{BACKLOG_PUMP_BYTES};
	/* Host: when this peer's bundle next carries the ping list. */
	fix64 next_pings{};
	fix64 next_stats{};
};

/* Host: the newest INPUT accepted from a player (section 5.5, step 6). */
struct accepted_input
{
	bool valid{};
	::dcx::net_v2::input_chunk input;
	/* The sample time, clamped (see apply_input), on the host clock. */
	::dcx::net_interp::host_clock time{};
	fix64 arrival{};
};

/* Host: the shared controls of an INPUT (section 5.3, protocol 107) are
 * recorded only while they are this fresh; after a longer gap the
 * movement recording leaves them out and the analysis estimates.
 */
constexpr fix64 HOST_INPUT_CONTROLS_MAX_AGE{F1_0 / 10};

struct session_state
{
	uint32_t session_id{};
	/* Client: the token the host assigned. */
	uint32_t my_token{};
	per_player_array<peer> peers{};
	/* Host: the slot was handed to a new player who has not entered the
	 * game yet (no CLIENT_READY).  Kept across the peer's connection, so
	 * that a client restarting during its join, which comes back by
	 * callsign as a rejoin, is still admitted as new
	 * (net_v2::admission_is_new).
	 */
	per_player_array<bool> awaits_entry{};
	/* Stage 2 (section 5).  Host: the newest INPUT per player and the
	 * session's tick counter (section 2.3).  Client: the sequence of its
	 * INPUT chunks and the newest bundle header applied.
	 */
	per_player_array<accepted_input> inputs{};
	::dcx::net_interp::tick_accumulator tick;
	uint16_t input_seq{};
	bool have_bundle{};
	uint32_t bundle_tick{};
	::dcx::net_interp::host_clock bundle_time{};
	/* Pending best-effort v1 records (originator: the local player):
	 * event kind byte, player id, records.
	 */
	std::vector<uint8_t> event_buffer;
	/* Client join attempt. */
	join_attempt join;
	_sockaddr join_addr{};
	join_status join_result{join_status::idle};
	/* Client: the wait for the snapshot of a join in progress (section
	 * 4.4), from LEVEL_READY to LEVEL_GO.
	 */
	::dcx::net_v2::join_sync_wait sync_wait;
	/* Client: the host last sent a GAME_INFO_REQ; only its GAME_INFO and
	 * version JOIN_DENY are accepted.
	 */
	std::optional<_sockaddr> info_addr;
#if DXX_USE_TRACKER
	tracker_game_id join_tracker_id{};
#endif
	rate_limiter lite_limit{::dcx::net_v2::NET_V2_GAME_INFO_LITE_INTERVAL};
	rate_limiter info_limit{::dcx::net_v2::NET_V2_GAME_INFO_INTERVAL};
	rate_limiter join_limit{::dcx::net_v2::NET_V2_JOIN_REQUEST_INTERVAL};
	fix64 now{};
	fix64 last_broadcast{};
	fix64 last_endlevel{};
	/* Host: when this machine saw the reactor die (net_countdown.h,
	 * countdown_overdue); 0 while it lives.
	 */
	fix64 countdown_started{};
	fix64 last_extras{};
	/* Host: players whose "extras" wait for the current run to finish
	 * (Network_sending_extras and Player_joining_extras describe one
	 * player at a time).
	 */
	std::deque<playernum_t> extras_queue;
	/* The reason the next PLAYER_LEFT carries (set before
	 * multi_disconnect_player is called).
	 */
	kick_player_reason left_reason{kick_player_reason::timeout};
	/* Client: the level snapshot being applied. */
	uint32_t snapshot_crc{};
	unsigned snapshot_objects{};
	unsigned snapshot_parts{};
	bool snapshot_mode_static{};
	/* Datagrams rejected before or by the transport since the last report. */
	unsigned rejected_datagrams{};
	bool in_frame{};
	/* When frame() last ran (menu_pump runs it only if nothing else did). */
	fix64 last_frame{};
	/* Host: the lobby menu is up, so a JOIN_REQUEST while `starting` may
	 * add a player (not while the host sets up the teams, between the
	 * lobby and the level start).
	 */
	bool lobby_open{};
	/* Host: the nonce of the last JOIN_REQUEST logged. */
	uint32_t logged_join_nonce{};
	/* Client: a GAME_INFO from the asked host was ignored and logged. */
	bool info_drop_logged{};
	/* Client: JOIN_REQUESTs sent in the running attempt. */
	unsigned join_requests_sent{};
	/* Client: why the connection to the host ended while waiting for the
	 * level start, for the level wait's message (client_take_host_lost).
	 */
	std::optional<host_lost_info> host_lost;
};

session_state S;

/* Host: what the players and the tracker report as this game's address
 * (ADDRESS_SEEN, protocol 108, net_public_address.h).  A player reports
 * once per join; more often than ADDRESS_SEEN_INTERVAL is dropped.
 */
constexpr fix64 ADDRESS_SEEN_INTERVAL{F1_0 * 10};
::dcx::public_address_tally<MAX_PLAYERS> Public_tally;
/* The text of the last choice logged, to log only a change. */
std::string Public_logged;

[[nodiscard]]
std::optional<::dcx::seen_address> seen_from_sockaddr(const _sockaddr &a)
{
	if (a.sa.sa_family == AF_INET)
	{
		std::array<uint8_t, 4> b;
		memcpy(b.data(), &a.sin.sin_addr, b.size());
		return ::dcx::make_seen_ipv4(b, ntohs(a.sin.sin_port));
	}
#if DXX_USE_IPv6
	if (a.sa.sa_family == AF_INET6)
		return ::dcx::make_seen_ipv6(a.sin6.sin6_addr.s6_addr, ntohs(a.sin6.sin6_port));
#endif
	return std::nullopt;
}

/* Host: log the public address when the choice changes. */
void log_public_address_choice()
{
	const auto b{Public_tally.best()};
	if (!b)
		return;
	auto text{::dcx::seen_address_text(b->address)};
	if (text == Public_logged)
		return;
	con_printf(CON_NORMAL, "net: public address of this game: %s (%s)", text.c_str(), ::dcx::public_address_label(b->players, b->tracker).c_str());
	Public_logged = std::move(text);
}

/* Byte writers and readers for the message layouts.  The reader turns any
 * overrun into `ok == false` and zero values, so a handler parses first
 * and checks `ok` once.
 */
struct writer
{
	uint8_t *p;
	std::size_t pos{};
	void u8(const uint8_t v)
	{
		p[pos++] = v;
	}
	void u16(const uint16_t v)
	{
		PUT_INTEL_SHORT(&p[pos], v);
		pos += 2;
	}
	void u32(const uint32_t v)
	{
		PUT_INTEL_INT(&p[pos], v);
		pos += 4;
	}
	void bytes(const std::span<const uint8_t> b)
	{
		std::ranges::copy(b, &p[pos]);
		pos += b.size();
	}
	void bytes(const void *const b, const std::size_t n)
	{
		memcpy(&p[pos], b, n);
		pos += n;
	}
	/* A NUL-terminated string, at most `n` characters plus the NUL. */
	void cstring(const char *const s, const std::size_t n)
	{
		const auto len{strnlen(s, n)};
		memcpy(&p[pos], s, len);
		pos += len;
		p[pos++] = 0;
	}
	template <std::size_t N>
	void ntstr(const ntstring<N> &s)
	{
		cstring(s.data(), N);
	}
};

struct reader
{
	std::span<const uint8_t> b;
	std::size_t pos{};
	bool ok{true};
	[[nodiscard]]
	bool has(const std::size_t n) const
	{
		return pos + n <= b.size();
	}
	const uint8_t *take(const std::size_t n)
	{
		if (!has(n))
		{
			ok = false;
			return nullptr;
		}
		const auto r{&b[pos]};
		pos += n;
		return r;
	}
	uint8_t u8()
	{
		const auto r{take(1)};
		return r ? *r : 0;
	}
	uint16_t u16()
	{
		const auto r{take(2)};
		return r ? GET_INTEL_SHORT(r) : 0;
	}
	uint32_t u32()
	{
		const auto r{take(4)};
		return r ? GET_INTEL_INT(r) : 0;
	}
	int32_t i32()
	{
		return static_cast<int32_t>(u32());
	}
	/* A NUL-terminated string of at most N characters into an ntstring. */
	template <std::size_t N>
	void ntstr(ntstring<N> &out)
	{
		const auto rest{b.subspan(std::min(pos, b.size()))};
		const auto nul{std::ranges::find(rest, uint8_t{0})};
		if (nul == rest.end() || static_cast<std::size_t>(nul - rest.begin()) > N)
		{
			ok = false;
			out = {};
			return;
		}
		const std::size_t len{static_cast<std::size_t>(nul - rest.begin())};
		out.copy_if(reinterpret_cast<const char *>(rest.data()), len + 1);
		pos += len + 1;
	}
	[[nodiscard]]
	bool done() const
	{
		return ok && pos == b.size();
	}
};

std::pair<const char *, const char *> GetRankStringWithSpace(const netplayer_info::player_rank r)
{
	return PlayerCfg.NoRankings ? std::pair{"", ""} : std::pair{RankStrings[r], " "};
}

uint32_t random_u32()
{
	static std::optional<std::mt19937> generator;
	if (!generator)
	{
		uint32_t seed{static_cast<uint32_t>(timer_query())};
		try {
			seed ^= std::random_device()();
		} catch (const std::exception &e) {
			con_printf(CON_URGENT, "net: random_device failed (%s); using the timer as seed", e.what());
		}
		generator.emplace(seed);
	}
	return (*generator)();
}

uint32_t random_nonzero_u32()
{
	for (;;)
		if (const auto r{random_u32()})
			return r;
}

[[nodiscard]]
tick_period local_tick_period()
{
	return {::dcx::net_v2::net_seconds(1), netgame_tick_rate_valid(Netgame.TickRate) ? Netgame.TickRate : NETGAME_TICK_RATE_DEFAULT};
}

[[nodiscard]]
connection_config make_connection_config(const uint32_t token, const uint8_t local_player, const uint8_t remote_player)
{
	const auto tick{local_tick_period()};
	return connection_config{
		.session_id = S.session_id,
		.peer_token = token,
		.local_player_id = local_player,
		.remote_player_id = remote_player,
		.tick = tick,
		.max_packets_per_tick = ::dcx::net_v2::NET_V2_DEFAULT_MAX_PACKETS_PER_TICK,
		/* Both ends tick at the host's rate (section 2.3). */
		.peer_tick = tick,
	};
}

void send_raw(const std::span<const uint8_t> bytes, const _sockaddr &to)
{
	if (bytes.empty() || !UDP_Socket[0])
		return;
	dxx_sendto(UDP_Socket[0], bytes, 0, to);
}

void send_unconnected(const _sockaddr &to, const uint32_t session_id, const uint32_t token, const uint8_t player_id, const session_msg type, const std::span<const uint8_t> payload)
{
	packet_buffer buf;
	send_raw(::dcx::net_v2::build_unconnected(buf, session_id, token, player_id, ::dcx::net_v2::to_net_time(timer_query()), type, payload), to);
}

[[nodiscard]]
const char *close_reason_name(const close_reason r)
{
	switch (r)
	{
		case close_reason::none: return "none";
		case close_reason::timeout: return "timeout";
		case close_reason::unacked_timeout: return "unacknowledged for too long";
		case close_reason::queue_overflow: return "queue overflow";
		case close_reason::protocol_error: return "protocol errors";
		case close_reason::stream_stalled: return "stream stalled";
		case close_reason::local: return "closed";
	}
	return "unknown";
}

/* For the game log: kick, deny and leave reasons by name. */
[[nodiscard]]
const char *kick_reason_name(const kick_player_reason r)
{
	switch (r)
	{
		case kick_player_reason::closed: return "game closed";
		case kick_player_reason::full: return "game full";
		case kick_player_reason::endlevel: return "between levels";
		case kick_player_reason::dork: return "not selected";
		case kick_player_reason::aborted: return "game aborted";
		case kick_player_reason::level: return "wrong level";
		case kick_player_reason::kicked: return "kicked";
		case kick_player_reason::version: return "version mismatch";
		case kick_player_reason::duplicate_callsign: return "duplicate callsign";
		case kick_player_reason::queue_overflow: return "link too slow (queue overflow or unacknowledged data)";
		case kick_player_reason::protocol_error: return "protocol errors";
		case kick_player_reason::checksum: return "level checksum mismatch";
		case kick_player_reason::snapshot_failed: return "join not completed";
		case kick_player_reason::timeout: return "timeout";
		case kick_player_reason::quit: return "quit";
		case kick_player_reason::cancelled: return "cancelled";
		case kick_player_reason::host_shutdown: return "host shut down";
	}
	return "unknown";
}

[[nodiscard]]
const char *network_state_name(const network_state s)
{
	switch (s)
	{
		case network_state::menu: return "menu";
		case network_state::playing: return "playing";
		case network_state::browsing: return "browsing";
		case network_state::waiting: return "waiting for the level start";
		case network_state::starting: return "lobby";
		case network_state::endlevel: return "level end";
	}
	return "unknown";
}

[[nodiscard]]
unsigned net_clock_to_ms(const ::dcx::net_v2::net_clock t)
{
	return static_cast<unsigned>((t * 1000) / 65536);
}

/* Section 3.6 / 4.3: generous timeouts while no level runs
 * (net_v2::timeouts_for); applied every frame, so they switch with the
 * level start and end.
 */
void apply_timeouts(connection &c, const ::dcx::net_v2::peer_phase phase)
{
	const auto t{::dcx::net_v2::timeouts_for(Network_status == network_state::playing, phase)};
	const auto &cfg{c.config()};
	if (cfg.timeout != t.idle || cfg.unacked_timeout != t.unacked)
		c.set_timeouts(t.idle, t.unacked);
}

[[nodiscard]]
kick_player_reason kick_reason_from_close(const close_reason r)
{
	switch (r)
	{
		case close_reason::timeout:
			return kick_player_reason::timeout;
		case close_reason::protocol_error:
			return kick_player_reason::protocol_error;
		case close_reason::none:
		case close_reason::unacked_timeout:
		case close_reason::queue_overflow:
		case close_reason::stream_stalled:
		case close_reason::local:
			break;
	}
	return kick_player_reason::queue_overflow;
}

[[nodiscard]]
std::optional<kick_player_reason> build_kick_player_reason_from_untrusted(const uint8_t why)
{
	switch (why)
	{
		case static_cast<uint8_t>(kick_player_reason::closed):
		case static_cast<uint8_t>(kick_player_reason::full):
		case static_cast<uint8_t>(kick_player_reason::endlevel):
		case static_cast<uint8_t>(kick_player_reason::dork):
		case static_cast<uint8_t>(kick_player_reason::aborted):
		case static_cast<uint8_t>(kick_player_reason::level):
		case static_cast<uint8_t>(kick_player_reason::kicked):
		case static_cast<uint8_t>(kick_player_reason::version):
		case static_cast<uint8_t>(kick_player_reason::duplicate_callsign):
		case static_cast<uint8_t>(kick_player_reason::queue_overflow):
		case static_cast<uint8_t>(kick_player_reason::protocol_error):
		case static_cast<uint8_t>(kick_player_reason::checksum):
		case static_cast<uint8_t>(kick_player_reason::snapshot_failed):
		case static_cast<uint8_t>(kick_player_reason::timeout):
		case static_cast<uint8_t>(kick_player_reason::quit):
		case static_cast<uint8_t>(kick_player_reason::cancelled):
		case static_cast<uint8_t>(kick_player_reason::host_shutdown):
			return static_cast<kick_player_reason>(why);
		default:
			return std::nullopt;
	}
}

/* Peer lookup */

[[nodiscard]]
peer *find_peer_by_addr(const _sockaddr &addr)
{
	for (auto &p : S.peers)
		if (p.ph != peer::phase::none && p.addr == addr)
			return &p;
	return nullptr;
}

[[nodiscard]]
peer *find_peer_by_token(const uint32_t token)
{
	if (!token)
		return nullptr;
	for (auto &p : S.peers)
		if (p.ph != peer::phase::none && p.conn && p.token == token)
			return &p;
	return nullptr;
}

[[nodiscard]]
playernum_t peer_slot(const peer &p)
{
	return static_cast<playernum_t>(&p - S.peers.data());
}

[[nodiscard]]
bool peer_receives_broadcasts(const peer &p)
{
	return p.conn && (p.ph == peer::phase::syncing || p.ph == peer::phase::playing);
}

/* Best-effort events (EVENT_U) go only to a peer in the game.  A peer
 * still syncing is applying its level snapshot: an event that creates an
 * object (MULTI_CREATE_EXPLOSION, MULTI_DROP_BLOB) would take an object
 * number from a free list that does not yet know the numbers the snapshot
 * has placed, and overwrite a placed object - a player ship (the snapshot
 * then fails with TXT_NET_SYNC_FAILED) or an object linked into a segment
 * (cross-linked segment lists, which hang the client).  Reliable messages
 * are safe: they are ordered behind the snapshot.  v1 sent a joining
 * player no game data before its sync either.
 */
[[nodiscard]]
bool peer_receives_events(const peer &p)
{
	return p.conn && p.ph == peer::phase::playing;
}

/* Game data (LEGACY_MDATA, EVENT_U, INPUT, the endlevel status) is
 * accepted and relayed only from a peer in the game.  A peer still
 * joining or syncing is not in the level yet; a closing one lingers only
 * so that its LEAVE or KICK is acknowledged.
 */
[[nodiscard]]
bool peer_sends_game_data(const peer &p)
{
	return p.ph == peer::phase::playing;
}

/* Host: a join in progress is being served (v1 refused a join while
 * Network_send_objects or Network_sending_extras was set): a peer between
 * JOIN_ACCEPT and CLIENT_READY, or the extras after it.  A peer at
 * `ignore_addr` does not count: it is the same client asking again.
 */
[[nodiscard]]
bool join_in_progress(const _sockaddr &ignore_addr)
{
	per_player_array<::dcx::net_v2::join_peer_view> views{};
	for (auto &&[i, p] : enumerate(S.peers))
		views[i] = {.phase = p.ph, .same_address = p.addr == ignore_addr};
	return ::dcx::net_v2::join_in_progress(views, Network_sending_extras || !S.extras_queue.empty());
}

void drop_peer(peer &p)
{
	if (multi_i_am_master())
	{
		Public_tally.forget(peer_slot(p));
		/* Its ship goes with it (custom ships, net_ships.cpp). */
		if (p.conn)
		{
			net_ships_slot_cleared(peer_slot(p));
			net_mission_slot_cleared(peer_slot(p));
		}
	}
	p.conn.reset();
	p.pump_bytes = BACKLOG_PUMP_BYTES;
	p.ph = peer::phase::none;
	p.token = 0;
	p.nonce = 0;
	p.backlog.clear();
	p.backlog_bytes = 0;
	p.has_ready = false;
	p.in_level = false;
	p.next_pings = 0;
}

/* Reliable messages to a peer.  Messages beyond the connection's
 * comfortable fill go to the backlog, in order, and are moved to the
 * connection by pump_peer.
 */
void pump_peer(peer &p)
{
	if (!p.conn || p.backlog.empty())
		return;
	const auto stats{p.conn->stats()};
	std::size_t queued_bytes{stats.queue_bytes};
	std::size_t queued_messages{stats.queue_messages};
	while (!p.backlog.empty() && queued_bytes < p.pump_bytes && queued_messages < BACKLOG_PUMP_MESSAGES)
	{
		auto &m{p.backlog.front()};
		const auto size{m.payload.size()};
		if (p.conn->enqueue_reliable(static_cast<uint8_t>(m.type), m.payload) != ::dcx::net_v2::enqueue_result::ok)
			break;
		queued_bytes += size;
		++queued_messages;
		p.backlog_bytes -= size;
		p.backlog.pop_front();
	}
}

void peer_queue(peer &p, const session_msg type, const std::span<const uint8_t> payload)
{
	if (!p.conn || p.conn->state() == connection_state::closed)
		return;
	if (payload.size() > NET_V2_MAX_MESSAGE)
	{
		con_printf(CON_URGENT, "net: message type %u of %zu bytes to P#%u is too large; dropped", static_cast<unsigned>(type), payload.size(), peer_slot(p));
		return;
	}
	if (p.backlog.empty())
	{
		const auto stats{p.conn->stats()};
		if (stats.queue_bytes + payload.size() < p.pump_bytes && stats.queue_messages < BACKLOG_PUMP_MESSAGES)
		{
			p.conn->enqueue_reliable(static_cast<uint8_t>(type), payload);
			return;
		}
	}
	if (p.backlog_bytes + payload.size() > BACKLOG_MAX_BYTES)
	{
		con_printf(CON_URGENT, "net: P#%u cannot keep up (%zu bytes backlog); closing", peer_slot(p), p.backlog_bytes);
		p.conn->close();
		return;
	}
	p.backlog_bytes += payload.size();
	p.backlog.push_back({type, std::vector<uint8_t>(payload.begin(), payload.end())});
}

/* Host: to every peer in the game; client: to the host. */
void broadcast_reliable(const session_msg type, const std::span<const uint8_t> payload, const playernum_t exclude = MAX_PLAYERS)
{
	if (multi_i_am_master())
	{
		for (auto &&[i, p] : enumerate(S.peers))
			if (i != exclude && peer_receives_broadcasts(p))
				peer_queue(p, type, payload);
	}
	else if (auto &p{S.peers[0]}; peer_receives_broadcasts(p))
		peer_queue(p, type, payload);
}

void send_to_slot(const playernum_t pnum, const session_msg type, const std::span<const uint8_t> payload)
{
	if (pnum >= MAX_PLAYERS)
		return;
	if (auto &p{S.peers[pnum]}; p.conn)
		peer_queue(p, type, payload);
}

/* The game state as advertised: `endlevel` while the reactor is destroyed
 * or the time limit is about to end (the v1 get_effective_netgame_status).
 */
[[nodiscard]]
network_state effective_netgame_status()
{
	if (Network_status == network_state::endlevel)
		return network_state::endlevel;
	if (LevelUniqueObjectState.ControlCenterState.Control_center_destroyed)
		return network_state::endlevel;
	if (Netgame.PlayTimeAllowed.count())
	{
		const auto TicksPlayTimeRemaining = Netgame.PlayTimeAllowed - ThisLevelTime;
		if (TicksPlayTimeRemaining.count() < i2f(30))
			return network_state::endlevel;
	}
	return Netgame.game_status;
}

/* Wire layouts of the game description (sections 4.1 and 4.5) */

[[nodiscard]]
std::span<const uint8_t> build_game_info_lite(std::array<uint8_t, 128> &buf)
{
	writer w{buf.data()};
	w.bytes(Game_id);
	w.u16(Program_version.major);
	w.u16(Program_version.minor);
	w.u16(Program_version.micro);
	w.u32(S.session_id);
	w.u32(static_cast<uint32_t>(Netgame.levelnum));
	w.u8(underlying_value(Netgame.gamemode));
	w.u8(Netgame.RefusePlayers);
	w.u8(underlying_value(Netgame.difficulty));
	w.u8(underlying_value(effective_netgame_status()));
	w.u8(Netgame.numconnected);
	w.u8(Netgame.max_numplayers);
	w.u8(underlying_value(Netgame.game_flag));
	w.u8(Netgame.TickRate);
	w.ntstr(Netgame.game_name);
	w.ntstr(Netgame.mission_title);
	w.ntstr(Netgame.mission_name);
	return std::span<const uint8_t>(buf).first(w.pos);
}

[[nodiscard]]
std::optional<UDP_netgame_info_lite> parse_game_info_lite(const std::span<const uint8_t> payload, const _sockaddr &game_addr)
{
	reader r{payload};
	game_id_type id{};
	if (const auto p{r.take(4)})
		std::copy(p, p + 4, id.begin());
	if (!r.ok || id != Game_id)
		return std::nullopt;
	UDP_netgame_info_lite g{};
	g.game_addr = game_addr;
	g.program_iver[0] = r.u16();
	g.program_iver[1] = r.u16();
	g.program_iver[2] = r.u16();
	g.session_id = r.u32();
	g.levelnum = r.i32();
	g.gamemode = network_game_type{r.u8()};
	g.RefusePlayers = r.u8();
	g.difficulty = r.u8();
	const auto status{build_network_state_from_untrusted(r.u8())};
	g.numconnected = r.u8();
	g.max_numplayers = r.u8();
	g.game_flag = netgame_rule_flags{r.u8()};
	g.tick_rate = r.u8();
	r.ntstr(g.game_name);
	r.ntstr(g.mission_title);
	r.ntstr(g.mission_name);
	if (!r.done() || !status)
		return std::nullopt;
	g.game_status = *status;
	if (g.program_iver[0] != Program_version.major || g.program_iver[1] != Program_version.minor || g.program_iver[2] != Program_version.micro)
		return std::nullopt;
	if (!g.session_id)
		return std::nullopt;
	g.last_seen = timer_query();
	return g;
}

void write_game_settings(writer &w)
{
	w.u32(static_cast<uint32_t>(Netgame.levelnum));
	w.u8(underlying_value(Netgame.gamemode));
	w.u8(Netgame.RefusePlayers);
	w.u8(underlying_value(Netgame.difficulty));
	w.u8(underlying_value(effective_netgame_status()));
	w.u8(Netgame.numplayers);
	w.u8(Netgame.max_numplayers);
	w.u8(Netgame.numconnected);
	w.u8(underlying_value(Netgame.game_flag));
	w.u8(Netgame.team_vector);
	w.u8(Netgame.TickRate);
	w.u32(underlying_value(Netgame.AllowedItems));
	/* In cooperative games, never shuffle. */
	w.u32(+(Game_mode & GM_MULTI_COOP) ? 0 : Netgame.ShufflePowerupSeed);
	w.u8(Netgame.SecludedSpawns);
	w.u16(static_cast<uint16_t>(Netgame.SpawnGrantedItems.mask));
	w.u16(Netgame.DuplicatePowerups.get_packed_field());
#if DXX_BUILD_DESCENT == 1
	w.u8(0);
	w.u8(0);
	w.u8(0);
	w.u8(0);
#elif DXX_BUILD_DESCENT == 2
	w.u8(Netgame.Allow_marker_view);
	w.u8(Netgame.AlwaysLighting);
	w.u8(Netgame.ThiefModifierFlags);
	w.u8(Netgame.AllowGuidebot);
#endif
	w.u8(Netgame.ShowEnemyNames);
	w.u8(Netgame.BrightPlayers);
	w.u8(Netgame.InvulAppear);
	w.u8(Netgame.NoFriendlyFire);
	w.u8(Netgame.MouselookFlags);
	w.u8(Netgame.PitchLockFlags);
	/* Protocol 111: the rules of capture the flag (Classic), where the
	 * reserved byte (once PacketLossPrevention) was.
	 */
	w.u8(Netgame.CtfClassicFlags);
	/* Protocol 114: team-side spawns and the respawn delay. */
	w.u8(Netgame.TeamSpawns);
	w.u8(Netgame.RespawnDelay);
	w.u32(static_cast<uint32_t>(Netgame.KillGoal));
	w.u32(Netgame.PlayTimeAllowed.count());
	for (auto &i : Netgame.team_name)
		w.bytes(i.operator const char *(), CALLSIGN_LEN + 1);
	w.ntstr(Netgame.game_name);
	w.ntstr(Netgame.mission_title);
	w.ntstr(Netgame.mission_name);
	/* Protocol 115: the mission's bundle (net_v2_mission.h). */
	std::array<uint8_t, ::dcx::net_v2::mission_announcement::SIZE> mission;
	net_mission_write_announcement(mission.data());
	w.bytes(mission.data(), mission.size());
}

/* Reads GAME_SETTINGS into Netgame.  Leaves the reader positioned after
 * the strings; the caller checks `ok`.
 */
void read_game_settings(reader &r)
{
	const auto levelnum{r.i32()};
	const auto gamemode{network_game_type{r.u8()}};
	const auto refuse{r.u8()};
	const auto difficulty{r.u8()};
	const auto status{build_network_state_from_untrusted(r.u8())};
	const auto numplayers{r.u8()};
	const auto max_numplayers{r.u8()};
	const auto numconnected{r.u8()};
	const auto game_flag{netgame_rule_flags{r.u8()}};
	const auto team_vector{r.u8()};
	const auto tick_rate{r.u8()};
	const auto allowed{r.u32()};
	const auto seed{r.u32()};
	const auto secluded{r.u8()};
	const auto granted{r.u16()};
	const auto duplicate{r.u16()};
	const auto d2a{r.u8()};
	const auto d2b{r.u8()};
	const auto d2c{r.u8()};
	const auto d2d{r.u8()};
	const auto show_names{r.u8()};
	const auto bright{r.u8()};
	const auto invul{r.u8()};
	const auto no_ff{r.u8()};
	const auto mouselook{r.u8()};
	const auto pitchlock{r.u8()};
	const auto ctf_classic{r.u8()};
	const auto team_spawns{r.u8()};
	const auto respawn_delay{r.u8()};
	const auto killgoal{r.i32()};
	const auto playtime{r.i32()};
	per_team_array<callsign_t> team_name;
	for (auto &i : team_name)
		if (const auto p{r.take(CALLSIGN_LEN + 1)})
			i.copy(std::span<const char, CALLSIGN_LEN + 1>(reinterpret_cast<const char *>(p), CALLSIGN_LEN + 1));
	ntstring<NETGAME_NAME_LEN> game_name;
	ntstring<MISSION_NAME_LEN> mission_title;
	ntstring<8> mission_name;
	r.ntstr(game_name);
	r.ntstr(mission_title);
	r.ntstr(mission_name);
	const auto mission{r.take(::dcx::net_v2::mission_announcement::SIZE)};
	if (!r.ok || !mission || !status || max_numplayers > MAX_PLAYERS || numplayers > MAX_PLAYERS)
	{
		r.ok = false;
		return;
	}
	Netgame.levelnum = levelnum;
	Netgame.gamemode = gamemode;
	Netgame.RefusePlayers = refuse;
	Netgame.difficulty = cast_clamp_difficulty(difficulty);
	Netgame.game_status = *status;
	Netgame.numplayers = numplayers;
	Netgame.max_numplayers = max_numplayers;
	Netgame.numconnected = numconnected;
	Netgame.game_flag = game_flag;
	Netgame.team_vector = team_vector;
	Netgame.TickRate = netgame_tick_rate_valid(tick_rate) ? tick_rate : NETGAME_TICK_RATE_DEFAULT;
	Netgame.AllowedItems = static_cast<netflag_flag>(allowed);
	Netgame.ShufflePowerupSeed = seed;
	Netgame.SecludedSpawns = secluded;
	Netgame.SpawnGrantedItems = netgrant_flag{static_cast<std::underlying_type<netgrant_flag>::type>(granted)};
	Netgame.DuplicatePowerups.set_packed_field(duplicate);
#if DXX_BUILD_DESCENT == 1
	(void)d2a;
	(void)d2b;
	(void)d2c;
	(void)d2d;
#elif DXX_BUILD_DESCENT == 2
	if (unlikely(map_granted_flags_to_laser_level(Netgame.SpawnGrantedItems) > MAX_SUPER_LASER_LEVEL))
		/* Bogus input - reject whole entry */
		Netgame.SpawnGrantedItems = netgrant_flag::None;
	Netgame.Allow_marker_view = d2a;
	Netgame.AlwaysLighting = d2b;
	Netgame.ThiefModifierFlags = d2c;
	Netgame.AllowGuidebot = d2d;
#endif
	Netgame.ShowEnemyNames = show_names;
	Netgame.BrightPlayers = bright;
	Netgame.InvulAppear = invul;
	Netgame.NoFriendlyFire = no_ff;
	Netgame.MouselookFlags = mouselook;
	Netgame.PitchLockFlags = pitchlock;
	Netgame.CtfClassicFlags = ctf_classic & ::dcx::net_v2::CTF_RULES_KNOWN;
	Netgame.TeamSpawns = static_cast<uint8_t>(::dcx::team_spawn::rule_from_byte(team_spawns));
	Netgame.RespawnDelay = std::min<uint8_t>(respawn_delay, ::dcx::team_spawn::RESPAWN_DELAY_LIMIT);
	Netgame.KillGoal = killgoal;
	Netgame.PlayTimeAllowed = d_time_fix(playtime);
	Netgame.team_name = team_name;
	Netgame.game_name = game_name;
	Netgame.mission_title = mission_title;
	Netgame.mission_name = mission_name;
	net_mission_read_announcement(mission);
}

void write_player_list(writer &w)
{
	for (auto &&[i, np] : enumerate(Netgame.players))
	{
		w.bytes(&np.callsign[0u], CALLSIGN_LEN + 1);
		/* Bots section 2.2: bit 7 marks a bot. */
		w.u8(::dcx::net_v2::encode_list_connected(underlying_value(np.connected), player_is_bot(i)));
		w.u8(underlying_value(np.rank));
		w.u8((Netgame.team_vector >> i) & 1);
	}
}

void read_player_list(reader &r)
{
	for (auto &&[i, np] : enumerate(Netgame.players))
	{
		if (const auto p{r.take(CALLSIGN_LEN + 1)})
			np.callsign.copy_lower(std::span<const char, CALLSIGN_LEN>(reinterpret_cast<const char *>(p), CALLSIGN_LEN));
		const auto c{::dcx::net_v2::decode_list_connected(r.u8())};
		np.connected = player_connection_status{c.status};
		/* Only clients read the list (the host sets its own flags when
		 * it allocates the bots, after a session reset).
		 */
		set_player_is_bot(i, c.bot);
		np.rank = build_rank_from_untrusted(r.u8());
		r.u8();	/* team: derived from team_vector */
		/* Bots section 6.4: the host renamed a bot during the game. */
		if (c.bot && Network_status == network_state::playing && !multi_i_am_master() && i < N_players)
			if (auto &plr{*vmplayerptr(static_cast<playernum_t>(i))}; plr.connected != player_connection_status::disconnected && np.callsign[0u] && d_stricmp(plr.callsign, np.callsign))
			{
				con_printf(CON_NORMAL, "net: bot P#%u '%s' is now '%s'", static_cast<unsigned>(i), static_cast<const char *>(plr.callsign), static_cast<const char *>(np.callsign));
				plr.callsign = np.callsign;
			}
	}
}

/* Section 4.1: GAME_INFO is the session id, GAME_SETTINGS and PLAYER_LIST. */
[[nodiscard]]
std::span<const uint8_t> build_game_info(std::array<uint8_t, 4 + GAME_SETTINGS_FIXED_SIZE + 3 * 32 + PLAYER_LIST_SIZE> &buf)
{
	writer w{buf.data()};
	w.u32(S.session_id);
	write_game_settings(w);
	write_player_list(w);
	return std::span<const uint8_t>(buf).first(w.pos);
}

void send_game_settings(peer &p)
{
	std::array<uint8_t, GAME_SETTINGS_FIXED_SIZE + 3 * 32> buf;
	writer w{buf.data()};
	write_game_settings(w);
	peer_queue(p, session_msg::game_settings, std::span<const uint8_t>(buf).first(w.pos));
}

void send_player_list(peer &p)
{
	std::array<uint8_t, PLAYER_LIST_SIZE> buf;
	writer w{buf.data()};
	write_player_list(w);
	peer_queue(p, session_msg::player_list, buf);
}

/* Blown-up monitors (unchanged from v1) */

void process_monitor_vector(uint32_t vector)
{
	auto &Effects = LevelUniqueEffectsClipState.Effects;
	auto &TmapInfo = LevelUniqueTmapInfoState.TmapInfo;
	if (!vector)
		return;
	range_for (unique_segment &seg, vmsegptr)
	{
		range_for (auto &j, seg.sides)
		{
			const auto tm = j.tmap_num2;
			if (tm == texture2_value::None)
				continue;
			const auto opt_eclip_num{Effects.valid_index(TmapInfo[get_texture_index(tm)].eclip_num)};
			if (!opt_eclip_num)
				continue;
			const auto bm{Effects[*opt_eclip_num].dest_bm_num};
			if (bm >= Textures.size())
				continue;
			{
				if (vector & 1)
				{
					j.tmap_num2 = build_texture2_value(bm, get_texture_rotation_high(tm));
				}
				if (!(vector >>= 1))
					return;
			}
		}
	}
}

class blown_bitmap_array
{
	typedef int T;
	using array_t = std::array<T, 32>;
	typedef array_t::const_iterator const_iterator;
	array_t a;
	array_t::iterator e = a.begin();
public:
	bool exists(T t) const
	{
		const_iterator ce = e;
		return std::find(a.begin(), ce, t) != ce;
	}
	void insert_unique(T t)
	{
		if (exists(t))
			return;
		if (e == a.end())
		{
			LevelError("too many blown bitmaps; ignoring bitmap %i.", t);
			return;
		}
		*e = t;
		++e;
	}
};

unsigned create_monitor_vector()
{
	auto &Effects = LevelUniqueEffectsClipState.Effects;
	auto &TmapInfo = LevelUniqueTmapInfoState.TmapInfo;
	blown_bitmap_array blown_bitmaps;
	constexpr size_t max_textures = Textures.size();
	range_for (auto &i, partial_const_range(Effects, Num_effects))
	{
		if (i.dest_bm_num < max_textures)
		{
			blown_bitmaps.insert_unique(i.dest_bm_num);
		}
	}
	unsigned monitor_num{0};
	unsigned vector{0};
	range_for (const auto &&seg, vcsegptridx)
	{
		range_for (auto &j, seg->unique_segment::sides)
		{
			const auto tm2 = j.tmap_num2;
			if (tm2 == texture2_value::None)
				continue;
			const auto masked_tm2 = get_texture_index(tm2);
			const auto ec{TmapInfo[masked_tm2].eclip_num};
			const auto opt_eclip_num{Effects.valid_index(ec)};
			{
				if (opt_eclip_num &&
					Effects[*opt_eclip_num].dest_bm_num != texture_index{UINT16_MAX})
				{
				}
				else if (blown_bitmaps.exists(masked_tm2))
				{
					if (monitor_num >= 8 * sizeof(vector))
					{
						LevelError("too many blown monitors; ignoring segment %hu.", seg.get_unchecked_index());
						return vector;
					}
							vector |= (1 << monitor_num);
				}
				else
					continue;
				monitor_num++;
			}
		}
	}
	return(vector);
}

/* Fill the score-related fields of Netgame from the game (the v1 rejoin
 * sync did this before sending).
 */
void fill_netgame_scores()
{
	auto &Objects = LevelUniqueObjectState.Objects;
	auto &vcobjptr = Objects.vcptr;
	Netgame.kills = kill_matrix;
	for (unsigned j = 0; j < MAX_PLAYERS; ++j)
	{
		auto &objp = *vcobjptr(vcplayerptr(j)->objnum);
		auto &player_info = objp.ctype.player_info;
		Netgame.killed[j] = player_info.net_killed_total;
		Netgame.player_kills[j] = player_info.net_kills_total;
		Netgame.player_score[j] = player_info.mission.score;
	}
	Netgame.team_kills = team_kills;
}

/* SNAPSHOT_GAME (section 4.4): scores, states and level status. */
[[nodiscard]]
std::span<const uint8_t> build_snapshot_game(std::array<uint8_t, SNAPSHOT_GAME_SIZE> &buf, const uint16_t part, const bool in_progress)
{
	auto &Objects = LevelUniqueObjectState.Objects;
	auto &vcobjptr = Objects.vcptr;
	auto &LevelUniqueControlCenterState = LevelUniqueObjectState.ControlCenterState;
	writer w{buf.data()};
	w.u16(part);
	for (auto &i : Netgame.kills)
		for (auto &j : i)
			w.u16(j);
	for (auto &i : Netgame.killed)
		w.u16(i);
	for (auto &i : Netgame.player_kills)
		w.u16(i);
	for (auto &i : Netgame.player_score)
		w.u32(i);
	w.u16(static_cast<uint16_t>(Netgame.team_kills[team_number::blue]));
	w.u16(static_cast<uint16_t>(Netgame.team_kills[team_number::red]));
	w.u8(Netgame.team_vector);
	w.u8(Bounty_target);
	for (unsigned i = 0; i < MAX_PLAYERS; ++i)
		w.u8(vcobjptr(vcplayerptr(i)->objnum)->ctype.player_info.KillGoalCount);
	w.u32(in_progress ? create_monitor_vector() : 0);
	w.u16(static_cast<uint16_t>(LevelUniqueControlCenterState.Control_center_destroyed ? LevelUniqueControlCenterState.Countdown_seconds_left : -1));
	/* Thief's stolen items: the extras send MULTI_STOLEN_ITEMS; reserved
	 * here for the stage 5 layout.
	 */
	for (unsigned i = 0; i < 10; ++i)
		w.u8(0);
	w.u32(static_cast<uint32_t>(in_progress ? get_local_player().time_level : 0));
	for (auto &np : Netgame.players)
		w.u8(underlying_value(np.connected));
	assert(w.pos == SNAPSHOT_GAME_SIZE);
	return buf;
}

[[nodiscard]]
bool apply_snapshot_game(const std::span<const uint8_t> payload)
{
	if (payload.size() != SNAPSHOT_GAME_SIZE)
		return false;
	reader r{payload};
	r.u16();	/* part */
	for (auto &i : Netgame.kills)
		for (auto &j : i)
			j = r.u16();
	for (auto &i : Netgame.killed)
		i = r.u16();
	for (auto &i : Netgame.player_kills)
		i = r.u16();
	for (auto &i : Netgame.player_score)
		i = r.u32();
	Netgame.team_kills[team_number::blue] = static_cast<int16_t>(r.u16());
	Netgame.team_kills[team_number::red] = static_cast<int16_t>(r.u16());
	Netgame.team_vector = r.u8();
	r.u8();	/* Bounty_target: MULTI_DO_BOUNTY of the extras sets it */
	for (unsigned i = 0; i < MAX_PLAYERS; ++i)
		r.u8();	/* KillGoalCount: MULTI_KILLGOALS of the extras sets them */
	Netgame.monitor_vector = static_cast<int>(r.u32());
	r.u16();	/* countdown: the level-end status carries it */
	r.take(10);
	Netgame.level_time = static_cast<fix>(r.u32());
	for (auto &np : Netgame.players)
		np.connected = player_connection_status{r.u8()};
	return r.done();
}

/* State synchronisation (section 5, stage 2): the client's INPUT chunk at
 * every tick, the host's state bundle (STATE chunk) to every client at
 * every tick, and the snapshots both feed to the interpolation of the
 * remote ships and guided missiles (net_interp.cpp).  The host accepts
 * every INPUT (the checks of section 5.5 are stage 4), so it never sends
 * a correction yet; the client applies one if it gets it (section 5.6).
 */

using ::dcx::net_v2::state_bundle;
using ::dcx::net_v2::input_chunk;
using ::dcx::net_v2::player_record;
using ::dcx::net_v2::guided_record;
using ::dcx::net_v2::net_pose;
using ::dcx::net_v2::net_vec;
using ::dcx::net_v2::net_quat;
using ::dcx::net_v2::state_flag;
using ::dcx::net_v2::player_record_flag;
using ::dcx::net_v2::input_flag;
using ::dcx::net_v2::flag_bit;
using ::dcx::net_v2::net_clock;
using ::dcx::net_interp::host_clock;
using ::dcx::net_interp::snapshot;

[[nodiscard]]
bool state_sending_allowed()
{
	if (!(Game_mode & GM_NETWORK))
		return false;
	if (get_local_player().connected != player_connection_status::playing)
		return false;
	return Network_status == network_state::playing || Network_status == network_state::endlevel;
}

[[nodiscard]]
bool state_receiving_allowed()
{
	return +(Game_mode & GM_NETWORK) && (Network_status == network_state::playing || Network_status == network_state::endlevel);
}

[[nodiscard]]
bool segment_valid(const uint16_t s)
{
	return s < LevelSharedSegmentState.get_segments().get_count();
}

[[nodiscard]]
net_vec to_net_vec(const vms_vector &v)
{
	return {v.x, v.y, v.z};
}

/* An object's pose as it goes on the wire (velocities quantised, so that
 * the sender's snapshots equal the receivers').
 */
[[nodiscard]]
net_pose pose_of(const object_base &obj)
{
	const auto q{build_quaternionpos(obj)};
	return net_pose{
		.orient = net_quat{q.orient.w, q.orient.x, q.orient.y, q.orient.z},
		.pos = to_net_vec(q.pos),
		.segment = static_cast<uint16_t>(q.segment),
		.vel = ::dcx::net_v2::quantised_velocity(to_net_vec(q.vel)),
		.rotvel = ::dcx::net_v2::quantised_rotvel(to_net_vec(q.rotvel)),
	};
}

[[nodiscard]]
snapshot snapshot_of(const host_clock t, const net_pose &p, const bool alive)
{
	return snapshot{
		.time = t,
		.pos = p.pos,
		.orient = p.orient,
		.segment = p.segment,
		.vel = p.vel,
		.rotvel = p.rotvel,
		.alive = alive,
	};
}

[[nodiscard]]
snapshot snapshot_of(const host_clock t, const guided_record &g)
{
	return snapshot{
		.time = t,
		.pos = g.pos,
		.orient = g.orient,
		.segment = g.segment,
		.vel = g.vel,
		.rotvel = {},
		.alive = true,
	};
}

[[nodiscard]]
bool local_ship_alive()
{
	auto &vmobjptr = LevelUniqueObjectState.Objects.vmptr;
	return get_local_plrobj().type == object_type::OBJ_PLAYER && Player_dead_state == player_dead_state::no;
}

/* The local ship is in the level: alive, or tumbling in its death
 * sequence until it explodes (dead_player_frame turns it into a ghost
 * when it sends MULTI_PLAYER_DERES).  Its records stay live meanwhile,
 * with `dying`, so that the others see the tumble where it happens, as
 * v1's position packets showed it, and the explosion and the eggs where
 * the owner has them.
 */
[[nodiscard]]
bool local_ship_in_level()
{
	auto &vmobjptr = LevelUniqueObjectState.Objects.vmptr;
	return get_local_plrobj().type == object_type::OBJ_PLAYER;
}

#if DXX_BUILD_DESCENT == 2
/* The guided missile player `pnum` steers on this machine, if any. */
[[nodiscard]]
std::optional<guided_record> local_guided_record(const playernum_t pnum)
{
	auto &Objects = LevelUniqueObjectState.Objects;
	const auto &&gim = LevelUniqueObjectState.Guided_missile.get_player_active_guided_missile(Objects.vmptridx, pnum);
	if (gim == nullptr)
		return std::nullopt;
	const auto p{pose_of(*gim)};
	return guided_record{
		.pid = static_cast<uint8_t>(pnum),
		/* The missile's id (its FIRE, stage 4). */
		.id = net_combat_netid_of(gim),
		.gen = multi_guided_generation(pnum),
		.orient = p.orient,
		.pos = p.pos,
		.segment = p.segment,
		.vel = p.vel,
	};
}
#endif

/* Client: the INPUT chunk of this tick (section 5.3).  Nothing is sent
 * before the clock offset is known, since `sample_time` would be
 * meaningless; that is one round trip after the connection opened.
 */
void set_input_for_host(peer &p)
{
	auto &c = *p.conn;
	if (!c.clock_valid())
		return;
	auto &vmobjptr = LevelUniqueObjectState.Objects.vmptr;
	const auto &plrobj = get_local_plrobj();
	auto &player_info = plrobj.ctype.player_info;
	input_chunk in;
	in.seq = ++S.input_seq;
	const host_clock est{S.now + c.clock_offset()};
	in.sample_time = ::dcx::net_v2::to_net_time(est);
	in.view_time = ::dcx::net_v2::to_net_time(est - interp::view_delay());
	if (local_ship_in_level())
	{
		in.flags |= flag_bit(input_flag::alive);
		if (Player_dead_state != player_dead_state::no)
			in.flags |= flag_bit(input_flag::dying);
	}
#if DXX_BUILD_DESCENT == 2
	if (+(player_info.powerup_flags & player_flag::headlight_on))
		in.flags |= flag_bit(input_flag::headlight);
	/* Section 5.3 bit 1: the afterburner pushes this tick (the host's
	 * movement recording reads it, Documentation/movement-recording.md).
	 */
	if (Controls.state.afterburner && +(player_info.powerup_flags & player_flag::afterburner) && Afterburner_charge > 0 && Player_dead_state == player_dead_state::no)
		in.flags |= flag_bit(input_flag::afterburner);
#endif
	in.pose = pose_of(plrobj);
	in.weapon = static_cast<uint8_t>((underlying_value(player_info.Primary_weapon.get_active()) & 0x0f) | ((underlying_value(player_info.Secondary_weapon.get_active()) & 0x0f) << 4));
#if DXX_BUILD_DESCENT == 2
	in.guided = local_guided_record(Player_num);
#endif
	/* -sharemoves: the pilot's controls for the host's movement
	 * recording (section 5.3, protocol 107).  Nothing without consent.
	 */
	if (Player_dead_state == player_dead_state::no && local_ship_in_level())
		if (::dcx::movrec::control_array ctl; movement_record_shared_controls(ctl))
			in.controls = ctl;
	std::array<uint8_t, ::dcx::net_v2::NET_V2_MAX_INPUT_SIZE> buf;
	const auto n{::dcx::net_v2::write_input(buf, in)};
	c.set_unreliable_state(chunk_type::input, std::span<const uint8_t>(buf).first(n));
}

/* Host: an INPUT chunk from the peer in slot `p` (section 5.5 step 6,
 * without the checks of steps 1 to 4: every INPUT is accepted).  Only the
 * newest `input_seq` counts; the stored time is the sample time, clamped
 * into the last second and after the previous one, so that the history
 * stays ordered whatever the client's clock says.
 */
void apply_input(peer &p, const std::span<const uint8_t> payload)
{
	if (!state_receiving_allowed())
		return;
	auto in{::dcx::net_v2::read_input(payload)};
	if (!in)
		return;
	const auto slot{peer_slot(p)};
	if (slot == Player_num || slot >= N_players || slot >= MAX_PLAYERS)
		return;
	auto &st = S.inputs[slot];
	if (st.valid && ::dcx::net_v2::seq_diff(in->seq, st.input.seq) <= 0)
		return;
	if (!segment_valid(in->pose.segment))
		return;
	if (vcplayerptr(slot)->connected != player_connection_status::playing)
		return;
	if (in->guided && (in->guided->pid != slot || !segment_valid(in->guided->segment)))
		in->guided.reset();
	host_clock t{::dcx::net_interp::unwrap(in->sample_time, S.now)};
	t = std::clamp<host_clock>(t, S.now - ::dcx::net_v2::net_seconds(1), S.now);
	if (st.valid && t <= st.time)
		t = st.time + 1;
	st.valid = true;
	st.input = *in;
	st.time = t;
	st.arrival = S.now;
	Netgame.players[slot].LastPacketTime = S.now;
	if (in->has_flag(input_flag::alive))
	{
		interp::receive_ship(slot, snapshot_of(t, in->pose, true), S.now);
		/* Stage 4: the history the hits are judged against. */
		net_combat_host_position(slot, t, vms_vector{in->pose.pos.x, in->pose.pos.y, in->pose.pos.z}, segnum_t{in->pose.segment});
	}
	else
		interp::receive_ghost(slot);
	if (in->guided)
		interp::receive_guided(slot, in->guided->id, in->guided->gen, snapshot_of(t, *in->guided), S.now);
}

/* The flags and inventory fields of a player record from this machine's
 * copy of the player's object (section 5.2).  In stage 2 the receivers
 * only read `alive`: shields, energy, cloak and the rest are still the
 * owner's and reach the others through the v1 messages (stage 4).
 */
void fill_record_status(player_record &rec, const object &obj, const uint8_t primary)
{
	auto &player_info = obj.ctype.player_info;
	const auto &pf = player_info.powerup_flags;
	if (+(pf & player_flag::cloaked))
		rec.flags |= flag_bit(player_record_flag::cloaked);
	if (+(pf & player_flag::invulnerable))
		rec.flags |= flag_bit(player_record_flag::invulnerable);
#if DXX_BUILD_DESCENT == 2
	if (+(pf & player_flag::headlight_on))
		rec.flags |= flag_bit(player_record_flag::headlight);
	if (+(pf & player_flag::has_team_flag))
		rec.flags |= flag_bit(player_record_flag::flag_or_orbs);
#endif
	rec.shields = obj.shields;
	rec.energy = player_info.energy;
	rec.weapon = static_cast<uint8_t>((static_cast<unsigned>(player_info.laser_level) & 0x07) | (+(pf & player_flag::quad_lasers) ? 0x08 : 0) | ((primary & 0x0f) << 4));
}

/* Host: the parts of the bundle that are the same for every recipient. */
void build_common_bundle(state_bundle &s)
{
	auto &Objects = LevelUniqueObjectState.Objects;
	auto &vcobjptr = Objects.vcptr;
	s.tick = S.tick.tick();
	s.host_time = ::dcx::net_v2::to_net_time(S.now);
	s.level_time = static_cast<int32_t>(ThisLevelTime.count());
	if (LevelUniqueObjectState.ControlCenterState.Control_center_destroyed)
		s.your_flags |= flag_bit(state_flag::countdown);
	const auto period{local_tick_period().units()};
	const unsigned n{std::min<unsigned>(N_players, MAX_PLAYERS)};
	for (playernum_t i = 0; i < n; ++i)
	{
		auto &plr = *vcplayerptr(i);
		if (plr.connected != player_connection_status::playing)
			continue;
		auto &obj = *vcobjptr(plr.objnum);
		player_record rec;
		/* The host's own ship, and the ships of its bots
		 * (Documentation/multiplayer-bots.md section 2.2): the pose from
		 * the object, sampled now.
		 */
		if (const bool bot{bot_is_local(i)}; i == Player_num || bot)
		{
			if (bot ? obj.type != object_type::OBJ_PLAYER : !local_ship_in_level())
			{
				rec.flags = flag_bit(player_record_flag::ghost);
				s.players[i] = rec;
				continue;
			}
			rec.pose = pose_of(obj);
			rec.flags = flag_bit(player_record_flag::alive);
			if (bot ? bot_ship_dying(i) : Player_dead_state != player_dead_state::no)
				rec.flags |= flag_bit(player_record_flag::dying);
			fill_record_status(rec, obj, underlying_value(obj.ctype.player_info.Primary_weapon.get_active()));
		}
		else
		{
			auto &st = S.inputs[i];
			if (!st.valid)
			{
				rec.flags = flag_bit(player_record_flag::ghost);
				s.players[i] = rec;
				interp::set_lag_age(i, 0);
				continue;
			}
			/* Section 5.2: `input_age` in ticks since the INPUT arrived;
			 * `sample_age` since it was sampled, which also counts the
			 * player's uplink: that is what the lag marker shows.
			 */
			const auto sample_age{std::max<net_clock>(S.now - st.time, 0)};
			const auto input_age{std::max<net_clock>(S.now - st.arrival, 0)};
			interp::set_lag_age(i, std::max(sample_age, input_age));
			if (!st.input.has_flag(input_flag::alive))
			{
				rec.flags = flag_bit(player_record_flag::ghost);
				s.players[i] = rec;
				continue;
			}
			rec.pose = st.input.pose;
			rec.flags = flag_bit(player_record_flag::alive);
			if (st.input.has_flag(input_flag::dying))
				rec.flags |= flag_bit(player_record_flag::dying);
			rec.sample_age = static_cast<uint16_t>(std::min<net_clock>(sample_age, 65535));
			rec.input_age = static_cast<uint8_t>(std::min<net_clock>(input_age / period, 255));
			fill_record_status(rec, obj, st.input.weapon & 0x0f);
		}
		s.players[i] = rec;
	}
#if DXX_BUILD_DESCENT == 2
	for (playernum_t i = 0; i < n; ++i)
	{
		if (vcplayerptr(i)->connected != player_connection_status::playing)
			continue;
		std::optional<guided_record> g;
		if (i == Player_num)
			g = local_guided_record(i);
		else if (auto &st = S.inputs[i]; st.valid && st.input.has_flag(input_flag::alive))
			g = st.input.guided;
		if (g)
			s.guided[s.n_guided++] = *g;
	}
#endif
	for (auto &&[i, np] : enumerate(Netgame.players))
		s.pings[i] = ::dcx::net_v2::quantise_ping(i == Player_num ? 0u : static_cast<unsigned>(std::max<fix>(np.ping, 0)));
}

/* Host: the STATE chunk for peer `p` at this tick. */
void set_state_for_peer(peer &p, const state_bundle &common)
{
	auto s{common};
	const auto slot{peer_slot(p)};
	if (auto &st = S.inputs[slot]; st.valid)
		s.input_ack = st.input.seq;
	/* Section 5.2: the pings once per second. */
	if (S.now >= p.next_pings)
	{
		s.your_flags |= flag_bit(state_flag::has_pings);
		p.next_pings = S.now + F1_0;
	}
	/* Its own guided missile is not sent back to its owner. */
	unsigned kept{0};
	for (unsigned i = 0; i < s.n_guided; ++i)
		if (s.guided[i].pid != slot)
			s.guided[kept++] = s.guided[i];
	s.n_guided = kept;
	std::array<uint8_t, ::dcx::net_v2::NET_V2_MAX_STATE_SIZE> buf;
	const auto n{::dcx::net_v2::write_state(buf, s)};
	p.conn->set_unreliable_state(chunk_type::state, std::span<const uint8_t>(buf).first(n));
}

/* Client: the host's CORRECTION (section 5.6): the local ship snaps to the
 * host's accepted state, physics remainders reset.
 */
void apply_correction(const player_record &rec)
{
	if (!segment_valid(rec.pose.segment) || !local_ship_alive())
		return;
	auto &Objects = LevelUniqueObjectState.Objects;
	quaternionpos qpp{
		.orient = vms_quaternion{rec.pose.orient.w, rec.pose.orient.x, rec.pose.orient.y, rec.pose.orient.z},
		.pos = vms_vector{rec.pose.pos.x, rec.pose.pos.y, rec.pose.pos.z},
		.segment = segnum_t{rec.pose.segment},
		.vel = vms_vector{rec.pose.vel.x, rec.pose.vel.y, rec.pose.vel.z},
		.rotvel = vms_vector{rec.pose.rotvel.x, rec.pose.rotvel.y, rec.pose.rotvel.z},
	};
	extract_quaternionpos(Objects.vmptr, vmsegptr, Objects.vmptridx(get_local_player().objnum), qpp);
	con_puts(CON_VERBOSE, "net: position corrected by the host");
}

/* Client: the header of the newest bundle (latest wins, section 5.4). */
void apply_bundle_header(const state_bundle &s, const host_clock est, const host_clock host_time)
{
	if (s.has_flag(state_flag::has_pings))
		for (auto &&[i, np] : enumerate(Netgame.players))
			np.ping = static_cast<fix>(s.pings[i] * ::dcx::net_v2::NET_V2_PING_STEP_MS);
	/* The level time replaces MULTI_HEARTBEAT: the host's, advanced by the
	 * bundle's age.  Small differences are left alone so that the clock
	 * on the HUD does not twitch.  Like the game loop, which advances it,
	 * and v1, which sent it, only with a time limit.
	 */
	if (Network_status == network_state::playing && Netgame.PlayTimeAllowed.count())
	{
		const auto lt{static_cast<int64_t>(s.level_time) + (est - host_time)};
		if (lt >= 0 && std::abs(lt - static_cast<int64_t>(ThisLevelTime.count())) > F1_0 / 20)
			ThisLevelTime = d_time_fix(static_cast<uint32_t>(lt));
	}
	if (s.has_flag(state_flag::correction))
		if (const auto &own{s.players[Player_num]}; own && !own->is_ghost())
			apply_correction(*own);
}

/* Client: a player's record says it is in the level.  A client learns
 * that a player came back only from the host's records (as v1 did from
 * the position packets).
 */
void notice_player_in_level(const playernum_t pnum)
{
	auto &tplr = *vmplayerptr(pnum);
	if (tplr.connected == player_connection_status::disconnected)
	{
		tplr.connected = player_connection_status::playing;
		if (Newdemo_state == ND_STATE_RECORDING)
			newdemo_record_multi_reconnect(pnum);
		digi_play_sample(sound_effect::SOUND_HUD_MESSAGE, F1_0);
		const auto &&rankstr = GetRankStringWithSpace(Netgame.players[pnum].rank);
		HUD_init_message(HM_MULTI, "%s%s'%s' %s", rankstr.first, rankstr.second, static_cast<const char *>(tplr.callsign), TXT_REJOIN);
		multi_send_score();
	}
	/* A player that was late for the level start (the host started
	 * without waiting) entered as a join in progress.
	 */
	else if (tplr.connected == player_connection_status::waiting && Network_status == network_state::playing && !LevelUniqueObjectState.ControlCenterState.Control_center_destroyed)
		tplr.connected = player_connection_status::playing;
}

/* Client: a STATE chunk from the host. */
void apply_state(peer &p, const std::span<const uint8_t> payload)
{
	if (!state_receiving_allowed())
		return;
	const auto s{::dcx::net_v2::read_state(payload)};
	if (!s)
		return;
	auto &c = *p.conn;
	if (!c.clock_valid())
		return;
	const host_clock est{S.now + c.clock_offset()};
	const host_clock host_time{::dcx::net_interp::unwrap(s->host_time, est)};
	/* Snapshots are ordered by their own times whatever the order of the
	 * bundles (the rings sort them); only the header is latest-wins, by
	 * tick and, for two bundles of one tick, by time.
	 */
	const auto tick_ahead{static_cast<int32_t>(s->tick - S.bundle_tick)};
	const bool newest{!S.have_bundle || tick_ahead > 0 || (tick_ahead == 0 && host_time > S.bundle_time)};
	if (newest)
	{
		S.have_bundle = true;
		S.bundle_tick = s->tick;
		S.bundle_time = host_time;
		apply_bundle_header(*s, est, host_time);
	}
	const auto period{local_tick_period().units()};
	const bool in_level{get_local_player().connected != player_connection_status::disconnected && get_local_player().connected != player_connection_status::waiting};
	for (playernum_t i = 0; i < MAX_PLAYERS; ++i)
	{
		const auto &rec{s->players[i]};
		if (!rec || i == Player_num)
			continue;
		if (newest)
			notice_player_in_level(i);
		if (vcplayerptr(i)->connected != player_connection_status::playing)
			continue;
		if (i >= N_players)
		{
			if (!multi_quit_game && Network_status != network_state::waiting)
				con_printf(CON_VERBOSE, "net: record of P#%u beyond N_players %u ignored", i, N_players);
			continue;
		}
		Netgame.players[i].LastPacketTime = S.now;
		if (!in_level)
			continue;
		if (rec->is_ghost())
		{
			interp::receive_ghost(i);
			if (newest)
				interp::set_lag_age(i, 0);
			continue;
		}
		const host_clock t{host_time - rec->sample_age};
		interp::receive_ship(i, snapshot_of(t, rec->pose, rec->flags & flag_bit(player_record_flag::alive)), S.now);
		if (newest)
			interp::set_lag_age(i, std::max<net_clock>(rec->sample_age, rec->input_age * period));
	}
	if (!in_level)
		return;
	for (unsigned k = 0; k < s->n_guided; ++k)
	{
		const auto &g{s->guided[k]};
		if (g.pid == Player_num || g.pid >= N_players || vcplayerptr(g.pid)->connected != player_connection_status::playing)
			continue;
		/* The missile was sampled with its owner's ship. */
		const auto &owner{s->players[g.pid]};
		const host_clock t{owner && !owner->is_ghost() ? host_time - owner->sample_age : host_time};
		interp::receive_guided(g.pid, g.id, g.gen, snapshot_of(t, g), S.now);
	}
}

/* The v1 MULTI_* records (section 6.10, stage 1 column).  Everything
 * that changes game state travels as the reliable LEGACY_MDATA message;
 * the few latest-wins or cosmetic records go as best-effort events.
 */

[[nodiscard]]
bool legacy_record_is_event(const uint8_t command, const multiplayer_data_priority priority)
{
	/* A message the sender marked important (priority 2) is reliable
	 * whatever its type: the guided missile release and the inventory
	 * sent to a joining player.
	 */
	if (priority == multiplayer_data_priority::_2)
		return false;
	switch (static_cast<multiplayer_command_t>(command))
	{
		case multiplayer_command_t::MULTI_PLAY_SOUND:
		case multiplayer_command_t::MULTI_CREATE_EXPLOSION:
		case multiplayer_command_t::MULTI_ROBOT_POSITION:
		case multiplayer_command_t::MULTI_TYPING_STATE:
		case multiplayer_command_t::MULTI_GMODE_UPDATE:
#if DXX_BUILD_DESCENT == 2
		case multiplayer_command_t::MULTI_GUIDED:
		case multiplayer_command_t::MULTI_DROP_BLOB:
		case multiplayer_command_t::MULTI_SOUND_FUNCTION:
#endif
			return true;
		default:
			return false;
	}
}

/* Send the pending events to every peer in the game as one EVENT_U. */
void flush_events()
{
	if (S.event_buffer.empty())
		return;
	if (multi_i_am_master())
	{
		for (auto &p : S.peers)
			if (peer_receives_events(p))
				p.conn->send_unreliable(chunk_type::event_u, S.event_buffer);
	}
	else if (auto &p{S.peers[0]}; peer_receives_events(p))
		p.conn->send_unreliable(chunk_type::event_u, S.event_buffer);
	S.event_buffer.clear();
}

/* One EVENT_U names one originator (byte 1), so a record from another
 * originator than the pending ones starts a new EVENT_U.  Only the host
 * originates records for more than one player (its own and those of the
 * ships it flies itself); everyone else always passes Player_num.
 */
void queue_event_record(const std::span<const uint8_t> record, const playernum_t originator)
{
	if (!S.event_buffer.empty() && (S.event_buffer[1] != originator || S.event_buffer.size() + record.size() > NET_V2_MAX_EVENT))
		flush_events();
	if (S.event_buffer.empty())
	{
		S.event_buffer.push_back(static_cast<uint8_t>(event_kind::legacy));
		S.event_buffer.push_back(originator);
	}
	S.event_buffer.insert(S.event_buffer.end(), record.begin(), record.end());
}

/* A reliable run of v1 records from `originator`, to everyone in the game
 * but `exclude` (host) or to the host (client).
 */
void send_legacy_reliable(const playernum_t originator, const std::span<const uint8_t> records, const playernum_t exclude = MAX_PLAYERS)
{
	std::vector<uint8_t> msg;
	msg.reserve(1 + records.size());
	msg.push_back(originator);
	msg.insert(msg.end(), records.begin(), records.end());
	broadcast_reliable(session_msg::legacy_mdata, msg, exclude);
}

[[nodiscard]]
bool legacy_processing_allowed()
{
	return Network_status == network_state::playing || Network_status == network_state::endlevel || Network_status == network_state::waiting;
}

/* Best-effort events are applied only in the level.  While `waiting`
 * the client may be applying its level snapshot (see
 * peer_receives_events); events are cosmetic or latest-wins, so dropping
 * the few that arrive before LEVEL_GO loses nothing.
 */
[[nodiscard]]
bool event_processing_allowed()
{
	return Network_status == network_state::playing || Network_status == network_state::endlevel;
}

void receive_legacy_mdata(peer &p, const std::span<const uint8_t> payload)
{
	if (payload.empty() || !peer_sends_game_data(p))
		return;
	const playernum_t originator{payload[0]};
	const auto records{payload.subspan(1)};
	if (originator >= MAX_PLAYERS)
		return;
	if (multi_i_am_master())
	{
		/* A client may only originate its own records. */
		if (originator != peer_slot(p))
			return;
		broadcast_reliable(session_msg::legacy_mdata, payload, originator);
	}
	if (!legacy_processing_allowed())
		return;
	multi_process_bigdata(LevelSharedRobotInfoState, originator, records);
}

/* Stage 3 object messages (net_v2_objects.h): gated like LEGACY_MDATA,
 * never relayed as they are (the host answers or re-announces them
 * itself).
 */
[[nodiscard]]
bool is_object_message(const session_msg type)
{
	switch (type)
	{
		case session_msg::inventory:
		case session_msg::obj_create:
		case session_msg::obj_remove:
		case session_msg::pickup_request:
		case session_msg::pickup_grant:
		case session_msg::pickup_deny:
		case session_msg::drop_request:
		case session_msg::obj_settle:
			return true;
		default:
			return false;
	}
}

void receive_object_message(peer &p, const session_msg type, const std::span<const uint8_t> payload)
{
	if (!peer_sends_game_data(p) || !legacy_processing_allowed())
		return;
	net_objects_receive(peer_slot(p), static_cast<uint8_t>(type), payload);
}

void receive_event(peer &p, const std::span<const uint8_t> payload)
{
	if (payload.size() < 2 || payload[0] != static_cast<uint8_t>(event_kind::legacy) || !peer_sends_game_data(p))
		return;
	const playernum_t originator{payload[1]};
	const auto records{payload.subspan(2)};
	if (originator >= MAX_PLAYERS)
		return;
	if (multi_i_am_master())
	{
		if (originator != peer_slot(p))
			return;
		for (auto &&[i, q] : enumerate(S.peers))
			if (i != originator && peer_receives_events(q))
				q.conn->send_unreliable(chunk_type::event_u, payload);
	}
	if (!event_processing_allowed())
		return;
	multi_process_bigdata(LevelSharedRobotInfoState, originator, records);
}

/* Level end status (section 4.7, stage 1: the v1 endlevel payloads over
 * the reliable channel).
 */

void send_endlevel_status()
{
	auto &LevelUniqueControlCenterState = LevelUniqueObjectState.ControlCenterState;
	auto &Objects = LevelUniqueObjectState.Objects;
	auto &vcobjptr = Objects.vcptr;
	auto &vmobjptr = Objects.vmptr;
	if (multi_i_am_master())
	{
		std::array<uint8_t, LEGACY_ENDLEVEL_HOST_SIZE> buf;
		writer w{buf.data()};
		w.u8(static_cast<uint8_t>(LevelUniqueControlCenterState.Countdown_seconds_left));
		range_for (auto &i, Players)
		{
			w.u8(underlying_value(i.connected));
			auto &player_info = vcobjptr(i.objnum)->ctype.player_info;
			w.u16(player_info.net_kills_total);
			w.u16(player_info.net_killed_total);
		}
		range_for (auto &i, kill_matrix)
			range_for (auto &j, i)
				w.u16(j);
		for (auto &&[i, p] : enumerate(S.peers))
			if (i && p.conn && p.ph == peer::phase::playing && vcplayerptr(static_cast<playernum_t>(i))->connected != player_connection_status::disconnected)
				peer_queue(p, session_msg::legacy_endlevel_host, buf);
	}
	else
	{
		std::array<uint8_t, LEGACY_ENDLEVEL_CLIENT_SIZE> buf;
		writer w{buf.data()};
		w.u8(underlying_value(get_local_player().connected));
		w.u8(static_cast<uint8_t>(LevelUniqueControlCenterState.Countdown_seconds_left));
		auto &player_info = get_local_plrobj().ctype.player_info;
		w.u16(player_info.net_kills_total);
		w.u16(player_info.net_killed_total);
		range_for (auto &i, kill_matrix[Player_num])
			w.u16(i);
		if (auto &p{S.peers[0]}; p.conn && p.ph == peer::phase::playing)
			peer_queue(p, session_msg::legacy_endlevel_client, buf);
	}
}

/* Whether a level end report belongs to the level being played
 * (net_score_carry.h, endlevel_report_applies): the score screen, or the
 * reactor countdown (also started by a kill goal or the time limit).
 */
[[nodiscard]]
bool level_is_ending()
{
	return Network_status == network_state::endlevel || (Network_status == network_state::playing && LevelUniqueObjectState.ControlCenterState.Control_center_destroyed);
}

void receive_endlevel_client(const playernum_t pnum, const std::span<const uint8_t> data)
{
	auto &LevelUniqueControlCenterState = LevelUniqueObjectState.ControlCenterState;
	if (data.size() != LEGACY_ENDLEVEL_CLIENT_SIZE)
		return;
	const auto use{::dcx::net_v2::endlevel_report_applies(true, level_is_ending())};
	if (!use.status)
		return;
	reader r{data};
	const player_connection_status connected{r.u8()};
	if (connected == player_connection_status::disconnected)
	{
		S.left_reason = kick_player_reason::quit;
		multi_disconnect_player(pnum);
	}
	vmplayerptr(pnum)->connected = connected;
	const uint8_t countdown{r.u8()};
	if (Network_status != network_state::playing && vcplayerptr(pnum)->connected == player_connection_status::playing && countdown < LevelUniqueControlCenterState.Countdown_seconds_left)
		LevelUniqueControlCenterState.Countdown_seconds_left = countdown;
	/* The client's view of its kills, deaths and kill matrix row: the
	 * host's own counts are the game's (endlevel_report_applies).
	 */
	static_assert(!::dcx::net_v2::endlevel_report_applies(true, true).scores);
	r.take(2 + 2 + (MAX_PLAYERS * 2));
	if (vcplayerptr(pnum)->connected != player_connection_status::disconnected)
		Netgame.players[pnum].LastPacketTime = timer_query();
}

void receive_endlevel_host(const std::span<const uint8_t> data)
{
	auto &LevelUniqueControlCenterState = LevelUniqueObjectState.ControlCenterState;
	auto &Objects = LevelUniqueObjectState.Objects;
	auto &vmobjptr = Objects.vmptr;
	if (data.size() != LEGACY_ENDLEVEL_HOST_SIZE)
		return;
	const auto use{::dcx::net_v2::endlevel_report_applies(false, level_is_ending())};
	if (!use.status)
		return;
	reader r{data};
	const uint8_t countdown{r.u8()};
	if (Network_status != network_state::playing && countdown < LevelUniqueControlCenterState.Countdown_seconds_left)
		LevelUniqueControlCenterState.Countdown_seconds_left = countdown;
	/* Playing the countdown: the host's is the game's (net_countdown.h);
	 * it runs while the host plays the level itself (slot 0, the first
	 * entry of the list that follows).
	 */
	else if (Network_status == network_state::playing && LevelUniqueControlCenterState.Control_center_destroyed && (get_local_player().connected == player_connection_status::playing || get_local_player().connected == player_connection_status::died_in_mine))
	{
		const bool host_live{player_connection_status{data[1]} == player_connection_status::playing};
		if (const auto t{::dcx::net_v2::countdown_correction(LevelUniqueControlCenterState.Countdown_timer, countdown, host_live)})
		{
			con_printf(CON_NORMAL, "reactor: countdown T-%d s set to the host's T-%u s", LevelUniqueControlCenterState.Countdown_seconds_left, static_cast<unsigned>(countdown));
			LevelUniqueControlCenterState.Countdown_timer = *t;
			LevelUniqueControlCenterState.Countdown_seconds_left = ::dcx::net_v2::countdown_seconds_of_timer(*t);
			movement_record_level_event(::dcx::movrec::level_event_kind::countdown_set, Player_num, LevelUniqueControlCenterState.Countdown_seconds_left, 0);
		}
	}
	for (playernum_t i = 0; i < MAX_PLAYERS; i++)
	{
		if (i == Player_num)
		{
			r.take(5);
			continue;
		}
		const player_connection_status connected{r.u8()};
		if (connected == player_connection_status::disconnected)
			multi_disconnect_player(i);
		auto &player_info = vmobjptr(vcplayerptr(i)->objnum)->ctype.player_info;
		vmplayerptr(i)->connected = connected;
		const int16_t kills{static_cast<int16_t>(r.u16())};
		const int16_t killed{static_cast<int16_t>(r.u16())};
		if (use.scores)
		{
			player_info.net_kills_total = kills;
			player_info.net_killed_total = killed;
		}
		if (vcplayerptr(i)->connected != player_connection_status::disconnected)
			Netgame.players[i].LastPacketTime = timer_query();
	}
	for (playernum_t i = 0; i < MAX_PLAYERS; i++)
		for (playernum_t j = 0; j < MAX_PLAYERS; j++)
		{
			const auto v{r.u16()};
			if (use.scores && i != Player_num)
				kill_matrix[i][j] = v;
		}
}

/* Level snapshot for a join in progress (section 4.4; stage 1 carries the
 * v1 object entries: object number, owner, remote object number and the
 * object_rw).
 */

[[nodiscard]]
bool snapshot_includes(const object_base &objp)
{
	/* A powerup picked up in this frame is gone (its id is unbound);
	 * sending it would leave a copy on the joiner that nothing removes.
	 * Only powerups: a ship hit lethally in this frame also carries
	 * OF_SHOULD_BE_DEAD until the death sequence starts next frame, and
	 * the joiner must still receive every player object.
	 */
	if (objp.type == object_type::OBJ_POWERUP && (objp.flags & OF_SHOULD_BE_DEAD))
		return false;
	if (objp.type == object_type::OBJ_POWERUP || objp.type == object_type::OBJ_PLAYER || objp.type == object_type::OBJ_CNTRLCEN || objp.type == object_type::OBJ_GHOST || objp.type == object_type::OBJ_ROBOT || objp.type == object_type::OBJ_HOSTAGE)
		return true;
#if DXX_BUILD_DESCENT == 2
	if (objp.type == object_type::OBJ_WEAPON && get_weapon_id(objp) == weapon_id_type::PMINE_ID)
		return true;
#endif
	return false;
}

/* Serialise the level into the peer's backlog: LEVEL_START (in progress),
 * SNAPSHOT_BEGIN, the objects owned by nobody or by the joining player,
 * then the other players' objects (the v1 two passes), SNAPSHOT_GAME,
 * SNAPSHOT_END.  Everything the game sends afterwards is ordered behind
 * it by the reliable stream.
 */
void queue_snapshot(peer &p)
{
	auto &Objects = LevelUniqueObjectState.Objects;
	auto &vmobjptr = Objects.vmptr;
	const auto slot{peer_slot(p)};
	net_udp_update_netgame();
	fill_netgame_scores();
	net_objects_host_join(slot);
	net_combat_host_join(slot);
	if (p.is_new)
	{
		/* A new player enters the game (new_player) only with its
		 * CLIENT_READY, after this snapshot.  Describe the game it enters:
		 * its slot counted in the player count (otherwise its N_players
		 * leaves itself out, so that it drops every PLAYER_KILLED about
		 * its own deaths and misses itself in the kill list) and its
		 * scores as new_player will reset them, not those of a player who
		 * held the slot before.
		 */
		Netgame.numplayers = static_cast<uint8_t>(::dcx::net_v2::player_count_including(Netgame.numplayers, slot));
		Netgame.kills[slot] = {};
		for (auto &row : Netgame.kills)
			row[slot] = 0;
		Netgame.killed[slot] = 0;
		Netgame.player_kills[slot] = 0;
		Netgame.player_score[slot] = 0;
	}
	Netgame.level_time = get_local_player().time_level;
	Netgame.monitor_vector = create_monitor_vector();
	send_game_settings(p);
	send_player_list(p);
	{
		std::array<uint8_t, LEVEL_START_SIZE> buf;
		writer w{buf.data()};
		w.u32(static_cast<uint32_t>(Current_level_num));
		w.u32(0);	/* tick: stage 2 */
		w.u32(+(Game_mode & GM_MULTI_COOP) ? 0 : Netgame.ShufflePowerupSeed);
		for (auto &i : Netgame.locations)
			w.u8(static_cast<uint8_t>(i));
		w.u32(static_cast<uint32_t>(Netgame.level_time));
		w.u32(static_cast<uint32_t>(Netgame.control_invul_time));
		w.u8(1);	/* start_flags: level in progress */
		peer_queue(p, session_msg::level_start, buf);
	}
	uint32_t crc{0};
	unsigned object_count{0};
	unsigned parts{0};
	std::vector<uint8_t> msg;
	msg.reserve(3 + SNAPSHOT_OBJECTS_PER_MESSAGE * SNAPSHOT_OBJECT_ENTRY_SIZE);
	const auto count_objects{[&](const bool static_pass) {
		unsigned n{0};
		for (objnum_t i = 0; i <= Highest_object_index; ++i)
		{
			const auto &objp = *vmobjptr(i);
			if (!snapshot_includes(objp))
				continue;
			const bool owned_by_nobody_or_joiner{object_owner[i] == -1 || object_owner[i] == slot};
			if (owned_by_nobody_or_joiner == static_pass)
				++n;
		}
		return n;
	}};
	object_count = count_objects(true) + count_objects(false);
	{
		std::array<uint8_t, SNAPSHOT_BEGIN_SIZE> buf;
		writer w{buf.data()};
		w.u16(static_cast<uint16_t>(object_count));
		/* Parts: the object messages (3 per message, two passes) and
		 * SNAPSHOT_GAME.
		 */
		const unsigned static_count{count_objects(true)};
		const unsigned dynamic_count{object_count - static_count};
		parts = (static_count + SNAPSHOT_OBJECTS_PER_MESSAGE - 1) / SNAPSHOT_OBJECTS_PER_MESSAGE + (dynamic_count + SNAPSHOT_OBJECTS_PER_MESSAGE - 1) / SNAPSHOT_OBJECTS_PER_MESSAGE + 1;
		w.u16(static_cast<uint16_t>(parts));
		crc = ::dcx::net_v2::crc32_update(crc, buf);
		peer_queue(p, session_msg::snapshot_begin, buf);
	}
	uint16_t part{0};
	const auto flush_msg{[&] {
		if (msg.size() <= 3)
			return;
		PUT_INTEL_SHORT(&msg[0], part);
		++part;
		crc = ::dcx::net_v2::crc32_update(crc, msg);
		peer_queue(p, session_msg::snapshot_objects, msg);
		msg.clear();
	}};
	for (const bool static_pass : {true, false})
	{
		for (objnum_t i = 0; i <= Highest_object_index; ++i)
		{
			const auto &&objp = vmobjptr(i);
			if (!snapshot_includes(*objp))
				continue;
			const bool owned_by_nobody_or_joiner{object_owner[i] == -1 || object_owner[i] == slot};
			if (owned_by_nobody_or_joiner != static_pass)
				continue;
			if (msg.empty())
				msg.assign(3, 0);
			const auto &&[owner, remote_objnum] = objnum_local_to_remote(i);
			std::array<uint8_t, SNAPSHOT_OBJECT_ENTRY_SIZE> entry;
			writer w{entry.data()};
			w.u32(i);
			w.u8(static_cast<uint8_t>(owner));
			w.u32(remote_objnum);
			w.u16(net_objects_netid_of(Objects.vcptridx(i)));
			multi_object_to_object_rw(objp, reinterpret_cast<object_rw *>(&entry[w.pos]));
			msg.insert(msg.end(), entry.begin(), entry.end());
			if (++msg[2] == SNAPSHOT_OBJECTS_PER_MESSAGE)
				flush_msg();
		}
		flush_msg();
	}
	{
		std::array<uint8_t, SNAPSHOT_GAME_SIZE> buf;
		const auto b{build_snapshot_game(buf, part, true)};
		++part;
		crc = ::dcx::net_v2::crc32_update(crc, b);
		peer_queue(p, session_msg::snapshot_game, b);
	}
	{
		std::array<uint8_t, SNAPSHOT_END_SIZE> buf;
		writer w{buf.data()};
		w.u16(static_cast<uint16_t>(object_count));
		w.u16(part);
		w.u32(crc);
		peer_queue(p, session_msg::snapshot_end, buf);
	}
	assert(part == parts);
	con_printf(CON_VERBOSE, "net: snapshot of %u objects in %u parts queued for P#%u", object_count, part, slot);
	p.ph = peer::phase::syncing;
	p.phase_since = S.now;
}

/* Client: abort a join whose snapshot cannot be applied. */
void snapshot_failed()
{
	nm_messagebox_str(menu_title{nullptr}, nm_messagebox_tie(TXT_OK), menu_subtitle{TXT_NET_SYNC_FAILED});
	if (auto &p{S.peers[0]}; p.conn)
	{
		const uint8_t reason{underlying_value(kick_player_reason::snapshot_failed)};
		peer_queue(p, session_msg::leave, std::span<const uint8_t>(&reason, 1));
		p.ph = peer::phase::closing;
		p.close_at = S.now + ::dcx::net_v2::NET_V2_CLOSE_LINGER;
	}
	Network_status = network_state::menu;
}

void apply_snapshot_begin(const std::span<const uint8_t> payload)
{
	if (payload.size() != SNAPSHOT_BEGIN_SIZE)
		return;
	/* The counts come again in SNAPSHOT_END, which is checked. */
	S.snapshot_crc = ::dcx::net_v2::crc32_update(0, payload);
	// Clear object array
	init_objects();
	net_objects_snapshot_begin();
	net_combat_level_start();
	Network_rejoined = 1;
	S.snapshot_mode_static = true;
	S.snapshot_objects = 0;
	S.snapshot_parts = 0;
}

void apply_snapshot_objects(const std::span<const uint8_t> payload)
{
	auto &Objects = LevelUniqueObjectState.Objects;
	auto &Robot_info = LevelSharedRobotInfoState.Robot_info;
	auto &vmobjptridx = Objects.vmptridx;
	if (payload.size() < 3 || (payload.size() - 3) % SNAPSHOT_OBJECT_ENTRY_SIZE || payload[2] != (payload.size() - 3) / SNAPSHOT_OBJECT_ENTRY_SIZE)
	{
		snapshot_failed();
		return;
	}
	S.snapshot_crc = ::dcx::net_v2::crc32_update(S.snapshot_crc, payload);
	++S.snapshot_parts;
	const auto my_pnum{Player_num};
	for (std::size_t pos = 3; pos < payload.size(); pos += SNAPSHOT_OBJECT_ENTRY_SIZE)
	{
		reader r{payload.subspan(pos, SNAPSHOT_OBJECT_ENTRY_SIZE)};
		const unsigned uobjnum{r.u32()};
		objnum_t objnum = uobjnum;
		const int8_t obj_owner{static_cast<int8_t>(r.u8())};
		const int remote_objnum{r.i32()};
		const uint16_t netid{r.u16()};
		++S.snapshot_objects;
		if (obj_owner == my_pnum || obj_owner == -1)
		{
			if (!S.snapshot_mode_static)
				con_printf(CON_URGENT, "net: static object %hu after the dynamic ones in the snapshot", static_cast<uint16_t>(objnum));
			objnum = remote_objnum;
		}
		else
		{
			if (S.snapshot_mode_static)
			{
				special_reset_objects(LevelUniqueObjectState, Robot_info);
				S.snapshot_mode_static = false;
			}
			objnum = obj_allocate(LevelUniqueObjectState);
		}
		if (objnum == object_none || objnum >= MAX_OBJECTS)
		{
			snapshot_failed();
			return;
		}
		auto obj = vmobjptridx(objnum);
		if (obj->type != object_type::OBJ_NONE)
		{
			obj_unlink(Objects.vmptr, Segments.vmptr, obj);
			Assert(obj->segnum == segment_none);
		}
		multi_object_rw_to_object(reinterpret_cast<const object_rw *>(&payload[pos + SNAPSHOT_OBJECT_ENTRY_OBJECT_OFFSET]), obj);
		const auto segnum = obj->segnum;
		if (segnum != segment_none)
			obj_link_unchecked(Objects.vmptr, obj, Segments.vmptridx(segnum));
		if (obj_owner == my_pnum)
			map_objnum_local_to_local(objnum);
		else if (obj_owner != -1)
			map_objnum_local_to_remote(objnum, remote_objnum, obj_owner);
		else
			object_owner[objnum] = -1;
		net_objects_snapshot_bind(obj, netid);
	}
}

void apply_snapshot_end(const std::span<const uint8_t> payload)
{
	auto &Objects = LevelUniqueObjectState.Objects;
	auto &Robot_info = LevelSharedRobotInfoState.Robot_info;
	auto &vcobjptr = Objects.vcptr;
	if (payload.size() != SNAPSHOT_END_SIZE)
	{
		snapshot_failed();
		return;
	}
	reader r{payload};
	const unsigned object_count{r.u16()};
	const unsigned parts{r.u16()};
	const uint32_t crc{r.u32()};
	if (S.snapshot_mode_static)
	{
		special_reset_objects(LevelUniqueObjectState, Robot_info);
		S.snapshot_mode_static = false;
	}
	if (object_count != S.snapshot_objects || parts != S.snapshot_parts || crc != S.snapshot_crc)
	{
		con_printf(CON_URGENT, "net: snapshot mismatch: %u/%u objects, %u/%u parts, crc %08x/%08x", S.snapshot_objects, object_count, S.snapshot_parts, parts, S.snapshot_crc, crc);
		snapshot_failed();
		return;
	}
	unsigned nplayers{0};
	for (auto &obj : vcobjptr)
		if (obj.type == object_type::OBJ_PLAYER || obj.type == object_type::OBJ_GHOST)
			nplayers++;
	if (nplayers < Netgame.max_numplayers)
	{
		con_printf(CON_URGENT, "net: snapshot has %u player objects, expected %u", nplayers, Netgame.max_numplayers);
		snapshot_failed();
		return;
	}
	if (auto &p{S.peers[0]}; p.conn)
		peer_queue(p, session_msg::client_ready, {});
}

/* The v1 net_udp_read_sync_packet after its parsing: apply the level start
 * data in Netgame.
 */
void apply_level_go_internal()
{
	auto &Objects = LevelUniqueObjectState.Objects;
	auto &vmobjptr = Objects.vmptr;
	auto &vmobjptridx = Objects.vmptridx;

	/* The game includes this client's own slot, whatever the count says. */
	N_players = ::dcx::net_v2::player_count_including(Netgame.numplayers, Player_num);
	GameUniqueState.Difficulty_level = Netgame.difficulty;

	for (unsigned i = 0; i < N_players; ++i)
	{
		auto &plr = *vmplayerptr(i);
		plr.callsign = Netgame.players[i].callsign;
		plr.connected = Netgame.players[i].connected;
		auto &objp = *vmobjptr(plr.objnum);
		auto &player_info = objp.ctype.player_info;
		const int16_t kills{static_cast<int16_t>(Netgame.player_kills[i])};
		const int16_t killed{static_cast<int16_t>(Netgame.killed[i])};
		/* The host's level start copies what it read from the ships, so
		 * a change there is a bug; a client adopts the host's counts.
		 * Logged to pin down a count that changes at a level start.
		 */
		if (player_info.net_kills_total != kills || player_info.net_killed_total != killed)
			con_printf(multi_i_am_master() ? CON_URGENT : CON_VERBOSE, "net: level start: P#%u kills %i -> %i, deaths %i -> %i", i, player_info.net_kills_total, kills, player_info.net_killed_total, killed);
		player_info.net_kills_total = kills;
		player_info.net_killed_total = killed;
		if ((Network_rejoined) || (i != Player_num))
			player_info.mission.score = Netgame.player_score[i];
	}
	kill_matrix = Netgame.kills;

#if DXX_BUILD_DESCENT == 1
	{
		auto &player_info = get_local_plrobj().ctype.player_info;
		PlayerCfg.NetlifeKills -= player_info.net_kills_total;
		PlayerCfg.NetlifeKilled -= player_info.net_killed_total;
	}
#endif

	auto &plr = get_local_player();
	if (Network_rejoined)
	{
		process_monitor_vector(Netgame.monitor_vector);
		plr.time_level = Netgame.level_time;
	}

	team_kills = Netgame.team_kills;
	plr.connected = player_connection_status::playing;
	Netgame.players[Player_num].connected = player_connection_status::playing;
	Netgame.players[Player_num].rank=GetMyNetRanking();

	if (!Network_rejoined)
	{
		for (unsigned i = 0; i < NumNetPlayerPositions; ++i)
		{
			const auto &&o = vmobjptridx(vcplayerptr(i)->objnum);
			const auto location{Netgame.locations[i]};
			if (location >= NumNetPlayerPositions)
				continue;
			const auto &p = Player_init[location];
			o->pos = p.pos;
			o->orient = p.orient;
			obj_relink(vmobjptr, vmsegptr, o, vmsegptridx(p.segnum));
		}
	}

	get_local_plrobj().type = object_type::OBJ_PLAYER;

	/* The INPUTs and snapshots of the previous level describe other
	 * places.
	 */
	S.inputs = {};
	net_interp_reset();

	S.sync_wait.end();
	Network_status = network_state::playing;
	multi_sort_kill_list();
}

/* The "extras" a player joining a level in progress gets after the
 * snapshot: the parts of the level state that live in v1 MULTI_* records
 * (unchanged from v1 net_udp_send_extras, one step per 1/50 s).
 */

#if DXX_BUILD_DESCENT == 1
void send_door_updates()
{
	auto &Walls = LevelUniqueWallSubsystemState.Walls;
	auto &vcwallptridx = Walls.vcptridx;
	// Send door status when new player joins
	range_for (const auto &&p, vcwallptridx)
	{
		auto &w = *p;
		if ((w.type == WALL_DOOR && (w.state == wall_state::opening || w.state == wall_state::waiting)) || (w.type == WALL_BLASTABLE && +(w.flags & wall_flag::blasted)))
			multi_send_door_open(w.segnum, w.sidenum, {});
		else if (w.type == WALL_BLASTABLE && w.hps != WALL_HPS)
			multi_send_hostage_door_status(p);
	}
}
#elif DXX_BUILD_DESCENT == 2
void send_door_updates(const playernum_t pnum)
{
	// Send door status when new player joins
	auto &Walls = LevelUniqueWallSubsystemState.Walls;
	auto &vcwallptridx = Walls.vcptridx;
	range_for (const auto &&p, vcwallptridx)
	{
		auto &w = *p;
		if ((w.type == WALL_DOOR && (w.state == wall_state::opening || w.state == wall_state::waiting || w.state == wall_state::open)) || (w.type == WALL_BLASTABLE && +(w.flags & wall_flag::blasted)))
			multi_send_door_open_specific(pnum,w.segnum, w.sidenum,w.flags);
		else if (w.type == WALL_BLASTABLE && w.hps != WALL_HPS)
			multi_send_hostage_door_status(p);
		else
			multi_send_wall_status_specific(pnum,p,w.type,w.flags,w.state);
	}
}

void send_smash_lights(const playernum_t pnum)
{
	// send the lights that have been blown out
	range_for (const auto &&segp, vmsegptridx)
	{
		unique_segment &useg = segp;
		if (const auto light_subtracted = useg.light_subtracted; light_subtracted != sidemask_t{})
			multi_send_light_specific(pnum, segp, light_subtracted);
	}
}

void send_fly_thru_triggers(const playernum_t pnum)
{
	// send the fly thru triggers that have been disabled
	auto &Triggers = LevelUniqueWallSubsystemState.Triggers;
	auto &vctrgptridx = Triggers.vcptridx;
	range_for (const auto &&t, vctrgptridx)
	{
		if (+(t->flags & trigger_behavior_flags::disabled))
			multi_send_trigger_specific(pnum, t);
	}
}

#endif

void begin_extras(playernum_t pnum);
void start_next_extras();

void send_extras()
{
	if (S.last_extras + EXTRAS_INTERVAL > S.now)
		return;
	S.last_extras = S.now;

	Assert (Player_joining_extras>-1);

#if DXX_BUILD_DESCENT == 1
	if (Network_sending_extras==3 && (Netgame.PlayTimeAllowed.count() || Netgame.KillGoal))
#elif DXX_BUILD_DESCENT == 2
	if (Network_sending_extras==9)
		send_fly_thru_triggers(Player_joining_extras);
	if (Network_sending_extras==8)
		send_door_updates(Player_joining_extras);
	if (Network_sending_extras==7)
		multi_send_markers();
	if (Network_sending_extras==6 && +(Game_mode & GM_MULTI_ROBOTS))
		multi_send_stolen_items();
	if (Network_sending_extras==5 && (Netgame.PlayTimeAllowed.count() || Netgame.KillGoal))
#endif
		multi_send_kill_goal_counts();
#if DXX_BUILD_DESCENT == 2
	if (Network_sending_extras==4)
		send_smash_lights(Player_joining_extras);
#endif
	if (Network_sending_extras==2)
		/* Every player's INVENTORY (stage 3; v1 sent the flags and the
		 * host's inventory here).
		 */
		net_objects_send_all_inventories();
	if (Network_sending_extras==1 && +(Game_mode & GM_BOUNTY))
		multi_send_bounty();

	Network_sending_extras--;
	if (!Network_sending_extras)
	{
		Player_joining_extras=-1;
		start_next_extras();
	}
}

/* The next player whose extras waited, if still in the game. */
void start_next_extras()
{
	while (!S.extras_queue.empty())
	{
		const auto next{S.extras_queue.front()};
		S.extras_queue.pop_front();
		if (S.peers[next].ph == peer::phase::playing)
		{
			begin_extras(next);
			break;
		}
	}
}

void begin_extras(const playernum_t pnum)
{
#if DXX_BUILD_DESCENT == 1
	send_door_updates();
	Network_sending_extras=3; // start to send extras
#elif DXX_BUILD_DESCENT == 2
	Network_sending_extras=9; // start to send extras
#endif
	Player_joining_extras = pnum;
	S.last_extras = 0;
}

/* The extras run for one player at a time: a second player that became
 * ready meanwhile waits for the first run to finish, so that neither run
 * is cut short (Player_joining_extras names the recipient of the
 * player-specific records).
 */
void start_extras(const playernum_t pnum)
{
	if (Network_sending_extras)
	{
		if (Player_joining_extras != pnum && std::ranges::find(S.extras_queue, pnum) == S.extras_queue.end())
			S.extras_queue.push_back(pnum);
		return;
	}
	begin_extras(pnum);
}

/* Host: the extras belong to the level; a level end discards them. */
void cancel_extras()
{
	Network_sending_extras = 0;
	Player_joining_extras = -1;
	S.extras_queue.clear();
}

/* Host: the player in `pnum` is gone (or its slot is taken by a new
 * connection).  Its extras run ends and its queued run is forgotten: the
 * rest of a run for the previous connection would otherwise go to a new
 * player in the slot before its snapshot (useless, the snapshot replaces
 * the level state), and that player's own run would never start, since
 * start_extras takes a run already under way for the slot as its own.
 */
void drop_extras_for(const playernum_t pnum)
{
	std::erase(S.extras_queue, pnum);
	if (Network_sending_extras && Player_joining_extras == static_cast<int>(pnum))
	{
		Network_sending_extras = 0;
		Player_joining_extras = -1;
		start_next_extras();
	}
}

/* Players joining and leaving */

/* A new player entered the game (v1 net_udp_new_player: on the host when
 * the joiner is ready, on a client on PLAYER_JOINED).
 */
void new_player(const playernum_t pnum, const callsign_t &callsign, const netplayer_info::player_rank rank)
{
	auto &Objects = LevelUniqueObjectState.Objects;
	auto &vmobjptr = Objects.vmptr;

	Assert(pnum < Netgame.max_numplayers);

	if (Newdemo_state == ND_STATE_RECORDING)
		newdemo_record_multi_connect(pnum, pnum == N_players, callsign);

	auto &plr = *vmplayerptr(pnum);
	plr.callsign = callsign;
	Netgame.players[pnum].callsign = callsign;
	Netgame.players[pnum].rank = rank;

	plr.connected = player_connection_status::playing;
	/* The slot's kills, and the kills of it: both were those of the
	 * player who held the slot before (a bot the host removed, a bot a
	 * human replaced).  On the host and the clients alike; the joiner
	 * itself gets the same from its snapshot (queue_snapshot).
	 */
	kill_matrix[pnum] = {};
	for (auto &row : kill_matrix)
		row[pnum] = 0;
	auto &objp = *vmobjptr(plr.objnum);
	auto &player_info = objp.ctype.player_info;
	player_info.net_killed_total = 0;
	player_info.net_kills_total = 0;
	player_info.mission.score = 0;
	player_info.powerup_flags = {};
	player_info.KillGoalCount = 0;

	/* The host serialises joins, so a new player normally takes slot
	 * N_players; a slot further up (a lobby hole above a departed player)
	 * must still be counted.
	 */
	if (pnum >= N_players)
	{
		N_players = pnum + 1;
		Netgame.numplayers = N_players;
	}

	digi_play_sample(sound_effect::SOUND_HUD_MESSAGE, F1_0);

	const auto &&rankstr = GetRankStringWithSpace(rank);
	HUD_init_message(HM_MULTI, "%s%s'%s' %s", rankstr.first, rankstr.second, callsign.operator const char *(), TXT_JOINING);

	multi_make_ghost_player(pnum);

	multi_send_score();
#if DXX_BUILD_DESCENT == 2
	multi_sort_kill_list();
#endif
}

/* Host: put a peer into the closing phase with a KICK; the connection
 * lingers so that the message can be retransmitted.
 */
void kick_peer(peer &p, const kick_player_reason why)
{
	if (!p.conn)
		return;
	con_printf(CON_NORMAL, "net: KICK to P#%u: %s (%s)", peer_slot(p), kick_reason_name(why), network_state_name(Network_status));
	const uint8_t reason{underlying_value(why)};
	peer_queue(p, session_msg::kick, std::span<const uint8_t>(&reason, 1));
	p.ph = peer::phase::closing;
	p.close_at = S.now + ::dcx::net_v2::NET_V2_CLOSE_LINGER;
}

/* Host, while forming the game: a lobby slot is given up (LEAVE or kick).
 * Slots are not renumbered, since every peer's player id is fixed by its
 * JOIN_ACCEPT; a hole is a disconnected slot, and trailing holes are
 * trimmed.
 */
void vacate_slot(const playernum_t slot)
{
	Netgame.players[slot].callsign = {};
	Netgame.players[slot].rank = netplayer_info::player_rank::None;
	Netgame.players[slot].connected = player_connection_status::disconnected;
	Netgame.players[slot].protocol.udp.addr = {};
	vmplayerptr(slot)->connected = player_connection_status::disconnected;
	vmplayerptr(slot)->callsign = {};
	S.awaits_entry[slot] = false;
	while (N_players > 1 && vcplayerptr(N_players - 1)->connected == player_connection_status::disconnected && !Netgame.players[N_players - 1].callsign[0u] && S.peers[N_players - 1].ph == peer::phase::none)
		--N_players;
	Netgame.numplayers = N_players;
}

/* Host: a peer's connection ended (transport close, LEAVE, kick linger
 * over).  Tell the game and the other players.
 */
void host_peer_gone(peer &p, const kick_player_reason why)
{
	const auto slot{peer_slot(p)};
	const bool was_closing{p.ph == peer::phase::closing};
	drop_peer(p);
	drop_extras_for(slot);
	if (Network_status == network_state::starting)
	{
		if (vcplayerptr(slot)->connected != player_connection_status::disconnected)
		{
			S.left_reason = why;
			multi_disconnect_player(slot);
		}
		vacate_slot(slot);
		host_send_netgame_update();
		return;
	}
	if (vcplayerptr(slot)->connected != player_connection_status::disconnected)
	{
		S.left_reason = why;
		multi_disconnect_player(slot);
	}
	else if (!was_closing)
	{
		/* The player was already disconnected in the game; tell the others
		 * in case they did not know.
		 */
		const std::array<uint8_t, 2> buf{{static_cast<uint8_t>(slot), underlying_value(why)}};
		broadcast_reliable(session_msg::player_left, buf);
	}
	Netgame.players[slot].LastPacketTime = S.now;
}

void deny_join(const _sockaddr &to, const uint32_t nonce, const kick_player_reason why, const uint32_t session_id = S.session_id)
{
	const ::dcx::net_v2::join_deny d{
		.client_nonce = nonce,
		.reason = underlying_value(why),
		.version = Program_version,
	};
	std::array<uint8_t, ::dcx::net_v2::NET_V2_JOIN_DENY_SIZE> buf;
	d.write(buf.data());
	send_unconnected(to, session_id, 0, 0, session_msg::join_deny, buf);
	_sockaddr::presentation_buffer dbuf;
	con_printf(CON_NORMAL, "net: join refused for %s:%hu: %s", dxx_ntop(to, dbuf), dxx_sockaddr_port(to), kick_reason_name(why));
}

/* Host: create the connection to a newly accepted player in `slot` and
 * send JOIN_ACCEPT.
 */
void accept_peer(const playernum_t slot, const ::dcx::net_v2::join_request &req, const callsign_t &callsign, const netplayer_info::player_rank rank, const _sockaddr &from, const peer::phase ph, const bool is_new)
{
	/* A human takes the slot: whatever bot was there is gone. */
	bot_slot_released(slot);
	set_player_is_bot(slot, false);
	auto &p = S.peers[slot];
	drop_peer(p);
	drop_extras_for(slot);
	p.ph = ph;
	p.phase_since = S.now;
	p.token = random_nonzero_u32();
	p.nonce = req.client_nonce;
	p.addr = from;
	p.is_new = is_new;
	p.conn.emplace(make_connection_config(p.token, 0, slot), S.now);
	Netgame.players[slot].callsign = callsign;
	Netgame.players[slot].rank = rank;
	Netgame.players[slot].protocol.udp.addr = from;
	Netgame.players[slot].LastPacketTime = S.now;
	const ::dcx::net_v2::join_accept acc{
		.client_nonce = req.client_nonce,
		.player_id = static_cast<uint8_t>(slot),
		.tick_rate = Netgame.TickRate,
		.host_time = ::dcx::net_v2::to_net_time(S.now),
		.tick = 0,
		.client_time = req.client_time,
		.session_id = S.session_id,
	};
	acc.write(p.accept_payload.data());
	send_unconnected(from, S.session_id, p.token, 0, session_msg::join_accept, p.accept_payload);
	/* Everyone's ship, first thing on the new connection; the mission's
	 * description, for a client that lacks it.
	 */
	net_ships_client_joined(slot);
	net_mission_client_joined(slot);
	{
		_sockaddr::presentation_buffer dbuf;
		con_printf(CON_NORMAL, "net: accepted '%s' from %s:%hu as P#%u", callsign.operator const char *(), dxx_ntop(from, dbuf), dxx_sockaddr_port(from), slot);
	}
}

[[nodiscard]]
per_player_array<slot_view> build_slot_views(const callsign_t &callsign, const _sockaddr &from)
{
	per_player_array<slot_view> views{};
	for (auto &&[i, v] : enumerate(views))
	{
		const auto &np = Netgame.players[i];
		const auto &p = S.peers[i];
		const bool has_player{i < N_players && np.callsign[0u]};
		v.occupied = has_player || p.ph != peer::phase::none;
		v.connected = (has_player && vcplayerptr(static_cast<playernum_t>(i))->connected != player_connection_status::disconnected) || p.ph != peer::phase::none;
		v.callsign_matches = has_player && !d_stricmp(np.callsign, callsign);
		v.address_matches = np.protocol.udp.addr == from;
		v.last_packet_time = np.LastPacketTime;
		/* Bots section 2.3: a bot's slot (playing, or left) is never
		 * rejoined by callsign; a playing one may be replaced.
		 */
		const auto pn{static_cast<playernum_t>(i)};
		v.bot = has_player && player_is_bot(pn);
		if (v.bot)
		{
			v.connected = bot_is_local(pn);
			v.bot_order = bot_added_order(pn);
		}
	}
	/* The host itself is always occupied and connected. */
	views[0].occupied = true;
	views[0].connected = true;
	views[0].callsign_matches = !d_stricmp(get_local_player().callsign, callsign);
	return views;
}

/* Host, game in progress: admit a player (v1 net_udp_welcome_player with
 * the admission table of section 4.2).
 */
void welcome_player(const ::dcx::net_v2::join_request &req, const callsign_t &callsign, const netplayer_info::player_rank rank, const _sockaddr &from)
{
	auto &LevelUniqueControlCenterState = LevelUniqueObjectState.ControlCenterState;
	// Add a player to a game already in progress
	WaitForRefuseAnswer=0;

	// Don't accept new players if we're ending this level.  Its safe to
	// ignore since they'll request again later
	if (Network_status == network_state::endlevel || LevelUniqueControlCenterState.Control_center_destroyed)
	{
		deny_join(from, req.client_nonce, kick_player_reason::endlevel);
		return;
	}

	/* Not req.current_level: the client asks before it loads the level
	 * (it loads the host's level after JOIN_ACCEPT), so its number is
	 * whatever it played last - 1 after a restart.  Denying on it refused
	 * every restarted client on any level but the first; the level is
	 * checked where it means something, in LEVEL_READY.
	 */
	/* The same client restarted during its own join: drop the stale
	 * attempt, so that it neither holds a second slot nor blocks the new
	 * one.
	 */
	for (auto &p : S.peers)
		if ((p.ph == peer::phase::joining || p.ph == peer::phase::syncing) && p.addr == from)
			host_peer_gone(p, kick_player_reason::timeout);

	const auto views{build_slot_views(callsign, from)};
	const auto decision{::dcx::net_v2::decide_admission(views, Netgame.max_numplayers, (Netgame.game_flag & netgame_rule_flags::closed) != netgame_rule_flags::None, bots_replaceable())};
	if (::dcx::net_v2::admission_accepts(decision.result) && decision.slot >= Netgame.max_numplayers)
	{
		/* Only a stale slot from before could be above the limit. */
		deny_join(from, req.client_nonce, kick_player_reason::full);
		return;
	}
	const bool is_new{decision.slot < MAX_PLAYERS && ::dcx::net_v2::admission_is_new(decision.result, S.awaits_entry[static_cast<playernum_t>(decision.slot)])};
	switch (decision.result)
	{
		case admission_result::deny_closed:
			deny_join(from, req.client_nonce, kick_player_reason::closed);
			return;
		case admission_result::deny_full:
			deny_join(from, req.client_nonce, kick_player_reason::full);
			return;
		case admission_result::deny_duplicate_callsign:
			deny_join(from, req.client_nonce, kick_player_reason::duplicate_callsign);
			return;
		case admission_result::accept_replace:
			/* The same machine restarted before its connection timed out. */
			con_printf(CON_NORMAL, "net: '%s' reconnects from the same address; replacing P#%u", callsign.operator const char *(), decision.slot);
			if (S.peers[decision.slot].ph != peer::phase::none)
				S.peers[decision.slot].conn.reset();
			host_peer_gone(S.peers[decision.slot], kick_player_reason::timeout);
			[[fallthrough]];
		case admission_result::accept_rejoin:
			{
				const playernum_t slot{static_cast<playernum_t>(decision.slot)};
				if (slot == 0 || slot >= MAX_PLAYERS)
					return;
				/* Restarted during its own join: still a new player. */
				if (is_new)
					break;
				if (Newdemo_state == ND_STATE_RECORDING)
					newdemo_record_multi_reconnect(slot);
				digi_play_sample(sound_effect::SOUND_HUD_MESSAGE, F1_0);
				const auto &&rankstr = GetRankStringWithSpace(Netgame.players[slot].rank);
				HUD_init_message(HM_MULTI, "%s%s'%s' %s", rankstr.first, rankstr.second, static_cast<const char *>(vcplayerptr(slot)->callsign), TXT_REJOIN);
				multi_send_score();
			}
			break;
		case admission_result::accept_new:
			break;
		case admission_result::accept_replace_bot:
			/* Bots section 2.3: the bot leaves (its eggs, PLAYER_LEFT)
			 * and the human takes its slot as a new player.  The bot
			 * does not come back in this game.
			 */
			if (!bots_remove_for_human(static_cast<playernum_t>(decision.slot)))
			{
				deny_join(from, req.client_nonce, kick_player_reason::full);
				return;
			}
			break;
	}
	const playernum_t slot{static_cast<playernum_t>(decision.slot)};
	if (slot == 0 || slot >= MAX_PLAYERS)
		return;
	if (is_new && S.peers[slot].ph != peer::phase::none)
		host_peer_gone(S.peers[slot], kick_player_reason::timeout);
	{
		auto &obj = *LevelUniqueObjectState.Objects.vmptr(vcplayerptr(slot)->objnum);
		obj.ctype.player_info.KillGoalCount = 0;
	}
	S.awaits_entry[slot] = is_new;
	accept_peer(slot, req, callsign, rank, from, peer::phase::joining, is_new);
	auto &p = S.peers[slot];
	send_game_settings(p);
	send_player_list(p);
}

/* Host, forming the game: a player enters the lobby (v1 net_udp_add_player). */
void lobby_add_player(const ::dcx::net_v2::join_request &req, const callsign_t &callsign, const netplayer_info::player_rank rank, const _sockaddr &from)
{
	auto &Objects = LevelUniqueObjectState.Objects;
	auto &vmobjptr = Objects.vmptr;
	playernum_t slot{MAX_PLAYERS};
	/* The same machine again (restarted, or its accept was lost with a new
	 * nonce): the same slot.
	 */
	if (const auto p{find_peer_by_addr(from)})
		slot = peer_slot(*p);
	else
	{
		/* Slots are never renumbered (each peer's player id is fixed by
		 * its JOIN_ACCEPT), so admission is bounded by the player limit
		 * instead: every slot then has a start position.  Holes left by
		 * departed players are filled first.
		 */
		per_player_array<bool> occupied{};
		for (auto &&[i, o] : enumerate(occupied))
			o = !i || S.peers[i].ph != peer::phase::none || (i < N_players && Netgame.players[i].callsign[0u]);
		const auto s{::dcx::net_v2::choose_lobby_slot(occupied, Netgame.max_numplayers)};
		if (!s)
		{
			deny_join(from, req.client_nonce, kick_player_reason::full);
			return;
		}
		slot = static_cast<playernum_t>(*s);
	}
	/* Not `is_new`: a lobby player enters the game with the level start,
	 * not through new_player; should it be late for the level start, its
	 * snapshot must not reset it as a new player.
	 */
	accept_peer(slot, req, callsign, rank, from, peer::phase::playing, false);
	Netgame.players[slot].connected = player_connection_status::playing;
	vmplayerptr(slot)->connected = player_connection_status::playing;
	vmobjptr(vcplayerptr(slot)->objnum)->ctype.player_info.KillGoalCount = 0;
	if (slot >= N_players)
		N_players = slot + 1;
	Netgame.numplayers = N_players;
	host_send_netgame_update();
}

/* Host: the refuse prompt (v1 net_udp_do_refuse_stuff). */
void do_refuse_stuff(const ::dcx::net_v2::join_request &req, const callsign_t &callsign, const netplayer_info::player_rank rank, const _sockaddr &from)
{
	for (unsigned i = 0; i < MAX_PLAYERS; ++i)
	{
		if (!d_stricmp(vcplayerptr(i)->callsign, callsign) && from == Netgame.players[i].protocol.udp.addr)
		{
			welcome_player(req, callsign, rank, from);
			return;
		}
	}

	if (!WaitForRefuseAnswer)
	{
#if DXX_BUILD_DESCENT == 1
		digi_play_sample (sound_effect::SOUND_CONTROL_CENTER_WARNING_SIREN,F1_0*2);
#elif DXX_BUILD_DESCENT == 2
		digi_play_sample (sound_effect::SOUND_HUD_JOIN_REQUEST,F1_0*2);
#endif

		const auto &&rankstr = GetRankStringWithSpace(rank);
		if (+(Game_mode & GM_TEAM))
		{
			HUD_init_message(HM_MULTI, "%s%s'%s' wants to join", rankstr.first, rankstr.second, callsign.operator const char *());
			HUD_init_message(HM_MULTI, "Alt-1 assigns to team %s. Alt-2 to team %s", Netgame.team_name[team_number::blue].operator const char *(), Netgame.team_name[team_number::red].operator const char *());
		}
		else
		{
			HUD_init_message(HM_MULTI, "%s%s'%s' wants to join (accept: F6)", rankstr.first, rankstr.second, callsign.operator const char *());
		}

		strcpy(RefusePlayerName, callsign);
		RefuseTimeLimit=timer_query();
		RefuseThisPlayer=0;
		WaitForRefuseAnswer=1;
	}
	else
	{
		if (strcmp(RefusePlayerName, callsign))
			return;

		if (RefuseThisPlayer)
		{
			RefuseTimeLimit=0;
			RefuseThisPlayer=0;
			WaitForRefuseAnswer=0;
			if (+(Game_mode & GM_TEAM))
			{
				const auto views{build_slot_views(callsign, from)};
				const auto decision{::dcx::net_v2::decide_admission(views, Netgame.max_numplayers, false, bots_replaceable())};
				if (::dcx::net_v2::admission_accepts(decision.result))
				{
					Assert (RefuseTeam==1 || RefuseTeam==2);
					if (RefuseTeam==1)
						Netgame.team_vector &=(~(1<<decision.slot));
					else
						Netgame.team_vector |=(1<<decision.slot);
				}
				welcome_player(req, callsign, rank, from);
				host_send_netgame_update();
			}
			else
			{
				welcome_player(req, callsign, rank, from);
			}
			return;
		}

		if ((timer_query()) > RefuseTimeLimit+REFUSE_INTERVAL)
		{
			RefuseTimeLimit=0;
			RefuseThisPlayer=0;
			WaitForRefuseAnswer=0;
			if (!strcmp(RefusePlayerName, callsign))
				deny_join(from, req.client_nonce, kick_player_reason::dork);
			return;
		}
	}
}

void handle_join_request(const std::span<const uint8_t> payload, const _sockaddr &from)
{
	if (!multi_i_am_master() || !S.join_limit.allow(S.now))
		return;
	const auto req{::dcx::net_v2::join_request::read(payload)};
	if (!req || req->game_id != Game_id)
		return;
	if (req->version != Program_version)
	{
		deny_join(from, req->client_nonce, kick_player_reason::version);
		return;
	}
	callsign_t callsign;
	callsign.copy_lower(std::span<const char, CALLSIGN_LEN>(reinterpret_cast<const char *>(req->callsign.data()), CALLSIGN_LEN));
	const auto rank{build_rank_from_untrusted(req->rank)};
	/* The game log names every join attempt once (the client retries
	 * every 500 ms with the same nonce).
	 */
	const bool first_request{req->client_nonce != S.logged_join_nonce};
	if (first_request)
	{
		S.logged_join_nonce = req->client_nonce;
		_sockaddr::presentation_buffer dbuf;
		con_printf(CON_NORMAL, "net: join request from '%s' at %s:%hu (attempt %08x, game %s)", callsign.operator const char *(), dxx_ntop(from, dbuf), dxx_sockaddr_port(from), req->client_nonce, network_state_name(Network_status));
	}
	/* A retry of an attempt that already has a connection: the same
	 * answer while the accept may have been lost, nothing once the
	 * connection is established (a delayed or reordered retry).
	 */
	for (auto &p : S.peers)
	{
		if (p.ph == peer::phase::none || !p.conn)
			continue;
		switch (::dcx::net_v2::classify_duplicate_join(p.addr == from, p.nonce, req->client_nonce, p.conn->state() == connection_state::connecting, p.ph == peer::phase::closing))
		{
			case ::dcx::net_v2::duplicate_join::none:
				continue;
			case ::dcx::net_v2::duplicate_join::resend_accept:
				send_unconnected(from, S.session_id, p.token, 0, session_msg::join_accept, p.accept_payload);
				return;
			case ::dcx::net_v2::duplicate_join::ignore:
				return;
		}
	}
	switch (Network_status)
	{
		case network_state::starting:
			/* Between the lobby and the level start (team selection) the
			 * player list is settled: no answer, the client retries.
			 */
			if (S.lobby_open)
				lobby_add_player(*req, callsign, rank, from);
			else if (first_request)
				con_printf(CON_NORMAL, "net: join request from '%s' while the host sets up the level start; not answered (the client retries)", callsign.operator const char *());
			break;
		case network_state::playing:
			/* One join in progress at a time, as in v1: the joiner's
			 * N_players accounting and the extras assume it.  No answer;
			 * the client retries every 500 ms for 10 s.
			 */
			if (join_in_progress(from))
			{
				if (first_request)
					con_printf(CON_NORMAL, "net: '%s' waits: another player is joining (not answered; the client retries)", callsign.operator const char *());
				break;
			}
			if (Netgame.RefusePlayers)
				do_refuse_stuff(*req, callsign, rank, from);
			else
				welcome_player(*req, callsign, rank, from);
			break;
		case network_state::endlevel:
			deny_join(from, req->client_nonce, kick_player_reason::endlevel);
			break;
		case network_state::waiting:
			/* Answered once the level is loaded; the client retries. */
			if (first_request)
				con_printf(CON_NORMAL, "net: '%s' waits: the level is starting (not answered; the client retries)", callsign.operator const char *());
			break;
		case network_state::menu:
		case network_state::browsing:
			break;
	}
}

/* Host: a peer reports its level loaded. */
void handle_level_ready(peer &p, const std::span<const uint8_t> payload)
{
	if (payload.size() != LEVEL_READY_SIZE)
		return;
	reader r{payload};
	p.ready_level = r.i32();
	p.ready_checksum = r.u16();
	p.has_ready = true;
	const auto slot{peer_slot(p)};
	switch (Network_status)
	{
		case network_state::waiting:
			if (p.ready_level != Current_level_num)
				break;
			if (p.ready_checksum != my_segments_checksum)
			{
				kick_peer(p, kick_player_reason::checksum);
				break;
			}
			vmplayerptr(slot)->connected = player_connection_status::playing;
			Netgame.players[slot].LastPacketTime = S.now;
			break;
		case network_state::playing:
			if (p.ph == peer::phase::joining || vcplayerptr(slot)->connected == player_connection_status::waiting)
			{
				if (p.ready_level != Current_level_num)
				{
					kick_peer(p, kick_player_reason::level);
					break;
				}
				if (p.ready_checksum != my_segments_checksum)
				{
					kick_peer(p, kick_player_reason::checksum);
					break;
				}
				queue_snapshot(p);
			}
			break;
		case network_state::starting:
		case network_state::endlevel:
		case network_state::menu:
		case network_state::browsing:
			/* Kept for the level wait. */
			break;
	}
}

/* Host: tell everyone but `exclude` that a new player is in `slot`. */
void broadcast_player_joined(const playernum_t slot, const playernum_t exclude)
{
	std::array<uint8_t, PLAYER_JOINED_SIZE> buf;
	writer w{buf.data()};
	w.u8(slot);
	w.bytes(&Netgame.players[slot].callsign[0u], CALLSIGN_LEN + 1);
	w.u8(underlying_value(Netgame.players[slot].rank));
	w.u8((Netgame.team_vector >> slot) & 1);
	broadcast_reliable(session_msg::player_joined, buf, exclude);
}

/* Host: the joining peer applied the snapshot (v1 rejoin sync). */
void handle_client_ready(peer &p)
{
	if (p.ph != peer::phase::syncing)
		return;
	const auto slot{peer_slot(p)};
	p.ph = peer::phase::playing;
	if (p.is_new)
	{
		/* From now on the player is in the game: a later snapshot (a
		 * level it was late for) must not reset its score again.
		 */
		p.is_new = false;
		S.awaits_entry[slot] = false;
		new_player(slot, Netgame.players[slot].callsign, Netgame.players[slot].rank);
		broadcast_player_joined(slot, slot);
	}
	else
		vmplayerptr(slot)->connected = player_connection_status::playing;
	Netgame.players[slot].connected = player_connection_status::playing;
	Netgame.players[slot].LastPacketTime = S.now;
	/* Deathmatch: the site of its first spawn, ahead of LEVEL_GO. */
	net_spawn_host_join(slot);
	{
		std::array<uint8_t, LEVEL_GO_SIZE> buf;
		writer w{buf.data()};
		w.u32(0);
		w.u32(::dcx::net_v2::to_net_time(S.now));
		peer_queue(p, session_msg::level_go, buf);
	}
	p.in_level = true;
	start_extras(slot);
}

/* Client: JOIN_ACCEPT */
void handle_join_accept(const packet_header &h, const std::span<const uint8_t> payload, const _sockaddr &from)
{
	if (multi_i_am_master() || !S.join.active())
		return;
	const auto acc{::dcx::net_v2::join_accept::read(payload)};
	if (!acc || acc->client_nonce != S.join.nonce() || acc->session_id != S.session_id || !h.peer_token)
		return;
	if (acc->player_id == 0 || acc->player_id >= MAX_PLAYERS)
		return;
	if (netgame_tick_rate_valid(acc->tick_rate))
		Netgame.TickRate = acc->tick_rate;
	S.my_token = h.peer_token;
	auto &p = S.peers[0];
	drop_peer(p);
	p.ph = peer::phase::playing;
	p.token = h.peer_token;
	p.addr = from;
	p.conn.emplace(make_connection_config(p.token, acc->player_id, 0), S.now);
	p.conn->set_peer_tick(local_tick_period());
	Netgame.players[0].protocol.udp.addr = from;
	change_playernum_to(acc->player_id);
	Netgame.protocol.udp.your_index = acc->player_id;
	S.join.end();
	S.join_result = join_status::accepted;
	/* Joined: no GAME_INFO is taken from now on (a late answer must not
	 * describe the game anew).
	 */
	S.info_addr.reset();
	con_printf(CON_NORMAL, "net: joined session %08x as P#%u, tick rate %u Hz", S.session_id, acc->player_id, Netgame.TickRate);
	/* Our ship, to the host (custom ships, net_ships.cpp). */
	net_ships_start(false, static_cast<uint8_t>(acc->player_id));
	/* The mission, if this client lacks the host's version. */
	net_mission_client_connected();
	/* Protocol 108: tell the host the address it answered from, which
	 * behind a NAT router is its public one.
	 */
	if (const auto seen{seen_from_sockaddr(from)}; seen && ::dcx::seen_address_kind(*seen) != host_address_kind::unusable)
	{
		std::array<uint8_t, ::dcx::NET_V2_ADDRESS_SEEN_SIZE> buf;
		::dcx::write_address_seen(*seen, buf.data());
		peer_queue(p, session_msg::address_seen, buf);
	}
}

/* Host: a player's ADDRESS_SEEN (protocol 108). */
void handle_address_seen(const playernum_t slot, const std::span<const uint8_t> payload)
{
	using result = decltype(Public_tally)::report_result;
	switch (Public_tally.report(slot, payload, S.now, ADDRESS_SEEN_INTERVAL))
	{
		case result::accepted:
			if (const auto b{::dcx::read_address_seen(payload)})
				con_printf(CON_NORMAL, "net: P#%u reaches this game at %s", slot, ::dcx::seen_address_text(*b).c_str());
			log_public_address_choice();
			break;
		case result::unchanged:
			break;
		case result::not_public:
			con_printf(CON_VERBOSE, "net: P#%u reaches this game at a LAN address", slot);
			break;
		case result::malformed:
			con_printf(CON_VERBOSE, "net: P#%u sent a malformed ADDRESS_SEEN; ignored", slot);
			break;
		case result::rate_limited:
			con_printf(CON_VERBOSE, "net: P#%u sent ADDRESS_SEEN too often; ignored", slot);
			break;
	}
}

/* Client: the message for a refused join or a kick (v1 net_udp_process_dump). */
void show_refusal(const kick_player_reason why)
{
	const char *dump_string;
	switch (why)
	{
		case kick_player_reason::closed:
			dump_string = TXT_NET_GAME_CLOSED;
			break;
		case kick_player_reason::full:
			dump_string = TXT_NET_GAME_FULL;
			break;
		case kick_player_reason::endlevel:
			dump_string = TXT_NET_GAME_BETWEEN;
			break;
		case kick_player_reason::dork:
			dump_string = TXT_NET_GAME_NSELECT;
			break;
		case kick_player_reason::aborted:
			dump_string = TXT_NET_GAME_NSTART;
			break;
		case kick_player_reason::level:
			dump_string = TXT_NET_GAME_WRONGLEV;
			break;
		case kick_player_reason::checksum:
			dump_string = TXT_NETLEVEL_NMATCH;
			break;
		case kick_player_reason::duplicate_callsign:
			dump_string = "A player with your callsign\nis already in this game.";
			break;
		case kick_player_reason::version:
			dump_string = "Version mismatch! Cannot join Game.";
			break;
		case kick_player_reason::snapshot_failed:
			dump_string = TXT_NET_SYNC_FAILED;
			break;
		case kick_player_reason::queue_overflow:
			dump_string = "You were removed from the game.\nThe connection to the host\nis too slow. Sorry.";
			break;
		case kick_player_reason::protocol_error:
			dump_string = "You were removed from the game.\nThe host received malformed\npackets from you. Sorry.";
			break;
		case kick_player_reason::timeout:
			dump_string = "You were removed from the game.\nThe host did not hear from\nyou in time. Sorry.";
			break;
		case kick_player_reason::kicked:
			dump_string = "You were kicked by Host!";
			break;
		case kick_player_reason::host_shutdown:
			dump_string = "Host left the game!";
			break;
		case kick_player_reason::quit:
		case kick_player_reason::cancelled:
			dump_string = TXT_NET_GAME_NSTART;
			break;
		default:
			dump_string = TXT_NET_GAME_CLOSED;
			break;
	}
	nm_messagebox_str(menu_title{nullptr}, TXT_OK, menu_subtitle{dump_string});
}

void handle_join_deny(const std::span<const uint8_t> payload, const _sockaddr &from)
{
	if (multi_i_am_master())
		return;
	const auto d{::dcx::net_v2::join_deny::read(payload)};
	if (!d)
		return;
	const auto why{build_kick_player_reason_from_untrusted(d->reason)};
	if (!why)
		return;
	const bool version_mismatch{*why == kick_player_reason::version};
	const auto match{::dcx::net_v2::classify_join_deny(d->client_nonce, version_mismatch, S.join.active(), S.join.nonce(), from == S.join_addr, S.info_addr && from == *S.info_addr)};
	if (match == ::dcx::net_v2::join_deny_match::ignore)
		return;
	if (version_mismatch)
	{
		/* Also the answer to a GAME_INFO_REQ from a mismatched version:
		 * the connect menu shows the versions.
		 */
		Netgame.protocol.udp.program_iver[0] = d->version.major;
		Netgame.protocol.udp.program_iver[1] = d->version.minor;
		Netgame.protocol.udp.program_iver[2] = d->version.micro;
		Netgame.protocol.udp.program_iver[3] = d->proto;
		Netgame.protocol.udp.valid = -1;
	}
	if (match != ::dcx::net_v2::join_deny_match::join)
		return;
	con_printf(CON_NORMAL, "net: the host refused the join: %s", kick_reason_name(*why));
	S.join.end();
	S.join_result = join_status::denied;
	if (!version_mismatch)
		show_refusal(*why);
}

/* Client: the host removed us, or left. */
void handle_kick(const kick_player_reason why)
{
	con_printf(CON_NORMAL, "net: removed by the host: %s (network status: %s)", kick_reason_name(why), network_state_name(Network_status));
	con_flush_gamelog();
	if (auto &p{S.peers[0]}; p.conn)
		drop_peer(p);
	switch (why)
	{
		case kick_player_reason::kicked:
		case kick_player_reason::queue_overflow:
		case kick_player_reason::protocol_error:
		case kick_player_reason::timeout:
		case kick_player_reason::host_shutdown:
			{
				const auto g{Game_wind};
				if (g)
					g->set_visible(0);
				show_refusal(why);
				if (g && g == Game_wind)
					g->set_visible(1);
				multi_quit_game = 1;
				game_leave_menus();
				break;
			}
		default:
			Network_status = network_state::menu; // stop us from sending before message
			show_refusal(why);
			Network_status = network_state::menu;
			multi_reset_stuff();
			break;
	}
}

void handle_host_lost(const kick_player_reason why, const unsigned silent_ms = 0)
{
	con_printf(CON_NORMAL, "net: lost the host: %s (network status: %s)", kick_reason_name(why), network_state_name(Network_status));
	con_flush_gamelog();
	if (auto &p{S.peers[0]}; p.conn)
		drop_peer(p);
	if (Network_status == network_state::waiting || Network_status == network_state::browsing || Network_status == network_state::menu)
	{
		/* The level start menu notices the host is gone, and says why
		 * (client_take_host_lost).
		 */
		Netgame.players[0].connected = player_connection_status::disconnected;
		vmplayerptr(0u)->connected = player_connection_status::disconnected;
		S.host_lost = host_lost_info{.why = why, .silent_ms = silent_ms};
		return;
	}
	multi_disconnect_player(0);
}

/* Tracker (section 4.1): the outer layouts of opcodes 21-26 are dictated
 * by the tracker program and unchanged from v1; the version string ends
 * in the protocol version, and the game blob is a v2 GAME_INFO_LITE
 * datagram.
 */
#if DXX_USE_TRACKER
constexpr uint8_t UPID_TRACKER_REGISTER{21};	// Register or update a game on the tracker.
constexpr uint8_t UPID_TRACKER_REMOVE{22};	// Remove our game from the tracker.
constexpr uint8_t UPID_TRACKER_REQGAMES{23};	// Request a list of all games stored on the tracker.
constexpr uint8_t UPID_TRACKER_GAMEINFO{24};	// Packet containing info about a game
constexpr uint8_t UPID_TRACKER_ACK{25};	// An ACK packet from the tracker
constexpr uint8_t UPID_TRACKER_HOLEPUNCH{26};	// Hole punching process.

/* How long the host waits for the tracker before it says the tracker
 * does not answer: two registrations (the host registers every 10 s,
 * and the tracker program drops a registration that arrives while it
 * clears stale games, sending no ACK for it).
 */
constexpr fix64 TRACKER_ACK_TIMEOUT{F1_0 * 25};
/* The host asks for the game list to see its own entry (the address the
 * tracker sees it at) this often once it found it.
 */
constexpr unsigned TRACKER_SELF_CHECK_REGISTRATIONS{6};
/* A client sums up the tracker's answer to its game list request after
 * this long.
 */
constexpr fix64 TRACKER_LIST_SUMMARY_DELAY{F1_0 * 3};

_sockaddr TrackerSocket;
/* The tracker's name resolved (udp_tracker_init). */
bool Tracker_resolved;
/* What udp_tracker_init logged last, to log only a change. */
std::string Tracker_logged_address;
enum class TrackerAckState : uint8_t
{
	TACK_NOCONNECTION,   // No connection with tracker (yet);
	TACK_INTERNAL	= 1, // Got ACK on TrackerSocket
	TACK_EXTERNAL	= 2, // Got ACK on our game sopcket
	TACK_SEQCOMPL	= 3, // We had enough time to get all acks. If we missed something now, tell the user
};
TrackerAckState TrackerAckStatus;
fix64 TrackerAckTime;
/* Host: per hosted game.  An ACK of either kind arrived; the tracker's
 * game list holds this game (its own entry, found by the session id);
 * registrations sent; the timeout was reported.
 */
bool Tracker_acked;
bool Tracker_listed;
unsigned Tracker_registrations;
bool Tracker_timeout_reported;
/* The last send error logged (0: none), to log each kind once. */
int Tracker_send_error;
/* Client: the game list request in progress, and the games it brought. */
fix64 Tracker_list_requested;
bool Tracker_list_pending;
std::vector<uint16_t> Tracker_list_ids;
/* Client: the address a host answered a hole-punch request from, and the
 * game id last asked about (to log once).
 */
std::optional<_sockaddr> Holepunch_reply;
uint16_t Holepunch_logged_id;
/* Client: when the last hole-punch request went out.  An answer counts
 * only shortly after one (anyone can send the one-byte answer, and it
 * moves the join to its sender).
 */
fix64 Holepunch_asked_at;
bool Holepunch_asked;
constexpr fix64 HOLEPUNCH_ANSWER_WINDOW{F1_0 * 5};
/* Host: the client address of the last hole-punch request logged. */
std::string Holepunch_logged_client;

[[nodiscard]]
std::string sockaddr_text(const _sockaddr &a)
{
	typename _sockaddr::presentation_buffer dbuf;
	return format_address_port(dxx_ntop(a, dbuf), dxx_sockaddr_port(a));
}

void tracker_reset_host_state()
{
	TrackerAckStatus = TrackerAckState::TACK_NOCONNECTION;
	TrackerAckTime = timer_query();
	Tracker_acked = false;
	Tracker_listed = false;
	Tracker_registrations = 0;
	Tracker_timeout_reported = false;
}

/* The tracker program listens on IPv4 only, so its IPv4 address is
 * wanted even when the name also has an IPv6 one (which a dual-stack
 * lookup may return first, and which never reaches it).  As an
 * IPv4-mapped address on the IPv6 socket.
 */
int tracker_resolve(_sockaddr &out, const char *const host, const uint16_t port)
{
	addrinfo hints{};
	hints.ai_family = AF_INET;
	hints.ai_socktype = SOCK_DGRAM;
	char sport[6];
	snprintf(sport, sizeof(sport), "%hu", port);
	::dcx::RAIIaddrinfo result;
	if (result.getaddrinfo(host, sport, &hints) == 0 && result.get() && result->ai_addr && result->ai_addr->sa_family == AF_INET && result->ai_addrlen >= sizeof(sockaddr_in))
	{
		sockaddr_in v4;
		memcpy(&v4, result->ai_addr, sizeof(v4));
		out = {};
#if DXX_USE_IPv6
		out.sin6.sin6_family = AF_INET6;
		out.sin6.sin6_port = v4.sin_port;
		out.sin6.sin6_addr.s6_addr[10] = 0xff;
		out.sin6.sin6_addr.s6_addr[11] = 0xff;
		memcpy(&out.sin6.sin6_addr.s6_addr[12], &v4.sin_addr, 4);
#else
		out.sin = v4;
#endif
		return 0;
	}
	/* No IPv4 address: whatever the name has. */
	return udp_dns_filladdr(out, host, port, false, true);
}

/* Tracker initialization */
int udp_tracker_init()
{
	tracker_reset_host_state();
	Tracker_resolved = false;
	if (CGameArg.MplTrackerAddr.empty())
		return 0;

	const char *tracker_addr = CGameArg.MplTrackerAddr.c_str();

	// Fill the address
	if (tracker_resolve(TrackerSocket, tracker_addr, CGameArg.MplTrackerPort) < 0)
	{
		TrackerSocket = {};
		if (Tracker_logged_address != "?")
		{
			con_printf(CON_URGENT, "[Tracker] cannot resolve the tracker's name %s: no games from the tracker, and hosted games are not listed (test the name with nslookup %s)", tracker_addr, tracker_addr);
			Tracker_logged_address = "?";
		}
		return -1;
	}
	Tracker_resolved = true;
	if (auto text{sockaddr_text(TrackerSocket)}; text != Tracker_logged_address)
	{
		con_printf(CON_NORMAL, "[Tracker] tracker %s:%u is at %s", tracker_addr, CGameArg.MplTrackerPort, text.c_str());
		Tracker_logged_address = std::move(text);
	}
	return 0;
}

/* Send to the tracker; log a failure (once per kind) instead of losing
 * it silently.
 */
void tracker_send(const std::span<const uint8_t> bytes, const char *const what)
{
	if (!Tracker_resolved)
	{
		con_printf(CON_VERBOSE, "[Tracker] %s not sent: the tracker's address is not known", what);
		return;
	}
	if (bytes.empty() || !UDP_Socket[0])
		return;
	if (dxx_sendto(UDP_Socket[0], bytes, 0, TrackerSocket) >= 0)
		return;
#ifdef _WIN32
	const int e{WSAGetLastError()};
#else
	const int e{errno};
#endif
	if (e == Tracker_send_error)
		return;
	Tracker_send_error = e;
	con_printf(CON_URGENT, "[Tracker] sending %s to the tracker at %s failed (socket error %i)", what, sockaddr_text(TrackerSocket).c_str(), e);
}

/* Compares sender to tracker. Returns 1 if address matches, Returns 2 is address and port matches. */
int sender_is_tracker(const _sockaddr &sender, const _sockaddr &tracker)
{
	uint16_t sf, tf, sp, tp;

	sf = sender.sin.sin_family;
	tf = tracker.sin.sin_family;

#if DXX_USE_IPv6
	if (sf == AF_INET6)
	{
		if (tf == AF_INET)
		{
			if (IN6_IS_ADDR_V4MAPPED(&sender.sin6.sin6_addr))
			{
				if (memcmp(&sender.sin6.sin6_addr.s6_addr[12], &tracker.sin.sin_addr, sizeof(tracker.sin.sin_addr)))
					return 0;
				tp = tracker.sin.sin_port;
			}
			else
				return 0;
		}
		else if (tf == AF_INET6)
		{
			if (memcmp(&sender.sin6.sin6_addr, &tracker.sin6.sin6_addr, sizeof(sender.sin6.sin6_addr)))
				return 0;
			tp = tracker.sin6.sin6_port;
		}
		else
			return 0;
		sp = sender.sin6.sin6_port;
	}
	else
#endif
	if (sf == AF_INET)
	{
		if (sf != tf)
			return 0;
		if (memcmp(&sender.sin.sin_addr, &tracker.sin.sin_addr, sizeof(sender.sin.sin_addr)))
			return 0;
		sp = sender.sin.sin_port;
		tp = tracker.sin.sin_port;
	}
	else
		return 0;

	if (tp == sp)
		return 2;
	else
		return 1;
}

/* Host: the tracker's game list holds this game at `addr` (as the
 * tracker sees the game socket: the public address behind a NAT
 * router).
 */
void tracker_found_own_game(const _sockaddr &addr)
{
	if (!Tracker_listed)
		con_printf(CON_NORMAL, "[Tracker] the tracker lists this game at %s: registered", sockaddr_text(addr).c_str());
	Tracker_listed = true;
	if (const auto seen{seen_from_sockaddr(addr)}; seen && Public_tally.set_tracker(*seen))
		log_public_address_choice();
}

/* The tracker has sent us a game.  Let's list it.  The layout:
 * "a=<ip>/<port>,c=<game id, 2 bytes>,z=<GAME_INFO_LITE datagram>"
 * after the opcode.
 */
void udp_tracker_process_game(const std::span<const uint8_t> buf, const _sockaddr &sender_addr)
{
	// Only accept data from the tracker we specified: on a client looking at the netlist (i.e. network_state::browsing), or on the host looking for its own entry
	if (!sender_is_tracker(sender_addr, TrackerSocket))
		return;
	const bool browsing{Network_status == network_state::browsing};
	const bool own_check{multi_i_am_master() && Netgame.Tracker && S.session_id && !browsing};
	if (!browsing && !own_check)
		return;

	const char *p0 = NULL, *p1 = NULL;
	char sIP[47]{};
	std::array<char, 6> sPort{};
	uint16_t iPort{0};

	/* The text part is parsed with string functions: give it a terminator
	 * (the game id and the game blob are binary).
	 */
	std::vector<char> text(buf.begin(), buf.end());
	text.push_back(0);
	const char *const data = text.data();
	const auto data_len = buf.size();
	// Get the IP
	if ((p0 = strstr(data, "a=")) == NULL)
		return;
	p0 +=2;
	if ((p1 = strstr(p0, "/")) == NULL)
		return;
	if (p1-p0 < 1 || p1-p0 >= sizeof(sIP))
		return;
	memcpy(sIP, p0, p1-p0);

	// Get the port
	p1++;
	const auto p2 = strstr(p1, "c=");
	if (p2 == nullptr)
		return;
	if (p2-p1-1 < 1 || p2-p1-1 >= sizeof(sPort))
		return;
	memcpy(&sPort, p1, p2-p1-1);
	char *porterror;
	const unsigned long port = strtoul(sPort.data(), &porterror, 10);
	if (*porterror || static_cast<uint16_t>(port) != port)
		return;
	iPort = port;

	// Get the DNS stuff
	struct _sockaddr sAddr;
	if(udp_dns_filladdr(sAddr, sIP, iPort, true, true) < 0)
		return;
	/* The game id: 2 bytes after "c=", then ",z=".  Searched in the
	 * buffer, not with strstr: the id may hold a zero byte (which ended
	 * the search before, losing the game) or the bytes "z=".
	 */
	const std::size_t id_pos = (p2 - data) + 2;
	if (data_len < id_pos + 2)
		return;
	const auto TrackerGameID = tracker_game_id{GET_INTEL_SHORT(&buf[id_pos])};
	constexpr std::array<uint8_t, 2> z_key{{'z', '='}};
	const auto tail{buf.subspan(id_pos + 2)};
	const auto z{std::ranges::search(tail, z_key)};
	if (z.empty())
		return;
	const auto blob{tail.subspan(static_cast<std::size_t>(z.end() - tail.begin()))};
	if (blob.empty())
		return;

	// Now process the actual lite_game datagram contained.
	const auto m{::dcx::net_v2::parse_unconnected(blob, 0)};
	if (m.status != ::dcx::net_v2::unconnected_status::accepted || m.type != session_msg::game_info_lite)
		return;
	auto g{parse_game_info_lite(m.payload, sAddr)};
	if (!g)
		return;
	if (own_check)
	{
		if (g->session_id == S.session_id)
			tracker_found_own_game(sAddr);
		return;
	}
	if (const uint16_t id{underlying_value(TrackerGameID)}; Tracker_list_pending && std::ranges::find(Tracker_list_ids, id) == Tracker_list_ids.end())
	{
		Tracker_list_ids.push_back(id);
		con_printf(CON_NORMAL, "[Tracker] game list: \"%s\" (tracker id %u, %u/%u players) at %s", g->game_name.data(), id, g->numconnected, g->max_numplayers, sockaddr_text(sAddr).c_str());
	}
	g->TrackerGameID = TrackerGameID;
	net_udp_game_list_update(std::move(*g));
}

/* Client: sum up the answer to the game list request. */
void tracker_list_summary()
{
	if (!Tracker_list_pending || S.now < Tracker_list_requested + TRACKER_LIST_SUMMARY_DELAY)
		return;
	Tracker_list_pending = false;
	if (Tracker_list_ids.empty())
		con_puts(CON_NORMAL, "[Tracker] game list: no games from the tracker (none hosted for this version, or the tracker does not answer)");
	else
		con_printf(CON_NORMAL, "[Tracker] game list: %u game(s) from the tracker", static_cast<unsigned>(Tracker_list_ids.size()));
}

void tracker_send_request_games()
{
	std::array<uint8_t, 2 + 4 + sizeof("00000.00000.00000.00000")> pBuf{};
	pBuf[0] = UPID_TRACKER_REQGAMES;
	const std::size_t len = 1 + snprintf(reinterpret_cast<char *>(&pBuf[1]), pBuf.size() - 1, "%c%c%c%c" DXX_VERSION_STR ".%hu", Game_id[0], Game_id[1], Game_id[2], Game_id[3], MULTI_PROTO_VERSION);
	tracker_send(std::span<const uint8_t>(pBuf).first(len), "the game list request");
}

/* Process ACK's from tracker. We will get up to 5, each internal and external */
void udp_tracker_process_ack(const std::span<const uint8_t> data, const _sockaddr &sender_addr)
{
	if(!Netgame.Tracker)
		return;
	if (data.size() != 2)
		return;
	int addr_check = sender_is_tracker(sender_addr, TrackerSocket);
	if (!addr_check)
		return;
	if (!Tracker_acked)
	{
		Tracker_acked = true;
		if (Tracker_timeout_reported)
			con_puts(CON_NORMAL, "[Tracker] ACK received after all: your game is listed on the tracker.");
		/* The game is stored now: find its entry (the address the
		 * tracker sees).
		 */
		tracker_send_request_games();
	}

	switch (data[1])
	{
		case 0: // ack coming from the same socket we are already talking with the tracker
			if (TrackerAckStatus == TrackerAckState::TACK_NOCONNECTION && addr_check == 2)
			{
				TrackerAckStatus = TrackerAckState::TACK_INTERNAL;
				con_puts(CON_NORMAL, "[Tracker] Got internal ACK. Your game is hosted!");
			}
			break;
		case 1: // ack from another socket (same IP, different port) to see if we're reachable from the outside
			if (TrackerAckStatus <= TrackerAckState::TACK_INTERNAL)
			{
				TrackerAckStatus = TrackerAckState::TACK_EXTERNAL;
				con_puts(CON_NORMAL, "[Tracker] Got external ACK. Your game is hosted and game port is reachable!");
			}
			break;
	}
}

/* TRACKER_ACK_TIMEOUT passed since we registered our game. If we have not received all ACK's, yet, tell user about that! */
void udp_tracker_verify_ack_timeout()
{
	if (!Netgame.Tracker || !multi_i_am_master() || !Tracker_registrations || TrackerAckTime + TRACKER_ACK_TIMEOUT > timer_query() || TrackerAckStatus == TrackerAckState::TACK_SEQCOMPL)
		return;
	if (TrackerAckStatus == TrackerAckState::TACK_NOCONNECTION && !Tracker_listed)
	{
		TrackerAckStatus = TrackerAckState::TACK_SEQCOMPL; // set this now or we'll run into an endless loop if nm_messagebox triggers.
		Tracker_timeout_reported = true;
		con_printf(CON_URGENT, "[Tracker] No response from game tracker %s (%s) after %u registrations in %u s. Tracker address may be invalid or Tracker may be offline or otherwise unreachable; players can still join by IP.", CGameArg.MplTrackerAddr.c_str(), Tracker_resolved ? sockaddr_text(TrackerSocket).c_str() : "name not resolved", Tracker_registrations, static_cast<unsigned>(TRACKER_ACK_TIMEOUT / F1_0));
		if (Network_status == network_state::playing)
			HUD_init_message_literal(HM_MULTI, "No ACK from tracker. Please check game log.");
		else
			/* Not a blocking message box: this runs inside the network
			 * frame, and while a nested one waited for the host to click
			 * it nothing was sent or received, so every player in the
			 * lobby timed out.
			 */
			window_create<passive_messagebox>(menu_title{TXT_WARNING}, menu_subtitle{"No ACK from tracker.\nPlease check game log."}, TXT_OK, grd_curscreen->sc_canvas);
	}
	else if (TrackerAckStatus == TrackerAckState::TACK_NOCONNECTION)
		/* The tracker sends an ACK only for a game new to it; one it
		 * still had from this address and port (a quick restart) gets
		 * none, but its list shows it.
		 */
		con_puts(CON_NORMAL, "[Tracker] No ACK from the tracker, but its game list holds your game: it is registered.");
	else if (TrackerAckStatus == TrackerAckState::TACK_INTERNAL)
	{
		con_puts(CON_NORMAL, "[Tracker] No external signal from game tracker.  Your game port does not seem to be reachable.");
		con_puts(CON_NORMAL, Netgame.TrackerNATWarned == TrackerNATHolePunchWarn::UserEnabledHP ? std::span<const char>("Clients will attempt hole-punching to join your game.") : std::span<const char>("Clients will only be able to join your game if specifically configured in your router."));
	}
	TrackerAckStatus = TrackerAckState::TACK_SEQCOMPL;
}

/* We don't seem to be able to connect to a game. Ask Tracker to send hole punch request to host. */
void udp_tracker_request_holepunch(const tracker_game_id id)
{
	std::array<uint8_t, 3> pBuf;

	pBuf[0] = UPID_TRACKER_HOLEPUNCH;
	const uint16_t TrackerGameID = underlying_value(id);
	PUT_INTEL_SHORT(&pBuf[1], TrackerGameID);

	if (Holepunch_logged_id != TrackerGameID)
	{
		/* A new game: an answer from the host of another one must not
		 * redirect this join.
		 */
		Holepunch_reply.reset();
		Holepunch_logged_id = TrackerGameID;
		con_printf(CON_NORMAL, "[Tracker] Asking the tracker to have the host of game [%i] answer (hole punch).", TrackerGameID);
	}
	Holepunch_asked = true;
	Holepunch_asked_at = timer_query();
	tracker_send(pBuf, "a hole-punch request");
}

/* Tracker sent us an address from a client requesting hole punching.
 * We'll simply reply with another hole punch packet and wait for them to request our game info properly. */
void udp_tracker_process_holepunch(const std::span<const uint8_t> data, const _sockaddr &sender_addr)
{
	if (data.size() == 1 && !multi_i_am_master())
	{
		/* The host's answer comes from the game's real address (the
		 * tracker's game list may give another: the version of the
		 * tracker program running since 2024 lists every game at the
		 * address of the player asking).  Only as the answer to a
		 * request of ours, not from the tracker, and from a port.
		 */
		if (!Holepunch_asked || timer_query() > Holepunch_asked_at + HOLEPUNCH_ANSWER_WINDOW || sender_is_tracker(sender_addr, TrackerSocket) || !dxx_sockaddr_port(sender_addr))
			return;
		if (!Holepunch_reply || *Holepunch_reply != sender_addr)
			con_printf(CON_NORMAL, "[Tracker] Received hole-punch answer from a host at %s.", sockaddr_text(sender_addr).c_str());
		Holepunch_reply = sender_addr;
		return;
	}
	if (!Netgame.Tracker || !sender_is_tracker(sender_addr, TrackerSocket) || !multi_i_am_master())
		return;
	if (Netgame.TrackerNATWarned != TrackerNATHolePunchWarn::UserEnabledHP)
	{
		con_puts(CON_NORMAL, "[Tracker] Ignoring tracker hole-punch request because user disabled hole punch.");
		return;
	}
	if (data.size() < 2)
		return;

	/* "<ip>/<port>" after the opcode.  The original tracker ended it
	 * with a zero byte, the one running since 2024 does not; take both.
	 */
	std::vector<char> copy(data.begin() + 1, data.end());
	if (copy.back())
		copy.push_back(0);
	auto &delimiter = "/";

	const auto sIP = strtok(copy.data(), delimiter);
	if (!sIP)
		return;
	const auto pPort = strtok(NULL, delimiter);
	if (!pPort)
		return;
	char *porterror;
	const auto myport = strtoul(pPort, &porterror, 10);
	if (*porterror)
		return;
	const uint16_t iPort = myport;
	if (iPort != myport || !iPort)
		return;

	// Get the DNS stuff
	struct _sockaddr sAddr;
	if(udp_dns_filladdr(sAddr, sIP, iPort, true, true) < 0)
		return;

	if (auto text{sockaddr_text(sAddr)}; text != Holepunch_logged_client)
	{
		con_printf(CON_NORMAL, "[Tracker] Hole-punch request: answering a player at %s.", text.c_str());
		Holepunch_logged_client = std::move(text);
	}
	const std::array<uint8_t, 1> pBuf{{
		UPID_TRACKER_HOLEPUNCH
	}};
	send_raw(pBuf, sAddr);
}

/* A datagram from the tracker, or a hole-punch datagram: the v1 outer
 * layout.  Returns true if the datagram was one of those.
 */
[[nodiscard]]
bool receive_tracker_datagram(const std::span<const uint8_t> buf, const _sockaddr &from)
{
	switch (buf[0])
	{
		case UPID_TRACKER_GAMEINFO:
			udp_tracker_process_game(buf, from);
			return true;
		case UPID_TRACKER_ACK:
			if (multi_i_am_master())
				udp_tracker_process_ack(buf, from);
			return true;
		case UPID_TRACKER_HOLEPUNCH:
			udp_tracker_process_holepunch(buf, from);
			return true;
		default:
			return false;
	}
}
#endif

/* Discovery (section 4.1) */

void handle_game_info_lite_req(const std::span<const uint8_t> payload, const _sockaddr &from)
{
	if (!multi_i_am_master() || Network_status == network_state::menu)
		return;
	const auto req{::dcx::net_v2::game_info_request::read(payload)};
	/* No response to another kind of Descent, nor to another version: the
	 * peer was searching for games it can join.
	 */
	if (!req || req->game_id != Game_id || req->version != Program_version)
		return;
	if (!S.lite_limit.allow(S.now))
		return;
	net_udp_update_netgame();
	std::array<uint8_t, 128> buf;
	send_unconnected(from, 0, 0, 0, session_msg::game_info_lite, build_game_info_lite(buf));
}

void handle_game_info_req(const std::span<const uint8_t> payload, const _sockaddr &from)
{
	if (!multi_i_am_master() || Network_status == network_state::menu)
		return;
	const auto req{::dcx::net_v2::game_info_request::read(payload)};
	if (!req || req->game_id != Game_id)
		return;
	if (!S.info_limit.allow(S.now))
		return;
	if (req->version != Program_version)
	{
		/* Discovery: the requester knows no session yet. */
		deny_join(from, 0, kick_player_reason::version, 0);
		return;
	}
	net_udp_update_netgame();
	std::array<uint8_t, 4 + GAME_SETTINGS_FIXED_SIZE + 3 * 32 + PLAYER_LIST_SIZE> buf;
	send_unconnected(from, 0, 0, 0, session_msg::game_info, build_game_info(buf));
}

void handle_game_info_lite(const std::span<const uint8_t> payload, const _sockaddr &from)
{
	if (multi_i_am_master())
		return;
	if (auto g{parse_game_info_lite(payload, from)})
		net_udp_game_list_update(std::move(*g));
}

void handle_game_info(const std::span<const uint8_t> payload, const _sockaddr &from)
{
	/* Only the answer of the host that was asked, never while a join is
	 * under way or a connection to a host exists: the Netgame being joined
	 * (its session id and host address) must not be replaced by another
	 * game's description.  Not by Network_status, which a way out of a
	 * game could leave stale (net_v2::client_takes_game_info).
	 */
	const bool from_asked_host{S.info_addr && from == *S.info_addr};
	if (!::dcx::net_v2::client_takes_game_info(multi_i_am_master(), S.peers[0].conn.has_value(), S.join.active(), from_asked_host))
	{
		if (from_asked_host && !multi_i_am_master() && !S.info_drop_logged)
		{
			S.info_drop_logged = true;
			con_printf(CON_NORMAL, "net: GAME_INFO ignored (%s)", S.join.active() ? "a join is under way" : "still connected to a host");
		}
		return;
	}
	reader r{payload};
	const auto session_id{r.u32()};
	read_game_settings(r);
	read_player_list(r);
	if (!r.done() || !session_id)
	{
		con_printf(CON_VERBOSE, "net: malformed GAME_INFO (%zu bytes) ignored", payload.size());
		return;
	}
	if (Netgame.protocol.udp.valid != 1 || Netgame.protocol.udp.session_id != session_id)
	{
		_sockaddr::presentation_buffer dbuf;
		con_printf(CON_NORMAL, "net: game info from %s:%hu: session %08x, %s, %u/%u players", dxx_ntop(from, dbuf), dxx_sockaddr_port(from), session_id, network_state_name(Netgame.game_status), static_cast<unsigned>(Netgame.numplayers), static_cast<unsigned>(Netgame.max_numplayers));
	}
	Netgame.players[0].protocol.udp.addr = from;
	Netgame.protocol.udp.session_id = session_id;
	Netgame.protocol.udp.program_iver[0] = Program_version.major;
	Netgame.protocol.udp.program_iver[1] = Program_version.minor;
	Netgame.protocol.udp.program_iver[2] = Program_version.micro;
	Netgame.protocol.udp.program_iver[3] = MULTI_PROTO_VERSION;
	/* The slot with the local callsign, for a rejoin into a closed game. */
	Netgame.protocol.udp.your_index = MULTI_PNUM_UNDEF;
	for (auto &&[i, np] : enumerate(Netgame.players))
		if (np.callsign == InterfaceUniqueState.PilotName)
		{
			Netgame.protocol.udp.your_index = i;
			break;
		}
	Netgame.protocol.udp.valid = 1; // This game is valid! YAY!
}

/* Section 3.7 step 4: a datagram with flags.UNCONNECTED */
void handle_unconnected(const std::span<const uint8_t> datagram, const _sockaddr &from)
{
	const auto m{::dcx::net_v2::parse_unconnected(datagram, S.session_id)};
	if (m.status != ::dcx::net_v2::unconnected_status::accepted)
	{
		++S.rejected_datagrams;
		return;
	}
	switch (m.type)
	{
		case session_msg::game_info_lite_req:
			handle_game_info_lite_req(m.payload, from);
			break;
		case session_msg::game_info_req:
			handle_game_info_req(m.payload, from);
			break;
		case session_msg::game_info_lite:
			handle_game_info_lite(m.payload, from);
			break;
		case session_msg::game_info:
			handle_game_info(m.payload, from);
			break;
		case session_msg::join_request:
			if (m.header.session_id == S.session_id)
				handle_join_request(m.payload, from);
			break;
		case session_msg::join_accept:
			if (m.header.session_id == S.session_id)
				handle_join_accept(m.header, m.payload, from);
			break;
		case session_msg::join_deny:
			handle_join_deny(m.payload, from);
			break;
		default:
			++S.rejected_datagrams;
			break;
	}
}

/* Reliable session messages on a connection */

void handle_reliable(peer &p, const session_msg type, const std::span<const uint8_t> payload)
{
	const auto slot{peer_slot(p)};
	if (is_object_message(type))
	{
		receive_object_message(p, type, payload);
		return;
	}
	/* Host-assigned spawns (net_spawn.cpp): a request only from a client
	 * in the game; an answer also while this client still waits for
	 * LEVEL_GO (the site of its join in progress comes just before it).
	 */
	/* Stage 4 (net_combat.cpp): gated like the object messages; the host
	 * relays or answers them itself.
	 */
	switch (type)
	{
		case session_msg::fire:
		case session_msg::weapon_hit:
		case session_msg::damage:
		case session_msg::player_killed:
		case session_msg::player_spawn:
			if (peer_sends_game_data(p) && legacy_processing_allowed())
				net_combat_receive(slot, static_cast<uint8_t>(type), payload);
			return;
		default:
			break;
	}
	/* Game modes (net_modes.cpp): host to all, gated like the combat
	 * messages.
	 */
	if (type == session_msg::capture || type == session_msg::orb_bonus || type == session_msg::ctf_notice)
	{
		if (!multi_i_am_master() && peer_sends_game_data(p) && legacy_processing_allowed())
			net_modes_receive(slot, static_cast<uint8_t>(type), payload);
		return;
	}
	/* Mission transfer (net_mission.cpp): the manifest and the asset
	 * messages of kind 3, in any phase (a joining client downloads before
	 * it loads the level).
	 */
	if (type == session_msg::mission_manifest || ::dcx::net_v2::is_mission_asset_message(static_cast<uint8_t>(type), payload))
	{
		net_mission_receive(slot, static_cast<uint8_t>(type), payload);
		return;
	}
	/* Custom ships (net_ships.cpp): any phase; the exchange checks who
	 * may send what.
	 */
	if (type == session_msg::ship_info || type == session_msg::asset_request || type == session_msg::asset_data || type == session_msg::asset_unavailable)
	{
		net_ships_receive(slot, static_cast<uint8_t>(type), payload);
		return;
	}
	/* Taunts (taunt.cpp): a request to the host, the relay to the
	 * clients; only in the level.
	 */
	if (type == session_msg::taunt_request || type == session_msg::taunt)
	{
#if DXX_BUILD_DESCENT == 2
		if (peer_sends_game_data(p) && event_processing_allowed())
			net_taunt_receive(slot, static_cast<uint8_t>(type), payload);
#endif
		return;
	}
	if (type == session_msg::spawn_request || type == session_msg::spawn_site)
	{
		if (multi_i_am_master() ? peer_sends_game_data(p) : legacy_processing_allowed())
			net_spawn_receive(slot, static_cast<uint8_t>(type), payload);
		return;
	}
	if (multi_i_am_master())
	{
		switch (type)
		{
			case session_msg::legacy_mdata:
				receive_legacy_mdata(p, payload);
				break;
			case session_msg::level_ready:
				handle_level_ready(p, payload);
				break;
			case session_msg::client_ready:
				handle_client_ready(p);
				break;
			case session_msg::leave:
				{
					const auto why{payload.size() == 1 ? build_kick_player_reason_from_untrusted(payload[0]) : std::nullopt};
					con_printf(CON_NORMAL, "net: P#%u left (reason %u)", slot, payload.size() == 1 ? payload[0] : 0u);
					host_peer_gone(p, why ? *why : kick_player_reason::quit);
				}
				break;
			case session_msg::legacy_endlevel_client:
				if (peer_sends_game_data(p))
					receive_endlevel_client(slot, payload);
				break;
			case session_msg::address_seen:
				handle_address_seen(slot, payload);
				break;
			default:
				con_printf(CON_VERBOSE, "net: unexpected message type %u from P#%u", static_cast<unsigned>(type), slot);
				break;
		}
		return;
	}
	switch (type)
	{
		case session_msg::legacy_mdata:
			receive_legacy_mdata(p, payload);
			break;
		case session_msg::game_settings:
			{
				reader r{payload};
				read_game_settings(r);
				if (!r.done())
					con_printf(CON_VERBOSE, "net: malformed GAME_SETTINGS ignored");
			}
			break;
		case session_msg::player_list:
			{
				reader r{payload};
				read_player_list(r);
				if (!r.done())
					con_printf(CON_VERBOSE, "net: malformed PLAYER_LIST ignored");
			}
			break;
		case session_msg::player_joined:
			{
				if (payload.size() != PLAYER_JOINED_SIZE)
					break;
				reader r{payload};
				const playernum_t pnum{r.u8()};
				callsign_t callsign;
				if (const auto c{r.take(CALLSIGN_LEN + 1)})
					callsign.copy_lower(std::span<const char, CALLSIGN_LEN>(reinterpret_cast<const char *>(c), CALLSIGN_LEN));
				const auto rank{build_rank_from_untrusted(r.u8())};
				if (pnum >= Netgame.max_numplayers || pnum == Player_num)
					break;
				/* A human took the slot (bots section 2.3). */
				set_player_is_bot(pnum, false);
				new_player(pnum, callsign, rank);
			}
			break;
		case session_msg::player_left:
			if (payload.size() == 2 && payload[0] < MAX_PLAYERS && payload[0] != Player_num)
			{
				S.left_reason = build_kick_player_reason_from_untrusted(payload[1]).value_or(kick_player_reason::quit);
				multi_disconnect_player(payload[0]);
			}
			break;
		case session_msg::kick:
			if (const auto why{payload.size() == 1 ? build_kick_player_reason_from_untrusted(payload[0]) : std::nullopt})
				handle_kick(*why);
			break;
		case session_msg::host_shutdown:
			handle_host_lost(kick_player_reason::host_shutdown);
			break;
		case session_msg::level_start:
			{
				if (payload.size() != LEVEL_START_SIZE || Network_status != network_state::waiting)
					break;
				reader r{payload};
				r.i32();	/* levelnum: the level we loaded */
				r.u32();	/* tick: stage 2 */
				const auto seed{r.u32()};
				per_player_array<uint32_t> locations;
				for (auto &i : locations)
					i = r.u8();
				const auto level_time{static_cast<fix>(r.u32())};
				const auto control_invul_time{r.i32()};
				const auto flags{r.u8()};
				if (!r.done())
					break;
				if (seed)
					Netgame.ShufflePowerupSeed = seed;
				Netgame.locations = locations;
				Netgame.level_time = level_time;
				Netgame.control_invul_time = control_invul_time;
				if (flags & 1)
				{
					Network_rejoined = 1;
					/* A snapshot follows: a player late for a level start
					 * learns only here that it joins in progress.
					 */
					if (!S.sync_wait.active())
						S.sync_wait.begin(S.now);
				}
			}
			break;
		case session_msg::snapshot_begin:
			if (Network_status == network_state::waiting)
				apply_snapshot_begin(payload);
			break;
		case session_msg::snapshot_objects:
			if (Network_status == network_state::waiting)
				apply_snapshot_objects(payload);
			break;
		case session_msg::snapshot_game:
			if (Network_status == network_state::waiting)
			{
				S.snapshot_crc = ::dcx::net_v2::crc32_update(S.snapshot_crc, payload);
				++S.snapshot_parts;
				if (!apply_snapshot_game(payload))
					snapshot_failed();
			}
			break;
		case session_msg::snapshot_end:
			if (Network_status == network_state::waiting)
				apply_snapshot_end(payload);
			break;
		case session_msg::level_go:
			if (payload.size() == LEVEL_GO_SIZE && Network_status == network_state::waiting)
				apply_level_go_internal();
			break;
		case session_msg::legacy_endlevel_host:
			if (peer_sends_game_data(p))
				receive_endlevel_host(payload);
			break;
		default:
			con_printf(CON_VERBOSE, "net: unexpected message type %u from the host", static_cast<unsigned>(type));
			break;
	}
}

void handle_unreliable(peer &p, const ::dcx::net_v2::unreliable_view &u)
{
	switch (u.type)
	{
		case chunk_type::state:
			if (!multi_i_am_master() && peer_sends_game_data(p))
				apply_state(p, u.payload);
			break;
		case chunk_type::input:
			if (multi_i_am_master() && peer_sends_game_data(p))
				apply_input(p, u.payload);
			break;
		case chunk_type::event_u:
			receive_event(p, u.payload);
			break;
		case chunk_type::reliable:
		case chunk_type::session:
			break;
	}
}

/* Every datagram read from a socket */
void receive_datagram(const std::span<const uint8_t> datagram, const _sockaddr &from)
{
	if (datagram.empty())
		return;
#if DXX_USE_TRACKER
	if (receive_tracker_datagram(datagram, from))
		return;
#endif
	if (datagram.size() < ::dcx::net_v2::NET_V2_HEADER_SIZE || datagram.size() > NET_V2_MAX_PACKET)
	{
		++S.rejected_datagrams;
		return;
	}
	const auto h{packet_header::read(datagram)};
	if (!h || h->proto != ::dcx::net_v2::NET_V2_PROTO_VERSION)
	{
		/* A v1 build, or noise. */
		++S.rejected_datagrams;
		return;
	}
	if (h->has_flag(::dcx::net_v2::packet_flag::unconnected))
	{
		handle_unconnected(datagram, from);
		return;
	}
	peer *const p{multi_i_am_master() ? find_peer_by_token(h->peer_token) : (S.peers[0].conn && S.peers[0].token == h->peer_token ? &S.peers[0] : nullptr)};
	if (!p || !p->conn || h->session_id != S.session_id)
	{
		++S.rejected_datagrams;
		return;
	}
	const auto report{p->conn->on_receive(datagram, S.now)};
	if (report.status != receive_status::accepted)
	{
		++S.rejected_datagrams;
		return;
	}
	if (from != p->addr)
	{
		/* Section 3.6: a valid packet from a new address (NAT rebinding). */
		_sockaddr::presentation_buffer dbuf;
		con_printf(CON_NORMAL, "net: P#%u now at %s:%hu", peer_slot(*p), dxx_ntop(from, dbuf), dxx_sockaddr_port(from));
		p->addr = from;
		Netgame.players[peer_slot(*p)].protocol.udp.addr = from;
	}
	/* A client leaving (its LEAVE lingers for the acknowledgement) takes
	 * nothing more from the host: a LEVEL_GO arriving now would start the
	 * level it is giving up.
	 */
	if (!multi_i_am_master() && p->ph == peer::phase::closing)
		return;
	/* The report's views live in the connection and the datagram buffer;
	 * a handler may drop the peer or nest a menu, so copy first.
	 */
	std::vector<std::pair<session_msg, std::vector<uint8_t>>> reliable;
	reliable.reserve(report.reliable.size());
	for (auto &m : report.reliable)
		reliable.emplace_back(static_cast<session_msg>(m.type), std::vector<uint8_t>(m.payload.begin(), m.payload.end()));
	std::vector<std::pair<chunk_type, std::vector<uint8_t>>> unreliable;
	unreliable.reserve(report.unreliable.size());
	for (auto &u : report.unreliable)
		unreliable.emplace_back(u.type, std::vector<uint8_t>(u.payload.begin(), u.payload.end()));
	for (auto &[type, payload] : reliable)
	{
		if (!p->conn)
			return;
		handle_reliable(*p, type, payload);
	}
	for (auto &[type, payload] : unreliable)
	{
		if (!p->conn)
			return;
		handle_unreliable(*p, ::dcx::net_v2::unreliable_view{.type = type, .payload = payload});
	}
}

void read_sockets()
{
	std::array<uint8_t, 1500> packet;
	for (auto &sock : UDP_Socket)
	{
		if (!sock)
			continue;
		for (;;)
		{
			_sockaddr sender_addr{};
			const auto size = udp_receive_packet(sock, packet, sender_addr);
			if (!(size > 0))
				break;
			receive_datagram(std::span<const uint8_t>(packet).first(static_cast<std::size_t>(size)), sender_addr);
		}
	}
}

/* The frame: sockets in, connections ticked, packets out, timeouts. */

void client_join_frame()
{
	if (!S.join.active())
		return;
	if (S.join.timed_out(S.now))
	{
		S.join.end();
		S.join_result = join_status::timed_out;
		_sockaddr::presentation_buffer dbuf;
		con_printf(CON_NORMAL, "net: no answer to the join request: %u requests to %s:%hu (session %08x) in %u s", S.join_requests_sent, dxx_ntop(S.join_addr, dbuf), dxx_sockaddr_port(S.join_addr), S.session_id, net_clock_to_ms(::dcx::net_v2::NET_V2_JOIN_TIMEOUT) / 1000);
		return;
	}
#if DXX_USE_TRACKER
	/* No move to a hole-punch answer here: the game info came from
	 * S.join_addr (net_udp_game_connect already followed the answer), so
	 * the host is known to answer there.  A late answer from another
	 * address (the host's public address seen from its own LAN, or a
	 * stray datagram) would split the join across two addresses.
	 */
	if (S.join_tracker_id != tracker_game_id{} && S.join.holepunch_due(S.now))
		udp_tracker_request_holepunch(S.join_tracker_id);
#endif
	if (!S.join.due(S.now))
		return;
	::dcx::net_v2::join_request req{
		.game_id = Game_id,
		.version = Program_version,
		.client_nonce = S.join.nonce(),
		.rank = underlying_value(GetMyNetRanking()),
		.current_level = Current_level_num,
		.client_time = ::dcx::net_v2::to_net_time(S.now),
	};
	memcpy(req.callsign.data(), InterfaceUniqueState.PilotName.operator const char *(), CALLSIGN_LEN + 1);
	std::array<uint8_t, ::dcx::net_v2::NET_V2_JOIN_REQUEST_SIZE> buf;
	req.write(buf.data());
	send_unconnected(S.join_addr, S.session_id, 0, NET_V2_PLAYER_ID_NONE, session_msg::join_request, buf);
	++S.join_requests_sent;
}

/* The per-connection statistics of connection::stats() on the console
 * (Documentation/netv2-transport.md, "What the stats mean"), and the
 * host's smoothed round trip as the player's ping (section 3.5).
 */
void report_stats(peer &p)
{
	const auto slot{peer_slot(p)};
	const auto stats{p.conn->stats()};
	if (multi_i_am_master())
		Netgame.players[slot].ping = stats.rtt_valid ? static_cast<fix>(net_clock_to_ms(stats.srtt)) : 0;
	if (S.now < p.next_stats)
		return;
	p.next_stats = S.now + STATS_INTERVAL;
	con_printf(CON_NORMAL, "net P#%u: rtt %u ms (var %u) rto %u ms loss %.1f%% | pkts sent %llu recv %llu rejected %llu lost %llu | msgs sent %llu resent %llu (gap %llu rto %llu) delivered %llu | queue %zu msgs %zu B in flight %zu held %zu | events dropped %llu proto errors %llu%s",
		slot,
		stats.rtt_valid ? net_clock_to_ms(stats.srtt) : 0u,
		stats.rtt_valid ? net_clock_to_ms(stats.rttvar) : 0u,
		net_clock_to_ms(stats.rto),
		stats.loss_estimate * 100.0,
		static_cast<unsigned long long>(stats.packets_sent),
		static_cast<unsigned long long>(stats.packets_received),
		static_cast<unsigned long long>(stats.packets_rejected),
		static_cast<unsigned long long>(stats.packets_lost),
		static_cast<unsigned long long>(stats.message_sends),
		static_cast<unsigned long long>(stats.message_resends),
		static_cast<unsigned long long>(stats.resends_by_gap),
		static_cast<unsigned long long>(stats.resends_by_rto),
		static_cast<unsigned long long>(stats.messages_delivered),
		stats.queue_messages,
		stats.queue_bytes,
		stats.in_flight,
		stats.recv_window_pending,
		static_cast<unsigned long long>(stats.unreliable_dropped),
		static_cast<unsigned long long>(stats.protocol_errors),
		p.backlog.empty() ? "" : " (backlog)");
	if (S.rejected_datagrams)
	{
		con_printf(CON_NORMAL, "net: %u datagrams rejected before or by the transport", S.rejected_datagrams);
		S.rejected_datagrams = 0;
	}
}

/* A connection closed by the transport (timeout, unacknowledged data,
 * overflow, protocol errors).
 */
void handle_closed_connection(peer &p)
{
	const auto slot{peer_slot(p)};
	const auto why{p.conn->closed_because()};
	const auto st{p.conn->stats()};
	const auto &cfg{p.conn->config()};
	/* Always in the game log, with the timings: why a player was dropped
	 * is otherwise not known after the fact.
	 */
	con_printf(CON_NORMAL, "net: connection to P#%u closed: %s (last packet %u ms ago; limits %u ms silent, %u ms unacknowledged; rtt %u ms, loss %.1f%%, %llu packets received, queue %zu msgs; %s)",
		slot, close_reason_name(why),
		net_clock_to_ms(S.now - st.last_heard),
		net_clock_to_ms(cfg.timeout), net_clock_to_ms(cfg.unacked_timeout),
		st.rtt_valid ? net_clock_to_ms(st.srtt) : 0u, st.loss_estimate * 100.0,
		static_cast<unsigned long long>(st.packets_received), st.queue_messages,
		network_state_name(Network_status));
	if (multi_i_am_master())
		host_peer_gone(p, kick_reason_from_close(why));
	else
		handle_host_lost(kick_reason_from_close(why), net_clock_to_ms(S.now - st.last_heard));
}

void frame(const bool listen)
{
	if (S.in_frame)
		/* A menu nested inside a message handler polls too; the outer
		 * frame finishes first.
		 */
		return;
	if (!UDP_Socket[0])
		return;
	S.in_frame = true;
	S.now = timer_update();
	S.last_frame = S.now;

	/* Section 2.3: the session's tick counter, and the clock the
	 * interpolation shows the remote objects by (the host's own; on a
	 * client, its estimate of the host's).
	 */
	const auto tick_rate{local_tick_period()};
	S.tick.set_rate(static_cast<unsigned>(tick_rate.denominator));
	S.tick.advance(S.now);
	if (multi_i_am_master())
		interp::set_clock(true, 0, tick_rate.units());
	else if (const auto &hc{S.peers[0].conn})
		interp::set_clock(hc->clock_valid(), hc->clock_offset(), tick_rate.units());
	else
		interp::set_clock(false, 0, tick_rate.units());

	if (listen)
		read_sockets();

	client_join_frame();
	/* Custom ships: announcements, the bots' ships, paced transfers. */
	net_ships_frame();
	/* The mission transfer: paced sending, the client's timeouts. */
	net_mission_frame();

	/* Each connection paces its own packets (connection::begin_tick); a
	 * connection's tick carries the newest state.  The host builds the
	 * common part of the bundle once per frame at most.
	 */
	std::optional<state_bundle> common_bundle;
	const bool send_state{state_sending_allowed()};
	for (auto &p : S.peers)
	{
		if (!p.conn)
			continue;
		auto &c = *p.conn;
		apply_timeouts(c, p.ph);
		if (c.begin_tick(S.now))
		{
			flush_events();
			if (send_state && (p.ph == peer::phase::playing || p.ph == peer::phase::syncing))
			{
				if (multi_i_am_master())
				{
					if (!common_bundle)
						build_common_bundle(common_bundle.emplace());
					set_state_for_peer(p, *common_bundle);
				}
				else
					set_input_for_host(p);
			}
		}
		pump_peer(p);
		for (;;)
		{
			const auto packet{c.build_outgoing(S.now)};
			if (packet.empty())
				break;
			send_raw(packet, p.addr);
		}
		if (c.state() == connection_state::closed)
		{
			if (p.ph == peer::phase::closing)
				drop_peer(p);
			else
				handle_closed_connection(p);
			continue;
		}
		report_stats(p);
		/* A joining player that downloads the mission is not stalled:
		 * its join time counts from the end of the download.
		 */
		if (multi_i_am_master() && net_mission_host_busy(peer_slot(p)))
			p.phase_since = S.now;
		if (multi_i_am_master() && ::dcx::net_v2::join_stalled(p.ph, p.phase_since, S.now))
		{
			/* Section 4.4: a join in progress that does not finish.  The
			 * client may be alive (its connection keeps up) but stuck;
			 * the joins are serialised, so it must not block the others
			 * for ever.
			 */
			con_printf(CON_NORMAL, "net: P#%u did not complete its join in %u s; removed", peer_slot(p), static_cast<unsigned>(::dcx::net_v2::NET_V2_JOIN_SYNC_TIMEOUT / F1_0));
			kick_peer(p, kick_player_reason::snapshot_failed);
		}
		if (p.ph == peer::phase::closing && S.now >= p.close_at)
		{
			if (multi_i_am_master())
				host_peer_gone(p, kick_player_reason::kicked);
			else
				drop_peer(p);
		}
	}

	if (multi_i_am_master() && Network_status != network_state::menu && Network_status != network_state::browsing)
	{
		if (WaitForRefuseAnswer && S.now > RefuseTimeLimit + (F1_0 * 12))
			WaitForRefuseAnswer = 0;
		// broadcast lite_info every 10 seconds
		if (S.now >= S.last_broadcast + GAME_INFO_BROADCAST_INTERVAL)
		{
			S.last_broadcast = S.now;
			host_broadcast_game_info_lite();
#if DXX_USE_TRACKER
			if (Netgame.Tracker)
				tracker_register();
#endif
		}
		if (Network_sending_extras && Network_status == network_state::playing)
			send_extras();
	}
#if DXX_USE_TRACKER
	udp_tracker_verify_ack_timeout();
	tracker_list_summary();
#endif
	udp_traffic_stat();
	S.in_frame = false;
}

[[nodiscard]]
bool any_peer_closing()
{
	return std::ranges::any_of(S.peers, [](const peer &p) { return p.conn && p.ph == peer::phase::closing; });
}

/* Wait up to NET_V2_CLOSE_LINGER for the reliable messages still queued
 * (a LEAVE, KICK or HOST_SHUTDOWN on a closing connection) to be
 * acknowledged, driving the network meanwhile.  Not inside a frame.
 */
void linger_closing()
{
	if (S.in_frame || !UDP_Socket[0] || !any_peer_closing())
		return;
	const fix64 deadline{timer_query() + ::dcx::net_v2::NET_V2_CLOSE_LINGER};
	for (;;)
	{
		frame(true);
		bool pending{false};
		for (auto &p : S.peers)
			if (p.conn && p.conn->state() != connection_state::closed)
			{
				const auto stats{p.conn->stats()};
				if (stats.queue_messages || !p.backlog.empty())
					pending = true;
			}
		if (!pending || timer_query() >= deadline)
			break;
		timer_delay_ms(5);
	}
}

}

/* Public interface (net_v2_game.h) */

void probe_report()
{
	unsigned peers{0};
	std::size_t queue_messages{0}, queue_bytes{0}, in_flight{0}, recv_pending{0}, backlog{0}, backlog_bytes{0};
	for (const auto &p : S.peers)
	{
		if (!p.conn)
			continue;
		++peers;
		const auto st{p.conn->stats()};
		queue_messages += st.queue_messages;
		queue_bytes += st.queue_bytes;
		in_flight += st.in_flight;
		recv_pending += st.recv_window_pending;
		backlog += p.backlog.size();
		backlog_bytes += p.backlog_bytes;
	}
	con_printf(CON_VERBOSE, "session: net peers %u, reliable queue %zu msgs %zu B, in flight %zu, recv pending %zu, backlog %zu msgs %zu B, event buffer %zu B (capacity %zu), extras queue %zu",
		peers, queue_messages, queue_bytes, in_flight, recv_pending, backlog, backlog_bytes, S.event_buffer.size(), S.event_buffer.capacity(), S.extras_queue.size());
}

bool open_socket(const unsigned index, const uint16_t port)
{
	if (udp_open_socket(UDP_Socket[index], port) != 0)
		return false;
	event_background_task = &menu_pump;
	return true;
}

bool open_loopback_socket()
{
	if (udp_open_socket(UDP_Socket[0], 0, true) != 0)
		return false;
	event_background_task = &menu_pump;
	_sockaddr a{};
	socklen_t len = sizeof(a);
	if (getsockname(UDP_Socket[0], &a.sa, &len) == 0)
		con_printf(CON_NORMAL, "net: game socket on the loopback address only, port %u", ntohs(
#if DXX_USE_IPv6
			a.sin6.sin6_port
#else
			a.sin.sin_port
#endif
		));
	return true;
}

void close_sockets()
{
	/* A LEAVE, KICK or HOST_SHUTDOWN queued just before gets its second:
	 * in the lobby the peer would otherwise wait out the long lobby
	 * timeout.
	 */
	linger_closing();
	/* Whatever was queued last leaves with this frame. */
	frame(false);
	session_reset();
	UDP_Socket = {};
	event_background_task = nullptr;
}

void flush_sockets()
{
	for (auto &s : UDP_Socket)
		net_udp_flush(s);
}

bool socket_ready()
{
	return static_cast<bool>(UDP_Socket[0]);
}

void game_broadcast(const uint8_t type, const std::span<const uint8_t> payload, const playernum_t exclude)
{
	broadcast_reliable(static_cast<session_msg>(type), payload, exclude);
}

void game_send_to(const playernum_t slot, const uint8_t type, const std::span<const uint8_t> payload)
{
	send_to_slot(slot, static_cast<session_msg>(type), payload);
}

std::size_t game_queued_bytes(const playernum_t slot)
{
	if (slot >= MAX_PLAYERS)
		return 0;
	const auto &p{S.peers[slot]};
	return p.conn ? p.conn->stats().queue_bytes + p.backlog_bytes : 0;
}

game_link_counters game_link(const playernum_t slot)
{
	if (slot >= MAX_PLAYERS)
		return {};
	const auto &p{S.peers[slot]};
	if (!p.conn)
		return {};
	const auto s{p.conn->stats()};
	return {s.message_sends, s.message_resends, s.rtt_valid ? static_cast<double>(s.srtt) / F1_0 : 0.0};
}

void game_set_bulk(const playernum_t slot, const unsigned packets_per_tick)
{
	if (slot >= MAX_PLAYERS)
		return;
	auto &p{S.peers[slot]};
	p.pump_bytes = packets_per_tick >= ::dcx::net_v2::MISSION_BULK_PACKETS_PER_TICK ? ::dcx::net_v2::MISSION_WINDOW_LOBBY + ::dcx::net_v2::NET_V2_MAX_MESSAGE : BACKLOG_PUMP_BYTES;
	if (p.conn)
		p.conn->set_max_packets_per_tick(packets_per_tick ? packets_per_tick : ::dcx::net_v2::NET_V2_DEFAULT_MAX_PACKETS_PER_TICK);
	con_printf(CON_VERBOSE, "net: P#%u: %u packets per tick%s", slot, packets_per_tick ? packets_per_tick : ::dcx::net_v2::NET_V2_DEFAULT_MAX_PACKETS_PER_TICK, packets_per_tick ? " (mission transfer)" : "");
}

bool host_slot_is_client(const playernum_t slot)
{
	if (slot >= MAX_PLAYERS || !slot)
		return false;
	const auto &p{S.peers[slot]};
	return p.conn && p.ph != peer::phase::none && p.ph != peer::phase::closing;
}

void session_reset()
{
	con_printf(CON_VERBOSE, "teardown: network session reset");
	bots_session_reset();
	net_ships_reset();
	for (auto &p : S.peers)
		drop_peer(p);
	net_mission_reset();
	S.awaits_entry = {};
	S.session_id = 0;
	S.my_token = 0;
	S.inputs = {};
	S.tick = {};
	S.input_seq = 0;
	S.have_bundle = false;
	S.bundle_tick = 0;
	S.bundle_time = 0;
	net_interp_reset();
	S.event_buffer.clear();
	S.join.end();
	S.join_result = join_status::idle;
	S.sync_wait.end();
	S.info_addr.reset();
	S.extras_queue.clear();
	S.lite_limit.reset();
	S.info_limit.reset();
	S.join_limit.reset();
	S.in_frame = false;
	S.join_addr = {};
#if DXX_USE_TRACKER
	S.join_tracker_id = {};
#endif
	S.left_reason = kick_player_reason::timeout;
	S.snapshot_crc = 0;
	S.snapshot_objects = 0;
	S.snapshot_parts = 0;
	S.lobby_open = false;
	S.logged_join_nonce = 0;
	S.info_drop_logged = false;
	S.join_requests_sent = 0;
	S.host_lost.reset();
	/* Out of every session: back in the menus.  Some ways out of a game
	 * (the host lost or the wait cancelled before the level start) left
	 * the status at `waiting`, and the next join's GAME_INFO was then
	 * ignored until the program was restarted.  Whoever opens a session
	 * next sets its own status after this.
	 */
	Network_status = network_state::menu;
#if DXX_USE_TRACKER
	udp_tracker_init();
#endif
}

void poll()
{
	frame(true);
}

void request_game_list()
{
	const ::dcx::net_v2::game_info_request req{.game_id = Game_id, .version = Program_version};
	std::array<uint8_t, ::dcx::net_v2::NET_V2_GAME_INFO_REQ_SIZE> buf;
	req.write(buf.data());
	packet_buffer pkt;
	const auto dg{::dcx::net_v2::build_unconnected(pkt, 0, 0, NET_V2_PLAYER_ID_NONE, ::dcx::net_v2::to_net_time(timer_query()), session_msg::game_info_lite_req, buf)};
	if (UDP_Socket[0])
	{
		dxx_sendto(UDP_Socket[0], dg, 0, GBcast);
#if DXX_USE_IPv6
		dxx_sendto(UDP_Socket[0], dg, 0, GMcast_v6);
#endif
	}
}

void request_game_info(const _sockaddr &host)
{
	S.info_addr = host;
	const ::dcx::net_v2::game_info_request req{.game_id = Game_id, .version = Program_version};
	std::array<uint8_t, ::dcx::net_v2::NET_V2_GAME_INFO_REQ_SIZE> buf;
	req.write(buf.data());
	send_unconnected(host, 0, 0, NET_V2_PLAYER_ID_NONE, session_msg::game_info_req, buf);
}

void client_begin_join(const _sockaddr &host, const uint32_t session_id
#if DXX_USE_TRACKER
	, const tracker_game_id tracker_id
#endif
	)
{
	drop_peer(S.peers[0]);
	S.session_id = session_id;
	S.join_addr = host;
#if DXX_USE_TRACKER
	S.join_tracker_id = tracker_id;
#endif
	S.join_result = join_status::joining;
	S.join.begin(timer_query(), random_nonzero_u32());
	S.join_requests_sent = 0;
	S.host_lost.reset();
#if DXX_USE_TRACKER
	Holepunch_reply.reset();
#endif
	_sockaddr::presentation_buffer dbuf;
	con_printf(CON_NORMAL, "net: joining session %08x at %s:%hu (attempt %08x)", session_id, dxx_ntop(host, dbuf), dxx_sockaddr_port(host), S.join.nonce());
}

join_status client_join_status()
{
	return S.join_result;
}

void client_end_join()
{
	S.join.end();
	S.join_result = join_status::idle;
}

void client_cancel_join()
{
	con_puts(CON_NORMAL, "net: join cancelled by the player");
	S.join.end();
	S.join_result = join_status::idle;
	/* Accepted meanwhile: the host counts this client as joining (and
	 * serves no other join) until it hears that it left.
	 */
	client_send_leave(kick_player_reason::cancelled);
}

void client_send_level_ready()
{
	/* A game in progress answers with a snapshot, which is bounded; a
	 * fresh level start waits for the host and the other players.
	 */
	if (Netgame.game_status == network_state::playing)
		S.sync_wait.begin(timer_query());
	else
		S.sync_wait.end();
	std::array<uint8_t, LEVEL_READY_SIZE> buf;
	writer w{buf.data()};
	w.u32(static_cast<uint32_t>(Current_level_num));
	w.u16(my_segments_checksum);
	send_to_slot(0, session_msg::level_ready, buf);
}

bool client_sync_timed_out()
{
	return S.sync_wait.expired(timer_query());
}

std::optional<host_lost_info> client_take_host_lost()
{
	return std::exchange(S.host_lost, std::nullopt);
}

void menu_pump()
{
	/* Inside a frame (a message box a handler opened) the outer frame
	 * finishes first; in a level the game loop drives the network.
	 */
	if (S.in_frame || !UDP_Socket[0])
		return;
	/* The lobby and the level wait; or a KICK, LEAVE or HOST_SHUTDOWN
	 * still lingering after the game was given up (the host's setup menu
	 * after an aborted lobby polls nothing).
	 */
	if (Network_status != network_state::starting && Network_status != network_state::waiting && !any_peer_closing())
		return;
	/* A menu's polling handler ran it just now. */
	if (timer_query() - S.last_frame < MENU_PUMP_INTERVAL)
		return;
	frame(true);
}

void host_set_lobby_open(const bool open)
{
	S.lobby_open = open;
}

void client_send_leave(const kick_player_reason reason)
{
	auto &p = S.peers[0];
	if (!p.conn)
		return;
	const uint8_t why{underlying_value(reason)};
	peer_queue(p, session_msg::leave, std::span<const uint8_t>(&why, 1));
	p.ph = peer::phase::closing;
	p.close_at = timer_query() + ::dcx::net_v2::NET_V2_CLOSE_LINGER;
}

void host_open_session(const uint32_t fixed_id)
{
	session_reset();
	S.session_id = fixed_id ? fixed_id : random_nonzero_u32();
	Netgame.protocol.udp.session_id = S.session_id;
	S.last_broadcast = 0;
#if DXX_USE_TRACKER
	tracker_reset_host_state();
#endif
	Public_tally.clear();
	Public_logged.clear();
	con_printf(CON_NORMAL, "net: hosting session %08x at %u Hz", S.session_id, Netgame.TickRate);
	net_ships_start(true, 0);
	net_mission_host_start();
}

void host_broadcast_game_info_lite()
{
	/* -botarena: nobody to tell. */
	if (!UDP_Socket[0] || CGameArg.DbgBotArenaSeconds)
		return;
	net_udp_update_netgame();
	std::array<uint8_t, 128> buf;
	packet_buffer pkt;
	const auto dg{::dcx::net_v2::build_unconnected(pkt, 0, 0, 0, ::dcx::net_v2::to_net_time(timer_query()), session_msg::game_info_lite, build_game_info_lite(buf))};
	dxx_sendto(UDP_Socket[0], dg, 0, GBcast);
#if DXX_USE_IPv6
	dxx_sendto(UDP_Socket[0], dg, 0, GMcast_v6);
#endif
}

void host_send_netgame_update()
{
	net_udp_update_netgame();
	for (auto &&[i, p] : enumerate(S.peers))
	{
		if (!i || !p.conn || p.ph == peer::phase::closing)
			continue;
		send_game_settings(p);
		send_player_list(p);
	}
	host_broadcast_game_info_lite();
}

void host_begin_level_wait()
{
	for (auto &&[i, p] : enumerate(S.peers))
	{
		if (!i)
			continue;
		/* Nobody is in the new level before its LEVEL_GO. */
		p.in_level = false;
		auto &plr = *vmplayerptr(static_cast<playernum_t>(i));
		if (plr.connected == player_connection_status::disconnected)
			continue;
		/* A bot is ready at once (Documentation/multiplayer-bots.md
		 * section 2.3): it has no peer to report LEVEL_READY.
		 */
		if (bot_is_local(static_cast<playernum_t>(i)))
		{
			plr.connected = player_connection_status::playing;
			continue;
		}
		if (p.conn && p.has_ready && p.ready_level == Current_level_num)
		{
			if (p.ready_checksum != my_segments_checksum)
			{
				kick_peer(p, kick_player_reason::checksum);
				continue;
			}
			plr.connected = player_connection_status::playing;
		}
		else
			plr.connected = player_connection_status::waiting;
	}
}

void host_send_level_start()
{
	net_udp_update_netgame();
	fill_netgame_scores();
	std::array<uint8_t, LEVEL_START_SIZE> start;
	{
		writer w{start.data()};
		w.u32(static_cast<uint32_t>(Current_level_num));
		w.u32(0);
		w.u32(+(Game_mode & GM_MULTI_COOP) ? 0 : Netgame.ShufflePowerupSeed);
		for (auto &i : Netgame.locations)
			w.u8(static_cast<uint8_t>(i));
		w.u32(0);	/* level_time: a fresh level */
		w.u32(static_cast<uint32_t>(Netgame.control_invul_time));
		w.u8(0);
	}
	std::array<uint8_t, SNAPSHOT_GAME_SIZE> game;
	(void)build_snapshot_game(game, 0, false);
	std::array<uint8_t, LEVEL_GO_SIZE> go;
	{
		writer w{go.data()};
		w.u32(0);
		w.u32(::dcx::net_v2::to_net_time(timer_query()));
	}
	for (auto &&[i, p] : enumerate(S.peers))
	{
		if (!i || !p.conn || p.ph != peer::phase::playing || vcplayerptr(static_cast<playernum_t>(i))->connected != player_connection_status::playing)
			continue;
		p.has_ready = false;
		p.in_level = true;
		send_game_settings(p);
		send_player_list(p);
		peer_queue(p, session_msg::level_start, start);
		peer_queue(p, session_msg::snapshot_game, game);
		peer_queue(p, session_msg::level_go, go);
	}
}

void host_kick_all(const kick_player_reason reason)
{
	S.now = timer_query();
	for (auto &&[i, p] : enumerate(S.peers))
		if (i && p.conn && p.ph != peer::phase::closing)
			kick_peer(p, reason);
}

void host_end_level()
{
	S.now = timer_query();
	for (auto &&[i, p] : enumerate(S.peers))
	{
		if (!i || !p.conn || p.ph == peer::phase::closing || p.ph == peer::phase::none || p.in_level)
			continue;
		/* Accepted into this level (or late for its start) but not in it
		 * yet: it is loading or syncing a level that is over, and the
		 * next level start does not reach it.  Tell it the game is
		 * between levels; it can join the next level.
		 */
		const auto slot{static_cast<playernum_t>(i)};
		con_printf(CON_NORMAL, "net: P#%u was still joining at the level end; removed", slot);
		kick_peer(p, kick_player_reason::endlevel);
		if (vcplayerptr(slot)->connected != player_connection_status::disconnected)
		{
			S.left_reason = kick_player_reason::endlevel;
			multi_disconnect_player(slot);
		}
	}
	cancel_extras();
}

bool host_slot_has_peer(const playernum_t slot)
{
	return slot < MAX_PLAYERS && S.peers[slot].ph != peer::phase::none;
}

bool recording_clock(uint32_t &session_id, std::int64_t &host_clock)
{
	session_id = S.session_id;
	const net_clock now{timer_query()};
	host_clock = now;
	if (multi_i_am_master())
		return session_id != 0;
	if (const auto &hc{S.peers[0].conn}; hc && hc->clock_valid())
	{
		host_clock = now + hc->clock_offset();
		return true;
	}
	return false;
}

int host_input_afterburner(const playernum_t slot)
{
	if (slot >= MAX_PLAYERS || slot == Player_num)
		return -1;
	const auto &st{S.inputs[slot]};
	if (!st.valid)
		return -1;
	return st.input.has_flag(input_flag::afterburner) ? 1 : 0;
}

bool host_input_controls(const playernum_t slot, std::array<std::int8_t, 6> &controls)
{
	if (!multi_i_am_master() || slot >= MAX_PLAYERS || slot == Player_num)
		return false;
	const auto &st{S.inputs[slot]};
	/* A gap in the client's INPUTs: the analysis estimates rather than
	 * repeat old controls.
	 */
	if (!st.valid || !st.input.controls || S.now - st.arrival > HOST_INPUT_CONTROLS_MAX_AGE)
		return false;
	controls = *st.input.controls;
	return true;
}

bool host_join_in_progress()
{
	return multi_i_am_master() && join_in_progress(_sockaddr{});
}

std::optional<playernum_t> host_free_slot_for_bot()
{
	if (!multi_i_am_master())
		return std::nullopt;
	/* The rule of a joining human who finds room (section 4.2): a free
	 * slot below the player limit, else a departed bot's.  A slot with a
	 * connection (a join under way, a linger) counts as taken; a
	 * disconnected human's slot is kept for their return.
	 */
	const auto views{build_slot_views(callsign_t{}, _sockaddr{})};
	const auto s{::dcx::net_v2::free_admission_slot(views, std::min<unsigned>(Netgame.max_numplayers, MAX_PLAYERS))};
	if (!s || !*s || *s >= MAX_PLAYERS || host_slot_has_peer(static_cast<playernum_t>(*s)))
		return std::nullopt;
	return static_cast<playernum_t>(*s);
}

void host_add_player(const playernum_t slot)
{
	if (!multi_i_am_master() || !slot || slot >= MAX_PLAYERS)
		return;
	S.now = timer_query();
	drop_extras_for(slot);
	net_objects_host_join(slot);
	net_combat_host_join(slot);
	S.awaits_entry[slot] = false;
	new_player(slot, Netgame.players[slot].callsign, Netgame.players[slot].rank);
	Netgame.players[slot].connected = player_connection_status::playing;
	Netgame.players[slot].LastPacketTime = S.now;
	/* PLAYER_JOINED clears the slot's bot flag on the clients (a human
	 * took it); the player list behind it sets it again.
	 */
	broadcast_player_joined(slot, MAX_PLAYERS);
	host_send_player_list();
}

void host_send_player_list()
{
	if (!multi_i_am_master())
		return;
	/* The list's `connected` bytes as the game has them now. */
	net_udp_update_netgame();
	/* A peer still joining too (it has its first list from its
	 * JOIN_ACCEPT, and loads the level now): a bot renamed meanwhile
	 * must not keep its old name there.  Its snapshot brings the list
	 * once more; the reliable stream keeps them in order.
	 */
	for (auto &&[i, p] : enumerate(S.peers))
		if (i && p.conn && (p.ph == peer::phase::joining || peer_receives_broadcasts(p)))
			send_player_list(p);
}

void host_remove_player(const playernum_t slot, const kick_player_reason why)
{
	if (!multi_i_am_master() || !slot || slot >= MAX_PLAYERS || vcplayerptr(slot)->connected == player_connection_status::disconnected)
		return;
	S.now = timer_query();
	S.left_reason = why;
	multi_disconnect_player(slot);
	if (Network_status == network_state::starting)
	{
		vacate_slot(slot);
		host_send_netgame_update();
	}
}

void apply_level_go()
{
	apply_level_go_internal();
}

#if DXX_USE_TRACKER
void tracker_register()
{
	if (!UDP_Socket[0])
		return;
	net_udp_update_netgame();
	std::array<uint8_t, 128> lite;
	packet_buffer pkt;
	const auto dg{::dcx::net_v2::build_unconnected(pkt, 0, 0, 0, ::dcx::net_v2::to_net_time(timer_query()), session_msg::game_info_lite, build_game_info_lite(lite))};
	std::array<uint8_t, 1 + sizeof("b=") + 4 + sizeof("00000.00000.00000.00000,z=") + NET_V2_MAX_PACKET> pBuf{};
	pBuf[0] = UPID_TRACKER_REGISTER;
	std::size_t len{1};
	len += snprintf(reinterpret_cast<char *>(&pBuf[1]), pBuf.size() - 1, "b=%c%c%c%c" DXX_VERSION_STR ".%hu,z=", Game_id[0], Game_id[1], Game_id[2], Game_id[3], MULTI_PROTO_VERSION);
	memcpy(&pBuf[len], dg.data(), dg.size());
	len += dg.size();
	if (!Tracker_registrations++)
	{
		/* The wait for the tracker's answer starts with the first
		 * registration of the game.
		 */
		TrackerAckTime = timer_query();
		con_printf(CON_NORMAL, "[Tracker] Registering the game with tracker %s:%u (%s) as \"%c%c%c%c" DXX_VERSION_STR ".%hu\"; refreshed every %u s", CGameArg.MplTrackerAddr.c_str(), CGameArg.MplTrackerPort, Tracker_resolved ? sockaddr_text(TrackerSocket).c_str() : "name not resolved", Game_id[0], Game_id[1], Game_id[2], Game_id[3], MULTI_PROTO_VERSION, static_cast<unsigned>(GAME_INFO_BROADCAST_INTERVAL / F1_0));
	}
	else
		con_printf(CON_VERBOSE, "[Tracker] Refreshing the game's registration (%u)", Tracker_registrations);
	tracker_send(std::span<const uint8_t>(pBuf).first(len), "the game's registration");
	/* Find this game's entry in the tracker's list: the address the
	 * tracker sees it at.  Until found (and as a check that the tracker
	 * has it when no ACK came), then now and then (the router may change
	 * the address).
	 */
	if (Tracker_acked && (!Tracker_listed || !(Tracker_registrations % TRACKER_SELF_CHECK_REGISTRATIONS)))
		tracker_send_request_games();
	else if (!Tracker_acked && Tracker_registrations > 1)
		tracker_send_request_games();
}

void tracker_unregister()
{
	const std::array<uint8_t, 1> pBuf{{UPID_TRACKER_REMOVE}};
	con_puts(CON_NORMAL, "[Tracker] Removing the game from the tracker.");
	tracker_send(pBuf, "the game's removal");
}

void tracker_request_holepunch(const tracker_game_id id)
{
	udp_tracker_request_holepunch(id);
}

void tracker_request_games()
{
	con_printf(CON_NORMAL, "[Tracker] Requesting the game list from tracker %s:%u (%s)", CGameArg.MplTrackerAddr.c_str(), CGameArg.MplTrackerPort, Tracker_resolved ? sockaddr_text(TrackerSocket).c_str() : "name not resolved");
	Tracker_list_requested = timer_query();
	Tracker_list_pending = true;
	Tracker_list_ids.clear();
	Holepunch_reply.reset();
	tracker_send_request_games();
}

std::optional<_sockaddr> tracker_take_holepunch_reply()
{
	return std::exchange(Holepunch_reply, std::nullopt);
}
#endif

std::optional<public_address_view> host_public_address()
{
	if (!multi_i_am_master())
		return std::nullopt;
	const auto b{Public_tally.best()};
	if (!b)
		return std::nullopt;
	return public_address_view{::dcx::seen_address_text(b->address), ::dcx::public_address_label(b->players, b->tracker)};
}

}

/* multi::dispatch (multi.h): the gameplay layer's view of the network. */

namespace multi {
namespace udp {

const dispatch_table dispatch{};

void dispatch_table::send_data(const std::span<const uint8_t> buf, const multiplayer_data_priority priority, const playernum_t originator) const
{
	assert(Game_mode & GM_MULTI);
	if (!(Game_mode & GM_NETWORK) || !UDP_Socket[0] || buf.empty())
		return;
#if DXX_HAVE_POISON_VALGRIND
	DXX_CHECK_MEM_IS_DEFINED(buf);
#endif
	if (net_v2::legacy_record_is_event(buf[0], priority))
	{
		net_v2::queue_event_record(buf, originator);
		if (priority != multiplayer_data_priority::_0)
			net_v2::flush_events();
		return;
	}
	/* Reliable.  As in v1, records still waiting in the buffer travel with
	 * it: a MULTI_POSITION queued right before an important record rides
	 * along, reliably.
	 */
	auto &pending = net_v2::S.event_buffer;
	/* Pending events of another originator go on their own, as events. */
	if (pending.size() > 2 && pending[1] != originator)
		net_v2::flush_events();
	std::vector<uint8_t> records;
	records.reserve(pending.size() + buf.size());
	if (pending.size() > 2)
		records.insert(records.end(), pending.begin() + 2, pending.end());
	pending.clear();
	records.insert(records.end(), buf.begin(), buf.end());
	net_v2::send_legacy_reliable(originator, records);
}

void dispatch_table::send_data_direct(const std::span<const uint8_t> data, const playernum_t pnum, int) const
{
	if (!(Game_mode&GM_NETWORK) || !UDP_Socket[0])
		return;
	if (data.empty())
		return;
	if (!multi_i_am_master() && pnum != 0)
		Error("Client sent direct data to non-Host in send_data_direct()!\n");
	std::vector<uint8_t> msg;
	msg.reserve(1 + data.size());
	msg.push_back(Player_num);
	msg.insert(msg.end(), data.begin(), data.end());
	net_v2::send_to_slot(pnum, ::dcx::net_v2::session_msg::legacy_mdata, msg);
}

void dispatch_table::do_protocol_frame(int, int listen) const
{
	auto &LevelUniqueControlCenterState = LevelUniqueObjectState.ControlCenterState;
	if (!(Game_mode&GM_NETWORK) || !UDP_Socket[0])
		return;
	auto &S = net_v2::S;
	const fix64 now{timer_query()};
	/* The v1 robot frame: robot positions and fire, at 10 Hz. */
	static fix64 last_robot_time;
	if (now >= last_robot_time + (F1_0 / 10))
	{
		last_robot_time = now;
		multi_send_robot_frame();
	}
#if DXX_BUILD_DESCENT == 2
	/* The thief's position, at the tick rate (a legacy record until the
	 * host owns robots; guided missiles travel in the state bundle).
	 */
	static fix64 last_thief_time;
	if (now >= last_thief_time + F1_0 / net_v2::local_tick_period().denominator)
	{
		last_thief_time = now;
		multi_send_thief_frame();
	}
#endif
	if (!LevelUniqueControlCenterState.Control_center_destroyed)
		S.countdown_started = 0;
	else if (!S.countdown_started)
		S.countdown_started = now;
	/* The host ends a countdown that stopped (net_countdown.h). */
	else if (multi_i_am_master() && LevelUniqueControlCenterState.Countdown_seconds_left > 0 && ::dcx::net_v2::countdown_overdue(now - S.countdown_started, LevelUniqueControlCenterState.Total_countdown_time))
	{
		con_printf(CON_NORMAL, "reactor: countdown overdue (T-%d s of %d s after %d s); the host ends it", LevelUniqueControlCenterState.Countdown_seconds_left, LevelUniqueControlCenterState.Total_countdown_time, static_cast<int>((now - S.countdown_started) / F1_0));
		LevelUniqueControlCenterState.Countdown_timer = std::min(LevelUniqueControlCenterState.Countdown_timer, ::dcx::net_v2::countdown_timer_of_seconds(0));
		LevelUniqueControlCenterState.Countdown_seconds_left = 0;
		movement_record_level_event(::dcx::movrec::level_event_kind::countdown_set, Player_num, 0, 1);
		S.last_endlevel = 0;
	}
	if (now >= S.last_endlevel + net_v2::ENDLEVEL_INTERVAL && LevelUniqueControlCenterState.Control_center_destroyed)
	{
		S.last_endlevel = now;
		net_v2::send_endlevel_status();
	}
	net_v2::frame(listen != 0);
}

void dispatch_table::send_endlevel_packet() const
{
	net_v2::send_endlevel_status();
	net_v2::frame(false);
}

void dispatch_table::kick_player(const _sockaddr &dump_addr, const kick_player_reason why) const
{
	net_v2::S.now = timer_query();
	if (const auto p{net_v2::find_peer_by_addr(dump_addr)})
	{
		const auto slot{net_v2::peer_slot(*p)};
		net_v2::kick_peer(*p, why);
		if (multi_i_am_master() && slot && vcplayerptr(slot)->connected != player_connection_status::disconnected)
		{
			net_v2::S.left_reason = why;
			multi_disconnect_player(slot);
		}
		if (Network_status == network_state::starting)
		{
			net_v2::vacate_slot(slot);
			net_v2::host_send_netgame_update();
		}
		return;
	}
	/* Nobody we know: a join that was not accepted (the refuse prompt
	 * timed out).
	 */
	net_v2::deny_join(dump_addr, 0, why, 0);
}

// do UDP stuff to disconnect a player. Should ONLY be called from multi_disconnect_player()
void dispatch_table::disconnect_player(int playernum) const
{
	if (playernum == Player_num)
	{
		Int3(); // Weird, see Rob
		return;
	}
	auto &S = net_v2::S;
	/* Its snapshots describe a ship that is gone. */
	net_v2::interp::reset_player(playernum);
	if (multi_i_am_master())
	{
		const std::array<uint8_t, 2> buf{{static_cast<uint8_t>(playernum), underlying_value(S.left_reason)}};
		net_v2::broadcast_reliable(::dcx::net_v2::session_msg::player_left, buf, playernum);
		S.left_reason = kick_player_reason::timeout;
		auto &p = S.peers[playernum];
		/* A kicked peer's connection lingers so that the KICK arrives; a
		 * peer that announced its leaving in the game (MULTI_QUIT) lingers
		 * so that its LEAVE is acknowledged.
		 */
		if (p.conn && p.ph != net_v2::peer::phase::closing)
		{
			p.ph = net_v2::peer::phase::closing;
			p.close_at = timer_query() + ::dcx::net_v2::NET_V2_CLOSE_LINGER;
		}
		Netgame.players[playernum].LastPacketTime = timer_query();
		S.inputs[playernum] = {};
	}
	else if (playernum == 0)
		net_v2::drop_peer(S.peers[0]);
}

void dispatch_table::leave_game() const
{
	int nsave;
	auto &S = net_v2::S;
	con_printf(CON_VERBOSE, "teardown: leave_game (%s)", multi_i_am_master() ? "host" : "client");

	dispatch->do_protocol_frame(1, 1);

	if (multi_i_am_master())
	{
		while (Network_sending_extras>1 && Player_joining_extras!=-1)
		{
			timer_update();
			S.now = timer_query();
			net_v2::send_extras();
		}

		for (auto &&[i, p] : enumerate(S.peers))
			if (i && p.conn)
			{
				net_v2::peer_queue(p, ::dcx::net_v2::session_msg::host_shutdown, {});
				p.ph = net_v2::peer::phase::closing;
				p.close_at = timer_query() + ::dcx::net_v2::NET_V2_CLOSE_LINGER;
			}
	}
	else
		net_v2::client_send_leave(kick_player_reason::quit);

	/* Wait up to a second for the LEAVE or HOST_SHUTDOWN to be
	 * acknowledged.
	 */
	net_v2::linger_closing();

	if (multi_i_am_master())
	{
		Netgame.numplayers = 0;
		nsave=N_players;
		N_players=0;
		net_v2::host_broadcast_game_info_lite();
		N_players=nsave;
#if DXX_USE_TRACKER
		if( Netgame.Tracker )
			net_v2::tracker_unregister();
#endif
	}

	get_local_player().connected = player_connection_status::disconnected;
	change_playernum_to(0);
#if DXX_BUILD_DESCENT == 2
	write_player_file();
#endif

	con_printf(CON_VERBOSE, "teardown: leaving the network game, closing the sockets");
	net_v2::flush_sockets();
	net_v2::close_sockets();
	/* The end of a session: the buffered log reaches the disk even if
	 * something after this crashes.
	 */
	con_flush_gamelog();
	/* Out of the game: a later join's GAME_INFO is accepted only in the
	 * menus (handle_game_info), and the manual join does not set the
	 * state itself.
	 */
	Network_status = network_state::menu;
}

}
}

}
