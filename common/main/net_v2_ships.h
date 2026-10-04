/*
 * This file is part of the DXX-Rebirth project <https://github.com/dxx-rebirth/dxx-rebirth/>.
 * It is copyright by its individual contributors, as recorded in the
 * project's Git history.  See COPYING.txt at the top level for license
 * terms and a link to the Git history.
 */
/*
 * Custom ships over the network (Documentation/custom-ships.md section
 * 5, exp-visuals protocol): every player announces its ship (SHIP_INFO),
 * the host tells everyone every player's, and a machine that lacks a
 * ship gets it from the host (SHIP_REQUEST, SHIP_DATA), the host first
 * fetching it from the player who flies it if it lacks it too.  No
 * prompt (decisions D8, D9): a pilot option refuses ships from the host.
 *
 * Message layouts, the assembly of a received ship and the whole
 * exchange as a state machine over an interface to the game and the
 * transport, so that common/unittest/net_v2_ships.cpp can run a host and
 * clients without the game.  Depends on the standard library and
 * net_v2.h only.
 */

#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "net_v2.h"

namespace dcx {

namespace net_v2 {

/* Message ids (session_msg, net_v2_session.h; 0x50/0x51 are the
 * taunts').  REQUEST, DATA and UNAVAILABLE move any kind of asset, named
 * by kind and SHA-256: ships (kind 1) and the taunts' sounds (kind 2,
 * Documentation/taunts.md).
 */
constexpr std::uint8_t SHIP_MSG_INFO{0x4b};
constexpr std::uint8_t SHIP_MSG_REQUEST{0x4c};
constexpr std::uint8_t SHIP_MSG_DATA{0x4d};
constexpr std::uint8_t SHIP_MSG_UNAVAILABLE{0x4e};

constexpr std::size_t SHIP_HASH_SIZE{32};
using ship_hash = std::array<std::uint8_t, SHIP_HASH_SIZE>;

enum class asset_kind : std::uint8_t
{
	ship = 1,
	/* The taunts' sounds (Documentation/taunts.md). */
	taunt = 2,
};

/* The largest asset of a kind; 0: not a kind this version knows. */
[[nodiscard]]
constexpr std::uint32_t asset_max_size(const std::uint8_t kind)
{
	switch (kind)
	{
		case static_cast<std::uint8_t>(asset_kind::ship):
			return 1u << 20;
		case static_cast<std::uint8_t>(asset_kind::taunt):
			return 128u << 10;
		default:
			return 0;
	}
}

struct asset_key
{
	std::uint8_t kind{static_cast<std::uint8_t>(asset_kind::ship)};
	ship_hash hash{};
	constexpr auto operator<=>(const asset_key &) const = default;
};
/* The name field: up to 24 characters, NUL padded. */
constexpr std::size_t SHIP_NAME_FIELD{25};
constexpr std::uint32_t SHIP_MAX_SIZE{1u << 20};
/* ASSET_DATA: kind, hash, total size, offset, then the bytes. */
constexpr std::size_t SHIP_DATA_HEADER{1 + SHIP_HASH_SIZE + 8};
constexpr std::size_t SHIP_DATA_CHUNK{896};
static_assert(SHIP_DATA_HEADER + SHIP_DATA_CHUNK <= NET_V2_MAX_MESSAGE);
/* Pacing: bytes per second in the lobby and during a level, and how
 * much of the reliable queue the transfers may fill (gameplay messages
 * never wait behind more than this).
 */
constexpr std::size_t SHIP_RATE_LOBBY{96 * 1024};
constexpr std::size_t SHIP_RATE_LEVEL{16 * 1024};
constexpr std::size_t SHIP_QUEUE_LIMIT{12 * 1024};
/* Seconds between two changes of a player's ship that the host takes. */
constexpr double SHIP_CHANGE_INTERVAL{3};

/* SHIP_INFO (pid, flags, size, hash, name): any player to the host for
 * itself, the host to everyone for every player (its bots too).
 */
struct ship_info_msg
{
	static constexpr std::size_t SIZE{1 + 1 + 4 + SHIP_HASH_SIZE + SHIP_NAME_FIELD};
	static constexpr std::uint8_t FLAG_PYRO{1};
	std::uint8_t pid{};
	bool pyro{true};
	std::uint32_t size{};
	ship_hash hash{};
	std::string name;
	void write(std::uint8_t *const p) const
	{
		p[0] = pid;
		p[1] = pyro ? FLAG_PYRO : 0;
		net_put_le32(p + 2, size);
		std::ranges::copy(hash, p + 6);
		std::fill_n(p + 6 + SHIP_HASH_SIZE, SHIP_NAME_FIELD, 0);
		std::copy_n(name.data(), std::min(name.size(), SHIP_NAME_FIELD - 1), p + 6 + SHIP_HASH_SIZE);
	}
	[[nodiscard]]
	static std::optional<ship_info_msg> read(const std::span<const std::uint8_t> b)
	{
		if (b.size() != SIZE || (b[1] & ~FLAG_PYRO))
			return std::nullopt;
		ship_info_msg m;
		m.pid = b[0];
		m.pyro = b[1] & FLAG_PYRO;
		m.size = net_get_le32(&b[2]);
		std::copy_n(&b[6], SHIP_HASH_SIZE, m.hash.begin());
		const auto name{b.subspan(6 + SHIP_HASH_SIZE, SHIP_NAME_FIELD)};
		const auto nul{std::ranges::find(name, std::uint8_t{0})};
		if (nul == name.end())
			return std::nullopt;
		for (auto i{name.begin()}; i != nul; ++i)
		{
			const auto c{*i};
			if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-'))
				return std::nullopt;
			m.name.push_back(static_cast<char>(c));
		}
		/* A Pyro carries no ship; a ship is named, of an allowed size. */
		if (m.pyro ? m.size || m.hash != ship_hash{} || !m.name.empty() : !m.size || m.size > SHIP_MAX_SIZE || m.name.empty())
			return std::nullopt;
		return m;
	}
};

/* SHIP_REQUEST (hash): a client to the host, or the host to the player
 * whose ship it fetches.
 */
struct ship_request_msg
{
	static constexpr std::size_t SIZE{1 + SHIP_HASH_SIZE};
	asset_key key;
	void write(std::uint8_t *const p) const
	{
		p[0] = key.kind;
		std::ranges::copy(key.hash, p + 1);
	}
	[[nodiscard]]
	static std::optional<ship_request_msg> read(const std::span<const std::uint8_t> b)
	{
		if (b.size() != SIZE || !asset_max_size(b[0]))
			return std::nullopt;
		ship_request_msg m;
		m.key.kind = b[0];
		std::copy_n(&b[1], SHIP_HASH_SIZE, m.key.hash.begin());
		return m;
	}
};

/* SHIP_DATA (hash, total, offset, bytes): in order, as the reliable
 * stream keeps it.
 */
struct ship_data_msg
{
	asset_key key;
	std::uint32_t total{}, offset{};
	std::span<const std::uint8_t> data;
	[[nodiscard]]
	std::size_t size() const
	{
		return SHIP_DATA_HEADER + data.size();
	}
	void write(std::uint8_t *const p) const
	{
		p[0] = key.kind;
		std::ranges::copy(key.hash, p + 1);
		net_put_le32(p + 1 + SHIP_HASH_SIZE, total);
		net_put_le32(p + 1 + SHIP_HASH_SIZE + 4, offset);
		std::ranges::copy(data, p + SHIP_DATA_HEADER);
	}
	[[nodiscard]]
	static std::optional<ship_data_msg> read(const std::span<const std::uint8_t> b)
	{
		if (b.size() <= SHIP_DATA_HEADER || b.size() > SHIP_DATA_HEADER + SHIP_DATA_CHUNK)
			return std::nullopt;
		ship_data_msg m;
		m.key.kind = b[0];
		std::copy_n(&b[1], SHIP_HASH_SIZE, m.key.hash.begin());
		m.total = net_get_le32(&b[1 + SHIP_HASH_SIZE]);
		m.offset = net_get_le32(&b[1 + SHIP_HASH_SIZE + 4]);
		m.data = b.subspan(SHIP_DATA_HEADER);
		if (!m.total || m.total > asset_max_size(m.key.kind) || m.offset >= m.total || m.data.size() > m.total - m.offset)
			return std::nullopt;
		return m;
	}
};

enum class ship_unavailable_reason : std::uint8_t
{
	unknown = 0,	/* nobody here has it */
	refused = 1,	/* its owner sends no ships */
	owner_left = 2,
	invalid = 3,	/* the data did not check out */
};

struct ship_unavailable_msg
{
	static constexpr std::size_t SIZE{1 + SHIP_HASH_SIZE + 1};
	asset_key key;
	ship_unavailable_reason reason{};
	void write(std::uint8_t *const p) const
	{
		p[0] = key.kind;
		std::ranges::copy(key.hash, p + 1);
		p[1 + SHIP_HASH_SIZE] = static_cast<std::uint8_t>(reason);
	}
	[[nodiscard]]
	static std::optional<ship_unavailable_msg> read(const std::span<const std::uint8_t> b)
	{
		if (b.size() != SIZE || !asset_max_size(b[0]) || b[1 + SHIP_HASH_SIZE] > static_cast<std::uint8_t>(ship_unavailable_reason::invalid))
			return std::nullopt;
		ship_unavailable_msg m;
		m.key.kind = b[0];
		std::copy_n(&b[1], SHIP_HASH_SIZE, m.key.hash.begin());
		m.reason = static_cast<ship_unavailable_reason>(b[1 + SHIP_HASH_SIZE]);
		return m;
	}
};

/* A ship being received: parts must come in order, of one size, never
 * past it.
 */
class ship_assembler
{
	asset_key expected;
	std::uint32_t total{};
	std::vector<std::uint8_t> bytes;
public:
	enum class result
	{
		more,
		complete,
		error,
	};
	explicit ship_assembler(const asset_key &k, const std::uint32_t expected_size = 0) :
		expected{k}, total{expected_size}
	{
	}
	[[nodiscard]]
	result add(const ship_data_msg &m)
	{
		if (m.key != expected || (total && m.total != total) || m.offset != bytes.size())
			return result::error;
		total = m.total;
		bytes.insert(bytes.end(), m.data.begin(), m.data.end());
		return bytes.size() == total ? result::complete : result::more;
	}
	[[nodiscard]]
	std::vector<std::uint8_t> take()
	{
		return std::move(bytes);
	}
	[[nodiscard]]
	std::size_t received() const
	{
		return bytes.size();
	}
};

/* What the exchange needs from the game and the transport.  Slots are
 * player numbers; a client talks only to slot 0, the host.
 */
class ship_exchange_env
{
public:
	virtual ~ship_exchange_env() = default;
	/* This machine has the asset. */
	virtual bool has_asset(const asset_key &) = 0;
	/* The local file of the asset, if this machine has it. */
	virtual std::shared_ptr<const std::vector<std::uint8_t>> asset_file(const asset_key &) = 0;
	/* A received asset: check it (SHA-256, the kind's own rules) and keep
	 * it; false if it does not check out.
	 */
	virtual bool store_asset(const asset_key &, std::span<const std::uint8_t>) = 0;
	/* A player's ship (nullptr: the Pyro); called again when it arrives. */
	virtual void player_ship(std::uint8_t pid, const ship_info_msg *) = 0;
	virtual void send(std::uint8_t slot, std::uint8_t type, std::span<const std::uint8_t> payload) = 0;
	/* Bytes waiting in the reliable queue to that slot. */
	virtual std::size_t queued_bytes(std::uint8_t slot) = 0;
	/* Host: the slot has a connected client (not a bot, not empty). */
	virtual bool is_client(std::uint8_t slot) = 0;
	/* A transfer started, finished or failed (for the log). */
	virtual void note(std::string_view what, const asset_key &, std::uint8_t slot) = 0;
};

class ship_exchange
{
public:
	static constexpr unsigned MAX_SLOTS{8};
private:
	struct outgoing
	{
		asset_key key;
		std::shared_ptr<const std::vector<std::uint8_t>> bytes;
		std::uint32_t offset{};
	};
	/* Host: an asset it fetches from its owner, and who waits for it. */
	struct fetch
	{
		std::uint8_t owner{};
		ship_assembler assembly;
		std::set<std::uint8_t> waiters;
	};
	/* Client: an asset it asked the host for. */
	enum class want_state
	{
		pending,
		done,
		failed,
	};
	struct want
	{
		want_state state{want_state::pending};
		std::optional<ship_assembler> assembly;
		std::uint32_t size{};
	};
	/* Host: who has which asset (announced by a player), and its size. */
	struct owner_entry
	{
		std::uint8_t slot;
		std::uint32_t size;
	};
	ship_exchange_env &env;
	bool host;
	std::uint8_t self;
	std::array<std::optional<ship_info_msg>, MAX_SLOTS> table{};
	std::array<std::deque<outgoing>, MAX_SLOTS> out{};
	std::map<asset_key, fetch> fetches;
	std::map<asset_key, want> wants;
	std::multimap<asset_key, owner_entry> owners;
	/* Pacing: bytes the transfers may still send, refilled per second. */
	double allowance{};
	/* Seconds the exchange has run (from pump), and when each slot last
	 * changed its ship: a change sooner than SHIP_CHANGE_INTERVAL after
	 * the last is ignored, so that nobody floods the others.
	 */
	double clock{};
	std::array<double, MAX_SLOTS> last_change{};
	template <typename M>
	void send_msg(const std::uint8_t slot, const std::uint8_t type, const M &m)
	{
		std::array<std::uint8_t, M::SIZE> buf;
		m.write(buf.data());
		env.send(slot, type, buf);
	}
	void unavailable(const std::uint8_t slot, const asset_key &k, const ship_unavailable_reason r)
	{
		send_msg(slot, SHIP_MSG_UNAVAILABLE, ship_unavailable_msg{k, r});
	}
	void queue_send(const std::uint8_t slot, const asset_key &k, std::shared_ptr<const std::vector<std::uint8_t>> bytes)
	{
		if (slot >= MAX_SLOTS || !bytes)
			return;
		auto &q{out[slot]};
		/* Asked again while on its way: once is enough. */
		if (std::ranges::any_of(q, [&k](const outgoing &o) { return o.key == k; }))
			return;
		env.note("sending", k, slot);
		q.push_back({k, std::move(bytes), 0});
	}
	[[nodiscard]]
	static asset_key ship_key(const ship_hash &h)
	{
		return {static_cast<std::uint8_t>(asset_kind::ship), h};
	}
	/* Host: a connected client other than `not_slot` that has it. */
	[[nodiscard]]
	std::optional<owner_entry> owner_of(const asset_key &k, const std::uint8_t not_slot) const
	{
		const auto [b, e]{owners.equal_range(k)};
		for (auto i{b}; i != e; ++i)
			if (i->second.slot != not_slot && env.is_client(i->second.slot))
				return i->second;
		return std::nullopt;
	}
	void host_fetch(const asset_key &k, const std::uint8_t owner, const std::optional<std::uint8_t> waiter, const std::uint32_t size)
	{
		auto [i, inserted]{fetches.try_emplace(k, fetch{owner, ship_assembler{k, size}, {}})};
		if (waiter)
			i->second.waiters.insert(*waiter);
		if (inserted)
		{
			env.note("fetching from its owner", k, owner);
			send_msg(owner, SHIP_MSG_REQUEST, ship_request_msg{k});
		}
	}
	void show_everyone_with(const asset_key &k)
	{
		if (k.kind != static_cast<std::uint8_t>(asset_kind::ship))
			return;
		for (std::uint8_t p = 0; p < MAX_SLOTS; ++p)
			if (table[p] && !table[p]->pyro && table[p]->hash == k.hash)
				env.player_ship(p, &*table[p]);
	}
	void receive_info(const std::uint8_t from, const ship_info_msg &m)
	{
		if (m.pid >= MAX_SLOTS)
			return;
		if (host)
		{
			/* A player speaks for itself only. */
			if (m.pid != from || from == self || !env.is_client(from))
				return;
			/* Nothing new: nothing to tell anyone. */
			if (table[from] && table[from]->pyro == m.pyro && table[from]->hash == m.hash && table[from]->size == m.size)
				return;
			if (table[from] && clock - last_change[from] < SHIP_CHANGE_INTERVAL)
				return;
			last_change[from] = clock;
			/* What the host fetched of its old ship is not needed. */
			for (auto i{fetches.begin()}; i != fetches.end();)
				if (i->second.owner == from && i->first.kind == static_cast<std::uint8_t>(asset_kind::ship) && !(i->first == ship_key(m.hash) && !m.pyro))
					fetch_failed(i++, ship_unavailable_reason::owner_left);
				else
					++i;
			table[from] = m;
			std::erase_if(owners, [from](const auto &o) { return o.second.slot == from && o.first.kind == static_cast<std::uint8_t>(asset_kind::ship); });
			if (!m.pyro)
				owners.insert({ship_key(m.hash), {from, m.size}});
			env.player_ship(from, m.pyro ? nullptr : &m);
			broadcast_info(from);
			/* The host draws it too: fetch it if it lacks it. */
			if (!m.pyro && accept && !env.has_asset(ship_key(m.hash)))
				host_fetch(ship_key(m.hash), from, std::nullopt, m.size);
		}
		else
		{
			if (from != 0 || m.pid == self)
				return;
			table[m.pid] = m;
			env.player_ship(m.pid, m.pyro ? nullptr : &m);
			if (!m.pyro)
			{
				/* Announced anew: one that failed before may work now. */
				if (const auto i{wants.find(ship_key(m.hash))}; i != wants.end() && i->second.state == want_state::failed)
					wants.erase(i);
				request(ship_key(m.hash), m.size);
			}
		}
	}
	void receive_request(const std::uint8_t from, const ship_request_msg &m)
	{
		const auto &k{m.key};
		if (host)
		{
			if (from == self || !env.is_client(from))
				return;
			if (auto f{env.asset_file(k)})
			{
				queue_send(from, k, std::move(f));
				return;
			}
			if (const auto i{fetches.find(k)}; i != fetches.end())
			{
				i->second.waiters.insert(from);
				return;
			}
			const auto owner{owner_of(k, from)};
			if (!owner)
			{
				unavailable(from, k, ship_unavailable_reason::unknown);
				return;
			}
			host_fetch(k, owner->slot, from, owner->size);
		}
		else
		{
			/* The host fetches this player's asset. */
			if (from != 0)
				return;
			if (!accept_sending)
			{
				unavailable(0, k, ship_unavailable_reason::refused);
				return;
			}
			if (auto f{env.asset_file(k)})
				queue_send(0, k, std::move(f));
			else
				unavailable(0, k, ship_unavailable_reason::unknown);
		}
	}
	void fetch_failed(const std::map<asset_key, fetch>::iterator i, const ship_unavailable_reason r)
	{
		for (const auto w : i->second.waiters)
			unavailable(w, i->first, r);
		/* Another kind's owner that failed is forgotten, so that the next
		 * request goes to another owner (it registers again when it
		 * announces the asset again).  Ships keep theirs: SHIP_INFO.
		 */
		if (i->first.kind != static_cast<std::uint8_t>(asset_kind::ship))
		{
			const auto owner{i->second.owner};
			const auto [b, e]{owners.equal_range(i->first)};
			for (auto o{b}; o != e;)
				o = o->second.slot == owner ? owners.erase(o) : std::next(o);
		}
		env.note("could not fetch", i->first, i->second.owner);
		fetches.erase(i);
	}
	void receive_data(const std::uint8_t from, const ship_data_msg &m)
	{
		const auto &k{m.key};
		if (host)
		{
			const auto i{fetches.find(k)};
			if (i == fetches.end() || i->second.owner != from)
				return;
			const auto r{i->second.assembly.add(m)};
			if (r == ship_assembler::result::more)
				return;
			if (r == ship_assembler::result::error)
			{
				fetch_failed(i, ship_unavailable_reason::invalid);
				return;
			}
			const auto bytes{i->second.assembly.take()};
			if (!env.store_asset(k, bytes))
			{
				fetch_failed(i, ship_unavailable_reason::invalid);
				return;
			}
			env.note("received", k, from);
			auto file{env.asset_file(k)};
			for (const auto w : i->second.waiters)
				queue_send(w, k, file);
			fetches.erase(i);
			show_everyone_with(k);
		}
		else
		{
			if (from != 0)
				return;
			const auto i{wants.find(k)};
			if (i == wants.end() || i->second.state != want_state::pending)
				return;
			auto &w{i->second};
			if (!w.assembly)
				w.assembly.emplace(k, w.size);
			const auto r{w.assembly->add(m)};
			if (r == ship_assembler::result::more)
				return;
			w.state = want_state::failed;
			if (r == ship_assembler::result::complete && env.store_asset(k, w.assembly->take()))
			{
				w.state = want_state::done;
				env.note("received", k, from);
				show_everyone_with(k);
			}
			else
				env.note("rejected", k, from);
			w.assembly.reset();
		}
	}
	void receive_unavailable(const std::uint8_t from, const ship_unavailable_msg &m)
	{
		if (host)
		{
			const auto i{fetches.find(m.key)};
			if (i != fetches.end() && i->second.owner == from)
				fetch_failed(i, m.reason);
		}
		else if (from == 0)
		{
			if (const auto i{wants.find(m.key)}; i != wants.end() && i->second.state == want_state::pending)
			{
				i->second.state = want_state::failed;
				env.note("not available", m.key, 0);
			}
		}
	}
	/* Host: tell every client a player's ship. */
	void broadcast_info(const std::uint8_t pid)
	{
		ship_info_msg m{};
		if (table[pid])
			m = *table[pid];
		m.pid = pid;
		for (std::uint8_t s = 1; s < MAX_SLOTS; ++s)
			if (env.is_client(s))
				send_msg(s, SHIP_MSG_INFO, m);
	}
public:
	/* This machine accepts ships and other assets (the pilot option of D8). */
	bool accept{true};
	/* This machine sends its own assets to the host when asked. */
	bool accept_sending{true};
	ship_exchange(ship_exchange_env &e, const bool is_host, const std::uint8_t self_slot) :
		env{e}, host{is_host}, self{self_slot}
	{
	}
	[[nodiscard]]
	bool is_host() const
	{
		return host;
	}
	[[nodiscard]]
	const std::optional<ship_info_msg> &info(const std::uint8_t pid) const
	{
		static const std::optional<ship_info_msg> none;
		return pid < MAX_SLOTS ? table[pid] : none;
	}
	/* Client: get an asset this machine lacks from the host (ships ask by
	 * themselves; another kind announces its assets its own way and asks
	 * through here).  `accept` is the pilot's option for ships; another
	 * kind's caller decides for itself (the taunts: "Hear other players'
	 * horns").
	 */
	void request(const asset_key &k, const std::uint32_t size)
	{
		if (host || (!accept && k.kind == static_cast<std::uint8_t>(asset_kind::ship)) || !asset_max_size(k.kind) || size > asset_max_size(k.kind) || env.has_asset(k))
			return;
		/* Another kind asks again after a failure (its owner may be back);
		 * the caller paces its asking (the taunts' rate limits).
		 */
		if (const auto i{wants.find(k)}; i != wants.end())
		{
			if (i->second.state != want_state::failed || k.kind == static_cast<std::uint8_t>(asset_kind::ship))
				return;
			wants.erase(i);
		}
		wants[k].size = size;
		env.note("asking the host for", k, 0);
		send_msg(0, SHIP_MSG_REQUEST, ship_request_msg{k});
	}
	/* Host: player `slot` has the asset (for another kind's
	 * announcements; ships register through SHIP_INFO).
	 */
	void note_owner(const asset_key &k, const std::uint8_t slot, const std::uint32_t size)
	{
		if (!host || slot >= MAX_SLOTS || !size || size > asset_max_size(k.kind))
			return;
		const auto [b, e]{owners.equal_range(k)};
		for (auto i{b}; i != e; ++i)
			if (i->second.slot == slot)
				return;
		owners.insert({k, {slot, size}});
	}
	/* Host: get an asset of another kind (a taunt's sound) from its owner
	 * `slot` for this machine itself; clients that ask later wait for the
	 * same fetch.
	 */
	void host_want(const asset_key &k, const std::uint8_t slot, const std::uint32_t size)
	{
		if (!host || slot >= MAX_SLOTS || slot == self || !size || size > asset_max_size(k.kind) || !env.is_client(slot) || env.has_asset(k) || fetches.contains(k))
			return;
		host_fetch(k, slot, std::nullopt, size);
	}
	/* This machine's own ship (and, on the host, a bot's): announce it.
	 * A client sends it to the host; the host tells everyone.
	 */
	void set_local(const std::uint8_t pid, const ship_info_msg &m)
	{
		if (pid >= MAX_SLOTS)
			return;
		auto copy{m};
		copy.pid = pid;
		table[pid] = copy;
		env.player_ship(pid, copy.pyro ? nullptr : &copy);
		if (host)
			broadcast_info(pid);
		else
			send_msg(0, SHIP_MSG_INFO, copy);
	}
	/* Host: a slot was emptied (a player or bot left). */
	void slot_cleared(const std::uint8_t pid)
	{
		if (pid >= MAX_SLOTS)
			return;
		table[pid].reset();
		env.player_ship(pid, nullptr);
		out[pid].clear();
		last_change[pid] = 0;
		std::erase_if(owners, [pid](const auto &o) { return o.second.slot == pid; });
		/* The others draw a Pyro there until someone new announces. */
		if (host)
			broadcast_info(pid);
		for (auto i{fetches.begin()}; i != fetches.end();)
		{
			i->second.waiters.erase(pid);
			if (i->second.owner == pid)
				fetch_failed(i++, ship_unavailable_reason::owner_left);
			else
				++i;
		}
	}
	/* Host: a client connected to `slot`; it learns everyone's ship. */
	void client_joined(const std::uint8_t slot)
	{
		if (!host || slot >= MAX_SLOTS)
			return;
		table[slot].reset();
		last_change[slot] = 0;
		env.player_ship(slot, nullptr);
		for (std::uint8_t p = 0; p < MAX_SLOTS; ++p)
			if (p != slot && table[p])
				send_msg(slot, SHIP_MSG_INFO, *table[p]);
	}
	void receive(const std::uint8_t from, const std::uint8_t type, const std::span<const std::uint8_t> payload)
	{
		if (from >= MAX_SLOTS)
			return;
		switch (type)
		{
			case SHIP_MSG_INFO:
				if (const auto m{ship_info_msg::read(payload)})
					receive_info(from, *m);
				break;
			case SHIP_MSG_REQUEST:
				if (const auto m{ship_request_msg::read(payload)})
					receive_request(from, *m);
				break;
			case SHIP_MSG_DATA:
				if (const auto m{ship_data_msg::read(payload)})
					receive_data(from, *m);
				break;
			case SHIP_MSG_UNAVAILABLE:
				if (const auto m{ship_unavailable_msg::read(payload)})
					receive_unavailable(from, *m);
				break;
			default:
				break;
		}
	}
	/* Send what the pacing allows: `seconds` since the last call, at the
	 * lobby's or the level's rate.
	 */
	void pump(const double seconds, const bool in_level)
	{
		clock += std::max(seconds, 0.0);
		const double rate{static_cast<double>(in_level ? SHIP_RATE_LEVEL : SHIP_RATE_LOBBY)};
		allowance = std::min(allowance + rate * std::max(seconds, 0.0), rate / 4);
		for (std::uint8_t slot = 0; slot < MAX_SLOTS && allowance > 0; ++slot)
		{
			auto &q{out[slot]};
			while (!q.empty() && allowance > 0 && env.queued_bytes(slot) < SHIP_QUEUE_LIMIT)
			{
				auto &o{q.front()};
				const auto total{static_cast<std::uint32_t>(o.bytes->size())};
				const auto n{std::min<std::size_t>(SHIP_DATA_CHUNK, total - o.offset)};
				const ship_data_msg m{o.key, total, o.offset, std::span(*o.bytes).subspan(o.offset, n)};
				std::array<std::uint8_t, SHIP_DATA_HEADER + SHIP_DATA_CHUNK> buf;
				m.write(buf.data());
				env.send(slot, SHIP_MSG_DATA, std::span(buf).first(m.size()));
				o.offset += static_cast<std::uint32_t>(n);
				allowance -= static_cast<double>(m.size());
				if (o.offset >= total)
				{
					env.note("sent", o.key, slot);
					q.pop_front();
				}
			}
		}
	}
	/* Anything still to send or to receive. */
	[[nodiscard]]
	bool busy() const
	{
		return !fetches.empty() || std::ranges::any_of(out, [](const auto &q) { return !q.empty(); }) ||
			std::ranges::any_of(wants, [](const auto &w) { return w.second.state == want_state::pending; });
	}
};

}

}
