/*
 * This file is part of the DXX-Rebirth project <https://github.com/dxx-rebirth/dxx-rebirth/>.
 * It is copyright by its individual contributors, as recorded in the
 * project's Git history.  See COPYING.txt at the top level for license
 * terms and a link to the Git history.
 */
/*
 * Multiplayer network protocol v2, stage 1: the interface between the
 * session layer (similar/main/net_v2.cpp: sockets, discovery, join
 * handshake, connections, message dispatch, level snapshot) and the menus
 * and level start flow that stayed in similar/main/net_udp.cpp.
 *
 * Everything here is internal to the two files; the rest of the game goes
 * through multi::dispatch (multi.h).
 */

#pragma once

#include <cstdint>
#include <cstring>
#include <span>
#include "dxxsconf.h"
#include "dsx-ns.h"
#include "maths.h"
#include "multi.h"
#include "net_udp.h"

#if DXX_USE_MULTIPLAYER

/* net_interp.h, which is not included here: it declares namespace
 * dcx::net_v2, which would make `net_v2` ambiguous in net_udp.cpp.
 */
namespace dcx::net_interp {
struct snapshot;
}

namespace dcx {

[[nodiscard]]
static inline bool operator==(const _sockaddr &l, const _sockaddr &r)
{
	return !memcmp(&l, &r, sizeof(l));
}

[[nodiscard]]
static inline bool operator!=(const _sockaddr &l, const _sockaddr &r)
{
	return !(l == r);
}

/* Format an address for messages.  Returns a pointer into `dbuf`. */
const char *dxx_ntop(const _sockaddr &sa, _sockaddr::presentation_buffer &dbuf);

/* The port of an address, in host byte order. */
[[nodiscard]]
uint16_t dxx_sockaddr_port(const _sockaddr &sa);

/* Resolve `host` into `addr`.  Shows a message box on failure unless
 * `silent`.  Returns 0 on success.
 */
int udp_dns_filladdr(_sockaddr &addr, const char *host, uint16_t port, bool numeric_only, bool silent);

}

#ifdef DXX_BUILD_DESCENT
namespace dsx {

namespace net_v2 {

/* Sockets.  Index 0 is the game socket, index 1 the default port opened
 * in addition when the game port differs from it, so that LAN broadcasts
 * are still received.  open_socket shows a message box on failure.
 */
[[nodiscard]]
bool open_socket(unsigned index, uint16_t port);
void close_sockets();
void flush_sockets();
[[nodiscard]]
bool socket_ready();

/* Forget the session (peers, connections, join attempt, session id).  The
 * sockets stay open.
 */
void session_reset();

/* Read every pending datagram and drive every connection (acknowledge,
 * retransmit, keep alive).  Menus call this from their polling handlers;
 * in the game, multi::dispatch->do_protocol_frame does the same and more.
 */
void poll();

/* Discovery (section 4.1): broadcast a GAME_INFO_LITE_REQ on the LAN (the
 * tracker is asked separately); request the full game info from one host.
 */
void request_game_list();
void request_game_info(const _sockaddr &host);

/* The join handshake, client side (section 4.2).  client_begin_join sends
 * JOIN_REQUEST until the host answers or the attempt times out; the menu
 * polls client_join_status and calls client_end_join once it has acted on
 * a final status.  On `accepted`, Player_num is already the assigned slot
 * and the connection to the host exists.  On `denied`, the reason was
 * already shown to the user (or Netgame.protocol.udp.valid is -1 for a
 * version mismatch).
 */
enum class join_status : uint8_t
{
	idle,
	joining,
	accepted,
	denied,
	timed_out,
};
void client_begin_join(const _sockaddr &host, uint32_t session_id
#if DXX_USE_TRACKER
	, tracker_game_id tracker_id
#endif
	);
[[nodiscard]]
join_status client_join_status();
void client_end_join();

/* Level start, client side (section 4.3): tell the host the level is
 * loaded; leave the game while waiting for it to start.
 */
void client_send_level_ready();
void client_send_leave(kick_player_reason reason);

/* Host side. */
/* Choose a session id and become the host of a new session. */
void host_open_session();
/* Send GAME_SETTINGS and PLAYER_LIST to every peer and broadcast
 * GAME_INFO_LITE (the v1 "netgame update").
 */
void host_send_netgame_update();
void host_broadcast_game_info_lite();
/* Level start (section 4.3): the players that have not reported the
 * current level loaded are set to `waiting`; called when the host starts
 * waiting for them.  A peer's LEVEL_READY then sets it back to `playing`
 * (or kicks it on a checksum mismatch).  If the host starts without
 * waiting, a player still `waiting` gets no LEVEL_START; its LEVEL_READY
 * later makes it a join in progress (snapshot, CLIENT_READY, LEVEL_GO).
 */
void host_begin_level_wait();
/* Send LEVEL_START, the game snapshot and LEVEL_GO to every ready player
 * (the v1 "sync"), then apply the same locally.
 */
void host_send_level_start();
/* KICK every connected peer (game aborted, not enough start positions). */
void host_kick_all(kick_player_reason reason);
/* The level ended: KICK(endlevel) every peer that was accepted but never
 * entered it (still loading, syncing, or late for the level start), since
 * the next level start would not reach it; it may join the next level.
 */
void host_end_level();

/* Apply the level start data in Netgame to the local game state (the v1
 * read_sync_packet without the parsing): player list, scores, ship
 * placement.  Called by the host after host_send_level_start and by a
 * client on LEVEL_GO.
 */
void apply_level_go();

#if DXX_USE_TRACKER
void tracker_register();
void tracker_unregister();
void tracker_request_games();
void tracker_request_holepunch(tracker_game_id id);
#endif

}

/* Implemented by net_interp.cpp for net_v2.cpp (stage 2, section 5.4):
 * the snapshots of the remote ships and guided missiles.  Times are the
 * local clock (`now`, timer_query) and the host clock
 * (::dcx::net_interp::host_clock, the local clock plus the offset given
 * to set_clock).
 */
namespace net_v2::interp {

/* The estimated host clock is the local clock plus `offset` once `valid`
 * (on the host: always, offset 0).  `tick_period` is the base of every
 * entity's interpolation delay.  Call every frame.
 */
void set_clock(bool valid, std::int64_t offset, std::int64_t tick_period);
/* A snapshot of player `pnum`'s ship, received at `now`. */
void receive_ship(playernum_t pnum, const ::dcx::net_interp::snapshot &s, std::int64_t now);
/* Player `pnum` is dead or not spawned: forget its snapshots. */
void receive_ghost(playernum_t pnum);
/* A snapshot of player `pnum`'s guided missile, `id` the owner's object
 * number of it.
 */
void receive_guided(playernum_t pnum, uint16_t id, const ::dcx::net_interp::snapshot &s, std::int64_t now);
/* How old player `pnum`'s newest state was at the host (lag marker). */
void set_lag_age(playernum_t pnum, std::int64_t age);
/* How far in the past the host's own entities are shown: the `view_time`
 * of INPUT is its sample time minus this.  Zero on the host.
 */
[[nodiscard]]
std::int64_t view_delay();
void reset_player(playernum_t pnum);

}

/* Implemented by net_udp.cpp for net_v2.cpp. */

/* Update the Netgame structure from the current game state before it is
 * sent (numconnected, player states, kills, level number, hoard flags).
 */
void net_udp_update_netgame();

/* A GAME_INFO_LITE arrived: add it to, refresh it in, or remove it from
 * the game list menu, if that menu is open.
 */
void net_udp_game_list_update(UDP_netgame_info_lite &&game);

}
#endif

#endif
