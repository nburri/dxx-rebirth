/*
 * This file is part of the DXX-Rebirth project <https://github.com/dxx-rebirth/dxx-rebirth/>.
 * It is copyright by its individual contributors, as recorded in the
 * project's Git history.  See COPYING.txt at the top level for license
 * terms and a link to the Git history.
 */
/*
 *
 * Menus and level start flow of UDP network play: the game list, manual
 * and list join, the netgame setup menus, the lobby, and the level sync
 * at the start of every level.  The wire protocol (v2,
 * Documentation/network-protocol-v2.md) lives in net_v2.cpp; the two
 * files talk through net_v2_game.h.
 *
 */

#include "dxxsconf.h"
#include <algorithm>
#include <random>
#include <ranges>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pstypes.h"
#include "digi.h"
#include "window.h"
#include "strutil.h"
#include "args.h"
#include "timer.h"
#include "newmenu.h"
#include "key.h"
#include "object.h"
#include "dxxerror.h"
#include "laser.h"
#include "player.h"
#include "gameseq.h"
#include "net_udp.h"
#include "net_v2_game.h"
#include "game.h"
#include "gauges.h"
#include "multi.h"
#include "bot.h"
#include "palette.h"
#include "powerup.h"
#include "menu.h"
#include "gameseg.h"
#include "sounds.h"
#include "text.h"
#include "newdemo.h"
#include "multibot.h"
#include "state.h"
#include "wall.h"
#include "bm.h"
#include "effects.h"
#include "physics.h"
#include "hudmsg.h"
#include "switch.h"
#include "textures.h"
#include "event.h"
#include "playsave.h"
#include "gamefont.h"
#include "vers_id.h"
#include "u_mem.h"
#include "weapon.h"

#include "compiler-cf_assert.h"
#include "compiler-range_for.h"
#include "d_enumerate.h"
#include "d_levelstate.h"
#include "d_range.h"
#include "d_zip.h"
#include "partial_range.h"
#include "clipboard.h"
#include "net_address_text.h"
#include <array>
#include <utility>

#if !defined(WIN32)
#include <arpa/inet.h>
#include <netdb.h>
#include <sys/socket.h>
#endif

namespace dcx {

namespace {

static std::pair<const char *, const char *> GetRankStringWithSpace(const netplayer_info::player_rank r)
{
	return PlayerCfg.NoRankings ? std::pair{"", ""} : std::pair{RankStrings[r], " "};
}

static uint16_t UDP_MyPort;

}

}

namespace {
enum class join_netgame_status_code : uint8_t
{
	game_in_disallowed_state,
	game_has_capacity,
	game_is_full,
	game_refuses_players,
};

struct net_udp_select_teams_menu_items
{
	static constexpr std::integral_constant<std::size_t, 0> idx_label_blue_team{};
	unsigned team_vector;
	unsigned idx_label_red_team;
	unsigned idx_item_accept;
	const color_palette_index blue_team_color;
	const color_palette_index red_team_color;
	per_team_array<callsign_t> team_names;
	/* Labels plus slots for every player */
	std::array<newmenu_item, std::size(Netgame.players) + 4> m;
	net_udp_select_teams_menu_items(unsigned num_players);
	unsigned setup_team_sensitive_entries(unsigned num_players);
};

struct net_udp_select_teams_menu : net_udp_select_teams_menu_items, newmenu
{
	net_udp_select_teams_menu(const unsigned num_players, grs_canvas &src) :
		net_udp_select_teams_menu_items(num_players),
		newmenu(menu_title{nullptr}, menu_subtitle{TXT_TEAM_SELECTION}, menu_filename{nullptr}, tiny_mode_flag::normal, tab_processing_flag::ignore, adjusted_citem::create(partial_range(m, idx_item_accept + 1), 0), src)
	{
	}
	virtual window_event_result event_handler(const d_event &event) override;
};

net_udp_select_teams_menu_items::net_udp_select_teams_menu_items(const unsigned num_players) :
	blue_team_color(BM_XRGB(player_rgb[player_ship_color::blue].r, player_rgb[player_ship_color::blue].g, player_rgb[player_ship_color::blue].b)),
	red_team_color(BM_XRGB(player_rgb[player_ship_color::red].r, player_rgb[player_ship_color::red].g, player_rgb[player_ship_color::red].b))
{
	const auto set_team_name = [](callsign_t &team_name, const auto &&s) {
		if constexpr (std::is_array<typename std::remove_cvref<decltype(s)>::type>::value)
			/* When the input is an array (due to
			 * -D'DXX_USE_BUILTIN_ENGLISH_TEXT_STRINGS'), construct a std::span
			 *  implicitly with the correct length.
			 */
			team_name.copy(s);
		else
			/* Otherwise, construct one explicitly with the dynamically
			 * detected length. */
			team_name.copy(std::span(s, strlen(s)));
	};
	set_team_name(team_names[team_number::blue], TXT_BLUE);
	set_team_name(team_names[team_number::red], TXT_RED);
	/* Round blue team up.  Round red team down. */
	const unsigned num_blue_players = (num_players + 1) >> 1;
	// Put first half of players on team A
	team_vector = ((1 << num_players) - 1) & ~((1 << num_blue_players) - 1);
	bots_apply_team_preferences(team_vector, num_players);
	/* Blue team label is always in the same position.  Red team label
	 * varies based on how many players are on the blue team, so the red
	 * team label is set by setup_team_sensitive_entries.
	 */
	nm_set_item_input(m[idx_label_blue_team], team_names[team_number::blue].a);
	const unsigned idx_label_blank0 = setup_team_sensitive_entries(num_players);
	idx_item_accept = idx_label_blank0 + 1;
	nm_set_item_text(m[idx_label_blank0], "");
	nm_set_item_menu(m[idx_item_accept], TXT_ACCEPT);
}

unsigned net_udp_select_teams_menu_items::setup_team_sensitive_entries(const unsigned num_players)
{
	const unsigned blue_team_first_player = idx_label_blue_team + 1;
	const auto tv{team_vector};
	auto mi = std::next(m.begin(), blue_team_first_player);
	unsigned ir = blue_team_first_player;
	for (auto &&[i, ngp] : enumerate(partial_range(Netgame.players, num_players)))
	{
		if (tv & (1 << i))
			continue;
		nm_set_item_menu(*mi, ngp.callsign);
		mi->value = i;
		++ mi;
		++ ir;
	}
	idx_label_red_team = ir;
	nm_set_item_input(*mi, team_names[team_number::red].a);
	++ mi;
	++ ir;
	for (auto &&[i, ngp] : enumerate(partial_range(Netgame.players, num_players)))
	{
		if (!(tv & (1 << i)))
			continue;
		nm_set_item_menu(*mi, ngp.callsign);
		mi->value = i;
		++ mi;
		++ ir;
	}
	return ir;
}

window_event_result net_udp_select_teams_menu::event_handler(const d_event &event)
{
	switch (event.type)
	{
		case event_type::newmenu_selected:
			{
				const auto citem = static_cast<const d_select_event &>(event).citem;
				if (citem == idx_item_accept)
				{
					Netgame.team_vector = team_vector;
					Netgame.team_name = team_names;
					return window_event_result::close;
				}
				else if (citem == idx_label_blue_team || citem == idx_label_red_team)
					/* No handling required, but use this return code to
					 * reuse the `handled` exit path.
					 */
					return window_event_result::handled;
				const auto player_team_mask = 1 << m[citem].value;
				enum class prior_team_color : unsigned
				{
					blue,
				} prior_team = static_cast<prior_team_color>(team_vector & player_team_mask);
				team_vector ^= player_team_mask;
				/* Rebuild the menu entries to reflect that a player has
				 * changed teams.
				 */
				setup_team_sensitive_entries(N_players);
				/* idx_label_red_team is changed by the call to
				 * setup_team_sensitive_entries, so this test is not
				 * redundant relative to the label handling above.
				 */
				if (citem != idx_label_red_team)
					/* If the selection after the change is not pointing
					 * at the label for "Red", then no special handling
					 * is needed.  The selection will now point at a
					 * player who was a teammate of the player who was
					 * moved by this handler.
					 */
					return window_event_result::handled;
				/* If the selection is now on the label "Red" ... */
				if (citem < idx_item_accept - 1 && (citem == idx_label_blue_team + 1 || prior_team != prior_team_color::blue))
					/* If the red team is not empty, and either the blue
					 * team is now empty or the previous team was not
					 * blue, increment the position to point to
					 * red-player-1.
					 *
					 * Otherwise (red is empty) or (blue is not empty
					 * and previous team was blue), decrement the
					 * position, so that the selection points to the
					 * now-last blue player.
					 *
					 * This logic allows the host to quickly transfer
					 * multiple adjacent players to the other team.
					 */
					++ this->citem;
				else
					-- this->citem;
			}
			return window_event_result::handled;
		case event_type::newmenu_draw:
			{
				const auto draw_team_color_box = [&canv = this->w_canv](const newmenu_item &mi, const color_palette_index cpi) {
					const unsigned height = mi.h - 8;
					/* Yes, height is used in an X term.  The box should
					 * be square.
					 */
					gr_urect(canv, mi.x - 12 - height, mi.y, mi.x - 12, mi.y + height, cpi);
				};
				draw_team_color_box(m[idx_label_blue_team], blue_team_color);
				draw_team_color_box(m[idx_label_red_team], red_team_color);
#ifndef NDEBUG
				/* Non-developers probably do not care about the player
				 * index, so hide this from them.
				 */
				const auto draw_player_number = [&canv = this->w_canv, &game_font = *GAME_FONT, team_vector = this->team_vector, blue_team_color = this->blue_team_color, red_team_color = this->red_team_color](const newmenu_item &mi) {
					const unsigned height = mi.h - 8;
					gr_set_fontcolor(canv, (1 << mi.value) & team_vector ? red_team_color : blue_team_color, -1);
					gr_uprintf(canv, game_font, mi.x - 12 - height, mi.y, "%d", 1 + mi.value);
				};
				for (auto &mi : partial_range(m, idx_label_blue_team + 1, idx_label_red_team))
					draw_player_number(mi);
				for (auto &mi : partial_range(m, idx_label_red_team + 1, idx_item_accept - 1))
					draw_player_number(mi);
#endif
			}
			break;
		default:
			break;
	}
	return newmenu::event_handler(event);
}
// Prototypes
static void net_udp_init();
static void net_udp_close();
static int net_udp_start_game();

/* Read every pending datagram and drive every connection; the menus call
 * this from their polling handlers.
 */
static void net_udp_listen()
{
	::dsx::net_v2::poll();
}
}

namespace dcx {

namespace {
class start_poll_menu_items
{
	/* The host must play */
	unsigned playercount{1};
public:
	std::array<newmenu_item, MAX_PLAYERS + 4> m;
	unsigned get_player_count() const
	{
		return playercount;
	}
	void set_player_count(const unsigned c)
	{
		playercount = c;
	}
};

static void reset_UDP_MyPort()
{
	UDP_MyPort = CGameArg.MplUdpMyPort >= 1024 ? CGameArg.MplUdpMyPort : UDP_PORT_DEFAULT;
}

static bool convert_text_portstring(const std::array<char, 6> &portstring, uint16_t &outport, bool allow_privileged, bool silent)
{
	char *porterror;
	unsigned long myport = strtoul(portstring.data(), &porterror, 10);
	if (*porterror || static_cast<uint16_t>(myport) != myport || (!allow_privileged && myport < 1024))
	{
		if (!silent)
			nm_messagebox(menu_title{TXT_ERROR}, {TXT_OK}, "Illegal port \"%s\"", portstring.data());
		return false;
	}
	else
		outport = myport;
	return true;
}
}
}

namespace {
struct direct_join
{
	enum class connect_type : uint8_t
	{
		idle,
		connecting,
		request_join,
		/* JOIN_REQUEST sent, waiting for the host's answer. */
		joining,
	};
	struct _sockaddr host_addr;
	fix64 start_time, last_time;
	connect_type connecting = connect_type::idle;
#if DXX_USE_TRACKER
	tracker_game_id gameid;
#endif
};

struct manual_join_user_inputs
{
	std::array<char, 6> hostportbuf, guestportbuf;
	std::array<char, 128> hostaddrbuf;
};

struct manual_join_menu_items : direct_join, manual_join_user_inputs
{
	enum {
		label_host_address,
		input_host_address,
		label_host_port,
		input_host_port,
		label_guest_port,
		input_guest_port,
		label_status_text,
	};
	static manual_join_user_inputs s_last_inputs;
	std::array<newmenu_item, 7> m;
	manual_join_menu_items()
	{
		if (s_last_inputs.hostaddrbuf[0])
			hostaddrbuf = s_last_inputs.hostaddrbuf;
		else
			snprintf(hostaddrbuf.data(), hostaddrbuf.size(), "%s", CGameArg.MplUdpHostAddr.c_str());
		if (s_last_inputs.hostportbuf[0])
			hostportbuf = s_last_inputs.hostportbuf;
		else
			snprintf(hostportbuf.data(), hostportbuf.size(), "%hu", CGameArg.MplUdpHostPort ? CGameArg.MplUdpHostPort : UDP_PORT_DEFAULT);
		if (s_last_inputs.guestportbuf[0])
			guestportbuf = s_last_inputs.guestportbuf;
		else
			snprintf(guestportbuf.data(), guestportbuf.size(), "%hu", UDP_MyPort);
		nm_set_item_text(m[label_host_address], "GAME ADDRESS OR HOSTNAME:");
		nm_set_item_text(m[label_host_port], "GAME PORT:");
		nm_set_item_text(m[label_guest_port], "MY PORT:");
		nm_set_item_text(m[label_status_text], "");
		nm_set_item_input(m[input_host_address], hostaddrbuf);
		nm_set_item_input(m[input_host_port], hostportbuf);
		nm_set_item_input(m[input_guest_port], guestportbuf);
	}
};

struct manual_join_menu : manual_join_menu_items, newmenu
{
	manual_join_menu(grs_canvas &src) :
		newmenu(menu_title{nullptr}, menu_subtitle{"ENTER GAME ADDRESS\nCtrl+V pastes address or address:port"}, menu_filename{nullptr}, tiny_mode_flag::normal, tab_processing_flag::ignore, adjusted_citem::create(m, input_host_address), src)
	{
	}
	virtual window_event_result event_handler(const d_event &event) override;
	/* Ctrl+V into the address or the game port field: a pasted
	 * "host:port" or "[ipv6]:port" fills both fields.  False when the
	 * clipboard holds no such address (then the field gets the usual
	 * paste).
	 */
	bool paste_address();
};

bool manual_join_menu::paste_address()
{
	const auto a{parse_pasted_address(clipboard_get_text())};
	if (!a)
		return false;
	/* Into the port field only a full address: a bare number there is
	 * a port, which the usual paste handles.
	 */
	if (citem == input_host_port && !a->port)
		return false;
	if (a->host.size() >= hostaddrbuf.size())
		return false;
	std::snprintf(hostaddrbuf.data(), hostaddrbuf.size(), "%s", a->host.c_str());
	m[input_host_address].value = static_cast<int>(a->host.size());
	if (a->port)
	{
		std::snprintf(hostportbuf.data(), hostportbuf.size(), "%u", static_cast<unsigned>(a->port));
		m[input_host_port].value = -1;
	}
	return true;
}

struct netgame_list_game_menu_items
{
	enum
	{
		header_rows = 4,
		non_game_rows = header_rows + 1,
		menuitem_count = UDP_NETGAMES_PPAGE + non_game_rows,
	};
	std::array<newmenu_item, menuitem_count> menus;
	std::array<std::array<char, 92>, menuitem_count - non_game_rows> ljtext;
	netgame_list_game_menu_items()
	{
#if DXX_USE_TRACKER
#define DXX_NETGAME_LIST_SCAN_STRING	"\tF4/F5/F6: (Re)Scan for all/LAN/Tracker Games."
#else
#define DXX_NETGAME_LIST_SCAN_STRING	"\tF4: (Re)Scan for LAN Games."
#endif
		nm_set_item_text(menus[0], DXX_NETGAME_LIST_SCAN_STRING);
#undef DXX_NETGAME_LIST_SCAN_STRING
		nm_set_item_text(menus[1], "\tPgUp/PgDn: Flip Pages.");
		nm_set_item_text(menus[2], "");
		nm_set_item_text(menus[3], "\tGAME \tMODE \t#PLYRS \tMISSION \tLEV \tSTATUS");

		for (auto &&[i, lj, mi] : enumerate(zip(ljtext, unchecked_partial_range(menus, header_rows + 0u, menus.size() - 1)), 1u))
		{
			snprintf(lj.data(), lj.size(), "%u.                                                                      ", i);
			nm_set_item_menu(mi, lj.data());
		}
		nm_set_item_text(menus.back(), "\t");
	}
};

struct netgame_list_game_menu;

netgame_list_game_menu *netgame_list_menu;

struct netgame_list_game_menu : netgame_list_game_menu_items, direct_join, newmenu
{
	unsigned num_active_udp_games{0};
	uint8_t num_active_udp_changed{1};
	std::array<UDP_netgame_info_lite, UDP_MAX_NETGAMES> Active_udp_games{};
	netgame_list_game_menu(grs_canvas &src) :
		newmenu(menu_title{"NETGAMES"}, menu_subtitle{nullptr}, menu_filename{nullptr}, tiny_mode_flag::tiny, tab_processing_flag::process, adjusted_citem::create(menus, 0), src)
	{
		assert(!netgame_list_menu);
		netgame_list_menu = this;
	}
	~netgame_list_game_menu()
	{
		assert(netgame_list_menu == this);
		netgame_list_menu = nullptr;
	}
	virtual window_event_result event_handler(const d_event &event) override;
};

manual_join_user_inputs manual_join_menu_items::s_last_inputs;
}

