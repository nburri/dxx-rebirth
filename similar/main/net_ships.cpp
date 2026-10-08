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
 * another); the host gives its bots their ships (the bot's setting, its
 * style profile's ship, else one of its own at random) and tells
 * everyone every player's; a machine that lacks a ship gets it from the
 * host, paced, verified, and kept in ships/cache/, unless the pilot
 * refuses ships or does not show custom ships (Options -> Ship...).
 */

#include "dxxsconf.h"

#if DXX_USE_MULTIPLAYER

#include <algorithm>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

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
#include "taunt.h"
#include "physfsx.h"

namespace dsx {

namespace {

namespace nv = ::dcx::net_v2;
namespace cs = ::dcx::custom_ship;
namespace b = ::dcx::bot;

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
#if DXX_BUILD_DESCENT == 2
		/* Documentation/taunts.md, phase 2. */
		if (k.kind == static_cast<std::uint8_t>(nv::asset_kind::taunt))
			return taunt_asset_has(k.hash);
#endif
		return k.kind == static_cast<std::uint8_t>(nv::asset_kind::ship) && cs::find_hash(k.hash);
	}
	std::shared_ptr<const std::vector<std::uint8_t>> asset_file(const nv::asset_key &k) override
	{
#if DXX_BUILD_DESCENT == 2
		if (k.kind == static_cast<std::uint8_t>(nv::asset_kind::taunt))
			return taunt_asset_file(k.hash);
#endif
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
#if DXX_BUILD_DESCENT == 2
		if (k.kind == static_cast<std::uint8_t>(nv::asset_kind::taunt))
			return taunt_asset_store(k.hash, bytes);
#endif
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
		const bool taunt{k.kind == static_cast<std::uint8_t>(nv::asset_kind::taunt)};
		const auto e{taunt ? nullptr : cs::find_hash(k.hash)};
		con_printf(CON_NORMAL, "ships: %.*s %s (%s) %s P#%u", static_cast<int>(what.size()), what.data(), e ? e->name.c_str() : taunt ? "taunt sample" : "ship", hash_text(k).c_str(), multi_i_am_master() ? "with" : "via", slot);
	}
};

game_ship_env Env;
std::optional<nv::ship_exchange> X;
fix64 Last_pump;
uint8_t Self;
/* What each bot slot's ship was chosen from (host): the callsign, the
 * ship setting and the style profile's ship.  The ship is chosen again
 * (and announced) when one of them changes, so that a bot keeps its
 * ship for the session otherwise.
 */
std::array<std::string, MAX_PLAYERS> Bot_keys;

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

/* What bot `pnum` flies, as the host has it: its setting and the ship
 * its style profile names (Documentation/multiplayer-bots.md section
 * 9.20).
 */
struct bot_wish
{
	b::ship_choice choice{};
	std::string profile_ship;
	std::string callsign;
	[[nodiscard]]
	std::string key() const
	{
		return callsign + '|' + format_ship_choice(choice).data() + '|' + profile_ship;
	}
};

bot_wish bot_wish_of(const playernum_t pnum)
{
	bot_wish w;
	w.callsign = static_cast<const char *>(vcplayerptr(pnum)->callsign);
	if (const auto cfg{bot_local_config(pnum)})
	{
		w.choice = cfg->ship;
		if (cfg->profile[0])
			if (const auto ls{bots_style_library().find(cfg->profile.data())})
				w.profile_ship = ls->profile.ship;
	}
	return w;
}

/* A bot's ship: its setting, else its style profile's ship, else one of
 * the host's own ships chosen by the bot's name; a ship the host does not
 * have (any more) is replaced by one at random, said on the console.
 */
nv::ship_info_msg bot_info(const bot_wish &w)
{
	/* A ship file deleted since the folder was read: read it again. */
	if (std::ranges::any_of(cs::list(), [](const cs::entry &e) { return !PHYSFS_exists(e.path.c_str()); }))
		cs::rescan();
	const auto &ships{cs::list()};
	std::vector<b::ship_candidate> candidates;
	candidates.reserve(ships.size());
	for (const auto &e : ships)
		candidates.push_back({e.name, e.hash, e.cached});
	const auto r{b::resolve_ship(w.choice, w.profile_ship, w.callsign, candidates)};
	const auto e{r.ship ? &ships[*r.ship] : nullptr};
	const char *const flies{e ? e->name.c_str() : "the Pyro-GX"};
	if (r.missing)
	{
		const char *const wanted{r.from_profile ? w.profile_ship.c_str() : w.choice.name.data()};
		con_printf(CON_URGENT, "ships: bot '%s': no ship \"%s\" in the ships folder%s; it flies %s (Random)", w.callsign.c_str(), wanted, r.from_profile ? " (its style's)" : "", flies);
	}
	else
		con_printf(CON_NORMAL, "ships: bot '%s' flies %s%s", w.callsign.c_str(), flies, r.random ? " (Random)" : r.from_profile ? " (its style's)" : "");
	return info_of(e);
}

/* Ships are fetched for drawing only: not when the pilot refuses them or
 * does not show custom ships (a host still relays them to its clients).
 */
bool accepts_ships()
{
	return PlayerCfg.AcceptShips && custom_ships_shown();
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
	Bot_keys = {};
	custom_ship_clear_players();
	cs::rescan();
	Self = self;
	X.emplace(Env, host, self);
	X->accept = accepts_ships();
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
#if DXX_BUILD_DESCENT == 2
	taunt_slot_reset(slot);
#endif
	if (X)
		X->client_joined(static_cast<uint8_t>(slot));
}

void net_ships_slot_cleared(const playernum_t slot)
{
#if DXX_BUILD_DESCENT == 2
	taunt_slot_reset(slot);
#endif
	if (X && slot)
		X->slot_cleared(static_cast<uint8_t>(slot));
}

void net_ships_taunt_request(const std::span<const uint8_t, 32> hash, const uint32_t size)
{
	if (!X)
		return;
	nv::asset_key k{static_cast<std::uint8_t>(nv::asset_kind::taunt), {}};
	std::ranges::copy(hash, k.hash.begin());
	X->request(k, size);
}

void net_ships_taunt_owner(const playernum_t slot, const std::span<const uint8_t, 32> hash, const uint32_t size, const bool want)
{
	if (!X || slot >= MAX_PLAYERS)
		return;
	nv::asset_key k{static_cast<std::uint8_t>(nv::asset_kind::taunt), {}};
	std::ranges::copy(hash, k.hash.begin());
	X->note_owner(k, static_cast<uint8_t>(slot), size);
	if (want)
		X->host_want(k, static_cast<uint8_t>(slot), size);
}

void net_ships_frame()
{
	if (!X)
		return;
	if (const bool accept{accepts_ships()}; accept != X->accept)
	{
		X->accept = accept;
		/* Turned on: fetch the ships already announced that are missing. */
		if (accept)
			X->request_missing_ships();
	}
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
				/* A bot (new, or another bot in the slot, renamed, or
				 * given another ship): its ship.
				 */
				const auto wish{bot_wish_of(p)};
				if (auto key{wish.key()}; !X->info(p) || Bot_keys[p] != key)
				{
					Bot_keys[p] = std::move(key);
					X->set_local(static_cast<uint8_t>(p), bot_info(wish));
				}
			}
			else if (!Bot_keys[p].empty())
			{
				/* The bot left; a joining human announces its own. */
				Bot_keys[p].clear();
#if DXX_BUILD_DESCENT == 2
				taunt_slot_reset(p);
#endif
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
