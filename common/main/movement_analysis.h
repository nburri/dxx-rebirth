/*
 * This file is part of the DXX-Rebirth project <https://github.com/dxx-rebirth/dxx-rebirth/>.
 * It is copyright by its individual contributors, as recorded in the
 * project's Git history.  See COPYING.txt at the top level for license
 * terms and a link to the Git history.
 */
/*
 * Analysis of the movement recordings (step 2 of
 * Documentation/movement-recording.md): reads recordings, merges those of
 * one game made on several machines, and reduces every player's flying
 * to a movement profile (`player_stats`), which `propose_profile` maps
 * onto the bots' parameters (bot_style_profile.h).  The tool is
 * common/tools/movrec_analyse.cpp, the test
 * common/unittest/movement_analysis.cpp.
 *
 * The stages:
 *
 *	load_recording   one file -> `recording` (samples and events with
 *	                 the callsign of their slot at that moment)
 *	group_sessions   files of one game -> `session`, each file with a
 *	                 mapping of its time to the game's shared clock
 *	merge_session    -> `merged_session`: per player one stream of
 *	                 samples (its own machine's where it has one, for the
 *	                 exact controls) and the events, each once
 *	build_track      -> `track`: samples in game units, the controls
 *	                 (exact, or estimated from the motion)
 *	analyse          tracks of one player -> `player_stats`
 *	propose_profile  -> `bot::style_profile`
 *	write_report     -> text for people
 *
 * Header-only, standard library only (and the bots' pure headers for
 * their constants).
 */

#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "movement_record_reader.h"
#include "level_geometry.h"
#include "bot_brain.h"
#include "bot_goals.h"
#include "bot_weapons.h"
#include "bot_style_profile.h"

