/*
 * This file is part of the DXX-Rebirth project <https://github.com/dxx-rebirth/dxx-rebirth/>.
 * It is copyright by its individual contributors, as recorded in the
 * project's Git history.  See COPYING.txt at the top level for license
 * terms and a link to the Git history.
 */
/*
 * Test of the game-independent part of the v2 object authority
 * (net_v2_objects.h, stage 3): network object ids and their tables,
 * the pickup and drop rules, the host's copy of a client's inventory
 * under grants in flight, the host's pickup decision, the client's table
 * of pickups it asked for, the respawn bookkeeping, the range check and
 * the wire layouts; and a model of one host and several clients that
 * plays simultaneous, stale and duplicate requests, denials, partial
 * cannon ammunition, drops and a join in progress through the reliable
 * ordered channel, and checks that every machine ends up with the same
 * objects and that nothing is ever granted twice; and the queue of the
 * host's own pickups under -lagtest (net_v2_lagtest.h).
 *
 * Build and run with SCons:
 *
 *	scons sdl2=1 d1x=0 d2x=1 register_runtime_test_plain_link_targets=1 test-net-v2-authority
 *	build/common/test-net-v2-authority
 *
 * or directly:
 *
 *	g++ -std=gnu++23 -O2 -Wall -Wextra -Icommon/main common/unittest/net_v2_authority.cpp -o test-net-v2-authority
 */

#include <cstdio>
#include <cstdlib>
#include <deque>
#include <map>
#include <memory>
#include <random>
#include <vector>

#include "net_v2_objects.h"
#include "net_v2_lagtest.h"

using namespace dcx::net_v2;