namespace dsx {

/* A GAME_INFO_LITE arrived (LAN or tracker): update the game list. */
void net_udp_game_list_update(UDP_netgame_info_lite &&recv_game)
{
	const auto menu{netgame_list_menu};
	if (!menu)
		return;
	menu->num_active_udp_changed = 1;
	auto r = partial_range(menu->Active_udp_games, menu->num_active_udp_games);
	const auto &&i{std::ranges::find_if(r, [&recv_game](const UDP_netgame_info_lite &g) { return g.session_id == recv_game.session_id; })};
	if (i == menu->Active_udp_games.end())
	{
		return;
	}
	*i = std::move(recv_game);
#if DXX_BUILD_DESCENT == 2
	// See if this is really a Hoard game
	// If so, adjust all the data accordingly
	if (HoardEquipped() != hoard_availability_state::Missing)
	{
		if (const auto game_flag{i->game_flag}; (game_flag & netgame_rule_flags::hoard) != netgame_rule_flags::None)
		{
			i->gamemode = (game_flag & netgame_rule_flags::team_hoard) != netgame_rule_flags::None
				? network_game_type::team_hoard
				: network_game_type::hoard;
			i->game_status = (game_flag & netgame_rule_flags::really_endlevel) != netgame_rule_flags::None
				? network_state::endlevel
				: (
					(game_flag & netgame_rule_flags::really_forming) != netgame_rule_flags::None
					? network_state::starting
					: network_state::playing
				);
		}
	}
#endif
	if (i == r.end())
	{
		if (i->numconnected)
			++ menu->num_active_udp_games;
	}
	else if (!i->numconnected)
	{
		// Delete this game
		std::move(std::next(i), r.end(), i);
		-- menu->num_active_udp_games;
	}
}

namespace {

direct_join::connect_type net_udp_show_game_info(const netgame_info &Netgame);
static bool net_udp_join_precheck();
static int net_udp_do_join_game();

static void net_udp_show_version_mismatch()
{
	nm_messagebox(menu_title{TXT_ERROR}, {TXT_OK}, "Version mismatch! Cannot join Game.\n\nHost game version: %i.%i.%i\nHost game protocol: %i\n(%s)\n\nYour game version: " DXX_VERSION_STR "\nYour game protocol: %i\n(%s)", Netgame.protocol.udp.program_iver[0], Netgame.protocol.udp.program_iver[1], Netgame.protocol.udp.program_iver[2], Netgame.protocol.udp.program_iver[3], (Netgame.protocol.udp.program_iver[3]==0?"RELEASE VERSION":"DEVELOPMENT BUILD, BETA, etc."), MULTI_PROTO_VERSION, (MULTI_PROTO_VERSION==0?"RELEASE VERSION":"DEVELOPMENT BUILD, BETA, etc."));
}

static void net_udp_show_no_response(const _sockaddr &host_addr)
{
	typename _sockaddr::presentation_buffer dbuf;
	nm_messagebox(menu_title{TXT_ERROR}, {TXT_OK},
"No response by host.\n\n\
Possible reasons:\n\
* No game on %s (anymore)\n\
* Host port %hu is not open\n\
* Game is hosted on a different port\n\
* Host uses a game version\n\
  I do not understand", dxx_ntop(host_addr, dbuf), dxx_sockaddr_port(host_addr));
}

// Connect to a game host and get full info. Eventually we join!
static int net_udp_game_connect(direct_join *const dj)
{
	if (dj->connecting == direct_join::connect_type::joining)
	{
		/* JOIN_REQUEST is on its way (section 4.2); wait for the answer. */
		timer_delay2(5);
		net_udp_listen();
		switch (net_v2::client_join_status())
		{
			case net_v2::join_status::joining:
				return 0;
			case net_v2::join_status::accepted:
				net_v2::client_end_join();
				dj->connecting = direct_join::connect_type::idle;
				return net_udp_do_join_game();
			case net_v2::join_status::denied:
				net_v2::client_end_join();
				dj->connecting = direct_join::connect_type::idle;
				if (Netgame.protocol.udp.valid == -1)
					net_udp_show_version_mismatch();
				return 0;
			case net_v2::join_status::timed_out:
				net_v2::client_end_join();
				dj->connecting = direct_join::connect_type::idle;
				net_udp_show_no_response(dj->host_addr);
				return 0;
			case net_v2::join_status::idle:
				dj->connecting = direct_join::connect_type::idle;
				return 0;
		}
		return 0;
	}

	// Get full game info so we can show it.

	// Timeout after 10 seconds
	if (timer_query() >= dj->start_time + (F1_0*10))
	{
		dj->connecting = direct_join::connect_type::idle;
		net_udp_show_no_response(dj->host_addr);
		return 0;
	}

	if (Netgame.protocol.udp.valid == -1)
	{
		net_udp_show_version_mismatch();
		dj->connecting = direct_join::connect_type::idle;
		return 0;
	}

	if (timer_query() >= dj->last_time + F1_0)
	{
		net_v2::request_game_info(dj->host_addr);
#if DXX_USE_TRACKER
		if (const auto g = dj->gameid; g != tracker_game_id{})
			if (timer_query() >= dj->start_time + (F1_0*4))
				net_v2::tracker_request_holepunch(g);
#endif
		dj->last_time = timer_query();
	}
	timer_delay2(5);
	net_udp_listen();

	if (Netgame.protocol.udp.valid != 1)
		return 0;		// still trying to connect

	if (dj->connecting == direct_join::connect_type::connecting)
	{
		// show info menu and check if we join
		const auto connecting = net_udp_show_game_info(Netgame);
		dj->connecting = connecting;
		if (connecting == direct_join::connect_type::request_join)
		{
			if (!net_udp_join_precheck())
			{
				dj->connecting = direct_join::connect_type::idle;
				return 0;
			}
			net_v2::client_begin_join(dj->host_addr, Netgame.protocol.udp.session_id
#if DXX_USE_TRACKER
				, dj->gameid
#endif
				);
			dj->connecting = direct_join::connect_type::joining;
		}
		return 0;
	}
	dj->connecting = direct_join::connect_type::idle;

	return 0;
}

}

}
window_event_result manual_join_menu::event_handler(const d_event &event)
{
	switch (event.type)
	{
		case event_type::key_command:
			if (connecting == direct_join::connect_type::idle && key_is_paste(event_key_get(event)) && (citem == input_host_address || citem == input_host_port) && paste_address())
				return window_event_result::handled;
			if (connecting != direct_join::connect_type::idle && event_key_get(event) == KEY_ESC)
			{
				if (connecting == direct_join::connect_type::joining)
					net_v2::client_cancel_join();
				connecting = direct_join::connect_type::idle;
				nm_set_item_text(m[label_status_text], "");
				return window_event_result::handled;
			}
			break;
		case event_type::idle:
			if (connecting != direct_join::connect_type::idle)
			{
				if (net_udp_game_connect(this))
					return window_event_result::close;	// Success!
				else if (connecting == direct_join::connect_type::idle)
					nm_set_item_text(m[label_status_text], "");
			}
			break;

		case event_type::newmenu_selected:
		{
			net_udp_init(); // yes, redundant call but since the menu does not know any better it would allow any IP entry as long as Netgame-entry looks okay... my head hurts...
			if (!convert_text_portstring(guestportbuf, UDP_MyPort, false, false))
				return window_event_result::handled;
			if (!net_v2::open_socket(0, UDP_MyPort))
				return window_event_result::handled;
			uint16_t hostport;
			if (!convert_text_portstring(hostportbuf, hostport, true, false))
				return window_event_result::handled;
			// Resolve address
			if (udp_dns_filladdr(host_addr, hostaddrbuf.data(), hostport, false, false) < 0)
				return window_event_result::handled;
			else
			{
				s_last_inputs = *this;
				multi_new_game();
				N_players = 0;
				change_playernum_to(1);
				start_time = timer_query();
				last_time = 0;
				
				Netgame.players[0].protocol.udp.addr = host_addr;
				connecting = direct_join::connect_type::connecting;
				nm_set_item_text(m[label_status_text], "Connecting...");
				return window_event_result::handled;
			}
		}
		case event_type::window_close:
			if (!Game_wind) // they cancelled
				net_udp_close();
			break;
		default:
			break;
	}
	return newmenu::event_handler(event);
}

void net_udp_manual_join_game()
{
	net_udp_init();

	reset_UDP_MyPort();

	auto menu = window_create<manual_join_menu>(grd_curscreen->sc_canvas);
	(void)menu;
}

