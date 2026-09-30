/*
 * Portions of this file are copyright Rebirth contributors and licensed as
 * described in COPYING.txt.
 * Portions of this file are copyright Parallax Software and licensed
 * according to the Parallax license below.
 * See COPYING.txt for license details.

THE COMPUTER CODE CONTAINED HEREIN IS THE SOLE PROPERTY OF PARALLAX
SOFTWARE CORPORATION ("PARALLAX").  PARALLAX, IN DISTRIBUTING THE CODE TO
END-USERS, AND SUBJECT TO ALL OF THE TERMS AND CONDITIONS HEREIN, GRANTS A
ROYALTY-FREE, PERPETUAL LICENSE TO SUCH END-USERS FOR USE BY SUCH END-USERS
IN USING, DISPLAYING,  AND CREATING DERIVATIVE WORKS THEREOF, SO LONG AS
SUCH USE, DISPLAY OR CREATION IS FOR NON-COMMERCIAL, ROYALTY OR REVENUE
FREE PURPOSES.  IN NO EVENT SHALL THE END-USER USE THE COMPUTER CODE
CONTAINED HEREIN FOR REVENUE-BEARING PURPOSES.  THE END-USER UNDERSTANDS
AND AGREES TO THE TERMS HEREIN AND ACCEPTS THE SAME BY USE OF THIS FILE.
COPYRIGHT 1993-1999 PARALLAX SOFTWARE CORPORATION.  ALL RIGHTS RESERVED.
*/

/*
 *
 * Defines and exported variables for multi.c
 *
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include "dxxsconf.h"
#include "dsx-ns.h"

namespace dcx {

constexpr std::size_t MAX_MESSAGE_LEN{35};

enum class multi_macro_message_index : uint8_t
{
	_0,
	_1,
	_2,
	_3,
	None = UINT8_MAX,
};

}

#if DXX_USE_MULTIPLAYER

#include <array>
#include <optional>
#include <ranges>
#include <span>
#include "fwd-event.h"
#include "fwd-game.h"
#include "fwd-piggy.h"
#include "fwd-vecmat.h"
#include "objnum.h"
#include "player-callsign.h"

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <io.h>
#else
#include <sys/socket.h>
#include <netinet/in.h>
#endif

#include "pack.h"
#include "ntstring.h"

#ifdef DXX_BUILD_DESCENT
#include "fwd-wall.h"
#include "game.h"
#include "gameplayopt.h"
#include "mission.h"
#include "player.h"
#endif

namespace dcx {

enum class wall_flags : uint8_t;
enum class sound_stack : bool;

struct _sockaddr
{
	union {
		sockaddr sa;
		sockaddr_in sin;
#if DXX_USE_IPv6
		sockaddr_in6 sin6;
#define DXX_IPv6(v4,v6) v6
#else
#define DXX_IPv6(v4,v6) v4
#endif
	};
	using presentation_buffer = std::array<char, DXX_IPv6(INET_ADDRSTRLEN, INET6_ADDRSTRLEN)>;
	static constexpr auto address_family{DXX_IPv6(AF_INET, AF_INET6)};
#undef DXX_IPv6
};

// PROTOCOL VARIABLES AND DEFINES

enum class network_game_type : uint8_t
{
	anarchy,
	team_anarchy,
	robot_anarchy,
	cooperative,
	capture_flag,
	hoard,
	team_hoard,
	bounty,
};

enum class multiplayer_data_priority : uint8_t
{
	_0,
	_1,
	_2,
};

/* These values are sent over the network (JOIN_DENY, KICK, LEAVE and
 * PLAYER_LEFT of the v2 session layer).  If new values are added or existing
 * entries are renumbered, the multiplayer protocol version must be changed.
 */
enum class kick_player_reason : uint8_t
{
	closed, // no new players allowed after game started
	full, // player count maxed out
	endlevel,
	dork,
	aborted,
	level,
	kicked,
	/* Program or protocol version mismatch (JOIN_DENY only). */
	version,
	/* The callsign is already connected from another address. */
	duplicate_callsign,
	/* The reliable queue to this peer overflowed or a message stayed
	 * unacknowledged for too long: the link is too slow or dead.
	 */
	queue_overflow,
	/* Too many malformed packets from this peer. */
	protocol_error,
	/* The level checksum of the peer does not match the host's. */
	checksum,
	/* The level snapshot could not be applied. */
	snapshot_failed,
	/* No packet from the peer for the transport timeout. */
	timeout,
	/* The peer left on its own (LEAVE, PLAYER_LEFT). */
	quit,
	/* The peer left the game before it started (LEAVE while starting). */
	cancelled,
	/* The host ended the game (HOST_SHUTDOWN). */
	host_shutdown,
};

enum class player_ship_color : uint8_t;

static inline player_ship_color get_team_color(const team_number tnum)
{
	return static_cast<player_ship_color>(tnum);
}

}

/* What version of the multiplayer protocol is this?  Increment each time
 * something drastic changes in multiplayer without the version number changing.
 * 100 is the first version of the v2 protocol
 * (Documentation/network-protocol-v2.md); it is bumped by one per stage that
 * changes the wire format, so that mismatched experimental builds refuse each
 * other instead of misbehaving.  It equals NET_V2_PROTO_VERSION in net_v2.h.
 * 101: stage 2 (state bundle, INPUT, interpolation).
 * 102: stage 3 (object ids, host-decided pickups and drops, INVENTORY).
 * 103: stage 3 review fixes (PICKUP_GRANT `life`, MULTI_PLAYER_DERES mine
 * counts).
 * 104: bots B2 (the PLAYER_LIST bot flag, Documentation/multiplayer-bots.md
 * section 2.2).
 * 105: host-assigned spawns (SPAWN_REQUEST, SPAWN_SITE).
 */
constexpr std::uint16_t MULTI_PROTO_VERSION{105};
// PROTOCOL VARIABLES AND DEFINES - END

