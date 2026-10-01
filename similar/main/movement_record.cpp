/*
 * This file is part of the DXX-Rebirth project <https://github.com/dxx-rebirth/dxx-rebirth/>.
 * It is copyright by its individual contributors, as recorded in the
 * project's Git history.  See COPYING.txt at the top level for license
 * terms and a link to the Git history.
 */
/*
 * Movement recording (-recordmoves, Documentation/movement-recording.md):
 * at a fixed rate of game time, every recorded player's pose, velocity,
 * controls, vitals and fight context; in between, the events.  One file
 * per game session in recordings/ of the PhysFS write directory, the
 * directory of gamelog.txt.
 *
 * Nothing is allocated per record: records are encoded into a buffer on
 * the stack and appended to a fixed chunk buffer, which is written and
 * flushed once per second of game time or when full.
 */

#include "dxxsconf.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <optional>
#include <string>

#include "movement_record.h"
#include "movement_record_format.h"
#include "args.h"
#include "bm.h"
#include "bot.h"
#include "console.h"
#include "controls.h"
#include "d_levelstate.h"
#include "d_underlying_value.h"
#include "fvi.h"
#include "game.h"
#include "gameseq.h"
#include "kconfig.h"
#include "laser.h"
#include "mission.h"
#include "multi.h"
#include "newdemo.h"
#include "object.h"
#include "physfsx.h"
#include "player.h"
#include "robot.h"
#include "segment.h"
#include "vers_id.h"
#include "weapon.h"
#if DXX_USE_MULTIPLAYER
#include "net_v2_game.h"
#endif

