/*
 * This file is part of the DXX-Rebirth project <https://github.com/dxx-rebirth/dxx-rebirth/>.
 * It is copyright by its individual contributors, as recorded in the
 * project's Git history.  See COPYING.txt at the top level for license
 * terms and a link to the Git history.
 */
/*
 * Test of the game-independent part of stage 4 of the v2 protocol
 * (net_v2_combat.h): the host's position history and the rewind within
 * its window, the verdicts on hit reports, the damage rules (death,
 * invulnerability, cloak, friendly fire), kill credit and scores, the
 * rate limits, the checks of a client's reported state and the
 * correction, the catch-up of a late shot on synthetic geometry, the
 * host's copy of a client's shields under damage in flight, the wire
 * layouts; and a model of a host and its clients, with latency, jitter
 * and loss, in which every machine fires at the ships it is shown,
 * reports its hits, and must end with the same damage, kills and scores
 * as every other machine.
 *
 * Build and run with SCons:
 *
 *	scons sdl2=1 d1x=0 d2x=1 register_runtime_test_plain_link_targets=1 test-net-v2-combat
 *	build/common/test-net-v2-combat
 *
 * or directly:
 *
 *	g++ -std=gnu++23 -O2 -Wall -Wextra -Icommon/main common/unittest/net_v2_combat.cpp -o test-net-v2-combat
 */

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <random>
#include <vector>

#include "net_v2_combat.h"
#include "net_interp.h"

using namespace dcx::net_v2;