/* The network tick rate (positions per second, and the pacing of every
 * connection), chosen by the host: 30, 60 or 120 Hz
 * (Documentation/network-protocol-v2.md, section 2.3 and decision 1).
 */
constexpr uint8_t NETGAME_TICK_RATE_DEFAULT{60};
[[nodiscard]]
constexpr bool netgame_tick_rate_valid(const unsigned rate)
{
	return rate == 30 || rate == 60 || rate == 120;
}

#ifdef DXX_BUILD_DESCENT
#if DXX_BUILD_DESCENT == 1
constexpr std::size_t MAX_NET_CREATE_OBJECTS{20u};
#elif DXX_BUILD_DESCENT == 2
constexpr std::size_t MAX_NET_CREATE_OBJECTS{40u};
#endif

#define NETFLAG_LABEL_QUAD	 "Quad Lasers"
#define NETFLAG_LABEL_VULCAN	 "Vulcan cannon"
#define NETFLAG_LABEL_SPREAD	 "Spreadfire cannon"
#define NETFLAG_LABEL_PLASMA	 "Plasma cannon"
#define NETFLAG_LABEL_FUSION	 "Fusion cannon"
#define for_each_netflag_value(VALUE)	\
	VALUE(NETFLAG_DOLASER, "Laser upgrade")	\
	VALUE(NETFLAG_DOQUAD, NETFLAG_LABEL_QUAD)	\
	VALUE(NETFLAG_DOVULCAN, NETFLAG_LABEL_VULCAN)	\
	VALUE(NETFLAG_DOSPREAD, NETFLAG_LABEL_SPREAD)	\
	VALUE(NETFLAG_DOPLASMA, NETFLAG_LABEL_PLASMA)	\
	VALUE(NETFLAG_DOFUSION, NETFLAG_LABEL_FUSION)	\
	VALUE(NETFLAG_DOHOMING, "Homing Missiles")	\
	VALUE(NETFLAG_DOPROXIM, "Proximity Bombs")	\
	VALUE(NETFLAG_DOSMART, "Smart Missiles")	\
	VALUE(NETFLAG_DOMEGA, "Mega Missiles")	\
	VALUE(NETFLAG_DOCLOAK, "Cloaking")	\
	VALUE(NETFLAG_DOINVUL, "Invulnerability")	\
	D2X_MP_NETFLAGS(VALUE)	\

#define for_each_netgrant_value(VALUE)	\
	VALUE(NETGRANT_QUAD, NETFLAG_LABEL_QUAD)	\
	VALUE(NETGRANT_VULCAN, NETFLAG_LABEL_VULCAN)	\
	VALUE(NETGRANT_SPREAD, NETFLAG_LABEL_SPREAD)	\
	VALUE(NETGRANT_PLASMA, NETFLAG_LABEL_PLASMA)	\
	VALUE(NETGRANT_FUSION, NETFLAG_LABEL_FUSION)	\
	D2X_MP_NETGRANT(VALUE)

