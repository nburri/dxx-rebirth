/*
 * This file is part of the DXX-Rebirth project <https://github.com/dxx-rebirth/dxx-rebirth/>.
 * It is copyright by its individual contributors, as recorded in the
 * project's Git history.  See COPYING.txt at the top level for license
 * terms and a link to the Git history.
 */
/*
 * Test of the game mode rules the host decides (net_v2_modes.h, stage
 * 6a): the capture rule, the scores of a capture and the kill goal, the
 * flag count, the CAPTURE wire layout, the host's copy of a client's
 * inventory with host-owned flags (net_v2_objects.h); and a model of a
 * host and its clients that plays pickups, deaths, drops and captures
 * with reports late and out of step, and checks after every step that
 * each flag is in the level or carried exactly once.  Capture the flag
 * (Classic): the options, flag touches, captures with the own flag home
 * or away, the home choice, CTF_NOTICE, the own flag's return in the
 * pickup rules, and a model of every option combination.
 *
 * Build and run with SCons:
 *
 *	scons sdl2=1 d1x=0 d2x=1 register_runtime_test_plain_link_targets=1 test-net-v2-modes
 *	build/common/test-net-v2-modes
 *
 * or directly:
 *
 *	g++ -std=gnu++23 -O2 -Wall -Wextra -Icommon/main common/unittest/net_v2_modes.cpp -o test-net-v2-modes
 */

#include <cstdio>
#include <cstdlib>
#include <deque>
#include <random>
#include <vector>

#include "net_v2_modes.h"
#include "net_v2_objects.h"

using namespace dcx::net_v2;