namespace {

void check_failed(const char *const expr, const char *const file, const int line)
{
	std::fprintf(stderr, "%s:%d: check failed: %s\n", file, line, expr);
	std::exit(1);
}

#define CHECK(cond)	do { if (!(cond)) check_failed(#cond, __FILE__, __LINE__); } while (0)

constexpr std::int32_t F1{65536};
constexpr std::size_t MAX_OBJECTS{350};
using table_type = netid_table<MAX_OBJECTS>;

/* Game-like values (D2): player flag bits, weapon bits. */
constexpr std::uint32_t FLAG_QUAD{0x400};
constexpr std::uint32_t FLAG_CLOAKED{0x40};
constexpr std::uint32_t FLAG_AMMO_RACK{0x8000};
constexpr std::uint32_t FLAG_TEAM{0x100000};
constexpr std::uint16_t BIT_LASER{1 << 0};
constexpr std::uint16_t BIT_VULCAN{1 << 1};
constexpr std::uint16_t BIT_PLASMA{1 << 3};
constexpr std::uint16_t BIT_GAUSS{1 << 6};
constexpr std::uint16_t BIT_OMEGA{1 << 9};

constexpr pickup_desc SHIELD{pickup_kind::shield, 0, 0, 0};
constexpr pickup_desc ENERGY{pickup_kind::energy, 0, 0, 0};
constexpr pickup_desc LASER{pickup_kind::laser, 0, 0, 0};
constexpr pickup_desc SUPER_LASER{pickup_kind::super_laser, 0, 0, 0};
constexpr pickup_desc PLASMA{pickup_kind::primary, 3, 0, BIT_PLASMA};
constexpr pickup_desc OMEGA{pickup_kind::omega, 9, 0, BIT_OMEGA};
constexpr pickup_desc VULCAN{pickup_kind::vulcan_cannon, 1, 0, BIT_VULCAN};
constexpr pickup_desc GAUSS{pickup_kind::vulcan_cannon, 6, 0, BIT_GAUSS};
constexpr pickup_desc VULCAN_AMMO{pickup_kind::vulcan_ammo, 0, 98, 0};
constexpr pickup_desc MISSILE_1{pickup_kind::secondary, 0, 1, 0};
constexpr pickup_desc MISSILE_4{pickup_kind::secondary, 0, 4, 0};
constexpr pickup_desc MEGA{pickup_kind::secondary, 3, 1, 0};
constexpr pickup_desc QUAD{pickup_kind::flag_item, 0, 0, FLAG_QUAD};
constexpr pickup_desc CLOAK{pickup_kind::flag_item, 0, 0, FLAG_CLOAKED};
constexpr pickup_desc FLAG_BLUE{pickup_kind::team_flag, 1, 0, 0};	/* taken by the red team */
constexpr pickup_desc ORB{pickup_kind::orb, 0, 0, 0};
constexpr pickup_desc KEY{pickup_kind::none, 0, 0, 0};

[[nodiscard]]
inventory_rules make_rules(const std::uint8_t team = 0)
{
	inventory_rules r;
	r.max_energy = 200 * F1;
	r.max_shields = 200 * F1;
	r.energy_boost = 18 * F1;
	r.shield_boost = 18 * F1;
	r.max_laser_level = 3;
	r.max_super_laser_level = 5;
	r.vulcan_ammo_max = 392 * 4;
	r.secondary_max = {{20, 10, 5, 5, 10, 20, 20, 15, 10, 10}};
	r.ammo_rack_bit = FLAG_AMMO_RACK;
	r.has_team_flag_bit = FLAG_TEAM;
	r.max_orbs = 12;
	r.capture_mode = true;
	r.hoard_mode = true;
	r.team = team;
	r.max_omega_charge = F1;
	return r;
}

[[nodiscard]]
inventory spawn_inventory()
{
	inventory i;
	i.primary_flags = BIT_LASER;
	i.secondary = {{2, 0, 0, 0, 0, 0, 0, 0, 0, 0}};
	i.shields = 100 * F1;
	i.energy = 100 * F1;
	return i;
}

void test_netid_encoding()
{
	CHECK(level_netid(0) == 0x8000);
	CHECK(level_netid(349) == (0x8000 | 349));
	CHECK(is_level_netid(level_netid(17)));
	CHECK(!is_level_netid(dynamic_netid(0, 17)));
	CHECK(dynamic_netid(0, 4095) == 0x0fff);
	CHECK(dynamic_netid(7, 1) == 0x7001);
	CHECK(dynamic_netid(0, 4096) == dynamic_netid(0, 0));
	/* A dynamic id never looks like the NONE value. */
	for (unsigned c = 0; c < NETID_CREATORS; ++c)
		for (unsigned n = 0; n < 4096; ++n)
			CHECK(dynamic_netid(static_cast<std::uint8_t>(c), static_cast<std::uint16_t>(n)) != NETID_NONE);
}

void test_netid_table()
{
	const auto t{std::make_unique<table_type>()};
	t->reset();
	CHECK(t->size() == 0);
	CHECK(!t->find(5));
	CHECK(t->bind(level_netid(10), 10, 77));
	CHECK(t->size() == 1);
	CHECK(t->find(level_netid(10))->objnum == 10);
	CHECK(t->netid_of(10, 77) == level_netid(10));
	/* The slot was reused by another object: not the one the id names. */
	CHECK(t->netid_of(10, 78) == NETID_NONE);
	CHECK(t->netid_at(10) == level_netid(10));
	/* Binding the object number again drops the old id. */
	CHECK(t->bind(dynamic_netid(0, 3), 10, 90));
	CHECK(!t->bound(level_netid(10)));
	CHECK(t->netid_of(10, 90) == dynamic_netid(0, 3));
	CHECK(t->size() == 1);
	/* Binding the id again moves it. */
	CHECK(t->bind(dynamic_netid(0, 3), 11, 91));
	CHECK(t->netid_at(10) == NETID_NONE);
	CHECK(t->netid_of(11, 91) == dynamic_netid(0, 3));
	CHECK(t->size() == 1);
	t->unbind(dynamic_netid(0, 3));
	CHECK(t->size() == 0);
	CHECK(t->netid_at(11) == NETID_NONE);
	/* Invalid binds. */
	CHECK(!t->bind(NETID_NONE, 1, 1));
	CHECK(!t->bind(1, MAX_OBJECTS, 1));
	t->unbind(NETID_NONE);
	t->unbind(1234);
	CHECK(t->size() == 0);
}

void test_netid_allocation_and_reuse()
{
	const auto t{std::make_unique<netid_table<5000>>()};
	t->reset();
	/* Ids come from the creator's running counter. */
	const auto a{t->allocate(0)}, b{t->allocate(0)};
	CHECK(a == dynamic_netid(0, 0));
	CHECK(b == dynamic_netid(0, 1));
	CHECK(t->allocate(3) == dynamic_netid(3, 0));
	/* An id is not reused while bound: bind a, run the counter around. */
	CHECK(t->bind(a, 1, 1));
	for (unsigned i = 2; i < 4096; ++i)
		(void)t->allocate(0);
	/* The counter is at 0 again; 0 (= a) is bound, so the next is 1. */
	CHECK(t->allocate(0) == dynamic_netid(0, 1));
	/* After removal, the id is free again when the counter comes back. */
	t->unbind(a);
	for (unsigned i = 2; i < 4096; ++i)
		(void)t->allocate(0);
	CHECK(t->allocate(0) == a);
	/* All 4096 bound: no id. */
	t->reset();
	for (std::uint16_t i = 0; i < 4096; ++i)
		CHECK(t->bind(dynamic_netid(0, i), i, i));
	CHECK(t->allocate(0) == NETID_NONE);
	CHECK(t->allocate(1) == dynamic_netid(1, 0));
}

void test_pickup_rules()
{
	const auto r{make_rules(1)};
	auto inv{spawn_inventory()};
	/* Shields and energy: boost up to the maximum. */
	{
		const auto o{evaluate_pickup(inv, r, SHIELD, 1)};
		CHECK(o.usable && o.consumed && o.taken == static_cast<std::uint32_t>(18 * F1));
		auto full{inv};
		full.shields = 195 * F1;
		const auto o2{evaluate_pickup(full, r, SHIELD, 1)};
		CHECK(o2.usable && o2.taken == static_cast<std::uint32_t>(5 * F1));
		apply_pickup(full, r, SHIELD, o2);
		CHECK(full.shields == 200 * F1);
		CHECK(!evaluate_pickup(full, r, SHIELD, 1).usable);
		full.energy = 200 * F1;
		CHECK(!evaluate_pickup(full, r, ENERGY, 1).usable);
	}
	/* Lasers and super lasers. */
	{
		auto i{inv};
		for (int k = 0; k < 3; ++k)
		{
			const auto o{evaluate_pickup(i, r, LASER, 1)};
			CHECK(o.usable);
			apply_pickup(i, r, LASER, o);
		}
		CHECK(i.laser_level == 3);
		CHECK(!evaluate_pickup(i, r, LASER, 1).usable);
		auto s{inv};
		s.laser_level = 1;
		apply_pickup(s, r, SUPER_LASER, evaluate_pickup(s, r, SUPER_LASER, 1));
		CHECK(s.laser_level == 4);
		apply_pickup(s, r, SUPER_LASER, evaluate_pickup(s, r, SUPER_LASER, 1));
		CHECK(s.laser_level == 5);
		CHECK(!evaluate_pickup(s, r, SUPER_LASER, 1).usable);
		/* Super lasers make the normal laser unusable too. */
		CHECK(!evaluate_pickup(s, r, LASER, 1).usable);
	}
	/* Primary weapons: only if not had. */
	{
		auto i{inv};
		const auto o{evaluate_pickup(i, r, PLASMA, 1)};
		CHECK(o.usable && o.consumed);
		apply_pickup(i, r, PLASMA, o);
		CHECK(i.primary_flags & BIT_PLASMA);
		CHECK(!evaluate_pickup(i, r, PLASMA, 1).usable);
		const auto om{evaluate_pickup(i, r, OMEGA, F1 / 2)};
		CHECK(om.usable && om.taken == static_cast<std::uint32_t>(F1 / 2));
		apply_pickup(i, r, OMEGA, om);
		CHECK(i.omega_charge == F1 / 2 && (i.primary_flags & BIT_OMEGA));
	}
	/* Missiles: up to the maximum, doubled by the ammo rack; the pack is
	 * consumed even if only part of it fits.
	 */
	{
		auto i{inv};
		i.secondary[0] = 18;
		const auto o{evaluate_pickup(i, r, MISSILE_4, 1)};
		CHECK(o.usable && o.consumed && o.taken == 2);
		apply_pickup(i, r, MISSILE_4, o);
		CHECK(i.secondary[0] == 20);
		CHECK(!evaluate_pickup(i, r, MISSILE_1, 1).usable);
		i.powerup_flags |= FLAG_AMMO_RACK;
		CHECK(evaluate_pickup(i, r, MISSILE_1, 1).usable);
		i.secondary[3] = 5;
		CHECK(evaluate_pickup(i, r, MEGA, 1).usable);
		i.powerup_flags &= ~FLAG_AMMO_RACK;
		CHECK(!evaluate_pickup(i, r, MEGA, 1).usable);
	}
	/* Vulcan cannon: a new cannon goes away with what fits; a cannon the
	 * player has gives ammunition and stays with the rest.
	 */
	{
		auto i{inv};
		i.vulcan_ammo = 392 * 4 - 100;
		const auto o{evaluate_pickup(i, r, VULCAN, 196)};
		CHECK(o.usable && o.consumed && o.taken == 100 && o.remaining == 0);
		apply_pickup(i, r, VULCAN, o);
		CHECK((i.primary_flags & BIT_VULCAN) && i.vulcan_ammo == 392 * 4);
		/* Full: the cannon the player has is not usable. */
		CHECK(!evaluate_pickup(i, r, VULCAN, 196).usable);
		i.vulcan_ammo -= 50;
		const auto p{evaluate_pickup(i, r, VULCAN, 196)};
		CHECK(p.usable && !p.consumed && p.taken == 50 && p.remaining == 146);
		/* Gauss shares the vulcan ammunition. */
		const auto g{evaluate_pickup(i, r, GAUSS, 196)};
		CHECK(g.usable && g.consumed && g.taken == 50);
		/* An empty cannon can still give the weapon. */
		auto e{inv};
		const auto eo{evaluate_pickup(e, r, VULCAN, 0)};
		CHECK(eo.usable && eo.consumed && eo.taken == 0);
		/* Vulcan ammunition powerup. */
		auto v{inv};
		v.vulcan_ammo = 392 * 4 - 10;
		const auto va{evaluate_pickup(v, r, VULCAN_AMMO, 1)};
		CHECK(va.usable && va.consumed && va.taken == 10);
	}
	/* Flag items. */
	{
		auto i{inv};
		CHECK(evaluate_pickup(i, r, QUAD, 1).usable);
		i.powerup_flags |= FLAG_QUAD;
		CHECK(!evaluate_pickup(i, r, QUAD, 1).usable);
		i.powerup_flags |= FLAG_CLOAKED;
		CHECK(!evaluate_pickup(i, r, CLOAK, 1).usable);
	}
	/* CTF: the blue flag is for the red team only. */
	{
		auto i{inv};
		CHECK(evaluate_pickup(i, make_rules(1), FLAG_BLUE, 1).usable);
		CHECK(!evaluate_pickup(i, make_rules(0), FLAG_BLUE, 1).usable);
		auto nc{make_rules(1)};
		nc.capture_mode = false;
		CHECK(!evaluate_pickup(i, nc, FLAG_BLUE, 1).usable);
		apply_pickup(i, r, FLAG_BLUE, evaluate_pickup(i, r, FLAG_BLUE, 1));
		CHECK(i.powerup_flags & FLAG_TEAM);
	}
	/* Hoard orbs, up to the maximum. */
	{
		auto i{inv};
		for (int k = 0; k < 12; ++k)
		{
			const auto o{evaluate_pickup(i, r, ORB, 1)};
			CHECK(o.usable);
			apply_pickup(i, r, ORB, o);
		}
		CHECK(i.orbs == 12 && (i.powerup_flags & FLAG_TEAM));
		CHECK(!evaluate_pickup(i, r, ORB, 1).usable);
		auto nh{r};
		nh.hoard_mode = false;
		CHECK(!evaluate_pickup(inv, nh, ORB, 1).usable);
	}
	/* Keys are not arbitrated. */
	CHECK(!evaluate_pickup(inv, r, KEY, 1).usable);
	/* An outcome that was not usable changes nothing. */
	{
		auto i{inv};
		apply_pickup(i, r, PLASMA, {});
		CHECK(i == inv);
	}
}

void test_drop_rules()
{
	const auto r{make_rules(1)};
	auto inv{spawn_inventory()};
	CHECK(!evaluate_drop(inv, r, LASER, 0));
	inv.laser_level = 2;
	CHECK(evaluate_drop(inv, r, LASER, 0));
	apply_drop(inv, r, LASER, 0);
	CHECK(inv.laser_level == 1);
	/* Super lasers cannot be dropped. */
	auto s{inv};
	s.laser_level = 4;
	CHECK(!evaluate_drop(s, r, LASER, 0));
	CHECK(!evaluate_drop(inv, r, PLASMA, 0));
	inv.primary_flags |= BIT_PLASMA | BIT_VULCAN;
	inv.vulcan_ammo = 300;
	CHECK(evaluate_drop(inv, r, PLASMA, 0));
	/* A cannon cannot carry more ammunition than the player has. */
	CHECK(!evaluate_drop(inv, r, VULCAN, 301));
	CHECK(evaluate_drop(inv, r, VULCAN, 300));
	apply_drop(inv, r, VULCAN, 200);
	CHECK(!(inv.primary_flags & BIT_VULCAN) && inv.vulcan_ammo == 100);
	/* Missiles in packs. */
	inv.secondary[0] = 3;
	CHECK(!evaluate_drop(inv, r, MISSILE_4, 0));
	CHECK(evaluate_drop(inv, r, MISSILE_1, 0));
	apply_drop(inv, r, MISSILE_1, 0);
	CHECK(inv.secondary[0] == 2);
	/* Flags and orbs. */
	CHECK(!evaluate_drop(inv, r, FLAG_BLUE, 0));
	inv.powerup_flags |= FLAG_TEAM;
	CHECK(evaluate_drop(inv, r, FLAG_BLUE, 0));
	inv.orbs = 2;
	apply_drop(inv, r, ORB, 0);
	CHECK(inv.orbs == 1 && (inv.powerup_flags & FLAG_TEAM));
	apply_drop(inv, r, ORB, 0);
	CHECK(inv.orbs == 0 && !(inv.powerup_flags & FLAG_TEAM));
	CHECK(!evaluate_drop(inv, r, ORB, 0));
	/* Not droppable at all. */
	CHECK(!evaluate_drop(inv, r, SHIELD, 0));
	CHECK(!evaluate_drop(inv, r, KEY, 0));
}

void test_inventory_mirror()
{
	const auto r{make_rules()};
	inventory_mirror m;
	auto inv{spawn_inventory()};
	m.reset(inv);
	CHECK(m.current() == inv);
	/* A grant in flight survives a report sent before it arrived. */
	const auto o{evaluate_pickup(m.current(), r, PLASMA, 1)};
	const auto seq{m.on_grant(r, PLASMA, o)};
	CHECK(seq == 1 && m.pending() == 1);
	CHECK(m.current().primary_flags & BIT_PLASMA);
	auto fired{inv};
	fired.energy -= 5 * F1;
	m.on_report(r, fired, 0);
	CHECK(m.current().primary_flags & BIT_PLASMA);
	CHECK(m.current().energy == fired.energy);
	CHECK(m.pending() == 1);
	/* The report that includes the grant retires it; the grant is not
	 * applied twice.
	 */
	auto with{fired};
	with.primary_flags |= BIT_PLASMA;
	m.on_report(r, with, 1);
	CHECK(m.pending() == 0);
	CHECK(m.current() == with);
	/* Two missile grants in flight, the client full after the first. */
	auto nearly{with};
	nearly.secondary[0] = 16;
	m.on_report(r, nearly, 1);
	const auto a{evaluate_pickup(m.current(), r, MISSILE_4, 1)};
	CHECK(a.usable);
	m.on_grant(r, MISSILE_4, a);
	/* The host's copy is now full: a second request is denied. */
	CHECK(!evaluate_pickup(m.current(), r, MISSILE_1, 1).usable);
	/* The client fired two before the grant arrived; its report says 14
	 * with 1 grant applied: the copy is 14 + 4 = 18.
	 */
	auto shot{nearly};
	shot.secondary[0] = 14;
	m.on_report(r, shot, 1);
	CHECK(m.current().secondary[0] == 18);
	auto after{shot};
	after.secondary[0] = 18;
	m.on_report(r, after, 2);
	CHECK(m.current().secondary[0] == 18 && m.pending() == 0);
	/* A drop: base and copy lose the item. */
	m.on_drop(r, PLASMA, 0);
	CHECK(!(m.current().primary_flags & BIT_PLASMA));
	/* Death: nothing carried; the grant count continues. */
	m.on_grant(r, SHIELD, evaluate_pickup(m.current(), r, SHIELD, 1));
	CHECK(m.life() == 0);
	m.clear();
	CHECK(m.current() == inventory{} && m.pending() == 0 && m.issued() == 3);
	/* The next life begins; a new session starts at life 0 again. */
	CHECK(m.life() == 1);
	m.clear();
	CHECK(m.life() == 2);
	m.reset({});
	CHECK(m.life() == 0 && m.current() == inventory{});
	/* The client's count follows the host's: one life per deres, however
	 * often it is sent before the reappearance.
	 */
	own_life l;
	CHECK(l.current(0));
	l.on_deres();
	CHECK(l.life == 1 && !l.current(0) && l.current(1));
	l.on_deres();
	CHECK(l.life == 1);
	l.on_reappear();
	l.on_deres();
	CHECK(l.life == 2);
	for (int i = 0; i < 300; ++i)
	{
		l.on_reappear();
		l.on_deres();
	}
	CHECK(l.life == static_cast<std::uint8_t>(302));
	l.reset();
	CHECK(l.life == 0 && !l.dropped);
	/* Many grants in flight: bounded, the newest kept. */
	m.reset(spawn_inventory());
	constexpr int many{static_cast<int>(inventory_mirror::MAX_PENDING) + 8};
	for (int i = 0; i < many; ++i)
		m.on_grant(r, SHIELD, {true, true, 1, 0});
	CHECK(m.pending() == inventory_mirror::MAX_PENDING);
	CHECK(m.issued() == many);
	m.on_report(r, spawn_inventory(), many);
	CHECK(m.pending() == 0);
	/* The grant count wraps. */
	m.reset(spawn_inventory());
	for (int i = 0; i < 65535; ++i)
		(void)m.on_grant(r, SHIELD, {true, true, 0, 0});
	m.on_report(r, spawn_inventory(), 65535);
	CHECK(m.pending() == 0);
	m.on_grant(r, SHIELD, {true, true, F1, 0});	/* seq 0 */
	m.on_report(r, spawn_inventory(), 65535);
	CHECK(m.pending() == 1 && m.current().shields == 101 * F1);
	m.on_report(r, spawn_inventory(), 0);
	CHECK(m.pending() == 0 && m.current().shields == 100 * F1);
}

void test_decide_pickup()
{
	const auto r{make_rules(1)};
	const auto inv{spawn_inventory()};
	pickup_object_view o{true, PLASMA, 1, 0xff, 0};
	requester_view q{2, true, true};
	CHECK(decide_pickup(o, q, inv, r, 0).grant);
	/* Order of the checks. */
	o.exists = false;
	CHECK(decide_pickup(o, q, inv, r, 0).reason == deny_reason::gone);
	o.exists = true;
	q.alive = false;
	CHECK(decide_pickup(o, q, inv, r, 0).reason == deny_reason::dead);
	q.alive = true;
	q.in_range = false;
	CHECK(decide_pickup(o, q, inv, r, 0).reason == deny_reason::range);
	q.in_range = true;
	/* The spitter waits 2 s; everyone else may take it at once. */
	o.spat_owner = 2;
	o.spat_until = 2 * F1;
	CHECK(decide_pickup(o, q, inv, r, F1).reason == deny_reason::spat);
	CHECK(decide_pickup(o, q, inv, r, 2 * F1).grant);
	q.pid = 3;
	CHECK(decide_pickup(o, q, inv, r, F1).grant);
	auto has{inv};
	has.primary_flags |= BIT_PLASMA;
	const auto d{decide_pickup(o, q, has, r, 3 * F1)};
	CHECK(!d.grant && d.reason == deny_reason::cannot_use);
	o.desc = KEY;
	CHECK(decide_pickup(o, q, inv, r, 0).reason == deny_reason::not_arbitrated);
}

void test_pending_pickups()
{
	pending_pickups<4> p;
	constexpr std::int64_t timeout{F1}, cooldown{F1 / 2};
	std::optional<pending_pickup> ev;
	CHECK(p.can_request(7, 0));
	p.add({7, 10, 1, 5, true, 0, 0}, ev);
	CHECK(!ev);
	CHECK(!p.can_request(7, 100));
	CHECK(p.hidden_count() == 1);
	/* Deny: shown again, not asked for during the cooldown. */
	const auto was{p.deny(7, 1000, cooldown)};
	CHECK(was && was->hidden && was->saved_render == 5 && was->objnum == 10);
	CHECK(p.hidden_count() == 0);
	CHECK(!p.can_request(7, 1000 + cooldown - 1));
	CHECK(p.can_request(7, 1000 + cooldown));
	/* Another deny for an unknown id does nothing. */
	CHECK(!p.deny(8, 0, cooldown));
	/* Timeout. */
	p.add({8, 11, 2, 5, true, 2000, 0}, ev);
	unsigned timed_out{0};
	p.expire(2000 + timeout - 1, timeout, cooldown, [&](const pending_pickup &) { ++timed_out; });
	CHECK(timed_out == 0);
	p.expire(2000 + timeout, timeout, cooldown, [&](const pending_pickup &e) { ++timed_out; CHECK(e.netid == 8); });
	CHECK(timed_out == 1);
	CHECK(!p.can_request(8, 2000 + timeout));
	/* Ended cooldowns are forgotten. */
	p.expire(10 * F1, timeout, cooldown, [&](const pending_pickup &) { ++timed_out; });
	CHECK(timed_out == 1);
	CHECK(!p.find(7) && !p.find(8));
	/* Grant: forgotten. */
	p.add({9, 12, 3, 5, true, 0, 0}, ev);
	CHECK(p.erase(9) && !p.find(9));
	CHECK(!p.erase(9));
	/* A full table evicts the oldest request, which the caller shows. */
	for (netid_t i = 20; i < 24; ++i)
		p.add({i, i, 0, 5, true, i, 0}, ev);
	p.add({30, 30, 0, 5, true, 100, 0}, ev);
	CHECK(ev && ev->netid == 20);
	CHECK(p.find(30) && !p.find(20));
	CHECK(p.hidden_count() == 4);
}

void test_respawn_bookkeeping()
{
	CHECK(!respawn_allowed(0, 0, 1));
	CHECK(!respawn_allowed(2, 3, 1));
	CHECK(!respawn_allowed(2, 2, 1));
	CHECK(respawn_allowed(2, 1, 1));
	CHECK(!respawn_allowed(8, 5, 4));
	CHECK(respawn_allowed(8, 4, 4));
	/* One missile in the level: taken (still counted, the player carries
	 * it), fired (missing): respawned once after 2 s, and not again.
	 */
	std::uint32_t level{1}, carried{0};
	const std::uint32_t initial{1};
	respawn_timer t;
	unsigned spawned{0};
	const auto run{[&](const int ms) {
		for (int i = 0; i < ms; i += 500)
			if (t.step(respawn_allowed(initial, level + carried, 1), F1 / 2, 2 * F1))
			{
				++spawned;
				++level;
			}
	}};
	run(5000);
	CHECK(spawned == 0);
	--level;
	++carried;
	run(5000);
	CHECK(spawned == 0);
	--carried;
	run(1500);
	CHECK(spawned == 0);
	run(500);
	CHECK(spawned == 1 && level == 1);
	run(10000);
	CHECK(spawned == 1);
	/* The item came back by itself (a death drop) before the timer ran
	 * out: the timer restarts, nothing is spawned.
	 */
	--level;
	run(1500);
	++level;
	run(10000);
	CHECK(spawned == 1);
}

void test_range()
{
	const std::int32_t obj{3 * F1}, ship{5 * F1};
	CHECK(pickup_in_range(0, obj, ship, 0));
	CHECK(pickup_in_range(14 * std::int64_t{F1}, obj, ship, 0));
	CHECK(!pickup_in_range(14 * std::int64_t{F1} + 1, obj, ship, 0));
	/* A fast ship gets a quarter second of its speed. */
	CHECK(pickup_in_range(24 * std::int64_t{F1}, obj, ship, 40 * std::int64_t{F1}));
	CHECK(!pickup_in_range(25 * std::int64_t{F1}, obj, ship, 40 * std::int64_t{F1}));
	CHECK(!pickup_in_range(100 * std::int64_t{F1}, obj, ship, -5));
}

void test_wire()
{
	{
		inventory_msg m{3, 0x1234, spawn_inventory()};
		m.inv.secondary = {{1, 2, 3, 4, 5, 6, 7, 8, 9, 10}};
		m.inv.powerup_flags = 0x80000001u;
		m.inv.omega_charge = -5;
		m.inv.faking_invul = true;
		std::array<std::uint8_t, inventory_msg::SIZE> b;
		m.write(b);
		const auto r{inventory_msg::read(b)};
		CHECK(r && r->pid == 3 && r->seq == 0x1234 && r->inv == m.inv);
		CHECK(!inventory_msg::read(std::span<const std::uint8_t>(b).first(35)));
	}
	{
		obj_create_msg m{dynamic_netid(0, 77), 7, 12, 300, {{1, -2, 3 * F1}}, {{-F1, 0, 7}}, 196, 0x7, 2};
		std::array<std::uint8_t, obj_create_msg::SIZE> b;
		m.write(b);
		const auto r{obj_create_msg::read(b)};
		CHECK(r && r->netid == m.netid && r->type == 7 && r->id == 12 && r->segnum == 300 && r->pos == m.pos && r->velocity == m.velocity && r->count == 196 && r->flags == 7 && r->owner == 2);
		CHECK(r->has(obj_create_flag::spat) && r->has(obj_create_flag::appear));
		m.netid = NETID_NONE;
		m.write(b);
		CHECK(!obj_create_msg::read(b));
	}
	{
		std::array<std::uint8_t, obj_remove_msg::SIZE> b;
		obj_remove_msg{level_netid(5), obj_remove_reason::expired}.write(b);
		const auto r{obj_remove_msg::read(b)};
		CHECK(r && r->netid == level_netid(5) && r->reason == obj_remove_reason::expired);
		b[2] = 9;
		CHECK(!obj_remove_msg::read(b));
	}
	{
		std::array<std::uint8_t, obj_settle_msg::SIZE> b;
		obj_settle_msg{5, 6, {{7, 8, 9}}}.write(b);
		const auto r{obj_settle_msg::read(b)};
		CHECK(r && r->netid == 5 && r->segnum == 6 && r->pos == (std::array<std::int32_t, 3>{{7, 8, 9}}));
	}
	{
		std::array<std::uint8_t, pickup_request_msg::SIZE> b;
		pickup_request_msg{0x8123, 17}.write(b);
		const auto r{pickup_request_msg::read(b)};
		CHECK(r && r->netid == 0x8123 && r->powerup_id == 17);
	}
	{
		std::array<std::uint8_t, pickup_grant_msg::SIZE> b;
		pickup_grant_msg{4, 0x8001, 2, 0x10000, 146, 1, 200}.write(b);
		const auto r{pickup_grant_msg::read(b)};
		CHECK(r && r->pid == 4 && r->netid == 0x8001 && r->powerup_id == 2 && r->count == 0x10000 && r->remaining == 146 && r->removed() && r->life == 200);
		CHECK(!pickup_grant_msg::read(std::span<const std::uint8_t>(b).first(13)));
		b[0] = 8;
		CHECK(!pickup_grant_msg::read(b));
	}
	{
		std::array<std::uint8_t, pickup_deny_msg::SIZE> b;
		pickup_deny_msg{9, deny_reason::cannot_use}.write(b);
		const auto r{pickup_deny_msg::read(b)};
		CHECK(r && r->netid == 9 && r->reason == deny_reason::cannot_use);
		b[2] = 200;
		CHECK(!pickup_deny_msg::read(b));
	}
	{
		std::array<std::uint8_t, drop_request_msg::SIZE> b;
		drop_request_msg{3, 0xdeadbeef}.write(b);
		const auto r{drop_request_msg::read(b)};
		CHECK(r && r->powerup_id == 3 && r->count == 0xdeadbeef);
		CHECK(!drop_request_msg::read(std::span<const std::uint8_t>(b).first(4)));
	}
	{
		std::array<std::uint8_t, spawn_request_msg::SIZE> b;
		spawn_request_msg{17}.write(b);
		const auto r{spawn_request_msg::read(b)};
		CHECK(r && r->request == 17);
		b[0] = 0;	/* 0 names a join in progress, never a request */
		CHECK(!spawn_request_msg::read(b));
		CHECK(!spawn_request_msg::read(std::span<const std::uint8_t>(b).first(0)));
	}
	{
		std::array<std::uint8_t, spawn_site_msg::SIZE> b;
		spawn_site_msg{SPAWN_REQUEST_JOIN, 5}.write(b);
		const auto r{spawn_site_msg::read(b)};
		CHECK(r && r->request == SPAWN_REQUEST_JOIN && r->site == 5);
		spawn_site_msg{200, SPAWN_SITE_NONE}.write(b);
		const auto n{spawn_site_msg::read(b)};
		CHECK(n && n->request == 200 && n->site == SPAWN_SITE_NONE);
		CHECK(!spawn_site_msg::read(std::span<const std::uint8_t>(b).first(1)));
	}
}

/* A model of a game: one host and clients, each with its own copy of the
 * level's powerups keyed by net id, connected by reliable ordered
 * channels.  The host runs the arbitration exactly as the game does
 * (decide_pickup with its copy of the requester's inventory), clients
 * apply grants to their own inventory and report it back.
 */
struct model_object
{
	std::uint8_t type{};
	pickup_desc desc{};
	std::uint32_t count{1};
	bool hidden{};
	friend bool operator==(const model_object &a, const model_object &b)
	{
		return a.type == b.type && a.count == b.count;
	}
};

struct model_message
{
	enum class kind { request, grant, deny, create, remove, report, drop, deres, reappear } k{};
	std::uint8_t from{};
	pickup_request_msg request{};
	pickup_grant_msg grant{};
	pickup_deny_msg deny{};
	obj_create_msg create{};
	obj_remove_msg remove{};
	inventory_msg report{};
	drop_request_msg drop{};
};

struct model_client
{
	std::map<netid_t, model_object> objects;
	inventory inv{spawn_inventory()};
	std::uint16_t applied{};
	pending_pickups<> pending;
	std::deque<model_message> inbox;
	unsigned grants_received{};
	own_life life;
	/* Sent its deres, not reappeared yet. */
	bool dead{};
};

struct model_game
{
	static constexpr unsigned PLAYERS{4};
	std::vector<pickup_desc> types{SHIELD, ENERGY, PLASMA, VULCAN, MISSILE_4, MEGA, QUAD, FLAG_BLUE, ORB, LASER};
	std::map<netid_t, model_object> host_objects;
	std::unique_ptr<table_type> table{std::make_unique<table_type>()};
	std::array<inventory_mirror, PLAYERS> mirrors;
	/* The host dropped the player's items; it has not reappeared. */
	std::array<bool, PLAYERS> dropped{};
	std::array<model_client, PLAYERS> clients;
	std::deque<model_message> host_inbox;
	std::int64_t now{};
	std::uint16_t next_objnum{};
	/* Grants per object id, for the "never twice" check. */
	std::map<netid_t, unsigned> consumed_grants;
	unsigned grants{}, denies{};
	std::uint32_t cannon_ammo_granted{};
	/* Plasma cannons the host created or removed on its own (not the
	 * drops of dead and departed players), for the conservation check.
	 */
	unsigned plasma_created{}, plasma_removed{}, plasma_dropped{};
	unsigned stale_grants{};

