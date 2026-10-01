/*
 * This file is part of the DXX-Rebirth project <https://github.com/dxx-rebirth/dxx-rebirth/>.
 * It is copyright by its individual contributors, as recorded in the
 * project's Git history.  See COPYING.txt at the top level for license
 * terms and a link to the Git history.
 */
/*
 * How the bots fly a free-for-all on a real level
 * (Documentation/multiplayer-bots.md section 9.15).
 *
 * test-bot-fight-sim flies one bot in the open against a scripted
 * enemy; its numbers were near the human's, and the bots of the next
 * playtest (exp-31, 2026-10-01) were not: on a real level a fight is cut
 * up by walls, pickups, retreats and enemies out of the field of view,
 * and there the bots fell back to the velocity controller.  This
 * simulation flies several bots on the geometry of a real level, read
 * from the mission's HOG (level_geometry.h), with their navigation
 * (bot_nav.h: the path, the string pulling, the stuck recovery), their
 * goals (bot_goals.h: hunt, collect, retreat, roam), their shields,
 * shots, dodges and pickups, and the movement of bot_tick
 * (similar/main/bot.cpp: bot_brain.h, bot_movement.h), against each
 * other and an enemy that is either a human's track replayed from a
 * movement recording (where he flew and where he shot) or a scripted
 * one.  The flight is written as the movement recorder writes it, with
 * the bots' movement modes (format minor 4), and the analysis of
 * movrec-analyse (movement_analysis.h) reads it.
 *
 * The level data are not part of the source: the test takes the HOG
 * (and the recording) from its arguments or the environment and skips
 * without them.
 *
 *	test-bot-level-sim [-v] [-o DIR] [-hog FILE [-level NAME] [-track FILE.dmr]]...
 *
 *	BOT_LEVEL_SIM_HOG=FILE[;FILE...]	the same as -hog for each
 *	BOT_LEVEL_SIM_TRACK=FILE[;FILE...]	the track for each HOG in turn
 *
 * -hog: the mission's .hog; -level: the level file in it (default: the
 * one whose segment count the track's recording names, else the first);
 * -track: a movement recording made on that level; its local human is
 * the replayed enemy.  Each map is flown twice: with the movement as it
 * was before section 9.15 (bots-real-flight) and as it is; -v prints the
 * analysis' report of each bot; -o writes the recordings into DIR.
 *
 * The checks, over all the bots of a map together (checks_of): the
 * keys' share of the fight time (an enemy in sight within 400 units, as
 * the analysis counts a fight), the mode changes and spells, being
 * stuck, the walls touched while retreating, the afterburner pickups;
 * and against the movement before: the speed, the speed across, the
 * strafe reversals, the vertical share, the afterburner fleeing.
 */

#include <algorithm>
#include <array>
#include <cinttypes>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <numbers>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "level_geometry.h"
#include "movement_analysis.h"
#include "movement_record_reader.h"
#include "bot_goals.h"
#include "bot_movement.h"
#include "bot_nav.h"
#include "bot_weapons.h"
#include "bot_flight_model.h"

namespace geo = dcx::movrec::geometry;
namespace mr = dcx::movrec;
namespace an = dcx::movrec::analysis;
using namespace dcx::bot;
using namespace dcx::bot::flight_model;

static_assert(static_cast<uint8_t>(move_mode::fight_keys) == mr::bot_modes::fight);
static_assert(static_cast<uint8_t>(move_mode::path_keys) == mr::bot_modes::path_keys);
static_assert(static_cast<uint8_t>(move_mode::recover) == mr::bot_modes::recover);
static_assert(MOVE_MODE_COUNT == mr::bot_modes::count);