namespace {

void check_failed(const char *const expr, const char *const file, const int line)
{
	std::fprintf(stderr, "%s:%d: check failed: %s\n", file, line, expr);
	std::exit(1);
}

#define CHECK(cond)	do { if (!(cond)) check_failed(#cond, __FILE__, __LINE__); } while (0)

constexpr std::uint32_t FLAG_QUAD{0x400};
constexpr std::uint32_t FLAG_TEAM{0x100000};

[[nodiscard]]
inventory_rules ctf_rules(const std::uint8_t team)
{
	inventory_rules r;
	r.max_energy = 200 << 16;
	r.max_shields = 200 << 16;
	r.has_team_flag_bit = FLAG_TEAM;
	r.host_owned_flags = FLAG_TEAM;
	r.capture_mode = true;
	r.team = team;
	return r;
}

/* The flag of team `flag` (taken by the other team). */
[[nodiscard]]
constexpr pickup_desc flag_desc(const std::uint8_t flag)
{
	return {pickup_kind::team_flag, other_team(flag), 0, 0};
}

void test_capture_rule()
{
	capture_check c{true, CTF_TEAM_BLUE, true, CTF_TEAM_BLUE};
	CHECK(capture_due(c));
	/* The other team's goal, no goal, no flag, dead. */
	c.goal = CTF_TEAM_RED;
	CHECK(!capture_due(c));
	c.goal.reset();
	CHECK(!capture_due(c));
	c.goal = CTF_TEAM_BLUE;
	c.carries_flag = false;
	CHECK(!capture_due(c));
	c.carries_flag = true;
	c.alive = false;
	CHECK(!capture_due(c));
	/* Red in the red goal. */
	CHECK(capture_due({true, CTF_TEAM_RED, true, CTF_TEAM_RED}));
	CHECK(!capture_due({true, CTF_TEAM_RED, true, CTF_TEAM_BLUE}));
	CHECK(other_team(CTF_TEAM_BLUE) == CTF_TEAM_RED);
	CHECK(other_team(CTF_TEAM_RED) == CTF_TEAM_BLUE);
}

void test_scores()
{
	const auto s{capture_score({10, 3, 4})};
	CHECK(s.team_score == 15);
	CHECK(s.kills == 8);
	CHECK(s.kill_goal_count == 9);
	/* Negative scores (suicides) still gain five. */
	const auto n{capture_score({-2, -1, 0})};
	CHECK(n.team_score == 3 && n.kills == 4 && n.kill_goal_count == 5);
	/* Kill goal 10 = 50 points: a team game counts the team. */
	CHECK(!kill_goal_reached(0, true, {500, 500, 500}));
	CHECK(kill_goal_reached(10, true, {50, 0, 0}));
	CHECK(!kill_goal_reached(10, true, {49, 60, 60}));
	CHECK(kill_goal_reached(10, false, {0, 0, 50}));
	CHECK(!kill_goal_reached(10, false, {60, 0, 49}));
}

void test_census()
{
	flag_census c;
	const std::array<std::uint8_t, CTF_TEAMS> one{{1, 1}};
	c.in_level = {{1, 1}};
	CHECK(c.holds(one));
	/* Taken: carried instead. */
	c.in_level[CTF_TEAM_RED] = 0;
	c.carried[CTF_TEAM_RED] = 1;
	CHECK(c.holds(one));
	/* A duplicate: dropped and still carried. */
	c.in_level[CTF_TEAM_RED] = 1;
	CHECK(!c.holds(one));
	/* A lost flag. */
	c.in_level[CTF_TEAM_RED] = 0;
	c.carried[CTF_TEAM_RED] = 0;
	CHECK(!c.holds(one));
	CHECK(c.total(CTF_TEAM_BLUE) == 1);
	CHECK(c.total(7) == 0);
	/* A level without flags. */
	CHECK(flag_census{}.holds({{0, 0}}));
}

void test_capture_wire()
{
	capture_msg m;
	m.pid = 5;
	m.team = CTF_TEAM_RED;
	m.flag = CTF_TEAM_BLUE;
	m.scores = {-300, 1234, -7};
	std::array<std::uint8_t, capture_msg::SIZE> buf;
	m.write(buf);
	const auto r{capture_msg::read(buf)};
	CHECK(r);
	CHECK(r->pid == 5 && r->team == CTF_TEAM_RED && r->flag == CTF_TEAM_BLUE);
	CHECK(r->scores.team_score == -300 && r->scores.kills == 1234 && r->scores.kill_goal_count == -7);
	/* Wrong size. */
	CHECK(!capture_msg::read(std::span<const std::uint8_t>(buf).first(capture_msg::SIZE - 1)));
	std::array<std::uint8_t, capture_msg::SIZE + 1> longer{};
	CHECK(!capture_msg::read(longer));
	/* A player out of range, a team out of range, a team's own flag. */
	auto bad{buf};
	bad[0] = NET_V2_MAX_PLAYERS;
	CHECK(!capture_msg::read(bad));
	bad = buf;
	bad[1] = 2;
	CHECK(!capture_msg::read(bad));
	bad = buf;
	bad[2] = CTF_TEAM_RED;
	CHECK(!capture_msg::read(bad));
}

void test_host_owned_flags()
{
	const auto r{ctf_rules(CTF_TEAM_BLUE)};
	inventory_mirror m;
	m.reset({});
	/* A grant of the red flag (taken by blue). */
	const auto d{flag_desc(CTF_TEAM_RED)};
	const auto o{evaluate_pickup(m.current(), r, d, 0)};
	CHECK(o.usable);
	const auto seq{m.on_grant(r, d, o)};
	CHECK(m.current().powerup_flags & FLAG_TEAM);
	/* A report sent before the grant arrived: the flag stays. */
	m.on_report(r, {}, static_cast<std::uint16_t>(seq - 1));
	CHECK(m.current().powerup_flags & FLAG_TEAM);
	/* A report with the grant that says "no flag" (the client lost it
	 * in a way only the host decides): the flag stays as well.
	 */
	m.on_report(r, {}, seq);
	CHECK(m.current().powerup_flags & FLAG_TEAM);
	CHECK(m.base().powerup_flags & FLAG_TEAM);
	/* The capture: gone, and a report sent before it arrived does not
	 * bring it back.
	 */
	m.take_flags(FLAG_TEAM);
	CHECK(!(m.current().powerup_flags & FLAG_TEAM));
	inventory stale;
	stale.powerup_flags = FLAG_TEAM | FLAG_QUAD;
	m.on_report(r, stale, seq);
	CHECK(!(m.current().powerup_flags & FLAG_TEAM));
	/* Other flags still follow the reports. */
	CHECK(m.current().powerup_flags & FLAG_QUAD);
	m.on_report(r, {}, seq);
	CHECK(!(m.current().powerup_flags & FLAG_QUAD));
	/* A drop the host made (DROP_REQUEST). */
	const auto seq2{m.on_grant(r, d, evaluate_pickup(m.current(), r, d, 0))};
	CHECK(m.current().powerup_flags & FLAG_TEAM);
	m.on_drop(r, d, 0);
	CHECK(!(m.current().powerup_flags & FLAG_TEAM));
	m.on_report(r, stale, seq2);
	CHECK(!(m.current().powerup_flags & FLAG_TEAM));
	/* Without host ownership (another mode), the report decides. */
	auto plain{r};
	plain.host_owned_flags = 0;
	m.on_report(plain, stale, seq2);
	CHECK(m.current().powerup_flags & FLAG_TEAM);
	/* The accounting counts the flag the player's team takes, not the
	 * other.
	 */
	inventory carrier;
	carrier.powerup_flags = FLAG_TEAM;
	CHECK(units_carried(carrier, ctf_rules(CTF_TEAM_BLUE), flag_desc(CTF_TEAM_RED)) == 1);
	CHECK(units_carried(carrier, ctf_rules(CTF_TEAM_BLUE), flag_desc(CTF_TEAM_BLUE)) == 0);
	CHECK(units_carried(carrier, ctf_rules(CTF_TEAM_RED), flag_desc(CTF_TEAM_BLUE)) == 1);
	CHECK(units_carried({}, ctf_rules(CTF_TEAM_RED), flag_desc(CTF_TEAM_BLUE)) == 0);
	/* A death empties the copy. */
	m.clear();
	CHECK(!(m.current().powerup_flags & FLAG_TEAM));
}

/* A model of the host and four clients (two per team) in a capture the
 * flag game.  The host keeps a copy of each client's inventory; each
 * client's reports reach it late, some very late, and in between the
 * host grants flags, the clients die (their flag drops into the level),
 * drop them, and score.  After every step, every flag is in the level
 * or carried by exactly one player, as the host sees it; and a client's
 * own view agrees with the host's once every message has arrived.
 */
struct model_client
{
	std::uint8_t team{};
	bool alive{true};
	/* The client's own inventory and its count of grants received. */
	inventory own{};
	std::uint16_t applied{};
	/* The client's life (its deaths), as the host's copy counts them. */
	std::uint8_t life{};
	/* What is on the way to the host (reports) and to the client
	 * (grants: the descriptor and the outcome; captures, as the "take"
	 * of a flag).
	 */
	struct report
	{
		inventory inv;
		std::uint16_t applied;
	};
	std::deque<report> to_host;
	struct to_client_msg
	{
		bool capture;
		pickup_desc desc;
		pickup_outcome outcome;
		std::uint8_t life;
	};
	std::deque<to_client_msg> to_client;
};

void test_model()
{
	constexpr unsigned CLIENTS{4};
	for (unsigned seed = 1; seed <= 200; ++seed)
	{
		std::mt19937 rng{seed};
		std::array<model_client, CLIENTS> c;
		std::array<inventory_mirror, CLIENTS> host;
		for (unsigned i = 0; i < CLIENTS; ++i)
		{
			c[i].team = static_cast<std::uint8_t>(i & 1);
			host[i].reset({});
		}
		std::array<unsigned, CTF_TEAMS> in_level{{1, 1}};
		unsigned captures{0};
		const auto census{[&] {
			flag_census f;
			for (std::size_t t = 0; t < CTF_TEAMS; ++t)
				f.in_level[t] = static_cast<std::uint8_t>(in_level[t]);
			for (unsigned i = 0; i < CLIENTS; ++i)
				if (host[i].current().powerup_flags & FLAG_TEAM)
					++f.carried[other_team(c[i].team)];
			return f;
		}};
		for (unsigned step = 0; step < 4000; ++step)
		{
			const unsigned i{static_cast<unsigned>(rng() % CLIENTS)};
			auto &cl{c[i]};
			auto &mirror{host[i]};
			const auto rules{ctf_rules(cl.team)};
			const std::uint8_t enemy_flag{other_team(cl.team)};
			switch (rng() % 8)
			{
				case 0:
					/* The client touches the enemy flag in the level
					 * and asks (only if it carries none itself): the
					 * host grants it if it lies there and its copy
					 * carries none either.
					 */
					if (cl.alive && in_level[enemy_flag] && !(cl.own.powerup_flags & FLAG_TEAM) && !(mirror.current().powerup_flags & FLAG_TEAM))
					{
						const auto d{flag_desc(enemy_flag)};
						const auto o{evaluate_pickup(mirror.current(), rules, d, 0)};
						CHECK(o.usable);
						mirror.on_grant(rules, d, o);
						--in_level[enemy_flag];
						cl.to_client.push_back({false, d, o, mirror.life()});
					}
					break;
				case 1:
					/* The client reports (now or after a delay). */
					cl.to_host.push_back({cl.own, cl.applied});
					break;
				case 2:
				case 3:
					/* A report arrives, sometimes not the oldest
					 * (a very late one overtaken does not happen on an
					 * ordered channel, but a report always describes
					 * the past).
					 */
					if (!cl.to_host.empty())
					{
						const auto r{cl.to_host.front()};
						cl.to_host.pop_front();
						mirror.on_report(rules, r.inv, r.applied);
					}
					break;
				case 4:
					/* A message reaches the client. */
					if (!cl.to_client.empty())
					{
						const auto m{cl.to_client.front()};
						cl.to_client.pop_front();
						if (m.capture)
							cl.own.powerup_flags &= ~FLAG_TEAM;
						else
						{
							++cl.applied;
							/* A grant for an old life is not applied. */
							if (cl.alive && m.life == cl.life)
								apply_pickup(cl.own, rules, m.desc, m.outcome);
						}
					}
					break;
				case 5:
					/* The host decides the client's death and drops
					 * what its copy holds; the client respawns empty.
					 */
					if (cl.alive)
					{
						if (mirror.current().powerup_flags & FLAG_TEAM)
							++in_level[enemy_flag];
						mirror.clear();
						cl.own = {};
						++cl.life;
						/* Grants for the old life are not applied (the
						 * life rule); they still count.
						 */
						cl.alive = (rng() & 1) != 0;
					}
					else
						cl.alive = true;
					break;
				case 6:
					/* The client is in its goal: the host decides from
					 * its copy.
					 */
					if (capture_due({cl.alive, cl.team, (mirror.current().powerup_flags & FLAG_TEAM) != 0, cl.team}))
					{
						mirror.take_flags(FLAG_TEAM);
						++in_level[enemy_flag];
						++captures;
						cl.to_client.push_back({true, {}, {}, mirror.life()});
					}
					break;
				case 7:
					/* The client drops the flag it has (DROP_REQUEST):
					 * the host spits it if its copy has it (not if it
					 * was captured meanwhile: the request is refused).
					 */
					if (cl.alive && (cl.own.powerup_flags & FLAG_TEAM))
					{
						cl.own.powerup_flags &= ~FLAG_TEAM;
						if (!evaluate_drop(mirror.current(), rules, flag_desc(enemy_flag), 0))
							break;
						cl.to_host.push_back({cl.own, cl.applied});
						mirror.on_drop(rules, flag_desc(enemy_flag), 0);
						++in_level[enemy_flag];
					}
					break;
			}
			const auto f{census()};
			CHECK(f.holds({{1, 1}}));
		}
		/* Everything arrives: the clients agree with the host. */
		for (unsigned i = 0; i < CLIENTS; ++i)
		{
			auto &cl{c[i]};
			while (!cl.to_client.empty())
			{
				const auto m{cl.to_client.front()};
				cl.to_client.pop_front();
				if (m.capture)
					cl.own.powerup_flags &= ~FLAG_TEAM;
				else
				{
					++cl.applied;
					if (cl.alive && m.life == cl.life)
						apply_pickup(cl.own, ctf_rules(cl.team), m.desc, m.outcome);
				}
			}
			while (!cl.to_host.empty())
			{
				host[i].on_report(ctf_rules(cl.team), cl.to_host.front().inv, cl.to_host.front().applied);
				cl.to_host.pop_front();
			}
			host[i].on_report(ctf_rules(cl.team), cl.own, cl.applied);
			if (cl.alive)
				CHECK(((host[i].current().powerup_flags & FLAG_TEAM) != 0) == ((cl.own.powerup_flags & FLAG_TEAM) != 0));
		}
		CHECK(census().holds({{1, 1}}));
		CHECK(captures > 0);
	}
}

void test_classic_rules()
{
	/* The bits: without "classic" nothing applies. */
	CHECK(!ctf_rules::from_bits(CTF_RULE_DROPPED_RETURNS | CTF_RULE_TOUCH_RETURNS | CTF_RULE_HOME_TO_SCORE).classic);
	CHECK(!ctf_rules::from_bits(CTF_RULE_DROPPED_RETURNS | CTF_RULE_TOUCH_RETURNS | CTF_RULE_HOME_TO_SCORE).touch_returns);
	for (unsigned bits = 0; bits < 16; ++bits)
	{
		const auto r{ctf_rules::from_bits(static_cast<std::uint8_t>(bits))};
		const bool c{(bits & CTF_RULE_CLASSIC) != 0};
		CHECK(r.classic == c);
		CHECK(r.dropped_returns == (c && (bits & CTF_RULE_DROPPED_RETURNS)));
		CHECK(r.touch_returns == (c && (bits & CTF_RULE_TOUCH_RETURNS)));
		CHECK(r.home_to_score == (c && (bits & CTF_RULE_HOME_TO_SCORE)));
		CHECK(dropped_flag_goes_home(r) == r.dropped_returns);
		/* The idle return only where nothing else returns a flag and a
		 * team needs its own flag home.
		 */
		CHECK(idle_flag_returns(r) == (r.home_to_score && !r.touch_returns && !r.dropped_returns));
		CHECK(flag_respawns_home(r) == c);
		/* Touching flags: the other team's is taken unless one is
		 * carried; the own one goes home only with the rule and away
		 * from home.
		 */
		for (const bool at_home : {false, true})
			for (const bool carries : {false, true})
			{
				CHECK(evaluate_flag_touch(r, CTF_TEAM_BLUE, CTF_TEAM_RED, at_home, carries) == (carries ? flag_touch::none : flag_touch::take));
				CHECK(evaluate_flag_touch(r, CTF_TEAM_RED, CTF_TEAM_BLUE, at_home, carries) == (carries ? flag_touch::none : flag_touch::take));
				CHECK(evaluate_flag_touch(r, CTF_TEAM_BLUE, CTF_TEAM_BLUE, at_home, carries) == (r.touch_returns && !at_home ? flag_touch::return_home : flag_touch::none));
			}
		/* Captures: only a carrier in its own goal; with home_to_score
		 * only while its own flag is at home.
		 */
		const capture_check in_goal{true, CTF_TEAM_RED, true, CTF_TEAM_RED};
		CHECK(evaluate_capture(r, in_goal, true) == capture_verdict::score);
		CHECK(evaluate_capture(r, in_goal, false) == (r.home_to_score ? capture_verdict::own_flag_away : capture_verdict::score));
		CHECK(evaluate_capture(r, {true, CTF_TEAM_RED, true, CTF_TEAM_BLUE}, true) == capture_verdict::none);
		CHECK(evaluate_capture(r, {true, CTF_TEAM_RED, false, CTF_TEAM_RED}, false) == capture_verdict::none);
		CHECK(evaluate_capture(r, {false, CTF_TEAM_RED, true, CTF_TEAM_RED}, false) == capture_verdict::none);
	}
	CHECK(ctf_rules::from_bits(CTF_RULE_CLASSIC | CTF_RULES_DEFAULT).touch_returns);
	CHECK(ctf_rules::from_bits(CTF_RULE_CLASSIC | CTF_RULES_DEFAULT).home_to_score);
	CHECK(!ctf_rules::from_bits(CTF_RULE_CLASSIC | CTF_RULES_DEFAULT).dropped_returns);
}

void test_choose_home()
{
	CHECK(!choose_home({}));
	const std::array<goal_segment, 1> one{{{17, 5}}};
	CHECK(choose_home(one) == 17);
	/* The largest; the lowest number of equal ones, in any order. */
	const std::array<goal_segment, 4> some{{{40, 9}, {12, 30}, {8, 30}, {3, 29}}};
	CHECK(choose_home(some) == 8);
	const std::array<goal_segment, 4> reversed{{{3, 29}, {8, 30}, {12, 30}, {40, 9}}};
	CHECK(choose_home(reversed) == 8);
}

void test_notice_wire()
{
	for (const auto kind : {ctf_notice_kind::returned, ctf_notice_kind::own_flag_away})
		for (const std::uint8_t pid : {std::uint8_t{0}, std::uint8_t{7}, NET_V2_PLAYER_ID_NONE})
		{
			const ctf_notice_msg m{kind, CTF_TEAM_RED, pid};
			std::array<std::uint8_t, ctf_notice_msg::SIZE> buf;
			m.write(buf);
			const auto r{ctf_notice_msg::read(buf)};
			/* "Your flag must be home" is for a player. */
			if (kind == ctf_notice_kind::own_flag_away && pid == NET_V2_PLAYER_ID_NONE)
			{
				CHECK(!r);
				continue;
			}
			CHECK(r && r->kind == kind && r->team == CTF_TEAM_RED && r->pid == pid);
		}
	std::array<std::uint8_t, ctf_notice_msg::SIZE> bad{{2, 0, 0}};
	CHECK(!ctf_notice_msg::read(bad));
	bad = {{0, 2, 0}};
	CHECK(!ctf_notice_msg::read(bad));
	bad = {{0, 0, 8}};
	CHECK(!ctf_notice_msg::read(bad));
	CHECK(!ctf_notice_msg::read(std::span<const std::uint8_t>(bad).first(2)));
}

void test_own_flag_pickup()
{
	auto r{ctf_rules(CTF_TEAM_BLUE)};
	/* Its own (blue) flag: not usable without the rule. */
	CHECK(!evaluate_pickup({}, r, flag_desc(CTF_TEAM_BLUE), 0).usable);
	r.own_flag_returns = true;
	const auto o{evaluate_pickup({}, r, flag_desc(CTF_TEAM_BLUE), 0)};
	CHECK(o.usable && o.consumed);
	/* A return gives nothing. */
	inventory inv;
	apply_pickup(inv, r, flag_desc(CTF_TEAM_BLUE), o);
	CHECK(!(inv.powerup_flags & FLAG_TEAM));
	/* Even while carrying the other flag. */
	inv.powerup_flags = FLAG_TEAM;
	CHECK(evaluate_pickup(inv, r, flag_desc(CTF_TEAM_BLUE), 0).usable);
	/* The other team's flag as before. */
	CHECK(!evaluate_pickup(inv, r, flag_desc(CTF_TEAM_RED), 0).usable);
	CHECK(evaluate_pickup({}, r, flag_desc(CTF_TEAM_RED), 0).usable);
	CHECK(units_carried(inv, r, flag_desc(CTF_TEAM_BLUE)) == 0);
}

/* Capture the flag (Classic), every combination of the options: two
 * teams, two players each; flags are at home, carried or dropped.  The
 * model plays touches, deaths, captures and the time the flags lie, and
 * checks after every step that each flag is in exactly one place, that
 * a capture puts the flag home, that a capture happens only with the
 * scoring team's flag at home when the rule says so, and that a dropped
 * flag goes home at once when the rule says so.
 */
void test_classic_model()
{
	enum class place : std::uint8_t { home, carried, dropped };
	for (unsigned bits = 0; bits < 8; ++bits)
	{
		const auto r{ctf_rules::from_bits(static_cast<std::uint8_t>(CTF_RULE_CLASSIC | (bits << 1)))};
		for (unsigned seed = 1; seed <= 50; ++seed)
		{
			std::mt19937 rng{seed};
			std::array<place, CTF_TEAMS> where{{place::home, place::home}};
			/* Steps a flag lay dropped (the idle return). */
			std::array<unsigned, CTF_TEAMS> idle{};
			/* Who carries each flag (meaningful while carried). */
			std::array<unsigned, CTF_TEAMS> carrier{};
			unsigned captures{0}, refused{0}, returns{0};
			for (unsigned step = 0; step < 3000; ++step)
			{
				const unsigned p{static_cast<unsigned>(rng() % 4)};
				const std::uint8_t team{static_cast<std::uint8_t>(p & 1)};
				const std::uint8_t enemy{other_team(team)};
				const bool carries{where[enemy] == place::carried && carrier[enemy] == p};
				switch (rng() % 4)
				{
					case 0:
						/* Touches a flag where it lies. */
						{
							const std::uint8_t f{static_cast<std::uint8_t>(rng() & 1)};
							if (where[f] == place::carried)
								break;
							switch (evaluate_flag_touch(r, team, f, where[f] == place::home, carries))
							{
								case flag_touch::none:
									break;
								case flag_touch::take:
									CHECK(f == enemy);
									where[f] = place::carried;
									carrier[f] = p;
									break;
								case flag_touch::return_home:
									CHECK(f == team && where[f] == place::dropped && r.touch_returns);
									where[f] = place::home;
									++returns;
									break;
							}
						}
						break;
					case 1:
						/* Dies: a carried flag drops, or goes home. */
						if (carries)
							where[enemy] = dropped_flag_goes_home(r) ? place::home : place::dropped;
						break;
					case 2:
						/* In its own goal. */
						switch (evaluate_capture(r, {true, team, carries, team}, where[team] == place::home))
						{
							case capture_verdict::none:
								CHECK(!carries);
								break;
							case capture_verdict::score:
								CHECK(carries);
								CHECK(!r.home_to_score || where[team] == place::home);
								/* Classic: the captured flag goes home. */
								where[enemy] = flag_respawns_home(r) ? place::home : place::dropped;
								++captures;
								break;
							case capture_verdict::own_flag_away:
								CHECK(carries && r.home_to_score && where[team] != place::home);
								++refused;
								break;
						}
						break;
					case 3:
						/* Drops the flag by hand (it stays). */
						if (carries)
							where[enemy] = place::dropped;
						break;
				}
				/* A flag that lies dropped returns after a while when no
				 * rule would return it and both teams could otherwise be
				 * stuck (idle_flag_returns).  Each flag is in one place,
				 * and a carried flag has exactly one carrier, of the other
				 * team.
				 */
				for (std::size_t f = 0; f < CTF_TEAMS; ++f)
				{
					if (where[f] == place::carried)
						CHECK((carrier[f] & 1) != f);
					if (where[f] != place::dropped)
						idle[f] = 0;
					else if (++idle[f] >= 200 && idle_flag_returns(r))
					{
						where[f] = place::home;
						idle[f] = 0;
						++returns;
					}
				}
			}
			/* With only the idle return, the random players of the model
			 * pick a lying flag up again long before it returns: a team
			 * there needs to guard its dropped flag, which the model
			 * does not play.
			 */
			if (!idle_flag_returns(r))
				CHECK(captures > 0);
			if (r.home_to_score && !r.dropped_returns)
				CHECK(refused > 0);
			if (r.touch_returns && !r.dropped_returns)
				CHECK(returns > 0);
		}
	}
}

}

int main()
{
	test_capture_rule();
	test_scores();
	test_census();
	test_capture_wire();
	test_host_owned_flags();
	test_model();
	test_classic_rules();
	test_choose_home();
	test_notice_wire();
	test_own_flag_pickup();
	test_classic_model();
	std::puts("net_v2_modes: all checks passed");
	return 0;
}