	[[nodiscard]]
	inventory_rules rules(const unsigned pid) const
	{
		return make_rules(static_cast<std::uint8_t>(pid & 1));
	}
	void start()
	{
		table->reset();
		for (unsigned i = 0; i < PLAYERS; ++i)
			mirrors[i].reset(spawn_inventory());
		/* Level objects: identical everywhere. */
		for (std::uint16_t o = 0; o < 20; ++o)
		{
			const auto id{level_netid(o)};
			const std::uint8_t type{static_cast<std::uint8_t>(o % types.size())};
			model_object m{type, types[type], types[type].kind == pickup_kind::vulcan_cannon ? 196u : 1u};
			host_objects[id] = m;
			table->bind(id, o, 0);
			for (auto &c : clients)
				c.objects[id] = m;
		}
		next_objnum = 20;
	}
	void broadcast(const model_message &m, const unsigned exclude = PLAYERS)
	{
		for (unsigned i = 1; i < PLAYERS; ++i)
			if (i != exclude)
				clients[i].inbox.push_back(m);
	}
	/* Host: create an object (respawn, death drop). */
	void host_create(const std::uint8_t type, const std::uint32_t count, const std::uint8_t owner = 0xff, const bool drop = false)
	{
		if (types[type].kind == pickup_kind::primary && types[type].bit == BIT_PLASMA)
			++(drop ? plasma_dropped : plasma_created);
		const auto id{table->allocate(0)};
		CHECK(id != NETID_NONE);
		table->bind(id, next_objnum++ % MAX_OBJECTS, 0);
		host_objects[id] = {type, types[type], count};
		obj_create_msg c{};
		c.netid = id;
		c.id = type;
		c.count = count;
		c.owner = owner;
		model_message m{.k = model_message::kind::create};
		m.create = c;
		broadcast(m);
		clients[0].objects[id] = host_objects[id];
	}
	void host_remove(const netid_t id)
	{
		if (const auto it{host_objects.find(id)}; it != host_objects.end() && it->second.desc.kind == pickup_kind::primary && it->second.desc.bit == BIT_PLASMA)
			++plasma_removed;
		host_objects.erase(id);
		table->unbind(id);
		clients[0].objects.erase(id);
		model_message m{.k = model_message::kind::remove};
		m.remove = {id, obj_remove_reason::expired};
		broadcast(m);
	}
	/* Host: a request from `pid` (the host's own touch is pid 0,
	 * decided at once).
	 */
	void host_request(const unsigned pid, const pickup_request_msg &rq)
	{
		const auto it{host_objects.find(rq.netid)};
		pickup_object_view o{};
		if (it != host_objects.end() && it->second.type == rq.powerup_id)
		{
			o.exists = true;
			o.desc = it->second.desc;
			o.count = it->second.count;
		}
		const auto inv{pid == 0 ? clients[0].inv : mirrors[pid].current()};
		const auto d{decide_pickup(o, {static_cast<std::uint8_t>(pid), pid == 0 || !dropped[pid], true}, inv, rules(pid), now)};
		if (!d.grant)
		{
			++denies;
			if (pid != 0)
			{
				model_message m{.k = model_message::kind::deny, .from = 0};
				m.deny = {rq.netid, d.reason};
				clients[pid].inbox.push_back(m);
			}
			return;
		}
		++grants;
		auto &obj{it->second};
		if (d.outcome.consumed)
			++consumed_grants[rq.netid];
		if (obj.desc.kind == pickup_kind::vulcan_cannon)
			cannon_ammo_granted += d.outcome.taken;
		pickup_grant_msg g{static_cast<std::uint8_t>(pid), rq.netid, rq.powerup_id, d.outcome.taken, d.outcome.remaining, static_cast<std::uint8_t>(d.outcome.consumed ? 1 : 0), mirrors[pid].life()};
		if (pid == 0)
			apply_pickup(clients[0].inv, rules(0), obj.desc, d.outcome);
		else
			mirrors[pid].on_grant(rules(pid), obj.desc, d.outcome);
		if (d.outcome.consumed)
		{
			host_objects.erase(it);
			table->unbind(rq.netid);
			clients[0].objects.erase(rq.netid);
		}
		else
		{
			obj.count = d.outcome.remaining;
			clients[0].objects[rq.netid].count = d.outcome.remaining;
		}
		model_message m{.k = model_message::kind::grant};
		m.grant = g;
		broadcast(m);
	}
	void host_receive()
	{
		while (!host_inbox.empty())
		{
			const auto m{host_inbox.front()};
			host_inbox.pop_front();
			switch (m.k)
			{
				case model_message::kind::request:
					host_request(m.from, m.request);
					break;
				case model_message::kind::report:
					/* A dead player's report waits for its reappearance. */
					if (!dropped[m.from])
						mirrors[m.from].on_report(rules(m.from), m.report.inv, m.report.seq);
					break;
				case model_message::kind::deres:
					host_drop(m.from);
					break;
				case model_message::kind::reappear:
					dropped[m.from] = false;
					break;
				case model_message::kind::drop:
					if (dropped[m.from])
						break;
					{
						const auto type{m.drop.powerup_id};
						const auto &desc{types[type]};
						if (!evaluate_drop(mirrors[m.from].current(), rules(m.from), desc, m.drop.count))
							break;
						mirrors[m.from].on_drop(rules(m.from), desc, m.drop.count);
						host_create(type, desc.kind == pickup_kind::vulcan_cannon ? m.drop.count : 1u, static_cast<std::uint8_t>(m.from));
					}
					break;
				default:
					break;
			}
		}
	}
	/* Host: player `pid` died (its deres) or left: drop the plasma cannon
	 * and quad lasers of the host's copy (grants in flight included),
	 * once per life, and forget the rest.
	 */
	void host_drop(const unsigned pid)
	{
		if (pid == 0 || dropped[pid])
			return;
		dropped[pid] = true;
		const auto inv{mirrors[pid].current()};
		if (inv.primary_flags & BIT_PLASMA)
			host_create(2, 1, 0xff, true);
		if (inv.powerup_flags & FLAG_QUAD)
			host_create(6, 1, 0xff, true);
		mirrors[pid].clear();
	}
	/* Host: a player joins slot `pid` (a new player, or the one who left
	 * it): an empty copy until its first report (net_objects_host_join).
	 */
	void host_join(const unsigned pid)
	{
		mirrors[pid].reset({});
		dropped[pid] = false;
		auto &c{clients[pid]};
		c = model_client{};
		c.objects = host_objects;
		for (auto &[id, o] : c.objects)
			o.hidden = false;
	}
	/* Client `pid` dies: its inventory, then its deres. */
	void client_die(const unsigned pid)
	{
		auto &c{clients[pid]};
		if (pid == 0 || c.dead)
			return;
		model_message rep{.k = model_message::kind::report, .from = static_cast<std::uint8_t>(pid)};
		rep.report = {static_cast<std::uint8_t>(pid), c.applied, c.inv};
		host_inbox.push_back(rep);
		host_inbox.push_back({.k = model_message::kind::deres, .from = static_cast<std::uint8_t>(pid)});
		c.life.on_deres();
		c.dead = true;
	}
	/* Client `pid` respawns with a new ship. */
	void client_respawn(const unsigned pid)
	{
		auto &c{clients[pid]};
		if (!c.dead)
			return;
		c.dead = false;
		c.inv = spawn_inventory();
		c.life.on_reappear();
		host_inbox.push_back({.k = model_message::kind::reappear, .from = static_cast<std::uint8_t>(pid)});
		model_message rep{.k = model_message::kind::report, .from = static_cast<std::uint8_t>(pid)};
		rep.report = {static_cast<std::uint8_t>(pid), c.applied, c.inv};
		host_inbox.push_back(rep);
	}
	/* Plasma cannons in the level and carried by live players. */
	[[nodiscard]]
	unsigned plasma_in_game() const
	{
		unsigned n{0};
		for (const auto &[id, o] : host_objects)
			if (o.desc.kind == pickup_kind::primary && o.desc.bit == BIT_PLASMA)
				++n;
		for (const auto &c : clients)
			if (!c.dead && (c.inv.primary_flags & BIT_PLASMA))
				++n;
		return n;
	}
	/* Client `pid` touches the object: asks if it can use it. */
	void client_touch(const unsigned pid, const netid_t id)
	{
		auto &c{clients[pid]};
		if (c.dead)
			return;
		const auto it{c.objects.find(id)};
		if (it == c.objects.end() || it->second.hidden)
			return;
		if (pid == 0)
		{
			host_request(0, {id, it->second.type});
			return;
		}
		if (!c.pending.can_request(id, now))
			return;
		if (!evaluate_pickup(c.inv, rules(pid), it->second.desc, it->second.count).usable)
			return;
		model_message rep{.k = model_message::kind::report, .from = static_cast<std::uint8_t>(pid)};
		rep.report = {static_cast<std::uint8_t>(pid), c.applied, c.inv};
		host_inbox.push_back(rep);
		model_message m{.k = model_message::kind::request, .from = static_cast<std::uint8_t>(pid)};
		m.request = {id, it->second.type};
		host_inbox.push_back(m);
		std::optional<pending_pickup> ev;
		c.pending.add({id, 0, 0, 0, true, now, 0}, ev);
		if (ev)
			if (const auto e{c.objects.find(ev->netid)}; e != c.objects.end())
				e->second.hidden = false;
		it->second.hidden = true;
	}
	/* Client `pid` fires a missile or spends energy. */
	void client_consume(const unsigned pid)
	{
		auto &c{clients[pid]};
		if (c.dead)
			return;
		if (c.inv.secondary[0])
			--c.inv.secondary[0];
		c.inv.energy = std::max(0, c.inv.energy - 3 * F1);
		c.inv.shields = std::max(1, c.inv.shields - 7 * F1);
		if (c.inv.vulcan_ammo >= 30)
			c.inv.vulcan_ammo -= 30;
		if (pid != 0)
		{
			model_message rep{.k = model_message::kind::report, .from = static_cast<std::uint8_t>(pid)};
			rep.report = {static_cast<std::uint8_t>(pid), c.applied, c.inv};
			host_inbox.push_back(rep);
		}
	}
	void client_drop_missiles(const unsigned pid)
	{
		auto &c{clients[pid]};
		if (pid == 0 || c.dead || c.inv.secondary[0] < 4)
			return;
		c.inv.secondary[0] -= 4;
		model_message m{.k = model_message::kind::drop, .from = static_cast<std::uint8_t>(pid)};
		m.drop = {4, 0};	/* type 4 = MISSILE_4 */
		host_inbox.push_back(m);
	}
	void client_receive(const unsigned pid, const std::size_t max)
	{
		auto &c{clients[pid]};
		for (std::size_t n = 0; n < max && !c.inbox.empty(); ++n)
		{
			const auto m{c.inbox.front()};
			c.inbox.pop_front();
			switch (m.k)
			{
				case model_message::kind::grant:
					{
						const auto &g{m.grant};
						const auto it{c.objects.find(g.netid)};
						CHECK(it != c.objects.end());
						if (g.pid == pid)
						{
							++c.applied;
							++c.grants_received;
							c.pending.erase(g.netid);
							/* A grant for a life that ended was in the
							 * host's drop of that life
							 * (net_objects.cpp apply_own_grant).
							 */
							if (!c.life.current(g.life))
								++stale_grants;
							else
							{
								/* Apply the granted amount, as do_powerup
								 * does with the count the grant sets.
								 */
								const auto &desc{it->second.desc};
								pickup_outcome o{true, g.removed(), g.count, g.remaining};
								if (desc.kind == pickup_kind::energy || desc.kind == pickup_kind::shield)
									o = evaluate_pickup(c.inv, rules(pid), desc, 1);
								apply_pickup(c.inv, rules(pid), desc, o);
							}
						}
						if (g.removed())
							c.objects.erase(it);
						else
						{
							it->second.count = g.remaining;
							if (g.pid == pid)
								it->second.hidden = false;
						}
					}
					break;
				case model_message::kind::deny:
					if (const auto was{c.pending.deny(m.deny.netid, now, F1 / 2)})
						if (const auto it{c.objects.find(m.deny.netid)}; it != c.objects.end())
							it->second.hidden = false;
					break;
				case model_message::kind::create:
					{
						const auto &cr{m.create};
						c.objects[cr.netid] = {cr.id, types[cr.id], cr.count};
					}
					break;
				case model_message::kind::remove:
					c.objects.erase(m.remove.netid);
					c.pending.erase(m.remove.netid);
					break;
				default:
					break;
			}
		}
		c.pending.expire(now, F1, F1 / 2, [&](const pending_pickup &e) {
			if (const auto it{c.objects.find(e.netid)}; it != c.objects.end())
				it->second.hidden = false;
		});
	}
	void settle()
	{
		for (int round = 0; round < 64; ++round)
		{
			host_receive();
			for (unsigned i = 1; i < PLAYERS; ++i)
				client_receive(i, 1000);
		}
		host_receive();
	}
	/* Every machine has the host's objects, with the same content, and
	 * nothing is hidden once all answers arrived.
	 */
	void check_agreement() const
	{
		for (unsigned i = 0; i < PLAYERS; ++i)
		{
			const auto &c{clients[i]};
			CHECK(c.objects.size() == host_objects.size());
			for (const auto &[id, o] : host_objects)
			{
				const auto it{c.objects.find(id)};
				CHECK(it != c.objects.end());
				CHECK(it->second == o);
				CHECK(!it->second.hidden);
			}
		}
		CHECK(table->size() == host_objects.size());
		for (const auto &[id, n] : consumed_grants)
			CHECK(n == 1);
	}
};

/* Two ships touch the same weapon in the same frame: exactly one gets it,
 * whichever request the host sees first, and the other sees it vanish.
 */
void test_simultaneous_requests()
{
	for (int order = 0; order < 2; ++order)
	{
		model_game g;
		g.start();
		const auto plasma{level_netid(2)};
		CHECK(g.host_objects[plasma].desc.kind == pickup_kind::primary);
		const unsigned first{order ? 2u : 1u}, second{order ? 1u : 2u};
		g.client_touch(first, plasma);
		g.client_touch(second, plasma);
		/* Both hid it locally. */
		CHECK(g.clients[1].objects[plasma].hidden && g.clients[2].objects[plasma].hidden);
		g.settle();
		CHECK(g.grants == 1 && g.denies == 1);
		CHECK(g.clients[first].inv.primary_flags & BIT_PLASMA);
		CHECK(!(g.clients[second].inv.primary_flags & BIT_PLASMA));
		CHECK(g.mirrors[first].current().primary_flags & BIT_PLASMA);
		g.check_agreement();
	}
	/* The host touches it in the same frame as a client whose request is
	 * still on its way: the host's own touch is decided first.
	 */
	{
		model_game g;
		g.start();
		const auto plasma{level_netid(2)};
		g.client_touch(1, plasma);
		g.client_touch(0, plasma);
		g.settle();
		CHECK(g.grants == 1 && g.denies == 1);
		CHECK(g.clients[0].inv.primary_flags & BIT_PLASMA);
		CHECK(!(g.clients[1].inv.primary_flags & BIT_PLASMA));
		g.check_agreement();
	}
}

/* Deny path: a client full of missiles touches a missile pack: it does not
 * even ask, and the pack stays everywhere.  A client whose copy on the
 * host is full (a grant in flight) is denied and shows the pack again.
 */
void test_deny_and_restore()
{
	model_game g;
	g.start();
	const auto pack_a{level_netid(4)}, pack_b{level_netid(14)};
	CHECK(g.host_objects[pack_a].desc.kind == pickup_kind::secondary && g.host_objects[pack_b].desc.kind == pickup_kind::secondary);
	auto &c{g.clients[1]};
	c.inv.secondary[0] = 20;
	g.mirrors[1].on_report(g.rules(1), c.inv, 0);
	g.client_touch(1, pack_a);
	CHECK(!c.objects[pack_a].hidden);
	CHECK(g.host_inbox.empty());
	g.settle();
	g.check_agreement();
	/* Room for one pack: both touched within one round trip. */
	c.inv.secondary[0] = 16;
	g.client_touch(1, pack_a);
	g.client_touch(1, pack_b);
	CHECK(c.objects[pack_a].hidden && c.objects[pack_b].hidden);
	g.settle();
	CHECK(g.grants == 1 && g.denies == 1);
	CHECK(c.inv.secondary[0] == 20);
	CHECK(g.host_objects.count(pack_a) == 0 && g.host_objects.count(pack_b) == 1);
	g.check_agreement();
	/* The denied pack is not asked for again during the cooldown. */
	c.inv.secondary[0] = 10;
	g.client_touch(1, pack_b);
	CHECK(g.host_inbox.empty());
	g.now += F1;
	g.client_touch(1, pack_b);
	CHECK(!g.host_inbox.empty());
	g.settle();
	CHECK(c.inv.secondary[0] == 14);
	g.check_agreement();
}

/* A request for an object that is gone (taken by another, expired, or its
 * id now names something else), and a request repeated after the client's
 * timeout: never a second grant.
 */
void test_stale_and_duplicate_requests()
{
	model_game g;
	g.start();
	const auto shield{level_netid(0)}, quad{level_netid(6)};
	/* Expired on the host while the request was on its way. */
	g.client_touch(1, shield);
	g.host_remove(shield);
	g.settle();
	CHECK(g.grants == 0 && g.denies == 1);
	g.check_agreement();
	/* The client timed out and asks again before the first answer. */
	g.client_touch(2, quad);
	g.now += 2 * F1;
	g.client_receive(2, 0);	/* expire */
	CHECK(!g.clients[2].objects[quad].hidden);
	g.now += F1;
	g.client_receive(2, 0);
	g.client_touch(2, quad);
	g.settle();
	CHECK(g.grants == 1 && g.denies == 2);
	CHECK(g.clients[2].grants_received == 1);
	g.check_agreement();
	/* An id reused by a different object: the powerup type differs, the
	 * request is for something that no longer exists.
	 */
	const auto id{level_netid(8)};
	g.host_objects[id].type = 0;
	g.host_objects[id].desc = SHIELD;
	pickup_request_msg rq{id, 8};
	g.host_request(3, rq);
	CHECK(g.denies == 3);
}

/* Partial vulcan ammunition: several players take from one cannon they
 * all have; the ammunition is never created or lost.
 */
void test_cannon_ammunition()
{
	model_game g;
	g.start();
	const auto cannon{level_netid(3)};
	CHECK(g.host_objects[cannon].desc.kind == pickup_kind::vulcan_cannon);
	for (unsigned i = 1; i < model_game::PLAYERS; ++i)
	{
		auto &c{g.clients[i]};
		c.inv.primary_flags |= BIT_VULCAN;
		c.inv.vulcan_ammo = static_cast<std::uint16_t>(392 * 4 - 60);
		g.mirrors[i].on_report(g.rules(i), c.inv, 0);
	}
	for (unsigned i = 1; i < model_game::PLAYERS; ++i)
		g.client_touch(i, cannon);
	g.settle();
	CHECK(g.grants == 3);
	CHECK(g.cannon_ammo_granted == 180);
	CHECK(g.host_objects[cannon].count == 196 - 180);
	for (unsigned i = 1; i < model_game::PLAYERS; ++i)
		CHECK(g.clients[i].inv.vulcan_ammo == 392 * 4);
	g.check_agreement();
	/* Everyone full: the cannon stays with its 16 rounds. */
	for (unsigned i = 1; i < model_game::PLAYERS; ++i)
		g.client_touch(i, cannon);
	CHECK(g.host_inbox.empty());
	/* A player without the cannon takes it, with what fits. */
	g.clients[0].inv.vulcan_ammo = 0;
	g.client_touch(0, cannon);
	CHECK(g.host_objects.count(cannon) == 0);
	CHECK(g.clients[0].inv.vulcan_ammo == 16 && (g.clients[0].inv.primary_flags & BIT_VULCAN));
	g.settle();
	g.check_agreement();
}

/* A random game: players touch random objects, fire, drop missiles; the
 * host creates and expires objects; the channels deliver in order but
 * with random delays.  At the end every machine agrees, no consumed
 * object was granted twice, and each client's copy on the host equals the
 * client's inventory once its last report arrived.
 */
void test_random_game()
{
	unsigned stale_total{0}, dropped_total{0};
	for (unsigned seed = 1; seed <= 30; ++seed)
	{
		std::mt19937 rng{seed};
		model_game g;
		g.start();
		for (int step = 0; step < 3000; ++step)
		{
			g.now += F1 / 60;
			const auto pick{rng() % 100};
			const unsigned pid{static_cast<unsigned>(rng() % model_game::PLAYERS)};
			if (pick < 50 && !g.host_objects.empty())
			{
				auto it{g.host_objects.begin()};
				std::advance(it, rng() % g.host_objects.size());
				const auto id{it->first};
				g.client_touch(pid, id);
				/* Often someone else touches it in the same frame. */
				if (rng() % 2)
					g.client_touch((pid + 1 + rng() % (model_game::PLAYERS - 1)) % model_game::PLAYERS, id);
			}
			else if (pick < 65)
				g.client_consume(pid);
			else if (pick < 70)
				g.client_drop_missiles(pid);
			else if (pick < 75)
				g.host_create(static_cast<std::uint8_t>(rng() % g.types.size()), 196);
			else if (pick < 77 && !g.host_objects.empty())
			{
				auto it{g.host_objects.begin()};
				std::advance(it, rng() % g.host_objects.size());
				g.host_remove(it->first);
			}
			else if (pick < 79)
				g.client_die(pid);
			else if (pick < 83)
				g.client_respawn(pid);
			/* The network: the host reads everything, clients read a
			 * random part of what arrived.
			 */
			if (rng() % 3 == 0)
				g.host_receive();
			for (unsigned i = 1; i < model_game::PLAYERS; ++i)
				g.client_receive(i, rng() % 3);
		}
		for (unsigned i = 1; i < model_game::PLAYERS; ++i)
			g.client_respawn(i);
		g.settle();
		g.check_agreement();
		/* No plasma cannon appeared from nowhere or vanished: a grant
		 * for a life that ended is never applied to the next ship.
		 */
		CHECK(g.plasma_in_game() == 2 + g.plasma_created - g.plasma_removed);
		/* Final reports: the host's copies are exact. */
		for (unsigned i = 1; i < model_game::PLAYERS; ++i)
		{
			model_message rep{.k = model_message::kind::report, .from = static_cast<std::uint8_t>(i)};
			rep.report = {static_cast<std::uint8_t>(i), g.clients[i].applied, g.clients[i].inv};
			g.host_inbox.push_back(rep);
		}
		g.host_receive();
		for (unsigned i = 1; i < model_game::PLAYERS; ++i)
		{
			CHECK(g.mirrors[i].current() == g.clients[i].inv);
			CHECK(g.mirrors[i].pending() == 0);
			CHECK(g.mirrors[i].life() == g.clients[i].life.life);
		}
		CHECK(g.grants > 50);
		stale_total += g.stale_grants;
		dropped_total += g.plasma_dropped;
	}
	/* The random games did reach the cases they are meant to cover. */
	CHECK(stale_total > 0);
	CHECK(dropped_total > 0);
}

/* A client asks for a cannon and dies before the grant arrives (review
 * finding: the grant arrived after the respawn and gave the new ship the
 * cannon that the host had also dropped).  The grant is for the life
 * that ended: the host drops the cannon once, and the new ship does not
 * get it, whether the grant arrives before or after the respawn.
 */
void test_grant_after_death()
{
	for (int respawn_first = 0; respawn_first < 2; ++respawn_first)
	{
		model_game g;
		g.start();
		const auto plasma{level_netid(2)};
		CHECK(g.plasma_in_game() == 2);
		g.client_touch(1, plasma);
		g.client_die(1);
		/* The host grants (life 0), then drops the cannon with the rest. */
		g.host_receive();
		CHECK(g.grants == 1 && g.host_objects.count(plasma) == 0);
		CHECK(g.plasma_dropped == 1 && g.mirrors[1].life() == 1);
		CHECK(g.mirrors[1].current() == inventory{});
		if (respawn_first)
			g.client_respawn(1);
		g.settle();
		g.client_respawn(1);
		g.settle();
		CHECK(g.stale_grants == 1);
		CHECK(!(g.clients[1].inv.primary_flags & BIT_PLASMA));
		CHECK(g.clients[1].inv == spawn_inventory());
		CHECK(g.plasma_in_game() == 2);
		g.check_agreement();
		/* The copy and the client agree once the next report arrives. */
		model_message rep{.k = model_message::kind::report, .from = 1};
		rep.report = {1, g.clients[1].applied, g.clients[1].inv};
		g.host_inbox.push_back(rep);
		g.host_receive();
		CHECK(g.mirrors[1].current() == g.clients[1].inv && g.mirrors[1].pending() == 0);
		CHECK(g.mirrors[1].life() == g.clients[1].life.life);
		/* The new life's grants apply again. */
		netid_t dropped{NETID_NONE};
		for (const auto &[id, o] : g.host_objects)
			if (!is_level_netid(id) && o.desc.kind == pickup_kind::primary)
				dropped = id;
		CHECK(dropped != NETID_NONE);
		g.client_touch(1, dropped);
		g.settle();
		CHECK(g.clients[1].inv.primary_flags & BIT_PLASMA);
		CHECK(g.plasma_in_game() == 2);
		g.check_agreement();
	}
}

/* A player who carries a cannon leaves; the host drops it.  A player who
 * joins the slot (or the same one, rejoining) starts with an empty copy,
 * not with what the departed ship held: if it leaves again before its
 * first report, nothing is dropped a second time.
 */
void test_rejoin_after_drop()
{
	model_game g;
	g.start();
	const auto plasma{level_netid(2)};
	g.client_touch(1, plasma);
	g.settle();
	CHECK(g.clients[1].inv.primary_flags & BIT_PLASMA);
	/* Disconnect: the host drops from its copy. */
	g.host_drop(1);
	g.clients[1].dead = true;
	CHECK(g.plasma_dropped == 1);
	g.settle();
	CHECK(g.plasma_in_game() == 2);
	g.host_join(1);
	CHECK(g.mirrors[1].current() == inventory{} && g.mirrors[1].life() == 0);
	/* Leaves again before its first report. */
	g.host_drop(1);
	g.clients[1].dead = true;
	CHECK(g.plasma_dropped == 1);
	g.settle();
	CHECK(g.plasma_in_game() == 2);
	/* Joins again and reports: the copy is its inventory. */
	g.host_join(1);
	model_message rep{.k = model_message::kind::report, .from = 1};
	rep.report = {1, 0, g.clients[1].inv};
	g.host_inbox.push_back(rep);
	g.settle();
	CHECK(g.mirrors[1].current() == spawn_inventory());
	g.check_agreement();
}

/* Join in progress: the snapshot carries every net id with its object;
 * the joiner places the objects at object numbers of its own and binds
 * the ids to them.  The joiner then sees exactly the host's powerups, and
 * a later grant or removal names the same object on both.
 */
void test_joiner_snapshot()
{
	struct object_slot
	{
		bool used{};
		std::uint16_t signature{};
		std::uint8_t type{};
		std::uint32_t count{};
	};
	std::array<object_slot, MAX_OBJECTS> host{}, joiner{};
	const auto host_table{std::make_unique<table_type>()};
	const auto joiner_table{std::make_unique<table_type>()};
	host_table->reset();
	std::uint16_t sig{1};
	/* Level objects, some of them taken meanwhile, and host-created ones
	 * in reused slots.
	 */
	for (std::uint16_t o = 0; o < 60; ++o)
	{
		host[o] = {true, sig++, static_cast<std::uint8_t>(o % 9), 1};
		host_table->bind(level_netid(o), o, host[o].signature);
	}
	for (std::uint16_t o = 0; o < 60; o += 3)
	{
		host_table->unbind(level_netid(o));
		host[o] = {};
	}
	for (std::uint16_t o = 0; o < 60; o += 6)
	{
		host[o] = {true, sig++, 3, 196};
		const auto id{host_table->allocate(0)};
		host_table->bind(id, o, host[o].signature);
	}
	/* An object without a net id (a robot's egg in a robot game). */
	host[100] = {true, sig++, 5, 1};
	/* Snapshot entries: object number and net id (NONE if unbound). */
	struct entry
	{
		std::uint16_t objnum;
		netid_t netid;
		object_slot content;
	};
	std::vector<entry> entries;
	for (std::uint16_t o = 0; o < MAX_OBJECTS; ++o)
		if (host[o].used)
			entries.push_back({o, host_table->netid_of(o, host[o].signature), host[o]});
	/* The joiner allocates its own numbers (from the top, to make them
	 * differ) and binds.
	 */
	joiner_table->reset();
	std::uint16_t next{MAX_OBJECTS - 1};
	std::uint16_t jsig{500};
	for (const auto &e : entries)
	{
		const std::uint16_t o{next--};
		joiner[o] = e.content;
		joiner[o].signature = jsig++;
		if (e.netid != NETID_NONE)
			CHECK(joiner_table->bind(e.netid, o, joiner[o].signature));
	}
	CHECK(joiner_table->size() == host_table->size());
	/* Every id names the same object on both. */
	unsigned checked{0};
	for (std::uint32_t id = 0; id < 0x10000; ++id)
	{
		const auto h{host_table->find(static_cast<netid_t>(id))};
		const auto j{joiner_table->find(static_cast<netid_t>(id))};
		CHECK(h.has_value() == j.has_value());
		if (!h)
			continue;
		const auto &ho{host[h->objnum]};
		const auto &jo{joiner[j->objnum]};
		CHECK(ho.type == jo.type && ho.count == jo.count);
		CHECK(joiner_table->netid_of(j->objnum, jo.signature) == static_cast<netid_t>(id));
		++checked;
	}
	CHECK(checked == host_table->size());
	/* The joiner's object in a reused slot is not taken for the old id. */
	const auto some{*joiner_table->find(level_netid(1))};
	joiner[some.objnum].signature = static_cast<std::uint16_t>(joiner[some.objnum].signature + 1);
	CHECK(joiner_table->netid_of(some.objnum, joiner[some.objnum].signature) == NETID_NONE);
}


/* -lagtest (net_v2_lagtest.h): the host's own pickups as a client's. */
void test_lag_pickups()
{
	using stage = lag_pickup::stage;
	CHECK(lagtest_time(0) == 0);
	CHECK(lagtest_time(1000) == F1);
	CHECK(lagtest_time(500) == F1 / 2);
	lag_pickups<4> q;
	/* The round trip splits into the request's way and the answer's,
	 * together exactly the round trip.
	 */
	q.set_round_trip(lagtest_time(1));
	CHECK(q.up() == 32 && q.down() == 33);
	const std::int64_t rtt{lagtest_time(60)};
	q.set_round_trip(rtt);
	CHECK(q.up() + q.down() == rtt && q.up() <= q.down());
	/* Ordering and due times: A and B touched at 0 (in that order), C
	 * at 10.  Nothing is due before half the round trip.
	 */
	CHECK(q.request(0, 0x8001, 5, 1, 11));
	CHECK(q.request(0, 0x8002, 6, 2, 12));
	CHECK(q.request(10, 0x8003, 7, 3, 13));
	CHECK(q.size() == 3);
	CHECK(q.holds(1, 11) && q.holds(3, 13) && !q.holds(1, 12) && !q.holds(4, 11));
	CHECK(!q.next_due(q.up() - 1));
	/* A clock that stands still (a paused game) makes nothing due. */
	CHECK(!q.next_due(q.up() - 1));
	const auto a{q.next_due(q.up())};
	CHECK(a && a->what == stage::request && a->netid == 0x8001 && a->powerup_id == 5);
	CHECK(!q.holds(1, 11));
	const auto b{q.next_due(q.up())};
	CHECK(b && b->netid == 0x8002);
	CHECK(!q.next_due(q.up()));
	/* A is granted and B denied when they arrive: the answers are due
	 * the other half later, one full round trip after the touch.
	 */
	const auto rules{make_rules()};
	CHECK(q.grant(q.up(), *a, MISSILE_4, {true, true, 4, 0}, 0));
	CHECK(q.deny(q.up(), *b));
	CHECK(q.holds(1, 11) && q.holds(2, 12));
	const auto c{q.next_due(10 + q.up())};
	CHECK(c && c->netid == 0x8003 && c->objnum == 3);
	CHECK(q.deny(10 + q.up(), *c));
	CHECK(!q.next_due(rtt - 1));
	const auto ga{q.next_due(rtt + 100)};
	CHECK(ga && ga->what == stage::grant && ga->netid == 0x8001 && ga->due == rtt && ga->outcome.taken == 4 && ga->life == 0);
	const auto db{q.next_due(rtt + 100)};
	CHECK(db && db->what == stage::deny && db->netid == 0x8002 && db->due == rtt);
	const auto dc{q.next_due(rtt + 100)};
	CHECK(dc && dc->what == stage::deny && dc->due == 10 + rtt);
	CHECK(!q.next_due(rtt + 100) && q.size() == 0);
	/* A full queue refuses a touch (the powerup is not asked for). */
	for (std::uint16_t i = 0; i < 4; ++i)
		CHECK(q.request(0, static_cast<netid_t>(0x8010 + i), 1, i, 1));
	CHECK(!q.request(0, 0x8020, 1, 9, 1));
	q.reset();
	CHECK(q.size() == 0 && !q.next_due(1 << 30));

	/* The decision counts the grants still on their way (as the host's
	 * copy of a client does): two packs of 4 missiles within one round
	 * trip with room for 2 give 2, then nothing.
	 */
	auto ship{spawn_inventory()};
	ship.secondary[0] = 18;
	own_life life;
	CHECK(q.request(0, 0x8001, 1, 1, 1));
	CHECK(q.request(0, 0x8002, 1, 2, 1));
	for (int i = 0; i < 2; ++i)
	{
		const auto r{q.next_due(q.up())};
		CHECK(r);
		const auto inv{q.with_grants(ship, rules, life.life, true)};
		const auto o{evaluate_pickup(inv, rules, MISSILE_4, 0)};
		if (i == 0)
		{
			CHECK(o.usable && o.taken == 2);
			CHECK(q.grant(q.up(), *r, MISSILE_4, o, life.life));
		}
		else
		{
			CHECK(!o.usable);
			CHECK(q.deny(q.up(), *r));
		}
	}
	/* Shields count for the decision, not for a dead ship's inventory. */
	CHECK(q.request(q.up() - 1, 0x8003, 2, 3, 1));
	const auto s{q.next_due(2 * q.up() - 1)};
	CHECK(s && s->netid == 0x8003);
	CHECK(q.grant(2 * q.up() - 1, *s, SHIELD, {true, true, 18 * F1, 0}, life.life));
	CHECK(q.with_grants(ship, rules, life.life, true).secondary[0] == 20);
	CHECK(q.with_grants(ship, rules, life.life, true).shields == ship.shields + 18 * F1);
	CHECK(q.with_grants(ship, rules, life.life, false).shields == ship.shields);
	CHECK(q.with_grants(ship, rules, life.life, false).secondary[0] == 20);
	/* A grant for another life is not counted. */
	CHECK(q.with_grants(ship, rules, static_cast<std::uint8_t>(life.life + 1), true) == ship);

	/* The ship dies with the missile grant on its way: at the deres it
	 * goes into the ship (so the drop has it, as the host's copy of a
	 * client would), the life ends, and the grant that arrives after
	 * that is not applied again (the client's late grant rule).
	 */
	const auto folded{q.with_grants(ship, rules, life.life, false)};
	CHECK(folded.secondary[0] == 20 && folded.shields == ship.shields);
	ship = folded;
	life.on_deres();
	/* The next life's decisions do not count the old life's grants. */
	CHECK(q.with_grants(spawn_inventory(), rules, life.life, true) == spawn_inventory());
	unsigned applied{0}, ignored{0}, denied{0};
	while (const auto e{q.next_due(1 << 30)})
	{
		if (e->what == stage::deny)
		{
			++denied;
			continue;
		}
		if (life.current(e->life))
			++applied;
		else
			++ignored;
	}
	CHECK(applied == 0 && ignored == 2 && denied == 1);
	CHECK(ship.secondary[0] == 20);
	/* A grant decided in the new life applies. */
	life.on_reappear();
	CHECK(q.request(0, 0x8004, 1, 4, 1));
	const auto n{q.next_due(q.up())};
	CHECK(n && q.grant(q.up(), *n, MISSILE_1, {true, true, 1, 0}, life.life));
	const auto ng{q.next_due(rtt)};
	CHECK(ng && life.current(ng->life));
}
}