namespace {

void check_failed(const char *const expr, const char *const file, const int line)
{
	std::fprintf(stderr, "%s:%d: check failed: %s\n", file, line, expr);
	std::exit(1);
}

#define CHECK(cond)	do { if (!(cond)) check_failed(#cond, __FILE__, __LINE__); } while (0)

constexpr std::int32_t F1{65536};
constexpr net_clock TICK{net_seconds(1) / 60};

[[nodiscard]]
net_vec vec(const double x, const double y, const double z)
{
	return {static_cast<std::int32_t>(std::lround(x * F1)), static_cast<std::int32_t>(std::lround(y * F1)), static_cast<std::int32_t>(std::lround(z * F1))};
}

void test_history()
{
	position_history<> h;
	CHECK(h.empty());
	CHECK(!h.rewind(100));
	/* A straight path, 1 unit per tick along x. */
	for (int i = 0; i < 10; ++i)
		CHECK(h.push(1000 + i * TICK, vec(i, 0, 0), static_cast<std::uint16_t>(i / 5)));
	CHECK(h.size() == 10);
	/* The ring stays ordered. */
	CHECK(!h.push(1000 + 9 * TICK, vec(99, 0, 0), 0));
	CHECK(!h.push(1000, vec(99, 0, 0), 0));
	CHECK(h.size() == 10);
	/* Exactly on an entry, and between two. */
	{
		const auto r{h.rewind(1000 + 3 * TICK)};
		CHECK(r && r->quality == rewind_quality::interpolated && r->pos == vec(3, 0, 0) && r->beyond == 0);
	}
	{
		const auto r{h.rewind(1000 + 3 * TICK + TICK / 2)};
		CHECK(r && r->quality == rewind_quality::interpolated);
		CHECK(std::abs(r->pos.x - (3 * F1 + F1 / 2)) <= 1);
		CHECK(r->segment == 0);
	}
	/* The segment of the nearer entry. */
	CHECK(h.rewind(1000 + 4 * TICK + TICK / 4)->segment == 0);
	CHECK(h.rewind(1000 + 4 * TICK + 3 * TICK / 4)->segment == 1);
	/* Linear on every point of the path. */
	for (net_clock t = 1000; t <= 1000 + 9 * TICK; t += 37)
	{
		const auto r{h.rewind(t)};
		const double expect{static_cast<double>(t - 1000) / TICK};
		CHECK(r && std::abs(r->pos.x / 65536.0 - expect) < 0.001);
	}
	/* Older than the ring: the oldest, marked stale. */
	{
		const auto r{h.rewind(10)};
		CHECK(r && r->quality == rewind_quality::stale && r->pos == vec(0, 0, 0));
	}
	/* Newer than the ring: the newest, and for how long it is unknown. */
	{
		const auto r{h.rewind(1000 + 9 * TICK + 500)};
		CHECK(r && r->quality == rewind_quality::newest && r->pos == vec(9, 0, 0) && r->beyond == 500);
	}
	/* A full ring drops its oldest. */
	for (int i = 10; i < 100; ++i)
		CHECK(h.push(1000 + i * TICK, vec(i, 0, 0), 0));
	CHECK(h.size() == NET_V2_HISTORY);
	CHECK(h.oldest().pos == vec(100 - static_cast<int>(NET_V2_HISTORY), 0, 0));
	CHECK(h.newest().pos == vec(99, 0, 0));
	CHECK(h.rewind(1000 + 50 * TICK)->pos == vec(50, 0, 0));
	/* 64 entries at 60 Hz cover over a second. */
	CHECK(h.newest().time - h.oldest().time > net_seconds(1));
	/* A jump (a respawn) is not a path: the earlier place until the
	 * later entry's time.
	 */
	CHECK(h.push(1000 + 100 * TICK, vec(500, 0, 0), 7));
	{
		const auto r{h.rewind(1000 + 99 * TICK + TICK / 2)};
		CHECK(r && r->quality == rewind_quality::snapped && r->pos == vec(99, 0, 0));
		CHECK(h.rewind(1000 + 100 * TICK)->pos == vec(500, 0, 0));
	}
	h.clear();
	CHECK(h.empty() && !h.rewind(1000));
}

void test_rewind_window()
{
	const net_clock now{net_seconds(100)};
	const auto window{rewind_window(0)};
	CHECK(window == NET_V2_REWIND_DEFAULT && window == net_milliseconds(200));
	CHECK(rewind_window(120) == net_milliseconds(120));
	CHECK(rewind_window(5000) == NET_V2_REWIND_LIMIT);
	/* Within the window: the shooter's view. */
	{
		const auto r{limit_rewind(now, now - net_milliseconds(150), window)};
		CHECK(r.time == now - net_milliseconds(150) && !r.limited);
	}
	{
		const auto r{limit_rewind(now, now - window, window)};
		CHECK(r.time == now - window && !r.limited);
	}
	/* Older: the window's edge. */
	{
		const auto r{limit_rewind(now, now - net_milliseconds(350), window)};
		CHECK(r.time == now - window && r.limited);
	}
	/* The future is now. */
	{
		const auto r{limit_rewind(now, now + net_milliseconds(30), window)};
		CHECK(r.time == now && !r.limited);
	}
}

[[nodiscard]]
shot_record make_shot(const netid_t id, const std::uint8_t owner, const net_clock fire_time)
{
	shot_record s;
	s.id = id;
	s.owner = owner;
	s.weapon_id = 13;
	s.child_weapon_id = 13;
	s.fire_time = fire_time;
	s.origin = vec(0, 0, 0);
	s.max_speed = 120 * NET_V2_UNIT;
	s.lifetime = net_seconds(5);
	return s;
}

void test_hit_verdicts()
{
	const net_clock t0{net_seconds(50)};
	const std::int64_t size{4 * NET_V2_UNIT + NET_V2_UNIT / 2};
	shot_registry reg;
	const auto id{dynamic_netid(1, 7)};
	reg.add(make_shot(id, 1, t0));
	CHECK(reg.size() == 1);
	/* The target stands 60 units down the x axis; the shot needs half a
	 * second to get there.
	 */
	const std::optional<rewound> target{rewound{vec(60, 0, 0), 3, rewind_quality::interpolated, 0}};
	const auto now{t0 + net_milliseconds(520)};
	const auto seen{now - net_milliseconds(90)};
	const auto judge{[&](const netid_t i, const std::uint8_t from, const std::uint8_t weapon, const std::uint8_t tgt, const net_clock at, const net_vec &p, const std::optional<rewound> &where = std::nullopt) {
		return judge_direct_hit(reg.find(i), from, weapon, tgt, at, seen, p, where, size, 0);
	}};
	const auto surface{vec(55.6, 0, 0)};
	CHECK(judge(id, 1, 13, 2, now, surface, target) == hit_verdict::accept);
	CHECK(judge(dynamic_netid(1, 8), 1, 13, 2, now, surface, target) == hit_verdict::unknown_shot);
	CHECK(judge(id, 3, 13, 2, now, surface, target) == hit_verdict::not_owner);
	CHECK(judge(id, 1, 14, 2, now, surface, target) == hit_verdict::wrong_weapon);
	CHECK(judge(id, 1, 13, 9, now, surface, target) == hit_verdict::no_target);
	CHECK(judge(id, 1, 13, 2, t0 + net_seconds(6), surface, target) == hit_verdict::expired);
	CHECK(judge(id, 1, 13, 2, now, surface) == hit_verdict::no_target);
	/* The target was seen before the shot existed, by more than any
	 * display delay.
	 */
	CHECK(judge_direct_hit(reg.find(id), 1, 13, 2, now, t0 - net_milliseconds(600), surface, target, size, 0) == hit_verdict::too_early);
	CHECK(judge_direct_hit(reg.find(id), 1, 13, 2, now, t0 - net_milliseconds(100), surface, target, size, 0) == hit_verdict::accept);
	/* Further than the shot can have flown: 100 ms after firing it is at
	 * most 12 units out, plus the slack.
	 */
	CHECK(judge(id, 1, 13, 2, t0 + net_milliseconds(100), surface, target) == hit_verdict::out_of_reach);
	CHECK(judge(id, 1, 13, 2, t0 + net_milliseconds(100), vec(21, 0, 0), rewound{vec(24, 0, 0), 0, rewind_quality::interpolated, 0}) == hit_verdict::accept);
	/* The point must be on the target where the host has it: within its
	 * size and the tolerance.
	 */
	CHECK(judge(id, 1, 13, 2, now, vec(53.6, 0, 0), target) == hit_verdict::accept);
	CHECK(judge(id, 1, 13, 2, now, vec(53.4, 0, 0), target) == hit_verdict::missed);
	CHECK(judge(id, 1, 13, 2, now, vec(60, 6.4, 0), target) == hit_verdict::accept);
	CHECK(judge(id, 1, 13, 2, now, vec(60, 6.6, 0), target) == hit_verdict::missed);
	/* The projectile's own size is slack. */
	CHECK(judge_direct_hit(reg.find(id), 1, 13, 2, now, seen, vec(60, 8.4, 0), target, size, 2 * NET_V2_UNIT) == hit_verdict::accept);
	/* A target whose newest position is older than the time asked for
	 * may have moved on: at most 100 ms at ship speed.
	 */
	CHECK(judge(id, 1, 13, 2, now, vec(60, 12, 0), rewound{vec(60, 0, 0), 0, rewind_quality::newest, net_milliseconds(40)}) == hit_verdict::accept);
	CHECK(judge(id, 1, 13, 2, now, vec(60, 13, 0), rewound{vec(60, 0, 0), 0, rewind_quality::newest, net_milliseconds(40)}) == hit_verdict::missed);
	CHECK(judge(id, 1, 13, 2, now, vec(60, 25, 0), rewound{vec(60, 0, 0), 0, rewind_quality::newest, net_seconds(3)}) == hit_verdict::missed);
	/* One hit: then the shot is used up. */
	consume_shot(*reg.find(id), 2);
	CHECK(judge(id, 1, 13, 2, now, surface, target) == hit_verdict::consumed);
	CHECK(judge(id, 1, 13, 4, now, surface, target) == hit_verdict::consumed);
	/* A persistent shot (fusion) hits each target once. */
	const auto fusion{dynamic_netid(1, 9)};
	{
		auto s{make_shot(fusion, 1, t0)};
		s.total_limit = NET_V2_MAX_PLAYERS;
		reg.add(s);
	}
	CHECK(judge(fusion, 1, 13, 2, now, surface, target) == hit_verdict::accept);
	consume_shot(*reg.find(fusion), 2);
	CHECK(judge(fusion, 1, 13, 2, now, surface, target) == hit_verdict::consumed);
	CHECK(judge(fusion, 1, 13, 4, now, surface, target) == hit_verdict::accept);
	/* A shot with children (a smart missile): the child's weapon is
	 * taken under the parent's id, as often as there are children.
	 */
	const auto smart{dynamic_netid(1, 10)};
	{
		auto s{make_shot(smart, 1, t0)};
		s.weapon_id = 17;
		s.child_weapon_id = 19;
		s.per_target_limit = 3;
		s.total_limit = 24;
		reg.add(s);
	}
	CHECK(judge(smart, 1, 17, 2, now, surface, target) == hit_verdict::accept);
	CHECK(judge(smart, 1, 19, 2, now, surface, target) == hit_verdict::accept);
	CHECK(judge(smart, 1, 13, 2, now, surface, target) == hit_verdict::wrong_weapon);
	for (int i = 0; i < 3; ++i)
	{
		CHECK(judge(smart, 1, 19, 2, now, surface, target) == hit_verdict::accept);
		consume_shot(*reg.find(smart), 2);
	}
	CHECK(judge(smart, 1, 19, 2, now, surface, target) == hit_verdict::consumed);
	/* A reused id replaces the record. */
	reg.add(make_shot(id, 1, now));
	CHECK(judge(id, 1, 13, 2, now + net_milliseconds(600), surface, target) == hit_verdict::accept);
	/* Shots whose time is over are forgotten. */
	reg.expire(now + net_seconds(3));
	CHECK(reg.size() == 3);
	reg.expire(now + net_seconds(30));
	CHECK(reg.size() == 0);
	CHECK(std::string_view{hit_verdict_name(hit_verdict::missed)} == "missed");
	CHECK(std::string_view{hit_verdict_name(hit_verdict::no_damage)} == "no damage");
	/* Ids of one shot's projectiles: consecutive within the creator. */
	CHECK(netid_advance(dynamic_netid(3, 4094), 0) == dynamic_netid(3, 4094));
	CHECK(netid_advance(dynamic_netid(3, 4094), 1) == dynamic_netid(3, 4095));
	CHECK(netid_advance(dynamic_netid(3, 4094), 2) == dynamic_netid(3, 0));
	CHECK(netid_creator(netid_advance(dynamic_netid(3, 4094), 5)) == 3);
}

void test_splash()
{
	const std::int32_t max_damage{100 * F1};
	const std::int64_t radius{40 * NET_V2_UNIT};
	CHECK(splash_amount(max_damage, radius, 0) == max_damage);
	CHECK(splash_amount(max_damage, radius, radius / 2) == max_damage / 2);
	CHECK(splash_amount(max_damage, radius, radius / 4) == max_damage / 4 * 3);
	CHECK(splash_amount(max_damage, radius, radius) == 0);
	CHECK(splash_amount(max_damage, radius, radius * 2) == 0);
	CHECK(splash_amount(max_damage, 0, 0) == 0);
	const net_clock t0{net_seconds(50)};
	shot_registry reg;
	const auto id{dynamic_netid(2, 1)};
	{
		auto s{make_shot(id, 2, t0)};
		s.total_limit = NET_V2_MAX_PLAYERS;
		reg.add(s);
	}
	const auto now{t0 + net_seconds(1)};
	const auto centre{vec(50, 0, 0)};
	/* The host's damage is from where it has the target, whatever the
	 * shooter saw.
	 */
	{
		const auto v{judge_splash_hit(reg.find(id), 2, 13, 0, now, now, centre, rewound{vec(50, 10, 0), 0, rewind_quality::interpolated, 0}, max_damage, radius)};
		CHECK(v.verdict == hit_verdict::accept && std::abs(v.damage - 75 * F1) < 8);
	}
	{
		const auto v{judge_splash_hit(reg.find(id), 2, 13, 0, now, now, centre, rewound{vec(50, 45, 0), 0, rewind_quality::interpolated, 0}, max_damage, radius)};
		CHECK(v.verdict == hit_verdict::missed && v.damage == 0);
	}
	CHECK(judge_splash_hit(reg.find(id), 5, 13, 0, now, now, centre, rewound{vec(50, 10, 0), 0, rewind_quality::interpolated, 0}, max_damage, radius).verdict == hit_verdict::not_owner);
	CHECK(judge_splash_hit(nullptr, 2, 13, 0, now, now, centre, rewound{}, max_damage, radius).verdict == hit_verdict::unknown_shot);
	CHECK(judge_splash_hit(reg.find(id), 2, 13, 0, now, now, centre, std::nullopt, max_damage, radius).verdict == hit_verdict::no_target);
	/* The shooter's claim counts for a direct hit, up to what the weapon
	 * can do.
	 */
	CHECK(clamp_claim(7 * F1, 10 * F1) == 7 * F1);
	CHECK(clamp_claim(70 * F1, 10 * F1) == 10 * F1);
	CHECK(clamp_claim(-5, 10 * F1) == 0);
	CHECK(clamp_claim(5, -10) == 0);
}

void test_damage_rules()
{
	const victim_view alive{.playing = true, .alive = true, .invulnerable = false, .cloaked = false, .team = 0};
	const damage_context anarchy{};
	const std::int32_t d{5 * F1};
	CHECK(evaluate_damage(alive, attacker_kind::player, 1, anarchy, false, d) == damage_verdict::apply);
	CHECK(evaluate_damage(alive, attacker_kind::robot, 0, anarchy, false, d) == damage_verdict::apply);
	CHECK(evaluate_damage(alive, attacker_kind::none, 0, anarchy, true, d) == damage_verdict::apply);
	CHECK(evaluate_damage(alive, attacker_kind::player, 1, anarchy, false, 0) == damage_verdict::nothing);
	/* Dead, dying, not in the game. */
	{
		auto v{alive};
		v.alive = false;
		CHECK(evaluate_damage(v, attacker_kind::player, 1, anarchy, false, d) == damage_verdict::dead);
		v = alive;
		v.playing = false;
		CHECK(evaluate_damage(v, attacker_kind::player, 1, anarchy, true, d) == damage_verdict::not_playing);
	}
	/* Invulnerable: nothing, not even what is never friendly. */
	{
		auto v{alive};
		v.invulnerable = true;
		CHECK(evaluate_damage(v, attacker_kind::player, 1, anarchy, false, d) == damage_verdict::invulnerable);
		CHECK(evaluate_damage(v, attacker_kind::none, 0, anarchy, true, d) == damage_verdict::invulnerable);
	}
	/* A cloak hides; it does not protect. */
	{
		auto v{alive};
		v.cloaked = true;
		CHECK(evaluate_damage(v, attacker_kind::player, 1, anarchy, false, d) == damage_verdict::apply);
	}
	/* Friendly fire.  In anarchy the setting means nothing. */
	{
		damage_context c{.no_friendly_fire = true};
		CHECK(evaluate_damage(alive, attacker_kind::player, 0, c, false, d) == damage_verdict::apply);
	}
	/* A team game with friendly fire on: a team mate hurts. */
	{
		const damage_context c{.team_game = true};
		CHECK(evaluate_damage(alive, attacker_kind::player, 0, c, false, d) == damage_verdict::apply);
		CHECK(evaluate_damage(alive, attacker_kind::player, 1, c, false, d) == damage_verdict::apply);
	}
	/* Friendly fire off: a team mate's weapon (the victim's own
	 * included) does nothing, the other team's does; a bump or a wall
	 * always does; so does a robot.
	 */
	{
		const damage_context c{.team_game = true, .no_friendly_fire = true};
		CHECK(evaluate_damage(alive, attacker_kind::player, 0, c, false, d) == damage_verdict::friendly);
		CHECK(evaluate_damage(alive, attacker_kind::player, 1, c, false, d) == damage_verdict::apply);
		CHECK(evaluate_damage(alive, attacker_kind::player, 0, c, true, d) == damage_verdict::apply);
		CHECK(evaluate_damage(alive, attacker_kind::robot, 0, c, false, d) == damage_verdict::apply);
		CHECK(evaluate_damage(alive, attacker_kind::reactor, 0, c, false, d) == damage_verdict::apply);
	}
	/* Cooperative: no player's weapon hurts a player. */
	{
		const damage_context c{.coop = true, .no_friendly_fire = true};
		CHECK(evaluate_damage(alive, attacker_kind::player, 1, c, false, d) == damage_verdict::friendly);
		CHECK(evaluate_damage(alive, attacker_kind::robot, 1, c, false, d) == damage_verdict::apply);
	}
	{
		const damage_context c{.endlevel = true};
		CHECK(evaluate_damage(alive, attacker_kind::player, 1, c, false, d) == damage_verdict::endlevel);
	}
}

void test_kills()
{
	/* Credit. */
	CHECK(kill_credit(attacker_kind::player, 3, true).kind == attacker_kind::player);
	CHECK(kill_credit(attacker_kind::player, 3, true).pid == 3);
	CHECK(kill_credit(attacker_kind::player, 3, false).kind == attacker_kind::none);
	CHECK(kill_credit(attacker_kind::player, 9, true).kind == attacker_kind::none);
	CHECK(kill_credit(attacker_kind::reactor, 3, true).kind == attacker_kind::reactor);
	CHECK(kill_credit(attacker_kind::reactor, 3, true).pid == NET_V2_PLAYER_ID_NONE);
	CHECK(kill_credit(attacker_kind::none, 3, true).kind == attacker_kind::none);
	const kill_attribution p1{attacker_kind::player, 1};
	const kill_attribution p2{attacker_kind::player, 2};
	/* Anarchy. */
	{
		score_board b;
		const auto o{apply_kill(b, {}, 2, p1, 0)};
		CHECK(o.killer_adjust == 1 && o.counts_for_goal);
		CHECK(b.kill_matrix[1][2] == 1 && b.kills[1] == 1 && b.goal[1] == 1 && b.deaths[2] == 1 && b.kills[2] == 0);
		/* Suicide. */
		const auto s{apply_kill(b, {}, 2, p2, 0)};
		CHECK(s.killer_adjust == -1 && !s.counts_for_goal);
		CHECK(b.kill_matrix[2][2] == 1 && b.kills[2] == -1 && b.goal[2] == -1 && b.deaths[2] == 2);
		/* The reactor: a death and a kill less. */
		apply_kill(b, {}, 1, {attacker_kind::reactor, NET_V2_PLAYER_ID_NONE}, 0);
		CHECK(b.kills[1] == 0 && b.deaths[1] == 1 && b.goal[1] == 0);
		/* A robot or its mine: a death. */
		apply_kill(b, {}, 1, {attacker_kind::robot, NET_V2_PLAYER_ID_NONE}, 0);
		apply_kill(b, {}, 1, {attacker_kind::mine, NET_V2_PLAYER_ID_NONE}, 0);
		CHECK(b.kills[1] == 0 && b.deaths[1] == 3);
		/* Nobody: nothing is counted. */
		const auto before{b};
		apply_kill(b, {}, 1, {}, 0);
		apply_kill(b, {}, 9, p1, 0);
		apply_kill(b, {}, 1, {attacker_kind::player, 9}, 0);
		CHECK(b == before);
	}
	/* Teams: players 0 and 2 red (bit clear), 1 and 3 blue. */
	{
		const std::uint8_t tv{0b1010};
		CHECK(team_of(tv, 0) == 0 && team_of(tv, 1) == 1 && team_of(tv, 3) == 1);
		score_board b;
		const kill_mode team{.team = true};
		apply_kill(b, team, 2, p1, tv);
		CHECK(b.team_kills[1] == 1 && b.team_kills[0] == 0 && b.kills[1] == 1 && b.deaths[2] == 1);
		/* A team mate killed: one off. */
		const auto o{apply_kill(b, team, 3, p1, tv)};
		CHECK(o.killer_adjust == -1);
		CHECK(b.team_kills[1] == 0 && b.kills[1] == 0 && b.goal[1] == 0 && b.deaths[3] == 1 && b.kill_matrix[1][3] == 1);
		/* Suicide costs the team. */
		apply_kill(b, team, 2, p2, tv);
		CHECK(b.team_kills[0] == -1 && b.kills[2] == -1);
	}
	/* Hoard: kills are not counted (orbs are), deaths and the matrix are. */
	{
		score_board b;
		const kill_mode hoard{.hoard = true};
		const auto o{apply_kill(b, hoard, 2, p1, 0)};
		CHECK(o.killer_adjust == 0 && b.kills[1] == 0 && b.deaths[2] == 1 && b.kill_matrix[1][2] == 1);
		apply_kill(b, hoard, 2, p2, 0);
		CHECK(b.kills[2] == 0 && b.deaths[2] == 1 && b.kill_matrix[2][2] == 1);
	}
	/* Bounty: only kills of or by the target count; the target's killer
	 * is the new target; a target that kills itself is redrawn.
	 */
	{
		score_board b;
		b.bounty_target = 2;
		const kill_mode bounty{.bounty = true};
		auto o{apply_kill(b, bounty, 3, p1, 0)};
		CHECK(o.killer_adjust == 0 && !o.bounty_to_killer && b.kills[1] == 0 && b.deaths[3] == 1);
		o = apply_kill(b, bounty, 3, p2, 0);
		CHECK(o.killer_adjust == 1 && !o.bounty_to_killer && b.kills[2] == 1 && b.bounty_target == 2);
		o = apply_kill(b, bounty, 2, p1, 0);
		CHECK(o.killer_adjust == 1 && o.bounty_to_killer && b.kills[1] == 1 && b.bounty_target == 1);
		o = apply_kill(b, bounty, 1, p1, 0);
		CHECK(o.bounty_redraw && b.kills[1] == 0);
		o = apply_kill(b, bounty, 3, {attacker_kind::player, 3}, 0);
		CHECK(!o.bounty_redraw);
	}
}

void test_rates()
{
	/* Section 3.6: 64 per second, a burst of 128. */
	{
		token_bucket b{NET_V2_COMBAT_RATE, NET_V2_COMBAT_BURST};
		net_clock now{net_seconds(10)};
		b.reset(now);
		unsigned taken{0};
		for (unsigned i = 0; i < 1000; ++i)
			taken += b.take(now);
		CHECK(taken == NET_V2_COMBAT_BURST);
		/* One second later, 64 more. */
		now += net_seconds(1);
		taken = 0;
		for (unsigned i = 0; i < 1000; ++i)
			taken += b.take(now);
		CHECK(taken == NET_V2_COMBAT_RATE);
		/* A legitimate client (25 per second) is never refused. */
		b.reset(now);
		for (unsigned i = 0; i < 25 * 60; ++i)
		{
			now += net_seconds(1) / 25;
			CHECK(b.take(now));
		}
		/* A flood of 1000 per second gets 64 per second. */
		b.reset(now);
		taken = 0;
		for (unsigned i = 0; i < 10000; ++i)
		{
			now += net_seconds(1) / 1000;
			taken += b.take(now);
		}
		CHECK(taken >= NET_V2_COMBAT_BURST + 630 && taken <= NET_V2_COMBAT_BURST + 650);
	}
	/* A weapon at its own rate, at any frame rate: never refused. */
	const net_clock wait{net_seconds(1) / 20};
	for (const unsigned fps : {10u, 30u, 60u, 500u})
	{
		fire_limiter f;
		const net_clock frame{net_seconds(1) / fps};
		net_clock next{net_seconds(5)};
		unsigned shots{0};
		for (net_clock now = net_seconds(5); now < net_seconds(15); now += frame)
			/* As the game fires: every shot due by this frame, all with
			 * the frame's time.
			 */
			while (next <= now)
			{
				CHECK(f.allow(now, wait));
				next += wait;
				++shots;
			}
		CHECK(shots >= 199 && shots <= 201);
	}
	/* Twice the rate: about the rate divided by 0.8 gets through. */
	{
		fire_limiter f;
		unsigned allowed{0};
		for (unsigned i = 0; i < 400; ++i)
			allowed += f.allow(net_seconds(5) + i * (wait / 2), wait);
		CHECK(allowed >= 250 && allowed <= 260);
	}
	/* A pause refills the slack, no more. */
	{
		fire_limiter f;
		unsigned allowed{0};
		for (unsigned i = 0; i < 100; ++i)
			allowed += f.allow(net_seconds(5), wait);
		CHECK(allowed == 7);
		allowed = 0;
		for (unsigned i = 0; i < 100; ++i)
			allowed += f.allow(net_seconds(60), wait);
		CHECK(allowed == 7);
		f.reset();
		CHECK(f.allow(net_seconds(1), wait));
	}
	/* The checks of FIRE. */
	{
		const net_clock now{net_seconds(30)};
		const std::optional<rewound> at{rewound{vec(10, 0, 0), 0, rewind_quality::interpolated, 0}};
		CHECK(judge_fire(now, now - net_milliseconds(40), true, true, vec(10, 1, 0), at) == fire_verdict::accept);
		CHECK(judge_fire(now, now - net_milliseconds(40), false, true, vec(10, 1, 0), at) == fire_verdict::dead);
		CHECK(judge_fire(now, now - net_milliseconds(600), true, true, vec(10, 1, 0), at) == fire_verdict::time);
		CHECK(judge_fire(now, now + net_milliseconds(80), true, true, vec(10, 1, 0), at) == fire_verdict::time);
		CHECK(judge_fire(now, now + net_milliseconds(30), true, true, vec(10, 1, 0), at) == fire_verdict::accept);
		CHECK(judge_fire(now, now, true, false, vec(10, 1, 0), at) == fire_verdict::not_owned);
		CHECK(judge_fire(now, now, true, true, vec(10, 12, 0), at) == fire_verdict::accept);
		CHECK(judge_fire(now, now, true, true, vec(10, 14, 0), at) == fire_verdict::origin);
		/* The ship's positions stopped arriving 200 ms before the shot:
		 * it may be 30 units further.
		 */
		CHECK(judge_fire(now, now, true, true, vec(10, 40, 0), rewound{vec(10, 0, 0), 0, rewind_quality::newest, net_milliseconds(200)}) == fire_verdict::accept);
		CHECK(judge_fire(now, now, true, true, vec(10, 60, 0), rewound{vec(10, 0, 0), 0, rewind_quality::newest, net_milliseconds(200)}) == fire_verdict::origin);
		/* No history (just spawned): nothing to compare with. */
		CHECK(judge_fire(now, now, true, true, vec(500, 0, 0), std::nullopt) == fire_verdict::accept);
		CHECK(std::string_view{fire_verdict_name(fire_verdict::origin)} == "origin");
	}
}

void test_input_validation()
{
	const net_clock now{net_seconds(20)};
	accepted_position prev{true, now - TICK, vec(0, 0, 0)};
	const auto slow{vec(40, 0, 0)};
	/* A ship at 40 units per second moved 0.67 units in a tick. */
	CHECK(validate_input(prev, now, now, vec(0.67, 0, 0), slow) == input_verdict::accept);
	/* As far as a ship gets at top speed, plus the slack. */
	CHECK(validate_input(prev, now, now, vec(7.4, 0, 0), slow) == input_verdict::accept);
	CHECK(validate_input(prev, now, now, vec(7.6, 0, 0), slow) == input_verdict::reject_teleport);
	CHECK(validate_input(prev, now, now, vec(300, 0, 0), slow) == input_verdict::reject_teleport);
	/* After a gap (loss) it may be further. */
	prev.time = now - net_milliseconds(500);
	CHECK(validate_input(prev, now, now, vec(79, 0, 0), slow) == input_verdict::accept);
	CHECK(validate_input(prev, now, now, vec(81, 0, 0), slow) == input_verdict::reject_teleport);
	/* Faster than any ship. */
	CHECK(validate_input(prev, now, now, vec(1, 0, 0), vec(149, 0, 0)) == input_verdict::accept);
	CHECK(validate_input(prev, now, now, vec(1, 0, 0), vec(120, 120, 0)) == input_verdict::reject_speed);
	/* A clock that is off, or an old packet: dropped, not corrected. */
	CHECK(validate_input(prev, now, now - net_milliseconds(1100), vec(1, 0, 0), slow) == input_verdict::drop_time);
	CHECK(validate_input(prev, now, now + net_milliseconds(150), vec(1, 0, 0), slow) == input_verdict::drop_time);
	CHECK(validate_input(prev, now, now + net_milliseconds(50), vec(1, 0, 0), slow) == input_verdict::accept);
	/* A new ship (after a death, a respawn) may be anywhere. */
	CHECK(validate_input({}, now, now, vec(3000, -200, 50), slow) == input_verdict::accept);
	/* The console warning: three rejections within a second, one line
	 * per second.
	 */
	{
		rejection_monitor m;
		CHECK(!m.on_reject(now));
		CHECK(!m.on_reject(now + net_milliseconds(2000)));
		CHECK(!m.on_reject(now + net_milliseconds(4000)));
		CHECK(!m.on_reject(now + net_milliseconds(4100)));
		CHECK(m.on_reject(now + net_milliseconds(4200)));
		CHECK(!m.on_reject(now + net_milliseconds(4300)));
		CHECK(!m.on_reject(now + net_milliseconds(5100)));
		CHECK(m.on_reject(now + net_milliseconds(5300)));
	}
	/* The client: one snap per correction, not one per bundle that still
	 * carries the flag.
	 */
	{
		correction_gate g;
		const net_clock hold{net_milliseconds(250)};
		unsigned applied{0};
		for (net_clock t = now; t < now + net_milliseconds(240); t += TICK)
			applied += g.apply(t, hold);
		CHECK(applied == 1);
		/* Still wrong after the hold: corrected again. */
		CHECK(g.apply(now + net_milliseconds(260), hold));
		g.reset();
		CHECK(g.apply(now + net_milliseconds(261), hold));
	}
}

/* Synthetic geometry for the catch-up: a wall (a plane at x = wall), or
 * a closed box.  The step is a sweep, as the game's physics: it finds
 * the first wall on the step's path.
 */
struct flying
{
	double x{}, y{}, z{};
	double vx{}, vy{}, vz{};
	bool alive{true};
	unsigned bounces{};
};

[[nodiscard]]
bool step_to_wall(flying &f, const double wall, const net_clock dt)
{
	const double s{static_cast<double>(dt) / net_seconds(1)};
	const double nx{f.x + f.vx * s};
	if (f.x <= wall && nx >= wall)
	{
		/* Hit: the shot ends on the wall. */
		const double part{(wall - f.x) / (nx - f.x)};
		f.y += f.vy * s * part;
		f.z += f.vz * s * part;
		f.x = wall;
		f.alive = false;
		return false;
	}
	f.x = nx;
	f.y += f.vy * s;
	f.z += f.vz * s;
	return true;
}

/* A bouncing shot in the box [-half, half]^3. */
[[nodiscard]]
bool step_in_box(flying &f, const double half, const net_clock dt)
{
	double left{static_cast<double>(dt) / net_seconds(1)};
	for (unsigned guard = 0; left > 0 && guard < 64; ++guard)
	{
		double first{left};
		int axis{-1};
		double *const p[3]{&f.x, &f.y, &f.z};
		double *const v[3]{&f.vx, &f.vy, &f.vz};
		for (int a = 0; a < 3; ++a)
		{
			if (*v[a] == 0)
				continue;
			const double t{((*v[a] > 0 ? half : -half) - *p[a]) / *v[a]};
			if (t >= 0 && t < first)
			{
				first = t;
				axis = a;
			}
		}
		for (int a = 0; a < 3; ++a)
			*p[a] += *v[a] * first;
		left -= first;
		if (axis < 0)
			break;
		*v[axis] = -*v[axis];
		++f.bounces;
	}
	return true;
}

void test_catch_up()
{
	const net_clock now{net_seconds(40)};
	CHECK(catch_up_time(now, now - net_milliseconds(40)) == net_milliseconds(40));
	CHECK(catch_up_time(now, now - net_milliseconds(900)) == NET_V2_CATCH_UP_MAX);
	CHECK(catch_up_time(now, now + net_milliseconds(20)) == 0);
	/* The steps: the whole time, none longer than the step. */
	{
		unsigned steps{0};
		net_clock longest{0}, sum{0};
		const auto done{catch_up(net_milliseconds(250), NET_V2_CATCH_UP_STEP, [&](const net_clock dt) {
			++steps;
			sum += dt;
			longest = std::max(longest, dt);
			return true;
		})};
		CHECK(done == net_milliseconds(250) && sum == done);
		CHECK(steps == 16 && longest == NET_V2_CATCH_UP_STEP);
		CHECK(catch_up(0, NET_V2_CATCH_UP_STEP, [](net_clock) { return true; }) == 0);
		CHECK(catch_up(100, NET_V2_CATCH_UP_STEP, [](net_clock) { return true; }) == 100);
	}
	std::mt19937 rng{20260930};
	std::uniform_real_distribution<double> speed{60, 900}, wall_at{0.5, 120}, late{0, 400}, side{-40, 40};
	unsigned stopped{0}, tunnelled_by_teleport{0};
	for (unsigned i = 0; i < 20000; ++i)
	{
		const double wall{wall_at(rng)};
		flying f{.x = 0, .y = 0, .z = 0, .vx = speed(rng), .vy = side(rng), .vz = side(rng)};
		const auto dt{catch_up_time(now, now - net_milliseconds(static_cast<net_clock>(late(rng))))};
		const auto start{f};
		const auto done{catch_up(dt, NET_V2_CATCH_UP_STEP, [&](const net_clock step) { return step_to_wall(f, wall, step); })};
		const double seconds{static_cast<double>(dt) / net_seconds(1)};
		const double free_x{start.vx * seconds};
		/* Never beyond the wall. */
		CHECK(f.x <= wall + 1e-9);
		if (free_x >= wall)
		{
			/* It would have passed the wall: it ended on it, and the
			 * stepping stopped there.
			 */
			CHECK(!f.alive && std::abs(f.x - wall) < 1e-9);
			CHECK(done <= dt);
			++stopped;
			++tunnelled_by_teleport;
		}
		else
		{
			/* Free flight: exactly where the time puts it. */
			CHECK(f.alive && done == dt);
			CHECK(std::abs(f.x - free_x) < 1e-6 && std::abs(f.y - start.vy * seconds) < 1e-6);
		}
	}
	/* The test is not vacuous: placing the shot at origin + velocity *
	 * time would have put thousands of them behind the wall.
	 */
	CHECK(stopped > 3000 && tunnelled_by_teleport == stopped);
	/* A bouncing shot (phoenix) in a closed box stays in the box and
	 * keeps its speed, however late it is.
	 */
	unsigned bounced{0};
	for (unsigned i = 0; i < 5000; ++i)
	{
		const double half{10};
		flying f{.x = side(rng) / 5, .y = side(rng) / 5, .z = side(rng) / 5, .vx = speed(rng) / 3, .vy = side(rng) * 3, .vz = side(rng) * 3};
		const double v0{std::sqrt(f.vx * f.vx + f.vy * f.vy + f.vz * f.vz)};
		const auto dt{catch_up_time(now, now - net_milliseconds(static_cast<net_clock>(late(rng))))};
		catch_up(dt, NET_V2_CATCH_UP_STEP, [&](const net_clock step) { return step_in_box(f, half, step); });
		CHECK(std::abs(f.x) <= half + 1e-6 && std::abs(f.y) <= half + 1e-6 && std::abs(f.z) <= half + 1e-6);
		CHECK(std::abs(std::sqrt(f.vx * f.vx + f.vy * f.vy + f.vz * f.vz) - v0) < 1e-6);
		bounced += f.bounces > 0;
	}
	CHECK(bounced > 2000);
}

void test_mirror_damage()
{
	const inventory_rules r{.max_energy = 200 * F1, .max_shields = 200 * F1, .energy_boost = 18 * F1, .shield_boost = 18 * F1};
	inventory_mirror m;
	inventory inv;
	inv.shields = 100 * F1;
	m.reset(inv);
	/* The client reports 100 with nothing applied. */
	m.on_report(r, inv, 0);
	CHECK(m.on_damage(30 * F1) == 70 * F1);
	CHECK(m.current().shields == 70 * F1 && m.pending() == 1 && m.issued() == 1);
	/* A report sent before the damage arrived does not give the shields
	 * back.
	 */
	m.on_report(r, inv, 0);
	CHECK(m.current().shields == 70 * F1 && m.pending() == 1);
	/* More damage and a shield boost granted in between, in order. */
	const pickup_desc shield{pickup_kind::shield, 0, 0, 0};
	const auto o{evaluate_pickup(m.current(), r, shield, 0)};
	CHECK(o.usable);
	CHECK(m.on_grant(r, shield, o) == 2);
	CHECK(m.current().shields == 88 * F1);
	CHECK(m.on_damage(10 * F1) == 78 * F1);
	/* The client applied the first damage only. */
	inv.shields = 70 * F1;
	m.on_report(r, inv, 1);
	CHECK(m.current().shields == 78 * F1 && m.pending() == 2);
	/* It applied everything, and gained 5 by its converter meanwhile:
	 * the copy follows the report.
	 */
	inv.shields = 83 * F1;
	m.on_report(r, inv, 3);
	CHECK(m.current().shields == 83 * F1 && m.pending() == 0);
	/* Lethal damage shows at once, whatever the last report said. */
	CHECK(m.on_damage(90 * F1) < 0);
	m.on_report(r, inv, 3);
	CHECK(m.current().shields < 0);
	/* Far more damage in flight than the copy keeps: the newest stay. */
	m.clear();
	inv.shields = 100 * F1;
	m.on_report(r, inv, m.issued());
	for (unsigned i = 0; i < inventory_mirror::MAX_PENDING + 10; ++i)
		m.on_damage(F1 / 16);
	CHECK(m.pending() == inventory_mirror::MAX_PENDING);
	/* A client's own damage, summed and sent in portions. */
	{
		damage_accumulator a;
		const net_clock now{net_seconds(9)};
		const net_clock interval{net_milliseconds(50)};
		CHECK(a.take(now, interval) == 0);
		a.add(3 * F1);
		a.add(-5);
		a.add(2 * F1);
		CHECK(a.take(now, interval) == 5 * F1);
		CHECK(a.take(now, interval) == 0);
		a.add(F1);
		CHECK(a.take(now + net_milliseconds(20), interval) == 0);
		a.add(F1);
		CHECK(a.take(now + net_milliseconds(50), interval) == 2 * F1);
		a.add(F1);
		a.reset();
		CHECK(a.take(now + net_seconds(1), interval) == 0);
	}
}

template <typename M>
void check_wire(const M &m)
{
	std::array<std::uint8_t, M::SIZE> buf{};
	m.write(buf);
	const auto r{M::read(buf)};
	CHECK(r);
	std::array<std::uint8_t, M::SIZE> again{};
	r->write(again);
	CHECK(buf == again);
	/* A wrong size is refused. */
	CHECK(!M::read(std::span<const std::uint8_t>(buf).first(M::SIZE - 1)));
	std::array<std::uint8_t, M::SIZE + 1> longer{};
	CHECK(!M::read(longer));
}

void test_wire()
{
	{
		fire_msg m;
		m.pid = 3;
		m.fire_time = 0xfedcba98;
		m.weapon = 104;
		m.level = 5;
		m.flags = 0x81;
		m.origin = {-123456789, 5, 2000000000};
		m.segment = 8999;
		m.orient = {-32768, 32767, -1, 12345};
		m.seed = 0xbeef;
		m.netid = dynamic_netid(3, 4095);
		m.count = 17;
		m.track = 6;
		check_wire(m);
		std::array<std::uint8_t, fire_msg::SIZE> buf{};
		m.write(buf);
		const auto r{fire_msg::read(buf)};
		CHECK(r->pid == 3 && r->fire_time == 0xfedcba98 && r->weapon == 104 && r->level == 5 && r->flags == 0x81);
		CHECK(r->origin == m.origin && r->segment == 8999 && r->orient == m.orient && r->seed == 0xbeef);
		CHECK(r->netid == m.netid && r->count == 17 && r->track == 6);
		/* Refused: no such player, too many projectiles, a level
		 * object's id.
		 */
		auto bad{m};
		bad.pid = 8;
		bad.write(buf);
		CHECK(!fire_msg::read(buf));
		bad = m;
		bad.count = NET_V2_FIRE_MAX_PROJECTILES + 1;
		bad.write(buf);
		CHECK(!fire_msg::read(buf));
		bad = m;
		bad.netid = level_netid(5);
		bad.write(buf);
		CHECK(!fire_msg::read(buf));
		bad = m;
		bad.netid = NETID_NONE;
		bad.write(buf);
		CHECK(!fire_msg::read(buf));
		/* Nothing was created: no id needed. */
		bad.count = 0;
		bad.write(buf);
		CHECK(fire_msg::read(buf));
	}
	{
		weapon_hit_msg m;
		m.netid = dynamic_netid(2, 77);
		m.weapon_id = 29;
		m.kind = hit_kind::splash;
		m.target = 7;
		m.target_time = 0x80000001;
		m.point = {1, -2, 3};
		m.segment = 17;
		m.damage = -1;
		check_wire(m);
		std::array<std::uint8_t, weapon_hit_msg::SIZE> buf{};
		m.write(buf);
		const auto r{weapon_hit_msg::read(buf)};
		CHECK(r->netid == m.netid && r->weapon_id == 29 && r->kind == hit_kind::splash && r->target == 7);
		CHECK(r->target_time == 0x80000001 && r->point == m.point && r->segment == 17 && r->damage == -1);
		buf[3] = NET_V2_HIT_KINDS;
		CHECK(!weapon_hit_msg::read(buf));
		buf[3] = 0;
		buf[4] = 8;
		CHECK(!weapon_hit_msg::read(buf));
	}
	{
		damage_msg m;
		m.victim = 1;
		m.attacker = 4;
		m.weapon_id = 9;
		m.amount = 5 * F1;
		m.shields = -3 * F1;
		m.point = {7, 8, -9};
		check_wire(m);
		std::array<std::uint8_t, damage_msg::SIZE> buf{};
		m.write(buf);
		const auto r{damage_msg::read(buf)};
		CHECK(r->victim == 1 && r->attacker == 4 && r->weapon_id == 9 && r->amount == 5 * F1 && r->shields == -3 * F1 && r->point == m.point);
		m.amount = -1;
		m.write(buf);
		CHECK(!damage_msg::read(buf));
		m.amount = 1;
		m.victim = 8;
		m.write(buf);
		CHECK(!damage_msg::read(buf));
	}
	{
		player_killed_msg m;
		m.victim = 5;
		m.killer = 2;
		m.kind = attacker_kind::player;
		m.weapon_id = 18;
		m.team_vector = 0xa5;
		m.bounty_target = 3;
		m.matrix = 65535;
		m.victim_deaths = 300;
		m.victim_kills = -7;
		m.killer_kills = -32768;
		check_wire(m);
		std::array<std::uint8_t, player_killed_msg::SIZE> buf{};
		m.write(buf);
		const auto r{player_killed_msg::read(buf)};
		CHECK(r->victim == 5 && r->killer == 2 && r->kind == attacker_kind::player && r->weapon_id == 18 && r->team_vector == 0xa5 && r->bounty_target == 3);
		CHECK(r->matrix == 65535 && r->victim_deaths == 300 && r->victim_kills == -7 && r->killer_kills == -32768);
		/* A killing player must be one; anything else has no player. */
		m.killer = NET_V2_PLAYER_ID_NONE;
		m.write(buf);
		CHECK(!player_killed_msg::read(buf));
		m.kind = attacker_kind::reactor;
		m.write(buf);
		CHECK(player_killed_msg::read(buf));
		m.killer = 2;
		m.write(buf);
		CHECK(!player_killed_msg::read(buf));
		buf[2] = 9;
		CHECK(!player_killed_msg::read(buf));
	}
	{
		player_spawn_msg m;
		m.pid = 7;
		m.flags = static_cast<std::uint8_t>(spawn_flag::invulnerable);
		m.site = 4;
		m.pos = {-5, 6, 7};
		m.segment = 1234;
		m.orient = {1, 2, 3, -4};
		check_wire(m);
		std::array<std::uint8_t, player_spawn_msg::SIZE> buf{};
		m.write(buf);
		const auto r{player_spawn_msg::read(buf)};
		CHECK(r->pid == 7 && r->flags == 1 && r->site == 4 && r->pos == m.pos && r->segment == 1234 && r->orient == m.orient);
		m.pid = 8;
		m.write(buf);
		CHECK(!player_spawn_msg::read(buf));
	}
}

/* The model: one host (player 0, no latency) and its clients.  Every
 * machine flies its ship on a fixed path, sends its state every tick,
 * is shown the others by the game's interpolation (net_interp.h), fires
 * projectiles at the ships it is shown, and reports the hits of its own
 * projectiles with the time the target was shown at.  The host judges
 * the reports against its position history, applies the damage and
 * announces damage and kills on ordered reliable channels.
 */
namespace sim {

using dcx::net_interp::entity_track;
using dcx::net_interp::snapshot;

constexpr net_clock STEP{TICK / 4};
constexpr double SHIP_SIZE{4.5};
constexpr double SHOT_SPEED{150};
constexpr std::int32_t SHOT_DAMAGE{18 * F1};
constexpr net_clock FIRE_WAIT{net_milliseconds(250)};
constexpr net_clock RESPAWN_DELAY{net_seconds(1)};
constexpr std::uint8_t WEAPON{11};

struct config
{
	unsigned players{4};
	std::array<unsigned, NET_V2_MAX_PLAYERS> latency_ms{};
	unsigned jitter_ms{};
	double loss{};
	net_clock window{NET_V2_REWIND_DEFAULT};
	unsigned seconds{60};
	unsigned seed{1};
	kill_mode mode{};
	std::uint8_t team_vector{};
	bool no_friendly_fire{};
	/* The host judges a report against where it has the target now (no
	 * history): what it did before stage 4 for its own shots only.
	 */
	bool no_rewind{};
	/* This client sends every hit report twice. */
	unsigned duplicating_client{NET_V2_MAX_PLAYERS};
};

enum class kind : std::uint8_t
{
	fire,
	hit,
	damage,
	killed,
	spawn,
	report,
};

struct message
{
	kind what{};
	std::vector<std::uint8_t> bytes;
};

template <typename M>
[[nodiscard]]
message encode(const kind what, const M &m)
{
	std::array<std::uint8_t, M::SIZE> buf{};
	m.write(buf);
	return {what, {buf.begin(), buf.end()}};
}

/* Ordered and reliable: a message is never delivered before one sent
 * earlier.
 */
struct channel
{
	std::deque<std::pair<net_clock, message>> queue;
	net_clock last_due{};
	void send(const net_clock due, message m)
	{
		last_due = std::max(last_due, due);
		queue.emplace_back(last_due, std::move(m));
	}
};

struct state_record
{
	std::uint8_t pid{};
	bool alive{};
	net_clock time{};
	net_vec pos;
	net_vec vel;
};

struct input
{
	net_clock sample_time{};
	bool alive{};
	net_vec pos;
	net_vec vel;
};

struct projectile
{
	netid_t id{};
	double x{}, y{}, z{};
	double vx{}, vy{}, vz{};
	net_clock dies{};
};

struct tallies
{
	std::array<std::array<unsigned, NET_V2_MAX_PLAYERS>, NET_V2_MAX_PLAYERS> damage_events{};
	std::array<std::array<std::int64_t, NET_V2_MAX_PLAYERS>, NET_V2_MAX_PLAYERS> damage_sum{};
	unsigned kills{};
	bool operator==(const tallies &) const = default;
};

struct machine
{
	unsigned id{};
	net_clock clock_error{};
	net_clock phase{};
	std::array<entity_track, NET_V2_MAX_PLAYERS> tracks{};
	std::array<bool, NET_V2_MAX_PLAYERS> seen_alive{};
	std::int32_t shields{100 * F1};
	bool alive{true};
	net_clock respawn_at{};
	std::uint16_t applied{};
	std::uint16_t next_shot{};
	net_clock next_fire{};
	net_clock next_report{};
	score_board board;
	tallies tally;
	std::vector<projectile> shots;
	unsigned reports_sent{};
};

struct world
{
	config cfg;
	std::mt19937 rng;
	net_clock now{net_seconds(100)};
	std::vector<machine> machines;
	std::vector<channel> up, down;
	std::vector<std::deque<std::pair<net_clock, input>>> inputs;
	std::vector<std::deque<std::pair<net_clock, std::vector<state_record>>>> states;
	/* The host. */
	std::array<position_history<>, NET_V2_MAX_PLAYERS> history{};
	std::array<inventory_mirror, NET_V2_MAX_PLAYERS> mirrors{};
	std::array<bool, NET_V2_MAX_PLAYERS> host_alive{};
	std::array<net_clock, NET_V2_MAX_PLAYERS> death_time{};
	std::array<accepted_position, NET_V2_MAX_PLAYERS> accepted{};
	std::array<input, NET_V2_MAX_PLAYERS> newest_input{};
	std::array<fire_limiter, NET_V2_MAX_PLAYERS> fire_limit{};
	std::vector<token_bucket> buckets;
	shot_registry registry;
	score_board board;
	tallies tally;
	std::array<unsigned, NET_V2_HIT_VERDICTS> verdicts{};
	unsigned limited{};
	unsigned fires_accepted{}, fires_refused{};
	std::array<unsigned, NET_V2_FIRE_VERDICTS> fire_verdicts{};
	unsigned inputs_rejected{};
	net_clock rewind_sum{}, rewind_max{}, view_age_max{};
	bool firing{true};
	bool reporting{true};