namespace {

static void copy_truncate_string(const grs_font &cv_font, const font_x_scaled_float strbound, std::array<char, 25> &out, const ntstring<25> &in)
{
	size_t k = 0, x = 0;
	char thold[2];
	thold[1] = 0;
	const std::size_t outsize = out.size();
	range_for (const char c, in)
	{
		if (unlikely(c == '\t'))
			continue;
		if (unlikely(!c))
			break;
		thold[0] = c;
		const auto tx = gr_get_string_size(cv_font, thold).width;
		if ((x += tx) >= strbound)
		{
			const std::size_t outbound = outsize - 4;
			if (k > outbound)
				k = outbound;
			out[k] = out[k + 1] = out[k + 2] = '.';
			k += 3;
			break;
		}
		out[k++] = c;
		if (k >= outsize - 1)
			break;
	}
	out[k] = 0;
}

window_event_result netgame_list_game_menu::event_handler(const d_event &event)
{
	// Polling loop for Join Game menu
	int newpage{0};
	static int NLPage = 0;
	switch (event.type)
	{
		case event_type::window_activated:
		{
			Netgame.protocol.udp.valid = 0;
			Active_udp_games = {};
			num_active_udp_changed = 1;
			num_active_udp_games = 0;
			net_v2::request_game_list();
#if DXX_USE_TRACKER
			net_v2::tracker_request_games();
#endif
			if (connecting == direct_join::connect_type::idle) // fallback/failsafe!
				nm_set_item_text(menus[UDP_NETGAMES_PPAGE+4], "\t");
			break;
		}
		case event_type::idle:
			if (connecting != direct_join::connect_type::idle)
			{
				if (net_udp_game_connect(this))
					return window_event_result::close;	// Success!
				if (connecting == direct_join::connect_type::idle) // connect wasn't successful - get rid of the message.
					nm_set_item_text(menus[UDP_NETGAMES_PPAGE+4], "\t");
			}
			break;
		case event_type::key_command:
		{
			int key = event_key_get(event);
			if (key == KEY_PAGEUP)
			{
				NLPage--;
				newpage++;
				if (NLPage < 0)
					NLPage = UDP_NETGAMES_PAGES-1;
				key = 0;
				break;
			}
			if (key == KEY_PAGEDOWN)
			{
				NLPage++;
				newpage++;
				if (NLPage >= UDP_NETGAMES_PAGES)
					NLPage = 0;
				key = 0;
				break;
			}
			if( key == KEY_F4 )
			{
				// Empty the list
				Active_udp_games = {};
				num_active_udp_changed = 1;
				num_active_udp_games = 0;
				
				// Request LAN games
				net_v2::request_game_list();
#if DXX_USE_TRACKER
				net_v2::tracker_request_games();
#endif
				// All done
				break;
			}
#if DXX_USE_TRACKER
			if (key == KEY_F5)
			{
				Active_udp_games = {};
				num_active_udp_changed = 1;
				num_active_udp_games = 0;
				net_v2::request_game_list();
				break;
			}

			if( key == KEY_F6 )
			{
				// Zero the list
				Active_udp_games = {};
				num_active_udp_changed = 1;
				num_active_udp_games = 0;
				
				// Request from the tracker
				net_v2::tracker_request_games();
				
				// Break off
				break;
			}
#endif
			if (key == KEY_ESC)
			{
				if (connecting != direct_join::connect_type::idle)
				{
					if (connecting == direct_join::connect_type::joining)
						net_v2::client_cancel_join();
					connecting = direct_join::connect_type::idle;
					nm_set_item_text(menus[UDP_NETGAMES_PPAGE+4], "\t");
					return window_event_result::handled;
				}
				break;
			}
			break;
		}
		case event_type::newmenu_selected:
		{
			auto &citem = static_cast<const d_select_event &>(event).citem;
			if (((citem+(NLPage*UDP_NETGAMES_PPAGE)) >= 4) && (((citem+(NLPage*UDP_NETGAMES_PPAGE))-3) <= num_active_udp_games))
			{
				multi_new_game();
				N_players = 0;
				change_playernum_to(1);
				start_time = timer_query();
				last_time = 0;
				host_addr = Active_udp_games[(citem+(NLPage*UDP_NETGAMES_PPAGE))-4].game_addr;
				Netgame.players[0].protocol.udp.addr = host_addr;
				connecting = direct_join::connect_type::connecting;
#if DXX_USE_TRACKER
				gameid = Active_udp_games[(citem+(NLPage*UDP_NETGAMES_PPAGE))-4].TrackerGameID;
#endif
				nm_set_item_text(menus[UDP_NETGAMES_PPAGE+4], "\tConnecting. Please wait...");
			}
			else
			{
				window_create<passive_messagebox>(menu_title{TXT_SORRY}, menu_subtitle{TXT_INVALID_CHOICE}, TXT_OK, grd_curscreen->sc_canvas);
				// invalid game selected - stay in the menu
			}
			return window_event_result::handled;
		}
		case event_type::window_close:
		{
			if (!Game_wind)
			{
				net_udp_close();
				Network_status = network_state::menu;	// they cancelled
			}
			return window_event_result::ignored;
		}
		default:
			break;
	}

	net_udp_listen();

	/* Section 4.1: an entry not refreshed for 30 s is gone. */
	{
		const auto now{timer_query()};
		auto r = partial_range(Active_udp_games, num_active_udp_games);
		const auto e{std::remove_if(r.begin(), r.end(), [now](const UDP_netgame_info_lite &g) { return g.last_seen + (F1_0 * 30) < now; })};
		if (e != r.end())
		{
			num_active_udp_games = std::distance(r.begin(), e);
			num_active_udp_changed = 1;
		}
	}

	if (!num_active_udp_changed && !newpage)
		return newmenu::event_handler(event);

	num_active_udp_changed = 0;

	// Copy the active games data into the menu options
	for (int i = 0; i < UDP_NETGAMES_PPAGE; i++)
	{
		const auto &augi = Active_udp_games[(i + (NLPage * UDP_NETGAMES_PPAGE))];
		int nplayers{0};
		char levelname[8];

		if ((i+(NLPage*UDP_NETGAMES_PPAGE)) >= num_active_udp_games)
		{
			auto &p = ljtext[i];
			snprintf(p.data(), p.size(), "%d.                                                                      ", (i + (NLPage * UDP_NETGAMES_PPAGE)) + 1);
			continue;
		}

		// These next two loops protect against menu skewing
		// if missiontitle or gamename contain a tab

		const auto &&fspacx = FSPACX();
		const auto &cv_font = *grd_curcanv->cv_font;
		std::array<char, 25> MissName, GameName;
		const auto &&fspacx55 = fspacx(55);
		copy_truncate_string(cv_font, fspacx55, MissName, augi.mission_title);
		copy_truncate_string(cv_font, fspacx55, GameName, augi.game_name);

		nplayers = augi.numconnected;

		const int levelnum = augi.levelnum;
		if (levelnum < 0)
		{
			cf_assert(-levelnum < MAX_SECRET_LEVELS_PER_MISSION);
			snprintf(levelname, sizeof(levelname), "S%d", -levelnum);
		}
		else
		{
			cf_assert(levelnum < MAX_LEVELS_PER_MISSION);
			snprintf(levelname, sizeof(levelname), "%d", levelnum);
		}

		const char *status;
		if (const auto game_status = augi.game_status; game_status == network_state::starting)
			status = "FORMING ";
		else if (game_status == network_state::playing)
		{
			if (augi.RefusePlayers)
				status = "RESTRICT";
			else if ((augi.game_flag & netgame_rule_flags::closed) != netgame_rule_flags::None)
				status = "CLOSED  ";
			else
				status = "OPEN    ";
		}
		else
			status = "BETWEEN ";
		
		const auto gamemode{augi.gamemode};
		auto &p = ljtext[i];
		snprintf(p.data(), p.size(), "%d.\t%.24s \t%.7s \t%3u/%u \t%.24s \t %s \t%s", (i + (NLPage * UDP_NETGAMES_PPAGE)) + 1, GameName.data(), GMNamesShrt.valid_index(gamemode) ? GMNamesShrt[gamemode] : "INVALID", nplayers, augi.max_numplayers, MissName.data(), levelname, status);
	}
	return newmenu::event_handler(event);
}

}

void net_udp_list_join_game(grs_canvas &canvas)
{
	net_udp_init();
	const auto gamemyport{CGameArg.MplUdpMyPort};
	if (!net_v2::open_socket(0, gamemyport >= 1024 ? gamemyport : UDP_PORT_DEFAULT))
		return;

	if (gamemyport >= 1024 && gamemyport != UDP_PORT_DEFAULT)
		if (!net_v2::open_socket(1, UDP_PORT_DEFAULT))
			nm_messagebox_str(menu_title{TXT_WARNING}, nm_messagebox_tie(TXT_OK), menu_subtitle{"Cannot open default port!\nYou can only scan for games\nmanually."});

	change_playernum_to(1);
	N_players = 0;
	Network_sending_extras=0;
	Network_rejoined=0;

	Network_status = network_state::browsing; // We are looking at a game menu

	net_v2::flush_sockets();
	net_udp_listen();  // Throw out old info

	gr_set_fontcolor(canvas, BM_XRGB(15, 15, 23),-1);

	auto menu = window_create<netgame_list_game_menu>(canvas);
	(void)menu;
}
namespace {

void net_udp_init()
{
	// So you want to play a netgame, eh?  Let's a get a few things straight

#ifdef _WIN32
{
	WORD wVersionRequested;
	WSADATA wsaData;
	wVersionRequested = MAKEWORD(2, 2);
	WSACleanup();
	if (WSAStartup( wVersionRequested, &wsaData))
		nm_messagebox_str(menu_title{TXT_ERROR}, nm_messagebox_tie(TXT_OK), menu_subtitle{"Cannot init Winsock!"}); // no break here... game will fail at socket creation anyways...
}
#endif

	Netgame = {};
	net_v2::session_reset();

	multi_new_game();
	net_v2::flush_sockets();
}

void net_udp_close()
{
	net_v2::close_sockets();
#ifdef _WIN32
	WSACleanup();
#endif
}

}
namespace dsx {
namespace multi {
namespace udp {

int dispatch_table::end_current_level(
#if DXX_BUILD_DESCENT == 1
	next_level_request_secret_flag *const secret
#endif
	) const
{
	// Do whatever needs to be done between levels
#if DXX_BUILD_DESCENT == 1
	{
		// We do not really check if a player has actually found a secret level... yeah, I am too lazy! So just go there and pretend we did!
		range_for (const auto i, unchecked_partial_range(Current_mission->secret_level_table.get(), Current_mission->n_secret_levels))
		{
			if (Current_level_num == i)
			{
				*secret = next_level_request_secret_flag::use_secret;
				break;
			}
		}
	}
#endif

	Network_status = network_state::endlevel; // We are between levels
	if (multi_i_am_master())
	{
		net_v2::host_end_level();
		bots_level_end();
	}
	net_udp_listen();
	/* Reliable now: once is enough. */
	dispatch->send_endlevel_packet();
	range_for (auto &i, partial_range(Netgame.players, N_players)) 
	{
		i.LastPacketTime = timer_query();
	}

	net_udp_update_netgame();

	return(0);
}
}
}
}

namespace {
static join_netgame_status_code net_udp_can_join_netgame(const netgame_info *const game)
{
	// Can this player rejoin a netgame in progress?
	if (game->game_status == network_state::starting)
		return join_netgame_status_code::game_has_capacity;

	if (game->game_status != network_state::playing)
		return join_netgame_status_code::game_in_disallowed_state;

	// Game is in progress, figure out if this guy can re-join it

	const unsigned num_players = game->numplayers;

	if ((game->game_flag & netgame_rule_flags::closed) == netgame_rule_flags::None)
	{
		// Look for player that is not connected
		
		if (game->numconnected==game->max_numplayers)
			return join_netgame_status_code::game_is_full;
		if (game->RefusePlayers)
			return join_netgame_status_code::game_refuses_players;
		if (num_players < game->max_numplayers)
			return join_netgame_status_code::game_has_capacity;
		if (game->numconnected<num_players)
			return join_netgame_status_code::game_has_capacity;
	}

	// Search to see if we were already in this closed netgame in progress

	auto &plr = get_local_player();
	for (const auto i : xrange(num_players))
	{
		if (plr.callsign == game->players[i].callsign && i == game->protocol.udp.your_index)
			return join_netgame_status_code::game_has_capacity;
	}
	return join_netgame_status_code::game_in_disallowed_state;
}
}
namespace dsx {
namespace {

/* "Copy game address" (Ctrl+C in the host's waiting screen and in the
 * netgame info): the addresses a joiner could type into "join game
 * manually", each selectable to copy it.  The best one is copied when
 * the menu opens.
 */
struct host_address_menu_items
{
	enum
	{
		max_candidates = 8,
		/* blank, status, blank, three note lines */
		extra_rows = 6,
	};
	std::vector<host_address_candidate> candidates;
	std::array<std::array<char, 80>, max_candidates> lines;
	std::array<char, 80> status;
	std::array<std::array<char, 48>, 3> notes;
	std::array<newmenu_item, max_candidates + extra_rows> m;
	unsigned count;
	host_address_menu_items(std::vector<host_address_candidate> &&c, const uint16_t port, const bool own) :
		candidates{std::move(c)}
	{
		if (candidates.size() > max_candidates)
			candidates.resize(max_candidates);
		const unsigned n = candidates.size();
		const auto label = [own](const host_address_kind k) {
			if (!own)
				return "  (the host)";
			switch (k)
			{
				case host_address_kind::public_ipv4:
					return "  (Internet)";
				case host_address_kind::lan_ipv4:
					return "  (LAN / VPN)";
				case host_address_kind::public_ipv6:
					return "  (IPv6 Internet)";
				case host_address_kind::lan_ipv6:
					return "  (IPv6 LAN)";
				case host_address_kind::unusable:
					break;
			}
			return "";
		};
		for (unsigned i = 0; i < n; ++i)
		{
			auto &line{lines[i]};
			std::snprintf(line.data(), line.size(), "%s%s", candidates[i].text.c_str(), label(candidates[i].kind));
			nm_set_item_menu(m[i], line.data());
		}
		status[0] = 0;
		for (auto &l : notes)
			l[0] = 0;
		if (own && std::ranges::none_of(candidates, [](const host_address_candidate &c) { return c.kind == host_address_kind::public_ipv4; }))
		{
			/* Behind a NAT router: the game cannot know the public
			 * address, and asking an outside service is not wanted.
			 */
			std::snprintf(notes[0].data(), notes[0].size(), "Internet players need your public IP");
			std::snprintf(notes[1].data(), notes[1].size(), "(see your router) and UDP port %u", static_cast<unsigned>(port));
			std::snprintf(notes[2].data(), notes[2].size(), "forwarded to this computer.");
		}
		unsigned i = n;
		nm_set_item_text(m[i++], "");
		nm_set_item_text(m[i++], status.data());
		if (notes[0][0])
		{
			nm_set_item_text(m[i++], "");
			for (auto &l : notes)
				nm_set_item_text(m[i++], l.data());
		}
		count = i;
	}
	void copy(const unsigned i)
	{
		if (i >= candidates.size())
			return;
		const auto &text{candidates[i].text};
		if (clipboard_set_text(text.c_str()))
		{
			std::snprintf(status.data(), status.size(), "Copied %s", text.c_str());
			if (Game_wind)
				HUD_init_message(HM_MULTI, "Copied %s", text.c_str());
		}
		else
			std::snprintf(status.data(), status.size(), "No clipboard: write it down");
	}
};

struct host_address_menu : host_address_menu_items, newmenu
{
	host_address_menu(std::vector<host_address_candidate> &&c, const uint16_t port, const bool own, grs_canvas &src) :
		host_address_menu_items(std::move(c), port, own),
		newmenu(menu_title{nullptr}, menu_subtitle{own ? "GAME ADDRESS\nEnter: copy" : "HOST ADDRESS\nEnter: copy"}, menu_filename{nullptr}, tiny_mode_flag::normal, tab_processing_flag::ignore, adjusted_citem::create(unchecked_partial_range(m, count), 0), src)
	{
		copy(0);
	}
	virtual window_event_result event_handler(const d_event &event) override
	{
		switch (event.type)
		{
			case event_type::newmenu_selected:
				copy(static_cast<const d_select_event &>(event).citem);
				return window_event_result::handled;
			case event_type::key_command:
				if (key_is_copy(event_key_get(event)))
				{
					if (citem >= 0)
						copy(citem);
					return window_event_result::handled;
				}
				break;
			default:
				break;
		}
		return newmenu::event_handler(event);
	}
};

}

void net_udp_copy_game_address()
{
	const bool own{multi_i_am_master()};
	std::vector<host_address_candidate> candidates;
	uint16_t port;
	if (own)
	{
		port = UDP_MyPort;
		candidates = host_address_candidates(port);
	}
	else
	{
		/* A client: the host's address as this client reaches it, to
		 * pass on to other players.
		 */
		const auto &addr{Netgame.players[0].protocol.udp.addr};
		port = dxx_sockaddr_port(addr);
		typename _sockaddr::presentation_buffer dbuf;
		candidates.push_back({format_address_port(dxx_ntop(addr, dbuf), port), host_address_kind::public_ipv4});
	}
	if (candidates.empty())
	{
		nm_messagebox(menu_title{nullptr}, {TXT_OK}, "No network address found.\nThe game port is UDP %u.", static_cast<unsigned>(port));
		return;
	}
	window_create<host_address_menu>(std::move(candidates), port, own, grd_curscreen->sc_canvas);
}

void net_udp_probe_report()
{
	net_v2::probe_report();
}

void net_udp_update_netgame()
{
	auto &Objects = LevelUniqueObjectState.Objects;
	auto &vcobjptr = Objects.vcptr;
	// Update the netgame struct with current game variables
	Netgame.numconnected=0;
	range_for (auto &i, partial_const_range(Players, N_players))
		if (i.connected != player_connection_status::disconnected)
			Netgame.numconnected++;

#if DXX_BUILD_DESCENT == 2
// This is great: D2 1.0 and 1.1 ignore upper part of the game_flags field of
//	the lite_info struct when you're sitting on the join netgame screen.  We can
//	"sneak" Hoard information into this field.  This is better than sending 
//	another packet that could be lost in transit.

	if (HoardEquipped() != hoard_availability_state::Missing)
	{
		const auto is_hoard_game{game_mode_hoard(Game_mode)};
		const auto game_flag_no_hoard{Netgame.game_flag & ~(netgame_rule_flags::hoard | netgame_rule_flags::team_hoard)};
		Netgame.game_flag = is_hoard_game
			? (
				+(Game_mode & GM_TEAM)
				? (game_flag_no_hoard | netgame_rule_flags::hoard | netgame_rule_flags::team_hoard)
				: (game_flag_no_hoard | netgame_rule_flags::hoard)
			)
			: game_flag_no_hoard;
	}
#endif
	if (Network_status == network_state::starting)
		return;

	Netgame.numplayers = N_players;
	Netgame.game_status = Network_status;

	Netgame.kills = kill_matrix;
	for (unsigned i = 0; i < MAX_PLAYERS; ++i) 
	{
		auto &plr = *vcplayerptr(i);
		Netgame.players[i].connected = plr.connected;
		auto &objp = *vcobjptr(plr.objnum);
		auto &player_info = objp.ctype.player_info;
		Netgame.killed[i] = player_info.net_killed_total;
		Netgame.player_kills[i] = player_info.net_kills_total;
#if DXX_BUILD_DESCENT == 2
		Netgame.player_score[i] = player_info.mission.score;
#endif
		Netgame.net_player_flags[i] = player_info.powerup_flags;
	}
	Netgame.team_kills = team_kills;
	Netgame.levelnum = Current_level_num;
}
}