namespace {

/* Bots (Documentation/multiplayer-bots.md section 9.2): the host flies
 * them, so a bot's ship is the truth for its inventory (the host's copy
 * is assigned from it, no grant is ever on its way to it) and its touch
 * is decided at once, like the host's own.  A small host world with the
 * pure components as net_objects.cpp and multi.cpp use them: the ground
 * (net ids), the copies, the respawn rule and the accounting helpers of
 * the host's log.  Slot 0 is the host, 1 a client, 2 and 3 bots.
 */
constexpr pickup_desc SHAKER{pickup_kind::secondary, 9, 1, 0};

struct bot_world
{
	static constexpr unsigned PLAYERS{4};
	struct ground_object
	{
		pickup_desc desc{};
		std::uint32_t count{1};
	};
	std::unique_ptr<table_type> table{std::make_unique<table_type>()};
	std::map<netid_t, ground_object> ground;
	std::array<inventory_mirror, PLAYERS> mirrors;
	std::array<bool, PLAYERS> dropped{};
	/* The host's and the bots' ships; the client's own inventory. */
	std::array<inventory, PLAYERS> ships;
	std::array<bool, PLAYERS> is_bot{{false, false, true, true}};
	std::uint32_t initial{};
	unsigned grants{}, denies{}, drops{};
	std::uint16_t next_objnum{};
	bot_world()
	{
		for (unsigned i = 0; i < PLAYERS; ++i)
		{
			ships[i] = spawn_inventory();
			mirrors[i].reset(spawn_inventory());
		}
	}
	netid_t create(const pickup_desc &d, const bool level = false)
	{
		const std::uint16_t objnum{next_objnum++};
		const netid_t id{level ? level_netid(objnum) : table->allocate(0)};
		table->bind(id, objnum, 0);
		ground[id] = {d, 1};
		return id;
	}
	/* The respawn bookkeeping's count: the ground and what every live
	 * player carries in the host's view (bots' and host's ships, the
	 * clients' copies with their grants in flight).
	 */
	[[nodiscard]]
	std::uint32_t counted(const pickup_desc &d) const
	{
		std::uint32_t n{};
		for (const auto &[id, o] : ground)
			if (same_item(o.desc, d))
				n += units_on_ground(o.desc);
		for (unsigned i = 0; i < PLAYERS; ++i)
			if (!dropped[i])
				n += units_carried(i == 1 ? mirrors[i].current() : ships[i], make_rules(), d);
		return n;
	}
	[[nodiscard]]
	bool respawn_due(const pickup_desc &d) const
	{
		return respawn_allowed(initial, counted(d), 1);
	}
	[[nodiscard]]
	pickup_object_view view(const netid_t id) const
	{
		pickup_object_view o{};
		if (const auto it{ground.find(id)}; it != ground.end() && table->bound(id))
		{
			o.exists = true;
			o.desc = it->second.desc;
			o.count = it->second.count;
		}
		return o;
	}
	bool decide(const unsigned pid, const netid_t id)
	{
		const auto d{decide_pickup(view(id), {static_cast<std::uint8_t>(pid), !dropped[pid], true}, mirrors[pid].current(), make_rules(), 0)};
		if (!d.grant)
		{
			++denies;
			return false;
		}
		++grants;
		mirrors[pid].on_grant(make_rules(), ground[id].desc, d.outcome);
		ground.erase(id);
		table->unbind(id);
		return true;
	}
	/* A bot's touch (net_objects_bot_touch): the copy from the ship, the
	 * decision, the grant into the copy and the ship; then the copy is
	 * the ship again (bot_touch_powerup's own-ship inventory).
	 */
	bool bot_touch(const unsigned pid, const netid_t id)
	{
		mirrors[pid].assign(ships[pid]);
		if (!decide(pid, id))
			return false;
		ships[pid] = mirrors[pid].current();
		mirrors[pid].assign(ships[pid]);
		return true;
	}
	/* A client's request, decided with the host's copy; the client
	 * applies the grant when it arrives.
	 */
	bool client_request(const unsigned pid, const netid_t id)
	{
		const auto d{view(id).desc};
		if (!decide(pid, id))
			return false;
		apply_pickup(ships[pid], make_rules(), d, evaluate_pickup(ships[pid], make_rules(), d, 1));
		return true;
	}
	/* A bot's death (explode: multi_send_player_deres brings the copy up
	 * to date, then net_objects_host_drop_player_eggs), once per life; a
	 * disconnect after it drops nothing more.
	 */
	unsigned bot_die(const unsigned pid)
	{
		if (!dropped[pid])
			mirrors[pid].assign(ships[pid]);
		if (dropped[pid])
			return 0;
		dropped[pid] = true;
		const unsigned n{mirrors[pid].current().secondary[SHAKER.index]};
		for (unsigned i = 0; i < n; ++i)
			create(SHAKER);
		drops += n;
		mirrors[pid].clear();
		ships[pid] = {};
		return n;
	}
};

void test_bot_accounting()
{
	/* The accounting helpers: a 4-pack is 4 missiles, a shaker 1. */
	CHECK(same_item(MISSILE_1, MISSILE_4) && !same_item(MISSILE_1, MEGA) && !same_item(PLASMA, VULCAN));
	CHECK(units_on_ground(MISSILE_4) == 4 && units_on_ground(SHAKER) == 1 && units_on_ground(PLASMA) == 1 && units_on_ground(KEY) == 0);
	{
		auto inv{spawn_inventory()};
		inv.secondary[SHAKER.index] = 2;
		CHECK(units_carried(inv, make_rules(), SHAKER) == 2 && units_carried(inv, make_rules(), MISSILE_4) == 2);
		CHECK(units_carried(inv, make_rules(), PLASMA) == 0);
	}
	/* A bot carries the level's only shaker: nothing is missing, no
	 * respawn; only when it fires it does the level miss one.
	 */
	{
		bot_world w;
		const auto shaker{w.create(SHAKER, true)};
		w.initial = w.counted(SHAKER);
		CHECK(w.initial == 1 && !w.respawn_due(SHAKER));
		CHECK(w.bot_touch(2, shaker));
		CHECK(w.ground.empty() && w.ships[2].secondary[SHAKER.index] == 1);
		CHECK(w.counted(SHAKER) == 1 && !w.respawn_due(SHAKER));
		/* The host's copy is the ship: nothing in flight. */
		CHECK(w.mirrors[2].pending() == 0 && w.mirrors[2].current() == w.ships[2]);
		--w.ships[2].secondary[SHAKER.index];
		CHECK(w.counted(SHAKER) == 0 && w.respawn_due(SHAKER));
	}
	/* The bot dies with it: it is dropped exactly once (a kick or a
	 * second explosion after the death drops nothing), the count stays,
	 * and a client that takes the drop carries the only one.
	 */
	{
		bot_world w;
		const auto shaker{w.create(SHAKER, true)};
		w.initial = w.counted(SHAKER);
		CHECK(w.bot_touch(3, shaker));
		CHECK(w.bot_die(3) == 1);
		CHECK(w.bot_die(3) == 0);
		CHECK(w.drops == 1 && w.ground.size() == 1);
		CHECK(w.counted(SHAKER) == 1 && !w.respawn_due(SHAKER));
		const auto dropped_id{w.ground.begin()->first};
		CHECK(!is_level_netid(dropped_id));
		/* A dead bot takes nothing. */
		CHECK(!w.bot_touch(3, dropped_id));
		CHECK(w.client_request(1, dropped_id));
		CHECK(w.ground.empty() && w.counted(SHAKER) == 1 && !w.respawn_due(SHAKER));
		CHECK(w.ships[1].secondary[SHAKER.index] == 1);
	}
	/* A bot and a client on the same shaker in the same frame, in both
	 * orders: one grant, the other finds it gone, one shaker in all.
	 */
	for (int client_first = 0; client_first < 2; ++client_first)
	{
		bot_world w;
		const auto shaker{w.create(SHAKER, true)};
		w.initial = w.counted(SHAKER);
		bool client_got, bot_got;
		if (client_first)
		{
			client_got = w.client_request(1, shaker);
			bot_got = w.bot_touch(2, shaker);
		}
		else
		{
			bot_got = w.bot_touch(2, shaker);
			client_got = w.client_request(1, shaker);
		}
		CHECK(client_got != bot_got && client_got == (client_first != 0));
		CHECK(w.grants == 1 && w.denies == 1);
		CHECK(w.ships[1].secondary[SHAKER.index] + w.ships[2].secondary[SHAKER.index] == 1);
		CHECK(w.counted(SHAKER) == 1 && !w.respawn_due(SHAKER));
		CHECK(!w.table->bound(shaker));
	}
}

/* The host's "suspicious inventory" check: a client's report that holds
 * more than the host's copy expects.  A grant it acknowledges, a report
 * from before a grant arrived, a drop and a new ship's first report are
 * not suspicious; a missile from nowhere is.
 */
void test_unexplained_gain()
{
	const auto r{make_rules()};
	inventory_mirror m;
	m.reset(spawn_inventory());
	CHECK(!m.has_report());
	m.on_report(r, spawn_inventory(), 0);
	CHECK(m.has_report());
	CHECK(!unexplained_gain(spawn_inventory(), spawn_inventory()));
	/* A grant on its way; a report sent before it arrived. */
	const auto seq{m.on_grant(r, SHAKER, {true, true, 1, 0})};
	auto expected{m.current()};
	CHECK(expected.secondary[SHAKER.index] == 1 && m.base().secondary[SHAKER.index] == 0);
	m.on_report(r, spawn_inventory(), static_cast<std::uint16_t>(seq - 1));
	CHECK(!unexplained_gain(expected, m.current()));
	/* The report that acknowledges it. */
	auto with{spawn_inventory()};
	with.secondary[SHAKER.index] = 1;
	expected = m.current();
	m.on_report(r, with, seq);
	CHECK(!unexplained_gain(expected, m.current()) && m.pending() == 0);
	/* A second shaker from nowhere. */
	auto dup{with};
	dup.secondary[SHAKER.index] = 2;
	expected = m.current();
	m.on_report(r, dup, seq);
	const auto g{unexplained_gain(expected, m.current())};
	CHECK(g && g->field == inventory_gain::what::secondary && g->index == SHAKER.index && g->expected == 1 && g->reported == 2);
	/* Fired, dropped, energy and shields: less is never suspicious. */
	auto less{spawn_inventory()};
	less.secondary[0] = 0;
	less.energy = 1;
	CHECK(!unexplained_gain(spawn_inventory(), less));
	/* A weapon, a laser level, an orb. */
	auto gained{spawn_inventory()};
	gained.primary_flags |= BIT_PLASMA;
	const auto p{unexplained_gain(spawn_inventory(), gained)};
	CHECK(p && p->field == inventory_gain::what::primary && p->index == 3);
	gained = spawn_inventory();
	gained.laser_level = 2;
	CHECK(unexplained_gain(spawn_inventory(), gained)->field == inventory_gain::what::laser);
	gained = spawn_inventory();
	gained.orbs = 1;
	CHECK(unexplained_gain(spawn_inventory(), gained)->field == inventory_gain::what::orbs);
	/* The death drop empties the copy: the new ship's first report is
	 * not compared.
	 */
	m.clear();
	CHECK(!m.has_report());
}

}

int main()
{
	test_netid_encoding();
	test_netid_table();
	test_netid_allocation_and_reuse();
	test_pickup_rules();
	test_drop_rules();
	test_inventory_mirror();
	test_decide_pickup();
	test_pending_pickups();
	test_respawn_bookkeeping();
	test_range();
	test_wire();
	test_simultaneous_requests();
	test_deny_and_restore();
	test_stale_and_duplicate_requests();
	test_cannon_ammunition();
	test_grant_after_death();
	test_rejoin_after_drop();
	test_random_game();
	test_joiner_snapshot();
	test_lag_pickups();
	test_bot_accounting();
	test_unexplained_gain();
	std::puts("all tests passed");
	return 0;
}