namespace dcx::movrec::analysis {

/* The ship's flight model, as physics.cpp integrates it: under a thrust
 * `c` (a share of the maximum, per axis of the ship) the velocity goes
 * toward `c * max_speed` at `linear_rate` per second,
 *
 *	v(t + dt) = v(t) * R + c * max_speed * (1 - R),  R = exp(-linear_rate * dt)
 *
 * and the rotational velocity likewise toward `c * max_turn` at
 * `turn_rate`.
 */
struct ship_model
{
	double max_speed{};	/* units/s at full thrust (the afterburner doubles the thrust) */
	double linear_rate{};	/* 1/s */
	double max_turn{};	/* revolutions/s about one axis at full deflection */
	double turn_rate{};	/* 1/s */
};

/* The Pyro-GX (mass 4, drag 0.033, thrust 7.8, rotational thrust 0.14;
 * the rotational drag is 5/2 of the drag): about 58.5 units/s and 0.41
 * revolutions/s.
 */
[[nodiscard]]
inline ship_model pyro_gx()
{
	constexpr double mass{4}, drag{0.033}, thrust{7.8}, rotthrust{0.14};
	/* physics.cpp: drag_reference_fraction_of_step, at 200 frames/s. */
	constexpr double reference{20928.0 / 65536};
	constexpr double frames_per_second{200};
	const auto model{[](const double d) {
		const double per_frame{reference * d};
		/* rate, steady state velocity per unit of acceleration */
		return std::pair{-std::log(1 - per_frame) * frames_per_second, reference * (1 - per_frame) / per_frame};
	}};
	const auto lin{model(drag)};
	const auto rot{model(drag * 5 / 2)};
	return {thrust / mass * lin.second, lin.first, rotthrust / mass * rot.second, rot.first};
}

/*
 * Loading.
 */

struct file_player
{
	std::string callsign;
	bool bot{};
	/* Flown on the recording machine (exact controls). */
	bool local{};
};

struct file_sample
{
	std::uint32_t time_ms{};
	std::uint16_t level{};
	/* Index into recording::players; of the enemy too, if it is a player. */
	int who{-1};
	int enemy{-1};
	sample s;
};

struct file_event
{
	std::uint16_t level{};
	int who{-1};
	int other{-1};
	event_record e;
};

struct file_level
{
	level_record rec;
	std::uint32_t start_ms{};
};

struct recording
{
	std::string name;
	file_header header;
	read_stats stats;
	std::vector<file_player> players;
	std::vector<file_level> levels;
	std::vector<file_sample> samples;
	std::vector<file_event> events;
	std::vector<sync_record> syncs;
	std::uint32_t end_ms{};
	[[nodiscard]]
	bool host() const
	{
		return (header.flags & static_cast<std::uint16_t>(header_flag::host)) != 0;
	}
	[[nodiscard]]
	bool multiplayer() const
	{
		return (header.flags & static_cast<std::uint16_t>(header_flag::multiplayer)) != 0;
	}
	/* The network session (0: none known). */
	[[nodiscard]]
	std::uint32_t session_id() const
	{
		for (const auto &y : syncs)
			if (y.session_id)
				return y.session_id;
		return 0;
	}
};

[[nodiscard]]
inline std::string lower(const std::string_view s)
{
	std::string r{s};
	for (auto &c : r)
		if (c >= 'A' && c <= 'Z')
			c = static_cast<char>(c - 'A' + 'a');
	return r;
}

/* A recording held in memory, or nothing if its header cannot be read.
 * A file cut short or damaged gives what could be read.
 */
[[nodiscard]]
inline std::optional<recording> load_recording(const std::span<const std::uint8_t> bytes, std::string name)
{
	recording rec;
	rec.name = std::move(name);
	/* Who holds each slot now. */
	std::array<int, 256> slot;
	slot.fill(-1);
	const auto player_index{[&rec](const std::string &callsign, const bool bot, const bool local) {
		for (std::size_t i{}; i != rec.players.size(); ++i)
			if (rec.players[i].bot == bot && lower(rec.players[i].callsign) == lower(callsign))
			{
				rec.players[i].local |= local;
				return static_cast<int>(i);
			}
		rec.players.push_back({callsign, bot, local});
		return static_cast<int>(rec.players.size() - 1);
	}};
	const auto holder{[&](const std::uint8_t pid) {
		if (pid == PLAYER_NONE)
			return -1;
		if (slot[pid] < 0)
			slot[pid] = player_index("player" + std::to_string(pid), false, false);
		return slot[pid];
	}};
	/* The slots at the start are in the header. */
	const auto head{decode_header(bytes)};
	if (!head)
		return std::nullopt;
	for (std::size_t i{}; i != head->first.num_players; ++i)
		if (const auto &p{head->first.players[i]}; p.flags & player_flag::connected)
			slot[p.pid] = player_index(p.callsign, (p.flags & player_flag::bot) != 0, (p.flags & player_flag::local) != 0);
	std::uint32_t now_ms{};
	const auto result{read_recording(bytes, [&](const record &r) {
		if (const auto t{std::get_if<tick_record>(&r)})
		{
			now_ms = t->time_ms;
			rec.end_ms = std::max(rec.end_ms, now_ms);
		}
		else if (const auto l{std::get_if<level_record>(&r)})
			rec.levels.push_back({*l, now_ms});
		else if (const auto p{std::get_if<player_record>(&r)})
		{
			if (p->flags & player_flag::connected)
				slot[p->pid] = player_index(p->callsign, (p->flags & player_flag::bot) != 0, (p->flags & player_flag::local) != 0);
			else
				slot[p->pid] = -1;
		}
		else if (const auto y{std::get_if<sync_record>(&r)})
			rec.syncs.push_back(*y);
		else if (const auto s{std::get_if<sample>(&r)})
		{
			/* A sample of no player (a damaged or hostile file). */
			if (s->pid == PLAYER_NONE)
				return;
			if (rec.levels.empty())
				rec.levels.push_back({{head->first.level_num, 0, head->first.game_mode, head->first.mission, head->first.level_name, head->first.mission_file, head->first.level_file}, now_ms});
			file_sample fs;
			fs.time_ms = now_ms;
			fs.level = static_cast<std::uint16_t>(rec.levels.size() - 1);
			fs.who = holder(s->pid);
			if ((s->context & context_flag::kind_mask) == context_flag::kind_player && s->enemy_id < PLAYER_NONE)
				fs.enemy = holder(static_cast<std::uint8_t>(s->enemy_id));
			fs.s = *s;
			if (s->flags2 & sample_flag2::local)
				rec.players[static_cast<std::size_t>(fs.who)].local = true;
			rec.samples.push_back(fs);
		}
		else if (const auto e{std::get_if<event_record>(&r)})
		{
			if (e->type == record_type::end)
				return;
			file_event fe;
			fe.level = static_cast<std::uint16_t>(rec.levels.empty() ? 0 : rec.levels.size() - 1);
			fe.who = holder(e->pid);
			if ((e->type == record_type::hit || e->type == record_type::kill) && e->kind == attacker_kind::player)
				fe.other = holder(e->other);
			fe.e = *e;
			rec.end_ms = std::max(rec.end_ms, e->time_ms);
			rec.events.push_back(fe);
		}
	})};
	if (!result.header)
		return std::nullopt;
	rec.header = *result.header;
	rec.stats = result.stats;
	return rec;
}


/*
 * Sessions: the recordings of one game, on one clock.
 */

/* A file's time (ms since its start) on the session's clock: the offset
 * of the newest step at or before the time (of the first step for
 * earlier times).  The offset changes between levels (the file's time
 * stands still between two levels, the session's clock does not) and,
 * by a few ms, with every sync record.
 */
struct file_clock
{
	struct step
	{
		std::uint32_t from_ms{};
		std::int64_t offset{};
	};
	std::vector<step> steps;
	[[nodiscard]]
	std::int64_t to_session(const std::uint32_t t) const
	{
		if (steps.empty())
			return t;
		auto i{std::upper_bound(steps.begin(), steps.end(), t, [](const std::uint32_t v, const step &s) { return v < s.from_ms; })};
		if (i != steps.begin())
			--i;
		return std::int64_t{t} + i->offset;
	}
};

/* How a file's clock was found. */
enum class clock_source : std::uint8_t
{
	/* The only file, or the reference of its session. */
	own,
	/* Its sync records (the network session's shared clock). */
	sync,
	/* The path of a player both files recorded. */
	trajectory,
};

struct session_file
{
	std::size_t file{};
	file_clock clock;
	clock_source source{clock_source::own};
};

struct session
{
	std::vector<session_file> files;
};

namespace detail {

[[nodiscard]]
inline bool has_valid_sync(const recording &r)
{
	return std::any_of(r.syncs.begin(), r.syncs.end(), [](const sync_record &y) { return (y.flags & sync_flag::clock_valid) != 0; });
}

[[nodiscard]]
inline file_clock clock_from_sync(const recording &r)
{
	file_clock c;
	for (const auto &y : r.syncs)
		if (y.flags & sync_flag::clock_valid)
			c.steps.push_back({y.time_ms, y.host_ms - std::int64_t{y.time_ms}});
	std::stable_sort(c.steps.begin(), c.steps.end(), [](const file_clock::step &a, const file_clock::step &b) { return a.from_ms < b.from_ms; });
	return c;
}

/* A file that stands alone: its start on the wall clock. */
[[nodiscard]]
inline file_clock clock_from_start(const recording &r)
{
	return {{{0, r.header.start_unix_time * 1000}}};
}

[[nodiscard]]
inline int find_player(const recording &r, const file_player &p)
{
	for (std::size_t i{}; i != r.players.size(); ++i)
		if (r.players[i].bot == p.bot && lower(r.players[i].callsign) == lower(p.callsign))
			return static_cast<int>(i);
	return -1;
}

[[nodiscard]]
inline bool same_level(const level_record &a, const level_record &b)
{
	return a.level_num == b.level_num && a.level_name == b.level_name && a.mission == b.mission;
}

/* The offset of `f`'s level `level` to the session's clock, from the
 * path of a player that the reference file `ref` (on the session's clock
 * through `ref_clock`) recorded too: every position of `f` votes for the
 * offsets to the moments at which `ref` has the ship at the same place;
 * the right offset collects the votes of all (70 % are asked for), a
 * wrong one only those of places the ship visited twice.  Nothing if the files share no level
 * and player, or no offset stands out.
 */
[[nodiscard]]
inline std::optional<std::int64_t> align_level(const recording &ref, const file_clock &ref_clock, const recording &f, const std::uint16_t level)
{
	constexpr double NEAR_UNITS{2.5};
	constexpr std::int64_t BIN_MS{40};
	constexpr std::size_t MAX_PROBES{400};
	constexpr double MIN_SPEED{15};
	const auto &lrec{f.levels[level].rec};
	std::vector<bool> level_matches;
	for (const auto &l : ref.levels)
		level_matches.push_back(same_level(l.rec, lrec));
	const auto moving{[](const sample &s) {
		double v2{};
		for (const auto c : s.vel)
			v2 += (c / 64.0) * (c / 64.0);
		return v2 > MIN_SPEED * MIN_SPEED;
	}};
	std::optional<std::int64_t> best;
	std::size_t best_votes{};
	for (std::size_t fp{}; fp != f.players.size(); ++fp)
	{
		const int rp{find_player(ref, f.players[fp])};
		if (rp < 0)
			continue;
		/* The moving, living samples of the player in `f`'s level. */
		std::vector<const file_sample *> probes, targets;
		for (const auto &s : f.samples)
			if (s.level == level && s.who == static_cast<int>(fp) && (s.s.flags & sample_flag::alive) && moving(s.s))
				probes.push_back(&s);
		for (const auto &s : ref.samples)
			if (s.who == rp && (s.s.flags & sample_flag::alive) && level_matches[s.level])
				targets.push_back(&s);
		if (probes.size() < 30 || targets.empty())
			continue;
		const std::size_t stride{std::max<std::size_t>(1, probes.size() / MAX_PROBES)};
		/* Per bin of offsets: the probe and its offset. */
		std::map<std::int64_t, std::vector<std::pair<std::size_t, std::int64_t>>> votes;
		std::size_t used{};
		for (std::size_t i{}; i < probes.size(); i += stride)
		{
			++used;
			const auto &a{*probes[i]};
			for (const auto bp : targets)
			{
				const auto &b{*bp};
				double d2{};
				for (std::size_t k{}; k != 3; ++k)
				{
					const double d{(a.s.pos[k] - b.s.pos[k]) / 256.0};
					d2 += d * d;
				}
				if (d2 > NEAR_UNITS * NEAR_UNITS)
					continue;
				const std::int64_t offset{ref_clock.to_session(b.time_ms) - std::int64_t{a.time_ms}};
				votes[offset >= 0 ? offset / BIN_MS : -((-offset + BIN_MS - 1) / BIN_MS)].emplace_back(i, offset);
			}
		}
		for (const auto &[bin, v] : votes)
		{
			/* With the neighbour bins, so that a peak on a border counts whole. */
			auto all{v};
			for (const std::int64_t n : {bin - 1, bin + 1})
				if (const auto i{votes.find(n)}; i != votes.end())
					all.insert(all.end(), i->second.begin(), i->second.end());
			/* How many of the probes agree (a probe may match several
			 * samples of the reference): most of them must.
			 */
			std::sort(all.begin(), all.end());
			std::size_t probes_agreeing{};
			std::vector<std::int64_t> offsets;
			for (std::size_t k{}; k != all.size(); ++k)
			{
				if (!k || all[k].first != all[k - 1].first)
					++probes_agreeing;
				offsets.push_back(all[k].second);
			}
			/* Among the windows most probes agree on, the one with the
			 * most matches: the middle of the peak.
			 */
			if (all.size() > best_votes && probes_agreeing * 10 >= used * 7)
			{
				best_votes = all.size();
				std::nth_element(offsets.begin(), offsets.begin() + static_cast<std::ptrdiff_t>(offsets.size() / 2), offsets.end());
				best = offsets[offsets.size() / 2];
			}
		}
	}
	return best;
}

[[nodiscard]]
inline std::optional<file_clock> clock_from_trajectory(const recording &ref, const file_clock &ref_clock, const recording &f)
{
	file_clock c;
	for (std::size_t l{}; l != f.levels.size(); ++l)
		if (const auto o{align_level(ref, ref_clock, f, static_cast<std::uint16_t>(l))})
			c.steps.push_back({f.levels[l].start_ms, *o});
	if (c.steps.empty())
		return std::nullopt;
	return c;
}

/* Recordings that may be of the same game: the same network session, or
 * (files without one: format minor 0, or the clock was not known yet)
 * the same mission, started within half an hour, with a player in
 * common.
 */
[[nodiscard]]
inline bool may_share_game(const recording &a, const recording &b)
{
	if (!a.multiplayer() || !b.multiplayer())
		return false;
	if (const auto ia{a.session_id()}, ib{b.session_id()}; ia && ib)
		return ia == ib;
	if (a.host() && b.host())
		return false;
	if (a.header.mission != b.header.mission || std::abs(a.header.start_unix_time - b.header.start_unix_time) > 1800)
		return false;
	return std::any_of(a.players.begin(), a.players.end(), [&b](const file_player &p) { return find_player(b, p) >= 0; });
}

}

/* The files grouped into games, each file with its clock.  A file that
 * belongs to a game by its header but cannot be put on the game's clock
 * is a game of its own (`notes` says so).
 */
[[nodiscard]]
inline std::vector<session> group_sessions(const std::span<const recording> files, std::vector<std::string> *const notes = nullptr)
{
	std::vector<std::vector<std::size_t>> groups;
	/* Two files of different network sessions (or two hosts' files) are
	 * never one game, even if a file without a session (minor 0) looks
	 * like both.
	 */
	const auto other_session{[&files](const std::size_t a, const std::size_t b) {
		if (files[a].host() && files[b].host())
			return true;
		const auto ia{files[a].session_id()}, ib{files[b].session_id()};
		return ia && ib && ia != ib;
	}};
	for (std::size_t i{}; i != files.size(); ++i)
	{
		const auto g{std::find_if(groups.begin(), groups.end(), [&](const std::vector<std::size_t> &members) {
			return std::any_of(members.begin(), members.end(), [&](const std::size_t m) { return detail::may_share_game(files[m], files[i]); })
				&& std::none_of(members.begin(), members.end(), [&](const std::size_t m) { return other_session(m, i); });
		})};
		if (g == groups.end())
			groups.push_back({i});
		else
			g->push_back(i);
	}
	std::vector<session> sessions;
	for (const auto &members : groups)
	{
		/* The reference: a file on the shared clock, the host's first. */
		const auto rank{[&files](const std::size_t m) {
			return (detail::has_valid_sync(files[m]) ? 2 : 0) + (files[m].host() ? 1 : 0);
		}};
		const std::size_t ref{*std::max_element(members.begin(), members.end(), [&rank](const std::size_t a, const std::size_t b) { return rank(a) < rank(b); })};
		const bool ref_sync{detail::has_valid_sync(files[ref])};
		session s;
		s.files.push_back({ref, ref_sync ? detail::clock_from_sync(files[ref]) : detail::clock_from_start(files[ref]), members.size() > 1 && ref_sync ? clock_source::sync : clock_source::own});
		for (const std::size_t m : members)
		{
			if (m == ref)
				continue;
			if (ref_sync && detail::has_valid_sync(files[m]))
			{
				s.files.push_back({m, detail::clock_from_sync(files[m]), clock_source::sync});
				continue;
			}
			if (auto c{detail::clock_from_trajectory(files[ref], s.files.front().clock, files[m])})
			{
				s.files.push_back({m, std::move(*c), clock_source::trajectory});
				continue;
			}
			if (notes)
				notes->push_back(files[m].name + ": looks like the same game as " + files[ref].name + ", but the two cannot be put on one clock; analysed as a game of its own (players in both count twice)");
			sessions.push_back({{{m, detail::clock_from_start(files[m]), clock_source::own}}});
		}
		sessions.push_back(std::move(s));
	}
	return sessions;
}

/*
 * Merging: per player of a session one stream of samples, and every
 * event once.
 */

struct merged_sample
{
	std::int64_t t{};	/* session clock, ms */
	std::uint32_t local_ms{};	/* the file's own time, for differences */
	std::uint16_t file{};	/* index into session::files */
	std::uint16_t level{};	/* of that file */
	int enemy{-1};		/* index into merged_session::players */
	sample s;
};

struct merged_event
{
	std::int64_t t{};
	int who{-1};
	int other{-1};
	event_record e;
};

struct session_player
{
	std::string callsign;
	bool bot{};
	std::vector<merged_sample> samples;
};

struct merged_session
{
	/* Seconds per sample of each file of the session (its tick rate;
	 * -recordmoves-rate may differ between machines).
	 */
	std::vector<double> tick_s;
	std::vector<session_player> players;
	/* Sorted by time. */
	std::vector<merged_event> events;
};

namespace detail {

/* The times a source has samples of a player: sorted, disjoint. */
using intervals = std::vector<std::pair<std::int64_t, std::int64_t>>;

[[nodiscard]]
inline bool covered(const intervals &iv, const std::int64_t t)
{
	auto i{std::upper_bound(iv.begin(), iv.end(), t, [](const std::int64_t v, const std::pair<std::int64_t, std::int64_t> &p) { return v < p.first; })};
	return i != iv.begin() && t <= (--i)->second;
}

}

[[nodiscard]]
inline merged_session merge_session(const std::span<const recording> files, const session &ses)
{
	merged_session out;
	if (ses.files.empty())
		return out;
	for (const auto &sf : ses.files)
		out.tick_s.push_back(1.0 / files[sf.file].header.tick_rate);
	const std::size_t nf{ses.files.size()};
	/* players of the session, and each file's players in them */
	std::vector<std::vector<int>> index(nf);
	for (std::size_t fi{}; fi != nf; ++fi)
	{
		const auto &f{files[ses.files[fi].file]};
		for (const auto &p : f.players)
		{
			const auto at{std::find_if(out.players.begin(), out.players.end(), [&p](const session_player &q) { return q.bot == p.bot && lower(q.callsign) == lower(p.callsign); })};
			index[fi].push_back(static_cast<int>(at - out.players.begin()));
			if (at == out.players.end())
				out.players.push_back({p.callsign, p.bot, {}});
		}
	}
	const std::size_t np{out.players.size()};
	/* How good a source a file is for a player: its own machine (exact
	 * controls) before the host (one consistent world) before another
	 * client's view; then the order of the files.
	 */
	const auto priority{[&](const std::size_t fi, const int fp) {
		const auto &f{files[ses.files[fi].file]};
		return f.players[static_cast<std::size_t>(fp)].local ? 3 : f.host() ? 2 : 1;
	}};
	/* coverage[player][file] */
	std::vector<std::vector<detail::intervals>> coverage(np, std::vector<detail::intervals>(nf));
	std::vector<std::vector<int>> prio(np, std::vector<int>(nf, 0));
	for (std::size_t fi{}; fi != nf; ++fi)
	{
		const auto &sf{ses.files[fi]};
		const auto &f{files[sf.file]};
		const auto gap{static_cast<std::int64_t>(4000.0 / f.header.tick_rate)};
		for (std::size_t fp{}; fp != f.players.size(); ++fp)
			prio[static_cast<std::size_t>(index[fi][fp])][fi] = priority(fi, static_cast<int>(fp));
		for (const auto &s : f.samples)
		{
			if (s.who < 0)
				continue;
			auto &iv{coverage[static_cast<std::size_t>(index[fi][static_cast<std::size_t>(s.who)])][fi]};
			const auto t{sf.clock.to_session(s.time_ms)};
			if (!iv.empty() && t >= iv.back().first && t - iv.back().second <= gap)
				iv.back().second = std::max(iv.back().second, t);
			else
				iv.emplace_back(t, t);
		}
		for (std::size_t p{}; p != np; ++p)
		{
			auto &iv{coverage[p][fi]};
			std::sort(iv.begin(), iv.end());
			/* Half a tick of margin on both sides. */
			for (auto &i : iv)
			{
				i.first -= gap / 8;
				i.second += gap / 8;
			}
		}
	}
	/* The file to take player `p`'s data at `t` from: the best source
	 * that has samples of it then; if none has, the best that knows the
	 * player at all.
	 */
	const auto better{[&](const std::size_t p, const std::size_t a, const std::size_t b) {
		return prio[p][a] > prio[p][b] || (prio[p][a] == prio[p][b] && a < b);
	}};
	const auto source{[&](const std::size_t p, const std::int64_t t) {
		std::optional<std::size_t> with, any;
		for (std::size_t fi{}; fi != nf; ++fi)
		{
			if (!prio[p][fi])
				continue;
			if (!any || better(p, fi, *any))
				any = fi;
			if (detail::covered(coverage[p][fi], t) && (!with || better(p, fi, *with)))
				with = fi;
		}
		return with ? with : any;
	}};
	for (std::size_t fi{}; fi != nf; ++fi)
	{
		const auto &sf{ses.files[fi]};
		const auto &f{files[sf.file]};
		for (const auto &s : f.samples)
		{
			if (s.who < 0)
				continue;
			const auto p{static_cast<std::size_t>(index[fi][static_cast<std::size_t>(s.who)])};
			const auto t{sf.clock.to_session(s.time_ms)};
			if (source(p, t) != fi)
				continue;
			out.players[p].samples.push_back({t, s.time_ms, static_cast<std::uint16_t>(fi), s.level, s.enemy < 0 ? -1 : index[fi][static_cast<std::size_t>(s.enemy)], s.s});
		}
		for (const auto &e : f.events)
		{
			if (e.who < 0)
				continue;
			const auto p{static_cast<std::size_t>(index[fi][static_cast<std::size_t>(e.who)])};
			const auto t{sf.clock.to_session(e.e.time_ms)};
			if (source(p, t) != fi)
				continue;
			out.events.push_back({t, static_cast<int>(p), e.other < 0 ? -1 : index[fi][static_cast<std::size_t>(e.other)], e.e});
		}
	}
	for (auto &p : out.players)
		std::stable_sort(p.samples.begin(), p.samples.end(), [](const merged_sample &a, const merged_sample &b) { return a.t < b.t; });
	std::stable_sort(out.events.begin(), out.events.end(), [](const merged_event &a, const merged_event &b) { return a.t < b.t; });
	return out;
}

/*
 * Tracks: a player's samples of one session in game units, with the
 * controls: the recorded ones where the file has them (ships flown on the
 * recording machine), else estimated from the motion.
 */

/* Thresholds of the analysis, in one place. */
namespace limits {
/* A control counts as used beyond this share of full deflection. */
constexpr double CONTROL_USED{0.3};
/* An enemy in sight within this distance is a fight. */
constexpr double FIGHT_RANGE{400};
/* Moving toward or away from the enemy: this share of the top speed. */
constexpr double APPROACH_SHARE{0.15};
/* Turned away from the enemy: more than this off the nose (degrees). */
constexpr double TURNED_AWAY_DEG{60};
/* A turn: rotating faster than this share of the top rate ... */
constexpr double TURN_RATE_SHARE{0.35};
/* ... a large one: through at least this angle (the bots'
 * REVERSE_TURN_START), in at most this time per 180 degrees.
 */
constexpr double LARGE_TURN_DEG{110};
constexpr double LARGE_TURN_MAX_MS{2500};
/* A turn is flown backwards (sideways, forwards) if the thrust is that
 * for this share of it.
 */
constexpr double TURN_THRUST_SHARE{0.4};
/* The push after a turn: the mean forward thrust in this time. */
constexpr double BOOST_MS{800};
constexpr double BOOST_THRUST{0.6};
/* Dodging (scan_dodge): a burst is the first shot of a shooter after
 * FIRE_ONSET_MS without one, fired within DODGE_RANGE with the player
 * in its 30 degree cone.  The player's answer is a switch of its
 * sideways/vertical thrust (a run of the strafe starts, from none or in
 * another direction) between REACT_FROM_MS and REACT_TO_MS after the
 * shot.  The same is looked at every BASELINE_STRIDE_MS of the quiet
 * moments in the same situation (no shot aimed at the player
 * QUIET_BEFORE_MS before to DODGE_WINDOW_MS after); those are sorted by the phase of the weave
 * (the time since its last switch, PHASE_BIN_MS bins up to PHASE_BINS,
 * and whether a run is on), so a burst is compared with quiet moments
 * at the same point of the player's rhythm.  A phase with fewer than
 * MIN_PHASE_N quiet moments borrows from its neighbours.
 */
constexpr std::int64_t FIRE_ONSET_MS{1000};
constexpr double DODGE_RANGE{300};
constexpr std::int64_t REACT_FROM_MS{60};
constexpr std::int64_t REACT_TO_MS{450};
constexpr std::int64_t DODGE_WINDOW_MS{700};
constexpr std::int64_t QUIET_BEFORE_MS{1500};
constexpr std::int64_t BASELINE_STRIDE_MS{100};
constexpr std::int64_t PHASE_BIN_MS{50};
constexpr std::size_t PHASE_BINS{20};
constexpr unsigned MIN_PHASE_N{8};
/* A switch counts when its run lasts at least this long (a control
 * that hovers about CONTROL_USED is no switch).
 */
constexpr double SWITCH_MIN_S{0.06};
/* A hit of the shooter this long after the burst is its hit. */
constexpr std::int64_t BURST_HIT_MS{1500};
/* The room the measure needs: the bursts' chances of a switch the
 * weave would not have made, summed (with fewer, a dodge cannot show).
 */
constexpr double DODGE_MIN_ROOM{4};
/* Missiles at most this far apart are one volley. */
constexpr std::int64_t VOLLEY_GAP_MS{700};
/* Two volleys further apart than this are not of one fight: the time
 * between them says nothing about how fast the player fires missiles.
 */
constexpr std::int64_t VOLLEY_SAME_FIGHT_MS{15000};
/* A pickup is a detour if the course this long before it pointed more
 * than this away from it.
 */
constexpr std::int64_t DETOUR_LOOK_BACK_MS{1500};
constexpr double DETOUR_DEG{40};
/* Section 8.9, the power pickups: the pickups of every recorded
 * player give where a powerup lay; it lay there at most SIGHT_LOOK_BACK_MS
 * before it was taken (or since a ship blew up within DROP_RADIUS of it:
 * its drop).  A player had it in sight with it within VIEW_HALF_DEG of
 * the nose (on the screen) and a free line to it through the level
 * (section 8.8; SIGHT_SLACK short of it is enough), checked every
 * SIGHT_STRIDE samples; without the level's geometry, within
 * SIGHT_NO_GEOMETRY.  It went for it when it came within GO_FOR_NEAR of
 * it, or GO_FOR_SHARE of the distance at the first sight, and at least
 * GO_FOR_MIN_CLOSE closer than then.
 */
constexpr std::int64_t SIGHT_LOOK_BACK_MS{15000};
constexpr double DROP_RADIUS{40};
constexpr double VIEW_HALF_DEG{45};
constexpr double SIGHT_SLACK{5};
constexpr std::size_t SIGHT_STRIDE{3};
constexpr double SIGHT_NO_GEOMETRY{150};
constexpr double GO_FOR_NEAR{25};
constexpr double GO_FOR_SHARE{0.5};
constexpr double GO_FOR_MIN_CLOSE{10};
/* After losing sight: a pursuit ends when the player has not moved
 * toward the enemy for this long; one counts from this length.
 */
constexpr double PURSUIT_STALL_S{1.0};
constexpr double PURSUIT_MIN_S{1.0};
constexpr double PURSUIT_MAX_S{30};
/* The afterburner, where only estimated controls say: forward thrust
 * above this.
 */
constexpr double BURN_THRUST{1.3};
/* Too little to say anything about a player. */
constexpr double MIN_ALIVE_S{20};
/* A room class needs this much time alive to be reported. */
constexpr double ROOM_MIN_S{10};
}

struct track_point
{
	merged_sample m;
	sample_units u;
	bool alive{};
	/* The seconds this sample stands for: a tick of its file. */
	double w{};
	/* Seconds since the point before, if that is the sample before it
	 * in the same file and level and the ship lived at both; else 0.
	 */
	double dt{};
	/* forward, sideways (right +), vertical (up +), pitch, heading,
	 * bank; 1 = full deflection.
	 */
	std::array<double, 6> ctl{};
	bool has_ctl{};
	bool exact{};
	/* Exact, shared over the network by the player's machine
	 * (-sharemoves, format minor 2).
	 */
	bool shared{};
	/* The estimate from the motion (smoothed over three samples), for
	 * every point that has one: compared with the exact controls, it is
	 * the estimator's own test.
	 */
	std::array<double, 6> est{};
	bool est_valid{};
	bool ab_known{};
	bool ab{};
	bool ab_estimated{};
	/* An enemy in sight, and near enough to fight. */
	bool los{};
	bool fight{};
	/* Which enemy: a player of the session, or 1000 + a robot's number;
	 * -1 none.
	 */
	int enemy_key{-1};
	/* My own speed toward the enemy (units/s). */
	double own_approach{};
	/* The level's geometry (section 8.8), if known: the room of the
	 * segment the ship is in, the free distance ahead, behind, right,
	 * left, up and down (ship frame), and toward the enemy (the sight
	 * line, through it to the wall behind it) and away from it; `line`
	 * is the two together, the whole line of fire.
	 */
	const geometry::level_geometry *geo{};
	bool room_known{};
	geometry::room_class room{};
	double room_size{};
	std::array<double, 6> free{};
	bool line_known{};
	double sight{}, line{};
};

/* The geometry of every level of every recording (by the order of the
 * files given to analyse_recordings, then the recording's levels), where
 * the missions are known (movrec-analyse --missions); null where not.
 */
using level_geometries = std::vector<std::vector<const geometry::level_geometry *>>;

struct track
{
	std::vector<track_point> pts;
	/* Events of this player; hits and kills it dealt; the shots of the
	 * others.
	 */
	std::vector<merged_event> own, dealt, enemy_fire;
	/* Per shot of `enemy_fire`: where the shooter was and where its nose
	 * pointed, if the session has a sample of it then (it was recorded).
	 */
	struct shooter_pose
	{
		bool known{};
		vec3 pos{}, forward{};
	};
	std::vector<shooter_pose> enemy_fire_from;
	/* Section 8.9: every pickup of the session (by any recorded player):
	 * where the powerup lay (the picker's place then), what it was, from
	 * when it lay there at the earliest (limits::SIGHT_LOOK_BACK_MS, or a
	 * drop), the level (the picker's file and level, and its geometry if
	 * known), and whether this player took it.
	 */
	struct powerup_spot
	{
		std::int64_t t{}, since{};
		std::uint8_t id{};
		vec3 pos{};
		std::uint16_t file{}, level{};
		const geometry::level_geometry *geo{};
		bool own{};
	};
	std::vector<powerup_spot> spots;
	/* The point nearest to time `t`, within `tolerance_ms`. */
	[[nodiscard]]
	std::optional<std::size_t> at(const std::int64_t t, const std::int64_t tolerance_ms) const
	{
		if (pts.empty())
			return std::nullopt;
		auto i{std::lower_bound(pts.begin(), pts.end(), t, [](const track_point &p, const std::int64_t v) { return p.m.t < v; })};
		if (i == pts.end() || (i != pts.begin() && t - std::prev(i)->m.t < i->m.t - t))
			--i;
		if (std::abs(i->m.t - t) > tolerance_ms)
			return std::nullopt;
		return static_cast<std::size_t>(i - pts.begin());
	}
};

namespace detail {

/* Section 8.8: the room around the ship of `p`, in the level `g`. */
inline void measure_room(track_point &p, const geometry::level_geometry &g)
{
	const auto r{g.room_at(p.m.s.segment)};
	if (!r)
		return;
	p.geo = &g;
	p.room_known = true;
	p.room = r->kind;
	p.room_size = r->room;
	const auto &o{p.u.orient};
	const std::array<vec3, 3> axes{{o.forward, o.right, o.up}};
	for (std::size_t k{}; k != 3; ++k)
	{
		const auto &a{axes[k]};
		p.free[2 * k] = geometry::free_distance(g.mesh, p.m.s.segment, p.u.pos, a);
		p.free[2 * k + 1] = geometry::free_distance(g.mesh, p.m.s.segment, p.u.pos, vec3{{-a[0], -a[1], -a[2]}});
	}
	if (p.enemy_key >= 0 && p.u.enemy_distance > 0)
	{
		vec3 to;
		for (std::size_t k{}; k != 3; ++k)
			to[k] = p.u.enemy_rel_pos[k] / p.u.enemy_distance;
		p.sight = geometry::free_distance(g.mesh, p.m.s.segment, p.u.pos, to);
		p.line = p.sight + geometry::free_distance(g.mesh, p.m.s.segment, p.u.pos, vec3{{-to[0], -to[1], -to[2]}});
		p.line_known = true;
	}
}

}

[[nodiscard]]
inline track build_track(const merged_session &ms, const std::size_t player, const ship_model &ship = pyro_gx(), const level_geometries *const geo = nullptr, const session *const ses = nullptr)
{
	track tr;
	const auto &src{ms.players[player].samples};
	tr.pts.reserve(src.size());
	std::vector<std::array<double, 6>> raw(src.size());
	std::vector<bool> raw_valid(src.size());
	for (std::size_t i{}; i != src.size(); ++i)
	{
		track_point p;
		p.m = src[i];
		p.u = to_units(p.m.s);
		p.alive = (p.m.s.flags & sample_flag::alive) != 0;
		p.w = ms.tick_s[p.m.file];
		const double max_gap{3.5 * p.w};
		if (i && p.alive)
		{
			const auto &a{tr.pts.back()};
			const double dt{(static_cast<double>(p.m.local_ms) - static_cast<double>(a.m.local_ms)) / 1000.0};
			if (a.alive && a.m.file == p.m.file && a.m.level == p.m.level && dt > 0 && dt <= max_gap)
			{
				p.dt = dt;
				/* The thrust that turns the velocity before into this one. */
				const double keep{std::exp(-ship.linear_rate * dt)};
				vec3 c;
				for (std::size_t k{}; k != 3; ++k)
					c[k] = (p.u.vel[k] - a.u.vel[k] * keep) / ((1 - keep) * ship.max_speed);
				const auto cs{to_ship_frame(p.u.orient, c)};
				const double rkeep{std::exp(-ship.turn_rate * dt)};
				raw[i] = {{cs[2], cs[0], cs[1], 0, 0, 0}};
				for (std::size_t k{}; k != 3; ++k)
					raw[i][3 + k] = (p.u.rotvel[k] - a.u.rotvel[k] * rkeep) / ((1 - rkeep) * ship.max_turn);
				/* More than any thrust gives: a wall, a blast, a respawn. */
				raw_valid[i] = length(c) < 3;
			}
		}
		if (const auto kind{p.m.s.context & context_flag::kind_mask}; p.alive && kind != context_flag::kind_none)
		{
			p.enemy_key = kind == context_flag::kind_player ? p.m.enemy : 1000 + p.m.s.enemy_id;
			p.los = (p.m.s.context & context_flag::line_of_sight) != 0;
			p.fight = p.los && p.u.enemy_distance < limits::FIGHT_RANGE && !p.u.enemy_distance_scaled;
			if (p.u.enemy_distance > 0)
				p.own_approach = dot(p.u.vel, p.u.enemy_rel_pos) / p.u.enemy_distance;
		}
		if (geo && ses && p.alive && p.m.file < ses->files.size())
			if (const auto f{ses->files[p.m.file].file}; f < geo->size() && p.m.level < (*geo)[f].size())
				if (const auto g{(*geo)[f][p.m.level]})
					detail::measure_room(p, *g);
		tr.pts.push_back(p);
	}
	for (std::size_t i{}; i != tr.pts.size(); ++i)
	{
		auto &p{tr.pts[i]};
		if (raw_valid[i])
		{
			/* The mean with the neighbours of the same run. */
			auto sum{raw[i]};
			unsigned n{1};
			const auto add{[&](const std::size_t j) {
				for (std::size_t k{}; k != 6; ++k)
					sum[k] += raw[j][k];
				++n;
			}};
			if (i && raw_valid[i - 1] && p.dt > 0)
				add(i - 1);
			if (i + 1 != tr.pts.size() && raw_valid[i + 1] && tr.pts[i + 1].dt > 0)
				add(i + 1);
			for (std::size_t k{}; k != 6; ++k)
				p.est[k] = std::clamp(sum[k] / n, -1.0, k == 0 ? 2.0 : 1.0);
			p.est_valid = true;
		}
		p.exact = p.alive && (p.m.s.flags & sample_flag::controls);
		p.shared = p.exact && (p.m.s.flags2 & sample_flag2::controls_shared);
		p.has_ctl = p.exact || p.est_valid;
		if (p.exact)
			p.ctl = p.u.controls;
		else if (p.est_valid)
			p.ctl = p.est;
		if (p.m.s.flags2 & sample_flag2::afterburner_known)
		{
			p.ab_known = true;
			p.ab = (p.m.s.flags & sample_flag::afterburner) != 0;
		}
		else if (p.est_valid)
		{
			p.ab_known = p.ab_estimated = true;
			p.ab = p.est[0] > limits::BURN_THRUST;
		}
	}
	const auto me{static_cast<int>(player)};
	for (const auto &e : ms.events)
	{
		if (e.who == me)
			tr.own.push_back(e);
		else if (e.other == me && (e.e.type == record_type::hit || e.e.type == record_type::kill))
			tr.dealt.push_back(e);
		if (e.who != me && e.e.type == record_type::fire)
		{
			tr.enemy_fire.push_back(e);
			track::shooter_pose pose;
			if (e.who >= 0 && static_cast<std::size_t>(e.who) < ms.players.size())
			{
				const auto &their{ms.players[static_cast<std::size_t>(e.who)].samples};
				auto i{std::lower_bound(their.begin(), their.end(), e.t, [](const merged_sample &m, const std::int64_t v) { return m.t < v; })};
				if (i != their.begin() && (i == their.end() || e.t - std::prev(i)->t < i->t - e.t))
					--i;
				if (i != their.end() && std::abs(i->t - e.t) <= 100 && (i->s.flags & sample_flag::alive))
				{
					const auto u{to_units(i->s)};
					pose = {true, u.pos, u.orient.forward};
				}
			}
			tr.enemy_fire_from.push_back(pose);
		}
	}
	/* Section 8.9: the pickups, and the deaths (their drops). */
	{
		const auto sample_at{[&ms](const int who, const std::int64_t t, const std::int64_t tolerance) -> const merged_sample * {
			if (who < 0 || static_cast<std::size_t>(who) >= ms.players.size())
				return nullptr;
			const auto &their{ms.players[static_cast<std::size_t>(who)].samples};
			auto i{std::lower_bound(their.begin(), their.end(), t, [](const merged_sample &m, const std::int64_t v) { return m.t < v; })};
			/* The last one at or before `t` that lived. */
			if (i == their.end() || i->t > t)
			{
				if (i == their.begin())
					return nullptr;
				--i;
			}
			for (;; --i)
			{
				if (t - i->t > tolerance)
					return nullptr;
				if (i->s.flags & sample_flag::alive)
					return &*i;
				if (i == their.begin())
					return nullptr;
			}
		}};
		const auto geo_of{[&](const merged_sample &m) -> const geometry::level_geometry * {
			if (!geo || !ses || m.file >= ses->files.size())
				return nullptr;
			const auto f{ses->files[m.file].file};
			return f < geo->size() && m.level < (*geo)[f].size() ? (*geo)[f][m.level] : nullptr;
		}};
		struct drop
		{
			std::int64_t t;
			vec3 pos;
		};
		std::vector<drop> drops;
		for (const auto &e : ms.events)
			if (e.e.type == record_type::death)
				if (const auto m{sample_at(e.who, e.t, 500)})
					drops.push_back({e.t, to_units(m->s).pos});
		for (const auto &e : ms.events)
		{
			if (e.e.type != record_type::pickup)
				continue;
			const auto m{sample_at(e.who, e.t, 150)};
			if (!m)
				continue;
			track::powerup_spot sp;
			sp.t = e.t;
			sp.since = e.t - limits::SIGHT_LOOK_BACK_MS;
			sp.id = e.e.id;
			sp.pos = to_units(m->s).pos;
			sp.file = m->file;
			sp.level = m->level;
			sp.geo = geo_of(*m);
			sp.own = e.who == me;
			for (const auto &d : drops)
			{
				if (d.t > e.t)
					break;
				const vec3 off{{d.pos[0] - sp.pos[0], d.pos[1] - sp.pos[1], d.pos[2] - sp.pos[2]}};
				if (d.t > sp.since && length(off) <= limits::DROP_RADIUS)
					sp.since = d.t;
			}
			tr.spots.push_back(sp);
		}
	}
	return tr;
}

/*
 * The movement profile.
 */

/* A distribution in a few numbers. */
struct summary
{
	std::size_t n{};
	double mean{}, p10{}, p25{}, p50{}, p75{}, p90{};
};

[[nodiscard]]
inline summary summarise(std::vector<double> v)
{
	summary s;
	s.n = v.size();
	if (v.empty())
		return s;
	std::sort(v.begin(), v.end());
	double sum{};
	for (const double x : v)
		sum += x;
	s.mean = sum / static_cast<double>(v.size());
	const auto q{[&v](const double f) {
		const double at{f * static_cast<double>(v.size() - 1)};
		const auto i{static_cast<std::size_t>(at)};
		const double frac{at - static_cast<double>(i)};
		return i + 1 < v.size() ? v[i] * (1 - frac) + v[i + 1] * frac : v[i];
	}};
	s.p10 = q(0.10);
	s.p25 = q(0.25);
	s.p50 = q(0.50);
	s.p75 = q(0.75);
	s.p90 = q(0.90);
	return s;
}

constexpr std::size_t SPEED_BINS{8};		/* 20 units/s each, the last open */
constexpr double SPEED_BIN_WIDTH{20};
/* The bots' distances: inside the fight band's near edge (35), to the
 * close/mid border of the weapon table (60), to the band's far edge
 * (95), to the mid/distant border (150), 250, beyond.
 */
inline constexpr std::array<double, 5> DISTANCE_EDGES{{35, 60, 95, 150, 250}};
constexpr std::size_t DISTANCE_BINS{DISTANCE_EDGES.size() + 1};
constexpr std::size_t SHIELD_BUCKETS{5};	/* 25 shields each, the last 100 and more */
constexpr std::size_t WEAPON_SLOTS{10};
/* Section 8.8: bands of the length of the line of fire. */
inline constexpr std::array<double, 3> LINE_EDGES{{100, 200, 400}};
constexpr std::size_t LINE_BANDS{LINE_EDGES.size() + 1};
/* Dodging: the phases of the weave (the time since its last switch in
 * PHASE_BINS bins and one open bin; idle or in a run), and the bins of
 * the time to the first switch.
 */
constexpr std::size_t DODGE_PHASES{2 * (limits::PHASE_BINS + 1)};
constexpr std::int64_t DODGE_LATENCY_BIN_MS{20};
constexpr std::size_t DODGE_LATENCY_BINS{limits::DODGE_WINDOW_MS / DODGE_LATENCY_BIN_MS};

inline constexpr std::array<const char *, WEAPON_SLOTS> primary_names{{"laser", "vulcan", "spreadfire", "plasma", "fusion", "super laser", "gauss", "helix", "phoenix", "omega"}};
inline constexpr std::array<const char *, WEAPON_SLOTS> secondary_names{{"concussion", "homing", "proximity bomb", "smart", "mega", "flash", "guided", "smart mine", "mercury", "earthshaker"}};
inline constexpr std::array<const char *, bot::BOT_RANGE_BANDS> band_names{{"close (< 60)", "mid (60-150)", "distant (> 150)"}};
/* Section 8.9: the power pickups (the bots' power_class missiles and
 * the omega cannon: the game's POW_SMARTBOMB_WEAPON 20, POW_MEGA_WEAPON
 * 21, POW_OMEGA_WEAPON 31, POW_EARTHSHAKER_MISSILE 45 of Descent 2) and
 * every other pickup.
 */
constexpr std::size_t PICKUP_CLASSES{2};
inline constexpr std::array<const char *, PICKUP_CLASSES> pickup_class_names{{"power pickups", "other pickups"}};

[[nodiscard]]
constexpr std::size_t pickup_class_of(const std::uint8_t powerup_id)
{
	return powerup_id == 20 || powerup_id == 21 || powerup_id == 31 || powerup_id == 45 ? 0 : 1;
}

struct shield_bucket
{
	double seconds{};
	/* My own speed toward the enemy, mean (units/s, negative: away). */
	double approach_mean{};
	/* Shares of the time: closing in; backing off while facing the
	 * enemy; flying away turned from it.
	 */
	double approach{}, back_off{}, retreat{};
};

struct player_stats
{
	std::string callsign;
	bool bot{};
	unsigned sessions{};
	/* Seconds: recorded, alive, with exact controls, with estimated. */
	double recorded_s{}, alive_s{}, exact_s{}, estimated_s{};
	/* Of the exact: shared by the player's machine over the network
	 * (-sharemoves), not recorded there.
	 */
	double shared_s{};
	/* The estimator against the exact controls, where both exist: root
	 * mean square error of the three thrust axes (share of full thrust).
	 */
	double estimator_rms{};
	std::size_t estimator_n{};
	unsigned kills{}, deaths{}, suicides{};

