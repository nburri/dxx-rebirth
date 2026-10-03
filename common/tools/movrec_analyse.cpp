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
 *	               [--skill NAME] [--min-seconds N] [--missions DIR]
 *	               [--fidelity PROFILE[@BOT]]... FILE...
 *
 * Reads the recordings, puts those of one game made on several machines
 * together (each player's own recording is used for that player, for its
 * exact controls), and prints per player a report: the key traits in
 * words, the numbers behind them, and the proposed bot style.  With
 * --out it writes DIR/<callsign>.botstyle per player and
 * DIR/<callsign>.report.txt; without, the profile follows the report on
 * the standard output.  With --missions it finds every recorded level in
 * the folder of missions (.hog and .mn2) and reports the traits per level
 * and room (section 8.8 of the document).  With --fidelity, every bot
 * reported is compared with the profile's measured values, trait by trait
 * (section 9.18 of Documentation/multiplayer-bots.md): PROFILE@BOT the
 * bot BOT, else a bot whose name starts with the profile's callsign's
 * first three letters, or every bot when none does.  A game of capture
 * the flag or hoard (format minor 6) gets a section of its own after the
 * list of games: per team the scores, per player (bots included) the
 * flags and orbs taken, carried, scored and lost and where it flew
 * (section 8.13).
 *
 * Build: scons sdl2=1 d1x=0 d2x=1 register_runtime_test_plain_link_targets=1 movrec-analyse
 * Binary: build/common/movrec-analyse
 */

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "movement_analysis.h"
#include "level_geometry.h"

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
	const char *missions{};
	/* The profile and the bot it is compared with (empty: by name). */
	std::vector<std::pair<dcx::bot::style_profile, std::string>> fidelity;
};

/* Mission files larger than this are not read (untrusted input). */
constexpr std::uintmax_t MAX_MISSION_FILE{256u << 20};

/* The missions of a folder (--missions): per .mn2 its .hog, read when a
 * level of it is asked for; the geometry of every level found, once.
 */