namespace {

bool verbose;
/* -o DIR: the recordings are written there. */
const char *out_dir;

using bvec = dcx::bot::vec3;

[[nodiscard]]
geo::vec3 to_geo(const bvec &v)
{
	return {{v.x, v.y, v.z}};
}

[[nodiscard]]
bvec from_geo(const geo::vec3 &v)
{
	return {v[0], v[1], v[2]};
}

[[nodiscard]]
mr::vec3 to_mr(const bvec &v)
{
	return {{v.x, v.y, v.z}};
}

constexpr double FPS{60};
constexpr unsigned RECORD_RATE{30};
/* The Pyro-GX's size (obj.size), and how near a wall its centre comes. */
constexpr double SHIP_SIZE{4.4};
constexpr double WALL_CLEARANCE{3};
/* The shots: a laser's speed, its damage, the rate of fire.  The bots
 * of exp-31 took 14 a hit (splash included) and 3500-3800 in 12 minutes
 * each; the shots here hit less often (no missiles, no splash), so each
 * does more, for as many deaths and retreats.
 */
constexpr double SHOT_SPEED{120};
constexpr double SHOT_DAMAGE{25};
/* A shot hits a ship whose centre passes within this (the ship's size
 * and the laser's).
 */
constexpr double SHOT_HIT_RADIUS{SHIP_SIZE + 1.5};
constexpr double SHOT_INTERVAL{0.25};
constexpr double SHOT_LIFE{2.5};
/* bot.cpp's constants the simulation needs. */
constexpr unsigned BOT_NAV_NODE_LIMIT{4000};
constexpr double BOT_DISTANCE_MAX_COST{2500};
constexpr unsigned BOT_DISTANCE_NODE_LIMIT{3000};
constexpr unsigned BOT_REVENGE_TICKS{3 * BOT_TICK_RATE};
constexpr double BOT_ROAM_MIN_DISTANCE{120};
constexpr double BOT_DODGE_SCAN{150};
constexpr double BOT_DODGE_HORIZON{0.7};
constexpr unsigned BOT_DODGE_TICKS{ticks_from_ms(350)};
constexpr double BOT_BEND_MAX_ANGLE{radians(40)};
constexpr unsigned BOT_FLEE_TRIES{10};
constexpr unsigned BOT_HUNT_REPLAN_TICKS{BOT_TICK_RATE / 2};
constexpr unsigned BOT_COLLECT_IGNORE_TICKS{5 * BOT_TICK_RATE};
constexpr unsigned BOT_COLLECT_UNREACHABLE_TICKS{10 * BOT_TICK_RATE};
/* A dead bot comes back after this long. */
constexpr double RESPAWN_S{3};
/* The pickups: shields, energy and afterburners at segment centres. */
constexpr unsigned SHIELD_PICKUPS{10};
constexpr unsigned ENERGY_PICKUPS{4};
constexpr unsigned AFTERBURNER_PICKUPS{2};
constexpr double PICKUP_RESPAWN_S{30};
constexpr double PICKUP_REACH{5};
/* The replayed human's aim: within this of his nose, this far, with
 * this error.
 */
constexpr double ENEMY_AIM_CONE{radians(60)};
constexpr double ENEMY_AIM_RANGE{300};
constexpr double ENEMY_AIM_ERROR_DEG{1.5};
/* The scripted enemy's flight without a track (s). */
constexpr double SCRIPTED_S{600};

/*
 * The level.
 */
struct world
{
	geo::level mesh;
	nav_graph graph;
	std::vector<bvec> side_centres;
	astar_search search;
	path_result path;
	unsigned traces{};
	[[nodiscard]]
	geo::ray_end trace(const std::size_t seg, const bvec &from, const bvec &dir, const double limit)
	{
		++traces;
		return geo::trace_ray(mesh, seg, to_geo(from), to_geo(dir), limit);
	}
	/* A clear straight line from `from` (in `seg`) to `to`. */
	[[nodiscard]]
	bool clear(const std::size_t seg, const bvec &from, const bvec &to)
	{
		const auto d{to - from};
		const double l{length(d)};
		if (l < 1e-6)
			return true;
		return !trace(seg, from, d * (1 / l), l).wall;
	}
	/* The same for a ship's body (bot.cpp's line_clear with a radius):
	 * the line itself and four lines `rad` off it.
	 */
	[[nodiscard]]
	bool clear_wide(const std::size_t seg, const bvec &from, const bvec &to, const double rad)
	{
		if (!clear(seg, from, to))
			return false;
		const auto f{normalized(to - from)};
		if (f == bvec{})
			return true;
		auto r{normalized(cross(std::abs(f.y) < 0.9 ? bvec{0, 1, 0} : bvec{1, 0, 0}, f))};
		const auto u{cross(f, r)};
		for (const auto &off : {r * rad, r * -rad, u * rad, u * -rad})
		{
			const auto side{trace(seg, from, normalized(off), rad)};
			if (side.wall)
				return false;
			if (!clear(side.segment, from + off, to + off))
				return false;
		}
		return true;
	}
	/* The segment at `to`, reached in a straight line from `from`. */
	[[nodiscard]]
	std::size_t segment_at(const std::size_t seg, const bvec &from, const bvec &to)
	{
		const auto d{to - from};
		const double l{length(d)};
		if (l < 1e-9)
			return seg;
		return trace(seg, from, d * (1 / l), l).segment;
	}
	/* The free distance from `from` along `dir`, up to `limit`. */
	[[nodiscard]]
	double room(const std::size_t seg, const bvec &from, const bvec &dir, const double limit)
	{
		const auto r{trace(seg, from, normalized(dir), limit)};
		return r.wall ? r.distance : limit;
	}
};

struct move_result
{
	bvec pos;
	std::size_t seg{};
	bool hit{};
	bvec normal;
};

/* A ship's move by `delta`: it stops WALL_CLEARANCE short of a wall. */
[[nodiscard]]
move_result move_ship(world &w, const std::size_t seg, const bvec &pos, const bvec &delta)
{
	move_result out{pos, seg, false, {}};
	const double l{length(delta)};
	if (l < 1e-9)
		return out;
	const auto dir{delta * (1 / l)};
	const auto r{w.trace(seg, pos, dir, l + WALL_CLEARANCE)};
	if (!r.wall)
	{
		out.pos = pos + delta;
		out.seg = w.segment_at(seg, pos, out.pos);
		return out;
	}
	const double go{std::max(0.0, r.distance - WALL_CLEARANCE)};
	out.pos = pos + dir * go;
	out.seg = w.segment_at(seg, pos, out.pos);
	out.hit = true;
	out.normal = from_geo(r.normal);
	return out;
}

[[nodiscard]]
bvec segment_centre(const geo::level &l, const std::size_t seg)
{
	bvec c;
	for (const auto v : l.segments[seg].verts)
		c += from_geo(l.vertices[v]);
	return c * (1.0 / 8);
}

[[nodiscard]]
bvec side_centre(const geo::level &l, const std::size_t seg, const std::size_t side)
{
	bvec c;
	for (const auto k : geo::side_verts[side])
		c += from_geo(l.vertices[l.segments[seg].verts[k]]);
	return c * (1.0 / 4);
}

void build_graph(world &w)
{
	const auto &l{w.mesh};
	const auto n{l.segments.size()};
	w.graph.begin(n);
	w.side_centres.assign(n * 6, bvec{});
	for (std::size_t i{}; i != n; ++i)
	{
		w.graph.set_position(static_cast<uint32_t>(i), segment_centre(l, i));
		for (std::size_t k{}; k != 6; ++k)
			w.side_centres[i * 6 + k] = side_centre(l, i, k);
	}
	for (std::size_t i{}; i != n; ++i)
		for (std::size_t k{}; k != 6; ++k)
		{
			const auto c{l.segments[i].children[k]};
			if (c == geo::SEGMENT_NONE || c == geo::SEGMENT_EXIT || c >= n)
				continue;
			const auto &mid{w.side_centres[i * 6 + k]};
			const double cost{distance(w.graph.position(static_cast<uint32_t>(i)), mid) + distance(mid, w.graph.position(c))};
			w.graph.add_edge(static_cast<uint32_t>(i), {c, static_cast<uint8_t>(k), static_cast<float>(cost)});
		}
	w.graph.finish();
}

/*
 * The replayed enemy: a human's track from a movement recording.
 */
struct track_point
{
	double t{};
	bvec pos, vel;
	frame3 orient;
	std::size_t seg{};
	bool alive{};
};

struct enemy_track
{
	std::vector<track_point> pts;
	std::vector<double> shots;
	std::string level_file;
	std::size_t segments{};
	double length_s{};
};

[[nodiscard]]
std::optional<enemy_track> load_track(const char *const path)
{
	const auto data{mr::load_file(path)};
	if (!data)
		return std::nullopt;
	auto rec{an::load_recording(*data, path)};
	if (!rec || rec->levels.empty())
		return std::nullopt;
	/* The recording machine's own human. */
	int who{-1};
	for (std::size_t i{}; i != rec->players.size(); ++i)
		if (rec->players[i].local && !rec->players[i].bot)
		{
			who = static_cast<int>(i);
			break;
		}
	if (who < 0)
		return std::nullopt;
	enemy_track t;
	t.level_file = rec->levels.front().rec.level_file;
	t.segments = rec->levels.front().rec.segments;
	for (const auto &s : rec->samples)
	{
		if (s.who != who || s.level)
			continue;
		const auto u{mr::to_units(s.s)};
		track_point p;
		p.t = s.time_ms / 1000.0;
		p.pos = {u.pos[0], u.pos[1], u.pos[2]};
		p.vel = {u.vel[0], u.vel[1], u.vel[2]};
		p.orient.r = {u.orient.right[0], u.orient.right[1], u.orient.right[2]};
		p.orient.u = {u.orient.up[0], u.orient.up[1], u.orient.up[2]};
		p.orient.f = {u.orient.forward[0], u.orient.forward[1], u.orient.forward[2]};
		p.seg = s.s.segment;
		p.alive = (s.s.flags & mr::sample_flag::alive) != 0;
		t.pts.push_back(p);
	}
	for (const auto &e : rec->events)
		if (e.who == who && !e.level && e.e.type == mr::record_type::fire && e.e.kind == mr::fire_kind::primary)
			t.shots.push_back(e.e.time_ms / 1000.0);
	if (t.pts.size() < 2)
		return std::nullopt;
	const double t0{t.pts.front().t};
	for (auto &p : t.pts)
		p.t -= t0;
	for (auto &s : t.shots)
		s -= t0;
	t.length_s = t.pts.back().t;
	return t;
}

/*
 * The ships.
 */
enum class sim_goal : uint8_t
{
	none,
	roam,
	hunt,
	collect,
	retreat,
};

struct shot
{
	bvec pos, vel;
	std::size_t seg{};
	unsigned owner{};
	double life{};
	uint16_t sig{};
};

struct pickup
{
	item kind{item::shield};
	std::size_t seg{};
	bvec pos;
	double back_at{};
	[[nodiscard]]
	bool present(const double t) const
	{
		return t >= back_at;
	}
};

struct bot_setup
{
	bot_skill skill;
	bot_style style;
};

/* Five bots as in the playtests: two Insane, an Ace, two Hotshots. */
inline constexpr std::array<bot_setup, 5> bot_setups{{
	{bot_skill::insane, bot_style::balanced},
	{bot_skill::insane, bot_style::aggressive},
	{bot_skill::ace, bot_style::balanced},
	{bot_skill::hotshot, bot_style::cautious},
	{bot_skill::hotshot, bot_style::balanced},
}};
constexpr unsigned BOTS{static_cast<unsigned>(bot_setups.size())};
/* Ship 0 is the enemy (replayed or scripted), 1.. the bots. */
constexpr unsigned SHIPS{BOTS + 1};

struct percept
{
	uint8_t target{0xff};
	bool visible{};
	bvec pos, vel;
};

/* Counters of one bot, for the checks. */
struct tally
{
	double alive_s{}, fight_s{}, fight_keys_s{};
	unsigned mode_changes{};
	double recover_s{};
	unsigned wall_hits{}, retreat_wall_hits{};
	unsigned afterburner_pickups{}, shield_pickups{};
	double afterburner_held_s{};
	double retreat_s{}, retreat_burn_s{};
	std::vector<double> spells;
	unsigned kills{}, deaths{};
};

struct sim_bot
{
	unsigned id{};
	bot_skill skill_level{};
	skill_params sk;
	style_params st;
	tune_params tune;
	bot_rng rng{1};
	ship s;
	std::size_t seg{};
	bool alive{true};
	double respawn_at{};
	double shields{100};
	bool has_afterburner{};
	double charge{1};
	bool burning{};
	double next_shot{};
	/* Perception and memory. */
	std::array<target_memory, SHIPS> memory{};
	std::array<bool, SHIPS> visible_now{};
	std::optional<uint8_t> target;
	delay_line<percept, 16> seen;
	bool shot_clear{};
	uint8_t last_attacker{0xff};
	uint32_t attacked_tick{};
	aim_error aim;
	aim_lead lead;
	juke_state juke;
	approach_key approach;
	turn_round_state turning;
	slide_state slide;
	lateral_keys lateral;
	dcx::bot::path_keys pkeys;
	mode_hold hold;
	fire_blocked blocked;
	move_mode mode{move_mode::none};
	fight_room room;
	int heading_pref{1};
	bvec face_dir{0, 0, 1}, face_rate, move_cmd, aim_dir{0, 0, 1};
	bool fire{};
	uint32_t dodge_salt{};
	bvec dodge_dir;
	uint32_t dodge_from{}, dodge_until{};
	bvec avoid;
	uint32_t avoid_until{};
	bool fleeing{}, flee_turned{}, flee_burn{};
	uint32_t flee_roll_at{};
	bool roam_burn_ok{true};
	uint32_t roam_roll_at{};
	/* Navigation. */
	sim_goal goal{sim_goal::none};
	uint32_t goal_seg{};
	std::optional<std::size_t> collect_pickup;
	std::vector<bvec> points;
	std::vector<std::pair<uint32_t, uint8_t>> point_edges;
	std::size_t point_index{}, steer_index{};
	uint32_t next_plan_tick{}, last_plan_tick{};
	stuck_detector stuck;
	edge_penalties penalties;
	bvec recover_dir;
	nav_distances dist;
	std::vector<uint32_t> visited;
	std::vector<uint32_t> pickup_ignore;
	std::optional<goal_kind> chosen;
	/* Recording. */
	steer_output ctl;
	bool burn_now{};
	double burn_scale{};
	tally n;
	move_mode last_mode{move_mode::none};
	double spell_start{};
	void clear_path()
	{
		points.clear();
		point_edges.clear();
		point_index = steer_index = 0;
		stuck.restart_window();
	}
};

struct enemy_state
{
	bool alive{};
	bvec pos, vel;
	frame3 orient;
	std::size_t seg{};
	/* The scripted enemy's own path. */
	std::vector<bvec> points;
	std::size_t index{};
	double next_shot{};
	double strafe_phase{};
};

struct sim_options
{
	bool before{};
	uint32_t seed{1};
};

struct sim
{
	world &w;
	const enemy_track *track{};
	sim_options opt;
	std::array<sim_bot, BOTS> bots;
	enemy_state enemy;
	std::size_t track_index{};
	std::size_t shot_index{};
	std::vector<shot> shots;
	uint16_t next_sig{1};
	/* Shots fired, hits, shots into a wall. */
	unsigned fired{}, hits{}, walled{}, enemy_hits{};
	std::vector<pickup> pickups;
	bot_rng world_rng{7};
	limits lim{ship_limits()};
	uint32_t tick{};
	double t{};
	/* Who hit whom when (the recorder's attacked mask). */
	std::array<std::array<double, SHIPS>, SHIPS> hit_at{};
	std::vector<std::uint8_t> bytes;
	std::unique_ptr<mr::chunk_builder> chunk{std::make_unique<mr::chunk_builder>()};
	mr::record_buffer buf;
	sim(world &wd, const enemy_track *tr, const sim_options &o) :
		w{wd}, track{tr}, opt{o}, world_rng{bot_seed(o.seed, 7, 3)}
	{
	}
	[[nodiscard]]
	double max_speed() const
	{
		return lim.max_speed;
	}
	/* Ship `i`'s place, if it is alive. */
	[[nodiscard]]
	bool ship_alive(const unsigned i) const
	{
		return i ? bots[i - 1].alive : enemy.alive;
	}
	[[nodiscard]]
	bvec ship_pos(const unsigned i) const
	{
		return i ? bots[i - 1].s.pos : enemy.pos;
	}
	[[nodiscard]]
	bvec ship_vel(const unsigned i) const
	{
		return i ? bots[i - 1].s.vel : enemy.vel;
	}
	[[nodiscard]]
	const frame3 &ship_orient(const unsigned i) const
	{
		return i ? bots[i - 1].s.orient : enemy.orient;
	}
	[[nodiscard]]
	std::size_t ship_seg(const unsigned i) const
	{
		return i ? bots[i - 1].seg : enemy.seg;
	}
	[[nodiscard]]
	uint32_t random_segment(bot_rng &rng)
	{
		const auto n{static_cast<uint32_t>(w.graph.size())};
		for (unsigned k{}; k != 64; ++k)
		{
			const uint32_t s{rng.below(n)};
			if (w.graph.neighbours(s).size() >= 1)
				return s;
		}
		return 0;
	}
	void setup();
	void spawn(sim_bot &b);
	void plan_path(sim_bot &b, uint32_t goal_seg, std::optional<bvec> goal_pos);
	void set_goal(sim_bot &b, sim_goal g, uint32_t seg, std::optional<bvec> pos);
	void choose_roam_goal(sim_bot &b);
	bvec follow_path(sim_bot &b, bool engaged);
	void perceive(sim_bot &b);
	void think(sim_bot &b);
	void brain_tick(sim_bot &b);
	void decide_afterburner(sim_bot &b, const bvec &wanted);
	void move_enemy();
	void fly_bot(sim_bot &b);
	void fire_shot(unsigned owner, const bvec &from, const bvec &dir, std::size_t seg);
	void move_shots();
	void damage(unsigned victim, unsigned attacker, double amount);
	void touch_pickups(sim_bot &b);
	void record();
	void put(std::span<const std::uint8_t> r);
	void flush();
	void run(double seconds);
};

void sim::put(const std::span<const std::uint8_t> r)
{
	if (!chunk->fits(r.size()))
		flush();
	chunk->append(r);
}

void sim::flush()
{
	if (chunk->empty())
		return;
	const auto c{chunk->finish()};
	bytes.insert(bytes.end(), c.begin(), c.end());
	chunk->reset();
}

void sim::setup()
{
	mr::file_header h;
	h.tick_rate = RECORD_RATE;
	h.flags = static_cast<std::uint16_t>(mr::header_flag::multiplayer) | static_cast<std::uint16_t>(mr::header_flag::host) | static_cast<std::uint16_t>(mr::header_flag::bots_recorded);
	h.start_unix_time = 1'790'000'000;
	h.local_pid = 0;
	h.program = "test-bot-level-sim";
	h.mission = "level sim";
	h.level_name = "level";
	h.level_num = 1;
	h.num_players = SHIPS;
	h.players[0] = {0, static_cast<std::uint8_t>(mr::player_flag::connected | mr::player_flag::recorded), 0xff, "enemy"};
	for (unsigned i{1}; i != SHIPS; ++i)
	{
		char name[16];
		std::snprintf(name, sizeof(name), "bot%u", i);
		h.players[i] = {static_cast<std::uint8_t>(i), static_cast<std::uint8_t>(mr::player_flag::connected | mr::player_flag::recorded | mr::player_flag::bot | mr::player_flag::local), 0xff, name};
	}
	std::array<std::uint8_t, mr::MAX_HEADER_SIZE> hb;
	const auto n{mr::encode_header(hb, h)};
	bytes.assign(hb.begin(), hb.begin() + static_cast<std::ptrdiff_t>(n));
	put(mr::encode_level(buf, 1, static_cast<std::uint16_t>(w.mesh.segments.size()), 0x4, h.mission, h.level_name));
	for (unsigned i{}; i != SHIPS; ++i)
		put(mr::encode_player(buf, h.players[i].pid, h.players[i].flags, h.players[i].team, h.players[i].callsign));
	/* The pickups, at segment centres drawn from the world's numbers. */
	const auto add{[this](const item k, const unsigned count) {
		for (unsigned i{}; i != count; ++i)
		{
			const auto s{random_segment(world_rng)};
			pickups.push_back({k, s, w.graph.position(s), 0});
		}
	}};
	add(item::shield, SHIELD_PICKUPS);
	add(item::energy, ENERGY_PICKUPS);
	add(item::afterburner, AFTERBURNER_PICKUPS);
	for (unsigned i{}; i != BOTS; ++i)
	{
		auto &b{bots[i]};
		b.id = i + 1;
		b.skill_level = bot_setups[i].skill;
		b.sk = skill_of(b.skill_level);
		b.st = style_of(bot_setups[i].style);
		b.rng = bot_rng{bot_seed(opt.seed, b.id, 1)};
		b.visited.assign(w.graph.size(), 0);
		b.pickup_ignore.assign(pickups.size(), 0);
		spawn(b);
	}
	if (!track)
	{
		enemy.alive = true;
		enemy.seg = random_segment(world_rng);
		enemy.pos = w.graph.position(static_cast<uint32_t>(enemy.seg));
		enemy.orient = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
	}
}

/* A new life at a random segment centre, away from the enemies. */
void sim::spawn(sim_bot &b)
{
	uint32_t best{0};
	double best_d{-1};
	for (unsigned k{}; k != 8; ++k)
	{
		const auto s{random_segment(b.rng)};
		double nearest{1e18};
		for (unsigned j{}; j != SHIPS; ++j)
			if (j != b.id && ship_alive(j))
				nearest = std::min(nearest, distance(ship_pos(j), w.graph.position(s)));
		if (nearest > best_d)
		{
			best_d = nearest;
			best = s;
		}
	}
	b.seg = best;
	b.s = {};
	b.s.pos = w.graph.position(best);
	const double a{b.rng.uniform(0, 2 * std::numbers::pi)};
	b.s.orient.f = {std::cos(a), 0, std::sin(a)};
	b.s.orient.u = {0, 1, 0};
	b.s.orient.r = cross(b.s.orient.u, b.s.orient.f);
	b.alive = true;
	b.shields = 100;
	b.has_afterburner = false;
	b.charge = 1;
	b.burning = false;
	for (auto &m : b.memory)
		m = {};
	b.visible_now = {};
	b.target.reset();
	b.seen.clear();
	b.shot_clear = false;
	b.attacked_tick = tick;
	b.aim.reset();
	b.lead.reset();
	b.juke.reset();
	b.approach.reset();
	b.turning.reset();
	b.slide.reset();
	b.lateral.reset();
	b.pkeys.reset();
	b.hold.reset();
	b.blocked.reset();
	b.mode = move_mode::none;
	b.room = {};
	b.face_dir = b.s.orient.f;
	b.face_rate = {};
	b.move_cmd = {};
	b.dodge_salt = b.rng.next();
	b.dodge_from = b.dodge_until = 0;
	b.avoid = {};
	b.avoid_until = 0;
	b.fleeing = b.flee_turned = b.flee_burn = false;
	b.goal = sim_goal::none;
	b.collect_pickup.reset();
	b.clear_path();
	b.stuck.reset();
	b.penalties.clear();
	b.chosen.reset();
}

void sim::plan_path(sim_bot &b, const uint32_t goal_seg, const std::optional<bvec> goal_pos)
{
	b.clear_path();
	b.next_plan_tick = tick + 2 * BOT_TICK_RATE;
	b.last_plan_tick = tick;
	const auto start{static_cast<uint32_t>(b.seg)};
	if (start >= w.graph.size() || goal_seg >= w.graph.size())
		return;
	if (!w.search.find(w.graph, start, goal_seg, [](uint32_t, const nav_edge &) {
		return true;
	}, [&b](const uint32_t from, const nav_edge &e) {
		return b.penalties.cost(from, e.side);
	}, BOT_NAV_NODE_LIMIT, w.path))
		return;
	const auto &steps{w.path.steps};
	for (std::size_t i{1}; i < steps.size(); ++i)
	{
		const auto from{steps[i - 1].node};
		const auto side{steps[i].side};
		b.points.push_back(w.side_centres[from * 6 + side]);
		b.point_edges.emplace_back(from, side);
		b.points.push_back(w.graph.position(steps[i].node));
		b.point_edges.emplace_back(from, side);
	}
	if (w.path.complete && goal_pos && w.clear(goal_seg, w.graph.position(goal_seg), *goal_pos))
	{
		if (b.points.empty())
		{
			b.points.push_back(*goal_pos);
			b.point_edges.emplace_back(start, NAV_NO_SIDE);
		}
		else
			b.points.back() = *goal_pos;
	}
	b.stuck.restart_window();
}

void sim::set_goal(sim_bot &b, const sim_goal g, const uint32_t seg, const std::optional<bvec> pos)
{
	if (b.goal != g || b.goal_seg != seg)
	{
		b.stuck.reset();
		b.penalties.clear();
	}
	b.goal = g;
	b.goal_seg = seg;
	plan_path(b, seg, pos);
}

void sim::choose_roam_goal(sim_bot &b)
{
	const auto below{[&b](const uint32_t n) {
		return b.rng.below(n);
	}};
	const auto seconds_since{[this, &b](const uint32_t s) {
		return s < b.visited.size() && b.visited[s] ? (tick - (b.visited[s] - 1)) / static_cast<double>(BOT_TICK_RATE) : 1e9;
	}};
	const auto explore{pick_explore_goal(w.graph, static_cast<uint32_t>(b.seg), below, [&b](const uint32_t s) {
		return b.dist.cost(s);
	}, seconds_since)};
	const uint32_t s{explore ? *explore : pick_roam_goal(w.graph, static_cast<uint32_t>(b.seg), b.s.pos, BOT_ROAM_MIN_DISTANCE, below)};
	set_goal(b, sim_goal::roam, s, std::nullopt);
}

/* bot.cpp's follow_path. */
bvec sim::follow_path(sim_bot &b, const bool engaged)
{
	const auto pos{b.s.pos};
	const auto frame{b.s.orient};
	const double vmax{max_speed()};
	if (b.points.empty())
	{
		if (!engaged)
		{
			b.face_dir = frame.f;
			b.face_rate = {};
		}
		return {};
	}
	const double reach{std::max(SHIP_SIZE * 1.5, 5.0)};
	const bool collecting{b.goal == sim_goal::collect};
	b.point_index = advance_along(b.points, b.point_index, pos, reach);
	if (b.steer_index < b.point_index)
		b.steer_index = b.point_index;
	const auto &target{b.points[b.steer_index]};
	const auto to{target - pos};
	const double dist{length(to)};
	const bool last{b.steer_index + 1 == b.points.size()};
	double speed{vmax};
	if (last)
		speed = std::min(speed, collecting ? dist * 3 + 8 : dist * 1.5);
	else if (opt.before)
	{
		const auto turn{angle_between(to, b.points[b.steer_index + 1] - target)};
		if (turn > radians(60) && dist < 40)
			speed = std::min(speed, std::max(vmax * 0.4, dist * 1.5));
	}
	else
		speed = corner_speed(angle_between(to, b.points[b.steer_index + 1] - target), dist, vmax);
	if (!engaged)
	{
		b.face_dir = normalized(to);
		b.face_rate = {};
	}
	switch (b.stuck.update(remaining_length(b.points, b.point_index, pos)))
	{
		case stuck_event::none:
			break;
		case stuck_event::stuck:
		{
			const auto &e{b.point_edges[std::min(b.point_index, b.point_edges.size() - 1)]};
			if (e.second != NAV_NO_SIDE)
				b.penalties.add(e.first, e.second, 200);
			const double side{b.rng.uniform() < 0.5 ? -1.0 : 1.0};
			b.recover_dir = normalized(frame.f * -0.7 + frame.r * (0.7 * side) + frame.u * b.rng.uniform(-0.3, 0.3));
			break;
		}
		case stuck_event::give_up:
			if (collecting && b.collect_pickup)
				b.pickup_ignore[*b.collect_pickup] = tick + BOT_COLLECT_UNREACHABLE_TICKS;
			b.goal = sim_goal::none;
			b.clear_path();
			return {};
	}
	if (last && dist < (collecting ? 2.0 : reach))
	{
		if (b.goal == sim_goal::hunt && b.target)
			b.memory[*b.target] = {};
		if (collecting && b.collect_pickup)
			b.pickup_ignore[*b.collect_pickup] = tick + BOT_COLLECT_IGNORE_TICKS;
		b.goal = sim_goal::none;
		b.clear_path();
		return {};
	}
	return normalized(to) * speed;
}

/* bot.cpp's perceive. */
void sim::perceive(sim_bot &b)
{
	const auto pos{b.s.pos};
	const auto frame{b.s.orient};
	if (b.seg < b.visited.size())
		b.visited[b.seg] = tick + 1;
	for (unsigned i{}; i != SHIPS; ++i)
	{
		b.visible_now[i] = false;
		if (i == b.id)
			continue;
		if (!ship_alive(i))
		{
			b.memory[i] = {};
			continue;
		}
		const auto tpos{ship_pos(i)};
		const auto to{tpos - pos};
		const double dist{length(to)};
		const bool revenge{b.last_attacker == i && tick - b.attacked_tick < BOT_REVENGE_TICKS};
		if (dist > b.sk.awareness && !revenge)
			continue;
		if (!revenge && !in_field_of_view(frame.f, to, b.sk.fov_half_deg))
			continue;
		if (!w.clear(b.seg, pos, tpos))
			continue;
		b.visible_now[i] = true;
		b.memory[i] = {true, tpos, ship_vel(i), static_cast<uint16_t>(ship_seg(i)), tick};
	}
	percept p;
	b.shot_clear = false;
	if (b.target)
	{
		const auto t{*b.target};
		p.target = t;
		p.visible = b.visible_now[t];
		p.pos = b.memory[t].pos;
		p.vel = b.memory[t].vel;
		/* The line of fire: the line of sight, and from both guns. */
		if (p.visible)
			b.shot_clear = w.clear_wide(b.seg, pos, ship_pos(t), 1.5);
	}
	b.seen.push(p);
	if (!b.points.empty())
	{
		const auto reachable{[&](const std::size_t k) {
			return w.clear_wide(b.seg, pos, b.points[k], SHIP_SIZE * 2 / 3);
		}};
		b.steer_index = opt.before ? pull_string(b.point_index, b.points.size(), 4, reachable) : pull_string_ahead(b.point_index, b.steer_index, b.points.size(), reachable);
	}
	/* Wall avoidance. */
	const auto vel{b.s.vel};
	const double speed{length(vel)};
	if (speed > 5)
	{
		const double probe_time{!opt.before && b.mode == move_mode::fight_keys ? 0.25 : 0.4};
		const auto dir{vel * (1 / speed)};
		const auto r{w.trace(b.seg, pos, dir, speed * probe_time + SHIP_SIZE / 2)};
		if (r.wall)
		{
			const double hit_distance{std::max(0.0, r.distance - SHIP_SIZE / 2)};
			const bool following{opt.before ? !(p.visible && b.shot_clear) : follows_path(b.mode)};
			const bool bend{following && b.steer_index < b.points.size() && wall_hit_is_bend(pos, vel, b.points[b.steer_index], hit_distance, BOT_BEND_MAX_ANGLE)};
			const auto normal{from_geo(r.normal)};
			const double into{-dot(vel, normal)};
			if (!bend && into > 0)
			{
				const double nearness{1 - std::clamp(hit_distance / (speed * probe_time), 0.0, 1.0)};
				b.avoid = normal * (into * (0.5 + nearness) + max_speed() * 0.1);
				b.avoid_until = tick + PERCEPTION_DIVISOR * 2;
			}
		}
	}
	/* The room along the ship's axes for the juke. */
	if (!opt.before && b.mode == move_mode::fight_keys)
		b.room = {
			.left = w.room(b.seg, pos, -frame.r, JUKE_WALL_ROOM),
			.right = w.room(b.seg, pos, frame.r, JUKE_WALL_ROOM),
			.down = w.room(b.seg, pos, -frame.u, JUKE_WALL_ROOM),
			.up = w.room(b.seg, pos, frame.u, JUKE_WALL_ROOM),
			.back = w.room(b.seg, pos, -frame.f, JUKE_WALL_ROOM),
		};
	else
		b.room = {};
	/* The dodge. */
	const double dodge_prob{effective_dodge(b.sk, b.st)};
	if (dodge_prob > 0 && tick >= b.dodge_until)
	{
		const double radius{SHIP_SIZE + 3};
		for (const auto &sh : shots)
		{
			if (sh.owner == b.id)
				continue;
			const auto rel{sh.pos - pos};
			if (length(rel) > BOT_DODGE_SCAN)
				continue;
			const auto away{dodge_direction(rel, sh.vel - vel, BOT_DODGE_HORIZON, dodge_radius(radius, false), frame.r)};
			if (!away)
				continue;
			if (dodge_roll(b.dodge_salt, sh.sig) >= dodge_chance(dodge_prob, false))
				continue;
			b.dodge_dir = *away;
			b.dodge_from = tick + ticks_from_ms(b.sk.reaction_ms) / 2;
			b.dodge_until = b.dodge_from + BOT_DODGE_TICKS;
			break;
		}
	}
}

/* bot.cpp's think, with what the simulation has: the target, the
 * pickups (all known), the goal.
 */
void sim::think(sim_bot &b)
{
	const auto pos{b.s.pos};
	const unsigned memory_ticks{ticks_from_ms(effective_memory_ms(b.sk, b.st))};
	std::array<target_candidate, SHIPS> cand{};
	unsigned n{};
	for (unsigned i{}; i != SHIPS; ++i)
	{
		if (i == b.id || !b.memory[i].valid)
			continue;
		auto &c{cand[n++]};
		c.id = static_cast<uint8_t>(i);
		c.visible = b.visible_now[i];
		c.confidence = memory_confidence(b.memory[i], tick, memory_ticks);
		c.distance = distance(pos, b.memory[i].pos);
		c.damaged_me_recently = b.last_attacker == i && tick - b.attacked_tick < BOT_REVENGE_TICKS;
		c.low_shields = i && bots[i - 1].shields < 30;
	}
	b.target = choose_target(std::span(cand.data(), n), b.target, b.sk.awareness);
	double target_score_v{};
	bool target_visible{};
	if (b.target)
	{
		for (unsigned k{}; k != n; ++k)
			if (cand[k].id == *b.target)
				target_score_v = target_score(cand[k], b.sk.awareness);
		target_visible = b.visible_now[*b.target];
	}
	b.dist.compute(w.graph, static_cast<uint32_t>(b.seg), [](uint32_t, const nav_edge &) {
		return true;
	}, [&b](const uint32_t from, const nav_edge &e) {
		return b.penalties.cost(from, e.side);
	}, BOT_DISTANCE_MAX_COST, BOT_DISTANCE_NODE_LIMIT);
	resource_view res;
	res.shields = b.shields;
	res.energy = 100;
	res.afterburner = b.has_afterburner;
	const auto value_of{[this, &res](const item k) {
		/* Before section 9.15 the afterburner was worth 2. */
		if (opt.before && k == item::afterburner)
			return res.afterburner ? 0.0 : 2.0;
		return item_value({k}, res);
	}};
	/* The best pickup (best_collect) and the best near one (best_grab). */
	double collect_u{}, collect_path{}, grab_path{}, grab_value{}, grab_score{};
	bool collect_upgrade{}, grab_shields{};
	std::optional<std::size_t> collect_i, grab_i;
	for (std::size_t i{}; i != pickups.size(); ++i)
	{
		const auto &pk{pickups[i]};
		if (!pk.present(t) || tick < b.pickup_ignore[i])
			continue;
		const auto cost{b.dist.cost(static_cast<uint32_t>(pk.seg))};
		if (!cost)
			continue;
		const double value{value_of(pk.kind)};
		if (value <= 0)
			continue;
		if (const double u{collect_utility(value, *cost)}; u > collect_u)
		{
			collect_u = u;
			collect_path = *cost;
			collect_i = i;
			collect_upgrade = collect_in_fight({pk.kind}, res);
		}
		const double straight{distance(pos, pk.pos)};
		const double path{pk.seg == b.seg ? straight : std::min(*cost, *cost + straight)};
		if (grab_candidate(value, straight, path))
			if (const double sc{grab_rank(value, path)}; sc > grab_score)
			{
				grab_score = sc;
				grab_path = path;
				grab_value = value;
				grab_shields = pk.kind == item::shield;
				grab_i = i;
			}
	}
	const bool attacked{b.last_attacker < SHIPS && tick - b.attacked_tick < BOT_REVENGE_TICKS};
	const double retreat_shields{style_retreat_shields(b.st, 1)};
	const goal_inputs gin{
		.has_target = b.target.has_value(),
		.target_visible = target_visible,
		.target_score = target_score_v,
		.threatened = b.target.has_value() || attacked,
		.shields = b.shields,
		.invulnerable = false,
		.collect = collect_u,
		.collect_path = collect_path,
		.collect_upgrade = collect_upgrade,
		.grab = grab_i.has_value(),
		.grab_shields = grab_shields,
		.grab_value = grab_value,
		.grab_invulnerability = false,
		.grab_path = grab_path,
		.armed = armed_level::none,
		.weak = false,
		.seek = 0,
		.phase_engage = 1,
		.phase_collect = 0,
		.third_party = 1,
		.refuel = 0,
		.retreat_shields = retreat_shields,
		.engage_weight = b.st.engage_weight * style_engage_factor(b.st, 1),
		.collect_weight = b.st.collect_weight,
		.collector = false,
		.detour_scale = b.tune.grab_detour_scale,
		.pursuing = false,
		.power = 0,
		.power_fight = 0,
		.power_invulnerability = false,
		.kill_imminent = false,
		.power_current = false,
		.current = b.chosen,
	};
	const auto goal{choose_goal(gin)};
	const auto u{goal_utility(gin)};
	b.chosen = goal;
	if (goal == goal_kind::collect && u.collect_from == collect_source::grab && grab_i)
		collect_i = grab_i;
	const bool replan_due{b.points.empty() || tick >= b.next_plan_tick};
	switch (goal)
	{
		case goal_kind::engage:
		case goal_kind::hunt:
			if (b.target)
			{
				const auto &m{b.memory[*b.target]};
				const bool moved{b.goal != sim_goal::hunt || b.goal_seg != m.segment};
				if (replan_due || (moved && tick - b.last_plan_tick >= BOT_HUNT_REPLAN_TICKS))
					set_goal(b, sim_goal::hunt, m.segment, m.pos);
				return;
			}
			break;
		case goal_kind::collect:
			if (collect_i)
			{
				const bool other{b.goal != sim_goal::collect || b.collect_pickup != collect_i};
				b.collect_pickup = collect_i;
				if (other || replan_due)
					set_goal(b, sim_goal::collect, static_cast<uint32_t>(pickups[*collect_i].seg), pickups[*collect_i].pos);
				return;
			}
			break;
		case goal_kind::retreat:
		{
			if (b.goal == sim_goal::retreat && !b.points.empty())
			{
				if (tick >= b.next_plan_tick)
					plan_path(b, b.goal_seg, std::nullopt);
				return;
			}
			bvec threat{pos};
			if (b.target)
				threat = b.memory[*b.target].pos;
			else if (attacked && b.memory[b.last_attacker].valid)
				threat = b.memory[b.last_attacker].pos;
			/* A shield pickup away from the threat, else the furthest
			 * of a few places for the least flying.
			 */
			std::optional<std::size_t> best_pk;
			double best_u{0};
			for (std::size_t i{}; i != pickups.size(); ++i)
			{
				const auto &pk{pickups[i]};
				if (pk.kind != item::shield || !pk.present(t) || tick < b.pickup_ignore[i])
					continue;
				const auto cost{b.dist.cost(static_cast<uint32_t>(pk.seg))};
				if (!cost)
					continue;
				const double uu{collect_utility(value_of(pk.kind), *cost) * retreat_direction_factor(pos, pk.pos, threat)};
				if (uu > best_u)
				{
					best_u = uu;
					best_pk = i;
				}
			}
			if (best_pk && best_u > 0.3)
			{
				b.collect_pickup = best_pk;
				set_goal(b, sim_goal::retreat, static_cast<uint32_t>(pickups[*best_pk].seg), pickups[*best_pk].pos);
				return;
			}
			std::optional<uint32_t> place;
			double best_flee{-1e9};
			const auto nseg{static_cast<uint32_t>(w.graph.size())};
			for (unsigned i{}; i != BOT_FLEE_TRIES; ++i)
			{
				const uint32_t s{b.rng.below(nseg)};
				const auto cost{b.dist.cost(s)};
				if (!cost || *cost < 80)
					continue;
				const double f{flee_score(w.graph.position(s), threat, *cost)};
				if (f > best_flee)
				{
					best_flee = f;
					place = s;
				}
			}
			if (place)
			{
				set_goal(b, sim_goal::retreat, *place, std::nullopt);
				return;
			}
			break;
		}
		case goal_kind::refuel:
		case goal_kind::roam:
			break;
	}
	if (b.goal != sim_goal::roam)
		b.goal = sim_goal::none;
	if (b.goal == sim_goal::none || b.points.empty())
		choose_roam_goal(b);
}

/* bot.cpp's decide_afterburner. */
void sim::decide_afterburner(sim_bot &b, const bvec &wanted)
{
	const auto pos{b.s.pos};
	const auto frame{b.s.orient};
	bool chasing_far{};
	if (b.target && b.goal == sim_goal::hunt && b.memory[*b.target].valid)
		chasing_far = distance(pos, b.memory[*b.target].pos) > b.st.burn_chase_distance;
	bool long_straight{};
	if ((b.goal == sim_goal::roam || b.goal == sim_goal::collect) && b.steer_index < b.points.size())
		long_straight = (opt.before ? distance(pos, b.points[b.steer_index]) : straight_ahead(b.points, b.steer_index, pos)) > BOT_LONG_STRAIGHT;
	if (long_straight)
	{
		if (tick >= b.roam_roll_at)
		{
			b.roam_burn_ok = b.tune.roam_burn >= 1 || b.rng.uniform() < b.tune.roam_burn;
			b.roam_roll_at = tick + FLEE_ROLL_TICKS;
		}
		long_straight = b.roam_burn_ok;
	}
	const bool dodging{tick >= b.dodge_from && tick < b.dodge_until};
	const bool aligned{length(wanted) > max_speed() * 0.5 && angle_between(frame.f, wanted) < radians(25)};
	b.burning = !b.stuck.recovering() && want_afterburner({
		.have = b.has_afterburner,
		.charge = b.charge,
		.use = afterburner_of(b.skill_level),
		.chasing_far = chasing_far,
		.retreating = b.goal == sim_goal::retreat && b.flee_burn,
		.dodging = dodging,
		.long_straight = long_straight,
		.turn_boost = b.turning.phase == turn_phase::boost && b.turning.burn,
		.aligned = aligned,
		.burning = b.burning,
	});
}

/* bot.cpp's bot_tick: the tactics of a tick and the movement. */
void sim::brain_tick(sim_bot &b)
{
	const auto &sk{b.sk};
	if (layer_due(tick, PERCEPTION_DIVISOR, b.id))
		perceive(b);
	if (layer_due(tick, STRATEGY_DIVISOR, b.id))
		think(b);
	b.aim.update(b.rng, radians(sk.aim_sigma_deg), ticks_from_ms(sk.aim_drift_ms));
	b.lead.update(b.rng, sk.lead, ticks_from_ms(sk.aim_drift_ms));
	const double range_lo{b.tune.range_lo * b.st.range_scale}, range_hi{b.tune.range_hi * b.st.range_scale};
	b.juke.update(b.rng, ticks_from_ms(sk.strafe_min_ms), ticks_from_ms(sk.strafe_max_ms), range_lo, range_hi, sk.strafe_vertical);
	if (b.goal == sim_goal::retreat)
	{
		if (!b.fleeing || tick >= b.flee_roll_at)
		{
			b.flee_turned = b.rng.uniform() < FLEE_TURNED_SHARE;
			b.flee_burn = b.rng.uniform() < b.tune.flee_burn;
			b.flee_roll_at = tick + FLEE_ROLL_TICKS;
		}
		b.fleeing = true;
	}
	else
		b.fleeing = b.flee_turned = b.flee_burn = false;
	const auto pos{b.s.pos};
	const auto vel{b.s.vel};
	const auto frame{b.s.orient};
	const double vmax{max_speed()};
	const unsigned reaction_ticks{ticks_from_ms(sk.reaction_ms)};
	const auto *const p{b.seen.delayed(reaction_ticks / PERCEPTION_DIVISOR)};
	b.fire = false;
	if (b.stuck.tick_recovery() && b.goal != sim_goal::none)
		plan_path(b, b.goal_seg, std::nullopt);
	bvec wanted;
	bool use_keys{};
	thrust_keys keys;
	bool aim_set{};
	const bool engaged_now{p && p->visible && b.target && *b.target == p->target && b.visible_now[p->target]};
	const uint32_t fight_ticks{ticks_from_ms(FIGHT_KEYS_MS)};
	const bool in_fight{!opt.before && (engaged_now ||
		(b.target && b.memory[*b.target].valid && tick - b.memory[*b.target].tick < fight_ticks) ||
		(b.last_attacker < SHIPS && tick - b.attacked_tick < fight_ticks))};
	if (!in_fight)
		b.hold.release();
	auto mode{move_mode::path};
	const bool path_goal{b.goal == sim_goal::collect || b.goal == sim_goal::retreat};
	if (engaged_now)
	{
		const double delay{reaction_ticks / static_cast<double>(BOT_TICK_RATE)};
		const auto est{p->pos + p->vel * delay};
		const auto aim{aim_point(pos, est, p->vel, SHOT_SPEED, b.lead.factor())};
		b.face_dir = apply_aim_offset(aim - pos, frame.u, b.aim.yaw(), b.aim.pitch());
		b.face_rate = line_of_sight_rate(aim - pos, p->vel - vel);
		b.aim_dir = b.face_dir;
		aim_set = true;
		const auto to{est - pos};
		const double dist{length(to)};
		bool combat{(opt.before ? b.shot_clear : b.blocked.fight_on(b.shot_clear, tick)) && !path_goal};
		if (!opt.before)
			combat = b.hold.update(combat, tick);
		if (combat)
		{
			b.stuck.cancel_recovery();
			const double band{range_hi - range_lo};
			const double close{effective_close_speed(b.st)};
			const double forward{b.approach.update(dist, b.juke.range(), band) * close};
			/* No heavy missiles here: no standoff, so no "no back" (bot.cpp). */
			if (!opt.before)
				juke_turn_from_walls(b.juke, b.room);
			keys = keys_off_walls(fight_keys(b.juke, forward, effective_strafe_speed(sk, b.st), !opt.before && !b.shot_clear), b.room);
			use_keys = true;
			mode = move_mode::fight_keys;
		}
		else
			wanted = follow_path(b, !(b.fleeing && b.flee_turned && b.goal == sim_goal::retreat));
		const double err{angle_between(frame.f, b.face_dir)};
		const bool free_move{combat};
		const auto turn{free_move ? b.turning.update(err, tick, dist, range_lo, b.rng, b.tune.turns) : turn_phase::none};
		if (!free_move)
		{
			b.turning.reset();
			b.slide.reset();
		}
		else
		{
			bool reversing{};
			if (turn == turn_phase::reversing || turn == turn_phase::boost)
			{
				const auto v{turn_round_velocity(turn, {}, to, vel, frame.r * static_cast<double>(b.heading_pref), vmax, b.tune.turns.reverse_speed)};
				if (turn != turn_phase::reversing || reverse_turn_has_room(w.room(b.seg, pos, normalized(v), REVERSE_TURN_CLEARANCE)))
				{
					wanted = v;
					use_keys = false;
					reversing = true;
					mode = move_mode::turn_keys;
				}
			}
			const double slide_err{turn == turn_phase::sliding || (turn == turn_phase::reversing && !reversing) ? std::max(err, SLIDE_START + 0.01) : err};
			if (const int side{b.slide.update(reversing ? 0 : slide_err, dot(vel, frame.r), b.heading_pref)}; side && !reversing)
			{
				keys = slide_keys(side);
				use_keys = true;
				mode = move_mode::slide;
			}
		}
		const double aim_err{angle_between(frame.f, b.aim_dir)};
		const auto rel_vel{p->vel - vel};
		const auto los{normalized(to)};
		const double lateral{length(rel_vel - los * dot(rel_vel, los))};
		b.fire = should_fire(aim_err, fire_cone_with_spread(radians(sk.fire_cone_deg), 0), b.shot_clear, dist, 400) &&
			long_shot_worthwhile(dist, SHOT_SPEED, lateral, radians(sk.aim_sigma_deg), SHIP_SIZE);
	}
	else
	{
		b.slide.reset();
		b.blocked.reset();
		wanted = follow_path(b, false);
		const auto *const m{b.target && b.memory[*b.target].valid ? &b.memory[*b.target] : nullptr};
		if (in_fight && m && b.hold.update(false, tick) && !b.stuck.recovering())
		{
			const auto est{m->pos + m->vel * ((tick - m->tick) / static_cast<double>(BOT_TICK_RATE))};
			const auto to{est - pos};
			const double dist{length(to)};
			b.face_dir = normalized(to);
			b.face_rate = {};
			if (b.turning.phase == turn_phase::boost)
				b.turning.reset();
			const double forward{b.approach.update(dist, b.juke.range(), range_hi - range_lo) * effective_close_speed(b.st)};
			juke_turn_from_walls(b.juke, b.room);
			keys = keys_off_walls(fight_keys(b.juke, forward, effective_strafe_speed(sk, b.st)), b.room);
			use_keys = true;
			mode = move_mode::fight_keys;
		}
		else if (b.turning.phase == turn_phase::boost)
			b.turning.reset();
	}
	const bool dodging{tick >= b.dodge_from && tick < b.dodge_until};
	const bool avoiding{tick < b.avoid_until};
	std::array<bool, 2> immediate{};
	if (opt.before)
	{
		if (b.stuck.recovering())
		{
			wanted = b.recover_dir * vmax;
			use_keys = false;
			mode = move_mode::recover;
		}
		else if (dodging && !use_keys)
			wanted += b.dodge_dir * vmax;
		if (avoiding && !use_keys)
			wanted += b.avoid;
		if (use_keys)
		{
			auto cmd{frame.to_world(keys.local())};
			if (dodging)
				cmd += b.dodge_dir;
			if (avoiding)
				cmd += b.avoid * (3 / vmax);
			b.move_cmd = cmd;
			/* PR #63 added the dodge and the push off a wall to the
			 * keys: no keys any more, as the recordings read them.
			 */
			if (dodging || avoiding)
				mode = move_mode::path;
		}
		else
		{
			b.move_cmd = velocity_command(wanted, vel, vmax);
			if (mode == move_mode::turn_keys)
				mode = move_mode::path;
		}
		immediate.fill(dodging || b.stuck.recovering() || avoiding);
		/* PR #63's modes: the velocity controller is not keys. */
		if (!use_keys && mode != move_mode::recover)
			mode = move_mode::path;
	}
	else
	{
		if (b.stuck.recovering())
		{
			wanted = b.recover_dir * vmax;
			use_keys = false;
			mode = move_mode::recover;
		}
		else if (!use_keys && in_fight && mode != move_mode::duck)
		{
			keys = b.pkeys.update(frame.to_local(path_key_command(wanted, vel, vmax)), tick, !aim_set || (b.fleeing && b.flee_turned));
			use_keys = true;
			if (mode != move_mode::turn_keys)
				mode = move_mode::path_keys;
		}
		if (mode != move_mode::path_keys && mode != move_mode::turn_keys)
			b.pkeys.reset();
		if (use_keys)
		{
			if (dodging)
				keys = dodge_key(keys, frame.to_local(b.dodge_dir));
			if (avoiding)
			{
				const auto a{avoid_keys(keys, frame.to_local(b.avoid), vmax)};
				keys = a.keys;
				immediate = a.immediate;
			}
			b.move_cmd = frame.to_world(keys.local());
		}
		else
		{
			if (dodging && mode != move_mode::recover)
				wanted += b.dodge_dir * vmax;
			if (avoiding)
				wanted += b.avoid;
			b.move_cmd = velocity_command(wanted, vel, vmax);
			immediate.fill(dodging || b.stuck.recovering() || avoiding);
		}
	}
	b.move_cmd = frame.to_world(b.lateral.apply(frame.to_local(b.move_cmd), tick, immediate));
	b.mode = mode;
	if (!aim_set)
		b.aim_dir = b.face_dir;
	decide_afterburner(b, use_keys ? (opt.before ? bvec{} : mode != move_mode::path_keys && mode != move_mode::turn_keys ? frame.to_world(keys.local()) * vmax : wanted) : wanted);
}

void sim::fire_shot(const unsigned owner, const bvec &from, const bvec &dir, const std::size_t seg)
{
	/* `from` is the gun, a little ahead of the ship's centre (`seg` its
	 * segment): a gun in the wall fires nothing.
	 */
	const auto centre{ship_pos(owner)};
	if (!w.clear(seg, centre, from))
		return;
	shot s;
	s.pos = from;
	s.vel = normalized(dir) * SHOT_SPEED;
	s.seg = w.segment_at(seg, centre, from);
	s.owner = owner;
	s.life = SHOT_LIFE;
	s.sig = next_sig++;
	if (!next_sig)
		next_sig = 1;
	shots.push_back(s);
	++fired;
}

void sim::damage(const unsigned victim, const unsigned attacker, const double amount)
{
	hit_at[victim][attacker] = t;
	if (!victim)
		return;
	auto &b{bots[victim - 1]};
	if (!b.alive)
		return;
	b.shields -= amount;
	b.last_attacker = static_cast<uint8_t>(attacker);
	b.attacked_tick = tick;
	if (b.shields > 0)
		return;
	b.alive = false;
	b.respawn_at = t + RESPAWN_S;
	++b.n.deaths;
	if (attacker)
		++bots[attacker - 1].n.kills;
	/* The afterburner it carried comes back to the level. */
	if (b.has_afterburner)
		for (auto &pk : pickups)
			if (pk.kind == item::afterburner && !pk.present(t))
			{
				pk.back_at = t + 5;
				break;
			}
	b.has_afterburner = false;
}

void sim::move_shots()
{
	const double dt{1 / FPS};
	for (auto &s : shots)
	{
		const double l{SHOT_SPEED * dt};
		const auto dir{s.vel * (1 / SHOT_SPEED)};
		/* A ship within its size of the shot's step. */
		for (unsigned j{}; j != SHIPS && s.life > 0; ++j)
		{
			if (j == s.owner || !ship_alive(j))
				continue;
			const auto rel{ship_pos(j) - s.pos};
			const double along{std::clamp(dot(rel, dir), 0.0, l)};
			if (length(rel - dir * along) < SHOT_HIT_RADIUS)
			{
				damage(j, s.owner, SHOT_DAMAGE);
				++hits;
				if (!s.owner)
					++enemy_hits;
				s.life = 0;
			}
		}
		if (s.life <= 0)
			continue;
		const auto r{w.trace(s.seg, s.pos, dir, l)};
		if (r.wall)
		{
			s.life = 0;
			++walled;
			continue;
		}
		s.pos += dir * l;
		s.seg = r.segment;
		s.life -= dt;
	}
	shots.erase(std::remove_if(shots.begin(), shots.end(), [](const shot &s) {
		return s.life <= 0;
	}), shots.end());
}

void sim::touch_pickups(sim_bot &b)
{
	for (auto &pk : pickups)
	{
		if (!pk.present(t) || distance(pk.pos, b.s.pos) > PICKUP_REACH + SHIP_SIZE)
			continue;
		switch (pk.kind)
		{
			case item::shield:
				if (b.shields >= 200)
					continue;
				b.shields = std::min(200.0, b.shields + 18);
				++b.n.shield_pickups;
				break;
			case item::afterburner:
				if (b.has_afterburner)
					continue;
				b.has_afterburner = true;
				++b.n.afterburner_pickups;
				break;
			default:
				break;
		}
		pk.back_at = t + PICKUP_RESPAWN_S;
		/* Taken: no goal any more (bot_touch_powerup). */
		if (b.goal == sim_goal::collect || b.goal == sim_goal::retreat)
			if (b.collect_pickup && &pickups[*b.collect_pickup] == &pk)
			{
				if (b.goal == sim_goal::collect)
					b.goal = sim_goal::none;
				b.collect_pickup.reset();
			}
	}
}

/* The enemy: the track's place at this time, or the scripted flight. */
void sim::move_enemy()
{
	const double dt{1 / FPS};
	if (track)
	{
		const auto &pts{track->pts};
		while (track_index + 1 < pts.size() && pts[track_index + 1].t <= t)
			++track_index;
		const auto &a{pts[track_index]};
		const auto &c{pts[std::min(track_index + 1, pts.size() - 1)]};
		const double span{c.t - a.t};
		const double k{span > 1e-6 ? std::clamp((t - a.t) / span, 0.0, 1.0) : 0.0};
		enemy.alive = a.alive && a.seg < w.mesh.segments.size();
		enemy.pos = a.pos + (c.pos - a.pos) * k;
		enemy.vel = a.vel + (c.vel - a.vel) * k;
		enemy.orient = a.orient;
		/* The segment of the place between the two samples. */
		enemy.seg = enemy.alive ? w.segment_at(a.seg, a.pos, enemy.pos) : a.seg;
		/* His shots, when he fired: at the bot nearest his nose within
		 * ENEMY_AIM_CONE (a human aims at what he shoots at, and the
		 * bots here are not where the recorded ones were), led, with
		 * a human's error; else along his nose.
		 */
		while (shot_index < track->shots.size() && track->shots[shot_index] <= t)
		{
			if (enemy.alive)
			{
				auto dir{enemy.orient.f};
				double best{std::cos(ENEMY_AIM_CONE)};
				for (unsigned j{1}; j != SHIPS; ++j)
				{
					if (!ship_alive(j))
						continue;
					const auto to{ship_pos(j) - enemy.pos};
					const double d{length(to)};
					if (d < 1 || d > ENEMY_AIM_RANGE || dot(to, enemy.orient.f) < best * d || !w.clear(enemy.seg, enemy.pos, ship_pos(j)))
						continue;
					best = dot(to, enemy.orient.f) / d;
					const auto aim{aim_point(enemy.pos, ship_pos(j), ship_vel(j), SHOT_SPEED, 1)};
					dir = apply_aim_offset(aim - enemy.pos, enemy.orient.u, world_rng.uniform(-1, 1) * radians(ENEMY_AIM_ERROR_DEG), world_rng.uniform(-1, 1) * radians(ENEMY_AIM_ERROR_DEG));
				}
				fire_shot(0, enemy.pos + dir * SHIP_SIZE, dir, enemy.seg);
			}
			++shot_index;
		}
		return;
	}
	/* Scripted: a path to a random place after another at a human's
	 * speed, strafing across it, facing the nearest bot in sight and
	 * shooting at it.
	 */
	if (enemy.points.empty() || enemy.index >= enemy.points.size())
	{
		enemy.points.clear();
		enemy.index = 0;
		const auto goal{random_segment(world_rng)};
		if (w.search.find(w.graph, static_cast<uint32_t>(enemy.seg), goal, [](uint32_t, const nav_edge &) {
			return true;
		}, [](uint32_t, const nav_edge &) {
			return 0.0;
		}, BOT_NAV_NODE_LIMIT, w.path))
			for (const auto &st : w.path.steps)
				enemy.points.push_back(w.graph.position(st.node));
		if (enemy.points.empty())
			enemy.points.push_back(w.graph.position(goal));
	}
	const auto to{enemy.points[enemy.index] - enemy.pos};
	if (length(to) < 6)
		++enemy.index;
	enemy.strafe_phase += dt * 2.3;
	const auto dir{normalized(to)};
	auto across{normalized(cross(dir, bvec{0, 1, 0}))};
	if (across == bvec{})
		across = {1, 0, 0};
	const auto want{dir * 52 + across * (25 * std::sin(enemy.strafe_phase))};
	enemy.vel += (want - enemy.vel) * (1 - std::exp(-3 * dt));
	const auto m{move_ship(w, enemy.seg, enemy.pos, enemy.vel * dt)};
	enemy.pos = m.pos;
	enemy.seg = m.seg;
	if (m.hit)
		enemy.vel -= m.normal * std::min(0.0, dot(enemy.vel, m.normal));
	std::optional<unsigned> seen;
	double best{1e18};
	for (unsigned j{1}; j != SHIPS; ++j)
	{
		if (!ship_alive(j))
			continue;
		const double d{distance(ship_pos(j), enemy.pos)};
		if (d < best && d < 500 && w.clear(enemy.seg, enemy.pos, ship_pos(j)))
		{
			best = d;
			seen = j;
		}
	}
	auto f{seen ? normalized(ship_pos(*seen) - enemy.pos) : dir};
	if (f == bvec{})
		f = {0, 0, 1};
	auto r{normalized(cross(bvec{0, 1, 0}, f))};
	if (r == bvec{})
		r = {1, 0, 0};
	enemy.orient = {r, cross(f, r), f};
	if (seen && t >= enemy.next_shot)
	{
		enemy.next_shot = t + SHOT_INTERVAL;
		fire_shot(0, enemy.pos + f * SHIP_SIZE, f + across * world_rng.uniform(-0.05, 0.05), enemy.seg);
	}
}

void sim::fly_bot(sim_bot &b)
{
	const double dt{1 / FPS};
	if (!b.alive)
	{
		if (t >= b.respawn_at)
			spawn(b);
		return;
	}
	brain_tick(b);
	const fix ft{to_fix(dt)};
	b.ctl = steer(b.s, b.face_dir, b.face_rate, b.move_cmd, lim, b.sk.turn_cap, b.heading_pref);
	b.burn_now = b.burning && b.ctl.forward > 0 && b.charge > 0;
	b.burn_scale = 1 + 2 * std::min(0.5, b.charge);
	const auto before_pos{b.s.pos};
	b.s.step(b.ctl, ft, b.burn_now ? b.burn_scale : 0);
	if (b.burn_now)
		b.charge = std::max(0.0, b.charge - 1 / (3 * FPS));
	else
		b.charge = std::min(1.0, b.charge + 1 / (8 * FPS));
	/* The walls. */
	const auto m{move_ship(w, b.seg, before_pos, b.s.pos - before_pos)};
	b.s.pos = m.pos;
	b.seg = m.seg;
	if (m.hit)
	{
		b.s.vel -= m.normal * std::min(0.0, dot(b.s.vel, m.normal));
		++b.n.wall_hits;
		if (b.goal == sim_goal::retreat)
			++b.n.retreat_wall_hits;
	}
	/* The bot's own shots. */
	if (b.fire && t >= b.next_shot)
	{
		b.next_shot = t + SHOT_INTERVAL;
		fire_shot(b.id, b.s.pos + b.s.orient.f * SHIP_SIZE, b.s.orient.f, b.seg);
	}
	touch_pickups(b);
	/* The counters. */
	auto &n{b.n};
	n.alive_s += dt;
	if (b.has_afterburner)
		n.afterburner_held_s += dt;
	if (b.mode == move_mode::recover)
		n.recover_s += dt;
	if (b.goal == sim_goal::retreat)
	{
		n.retreat_s += dt;
		if (b.burn_now)
			n.retreat_burn_s += dt;
	}
	bool fight{};
	for (unsigned j{}; j != SHIPS && !fight; ++j)
		if (j != b.id && ship_alive(j) && distance(ship_pos(j), b.s.pos) < 400 && w.clear(b.seg, b.s.pos, ship_pos(j)))
			fight = true;
	if (fight)
	{
		n.fight_s += dt;
		if (is_keys(b.mode))
			n.fight_keys_s += dt;
	}
	if (b.mode != b.last_mode)
	{
		if (b.last_mode != move_mode::none)
		{
			++n.mode_changes;
			n.spells.push_back(t - b.spell_start);
		}
		b.last_mode = b.mode;
		b.spell_start = t;
	}
}

/* One sample of each ship, as the recorder writes it (fill_context). */
void sim::record()
{
	const auto now{static_cast<std::uint32_t>(std::lround(t * 1000))};
	const auto rtick{static_cast<std::uint32_t>(std::lround(t * RECORD_RATE))};
	put(mr::encode(buf, mr::tick_record{rtick, now}));
	if (rtick % RECORD_RATE == 0)
		put(mr::encode(buf, mr::sync_record{now, 0x5151u, 9'000'000 + now, static_cast<std::uint8_t>(mr::sync_flag::clock_valid | mr::sync_flag::host)}));
	const auto q_pos{[](const bvec &v) {
		return std::array<std::int32_t, 3>{{static_cast<std::int32_t>(std::lround(v.x * 256)), static_cast<std::int32_t>(std::lround(v.y * 256)), static_cast<std::int32_t>(std::lround(v.z * 256))}};
	}};
	const auto q16{[](const bvec &v, const double scale) {
		const auto one{[scale](const double c) {
			return static_cast<std::int16_t>(std::clamp(std::lround(c * scale), -32767L, 32767L));
		}};
		return std::array<std::int16_t, 3>{{one(v.x), one(v.y), one(v.z)}};
	}};
	for (unsigned i{}; i != SHIPS; ++i)
	{
		mr::sample s;
		s.pid = static_cast<std::uint8_t>(i);
		const bool alive{ship_alive(i)};
		if (alive)
			s.flags |= mr::sample_flag::alive;
		s.segment = static_cast<std::uint16_t>(ship_seg(i));
		const auto pos{ship_pos(i)};
		const auto &o{ship_orient(i)};
		s.pos = q_pos(pos);
		s.quat = mr::quantise_quaternion(mr::quaternion_from_axes(to_mr(o.r), to_mr(o.u), to_mr(o.f)));
		s.vel = q16(ship_vel(i), 64);
		s.energy = 100;
		if (i)
		{
			const auto &b{bots[i - 1]};
			s.flags |= mr::sample_flag::controls;
			if (b.burn_now)
				s.flags |= mr::sample_flag::afterburner;
			s.flags2 = mr::sample_flag2::local | mr::sample_flag2::vitals_exact | mr::sample_flag2::afterburner_known | mr::sample_flag2::bot;
			const std::array<double, 6> c{{b.burn_now ? b.burn_scale : b.ctl.forward, b.ctl.sideways, b.ctl.vertical, b.ctl.pitch, b.ctl.heading, 0}};
			for (std::size_t k{}; k != 6; ++k)
				s.controls[k] = mr::quantise_control(c[k]);
			s.rotvel = q16({b.s.pitch_rate, b.s.heading_rate, b.s.bank_rate}, 4096);
			s.shields = static_cast<std::uint8_t>(std::clamp(b.shields, 0.0, 255.0));
			s.bot_known = true;
			s.bot_mode = static_cast<std::uint8_t>(alive ? b.mode : move_mode::none);
			s.bot_goal = static_cast<std::uint8_t>(b.goal);
		}
		else
			s.shields = 100;
		if (alive)
		{
			/* The nearest enemy in sight (of the four nearest), else the
			 * nearest; who aims at me; who hit me in the last 2 s.
			 */
			std::array<std::pair<double, unsigned>, SHIPS> order{};
			unsigned m{};
			for (unsigned j{}; j != SHIPS; ++j)
			{
				if (j == i)
					continue;
				if (t - hit_at[i][j] <= 2 && hit_at[i][j] > 0)
					s.attacked_mask |= static_cast<std::uint8_t>(1u << j);
				if (!ship_alive(j))
					continue;
				const auto d{ship_pos(j) - pos};
				const double dl{length(d)};
				order[m++] = {dl, j};
				if (dl > 0 && dl < 800 && -dot(ship_orient(j).f, d) > 0.9659258262890683 * dl && w.clear(ship_seg(i), pos, ship_pos(j)))
					s.aimed_at_mask |= static_cast<std::uint8_t>(1u << j);
			}
			std::sort(order.begin(), order.begin() + m);
			if (m)
			{
				unsigned chosen{order[0].second};
				bool sight{};
				for (unsigned k{}; k != std::min(m, 4u); ++k)
					if (w.clear(ship_seg(i), pos, ship_pos(order[k].second)))
					{
						chosen = order[k].second;
						sight = true;
						break;
					}
				const auto d{ship_pos(chosen) - pos};
				const double dl{length(d)};
				std::uint8_t ctx{mr::context_flag::kind_player};
				if (sight)
					ctx |= mr::context_flag::line_of_sight;
				if (dl > 0 && dot(o.f, d) > 0.8660254037844387 * dl)
					ctx |= mr::context_flag::in_my_cone;
				if (dl > 0 && -dot(ship_orient(chosen).f, d) > 0.8660254037844387 * dl)
					ctx |= mr::context_flag::me_in_its_cone;
				s.context = ctx;
				s.enemy_id = static_cast<std::uint16_t>(chosen);
				s.enemy_rel_pos = q16(d, 16);
				s.enemy_rel_vel = q16(ship_vel(chosen) - ship_vel(i), 64);
			}
		}
		put(mr::encode(buf, s));
	}
}

void sim::run(const double seconds)
{
	setup();
	const auto frames{static_cast<std::uint64_t>(seconds * FPS)};
	double next_record{0};
	for (std::uint64_t f{}; f != frames; ++f)
	{
		t = f / FPS;
		++tick;
		move_enemy();
		for (auto &b : bots)
			fly_bot(b);
		move_shots();
		if (t + 1e-9 >= next_record)
		{
			next_record += 1.0 / RECORD_RATE;
			record();
		}
	}
	put(mr::encode(buf, mr::event_record{mr::record_type::end, static_cast<std::uint32_t>(std::lround(t * 1000)), mr::PLAYER_NONE, mr::PLAYER_NONE, mr::end_reason::closed, 0, 0, 0}));
	flush();
}

/*
 * The measurements.
 */
struct map_result
{
	std::string name;
	bool before{};
	double alive_s{}, fight_s{}, keys_share{}, changes_per_min{}, spell_median{};
	double recover_share{}, retreat_wall_per_min{}, wall_per_min{};
	double afterburner_held_share{}, retreat_burn_share{};
	unsigned afterburner_pickups{}, shield_pickups{}, kills{}, deaths{};
	/* From the analysis, over the bots together. */
	double speed{}, reversals{}, vertical{}, thrust_across{}, speed_across{}, push_after_turn{}, ab_fleeing{}, analysed_keys{};
	double traces_per_s{};
	unsigned fired{}, hits{}, walled{}, enemy_hits{};
};

[[nodiscard]]
double median(std::vector<double> v)
{
	if (v.empty())
		return 0;
	std::sort(v.begin(), v.end());
	return v[v.size() / 2];
}

[[nodiscard]]
map_result fly_map(world &w, const enemy_track *const track, const std::string &name, const bool before)
{
	const double seconds{track ? track->length_s : SCRIPTED_S};
	auto s{std::make_unique<sim>(w, track, sim_options{before, 1})};
	w.traces = 0;
	s->run(seconds);
	map_result r;
	r.name = name;
	r.before = before;
	r.traces_per_s = w.traces / std::max(1.0, seconds);
	r.fired = s->fired;
	r.hits = s->hits;
	r.walled = s->walled;
	r.enemy_hits = s->enemy_hits;
	std::vector<double> spells;
	double changes{}, recover{}, retreat_walls{}, walls{}, held{}, retreat{}, retreat_burn{};
	for (const auto &b : s->bots)
	{
		const auto &n{b.n};
		r.alive_s += n.alive_s;
		r.fight_s += n.fight_s;
		r.keys_share += n.fight_keys_s;
		changes += n.mode_changes;
		recover += n.recover_s;
		walls += n.wall_hits;
		retreat_walls += n.retreat_wall_hits;
		held += n.afterburner_held_s;
		retreat += n.retreat_s;
		retreat_burn += n.retreat_burn_s;
		r.afterburner_pickups += n.afterburner_pickups;
		r.shield_pickups += n.shield_pickups;
		r.kills += n.kills;
		r.deaths += n.deaths;
		spells.insert(spells.end(), n.spells.begin(), n.spells.end());
	}
	r.keys_share = r.fight_s > 0 ? r.keys_share / r.fight_s : 0;
	r.changes_per_min = changes / std::max(1e-9, r.alive_s / 60);
	r.spell_median = median(spells);
	r.recover_share = recover / std::max(1e-9, r.alive_s);
	r.wall_per_min = walls / std::max(1e-9, r.alive_s / 60);
	r.retreat_wall_per_min = retreat_walls / std::max(1e-9, r.alive_s / 60);
	r.afterburner_held_share = held / std::max(1e-9, r.alive_s);
	r.retreat_burn_share = retreat_burn / std::max(1e-9, retreat);
	/* The analysis of the recording, every bot as one player (the
	 * bots' samples renamed to one callsign: analysed together).
	 */
	if (out_dir)
	{
		const std::string path{std::string{out_dir} + "/" + name + (before ? "-before" : "") + ".dmr"};
		if (std::FILE *const f{std::fopen(path.c_str(), "wb")})
		{
			std::fwrite(s->bytes.data(), 1, s->bytes.size(), f);
			std::fclose(f);
		}
	}
	auto rec{an::load_recording(s->bytes, name)};
	if (!rec)
	{
		std::fprintf(stderr, "test-bot-level-sim: the recording does not load\n");
		std::exit(1);
	}
	const auto result{an::analyse_recordings(std::span<const an::recording>(&*rec, 1), bot_skill::insane)};
	double weight{};
	for (const auto &pr : result.players)
	{
		const auto &st{pr.stats};
		if (!st.bot)
			continue;
		if (verbose)
			std::printf("%s\n", an::write_report(st, pr.profile).c_str());
		const double wt{st.fight_s};
		weight += wt;
		r.speed += st.speed.mean * wt;
		r.reversals += st.strafe_reversals_per_min * wt;
		r.vertical += st.strafe_vertical * wt;
		r.thrust_across += st.strafe_thrust * wt;
		r.speed_across += st.strafe_speed * wt;
		r.push_after_turn += st.turn_boost_share * wt;
		r.ab_fleeing += st.ab_situation_rate[1] * wt;
		r.analysed_keys += st.bot_keys_fight_share * wt;
	}
	if (weight > 0)
		for (auto *const v : {&r.speed, &r.reversals, &r.vertical, &r.thrust_across, &r.speed_across, &r.push_after_turn, &r.ab_fleeing, &r.analysed_keys})
			*v /= weight;
	return r;
}

void print_result(const map_result &r)
{
	std::printf("test-bot-level-sim: %-10s %-6s keys %3.0f%% (analysis %3.0f%%) of %5.1f min fight, %4.1f changes/min, spell %4.2f s | speed %4.1f rev/min %5.1f vert %.2f thrust across %3.0f%% speed across %3.0f%% push %3.0f%% ab flee %3.0f%% | stuck %4.1f%% walls %4.1f/min (retreat %4.1f/min) ab held %3.0f%% (%u pickups) retreat burn %3.0f%% | kills %u deaths %u shots %u hits %u (enemy %u) walls %u | %.0f rays/s\n",
		r.name.c_str(), r.before ? "before" : "now",
		100 * r.keys_share, 100 * r.analysed_keys, r.fight_s / 60, r.changes_per_min, r.spell_median,
		r.speed, r.reversals, r.vertical, 100 * r.thrust_across, 100 * r.speed_across, 100 * r.push_after_turn, 100 * r.ab_fleeing,
		100 * r.recover_share, r.wall_per_min, r.retreat_wall_per_min, 100 * r.afterburner_held_share, r.afterburner_pickups, 100 * r.retreat_burn_share,
		r.kills, r.deaths, r.fired, r.hits, r.enemy_hits, r.walled, r.traces_per_s);
}

struct check
{
	std::string name;
	double value, lo, hi;
};

/* The checks of a map: the movement now (section 9.15) against the
 * movement before it (`b`, flown on the same map and track).  The
 * human on these maps (the recordings of exp-31): speed 54-60, 36-52
 * strafe reversals a minute, vertical share 0.29-0.6, speed across
 * 77-81 %, the afterburner in 9-24 % of the flight away.  Absolute: the
 * keys' share of the fight time, the mode spells and changes, stuck,
 * walls touched while retreating, the afterburner pickups.  Against
 * before: faster, faster across, no more reversals and no more up and
 * down, the afterburner fleeing at least as often.  The margins hold
 * over the seeds and both maps of the playtests (a chaotic simulation:
 * another compiler changes a trajectory as another seed does).
 */
[[nodiscard]]
std::vector<check> checks_of(const map_result &r, const map_result &b)
{
	return {
		{r.name + " keys share of the fight time", r.keys_share, 0.7, 1},
		{r.name + " keys share (analysis)", r.analysed_keys, 0.7, 1},
		{r.name + " keys share - before", r.keys_share - b.keys_share, 0.15, 1},
		{r.name + " mode changes per minute", r.changes_per_min, 0, 40},
		{r.name + " mode changes per minute - before", r.changes_per_min - b.changes_per_min, -1e9, -15},
		{r.name + " median mode spell (s)", r.spell_median, 0.7, 1e9},
		{r.name + " speed - before", r.speed - b.speed, 2, 1e9},
		{r.name + " speed across - before", r.speed_across - b.speed_across, 0.03, 1e9},
		{r.name + " strafe reversals per minute - before", r.reversals - b.reversals, -1e9, 8},
		{r.name + " vertical share - before", r.vertical - b.vertical, -1e9, 0.05},
		{r.name + " afterburner fleeing - before", r.ab_fleeing - b.ab_fleeing, -0.03, 1},
		{r.name + " stuck share", r.recover_share, 0, 0.03},
		{r.name + " walls touched retreating per minute", r.retreat_wall_per_min, 0, 6},
		{r.name + " afterburner pickups", static_cast<double>(r.afterburner_pickups), 1, 1e9},
	};
}

struct map_arg
{
	std::string hog, level, track;
};

[[nodiscard]]
std::vector<std::string> split(const char *const s)
{
	std::vector<std::string> out;
	if (!s)
		return out;
	std::string cur;
	for (const char *p{s}; *p; ++p)
		if (*p == ';')
		{
			if (!cur.empty())
				out.push_back(cur);
			cur.clear();
		}
		else
			cur += *p;
	if (!cur.empty())
		out.push_back(cur);
	return out;
}

/* The level from the HOG: `level` by name, else the one with the
 * track's segment count, else the first.
 */
[[nodiscard]]
std::optional<geo::level> load_level(const map_arg &m, const enemy_track *const track, std::string &name)
{
	const auto data{mr::load_file(m.hog.c_str())};
	if (!data)
		return std::nullopt;
	const auto dir{geo::read_hog(*data)};
	if (!dir)
		return std::nullopt;
	std::optional<geo::level> first;
	std::string first_name;
	for (const auto &e : *dir)
	{
		const auto lname{geo::lower(e.name)};
		if (!(lname.ends_with(".rl2") || lname.ends_with(".rdl")))
			continue;
		if (!m.level.empty() && lname != geo::lower(m.level))
			continue;
		const auto bytes{geo::hog_file(*data, *dir, e.name)};
		if (!bytes)
			continue;
		auto l{geo::read_level(*bytes, e.name)};
		if (!l)
			continue;
		if (track && track->segments && l->segments.size() == track->segments)
		{
			name = e.name;
			return l;
		}
		if (!first)
		{
			first = std::move(l);
			first_name = e.name;
		}
	}
	name = first_name;
	return first;
}

}