#define MULTI_GAME_TYPE_COUNT	8
namespace dsx {
#if DXX_BUILD_DESCENT == 1
constexpr std::integral_constant<unsigned, 13> MULTI_GAME_NAME_LENGTH{};
constexpr std::integral_constant<unsigned, 18> MULTI_ALLOW_POWERUP_TEXT_LENGTH{};
#define MULTI_ALLOW_POWERUP_MAX 12
#define D2X_MP_NETFLAGS(VALUE)
#define DXX_GRANT_LASER_LEVEL_BITS	2
#define D2X_MP_NETGRANT(VALUE)
#elif DXX_BUILD_DESCENT == 2
constexpr std::integral_constant<unsigned, 17> MULTI_GAME_NAME_LENGTH{};
constexpr std::integral_constant<unsigned, 21> MULTI_ALLOW_POWERUP_TEXT_LENGTH{};
#define MULTI_ALLOW_POWERUP_MAX 26
#define NETFLAG_LABEL_GAUSS	"Gauss cannon"
#define NETFLAG_LABEL_HELIX	"Helix cannon"
#define NETFLAG_LABEL_PHOENIX	"Phoenix cannon"
#define NETFLAG_LABEL_OMEGA	"Omega cannon"
#define NETFLAG_LABEL_AFTERBURNER	"Afterburners"
#define NETFLAG_LABEL_AMMORACK	"Ammo rack"
#define NETFLAG_LABEL_CONVERTER	"Energy Converter"
#define NETFLAG_LABEL_HEADLIGHT	"Headlight"
#define D2X_MP_NETFLAGS(VALUE)	\
	VALUE(NETFLAG_DOSUPERLASER, "Super lasers")	\
	VALUE(NETFLAG_DOGAUSS, NETFLAG_LABEL_GAUSS)	\
	VALUE(NETFLAG_DOHELIX, NETFLAG_LABEL_HELIX)	\
	VALUE(NETFLAG_DOPHOENIX, NETFLAG_LABEL_PHOENIX)	\
	VALUE(NETFLAG_DOOMEGA, NETFLAG_LABEL_OMEGA)	\
	VALUE(NETFLAG_DOFLASH, "Flash Missiles")	\
	VALUE(NETFLAG_DOGUIDED, "Guided Missiles")	\
	VALUE(NETFLAG_DOSMARTMINE, "Smart Mines")	\
	VALUE(NETFLAG_DOMERCURY, "Mercury Missiles")	\
	VALUE(NETFLAG_DOSHAKER, "Earthshaker Missiles")	\
	VALUE(NETFLAG_DOAFTERBURNER, NETFLAG_LABEL_AFTERBURNER)	\
	VALUE(NETFLAG_DOAMMORACK, NETFLAG_LABEL_AMMORACK)	\
	VALUE(NETFLAG_DOCONVERTER, NETFLAG_LABEL_CONVERTER)	\
	VALUE(NETFLAG_DOHEADLIGHT, NETFLAG_LABEL_HEADLIGHT)

#define DXX_GRANT_LASER_LEVEL_BITS	3
#define D2X_MP_NETGRANT(VALUE)	\
	VALUE(NETGRANT_GAUSS, NETFLAG_LABEL_GAUSS)	\
	VALUE(NETGRANT_HELIX, NETFLAG_LABEL_HELIX)	\
	VALUE(NETGRANT_PHOENIX, NETFLAG_LABEL_PHOENIX)	\
	VALUE(NETGRANT_OMEGA, NETFLAG_LABEL_OMEGA)	\
	VALUE(NETGRANT_AFTERBURNER, NETFLAG_LABEL_AFTERBURNER)	\
	VALUE(NETGRANT_AMMORACK, NETFLAG_LABEL_AMMORACK)	\
	VALUE(NETGRANT_CONVERTER, NETFLAG_LABEL_CONVERTER)	\
	VALUE(NETGRANT_HEADLIGHT, NETFLAG_LABEL_HEADLIGHT)

#endif

namespace multi {

struct dispatch_table
{
	constexpr const dispatch_table *operator->() const
	{
		return this;
	}
	/* Send gameplay records that `originator` caused.  The local player
	 * is `Player_num`; the host also originates records for the ships it
	 * flies itself (Documentation/multiplayer-bots.md).
	 */
	virtual void send_data(std::span<const uint8_t> data, multiplayer_data_priority, playernum_t originator) const = 0;
	virtual void send_data_direct(std::span<const uint8_t> data, playernum_t pnum, int needack) const = 0;
	virtual void do_protocol_frame(int force, int listen) const = 0;
	virtual window_event_result level_sync() const = 0;
	virtual void send_endlevel_packet() const = 0;
	virtual void kick_player(const _sockaddr &dump_addr, kick_player_reason why) const = 0;
	virtual void disconnect_player(int playernum) const = 0;
	virtual int end_current_level(
#if DXX_BUILD_DESCENT == 1
		next_level_request_secret_flag *secret
#endif
		) const = 0;
	virtual void leave_game() const = 0;
};
}

#define define_netflag_bit_enum(NAME,STR)	BIT_##NAME,
#define define_netflag_bit_mask(NAME,STR)	NAME = 1u << static_cast<uint8_t>(netflag_bit::BIT_##NAME),
#define define_netflag_powerup_mask(NAME,STR)	| static_cast<uint32_t>(netflag_flag::NAME)
enum netflag_bit : uint8_t
{
	for_each_netflag_value(define_netflag_bit_enum)
};
// Bitmask for netgame_info->AllowedItems to set allowed items in Netgame
enum class netflag_flag :
#if DXX_BUILD_DESCENT == 1
	uint16_t
#elif DXX_BUILD_DESCENT == 2
	uint32_t
#endif
{
	None = 0,
	for_each_netflag_value(define_netflag_bit_mask)
};
#undef define_netflag_bit_mask
enum netgrant_bit : uint8_t
{
	BIT_NETGRANT_LASER = DXX_GRANT_LASER_LEVEL_BITS - 1,
	for_each_netgrant_value(define_netflag_bit_enum)
	BIT_NETGRANT_MAXIMUM
};
#define define_netgrant_bit_mask(NAME,STR)	NAME = 1u << static_cast<uint8_t>(netgrant_bit::BIT_##NAME),
enum netgrant_flag :
#if DXX_BUILD_DESCENT == 1
	uint8_t
#elif DXX_BUILD_DESCENT == 2
	uint16_t
#endif
{
	None = 0,
	for_each_netgrant_value(define_netgrant_bit_mask)
};
#undef define_netflag_bit_enum
#undef define_netgrant_bit_mask

struct packed_spawn_granted_items
{
	netgrant_flag mask{netgrant_flag::None};
	static_assert(BIT_NETGRANT_MAXIMUM <= sizeof(mask) << 3, "mask too small");
	constexpr packed_spawn_granted_items() = default;
	constexpr packed_spawn_granted_items(const netgrant_flag m) :
		mask{m}
	{
	}
	explicit operator bool() const { return mask != netgrant_flag::None; }
	bool has_quad_laser() const { return mask & NETGRANT_QUAD; }
#if DXX_BUILD_DESCENT == 2
	bool has_afterburner() const { return mask & NETGRANT_AFTERBURNER; }
#endif
};

class packed_netduplicate_items
{
public:
	enum
	{
		primary_shift = 0,
		primary_width = 3,
		secondary_shift = primary_shift + primary_width,
		secondary_width = 3,
#if DXX_BUILD_DESCENT == 2
		accessory_shift = secondary_shift + secondary_width,
		accessory_width = 3,
#endif
	};
private:
#if DXX_BUILD_DESCENT == 1
	typedef uint8_t count_type;
#elif DXX_BUILD_DESCENT == 2
	typedef uint16_t count_type;
#endif
	count_type count;
	template <uint_fast32_t shift, uint_fast32_t width>
	uint_fast32_t get_sub_field() const
	{
		static_assert(shift + width <= sizeof(count) << 3, "shift+width too big");
		constexpr auto low_mask = (1 << width) - 1;
		return (count >> shift) & low_mask;
	}
public:
	template <uint_fast32_t shift, uint_fast32_t width>
	void set_sub_field(uint_fast32_t value)
	{
		constexpr auto low_mask = (1 << width) - 1;
		constexpr auto shifted_mask = low_mask << shift;
		count = (count & ~shifted_mask) | (value << shift);
	}
#define DEFINE_ACCESSOR(N)	\
	uint_fast32_t get_##N##_count() const	\
	{	\
		return get_sub_field<N##_shift, N##_width>();	\
	}	\
	void set_##N##_count(uint_fast32_t value)	\
	{	\
		set_sub_field<N##_shift, N##_width>(value);	\
	}
	DEFINE_ACCESSOR(primary);
	DEFINE_ACCESSOR(secondary);
#if DXX_BUILD_DESCENT == 2
	DEFINE_ACCESSOR(accessory);
#endif
	count_type get_packed_field() const
	{
		return count;
	}
	void set_packed_field(count_type c)
	{
		count = c;
	}
};

static inline laser_level map_granted_flags_to_laser_level(const packed_spawn_granted_items &grant)
{
	/* Laser level in lowest bits */
	return laser_level{static_cast<uint8_t>(grant.mask & ((1 << DXX_GRANT_LASER_LEVEL_BITS) - 1))};
}
player_flags map_granted_flags_to_player_flags(packed_spawn_granted_items grant);
uint_fast32_t map_granted_flags_to_primary_weapon_flags(packed_spawn_granted_items grant);
uint16_t map_granted_flags_to_vulcan_ammo(packed_spawn_granted_items grant);
void multi_digi_link_sound_to_pos(sound_effect soundnum, vcsegptridx_t segnum, sidenum_t sidenum, const vms_vector &pos, int forever, fix max_volume);
void multi_object_to_object_rw(const object &obj, object_rw *obj_rw);
void multi_object_rw_to_object(const object_rw *obj_rw, object &obj);

using GMNames_array = enumerated_array<char[MULTI_GAME_NAME_LENGTH], MULTI_GAME_TYPE_COUNT, network_game_type>;
extern const GMNames_array GMNames;
using multi_allow_powerup_text_array = std::array<char[MULTI_ALLOW_POWERUP_TEXT_LENGTH], MULTI_ALLOW_POWERUP_MAX>;
extern const multi_allow_powerup_text_array multi_allow_powerup_text;
extern const enumerated_array<char[8], MULTI_GAME_TYPE_COUNT, network_game_type> GMNamesShrt;
}