namespace {

/*
 * Polling loop waiting for the level to start after LEVEL_READY was sent
 */
static int net_udp_sync_poll( newmenu *,const d_event &event, const unused_newmenu_userdata_t *)
{
	int rval{0};

	if (event.type != event_type::window_draw)
		return 0;
	net_udp_listen();

	// Leave if Host disconnects
	if (Netgame.players[0].connected == player_connection_status::disconnected)
		rval = -2;

	/* A join in progress whose snapshot does not come: give up rather
	 * than wait for ever (the host's connection may well be alive).
	 */
	if (net_v2::client_sync_timed_out())
		rval = -2;

	if (Network_status != network_state::waiting)	// Status changed to playing, exit the menu
		rval = -2;

	return rval;
}
static int net_udp_start_poll(newmenu *, const d_event &event, start_poll_menu_items *const items)
{
	if (event.type == event_type::key_command && key_is_copy(event_key_get(event)))
	{
		net_udp_copy_game_address();
		return 1;
	}
	if (event.type != event_type::window_draw)
		return 0;
	assert(Network_status == network_state::starting);

	auto &menus = items->m;
	const unsigned nitems = menus.size();
	menus[0].value = 1;
	range_for (auto &i, partial_range(menus, N_players, nitems))
		i.value = 0;

	const auto predicate = [](const newmenu_item &i) {
		return i.value;
	};
	const auto nm = std::count_if(menus.begin(), std::next(menus.begin(), nitems), predicate);
	if ( nm > Netgame.max_numplayers ) {
		nm_messagebox(menu_title{TXT_ERROR}, {TXT_OK}, "%s %d %s", TXT_SORRY_ONLY, Netgame.max_numplayers, TXT_NETPLAYERS_IN);
		// Turn off the last player highlighted
		for (int i = N_players; i > 0; i--)
			if (menus[i].value == 1) 
			{
				menus[i].value = 0;
				break;
			}
	}

	net_udp_listen();

	for (int i=0; i<N_players; i++ ) // fill this in always in case players change but not their numbers
	{
		const auto &&rankstr = GetRankStringWithSpace(Netgame.players[i].rank);
		snprintf(menus[i].text, 45, "%d. %s%s%-20s", i+1, rankstr.first, rankstr.second, static_cast<const char *>(Netgame.players[i].callsign));
	}

	const unsigned players_last_poll = items->get_player_count();
	if (players_last_poll == Netgame.numplayers)
		return 0;
	items->set_player_count(Netgame.numplayers);
	// A new player
	if (players_last_poll < Netgame.numplayers)
	{
		digi_play_sample (sound_effect::SOUND_HUD_MESSAGE,F1_0);
		if (N_players <= Netgame.max_numplayers)
			menus[N_players-1].value = 1;
	} 
	else	// One got removed...
	{
		digi_play_sample (sound_effect::SOUND_HUD_KILL,F1_0);
  
		const auto j = std::min(N_players, static_cast<unsigned>(Netgame.max_numplayers));
		/* Reset all the user's choices, since there is insufficient
		 * integration to move the checks based on the position of the
		 * departed player(s).  Without this reset, names would move up
		 * one line, but checkboxes would not.
		 */
		range_for (auto &i, partial_range(menus, j))
			i.value = 1;
		range_for (auto &i, partial_range(menus, j, N_players))
			i.value = 0;
		range_for (auto &i, partial_range(menus, N_players, players_last_poll))
		{
			/* The default format string is "%d. "
			 * For single digit numbers, [3] is the first character
			 * after the space.  For double digit numbers, [3] is the
			 * space.  Users cannot see the trailing space or its
			 * absence, so always overwrite [3].  This would break if
			 * the menu allowed more than 99 players.
			 */
			i.text[3] = 0;
			i.value = 0;
		}
	}
	return 0;
}

#if DXX_USE_TRACKER
#define DXX_UDP_MENU_TRACKER_OPTION(VERB)	\
	DXX_MENUITEM(VERB, CHECK, "Track this game on", opt_tracker, Netgame.Tracker) \
	DXX_MENUITEM(VERB, TEXT, tracker_addr_txt, opt_tracker_addr)	\
	DXX_MENUITEM(VERB, CHECK, "Enable tracker NAT hole punch", opt_tracker_nathp, TrackerNATWarned)	\

#else
#define DXX_UDP_MENU_TRACKER_OPTION(VERB)
#endif

#if DXX_BUILD_DESCENT == 1
#define D2X_UDP_MENU_OPTIONS(VERB)	\

#elif DXX_BUILD_DESCENT == 2
#define D2X_UDP_MENU_OPTIONS(VERB)	\
	DXX_MENUITEM(VERB, CHECK, "Allow Marker camera views", opt_marker_view, Netgame.Allow_marker_view)	\
	DXX_MENUITEM(VERB, CHECK, "Indestructible lights", opt_light, Netgame.AlwaysLighting)	\
	DXX_MENUITEM(VERB, CHECK, "Remove Thief at level start", opt_thief_presence, thief_absent)	\
	DXX_MENUITEM(VERB, CHECK, "Prevent Thief Stealing Energy Weapons", opt_thief_steal_energy, thief_cannot_steal_energy_weapons)	\
	DXX_MENUITEM(VERB, CHECK, "Allow Guidebot (coop only; experimental)", opt_guidebot_enabled, Netgame.AllowGuidebot)	\

#endif

constexpr std::integral_constant<unsigned, F1_0 * 60> reactor_invul_time_mini_scale{};
constexpr std::integral_constant<unsigned, 5 * reactor_invul_time_mini_scale> reactor_invul_time_scale{};

#if DXX_BUILD_DESCENT == 1
#define D2X_DUPLICATE_POWERUP_OPTIONS(VERB)	                           \

#elif DXX_BUILD_DESCENT == 2
#define D2X_DUPLICATE_POWERUP_OPTIONS(VERB)	                           \
	DXX_MENUITEM(VERB, SLIDER, extraAccessory, opt_extra_accessory, accessory, 0, (1 << packed_netduplicate_items::accessory_width) - 1)	\

#endif

#define DXX_DUPLICATE_POWERUP_OPTIONS(VERB)	                           \
	DXX_MENUITEM(VERB, SLIDER, extraPrimary, opt_extra_primary, primary, 0, (1 << packed_netduplicate_items::primary_width) - 1)	\
	DXX_MENUITEM(VERB, SLIDER, extraSecondary, opt_extra_secondary, secondary, 0, (1 << packed_netduplicate_items::secondary_width) - 1)	\
	D2X_DUPLICATE_POWERUP_OPTIONS(VERB)                                

#define DXX_UDP_MENU_OPTIONS(VERB)	                                    \
	DXX_MENUITEM(VERB, TEXT, "Game Options", game_label)	                     \
	DXX_MENUITEM(VERB, SLIDER, get_annotated_difficulty_string(Netgame.difficulty), opt_difficulty, difficulty, underlying_value(Difficulty_level_type::_0), underlying_value(Difficulty_level_type::_4))	\
	DXX_MENUITEM(VERB, SCALE_SLIDER, srinvul, opt_cinvul, Netgame.control_invul_time, 0, 10, reactor_invul_time_scale)	\
	DXX_MENUITEM(VERB, SLIDER, PlayText, opt_playtime, PlayTimeAllowed, 0, 12)	\
	DXX_MENUITEM(VERB, SLIDER, KillText, opt_killgoal, Netgame.KillGoal, 0, 20)	\
	DXX_MENUITEM(VERB, TEXT, "", blank_1)                                     \
	DXX_MENUITEM(VERB, TEXT, "Duplicate Powerups", duplicate_label)	          \
	DXX_DUPLICATE_POWERUP_OPTIONS(VERB)		                              \
	DXX_MENUITEM(VERB, TEXT, "", blank_5)                                     \
	DXX_MENUITEM(VERB, TEXT, "Spawn Options", spawn_label)	                   \
	DXX_MENUITEM(VERB, SLIDER, SecludedSpawnText, opt_secluded_spawns, Netgame.SecludedSpawns, 0, MAX_PLAYERS - 1)	\
	DXX_MENUITEM(VERB, SLIDER, SpawnInvulnerableText, opt_start_invul, Netgame.InvulAppear, 0, 8)	\
	DXX_MENUITEM(VERB, TEXT, "", blank_2)                                     \
	DXX_MENUITEM(VERB, TEXT, "Object Options", powerup_label)	                \
	DXX_MENUITEM(VERB, CHECK, "Shuffle powerups in anarchy games", opt_shuffle_powerups, Netgame.ShufflePowerupSeed)	\
	DXX_MENUITEM(VERB, MENU, "Set Objects allowed...", opt_setpower)	         \
	DXX_MENUITEM(VERB, MENU, "Set Objects granted at spawn...", opt_setgrant)	\
	DXX_MENUITEM(VERB, TEXT, "", blank_3)                                     \
	DXX_MENUITEM(VERB, TEXT, "Misc. Options", misc_label)	                    \
	DXX_MENUITEM(VERB, CHECK, TXT_SHOW_ON_MAP, opt_show_on_map, game_flag_show_all_players_on_automap)	\
	D2X_UDP_MENU_OPTIONS(VERB)	                                        \
	DXX_MENUITEM(VERB, CHECK, "Bright player ships", opt_bright, Netgame.BrightPlayers)	\
	DXX_MENUITEM(VERB, CHECK, "Show enemy names on HUD", opt_show_names, Netgame.ShowEnemyNames)	\
	DXX_MENUITEM(VERB, CHECK, "No friendly fire (Team, Coop)", opt_ffire, Netgame.NoFriendlyFire)	\
	DXX_MENUITEM(VERB, FCHECK, game_is_cooperative ? "Allow coop mouselook" : "Allow anarchy mouselook", opt_mouselook, Netgame.MouselookFlags, MouselookMPFlag(game_is_cooperative))	\
	DXX_MENUITEM(VERB, FCHECK, game_is_cooperative ? "Release coop pitch lock" : "Release anarchy pitch lock", opt_pitch_lock, Netgame.PitchLockFlags, MouselookMPFlag(game_is_cooperative))  \
	DXX_MENUITEM(VERB, TEXT, "", blank_4)                                     \
	DXX_MENUITEM_AUTOSAVE_LABEL_INPUT(VERB)	\
	DXX_MENUITEM(VERB, TEXT, "", blank_6)                                     \
	DXX_MENUITEM(VERB, TEXT, "Network Options", network_label)	               \
	DXX_MENUITEM(VERB, SLIDER, tickrate_text, opt_tickrate, tick_rate_index, 0, 2)	\
	DXX_MENUITEM(VERB, TEXT, "Network port", opt_label_port)	\
	DXX_MENUITEM(VERB, INPUT, portstring, opt_port)	\
	DXX_UDP_MENU_TRACKER_OPTION(VERB)

static unsigned MouselookMPFlag(const unsigned game_is_cooperative)
{
	return game_is_cooperative ? MouselookMode::MPCoop : MouselookMode::MPAnarchy;
}

/* The tick rate slider: 30, 60, 120 Hz (decision 1). */
static constexpr std::array<uint8_t, 3> tick_rate_choices{{30, 60, 120}};

static unsigned tick_rate_to_index(const unsigned rate)
{
	for (auto &&[i, r] : enumerate(tick_rate_choices))
		if (r == rate)
			return i;
	return 1;
}

struct netgame_powerups_allowed_menu_items
{
	std::array<newmenu_item, multi_allow_powerup_text.size()> m;
	netgame_powerups_allowed_menu_items()
	{
		const auto AllowedItems{underlying_value(Netgame.AllowedItems)};
		for (auto &&[i, t, mi] : enumerate(zip(multi_allow_powerup_text, m)))
			nm_set_item_checkbox(mi, t, AllowedItems & (1u << i));
	}
};

struct netgame_powerups_allowed_menu : netgame_powerups_allowed_menu_items, newmenu
{
	netgame_powerups_allowed_menu(grs_canvas &src) :
		newmenu(menu_title{nullptr}, menu_subtitle{"Objects to allow"}, menu_filename{nullptr}, tiny_mode_flag::normal, tab_processing_flag::ignore, adjusted_citem::create(m, 0), src)
	{
	}
	virtual window_event_result event_handler(const d_event &event) override;
};

window_event_result netgame_powerups_allowed_menu::event_handler(const d_event &event)
{
	switch (event.type)
	{
		case event_type::window_close:
			{
				typename std::underlying_type<netflag_flag>::type AllowedItems = 0;
				for (auto &&[i, mi] : enumerate(m))
					if (mi.value)
						AllowedItems |= (1 << i);
				Netgame.AllowedItems = netflag_flag{AllowedItems};
				break;
			}
		default:
			break;
	}
	return newmenu::event_handler(event);
}

static void net_udp_set_power (void)
{
	auto menu = window_create<netgame_powerups_allowed_menu>(grd_curscreen->sc_canvas);
	(void)menu;
}

#if DXX_BUILD_DESCENT == 1
#define D2X_GRANT_POWERUP_MENU(VERB)
#elif DXX_BUILD_DESCENT == 2
#define D2X_GRANT_POWERUP_MENU(VERB)	\
	DXX_MENUITEM(VERB, CHECK, NETFLAG_LABEL_GAUSS, opt_gauss, menu_bit_wrapper<netgrant_flag::NETGRANT_GAUSS>(flags))	\
	DXX_MENUITEM(VERB, CHECK, NETFLAG_LABEL_HELIX, opt_helix, menu_bit_wrapper<netgrant_flag::NETGRANT_HELIX>(flags))	\
	DXX_MENUITEM(VERB, CHECK, NETFLAG_LABEL_PHOENIX, opt_phoenix, menu_bit_wrapper<netgrant_flag::NETGRANT_PHOENIX>(flags))	\
	DXX_MENUITEM(VERB, CHECK, NETFLAG_LABEL_OMEGA, opt_omega, menu_bit_wrapper<netgrant_flag::NETGRANT_OMEGA>(flags))	\
	DXX_MENUITEM(VERB, CHECK, NETFLAG_LABEL_AFTERBURNER, opt_afterburner, menu_bit_wrapper<netgrant_flag::NETGRANT_AFTERBURNER>(flags))	\
	DXX_MENUITEM(VERB, CHECK, NETFLAG_LABEL_AMMORACK, opt_ammo_rack, menu_bit_wrapper<netgrant_flag::NETGRANT_AMMORACK>(flags))	\
	DXX_MENUITEM(VERB, CHECK, NETFLAG_LABEL_CONVERTER, opt_converter, menu_bit_wrapper<netgrant_flag::NETGRANT_CONVERTER>(flags))	\
	DXX_MENUITEM(VERB, CHECK, NETFLAG_LABEL_HEADLIGHT, opt_headlight, menu_bit_wrapper<netgrant_flag::NETGRANT_HEADLIGHT>(flags))	\

#endif

#define DXX_GRANT_POWERUP_MENU(VERB)	\
	DXX_MENUITEM(VERB, NUMBER, "Laser level", opt_laser_level, menu_number_bias_wrapper<1>(laser_level), static_cast<unsigned>(laser_level::_1) + 1, static_cast<unsigned>(DXX_MAXIMUM_LASER_LEVEL) + 1)	\
	DXX_MENUITEM(VERB, CHECK, NETFLAG_LABEL_QUAD, opt_quad_lasers, menu_bit_wrapper<netgrant_flag::NETGRANT_QUAD>(flags))	\
	DXX_MENUITEM(VERB, CHECK, NETFLAG_LABEL_VULCAN, opt_vulcan, menu_bit_wrapper<netgrant_flag::NETGRANT_VULCAN>(flags))	\
	DXX_MENUITEM(VERB, CHECK, NETFLAG_LABEL_SPREAD, opt_spreadfire, menu_bit_wrapper<netgrant_flag::NETGRANT_SPREAD>(flags))	\
	DXX_MENUITEM(VERB, CHECK, NETFLAG_LABEL_PLASMA, opt_plasma, menu_bit_wrapper<netgrant_flag::NETGRANT_PLASMA>(flags))	\
	DXX_MENUITEM(VERB, CHECK, NETFLAG_LABEL_FUSION, opt_fusion, menu_bit_wrapper<netgrant_flag::NETGRANT_FUSION>(flags))	\
	D2X_GRANT_POWERUP_MENU(VERB)

class more_game_options_menu_items
{
protected:
	const unsigned game_is_cooperative;
	char tickrate_text[sizeof("Tick rate: 120 Hz")];
	std::array<char, sizeof("65535")> portstring;
	/* Reactor life and Maximum time are stored in a uint32_t, and have
	 * a theoretical maximum of 1092 after converting from internal game
	 * time (seconds in fixed point) to minutes.  User input limitations
	 * prevent setting a value higher than 50 minutes.  Even if the code
	 * is modified to verify a maximum value of 50 immediately before
	 * formatting it, the gcc value range propagation pass fails to
	 * detect the reduced range and issues a warning as if the value
	 * could be 1092.  Eliminate the bogus warning by using a buffer
	 * large enough for the theoretical maximum.
	 */
	char srinvul[sizeof("Reactor life: 1092 min")];
	char PlayText[sizeof("Max time: 1092 min")];
	char SpawnInvulnerableText[sizeof("Invul. Time: 0.0 sec")];
	char SecludedSpawnText[sizeof("Use 0 Furthest Sites")];
	char KillText[sizeof("Kill goal: 000 kills")];
	char extraPrimary[sizeof("Primaries: 0")];
	char extraSecondary[sizeof("Secondaries: 0")];
#if DXX_BUILD_DESCENT == 2
	char extraAccessory[sizeof("Accessories: 0")];
#endif
#if DXX_USE_TRACKER
        char tracker_addr_txt[sizeof("65535") + 28];
#endif
	human_readable_mmss_time<decltype(d_gameplay_options::AutosaveInterval)::rep> AutosaveInterval;
	using menu_array = std::array<newmenu_item, DXX_UDP_MENU_OPTIONS(COUNT)>;
	DXX_UDP_MENU_OPTIONS(DECL);
	menu_array m;
	static const char *get_annotated_difficulty_string(const Difficulty_level_type d)
	{
		static constexpr enumerated_array<char[20], 5, Difficulty_level_type> text{{{
			"Difficulty: Trainee",
			"Difficulty: Rookie",
			"Difficulty: Hotshot",
			"Difficulty: Ace",
			"Difficulty: Insane"
		}}};
		switch (d)
		{
			case Difficulty_level_type::_0:
			case Difficulty_level_type::_1:
			case Difficulty_level_type::_2:
			case Difficulty_level_type::_3:
			case Difficulty_level_type::_4:
				return text[d];
			default:
				// Empty string at the end of `Ace`
				return &text[Difficulty_level_type::_3][16];
		}
	}
	static int handler(newmenu *, const d_event &event, more_game_options_menu_items *items);
public:
	menu_array &get_menu_items()
	{
		return m;
	}
	void update_difficulty_string(const Difficulty_level_type difficulty)
	{
		/* Cast away const because newmenu_item uses `char *text` even
		 * for fields where text is treated as `const char *`.
		 */
		m[opt_difficulty].text = const_cast<char *>(get_annotated_difficulty_string(difficulty));
	}
	void update_extra_primary_string(unsigned primary)
	{
		snprintf(extraPrimary, sizeof(extraPrimary), "Primaries: %u", primary);
	}
	void update_extra_secondary_string(unsigned secondary)
	{
		snprintf(extraSecondary, sizeof(extraSecondary), "Secondaries: %u", secondary);
	}
#if DXX_BUILD_DESCENT == 2
	void update_extra_accessory_string(unsigned accessory)
	{
		snprintf(extraAccessory, sizeof(extraAccessory), "Accessories: %u", accessory);
	}
#endif
	void update_tickrate_string(const unsigned index)
	{
		snprintf(tickrate_text, sizeof(tickrate_text), "Tick rate: %u Hz", tick_rate_choices[std::min<std::size_t>(index, tick_rate_choices.size() - 1)]);
	}
	void update_portstring()
	{
		snprintf(portstring.data(), portstring.size(), "%hu", UDP_MyPort);
	}
	void update_reactor_life_string(unsigned t)
	{
		snprintf(srinvul, sizeof(srinvul), "%s: %u %s", TXT_REACTOR_LIFE, t, TXT_MINUTES_ABBREV);
	}
	void update_max_play_time_string()
	{
		snprintf(PlayText, sizeof(PlayText), "Max time: %d %s", Netgame.PlayTimeAllowed.count() / (F1_0 * 60), TXT_MINUTES_ABBREV);
	}
	void update_spawn_invuln_string()
	{
		snprintf(SpawnInvulnerableText, sizeof(SpawnInvulnerableText), "Invul. Time: %1.1f sec", static_cast<float>(Netgame.InvulAppear) / 2);
	}
	void update_secluded_spawn_string()
	{
		const unsigned SecludedSpawns = Netgame.SecludedSpawns;
		cf_assert(SecludedSpawns < MAX_PLAYERS);
		snprintf(SecludedSpawnText, sizeof(SecludedSpawnText), "Use %u Furthest Sites", SecludedSpawns + 1);
	}
	void update_kill_goal_string()
	{
		snprintf(KillText, sizeof(KillText), "Kill Goal: %3d", Netgame.KillGoal * 5);
	}
	enum
	{
		DXX_UDP_MENU_OPTIONS(ENUM)
	};
	more_game_options_menu_items(const unsigned game_is_cooperative) :
		game_is_cooperative(game_is_cooperative),
		AutosaveInterval{build_human_readable_time(Netgame.MPGameplayOptions.AutosaveInterval)}
	{
		const auto edifficulty{Netgame.difficulty};
		const auto difficulty = underlying_value(edifficulty);
		update_difficulty_string(edifficulty);
		const unsigned tick_rate_index{tick_rate_to_index(Netgame.TickRate)};
		update_tickrate_string(tick_rate_index);
		update_portstring();
		update_reactor_life_string(Netgame.control_invul_time / reactor_invul_time_mini_scale);
		update_max_play_time_string();
		update_spawn_invuln_string();
		update_secluded_spawn_string();
		update_kill_goal_string();
		auto primary = Netgame.DuplicatePowerups.get_primary_count();
		auto secondary = Netgame.DuplicatePowerups.get_secondary_count();
#if DXX_BUILD_DESCENT == 2
		auto accessory = Netgame.DuplicatePowerups.get_accessory_count();
		const auto thief_absent = Netgame.ThiefModifierFlags & ThiefModifier::Absent;
		const auto thief_cannot_steal_energy_weapons = Netgame.ThiefModifierFlags & ThiefModifier::NoEnergyWeapons;
		update_extra_accessory_string(accessory);
#endif
		update_extra_primary_string(primary);
		update_extra_secondary_string(secondary);
#if DXX_USE_TRACKER
		const unsigned TrackerNATWarned = Netgame.TrackerNATWarned == TrackerNATHolePunchWarn::UserEnabledHP;
#endif
		const unsigned PlayTimeAllowed = std::chrono::duration_cast<std::chrono::duration<int, netgame_info::play_time_allowed_abi_ratio>>(Netgame.PlayTimeAllowed).count();
		const auto game_flag_show_all_players_on_automap{underlying_value(Netgame.game_flag & netgame_rule_flags::show_all_players_on_automap)};
		DXX_UDP_MENU_OPTIONS(ADD);
#if DXX_USE_TRACKER
		const auto &tracker_addr = CGameArg.MplTrackerAddr;
		if (tracker_addr.empty())
                {
			nm_set_item_text(m[opt_tracker], "Tracker use disabled");
                        nm_set_item_text(m[opt_tracker_addr], "<Tracker address not set>");
                }
		else
                {
			snprintf(tracker_addr_txt, sizeof(tracker_addr_txt), "%s:%u", tracker_addr.c_str(), CGameArg.MplTrackerPort);
                }
#endif
	}
	void read() const
	{
		unsigned primary, secondary;
#if DXX_BUILD_DESCENT == 2
		unsigned accessory;
		uint8_t thief_absent;
		uint8_t thief_cannot_steal_energy_weapons;
#endif
		uint8_t difficulty;
#if DXX_USE_TRACKER
		unsigned TrackerNATWarned;
#endif
		unsigned PlayTimeAllowed;
		uint8_t game_flag_show_all_players_on_automap;
		unsigned tick_rate_index;
		DXX_UDP_MENU_OPTIONS(READ);
		Netgame.TickRate = tick_rate_choices[std::min<std::size_t>(tick_rate_index, tick_rate_choices.size() - 1)];
		Netgame.difficulty = cast_clamp_difficulty(difficulty);
		Netgame.PlayTimeAllowed = std::chrono::duration<int, netgame_info::play_time_allowed_abi_ratio>(PlayTimeAllowed);
		if (game_flag_show_all_players_on_automap)
			Netgame.game_flag |= netgame_rule_flags::show_all_players_on_automap;
		else
			Netgame.game_flag &= ~netgame_rule_flags::show_all_players_on_automap;
		auto &items = Netgame.DuplicatePowerups;
		items.set_primary_count(primary);
		items.set_secondary_count(secondary);
#if DXX_BUILD_DESCENT == 2
		items.set_accessory_count(accessory);
		Netgame.ThiefModifierFlags =
			(thief_absent ? ThiefModifier::Absent : 0) |
			(thief_cannot_steal_energy_weapons ? ThiefModifier::NoEnergyWeapons : 0);
#endif
#if DXX_USE_TRACKER
		Netgame.TrackerNATWarned = TrackerNATWarned ? TrackerNATHolePunchWarn::UserEnabledHP : TrackerNATHolePunchWarn::UserRejectedHP;
#endif
		convert_text_portstring(portstring, UDP_MyPort, false, false);
		parse_human_readable_time(Netgame.MPGameplayOptions.AutosaveInterval, AutosaveInterval);
	}
};

struct more_game_options_menu : more_game_options_menu_items, newmenu
{
	more_game_options_menu(unsigned game_is_cooperative, grs_canvas &);
	static void net_udp_more_game_options(unsigned game_is_cooperative);
	virtual window_event_result event_handler(const d_event &event) override;
};

more_game_options_menu::more_game_options_menu(const unsigned game_is_cooperative, grs_canvas &canvas) :
	more_game_options_menu_items(game_is_cooperative),
	newmenu(menu_title{nullptr}, menu_subtitle{"Advanced netgame options"}, menu_filename{nullptr}, tiny_mode_flag::normal, tab_processing_flag::ignore, adjusted_citem::create(m, 0), canvas)
{
}

class grant_powerup_menu_items
{
public:
	enum
	{
		DXX_GRANT_POWERUP_MENU(ENUM)
	};
	std::array<newmenu_item, DXX_GRANT_POWERUP_MENU(COUNT)> m;
	grant_powerup_menu_items(const laser_level level, const packed_spawn_granted_items p)
	{
		auto &flags = p.mask;
		auto laser_level{static_cast<uint8_t>(level)};
		DXX_GRANT_POWERUP_MENU(ADD);
	}
	void read(packed_spawn_granted_items &p) const
	{
		uint8_t laser_level{};
		typename std::underlying_type<netgrant_flag>::type flags{};
		DXX_GRANT_POWERUP_MENU(READ);
		flags |= laser_level;
		p.mask = netgrant_flag{flags};
	}
};

struct grant_powerup_menu : grant_powerup_menu_items, newmenu
{
	grant_powerup_menu(const laser_level level, const packed_spawn_granted_items p, grs_canvas &src) :
		grant_powerup_menu_items(level, p),
		newmenu(menu_title{nullptr}, menu_subtitle{"Powerups granted at player spawn"}, menu_filename{nullptr}, tiny_mode_flag::normal, tab_processing_flag::ignore, adjusted_citem::create(m, 0), src)
	{
	}
	virtual window_event_result event_handler(const d_event &event) override;
};

window_event_result grant_powerup_menu::event_handler(const d_event &event)
{
	switch (event.type)
	{
		case event_type::window_close:
			read(Netgame.SpawnGrantedItems);
			break;
		default:
			break;
	}
	return newmenu::event_handler(event);
}

static void net_udp_set_grant_power()
{
	const auto SpawnGrantedItems{Netgame.SpawnGrantedItems};
	auto menu = window_create<grant_powerup_menu>(map_granted_flags_to_laser_level(SpawnGrantedItems), SpawnGrantedItems, grd_curscreen->sc_canvas);
	(void)menu;
}

void more_game_options_menu::net_udp_more_game_options(const unsigned game_is_cooperative)
{
	auto menu = window_create<more_game_options_menu>(game_is_cooperative, grd_curscreen->sc_canvas);
	(void)menu;
}

window_event_result more_game_options_menu::event_handler(const d_event &event)
{
	switch (event.type)
	{
		case event_type::newmenu_changed:
		{
			auto &citem = static_cast<const d_change_event &>(event).citem;
			auto &menus = m;
			if (citem == opt_difficulty)
			{
				Netgame.difficulty = cast_clamp_difficulty(menus[opt_difficulty].value);
				update_difficulty_string(Netgame.difficulty);
			}
			else if (citem == opt_cinvul)
				update_reactor_life_string(menus[opt_cinvul].value * (reactor_invul_time_scale / reactor_invul_time_mini_scale));
			else if (citem == opt_playtime)
			{
				if (game_is_cooperative)
				{
					nm_messagebox_str(menu_title{TXT_SORRY}, nm_messagebox_tie(TXT_OK), menu_subtitle{"You can't change those for coop!"});
					menus[opt_playtime].value=0;
					return window_event_result::ignored;
				}
				Netgame.PlayTimeAllowed = std::chrono::duration<int, netgame_info::play_time_allowed_abi_ratio>(menus[opt_playtime].value);
				update_max_play_time_string();
			}
			else if (citem == opt_killgoal)
			{
				if (game_is_cooperative)
				{
					nm_messagebox_str(menu_title{TXT_SORRY}, nm_messagebox_tie(TXT_OK), menu_subtitle{"You can't change those for coop!"});
					menus[opt_killgoal].value=0;
					return window_event_result::ignored;
				}
				Netgame.KillGoal=menus[opt_killgoal].value;
				update_kill_goal_string();
			}
			else if(citem == opt_extra_primary)
			{
				auto primary = menus[opt_extra_primary].value;
				update_extra_primary_string(primary);
			}
			else if(citem == opt_extra_secondary)
			{
				auto secondary = menus[opt_extra_secondary].value;
				update_extra_secondary_string(secondary);
			}
#if DXX_BUILD_DESCENT == 2
			else if(citem == opt_extra_accessory)
			{
				auto accessory = menus[opt_extra_accessory].value;
				update_extra_accessory_string(accessory);
			}
#endif
			else if (citem == opt_start_invul)
			{
				Netgame.InvulAppear = menus[opt_start_invul].value;
				update_spawn_invuln_string();
			}
			else if (citem == opt_secluded_spawns)
			{
				Netgame.SecludedSpawns = menus[opt_secluded_spawns].value;
				update_secluded_spawn_string();
			}
			else if (citem == opt_tickrate)
				update_tickrate_string(menus[opt_tickrate].value);
			break;
		}
		case event_type::newmenu_selected:
		{
			auto &citem = static_cast<const d_select_event &>(event).citem;
			if (citem == opt_setpower)
				net_udp_set_power();
			else if (citem == opt_setgrant)
				net_udp_set_grant_power();
			else
				break;
			return window_event_result::handled;
		}
		case event_type::window_close:
			read();
			GameUniqueState.Difficulty_level = Netgame.difficulty;
			break;
		default:
			break;
	}
	return newmenu::event_handler(event);
}

struct param_opt
{
	enum {
		name = 2,
		label_level,
		level,
	};
	int start_game, mode, mode_end, moreopts, bots;
	int closed, refuse, maxnet, anarchy, team_anarchy, robot_anarchy, coop, bounty;
#if DXX_BUILD_DESCENT == 2
	int capture, hoard, team_hoard;
#endif
	std::array<char, sizeof("S100")> slevel{{"1"}};
	char srmaxnet[sizeof("Maximum players: 99")];
	char sbots[48];
	ntstring<NM_MAX_TEXT_LEN> max_numplayers_saved_text;
	std::array<newmenu_item, 23> m;
	void update_bots_label()
	{
		bots_setup_label(sbots, sizeof(sbots), Netgame.gamemode);
	}
	void update_netgame_max_players()
	{
		Netgame.max_numplayers = m[maxnet].value + 2;
		update_max_players_string();
	}
	void update_max_players_string()
	{
		const unsigned max_numplayers = Netgame.max_numplayers;
		cf_assert(max_numplayers < MAX_PLAYERS);
		snprintf(srmaxnet, sizeof(srmaxnet), "Maximum players: %u", max_numplayers);
	}
};

static int net_udp_game_param_handler( newmenu *menu,const d_event &event, param_opt *opt )
{
	newmenu_item *menus = newmenu_get_items(menu);
	switch (event.type)
	{
		case event_type::newmenu_changed:
		{
			auto &citem = static_cast<const d_change_event &>(event).citem;
#if DXX_BUILD_DESCENT == 1
			if (citem == opt->team_anarchy)
			{
				menus[opt->closed].value = 1;
				menus[opt->closed-1].value = 0;
				menus[opt->closed+1].value = 0;
			}
#elif DXX_BUILD_DESCENT == 2
			if (((HoardEquipped() != hoard_availability_state::Missing && (citem == opt->team_hoard)) || ((citem == opt->team_anarchy) || (citem == opt->capture))) && !menus[opt->closed].value && !menus[opt->refuse].value)
			{
				menus[opt->refuse].value = 1;
				menus[opt->refuse-1].value = 0;
				menus[opt->refuse-2].value = 0;
			}
#endif
			
			if (menus[opt->coop].value)
			{
				/* In cooperative games, always show all players on the map,
				 * regardless of what the host requested.
				 */
				Netgame.game_flag |= netgame_rule_flags::show_all_players_on_automap;

				Netgame.PlayTimeAllowed = {};
				Netgame.KillGoal = 0;
			}
			if (citem == opt->level)
			{
				auto &slevel = opt->slevel;
#if DXX_BUILD_DESCENT == 1
				if (tolower(static_cast<unsigned>(slevel[0])) == 's')
					Netgame.levelnum = -strtol(&slevel[1], 0, 0);
				else
#endif
					Netgame.levelnum = strtol(slevel.data(), 0, 0);
			}
			
			if (citem == opt->maxnet)
			{
				opt->update_netgame_max_players();
			}

			if ((citem >= opt->mode) && (citem <= opt->mode_end))
			{
				if ( menus[opt->anarchy].value )
					Netgame.gamemode = network_game_type::anarchy;
				
				else if (menus[opt->team_anarchy].value) {
					Netgame.gamemode = network_game_type::team_anarchy;
				}
#if DXX_BUILD_DESCENT == 2
				else if (menus[opt->capture].value)
					Netgame.gamemode = network_game_type::capture_flag;
				else if (const auto hoard{HoardEquipped()}; hoard != hoard_availability_state::Missing && menus[opt->hoard].value)
					Netgame.gamemode = network_game_type::hoard;
				else if (hoard != hoard_availability_state::Missing && menus[opt->team_hoard].value)
					Netgame.gamemode = network_game_type::team_hoard;
#endif
				else if( menus[opt->bounty].value )
					Netgame.gamemode = network_game_type::bounty;
		 		else if (ANARCHY_ONLY_MISSION) {
					int i{0};
		 			nm_messagebox_str(menu_title{nullptr}, nm_messagebox_tie(TXT_OK), menu_subtitle{TXT_ANARCHY_ONLY_MISSION});
					for (i = opt->mode; i <= opt->mode_end; i++)
						menus[i].value = 0;
					menus[opt->anarchy].value = 1;
		 			return 0;
		 		}
				else if ( menus[opt->robot_anarchy].value ) 
					Netgame.gamemode = network_game_type::robot_anarchy;
				else if ( menus[opt->coop].value ) 
					Netgame.gamemode = network_game_type::cooperative;
				else Int3(); // Invalid mode -- see Rob
			}

			if (menus[opt->closed].value)
				Netgame.game_flag |= netgame_rule_flags::closed;
			else
				Netgame.game_flag &= ~netgame_rule_flags::closed;
			Netgame.RefusePlayers=menus[opt->refuse].value;
			opt->update_bots_label();
			break;
		}
		case event_type::newmenu_selected:
		{
			auto &citem = static_cast<const d_select_event &>(event).citem;
#if DXX_BUILD_DESCENT == 1
			if (Netgame.levelnum < Current_mission->last_secret_level || Netgame.levelnum > Current_mission->last_level || Netgame.levelnum == 0)
#elif DXX_BUILD_DESCENT == 2
			if (Netgame.levelnum < 1 || Netgame.levelnum > Current_mission->last_level)
#endif
			{
				auto &slevel = opt->slevel;
				strcpy(slevel.data(), "1");
				window_create<passive_messagebox>(menu_title{TXT_ERROR}, menu_subtitle{TXT_LEVEL_OUT_RANGE}, TXT_OK, grd_curscreen->sc_canvas);
				return 1;
			}

			if (citem==opt->moreopts)
			{
				more_game_options_menu::net_udp_more_game_options(menus[opt->coop].value);
				return 1;
			}
			if (citem == opt->bots)
			{
				bots_setup_menu(Netgame.gamemode, Netgame.max_numplayers);
				opt->update_bots_label();
				return 1;
			}
			if (citem==opt->start_game)
				return !net_udp_start_game();
			return 1;
		}
		default:
			break;
	}
	
	return 0;
}

}


