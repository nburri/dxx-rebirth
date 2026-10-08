/*
 * This file is part of the DXX-Rebirth project <https://github.com/dxx-rebirth/dxx-rebirth/>.
 * It is copyright by its individual contributors, as recorded in the
 * project's Git history.  See COPYING.txt at the top level for license
 * terms and a link to the Git history.
 */
/*
 * Test of the capture-the-flag cues (ctf_cues.h): what a listener of
 * either team hears for each event of either flag (its side's cue, the
 * original voice after it), the sound ids, the conversion to the game's
 * 8-bit sounds, and the bundled files (data/sounds: they decode, are
 * mono, 0.5 to 1.5 s long, at the loudness of the game's voices).  The
 * argument is the directory of the top of the tree (default: the current
 * directory).
 */

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <set>
#include <string>
#include <vector>

#include "ctf_cues.h"
#include "taunt_sample.h"

using namespace dcx::ctf_cues;

namespace {

void check_failed(const char *const expr, const char *const file, const int line)
{
	std::fprintf(stderr, "%s:%d: check failed: %s\n", file, line, expr);
	std::exit(1);
}

#define CHECK(cond)	do { if (!(cond)) check_failed(#cond, __FILE__, __LINE__); } while (0)

std::string top{"."};

constexpr std::uint8_t BLUE{TEAM_BLUE}, RED{TEAM_RED};

/* The perspective: the same event sounds different to the two teams,
 * and the voice names the team that acted (or "you").
 */
void test_mapping()
{
	/* Red takes the blue flag. */
	CHECK((announce(event::flag_taken, BLUE, BLUE, false) == announcement{cue::own_flag_taken, voice::red_got_flag}));
	CHECK((announce(event::flag_taken, BLUE, RED, false) == announcement{cue::enemy_flag_taken, voice::red_got_flag}));
	CHECK((announce(event::flag_taken, BLUE, RED, true) == announcement{cue::enemy_flag_taken, voice::you_got_flag}));
	/* Blue takes the red flag. */
	CHECK((announce(event::flag_taken, RED, RED, false) == announcement{cue::own_flag_taken, voice::blue_got_flag}));
	CHECK((announce(event::flag_taken, RED, BLUE, false) == announcement{cue::enemy_flag_taken, voice::blue_got_flag}));
	CHECK((announce(event::flag_taken, RED, BLUE, true) == announcement{cue::enemy_flag_taken, voice::you_got_flag}));

	/* Drops and returns: no voice in the original game. */
	CHECK((announce(event::flag_dropped, BLUE, BLUE, false) == announcement{cue::own_flag_dropped, voice::none}));
	CHECK((announce(event::flag_dropped, BLUE, RED, false) == announcement{cue::enemy_flag_dropped, voice::none}));
	CHECK((announce(event::flag_dropped, RED, BLUE, true) == announcement{cue::enemy_flag_dropped, voice::none}));
	CHECK((announce(event::flag_returned, RED, RED, false) == announcement{cue::own_flag_returned, voice::none}));
	CHECK((announce(event::flag_returned, RED, BLUE, false) == announcement{cue::enemy_flag_returned, voice::none}));
	/* Returned by the listener itself: still its own flag's cue. */
	CHECK((announce(event::flag_returned, BLUE, BLUE, true) == announcement{cue::own_flag_returned, voice::none}));

	/* Red captures the blue flag. */
	CHECK((announce(event::capture, BLUE, RED, false) == announcement{cue::we_scored, voice::red_scored}));
	CHECK((announce(event::capture, BLUE, RED, true) == announcement{cue::we_scored, voice::you_scored}));
	CHECK((announce(event::capture, BLUE, BLUE, false) == announcement{cue::they_scored, voice::red_scored}));
	/* Blue captures the red flag. */
	CHECK((announce(event::capture, RED, BLUE, false) == announcement{cue::we_scored, voice::blue_scored}));
	CHECK((announce(event::capture, RED, RED, false) == announcement{cue::they_scored, voice::blue_scored}));

	/* Every event: the two teams hear different cues; one team hears
	 * different cues for the two flags; every cue is reachable.
	 */
	std::set<cue> heard;
	for (const auto e : {event::flag_taken, event::flag_dropped, event::flag_returned, event::capture})
		for (const std::uint8_t flag : {BLUE, RED})
		{
			const auto blue{announce(e, flag, BLUE, false)}, red{announce(e, flag, RED, false)};
			CHECK(blue.sound != red.sound);
			/* The voice is the same for both teams (it names a team). */
			CHECK(blue.then == red.then);
			CHECK(announce(e, flag, BLUE, false).sound != announce(e, other_team(flag), BLUE, false).sound);
			heard.insert(blue.sound);
			heard.insert(red.sound);
		}
	CHECK(heard.size() == CUES);
}

/* The ids: distinct, below the game's 254 sounds, none DESCENT2.HAM
 * maps (its unmapped ids; the hoard takes 84 to 87).
 */
void test_ids()
{
	constexpr std::array<std::uint8_t, 58> ham_free{{0, 78, 79, 84, 85, 86, 87, 88, 89, 90, 96, 97, 98, 99, 110, 111, 112, 116, 119, 120, 131, 134, 135, 136, 137, 138, 139, 159, 164, 165, 166, 174, 175, 177, 179, 182, 189, 191, 198, 199, 206, 207, 208, 210, 212, 213, 214, 215, 216, 217, 218, 228, 229, 234, 239, 243, 252, 253}};
	std::set<std::uint8_t> ids;
	for (const auto id : SOUND_IDS)
	{
		CHECK(id < 254);
		CHECK(id < 84 || id > 87);
		CHECK(std::ranges::find(ham_free, id) != ham_free.end());
		ids.insert(id);
	}
	CHECK(ids.size() == CUES);
	std::set<std::string> files(FILES.begin(), FILES.end());
	CHECK(files.size() == CUES);
}

void test_conversion()
{
	const std::vector<float> s{0.0f, 1.0f, -1.0f, 0.5f, -0.5f, 2.0f, NAN, 0.0f};
	const auto full{to_game_sound(s, false)};
	CHECK(full.size() == s.size());
	CHECK(full[0] == 128);
	CHECK(full[1] == 255);
	CHECK(full[2] == 0);
	CHECK(full[3] == 192);
	CHECK(full[4] == 64);
	CHECK(full[5] == 255);
	CHECK(full[6] == 128);
	const auto half{to_game_sound(s, true)};
	CHECK(half.size() == 4);
	/* The average of each pair. */
	CHECK(half[0] == 192);	/* 0.5 */
	CHECK(half[1] == 96);	/* -0.25 */
	CHECK(half[2] == 224);	/* 0.75 */
	CHECK(half[3] == 128);	/* not a number: silence */
	/* Cut to MAX_SECONDS. */
	const std::vector<float> longer(22050 * (MAX_SECONDS + 1), 0.25f);
	CHECK(to_game_sound(longer, false).size() == 22050 * MAX_SECONDS);
	CHECK(to_game_sound(longer, true).size() == 22050 * MAX_SECONDS / 2);
}

/* The files.  Their loudness is made to the game's voices
 * (data/sounds/src/ctf_cues.py: the A-weighted RMS, 95th percentile of
 * 50 ms windows, 0.355); unweighted, as taunt_sample.h measures it, it
 * depends on the timbre, within 0.2 to 0.5.
 */
void test_files()
{
	for (const auto name : FILES)
	{
		const std::string path{top + "/data/" + name};
		std::ifstream f{path, std::ios::binary};
		if (!f)
		{
			std::fprintf(stderr, "cannot read %s\n", path.c_str());
			std::exit(1);
		}
		const std::vector<std::uint8_t> bytes{std::istreambuf_iterator<char>{f}, std::istreambuf_iterator<char>{}};
		CHECK(bytes.size() <= MAX_FILE_SIZE);
		std::string error;
		const auto a{dcx::taunt::decode_audio_file(bytes, error)};
		CHECK(a.has_value());
		CHECK(a->rate == 22050);
		const double seconds{static_cast<double>(a->samples.size()) / a->rate};
		CHECK(seconds >= 0.5 && seconds <= 1.5);
		float peak{};
		for (const float v : a->samples)
			peak = std::max(peak, std::fabs(v));
		CHECK(peak <= 0.98f);
		CHECK(peak >= 0.45f);
		const float loud{dcx::taunt::loudness(a->samples)};
		CHECK(loud >= 0.2f && loud <= 0.5f);
		/* No click: it starts and ends near silence. */
		CHECK(std::fabs(a->samples.front()) < 0.01f);
		CHECK(std::fabs(a->samples.back()) < 0.01f);
		const auto game{to_game_sound(a->samples, false)};
		CHECK(game.size() == a->samples.size());
		std::printf("ctf cue %-34s %.2f s, peak %.2f, loudness %.2f\n", name, seconds, peak, loud);
	}
}

}

int main(const int argc, char **const argv)
{
	if (argc > 1)
		top = argv[1];
	test_mapping();
	test_ids();
	test_conversion();
	test_files();
	std::puts("ctf cues: all checks passed");
	return 0;
}