namespace dcx {

/* Stub for mods that remap player colors */
static constexpr player_ship_color get_player_color(const playernum_t pnum)
{
	return static_cast<player_ship_color>(pnum);
}

extern std::array<objnum_t, MAX_NET_CREATE_OBJECTS> Net_create_objnums;
extern unsigned Net_create_loc;
/* Friendly fire is off and `attacker` is a teammate of `victim`: the
 * damage does not apply.
 */
int multi_maybe_disable_friendly_fire(const object_base *attacker, playernum_t victim);
}

namespace dsx {

void multi_send_fire(const vms_matrix &orient, int laser_gun, laser_level, int laser_flags, objnum_t laser_track, imobjptridx_t is_bomb_objnum, playernum_t pnum);
void multi_send_destroy_controlcen(objnum_t objnum, playernum_t player);
void multi_send_kill(vmobjptridx_t objnum);
void multi_send_remobj(vmobjidx_t objnum);
void multi_send_door_open(vcsegidx_t segnum, sidenum_t side, wall_flags flag);
void multi_reset_player_object(object &objp);
}
#endif

enum class msgsend_state : uint8_t {
	none,
	typing,
	automap,
};

enum deres_type_t {
	deres_explode,
	deres_drop,
};

// Exported functions

struct owned_remote_objnum
{
	int8_t owner;
	uint16_t objnum;
};
enum class trgnum_t : uint8_t;

objnum_t objnum_remote_to_local(uint16_t remote_obj, int8_t owner);
owned_remote_objnum objnum_local_to_remote(objnum_t local);
void map_objnum_local_to_remote(objnum_t local, int remote, int owner);
void map_objnum_local_to_local(objnum_t objnum);
void map_objnum_local_to_local(objnum_t objnum, playernum_t owner);
void reset_network_objects();

void multi_init_objects(void);
window_event_result multi_do_frame();

#ifdef DXX_BUILD_DESCENT
namespace dsx {

enum class multi_endlevel_type : bool
{
	normal,
#if DXX_BUILD_DESCENT == 1
	secret,
#endif
};
#if DXX_BUILD_DESCENT == 1
void multi_send_endlevel_start(multi_endlevel_type);
#elif DXX_BUILD_DESCENT == 2
void multi_send_endlevel_start();
static inline void multi_send_endlevel_start(multi_endlevel_type)
{
	multi_send_endlevel_start();
}
#endif
/* Player `pnum` (the local player, or on the host a bot) exploded. */
void multi_send_player_deres(deres_type_t type, playernum_t pnum);
/* Host (Documentation/multiplayer-bots.md section 6.4): a chat line from
 * the host to everyone (MULTI_MESSAGE), shown on the host's HUD too.
 * At most 34 characters are sent; `text` must not hold ": " (the
 * receiver would read a recipient).
 */
void multi_send_host_notice(const char *text);
/* Host: Netgame.team_vector changed (a bot added or moved): the ships'
 * colours, the cockpit, and the clients (MULTI_GMODE_UPDATE).
 */
void multi_host_teams_changed();
}
void multi_send_play_sound(sound_effect sound_num, fix volume, sound_stack once);
void multi_send_reappear(playernum_t pnum);
void multi_send_create_explosion(playernum_t);
void multi_send_controlcen_fire(const vms_vector &to_target, int gun_num, objnum_t objnum);
namespace dcx {
void multi_send_cloak(playernum_t pnum);
void multi_send_decloak(playernum_t pnum);
}
void multi_digi_play_sample(sound_effect sndnum, fix max_volume);
void multi_digi_play_sample_once(sound_effect soundnum, fix max_volume);
void multi_send_score(void);
void multi_send_trigger(trgnum_t trigger);
#if DXX_BUILD_DESCENT == 2
namespace dsx {
struct marker_message_text_t;
void multi_send_drop_marker(unsigned player, const vms_vector &position, player_marker_index messagenum, const marker_message_text_t &text);
void multi_send_markers();
void multi_send_guided_release(const object_base &miss);
/* The generation of player `pnum`'s newest guided missile: counted by the
 * owner when it fires one, carried in bits 1-7 of the fire message's
 * flags byte and in the v2 guided record (stage 2), so that the records
 * of a new missile that reuses the previous one's object slot are not
 * applied to the previous one's copy.
 */
[[nodiscard]]
uint8_t multi_guided_generation(playernum_t pnum);
void multi_send_orb_bonus(playernum_t pnum, uint8_t);
void multi_send_effect_blowup(vcsegidx_t segnum, sidenum_t side, const vms_vector &pnt);
#ifndef RELEASE
void multi_add_lifetime_kills(int count);
#endif
}
#endif
#endif
void multi_send_bounty( void );