namespace dsx {

namespace {

/* A new game hosted here: the defaults, then the pilot's netgame
 * profile, for the current mission.
 */
static void net_udp_setup_defaults()
{
	multi_new_game();

	change_playernum_to(0);

	{
		const player *const self = &get_local_player();
		range_for (auto &i, Players)
			if (&i != self)
				i.callsign = {};
	}

	Netgame.max_numplayers = MAX_PLAYERS;
	Netgame.KillGoal=0;
	Netgame.PlayTimeAllowed = {};
#if DXX_BUILD_DESCENT == 1
	Netgame.RefusePlayers=0;
#elif DXX_BUILD_DESCENT == 2
	Netgame.Allow_marker_view=1;
	Netgame.ThiefModifierFlags = 0;
#endif
	Netgame.difficulty=PlayerCfg.DefaultDifficulty;
	snprintf(Netgame.game_name.data(), Netgame.game_name.size(), "%s%s", static_cast<const char *>(InterfaceUniqueState.PilotName), TXT_S_GAME);
	reset_UDP_MyPort();
	Netgame.ShufflePowerupSeed = 0;
	Netgame.BrightPlayers = 1;
	Netgame.InvulAppear = 4;
	Netgame.SecludedSpawns = MAX_PLAYERS - 1;
	Netgame.AllowedItems = Netgame.MaskAllKnownAllowedItems;
	Netgame.TickRate = NETGAME_TICK_RATE_DEFAULT;
	Netgame.NoFriendlyFire = 0;
	Netgame.MouselookFlags = 0;
	Netgame.PitchLockFlags = 0;

#if DXX_USE_TRACKER
	Netgame.Tracker = 1;
#endif

	read_netgame_profile(&Netgame);
	if (!netgame_tick_rate_valid(Netgame.TickRate))
		Netgame.TickRate = NETGAME_TICK_RATE_DEFAULT;

#if DXX_BUILD_DESCENT == 2
	if (HoardEquipped() == hoard_availability_state::Missing && (Netgame.gamemode == network_game_type::hoard || Netgame.gamemode == network_game_type::team_hoard)) // did we restore a hoard mode but don't have hoard installed right now? then fall back to anarchy!
		Netgame.gamemode = network_game_type::anarchy;
#endif

	Netgame.mission_name.copy_if(&*Current_mission->filename, Netgame.mission_name.size());
	Netgame.mission_title = Current_mission->mission_name;
}

}

window_event_result net_udp_setup_game(const d_select_event &)
{
	param_opt opt;
	auto &m = opt.m;
	char level_text[32];

	net_udp_init();
	net_udp_setup_defaults();
	Netgame.levelnum = 1;

	unsigned optnum{0};
	opt.start_game=optnum;
	nm_set_item_menu(  m[optnum], "Start Game"); optnum++;
	nm_set_item_text(m[optnum], TXT_DESCRIPTION); optnum++;

	nm_set_item_input(m[optnum], Netgame.game_name); optnum++;

#define DXX_LEVEL_FORMAT_LEADER	"%s (1-%d"
#define DXX_LEVEL_FORMAT_TRAILER	")"
#if DXX_BUILD_DESCENT == 1
	if (Current_mission->last_secret_level == -1)
		/* Exactly one secret level */
		snprintf(level_text, sizeof(level_text), DXX_LEVEL_FORMAT_LEADER ", S1" DXX_LEVEL_FORMAT_TRAILER, TXT_LEVEL_, Current_mission->last_level);
	else if (Current_mission->last_secret_level)
		/* More than one secret level */
		snprintf(level_text, sizeof(level_text), DXX_LEVEL_FORMAT_LEADER ", S1-S%d" DXX_LEVEL_FORMAT_TRAILER, TXT_LEVEL_, Current_mission->last_level, -Current_mission->last_secret_level);
	else
		/* No secret levels */
#endif
		snprintf(level_text, sizeof(level_text), DXX_LEVEL_FORMAT_LEADER DXX_LEVEL_FORMAT_TRAILER, TXT_LEVEL_, Current_mission->last_level);
#undef DXX_LEVEL_FORMAT_TRAILER
#undef DXX_LEVEL_FORMAT_LEADER

	nm_set_item_text(m[optnum], level_text); optnum++;

	nm_set_item_input(m[optnum], opt.slevel); optnum++;
	nm_set_item_text(m[optnum], TXT_OPTIONS); optnum++;

	opt.mode = optnum;
	nm_set_item_radio(m[optnum], TXT_ANARCHY, Netgame.gamemode == network_game_type::anarchy, 0); opt.anarchy=optnum; optnum++;
	nm_set_item_radio(m[optnum], TXT_TEAM_ANARCHY, Netgame.gamemode == network_game_type::team_anarchy, 0); opt.team_anarchy=optnum; optnum++;
	nm_set_item_radio(m[optnum], TXT_ANARCHY_W_ROBOTS, Netgame.gamemode == network_game_type::robot_anarchy, 0); opt.robot_anarchy=optnum; optnum++;
	nm_set_item_radio(m[optnum], TXT_COOPERATIVE, Netgame.gamemode == network_game_type::cooperative, 0); opt.coop=optnum; optnum++;
#if DXX_BUILD_DESCENT == 2
	nm_set_item_radio(m[optnum], "Capture the flag", Netgame.gamemode == network_game_type::capture_flag, 0); opt.capture=optnum; optnum++;

	if (HoardEquipped() != hoard_availability_state::Missing)
	{
		nm_set_item_radio(m[optnum], "Hoard", Netgame.gamemode == network_game_type::hoard, 0); opt.hoard=optnum; optnum++;
		nm_set_item_radio(m[optnum], "Team Hoard", Netgame.gamemode == network_game_type::team_hoard, 0); opt.team_hoard=optnum; optnum++;
	}
	else
	{
		opt.hoard = opt.team_hoard = 0; // NOTE: Make sure if you use these, use them in connection with HoardEquipped() only!
	}
#endif
	nm_set_item_radio(m[optnum], "Bounty", Netgame.gamemode == network_game_type::bounty, 0); opt.mode_end=opt.bounty=optnum; optnum++;

	nm_set_item_text(m[optnum], ""); optnum++;

	const auto closed{underlying_value(Netgame.game_flag & netgame_rule_flags::closed)};
	nm_set_item_radio(m[optnum], "Open game", !Netgame.RefusePlayers && closed, 1); optnum++;
	opt.closed = optnum;
	nm_set_item_radio(m[optnum], TXT_CLOSED_GAME, closed, 1); optnum++;
	opt.refuse = optnum;
	nm_set_item_radio(m[optnum], "Restricted Game              ",Netgame.RefusePlayers,1); optnum++;

	opt.maxnet = optnum;
	opt.update_max_players_string();
	nm_set_item_slider(m[optnum], opt.srmaxnet, Netgame.max_numplayers - 2, 0, Netgame.max_numplayers - 2, opt.max_numplayers_saved_text); optnum++;

	/* Documentation/multiplayer-bots.md section 6.1. */
	bots_setup_init();
	opt.bots = optnum;
	opt.update_bots_label();
	nm_set_item_menu(m[optnum], opt.sbots); optnum++;
	
	opt.moreopts=optnum;
	nm_set_item_menu(  m[optnum], "Advanced Options"); optnum++;

	Assert(optnum <= 21);

#if DXX_USE_TRACKER
	if (Netgame.TrackerNATWarned == TrackerNATHolePunchWarn::Unset)
	{
		const unsigned choice = nm_messagebox_str(menu_title{"NAT Hole Punch"}, nm_messagebox_tie("Yes, let Internet users join", "No, I will configure my router"),
menu_subtitle{"Rebirth now supports automatic\n"
"NAT hole punch through the\n"
"tracker.\n\n"
"This allows Internet users to\n"
"join your game, even if you do\n"
"not configure your router for\n"
"hosting.\n\n"
"Do you want to use this feature?"});
		if (choice <= 1)
			Netgame.TrackerNATWarned = static_cast<TrackerNATHolePunchWarn>(choice + 1);
	}
#endif

	const int i = newmenu_do2(menu_title{nullptr}, menu_subtitle{TXT_NETGAME_SETUP}, unchecked_partial_range(m, optnum), net_udp_game_param_handler, &opt, opt.start_game);

	if (i < 0)
		net_udp_close();

	write_netgame_profile(&Netgame);
#if DXX_USE_TRACKER
	/* Force off _after_ writing profile, so that command line does not
	 * change ngp file.
	 */
	if (CGameArg.MplTrackerAddr.empty())
		Netgame.Tracker = 0;
#endif

	return (i >= 0) ? window_event_result::close : window_event_result::handled;
}

namespace {

static void net_udp_set_game_mode(const network_game_type gamemode)
{
	Show_kill_list = show_kill_list_mode::_1;

	if (gamemode == network_game_type::anarchy)
		Game_mode = game_mode_flags::anarchy_no_robots;
	else if (gamemode == network_game_type::robot_anarchy)
		Game_mode = game_mode_flags::anarchy_with_robots;
	else if (gamemode == network_game_type::cooperative) 
		Game_mode = game_mode_flags::cooperative;
#if DXX_BUILD_DESCENT == 2
	else if (gamemode == network_game_type::capture_flag)
		{
		 Game_mode = game_mode_flags::capture_flag;
		Show_kill_list = show_kill_list_mode::team_kills;
		}
	else if (const auto hoard{HoardEquipped()}; hoard != hoard_availability_state::Missing && gamemode == network_game_type::hoard)
		Game_mode = game_mode_flags::hoard;
	else if (hoard != hoard_availability_state::Missing && gamemode == network_game_type::team_hoard)
		 {
		Game_mode = game_mode_flags::team_hoard;
 		Show_kill_list = show_kill_list_mode::team_kills;
		 }
#endif
	else if (gamemode == network_game_type::bounty)
		Game_mode = game_mode_flags::bounty;
	else if (gamemode == network_game_type::team_anarchy)
	{
		Game_mode = game_mode_flags::team_anarchy_no_robots;
		Show_kill_list = show_kill_list_mode::team_kills;
	}
	else
		Int3();
}
}
}

