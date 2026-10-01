/*
 * This file is part of the DXX-Rebirth project <https://github.com/dxx-rebirth/dxx-rebirth/>.
 * It is copyright by its individual contributors, as recorded in the
 * project's Git history.  See COPYING.txt at the top level for license
 * terms and a link to the Git history.
 */
/*
 * movrec-dump: summary and CSV export of movement recordings
 * (Documentation/movement-recording.md).
 *
 *	movrec-dump [--csv DIR] [--player N] [--records] FILE...
 *
 * Without options it prints, per file, the header, the file's health
 * (chunks, damage, truncation) and per player a summary of the movement
 * (speed, strafing, reversing, afterburner, fire, hits, kills, deaths,
 * pickups, distance to the enemy).  --csv writes one CSV of samples per
 * player and one of events into DIR; --player limits the output to one
 * player number; --records prints every record.
 *
 * Build: scons sdl2=1 d1x=0 d2x=1 register_runtime_test_plain_link_targets=1 movrec-dump
 * Binary: build/common/movrec-dump
 */

#include <array>
#include <cstdio>
#include <cstdlib>
#include <cctype>
#include <cinttypes>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

#include "movement_record_reader.h"

using namespace dcx::movrec;

namespace {

constexpr unsigned MAX_PID{8};

struct player_summary
{
	std::string callsign;
	std::uint8_t flags{};
	std::uint64_t samples{}, alive{}, with_controls{};
	/* Of them, shared by the player's machine (-sharemoves). */
	std::uint64_t shared_controls{};
	double speed_sum{};
	std::uint64_t reversing{}, strafing{}, climbing{};
	std::uint64_t ab_known{}, ab_on{};
	std::uint64_t enemy_seen{};
	double enemy_dist_sum{};
	std::uint64_t attacked{}, aimed_at{};
	std::uint64_t fire_primary{}, fire_secondary{};
	unsigned splash_taken{};
	std::uint64_t hits_taken{}, hits_dealt{}, kills{}, deaths{}, suicides{}, respawns{}, pickups{}, weapon_switches{};
	double damage_taken{}, damage_dealt{};
	/* Reverse thrust while turning hard: the controls of the ships flown
	 * on the recording machine.
	 */
	std::uint64_t turning{}, turning_reverse{};
};

struct options
{
	const char *csv_dir{};
	int only_player{-1};
	bool records{};
};

std::string file_stem(const char *const path)
{
	std::string s{path};
	if (const auto slash{s.find_last_of("/\\")}; slash != std::string::npos)
		s.erase(0, slash + 1);
	if (const auto dot{s.find_last_of('.')}; dot != std::string::npos && dot)
		s.erase(dot);
	return s;
}

std::string safe_name(const std::string &s)
{
	std::string r;
	for (const char c : s)
		r += (std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_') ? c : '_';
	return r.empty() ? std::string{"player"} : r;
}

double pct(const std::uint64_t a, const std::uint64_t b)
{
	return b ? 100.0 * static_cast<double>(a) / static_cast<double>(b) : 0.0;
}

int dump(const char *const path, const options &opt)
{
	const auto data{load_file(path)};
	if (!data)
	{
		std::fprintf(stderr, "%s: cannot read\n", path);
		return 1;
	}
	std::array<player_summary, MAX_PID> players{};
	std::array<std::FILE *, MAX_PID> csv{};
	std::FILE *events_csv{};
	const std::string stem{file_stem(path)};
	tick_record current_tick{};
	int level{-1};
	unsigned levels{};
	std::string level_lines;
	std::uint64_t ticks{}, events{};
	unsigned syncs{};
	std::uint32_t session_id{};
	std::uint32_t last_time_ms{};
	const auto wanted{[&opt](const unsigned pid) {
		return opt.only_player < 0 || static_cast<unsigned>(opt.only_player) == pid;
	}};
	const auto open_csv{[&](const unsigned pid) -> std::FILE * {
		if (!opt.csv_dir || pid >= MAX_PID || !wanted(pid))
			return nullptr;
		if (!csv[pid])
		{
			const std::string name{std::string{opt.csv_dir} + "/" + stem + "-p" + std::to_string(pid) + "-" + safe_name(players[pid].callsign) + ".csv"};
			csv[pid] = std::fopen(name.c_str(), "w");
			if (!csv[pid])
				std::fprintf(stderr, "%s: cannot write\n", name.c_str());
			else
				write_sample_csv_header(csv[pid]);
		}
		return csv[pid];
	}};
	if (opt.csv_dir)
	{
		const std::string name{std::string{opt.csv_dir} + "/" + stem + "-events.csv"};
		events_csv = std::fopen(name.c_str(), "w");
		if (!events_csv)
			std::fprintf(stderr, "%s: cannot write\n", name.c_str());
		else
			write_event_csv_header(events_csv);
	}
	const auto result{read_recording(*data, [&](const record &r) {
		if (const auto t{std::get_if<tick_record>(&r)})
		{
			current_tick = *t;
			last_time_ms = t->time_ms;
			++ticks;
			if (opt.records)
				std::printf("tick %u t=%.3f\n", t->tick, t->time_ms / 1000.0);
		}
		else if (const auto y{std::get_if<sync_record>(&r)})
		{
			++syncs;
			if (y->session_id)
				session_id = y->session_id;
			if (opt.records)
				std::printf("sync t=%.3f session %08x host clock %.3f s%s%s\n", y->time_ms / 1000.0, y->session_id, static_cast<double>(y->host_ms) / 1000.0, (y->flags & sync_flag::clock_valid) ? "" : " (not valid)", (y->flags & sync_flag::host) ? " (host)" : "");
		}
		else if (const auto l{std::get_if<level_record>(&r)})
		{
			level = l->level_num;
			++levels;
			std::array<char, 512> line;
			std::snprintf(line.data(), line.size(), "  level %d \"%s\" of \"%s\" (%u segments, game mode 0x%x) at %.1f s\n", l->level_num, l->level_name.c_str(), l->mission.c_str(), l->segments, l->game_mode, last_time_ms / 1000.0);
			level_lines += line.data();
		}
		else if (const auto p{std::get_if<player_record>(&r)})
		{
			if (p->pid < MAX_PID)
			{
				players[p->pid].callsign = p->callsign;
				players[p->pid].flags = p->flags;
			}
			if (opt.records)
				std::printf("player %u \"%s\" flags 0x%x team %u\n", p->pid, p->callsign.c_str(), p->flags, p->team);
		}
		else if (const auto s{std::get_if<sample>(&r)})
		{
			if (s->pid >= MAX_PID || !wanted(s->pid))
				return;
			auto &ps{players[s->pid]};
			const auto u{to_units(*s)};
			++ps.samples;
			if (s->flags & sample_flag::alive)
			{
				++ps.alive;
				ps.speed_sum += u.speed;
				if (u.vel_ship[2] < -5)
					++ps.reversing;
				if (std::abs(u.vel_ship[0]) > 10)
					++ps.strafing;
				if (std::abs(u.vel_ship[1]) > 10)
					++ps.climbing;
				if (s->flags2 & sample_flag2::afterburner_known)
				{
					++ps.ab_known;
					if (s->flags & sample_flag::afterburner)
						++ps.ab_on;
				}
				if (s->flags & sample_flag::controls)
				{
					++ps.with_controls;
					if (std::abs(u.controls[4]) > 0.5 || std::abs(u.controls[3]) > 0.5)
					{
						++ps.turning;
						if (u.controls[0] < -0.2)
							++ps.turning_reverse;
					}
				}
				if (u.has_enemy && (s->context & context_flag::line_of_sight))
				{
					++ps.enemy_seen;
					ps.enemy_dist_sum += u.enemy_distance;
				}
				if (s->attacked_mask)
					++ps.attacked;
				if (s->aimed_at_mask)
					++ps.aimed_at;
			}
			if (const auto f{open_csv(s->pid)})
				write_sample_csv_row(f, current_tick, level, *s);
			if (opt.records)
				std::printf("  sample p%u pos %.1f %.1f %.1f speed %.1f flags 0x%02x/0x%02x\n", s->pid, u.pos[0], u.pos[1], u.pos[2], u.speed, s->flags, s->flags2);
		}
		else if (const auto e{std::get_if<event_record>(&r)})
		{
			++events;
			const auto pid{e->pid};
			if (events_csv && (wanted(pid) || (e->other < MAX_PID && wanted(e->other))))
				write_event_csv_row(events_csv, level, *e);
			if (opt.records)
				std::printf("  %s t=%.3f pid %u other %u kind %u id %u value %u flags 0x%x\n", record_type_name(e->type), e->time_ms / 1000.0, e->pid, e->other, e->kind, e->id, e->value, e->flags);
			switch (e->type)
			{
				case record_type::fire:
					if (pid < MAX_PID)
						++(e->kind == fire_kind::secondary ? players[pid].fire_secondary : players[pid].fire_primary);
					break;
				case record_type::hit:
					if (pid < MAX_PID)
					{
						if (e->flags & hit_flag::splash)
							++players[pid].splash_taken;
						++players[pid].hits_taken;
						players[pid].damage_taken += e->value / 256.0;
					}
					if (e->other < MAX_PID && e->other != pid)
					{
						++players[e->other].hits_dealt;
						players[e->other].damage_dealt += e->value / 256.0;
					}
					break;
				case record_type::death:
					if (pid < MAX_PID)
						++players[pid].deaths;
					break;
				case record_type::kill:
					if (e->other < MAX_PID)
					{
						if (e->other == pid)
							++players[pid].suicides;
						else
							++players[e->other].kills;
					}
					break;
				case record_type::respawn:
					if (pid < MAX_PID)
						++players[pid].respawns;
					break;
				case record_type::pickup:
					if (pid < MAX_PID)
						++players[pid].pickups;
					break;
				case record_type::weapon:
					if (pid < MAX_PID)
						++players[pid].weapon_switches;
					break;
				default:
					break;
			}
		}
	})};
	for (const auto f : csv)
		if (f)
			std::fclose(f);
	if (events_csv)
		std::fclose(events_csv);
	if (!result.header)
	{
		std::fprintf(stderr, "%s: not a movement recording, or its header is damaged\n", path);
		return 1;
	}
	const auto &h{*result.header};
	const auto &st{result.stats};
	std::printf("%s: format %u.%u, %s, %u Hz, started %" PRId64 " (unix time)\n", path, h.version, h.minor, h.program.c_str(), h.tick_rate, h.start_unix_time);
	std::printf("  %s%s%s, local player %u, first level %d \"%s\" of \"%s\"\n",
		(h.flags & static_cast<std::uint16_t>(header_flag::multiplayer)) ? "multiplayer" : "single player",
		(h.flags & static_cast<std::uint16_t>(header_flag::host)) ? " host" : "",
		(h.flags & static_cast<std::uint16_t>(header_flag::bots_recorded)) ? ", bots recorded" : "",
		h.local_pid, h.level_num, h.level_name.c_str(), h.mission.c_str());
	const std::uint64_t file_size{data->size()};
	std::printf("  %" PRIu64 " bytes, %u chunks, %u damaged, %u missing, %" PRIu64 " records (%" PRIu64 " unknown, %" PRIu64 " malformed), %s\n",
		file_size, st.chunks_ok, st.chunks_bad, st.sequence_gaps, st.records, st.unknown_records, st.malformed_records,
		st.clean_end ? "closed by the game" : st.truncated ? "cut short (the game stopped while writing)" : "not closed");
	std::printf("  %u level(s), %" PRIu64 " ticks, %.1f s of game time, %" PRIu64 " events\n", levels, ticks, last_time_ms / 1000.0, events);
	if (syncs)
		std::printf("  %u sync records, network session %08x\n", syncs, session_id);
	std::fputs(level_lines.c_str(), stdout);
	for (unsigned pid{}; pid != MAX_PID; ++pid)
	{
		const auto &p{players[pid]};
		if (!p.samples || !wanted(pid))
			continue;
		const double dt{1.0 / h.tick_rate};
		std::printf("  player %u \"%s\"%s%s%s: %" PRIu64 " samples, alive %.1f s\n", pid, p.callsign.c_str(), (p.flags & player_flag::bot) ? " (bot)" : "", (p.flags & player_flag::local) ? " (local)" : "", (p.flags & player_flag::shares_controls) ? " (shares controls)" : "", p.samples, p.alive * dt);
		std::printf("    mean speed %.1f, reversing %.0f%%, strafing %.0f%%, climbing/diving %.0f%% of the time alive\n", p.alive ? p.speed_sum / p.alive : 0.0, pct(p.reversing, p.alive), pct(p.strafing, p.alive), pct(p.climbing, p.alive));
		if (p.ab_known)
			std::printf("    afterburner %.1f%% of %" PRIu64 " known samples\n", pct(p.ab_on, p.ab_known), p.ab_known);
		if (p.with_controls)
			std::printf("    controls in %" PRIu64 " samples (%.0f%% of the time alive: %" PRIu64 " flown here, %" PRIu64 " shared by the player's machine); turning hard %.0f%%, of which reverse thrust %.0f%%\n", p.with_controls, pct(p.with_controls, p.alive), p.with_controls - p.shared_controls, p.shared_controls, pct(p.turning, p.with_controls), pct(p.turning_reverse, p.turning));
		else if (p.alive)
			std::printf("    no controls (estimated from the motion by movrec-analyse)\n");
		std::printf("    enemy in sight %.0f%% (mean distance %.0f), under attack %.0f%%, aimed at %.0f%%\n", pct(p.enemy_seen, p.alive), p.enemy_seen ? p.enemy_dist_sum / p.enemy_seen : 0.0, pct(p.attacked, p.alive), pct(p.aimed_at, p.alive));
		std::printf("    fired %" PRIu64 " primary, %" PRIu64 " secondary; hits dealt %" PRIu64 " (%.0f), taken %" PRIu64 " (%.0f; %u splash); kills %" PRIu64 ", deaths %" PRIu64 " (%" PRIu64 " suicides), respawns %" PRIu64 "; pickups %" PRIu64 ", weapon switches %" PRIu64 "\n",
			p.fire_primary, p.fire_secondary,
			p.hits_dealt, p.damage_dealt, p.hits_taken, p.damage_taken, p.splash_taken,
			p.kills, p.deaths, p.suicides, p.respawns,
			p.pickups, p.weapon_switches);
	}
	return 0;
}

void usage()
{
	std::fputs("usage: movrec-dump [--csv DIR] [--player N] [--records] FILE...\n"
		"  --csv DIR     write DIR/<file>-p<N>-<callsign>.csv per player and DIR/<file>-events.csv\n"
		"  --player N    only player number N\n"
		"  --records     print every record\n", stderr);
}

}

int main(const int argc, char **const argv)
{
	options opt;
	std::vector<const char *> files;
	for (int i{1}; i < argc; ++i)
	{
		const char *const a{argv[i]};
		if (!std::strcmp(a, "--csv") && i + 1 < argc)
			opt.csv_dir = argv[++i];
		else if (!std::strcmp(a, "--player") && i + 1 < argc)
			opt.only_player = std::atoi(argv[++i]);
		else if (!std::strcmp(a, "--records"))
			opt.records = true;
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
			files.push_back(a);
	}
	if (files.empty())
	{
		usage();
		return 2;
	}
	int rc{};
	for (const auto f : files)
		rc |= dump(f, opt);
	return rc;
}
