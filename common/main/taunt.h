/*
 * This file is part of the DXX-Rebirth project <https://github.com/dxx-rebirth/dxx-rebirth/>.
 * It is copyright by its individual contributors, as recorded in the
 * project's Git history.  See COPYING.txt at the top level for license
 * terms and a link to the Git history.
 */
/*
 * Taunts (Documentation/taunts.md): the horn a player sounds with the
 * "Taunt / Horn" key, heard from the player's ship by everyone nearby.
 * The sample and the rules are in taunt_sample.h; this is the game's
 * side: the key, the playback, the network, the bots, muting.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

#ifdef DXX_BUILD_DESCENT
#include "fwd-player.h"

namespace dsx {

#if DXX_BUILD_DESCENT == 2
/* The local player pressed the taunt key (alive, in a level). */
void taunt_key_pressed();
/* Once per frame: the taunts playing follow their ships. */
void taunt_frame();
/* The own file is read again the next time it is needed (the sound
 * options changed); returns a line saying what it found.
 */
std::string taunt_own_status(bool reload);
/* The sound options: what the own horn sounds like now. */
void taunt_preview();
/* A level starts: the samples are made now (not at the first horn in the
 * fight), and the spam limits start afresh (a slot may have a new player).
 */
void taunt_level_start();
/* The host: bot `pnum` (flown here) killed a player. */
void taunt_bot_kill(playernum_t pnum);
/* TAUNT_REQUEST and TAUNT (Documentation/network-protocol-v2.md,
 * "Taunts"), from the player in slot `from`.
 */
void net_taunt_receive(playernum_t from, std::uint8_t type, std::span<const std::uint8_t> payload);
/* `/mute name`, `/unmute name`, `/mute`: true if `text` was one (it is
 * then not sent as a message).
 */
bool taunt_chat_command(const char *text);
/* Phase 2, the samples as asset kind 2 of the ships' transfer
 * (net_ships.cpp): this machine has the sample with this SHA-256 (its
 * own, or received); its bytes in the transfer format; a received one,
 * checked (SHA-256, every field of the format) and kept in
 * taunts/cache/.
 */
bool taunt_asset_has(std::span<const std::uint8_t, 32> hash);
std::shared_ptr<const std::vector<std::uint8_t>> taunt_asset_file(std::span<const std::uint8_t, 32> hash);
bool taunt_asset_store(std::span<const std::uint8_t, 32> hash, std::span<const std::uint8_t> bytes);
#else
static inline void taunt_frame()
{
}
static inline void taunt_level_start()
{
}
static inline bool taunt_chat_command(const char *)
{
	return false;
}
#endif

}
#endif
