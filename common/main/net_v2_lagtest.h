/*
 * This file is part of the DXX-Rebirth project <https://github.com/dxx-rebirth/dxx-rebirth/>.
 * It is copyright by its individual contributors, as recorded in the
 * project's Git history.  See COPYING.txt at the top level for license
 * terms and a link to the Git history.
 */
/*
 * Multiplayer network protocol v2, stage 3: the `-lagtest <ms>` test
 * option (Documentation/network-protocol-v2.md, "Stage 3 as
 * implemented").
 *
 * A host playing alone (with bots) feels its own pickups as a client with
 * a round trip of `ms` would: the touched powerup is hidden at once, the
 * request reaches the host's decision half a round trip later (so a bot
 * may take the powerup first, as another player could), and the answer
 * reaches the ship the other half later.  This is the queue of those
 * requests and answers; it depends on the standard library only, so that
 * common/unittest/net_v2_authority.cpp can exercise it without the game.
 * The game side is similar/main/net_objects.cpp.
 */

#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>

#include "net_v2_objects.h"

namespace dcx {

namespace net_v2 {

/* Milliseconds as game time (fix64, 1.0 = 65536). */
[[nodiscard]]
constexpr std::int64_t lagtest_time(const unsigned ms)
{
	return static_cast<std::int64_t>(ms) * 65536 / 1000;
}

struct lag_pickup
{
	enum class stage : std::uint8_t
	{
		/* On its way to the host's decision. */
		request,
		/* Decided; the answer is on its way to the ship. */
		grant,
		deny,
	};
	stage what{};
	std::int64_t due{};
	std::uint32_t order{};
	netid_t netid{NETID_NONE};
	std::uint8_t powerup_id{};
	/* The touched object, which the grant's effect is taken from even
	 * after its net id is gone (as a client still has its copy of the
	 * object when the grant arrives).
	 */
	std::uint16_t objnum{};
	std::uint16_t signature{};
	/* A grant: what the decision gave, and the life it is for. */
	pickup_desc desc{};
	pickup_outcome outcome{};
	std::uint8_t life{};
};

template <std::size_t N = 32>
class lag_pickups
{
public:
	void reset()
	{
		entries_.fill({});
		used_.fill(false);
		order_ = 0;
	}
	/* The round trip is split into the request's way (the smaller half)
	 * and the answer's.
	 */
	void set_round_trip(const std::int64_t rtt)
	{
		up_ = rtt / 2;
		down_ = rtt - up_;
	}
	[[nodiscard]]
	std::int64_t up() const
	{
		return up_;
	}
	[[nodiscard]]
	std::int64_t down() const
	{
		return down_;
	}
	/* The ship touched a powerup at `now`.  False when the queue is full
	 * (the caller does not ask, as if the touch had not happened).
	 */
	bool request(const std::int64_t now, const netid_t netid, const std::uint8_t powerup_id, const std::uint16_t objnum, const std::uint16_t signature)
	{
		lag_pickup p;
		p.what = lag_pickup::stage::request;
		p.due = now + up_;
		p.netid = netid;
		p.powerup_id = powerup_id;
		p.objnum = objnum;
		p.signature = signature;
		return push(p);
	}
	/* The host's decision on `req` at `now` (when it arrived). */
	bool grant(const std::int64_t now, const lag_pickup &req, const pickup_desc &desc, const pickup_outcome &outcome, const std::uint8_t life)
	{
		auto p{req};
		p.what = lag_pickup::stage::grant;
		p.due = now + down_;
		p.desc = desc;
		p.outcome = outcome;
		p.life = life;
		return push(p);
	}
	bool deny(const std::int64_t now, const lag_pickup &req)
	{
		auto p{req};
		p.what = lag_pickup::stage::deny;
		p.due = now + down_;
		return push(p);
	}
	/* The earliest entry due at `now` (the first queued among equal
	 * times), taken out of the queue.
	 */
	[[nodiscard]]
	std::optional<lag_pickup> next_due(const std::int64_t now)
	{
		std::size_t best{N};
		for (std::size_t i = 0; i < N; ++i)
		{
			if (!used_[i] || entries_[i].due > now)
				continue;
			if (best == N || entries_[i].due < entries_[best].due || (entries_[i].due == entries_[best].due && seq_before(entries_[i].order, entries_[best].order)))
				best = i;
		}
		if (best == N)
			return std::nullopt;
		used_[best] = false;
		return entries_[best];
	}
	/* `inv` with the grants for `life` that are decided but not applied
	 * yet, in the order they were decided: the host's copy of a client
	 * holds them as soon as it sends them.  Without `vitals` shields and
	 * energy are left out (a grant that reaches a dead ship).
	 */
	[[nodiscard]]
	inventory with_grants(inventory inv, const inventory_rules &r, const std::uint8_t life, const bool vitals) const
	{
		std::array<const lag_pickup *, N> g{};
		std::size_t n{0};
		for (std::size_t i = 0; i < N; ++i)
			if (used_[i] && entries_[i].what == lag_pickup::stage::grant && entries_[i].life == life)
			{
				if (!vitals && (entries_[i].desc.kind == pickup_kind::shield || entries_[i].desc.kind == pickup_kind::energy))
					continue;
				/* In the order they were decided. */
				std::size_t j{n++};
				for (; j > 0 && seq_before(entries_[i].order, g[j - 1]->order); --j)
					g[j] = g[j - 1];
				g[j] = &entries_[i];
			}
		for (std::size_t i = 0; i < n; ++i)
			apply_pickup(inv, r, g[i]->desc, g[i]->outcome);
		return inv;
	}
	/* Whether an entry refers to this object. */
	[[nodiscard]]
	bool holds(const std::uint16_t objnum, const std::uint16_t signature) const
	{
		for (std::size_t i = 0; i < N; ++i)
			if (used_[i] && entries_[i].objnum == objnum && entries_[i].signature == signature)
				return true;
		return false;
	}
	[[nodiscard]]
	std::size_t size() const
	{
		return static_cast<std::size_t>(std::count(used_.begin(), used_.end(), true));
	}
private:
	[[nodiscard]]
	static bool seq_before(const std::uint32_t a, const std::uint32_t b)
	{
		return static_cast<std::int32_t>(a - b) < 0;
	}
	bool push(lag_pickup p)
	{
		for (std::size_t i = 0; i < N; ++i)
			if (!used_[i])
			{
				p.order = order_++;
				entries_[i] = p;
				used_[i] = true;
				return true;
			}
		return false;
	}
	std::array<lag_pickup, N> entries_{};
	std::array<bool, N> used_{};
	std::uint32_t order_{};
	std::int64_t up_{};
	std::int64_t down_{};
};

}

}