namespace {

/* Level start, host side (section 4.3): shuffle the start positions,
 * send LEVEL_START to every ready player and apply it locally.
 */
static int net_udp_send_sync(void)
{
	const auto supported_start_positions_on_level{NumNetPlayerPositions};
	// Check if there are enough starting positions
	if (supported_start_positions_on_level < Netgame.max_numplayers)
	{
		nm_messagebox(menu_title{TXT_ERROR}, {TXT_OK}, "Not enough start positions\n(set %d got %d)\nNetgame aborted", Netgame.max_numplayers, supported_start_positions_on_level);
		// Tell everyone we're bailing
		Netgame.numplayers = 0;
		for (unsigned i = 1; i < N_players; ++i)
		{
			if (vcplayerptr(i)->connected == player_connection_status::disconnected || bot_is_local(i))
				continue;
			multi::udp::dispatch->kick_player(Netgame.players[i].protocol.udp.addr, kick_player_reason::aborted);
		}
		net_v2::host_broadcast_game_info_lite();
		return -1;
	}

	// Randomize their starting locations...
	d_srand(static_cast<fix>(timer_query()));
	for (auto &plr : partial_range(Players, supported_start_positions_on_level))
		/* Get rid of endlevel connect statuses.  A player still `waiting`
		 * (the host started without waiting for it) stays so: it gets no
		 * LEVEL_START and enters as a join in progress once it reports the
		 * level loaded.
		 */
		if (auto &connected = plr.connected; connected != player_connection_status::disconnected && connected != player_connection_status::waiting)
			connected = player_connection_status::playing;
	auto &&locations = partial_range(Netgame.locations, supported_start_positions_on_level);
	std::iota(locations.begin(), locations.end(), 0);
	if (!(Game_mode & GM_MULTI_COOP))
	{
		/* In cooperative games, use the locations in sequential order.
		 * In non-cooperative games, shuffle.
		 *
		 * High quality randomness is not required here.  Anything that
		 * seems random to users replaying a level should suffice.
		 */
		std::minstd_rand mrd(timer_query());
		std::shuffle(locations.begin(), locations.end(), mrd);
	}

	// Push current data into the level start messages

	net_udp_update_netgame();
	Netgame.game_status = network_state::playing;
	Netgame.segments_checksum = my_segments_checksum;

	net_v2::host_send_level_start();
	net_v2::apply_level_go(); // Apply it myself, as if I had received it
	return 0;
}

}
namespace dsx {
namespace {
/* The host is player 0. */
static void net_udp_host_takes_slot0()
{
	auto &Objects = LevelUniqueObjectState.Objects;
	auto &vmobjptr = Objects.vmptr;
	auto &host = Netgame.players[0];
	host.callsign = InterfaceUniqueState.PilotName;
	host.protocol.udp.addr = {};
	host.rank = GetMyNetRanking();
	host.connected = player_connection_status::playing;
	host.LastPacketTime = timer_query();
	vmobjptr(vcplayerptr(0u)->objnum)->ctype.player_info.KillGoalCount = 0;
	vmplayerptr(0u)->connected = player_connection_status::playing;
	N_players = 1;
	Netgame.numplayers = N_players;
	net_v2::host_send_netgame_update();
}

static int net_udp_select_players()
{
	int j;
	char text[MAX_PLAYERS+4][45];
	char subtitle[96];
	unsigned save_nplayers;              //how may people would like to join

	if (Netgame.ShufflePowerupSeed)
	{
		unsigned seed{0};
		try {
			seed = std::random_device()();
			if (!seed)
				/* random_device can return any number, including zero.
				 * Rebirth treats zero specially, interpreting it as a
				 * request not to shuffle.  Prevent a zero from
				 * random_device being interpreted as a request not to
				 * shuffle.
				 */
				seed = 1;
		} catch (const std::exception &e) {
			con_printf(CON_URGENT, "Failed to generate random number: %s", e.what());
			/* Fall out without setting `seed`, so that the option is
			 * disabled until the user notices the message and
			 * resolves the problem.
			 */
		}
		Netgame.ShufflePowerupSeed = seed;
	}

	net_udp_host_takes_slot0();
	start_poll_menu_items spd;
		
	for (int i=0; i< MAX_PLAYERS+4; i++ ) {
		snprintf(text[i], sizeof(text[i]), "%d. ", i + 1);
		nm_set_item_checkbox(spd.m[i], text[i], 0);
	}

	spd.m[0].value = 1;                         // Assume server will play...

	const auto &&rankstr = GetRankStringWithSpace(Netgame.players[Player_num].rank);
	snprintf( text[0], sizeof(text[0]), "%d. %s%s%-20s", 1, rankstr.first, rankstr.second, static_cast<const char *>(get_local_player().callsign));

	snprintf(subtitle, sizeof(subtitle), "%s %d %s\nCtrl+C: copy game address", TXT_TEAM_SELECT, Netgame.max_numplayers, TXT_TEAM_PRESS_ENTER);

#if DXX_USE_TRACKER
	if( Netgame.Tracker )
		net_v2::tracker_register();
#endif

GetPlayersAgain:
	j = newmenu_do2(menu_title{nullptr}, menu_subtitle{subtitle}, spd.m, net_udp_start_poll, &spd, 1);

	save_nplayers = N_players;

	if (j<0) 
	{
		// Aborted!
		// Dump all players and go back to menu mode
abort:
		// Tell everyone we're bailing
		for (unsigned i = 1; i < save_nplayers; ++i)
		{
			if (vcplayerptr(i)->connected == player_connection_status::disconnected || bot_is_local(i))
				continue;
			multi::udp::dispatch->kick_player(Netgame.players[i].protocol.udp.addr, kick_player_reason::aborted);
		}
		Netgame.numplayers = 0;
		net_v2::host_broadcast_game_info_lite();
		Netgame.numplayers = save_nplayers;

		Network_status = network_state::menu;
#if DXX_USE_TRACKER
		if( Netgame.Tracker )
			net_v2::tracker_unregister();
#endif
		return(0);
	}
	// Count number of players chosen

	N_players = 0;
	/* Slots are not renumbered, so the highest selected slot must be below
	 * the player limit too (the lobby admits no player above it; this
	 * keeps the invariant should that ever change).
	 */
	bool slot_beyond_limit{false};
	for (auto &&[idx, i] : enumerate(partial_const_range(spd.m, save_nplayers)))
	{
		if (i.value)
		{
			N_players++;
			if (idx >= Netgame.max_numplayers)
				slot_beyond_limit = true;
		}
	}
	
	if (N_players > Netgame.max_numplayers || slot_beyond_limit) {
		nm_messagebox(menu_title{TXT_ERROR}, {TXT_OK}, "%s %d %s", TXT_SORRY_ONLY, Netgame.max_numplayers, TXT_NETPLAYERS_IN);
		N_players = save_nplayers;
		goto GetPlayersAgain;
	}

// Let host join without Client available. Let's see if our players like that
	/* Remove players that aren't marked.  Their slots stay empty: every
	 * peer's player number is fixed by its JOIN_ACCEPT, so the list is
	 * not renumbered.
	 */
	N_players = save_nplayers;
	for (int i=1; i<save_nplayers; i++ )
	{
		if (!spd.m[i].value)
			multi::udp::dispatch->kick_player(Netgame.players[i].protocol.udp.addr, kick_player_reason::dork);
	}
	while (N_players > 1 && vcplayerptr(N_players - 1)->connected == player_connection_status::disconnected && !Netgame.players[N_players - 1].callsign[0u])
		--N_players;
	Netgame.numplayers = N_players;

	range_for (auto &i, partial_range(Netgame.players, N_players, Netgame.players.size()))
	{
		i.callsign = {};
		i.rank = netplayer_info::player_rank::None;
	}
	/* The bots take the free slots below the player limit
	 * (Documentation/multiplayer-bots.md section 2.3).
	 */
	if (const auto placed{bots_allocate_slots()}; placed < Bot_setup.count && bots_allowed_in_mode(Netgame.gamemode))
		nm_messagebox(menu_title{nullptr}, {TXT_OK}, "Only %u of %u bots fit\nbelow the player limit.", placed, Bot_setup.count);

#if DXX_BUILD_DESCENT == 1
	if (Netgame.gamemode == network_game_type::team_anarchy)
#elif DXX_BUILD_DESCENT == 2
	if (Netgame.gamemode == network_game_type::team_anarchy ||
	    Netgame.gamemode == network_game_type::capture_flag ||
		Netgame.gamemode == network_game_type::team_hoard)
#endif
		 if (run_blocking_newmenu<net_udp_select_teams_menu>(N_players, *grd_curcanv) == -1)
			goto abort;
	return(1);
}
}
}
namespace {

static int net_udp_start_game()
{
	if (!net_v2::open_socket(0, UDP_MyPort))
		return 0;

	if (UDP_MyPort != UDP_PORT_DEFAULT)
		if (!net_v2::open_socket(1, UDP_PORT_DEFAULT)) // Default port open for Broadcasts
			return 0;

	net_v2::host_open_session();

	N_players = 0;
	Netgame.game_status = network_state::starting;
	Netgame.numplayers = 0;

	Network_status = network_state::starting;

	net_udp_set_game_mode(Netgame.gamemode);

	Netgame.protocol.udp.your_index = 0; // I am Host. I need to know that y'know? For syncing later.

	if (!net_udp_select_players()
		|| StartNewLevel(Netgame.levelnum) == window_event_result::close)
	{
		Game_mode = {};
		return 0;	// see if we want to tweak the game we setup
	}
	state_set_next_autosave(GameUniqueState, Netgame.MPGameplayOptions.AutosaveInterval);
	net_v2::host_broadcast_game_info_lite(); // game started. broadcast our current status to everyone who wants to know

	return 1;	// don't keep params menu or mission listbox (may want to join a game next time)
}

}