	/* Speed (units/s) while alive. */
	summary speed;
	std::array<double, SPEED_BINS> speed_seconds{};
	/* Shares of the time alive above 85 % and below 20 % of the top speed. */
	double fast_share{}, slow_share{};

	/* Thrust: shares of the time with controls. */
	double forward_share{}, reverse_share{}, strafe_share{}, vertical_share{}, coast_share{}, roll_share{};
	/* The same in a fight (an enemy in sight within FIGHT_RANGE). */
	double fight_s{}, fight_strafe_share{}, fight_side_share{}, fight_vertical_share{}, fight_reverse_share{};

	/* Strafing in a fight: how long a run in one direction lasts (ms),
	 * reversals per minute of fight, the vertical against the sideways
	 * thrust (0 flat, 1 as much), the speed across (share of the top
	 * speed, upper quartile).
	 */
	summary strafe_run_ms;
	double strafe_reversals_per_min{}, strafe_vertical{}, strafe_speed{};
	/* The thrust across while strafing, share of full (mean). */
	double strafe_thrust{};

	/* Large turns. */
	unsigned large_turns{};
	/* Shares of them flown with reverse, sideways, forward thrust. */
	double reverse_turn_share{}, slide_turn_share{}, forward_turn_share{};
	/* The fastest backward speed in a reverse turn, share of the top speed. */
	double reverse_turn_speed{};
	summary turn_180_ms;
	/* The rotation rate in them, share of the top rate about one axis. */
	double turn_rate{};
	double turn_boost_share{}, turn_boost_burn_share{};

	/* Afterburner. */
	double ab_known_s{};
	/* Seconds of it known only from estimated thrust. */
	double ab_estimated_s{};
	double ab_share{};
	/* Seconds in each situation and the share of them burning: chasing
	 * (enemy in sight ahead, closing in), fleeing (enemy behind, moving
	 * away), crossing (no enemy in sight, not under fire), fighting (the
	 * rest).
	 */
	std::array<double, 4> ab_situation_s{}, ab_situation_rate{};
	summary ab_chase_distance;

	/* Distance to the enemy in sight (units). */
	double los_s{};
	summary los_distance, fire_distance;
	std::array<double, DISTANCE_BINS> distance_seconds{};

	/* Approach and retreat, by shields. */
	std::array<shield_bucket, SHIELD_BUCKETS> by_shields{};
	double approach_share{}, back_off_share{}, retreat_share{};
	/* The shields below which the player flies away from the enemy much
	 * more than above; `retreat_contrast` is the difference of the two
	 * shares (0: no such level found).
	 */
	double retreat_shields{}, retreat_contrast{};
	/* |speed toward or away| when moving so, share of the top speed. */
	double close_speed{};

	/* Dodging incoming fire (scan_dodge): the bursts aimed at the
	 * player that could be judged.
	 */
	unsigned dodge_triggers{};
	/* The share of them followed by a switch of the strafe within the
	 * reaction window; the share the quiet moments at the same phase of
	 * the weave give (what the rhythm alone would have done); the quiet
	 * moments looked at, and the share of all of them with a switch (how
	 * much the player weaves anyway).
	 */
	double dodge_rate{}, dodge_baseline{};
	std::size_t dodge_baseline_n{};
	double dodge_weave_rate{};
	/* The switches beyond the rhythm, per burst that left room for one:
	 * (switches - expected) / (bursts - expected), and its standard error.
	 * Not measurable with too little room (DODGE_MIN_ROOM: a weave that
	 * switches in the window nearly always).
	 */
	double dodge_prob{}, dodge_se{};
	double dodge_room{};
	bool dodge_measurable{};
	/* The median time from the shot to the extra switches (ms). */
	double dodge_reaction_ms{};
	/* Of the switches in the window: the share that reverse the sideways
	 * motion (against a shooter that leads), after bursts and in quiet
	 * moments; how many after bursts.
	 */
	double dodge_reverse_share{}, dodge_reverse_baseline{};
	unsigned dodge_reverse_n{};
	/* The afterburner lit in the window: after bursts, in quiet moments. */
	double dodge_burn_rate{}, dodge_burn_baseline{};
	/* The shooter hit within BURST_HIT_MS: after the bursts answered by a
	 * switch, after the others, and how many of each.
	 */
	double dodge_hit_after_switch{}, dodge_hit_after_none{};
	unsigned dodge_switch_n{}, dodge_none_n{};
	/* The shots of the other players in the recordings, and the hits
	 * they dealt to this player.  Hits without any shot: the recording
	 * lacks the enemies' fire (older recorders left out the shots of
	 * players whose samples were not recorded, such as the bots without
	 * -recordmoves-bots), and dodging cannot be measured.
	 */
	unsigned enemy_shots{}, hits_taken_from_players{};

	/* Weapons. */
	unsigned primary_shots{}, secondary_shots{};
	std::array<std::array<unsigned, WEAPON_SLOTS>, bot::BOT_RANGE_BANDS> primary_by_band{};
	std::array<unsigned, WEAPON_SLOTS> secondary_count{};
	summary secondary_distance;
	unsigned volleys{}, volley_max{};
	double volley_size{};
	summary volley_gap_s;

	/* Pickups. */
	unsigned pickups{};
	double pickups_per_min{}, pickup_detour_share{}, pickup_in_fight_share{};
	unsigned pickup_detour_n{};

	/* Section 8.9: how the player goes for what it sees, per pickup
	 * class (pickup_class_of): the powerups it had in sight before
	 * someone took them; of those, the share it took, went for (came
	 * close, limits::GO_FOR_NEAR / GO_FOR_SHARE) and went for but lost
	 * to another; the same for those first seen in a fight and outside
	 * one; of the ones it took, the distance at the first sight, the time
	 * from it to the pickup, the way flown beyond that distance, and the
	 * share it saw off its course (more than DETOUR_DEG from where it
	 * flew); and its pickups it never had in sight before.
	 */
	struct pickup_sight
	{
		unsigned seen{}, taken{}, went{}, lost{}, fight_seen{}, fight_went{}, calm_seen{}, calm_went{}, unseen_taken{};
		double taken_share{}, went_share{}, fight_went_share{}, calm_went_share{}, off_course_share{};
		summary sight_distance, take_s, extra_path;
	};
	std::array<pickup_sight, PICKUP_CLASSES> pickup_sight{};
	/* The share of the sightings judged with the level's geometry (else
	 * by distance alone, SIGHT_NO_GEOMETRY).
	 */
	double pickup_sight_geometry_share{};

	/* After losing sight of an enemy. */
	unsigned sight_losses{};
	double pursue_share{};
	summary pursuit_s;

	/* Hits. */
	unsigned hits_dealt{}, splash_dealt{}, hits_taken{};
	double damage_dealt{}, damage_taken{};
	/* Direct hits per primary shot (a shot of several bolts can hit more
	 * than once).
	 */
	double hits_per_shot{};

	/* Section 8.8: by level and by room class (where the level's
	 * geometry is known).
	 */
	struct room_traits
	{
		double alive_s{}, speed{}, fight_s{};
		/* Sideways or vertical thrust, share of the fight time with controls. */
		double strafe_share{};
		/* The distance to the enemy in sight; the line of fire then
		 * (the free distance toward the enemy and away from it); the
		 * distance as a share of that line; the free room to the nearer
		 * side (left or right) in a fight.
		 */
		summary distance, line, line_share, side_room;
		unsigned turns{};
		double reverse_turn_share{}, slide_turn_share{}, forward_turn_share{};
	};
	struct level_traits
	{
		std::string label, source, character;
		/* tight, medium, open; then all of them. */
		std::array<room_traits, geometry::ROOM_CLASSES> by_class{};
		room_traits all;
	};
	std::vector<level_traits> levels;
	/* Seconds alive with the room known. */
	double room_known_s{};
	/* The distance to the enemy in sight as a share of the line of fire
	 * (the free distance toward the enemy, through it, and away from
	 * it): the distance normalised by what the map offers.
	 */
	summary line_share;
	/* The distance at the primary shots by the length of the line of
	 * fire (LINE_EDGES): how much the map sets the distance.
	 */
	std::array<summary, LINE_BANDS> fire_distance_by_line{};
	/* Levels with at least 20 primary shots with the enemy in sight. */
	unsigned levels_with_fire{};

