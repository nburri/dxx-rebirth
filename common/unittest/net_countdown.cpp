/*
 * This file is part of the DXX-Rebirth project <https://github.com/dxx-rebirth/dxx-rebirth/>.
 * It is copyright by its individual contributors, as recorded in the
 * project's Git history.  See COPYING.txt at the top level for license
 * terms and a link to the Git history.
 */
/*
 * Test of the reactor countdown in a network game (net_countdown.h,
 * Documentation/network-protocol-v2.md section 4.7): a kill during the
 * countdown, and a playing client's countdown following the host's.
 *
 * Build and run with SCons:
 *
 *	scons sdl2=1 d1x=0 d2x=1 register_runtime_test_plain_link_targets=1 test-net-countdown
 *	build/common/test-net-countdown
 */

#include <cstdint>
#include <cstdio>
#include <cstdlib>

#include "net_countdown.h"

using namespace dcx::net_v2;

namespace {

void check_failed(const char *const expr, const char *const file, const int line)
{
	std::fprintf(stderr, "%s:%d: check failed: %s\n", file, line, expr);
	std::exit(1);
}

#define CHECK(cond)	do { if (!(cond)) check_failed(#cond, __FILE__, __LINE__); } while (0)

constexpr std::int32_t F1{COUNTDOWN_F1_0};

/* The game's countdown for a machine whose player is `playing` while
 * `playing_until` (seconds of game time) and whose timer is set by the
 * host's status every second when `follow_host`.  Returns the seconds
 * shown at `end` (the HUD's T- value).
 */
struct machine
{
	std::int32_t timer;
	bool playing{true};
	void frame(const std::int32_t frame_time)
	{
		/* do_controlcen_dead_frame: only a playing player's countdown runs. */
		if (playing)
			timer -= frame_time;
	}
	std::int32_t seconds() const
	{
		return countdown_seconds_of_timer(timer);
	}
};

void test_kill_marks()
{
	/* The bug: the local victim marked at the kill. */
	CHECK(!kill_marks_died_in_mine(true, true));
	CHECK(kill_marks_died_in_mine(true, false));
	CHECK(!kill_marks_died_in_mine(false, false));
	CHECK(!kill_marks_died_in_mine(false, true));
}

/* The reported game: a 60 s countdown (D2, Rookie); two players are killed
 * during it, at different times.  Before: each victim's own machine marked
 * itself died in the mine at the kill, kill_local_ship refused to start the
 * death sequence (it needs `playing`) and the countdown stopped there: 50 s
 * on one machine, 42 s on the other, and the exit trigger ignored them.
 */
void test_kill_during_countdown()
{
	constexpr std::int32_t frame{F1 / 50};
	for (const bool fixed : {false, true})
	{
		machine a{.timer = 60 * F1}, b{.timer = 60 * F1};
		for (int f = 0; f < 50 * 70; ++f)
		{
			const int t{f / 50};
			/* Killed at 10 s and 18 s into the countdown: each machine
			 * marks its own player (before: always; now: as the rule
			 * says for the local victim).
			 */
			const bool marks{fixed ? kill_marks_died_in_mine(true, true) : true};
			if (f == 50 * 10 && marks)
				a.playing = false;
			if (f == 50 * 18 && marks)
				b.playing = false;
			a.frame(frame);
			b.frame(frame);
			if (!fixed && t == 30)
			{
				CHECK(a.seconds() == 50);
				CHECK(b.seconds() == 42);
			}
		}
		if (fixed)
		{
			CHECK(a.timer <= 0);
			CHECK(b.timer <= 0);
		}
	}
}

void test_seconds()
{
	CHECK(countdown_seconds_of_timer(60 * F1) == 60);
	CHECK(countdown_seconds_of_timer(60 * F1 - F1 * 7 / 8) == 60);
	CHECK(countdown_seconds_of_timer(60 * F1 - F1 * 7 / 8 - 1) == 59);
	for (std::int32_t s = 0; s <= 120; ++s)
		CHECK(countdown_seconds_of_timer(countdown_timer_of_seconds(s)) == s);
}

void test_correction()
{
	/* Equal, or one second apart (the status's age): left alone. */
	CHECK(!countdown_correction(countdown_timer_of_seconds(40), 40, true));
	CHECK(!countdown_correction(countdown_timer_of_seconds(40), 39, true));
	CHECK(!countdown_correction(countdown_timer_of_seconds(40), 41, true));
	/* The host is ahead: down, whether the host plays or not. */
	CHECK(countdown_correction(countdown_timer_of_seconds(40), 30, true) == countdown_timer_of_seconds(30));
	CHECK(countdown_correction(countdown_timer_of_seconds(40), 30, false) == countdown_timer_of_seconds(30));
	/* The host is behind: up only while its own countdown runs. */
	CHECK(countdown_correction(countdown_timer_of_seconds(30), 40, true) == countdown_timer_of_seconds(40));
	CHECK(!countdown_correction(countdown_timer_of_seconds(30), 40, false));
	/* No countdown on the host, or the mine already blew up here. */
	CHECK(!countdown_correction(countdown_timer_of_seconds(30), COUNTDOWN_NONE, true));
	CHECK(!countdown_correction(0, 20, true));
	CHECK(!countdown_correction(-F1, 20, true));
	/* The host at 0: the mine blows up here too. */
	CHECK(*countdown_correction(countdown_timer_of_seconds(5), 0, true) <= 0);
}

/* A client whose countdown stopped (or lags) follows the host's, which
 * the status brings every second; a client that runs ahead of a host on
 * the score screen is never put back.
 */
void test_follow_host()
{
	constexpr std::int32_t frame{F1 / 50};
	machine host{.timer = 60 * F1}, client{.timer = 60 * F1};
	client.playing = false;	/* stopped, as with the bug */
	bool exploded{};
	for (int f = 0; f < 50 * 70 && !exploded; ++f)
	{
		host.frame(frame);
		client.frame(frame);
		if (f % 50 == 49)
		{
			if (const auto t{countdown_correction(client.timer, static_cast<std::uint8_t>(host.seconds()), true)})
				client.timer = *t;
			CHECK(client.seconds() - host.seconds() <= 1 && host.seconds() - client.seconds() <= 1);
		}
		exploded = client.timer <= 0;
	}
	CHECK(exploded);
}

void test_overdue()
{
	CHECK(!countdown_overdue(0, 60));
	CHECK(!countdown_overdue(std::int64_t{65} * F1, 60));
	CHECK(countdown_overdue(std::int64_t{65} * F1 + 1, 60));
}

/* The second report: one player escapes at once and looks at the score
 * screen, the other stays in the level.  The host that escaped takes the
 * lowest of the clients' reports (receive_endlevel_client); the client's
 * game stalls for 20 s (its frame time stands still).  The client's
 * countdown and so the host's stop; the host's backstop ends it, the
 * client follows the host's 0 and blows up, and the level ends for both.
 */
void test_escaped_host_backstop()
{
	constexpr std::int32_t frame{F1 / 50};
	machine client{.timer = 60 * F1};
	std::int32_t host_seconds{60};
	bool client_out{};
	int ended_at{};
	for (int f = 0; f < 50 * 120 && !client_out; ++f)
	{
		const std::int64_t real{std::int64_t{f} * frame};
		/* The client's game stands still from 10 s to 30 s. */
		if (!(f >= 50 * 10 && f < 50 * 30))
			client.frame(frame);
		if (f % 50 == 49)
		{
			/* The client's report: the host keeps the lowest. */
			if (client.seconds() < host_seconds)
				host_seconds = client.seconds();
			if (host_seconds > 0 && countdown_overdue(real, 60))
				host_seconds = 0;
			/* The host's status (the host does not play). */
			if (const auto t{countdown_correction(client.timer, static_cast<std::uint8_t>(host_seconds), false)})
				client.timer = *t;
		}
		if (client.timer <= 0)
		{
			client_out = true;
			ended_at = f / 50;
		}
	}
	CHECK(client_out);
	/* Ended by the backstop at 65 s, not at 80 s by the stalled game. */
	CHECK(ended_at >= 65 && ended_at <= 66);
}

}

int main()
{
	test_kill_marks();
	test_kill_during_countdown();
	test_seconds();
	test_correction();
	test_follow_host();
	test_overdue();
	test_escaped_host_backstop();
	std::puts("test-net-countdown: ok");
	return 0;
}