namespace dsx {

namespace {

namespace mr = ::dcx::movrec;

/* Recording stops at this size (a note in the console and an `end`
 * record): about 80 minutes of an 8 player game at 30 Hz.
 */
constexpr std::uint64_t MAX_FILE_BYTES{std::uint64_t{64} << 20};
constexpr std::uint32_t FLUSH_INTERVAL_MS{1000};
/* A player counts as attacked by another for this long after a hit. */
constexpr std::uint32_t ATTACK_MEMORY_MS{2000};
constexpr double COS_VIEW_CONE{0.8660254037844387};	/* 30 degrees */
constexpr double COS_AIM_CONE{0.9659258262890683};	/* 15 degrees */
constexpr double AIM_RANGE{800.0};
/* The nearest enemies tried for a line of sight, per player and tick. */
constexpr std::size_t ENEMY_CANDIDATES{4};

using vec3 = std::array<double, 3>;

[[nodiscard]]
vec3 to_d(const vms_vector &v)
{
	return {{v.x / 65536.0, v.y / 65536.0, v.z / 65536.0}};
}

[[nodiscard]]
double dot(const vec3 &a, const vec3 &b)
{
	return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

[[nodiscard]]
vec3 sub(const vec3 &a, const vec3 &b)
{
	return {{a[0] - b[0], a[1] - b[1], a[2] - b[2]}};
}

/* The difference of two fix vectors, quantised with `shift`; scaled
 * down as a whole when too long (the flag).
 */
[[nodiscard]]
std::pair<std::array<std::int16_t, 3>, bool> rel16(const vms_vector &a, const vms_vector &b, const int shift)
{
	return mr::scale_rel16({{
		std::int64_t{a.x} - std::int64_t{b.x},
		std::int64_t{a.y} - std::int64_t{b.y},
		std::int64_t{a.z} - std::int64_t{b.z},
	}}, shift);
}

struct player_track
{
	bool announced{};
	std::uint8_t pflags{};
	std::uint8_t team{};
	callsign_t callsign{};
	bool alive_known{};
	bool alive{};
	bool weapons_known{};
	std::uint8_t weapons{};
	/* Session time + 1 of the last hit by each player, 0 = none. */
	std::array<std::uint32_t, MAX_PLAYERS> hit_by_ms{};
};

/* Who holds a slot, across levels: a new occupant (another callsign, a
 * bot in place of a human, or a player who left and came back) starts
 * with a clean state, so that the old one's life, weapons and hits
 * do not turn into events of the new one (no `respawn` or `weapon`
 * event for the first sample of the newcomer, no "attacked by" from
 * hits the old occupant took or dealt).
 */
struct slot_occupant
{
	bool in_game{};
	bool bot{};
	callsign_t callsign{};
	/* The client's INPUT afterburner bit was 1 at least once: it is a
	 * client that reports the afterburner (clients before exp-25 always
	 * send 0, which is no information).
	 */
	bool afterburner_seen{};
	/* Host: the client shares its controls (-sharemoves): they arrived in
	 * its INPUT at least once.
	 */
	bool shares_controls{};
};

struct recorder
{
	/* Not RAII: a static destructor would run after PhysFS is gone.  The
	 * file is closed by movement_record_end_session; every chunk is
	 * already written when the program ends otherwise.
	 */
	PHYSFS_File *file{};
	bool failed{};
	bool capped{};
	mr::tick_scheduler sched;
	mr::chunk_builder chunk;
	std::uint32_t last_flush_ms{};
	std::uint64_t bytes{};
	bool level_known{};
	int level_num{};
	unsigned segments{};
	fix64 last_game_time{};
	const void *mission{};
	std::array<player_track, MAX_PLAYERS> players{};
	std::array<slot_occupant, MAX_PLAYERS> occupants{};
	std::uint32_t last_sync_ms{};
	bool sync_due{};
	/* Line of sight between two players this tick: 0 unknown, 1 clear,
	 * 2 blocked.
	 */
	std::array<std::array<std::uint8_t, MAX_PLAYERS>, MAX_PLAYERS> los{};
};

recorder R;

void close_file()
{
	if (R.file)
		PHYSFS_close(R.file);
	R.file = nullptr;
}

void write_bytes(const std::span<const std::uint8_t> bytes)
{
	if (!R.file || bytes.empty())
		return;
	if (PHYSFS_writeBytes(R.file, bytes.data(), bytes.size()) != static_cast<PHYSFS_sint64>(bytes.size()))
	{
		con_printf(CON_URGENT, "movement recording: write failed (%s), recording stopped", PHYSFS_getErrorByCode(PHYSFS_getLastErrorCode()));
		close_file();
		R.failed = true;
		return;
	}
	R.bytes += bytes.size();
}

void flush_chunk()
{
	R.last_flush_ms = R.sched.time_ms();
	if (R.chunk.empty())
		return;
	write_bytes(R.chunk.finish());
	R.chunk.reset();
	if (R.file)
		PHYSFS_flush(R.file);
}

void put_record(const std::span<const std::uint8_t> record);

void put_event(const mr::record_type type, const unsigned pid, const unsigned other, const unsigned kind, const unsigned id, const unsigned value, const unsigned flags)
{
	mr::record_buffer buf;
	put_record(mr::encode(buf, mr::event_record{
		.type = type,
		.time_ms = R.sched.time_ms(),
		.pid = static_cast<std::uint8_t>(pid),
		.other = static_cast<std::uint8_t>(other),
		.kind = static_cast<std::uint8_t>(kind),
		.id = static_cast<std::uint8_t>(id),
		.value = static_cast<std::uint16_t>(value),
		.flags = static_cast<std::uint8_t>(flags),
	}));
}

void put_record(const std::span<const std::uint8_t> record)
{
	if (!R.file || R.capped || record.empty())
		return;
	if (!R.chunk.fits(record.size()))
		flush_chunk();
	if (R.bytes + R.chunk.payload_size() + mr::CHUNK_HEADER_SIZE + record.size() + 64 > MAX_FILE_BYTES)
	{
		R.capped = true;
		mr::record_buffer buf;
		R.chunk.append(mr::encode(buf, mr::event_record{
			.type = mr::record_type::end,
			.time_ms = R.sched.time_ms(),
			.kind = mr::end_reason::size_limit,
		}));
		flush_chunk();
		con_printf(CON_URGENT, "movement recording: file size limit reached, recording stopped");
		return;
	}
	R.chunk.append(record);
}

[[nodiscard]]
bool mode_multi()
{
	return +(Game_mode & GM_MULTI);
}

[[nodiscard]]
bool is_bot(const unsigned pid)
{
	return mode_multi() && player_is_bot(pid);
}

[[nodiscard]]
bool locally_flown(const unsigned pid)
{
	return pid == Player_num || (mode_multi() && bot_is_local(pid));
}

[[nodiscard]]
bool player_in_game(const unsigned pid)
{
	if (!mode_multi())
		return pid == Player_num;
	return pid < N_players && vcplayerptr(pid)->connected == player_connection_status::playing;
}

[[nodiscard]]
bool player_recorded(const unsigned pid)
{
	return player_in_game(pid) && (!is_bot(pid) || CGameArg.SysRecordMovesBots);
}

[[nodiscard]]
std::uint8_t player_team(const unsigned pid)
{
#if DXX_USE_MULTIPLAYER
	if (mode_multi() && +(Game_mode & GM_TEAM))
		return underlying_value(multi_get_team_from_player(Netgame, pid));
#else
	(void)pid;
#endif
	return 0xff;
}

/* The player shares its controls (-sharemoves): the host knows it from
 * its INPUT, a sharing client of itself.
 */
[[nodiscard]]
bool shares_controls(const unsigned pid)
{
	if (!mode_multi() || !player_in_game(pid))
		return false;
	if (pid == Player_num)
		return CGameArg.SysShareMoves && !multi_i_am_master();
	return R.occupants[pid].in_game && R.occupants[pid].shares_controls;
}

[[nodiscard]]
std::uint8_t player_flags(const unsigned pid)
{
	std::uint8_t f{};
	if (player_in_game(pid))
		f |= mr::player_flag::connected;
	if (is_bot(pid))
		f |= mr::player_flag::bot;
	if (locally_flown(pid))
		f |= mr::player_flag::local;
	if (player_recorded(pid))
		f |= mr::player_flag::recorded;
	if (shares_controls(pid))
		f |= mr::player_flag::shares_controls;
	return f;
}

[[nodiscard]]
unsigned player_count()
{
	return mode_multi() ? std::min<unsigned>(N_players, MAX_PLAYERS) : Player_num + 1;
}

/* A PLAYER record for each slot whose details changed. */
void announce_players(const bool force)
{
	for (unsigned pid{}; pid != player_count(); ++pid)
	{
		auto &t{R.players[pid]};
		const auto f{player_flags(pid)};
		const auto team{player_team(pid)};
		const auto &cs{vcplayerptr(pid)->callsign};
		if (!force && t.announced && t.pflags == f && t.team == team && t.callsign == cs)
			continue;
		t.announced = true;
		t.pflags = f;
		t.team = team;
		t.callsign = cs;
		mr::record_buffer buf;
		put_record(mr::encode_player(buf, static_cast<std::uint8_t>(pid), f, team, static_cast<const char *>(cs)));
	}
}

[[nodiscard]]
const char *mission_name()
{
	return Current_mission ? static_cast<const char *>(Current_mission->mission_name) : "";
}

void begin_level()
{
	R.level_known = true;
	R.level_num = Current_level_num;
	R.segments = LevelSharedSegmentState.get_segments().get_count();
	R.mission = Current_mission.get();
	for (auto &t : R.players)
		t = {};
	R.sync_due = true;
	mr::record_buffer buf;
	put_record(mr::encode_level(buf, static_cast<std::int8_t>(std::clamp(Current_level_num, -128, 127)), static_cast<std::uint16_t>(R.segments), underlying_value(Game_mode), mission_name(), static_cast<const char *>(Current_level_name)));
	announce_players(true);
}

/* A new level: another number, mission or mine, or the game time
 * started over (StartNewLevel).
 */
void check_level()
{
	const unsigned segments{LevelSharedSegmentState.get_segments().get_count()};
	if (!R.level_known || R.level_num != Current_level_num || R.segments != segments || R.mission != Current_mission.get() || GameTime64 < R.last_game_time)
		begin_level();
	R.last_game_time = GameTime64;
}

bool begin_session()
{
	PHYSFS_mkdir("recordings");
	const std::time_t now{std::time(nullptr)};
	const std::tm *const lt{std::localtime(&now)};
	std::array<char, 80> name{};
	for (unsigned n{}; n != 100; ++n)
	{
		if (lt)
			std::snprintf(name.data(), name.size(), n ? "recordings/moves-%04d%02d%02d-%02d%02d%02d-%u.dmr" : "recordings/moves-%04d%02d%02d-%02d%02d%02d.dmr", lt->tm_year + 1900, lt->tm_mon + 1, lt->tm_mday, lt->tm_hour, lt->tm_min, lt->tm_sec, n);
		else
			std::snprintf(name.data(), name.size(), "recordings/moves-%u.dmr", n);
		if (!PHYSFS_exists(name.data()))
			break;
	}
	R.file = PHYSFS_openWrite(name.data());
	if (!R.file)
	{
		con_printf(CON_URGENT, "movement recording: cannot create %s: %s", name.data(), PHYSFS_getErrorByCode(PHYSFS_getLastErrorCode()));
		R.failed = true;
		return false;
	}
	const unsigned rate{CGameArg.SysRecordMovesRate ? CGameArg.SysRecordMovesRate : mr::DEFAULT_TICK_RATE};
	R.sched = mr::tick_scheduler{rate};
	R.chunk.restart();
	R.bytes = 0;
	R.capped = false;
	R.level_known = false;
	R.last_flush_ms = 0;
	R.last_game_time = GameTime64;
	R.occupants = {};
	R.sync_due = true;

	mr::file_header h;
	h.tick_rate = static_cast<std::uint16_t>(R.sched.tick_rate());
	if (mode_multi())
	{
		h.flags |= underlying_value(mr::header_flag::multiplayer);
		if (multi_i_am_master())
			h.flags |= underlying_value(mr::header_flag::host);
	}
	if (CGameArg.SysRecordMovesBots)
		h.flags |= underlying_value(mr::header_flag::bots_recorded);
	h.start_unix_time = static_cast<std::int64_t>(now);
	h.game_mode = underlying_value(Game_mode);
	h.local_pid = static_cast<std::uint8_t>(Player_num);
	h.program = DESCENT_VERSION;
	h.mission = mission_name();
	h.level_name = static_cast<const char *>(Current_level_name);
	h.level_num = static_cast<std::int8_t>(std::clamp(Current_level_num, -128, 127));
	for (unsigned pid{}; pid != player_count() && h.num_players < mr::MAX_RECORDED_PLAYERS; ++pid)
	{
		auto &p{h.players[h.num_players++]};
		p.pid = static_cast<std::uint8_t>(pid);
		p.flags = player_flags(pid);
		p.team = player_team(pid);
		p.callsign = static_cast<const char *>(vcplayerptr(pid)->callsign);
	}
	std::array<std::uint8_t, mr::MAX_HEADER_SIZE> hb;
	const auto n{mr::encode_header(hb, h)};
	write_bytes(std::span<const std::uint8_t>(hb).first(n));
	if (!R.file)
		return false;
	PHYSFS_flush(R.file);
	con_printf(CON_NORMAL, "movement recording: %s, %u samples per second", name.data(), R.sched.tick_rate());
	return true;
}

[[nodiscard]]
bool ship_alive(const unsigned pid, const object &obj)
{
	if (obj.type != object_type::OBJ_PLAYER)
		return false;
	if (pid == Player_num)
		return Player_dead_state == player_dead_state::no;
	return !(mode_multi() && bot_ship_dying(pid));
}

[[nodiscard]]
bool line_of_sight(const object &from, const object &to)
{
	auto &Objects = LevelUniqueObjectState.Objects;
	fvi_info hit;
	const auto type{find_vector_intersection(fvi_query{
		from.pos,
		to.pos,
		fvi_query::unused_ignore_obj_list,
		fvi_query::unused_LevelUniqueObjectState,
		fvi_query::unused_Robot_info,
		FQ_TRANSWALL,
		Objects.vcptridx(&from),
	}, from.segnum, 0, hit)};
	return type == fvi_hit_type::None;
}

[[nodiscard]]
bool players_see(const unsigned a, const object &oa, const unsigned b, const object &ob)
{
	auto &c{R.los[a][b]};
	if (!c)
	{
		c = line_of_sight(oa, ob) ? 1 : 2;
		R.los[b][a] = c;
	}
	return c == 1;
}

[[nodiscard]]
bool enemies(const unsigned a, const unsigned b)
{
	if (a == b || !mode_multi() || +(Game_mode & GM_MULTI_COOP))
		return false;
	return !(+(Game_mode & GM_TEAM) && player_team(a) == player_team(b));
}

/* The player `pid`'s ship when it is in the level and alive. */
[[nodiscard]]
const object *live_ship(const unsigned pid)
{
	if (!player_in_game(pid))
		return nullptr;
	auto &Objects = LevelUniqueObjectState.Objects;
	const auto objnum{vcplayerptr(pid)->objnum};
	if (objnum == object_none)
		return nullptr;
	const object &obj{*Objects.vcptr(objnum)};
	return ship_alive(pid, obj) ? &obj : nullptr;
}

struct candidate
{
	const object *obj{};
	std::uint8_t kind{};
	std::uint16_t id{};
	double d2{};
};

/* Keep the `ENEMY_CANDIDATES` nearest in `best`, sorted. */
void consider(std::array<candidate, ENEMY_CANDIDATES> &best, std::size_t &n, const candidate &c)
{
	if (n == best.size() && c.d2 >= best[n - 1].d2)
		return;
	std::size_t i{n < best.size() ? n++ : n - 1};
	while (i > 0 && best[i - 1].d2 > c.d2)
	{
		best[i] = best[i - 1];
		--i;
	}
	best[i] = c;
}

/* The fight context of `me` (player `pid`): the nearest enemy in sight,
 * else the nearest enemy; who aims at me.
 */
void fill_context(mr::sample &s, const unsigned pid, const object &me, const d_robot_info_array &Robot_info)
{
	std::array<candidate, ENEMY_CANDIDATES> best{};
	std::size_t n{};
	const auto my_pos{to_d(me.pos)};
	for (unsigned j{}; j != player_count(); ++j)
	{
		if (!enemies(pid, j))
			continue;
		const auto other{live_ship(j)};
		if (!other)
			continue;
		const auto d{sub(to_d(other->pos), my_pos)};
		const double d2{dot(d, d)};
		consider(best, n, candidate{other, mr::context_flag::kind_player, static_cast<std::uint16_t>(j), d2});
		/* Aiming at me: my ship in its narrow cone, near, in sight. */
		if (d2 > 0 && d2 < AIM_RANGE * AIM_RANGE && -dot(to_d(other->orient.fvec), d) > COS_AIM_CONE * std::sqrt(d2) && players_see(pid, me, j, *other))
			s.aimed_at_mask |= static_cast<std::uint8_t>(1u << j);
	}
	if (!mode_multi() || +(Game_mode & GM_MULTI_ROBOTS))
	{
		auto &Objects = LevelUniqueObjectState.Objects;
		for (auto &&o : Objects.vcptridx)
		{
			if (o->type != object_type::OBJ_ROBOT || robot_is_companion(Robot_info[get_robot_id(o)]))
				continue;
			const auto d{sub(to_d(o->pos), my_pos)};
			consider(best, n, candidate{&*o, mr::context_flag::kind_robot, o.get_unchecked_index(), dot(d, d)});
		}
	}
	if (!n)
		return;
	std::size_t chosen{0};
	bool sight{};
	for (std::size_t i{}; i != n; ++i)
	{
		const auto &c{best[i]};
		if (c.kind == mr::context_flag::kind_player ? players_see(pid, me, c.id, *c.obj) : line_of_sight(me, *c.obj))
		{
			chosen = i;
			sight = true;
			break;
		}
	}
	const auto &c{best[chosen]};
	const object &e{*c.obj};
	std::uint8_t ctx{c.kind};
	if (sight)
		ctx |= mr::context_flag::line_of_sight;
	const auto d{sub(to_d(e.pos), my_pos)};
	const double dist{std::sqrt(c.d2)};
	if (dist > 0)
	{
		if (dot(to_d(me.orient.fvec), d) > COS_VIEW_CONE * dist)
			ctx |= mr::context_flag::in_my_cone;
		if (-dot(to_d(e.orient.fvec), d) > COS_VIEW_CONE * dist)
			ctx |= mr::context_flag::me_in_its_cone;
	}
	if (e.type == object_type::OBJ_PLAYER && +(e.ctype.player_info.powerup_flags & player_flag::cloaked))
		ctx |= mr::context_flag::enemy_cloaked;
	const auto [rel_pos, pos_scaled]{rel16(e.pos, me.pos, mr::quant::REL_POS_SHIFT)};
	const auto [rel_vel, vel_scaled]{rel16(e.mtype.phys_info.velocity, me.mtype.phys_info.velocity, mr::quant::VEL_SHIFT)};
	if (pos_scaled)
		ctx |= mr::context_flag::rel_pos_scaled;
	if (vel_scaled)
		ctx |= mr::context_flag::rel_vel_scaled;
	s.context = ctx;
	s.enemy_id = c.id;
	s.enemy_rel_pos = rel_pos;
	s.enemy_rel_vel = rel_vel;
}

/* The controls of a ship flown here, from the thrust it was given this
 * frame (apply_pilot_controls), normalised to the ship's maximum; the
 * forward thrust unquantised (the afterburner).  Nothing without a ship
 * model.
 */
[[nodiscard]]
std::optional<double> ship_controls(const object &obj, mr::control_array &out)
{
	const auto &phys{obj.mtype.phys_info};
	const auto fwd{to_d(obj.orient.fvec)}, right{to_d(obj.orient.rvec)}, up{to_d(obj.orient.uvec)};
	const auto thrust{to_d(phys.thrust)};
	const double max_thrust{Player_ship->max_thrust / 65536.0};
	const double max_rot{Player_ship->max_rotthrust / 65536.0};
	if (!(max_thrust > 0 && max_rot > 0))
		return std::nullopt;
	const double forward{dot(thrust, fwd) / max_thrust};
	out = {{
		mr::quantise_control(forward),
		mr::quantise_control(dot(thrust, right) / max_thrust),
		mr::quantise_control(dot(thrust, up) / max_thrust),
		mr::quantise_control(phys.rotthrust.x / 65536.0 / max_rot),
		mr::quantise_control(phys.rotthrust.y / 65536.0 / max_rot),
		mr::quantise_control(phys.rotthrust.z / 65536.0 / max_rot),
	}};
	return forward;
}

void sample_player(const unsigned pid, const d_robot_info_array &Robot_info)
{
	auto &Objects = LevelUniqueObjectState.Objects;
	const auto objnum{vcplayerptr(pid)->objnum};
	if (objnum == object_none)
		return;
	const object &obj{*Objects.vcptr(objnum)};
	if (obj.type != object_type::OBJ_PLAYER && obj.type != object_type::OBJ_GHOST)
		return;
	auto &t{R.players[pid]};
	const bool alive{ship_alive(pid, obj)};
	if (t.alive_known && alive != t.alive)
		put_event(alive ? mr::record_type::respawn : mr::record_type::death, pid, mr::PLAYER_NONE, 0, 0, static_cast<std::uint16_t>(obj.segnum), 0);
	t.alive_known = true;
	t.alive = alive;

	const auto &pi{obj.ctype.player_info};
	const bool local{locally_flown(pid)};
	mr::sample s;
	s.pid = static_cast<std::uint8_t>(pid);
	if (alive)
		s.flags |= mr::sample_flag::alive;
	else if (obj.type == object_type::OBJ_PLAYER)
		s.flags |= mr::sample_flag::dying;
	if (+(pi.powerup_flags & player_flag::cloaked))
		s.flags |= mr::sample_flag::cloaked;
	if (+(pi.powerup_flags & player_flag::invulnerable))
		s.flags |= mr::sample_flag::invulnerable;
	if (local)
		s.flags2 |= mr::sample_flag2::local | mr::sample_flag2::vitals_exact;
	if (is_bot(pid))
		s.flags2 |= mr::sample_flag2::bot;
#if DXX_BUILD_DESCENT == 2
	if (+(pi.powerup_flags & player_flag::headlight_on))
		s.flags2 |= mr::sample_flag2::headlight;
	const bool guided{LevelUniqueObjectState.Guided_missile.get_player_active_guided_missile(pid) != object_none};
	if (guided)
		s.flags2 |= mr::sample_flag2::guided;
#else
	constexpr bool guided{false};
#endif
	s.segment = static_cast<std::uint16_t>(obj.segnum);
	s.pos = {{mr::quantise_pos(obj.pos.x), mr::quantise_pos(obj.pos.y), mr::quantise_pos(obj.pos.z)}};
	s.quat = mr::quantise_quaternion(mr::quaternion_from_axes(to_d(obj.orient.rvec), to_d(obj.orient.uvec), to_d(obj.orient.fvec)));
	const auto &phys{obj.mtype.phys_info};
	s.vel = {{mr::quantise_shift(phys.velocity.x, mr::quant::VEL_SHIFT), mr::quantise_shift(phys.velocity.y, mr::quant::VEL_SHIFT), mr::quantise_shift(phys.velocity.z, mr::quant::VEL_SHIFT)}};
	s.rotvel = {{mr::quantise_shift(phys.rotvel.x, mr::quant::ROTVEL_SHIFT), mr::quantise_shift(phys.rotvel.y, mr::quant::ROTVEL_SHIFT), mr::quantise_shift(phys.rotvel.z, mr::quant::ROTVEL_SHIFT)}};
	s.weapons = static_cast<std::uint8_t>((underlying_value(pi.Primary_weapon.get_active()) & 0x0f) | ((underlying_value(pi.Secondary_weapon.get_active()) & 0x0f) << 4));
	s.shields = mr::quantise_whole_units(obj.shields);
	s.energy = mr::quantise_whole_units(pi.energy);

	/* The controls, from the thrust the ship was given this frame
	 * (apply_pilot_controls): exact for the ships flown here.
	 */
	if (local && alive && !guided)
	{
		if (const auto forward{ship_controls(obj, s.controls)})
		{
			s.flags |= mr::sample_flag::controls;
			/* The afterburner scales the forward thrust above 1. */
			s.flags2 |= mr::sample_flag2::afterburner_known;
			if (*forward > 1.01)
				s.flags |= mr::sample_flag::afterburner;
		}
	}
	if (pid == Player_num)
	{
		s.flags2 |= mr::sample_flag2::buttons_known;
		if (Controls.state.fire_primary)
			s.flags |= mr::sample_flag::fire_primary;
		if (Controls.state.fire_secondary)
			s.flags |= mr::sample_flag::fire_secondary;
	}
#if DXX_USE_MULTIPLAYER
	else if (!local && mode_multi() && multi_i_am_master())
	{
		auto &o{R.occupants[pid]};
		/* A client's own report (INPUT, section 5.3 bit 1); known only
		 * once the client has set the bit (older clients never do).
		 */
		const auto ab{net_v2::host_input_afterburner(pid)};
		if (ab > 0)
			o.afterburner_seen = true;
		/* A client with -sharemoves sends its exact controls in INPUT
		 * (section 5.3, protocol 107): recorded as shared while they are
		 * fresh; in a gap the sample has none and the analysis
		 * estimates.  They never touch the ship.
		 */
		if (mr::control_array shared; net_v2::host_input_controls(pid, shared))
		{
			o.shares_controls = true;
			if (alive && !guided)
				mr::set_shared_controls(s, shared);
		}
		if (!(s.flags & mr::sample_flag::controls) && ab >= 0 && o.afterburner_seen)
		{
			s.flags2 |= mr::sample_flag2::afterburner_known;
			if (ab)
				s.flags |= mr::sample_flag::afterburner;
		}
	}
#endif

	const auto now_ms{R.sched.time_ms()};
	for (unsigned j{}; j != MAX_PLAYERS; ++j)
		if (const auto h{t.hit_by_ms[j]}; h && now_ms + 1 - h <= ATTACK_MEMORY_MS)
			s.attacked_mask |= static_cast<std::uint8_t>(1u << j);
	if (alive)
		fill_context(s, pid, obj, Robot_info);

	if (!t.weapons_known || t.weapons != s.weapons)
	{
		if (t.weapons_known)
			put_event(mr::record_type::weapon, pid, mr::PLAYER_NONE, 0, s.weapons & 0x0fu, static_cast<unsigned>(s.weapons >> 4), 0);
		t.weapons_known = true;
		t.weapons = s.weapons;
	}
	mr::record_buffer buf;
	put_record(mr::encode(buf, s));
}

/* Slot `pid` has a new occupant (or none any more): forget the old
 * one's state.
 */
void check_occupant(const unsigned pid)
{
	auto &o{R.occupants[pid]};
	const bool in_game{player_in_game(pid)};
	const bool bot{in_game && is_bot(pid)};
	const auto &cs{vcplayerptr(pid)->callsign};
	if (in_game == o.in_game && (!in_game || (bot == o.bot && cs == o.callsign)))
		return;
	auto &t{R.players[pid]};
	t.alive_known = false;
	t.weapons_known = false;
	t.hit_by_ms.fill(0);
	for (auto &p : R.players)
		p.hit_by_ms[pid] = 0;
	o.in_game = in_game;
	if (!in_game)
	{
		/* Gone (or between two levels): if the same player returns, it
		 * is still the same program, which reports the afterburner.  It
		 * may have been restarted without -sharemoves, though: sharing
		 * is noted again from its next controls.
		 */
		o.shares_controls = false;
		return;
	}
	if (bot != o.bot || !(cs == o.callsign))
	{
		o.afterburner_seen = false;
		o.shares_controls = false;
	}
	o.bot = bot;
	o.callsign = cs;
}

/* The link to the session's shared clock (sync_record). */
void put_sync()
{
	R.sync_due = false;
	R.last_sync_ms = R.sched.time_ms();
#if DXX_USE_MULTIPLAYER
	/* Only a network game has a clock to share. */
	if (!(Game_mode & GM_NETWORK))
		return;
	mr::sync_record y{.time_ms = R.sched.time_ms()};
	std::int64_t host_clock{};
	if (net_v2::recording_clock(y.session_id, host_clock))
		y.flags |= mr::sync_flag::clock_valid;
	/* net time units are 1/65536 s */
	y.host_ms = (host_clock * 1000) >> 16;
	if (multi_i_am_master())
		y.flags |= mr::sync_flag::host;
	mr::record_buffer buf;
	put_record(mr::encode(buf, y));
#endif
}

void sample_tick(const std::uint32_t tick, const d_robot_info_array &Robot_info)
{
	for (auto &row : R.los)
		row.fill(0);
	for (unsigned pid{}; pid != MAX_PLAYERS; ++pid)
		check_occupant(pid);
	announce_players(false);
	mr::record_buffer buf;
	put_record(mr::encode(buf, mr::tick_record{tick, R.sched.time_ms()}));
	if (R.sync_due || R.sched.time_ms() - R.last_sync_ms >= FLUSH_INTERVAL_MS)
		put_sync();
	for (unsigned pid{}; pid != player_count(); ++pid)
		if (player_recorded(pid))
			sample_player(pid, Robot_info);
}

[[nodiscard]]
unsigned player_of(const object &obj)
{
	if (obj.type != object_type::OBJ_PLAYER && obj.type != object_type::OBJ_GHOST)
		return mr::PLAYER_NONE;
	const unsigned pid{get_player_id(obj)};
	return pid < MAX_PLAYERS ? pid : mr::PLAYER_NONE;
}

[[nodiscard]]
std::uint8_t kind_of(const object &obj)
{
	switch (obj.type)
	{
		case object_type::OBJ_PLAYER:
		case object_type::OBJ_GHOST:
			return mr::attacker_kind::player;
		case object_type::OBJ_ROBOT:
			return mr::attacker_kind::robot;
		default:
			return mr::attacker_kind::other;
	}
}

}

void movement_record_frame(const d_robot_info_array &Robot_info)
{
	if (!CGameArg.SysRecordMoves || R.failed || Newdemo_state == ND_STATE_PLAYBACK)
		return;
	if (!R.file && !begin_session())
		return;
	check_level();
	if (const auto tick{R.sched.advance(FrameTime)}; tick && !R.capped)
		sample_tick(*tick, Robot_info);
	if (R.sched.time_ms() - R.last_flush_ms >= FLUSH_INTERVAL_MS)
		flush_chunk();
}

void movement_record_end_session()
{
	if (R.file && !R.capped)
	{
		put_event(mr::record_type::end, mr::PLAYER_NONE, mr::PLAYER_NONE, mr::end_reason::closed, 0, 0, 0);
		flush_chunk();
		con_printf(CON_NORMAL, "movement recording: closed, %llu bytes", static_cast<unsigned long long>(R.bytes));
	}
	close_file();
	R.failed = false;
	R.level_known = false;
}

void movement_record_fire(const object &shooter, const bool secondary, const unsigned weapon, const unsigned flags)
{
	if (!R.file)
		return;
	/* Every player's shot, also of a player whose samples are not
	 * recorded (a bot without -recordmoves-bots): the analysis of a
	 * recorded player needs its enemies' shots (dodging).
	 */
	const auto pid{player_of(shooter)};
	if (pid == mr::PLAYER_NONE)
		return;
	put_event(mr::record_type::fire, pid, mr::PLAYER_NONE, secondary ? mr::fire_kind::secondary : mr::fire_kind::primary, weapon, 0, flags);
}

void movement_record_fire_remote(const object &shooter, const uint8_t raw_weapon, const unsigned flags)
{
	if (!R.file)
		return;
#if DXX_USE_MULTIPLAYER
	if (raw_weapon == FLARE_ADJUST)
		return;
	if (raw_weapon >= MISSILE_ADJUST)
	{
		/* A missile's MULTI_FIRE flags are its gun and (guided) its
		 * generation, which the local path (do_missile_firing) does not
		 * record: 0 for every missile, from every source.
		 */
		if (const auto w{static_cast<unsigned>(raw_weapon - MISSILE_ADJUST)}; w < MAX_SECONDARY_WEAPONS)
			movement_record_fire(shooter, true, w, 0);
	}
	else if (raw_weapon < MAX_PRIMARY_WEAPONS)
		movement_record_fire(shooter, false, raw_weapon, flags);
#else
	(void)shooter;
	(void)raw_weapon;
	(void)flags;
#endif
}

namespace {

/* A hit event of `damage` to player `vpid` by player `apid` (or none). */
void record_damage(const unsigned vpid, const unsigned apid, const std::uint8_t akind, const unsigned weapon, const fix damage, const unsigned flags)
{
	if (!player_recorded(vpid) && (apid == mr::PLAYER_NONE || !player_recorded(apid)))
		return;
	/* Its own blast is not an attack on it. */
	if (apid < MAX_PLAYERS && apid != vpid)
		R.players[vpid].hit_by_ms[apid] = R.sched.time_ms() + 1;
	const auto amount{std::clamp<fix>(damage >> 8, 0, 0xffff)};
	put_event(mr::record_type::hit, vpid, apid, akind, weapon, static_cast<unsigned>(amount), flags | (locally_flown(vpid) ? mr::hit_flag::applied_here : 0u));
}

}

void movement_record_hit(const object &victim, const object &weapon, const fix damage)
{
	if (!R.file)
		return;
	const auto vpid{player_of(victim)};
	if (vpid == mr::PLAYER_NONE)
		return;
	auto &Objects = LevelUniqueObjectState.Objects;
	const auto &li{weapon.ctype.laser_info};
	unsigned apid{mr::PLAYER_NONE};
	std::uint8_t akind{mr::attacker_kind::other};
	if (li.parent_num != object_none)
	{
		const object &parent{*Objects.vcptr(li.parent_num)};
		if (laser_parent_is_matching_signature(li, parent))
		{
			akind = kind_of(parent);
			apid = player_of(parent);
		}
	}
	record_damage(vpid, apid, akind, underlying_value(get_weapon_id(weapon)), damage, 0);
}

void movement_record_splash(const object &victim, const object *const origin, const object *const parent, const fix damage)
{
	if (!R.file)
		return;
	const auto vpid{player_of(victim)};
	if (vpid == mr::PLAYER_NONE)
		return;
	unsigned apid{mr::PLAYER_NONE};
	std::uint8_t akind{mr::attacker_kind::other};
	/* The weapon's parent; else a ship that blew up (explode_badass_player). */
	if (const auto a{parent ? parent : origin && origin->type == object_type::OBJ_PLAYER ? origin : nullptr})
	{
		akind = kind_of(*a);
		apid = player_of(*a);
	}
	const unsigned weapon{origin && origin->type == object_type::OBJ_WEAPON ? underlying_value(get_weapon_id(*origin)) : 255u};
	record_damage(vpid, apid, akind, weapon, damage, mr::hit_flag::splash);
}

void movement_record_damage(const unsigned victim, const unsigned attacker, const std::uint8_t attacker_kind, const unsigned weapon, const fix damage, const bool splash)
{
	if (!R.file || victim >= MAX_PLAYERS)
		return;
	record_damage(victim, attacker < MAX_PLAYERS ? attacker : mr::PLAYER_NONE, attacker_kind, weapon, damage, splash ? mr::hit_flag::splash : 0u);
}

void movement_record_kill(const object &victim, const object *const killer)
{
	if (!R.file)
		return;
	const auto vpid{player_of(victim)};
	if (vpid == mr::PLAYER_NONE)
		return;
	put_event(mr::record_type::kill, vpid, killer ? player_of(*killer) : mr::PLAYER_NONE, killer ? kind_of(*killer) : mr::attacker_kind::none, 0, 0, 0);
}

bool movement_record_shared_controls(std::array<std::int8_t, 6> &controls)
{
	/* Consent: only with -sharemoves, only a client's own ship. */
	if (!CGameArg.SysShareMoves || !mode_multi() || multi_i_am_master() || Newdemo_state == ND_STATE_PLAYBACK)
		return false;
	if (Player_dead_state != player_dead_state::no)
		return false;
#if DXX_BUILD_DESCENT == 2
	if (LevelUniqueObjectState.Guided_missile.get_player_active_guided_missile(Player_num) != object_none)
		return false;
#endif
	const auto objnum{vcplayerptr(Player_num)->objnum};
	if (objnum == object_none)
		return false;
	const object &plr{*LevelUniqueObjectState.Objects.vcptr(objnum)};
	if (plr.type != object_type::OBJ_PLAYER)
		return false;
	return ship_controls(plr, controls).has_value();
}

void movement_record_pickup(const unsigned pnum, const unsigned powerup)
{
	if (!R.file || pnum >= MAX_PLAYERS || !player_recorded(pnum))
		return;
	put_event(mr::record_type::pickup, pnum, mr::PLAYER_NONE, 0, powerup, 0, 0);
}

}