void multi_consistency_error(int reset);
#ifdef DXX_BUILD_DESCENT
namespace dsx {
void multi_send_hostage_door_status(vcwallptridx_t wallnum);
void multi_prep_level_objects(const d_powerup_info_array &Powerup_info, const d_vclip_array &Vclip);
void multi_prep_level_player();
void multi_leave_game(void);
void multi_process_bigdata(const d_level_shared_robot_info_state &LevelSharedRobotInfoState, playernum_t pnum, std::span<const uint8_t> buf);
void multi_make_ghost_player(playernum_t);
void multi_make_player_ghost(playernum_t);
}
#endif
void multi_define_macro(int key);
void multi_send_macro(int key);
int multi_get_kill_list(playernum_array_t &sorted_kills);
void multi_new_game(void);
#ifdef DXX_BUILD_DESCENT
namespace dsx {
void multi_sort_kill_list(void);
}
#endif
void multi_reset_stuff(void);
void multi_disconnect_player(playernum_t);

#ifdef DXX_BUILD_DESCENT
namespace dsx {
void multi_initiate_save_game();
void multi_initiate_restore_game();
void multi_execute_save_game(d_game_unique_state::save_slot slot, const d_game_unique_state::savegame_description &desc, std::ranges::subrange<const player *> player_range);
}
#endif

// Exported variables

namespace dcx {

/* These values are sent over the network.  If they are changed, the
 * multiplayer protocol version must be updated.
 */
enum class network_state : uint8_t
{
	menu,
	playing,
	browsing,
	waiting,
	starting,
	endlevel,
};

std::optional<network_state> build_network_state_from_untrusted(uint8_t untrusted);

extern network_state Network_status;

// IMPORTANT: These variables needed for player rejoining done by protocol-specific code
extern int Network_rejoined;
extern int Network_sending_extras;
extern int Player_joining_extras;

extern per_player_array<per_player_array<uint16_t>> kill_matrix;
extern per_team_array<int16_t> team_kills;

extern ushort my_segments_checksum;

//do we draw the kill list on the HUD?
enum class show_kill_list_mode : int8_t
{
	None,
	_1,
	efficiency = 2,
	team_kills = 3,
};
extern show_kill_list_mode Show_kill_list;
extern bool Show_reticle_name;
extern fix Show_kill_list_timer;

// Used to send network messages

extern ntstring<MAX_MESSAGE_LEN - 1> Network_message;
extern int Network_message_reciever;

// Which player 'owns' each local object for network purposes
extern std::array<sbyte, MAX_OBJECTS> object_owner;

extern int multi_quit_game;

extern per_player_array<msgsend_state> multi_sending_message;
extern multi_macro_message_index multi_defining_message;

vms_vector multi_get_vector(std::span<const uint8_t, 12> buf);
void multi_put_vector(uint8_t *buf, const vms_vector &v);
team_number multi_get_team_from_player(uint8_t, playernum_t pnum);

}
extern void multi_send_message_start();
void multi_send_msgsend_state(msgsend_state state);

#ifdef DXX_BUILD_DESCENT
#if DXX_BUILD_DESCENT == 2
namespace dsx {
extern std::array<grs_main_bitmap, 2> Orb_icons;
struct hoard_highest_record
{
	unsigned points;
	unsigned player = UINT_MAX;
};

extern hoard_highest_record hoard_highest_record_stats;
}
#endif
#endif
namespace dcx {
extern playernum_t Bounty_target;

extern per_player_array<std::array<bitmap_index, N_PLAYER_SHIP_TEXTURES>> multi_player_textures;

// Globals for protocol-bound Refuse-functions
extern char RefuseThisPlayer,WaitForRefuseAnswer,RefuseTeam,RefusePlayerName[12];
extern fix64 RefuseTimeLimit;
#define REFUSE_INTERVAL (F1_0*8)

#ifdef DXX_BUILD_DESCENT
enum class netgame_rule_flags : uint8_t
{
	None = 0,
	/*
	 * These values are written to .NGP files and to the network.
	 * Changing them breaks ABI compatibility.
	 */
	closed = 1,
	show_all_players_on_automap = 4,
	/* if DXX_BUILD_DESCENT == 2 */
	hoard = 8,
	team_hoard = 16,
	really_endlevel = 32,
	really_forming = 64,
	/* endif */
};

template <>
inline constexpr bool enable_bit_enum_and<netgame_rule_flags, netgame_rule_flags>{true};

template <>
inline constexpr bool enable_bit_enum_bitnot<netgame_rule_flags>{true};

template <>
inline constexpr bool enable_bit_enum_boolnot<netgame_rule_flags>{true};

template <>
inline constexpr bool enable_bit_enum_or<netgame_rule_flags, netgame_rule_flags>{true};

#if DXX_USE_TRACKER
enum TrackerNATHolePunchWarn : uint8_t
{
	Unset,
	UserEnabledHP,
	UserRejectedHP,
};
#endif
#endif
}

#ifdef DXX_BUILD_DESCENT
namespace dsx {

#define NETGAME_NAME_LEN	25

extern struct netgame_info Netgame;

}
#endif

#define multi_i_am_master()	(Player_num == 0)
void change_playernum_to(playernum_t new_pnum);

