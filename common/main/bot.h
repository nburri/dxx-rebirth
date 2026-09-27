/*
 * This file is part of the DXX-Rebirth project <https://github.com/dxx-rebirth/dxx-rebirth/>.
 * It is copyright by its individual contributors, as recorded in the
 * project's Git history.  See COPYING.txt at the top level for license
 * terms and a link to the Git history.
 */
/*
 * Multiplayer bots (Documentation/multiplayer-bots.md): extra players of
 * a network game, flown by the host through the same controls, physics,
 * weapons and messages as a human.  To the clients a bot is an ordinary
 * remote player.
 *
 * - similar/main/bot_menu.cpp: the setup (count, names, skill, style,
 *   team) and its menus (section 6);
 * - similar/main/bot.cpp: the bots in the game on the host: slots, the
 *   brain on its fixed tick, navigation, firing, damage, death and
 *   respawn (sections 2 to 4 and 7.1);
 * - common/main/bot_brain.h, bot_nav.h: the game-independent logic.
 *
 * Nothing here acts outside a network game hosted on this machine.
 */

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include "dxxsconf.h"
#include "dsx-ns.h"
#include "fwd-player.h"
#include "player-callsign.h"
#include "bot_brain.h"

namespace dcx {

constexpr unsigned MAX_BOTS{MAX_PLAYERS - 1};

struct bot_config
{
	callsign_t name{};
	bot::bot_skill skill{bot::BOT_DEFAULT_SKILL};
	bot::bot_style style{bot::bot_style::balanced};
	bot::bot_team team{bot::bot_team::automatic};
};

/* The host's bot setup, from the setup menu (section 6.2).  Bots take
 * player slots only when the game starts (section 2.3).
 */
struct bot_setup
{
	unsigned count{};
	bot::bot_skill default_skill{bot::BOT_DEFAULT_SKILL};
	bot::bot_style default_style{bot::bot_style::balanced};
	std::array<bot_config, MAX_BOTS> bots{};
	bool initialized{};
};

extern bot_setup Bot_setup;

}

#ifdef DXX_BUILD_DESCENT
#include "fwd-object.h"
#include "fwd-segment.h"
#include "fwd-robot.h"
#include "maths.h"
#include "multi.h"

namespace dsx {

#if DXX_USE_MULTIPLAYER
/* Setup (bot_menu.cpp). */
/* The first time the host setup menu opens: the `-bots N` count and the
 * default names.
 */
void bots_setup_init();
/* The Bots screen (section 6.2) and its per-bot screen (6.3). */
void bots_setup_menu(network_game_type mode, unsigned max_players);
/* The label of the "Bots..." item of the host setup menu. */
void bots_setup_label(char *buf, std::size_t size, network_game_type mode);
/* Bots play anarchy, team anarchy and bounty until stage B7. */
[[nodiscard]]
bool bots_allowed_in_mode(network_game_type mode);

/* In the game (bot.cpp). */
/* The host, when the lobby closes: the configured bots take the lowest
 * free slots below the player limit.  Returns the number placed (fewer
 * than configured if the game is full).
 */
unsigned bots_allocate_slots();
/* The team menu's starting teams: bots with a team preference. */
void bots_apply_team_preferences(unsigned &team_vector, unsigned num_players);
/* Player `pnum` is a bot this machine flies (the host). */
[[nodiscard]]
bool bot_is_local(playernum_t pnum);
/* The bot's ship is in its death tumble (the bundle's `dying`). */
[[nodiscard]]
bool bot_ship_dying(playernum_t pnum);
/* The level started (after StartLevel): set the bots' ships up. */
void bots_level_start();
/* The level ended on the host: the bots are done with it. */
void bots_level_end();
/* The session ended or a new one began: no bots. */
void bots_session_reset();
/* Once per frame before the objects move: the brain's ticks, the
 * controls of this frame, the death sequences and respawns.
 */
void bots_frame(const d_robot_info_array &Robot_info);
/* Once per frame after the objects moved: the bots' primary fire. */
void bots_fire();
/* object_move_one, for a ship with control_source remote: a bot's ship
 * takes its controls (apply_pilot_controls).
 */
void bot_apply_controls(object &obj);
/* apply_damage_to_player for a bot's ship: true if `ship` is a local
 * bot (the damage was handled, or ignored as the rules say).
 */
bool bot_take_damage(object &ship, icobjptridx_t killer, fix damage, bool check_friendly_fire);
/* collide_player_and_wall for a bot's ship: open the door it flew into.
 * True if `ship` is a local bot.
 */
bool bot_hit_wall(const object &ship, vmsegptridx_t seg, sidenum_t side);
#else
[[nodiscard]]
static inline bool bot_is_local(playernum_t)
{
	return false;
}
[[nodiscard]]
static inline bool bot_ship_dying(playernum_t)
{
	return false;
}
static inline void bots_frame(const d_robot_info_array &)
{
}
static inline void bots_fire()
{
}
static inline void bot_apply_controls(object &)
{
}
static inline bool bot_take_damage(object &, icobjptridx_t, fix, bool)
{
	return false;
}
static inline bool bot_hit_wall(const object &, vmsegptridx_t, sidenum_t)
{
	return false;
}
static inline void bots_level_start()
{
}
#endif

}
#endif