	explicit world(const config &c) :
		cfg{c}, rng{c.seed}
	{
		machines.resize(c.players);
		up.resize(c.players);
		down.resize(c.players);
		inputs.resize(c.players);
		states.resize(c.players);
		std::uniform_int_distribution<net_clock> error{-net_milliseconds(2), net_milliseconds(2)}, phase{0, TICK - 1};
		for (unsigned i = 0; i < c.players; ++i)
		{
			auto &m{machines[i]};
			m.id = i;
			m.clock_error = i ? error(rng) : 0;
			m.phase = phase(rng) / STEP * STEP;
			m.next_fire = now + net_milliseconds(500 + 40 * i);
			for (auto &t : m.tracks)
				t.reset(TICK);
			m.seen_alive.fill(true);
			host_alive[i] = true;
			inventory inv;
			inv.shields = 100 * F1;
			mirrors[i].reset(inv);
			buckets.emplace_back(NET_V2_COMBAT_RATE, NET_V2_COMBAT_BURST);
			buckets.back().reset(now);
		}
	}
	[[nodiscard]]
	net_clock delay(const unsigned machine)
	{
		if (!machine)
			return 0;
		std::uniform_int_distribution<unsigned> jitter{0, cfg.jitter_ms};
		return net_milliseconds(cfg.latency_ms[machine] + jitter(rng));
	}
	[[nodiscard]]
	bool lost(const unsigned machine)
	{
		return machine && std::uniform_real_distribution<double>{0, 1}(rng) < cfg.loss;
	}
	/* Where player `p` flies at the true time `t`: a closed curve in a
	 * room of about 80 units, at up to about 75 units per second (a
	 * ship with its afterburner).
	 */
	[[nodiscard]]
	static std::array<double, 6> flight(const unsigned p, const net_clock t)
	{
		const double s{static_cast<double>(t) / net_seconds(1)};
		const double a{0.9 + 0.09 * p}, b{0.6 + 0.07 * p}, c{0.75 - 0.04 * p};
		const double fa{1.0 + p}, fb{2.0 + 0.7 * p}, fc{0.5 * p};
		const double radius{40};
		return {{
			radius * std::sin(a * s + fa), radius * std::sin(b * s + fb), radius * std::cos(c * s + fc),
			radius * a * std::cos(a * s + fa), radius * b * std::cos(b * s + fb), -radius * c * std::sin(c * s + fc),
		}};
	}
	template <typename M>
	void to_host(const unsigned from, const kind what, const M &m)
	{
		up[from].send(now + delay(from), encode(what, m));
	}
	template <typename M>
	void to_all(const kind what, const M &m)
	{
		for (unsigned i = 0; i < cfg.players; ++i)
			down[i].send(now + delay(i), encode(what, m));
	}
	void send_report(machine &m)
	{
		inventory_msg r;
		r.pid = static_cast<std::uint8_t>(m.id);
		r.seq = m.applied;
		r.inv.shields = m.shields;
		to_host(m.id, kind::report, r);
		m.next_report = now + net_milliseconds(100);
	}