// Multiplayer powerup capping
void MultiLevelInv_InitializeCount();
void MultiLevelInv_Recount();
void multi_send_kill_goal_counts();
#ifdef DXX_BUILD_DESCENT
namespace dsx {
extern bool MultiLevelInv_AllowSpawn(powerup_type_t powerup_type);
/* The respawn bookkeeping's counts for a powerup's item (a 4-pack counts
 * as its single missile), recounted now: for the host's accounting log.
 */
void MultiLevelInv_counts(powerup_type_t powerup_type, uint32_t &initial, uint32_t &current);
netflag_flag multi_powerup_is_allowed(powerup_type_t id, const netflag_flag AllowedItems);
netflag_flag multi_powerup_is_allowed(powerup_type_t id, const netflag_flag AllowedItems, const netflag_flag SpawnGrantedItems);
void show_netgame_info(const netgame_info &netgame);
const char *multi_common_deny_save_game(const fvcobjptr &vcobjptr, std::ranges::subrange<const player *> player_range);
const char *multi_interactive_deny_save_game(const fvcobjptr &vcobjptr, std::ranges::subrange<const player *> player_range, const d_level_unique_control_center_state &);
void multi_check_for_killgoal_winner(const d_robot_info_array &Robot_info);
}
#if DXX_BUILD_DESCENT == 2
namespace dsx {
extern void multi_send_stolen_items();
void multi_send_trigger_specific(playernum_t pnum, trgnum_t trig);
void multi_send_door_open_specific(playernum_t pnum, vcsegidx_t segnum, sidenum_t side, wall_flags flag);
void multi_send_wall_status_specific(playernum_t pnum, wallnum_t wallnum, uint8_t type, wall_flags flags, wall_state state);
void multi_send_light_specific (playernum_t pnum, vcsegptridx_t segnum, sidemask_t val);
void multi_send_capture_bonus (playernum_t pnum);
int multi_all_players_alive(const fvcobjptr &, std::ranges::subrange<const player *>);
void multi_send_seismic(fix);
void multi_send_drop_blobs(playernum_t);
void multi_send_sound_function (char,char, playernum_t pnum = Player_num);
void DropFlag();
void multi_send_finish_game ();
void init_hoard_data(d_vclip_array &Vclip);
void multi_apply_goal_textures();
void multi_send_escort_goal(const d_unique_buddy_state &);

enum class hoard_availability_state : uint8_t
{
	Missing,	/* Check was done, no file found */
	Found,	/* Check was done, file exists */
	None,	/* No check has been done */
};

hoard_availability_state HoardEquipped();
#if DXX_USE_EDITOR
void save_hoard_data(void);
#endif
}
#endif
#endif

//how to encode missiles & flares in weapon packets
#define MISSILE_ADJUST  100
#define FLARE_ADJUST    127

/*
 * The Network Players structure
 * Contains protocol-specific data with designated prefixes and general player-related data.
 * Note that not all of these infos will be sent to other users - some are used and/or set locally, only.
 */
struct netplayer_info : prohibit_void_ptr<>
{
	enum class player_rank : uint8_t
	{
		None,
		Cadet,
		Ensign,
		Lieutenant,
		LtCommander,
		Commander,
		Captain,
		ViceAdmiral,
		Admiral,
		Demigod
	};
	union
	{
#if DXX_USE_UDP
		struct
		{
			struct _sockaddr	addr; // IP address of this peer
		} udp;
#endif
	} protocol;	
	callsign_t					callsign;
	player_connection_status connected;
	player_rank					rank;
	fix							ping;
	fix64							LastPacketTime;
};

#ifdef DXX_BUILD_DESCENT
#if DXX_BUILD_DESCENT == 2
struct ThiefModifier
{
	enum Flags : uint8_t {
		Absent = 1,
		NoEnergyWeapons,
	};
};
#endif

namespace dsx {
/*
 * The Network Game structure
 * Contains protocol-specific data with designated prefixes and general game-related data.
 * Note that not all of these infos will be sent to clients - some are used and/or set locally, only.
 */
struct netgame_info : prohibit_void_ptr<>
{
	static constexpr std::integral_constant<netflag_flag, static_cast<netflag_flag>(0 for_each_netflag_value(define_netflag_powerup_mask))> MaskAllKnownAllowedItems{};
#undef define_netflag_powerup_mask
	using play_time_allowed_abi_ratio = std::ratio<5 * 60>;
#if DXX_USE_UDP
	union
	{
#if DXX_USE_UDP
		struct
		{
			struct _sockaddr		addr; // IP address of this netgame's host
			std::array<short, 4>			program_iver; // IVER of program for version checking
			sbyte				valid; // Status of Netgame info: -1 = Failed, Wrong version; 0 = No info, yet; 1 = Success
			uint8_t				your_index; // The local player's (re)join position in players[]
			uint32_t			session_id; // Random id of the session, chosen by the host (v2 protocol, section 3.1)
		} udp;
#endif
	} protocol;	
#endif
	ntstring<NETGAME_NAME_LEN> game_name;
	ntstring<MISSION_NAME_LEN> mission_title;
	ntstring<8> mission_name;
	int     					levelnum;
	Difficulty_level_type difficulty;
	network_game_type   		gamemode;
	ubyte   					RefusePlayers;
	network_state game_status;
	ubyte   					numplayers;
	ubyte   					max_numplayers;
	ubyte   					numconnected;
	netgame_rule_flags game_flag;
	ubyte   					team_vector;
	uint8_t						SecludedSpawns;
	uint8_t MouselookFlags;
	uint8_t PitchLockFlags;
	netflag_flag				AllowedItems;
	packed_spawn_granted_items SpawnGrantedItems;
	packed_netduplicate_items DuplicatePowerups;
	unsigned ShufflePowerupSeed;
	d_mp_gameplay_options MPGameplayOptions;
#if DXX_BUILD_DESCENT == 2
	uint8_t Allow_marker_view;
	uint8_t AlwaysLighting;
	uint8_t ThiefModifierFlags;
	uint8_t AllowGuidebot;
#endif
	uint8_t ShowEnemyNames;
	uint8_t BrightPlayers;
	uint8_t InvulAppear;
	ushort						segments_checksum;
	int						KillGoal;
	/* The UI enforces that this steps in units of 5 minutes, but for
	 * efficiency, it is stored as ticks (1 second = F1_0).  The UI
	 * imposes a maximum value that is small enough that overflow is
	 * impossible.
	 */
	d_time_fix PlayTimeAllowed;
	fix						level_time;
	int						control_invul_time;
	int						monitor_vector;
	/* Network tick rate in Hz (30, 60 or 120); replaces PacketsPerSec. */
	uint8_t	TickRate{NETGAME_TICK_RATE_DEFAULT};
	ubyte						NoFriendlyFire;
	per_team_array<callsign_t>						team_name;
	per_player_array<uint32_t>						locations;
	per_player_array<per_player_array<uint16_t>>	kills;
	per_team_array<int16_t>							team_kills;
	per_player_array<uint16_t>						killed;
	per_player_array<uint16_t>						player_kills;
	per_player_array<uint32_t>						player_score;
	per_player_array<player_flags>					net_player_flags;
	per_player_array<netplayer_info>				players;
#if DXX_USE_TRACKER
	ubyte						Tracker;
	TrackerNATHolePunchWarn TrackerNATWarned;
#endif
};


/*
 * Structure holding all powerup types we want to keep track of. Used to cap or respawn powerups and keep the level inventory steady.
 * Using uint32_t because we don't count powerup units but what the powerup contains (1 or 4 missiles, vulcam amount, etc) so we can keep track of overhead.
 * I'm sorry if this isn't very optimized but I found this easier to handle than a single variable per powerup.
 */
struct multi_level_inv
{
	per_powerup_type_array<uint32_t> Initial; // initial (level start) count of this powerup type
	per_powerup_type_array<uint32_t> Current; // current count of this powerup type
	per_powerup_type_array<fix> RespawnTimer; // incremented by FrameTime if initial-current > 0 and triggers respawn after 2 seconds. Since we deal with a certain delay from clients, their inventory updates may happen a while after they remove the powerup object and we do not want to respawn it on accident during that time window!
};

namespace multi
{
	struct level_checksum_mismatch : std::runtime_error
	{
		level_checksum_mismatch() :
			runtime_error("level checksum mismatch")
		{
		}
	};
	struct local_player_not_playing : std::runtime_error
	{
		local_player_not_playing() :
			runtime_error("local player not playing")
		{
		}
	};
}

static inline team_number multi_get_team_from_player(const netgame_info &Netgame, playernum_t pnum)
{
	return ::dcx::multi_get_team_from_player(Netgame.team_vector, pnum);
}

static inline player_ship_color get_player_or_team_color(const netgame_info &Netgame, const game_mode_flags Game_mode, const playernum_t pnum)
{
	return +(Game_mode & GM_TEAM)
		? get_team_color(multi_get_team_from_player(Netgame, pnum))
		: get_player_color(pnum);
}

}
#endif

