/*
 * This file is part of the DXX-Rebirth project <https://github.com/dxx-rebirth/dxx-rebirth/>.
 * It is copyright by its individual contributors, as recorded in the
 * project's Git history.  See COPYING.txt at the top level for license
 * terms and a link to the Git history.
 */
/*
 * movrec-analyse: from movement recordings to bot style profiles (step 2
 * of Documentation/movement-recording.md).
 *
 *	movrec-analyse [--out DIR] [--player CALLSIGN]... [--bots]
 *	               [--skill NAME] [--min-seconds N] FILE...
 *
 * Reads the recordings, puts those of one game made on several machines
 * together (each player's own recording is used for that player, for its
 * exact controls), and prints per player a report: the key traits in
 * words, the numbers behind them, and the proposed bot style.  With
 * --out it writes DIR/<callsign>.botstyle per player and
 * DIR/<callsign>.report.txt; without, the profile follows the report on
 * the standard output.
 *
 * Build: scons sdl2=1 d1x=0 d2x=1 register_runtime_test_plain_link_targets=1 movrec-analyse
 * Binary: build/common/movrec-analyse
 */

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "movement_analysis.h"

using namespace dcx::movrec;
using namespace dcx::movrec::analysis;

namespace {

struct options
{
	const char *out_dir{};
	std::vector<std::string> only;
	bool bots{};
	dcx::bot::bot_skill skill{dcx::bot::BOT_DEFAULT_SKILL};
	double min_seconds{limits::MIN_ALIVE_S};
};

std::string safe_name(const std::string &s)
{
	std::string r;
	for (const char c : s)
		r += (std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_') ? c : '_';
	return r.empty() ? std::string{"player"} : r;
}

bool write_text(const std::string &path, const std::string &text)
{
	std::FILE *const f{std::fopen(path.c_str(), "w")};
	if (!f)
	{
		std::fprintf(stderr, "%s: cannot write\n", path.c_str());
		return false;
	}
	const bool ok{std::fwrite(text.data(), 1, text.size(), f) == text.size()};
	return !std::fclose(f) && ok;
}

const char *clock_name(const clock_source c)
{
	switch (c)
	{
		case clock_source::own:
			return "its own clock";
		case clock_source::sync:
			return "the session clock (sync records)";
		case clock_source::trajectory:
			return "aligned by a shared player's path";
	}
	return "";
}

void usage()
{
	std::fputs("usage: movrec-analyse [--out DIR] [--player CALLSIGN]... [--bots] [--skill NAME] [--min-seconds N] FILE...\n"
		"  --out DIR          write DIR/<callsign>.botstyle and DIR/<callsign>.report.txt\n"
		"  --player CALLSIGN  only this player (may be given several times)\n"
		"  --bots             also the recorded bots (-recordmoves-bots)\n"
		"  --skill NAME       the skill the profile is scaled for: Trainee, Rookie, Hotshot (default), Ace, Insane\n"
		"  --min-seconds N    skip players alive for less than N seconds (default 20)\n", stderr);
}

}

int main(const int argc, char **const argv)
{
	options opt;
	std::vector<const char *> paths;
	for (int i{1}; i < argc; ++i)
	{
		const char *const a{argv[i]};
		if (!std::strcmp(a, "--out") && i + 1 < argc)
			opt.out_dir = argv[++i];
		else if (!std::strcmp(a, "--player") && i + 1 < argc)
			opt.only.push_back(lower(argv[++i]));
		else if (!std::strcmp(a, "--bots"))
			opt.bots = true;
		else if (!std::strcmp(a, "--min-seconds") && i + 1 < argc)
			opt.min_seconds = std::atof(argv[++i]);
		else if (!std::strcmp(a, "--skill") && i + 1 < argc)
		{
			const auto name{lower(argv[++i])};
			bool found{};
			for (std::size_t k{}; k != dcx::bot::BOT_SKILL_COUNT; ++k)
				if (lower(dcx::bot::bot_skill_names[k]) == name)
				{
					opt.skill = static_cast<dcx::bot::bot_skill>(k);
					found = true;
				}
			if (!found)
			{
				usage();
				return 2;
			}
		}
		else if (!std::strcmp(a, "-h") || !std::strcmp(a, "--help"))
		{
			usage();
			return 0;
		}
		else if (a[0] == '-' && a[1])
		{
			usage();
			return 2;
		}
		else
			paths.push_back(a);
	}
	if (paths.empty())
	{
		usage();
		return 2;
	}
	int rc{};
	std::vector<recording> files;
	for (const auto path : paths)
	{
		const auto data{load_file(path)};
		auto rec{data ? load_recording(*data, path) : std::nullopt};
		if (!rec)
		{
			std::fprintf(stderr, "%s: %s\n", path, data ? "not a movement recording, or its header is damaged" : "cannot read");
			rc = 1;
			continue;
		}
		const auto &st{rec->stats};
		std::printf("%s: format %u.%u, %u Hz, %.1f min, %u level(s), %u players%s%s%s\n", path, rec->header.version, rec->header.minor, rec->header.tick_rate, rec->end_ms / 60000.0,
			analysis::detail::count(rec->levels.size()), analysis::detail::count(rec->players.size()),
			rec->multiplayer() ? (rec->host() ? ", host" : ", client") : ", single player",
			st.clean_end ? "" : st.truncated ? ", cut short" : ", not closed",
			st.chunks_bad || st.sequence_gaps ? ", damaged chunks skipped" : "");
		files.push_back(std::move(*rec));
	}
	if (files.empty())
		return 1;
	const auto result{analyse_recordings(files, opt.skill)};
	std::printf("\n%u game(s):\n", analysis::detail::count(result.sessions.size()));
	for (const auto &ses : result.sessions)
	{
		const auto &first{files[ses.files.front().file]};
		std::printf("  \"%s\"%s", first.header.mission.c_str(), ses.files.size() > 1 ? ", recorded on several machines:\n" : ": ");
		for (const auto &sf : ses.files)
			std::printf("%s%s (%s)\n", ses.files.size() > 1 ? "    " : "", files[sf.file].name.c_str(), clock_name(sf.source));
	}
	for (const auto &n : result.notes)
		std::printf("  note: %s\n", n.c_str());
	for (const auto &pr : result.players)
	{
		const auto &s{pr.stats};
		if (s.bot && !opt.bots)
			continue;
		if (!opt.only.empty() && std::find(opt.only.begin(), opt.only.end(), lower(s.callsign)) == opt.only.end())
			continue;
		if (s.alive_s < opt.min_seconds)
		{
			std::printf("\n== %s ==\nskipped: alive for %.0f s only\n", s.callsign.c_str(), s.alive_s);
			continue;
		}
		const auto report{write_report(s, pr.profile)};
		const auto profile{dcx::bot::write_style_profile(pr.profile)};
		std::printf("\n%s", report.c_str());
		if (opt.out_dir)
		{
			const std::string base{std::string{opt.out_dir} + "/" + safe_name(s.callsign) + (s.bot ? "-bot" : "")};
			const std::string profile_path{base + std::string{dcx::bot::STYLE_PROFILE_EXTENSION}};
			if (write_text(profile_path, profile) && write_text(base + ".report.txt", report))
				std::printf("\nwritten: %s, %s.report.txt\n", profile_path.c_str(), base.c_str());
			else
				rc = 1;
		}
		else
			std::printf("\n--- %s%s ---\n%s", safe_name(s.callsign).c_str(), std::string{dcx::bot::STYLE_PROFILE_EXTENSION}.c_str(), profile.c_str());
	}
	return rc;
}
