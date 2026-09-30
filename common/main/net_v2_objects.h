/*
 * This file is part of the DXX-Rebirth project <https://github.com/dxx-rebirth/dxx-rebirth/>.
 * It is copyright by its individual contributors, as recorded in the
 * project's Git history.  See COPYING.txt at the top level for license
 * terms and a link to the Git history.
 */
/*
 * Multiplayer network protocol v2, stage 3: object authority and pickups
 * (Documentation/network-protocol-v2.md, sections 6.1-6.4).
 *
 * The game-independent part: network object ids and their tables, the
 * inventory model with the pickup and drop rules the host decides by, the
 * host's copy of a client's inventory (reconciled with the grants still
 * on their way to it), the client's table of pickups it asked for, the
 * respawn bookkeeping, the range check and the wire layouts of the
 * stage 3 messages.  Like net_v2.h it depends on the standard library
 * only, so that common/unittest/net_v2_authority.cpp can exercise it
 * without the game.  The game side is similar/main/net_objects.cpp.
 */

#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

#include "net_v2.h"

namespace dcx {

namespace net_v2 {

/* Section 6.1: network object ids. */
using netid_t = std::uint16_t;
constexpr netid_t NETID_NONE{0xffff};
constexpr netid_t NETID_LEVEL_BIT{0x8000};
constexpr netid_t NETID_COUNTER_MASK{0x0fff};
constexpr unsigned NETID_COUNTER_BITS{12};
constexpr unsigned NETID_CREATORS{8};

/* An object that exists at level load (after the multiplayer preparation
 * of the level, which is identical on every machine) is known by its
 * object number there.
 */
[[nodiscard]]
constexpr netid_t level_netid(const std::uint16_t objnum)
{
	return static_cast<netid_t>(NETID_LEVEL_BIT | (objnum & 0x7fff));
}

[[nodiscard]]
constexpr netid_t dynamic_netid(const std::uint8_t creator, const std::uint16_t counter)
{
	return static_cast<netid_t>(((creator & (NETID_CREATORS - 1)) << NETID_COUNTER_BITS) | (counter & NETID_COUNTER_MASK));
}

[[nodiscard]]
constexpr bool is_level_netid(const netid_t id)
{
	return (id & NETID_LEVEL_BIT) != 0;
}

/* The map between network ids and local object numbers.  A binding also
 * records the signature the object had when it was bound: an object slot
 * is reused as soon as its object is deleted, and the signature tells the
 * new occupant from the object the id was given to.  `MaxObjects` is the
 * size of the local object array.
 */
template <std::size_t MaxObjects>
class netid_table
{
public:
	static constexpr std::uint16_t NO_OBJECT{0xffff};
	struct binding
	{
		std::uint16_t objnum{NO_OBJECT};
		std::uint16_t signature{};
	};
	void reset()
	{
		local_of_.fill({});
		netid_of_.fill(NETID_NONE);
		next_counter_.fill(0);
		count_ = 0;
	}
	/* Bind `id` to the object `objnum` with `signature`.  Any previous
	 * binding of the id or of the object number is dropped.
	 */
	bool bind(const netid_t id, const std::uint16_t objnum, const std::uint16_t signature)
	{
		if (id == NETID_NONE || objnum >= MaxObjects)
			return false;
		unbind(id);
		if (const auto old{netid_of_[objnum]}; old != NETID_NONE)
			unbind(old);
		local_of_[id] = {objnum, signature};
		netid_of_[objnum] = id;
		++count_;
		return true;
	}
	void unbind(const netid_t id)
	{
		if (id == NETID_NONE)
			return;
		auto &b{local_of_[id]};
		if (b.objnum == NO_OBJECT)
			return;
		if (netid_of_[b.objnum] == id)
			netid_of_[b.objnum] = NETID_NONE;
		b = {};
		--count_;
	}
	[[nodiscard]]
	std::optional<binding> find(const netid_t id) const
	{
		if (id == NETID_NONE)
			return std::nullopt;
		const auto &b{local_of_[id]};
		if (b.objnum == NO_OBJECT)
			return std::nullopt;
		return b;
	}
	/* The id of the object in slot `objnum`, if the object there is the
	 * one it was given to.
	 */
	[[nodiscard]]
	netid_t netid_of(const std::uint16_t objnum, const std::uint16_t signature) const
	{
		if (objnum >= MaxObjects)
			return NETID_NONE;
		const auto id{netid_of_[objnum]};
		if (id == NETID_NONE || local_of_[id].signature != signature)
			return NETID_NONE;
		return id;
	}
	/* The id bound to slot `objnum`, whatever lives there now. */
	[[nodiscard]]
	netid_t netid_at(const std::uint16_t objnum) const
	{
		return objnum < MaxObjects ? netid_of_[objnum] : NETID_NONE;
	}
	[[nodiscard]]
	bool bound(const netid_t id) const
	{
		return find(id).has_value();
	}
	/* A new dynamic id for `creator`: the creator's running counter,
	 * skipping ids still bound (a live object never shares its id with a
	 * new one).  NETID_NONE only if all 4096 are bound.
	 */
	[[nodiscard]]
	netid_t allocate(const std::uint8_t creator)
	{
		auto &counter{next_counter_[creator & (NETID_CREATORS - 1)]};
		for (unsigned tries = 0; tries <= NETID_COUNTER_MASK; ++tries)
		{
			const netid_t id{dynamic_netid(creator, counter)};
			counter = static_cast<std::uint16_t>((counter + 1) & NETID_COUNTER_MASK);
			if (!bound(id))
				return id;
		}
		return NETID_NONE;
	}
	[[nodiscard]]
	std::size_t size() const
	{
		return count_;
	}
private:
	std::array<binding, 0x10000> local_of_{};
	std::array<netid_t, MaxObjects> netid_of_{[]{
		std::array<netid_t, MaxObjects> a;
		a.fill(NETID_NONE);
		return a;
	}()};
	std::array<std::uint16_t, NETID_CREATORS> next_counter_{};
	std::size_t count_{};
};

/* Section 6.2: a player's inventory, as the pickup rules see it.  The
 * flag words carry the game's bit values (primary weapon flags, player
 * flags); the rules name the few bits they need.  `laser_level` counts
 * from 0 as in the game.
 */
constexpr std::size_t NET_V2_SECONDARY_WEAPONS{10};

struct inventory
{
	std::uint16_t primary_flags{};
	std::uint8_t laser_level{};
	std::array<std::uint8_t, NET_V2_SECONDARY_WEAPONS> secondary{};
	std::uint16_t vulcan_ammo{};
	std::uint32_t powerup_flags{};
	std::uint8_t orbs{};
	std::int32_t shields{};
	std::int32_t energy{};
	std::int32_t omega_charge{};
	/* Invulnerable only for a moment after a respawn: may take a real
	 * invulnerability powerup (do_powerup's FakingInvul).
	 */
	bool faking_invul{};
	constexpr bool operator==(const inventory &) const = default;
};

/* The fields that change all the time while a player fires or is hit.
 * Everything else changes only on pickups, drops, deaths and toggles.
 */
[[nodiscard]]
constexpr bool inventory_differs_only_in_consumables(const inventory &a, const inventory &b)
{
	auto x{a};
	x.shields = b.shields;
	x.energy = b.energy;
	x.vulcan_ammo = b.vulcan_ammo;
	x.omega_charge = b.omega_charge;
	return x == b;
}

/* What a powerup gives, independent of the game's powerup numbers. */
enum class pickup_kind : std::uint8_t
{
	/* Not arbitrated by the host (keys: they stay in the level, and
	 * every player may take one).
	 */
	none,
	energy,
	shield,
	laser,
	super_laser,
	/* `index` = weapon index, `bit` = its primary weapon flag. */
	primary,
	/* Primary weapon whose object carries its charge in `count`. */
	omega,
	/* Vulcan or gauss cannon: the object carries ammo in `count`. */
	vulcan_cannon,
	/* `amount` rounds. */
	vulcan_ammo,
	/* `index` = secondary weapon index, `amount` = missiles. */
	secondary,
	/* A player flag (`bit`) such as quad lasers, cloak, afterburner. */
	flag_item,
	/* A CTF flag; `index` = the team that may take it. */
	team_flag,
	orb,
	/* Always usable (extra life, the debug mega powerup). */
	always,
};

struct pickup_desc
{
	pickup_kind kind{};
	std::uint8_t index{};
	std::uint16_t amount{};
	std::uint32_t bit{};
};

/* The limits, as the game defines them, and the facts about the player
 * and the game mode that the rules depend on.
 */
struct inventory_rules
{
	std::int32_t max_energy{};
	std::int32_t max_shields{};
	std::int32_t energy_boost{};
	std::int32_t shield_boost{};
	std::uint8_t max_laser_level{};
	std::uint8_t max_super_laser_level{};
	std::uint16_t vulcan_ammo_max{};
	std::array<std::uint8_t, NET_V2_SECONDARY_WEAPONS> secondary_max{};
	std::uint32_t ammo_rack_bit{};
	std::uint32_t has_team_flag_bit{};
	std::uint8_t max_orbs{};
	bool capture_mode{};
	bool hoard_mode{};
	std::uint8_t team{};
	std::int32_t max_omega_charge{};
	[[nodiscard]]
	constexpr unsigned ammo_max(const unsigned base, const inventory &inv) const
	{
		return (inv.powerup_flags & ammo_rack_bit) ? base * 2 : base;
	}
};

/* The host's verdict on a pickup.  `taken` is what the player receives:
 * boost points, missiles, rounds, the omega charge; `remaining` is the
 * ammunition left in a vulcan or gauss cannon that stays in the level.
 */
struct pickup_outcome
{
	bool usable{};
	bool consumed{};
	std::uint32_t taken{};
	std::uint32_t remaining{};
};

/* The rules of do_powerup in a multiplayer game (similar/main/powerup.cpp):
 * whether the player can use the powerup, and what it takes.  An object
 * that is not usable is not picked up at all and stays in the level.
 */
[[nodiscard]]
constexpr pickup_outcome evaluate_pickup(const inventory &inv, const inventory_rules &r, const pickup_desc &d, const std::uint32_t object_count)
{
	const auto boost{[](const std::int32_t have, const std::int32_t max, const std::int32_t add) -> pickup_outcome {
		if (have >= max)
			return {};
		return {true, true, static_cast<std::uint32_t>(std::min(add, max - have)), 0};
	}};
	switch (d.kind)
	{
		case pickup_kind::none:
			return {};
		case pickup_kind::energy:
			return boost(inv.energy, r.max_energy, r.energy_boost);
		case pickup_kind::shield:
			return boost(inv.shields, r.max_shields, r.shield_boost);
		case pickup_kind::laser:
			if (inv.laser_level >= r.max_laser_level)
				return {};
			return {true, true, 1, 0};
		case pickup_kind::super_laser:
			if (inv.laser_level >= r.max_super_laser_level)
				return {};
			return {true, true, 1, 0};
		case pickup_kind::primary:
			if (inv.primary_flags & d.bit)
				return {};
			return {true, true, 0, 0};
		case pickup_kind::omega:
			if (inv.primary_flags & d.bit)
				return {};
			return {true, true, std::min<std::uint32_t>(object_count, static_cast<std::uint32_t>(std::max(r.max_omega_charge, 0))), 0};
		case pickup_kind::vulcan_cannon:
			{
				const unsigned max{r.ammo_max(r.vulcan_ammo_max, inv)};
				const std::uint32_t room{inv.vulcan_ammo < max ? max - inv.vulcan_ammo : 0u};
				const std::uint32_t taken{std::min(object_count, room)};
				if (!(inv.primary_flags & d.bit))
					/* A new cannon: the object goes away with any
					 * ammunition that did not fit (as in do_powerup).
					 */
					return {true, true, taken, 0};
				if (!taken)
					return {};
				/* The player has the cannon: take the ammunition, and
				 * the cannon stays with the rest, even when empty.
				 */
				return {true, false, taken, object_count - taken};
			}
		case pickup_kind::vulcan_ammo:
			{
				const unsigned max{r.ammo_max(r.vulcan_ammo_max, inv)};
				if (inv.vulcan_ammo >= max)
					return {};
				return {true, true, std::min<std::uint32_t>(d.amount, max - inv.vulcan_ammo), 0};
			}
		case pickup_kind::secondary:
			{
				if (d.index >= NET_V2_SECONDARY_WEAPONS)
					return {};
				const unsigned max{r.ammo_max(r.secondary_max[d.index], inv)};
				const unsigned have{inv.secondary[d.index]};
				if (have >= max)
					return {};
				return {true, true, std::min<std::uint32_t>(d.amount, max - have), 0};
			}
		case pickup_kind::flag_item:
			if (inv.powerup_flags & d.bit)
				return {};
			return {true, true, 0, 0};
		case pickup_kind::team_flag:
			if (!r.capture_mode || r.team != d.index)
				return {};
			return {true, true, 0, 0};
		case pickup_kind::orb:
			if (!r.hoard_mode || inv.orbs >= r.max_orbs)
				return {};
			return {true, true, 1, 0};
		case pickup_kind::always:
			return {true, true, 0, 0};
	}
	return {};
}

/* Apply a granted pickup to an inventory: the host's copy of the player
 * or, for a grant that arrives while the player is dead, the player's
 * own.
 */
constexpr void apply_pickup(inventory &inv, const inventory_rules &r, const pickup_desc &d, const pickup_outcome &o)
{
	if (!o.usable)
		return;
	switch (d.kind)
	{
		case pickup_kind::none:
		case pickup_kind::always:
			break;
		case pickup_kind::energy:
			inv.energy = std::min(r.max_energy, inv.energy + static_cast<std::int32_t>(o.taken));
			break;
		case pickup_kind::shield:
			inv.shields = std::min(r.max_shields, inv.shields + static_cast<std::int32_t>(o.taken));
			break;
		case pickup_kind::laser:
			if (inv.laser_level < r.max_laser_level)
				++inv.laser_level;
			break;
		case pickup_kind::super_laser:
			if (inv.laser_level < r.max_laser_level)
				inv.laser_level = r.max_laser_level;
			if (inv.laser_level < r.max_super_laser_level)
				++inv.laser_level;
			break;
		case pickup_kind::primary:
			inv.primary_flags = static_cast<std::uint16_t>(inv.primary_flags | d.bit);
			break;
		case pickup_kind::omega:
			inv.primary_flags = static_cast<std::uint16_t>(inv.primary_flags | d.bit);
			inv.omega_charge = static_cast<std::int32_t>(o.taken);
			break;
		case pickup_kind::vulcan_cannon:
			inv.primary_flags = static_cast<std::uint16_t>(inv.primary_flags | d.bit);
			[[fallthrough]];
		case pickup_kind::vulcan_ammo:
			{
				const unsigned max{r.ammo_max(r.vulcan_ammo_max, inv)};
				inv.vulcan_ammo = static_cast<std::uint16_t>(std::min<unsigned>(max, inv.vulcan_ammo + o.taken));
			}
			break;
		case pickup_kind::secondary:
			if (d.index < NET_V2_SECONDARY_WEAPONS)
			{
				const unsigned max{r.ammo_max(r.secondary_max[d.index], inv)};
				inv.secondary[d.index] = static_cast<std::uint8_t>(std::min<unsigned>(max, inv.secondary[d.index] + o.taken));
			}
			break;
		case pickup_kind::flag_item:
			inv.powerup_flags |= d.bit;
			break;
		case pickup_kind::team_flag:
			inv.powerup_flags |= r.has_team_flag_bit;
			break;
		case pickup_kind::orb:
			if (inv.orbs < r.max_orbs)
				++inv.orbs;
			inv.powerup_flags |= r.has_team_flag_bit;
			break;
	}
}

/* Whether a player whose inventory is `inv` can drop the powerup `d`
 * carrying `count` (the ammunition of a cannon, the omega charge), and
 * its effect on the inventory.
 */
[[nodiscard]]
constexpr bool evaluate_drop(const inventory &inv, const inventory_rules &r, const pickup_desc &d, const std::uint32_t count)
{
	switch (d.kind)
	{
		case pickup_kind::laser:
			return inv.laser_level > 0 && inv.laser_level <= r.max_laser_level;
		case pickup_kind::primary:
			return (inv.primary_flags & d.bit) != 0;
		case pickup_kind::omega:
			return (inv.primary_flags & d.bit) != 0 && count <= static_cast<std::uint32_t>(std::max(r.max_omega_charge, 0));
		case pickup_kind::vulcan_cannon:
			return (inv.primary_flags & d.bit) != 0 && count <= inv.vulcan_ammo;
		case pickup_kind::secondary:
			return d.index < NET_V2_SECONDARY_WEAPONS && inv.secondary[d.index] >= d.amount;
		case pickup_kind::flag_item:
			return (inv.powerup_flags & d.bit) != 0;
		case pickup_kind::team_flag:
			return r.capture_mode && (inv.powerup_flags & r.has_team_flag_bit) != 0;
		case pickup_kind::orb:
			return r.hoard_mode && inv.orbs > 0;
		default:
			return false;
	}
}

constexpr void apply_drop(inventory &inv, const inventory_rules &r, const pickup_desc &d, const std::uint32_t count)
{
	if (!evaluate_drop(inv, r, d, count))
		return;
	switch (d.kind)
	{
		case pickup_kind::laser:
			--inv.laser_level;
			break;
		case pickup_kind::primary:
		case pickup_kind::omega:
			inv.primary_flags = static_cast<std::uint16_t>(inv.primary_flags & ~d.bit);
			break;
		case pickup_kind::vulcan_cannon:
			inv.primary_flags = static_cast<std::uint16_t>(inv.primary_flags & ~d.bit);
			inv.vulcan_ammo = static_cast<std::uint16_t>(inv.vulcan_ammo - count);
			break;
		case pickup_kind::secondary:
			inv.secondary[d.index] = static_cast<std::uint8_t>(inv.secondary[d.index] - d.amount);
			break;
		case pickup_kind::flag_item:
			inv.powerup_flags &= ~d.bit;
			break;
		case pickup_kind::team_flag:
			inv.powerup_flags &= ~r.has_team_flag_bit;
			break;
		case pickup_kind::orb:
			if (!--inv.orbs)
				inv.powerup_flags &= ~r.has_team_flag_bit;
			break;
		default:
			break;
	}
}

/* The host's copy of a client's inventory (stage 3: the client still
 * consumes its ammunition and energy and takes its damage, so the host
 * learns the inventory from the client's INVENTORY reports).  Every grant
 * the host sends is numbered; the client counts the grants it has
 * received and sends that count with each report.  A report therefore
 * describes the inventory *with* the grants up to its count, and the
 * copy is the report plus every later grant, applied again: a report
 * that was sent before a grant arrived never makes the host forget the
 * grant, and a grant is never counted twice.
 */
class inventory_mirror
{
public:
	/* Grants and damage (stage 4) in flight: a vulcan cannon alone hits
	 * twenty times per second.
	 */
	static constexpr std::size_t MAX_PENDING{128};
	struct pending_grant
	{
		std::uint16_t seq{};
		pickup_desc desc{};
		pickup_outcome outcome{};
		/* Stage 4: not a grant but damage the host applied (DAMAGE),
		 * which the player takes off its shields and counts as it counts
		 * a grant.
		 */
		std::int32_t damage{};
	};
	/* A new session for the player (level start, join): the client's
	 * count and life start at 0 as well.
	 */
	void reset(const inventory &inv)
	{
		base_ = current_ = inv;
		pending_count_ = 0;
		issued_ = 0;
		life_ = 0;
		has_report_ = false;
	}
	/* The player died and its items were dropped: nothing is carried
	 * any more; grants still on their way were dropped with the rest,
	 * and the player's next life begins (grants for the old one are
	 * recognised by their life, see own_life).
	 */
	void clear()
	{
		base_ = current_ = {};
		pending_count_ = 0;
		life_ = static_cast<std::uint8_t>(life_ + 1);
		has_report_ = false;
	}
	/* The host flies this player's ship itself (a bot): the ship is the
	 * truth, and no grant is on its way.  The life goes on.
	 */
	void assign(const inventory &inv)
	{
		base_ = current_ = inv;
		pending_count_ = 0;
	}
	/* The life the grants to this player are for (PICKUP_GRANT `life`). */
	[[nodiscard]]
	std::uint8_t life() const
	{
		return life_;
	}
	[[nodiscard]]
	const inventory &current() const
	{
		return current_;
	}
	/* The newest report (or assignment), without the grants on their
	 * way: current() less base() is what is in flight.
	 */
	[[nodiscard]]
	const inventory &base() const
	{
		return base_;
	}
	/* The player reported since this copy was started or emptied (a new
	 * session, a death drop): only then does a report that holds more
	 * than the copy expects mean something (unexplained_gain).
	 */
	[[nodiscard]]
	bool has_report() const
	{
		return has_report_;
	}
	[[nodiscard]]
	std::uint16_t issued() const
	{
		return issued_;
	}
	[[nodiscard]]
	std::size_t pending() const
	{
		return pending_count_;
	}
	/* A grant for this player; returns its number. */
	std::uint16_t on_grant(const inventory_rules &r, const pickup_desc &d, const pickup_outcome &o)
	{
		const std::uint16_t seq{++issued_};
		if (pending_count_ == MAX_PENDING)
		{
			/* Far more grants in flight than one round trip allows:
			 * forget the oldest (the next report covers it anyway).
			 */
			std::copy(pending_.begin() + 1, pending_.end(), pending_.begin());
			--pending_count_;
		}
		pending_[pending_count_++] = {seq, d, o, 0};
		apply_pickup(current_, r, d, o);
		return seq;
	}
	/* Damage the host applied to this player (stage 4, DAMAGE): numbered
	 * like a grant, so that a report sent before the damage arrived does
	 * not give the shields back.  Returns the shields left.
	 */
	std::int32_t on_damage(const std::int32_t amount)
	{
		const std::uint16_t seq{++issued_};
		if (pending_count_ == MAX_PENDING)
		{
			std::copy(pending_.begin() + 1, pending_.end(), pending_.begin());
			--pending_count_;
		}
		pending_[pending_count_++] = {seq, {}, {}, amount};
		current_.shields -= amount;
		return current_.shields;
	}
	/* The player dropped an item (DROP_REQUEST): its report sent after
	 * the drop says the same, and a report sent before it arrived before
	 * the request.
	 */
	void on_drop(const inventory_rules &r, const pickup_desc &d, const std::uint32_t count)
	{
		apply_drop(base_, r, d, count);
		apply_drop(current_, r, d, count);
	}
	/* A report of the player's inventory with `applied` grants. */
	void on_report(const inventory_rules &r, const inventory &inv, const std::uint16_t applied)
	{
		has_report_ = true;
		base_ = inv;
		std::size_t kept{0};
		for (std::size_t i = 0; i < pending_count_; ++i)
			if (seq_diff(pending_[i].seq, applied) > 0)
				pending_[kept++] = pending_[i];
		pending_count_ = kept;
		current_ = base_;
		for (std::size_t i = 0; i < pending_count_; ++i)
		{
			if (const auto damage{pending_[i].damage})
				current_.shields -= damage;
			else
				apply_pickup(current_, r, pending_[i].desc, pending_[i].outcome);
		}
	}
private:
	inventory base_{};
	inventory current_{};
	std::array<pending_grant, MAX_PENDING> pending_{};
	std::size_t pending_count_{};
	std::uint16_t issued_{};
	std::uint8_t life_{};
	bool has_report_{};
};

/* The host's powerup accounting log (net_objects.cpp): how many units of
 * the item a powerup gives lie in one object or are carried in an
 * inventory, counted as the respawn bookkeeping counts them (missiles
 * one by one, so a 4-pack is 4 of the single missile, vulcan rounds,
 * one per weapon or item).
 */
[[nodiscard]]
constexpr bool same_item(const pickup_desc &a, const pickup_desc &b)
{
	if (a.kind != b.kind)
		return false;
	switch (a.kind)
	{
		case pickup_kind::secondary:
		case pickup_kind::team_flag:
			return a.index == b.index;
		case pickup_kind::primary:
		case pickup_kind::omega:
		case pickup_kind::vulcan_cannon:
		case pickup_kind::flag_item:
			return a.bit == b.bit;
		default:
			return true;
	}
}

[[nodiscard]]
constexpr std::uint32_t units_on_ground(const pickup_desc &d)
{
	switch (d.kind)
	{
		case pickup_kind::none:
			return 0;
		case pickup_kind::secondary:
		case pickup_kind::vulcan_ammo:
			return d.amount;
		default:
			return 1;
	}
}

[[nodiscard]]
constexpr std::uint32_t units_carried(const inventory &inv, const inventory_rules &r, const pickup_desc &d)
{
	switch (d.kind)
	{
		case pickup_kind::secondary:
			return d.index < NET_V2_SECONDARY_WEAPONS ? inv.secondary[d.index] : 0;
		case pickup_kind::primary:
		case pickup_kind::omega:
		case pickup_kind::vulcan_cannon:
			return (inv.primary_flags & d.bit) ? 1 : 0;
		case pickup_kind::flag_item:
			return (inv.powerup_flags & d.bit) ? 1 : 0;
		case pickup_kind::laser:
		case pickup_kind::super_laser:
			return inv.laser_level;
		case pickup_kind::vulcan_ammo:
			return inv.vulcan_ammo;
		case pickup_kind::team_flag:
			return (inv.powerup_flags & r.has_team_flag_bit) ? 1 : 0;
		case pickup_kind::orb:
			return inv.orbs;
		default:
			return 0;
	}
}

/* A client's report that holds more of an item than the host's copy of
 * its inventory expects (its previous report, the grants since and its
 * drops): a player gains missiles, weapons, laser levels and orbs only
 * through the host's grants, so such a gain is how a duplicated powerup
 * shows on the host.  Ammunition, energy, shields and the omega charge
 * change on their own and are not compared; neither are the flags
 * (the headlight and the respawn invulnerability toggle by themselves).
 */
struct inventory_gain
{
	enum class what : std::uint8_t
	{
		secondary,
		primary,
		laser,
		orbs,
	};
	what field{};
	/* The secondary weapon or the primary weapon's bit number. */
	std::uint8_t index{};
	std::uint32_t expected{};
	std::uint32_t reported{};
};

[[nodiscard]]
constexpr std::optional<inventory_gain> unexplained_gain(const inventory &expected, const inventory &reported)
{
	for (std::uint8_t i = 0; i < NET_V2_SECONDARY_WEAPONS; ++i)
		if (reported.secondary[i] > expected.secondary[i])
			return inventory_gain{inventory_gain::what::secondary, i, expected.secondary[i], reported.secondary[i]};
	if (const unsigned extra = reported.primary_flags & ~unsigned{expected.primary_flags})
	{
		std::uint8_t bit{0};
		while (!(extra & (1u << bit)))
			++bit;
		return inventory_gain{inventory_gain::what::primary, bit, 0, 1};
	}
	if (reported.laser_level > expected.laser_level)
		return inventory_gain{inventory_gain::what::laser, 0, expected.laser_level, reported.laser_level};
	if (reported.orbs > expected.orbs)
		return inventory_gain{inventory_gain::what::orbs, 0, expected.orbs, reported.orbs};
	return std::nullopt;
}

/* The client's count of its own lives, the counterpart of
 * inventory_mirror::life on the host: a life ends when the player's items
 * are dropped (its MULTI_PLAYER_DERES; the host drops them once per life,
 * when that deres arrives) and the next begins at 0 again with a new
 * session.  A grant carries the life of the host's copy it was applied
 * to; a grant for an earlier life (the player asked just before dying,
 * and the answer arrives after the deres, maybe after the respawn) was
 * dropped with the rest of that life's items on the host and must not be
 * applied to the new ship.
 */
struct own_life
{
	std::uint8_t life{};
	bool dropped{};
	void reset()
	{
		life = 0;
		dropped = false;
	}
	/* The player sent its deres. */
	void on_deres()
	{
		if (dropped)
			return;
		dropped = true;
		life = static_cast<std::uint8_t>(life + 1);
	}
	/* The player reappeared (MULTI_REAPPEAR). */
	void on_reappear()
	{
		dropped = false;
	}
	/* Whether a grant for `grant_life` is for the life being played, or
	 * the one that just ended but whose deres is not sent yet.
	 */
	[[nodiscard]]
	bool current(const std::uint8_t grant_life) const
	{
		return grant_life == life;
	}
};

/* The client's pickups: a powerup the local ship touched is hidden and
 * asked for; it stays hidden until the host answers (or for `timeout`);
 * after a denial or a timeout the same powerup is not asked for again
 * during `cooldown`, so that a ship resting on a powerup the host will not
 * grant does not ask every frame.
 */
struct pending_pickup
{
	netid_t netid{NETID_NONE};
	std::uint16_t objnum{};
	std::uint16_t signature{};
	std::uint8_t saved_render{};
	bool hidden{};
	std::int64_t since{};
	std::int64_t retry_after{};
};

template <std::size_t N = 16>
class pending_pickups
{
public:
	void reset()
	{
		entries_.fill({});
	}
	[[nodiscard]]
	pending_pickup *find(const netid_t id)
	{
		for (auto &e : entries_)
			if (e.netid == id && id != NETID_NONE)
				return &e;
		return nullptr;
	}
	/* Whether the powerup may be asked for now. */
	[[nodiscard]]
	bool can_request(const netid_t id, const std::int64_t now)
	{
		const auto e{find(id)};
		return !e || (!e->hidden && now >= e->retry_after);
	}
	/* Record a request.  A full table drops its oldest entry; the caller
	 * shows that powerup again (returned in `evicted`).
	 */
	pending_pickup &add(const pending_pickup &p, std::optional<pending_pickup> &evicted)
	{
		evicted.reset();
		if (const auto e{find(p.netid)})
		{
			*e = p;
			return *e;
		}
		auto *slot{&entries_[0]};
		for (auto &e : entries_)
		{
			if (e.netid == NETID_NONE)
			{
				slot = &e;
				break;
			}
			if (e.since < slot->since)
				slot = &e;
		}
		if (slot->netid != NETID_NONE && slot->hidden)
			evicted = *slot;
		*slot = p;
		return *slot;
	}
	/* The host denied: the entry becomes a cooldown.  Returns the entry
	 * as it was, so that the caller shows the powerup again.
	 */
	std::optional<pending_pickup> deny(const netid_t id, const std::int64_t now, const std::int64_t cooldown)
	{
		const auto e{find(id)};
		if (!e)
			return std::nullopt;
		const auto was{*e};
		e->hidden = false;
		e->retry_after = now + cooldown;
		return was;
	}
	/* The powerup was granted or removed: forget it. */
	std::optional<pending_pickup> erase(const netid_t id)
	{
		const auto e{find(id)};
		if (!e)
			return std::nullopt;
		const auto was{*e};
		*e = {};
		return was;
	}
	/* Requests older than `timeout` become cooldowns (the caller shows the
	 * powerup again); cooldowns that ended are forgotten.  `f` is called
	 * for each request that timed out.
	 */
	template <typename F>
	void expire(const std::int64_t now, const std::int64_t timeout, const std::int64_t cooldown, F &&f)
	{
		for (auto &e : entries_)
		{
			if (e.netid == NETID_NONE)
				continue;
			if (e.hidden)
			{
				if (now - e.since >= timeout)
				{
					f(static_cast<const pending_pickup &>(e));
					e.hidden = false;
					e.retry_after = now + cooldown;
				}
			}
			else if (now >= e.retry_after)
				e = {};
		}
	}
	[[nodiscard]]
	std::size_t hidden_count() const
	{
		return static_cast<std::size_t>(std::count_if(entries_.begin(), entries_.end(), [](const pending_pickup &e) { return e.netid != NETID_NONE && e.hidden; }));
	}
private:
	std::array<pending_pickup, N> entries_{};
};

/* Respawn bookkeeping (the v1 MultiLevelInv rules, host only): an item is
 * respawned when the level holds `required` fewer of it than at the
 * start, counting what the players carry, and has done so for
 * `threshold` (2 s).
 */
[[nodiscard]]
constexpr bool respawn_allowed(const std::uint32_t initial, const std::uint32_t current, const std::uint32_t required)
{
	if (initial == 0 || current > initial)
		return false;
	return initial - current >= required;
}

struct respawn_timer
{
	std::int32_t elapsed{};
	/* Advance by `dt` while the item is missing; true when it is time to
	 * respawn one.
	 */
	bool step(const bool allowed, const std::int32_t dt, const std::int32_t threshold)
	{
		if (!allowed)
		{
			elapsed = 0;
			return false;
		}
		elapsed += dt;
		if (elapsed < threshold)
			return false;
		elapsed = 0;
		return true;
	}
};

/* The host's range check for a client's pickup request (fix units: 65536
 * per unit).  Without the position history of stage 4, the host compares
 * the object with the requester's newest ship position, which is at most
 * a tick and a trip away from where it touched the object: the slack
 * covers the interpolation of the object's own flight and a quarter of a
 * second of the ship's movement.
 */
constexpr std::int64_t PICKUP_RANGE_SLACK{6 * 65536};

[[nodiscard]]
constexpr bool pickup_in_range(const std::int64_t distance, const std::int32_t object_size, const std::int32_t ship_size, const std::int64_t ship_speed)
{
	return distance <= std::int64_t{object_size} + ship_size + PICKUP_RANGE_SLACK + std::max<std::int64_t>(ship_speed, 0) / 4;
}

/* A powerup comes to rest on the host below this speed (fix units per
 * second); the host then sends its final position (OBJ_SETTLE).
 */
constexpr std::int32_t SETTLE_SPEED{65536};

/* Wire layouts (section 6.10 numbering; the ids are in net_v2_session.h). */

namespace detail {

struct cursor
{
	std::span<std::uint8_t> out{};
	std::span<const std::uint8_t> in{};
	std::size_t pos{};
	bool ok{true};
	void u8(const std::uint8_t v)
	{
		out[pos++] = v;
	}
	void u16(const std::uint16_t v)
	{
		net_put_le16(&out[pos], v);
		pos += 2;
	}
	void u32(const std::uint32_t v)
	{
		net_put_le32(&out[pos], v);
		pos += 4;
	}
	void i32(const std::int32_t v)
	{
		u32(static_cast<std::uint32_t>(v));
	}
	[[nodiscard]]
	std::uint8_t r8()
	{
		return in[pos++];
	}
	[[nodiscard]]
	std::uint16_t r16()
	{
		const auto v{net_get_le16(&in[pos])};
		pos += 2;
		return v;
	}
	[[nodiscard]]
	std::uint32_t r32()
	{
		const auto v{net_get_le32(&in[pos])};
		pos += 4;
		return v;
	}
	[[nodiscard]]
	std::int32_t ri32()
	{
		return static_cast<std::int32_t>(r32());
	}
};

}

/* INVENTORY (0x20): client to host (the client's report, `seq` = grants
 * it has received) and host to all (the host's copy, `seq` informative).
 */
struct inventory_msg
{
	static constexpr std::size_t SIZE{36};
	std::uint8_t pid{};
	std::uint16_t seq{};
	inventory inv{};
	void write(std::span<std::uint8_t, SIZE> buf) const
	{
		detail::cursor c{.out = buf};
		c.u8(pid);
		c.u16(seq);
		c.u16(inv.primary_flags);
		c.u8(inv.laser_level);
		for (const auto s : inv.secondary)
			c.u8(s);
		c.u16(inv.vulcan_ammo);
		c.u32(inv.powerup_flags);
		c.u8(inv.orbs);
		c.i32(inv.shields);
		c.i32(inv.energy);
		c.i32(inv.omega_charge);
		c.u8(inv.faking_invul ? 1 : 0);
	}
	[[nodiscard]]
	static std::optional<inventory_msg> read(const std::span<const std::uint8_t> buf)
	{
		if (buf.size() != SIZE)
			return std::nullopt;
		detail::cursor c{.in = buf};
		inventory_msg m;
		m.pid = c.r8();
		m.seq = c.r16();
		m.inv.primary_flags = c.r16();
		m.inv.laser_level = c.r8();
		for (auto &s : m.inv.secondary)
			s = c.r8();
		m.inv.vulcan_ammo = c.r16();
		m.inv.powerup_flags = c.r32();
		m.inv.orbs = c.r8();
		m.inv.shields = c.ri32();
		m.inv.energy = c.ri32();
		m.inv.omega_charge = c.ri32();
		m.inv.faking_invul = c.r8() & 1;
		return m;
	}
};

/* OBJ_CREATE (0x21), host to all. */
enum class obj_create_flag : std::uint8_t
{
	/* Show the powerup appearance effect (a respawned item). */
	appear = 1 << 0,
	/* Dropped by a player (on death, or spat). */
	player_dropped = 1 << 1,
	/* Spat by `owner`, who cannot take it back for 2 s. */
	spat = 1 << 2,
};

struct obj_create_msg
{
	static constexpr std::size_t SIZE{36};
	netid_t netid{};
	std::uint8_t type{};
	std::uint8_t id{};
	std::uint16_t segnum{};
	std::array<std::int32_t, 3> pos{};
	std::array<std::int32_t, 3> velocity{};
	std::uint32_t count{};
	std::uint8_t flags{};
	std::uint8_t owner{0xff};
	[[nodiscard]]
	bool has(const obj_create_flag f) const
	{
		return (flags & static_cast<std::uint8_t>(f)) != 0;
	}
	void write(std::span<std::uint8_t, SIZE> buf) const
	{
		detail::cursor c{.out = buf};
		c.u16(netid);
		c.u8(type);
		c.u8(id);
		c.u16(segnum);
		for (const auto v : pos)
			c.i32(v);
		for (const auto v : velocity)
			c.i32(v);
		c.u32(count);
		c.u8(flags);
		c.u8(owner);
	}
	[[nodiscard]]
	static std::optional<obj_create_msg> read(const std::span<const std::uint8_t> buf)
	{
		if (buf.size() != SIZE)
			return std::nullopt;
		detail::cursor c{.in = buf};
		obj_create_msg m;
		m.netid = c.r16();
		m.type = c.r8();
		m.id = c.r8();
		m.segnum = c.r16();
		for (auto &v : m.pos)
			v = c.ri32();
		for (auto &v : m.velocity)
			v = c.ri32();
		m.count = c.r32();
		m.flags = c.r8();
		m.owner = c.r8();
		if (m.netid == NETID_NONE)
			return std::nullopt;
		return m;
	}
};

/* OBJ_REMOVE (0x22), host to all. */
enum class obj_remove_reason : std::uint8_t
{
	/* The object is gone on the host (destroyed, or its slot reused). */
	gone = 0,
	/* A powerup's life ran out: receivers show it disappearing. */
	expired = 1,
};

struct obj_remove_msg
{
	static constexpr std::size_t SIZE{3};
	netid_t netid{};
	obj_remove_reason reason{};
	void write(std::span<std::uint8_t, SIZE> buf) const
	{
		net_put_le16(buf.data(), netid);
		buf[2] = static_cast<std::uint8_t>(reason);
	}
	[[nodiscard]]
	static std::optional<obj_remove_msg> read(const std::span<const std::uint8_t> buf)
	{
		if (buf.size() != SIZE || buf[2] > static_cast<std::uint8_t>(obj_remove_reason::expired))
			return std::nullopt;
		return obj_remove_msg{net_get_le16(buf.data()), static_cast<obj_remove_reason>(buf[2])};
	}
};

/* OBJ_SETTLE (0x47, stage 3), host to all: a powerup came to rest; every
 * machine puts it at the host's final position.
 */
struct obj_settle_msg
{
	static constexpr std::size_t SIZE{16};
	netid_t netid{};
	std::uint16_t segnum{};
	std::array<std::int32_t, 3> pos{};
	void write(std::span<std::uint8_t, SIZE> buf) const
	{
		detail::cursor c{.out = buf};
		c.u16(netid);
		c.u16(segnum);
		for (const auto v : pos)
			c.i32(v);
	}
	[[nodiscard]]
	static std::optional<obj_settle_msg> read(const std::span<const std::uint8_t> buf)
	{
		if (buf.size() != SIZE)
			return std::nullopt;
		detail::cursor c{.in = buf};
		obj_settle_msg m;
		m.netid = c.r16();
		m.segnum = c.r16();
		for (auto &v : m.pos)
			v = c.ri32();
		return m;
	}
};

/* PICKUP_REQUEST (0x24), client to host. */
struct pickup_request_msg
{
	static constexpr std::size_t SIZE{3};
	netid_t netid{};
	std::uint8_t powerup_id{};
	void write(std::span<std::uint8_t, SIZE> buf) const
	{
		net_put_le16(buf.data(), netid);
		buf[2] = powerup_id;
	}
	[[nodiscard]]
	static std::optional<pickup_request_msg> read(const std::span<const std::uint8_t> buf)
	{
		if (buf.size() != SIZE)
			return std::nullopt;
		return pickup_request_msg{net_get_le16(buf.data()), buf[2]};
	}
};

/* PICKUP_GRANT (0x25), host to all.  It also removes the object or, for a
 * cannon that stays, sets its ammunition (the design's OBJ_REMOVE and
 * OBJ_AMMO for a pickup, folded into the grant).  `life` is the life of
 * the player it is for (inventory_mirror::life, own_life).
 */
enum class grant_flag : std::uint8_t
{
	removed = 1 << 0,
};

struct pickup_grant_msg
{
	static constexpr std::size_t SIZE{14};
	std::uint8_t pid{};
	netid_t netid{};
	std::uint8_t powerup_id{};
	std::uint32_t count{};
	std::uint32_t remaining{};
	std::uint8_t flags{};
	std::uint8_t life{};
	[[nodiscard]]
	bool removed() const
	{
		return (flags & static_cast<std::uint8_t>(grant_flag::removed)) != 0;
	}
	void write(std::span<std::uint8_t, SIZE> buf) const
	{
		detail::cursor c{.out = buf};
		c.u8(pid);
		c.u16(netid);
		c.u8(powerup_id);
		c.u32(count);
		c.u32(remaining);
		c.u8(flags);
		c.u8(life);
	}
	[[nodiscard]]
	static std::optional<pickup_grant_msg> read(const std::span<const std::uint8_t> buf)
	{
		if (buf.size() != SIZE)
			return std::nullopt;
		detail::cursor c{.in = buf};
		pickup_grant_msg m;
		m.pid = c.r8();
		m.netid = c.r16();
		m.powerup_id = c.r8();
		m.count = c.r32();
		m.remaining = c.r32();
		m.flags = c.r8();
		m.life = c.r8();
		if (m.pid >= NETID_CREATORS || m.netid == NETID_NONE)
			return std::nullopt;
		return m;
	}
};

/* PICKUP_DENY (0x26), host to the requester. */
enum class deny_reason : std::uint8_t
{
	gone,
	dead,
	range,
	spat,
	cannot_use,
	not_arbitrated,
};

struct pickup_deny_msg
{
	static constexpr std::size_t SIZE{3};
	netid_t netid{};
	deny_reason reason{};
	void write(std::span<std::uint8_t, SIZE> buf) const
	{
		net_put_le16(buf.data(), netid);
		buf[2] = static_cast<std::uint8_t>(reason);
	}
	[[nodiscard]]
	static std::optional<pickup_deny_msg> read(const std::span<const std::uint8_t> buf)
	{
		if (buf.size() != SIZE || buf[2] > static_cast<std::uint8_t>(deny_reason::not_arbitrated))
			return std::nullopt;
		return pickup_deny_msg{net_get_le16(buf.data()), static_cast<deny_reason>(buf[2])};
	}
};

/* DROP_REQUEST (0x3B), client to host: drop a weapon, missiles, a flag or
 * an orb.  The powerup id says which; `count` is the ammunition of a
 * cannon or the omega charge.  (The design's DROP_WEAPON_REQUEST and
 * DROP_FLAG_REQUEST in one message.)
 */
struct drop_request_msg
{
	static constexpr std::size_t SIZE{5};
	std::uint8_t powerup_id{};
	std::uint32_t count{};
	void write(std::span<std::uint8_t, SIZE> buf) const
	{
		buf[0] = powerup_id;
		net_put_le32(&buf[1], count);
	}
	[[nodiscard]]
	static std::optional<drop_request_msg> read(const std::span<const std::uint8_t> buf)
	{
		if (buf.size() != SIZE)
			return std::nullopt;
		return drop_request_msg{buf[0], net_get_le32(&buf[1])};
	}
};

/* SPAWN_REQUEST (0x48), client to host: the client's death sequence has
 * ended and it asks where to respawn.  `request` counts the client's
 * requests (1..255, 0 skipped) so that a late answer to an earlier one
 * is told apart.
 */
struct spawn_request_msg
{
	static constexpr std::size_t SIZE{1};
	std::uint8_t request{};
	void write(std::span<std::uint8_t, SIZE> buf) const
	{
		buf[0] = request;
	}
	[[nodiscard]]
	static std::optional<spawn_request_msg> read(const std::span<const std::uint8_t> buf)
	{
		if (buf.size() != SIZE || buf[0] == 0)
			return std::nullopt;
		return spawn_request_msg{buf[0]};
	}
};

/* SPAWN_SITE (0x49), host to one client: where it spawns.  `request` is
 * the request answered, or SPAWN_REQUEST_JOIN for the first spawn of a
 * join in progress (sent ahead of LEVEL_GO, unasked); `site` indexes the
 * level's player start positions, SPAWN_SITE_NONE if the host has none to
 * offer (the client chooses itself).
 */
constexpr std::uint8_t SPAWN_REQUEST_JOIN{0};
constexpr std::uint8_t SPAWN_SITE_NONE{0xff};

struct spawn_site_msg
{
	static constexpr std::size_t SIZE{2};
	std::uint8_t request{};
	std::uint8_t site{SPAWN_SITE_NONE};
	void write(std::span<std::uint8_t, SIZE> buf) const
	{
		buf[0] = request;
		buf[1] = site;
	}
	[[nodiscard]]
	static std::optional<spawn_site_msg> read(const std::span<const std::uint8_t> buf)
	{
		if (buf.size() != SIZE)
			return std::nullopt;
		return spawn_site_msg{buf[0], buf[1]};
	}
};

/* The host's decision on a pickup request, from what it knows about the
 * object and the requester.  Kept separate from the game so that the
 * order of the checks is tested.
 */
struct pickup_object_view
{
	/* The id names a live powerup of the requested type. */
	bool exists{};
	pickup_desc desc{};
	std::uint32_t count{};
	/* The player who spat it and until when (host clock) they cannot
	 * take it back.
	 */
	std::uint8_t spat_owner{0xff};
	std::int64_t spat_until{};
};

struct requester_view
{
	std::uint8_t pid{};
	/* Connected, in the level, alive (not dead or dying). */
	bool alive{};
	bool in_range{};
};

struct pickup_decision
{
	bool grant{};
	deny_reason reason{};
	pickup_outcome outcome{};
};

[[nodiscard]]
constexpr pickup_decision decide_pickup(const pickup_object_view &o, const requester_view &q, const inventory &inv, const inventory_rules &r, const std::int64_t now)
{
	if (!o.exists)
		return {false, deny_reason::gone, {}};
	if (o.desc.kind == pickup_kind::none)
		return {false, deny_reason::not_arbitrated, {}};
	if (!q.alive)
		return {false, deny_reason::dead, {}};
	if (!q.in_range)
		return {false, deny_reason::range, {}};
	if (o.spat_owner == q.pid && now < o.spat_until)
		return {false, deny_reason::spat, {}};
	const auto outcome{evaluate_pickup(inv, r, o.desc, o.count)};
	if (!outcome.usable)
		return {false, deny_reason::cannot_use, {}};
	return {true, {}, outcome};
}

}

}