	/* The host: a kill. */
	void host_kill(const std::uint8_t victim, const kill_attribution killer)
	{
		host_alive[victim] = false;
		death_time[victim] = now;
		accepted[victim] = {};
		player_killed_msg m;
		m.victim = victim;
		m.killer = killer.pid;
		m.kind = killer.kind;
		m.weapon_id = WEAPON;
		m.team_vector = cfg.team_vector;
		m.bounty_target = board.bounty_target;
		apply_kill(board, cfg.mode, victim, killer, cfg.team_vector);
		if (killer.kind == attacker_kind::player)
		{
			m.matrix = board.kill_matrix[killer.pid][victim];
			m.killer_kills = board.kills[killer.pid];
		}
		m.victim_deaths = board.deaths[victim];
		m.victim_kills = board.kills[victim];
		++tally.kills;
		to_all(kind::killed, m);
	}
	void host_fire(const unsigned from, const message &msg)
	{
		const auto m{fire_msg::read(msg.bytes)};
		CHECK(m);
		if (!buckets[from].take(now))
		{
			++fires_refused;
			return;
		}
		CHECK(m->pid == from && netid_creator(m->netid) == from);
		const auto fire_time{dcx::net_interp::unwrap(m->fire_time, now)};
		const bool alive{host_alive[from] || fire_time <= death_time[from]};
		auto verdict{judge_fire(now, fire_time, alive, true, m->origin, history[from].rewind(fire_time))};
		if (verdict == fire_verdict::accept && !fire_limit[from].allow(fire_time, FIRE_WAIT))
			verdict = fire_verdict::too_fast;
		++fire_verdicts[static_cast<std::size_t>(verdict)];
		if (verdict != fire_verdict::accept)
		{
			++fires_refused;
			return;
		}
		++fires_accepted;
		shot_record s;
		s.id = m->netid;
		s.owner = static_cast<std::uint8_t>(from);
		s.weapon_id = s.child_weapon_id = WEAPON;
		s.fire_time = fire_time;
		s.origin = m->origin;
		s.max_speed = static_cast<std::int64_t>(SHOT_SPEED * NET_V2_UNIT) + NET_V2_MAX_SHIP_SPEED;
		s.lifetime = net_seconds(4);
		registry.add(s);
	}
	void host_hit(const unsigned from, const message &msg)
	{
		const auto m{weapon_hit_msg::read(msg.bytes)};
		CHECK(m);
		const auto count{[this](const hit_verdict v) { ++verdicts[static_cast<std::size_t>(v)]; }};
		if (!buckets[from].take(now))
			return count(hit_verdict::rate_limited);
		const auto target_time{dcx::net_interp::unwrap(m->target_time, now)};
		const auto at{limit_rewind(now, target_time, cfg.window)};
		const auto shot{registry.find(m->netid)};
		auto target_at{history[m->target].rewind(at.time)};
		if (cfg.no_rewind && !history[m->target].empty())
			target_at = rewound{history[m->target].newest().pos, 0, rewind_quality::newest, 0};
		const auto verdict{judge_direct_hit(shot, static_cast<std::uint8_t>(from), m->weapon_id, m->target, now, target_time, m->point, target_at, static_cast<std::int64_t>(SHIP_SIZE * NET_V2_UNIT), NET_V2_UNIT / 2)};
		if (verdict != hit_verdict::accept)
			return count(verdict);
		const victim_view victim{
			.playing = true,
			.alive = host_alive[m->target] && mirrors[m->target].has_report(),
			.invulnerable = false,
			.cloaked = false,
			.team = team_of(cfg.team_vector, m->target),
		};
		const damage_context ctx{.team_game = cfg.mode.team, .coop = false, .no_friendly_fire = cfg.no_friendly_fire, .endlevel = false};
		const auto amount{clamp_claim(m->damage, SHOT_DAMAGE)};
		if (evaluate_damage(victim, attacker_kind::player, team_of(cfg.team_vector, static_cast<std::uint8_t>(from)), ctx, false, amount) != damage_verdict::apply)
			return count(hit_verdict::no_damage);
		count(hit_verdict::accept);
		if (at.limited)
			++limited;
		rewind_sum += now - at.time;
		rewind_max = std::max(rewind_max, now - at.time);
		view_age_max = std::max(view_age_max, now - target_time);
		consume_shot(*shot, m->target);
		damage_msg d;
		d.victim = m->target;
		d.attacker = static_cast<std::uint8_t>(from);
		d.weapon_id = m->weapon_id;
		d.amount = amount;
		d.shields = mirrors[m->target].on_damage(amount);
		d.point = m->point;
		++tally.damage_events[from][m->target];
		tally.damage_sum[from][m->target] += amount;
		to_all(kind::damage, d);
		if (d.shields < 0)
			host_kill(m->target, kill_credit(attacker_kind::player, static_cast<std::uint8_t>(from), true));
	}
	void host_receive(const unsigned from, const message &msg)
	{
		switch (msg.what)
		{
			case kind::fire:
				host_fire(from, msg);
				break;
			case kind::hit:
				host_hit(from, msg);
				break;
			case kind::spawn:
				{
					const auto m{player_spawn_msg::read(msg.bytes)};
					CHECK(m && m->pid == from && !host_alive[from]);
					host_alive[from] = true;
					history[from].clear();
					accepted[from] = {};
					mirrors[from].clear();
				}
				break;
			case kind::report:
				{
					const auto m{inventory_msg::read(msg.bytes)};
					CHECK(m && m->pid == from);
					if (!host_alive[from])
						break;
					mirrors[from].on_report({}, m->inv, m->seq);
					/* A death the damage did not show yet cannot be:
					 * every loss of shields is the host's.
					 */
					CHECK(mirrors[from].current().shields >= 0);
				}
				break;
			default:
				CHECK(false);
		}
	}
	void host_tick()
	{
		/* The states that arrived, oldest first. */
		for (unsigned p = 0; p < cfg.players; ++p)
		{
			auto &q{inputs[p]};
			std::stable_sort(q.begin(), q.end(), [](const auto &a, const auto &b) { return a.first < b.first; });
			while (!q.empty() && q.front().first <= now)
			{
				const auto in{q.front().second};
				q.pop_front();
				if (newest_input[p].sample_time >= in.sample_time)
					continue;
				const bool validated{in.alive && host_alive[p]};
				if (validated)
				{
					const auto v{validate_input(accepted[p], now, in.sample_time, in.pos, in.vel)};
					if (v != input_verdict::accept)
					{
						++inputs_rejected;
						continue;
					}
				}
				auto t{std::clamp(in.sample_time, now - net_seconds(1), now)};
				if (newest_input[p].sample_time && t <= accepted[p].time)
					t = accepted[p].time + 1;
				newest_input[p] = in;
				newest_input[p].sample_time = std::max(in.sample_time, newest_input[p].sample_time);
				if (in.alive)
				{
					history[p].push(t, in.pos, 0);
					accepted[p] = {validated, t, in.pos};
					newest_input[p].sample_time = t;
				}
				else
					accepted[p] = {};
			}
		}
		/* The bundle. */
		std::vector<state_record> bundle;
		for (unsigned p = 0; p < cfg.players; ++p)
			bundle.push_back({static_cast<std::uint8_t>(p), newest_input[p].alive, newest_input[p].sample_time, newest_input[p].pos, newest_input[p].vel});
		for (unsigned i = 0; i < cfg.players; ++i)
			if (!lost(i))
				states[i].emplace_back(now + delay(i), bundle);
		registry.expire(now);
	}