class mission_library
{
	std::vector<geometry::mission_entry> missions;
	std::map<std::string, std::unique_ptr<geometry::level_geometry>> cache;
	std::map<std::string, std::filesystem::path> hog_paths;
	static std::optional<std::vector<std::uint8_t>> read(const std::filesystem::path &p)
	{
		std::error_code ec;
		const auto size{std::filesystem::file_size(p, ec)};
		if (ec || size > MAX_MISSION_FILE)
			return std::nullopt;
		return load_file(p.string().c_str());
	}
public:
	/* The number of missions found. */
	std::size_t load(const char *const folder)
	{
		std::error_code ec;
		std::vector<std::filesystem::path> mn2s;
		std::filesystem::directory_iterator it{folder, ec};
		if (ec)
		{
			std::fprintf(stderr, "%s: cannot read the folder: %s\n", folder, ec.message().c_str());
			return 0;
		}
		for (const auto &e : it)
		{
			if (!e.is_regular_file(ec))
				continue;
			const auto ext{geometry::lower(e.path().extension().string())};
			if (ext == ".hog")
				hog_paths[geometry::lower(e.path().stem().string())] = e.path();
			else if (ext == ".mn2" || ext == ".msn")
				mn2s.push_back(e.path());
		}
		std::sort(mn2s.begin(), mn2s.end());
		for (const auto &p : mn2s)
		{
			const auto text{read(p)};
			if (!text)
				continue;
			geometry::mission_entry m;
			m.stem = p.stem().string();
			m.info = geometry::parse_mission(std::string_view{reinterpret_cast<const char *>(text->data()), text->size()});
			if (const auto h{hog_paths.find(geometry::lower(m.stem))}; h != hog_paths.end())
				m.hog_name = h->second.filename().string();
			missions.push_back(std::move(m));
		}
		return missions.size();
	}
	/* The geometry of a recorded level, or nothing; `note` says how it
	 * was found or why not.
	 */
	const geometry::level_geometry *find(const level_record &rec, std::string &note)
	{
		const geometry::level_query q{rec.mission, rec.mission_file, rec.level_file, rec.level_num, rec.segments};
		auto found{geometry::find_level(missions, q, [this](geometry::mission_entry &m) {
			const auto h{hog_paths.find(geometry::lower(m.stem))};
			if (h == hog_paths.end())
				return;
			if (auto bytes{read(h->second)})
				if (auto dir{geometry::read_hog(*bytes)})
				{
					m.hog = std::move(*bytes);
					m.dir = std::move(*dir);
				}
		}, MAX_FILE_NAME)};
		note = std::move(found.note);
		if (!found.mesh)
			return nullptr;
		const auto key{geometry::lower(found.mission->stem) + ":" + geometry::lower(found.level_file)};
		auto &slot{cache[key]};
		if (!slot)
			slot = std::make_unique<geometry::level_geometry>(geometry::measure(std::move(*found.mesh), "level " + std::to_string(rec.level_num) + " \"" + rec.level_name + "\" of \"" + rec.mission + "\"", found.mission->hog_name + ": " + found.level_file));
		return slot.get();
	}
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
	std::fputs("usage: movrec-analyse [--out DIR] [--player CALLSIGN]... [--bots] [--skill NAME] [--min-seconds N] [--missions DIR] FILE...\n"
		"  --out DIR          write DIR/<callsign>.botstyle and DIR/<callsign>.report.txt\n"
		"  --player CALLSIGN  only this player (may be given several times)\n"
		"  --bots             also the recorded bots (-recordmoves-bots)\n"
		"  --skill NAME       the skill the profile is scaled for: Trainee, Rookie, Hotshot (default), Ace, Insane\n"
		"  --min-seconds N    skip players alive for less than N seconds (default 20)\n"
		"  --missions DIR     the folder of the missions (.hog, .mn2): traits per level and room\n"
		"  --fidelity FILE[@BOT]  compare the bot BOT (else the bots named like the profile) with this profile (repeatable)\n", stderr);
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
		else if (!std::strcmp(a, "--missions") && i + 1 < argc)
			opt.missions = argv[++i];
		else if (!std::strcmp(a, "--fidelity") && i + 1 < argc)
		{
			std::string spec{argv[++i]};
			std::string bot;
			if (const auto at{spec.rfind('@')}; at != std::string::npos && spec.find('/', at) == std::string::npos)
			{
				bot = lower(spec.substr(at + 1));
				spec.resize(at);
			}
			const char *const path{spec.c_str()};
			const auto data{load_file(path)};
			auto p{data ? dcx::bot::parse_style_profile(std::string_view{reinterpret_cast<const char *>(data->data()), data->size()}) : std::nullopt};
			if (!p)
			{
				std::fprintf(stderr, "%s: %s\n", path, data ? "not a style profile" : "cannot read");
				return 2;
			}
			opt.fidelity.emplace_back(std::move(*p), std::move(bot));
		}
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
	/* Section 8.8: the levels' geometry. */
	mission_library library;
	level_geometries geo;
	if (opt.missions)
	{
		const auto n{library.load(opt.missions)};
		std::printf("\nmissions: %u in %s\n", analysis::detail::count(n), opt.missions);
		for (const auto &f : files)
		{
			auto &g{geo.emplace_back()};
			for (const auto &l : f.levels)
			{
				std::string note;
				g.push_back(library.find(l.rec, note));
				std::printf("  %s level %d \"%s\" of \"%s\" (%u segments): %s\n", f.name.c_str(), l.rec.level_num, l.rec.level_name.c_str(), l.rec.mission.c_str(), l.rec.segments, note.c_str());
				if (const auto p{g.back()})
					std::printf("    %s\n", p->character().c_str());
			}
		}
	}
	const auto result{analyse_recordings(files, opt.skill, pyro_gx(), opt.missions ? &geo : nullptr)};
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
	/* Section 8.13: capture the flag and hoard, every player of the game. */
	for (const auto &m : result.modes)
		std::fputs(write_mode_report(m).c_str(), stdout);
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
	/* Section 9.18: the bots against the profiles they fly. */
	for (const auto &[target, bot] : opt.fidelity)
	{
		const auto stem{lower(target.callsign.substr(0, 3))};
		const auto mine{[&stem, &bot](const player_stats &s) {
			if (!bot.empty())
				return s.bot && lower(s.callsign) == bot;
			return s.bot && !stem.empty() && lower(s.callsign).starts_with(stem);
		}};
		const bool any{std::any_of(result.players.begin(), result.players.end(), [&](const player_result &pr) { return mine(pr.stats); })};
		for (const auto &pr : result.players)
		{
			const auto &s{pr.stats};
			if (!s.bot || s.alive_s < opt.min_seconds || (any && !mine(s)))
				continue;
			std::printf("\n%s", fidelity_report(target, s).c_str());
		}
	}
	return rc;
}
