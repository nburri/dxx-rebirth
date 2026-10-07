/*
 * This file is part of the DXX-Rebirth project <https://github.com/dxx-rebirth/dxx-rebirth/>.
 * It is copyright by its individual contributors, as recorded in the
 * project's Git history.  See COPYING.txt at the top level for license
 * terms and a link to the Git history.
 */
/*
 * Mission transfer, the game's side (net_mission.cpp,
 * common/main/net_v2_mission.h).
 */

#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>

#include "dxxsconf.h"
#include "dsx-ns.h"
#include "fwd-player.h"

#ifdef DXX_BUILD_DESCENT
namespace dsx {

/* Host: the session opened; describe Current_mission. */
void net_mission_host_start();
void net_mission_reset();
void net_mission_frame();
/* MISSION_MANIFEST and ASSET_* messages of kind 3. */
void net_mission_receive(playernum_t from, uint8_t type, std::span<const uint8_t> payload);
/* Host: a client connected to / left `slot`. */
void net_mission_client_joined(playernum_t slot);
void net_mission_slot_cleared(playernum_t slot);
/* Host: files are on their way to `slot`; how far, in percent. */
[[nodiscard]]
bool net_mission_host_busy(playernum_t slot);
[[nodiscard]]
std::optional<unsigned> net_mission_host_progress(playernum_t slot);
/* Host: `slot` asked for the mission since it connected. */
[[nodiscard]]
bool net_mission_host_requested(playernum_t slot);
/* GAME_SETTINGS' mission announcement (net_v2::mission_announcement,
 * 37 bytes): the host writes its own, a client reads the host's.
 */
void net_mission_write_announcement(uint8_t *p);
void net_mission_read_announcement(const uint8_t *p);

enum class join_mission : uint8_t
{
	/* The host's version is here and loaded. */
	loaded,
	/* No version announced: load by name as before. */
	by_name,
	/* Join, then download it (net_mission_client_status). */
	download,
	/* Cannot join: the message says why. */
	failed,
};
/* Before the join: find the host's mission (file name `basename`, the
 * announced bundle) among this machine's, or decide to download it.
 */
[[nodiscard]]
join_mission net_mission_prepare_join(const char *basename, std::string &message);
/* The join was accepted: start the download if one is needed. */
void net_mission_client_connected();
[[nodiscard]]
bool net_mission_download_needed();
struct client_mission_status
{
	enum
	{
		running,
		done,
		failed,
	} state{running};
	unsigned percent{};
	uint64_t received{}, total{};
	std::string title, error;
};
[[nodiscard]]
client_mission_status net_mission_client_status();
/* The download is complete: load the mission (nullptr, or the error). */
const char *net_mission_client_finish();
void net_mission_client_cancel();

}
#endif