	[[nodiscard]]
	double estimated_share() const
	{
		return exact_s + estimated_s > 0 ? estimated_s / (exact_s + estimated_s) : 0;
	}
};

namespace detail {

struct accum
{
	double recorded_s{}, alive_s{}, exact_s{}, estimated_s{}, shared_s{};
	double est_err2{};
	std::size_t est_n{};
	std::vector<double> speed;
	std::array<double, SPEED_BINS> speed_seconds{};
	double fast_s{}, slow_s{};
	double ctl_s{}, forward_s{}, reverse_s{}, side_s{}, vert_s{}, coast_s{}, roll_s{};
	double fight_ctl_s{}, fight_strafe_s{}, fight_side_s{}, fight_vert_s{}, fight_reverse_s{}, fight_s{};
	std::vector<double> strafe_run_ms, lateral_speed;
	double strafe_side_sum{}, strafe_vert_sum{}, strafe_thrust_sum{};
	unsigned strafe_reversals{};
	unsigned turns{}, turns_ctl{}, turns_reverse{}, turns_slide{}, turns_forward{}, boosts{}, boost_burns{}, boost_n{};
	std::vector<double> turn_180_ms, reverse_speed, turn_rate;
	double ab_known_s{}, ab_estimated_s{}, ab_s{};
	std::array<double, 4> ab_situation_s{}, ab_situation_on_s{};
	std::vector<double> ab_chase_distance;
	double los_s{};
	std::vector<double> los_distance, fire_distance, secondary_distance;
	std::array<double, DISTANCE_BINS> distance_seconds{};
	std::array<double, 256> shield_s{}, shield_retreat_s{};
	std::array<double, SHIELD_BUCKETS> bucket_s{}, bucket_approach_sum{}, bucket_approach_s{}, bucket_back_s{}, bucket_retreat_s{};
	std::vector<double> close_speed;
	/* Dodging: the quiet moments per phase of the weave, and the bursts. */
	struct phase_cell
	{
		unsigned n{}, switched{};
		/* The first switch after the moment, per DODGE_LATENCY_BIN_MS. */
		std::array<unsigned, DODGE_LATENCY_BINS> first{};
	};
	std::array<phase_cell, DODGE_PHASES> quiet{};
	struct burst
	{
		std::size_t phase{};
		bool switched{};
		/* The first switch (ms after the shot), -1: none in DODGE_WINDOW_MS. */
		std::int64_t first{-1};
		bool hit{};
	};
	std::vector<burst> bursts;
	unsigned quiet_n{}, quiet_switched{};
	unsigned reverse_n{}, reverse{}, quiet_reverse_n{}, quiet_reverse{};
	unsigned burn_n{}, burn{}, quiet_burn_n{}, quiet_burn{};
	unsigned enemy_shots{}, hits_taken_from_players{};
	unsigned primary_shots{}, secondary_shots{};
	std::array<std::array<unsigned, WEAPON_SLOTS>, bot::BOT_RANGE_BANDS> primary_by_band{};
	std::array<unsigned, WEAPON_SLOTS> secondary_count{};
	std::vector<double> volley_sizes, volley_gap_s;
	unsigned pickups{}, pickup_detour_n{}, pickup_detours{}, pickup_in_fight{};
	struct pickup_sight_acc
	{
		unsigned seen{}, taken{}, went{}, lost{}, fight_seen{}, fight_went{}, calm_seen{}, calm_went{}, unseen_taken{}, off_course{}, off_course_n{};
		std::vector<double> sight_distance, take_s, extra_path;
	};
	std::array<pickup_sight_acc, PICKUP_CLASSES> pickup_sight{};
	unsigned sightings{}, sightings_geo{};
	unsigned sight_losses{}, pursuits{};
	std::vector<double> pursuit_s;
	unsigned hits_dealt{}, splash_dealt{}, hits_taken{}, kills{}, deaths{}, suicides{};
	double damage_dealt{}, damage_taken{};
	/* Section 8.8: per level (by its geometry) and room class. */
	struct room_acc
	{
		double alive_s{}, speed_sum{}, fight_s{}, fight_ctl_s{}, fight_strafe_s{};
		std::vector<double> distance, line, line_share, side_room;
		unsigned turns{}, turns_ctl{}, reverse{}, slide{}, forward{};
	};
	struct level_acc
	{
		const geometry::level_geometry *geo{};
		std::array<room_acc, geometry::ROOM_CLASSES + 1> by{};
	};
	std::vector<level_acc> levels;
	double room_known_s{};
	std::vector<double> line_share;
	std::array<std::vector<double>, LINE_BANDS> fire_distance_by_line;
	/* Primary shots with the enemy in sight per level (by its geometry). */
	std::vector<std::pair<const geometry::level_geometry *, unsigned>> fire_per_level;
	/* The large turns of the track being scanned: first point, and how
	 * flown ('r' reverse, 's' sliding, 'f' forward, 'c' otherwise, '-'
	 * without enough controls to tell).
	 */
	std::vector<std::pair<std::size_t, char>> turn_marks;
};

enum situation : std::size_t
{
	chasing,
	fleeing,
	crossing,
	fighting,
};

[[nodiscard]]
inline situation situation_of(const track_point &p, const double approach_speed, const double turning_rate)
{
	/* Chasing: the nose stays on the enemy.  A ship that swings away
	 * from it (the start of a flight) still has it ahead and still
	 * drifts toward it for a moment.
	 */
	if (p.los && (p.m.s.context & context_flag::in_my_cone) && p.own_approach > approach_speed && std::hypot(p.u.rotvel[0], p.u.rotvel[1]) < turning_rate)
		return chasing;
	if ((p.los || p.m.s.attacked_mask) && p.enemy_key >= 0 && p.u.enemy_off_nose_deg > 100 && p.own_approach < -approach_speed)
		return fleeing;
	if (!p.los && !p.m.s.attacked_mask)
		return crossing;
	return fighting;
}

/* Speed, thrust, afterburner, distance, approach: per sample. */
inline void scan_samples(const track &tr, accum &a, const ship_model &ship, const std::vector<bool> &after_turn)
{
	using limits::CONTROL_USED;
	const double approach_speed{limits::APPROACH_SHARE * ship.max_speed};
	for (std::size_t i{}; i != tr.pts.size(); ++i)
	{
		const auto &p{tr.pts[i]};
		const double w{p.w};
		a.recorded_s += w;
		if (!p.alive)
			continue;
		a.alive_s += w;
		a.speed.push_back(p.u.speed);
		a.speed_seconds[std::min(static_cast<std::size_t>(p.u.speed / SPEED_BIN_WIDTH), SPEED_BINS - 1)] += w;
		if (p.u.speed > 0.85 * ship.max_speed)
			a.fast_s += w;
		if (p.u.speed < 0.2 * ship.max_speed)
			a.slow_s += w;
		if (p.exact && p.est_valid)
		{
			for (std::size_t k{}; k != 3; ++k)
				a.est_err2 += (p.est[k] - p.u.controls[k]) * (p.est[k] - p.u.controls[k]);
			++a.est_n;
		}
		if (p.fight)
			a.fight_s += w;
		if (p.has_ctl)
		{
			(p.exact ? a.exact_s : a.estimated_s) += w;
			if (p.shared)
				a.shared_s += w;
			a.ctl_s += w;
			const bool fwd{p.ctl[0] > CONTROL_USED}, rev{p.ctl[0] < -CONTROL_USED};
			const bool side{std::abs(p.ctl[1]) > CONTROL_USED}, vert{std::abs(p.ctl[2]) > CONTROL_USED};
			if (fwd)
				a.forward_s += w;
			if (rev)
				a.reverse_s += w;
			if (side)
				a.side_s += w;
			if (vert)
				a.vert_s += w;
			if (!fwd && !rev && !side && !vert)
				a.coast_s += w;
			if (std::abs(p.ctl[5]) > CONTROL_USED)
				a.roll_s += w;
			if (p.fight)
			{
				a.fight_ctl_s += w;
				if (side || vert)
					a.fight_strafe_s += w;
				if (side)
					a.fight_side_s += w;
				if (vert)
					a.fight_vert_s += w;
				if (rev)
					a.fight_reverse_s += w;
			}
		}
		if (p.ab_known)
		{
			a.ab_known_s += w;
			if (p.ab_estimated)
				a.ab_estimated_s += w;
			const auto sit{situation_of(p, approach_speed, limits::TURN_RATE_SHARE * ship.max_turn)};
			a.ab_situation_s[sit] += w;
			if (p.ab)
			{
				a.ab_s += w;
				a.ab_situation_on_s[sit] += w;
				/* Not the push after a turn: that is its own habit, at
				 * any distance.
				 */
				if (sit == chasing && !p.u.enemy_distance_scaled && !after_turn[i])
					a.ab_chase_distance.push_back(p.u.enemy_distance);
			}
		}
		if (p.los && !p.u.enemy_distance_scaled)
		{
			a.los_s += w;
			a.los_distance.push_back(p.u.enemy_distance);
			const auto bin{static_cast<std::size_t>(std::upper_bound(DISTANCE_EDGES.begin(), DISTANCE_EDGES.end(), p.u.enemy_distance) - DISTANCE_EDGES.begin())};
			a.distance_seconds[bin] += w;
		}
		if (p.fight)
		{
			const bool away{p.own_approach < -approach_speed};
			const bool retreat{away && p.u.enemy_off_nose_deg > limits::TURNED_AWAY_DEG};
			const std::size_t b{std::min<std::size_t>(p.m.s.shields / 25u, SHIELD_BUCKETS - 1)};
			a.bucket_s[b] += w;
			a.bucket_approach_sum[b] += p.own_approach * w;
			if (p.own_approach > approach_speed)
				a.bucket_approach_s[b] += w;
			if (retreat)
				a.bucket_retreat_s[b] += w;
			else if (away)
				a.bucket_back_s[b] += w;
			a.shield_s[p.m.s.shields] += w;
			if (retreat)
				a.shield_retreat_s[p.m.s.shields] += w;
			if (std::abs(p.own_approach) > approach_speed)
				a.close_speed.push_back(std::abs(p.own_approach) / ship.max_speed);
		}
	}
}

/* The runs of the strafe in a fight. */
inline void scan_strafe(const track &tr, accum &a)
{
	bool in_run{};
	std::array<double, 2> dir{}, last_dir{};
	double run_s{};
	std::int64_t last_end{};
	bool have_last{};
	const auto end_run{[&](const std::int64_t t) {
		if (!in_run)
			return;
		in_run = false;
		if (run_s >= 0.1)
			a.strafe_run_ms.push_back(run_s * 1000);
		last_dir = dir;
		last_end = t;
		have_last = true;
	}};
	for (const auto &p : tr.pts)
	{
		const double l{std::hypot(p.ctl[1], p.ctl[2])};
		const bool active{p.fight && p.has_ctl && l >= limits::CONTROL_USED};
		if (in_run && (!active || p.dt <= 0 || p.ctl[1] * dir[0] + p.ctl[2] * dir[1] <= 0))
			end_run(p.m.t);
		if (!active)
			continue;
		if (!in_run)
		{
			in_run = true;
			run_s = 0;
			dir = {{p.ctl[1] / l, p.ctl[2] / l}};
			/* The other way, right after the last run: a reversal. */
			if (have_last && p.dt > 0 && p.m.t - last_end <= 300 && dir[0] * last_dir[0] + dir[1] * last_dir[1] <= 0)
				++a.strafe_reversals;
		}
		run_s += p.w;
		a.lateral_speed.push_back(std::hypot(p.u.vel_ship[0], p.u.vel_ship[1]));
		a.strafe_side_sum += std::abs(p.ctl[1]);
		a.strafe_vert_sum += std::abs(p.ctl[2]);
		a.strafe_thrust_sum += std::min(l, 1.0);
	}
	if (!tr.pts.empty())
		end_run(tr.pts.back().m.t);
}

/* The large turns: how they are flown.  Returns, per point, whether it
 * is in the push after one.
 */
inline std::vector<bool> scan_turns(const track &tr, accum &a, const ship_model &ship)
{
	const auto &pts{tr.pts};
	std::vector<bool> after_turn(pts.size());
	const auto rate{[&pts](const std::size_t i) {
		return std::hypot(pts[i].u.rotvel[0], pts[i].u.rotvel[1]);
	}};
	const double min_rate{limits::TURN_RATE_SHARE * ship.max_turn};
	for (std::size_t i{}; i < pts.size();)
	{
		if (!pts[i].alive || rate(i) < min_rate)
		{
			++i;
			continue;
		}
		std::size_t j{i};
		double angle{}, seconds{};
		while (j + 1 < pts.size() && pts[j + 1].dt > 0 && rate(j + 1) >= min_rate)
		{
			++j;
			angle += rate(j) * pts[j].dt * 360;
			seconds += pts[j].dt;
		}
		const std::size_t first{i};
		i = j + 1;
		if (angle < limits::LARGE_TURN_DEG)
			continue;
		const double ms_180{seconds * 1000 * 180 / angle};
		if (ms_180 > limits::LARGE_TURN_MAX_MS)
			continue;
		++a.turns;
		a.turn_180_ms.push_back(ms_180);
		a.turn_rate.push_back(angle / 360 / seconds / ship.max_turn);
		unsigned n{}, rev{}, fwd{}, slide{};
		double back_speed{};
		for (std::size_t k{first}; k <= j; ++k)
		{
			const auto &p{pts[k]};
			back_speed = std::max(back_speed, -p.u.vel_ship[2]);
			if (!p.has_ctl)
				continue;
			++n;
			if (p.ctl[0] < -limits::CONTROL_USED)
				++rev;
			else if (p.ctl[0] > limits::CONTROL_USED)
				++fwd;
			if (std::hypot(p.ctl[1], p.ctl[2]) > limits::CONTROL_USED)
				++slide;
		}
		char mark{'-'};
		if (n * 2 >= j - first + 1)
		{
			++a.turns_ctl;
			mark = 'c';
			if (rev >= limits::TURN_THRUST_SHARE * n)
			{
				++a.turns_reverse;
				a.reverse_speed.push_back(back_speed / ship.max_speed);
				mark = 'r';
			}
			else if (slide >= limits::TURN_THRUST_SHARE * n)
			{
				++a.turns_slide;
				mark = 's';
			}
			else if (fwd >= limits::TURN_THRUST_SHARE * n)
			{
				++a.turns_forward;
				mark = 'f';
			}
		}
		a.turn_marks.emplace_back(first, mark);
		/* The push after the turn. */
		double thrust{}, time{};
		bool burn{};
		unsigned m{};
		for (std::size_t k{j + 1}; k < pts.size() && pts[k].dt > 0 && time < limits::BOOST_MS / 1000; ++k)
		{
			time += pts[k].dt;
			after_turn[k] = true;
			if (!pts[k].has_ctl)
				continue;
			thrust += pts[k].ctl[0];
			++m;
			burn |= pts[k].ab_known && pts[k].ab;
		}
		if (m && time >= limits::BOOST_MS / 2000)
		{
			++a.boost_n;
			if (thrust / m > limits::BOOST_THRUST)
			{
				++a.boosts;
				if (burn)
					++a.boost_burns;
			}
		}
	}
	return after_turn;
}

/* The switches of the strafe: the moments a run of sideways/vertical
 * thrust starts, from none or in another direction (the runs of
 * scan_strafe, in a fight or not), with the run's direction (ship
 * frame: right, up).  A run shorter than SWITCH_MIN_S is no switch.
 */
struct strafe_switch
{
	std::int64_t t{};
	std::array<double, 2> dir{};
};

struct weave
{
	std::vector<strafe_switch> switches;
	/* Per point: in a run (of at least SWITCH_MIN_S); the time of the
	 * first point of its stretch without a gap (a death, a level, a
	 * hole in the recording).
	 */
	std::vector<bool> in_run;
	std::vector<std::int64_t> stretch_start;
};

[[nodiscard]]
inline weave weave_of(const track &tr)
{
	weave w;
	const auto &pts{tr.pts};
	w.in_run.assign(pts.size(), false);
	w.stretch_start.assign(pts.size(), 0);
	bool in_run{};
	std::array<double, 2> dir{};
	std::size_t run_first{};
	double run_s{};
	std::int64_t stretch{};
	const auto end_run{[&](const std::size_t end) {
		if (!in_run)
			return;
		in_run = false;
		if (run_s < limits::SWITCH_MIN_S)
			return;
		w.switches.push_back({pts[run_first].m.t, dir});
		for (std::size_t k{run_first}; k != end; ++k)
			w.in_run[k] = true;
	}};
	for (std::size_t i{}; i != pts.size(); ++i)
	{
		const auto &p{pts[i]};
		if (!i || p.dt <= 0)
			stretch = p.m.t;
		w.stretch_start[i] = stretch;
		const double l{std::hypot(p.ctl[1], p.ctl[2])};
		const bool active{p.alive && p.has_ctl && l >= limits::CONTROL_USED};
		if (in_run && (!active || p.dt <= 0 || p.ctl[1] * dir[0] + p.ctl[2] * dir[1] <= 0))
			end_run(i);
		if (!active)
			continue;
		if (!in_run)
		{
			in_run = true;
			run_first = i;
			run_s = 0;
			dir = {{p.ctl[1] / l, p.ctl[2] / l}};
		}
		run_s += p.w;
	}
	end_run(pts.size());
	return w;
}

/* What the player did after the moment `pts[i0]`, and the phase of its
 * weave then.  Nothing if the track has a gap within DODGE_WINDOW_MS, or
 * the rhythm before the moment is not known (a stretch that began less
 * than PHASE_BINS bins before, without a switch since).
 */
struct moment_look
{
	std::size_t phase{};
	/* A switch in the reaction window. */
	bool switched{};
	/* The first switch after the moment (ms), -1: none in DODGE_WINDOW_MS. */
	std::int64_t first{-1};
	/* The switch in the window turns against the sideways motion of the
	 * moment (known if the ship moved sideways then).
	 */
	std::optional<bool> reverse;
	/* The afterburner lit in the window (known if it was off then). */
	std::optional<bool> burn;
};

[[nodiscard]]
inline std::optional<moment_look> look_after(const track &tr, const weave &w, const std::size_t i0, const ship_model &ship)
{
	const auto &pts{tr.pts};
	const auto &p0{pts[i0]};
	const auto t0{p0.m.t};
	if (!p0.alive)
		return std::nullopt;
	std::size_t j{i0};
	bool burn{};
	while (j + 1 < pts.size() && pts[j + 1].dt > 0 && pts[j + 1].m.t - t0 <= limits::DODGE_WINDOW_MS)
	{
		++j;
		const auto after{pts[j].m.t - t0};
		if (after > limits::REACT_FROM_MS && after <= limits::REACT_TO_MS && pts[j].ab_known && pts[j].ab)
			burn = true;
	}
	if (pts[j].m.t - t0 < limits::DODGE_WINDOW_MS - 50)
		return std::nullopt;
	const auto &sw{w.switches};
	const auto next{std::upper_bound(sw.begin(), sw.end(), t0, [](const std::int64_t v, const strafe_switch &s) { return v < s.t; })};
	const auto stretch{w.stretch_start[i0]};
	const bool known{next != sw.begin() && std::prev(next)->t >= stretch};
	const std::int64_t since{t0 - (known ? std::prev(next)->t : stretch)};
	constexpr std::int64_t open_from{static_cast<std::int64_t>(limits::PHASE_BINS) * limits::PHASE_BIN_MS};
	if (!known && since < open_from)
		return std::nullopt;
	moment_look m;
	m.phase = (w.in_run[i0] ? limits::PHASE_BINS + 1 : 0) + static_cast<std::size_t>(std::min(since, open_from) / limits::PHASE_BIN_MS);
	if (next != sw.end() && next->t - t0 <= limits::DODGE_WINDOW_MS)
		m.first = next->t - t0;
	for (auto k{next}; k != sw.end() && k->t - t0 <= limits::REACT_TO_MS; ++k)
	{
		if (k->t - t0 <= limits::REACT_FROM_MS)
			continue;
		m.switched = true;
		if (std::hypot(p0.u.vel_ship[0], p0.u.vel_ship[1]) > 0.2 * ship.max_speed)
			m.reverse = k->dir[0] * p0.u.vel_ship[0] + k->dir[1] * p0.u.vel_ship[1] < 0;
		break;
	}
	if (p0.ab_known && !p0.ab)
		m.burn = burn;
	return m;
}

/* Reactions to incoming fire, against the quiet moments of the same
 * situation and phase of the weave.
 */
inline void scan_dodge(const track &tr, accum &a, const ship_model &ship)
{
	const auto &pts{tr.pts};
	const auto w{weave_of(tr)};
	const double cone{std::cos(bot::radians(30))};
	std::map<int, std::int64_t> last_shot;
	std::vector<std::int64_t> shots;
	a.enemy_shots += static_cast<unsigned>(tr.enemy_fire.size());
	const auto tally{[&a](const moment_look &m, const bool quiet) {
		if (m.reverse)
		{
			++(quiet ? a.quiet_reverse_n : a.reverse_n);
			if (*m.reverse)
				++(quiet ? a.quiet_reverse : a.reverse);
		}
		if (m.burn)
		{
			++(quiet ? a.quiet_burn_n : a.burn_n);
			if (*m.burn)
				++(quiet ? a.quiet_burn : a.burn);
		}
	}};
	for (std::size_t n{}; n != tr.enemy_fire.size(); ++n)
	{
		const auto &e{tr.enemy_fire[n]};
		const auto last{last_shot.find(e.who)};
		const bool onset{last == last_shot.end() || e.t - last->second > limits::FIRE_ONSET_MS};
		last_shot[e.who] = e.t;
		const auto i{tr.at(e.t, 100)};
		if (!i || !pts[*i].alive)
			continue;
		const auto &p{pts[*i]};
		/* Aimed at me: the shooter is the enemy in sight and faces me,
		 * or (another one, recorded too) its nose points at me; near.
		 * A shot of an enemy that is neither (not recorded) may have
		 * been: it is no quiet moment, but no burst to judge either.
		 */
		bool aimed{}, known{true};
		if (p.los && p.enemy_key == e.who)
			aimed = (p.m.s.context & context_flag::me_in_its_cone) && !p.u.enemy_distance_scaled && p.u.enemy_distance <= limits::DODGE_RANGE;
		else if (const auto &from{tr.enemy_fire_from[n]}; from.known)
		{
			const vec3 to_me{{p.u.pos[0] - from.pos[0], p.u.pos[1] - from.pos[1], p.u.pos[2] - from.pos[2]}};
			const double d{length(to_me)};
			aimed = d > 0 && d <= limits::DODGE_RANGE && dot(from.forward, to_me) >= cone * d;
		}
		else
			known = false;
		if (aimed || !known)
			shots.push_back(e.t);
		if (!onset || !aimed)
			continue;
		const auto m{look_after(tr, w, *i, ship)};
		if (!m)
			continue;
		accum::burst b{m->phase, m->switched, m->first, false};
		/* tr.own is sorted by time: the events are. */
		for (auto h{std::lower_bound(tr.own.begin(), tr.own.end(), e.t, [](const merged_event &x, const std::int64_t v) { return x.t < v; })}; h != tr.own.end() && h->t <= e.t + limits::BURST_HIT_MS; ++h)
			if (h->e.type == record_type::hit && h->other == e.who)
			{
				b.hit = true;
				break;
			}
		a.bursts.push_back(b);
		tally(*m, false);
	}
	/* shots is sorted: the events are.  Quiet: no shot aimed at me. */
	const auto shot_between{[&shots](const std::int64_t from, const std::int64_t to) {
		const auto i{std::lower_bound(shots.begin(), shots.end(), from)};
		return i != shots.end() && *i <= to;
	}};
	std::int64_t next{};
	bool any{};
	for (std::size_t i{}; i != pts.size(); ++i)
	{
		const auto &p{pts[i]};
		if ((any && p.m.t < next) || !p.alive || !p.los || p.enemy_key < 0 || p.enemy_key >= 1000)
			continue;
		if (!(p.m.s.context & context_flag::me_in_its_cone) || p.u.enemy_distance_scaled || p.u.enemy_distance > limits::DODGE_RANGE)
			continue;
		if (shot_between(p.m.t - limits::QUIET_BEFORE_MS, p.m.t + limits::DODGE_WINDOW_MS))
			continue;
		const auto m{look_after(tr, w, i, ship)};
		if (!m)
			continue;
		any = true;
		next = p.m.t + limits::BASELINE_STRIDE_MS;
		auto &c{a.quiet[m->phase]};
		++c.n;
		++a.quiet_n;
		if (m->switched)
		{
			++c.switched;
			++a.quiet_switched;
		}
		if (m->first >= 0)
			++c.first[std::min(static_cast<std::size_t>(m->first / DODGE_LATENCY_BIN_MS), DODGE_LATENCY_BINS - 1)];
		tally(*m, true);
	}
}

/* Own shots, missiles, pickups, hits. */
inline void scan_events(const track &tr, accum &a)
{
	const auto &pts{tr.pts};
	std::int64_t last_missile{}, volley_start{};
	unsigned volley{};
	bool any_volley{};
	const auto end_volley{[&] {
		if (volley)
			a.volley_sizes.push_back(volley);
		volley = 0;
	}};
	for (const auto &e : tr.own)
	{
		const auto i{tr.at(e.t, 150)};
		switch (e.e.type)
		{
			case record_type::fire:
				if (e.e.kind == fire_kind::primary)
				{
					++a.primary_shots;
					if (i && pts[*i].los && !pts[*i].u.enemy_distance_scaled)
					{
						const double d{pts[*i].u.enemy_distance};
						a.fire_distance.push_back(d);
						if (const auto &q{pts[*i]}; q.line_known)
						{
							a.fire_distance_by_line[static_cast<std::size_t>(std::upper_bound(LINE_EDGES.begin(), LINE_EDGES.end(), q.line) - LINE_EDGES.begin())].push_back(d);
							const auto f{std::find_if(a.fire_per_level.begin(), a.fire_per_level.end(), [&q](const auto &x) { return x.first == q.geo; })};
							if (f == a.fire_per_level.end())
								a.fire_per_level.emplace_back(q.geo, 1u);
							else
								++f->second;
						}
						if (e.e.id < WEAPON_SLOTS)
							++a.primary_by_band[static_cast<std::size_t>(bot::band_of(d))][e.e.id];
					}
				}
				else if (e.e.kind == fire_kind::secondary)
				{
					++a.secondary_shots;
					if (e.e.id < WEAPON_SLOTS)
						++a.secondary_count[e.e.id];
					if (i && pts[*i].los && !pts[*i].u.enemy_distance_scaled)
						a.secondary_distance.push_back(pts[*i].u.enemy_distance);
					/* Mines are dropped, not fired in volleys. */
					if (e.e.id < WEAPON_SLOTS && bot::role_of(static_cast<bot::secondary>(e.e.id)) == bot::missile_role::mine)
						break;
					if (volley && e.t - last_missile > limits::VOLLEY_GAP_MS)
						end_volley();
					if (!volley)
					{
						if (any_volley && e.t - volley_start <= limits::VOLLEY_SAME_FIGHT_MS)
							a.volley_gap_s.push_back(static_cast<double>(e.t - volley_start) / 1000.0);
						any_volley = true;
						volley_start = e.t;
					}
					++volley;
					last_missile = e.t;
				}
				break;
			case record_type::pickup:
			{
				++a.pickups;
				if (!i)
					break;
				const auto &p{pts[*i]};
				bool fight{};
				for (std::size_t k{*i}; k < pts.size() && p.m.t - pts[k].m.t <= 1000; --k)
				{
					fight |= pts[k].fight;
					if (!k)
						break;
				}
				if (fight)
					++a.pickup_in_fight;
				/* The course a moment before against the way to the pickup. */
				const auto j{tr.at(e.t - limits::DETOUR_LOOK_BACK_MS, 150)};
				if (!j || !pts[*j].alive || pts[*j].m.file != p.m.file || pts[*j].m.level != p.m.level || pts[*j].u.speed < 10)
					break;
				const auto &q{pts[*j]};
				const vec3 way{{p.u.pos[0] - q.u.pos[0], p.u.pos[1] - q.u.pos[1], p.u.pos[2] - q.u.pos[2]}};
				const double far_off{length(way)};
				if (far_off < 5)
					break;
				++a.pickup_detour_n;
				if (dot(way, q.u.vel) / (far_off * q.u.speed) < std::cos(bot::radians(limits::DETOUR_DEG)))
					++a.pickup_detours;
				break;
			}
			case record_type::hit:
				++a.hits_taken;
				if (e.e.kind == attacker_kind::player && e.other >= 0 && e.other != e.who)
					++a.hits_taken_from_players;
				a.damage_taken += e.e.value / 256.0;
				break;
			case record_type::death:
				++a.deaths;
				break;
			case record_type::kill:
				if (e.other == e.who)
					++a.suicides;
				break;
			default:
				break;
		}
	}
	end_volley();
	for (const auto &e : tr.dealt)
	{
		if (e.e.type == record_type::kill)
			++a.kills;
		else if (e.e.flags & hit_flag::splash)
		{
			++a.splash_dealt;
			a.damage_dealt += e.e.value / 256.0;
		}
		else
		{
			++a.hits_dealt;
			a.damage_dealt += e.e.value / 256.0;
		}
	}
}

/* Section 8.9: the powerups the player had in sight before someone took
 * them, and what it did about them.
 */
inline void scan_pickup_sight(const track &tr, accum &a)
{
	const auto &pts{tr.pts};
	const double cone{std::cos(bot::radians(limits::VIEW_HALF_DEG))};
	const auto same_level{[](const track_point &p, const track::powerup_spot &sp) {
		return (sp.geo && p.geo == sp.geo) || (p.m.file == sp.file && p.m.level == sp.level);
	}};
	const auto off{[](const vec3 &from, const vec3 &to) {
		return vec3{{to[0] - from[0], to[1] - from[1], to[2] - from[2]}};
	}};
	for (const auto &sp : tr.spots)
	{
		auto &c{a.pickup_sight[pickup_class_of(sp.id)]};
		auto i{static_cast<std::size_t>(std::lower_bound(pts.begin(), pts.end(), sp.since, [](const track_point &p, const std::int64_t v) { return p.m.t < v; }) - pts.begin())};
		/* The first sight. */
		std::optional<std::size_t> first;
		bool by_geo{};
		for (std::size_t k{0}; i < pts.size() && pts[i].m.t <= sp.t; ++i)
		{
			const auto &p{pts[i]};
			if (!p.alive || !same_level(p, sp) || k++ % limits::SIGHT_STRIDE)
				continue;
			const auto to{off(p.u.pos, sp.pos)};
			const double d{length(to)};
			if (d < 1)
			{
				first = i;
				break;
			}
			if (dot(to, p.u.orient.forward) / d < cone)
				continue;
			if (p.geo)
			{
				const vec3 dir{{to[0] / d, to[1] / d, to[2] / d}};
				if (geometry::free_distance(p.geo->mesh, p.m.s.segment, p.u.pos, dir, d + 1) < d - limits::SIGHT_SLACK)
					continue;
				by_geo = true;
			}
			else if (d > limits::SIGHT_NO_GEOMETRY)
				continue;
			first = i;
			break;
		}
		if (!first)
		{
			if (sp.own)
				++c.unseen_taken;
			continue;
		}
		++a.sightings;
		a.sightings_geo += by_geo;
		const auto &p0{pts[*first]};
		const double d0{length(off(p0.u.pos, sp.pos))};
		/* Came close to it afterwards (in the same life, until it was
		 * taken), and the way flown.
		 */
		double nearest{d0}, flown{};
		for (std::size_t k{*first + 1}; k < pts.size() && pts[k].m.t <= sp.t + 100 && pts[k].alive; ++k)
		{
			nearest = std::min(nearest, length(off(pts[k].u.pos, sp.pos)));
			if (pts[k].dt > 0)
				flown += length(off(pts[k - 1].u.pos, pts[k].u.pos));
		}
		/* Closer by GO_FOR_MIN_CLOSE at least: one seen right next to the
		 * ship and left there is not gone for.
		 */
		const bool went{sp.own || (nearest <= std::max(limits::GO_FOR_NEAR, limits::GO_FOR_SHARE * d0) && d0 - nearest >= limits::GO_FOR_MIN_CLOSE)};
		++c.seen;
		c.taken += sp.own;
		c.went += went;
		c.lost += went && !sp.own;
		if (p0.fight)
		{
			++c.fight_seen;
			c.fight_went += went;
		}
		else
		{
			++c.calm_seen;
			c.calm_went += went;
		}
		if (sp.own)
		{
			c.sight_distance.push_back(d0);
			c.take_s.push_back(static_cast<double>(sp.t - p0.m.t) / 1000.0);
			c.extra_path.push_back(std::max(flown - d0, 0.0));
			if (p0.u.speed >= 10 && d0 >= 5)
			{
				++c.off_course_n;
				const auto to{off(p0.u.pos, sp.pos)};
				if (dot(to, p0.u.vel) / (d0 * p0.u.speed) < std::cos(bot::radians(limits::DETOUR_DEG)))
					++c.off_course;
			}
		}
	}
}

/* What the player does when the enemy it fought leaves its sight. */
inline void scan_pursuit(const track &tr, accum &a, const ship_model &ship)
{
	const auto &pts{tr.pts};
	const double approach_speed{limits::APPROACH_SHARE * ship.max_speed};
	for (std::size_t i{1}; i < pts.size(); ++i)
	{
		const auto &before{pts[i - 1]};
		const auto &p{pts[i]};
		if (!before.fight || p.dt <= 0 || (p.los && p.enemy_key == before.enemy_key))
			continue;
		/* Flying away from it already: it did not lose it, it left. */
		if (before.own_approach < -approach_speed && before.u.enemy_off_nose_deg > limits::TURNED_AWAY_DEG)
			continue;
		const int enemy{before.enemy_key};
		double pursued{}, stalled{}, elapsed{};
		std::size_t k{i};
		for (; k < pts.size() && pts[k].dt > 0 && elapsed < limits::PURSUIT_MAX_S; ++k)
		{
			const auto &q{pts[k]};
			/* Another enemy took its place in the record, or it is in
			 * sight again.
			 */
			if (q.enemy_key != enemy || q.los)
				break;
			elapsed += q.dt;
			/* Drifting on without thrust is not following. */
			const bool coasting{q.has_ctl && std::abs(q.ctl[0]) < limits::CONTROL_USED && std::abs(q.ctl[1]) < limits::CONTROL_USED && std::abs(q.ctl[2]) < limits::CONTROL_USED};
			if (q.own_approach > approach_speed / 2 && !coasting)
			{
				pursued = elapsed;
				stalled = 0;
			}
			else if ((stalled += q.dt) > limits::PURSUIT_STALL_S)
				break;
		}
		++a.sight_losses;
		if (pursued >= limits::PURSUIT_MIN_S)
		{
			++a.pursuits;
			a.pursuit_s.push_back(pursued);
		}
		i = std::max(i, k);
	}
}

/* Section 8.8: the main traits per level and room class. */
inline void scan_rooms(const track &tr, accum &a)
{
	const auto &pts{tr.pts};
	const auto level_of{[&a](const geometry::level_geometry *const g) -> accum::level_acc & {
		for (auto &l : a.levels)
			if (l.geo == g)
				return l;
		return a.levels.emplace_back(accum::level_acc{g, {}});
	}};
	for (const auto &p : pts)
	{
		if (!p.alive || !p.room_known)
			continue;
		const double w{p.w};
		a.room_known_s += w;
		auto &l{level_of(p.geo)};
		for (auto *const r : {&l.by[static_cast<std::size_t>(p.room)], &l.by[geometry::ROOM_CLASSES]})
		{
			r->alive_s += w;
			r->speed_sum += p.u.speed * w;
			if (p.fight)
			{
				r->fight_s += w;
				r->side_room.push_back(std::min(p.free[2], p.free[3]));
				if (p.has_ctl)
				{
					r->fight_ctl_s += w;
					if (std::abs(p.ctl[1]) > limits::CONTROL_USED || std::abs(p.ctl[2]) > limits::CONTROL_USED)
						r->fight_strafe_s += w;
				}
			}
			if (p.los && !p.u.enemy_distance_scaled && p.line_known)
			{
				r->distance.push_back(p.u.enemy_distance);
				r->line.push_back(p.line);
				r->line_share.push_back(p.line > 0 ? std::min(p.u.enemy_distance / p.line, 1.0) : 1.0);
				if (r == &l.by[geometry::ROOM_CLASSES])
					a.line_share.push_back(r->line_share.back());
			}
		}
	}
	for (const auto &[first, mark] : a.turn_marks)
	{
		const auto &p{pts[first]};
		if (!p.room_known)
			continue;
		auto &l{level_of(p.geo)};
		for (auto *const r : {&l.by[static_cast<std::size_t>(p.room)], &l.by[geometry::ROOM_CLASSES]})
		{
			++r->turns;
			if (mark == '-')
				continue;
			++r->turns_ctl;
			r->reverse += mark == 'r';
			r->slide += mark == 's';
			r->forward += mark == 'f';
		}
	}
}

[[nodiscard]]
inline double ratio(const double a, const double b)
{
	return b > 0 ? a / b : 0;
}

/* A count as `unsigned`, without a cast that is useless where the two
 * types are the same.
 */
[[nodiscard]]
inline unsigned count(const std::size_t n)
{
	const unsigned r = n;
	return r;
}

/* The quiet moments of a phase of the weave; with fewer than
 * MIN_PHASE_N, with those of the neighbouring phases of the same kind
 * (idle or in a run), nearest first; with still too few, all.
 * `members`, if given, is set to the phases taken.
 */
[[nodiscard]]
inline accum::phase_cell quiet_near(const accum &a, const std::size_t phase, std::array<bool, DODGE_PHASES> *const members = nullptr)
{
	constexpr std::size_t per_kind{limits::PHASE_BINS + 1};
	const std::size_t kind{phase / per_kind}, bin{phase % per_kind};
	accum::phase_cell sum;
	std::array<bool, DODGE_PHASES> taken{};
	const auto add{[&sum, &taken, &a](const std::size_t ph) {
		const auto &c{a.quiet[ph]};
		taken[ph] = true;
		sum.n += c.n;
		sum.switched += c.switched;
		for (std::size_t k{}; k != DODGE_LATENCY_BINS; ++k)
			sum.first[k] += c.first[k];
	}};
	add(phase);
	for (std::size_t r{1}; sum.n < limits::MIN_PHASE_N && r != per_kind; ++r)
	{
		if (bin >= r)
			add(kind * per_kind + bin - r);
		if (bin + r < per_kind)
			add(kind * per_kind + bin + r);
	}
	if (sum.n < limits::MIN_PHASE_N)
	{
		sum = {};
		taken = {};
		for (std::size_t ph{}; ph != DODGE_PHASES; ++ph)
			add(ph);
	}
	if (members)
		*members = taken;
	return sum;
}

/* Section 8.4, dodging: the bursts against the quiet moments at the
 * same phase of the weave.  Every burst would have been followed by a
 * switch with the chance p0 its phase has in the quiet moments; the
 * switches beyond those chances, per chance left (1 - p0), are the
 * share of the bursts the player answered.
 */
inline void finish_dodge(const accum &a, player_stats &s)
{
	s.dodge_triggers = count(a.bursts.size());
	s.dodge_baseline_n = a.quiet_n;
	s.dodge_weave_rate = ratio(a.quiet_switched, a.quiet_n);
	s.dodge_reverse_n = a.reverse_n;
	s.dodge_reverse_share = ratio(a.reverse, a.reverse_n);
	s.dodge_reverse_baseline = ratio(a.quiet_reverse, a.quiet_reverse_n);
	s.dodge_burn_rate = ratio(a.burn, a.burn_n);
	s.dodge_burn_baseline = ratio(a.quiet_burn, a.quiet_burn_n);
	unsigned hit_switch{}, hit_none{};
	for (const auto &b : a.bursts)
	{
		(b.switched ? s.dodge_switch_n : s.dodge_none_n) += 1;
		if (b.hit)
			++(b.switched ? hit_switch : hit_none);
	}
	s.dodge_hit_after_switch = ratio(hit_switch, s.dodge_switch_n);
	s.dodge_hit_after_none = ratio(hit_none, s.dodge_none_n);
	if (a.bursts.empty() || a.quiet_n < limits::MIN_PHASE_N)
		return;
	double switched{}, expected{}, room{};
	/* How much the expected count moves per switch in each phase's
	 * quiet moments: a burst's p0 is the share of its pool of phases,
	 * and the pools of neighbouring phases overlap.
	 */
	std::array<double, DODGE_PHASES> weight{};
	std::array<double, DODGE_LATENCY_BINS> seen{}, due{};
	std::vector<double> p0s;
	for (const auto &b : a.bursts)
	{
		std::array<bool, DODGE_PHASES> pool;
		const auto c{quiet_near(a, b.phase, &pool)};
		const double p0{ratio(c.switched, c.n)};
		p0s.push_back(p0);
		for (std::size_t ph{}; ph != DODGE_PHASES; ++ph)
			if (pool[ph])
				weight[ph] += 1.0 / c.n;
		if (b.switched)
			++switched;
		expected += p0;
		room += 1 - p0;
		if (b.first >= 0)
			seen[std::min(static_cast<std::size_t>(b.first / DODGE_LATENCY_BIN_MS), DODGE_LATENCY_BINS - 1)] += 1;
		for (std::size_t k{}; k != DODGE_LATENCY_BINS; ++k)
			due[k] += ratio(c.first[k], c.n);
	}
	s.dodge_rate = switched / static_cast<double>(a.bursts.size());
	s.dodge_baseline = expected / static_cast<double>(a.bursts.size());
	s.dodge_room = room;
	if (room <= 0)
		return;
	const double excess{(switched - expected) / room};
	s.dodge_prob = std::clamp(excess, 0.0, 1.0);
	/* The standard error: the bursts' own chance (binomial, at the
	 * measured excess) and that of the quiet moments' shares.  The
	 * latter per phase of the quiet moments, with the weight of all the
	 * bursts whose pool takes it (pools that share a phase share its
	 * error); its chance of a switch is that of its own pool, kept off
	 * 0 and 1 (eight quiet moments without a switch do not make the
	 * chance certain).
	 */
	double var{};
	for (const double p0 : p0s)
	{
		const double q{std::clamp(p0 + s.dodge_prob * (1 - p0), 0.0, 1.0)};
		var += q * (1 - q);
	}
	for (std::size_t ph{}; ph != DODGE_PHASES; ++ph)
		if (weight[ph] > 0 && a.quiet[ph].n)
		{
			const auto c{quiet_near(a, ph)};
			const double p{(c.switched + 0.5) / (c.n + 1.0)};
			var += weight[ph] * weight[ph] * a.quiet[ph].n * p * (1 - p);
		}
	s.dodge_se = std::sqrt(var) / room;
	s.dodge_measurable = a.bursts.size() >= 8 && room >= limits::DODGE_MIN_ROOM;
	/* The reaction: the middle of the first switches that came earlier
	 * than the rhythm's, within the window.
	 */
	double mass{};
	std::array<double, DODGE_LATENCY_BINS> extra{};
	for (std::size_t k{}; k != DODGE_LATENCY_BINS; ++k)
	{
		const auto from{static_cast<std::int64_t>(k) * DODGE_LATENCY_BIN_MS};
		if (from + DODGE_LATENCY_BIN_MS <= limits::REACT_FROM_MS || from >= limits::REACT_TO_MS)
			continue;
		extra[k] = std::max(0.0, seen[k] - due[k]);
		mass += extra[k];
	}
	if (s.dodge_measurable && s.dodge_prob >= 0.1 && mass >= 2)
	{
		double cum{};
		for (std::size_t k{}; k != DODGE_LATENCY_BINS; ++k)
			if (extra[k] > 0 && (cum += extra[k]) >= mass / 2)
			{
				/* Within the bin, where the half is reached. */
				const double into{1 - (cum - mass / 2) / extra[k]};
				s.dodge_reaction_ms = (static_cast<double>(k) + into) * static_cast<double>(DODGE_LATENCY_BIN_MS);
				break;
			}
	}
}

}

/* The movement profile of one player from its tracks (one per session). */
[[nodiscard]]
inline player_stats analyse(const std::span<const track> tracks, const ship_model &ship = pyro_gx())
{
	using detail::ratio;
	detail::accum a;
	for (const auto &tr : tracks)
	{
		a.turn_marks.clear();
		const auto after_turn{detail::scan_turns(tr, a, ship)};
		detail::scan_samples(tr, a, ship, after_turn);
		detail::scan_strafe(tr, a);
		detail::scan_dodge(tr, a, ship);
		detail::scan_events(tr, a);
		detail::scan_pickup_sight(tr, a);
		detail::scan_pursuit(tr, a, ship);
		detail::scan_rooms(tr, a);
	}
	player_stats s;
	s.sessions = detail::count(tracks.size());
	s.recorded_s = a.recorded_s;
	s.alive_s = a.alive_s;
	s.exact_s = a.exact_s;
	s.shared_s = a.shared_s;
	s.estimated_s = a.estimated_s;
	s.estimator_n = a.est_n;
	s.estimator_rms = a.est_n ? std::sqrt(a.est_err2 / (3.0 * static_cast<double>(a.est_n))) : 0;
	s.kills = a.kills;
	s.deaths = a.deaths;
	s.suicides = a.suicides;

	s.speed = summarise(a.speed);
	s.speed_seconds = a.speed_seconds;
	s.fast_share = ratio(a.fast_s, a.alive_s);
	s.slow_share = ratio(a.slow_s, a.alive_s);

	s.forward_share = ratio(a.forward_s, a.ctl_s);
	s.reverse_share = ratio(a.reverse_s, a.ctl_s);
	s.strafe_share = ratio(a.side_s, a.ctl_s);
	s.vertical_share = ratio(a.vert_s, a.ctl_s);
	s.coast_share = ratio(a.coast_s, a.ctl_s);
	s.roll_share = ratio(a.roll_s, a.ctl_s);
	s.fight_s = a.fight_s;
	s.fight_strafe_share = ratio(a.fight_strafe_s, a.fight_ctl_s);
	s.fight_side_share = ratio(a.fight_side_s, a.fight_ctl_s);
	s.fight_vertical_share = ratio(a.fight_vert_s, a.fight_ctl_s);
	s.fight_reverse_share = ratio(a.fight_reverse_s, a.fight_ctl_s);

	s.strafe_run_ms = summarise(a.strafe_run_ms);
	s.strafe_reversals_per_min = ratio(a.strafe_reversals, a.fight_ctl_s / 60);
	s.strafe_vertical = std::clamp(ratio(a.strafe_vert_sum, a.strafe_side_sum), 0.0, 1.0);
	if (a.strafe_side_sum <= 0 && a.strafe_vert_sum > 0)
		s.strafe_vertical = 1;
	s.strafe_speed = summarise(a.lateral_speed).p75 / ship.max_speed;
	s.strafe_thrust = ratio(a.strafe_thrust_sum, static_cast<double>(a.lateral_speed.size()));

	s.large_turns = a.turns;
	s.reverse_turn_share = ratio(a.turns_reverse, a.turns_ctl);
	s.slide_turn_share = ratio(a.turns_slide, a.turns_ctl);
	s.forward_turn_share = ratio(a.turns_forward, a.turns_ctl);
	s.reverse_turn_speed = summarise(a.reverse_speed).mean;
	s.turn_180_ms = summarise(a.turn_180_ms);
	s.turn_rate = summarise(a.turn_rate).mean;
	s.turn_boost_share = ratio(a.boosts, a.boost_n);
	s.turn_boost_burn_share = ratio(a.boost_burns, a.boosts);

	s.ab_known_s = a.ab_known_s;
	s.ab_estimated_s = a.ab_estimated_s;
	s.ab_share = ratio(a.ab_s, a.ab_known_s);
	s.ab_situation_s = a.ab_situation_s;
	for (std::size_t i{}; i != 4; ++i)
		s.ab_situation_rate[i] = ratio(a.ab_situation_on_s[i], a.ab_situation_s[i]);
	s.ab_chase_distance = summarise(a.ab_chase_distance);

	s.los_s = a.los_s;
	s.los_distance = summarise(a.los_distance);
	s.fire_distance = summarise(a.fire_distance);
	s.distance_seconds = a.distance_seconds;

	double fight{}, approach{}, back{}, retreat{};
	for (std::size_t b{}; b != SHIELD_BUCKETS; ++b)
	{
		auto &o{s.by_shields[b]};
		o.seconds = a.bucket_s[b];
		o.approach_mean = ratio(a.bucket_approach_sum[b], a.bucket_s[b]);
		o.approach = ratio(a.bucket_approach_s[b], a.bucket_s[b]);
		o.back_off = ratio(a.bucket_back_s[b], a.bucket_s[b]);
		o.retreat = ratio(a.bucket_retreat_s[b], a.bucket_s[b]);
		fight += a.bucket_s[b];
		approach += a.bucket_approach_s[b];
		back += a.bucket_back_s[b];
		retreat += a.bucket_retreat_s[b];
	}
	s.approach_share = ratio(approach, fight);
	s.back_off_share = ratio(back, fight);
	s.retreat_share = ratio(retreat, fight);
	/* The shields that split the fight time into "flies away" below and
	 * "does not" above most clearly; at least 3 s of fight on each side.
	 */
	for (unsigned level{10}; level <= 120; level += 5)
	{
		double below{}, below_retreat{}, above{}, above_retreat{};
		for (unsigned v{}; v != 256; ++v)
		{
			(v < level ? below : above) += a.shield_s[v];
			(v < level ? below_retreat : above_retreat) += a.shield_retreat_s[v];
		}
		if (below < 3 || above < 3)
			continue;
		const double contrast{below_retreat / below - above_retreat / above};
		if (contrast > s.retreat_contrast + 1e-9)
		{
			s.retreat_contrast = contrast;
			s.retreat_shields = level;
		}
	}
	s.close_speed = summarise(a.close_speed).mean;

	detail::finish_dodge(a, s);
	s.enemy_shots = a.enemy_shots;
	s.hits_taken_from_players = a.hits_taken_from_players;

	s.primary_shots = a.primary_shots;
	s.secondary_shots = a.secondary_shots;
	s.primary_by_band = a.primary_by_band;
	s.secondary_count = a.secondary_count;
	s.secondary_distance = summarise(a.secondary_distance);
	s.volleys = detail::count(a.volley_sizes.size());
	const auto volley{summarise(a.volley_sizes)};
	s.volley_size = volley.mean;
	for (const double v : a.volley_sizes)
		s.volley_max = std::max(s.volley_max, static_cast<unsigned>(v));
	s.volley_gap_s = summarise(a.volley_gap_s);

	s.pickups = a.pickups;
	s.pickups_per_min = ratio(a.pickups, a.alive_s / 60);
	s.pickup_detour_n = a.pickup_detour_n;
	s.pickup_detour_share = ratio(a.pickup_detours, a.pickup_detour_n);
	s.pickup_in_fight_share = ratio(a.pickup_in_fight, a.pickups);

	for (std::size_t c{}; c != PICKUP_CLASSES; ++c)
	{
		const auto &f{a.pickup_sight[c]};
		auto &o{s.pickup_sight[c]};
		o.seen = f.seen;
		o.taken = f.taken;
		o.went = f.went;
		o.lost = f.lost;
		o.fight_seen = f.fight_seen;
		o.fight_went = f.fight_went;
		o.calm_seen = f.calm_seen;
		o.calm_went = f.calm_went;
		o.unseen_taken = f.unseen_taken;
		o.taken_share = ratio(f.taken, f.seen);
		o.went_share = ratio(f.went, f.seen);
		o.fight_went_share = ratio(f.fight_went, f.fight_seen);
		o.calm_went_share = ratio(f.calm_went, f.calm_seen);
		o.off_course_share = ratio(f.off_course, f.off_course_n);
		o.sight_distance = summarise(f.sight_distance);
		o.take_s = summarise(f.take_s);
		o.extra_path = summarise(f.extra_path);
	}
	s.pickup_sight_geometry_share = ratio(a.sightings_geo, a.sightings);

	s.sight_losses = a.sight_losses;
	s.pursue_share = ratio(a.pursuits, a.sight_losses);
	s.pursuit_s = summarise(a.pursuit_s);

	s.hits_dealt = a.hits_dealt;
	s.splash_dealt = a.splash_dealt;
	s.hits_taken = a.hits_taken;
	s.damage_dealt = a.damage_dealt;
	s.damage_taken = a.damage_taken;
	s.hits_per_shot = ratio(a.hits_dealt, a.primary_shots);

	s.room_known_s = a.room_known_s;
	s.line_share = summarise(a.line_share);
	for (std::size_t b{}; b != LINE_BANDS; ++b)
		s.fire_distance_by_line[b] = summarise(a.fire_distance_by_line[b]);
	for (const auto &[g, n] : a.fire_per_level)
		if (n >= 20)
			++s.levels_with_fire;
	for (const auto &l : a.levels)
	{
		auto &o{s.levels.emplace_back()};
		o.label = l.geo->name;
		o.source = l.geo->source;
		o.character = l.geo->character();
		for (std::size_t c{}; c != geometry::ROOM_CLASSES + 1; ++c)
		{
			const auto &r{l.by[c]};
			auto &t{c == geometry::ROOM_CLASSES ? o.all : o.by_class[c]};
			t.alive_s = r.alive_s;
			t.speed = ratio(r.speed_sum, r.alive_s);
			t.fight_s = r.fight_s;
			t.strafe_share = ratio(r.fight_strafe_s, r.fight_ctl_s);
			t.distance = summarise(r.distance);
			t.line = summarise(r.line);
			t.line_share = summarise(r.line_share);
			t.side_room = summarise(r.side_room);
			t.turns = r.turns;
			t.reverse_turn_share = ratio(r.reverse, r.turns_ctl);
			t.slide_turn_share = ratio(r.slide, r.turns_ctl);
			t.forward_turn_share = ratio(r.forward, r.turns_ctl);
		}
	}
	return s;
}

/*
 * From the movement profile to the bot's parameters.
 */

namespace detail {

/* `some` evidence makes a value worth using, `plenty` makes it sure.  A
 * value read from the controls is no better than medium when most of
 * them were estimated.
 */
[[nodiscard]]
inline bot::style_confidence confidence_of(const player_stats &s, const double evidence, const double some, const double plenty, const bool from_controls = false)
{
	if (evidence < some)
		return bot::style_confidence::low;
	if (evidence < plenty || (from_controls && s.estimated_share() > 0.5))
		return bot::style_confidence::medium;
	return bot::style_confidence::high;
}

/* The share of the afterburner time that is only estimated. */
[[nodiscard]]
inline bool burner_estimated(const player_stats &s)
{
	return s.ab_known_s > 0 && s.ab_estimated_s / s.ab_known_s > 0.5;
}

}

/* The proposed bot style of a player.  `base_skill` is the skill the
 * values that depend on one are scaled by (the chase memory, the missile
 * interval); the profile can be flown at any skill.  A value the
 * recordings say nothing about is left out (the bot takes the base
 * style's), one they say little about has a low confidence.
 */
[[nodiscard]]
inline bot::style_profile propose_profile(const player_stats &s, const bot::bot_skill base_skill = bot::BOT_DEFAULT_SKILL)
{
	using bot::style_confidence;
	using detail::confidence_of;
	bot::style_profile p;
	p.callsign = s.callsign;
	p.name = s.callsign + " style";
	p.base_skill = base_skill;
	{
		std::array<char, 160> buf;
		std::snprintf(buf.data(), buf.size(), "%u game%s, %.1f min alive, %.1f min in fights, controls %.0f%% exact", s.sessions, s.sessions == 1 ? "" : "s", s.alive_s / 60, s.fight_s / 60, 100 * (1 - s.estimated_share()));
		p.source = buf.data();
	}
	const auto &skill{bot::skill_of(base_skill)};
	const auto fight_confidence{confidence_of(s, s.fight_s, 60, 300)};

	/* style.retreat_shields */
	if (s.retreat_contrast >= 0.15)
		p.set("style.retreat_shields", s.retreat_shields, fight_confidence);
	else if (s.fight_s >= 60 && s.retreat_share < 0.05)
		/* Fights on at any shields. */
		p.set("style.retreat_shields", 10, std::min(fight_confidence, style_confidence::medium));

	/* style.engage_weight: of the time it moves toward or away from the
	 * enemy, the share toward it; with what it does when it loses sight.
	 */
	if (const double moving{s.approach_share + s.back_off_share + s.retreat_share}; s.fight_s >= 20 && moving >= 0.1)
	{
		double aggression{s.approach_share / moving};
		if (s.sight_losses >= 5)
			aggression = 0.6 * aggression + 0.4 * s.pursue_share;
		p.set("style.engage_weight", 0.6 + aggression, fight_confidence);
	}

	/* style.collect_weight: pickups that cost a detour, or a fight. */
	if (s.pickup_detour_n >= 5)
		p.set("style.collect_weight", 0.7 + s.pickup_detour_share + 0.6 * s.pickup_in_fight_share, confidence_of(s, s.pickup_detour_n, 10, 40));

	/* style.range_scale and the fight band: where it fires from.  With
	 * the levels known (section 8.8) and all the shots on one of them,
	 * no more than medium: on one map the distance is as much the map's
	 * as the pilot's.
	 */
	{
		constexpr double band_middle{65};	/* of BOT_RANGE_LO 35 and BOT_RANGE_HI 95 */
		const bool shots{s.fire_distance.n >= 20};
		const auto &d{shots ? s.fire_distance : s.los_distance};
		if (shots || s.los_s >= 30)
		{
			auto c{shots ? confidence_of(s, static_cast<double>(d.n), 50, 300) : std::min(fight_confidence, style_confidence::medium)};
			if (s.levels_with_fire == 1)
				c = std::min(c, style_confidence::medium);
			p.set("style.range_scale", d.p50 / band_middle, c);
			p.set("tune.range_lo", d.p25, c);
			p.set("tune.range_hi", d.p75, c);
		}
	}

	/* style.chase_memory, tune.pursuit_seconds */
	if (s.sight_losses >= 5)
	{
		const auto c{confidence_of(s, s.sight_losses, 10, 40)};
		const double seconds{s.pursue_share >= 0.2 ? s.pursuit_s.p50 : 0};
		p.set("style.chase_memory", seconds / bot::pursuit_seconds(base_skill, bot::bot_style::balanced), c);
		p.set("tune.pursuit_seconds", seconds, c);
	}

	/* style.close_scale */
	if (s.fight_s >= 20 && s.close_speed > 0)
		p.set("style.close_scale", s.close_speed / bot::COMBAT_CLOSE_SPEED, fight_confidence);

	/* style.burn_chase_distance and the other afterburner habits. */
	if (s.ab_known_s >= 30)
	{
		const auto cap{detail::burner_estimated(s) ? style_confidence::medium : style_confidence::high};
		if (const double t{s.ab_situation_s[detail::chasing]}; t >= 5)
		{
			const auto c{std::min(cap, confidence_of(s, t, 15, 90))};
			if (s.ab_situation_rate[detail::chasing] >= 0.1 && s.ab_chase_distance.n >= 10)
				/* Burns when the target is further than this: most of
				 * its burning chases start beyond it.
				 */
				p.set("style.burn_chase_distance", s.ab_chase_distance.p10, c);
			else
				p.set("style.burn_chase_distance", 1000, c);
		}
		if (const double t{s.ab_situation_s[detail::fleeing]}; t >= 3)
			p.set("tune.burn_retreat", s.ab_situation_rate[detail::fleeing], std::min(cap, confidence_of(s, t, 8, 40)));
		if (const double t{s.ab_situation_s[detail::crossing]}; t >= 10)
			p.set("tune.burn_roam", s.ab_situation_rate[detail::crossing], std::min(cap, confidence_of(s, t, 30, 180)));
	}

	/* The strafe. */
	if (const double t{s.exact_s + s.estimated_s > 0 ? s.fight_s : 0}; t >= 20)
	{
		const auto c{confidence_of(s, t, 45, 240, true)};
		const bool strafes{s.fight_strafe_share >= 0.2};
		p.set("skill.strafe", strafes, c);
		if (strafes)
		{
			const auto rc{std::min(c, confidence_of(s, static_cast<double>(s.strafe_run_ms.n), 10, 60, true))};
			if (s.strafe_run_ms.n >= 5)
			{
				p.set("skill.strafe_min_ms", s.strafe_run_ms.p25, rc);
				p.set("skill.strafe_max_ms", std::max(s.strafe_run_ms.p75, s.strafe_run_ms.p25 + 100), rc);
			}
			p.set("skill.strafe_vertical", s.strafe_vertical, c);
			/* The thrust of the bot's strafe keys (section 9.12 of
			 * Documentation/multiplayer-bots.md): the pilot's thrust
			 * across, whether or not the run is long enough to reach
			 * the top speed.
			 */
			p.set("skill.strafe_speed", std::max(s.strafe_thrust, 0.3), c);
		}
	}

	/* skill.dodge_prob: the share of the bursts answered beyond the
	 * weave's rhythm.  How sure: the standard error and the bursts; at
	 * most medium on estimated controls (the switches are read from
	 * them).
	 */
	if (s.dodge_measurable)
	{
		auto c{s.dodge_se <= 0.07 && s.dodge_triggers >= 60 ? style_confidence::high : s.dodge_se <= 0.15 && s.dodge_triggers >= 15 ? style_confidence::medium : style_confidence::low};
		if (s.estimated_share() > 0.5)
			c = std::min(c, style_confidence::medium);
		p.set("skill.dodge_prob", s.dodge_prob, c);
	}

	/* The large turns. */
	if (s.large_turns >= 3)
	{
		const auto c{confidence_of(s, s.large_turns, 8, 30, true)};
		p.set("tune.reverse_turn", s.reverse_turn_share, c);
		if (s.reverse_turn_share > 0)
			p.set("tune.reverse_turn_speed", s.reverse_turn_speed, c);
		p.set("tune.turn_boost", s.turn_boost_share, c);
		if (s.turn_boost_share > 0 && s.ab_known_s > 0)
			p.set("tune.turn_boost_burn", s.turn_boost_burn_share, std::min(c, detail::burner_estimated(s) ? style_confidence::medium : style_confidence::high));
	}

	/* Missiles. */
	if (s.volleys >= 4)
	{
		const auto c{confidence_of(s, s.volleys, 8, 30)};
		/* Only volleys of one fight tell the interval: without such
		 * pairs the median would be 0, the fastest firing there is.
		 */
		if (s.volley_gap_s.n >= 3)
			p.set("tune.missile_interval_scale", s.volley_gap_s.p50 / bot::missile_interval(std::max(skill.weapon_smarts, 1u)), std::min(c, confidence_of(s, static_cast<double>(s.volley_gap_s.n), 8, 30)));
		p.set("tune.volley_size", s.volley_size, c);
	}

	if (s.pickup_detour_n >= 5)
		p.set("tune.grab_detour", s.pickup_detour_share, confidence_of(s, s.pickup_detour_n, 10, 40));

	/* Section 8.9: tune.power_pickup, the share of the power pickups in
	 * sight the player went for; no more than medium when most sightings
	 * were judged without the level's geometry.
	 */
	if (const auto &pw{s.pickup_sight[0]}; pw.seen >= 5)
	{
		auto c{confidence_of(s, pw.seen, 8, 25)};
		if (s.pickup_sight_geometry_share < 0.5)
			c = std::min(c, style_confidence::medium);
		p.set("tune.power_pickup", pw.went_share, c);
	}

	/* The built-in style nearest to what was measured: the bot takes
	 * from it what the profile leaves out.
	 */
	{
		double best{1e9};
		for (std::size_t i{}; i != bot::BOT_STYLE_COUNT; ++i)
		{
			const auto &st{bot::style_table[i]};
			double d{};
			const auto term{[&](const std::string_view key, const double have, const double scale) {
				if (const auto e{p.find(key)}; e && e->confidence != style_confidence::low)
					d += ((e->value - have) / scale) * ((e->value - have) / scale);
			}};
			term("style.retreat_shields", st.retreat_shields, 35);
			term("style.engage_weight", st.engage_weight, 0.9);
			term("style.collect_weight", st.collect_weight, 1.2);
			term("style.range_scale", st.range_scale, 0.5);
			if (d < best - 1e-9)
			{
				best = d;
				p.base_style = static_cast<bot::bot_style>(i);
			}
		}
	}

	/* The file's order: the style, the skill's movement, the constants. */
	{
		const auto section{[](const bot::style_profile_entry &e) {
			return e.key.starts_with("style.") ? 0 : e.key.starts_with("skill.") ? 1 : 2;
		}};
		std::stable_sort(p.entries.begin(), p.entries.end(), [&section](const bot::style_profile_entry &a, const bot::style_profile_entry &b) { return section(a) < section(b); });
	}

	/* For people. */
	const auto info{[&p](const std::string_view key, const double v) {
		p.set(key, v, style_confidence::high);
	}};
	info("measured.alive_minutes", s.alive_s / 60);
	info("measured.fight_minutes", s.fight_s / 60);
	info("measured.estimated_controls_share", s.estimated_share());
	if (s.shared_s > 0)
		info("measured.shared_controls_share", s.shared_s / (s.exact_s + s.estimated_s));
	info("measured.speed_mean", s.speed.mean);
	if (s.los_distance.n)
		info("measured.enemy_distance_median", s.los_distance.p50);
	if (s.turn_180_ms.n)
		info("measured.turn_180_ms", s.turn_180_ms.p50);
	if (s.line_share.n >= 100)
		info("measured.line_share_median", s.line_share.p50);
	/* The reaction only of a dodge that clearly is one (two standard
	 * errors above nothing).
	 */
	if (s.dodge_measurable && s.dodge_prob >= 0.2 && s.dodge_prob - 2 * s.dodge_se > 0 && s.dodge_reaction_ms > 0)
		info("measured.dodge_reaction_ms", s.dodge_reaction_ms);
	if (s.dodge_measurable)
		info("measured.dodge_prob_se", s.dodge_se);
	if (s.ab_known_s > 0)
		info("measured.afterburner_share", s.ab_share);
	if (s.primary_shots)
		info("measured.hits_per_shot", s.hits_per_shot);
	if (const auto &pw{s.pickup_sight[0]}; pw.seen)
	{
		info("measured.power_seen", pw.seen);
		info("measured.power_taken_share", pw.taken_share);
		if (pw.fight_seen)
			info("measured.power_fight_went_share", pw.fight_went_share);
		if (pw.sight_distance.n)
			info("measured.power_sight_distance_median", pw.sight_distance.p50);
	}
	if (const auto &ot{s.pickup_sight[1]}; ot.seen)
		info("measured.other_pickup_went_share", ot.went_share);
	return p;
}

/*
 * The report for people.
 */

namespace detail {

#ifdef __GNUC__
__attribute__((format(printf, 2, 3)))
#endif
inline void appendf(std::string &out, const char *const fmt, ...)
{
	std::array<char, 512> buf;
	va_list ap;
	va_start(ap, fmt);
	std::vsnprintf(buf.data(), buf.size(), fmt, ap);
	va_end(ap);
	out += buf.data();
}

[[nodiscard]]
inline double pct(const double share)
{
	return 100 * share;
}

}

/* The player's key traits in plain words, the most telling first. */
[[nodiscard]]
inline std::vector<std::string> describe_traits(const player_stats &s, const ship_model &ship = pyro_gx())
{
	using detail::appendf;
	using detail::pct;
	std::vector<std::string> out;
	const auto line{[&out]() -> std::string & {
		return out.emplace_back();
	}};
	if (s.alive_s < limits::MIN_ALIVE_S)
	{
		appendf(line(), "Too little data (%.0f s alive) to describe the flying.", s.alive_s);
		return out;
	}
	if (s.fight_s >= 20 && s.exact_s + s.estimated_s > 0)
	{
		const char *const kind{s.fight_strafe_share >= 0.6 ? "Heavy strafer" : s.fight_strafe_share >= 0.2 ? "Strafes" : "Rarely strafes"};
		auto &l{line()};
		appendf(l, "%s: sideways or vertical thrust in %.0f%% of the fight time", kind, pct(s.fight_strafe_share));
		if (s.strafe_run_ms.n >= 5)
			appendf(l, ", a run in one direction lasts %.1f s (typically %.1f to %.1f s), %.0f reversals per minute", s.strafe_run_ms.p50 / 1000, s.strafe_run_ms.p25 / 1000, s.strafe_run_ms.p75 / 1000, s.strafe_reversals_per_min);
		if (s.fight_strafe_share >= 0.2)
			appendf(l, "; %s (vertical share %.2f), with %.0f%% of full thrust", s.strafe_vertical >= 0.5 ? "uses up and down as much as left and right" : s.strafe_vertical >= 0.2 ? "mixes in some up and down" : "almost flat", s.strafe_vertical, pct(s.strafe_thrust));
		l += '.';
	}
	if (s.large_turns >= 3)
	{
		const char *const kind{s.reverse_turn_share >= 0.5 ? "Turns round flying backwards" : s.slide_turn_share >= 0.5 ? "Slides through its turns" : s.forward_turn_share >= 0.5 ? "Pushes forward through its turns" : "Turns round in several ways"};
		auto &l{line()};
		appendf(l, "%s: of %u large turns %.0f%% with reverse thrust, %.0f%% sliding, %.0f%% pushing forward; half a turn takes %.2f s", kind, s.large_turns, pct(s.reverse_turn_share), pct(s.slide_turn_share), pct(s.forward_turn_share), s.turn_180_ms.p50 / 1000);
		if (s.reverse_turn_share > 0)
			appendf(l, "; backwards at up to %.0f%% of the top speed", pct(s.reverse_turn_speed));
		if (s.turn_boost_share >= 0.3)
			appendf(l, "; then pushes forward (%.0f%% of the turns%s)", pct(s.turn_boost_share), s.turn_boost_burn_share >= 0.5 ? ", mostly with the afterburner" : "");
		l += '.';
	}
	if (s.los_distance.n >= 100)
	{
		const auto &d{s.fire_distance.n >= 20 ? s.fire_distance : s.los_distance};
		const char *const kind{d.p50 >= 150 ? "Long-range fighter" : d.p50 >= 95 ? "Keeps its distance" : d.p50 < 45 ? "In-your-face fighter" : "Fights at medium range"};
		appendf(line(), "%s: %s at a median of %.0f units (half of the time between %.0f and %.0f); the bots' band is 35 to 95.", kind, s.fire_distance.n >= 20 ? "fires" : "has the enemy in sight", d.p50, d.p25, d.p75);
	}
	/* Section 8.8: the rooms, over all levels with a known geometry. */
	if (s.room_known_s >= 30)
	{
		std::array<double, geometry::ROOM_CLASSES> t{}, speed{}, strafe{}, strafe_t{}, dist{}, dist_t{};
		double share{}, share_t{};
		for (const auto &l : s.levels)
		{
			for (std::size_t c{}; c != geometry::ROOM_CLASSES; ++c)
			{
				const auto &r{l.by_class[c]};
				t[c] += r.alive_s;
				speed[c] += r.speed * r.alive_s;
				strafe[c] += r.strafe_share * r.fight_s;
				strafe_t[c] += r.fight_s;
				const auto n{static_cast<double>(r.distance.n)};
				dist[c] += r.distance.p50 * n;
				dist_t[c] += n;
			}
			const auto n{static_cast<double>(l.all.line_share.n)};
			share += l.all.line_share.p50 * n;
			share_t += n;
		}
		auto &l{line()};
		l += "Rooms:";
		const char *sep{" "};
		for (std::size_t c{}; c != geometry::ROOM_CLASSES; ++c)
		{
			if (t[c] < limits::ROOM_MIN_S)
				continue;
			appendf(l, "%s%s %.0f%% of the time (speed %.0f", sep, geometry::room_class_names[c], pct(t[c] / s.room_known_s), speed[c] / t[c]);
			if (strafe_t[c] >= limits::ROOM_MIN_S)
				appendf(l, ", strafes %.0f%% of the fights", pct(strafe[c] / strafe_t[c]));
			if (dist_t[c] >= 30)
				appendf(l, ", enemy at %.0f", dist[c] / dist_t[c]);
			l += ')';
			sep = "; ";
		}
		if (share_t >= 30)
			appendf(l, "; the enemy is at %.0f%% of the line of fire the level offers (median)", pct(share / share_t));
		l += '.';
	}
	if (s.fight_s >= 20)
	{
		auto &l{line()};
		const char *const kind{s.approach_share + s.back_off_share + s.retreat_share < 0.1 ? "Holds its distance" : s.approach_share > 1.5 * (s.back_off_share + s.retreat_share) ? "Presses the attack" : s.back_off_share + s.retreat_share > 1.5 * s.approach_share ? "Gives ground" : "Closes in and backs off in turn"};
		appendf(l, "%s: closing in %.0f%%, backing off while facing %.0f%%, flying away %.0f%% of the fight time", kind, pct(s.approach_share), pct(s.back_off_share), pct(s.retreat_share));
		if (s.retreat_contrast >= 0.15)
			appendf(l, "; breaks off below about %.0f shields (flies away %.0f percentage points more often below than above)", s.retreat_shields, pct(s.retreat_contrast));
		else if (s.retreat_share < 0.05)
			l += "; does not break off at low shields";
		l += '.';
	}
	if (s.ab_known_s >= 30)
	{
		auto &l{line()};
		const char *const kind{s.ab_share >= 0.25 ? "Heavy afterburner user" : s.ab_share >= 0.05 ? "Uses the afterburner" : "Hardly uses the afterburner"};
		appendf(l, "%s: %.0f%% of the time; chasing %.0f%%, fleeing %.0f%%, with no enemy in sight %.0f%%, otherwise %.0f%%", kind, pct(s.ab_share), pct(s.ab_situation_rate[detail::chasing]), pct(s.ab_situation_rate[detail::fleeing]), pct(s.ab_situation_rate[detail::crossing]), pct(s.ab_situation_rate[detail::fighting]));
		if (s.ab_situation_rate[detail::chasing] >= 0.1 && s.ab_chase_distance.n >= 10)
			appendf(l, "; chases with it from about %.0f units", s.ab_chase_distance.p10);
		if (detail::burner_estimated(s))
			l += " (estimated from the motion)";
		l += '.';
	}
	if (s.dodge_triggers >= 8 && !s.dodge_measurable)
	{
		if (s.dodge_baseline_n < limits::MIN_PHASE_N)
			appendf(line(), "Dodging cannot be measured: %u bursts aimed at it, but only %.0f quiet moments with an enemy facing it to compare with.", s.dodge_triggers, static_cast<double>(s.dodge_baseline_n));
		else
			appendf(line(), "Dodging cannot be told from its weave: its strafe switches in the reaction window after %.0f%% of the quiet moments, which leaves no room to see an answer to %u bursts.", pct(s.dodge_weave_rate), s.dodge_triggers);
	}
	else if (s.dodge_measurable)
	{
		auto &l{line()};
		/* Clearly above nothing: two standard errors. */
		const bool shown{s.dodge_prob - 2 * s.dodge_se > 0};
		const char *const kind{!shown ? "No clear reaction to incoming fire" : s.dodge_prob >= 0.5 ? "Dodges incoming fire" : s.dodge_prob >= 0.2 ? "Sometimes dodges incoming fire" : "Rarely dodges incoming fire"};
		appendf(l, "%s: answers %.0f%% (+-%.0f) of %u bursts aimed at it with a switch of its strafe the weave would not have made (a switch after %.0f%% of them, where its rhythm gives %.0f%%)", kind, pct(s.dodge_prob), pct(s.dodge_se), s.dodge_triggers, pct(s.dodge_rate), pct(s.dodge_baseline));
		if (shown && s.dodge_reaction_ms > 0)
			appendf(l, "; after about %.0f ms", s.dodge_reaction_ms);
		if (s.dodge_switch_n >= 5 && s.dodge_none_n >= 5)
			appendf(l, "; hit after %.0f%% of the bursts with a switch, %.0f%% of the others", pct(s.dodge_hit_after_switch), pct(s.dodge_hit_after_none));
		l += '.';
	}
	if (s.sight_losses >= 5)
	{
		auto &l{line()};
		const char *const kind{s.pursue_share >= 0.6 ? "Hunts a lost enemy" : s.pursue_share >= 0.2 ? "Sometimes follows a lost enemy" : "Lets a lost enemy go"};
		appendf(l, "%s: follows in %.0f%% of %u losses of sight", kind, pct(s.pursue_share), s.sight_losses);
		if (s.pursuit_s.n)
			appendf(l, ", for %.1f s (up to %.1f s)", s.pursuit_s.p50, s.pursuit_s.p90);
		l += '.';
	}
	if (s.secondary_shots >= 4 && s.volleys)
	{
		auto &l{line()};
		appendf(l, "Missiles: %u in %u volleys (%.1f per volley, at most %u)", s.secondary_shots, s.volleys, s.volley_size, s.volley_max);
		if (s.volley_gap_s.n)
			appendf(l, ", %.1f s between volleys", s.volley_gap_s.p50);
		if (s.secondary_distance.n)
			appendf(l, ", from %.0f units", s.secondary_distance.p50);
		l += '.';
	}
	if (s.pickups >= 5)
		appendf(line(), "Pickups: %.1f per minute, %.0f%% by leaving the course, %.0f%% in a fight.", s.pickups_per_min, pct(s.pickup_detour_share), pct(s.pickup_in_fight_share));
	if (const auto &pw{s.pickup_sight[0]}, &ot{s.pickup_sight[1]}; pw.seen >= 3)
	{
		auto &l{line()};
		const char *const kind{pw.went_share >= 0.7 ? "Goes for every power pickup it sees" : pw.went_share >= 0.4 ? "Often goes for the power pickups it sees" : "Lets most power pickups go"};
		appendf(l, "%s: of %u smart, mega, earthshaker missiles and omega cannons in sight it went for %.0f%% and took %.0f%%", kind, pw.seen, pct(pw.went_share), pct(pw.taken_share));
		if (pw.fight_seen >= 3)
			appendf(l, "; %.0f%% of the %u seen in a fight%s", pct(pw.fight_went_share), pw.fight_seen, pw.fight_went_share >= 0.5 ? " (breaks off for them)" : "");
		if (pw.sight_distance.n >= 3)
			appendf(l, "; taken from %.0f units away (median, up to %.0f), %.1f s after the first sight", pw.sight_distance.p50, pw.sight_distance.p90, pw.take_s.p50);
		if (ot.seen >= 5)
			appendf(l, "; other pickups in sight: went for %.0f%%", pct(ot.went_share));
		l += '.';
	}
	appendf(line(), "Speed: mean %.0f, median %.0f units/s (top speed %.0f); above 85%% of it %.0f%% of the time, nearly still %.0f%%.", s.speed.mean, s.speed.p50, ship.max_speed, pct(s.fast_share), pct(s.slow_share));
	return out;
}

/* The whole report of one player. */
[[nodiscard]]
inline std::string write_report(const player_stats &s, const bot::style_profile &profile, const ship_model &ship = pyro_gx())
{
	using detail::appendf;
	using detail::pct;
	std::string o;
	appendf(o, "== %s%s ==\n", s.callsign.c_str(), s.bot ? " (bot)" : "");
	appendf(o, "data: %u game%s, %.1f min recorded, %.1f min alive, %.1f min in fights; kills %u, deaths %u (%u suicides)\n", s.sessions, s.sessions == 1 ? "" : "s", s.recorded_s / 60, s.alive_s / 60, s.fight_s / 60, s.kills, s.deaths, s.suicides);
	appendf(o, "controls: exact %.1f min", s.exact_s / 60);
	if (s.shared_s > 0)
		appendf(o, " (%.1f min of it shared by the player's machine)", s.shared_s / 60);
	appendf(o, ", estimated from the motion %.1f min (%.0f%%)", s.estimated_s / 60, pct(s.estimated_share()));
	if (s.estimator_n)
		appendf(o, "; the estimate is off by %.2f of full thrust (rms) where both exist", s.estimator_rms);
	o += '\n';
	for (const auto &l : s.levels)
		appendf(o, "level: %s (%s), %.1f min alive here: %s\n", l.label.c_str(), l.source.c_str(), l.all.alive_s / 60, l.character.c_str());
	if (s.levels.empty() && s.room_known_s <= 0)
		o += "level: geometry not known (movrec-analyse --missions DIR)\n";
	o += "\nTraits\n";
	for (const auto &t : describe_traits(s, ship))
		o += "  - " + t + "\n";
	if (s.alive_s < limits::MIN_ALIVE_S)
		return o;

	o += "\nNumbers\n";
	appendf(o, "  speed (units/s): mean %.1f, p10 %.1f, median %.1f, p90 %.1f\n    seconds at", s.speed.mean, s.speed.p10, s.speed.p50, s.speed.p90);
	for (std::size_t i{}; i != SPEED_BINS; ++i)
		appendf(o, " %.0f%s: %.0f", static_cast<double>(i) * SPEED_BIN_WIDTH, i + 1 == SPEED_BINS ? "+" : "", s.speed_seconds[i]);
	o += '\n';
	appendf(o, "  thrust (share of the time with controls): forward %.0f%%, reverse %.0f%%, sideways %.0f%%, vertical %.0f%%, none %.0f%%; roll %.0f%%\n", pct(s.forward_share), pct(s.reverse_share), pct(s.strafe_share), pct(s.vertical_share), pct(s.coast_share), pct(s.roll_share));
	appendf(o, "  in a fight: sideways %.0f%%, vertical %.0f%%, either %.0f%%, reverse %.0f%%\n", pct(s.fight_side_share), pct(s.fight_vertical_share), pct(s.fight_strafe_share), pct(s.fight_reverse_share));
	appendf(o, "  strafe runs: %.0f, median %.0f ms (p25 %.0f, p75 %.0f), %.1f reversals/min, vertical share %.2f, thrust across %.0f%% of full, speed across %.0f%% of top (upper quartile)\n", static_cast<double>(s.strafe_run_ms.n), s.strafe_run_ms.p50, s.strafe_run_ms.p25, s.strafe_run_ms.p75, s.strafe_reversals_per_min, s.strafe_vertical, pct(s.strafe_thrust), pct(s.strafe_speed));
	appendf(o, "  large turns: %u; reverse %.0f%%, slide %.0f%%, forward %.0f%%; 180 degrees in %.0f ms (p25 %.0f, p75 %.0f) at %.0f%% of the top rate; push after %.0f%% (with afterburner %.0f%%)\n", s.large_turns, pct(s.reverse_turn_share), pct(s.slide_turn_share), pct(s.forward_turn_share), s.turn_180_ms.p50, s.turn_180_ms.p25, s.turn_180_ms.p75, pct(s.turn_rate), pct(s.turn_boost_share), pct(s.turn_boost_burn_share));
	if (s.ab_known_s > 0)
		appendf(o, "  afterburner: known for %.1f min%s, on %.1f%%; chasing %.0f%% of %.0f s, fleeing %.0f%% of %.0f s, no enemy in sight %.0f%% of %.0f s, else %.0f%% of %.0f s; distance when chasing with it p10 %.0f, median %.0f\n", s.ab_known_s / 60, detail::burner_estimated(s) ? " (estimated)" : "", pct(s.ab_share), pct(s.ab_situation_rate[detail::chasing]), s.ab_situation_s[detail::chasing], pct(s.ab_situation_rate[detail::fleeing]), s.ab_situation_s[detail::fleeing], pct(s.ab_situation_rate[detail::crossing]), s.ab_situation_s[detail::crossing], pct(s.ab_situation_rate[detail::fighting]), s.ab_situation_s[detail::fighting], s.ab_chase_distance.p10, s.ab_chase_distance.p50);
	else
		o += "  afterburner: not known (an older client, recorded on the host, too little motion to estimate)\n";
	appendf(o, "  enemy in sight: %.1f min; distance p10 %.0f, p25 %.0f, median %.0f, p75 %.0f, p90 %.0f\n    seconds at", s.los_s / 60, s.los_distance.p10, s.los_distance.p25, s.los_distance.p50, s.los_distance.p75, s.los_distance.p90);
	for (std::size_t i{}; i != DISTANCE_BINS; ++i)
	{
		if (i == 0)
			appendf(o, " <%.0f: %.0f", DISTANCE_EDGES[0], s.distance_seconds[i]);
		else if (i + 1 == DISTANCE_BINS)
			appendf(o, ", >%.0f: %.0f", DISTANCE_EDGES[i - 1], s.distance_seconds[i]);
		else
			appendf(o, ", %.0f-%.0f: %.0f", DISTANCE_EDGES[i - 1], DISTANCE_EDGES[i], s.distance_seconds[i]);
	}
	o += '\n';
	if (s.fire_distance.n)
		appendf(o, "  distance when firing (%.0f shots with the enemy in sight): p25 %.0f, median %.0f, p75 %.0f\n", static_cast<double>(s.fire_distance.n), s.fire_distance.p25, s.fire_distance.p50, s.fire_distance.p75);
	o += "  by shields (fight time; own speed toward the enemy; closing in / backing off / flying away):\n";
	for (std::size_t b{}; b != SHIELD_BUCKETS; ++b)
	{
		const auto &k{s.by_shields[b]};
		if (k.seconds <= 0)
			continue;
		if (b + 1 == SHIELD_BUCKETS)
			appendf(o, "    %3u+    ", detail::count(b) * 25u);
		else
			appendf(o, "    %3u-%-3u ", detail::count(b) * 25u, detail::count(b) * 25u + 24u);
		appendf(o, "%6.0f s  %+6.1f units/s  %3.0f%% / %3.0f%% / %3.0f%%\n", k.seconds, k.approach_mean, pct(k.approach), pct(k.back_off), pct(k.retreat));
	}
	appendf(o, "  dodging: %u bursts aimed at it, a strafe switch %.0f-%.0f ms after %.0f%%, the rhythm's chance %.0f%% (%.0f quiet moments, switch in %.0f%%); room %.1f; excess %.2f +- %.2f%s; reaction %.0f ms\n", s.dodge_triggers, static_cast<double>(limits::REACT_FROM_MS), static_cast<double>(limits::REACT_TO_MS), pct(s.dodge_rate), pct(s.dodge_baseline), static_cast<double>(s.dodge_baseline_n), pct(s.dodge_weave_rate), s.dodge_room, s.dodge_prob, s.dodge_se, s.dodge_measurable ? "" : " (not measurable)", s.dodge_reaction_ms);
	appendf(o, "    of the switches %.0f%% reverse the sideways motion (%u; quiet moments %.0f%%); afterburner lit after %.0f%% (quiet %.0f%%); hit within %.1f s after %.0f%% of %u bursts with a switch, %.0f%% of %u without\n", pct(s.dodge_reverse_share), s.dodge_reverse_n, pct(s.dodge_reverse_baseline), pct(s.dodge_burn_rate), pct(s.dodge_burn_baseline), static_cast<double>(limits::BURST_HIT_MS) / 1000, pct(s.dodge_hit_after_switch), s.dodge_switch_n, pct(s.dodge_hit_after_none), s.dodge_none_n);
	if (!s.enemy_shots && s.hits_taken_from_players)
		appendf(o, "  warning: took %u hits from other players but the recordings hold no shot of theirs; dodging cannot be measured (a recording of an older build, whose bots' shots were left out without -recordmoves-bots?)\n", s.hits_taken_from_players);
	appendf(o, "  primary shots: %u", s.primary_shots);
	for (std::size_t band{}; band != bot::BOT_RANGE_BANDS; ++band)
	{
		unsigned total{};
		for (const auto n : s.primary_by_band[band])
			total += n;
		if (!total)
			continue;
		appendf(o, "\n    %s:", band_names[band]);
		for (std::size_t w{}; w != WEAPON_SLOTS; ++w)
			if (const auto n{s.primary_by_band[band][w]})
				appendf(o, " %s %.0f%%", primary_names[w], 100.0 * n / total);
	}
	o += '\n';
	appendf(o, "  secondary shots: %u", s.secondary_shots);
	for (std::size_t w{}; w != WEAPON_SLOTS; ++w)
		if (const auto n{s.secondary_count[w]})
			appendf(o, ", %s %u", secondary_names[w], n);
	if (s.volleys)
		appendf(o, "; %u volleys of %.1f (max %u), %.1f s apart (median); distance median %.0f", s.volleys, s.volley_size, s.volley_max, s.volley_gap_s.p50, s.secondary_distance.p50);
	o += '\n';
	appendf(o, "  pickups: %u (%.1f/min), off course %.0f%% of %u, in a fight %.0f%%\n", s.pickups, s.pickups_per_min, pct(s.pickup_detour_share), s.pickup_detour_n, pct(s.pickup_in_fight_share));
	for (std::size_t c{}; c != PICKUP_CLASSES; ++c)
	{
		const auto &k{s.pickup_sight[c]};
		appendf(o, "  %s in sight before taken: %u; took %.0f%%, went for %.0f%% (lost the race %u); in a fight %u, went for %.0f%%; outside %u, went for %.0f%%; taken unseen %u\n", pickup_class_names[c], k.seen, pct(k.taken_share), pct(k.went_share), k.lost, k.fight_seen, pct(k.fight_went_share), k.calm_seen, pct(k.calm_went_share), k.unseen_taken);
		if (k.sight_distance.n)
			appendf(o, "    taken: from %.0f units at the first sight (p25 %.0f, p90 %.0f), %.1f s later (p90 %.1f), %.0f units flown beyond that (median), off course at the sight %.0f%%\n", k.sight_distance.p50, k.sight_distance.p25, k.sight_distance.p90, k.take_s.p50, k.take_s.p90, k.extra_path.p50, pct(k.off_course_share));
	}
	if (s.pickup_sight[0].seen + s.pickup_sight[1].seen)
		appendf(o, "    (sight judged with the level's geometry: %.0f%% of the sightings)\n", pct(s.pickup_sight_geometry_share));
	appendf(o, "  lost sight of the enemy %u times, followed %.0f%%; for %.1f s (median), p90 %.1f s\n", s.sight_losses, pct(s.pursue_share), s.pursuit_s.p50, s.pursuit_s.p90);
	appendf(o, "  hits: dealt %u direct and %u splash (%.0f shields), taken %u (%.0f shields); %.2f direct hits per primary shot\n", s.hits_dealt, s.splash_dealt, s.damage_dealt, s.hits_taken, s.damage_taken, s.hits_per_shot);
	if (!s.levels.empty())
	{
		o += "  by level and room (time alive; speed; sideways/vertical thrust in the fights; distance to the enemy in sight, median, of a line of fire of, the share of it; free room to the nearer side in the fights; large turns: reverse/slide/forward):\n";
		for (const auto &l : s.levels)
		{
			appendf(o, "    %s\n", l.label.c_str());
			for (std::size_t c{}; c != geometry::ROOM_CLASSES + 1; ++c)
			{
				const auto &r{c == geometry::ROOM_CLASSES ? l.all : l.by_class[c]};
				if (r.alive_s < 1)
					continue;
				appendf(o, "      %-7s %5.0f s  speed %4.1f", c == geometry::ROOM_CLASSES ? "all" : geometry::room_class_names[c], r.alive_s, r.speed);
				if (r.fight_s > 0)
					appendf(o, "  strafe %3.0f%%  side room %3.0f", pct(r.strafe_share), r.side_room.p50);
				if (r.distance.n)
					appendf(o, "  enemy at %3.0f of %3.0f (%2.0f%%)", r.distance.p50, r.line.p50, pct(r.line_share.p50));
				if (r.turns)
					appendf(o, "  turns %u: %.0f/%.0f/%.0f%%", r.turns, pct(r.reverse_turn_share), pct(r.slide_turn_share), pct(r.forward_turn_share));
				o += '\n';
			}
		}
		o += "  distance when firing by the line of fire (shots, median):";
		for (std::size_t b{}; b != LINE_BANDS; ++b)
		{
			const auto &f{s.fire_distance_by_line[b]};
			if (b == 0)
				appendf(o, " line < %.0f: ", LINE_EDGES[0]);
			else if (b + 1 == LINE_BANDS)
				appendf(o, "; >= %.0f: ", LINE_EDGES[b - 1]);
			else
				appendf(o, "; %.0f-%.0f: ", LINE_EDGES[b - 1], LINE_EDGES[b]);
			appendf(o, "%.0f, %.0f", static_cast<double>(f.n), f.p50);
		}
		appendf(o, "; the enemy at %.0f%% of the line (median of the time in sight)\n", pct(s.line_share.p50));
	}

	o += "\nProposed bot style (closest built-in style: ";
	o += bot::bot_style_names[std::min<std::size_t>(static_cast<std::size_t>(profile.base_style), bot::BOT_STYLE_COUNT - 1)];
	o += ")\n";
	const auto base{bot::style_of(profile.base_style)};
	for (const auto &e : profile.entries)
	{
		if (e.key.starts_with("measured."))
			continue;
		const auto k{bot::find_style_profile_key(e.key)};
		appendf(o, "  %-30s %8s  %-6s  %s\n", e.key.c_str(), bot::style_profile_detail::format_number(e.value).c_str(), bot::style_confidence_names[static_cast<std::size_t>(e.confidence)], k ? std::string{k->text}.c_str() : "");
	}
	appendf(o, "  (for comparison, %s: retreat %.0f, engage %.2f, collect %.2f, range %.2f, chase %.2f, burn from %.0f)\n", bot::bot_style_names[std::min<std::size_t>(static_cast<std::size_t>(profile.base_style), bot::BOT_STYLE_COUNT - 1)], base.retreat_shields, base.engage_weight, base.collect_weight, base.range_scale, base.chase_memory, base.burn_chase_distance);
	return o;
}

/*
 * All of it, from files to profiles.
 */

struct player_result
{
	player_stats stats;
	bot::style_profile profile;
};

struct analysis_result
{
	std::vector<session> sessions;
	std::vector<std::string> notes;
	/* One per player (callsign; a bot and a human of the same name are
	 * two), most time alive first.
	 */
	std::vector<player_result> players;
};

[[nodiscard]]
inline analysis_result analyse_recordings(const std::span<const recording> files, const bot::bot_skill base_skill = bot::BOT_DEFAULT_SKILL, const ship_model &ship = pyro_gx(), const level_geometries *const geo = nullptr)
{
	analysis_result r;
	r.sessions = group_sessions(files, &r.notes);
	struct entry
	{
		std::string callsign;
		bool bot{};
		std::vector<track> tracks;
	};
	std::vector<entry> entries;
	for (const auto &ses : r.sessions)
	{
		const auto ms{merge_session(files, ses)};
		for (std::size_t p{}; p != ms.players.size(); ++p)
		{
			const auto &sp{ms.players[p]};
			if (sp.samples.empty())
				continue;
			auto e{std::find_if(entries.begin(), entries.end(), [&sp](const entry &x) { return x.bot == sp.bot && lower(x.callsign) == lower(sp.callsign); })};
			if (e == entries.end())
				e = entries.insert(e, {sp.callsign, sp.bot, {}});
			e->tracks.push_back(build_track(ms, p, ship, geo, &ses));
		}
	}
	for (const auto &e : entries)
	{
		player_result pr;
		pr.stats = analyse(e.tracks, ship);
		pr.stats.callsign = e.callsign;
		pr.stats.bot = e.bot;
		pr.profile = propose_profile(pr.stats, base_skill);
		r.players.push_back(std::move(pr));
	}
	std::stable_sort(r.players.begin(), r.players.end(), [](const player_result &a, const player_result &b) { return a.stats.alive_s > b.stats.alive_s; });
	return r;
}

}