namespace dsx {

void net_udp_arena_prepare(const unsigned level, const unsigned bots)
{
#if DXX_USE_TRACKER
	/* No tracker: not even its address is looked up (net_udp_init). */
	CGameArg.MplTrackerAddr.clear();
#endif
	net_udp_init();
	net_udp_setup_defaults();
	/* The pilot's profile gave the game options (powerups, invulnerability
	 * after a respawn, tick rate...).  The arena plays anarchy on its
	 * level, without end, unannounced.
	 */
	Netgame.gamemode = network_game_type::anarchy;
	Netgame.levelnum = level;
	Netgame.max_numplayers = std::min<unsigned>(bots + 1, MAX_PLAYERS);
	Netgame.KillGoal = 0;
	Netgame.PlayTimeAllowed = {};
	Netgame.MPGameplayOptions.AutosaveInterval = {};
#if DXX_USE_TRACKER
	Netgame.Tracker = 0;
#endif
}

bool net_udp_arena_start(const uint32_t seed)
{
	const unsigned bots{Bot_setup.count};
	if (Netgame.ShufflePowerupSeed)
		Netgame.ShufflePowerupSeed = seed;
	if (!net_v2::open_loopback_socket())
		return false;
	net_v2::host_open_session(seed);
	N_players = 0;
	Netgame.game_status = network_state::starting;
	Netgame.numplayers = 0;
	Network_status = network_state::starting;
	net_udp_set_game_mode(Netgame.gamemode);
	Netgame.protocol.udp.your_index = 0;

	net_udp_host_takes_slot0();
	range_for (auto &i, partial_range(Netgame.players, N_players, Netgame.players.size()))
	{
		i.callsign = {};
		i.rank = netplayer_info::player_rank::None;
	}
	if (const auto placed{bots_allocate_slots()}; placed < bots)
		con_printf(CON_URGENT, "botarena: only %u of %u bots fit", placed, bots);
	if (StartNewLevel(Netgame.levelnum) == window_event_result::close)
	{
		Game_mode = {};
		return false;
	}
	return true;
}

}

