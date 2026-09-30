/*
 * This file is part of the DXX-Rebirth project <https://github.com/dxx-rebirth/dxx-rebirth/>.
 * It is copyright by its individual contributors, as recorded in the
 * project's Git history.  See COPYING.txt at the top level for license
 * terms and a link to the Git history.
 */
/*
 * Multiplayer network protocol v2: the game-independent part of stage 4
 * (Documentation/network-protocol-v2.md, sections 5.5, 6.5 and 6.6, and
 * "Stage 4 as implemented" in section 8): firing, hits, damage, kills
 * and respawn with lag compensation.
 *
 * - `position_history`, `limit_rewind`: the host's ring of each player's
 *   accepted positions and the rewind to the time a shooter saw a target
 *   at, within the rewind window;
 * - `shot_registry`, `judge_direct_hit`, `judge_splash_hit`: the shots
 *   the host accepted and its verdict on a shooter's hit report;
 * - `evaluate_damage`, `splash_amount`, `clamp_claim`: the damage rules
 *   (death, invulnerability, friendly fire, cloak);
 * - `score_board`, `apply_kill`, `kill_credit`: who a kill is credited
 *   to and what it does to the scores;
 * - `fire_limiter`, `token_bucket`: the rate limits;
 * - `validate_input`, `rejection_monitor`, `correction_gate`: the host's
 *   checks of a client's reported ship state and the correction;
 * - `catch_up_time`, `catch_up`: how far and in which steps a shot that
 *   arrived late is advanced;
 * - `damage_accumulator`: a client's damage to itself, sent in portions;
 * - `netid_advance`: the ids of the projectiles of one shot;
 * - the wire layouts of FIRE, WEAPON_HIT, DAMAGE, PLAYER_KILLED and
 *   PLAYER_SPAWN.
 *
 * Times are the host clock in net time units (1/65536 s) widened to 64
 * bits; positions, distances and damage are `fix` (1.0 = 65536).  The
 * game side is similar/main/net_combat.cpp.
 *
 * Standard library only, so that the rules are tested outside the game
 * (common/unittest/net_v2_combat.cpp).
 */

#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <unordered_map>
#include <utility>

#include "net_v2.h"
#include "net_v2_state.h"
#include "net_v2_objects.h"

