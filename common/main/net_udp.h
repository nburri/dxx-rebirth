/*
 * This file is part of the DXX-Rebirth project <https://github.com/dxx-rebirth/dxx-rebirth/>.
 * It is copyright by its individual contributors, as recorded in the
 * project's Git history.  See COPYING.txt at the top level for license
 * terms and a link to the Git history.
 */
/*
 *
 * Prototypes for UDP-protocol network management functions.
 *
 * The wire protocol is the v2 protocol of
 * Documentation/network-protocol-v2.md: the transport of
 * net_v2_transport.h under the session layer of similar/main/net_v2.cpp.
 * similar/main/net_udp.cpp keeps the menus (game list, join, setup) and
 * the level start flow.
 *
 */

#pragma once
#include "multi.h"
#include "newmenu.h"
#include "pack.h"
#include "ntstring.h"
#include "fwd-window.h"
#include "d_array.h"
#include <array>

// Exported functions
#ifdef DXX_BUILD_DESCENT
namespace dsx {
namespace multi {
namespace udp {
struct dispatch_table final : multi::dispatch_table
{
	virtual void send_data(std::span<const uint8_t> data, multiplayer_data_priority priority, playernum_t originator) const override;
	virtual void send_data_direct(std::span<const uint8_t> data, playernum_t pnum, int needack) const override;
	virtual void do_protocol_frame(int force, int listen) const override;
	virtual window_event_result level_sync() const override;
	virtual void send_endlevel_packet() const override;
	virtual void kick_player(const _sockaddr &dump_addr, kick_player_reason why) const override;
	virtual void disconnect_player(int playernum) const override;
	virtual int end_current_level(
#if DXX_BUILD_DESCENT == 1
		next_level_request_secret_flag *secret
#endif
		) const override;
	virtual void leave_game() const override;
};

extern const dispatch_table dispatch;
int kmatrix_poll2(newmenu *menu, const d_event &event, const unused_newmenu_userdata_t *);
void leave_game();
}
using udp::dispatch;
}

window_event_result net_udp_setup_game(const d_select_event &);
/* -botarena (Documentation/multiplayer-bots.md section 8.2), without the
 * menus, the lobby and the network (net_v2::open_loopback_socket):
 * prepare an anarchy game of the current mission's level `level` for
 * `bots` bots (the pilot's netgame profile, read here, may change the
 * bot setup), then host it with the bot setup, its randomness from
 * `seed`.  False if it could not start.
 */
void net_udp_arena_prepare(unsigned level, unsigned bots);
bool net_udp_arena_start(uint32_t seed);
/* Sizes of the network session's queues, for the -verbose frame probe. */
void net_udp_probe_report();
}
#endif
void net_udp_manual_join_game();
void net_udp_list_join_game(grs_canvas &canvas);

// Some defines
// Our default port - easy to remember: D = 4, X = 24, X = 24
namespace dcx {
constexpr uint16_t UDP_PORT_DEFAULT = 42424;
#define UDP_MANUAL_ADDR_DEFAULT "localhost"
#if DXX_USE_TRACKER
#ifndef TRACKER_ADDR_DEFAULT
/* Allow an alternate default at compile time */
#define TRACKER_ADDR_DEFAULT "tracker.dxx-rebirth.com"
#endif
constexpr uint16_t TRACKER_PORT_DEFAULT = 9999;
enum class tracker_game_id : uint16_t
{
};
#endif
#define UDP_MAX_NETGAMES 900
constexpr std::integral_constant<unsigned, 12> UDP_NETGAMES_PPAGE{}; // Netgames on one page of Netlist
}
#define UDP_NETGAMES_PAGES 75 // Pages available on Netlist (UDP_MAX_NETGAMES/UDP_NETGAMES_PPAGE)

// Structure keeping lite game infos (for netlist, etc.)
#ifdef DXX_BUILD_DESCENT
struct UDP_netgame_info_lite : public prohibit_void_ptr<>
{
	struct _sockaddr                game_addr;
	std::array<short, 3>                 program_iver;
	/* Section 4.1: the game list is keyed by the session id. */
	uint32_t                        session_id;
#if DXX_USE_TRACKER
	tracker_game_id			TrackerGameID;
#endif
	ntstring<NETGAME_NAME_LEN> game_name;
	ntstring<MISSION_NAME_LEN> mission_title;
	ntstring<8> mission_name;
	int32_t                         levelnum;
	network_game_type               gamemode;
	ubyte                           RefusePlayers;
	ubyte                           difficulty;
	network_state game_status;
	ubyte                           numconnected;
	ubyte                           max_numplayers;
	netgame_rule_flags game_flag;
	uint8_t                         tick_rate;
	/* Local time the entry was last refreshed; stale entries expire. */
	fix64                           last_seen;
};
#endif