int main(const int argc, char **const argv)
{
	std::vector<map_arg> maps;
	for (int i{1}; i < argc; ++i)
	{
		const std::string_view a{argv[i]};
		if (a == "-v")
			verbose = true;
		else if (a == "-o" && i + 1 < argc)
			out_dir = argv[++i];
		else if (a == "-hog" && i + 1 < argc)
			maps.push_back({argv[++i], {}, {}});
		else if (a == "-level" && i + 1 < argc && !maps.empty())
			maps.back().level = argv[++i];
		else if (a == "-track" && i + 1 < argc && !maps.empty())
			maps.back().track = argv[++i];
		else
		{
			std::fprintf(stderr, "usage: test-bot-level-sim [-v] [-o DIR] [-hog FILE [-level NAME] [-track FILE.dmr]]...\n");
			return 2;
		}
	}
	if (maps.empty())
	{
		const auto hogs{split(std::getenv("BOT_LEVEL_SIM_HOG"))};
		const auto tracks{split(std::getenv("BOT_LEVEL_SIM_TRACK"))};
		for (std::size_t i{}; i != hogs.size(); ++i)
			maps.push_back({hogs[i], {}, i < tracks.size() ? tracks[i] : std::string{}});
	}
	if (maps.empty())
	{
		std::puts("test-bot-level-sim: SKIP (no level: -hog FILE or BOT_LEVEL_SIM_HOG; the level data are not part of the source)");
		return 0;
	}
	unsigned failed{}, flown{};
	for (const auto &m : maps)
	{
		std::optional<enemy_track> track;
		if (!m.track.empty())
		{
			track = load_track(m.track.c_str());
			if (!track)
				std::printf("test-bot-level-sim: %s: no track (not readable, or no local human): a scripted enemy\n", m.track.c_str());
		}
		std::string level_name;
		auto level{load_level(m, track ? &*track : nullptr, level_name)};
		if (!level)
		{
			std::printf("test-bot-level-sim: SKIP %s (not readable, or no level in it)\n", m.hog.c_str());
			continue;
		}
		if (track && track->segments && level->segments.size() != track->segments)
		{
			std::printf("test-bot-level-sim: %s: the track's level has %zu segments, %s has %zu: a scripted enemy\n", m.track.c_str(), track->segments, level_name.c_str(), level->segments.size());
			track.reset();
		}
		auto w{std::make_unique<world>()};
		w->mesh = std::move(*level);
		build_graph(*w);
		std::printf("test-bot-level-sim: %s %s: %zu segments, %s, %.1f min\n", m.hog.c_str(), level_name.c_str(), w->mesh.segments.size(), track ? "the track's enemy" : "a scripted enemy", (track ? track->length_s : SCRIPTED_S) / 60);
		std::string name{level_name.substr(0, level_name.find('.'))};
		const auto b{fly_map(*w, track ? &*track : nullptr, name, true)};
		print_result(b);
		const auto r{fly_map(*w, track ? &*track : nullptr, name, false)};
		print_result(r);
		++flown;
		for (const auto &c : checks_of(r, b))
			if (!(c.value >= c.lo && c.value <= c.hi))
			{
				std::fprintf(stderr, "test-bot-level-sim: %s = %g, expected %g to %g\n", c.name.c_str(), c.value, c.lo, c.hi);
				++failed;
			}
	}
	if (failed)
		return 1;
	if (flown)
		std::puts("test-bot-level-sim: all checks passed");
	return 0;
}
