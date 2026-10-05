/*
 * This file is part of the DXX-Rebirth project <https://github.com/dxx-rebirth/dxx-rebirth/>.
 * It is copyright by its individual contributors, as recorded in the
 * project's Git history.  See COPYING.txt at the top level for license
 * terms and a link to the Git history.
 */
/*
 * Custom ships over the network (Documentation/custom-ships.md section
 * 5): the game's side of the exchange of net_v2_ships.h.  Every player
 * announces its ship when it joins (and again when the pilot picks
 * another); the host gives its bots ships from its own and tells
 * everyone every player's; a machine that lacks a ship gets it from the
 * host, paced, verified, and kept in ships/cache/, unless the pilot
 * refuses ships (Options -> Ship...).
 */

#include "dxxsconf.h"

#if DXX_USE_MULTIPLAYER

#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>

#include "net_v2_ships.h"
#include "net_v2_game.h"
#include "custom_ship.h"
#include "multi.h"
#include "bot.h"
#include "player.h"
#include "playsave.h"
#include "console.h"
#include "timer.h"
#include "game.h"

namespace dsx {

namespace {

namespace nv = ::dcx::net_v2;
namespace cs = ::dcx::custom_ship;

std::string hash_text(const nv::asset_key &k)
{
	return ::dcx::sha256_hex(k.hash).substr(0, 12);
}

struct game_ship_env final : nv::ship_exchange_env
{
	/* Files read once per session for sending. */
	std::map<nv::asset_key, std::shared_ptr<const std::vector<std::uint8_t>>> files;
	bool has_asset(const nv::asset_key &k) override
	{
		return k.kind == static_cast<std::uint8_t>(nv::asset_kind::ship) && cs::find_hash(k.hash);
	}
	std::shared_ptr<const std::vector<std::uint8_t>> asset_file(const nv::asset_key &k) override
	{
		if (k.kind != static_cast<std::uint8_t>(nv::asset_kind::ship))
			return nullptr;
		if (const auto f{files.find(k)}; f != files.end())
			return f->second;
		const auto e{cs::find_hash(k.hash)};
		if (!e)
			return nullptr;
		auto bytes{cs::read_file(*e)};
		if (!bytes)
			return nullptr;
		auto p{std::make_shared<const std::vector<std::uint8_t>>(std::move(*bytes))};
		files.emplace(k, p);
		return p;
	}
	bool store_asset(const nv::asset_key &k, const std::span<const std::uint8_t> bytes) override
	{
		if (k.kind != static_cast<std::uint8_t>(nv::asset_kind::ship))
			return false;
		std::string error;
		if (!cs::store_received(bytes, k.hash, error))
		{
			con_printf(CON_URGENT, "ships: a received ship (%s) is refused: %s", hash_text(k).c_str(), error.c_str());
			return false;
		}
		return true;
	}
	void player_ship(const std::uint8_t pid, const nv::ship_info_msg *const m) override
	{
		custom_ship_set_player(pid, m ? &m->hash : nullptr);
		if (m && cs::find_hash(m->hash))
			custom_ship_preload();
	}
	void send(const std::uint8_t slot, const std::uint8_t type, const std::span<const std::uint8_t> payload) override
	{
		net_v2::game_send_to(slot, type, payload);
	}
	std::size_t queued_bytes(const std::uint8_t slot) override
	{
		return net_v2::game_queued_bytes(slot);
	}
	bool is_client(const std::uint8_t slot) override
	{
		return net_v2::host_slot_is_client(slot);
	}
	void note(const std::string_view what, const nv::asset_key &k, const std::uint8_t slot) override
	{
		const auto e{cs::find_hash(k.hash)};
		con_printf(CON_NORMAL, "ships: %.*s %s (%s) %s P#%u", static_cast<int>(what.size()), what.data(), e ? e->name.c_str() : "ship", hash_text(k).c_str(), multi_i_am_master() ? "with" : "via", slot);
	}
};

game_ship_env Env;
std::optional<nv::ship_exchange> X;
fix64 Last_pump;
uint8_t Self;
/* The ship each bot slot got (host), by callsign, so that a bot keeps
 * its ship for the session.
 */
std::array<std::string, MAX_PLAYERS> Bot_callsigns;

nv::ship_info_msg info_of(const cs::entry *const e)
{
	nv::ship_info_msg m;
	if (e)
	{
		m.pyro = false;
		m.size = e->size;
		m.hash = e->hash;
		m.name = e->name;
	}
	return m;
}

/* The local pilot's ship as configured now. */
nv::ship_info_msg local_info()
{
	const std::string_view name{PlayerCfg.ShipName.data()};
	return info_of(name.empty() ? nullptr : cs::find_name(name));
}

/* A bot's ship: one of the host's own ships, chosen by the bot's name, so
 * that it keeps it for the session (decision: random from the host's
 * ships).
 */
nv::ship_info_msg bot_info(const playernum_t pnum)
{
	std::vector<const cs::entry *> own;
	for (const auto &e : cs::list())
		if (!e.cached)
			own.push_back(&e);
	if (own.empty())
		return {};
	const char *const callsign{vcplayerptr(pnum)->callsign};
	uint32_t h{2166136261u};
	for (auto p{callsign}; *p; ++p)
	{
		h ^= static_cast<uint8_t>(*p);
		h *= 16777619u;
	}
	return info_of(own[h % own.size()]);
}

bool same(const std::optional<nv::ship_info_msg> &a, const nv::ship_info_msg &b)
{
	return a && a->pyro == b.pyro && a->hash == b.hash;
}

}

void net_ships_start(const bool host, const uint8_t self)
{
	X.reset();
	Env.files.clear();
	Bot_callsigns = {};
	custom_ship_clear_players();
	cs::rescan();
	Self = self;
	X.emplace(Env, host, self);
	X->accept = PlayerCfg.AcceptShips;
	Last_pump = timer_query();
	X->set_local(self, local_info());
}

void net_ships_reset()
{
	X.reset();
	Env.files.clear();
}

void net_ships_receive(const playernum_t from, const uint8_t type, const std::span<const uint8_t> payload)
{
	if (X)
		X->receive(static_cast<uint8_t>(from), type, payload);
}

void net_ships_client_joined(const playernum_t slot)
{
	if (X)
		X->client_joined(static_cast<uint8_t>(slot));
}

void net_ships_slot_cleared(const playernum_t slot)
{
	if (X && slot)
		X->slot_cleared(static_cast<uint8_t>(slot));
}

void net_ships_frame()
{
	if (!X)
		return;
	X->accept = PlayerCfg.AcceptShips;
	/* The pilot picked another ship (between levels): announce it. */
	if (const auto l{local_info()}; !same(X->info(Self), l))
		X->set_local(Self, l);
	if (X->is_host())
	{
		for (playernum_t p = 1; p < MAX_PLAYERS; ++p)
		{
			const bool bot{player_is_bot(p) && p < N_players && vcplayerptr(p)->connected != player_connection_status::disconnected};
			if (bot)
			{
				/* A bot (new, or another bot in the slot): its ship. */
				const std::string callsign{static_cast<const char *>(vcplayerptr(p)->callsign)};
				if (!X->info(p) || Bot_callsigns[p] != callsign)
				{
					Bot_callsigns[p] = callsign;
					X->set_local(static_cast<uint8_t>(p), bot_info(p));
				}
			}
			else if (!Bot_callsigns[p].empty())
			{
				/* The bot left; a joining human announces its own. */
				Bot_callsigns[p].clear();
				if (!net_v2::host_slot_is_client(p))
					X->slot_cleared(static_cast<uint8_t>(p));
			}
		}
	}
	const auto now{timer_query()};
	const double seconds{f2fl(static_cast<fix>(std::min<fix64>(now - Last_pump, F1_0)))};
	Last_pump = now;
	X->pump(seconds, Network_status == network_state::playing);
}

}

#endif