netplayer_info::player_rank GetMyNetRanking();

namespace dcx {
using rank_strings_type = enumerated_array<char[16], 10, netplayer_info::player_rank>;
extern const rank_strings_type RankStrings;

netplayer_info::player_rank build_rank_from_untrusted(uint8_t untrusted);
}

#define PUT_INTEL_SEGNUM(D,S)	( DXX_BEGIN_COMPOUND_STATEMENT {	\
	const segnum_t PUT_INTEL_SEGNUM = S;	\
	PUT_INTEL_SHORT(D, static_cast<uint16_t>(PUT_INTEL_SEGNUM));	\
	} DXX_END_COMPOUND_STATEMENT )

#ifdef DXX_BUILD_DESCENT
namespace dsx {

/* Receiver-side interpolation of remote ships and guided missiles
 * (similar/main/net_interp.cpp, Documentation/network-protocol-v2.md
 * section 5.4).  The network layer feeds it snapshots; the game calls
 * net_interp_apply_all once per frame from object_move_all, before any
 * object moves, and object_move_one skips the physics of every object it
 * drives.
 */
void net_interp_apply_all();
[[nodiscard]]
bool net_interp_drives(vcobjidx_t obj);
/* The player's state is old at the host (section 5.2 `input_age`, and
 * the record's sample age): shown as a lag marker.
 */
[[nodiscard]]
bool net_interp_player_lagging(playernum_t pnum);
/* Forget every snapshot (level start, session end). */
void net_interp_reset();
/* Put player `pnum`'s ship where its newest snapshot has it (not at the
 * delayed render time), for a message that acts at the ship's position
 * now: MULTI_PLAYER_DERES, and on the host a DROP_REQUEST.  The next
 * net_interp_apply_all puts it back on its interpolated path.
 */
void net_interp_snap_to_newest(playernum_t pnum);
/* Where player `pnum`'s newest snapshot has its ship, and how fast it
 * moves there (the host's range check of a pickup request).  False if
 * there is no snapshot.
 */
[[nodiscard]]
bool net_interp_newest_position(playernum_t pnum, vms_vector &pos, fix &speed);
/* Where player `pnum`'s newest snapshot of its current life has its ship
 * (not the place it died, which net_interp_newest_position keeps until
 * the next life's first snapshot) and the segment it is in: the host's
 * spawn ranking.  False if there is none.
 */
[[nodiscard]]
bool net_interp_newest_live_position(playernum_t pnum, vms_vector &pos, segnum_t &segnum);
/* object_move_one, for an object net_interp_drives: the object
 * collisions along the path a remote ship was moved this frame
 * (phys_sweep_objects).
 */
void net_interp_sweep_driven(const d_robot_info_array &Robot_info, vmobjptridx_t obj);
/* Laser_create_new, for the muzzle flash of a shot of a remote player's
 * ship: net_interp_apply_all keeps the flash on the gun it came from as
 * the ship moves on (the flash object itself does not move).
 */
void net_interp_carry_flash(vcobjptridx_t ship, vcobjptridx_t flash);

/* Object authority and pickups (similar/main/net_objects.cpp,
 * Documentation/network-protocol-v2.md sections 6.1-6.4, stage 3).  In
 * a network game the host decides every pickup, creates and removes
 * every powerup and drops the eggs of dead and departed players; every
 * machine names a powerup by the same net id.  Nothing here acts outside
 * a network game.
 */
/* Level start, every machine: forget the old ids and give every powerup
 * of the level its level id (after multi_prep_level_objects).
 */
void net_objects_level_start();
/* A join in progress: the snapshot replaces the level's objects. */
void net_objects_snapshot_begin();
void net_objects_snapshot_bind(vmobjptridx_t obj, uint16_t netid);
/* The host, writing a snapshot: the id of an object (0xffff if none). */
[[nodiscard]]
uint16_t net_objects_netid_of(vcobjptridx_t obj);
/* The host: player `pnum` joins in progress; its inventory starts anew. */
void net_objects_host_join(playernum_t pnum);
/* Once per frame, from multi_do_frame. */
void net_objects_frame();
/* A stage 3 message (net_v2_session.h ids 0x20-0x26, 0x3b, 0x47) from
 * `from` (a client, on the host; the host, on a client).
 */
void net_objects_receive(playernum_t from, uint8_t type, std::span<const uint8_t> payload);
/* The local ship touched a powerup.  True if the pickup is handled by
 * the object authority (asked for, or decided on the host); false for an
 * object without a net id, which keeps the v1 handling.
 */
bool net_objects_touch(vmobjptridx_t powerup);
/* The host created a powerup (respawn, used item, drop): give it an id
 * and announce it.  `owner` is the player who spat it (it cannot take it
 * back for 2 s) or 0xff; `appear` shows the appearance effect (a
 * respawned item).
 */
void net_objects_announce(vmobjptridx_t obj, uint8_t owner, bool appear);
/* The host: player `pnum` died or left; drop what it carried (once until
 * it reappears).
 */
void net_objects_host_drop_player_eggs(playernum_t pnum);
/* Player `pnum` reappeared (MULTI_REAPPEAR): it may be dropped again. */
void net_objects_player_reappeared(playernum_t pnum);
/* The local player sent its MULTI_PLAYER_DERES: its life ended (grants
 * for it that arrive later are not applied).
 */
void net_objects_own_deres();
/* A client drops a weapon, missiles, a flag or an orb: ask the host to
 * create it.  False if not sent.
 */
bool net_objects_request_drop(powerup_type_t id, uint32_t count);
/* Send the local inventory now if it changed (before a pickup request, a
 * drop and a death, so the host decides with the current inventory).
 */
void net_objects_flush_inventory();
/* The host: player `pnum` is a ship the host flies itself (a bot).  Its
 * ship's inventory is the truth: bring the host's copy up to date (it
 * drops the eggs from it) and tell the clients if it changed (`force`:
 * now, even if only energy or ammunition changed).
 */
void net_objects_host_own_ship_inventory(playernum_t pnum, bool force);
/* The host: whether the bot in slot `pnum` could take a powerup of type
 * `id` (carrying `count`: a cannon's rounds, the omega charge) now, by
 * the rules every player's pickups follow (not full, not already had).
 */
[[nodiscard]]
bool net_objects_bot_can_use(playernum_t pnum, powerup_type_t id, uint32_t count);
/* The host: the ship of the bot in slot `pnum` touched `powerup`.  The
 * host decides as for any player (host_decide) and grants it at once
 * (PICKUP_GRANT).  True if granted: the caller then applies the pickup's
 * other effects (cloak, invulnerability) and sends the bot's INVENTORY
 * with net_objects_host_own_ship_inventory(pnum, true).  Otherwise
 * `deny` says why (net_v2 deny_reason; 0xff: not the host's to decide,
 * e.g. a powerup without a net id).
 */
bool net_objects_bot_touch(playernum_t pnum, vmobjptridx_t powerup, uint8_t &deny);
/* The host: send every player's inventory to everyone (the extras of a
 * join in progress).
 */
void net_objects_send_all_inventories();
[[nodiscard]]
bool net_objects_active();

/* Host-assigned spawns in a network deathmatch (similar/main/net_spawn.cpp,
 * Documentation/network-protocol-v2.md section 8, "Host-assigned
 * spawns"): the host chooses every spawn (assign_spawn, gameseq.h); a
 * client asks for its respawn (SPAWN_REQUEST) and spawns where the answer
 * (SPAWN_SITE) says, or chooses itself if no answer came within
 * SPAWN_ANSWER_TIMEOUT.  Nothing here acts outside a network deathmatch.
 */
/* Level start, every machine (after multi_prep_level_objects): forget
 * assignments and reservations.
 */
void net_spawn_level_start();
/* The local player's death sequence may end (dead_player_frame): false
 * while a client waits for the host's answer (the request goes out on
 * the first call), true once the answer came, it timed out, or no answer
 * is needed.
 */
[[nodiscard]]
bool net_spawn_ready();
/* choose_spawn on a client: the site the host assigned to the local
 * player (for the respawn it asked for, or at its join in progress), if
 * one came; it is used once.
 */
[[nodiscard]]
std::optional<unsigned> net_spawn_take_assigned();
/* A SPAWN_REQUEST or SPAWN_SITE (net_v2_session.h) from `from` (a
 * client, on the host; the host, on a client).
 */
void net_spawn_receive(playernum_t from, uint8_t type, std::span<const uint8_t> payload);
/* The host: player `pnum` enters the level in progress (CLIENT_READY);
 * assign its first spawn and send it ahead of LEVEL_GO.
 */
void net_spawn_host_join(playernum_t pnum);

}
#endif

#else

static inline window_event_result multi_do_frame()
{
	return window_event_result::handled;
}

static inline void multi_send_controlcen_fire(const vms_vector &, int, objnum_t)
{
}

#ifdef DXX_BUILD_DESCENT
namespace dsx {

#if DXX_BUILD_DESCENT == 2

static inline void multi_send_guided_release(const object_base &)
{
}

#endif

static inline void net_interp_apply_all()
{
}

[[nodiscard]]
static inline bool net_interp_drives(const vcobjidx_t)
{
	return false;
}

static inline void net_interp_sweep_driven(const d_robot_info_array &, vmobjptridx_t)
{
}

static inline void net_interp_carry_flash(vcobjptridx_t, vcobjptridx_t)
{
}

[[nodiscard]]
static inline bool net_spawn_ready()
{
	return true;
}

}
#endif

#endif