namespace {

/* Level start, client side (section 4.3): tell the host the level is
 * loaded and wait for LEVEL_GO.
 */
static int net_udp_wait_for_sync(void)
{
	char text[60];
	int choice{0};

	Network_status = network_state::waiting;

	std::array<newmenu_item, 2> m{{
		newmenu_item::nm_item_text{text},
		newmenu_item::nm_item_text{TXT_NET_LEAVE},
	}};
	net_v2::client_send_level_ready();

	snprintf(text, sizeof(text), "%s\n'%s' %s", TXT_NET_WAITING, static_cast<const char *>(Netgame.players[0].callsign), TXT_NET_TO_ENTER );

	while (choice > -1)
	{
		timer_update();
		choice = newmenu_do2(menu_title{nullptr}, menu_subtitle{TXT_WAIT}, m, net_udp_sync_poll, unused_newmenu_userdata);
	}

	if (Network_status != network_state::playing)
	{
		if (net_v2::client_sync_timed_out())
		{
			net_v2::client_send_leave(kick_player_reason::snapshot_failed);
			nm_messagebox_str(menu_title{TXT_ERROR}, nm_messagebox_tie(TXT_OK), menu_subtitle{"Failed to join the netgame.\nThe host did not send the\ngame state in time.\nTry joining again."});
		}
		else
			net_v2::client_send_leave(kick_player_reason::cancelled);
		N_players = 0;
		Game_mode = {};
		return(-1);     // they cancelled
	}
	return(0);
}

static int net_udp_request_poll( newmenu *,const d_event &event, const unused_newmenu_userdata_t *)
{
	// Polling loop for waiting-for-requests menu
	int num_ready{0};

	if (event.type != event_type::window_draw)
		return 0;
	net_udp_listen();

	range_for (auto &i, partial_const_range(Players, N_players))
	{
		if (i.connected == player_connection_status::playing || i.connected == player_connection_status::disconnected)
			num_ready++;
	}

	if (num_ready == N_players) // All players have checked in or are disconnected
	{
		timer_delay_ms(50);
		return -2;
	}

	return 0;
}

static int net_udp_wait_for_requests(void)
{
	// Wait for other players to load the level before we send the sync
	int choice;
	std::array<newmenu_item, 1> m{{
		newmenu_item::nm_item_text{TXT_NET_LEAVE},
	}};
	Network_status = network_state::waiting;

	get_local_player().connected = player_connection_status::playing;
	net_v2::host_begin_level_wait();

menu:
	choice = newmenu_do2(menu_title{nullptr}, menu_subtitle{TXT_WAIT}, m, net_udp_request_poll, unused_newmenu_userdata);

	if (choice == -1)
	{
		// User aborted
		choice = nm_messagebox_str(menu_title{nullptr}, nm_messagebox_tie(TXT_YES, TXT_NO, TXT_START_NOWAIT), menu_subtitle{TXT_QUITTING_NOW});
		if (choice == 2)
			/* Start without waiting: the players that are not ready stay
			 * `waiting` (they keep their slots, so N_players is unchanged)
			 * and join the level in progress when they are.
			 */
			return 0;
		if (choice != 0)
			goto menu;

		// User confirmed abort

		for (unsigned i = 0; i < N_players; ++i)
		{
			if (vcplayerptr(i)->connected != player_connection_status::disconnected && i != Player_num && !bot_is_local(i))
			{
				multi::udp::dispatch->kick_player(Netgame.players[i].protocol.udp.addr, kick_player_reason::aborted);
			}
		}
		return -1;
	}
	else if (choice != -2)
		goto menu;

	return 0;
}

}

namespace dsx {
namespace multi {
namespace udp {
/* Do required syncing after each level, before starting new one */
window_event_result dispatch_table::level_sync() const
{
	int result{0};

	if (multi_i_am_master() && N_players != 0)
	{
		result = net_udp_wait_for_requests();
		if (!result)
			result = net_udp_send_sync();
	}
	else
		result = net_udp_wait_for_sync();

	if (result)
	{
		get_local_player().connected = player_connection_status::disconnected;
		dispatch->send_endlevel_packet();
		show_menus();
		net_udp_close();
		return window_event_result::close;
	}
	return window_event_result::handled;
}
}
}
}

namespace dsx {
namespace {

/* The checks a player makes before asking to join (mission available,
 * level limits, game state).
 */
bool net_udp_join_precheck()
{
	if (Netgame.game_status == network_state::endlevel)
	{
		struct error_game_between_levels : passive_messagebox
		{
			error_game_between_levels() :
				passive_messagebox(menu_title{TXT_SORRY}, menu_subtitle{TXT_NET_GAME_BETWEEN2}, TXT_OK, grd_curscreen->sc_canvas)
				{
				}
		};
		run_blocking_newmenu<error_game_between_levels>();
		return false;
	}

	// Check for valid mission name
	{
		mission_entry_predicate mission_predicate;
		mission_predicate.filesystem_name = Netgame.mission_name;
#if DXX_BUILD_DESCENT == 2
		/* FIXME: This should be set to true and the version set
		 * accordingly.  However, currently the host does not provide
		 * the mission version to the guests.
		 */
		mission_predicate.check_version = false;
#endif
	if (const auto errstr = load_mission_by_name(mission_predicate, mission_name_type::guess))
	{
		struct error_mission_not_found :
			std::array<char, 96>,
			passive_messagebox
		{
			error_mission_not_found(const char *errstr) :
				passive_messagebox(menu_title{nullptr}, menu_subtitle{prepare_subtitle(*this, errstr)}, TXT_OK, grd_curscreen->sc_canvas)
				{
				}
			static const char *prepare_subtitle(std::array<char, 96> &b, const char *errstr)
			{
				auto r = b.data();
				std::snprintf(r, b.size(), "%s\n\n%s", TXT_MISSION_NOT_FOUND, errstr);
				return r;
			}
		};
		run_blocking_newmenu<error_mission_not_found>(errstr);
		return false;
	}
	}

#if DXX_BUILD_DESCENT == 2
	if (is_D2_OEM)
	{
		if (Netgame.levelnum>8)
		{
			struct error_using_oem_data : passive_messagebox
			{
				error_using_oem_data() :
					passive_messagebox(menu_title{nullptr}, menu_subtitle{"You are using OEM game data.  You can only play the first 8 levels."}, TXT_OK, grd_curscreen->sc_canvas)
					{
					}
			};
			run_blocking_newmenu<error_using_oem_data>();
			return false;
		}
	}

	if (is_MAC_SHARE)
	{
		if (Netgame.levelnum > 4)
		{
			struct error_using_mac_shareware : passive_messagebox
			{
				error_using_mac_shareware() :
					passive_messagebox(menu_title{nullptr}, menu_subtitle{"You are using Mac shareware data.  You can only play the first 4 levels."}, TXT_OK, grd_curscreen->sc_canvas)
					{
					}
			};
			run_blocking_newmenu<error_using_mac_shareware>();
			return false;
		}
	}

	if (HoardEquipped() == hoard_availability_state::Missing && (Netgame.gamemode == network_game_type::hoard || Netgame.gamemode == network_game_type::team_hoard))
	{
		struct error_hoard_not_available : passive_messagebox
		{
			error_hoard_not_available() :
				passive_messagebox(menu_title{TXT_SORRY}, menu_subtitle{"That is a hoard game.\nYou do not have HOARD.HAM installed.\nYou cannot join."}, TXT_OK, grd_curscreen->sc_canvas)
				{
				}
		};
		run_blocking_newmenu<error_hoard_not_available>();
		return false;
	}

	Network_status = network_state::browsing; // We are looking at a game menu
#endif

	if (net_udp_can_join_netgame(&Netgame) == join_netgame_status_code::game_in_disallowed_state)
	{
		struct error_cannot_join_game : passive_messagebox
		{
			error_cannot_join_game() :
				passive_messagebox(menu_title{TXT_SORRY}, menu_subtitle{Netgame.numplayers == Netgame.max_numplayers ? TXT_GAME_FULL : TXT_IN_PROGRESS}, TXT_OK, grd_curscreen->sc_canvas)
				{
				}
		};
		run_blocking_newmenu<error_cannot_join_game>();
		return false;
	}
	return true;
}

int net_udp_do_join_game()
{
	// Choice is valid, prepare to join in
	GameUniqueState.Difficulty_level = Netgame.difficulty;

	net_udp_set_game_mode(Netgame.gamemode);

	return StartNewLevel(Netgame.levelnum) == window_event_result::handled;     // look ma, we're in a game!!! (If level syncing didn't fail -kreatordxx)
}

}
}

namespace dsx {
namespace {
struct show_game_info_menu : std::array<newmenu_item, 2>, std::array<char, 512>, passive_newmenu
{
	const netgame_info &netgame;
	show_game_info_menu(grs_canvas &canvas, const netgame_info &netgame) :
		std::array<newmenu_item, 2>{{
			newmenu_item::nm_item_menu{"JOIN GAME"},
			newmenu_item::nm_item_menu{"GAME INFO"},
		}},
		passive_newmenu(menu_title{"WELCOME"}, menu_subtitle{(setup_subtitle_text(*this, netgame).data())}, menu_filename{nullptr}, tiny_mode_flag::normal, tab_processing_flag::ignore, adjusted_citem::create(static_cast<std::array<newmenu_item, 2> &>(*this), 0), canvas),
		netgame(netgame)
	{
	}
	virtual window_event_result event_handler(const d_event &event) override;
	static const std::array<char, 512> &setup_subtitle_text(std::array<char, 512> &, const netgame_info &);
};

window_event_result show_game_info_menu::event_handler(const d_event &event)
{
	switch (event.type)
	{
		case event_type::newmenu_selected:
		{
			auto &citem = static_cast<const d_select_event &>(event).citem;
			switch (citem)
			{
				case 0:
				default:
					return window_event_result::close;
				case 1:
					show_netgame_info(netgame);
					return window_event_result::handled;
			}
		}
		default:
			return newmenu::event_handler(event);
	}
}

const std::array<char, 512> &show_game_info_menu::setup_subtitle_text(std::array<char, 512> &rinfo, const netgame_info &netgame)
{
#if DXX_BUILD_DESCENT == 1
#define DXX_SECRET_LEVEL_FORMAT	"%s"
#define DXX_SECRET_LEVEL_PARAMETER	(netgame.levelnum >= 0 ? "" : "S"), \
	netgame.levelnum < 0 ? -netgame.levelnum :	/* else portion provided by invoker */
#elif DXX_BUILD_DESCENT == 2
#define DXX_SECRET_LEVEL_FORMAT
#define DXX_SECRET_LEVEL_PARAMETER
#endif
	const auto gamemode{netgame.gamemode};
	const unsigned
#if DXX_BUILD_DESCENT == 1
	players = netgame.numplayers;
#elif DXX_BUILD_DESCENT == 2
	players = netgame.numconnected;
#endif
#define GAME_INFO_FORMAT_TEXT(F)	\
	F("\nConnected to\n\"%." DXX_STRINGIZE(NETGAME_NAME_LEN) "s\"\n", netgame.game_name.data())	\
	F("%." DXX_STRINGIZE(MISSION_NAME_LEN) "s", netgame.mission_title.data())	\
	F(" - Lvl " DXX_SECRET_LEVEL_FORMAT "%i", DXX_SECRET_LEVEL_PARAMETER netgame.levelnum)	\
	F("\n\nDifficulty: %s", MENU_DIFFICULTY_TEXT(netgame.difficulty))	\
	F("\nGame Mode: %s", GMNames.valid_index(gamemode) ? GMNames[gamemode] : "INVALID")	\
	F("\nPlayers: %u/%i", players, netgame.max_numplayers)
#define EXPAND_FORMAT(A,B,...)	A
#define EXPAND_ARGUMENT(A,B,...)	, B, ## __VA_ARGS__
	std::snprintf(rinfo.data(), rinfo.size(), GAME_INFO_FORMAT_TEXT(EXPAND_FORMAT) GAME_INFO_FORMAT_TEXT(EXPAND_ARGUMENT));
#undef GAME_INFO_FORMAT_TEXT
	return rinfo;
}

direct_join::connect_type net_udp_show_game_info(const netgame_info &Netgame)
{
	switch (run_blocking_newmenu<show_game_info_menu>(*grd_curcanv, Netgame))
	{
		case 0:
		default:
			return direct_join::connect_type::request_join;
		case 1:
			return direct_join::connect_type::idle;
	}
}

}
}