namespace dcx {

namespace net_v2 {

/* One unit of distance, and one point of shields, as `fix`. */
constexpr std::int64_t NET_V2_UNIT{65536};

/* Section 6.6. */
constexpr std::size_t NET_V2_HISTORY{64};
/* How far back the host follows a shooter's view of its target.  A
 * report about an older view is judged at this limit: the shooter then
 * has to lead by the rest.
 */
constexpr net_clock NET_V2_REWIND_DEFAULT{net_milliseconds(200)};
constexpr net_clock NET_V2_REWIND_LIMIT{net_milliseconds(500)};
/* Two entries further apart than this are a jump (a respawn), not a
 * path: the same 20 units as the interpolation (section 5.4, step 4).
 */
constexpr std::int64_t NET_V2_HISTORY_SNAP{20 * NET_V2_UNIT};

/* Section 6.5, hits. */
constexpr std::int64_t NET_V2_HIT_TOLERANCE{2 * NET_V2_UNIT};
constexpr std::int64_t NET_V2_REACH_SLACK{10 * NET_V2_UNIT};
/* A target is shown at most this long before the moment of the hit (the
 * largest interpolation delay plus a tick).
 */
constexpr net_clock NET_V2_VIEW_AGE_MAX{net_milliseconds(500)};

/* Section 6.5, firing. */
constexpr net_clock NET_V2_FIRE_PAST{net_milliseconds(500)};
constexpr net_clock NET_V2_FIRE_FUTURE{net_milliseconds(50)};
constexpr std::int64_t NET_V2_FIRE_ORIGIN_SLACK{5 * NET_V2_UNIT};
constexpr net_clock NET_V2_CATCH_UP_MAX{net_milliseconds(250)};
constexpr net_clock NET_V2_CATCH_UP_STEP{net_seconds(1) / 60};
/* A shooter may be this much ahead of its weapon's rate (shots of one
 * frame carry one fire time; a frame is at most this long).
 */
constexpr net_clock NET_V2_FIRE_BURST{net_milliseconds(250)};
/* Section 3.6: FIRE and WEAPON_HIT from one client, per second, and the
 * burst.  The omega cannon fires twenty times a second, and a target near
 * its chain of blobs may be hit by several of them per shot.
 */
constexpr unsigned NET_V2_COMBAT_RATE{160};
constexpr unsigned NET_V2_COMBAT_BURST{320};

/* Section 5.5. */
constexpr std::int64_t NET_V2_MAX_SHIP_SPEED{150 * NET_V2_UNIT};
constexpr std::int64_t NET_V2_TELEPORT_SLACK{5 * NET_V2_UNIT};
constexpr net_clock NET_V2_INPUT_PAST{net_seconds(1)};
constexpr net_clock NET_V2_INPUT_FUTURE{net_milliseconds(100)};

[[nodiscard]]
inline std::int64_t net_distance(const net_vec &a, const net_vec &b)
{
	const double dx{static_cast<double>(a.x) - b.x};
	const double dy{static_cast<double>(a.y) - b.y};
	const double dz{static_cast<double>(a.z) - b.z};
	return std::llround(std::sqrt(dx * dx + dy * dy + dz * dz));
}

[[nodiscard]]
inline std::int64_t net_length(const net_vec &a)
{
	return net_distance(a, {});
}

/* The distance covered at `speed` (fix per second) in `t` (net time). */
[[nodiscard]]
constexpr std::int64_t net_travel(const std::int64_t speed, const net_clock t)
{
	return t <= 0 ? 0 : speed * t / 65536;
}

/* Section 6.5: the projectiles of one FIRE have consecutive ids: the
 * creator's counter goes on (and wraps) within the creator's ids.
 */
[[nodiscard]]
constexpr netid_t netid_advance(const netid_t first, const unsigned i)
{
	return dynamic_netid(static_cast<std::uint8_t>(first >> NETID_COUNTER_BITS), static_cast<std::uint16_t>((first & NETID_COUNTER_MASK) + i));
}

[[nodiscard]]
constexpr std::uint8_t netid_creator(const netid_t id)
{
	return static_cast<std::uint8_t>((id >> NETID_COUNTER_BITS) & (NETID_CREATORS - 1));
}

/* Section 6.6: the last NET_V2_HISTORY accepted positions of one player,
 * in time order.
 */
struct history_entry
{
	net_clock time{};
	net_vec pos;
	std::uint16_t segment{};
};

enum class rewind_quality : std::uint8_t
{
	/* Between two entries. */
	interpolated,
	/* At or after the newest entry: the newest. */
	newest,
	/* Before the oldest entry: the oldest ("stale", section 6.6). */
	stale,
	/* Between two entries that are a jump apart: the earlier one. */
	snapped,
};

struct rewound
{
	net_vec pos;
	std::uint16_t segment{};
	rewind_quality quality{};
	/* How far the requested time lies past the newest entry (0 within
	 * the ring): the position is unknown for that long.
	 */
	net_clock beyond{};
};

template <std::size_t N = NET_V2_HISTORY>
class position_history
{
public:
	void clear()
	{
		count_ = 0;
		head_ = 0;
	}
	[[nodiscard]]
	bool empty() const
	{
		return count_ == 0;
	}
	[[nodiscard]]
	std::size_t size() const
	{
		return count_;
	}
	[[nodiscard]]
	const history_entry &newest() const
	{
		return at(count_ - 1);
	}
	[[nodiscard]]
	const history_entry &oldest() const
	{
		return at(0);
	}
	/* Entry `i`, 0 being the oldest. */
	[[nodiscard]]
	const history_entry &at(const std::size_t i) const
	{
		return ring_[(head_ + i) % N];
	}
	/* Add the position at `time`.  The ring stays ordered: a time not
	 * after the newest is refused.
	 */
	bool push(const net_clock time, const net_vec &pos, const std::uint16_t segment)
	{
		if (count_ && time <= newest().time)
			return false;
		if (count_ == N)
		{
			ring_[head_] = {time, pos, segment};
			head_ = (head_ + 1) % N;
		}
		else
			ring_[(head_ + count_++) % N] = {time, pos, segment};
		return true;
	}
	/* Where the player was at `t`. */
	[[nodiscard]]
	std::optional<rewound> rewind(const net_clock t) const
	{
		if (!count_)
			return std::nullopt;
		const auto &last{newest()};
		if (t >= last.time)
			return rewound{last.pos, last.segment, rewind_quality::newest, t - last.time};
		const auto &first{oldest()};
		if (t <= first.time)
			return rewound{first.pos, first.segment, t == first.time ? rewind_quality::interpolated : rewind_quality::stale, 0};
		/* The pair around `t`: the last entry not after it, by binary
		 * search.
		 */
		std::size_t lo{0}, hi{count_ - 1};
		while (hi - lo > 1)
		{
			const auto mid{(lo + hi) / 2};
			if (at(mid).time <= t)
				lo = mid;
			else
				hi = mid;
		}
		const auto &a{at(lo)};
		const auto &b{at(hi)};
		if (net_distance(a.pos, b.pos) > NET_V2_HISTORY_SNAP)
			return rewound{a.pos, a.segment, rewind_quality::snapped, 0};
		const auto span{b.time - a.time};
		const auto part{t - a.time};
		const auto lerp{[span, part](const std::int32_t from, const std::int32_t to) {
			return static_cast<std::int32_t>(from + (static_cast<std::int64_t>(to) - from) * part / span);
		}};
		return rewound{
			{lerp(a.pos.x, b.pos.x), lerp(a.pos.y, b.pos.y), lerp(a.pos.z, b.pos.z)},
			part * 2 >= span ? b.segment : a.segment,
			rewind_quality::interpolated,
			0,
		};
	}
private:
	std::array<history_entry, N> ring_{};
	std::size_t head_{};
	std::size_t count_{};
};

/* The time a report is judged at: the time the shooter saw its target
 * at, but not in the future and not further back than the window.
 */
struct rewind_time
{
	net_clock time{};
	/* The shooter's view was older than the window allows. */
	bool limited{};
};

[[nodiscard]]
constexpr rewind_time limit_rewind(const net_clock now, const net_clock requested, const net_clock window)
{
	if (requested >= now)
		return {now, false};
	if (requested < now - window)
		return {now - window, true};
	return {requested, false};
}

/* The rewind window from the host's setting in milliseconds (0: the
 * default).
 */
[[nodiscard]]
constexpr net_clock rewind_window(const unsigned ms)
{
	if (!ms)
		return NET_V2_REWIND_DEFAULT;
	return std::min(net_milliseconds(ms), NET_V2_REWIND_LIMIT);
}

/* Section 6.5: what a shooter reports. */
enum class hit_kind : std::uint8_t
{
	/* The shooter's projectile touched the target. */
	direct,
	/* The target was in the blast of the shooter's projectile; the point
	 * is the centre of the explosion.
	 */
	splash,
	/* The sender damaged itself without a weapon (a wall, lava, a fusion
	 * overcharge); `damage` is the amount.
	 */
	self,
};
constexpr std::uint8_t NET_V2_HIT_KINDS{3};

enum class hit_verdict : std::uint8_t
{
	accept,
	malformed,
	rate_limited,
	unknown_shot,
	not_owner,
	wrong_weapon,
	expired,
	too_early,
	consumed,
	out_of_reach,
	no_target,
	missed,
	blocked,
	/* Valid, but the damage rules gave nothing (evaluate_damage). */
	no_damage,
};
constexpr std::size_t NET_V2_HIT_VERDICTS{14};

[[nodiscard]]
constexpr const char *hit_verdict_name(const hit_verdict v)
{
	constexpr std::array<const char *, NET_V2_HIT_VERDICTS> names{{
		"accepted", "malformed", "rate limited", "unknown shot", "not the owner", "wrong weapon", "expired", "before the shot", "already hit", "out of reach", "no target", "missed", "blocked", "no damage",
	}};
	const auto i{static_cast<std::size_t>(v)};
	return i < names.size() ? names[i] : "?";
}

/* A shot the host accepted: what a report about it is checked against.
 * Every projectile of a FIRE has its own record; the children a
 * projectile makes when it explodes (smart blobs, the earthshaker's
 * missiles) are reported under their parent's id.
 */
struct shot_record
{
	netid_t id{NETID_NONE};
	std::uint8_t owner{};
	/* The weapon that was fired, and the weapon its children are (the
	 * same if it has none); 0xff: unknown (the host could not create its
	 * copy), any weapon is taken.
	 */
	std::uint8_t weapon_id{0xff};
	std::uint8_t child_weapon_id{0xff};
	net_clock fire_time{};
	net_vec origin;
	/* The fastest anything of this shot flies (fix per second), and how
	 * far from the origin it may be at once (the omega cannon's blobs are
	 * laid along its whole range when it fires).
	 */
	std::int64_t max_speed{};
	std::int64_t reach{};
	/* No report after this long. */
	net_clock lifetime{};
	/* The damage multiplier of the host's copy (a fusion charge). */
	std::int32_t multiplier{65536};
	/* How often it may hit one target, and any target. */
	std::uint8_t per_target_limit{1};
	std::uint8_t total_limit{1};
	std::array<std::uint8_t, NET_V2_MAX_PLAYERS> hits{};
	std::uint8_t total{};
	[[nodiscard]]
	constexpr bool accepts_weapon(const std::uint8_t reported) const
	{
		return weapon_id == 0xff || reported == weapon_id || reported == child_weapon_id;
	}
};

class shot_registry
{
public:
	void reset()
	{
		shots_.clear();
		last_sweep_ = 0;
	}
	[[nodiscard]]
	std::size_t size() const
	{
		return shots_.size();
	}
	/* A new shot replaces a record with the same id (the id was reused:
	 * the old projectile is gone on its owner's machine).
	 */
	void add(const shot_record &r)
	{
		if (r.id != NETID_NONE)
			shots_.insert_or_assign(r.id, r);
	}
	[[nodiscard]]
	shot_record *find(const netid_t id)
	{
		const auto i{shots_.find(id)};
		return i == shots_.end() ? nullptr : &i->second;
	}
	/* Forget the shots of `owner` (its slot is taken by a new player). */
	void forget_owner(const std::uint8_t owner)
	{
		std::erase_if(shots_, [owner](const auto &kv) {
			return kv.second.owner == owner;
		});
	}
	/* Forget the shots whose time is over (called now and then). */
	void expire(const net_clock now)
	{
		if (now < last_sweep_ + net_seconds(1))
			return;
		last_sweep_ = now;
		std::erase_if(shots_, [now](const auto &kv) {
			return now > kv.second.fire_time + kv.second.lifetime + net_seconds(1);
		});
	}
private:
	std::unordered_map<netid_t, shot_record> shots_;
	net_clock last_sweep_{};
};

/* The checks of a report that do not need the target: the shot exists,
 * is the sender's, is of the reported weapon, is not over, may still hit
 * this target, and can have flown to the reported point.
 */
[[nodiscard]]
inline hit_verdict judge_shot(const shot_record *const shot, const std::uint8_t from, const std::uint8_t weapon_id, const std::uint8_t target, const net_clock now, const net_clock target_time, const net_vec &point)
{
	if (!shot)
		return hit_verdict::unknown_shot;
	if (shot->owner != from)
		return hit_verdict::not_owner;
	if (!shot->accepts_weapon(weapon_id))
		return hit_verdict::wrong_weapon;
	if (target >= NET_V2_MAX_PLAYERS)
		return hit_verdict::no_target;
	if (now > shot->fire_time + shot->lifetime)
		return hit_verdict::expired;
	/* The hit happened after the shot; the target was shown as it was a
	 * little earlier.
	 */
	if (target_time < shot->fire_time - NET_V2_VIEW_AGE_MAX)
		return hit_verdict::too_early;
	if (shot->total >= shot->total_limit || shot->hits[target] >= shot->per_target_limit)
		return hit_verdict::consumed;
	if (net_distance(point, shot->origin) > net_travel(shot->max_speed, now - shot->fire_time) + shot->reach + NET_V2_REACH_SLACK)
		return hit_verdict::out_of_reach;
	return hit_verdict::accept;
}

constexpr void consume_shot(shot_record &shot, const std::uint8_t target)
{
	++shot.total;
	if (target < NET_V2_MAX_PLAYERS)
		++shot.hits[target];
}

/* Section 6.5, step 5: the projectile touched the target where the host
 * has the target at the time the shooter saw it.  `slack` covers the
 * projectile's own size.
 */
[[nodiscard]]
inline hit_verdict judge_direct_hit(const shot_record *const shot, const std::uint8_t from, const std::uint8_t weapon_id, const std::uint8_t target, const net_clock now, const net_clock target_time, const net_vec &point, const std::optional<rewound> &target_at, const std::int64_t target_size, const std::int64_t slack)
{
	if (const auto v{judge_shot(shot, from, weapon_id, target, now, target_time, point)}; v != hit_verdict::accept)
		return v;
	if (!target_at)
		return hit_verdict::no_target;
	/* A position past the newest entry is unknown for that long: the
	 * target may have moved on.
	 */
	const auto unknown{net_travel(NET_V2_MAX_SHIP_SPEED, std::min(target_at->beyond, net_milliseconds(100)))};
	if (net_distance(point, target_at->pos) > target_size + NET_V2_HIT_TOLERANCE + slack + unknown)
		return hit_verdict::missed;
	return hit_verdict::accept;
}

/* The damage of a blast at `distance` from its centre: `max_damage` at
 * the centre, nothing from `max_distance` on (the game's rule).
 */
[[nodiscard]]
constexpr std::int32_t splash_amount(const std::int32_t max_damage, const std::int64_t max_distance, const std::int64_t distance)
{
	if (max_distance <= 0 || distance >= max_distance || max_damage <= 0)
		return 0;
	if (distance <= 0)
		return max_damage;
	return static_cast<std::int32_t>(max_damage - distance * max_damage / max_distance);
}

struct splash_verdict
{
	hit_verdict verdict{};
	std::int32_t damage{};
};

/* Section 6.5, step 6: the blast of the shooter's projectile at `centre`
 * and the target where the host has it at the time the shooter saw it.
 * The damage is the host's, from its own distance.
 */
[[nodiscard]]
inline splash_verdict judge_splash_hit(const shot_record *const shot, const std::uint8_t from, const std::uint8_t weapon_id, const std::uint8_t target, const net_clock now, const net_clock target_time, const net_vec &centre, const std::optional<rewound> &target_at, const std::int32_t max_damage, const std::int64_t max_distance)
{
	if (const auto v{judge_shot(shot, from, weapon_id, target, now, target_time, centre)}; v != hit_verdict::accept)
		return {v, 0};
	if (!target_at)
		return {hit_verdict::no_target, 0};
	const auto damage{splash_amount(max_damage, max_distance, net_distance(centre, target_at->pos))};
	if (damage <= 0)
		return {hit_verdict::missed, 0};
	return {hit_verdict::accept, damage};
}

/* The damage of a direct hit is the shooter's claim (a projectile that
 * passed through something is weaker), but never more than the weapon
 * can do.
 */
[[nodiscard]]
constexpr std::int32_t clamp_claim(const std::int32_t claimed, const std::int32_t host_max)
{
	return std::clamp(claimed, 0, std::max(host_max, 0));
}

/* Section 6.5, step 6: the damage rules. */
enum class attacker_kind : std::uint8_t
{
	player,
	robot,
	reactor,
	/* A robot's mine. */
	mine,
	none,
};

struct victim_view
{
	/* In the game and in the level. */
	bool playing{};
	/* Not dead, dying or a ghost on the host. */
	bool alive{};
	bool invulnerable{};
	/* Cloaked: hidden, not protected. */
	bool cloaked{};
	std::uint8_t team{};
};

struct damage_context
{
	bool team_game{};
	bool coop{};
	/* Netgame.NoFriendlyFire. */
	bool no_friendly_fire{};
	/* The level is ending: nobody is hurt any more. */
	bool endlevel{};
};

enum class damage_verdict : std::uint8_t
{
	apply,
	not_playing,
	dead,
	invulnerable,
	friendly,
	endlevel,
	nothing,
};

/* `always`: damage that is never friendly (a wall, a bump, the ship's
 * own fusion overcharge).  With friendly fire off, a team mate's weapon
 * does nothing, the victim's own included (as the game always had it);
 * in a cooperative game no player's weapon does.
 */
[[nodiscard]]
constexpr damage_verdict evaluate_damage(const victim_view &victim, const attacker_kind kind, const std::uint8_t attacker_team, const damage_context &ctx, const bool always, const std::int32_t amount)
{
	if (!victim.playing)
		return damage_verdict::not_playing;
	if (!victim.alive)
		return damage_verdict::dead;
	if (victim.invulnerable)
		return damage_verdict::invulnerable;
	if (!always && ctx.no_friendly_fire && kind == attacker_kind::player)
	{
		if (ctx.coop)
			return damage_verdict::friendly;
		if (ctx.team_game && attacker_team == victim.team)
			return damage_verdict::friendly;
	}
	if (ctx.endlevel)
		return damage_verdict::endlevel;
	if (amount <= 0)
		return damage_verdict::nothing;
	return damage_verdict::apply;
}

/* Who a death is credited to.  A player is credited for what its weapon
 * does after its own death or while it is a ghost; a player that left
 * the game is nobody.
 */
struct kill_attribution
{
	attacker_kind kind{attacker_kind::none};
	std::uint8_t pid{NET_V2_PLAYER_ID_NONE};
};

[[nodiscard]]
constexpr kill_attribution kill_credit(const attacker_kind kind, const std::uint8_t attacker, const bool attacker_in_game)
{
	if (kind != attacker_kind::player)
		return {kind, NET_V2_PLAYER_ID_NONE};
	if (attacker >= NET_V2_MAX_PLAYERS || !attacker_in_game)
		return {attacker_kind::none, NET_V2_PLAYER_ID_NONE};
	return {attacker_kind::player, attacker};
}

/* The scores a kill changes, and the rules (multi_compute_kill). */
struct kill_mode
{
	bool team{};
	bool hoard{};
	bool bounty{};
};

struct score_board
{
	std::array<std::array<std::uint16_t, NET_V2_MAX_PLAYERS>, NET_V2_MAX_PLAYERS> kill_matrix{};
	std::array<std::int16_t, NET_V2_MAX_PLAYERS> kills{};
	std::array<std::int16_t, NET_V2_MAX_PLAYERS> deaths{};
	std::array<std::int16_t, NET_V2_MAX_PLAYERS> goal{};
	std::array<std::int16_t, 2> team_kills{};
	std::uint8_t bounty_target{};
	constexpr bool operator==(const score_board &) const = default;
};

[[nodiscard]]
constexpr std::uint8_t team_of(const std::uint8_t team_vector, const std::uint8_t pid)
{
	return (team_vector >> pid) & 1;
}

struct kill_outcome
{
	/* The killer's kill count changed by this much (0: no count). */
	std::int16_t killer_adjust{};
	/* The bounty target died to another player: the killer is the new
	 * target.
	 */
	bool bounty_to_killer{};
	/* The bounty target killed itself: the host draws a new one. */
	bool bounty_redraw{};
	/* A kill between players that counted for the kill goal. */
	bool counts_for_goal{};
};

constexpr kill_outcome apply_kill(score_board &b, const kill_mode mode, const std::uint8_t victim, const kill_attribution killer, const std::uint8_t team_vector)
{
	kill_outcome out;
	if (victim >= NET_V2_MAX_PLAYERS)
		return out;
	switch (killer.kind)
	{
		case attacker_kind::none:
			/* Nobody: the death is not counted either (as before). */
			return out;
		case attacker_kind::reactor:
			if (mode.team)
				--b.team_kills[team_of(team_vector, victim)];
			++b.deaths[victim];
			--b.kills[victim];
			--b.goal[victim];
			return out;
		case attacker_kind::robot:
		case attacker_kind::mine:
			++b.deaths[victim];
			return out;
		case attacker_kind::player:
			break;
	}
	const auto k{killer.pid};
	if (k >= NET_V2_MAX_PLAYERS)
		return out;
	++b.kill_matrix[k][victim];
	if (k == victim)
	{
		if (!mode.hoard)
		{
			if (mode.team)
				--b.team_kills[team_of(team_vector, victim)];
			++b.deaths[victim];
			--b.kills[victim];
			--b.goal[victim];
			out.killer_adjust = -1;
		}
		if (mode.bounty && victim == b.bounty_target)
			out.bounty_redraw = true;
		return out;
	}
	std::int16_t adjust{1};
	if (mode.team && team_of(team_vector, victim) == team_of(team_vector, k))
		adjust = -1;
	if (!mode.hoard)
	{
		if (mode.team)
		{
			b.team_kills[team_of(team_vector, k)] = static_cast<std::int16_t>(b.team_kills[team_of(team_vector, k)] + adjust);
			b.kills[k] = static_cast<std::int16_t>(b.kills[k] + adjust);
			b.goal[k] = static_cast<std::int16_t>(b.goal[k] + adjust);
			out.killer_adjust = adjust;
		}
		else if (mode.bounty)
		{
			if (victim == b.bounty_target || k == b.bounty_target)
			{
				++b.kills[k];
				++b.goal[k];
				out.killer_adjust = 1;
				if (victim == b.bounty_target)
				{
					b.bounty_target = k;
					out.bounty_to_killer = true;
				}
			}
		}
		else
		{
			++b.kills[k];
			++b.goal[k];
			out.killer_adjust = 1;
		}
	}
	++b.deaths[victim];
	out.counts_for_goal = true;
	return out;
}

/* Section 3.6: at most `rate` per second, with a burst. */
class token_bucket
{
public:
	constexpr token_bucket(const unsigned rate, const unsigned burst) :
		rate_{rate}, capacity_{static_cast<std::int64_t>(burst) * 65536}, tokens_{capacity_}
	{
	}
	constexpr void reset(const net_clock now)
	{
		tokens_ = capacity_;
		last_ = now;
	}
	/* Take one; false if none is left. */
	[[nodiscard]]
	constexpr bool take(const net_clock now)
	{
		if (now > last_)
		{
			/* One token is 65536 units: `rate` per 65536 time units. */
			tokens_ = std::min(capacity_, tokens_ + (now - last_) * rate_);
			last_ = now;
		}
		else if (now < last_)
			last_ = now;
		if (tokens_ < 65536)
			return false;
		tokens_ -= 65536;
		return true;
	}
private:
	std::int64_t rate_;
	std::int64_t capacity_;
	std::int64_t tokens_;
	net_clock last_{};
};

/* Section 6.5: a weapon fires no faster than 0.8 of its delay, by the
 * shots' own fire times, with NET_V2_FIRE_BURST of slack.
 */
class fire_limiter
{
public:
	constexpr void reset()
	{
		started_ = false;
	}
	/* A shot at `fire_time` of a weapon that waits `wait` between
	 * shots.  `rounds`: a volley (a secondary's `fire_count`) fires that
	 * many shots, one per frame, before the weapon waits.
	 */
	[[nodiscard]]
	constexpr bool allow(const net_clock fire_time, const net_clock wait, const unsigned rounds = 1)
	{
		const auto step{std::max<net_clock>(wait * 4 / 5, 1)};
		if (!started_)
		{
			started_ = true;
			due_ = fire_time;
		}
		if (fire_time < due_ - NET_V2_FIRE_BURST)
		{
			/* The further rounds of the volley just started. */
			if (extra_ + 1 < rounds && fire_time <= volley_ + NET_V2_FIRE_BURST)
			{
				++extra_;
				return true;
			}
			return false;
		}
		due_ = std::max(due_, fire_time) + step;
		volley_ = fire_time;
		extra_ = 0;
		return true;
	}
private:
	bool started_{};
	unsigned extra_{};
	/* When the current volley started. */
	net_clock volley_{};
	/* When the next shot is due at the weapon's rate. */
	net_clock due_{};
};

/* The host's checks of FIRE besides the rate (section 6.5). */
enum class fire_verdict : std::uint8_t
{
	accept,
	malformed,
	rate_limited,
	dead,
	time,
	origin,
	not_owned,
	too_fast,
};
constexpr std::size_t NET_V2_FIRE_VERDICTS{8};

[[nodiscard]]
constexpr const char *fire_verdict_name(const fire_verdict v)
{
	constexpr std::array<const char *, NET_V2_FIRE_VERDICTS> names{{
		"accepted", "malformed", "rate limited", "shooter dead", "fire time", "origin", "weapon not owned", "firing too fast",
	}};
	const auto i{static_cast<std::size_t>(v)};
	return i < names.size() ? names[i] : "?";
}

/* `alive`: the shooter lives on the host, or died after `fire_time` (a
 * shot fired before the shooter learned of its death counts).
 * `shooter_at`: the shooter's own position at `fire_time`.
 */
[[nodiscard]]
inline fire_verdict judge_fire(const net_clock now, const net_clock fire_time, const bool alive, const bool owned, const net_vec &origin, const std::optional<rewound> &shooter_at)
{
	if (!alive)
		return fire_verdict::dead;
	if (fire_time < now - NET_V2_FIRE_PAST || fire_time > now + NET_V2_FIRE_FUTURE)
		return fire_verdict::time;
	if (!owned)
		return fire_verdict::not_owned;
	if (shooter_at)
	{
		/* Past the newest position the ship may have flown on, for a
		 * while: a client that stops sending INPUT does not fire from
		 * anywhere.
		 */
		const auto unknown{net_travel(NET_V2_MAX_SHIP_SPEED, std::min(shooter_at->beyond, NET_V2_FIRE_BURST))};
		if (shooter_at->quality != rewind_quality::snapped && net_distance(origin, shooter_at->pos) > NET_V2_FIRE_ORIGIN_SLACK + unknown + net_travel(NET_V2_MAX_SHIP_SPEED, net_milliseconds(50)))
			return fire_verdict::origin;
	}
	return fire_verdict::accept;
}

/* Section 5.5: the host's checks of a client's ship state. */
enum class input_verdict : std::uint8_t
{
	accept,
	/* Not applied, no correction: the clock is off or the packet old. */
	drop_time,
	/* Rejected (CORRECTION): faster than a ship flies. */
	reject_speed,
	/* Rejected: further from the last accepted position than a ship
	 * flies in the time between.
	 */
	reject_teleport,
	/* Rejected by the game side: not in the level's geometry. */
	reject_position,
};

struct accepted_position
{
	bool valid{};
	net_clock time{};
	net_vec pos;
};

/* `sample_time` is the unwrapped time of the state; `prev` the last
 * accepted state of this life (not valid after a death or a respawn: a
 * new ship may be anywhere).
 */
[[nodiscard]]
inline input_verdict validate_input(const accepted_position &prev, const net_clock now, const net_clock sample_time, const net_vec &pos, const net_vec &vel)
{
	if (sample_time < now - NET_V2_INPUT_PAST || sample_time > now + NET_V2_INPUT_FUTURE)
		return input_verdict::drop_time;
	if (net_length(vel) > NET_V2_MAX_SHIP_SPEED)
		return input_verdict::reject_speed;
	if (prev.valid)
	{
		const auto dt{std::max<net_clock>(std::min(sample_time, now) - prev.time, 0)};
		if (net_distance(pos, prev.pos) > net_travel(NET_V2_MAX_SHIP_SPEED, dt) + NET_V2_TELEPORT_SLACK)
			return input_verdict::reject_teleport;
	}
	return input_verdict::accept;
}

/* Three rejections within a second are worth a line on the host's
 * console (and no more than one line per second).
 */
class rejection_monitor
{
public:
	/* A rejection at `now`; true if the warning is due. */
	[[nodiscard]]
	constexpr bool on_reject(const net_clock now)
	{
		times_[next_++ % times_.size()] = now;
		++count_;
		if (count_ < times_.size())
			return false;
		const auto oldest{*std::min_element(times_.begin(), times_.end())};
		if (now - oldest > net_seconds(1) || (warned_ && now - last_warning_ < net_seconds(1)))
			return false;
		warned_ = true;
		last_warning_ = now;
		return true;
	}
private:
	std::array<net_clock, 3> times_{};
	std::size_t next_{};
	std::size_t count_{};
	bool warned_{};
	net_clock last_warning_{};
};

/* Client: the host repeats CORRECTION until a good INPUT reaches it,
 * which is a round trip after the client was put back.  The corrections
 * that arrive meanwhile are about the same state and would pull the
 * ship back again: they are skipped for `hold`.
 */
class correction_gate
{
public:
	constexpr void reset()
	{
		active_ = false;
	}
	[[nodiscard]]
	constexpr bool apply(const net_clock now, const net_clock hold)
	{
		if (active_ && now >= since_ && now - since_ < hold)
			return false;
		active_ = true;
		since_ = now;
		return true;
	}
private:
	bool active_{};
	net_clock since_{};
};

/* Section 6.5: a shot that arrives `now - fire_time` late is advanced by
 * that much (at most NET_V2_CATCH_UP_MAX), in steps of the weapon's own
 * physics: each step sweeps its path, so the shot stops at the first
 * wall or object instead of appearing beyond it.
 */
[[nodiscard]]
constexpr net_clock catch_up_time(const net_clock now, const net_clock fire_time)
{
	return std::clamp<net_clock>(now - fire_time, 0, NET_V2_CATCH_UP_MAX);
}

/* Run `step(dt)` until `total` has passed or it returns false (the shot
 * is gone).  Returns the time run.
 */
template <typename Step>
net_clock catch_up(const net_clock total, const net_clock max_step, Step &&step)
{
	net_clock done{0};
	while (done < total)
	{
		const auto dt{std::min(max_step, total - done)};
		done += dt;
		if (!step(dt))
			break;
	}
	return done;
}

/* A client's damage to itself (a wall scraped, lava, every frame) is
 * summed and reported at most every `interval`.
 */
class damage_accumulator
{
public:
	constexpr void reset()
	{
		pending_ = 0;
		has_sent_ = false;
	}
	constexpr void add(const std::int32_t amount)
	{
		if (amount > 0)
			pending_ += amount;
	}
	[[nodiscard]]
	constexpr bool empty() const
	{
		return pending_ <= 0;
	}
	/* The amount to report now, or 0. */
	[[nodiscard]]
	constexpr std::int32_t take(const net_clock now, const net_clock interval)
	{
		if (pending_ <= 0 || (has_sent_ && now >= last_ && now - last_ < interval))
			return 0;
		has_sent_ = true;
		last_ = now;
		const auto amount{static_cast<std::int32_t>(std::min<std::int64_t>(pending_, std::numeric_limits<std::int32_t>::max()))};
		pending_ = 0;
		return amount;
	}
private:
	std::int64_t pending_{};
	bool has_sent_{};
	net_clock last_{};
};

/* Wire layouts (the ids are in net_v2_session.h). */

namespace detail {

inline void put_vec(cursor &c, const net_vec &v)
{
	c.i32(v.x);
	c.i32(v.y);
	c.i32(v.z);
}

[[nodiscard]]
inline net_vec get_vec(cursor &c)
{
	net_vec v;
	v.x = c.ri32();
	v.y = c.ri32();
	v.z = c.ri32();
	return v;
}

inline void put_quat(cursor &c, const net_quat &q)
{
	c.u16(static_cast<std::uint16_t>(q.w));
	c.u16(static_cast<std::uint16_t>(q.x));
	c.u16(static_cast<std::uint16_t>(q.y));
	c.u16(static_cast<std::uint16_t>(q.z));
}

[[nodiscard]]
inline net_quat get_quat(cursor &c)
{
	net_quat q;
	q.w = static_cast<std::int16_t>(c.r16());
	q.x = static_cast<std::int16_t>(c.r16());
	q.y = static_cast<std::int16_t>(c.r16());
	q.z = static_cast<std::int16_t>(c.r16());
	return q;
}

}

/* The most projectiles one FIRE makes (the omega cannon's blobs and the
 * object they replace).
 */
constexpr unsigned NET_V2_FIRE_MAX_PROJECTILES{32};
/* `track`: no homing target. */
constexpr std::uint8_t NET_V2_TRACK_NONE{0xff};

/* FIRE (0x27): shooter to host, host to everyone else (section 6.5). */
struct fire_msg
{
	static constexpr std::size_t SIZE{36};
	std::uint8_t pid{};
	/* Host time at which the shot left the gun. */
	net_time fire_time{};
	/* Primary index, MISSILE_ADJUST + secondary index, or FLARE_ADJUST. */
	std::uint8_t weapon{};
	std::uint8_t level{};
	std::uint8_t flags{};
	/* The shooter's ship at `fire_time`. */
	net_vec origin;
	std::uint16_t segment{};
	net_quat orient;
	/* For the random values of the shot (spread, speed), or 0. */
	std::uint16_t seed{};
	/* The first projectile's id, and how many there are (consecutive). */
	netid_t netid{NETID_NONE};
	std::uint8_t count{};
	/* The player a homing weapon tracks, or NET_V2_TRACK_NONE. */
	std::uint8_t track{NET_V2_TRACK_NONE};
	void write(std::span<std::uint8_t, SIZE> buf) const
	{
		detail::cursor c{.out = buf};
		c.u8(pid);
		c.u32(fire_time);
		c.u8(weapon);
		c.u8(level);
		c.u8(flags);
		detail::put_vec(c, origin);
		c.u16(segment);
		detail::put_quat(c, orient);
		c.u16(seed);
		c.u16(netid);
		c.u8(count);
		c.u8(track);
	}
	[[nodiscard]]
	static std::optional<fire_msg> read(const std::span<const std::uint8_t> buf)
	{
		if (buf.size() != SIZE)
			return std::nullopt;
		detail::cursor c{.in = buf};
		fire_msg m;
		m.pid = c.r8();
		m.fire_time = c.r32();
		m.weapon = c.r8();
		m.level = c.r8();
		m.flags = c.r8();
		m.origin = detail::get_vec(c);
		m.segment = c.r16();
		m.orient = detail::get_quat(c);
		m.seed = c.r16();
		m.netid = c.r16();
		m.count = c.r8();
		m.track = c.r8();
		if (m.pid >= NET_V2_MAX_PLAYERS || m.count > NET_V2_FIRE_MAX_PROJECTILES)
			return std::nullopt;
		if (m.count && (m.netid == NETID_NONE || is_level_netid(m.netid)))
			return std::nullopt;
		return m;
	}
};

/* WEAPON_HIT (0x28): shooter to host (section 6.5).  For `self` (the
 * sender's own damage without a weapon of a player: a wall, lava, a
 * bump, a robot's shot), `target` is the player the damage is credited to
 * (the sender itself, or the other ship of a bump) and `cause` what did
 * it; `netid` is NETID_NONE.
 */
struct weapon_hit_msg
{
	static constexpr std::size_t SIZE{28};
	/* The projectile (its parent's id for a child), or NETID_NONE. */
	netid_t netid{NETID_NONE};
	/* The weapon that hit (the child's own). */
	std::uint8_t weapon_id{};
	hit_kind kind{};
	std::uint8_t target{};
	attacker_kind cause{attacker_kind::player};
	/* The host time the target was shown at on the shooter's screen
	 * when it was hit.
	 */
	net_time target_time{};
	/* Where the projectile touched the target, or the blast's centre. */
	net_vec point;
	std::uint16_t segment{};
	/* The shooter's damage. */
	std::int32_t damage{};
	void write(std::span<std::uint8_t, SIZE> buf) const
	{
		detail::cursor c{.out = buf};
		c.u16(netid);
		c.u8(weapon_id);
		c.u8(static_cast<std::uint8_t>(kind));
		c.u8(target);
		c.u8(static_cast<std::uint8_t>(cause));
		c.u32(target_time);
		detail::put_vec(c, point);
		c.u16(segment);
		c.i32(damage);
	}
	[[nodiscard]]
	static std::optional<weapon_hit_msg> read(const std::span<const std::uint8_t> buf)
	{
		if (buf.size() != SIZE)
			return std::nullopt;
		detail::cursor c{.in = buf};
		weapon_hit_msg m;
		m.netid = c.r16();
		m.weapon_id = c.r8();
		const auto kind{c.r8()};
		m.target = c.r8();
		const auto cause{c.r8()};
		m.target_time = c.r32();
		m.point = detail::get_vec(c);
		m.segment = c.r16();
		m.damage = c.ri32();
		if (kind >= NET_V2_HIT_KINDS || m.target >= NET_V2_MAX_PLAYERS || cause > static_cast<std::uint8_t>(attacker_kind::none))
			return std::nullopt;
		m.kind = static_cast<hit_kind>(kind);
		m.cause = static_cast<attacker_kind>(cause);
		return m;
	}
};

/* DAMAGE (0x29): host to all.  The victim takes `amount` off its
 * shields and counts the message (INVENTORY `seq`), as it counts a
 * grant: the host's copy of its shields is its newest report less the
 * damage sent after it.
 */
struct damage_msg
{
	static constexpr std::size_t SIZE{24};
	std::uint8_t victim{};
	/* A player, or NET_V2_PLAYER_ID_NONE. */
	std::uint8_t attacker{NET_V2_PLAYER_ID_NONE};
	std::uint8_t weapon_id{0xff};
	/* How it was done (for the movement recording): a direct hit, a
	 * blast or the victim's own damage, and what did it.  One byte:
	 * the kind in bits 0-3, the cause in bits 4-7.
	 */
	hit_kind kind{hit_kind::self};
	attacker_kind cause{attacker_kind::none};
	std::int32_t amount{};
	/* The host's value after the damage. */
	std::int32_t shields{};
	net_vec point;
	void write(std::span<std::uint8_t, SIZE> buf) const
	{
		detail::cursor c{.out = buf};
		c.u8(victim);
		c.u8(attacker);
		c.u8(weapon_id);
		c.u8(static_cast<std::uint8_t>(static_cast<unsigned>(kind) | static_cast<unsigned>(cause) << 4));
		c.i32(amount);
		c.i32(shields);
		detail::put_vec(c, point);
	}
	[[nodiscard]]
	static std::optional<damage_msg> read(const std::span<const std::uint8_t> buf)
	{
		if (buf.size() != SIZE)
			return std::nullopt;
		detail::cursor c{.in = buf};
		damage_msg m;
		m.victim = c.r8();
		m.attacker = c.r8();
		m.weapon_id = c.r8();
		const std::uint8_t how{c.r8()};
		m.amount = c.ri32();
		m.shields = c.ri32();
		m.point = detail::get_vec(c);
		const unsigned kind{how & 0xfu}, cause{static_cast<unsigned>(how >> 4)};
		if (m.victim >= NET_V2_MAX_PLAYERS || m.amount < 0 || kind >= NET_V2_HIT_KINDS || cause > static_cast<unsigned>(attacker_kind::none))
			return std::nullopt;
		m.kind = static_cast<hit_kind>(kind);
		m.cause = static_cast<attacker_kind>(cause);
		return m;
	}
};

/* PLAYER_KILLED (0x2a): host to all.  The team vector and the bounty
 * target are the values before the kill, which the rules are applied
 * with; the counts are the host's after it, which every machine takes.
 */
struct player_killed_msg
{
	static constexpr std::size_t SIZE{14};
	std::uint8_t victim{};
	std::uint8_t killer{NET_V2_PLAYER_ID_NONE};
	attacker_kind kind{attacker_kind::none};
	std::uint8_t weapon_id{0xff};
	std::uint8_t team_vector{};
	std::uint8_t bounty_target{};
	/* kill_matrix[killer][victim] (0 without a killing player). */
	std::uint16_t matrix{};
	std::int16_t victim_deaths{};
	std::int16_t victim_kills{};
	std::int16_t killer_kills{};
	void write(std::span<std::uint8_t, SIZE> buf) const
	{
		detail::cursor c{.out = buf};
		c.u8(victim);
		c.u8(killer);
		c.u8(static_cast<std::uint8_t>(kind));
		c.u8(weapon_id);
		c.u8(team_vector);
		c.u8(bounty_target);
		c.u16(matrix);
		c.u16(static_cast<std::uint16_t>(victim_deaths));
		c.u16(static_cast<std::uint16_t>(victim_kills));
		c.u16(static_cast<std::uint16_t>(killer_kills));
	}
	[[nodiscard]]
	static std::optional<player_killed_msg> read(const std::span<const std::uint8_t> buf)
	{
		if (buf.size() != SIZE)
			return std::nullopt;
		detail::cursor c{.in = buf};
		player_killed_msg m;
		m.victim = c.r8();
		m.killer = c.r8();
		const auto kind{c.r8()};
		m.weapon_id = c.r8();
		m.team_vector = c.r8();
		m.bounty_target = c.r8();
		m.matrix = c.r16();
		m.victim_deaths = static_cast<std::int16_t>(c.r16());
		m.victim_kills = static_cast<std::int16_t>(c.r16());
		m.killer_kills = static_cast<std::int16_t>(c.r16());
		if (m.victim >= NET_V2_MAX_PLAYERS || kind > static_cast<std::uint8_t>(attacker_kind::none))
			return std::nullopt;
		m.kind = static_cast<attacker_kind>(kind);
		if (m.kind == attacker_kind::player ? m.killer >= NET_V2_MAX_PLAYERS : m.killer != NET_V2_PLAYER_ID_NONE)
			return std::nullopt;
		return m;
	}
};

/* PLAYER_SPAWN (0x2b): the spawning player to the host, host to
 * everyone else.  The ship appears at the given place.
 */
enum class spawn_flag : std::uint8_t
{
	/* Invulnerable on appearing (Netgame.InvulAppear). */
	invulnerable = 1 << 0,
};

struct player_spawn_msg
{
	static constexpr std::size_t SIZE{25};
	std::uint8_t pid{};
	std::uint8_t flags{};
	/* The start site (0xff: not one). */
	std::uint8_t site{0xff};
	net_vec pos;
	std::uint16_t segment{};
	net_quat orient;
	void write(std::span<std::uint8_t, SIZE> buf) const
	{
		detail::cursor c{.out = buf};
		c.u8(pid);
		c.u8(flags);
		c.u8(site);
		detail::put_vec(c, pos);
		c.u16(segment);
		detail::put_quat(c, orient);
	}
	[[nodiscard]]
	static std::optional<player_spawn_msg> read(const std::span<const std::uint8_t> buf)
	{
		if (buf.size() != SIZE)
			return std::nullopt;
		detail::cursor c{.in = buf};
		player_spawn_msg m;
		m.pid = c.r8();
		m.flags = c.r8();
		m.site = c.r8();
		m.pos = detail::get_vec(c);
		m.segment = c.r16();
		m.orient = detail::get_quat(c);
		if (m.pid >= NET_V2_MAX_PLAYERS)
			return std::nullopt;
		return m;
	}
};

}

}