	void client_receive(machine &m, const message &msg)
	{
		switch (msg.what)
		{
			case kind::damage:
				{
					const auto d{damage_msg::read(msg.bytes)};
					CHECK(d);
					++m.tally.damage_events[d->attacker][d->victim];
					m.tally.damage_sum[d->attacker][d->victim] += d->amount;
					if (d->victim == m.id)
					{
						++m.applied;
						if (m.alive)
							m.shields -= d->amount;
					}
				}
				break;
			case kind::killed:
				{
					const auto k{player_killed_msg::read(msg.bytes)};
					CHECK(k);
					m.board.bounty_target = k->bounty_target;
					apply_kill(m.board, cfg.mode, k->victim, {k->kind, k->killer}, k->team_vector);
					/* The host's counts, which the rules gave here too. */
					if (k->kind == attacker_kind::player)
					{
						CHECK(m.board.kill_matrix[k->killer][k->victim] == k->matrix);
						CHECK(m.board.kills[k->killer] == k->killer_kills);
					}
					CHECK(m.board.deaths[k->victim] == k->victim_deaths);
					CHECK(m.board.kills[k->victim] == k->victim_kills);
					++m.tally.kills;
					m.seen_alive[k->victim] = false;
					if (k->victim == m.id)
					{
						/* The host decides: the ship dies here even if
						 * its own count says it has shields left.
						 */
						CHECK(m.alive);
						m.alive = false;
						m.respawn_at = now + RESPAWN_DELAY;
					}
				}
				break;
			default:
				CHECK(false);
		}
	}
	void client_tick(machine &m)
	{
		const auto est{now + m.clock_error};
		const auto f{flight(m.id, now)};
		const auto pos{vec(f[0], f[1], f[2])};
		const auto vel{vec(f[3], f[4], f[5])};
		if (!m.alive && now >= m.respawn_at)
		{
			m.alive = true;
			m.shields = 100 * F1;
			player_spawn_msg s;
			s.pid = static_cast<std::uint8_t>(m.id);
			s.pos = pos;
			to_host(m.id, kind::spawn, s);
			send_report(m);
		}
		/* INPUT. */
		if (!lost(m.id))
			inputs[m.id].emplace_back(now + delay(m.id), input{est, m.alive, pos, vel});
		/* The others, as shown. */
		struct shown
		{
			bool valid{};
			net_clock time{};
			double x{}, y{}, z{};
			double vx{}, vy{}, vz{};
		};
		std::array<shown, NET_V2_MAX_PLAYERS> view{};
		for (unsigned p = 0; p < cfg.players; ++p)
		{
			if (p == m.id)
				continue;
			auto &t{m.tracks[p]};
			t.delay.update(now);
			if (t.ring.empty())
				continue;
			const auto render{t.render_time(est)};
			const auto pose{dcx::net_interp::sample(t.ring, render)};
			if (!pose || !pose->alive)
				continue;
			view[p] = {true, std::clamp(render, t.ring.oldest().time, t.ring.newest().time + dcx::net_interp::NET_INTERP_EXTRAPOLATE_MAX), pose->pos.x / 65536.0, pose->pos.y / 65536.0, pose->pos.z / 65536.0, pose->vel.x / 65536.0, pose->vel.y / 65536.0, pose->vel.z / 65536.0};
		}
		/* Its projectiles: a sweep of this tick's path against every
		 * ship shown.
		 */
		const double dt{static_cast<double>(TICK) / net_seconds(1)};
		for (auto i{m.shots.begin()}; i != m.shots.end();)
		{
			auto &s{*i};
			bool gone{now >= s.dies};
			for (unsigned p = 0; p < cfg.players && !gone; ++p)
			{
				if (!view[p].valid)
					continue;
				const double dx{s.vx * dt}, dy{s.vy * dt}, dz{s.vz * dt};
				const double len2{dx * dx + dy * dy + dz * dz};
				/* The first point of the path within the ship's size of
				 * its centre: where the projectile touches it.
				 */
				const double ox{s.x - view[p].x}, oy{s.y - view[p].y}, oz{s.z - view[p].z};
				const double b{ox * dx + oy * dy + oz * dz};
				const double c{ox * ox + oy * oy + oz * oz - SHIP_SIZE * SHIP_SIZE};
				double u{0};
				if (c > 0)
				{
					const double disc{b * b - len2 * c};
					if (disc < 0)
						continue;
					u = (-b - std::sqrt(disc)) / len2;
					if (u < 0 || u > 1)
						continue;
				}
				const double cx{s.x + dx * u}, cy{s.y + dy * u}, cz{s.z + dz * u};
				weapon_hit_msg h;
				h.netid = s.id;
				h.weapon_id = WEAPON;
				h.kind = hit_kind::direct;
				h.target = static_cast<std::uint8_t>(p);
				h.target_time = to_net_time(view[p].time);
				h.point = vec(cx, cy, cz);
				h.damage = SHOT_DAMAGE;
				to_host(m.id, kind::hit, h);
				++m.reports_sent;
				if (m.id == cfg.duplicating_client)
					to_host(m.id, kind::hit, h);
				gone = true;
			}
			if (gone)
				i = m.shots.erase(i);
			else
			{
				s.x += s.vx * dt;
				s.y += s.vy * dt;
				s.z += s.vz * dt;
				++i;
			}
		}
		/* Fire at a ship shown, with lead. */
		if (firing && m.alive && now >= m.next_fire)
		{
			m.next_fire = now + FIRE_WAIT;
			std::vector<unsigned> targets;
			for (unsigned p = 0; p < cfg.players; ++p)
				if (view[p].valid)
					targets.push_back(p);
			if (!targets.empty())
			{
				const auto &t{view[targets[std::uniform_int_distribution<std::size_t>{0, targets.size() - 1}(rng)]]};
				double ax{t.x}, ay{t.y}, az{t.z};
				for (unsigned k = 0; k < 3; ++k)
				{
					const double d{std::sqrt((ax - f[0]) * (ax - f[0]) + (ay - f[1]) * (ay - f[1]) + (az - f[2]) * (az - f[2]))};
					const double flight_time{d / SHOT_SPEED};
					ax = t.x + t.vx * flight_time;
					ay = t.y + t.vy * flight_time;
					az = t.z + t.vz * flight_time;
				}
				const double d{std::sqrt((ax - f[0]) * (ax - f[0]) + (ay - f[1]) * (ay - f[1]) + (az - f[2]) * (az - f[2]))};
				if (d > 1)
				{
					projectile s;
					s.id = dynamic_netid(static_cast<std::uint8_t>(m.id), m.next_shot++);
					s.x = f[0];
					s.y = f[1];
					s.z = f[2];
					s.vx = (ax - f[0]) / d * SHOT_SPEED;
					s.vy = (ay - f[1]) / d * SHOT_SPEED;
					s.vz = (az - f[2]) / d * SHOT_SPEED;
					s.dies = now + net_seconds(3);
					m.shots.push_back(s);
					fire_msg fm;
					fm.pid = static_cast<std::uint8_t>(m.id);
					fm.fire_time = to_net_time(est);
					fm.weapon = WEAPON;
					fm.origin = pos;
					fm.netid = s.id;
					fm.count = 1;
					to_host(m.id, kind::fire, fm);
				}
			}
		}
		if (reporting && m.alive && now >= m.next_report)
			send_report(m);
	}
	void step()
	{
		now += STEP;
		/* Deliveries. */
		for (unsigned i = 0; i < cfg.players; ++i)
		{
			auto &q{up[i].queue};
			while (!q.empty() && q.front().first <= now)
			{
				const auto msg{std::move(q.front().second)};
				q.pop_front();
				host_receive(i, msg);
			}
		}
		for (unsigned i = 0; i < cfg.players; ++i)
		{
			auto &m{machines[i]};
			auto &q{down[i].queue};
			while (!q.empty() && q.front().first <= now)
			{
				const auto msg{std::move(q.front().second)};
				q.pop_front();
				client_receive(m, msg);
			}
			auto &s{states[i]};
			for (auto it{s.begin()}; it != s.end();)
			{
				if (it->first > now)
				{
					++it;
					continue;
				}
				for (const auto &r : it->second)
				{
					if (r.pid == i || !r.time)
						continue;
					auto &t{m.tracks[r.pid]};
					if (!r.alive)
					{
						t.ring.clear();
						continue;
					}
					snapshot sn;
					sn.time = r.time;
					sn.pos = r.pos;
					sn.vel = r.vel;
					t.receive(sn, now, now + m.clock_error);
				}
				it = s.erase(it);
			}
		}
		if ((now / STEP) % 4 == 0)
			host_tick();
		for (auto &m : machines)
			if ((now - m.phase) / STEP % 4 == 0)
				client_tick(m);
	}
	void run()
	{
		const auto end{now + net_seconds(cfg.seconds)};
		while (now < end)
			step();
		/* Cease fire; let every projectile, message and respawn finish. */
		firing = false;
		const auto drained{now + net_seconds(8)};
		while (now < drained)
			step();
		/* The last reports. */
		reporting = false;
		for (auto &m : machines)
		{
			CHECK(m.shots.empty() && m.alive);
			send_report(m);
		}
		const auto quiet{now + net_seconds(2)};
		while (now < quiet)
			step();
		for (unsigned i = 0; i < cfg.players; ++i)
			CHECK(up[i].queue.empty() && down[i].queue.empty());
	}
	/* Every machine has the host's scores, the host's damage, the host's
	 * kills, and the shields the host has for it.
	 */
	void check_consistent() const
	{
		for (const auto &m : machines)
		{
			CHECK(m.board == board);
			CHECK(m.tally == tally);
			CHECK(m.alive && host_alive[m.id]);
			CHECK(mirrors[m.id].current().shields == m.shields);
			CHECK(mirrors[m.id].pending() == 0);
		}
	}
	[[nodiscard]]
	unsigned count(const hit_verdict v) const
	{
		return verdicts[static_cast<std::size_t>(v)];
	}
	/* The only shots the host refuses are those of a ship it had already
	 * killed (fired before the shooter learned of it), and the only
	 * reports about shots it does not know are about those.
	 */
	void check_refusals() const
	{
		CHECK(inputs_rejected == 0);
		CHECK(fires_refused == fire_verdicts[static_cast<std::size_t>(fire_verdict::dead)]);
		CHECK(count(hit_verdict::unknown_shot) <= fires_refused);
		CHECK(fires_refused * 20 <= fires_accepted);
	}
	[[nodiscard]]
	unsigned reports() const
	{
		unsigned n{0};
		for (const auto v : verdicts)
			n += v;
		return n;
	}
};

}

void report(const char *const name, const sim::world &w)
{
	const auto accepted{w.count(hit_verdict::accept)};
	std::printf("%s: %u shots (%u refused), %u reports: %u accepted, %u missed, %u without damage, %u repeated; %u kills; rewind mean %.0f ms, max %.0f ms, oldest view %.0f ms, %u accepted beyond the window\n", name, w.fires_accepted, w.fires_refused, w.reports(), accepted, w.count(hit_verdict::missed), w.count(hit_verdict::no_damage), w.count(hit_verdict::consumed), w.tally.kills, accepted ? w.rewind_sum * 1000.0 / accepted / net_seconds(1) : 0.0, w.rewind_max * 1000.0 / net_seconds(1), w.view_age_max * 1000.0 / net_seconds(1), w.limited);
}

void test_simulation()
{
	/* A LAN-like game: the group's 15 to 25 ms. */
	{
		sim::config c;
		c.players = 4;
		c.latency_ms = {{0, 15, 20, 25}};
		c.jitter_ms = 6;
		c.loss = 0.02;
		c.seed = 11;
		sim::world w{c};
		w.run();
		report("lan", w);
		w.check_consistent();
		w.check_refusals();
		CHECK(w.tally.kills >= 10);
		CHECK(w.count(hit_verdict::accept) > 200);
		/* What a shooter saw hit, hit: no honest report is judged a
		 * miss, none needs more than the window, and the only refusals
		 * are hits on a ship the host had already killed.
		 */
		CHECK(w.count(hit_verdict::missed) == 0);
		CHECK(w.limited == 0);
		CHECK(w.rewind_max < NET_V2_REWIND_DEFAULT);
		CHECK(w.reports() == w.count(hit_verdict::accept) + w.count(hit_verdict::no_damage) + w.count(hit_verdict::unknown_shot));
		/* The host has no advantage: its reports are accepted like the
		 * clients'.
		 */
		for (const auto &m : w.machines)
		{
			unsigned credited{0};
			for (const auto n : w.tally.damage_events[m.id])
				credited += n;
			CHECK(m.reports_sent > 50);
			CHECK(credited * 10 >= m.reports_sent * 8);
		}
	}
	/* Eight players over the internet, in teams with friendly fire off. */
	{
		sim::config c;
		c.players = 8;
		c.latency_ms = {{0, 20, 30, 40, 50, 60, 35, 45}};
		c.jitter_ms = 15;
		c.loss = 0.05;
		c.seed = 12;
		c.mode.team = true;
		c.team_vector = 0b10101010;
		c.no_friendly_fire = true;
		c.seconds = 90;
		sim::world w{c};
		w.run();
		report("internet teams", w);
		w.check_consistent();
		w.check_refusals();
		CHECK(w.tally.kills >= 10);
		/* Nobody hurt a team mate. */
		for (unsigned a = 0; a < 8; ++a)
			for (unsigned v = 0; v < 8; ++v)
				if (team_of(c.team_vector, static_cast<std::uint8_t>(a)) == team_of(c.team_vector, static_cast<std::uint8_t>(v)))
					CHECK(w.tally.damage_events[a][v] == 0);
		CHECK(w.board.team_kills[0] + w.board.team_kills[1] == static_cast<int>(w.tally.kills));
		/* With loss and jitter a shown position is sometimes a guess
		 * (extrapolated): a few reports may miss, almost all hit.
		 */
		const auto judged{w.count(hit_verdict::accept) + w.count(hit_verdict::missed)};
		CHECK(judged > 300);
		CHECK(w.count(hit_verdict::missed) * 100 <= judged);
	}
	/* Bounty, with a client that sends every report twice: the second
	 * copy never counts.
	 */
	{
		sim::config c;
		c.players = 4;
		c.latency_ms = {{0, 25, 40, 30}};
		c.jitter_ms = 10;
		c.loss = 0.03;
		c.seed = 13;
		c.mode.bounty = true;
		c.duplicating_client = 2;
		sim::world w{c};
		w.run();
		report("bounty, duplicates", w);
		w.check_consistent();
		CHECK(w.tally.kills >= 10);
		CHECK(w.count(hit_verdict::consumed) > 30);
		unsigned by_two{0};
		for (const auto n : w.tally.damage_events[2])
			by_two += n;
		CHECK(by_two <= w.machines[2].reports_sent);
	}
	/* Players with 120 to 150 ms each way: their view of a target is
	 * older than the window, so the host follows it only that far.  The
	 * shooters then miss what they saw hit, but every machine still
	 * agrees on what happened.
	 */
	{
		sim::config c;
		c.players = 3;
		c.latency_ms = {{0, 120, 150}};
		c.jitter_ms = 10;
		c.loss = 0.02;
		c.seed = 14;
		sim::world w{c};
		w.run();
		report("high latency", w);
		w.check_consistent();
		CHECK(w.limited + w.count(hit_verdict::missed) > 20);
		CHECK(w.rewind_max <= NET_V2_REWIND_DEFAULT);
		/* The same game with the window at its limit: the views are
		 * honoured again.
		 */
		c.window = rewind_window(500);
		sim::world wide{c};
		wide.run();
		report("high latency, 500 ms window", wide);
		wide.check_consistent();
		CHECK(wide.limited == 0);
		CHECK(wide.count(hit_verdict::missed) * 50 <= wide.count(hit_verdict::accept));
		CHECK(wide.count(hit_verdict::accept) > w.count(hit_verdict::accept));
	}
	/* No rewind at all: the reports judged against where the host has
	 * the target at the moment they arrive.  A good part of what the
	 * shooters saw hit is refused, the more the faster the target: this
	 * is what the history is for.
	 */
	{
		sim::config c;
		c.players = 4;
		c.latency_ms = {{0, 15, 20, 25}};
		c.jitter_ms = 6;
		c.loss = 0.02;
		c.seed = 11;
		c.no_rewind = true;
		sim::world w{c};
		w.run();
		report("lan, no rewind", w);
		w.check_consistent();
		CHECK(w.count(hit_verdict::missed) * 20 > w.count(hit_verdict::accept));
	}
}

}

int main()
{
	test_history();
	test_rewind_window();
	test_hit_verdicts();
	test_splash();
	test_damage_rules();
	test_kills();
	test_rates();
	test_input_validation();
	test_catch_up();
	test_mirror_damage();
	test_wire();
	test_simulation();
	std::puts("all tests passed");
	return 0;
}
