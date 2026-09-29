/*
 * This file is part of the DXX-Rebirth project <https://github.com/dxx-rebirth/dxx-rebirth/>.
 * It is copyright by its individual contributors, as recorded in the
 * project's Git history.  See COPYING.txt at the top level for license
 * terms and a link to the Git history.
 */
/*
 * Host-assigned spawns in a network deathmatch
 * (Documentation/network-protocol-v2.md section 8, "Host-assigned
 * spawns").
 *
 * Every machine used to choose its own respawn site, ranking the sites
 * by the distance to the other ships as it saw them: remote ships
 * delayed by the interpolation, and nothing of a spawn another machine
 * chose a moment ago.  Two players (or a player and a bot) who respawned
 * at about the same time could take the same site.  Now the host
 * chooses every spawn with the same ranking (assign_spawn, gameseq.cpp)
 * from the newest states it has, and counts the sites it assigned in the
 * last seconds as taken.  A client whose death sequence ends asks
 * (SPAWN_REQUEST) and waits for the answer (SPAWN_SITE); if none comes
 * within SPAWN_ANSWER_TIMEOUT it chooses itself, as before.  A player
 * joining a level in progress gets its first site unasked, ahead of
 * LEVEL_GO.
 */

#include "dxxsconf.h"

#if DXX_USE_MULTIPLAYER

#include <array>
#include <optional>
#include <span>

#include "net_v2_objects.h"
#include "net_v2_session.h"
#include "net_v2_game.h"
#include "multi.h"
#include "gameseq.h"
#include "object.h"
#include "player.h"
#include "newdemo.h"
#include "timer.h"
#include "console.h"
#include "d_levelstate.h"

namespace dsx {

namespace {

namespace nv = ::dcx::net_v2;
using nv::session_msg;

/* The answer is reliable, so it is only this late on a link that is
 * about to time out; the player then spawns where it chooses itself.
 */
constexpr fix64 SPAWN_ANSWER_TIMEOUT{F1_0};

struct client_spawn_state
{
	/* The last request sent (0: none yet). */
	uint8_t request{};
	/* A request is on its way; since when (timer_query). */
	bool waiting{};
	fix64 sent{};
	/* The host answered (the current request, or a join in progress);
	 * `site` is empty if it had none to offer.
	 */
	bool answered{};
	std::optional<unsigned> site;
};

client_spawn_state C;

/* A network deathmatch, played (not a demo played back). */
[[nodiscard]]
bool spawns_assigned()
{
	return +(Game_mode & GM_NETWORK) && !(Game_mode & GM_MULTI_COOP) && Newdemo_state != ND_STATE_PLAYBACK;
}

void send_site(const playernum_t pnum, const uint8_t request, const spawn_choice spawn)
{
	nv::spawn_site_msg m{request, nv::SPAWN_SITE_NONE};
	if (spawn.what == spawn_choice::kind::site && spawn.site < nv::SPAWN_SITE_NONE)
		m.site = static_cast<uint8_t>(spawn.site);
	std::array<uint8_t, nv::spawn_site_msg::SIZE> buf;
	m.write(buf);
	::dsx::net_v2::game_send_to(pnum, static_cast<uint8_t>(session_msg::spawn_site), buf);
	con_printf(CON_VERBOSE, "net: spawn site %u assigned to P#%u (request %u)", m.site, pnum, request);
}

void host_receive_request(const playernum_t from, const std::span<const uint8_t> payload)
{
	const auto rq{nv::spawn_request_msg::read(payload)};
	if (!rq || from == Player_num || from >= N_players || !spawns_assigned())
		return;
	auto &Objects = LevelUniqueObjectState.Objects;
	send_site(from, rq->request, assign_spawn(Objects.vmptr, from));
}

void client_receive_site(const std::span<const uint8_t> payload)
{
	const auto m{nv::spawn_site_msg::read(payload)};
	if (!m)
		return;
	if (m->request != nv::SPAWN_REQUEST_JOIN && !(C.waiting && m->request == C.request))
	{
		con_printf(CON_VERBOSE, "net: late spawn site %u for request %u ignored", m->site, m->request);
		return;
	}
	C.waiting = false;
	C.answered = true;
	if (m->site != nv::SPAWN_SITE_NONE)
		C.site = m->site;
	else
		C.site.reset();
}

}

void net_spawn_level_start()
{
	C = {};
	spawn_reservations_reset();
}

bool net_spawn_ready()
{
	if (!spawns_assigned() || multi_i_am_master())
		return true;
	if (C.answered)
		return true;
	const fix64 now{timer_query()};
	if (!C.waiting)
	{
		if (!++C.request)
			C.request = 1;
		nv::spawn_request_msg rq{C.request};
		std::array<uint8_t, nv::spawn_request_msg::SIZE> buf;
		rq.write(buf);
		::dsx::net_v2::game_broadcast(static_cast<uint8_t>(session_msg::spawn_request), buf);
		C.waiting = true;
		C.sent = now;
		return false;
	}
	if (now - C.sent < SPAWN_ANSWER_TIMEOUT)
		return false;
	con_printf(CON_NORMAL, "net: no spawn site from the host within 1 s (request %u); choosing here", C.request);
	C.waiting = false;
	return true;
}

std::optional<unsigned> net_spawn_take_assigned()
{
	const auto site{C.site};
	C.site.reset();
	C.answered = false;
	C.waiting = false;
	return site;
}

void net_spawn_receive(const playernum_t from, const uint8_t type, const std::span<const uint8_t> payload)
{
	if (!spawns_assigned())
		return;
	const auto t{static_cast<session_msg>(type)};
	if (multi_i_am_master())
	{
		if (t == session_msg::spawn_request)
			host_receive_request(from, payload);
	}
	else if (t == session_msg::spawn_site)
		client_receive_site(payload);
}

void net_spawn_host_join(const playernum_t pnum)
{
	if (!spawns_assigned() || !multi_i_am_master())
		return;
	auto &Objects = LevelUniqueObjectState.Objects;
	const auto spawn{assign_spawn(Objects.vmptr, pnum)};
	if (spawn.what == spawn_choice::kind::site)
		send_site(pnum, nv::SPAWN_REQUEST_JOIN, spawn);
}

}

#endif
