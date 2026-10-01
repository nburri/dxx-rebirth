/*
 * This file is part of the DXX-Rebirth project <https://github.com/dxx-rebirth/dxx-rebirth/>.
 * It is copyright by its individual contributors, as recorded in the
 * project's Git history.  See COPYING.txt at the top level for license
 * terms and a link to the Git history.
 */
/*
 * How a bot moves in a fight, the game-independent part
 * (Documentation/multiplayer-bots.md section 9.15): the movement mode of
 * a tick and how long it is held, a path flown with keys, the dodge and
 * the push off a wall as single keys, the corner speed, the string
 * pulling far ahead and the straight flight ahead.  bot_tick
 * (similar/main/bot.cpp) and the level simulation
 * (common/unittest/bot_level_sim.cpp) both fly with it.
 *
 * Standard library only, a pure function of its inputs.
 */

#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>

#include "bot_brain.h"

namespace dcx::bot {

/* The movement of a tick, as the movement recorder writes it
 * (movement_record_format.h, sample::bot_mode; the values are the
 * file's).
 */
enum class move_mode : uint8_t
{
	/* Not flying (dead, no ship). */
	none = 0,
	/* The velocity controller along the path: no enemy about. */
	path = 1,
	/* The fight's keys: the strafe and the range key. */
	fight_keys = 2,
	/* The path (or a turn to an attacker) flown with keys in a fight. */
	path_keys = 3,
	/* Turning round to a target behind, flown with keys. */
	turn_keys = 4,
	/* The slide while the nose comes round. */
	slide = 5,
	/* Ducking out of sight, the corner's peek, a hold: the velocity
	 * controller to a point.
	 */
	duck = 6,
	/* The stuck recovery. */
	recover = 7,
};
constexpr unsigned MOVE_MODE_COUNT{8};

inline constexpr std::array<const char *, MOVE_MODE_COUNT> move_mode_names{{
	"none", "path", "fight", "path-keys", "turn", "slide", "duck", "recover",
}};

[[nodiscard]]
constexpr const char *name_of(const move_mode m)
{
	const auto i{static_cast<unsigned>(m)};
	return i < MOVE_MODE_COUNT ? move_mode_names[i] : "?";
}

/* The movement is keys (as a human flies), not the velocity controller. */
[[nodiscard]]
constexpr bool is_keys(const move_mode m)
{
	return m == move_mode::fight_keys || m == move_mode::path_keys || m == move_mode::turn_keys || m == move_mode::slide;
}

/* The movement follows the path, so a wall ahead beyond the point it
 * steers at is the bend it is about to take (wall avoidance,
 * wall_hit_is_bend): not in the fight's keys, the slide or the turn
 * round, where the bot does not follow its path.
 */
[[nodiscard]]
constexpr bool follows_path(const move_mode m)
{
	return m != move_mode::fight_keys && m != move_mode::slide && m != move_mode::turn_keys;
}

/* Section 9.15: the recordings of exp-31 (2026-10-01) showed the fight
 * flight of PR #63 (the keys) in only 30 % of the fight time: whenever
 * the bot collected, retreated or refuelled, whenever the line of fire
 * was blocked a moment, whenever its enemy was out of its field of view
 * and in the first reaction time after sight, it flew its path with the
 * velocity controller (speed 38 against the human's 56-60, up and down
 * nine tenths as much as left and right).  Now the whole fight is keys:
 * a bot flies with keys while it engages its target, and for
 * FIGHT_KEYS_MS after it last saw it or was hit.
 */
constexpr unsigned FIGHT_KEYS_MS{1500};

/* Section 9.15: the movement changed about 40 times a bot-minute (the
 * fight, the path, the fight).  Engaged, the choice between the fight's
 * keys and the path is held at least MODE_HOLD_MS.
 */
constexpr unsigned MODE_HOLD_MS{1500};

class mode_hold
{
	bool m_combat{};
	bool m_set{};
	uint32_t m_since{};
public:
	void reset()
	{
		*this = {};
	}
	/* Once per engaged tick: `want_combat` what the tick asks for; the
	 * fight's keys or not as held.  The first tick of an engagement
	 * takes what it asks for.
	 */
	[[nodiscard]]
	bool update(const bool want_combat, const uint32_t tick)
	{
		if (!m_set)
		{
			m_set = true;
			m_combat = want_combat;
			m_since = tick;
		}
		else if (want_combat != m_combat && tick - m_since >= ticks_from_ms(MODE_HOLD_MS))
		{
			m_combat = want_combat;
			m_since = tick;
		}
		return m_combat;
	}
	/* Out of the engagement: the next one chooses afresh. */
	void release()
	{
		m_set = false;
	}
	[[nodiscard]]
	bool combat() const
	{
		return m_set && m_combat;
	}
};

/* Section 9.15: engaged with its target in sight and the line of fire
 * blocked (a pillar's edge, a ship in the way, a grate), the bot went
 * to its path toward the target at once: in the level simulation a
 * fifth of the fight time, its keys turning this way and that with the
 * path's points (137 strafe reversals a minute).  The line of fire
 * comes back as the target or the bot moves on; the fight's keys go on
 * for FIRE_BLOCKED_MS of a blocked line (without closing in: into what
 * blocks it), the path takes over after it.
 */
constexpr unsigned FIRE_BLOCKED_MS{1000};

class fire_blocked
{
	bool m_blocked{};
	uint32_t m_since{};
public:
	void reset()
	{
		m_blocked = false;
	}
	/* Once per engaged tick: whether the fight's keys go on. */
	[[nodiscard]]
	bool fight_on(const bool shot_clear, const uint32_t tick)
	{
		if (shot_clear)
		{
			m_blocked = false;
			return true;
		}
		if (!m_blocked)
		{
			m_blocked = true;
			m_since = tick;
		}
		return tick - m_since < ticks_from_ms(FIRE_BLOCKED_MS);
	}
};

/* Section 9.15: a path flown with keys.  The wanted velocity with a
 * little of the velocity controller's correction (path_key_command, in
 * the ship's frame) becomes a key per axis: pressed beyond PATH_KEY_ON
 * of full thrust, released below
 * PATH_KEY_OFF (hysteresis: a command about one threshold would tap the
 * key at the tick rate).  Up and down need more (PATH_KEY_ON_VERTICAL):
 * the human strafed up or down a third as much as sideways, the
 * velocity controller nine tenths, its small corrections of the climb
 * on a path that is not level with the nose.  The forward key comes a
 * little sooner (PATH_KEY_ON_FORWARD).  A key pressed is held at least
 * PATH_KEY_HOLD_MS (a human's tap; the strafe of the analysis counts a
 * key let go and pressed the other way at once as a reversal).
 */
constexpr double PATH_KEY_ON{0.4};
constexpr double PATH_KEY_ON_VERTICAL{0.85};
constexpr double PATH_KEY_ON_FORWARD{0.25};
constexpr double PATH_KEY_OFF{0.15};
constexpr unsigned PATH_KEY_HOLD_MS{250};
/* With the nose along the path (not aiming at an enemy), the sideways
 * and vertical keys take more: the nose turns to the path, and the keys
 * of the turn's lag were a strafe this way and that at every bend.
 */
constexpr double PATH_KEY_ON_FACING{0.75};
/* The velocity controller corrects with a gain of 2 (velocity_command):
 * as keys, every correction across the nose was a strafe key flipped
 * (the strafe reversed 100 times a minute on the paths of the level
 * simulation).  The keys take a quarter of it.
 */
constexpr double PATH_KEY_GAIN{0.5};

/* The command the path's keys are taken from (world). */
[[nodiscard]]
inline vec3 path_key_command(const vec3 &wanted, const vec3 &vel, const double max_speed)
{
	if (max_speed <= 0)
		return {};
	return (wanted + (wanted - vel) * PATH_KEY_GAIN) * (1 / max_speed);
}

class path_keys
{
	std::array<int8_t, 3> m_key{};
	std::array<uint32_t, 3> m_since{};
public:
	void reset()
	{
		m_key = {};
	}
	/* Once per tick, on the command in the ship's frame (right, up,
	 * forward); the keys at full thrust.
	 */
	[[nodiscard]]
	thrust_keys update(const vec3 &local, const uint32_t tick, const bool facing_path = false)
	{
		const std::array<double, 3> c{{local.x, local.y, local.z}};
		const std::array<double, 3> on{{facing_path ? std::max(PATH_KEY_ON_FACING, PATH_KEY_ON) : PATH_KEY_ON, facing_path ? std::max(PATH_KEY_ON_FACING, PATH_KEY_ON_VERTICAL) : PATH_KEY_ON_VERTICAL, PATH_KEY_ON_FORWARD}};
		for (std::size_t i{}; i != 3; ++i)
		{
			const double v{c[i]};
			auto &k{m_key[i]};
			int8_t want{k};
			if (v > on[i])
				want = 1;
			else if (v < -on[i])
				want = -1;
			else if ((k > 0 && v < PATH_KEY_OFF) || (k < 0 && v > -PATH_KEY_OFF))
				want = 0;
			if (want == k)
				continue;
			/* Held long enough (a key not pressed is free). */
			if (k && tick - m_since[i] < ticks_from_ms(PATH_KEY_HOLD_MS))
				continue;
			k = want;
			m_since[i] = tick;
		}
		thrust_keys out;
		out.sideways = m_key[0];
		out.vertical = m_key[1];
		out.forward = m_key[2];
		return out;
	}
};

/* Section 9.15: the walls round a fight.  The bots of exp-31 strafed
 * into the walls and the wall avoidance pushed them back: a reversal of
 * the strafe at every wall.  The free distance along the ship's axes
 * (fight_room, measured while fighting with keys): a juke's run that
 * starts toward a wall nearer than JUKE_WALL_ROOM goes the other way if
 * there is more room there (juke_turn_from_walls); a key held toward a
 * wall nearer than JUKE_RELEASE_ROOM is let go (the run ends: no
 * reversal), and the reverse key with a wall behind nearer than that
 * (keys_off_walls).  In a corridor (both sides near) the strafe goes on
 * until the wall is close.
 */
constexpr double JUKE_WALL_ROOM{18};
constexpr double JUKE_RELEASE_ROOM{8};

struct fight_room
{
	double left{JUKE_WALL_ROOM}, right{JUKE_WALL_ROOM}, down{JUKE_WALL_ROOM}, up{JUKE_WALL_ROOM}, back{JUKE_WALL_ROOM};
};

inline void juke_turn_from_walls(juke_state &juke, const fight_room &r)
{
	if (!juke.started())
		return;
	const int side{juke.side()};
	if ((side > 0 && r.right < JUKE_WALL_ROOM && r.left > r.right) || (side < 0 && r.left < JUKE_WALL_ROOM && r.right > r.left))
		juke.turn_side();
	const int vertical{juke.vertical()};
	if ((vertical > 0 && r.up < JUKE_WALL_ROOM && r.down > r.up) || (vertical < 0 && r.down < JUKE_WALL_ROOM && r.up > r.down))
		juke.turn_vertical();
}

[[nodiscard]]
inline thrust_keys keys_off_walls(thrust_keys k, const fight_room &r)
{
	if ((k.sideways > 0 && r.right < JUKE_RELEASE_ROOM) || (k.sideways < 0 && r.left < JUKE_RELEASE_ROOM))
		k.sideways = 0;
	if ((k.vertical > 0 && r.up < JUKE_RELEASE_ROOM) || (k.vertical < 0 && r.down < JUKE_RELEASE_ROOM))
		k.vertical = 0;
	if (k.forward < 0 && r.back < JUKE_RELEASE_ROOM)
		k.forward = 0;
	return k;
}

/* Section 9.15: the dodge with keys is one key at full thrust, the axis
 * the dodge's direction (ship's frame) lies most along; the others keep
 * theirs.  PR #63 added the dodge's vector to the keys, which made
 * every axis a mix (no key at all, as the recordings read it).  A
 * strafe key that already takes the ship out of the shot's way
 * (DODGE_KEEP_SHARE of the dodge along it) is kept, at full thrust: a
 * dodge is no reversal of the strafe unless it has to be.
 */
constexpr double DODGE_KEEP_SHARE{0.3};

[[nodiscard]]
inline thrust_keys dodge_key(thrust_keys k, const vec3 &dodge_local)
{
	const double ax{std::abs(dodge_local.x)}, ay{std::abs(dodge_local.y)}, az{std::abs(dodge_local.z)};
	if (ax <= 0 && ay <= 0 && az <= 0)
		return k;
	const double l{std::sqrt(ax * ax + ay * ay + az * az)};
	if (k.sideways * dodge_local.x > DODGE_KEEP_SHARE * l)
	{
		k.sideways = k.sideways < 0 ? -1 : 1;
		return k;
	}
	if (k.vertical * dodge_local.y > DODGE_KEEP_SHARE * l)
	{
		k.vertical = k.vertical < 0 ? -1 : 1;
		return k;
	}
	if (ax >= ay && ax >= az)
		k.sideways = dodge_local.x < 0 ? -1 : 1;
	else if (ay >= az)
		k.vertical = dodge_local.y < 0 ? -1 : 1;
	else
		k.forward = dodge_local.z < 0 ? -1 : 1;
	return k;
}

/* Section 9.15: the push off a wall with keys.  On each axis where the
 * push (wall avoidance's velocity, ship's frame) is at least
 * AVOID_KEY_SHARE of the top speed: a key held into the wall is let go,
 * or turned round if the push is AVOID_FLIP_SHARE or more (the wall is
 * near); a free key pushes off.  Only such a turned key skips the
 * strafe's flip filter (lateral_keys, `immediate` per axis); PR #63
 * skipped it for every axis whenever the bot avoided a wall, and a
 * fight near a wall flipped its strafe at the tick rate.
 */
constexpr double AVOID_KEY_SHARE{0.15};
constexpr double AVOID_FLIP_SHARE{0.5};

struct avoid_keys_result
{
	thrust_keys keys;
	/* Sideways, vertical: the key was turned off the wall. */
	std::array<bool, 2> immediate{};
};

[[nodiscard]]
inline avoid_keys_result avoid_keys(thrust_keys k, const vec3 &avoid_local, const double max_speed)
{
	avoid_keys_result out;
	if (max_speed <= 0)
	{
		out.keys = k;
		return out;
	}
	const auto one{[max_speed](double &key, const double push, bool *const immediate) {
		const double a{push / max_speed};
		if (std::abs(a) < AVOID_KEY_SHARE)
			return;
		const double s{a < 0 ? -1.0 : 1.0};
		if (key * s < 0)
		{
			if (std::abs(a) < AVOID_FLIP_SHARE)
				key = 0;
			else
			{
				key = s;
				if (immediate)
					*immediate = true;
			}
		}
		else if (!key)
			key = s;
	}};
	one(k.sideways, avoid_local.x, &out.immediate[0]);
	one(k.vertical, avoid_local.y, &out.immediate[1]);
	one(k.forward, avoid_local.z, nullptr);
	out.keys = k;
	return out;
}

/* Section 9.15: the speed into a corner of the path.  B1 slowed to 0.4
 * of the top speed for every bend of more than 60 degrees within 40
 * units; the paths of real levels bend every few segments, and the bots
 * flew their paths at 30 units/s.  Now the slowing grows with the bend,
 * from none at CORNER_SLOW_FROM to CORNER_SLOWEST of the top speed at
 * CORNER_SLOW_FULL and beyond, within CORNER_SLOW_DISTANCE of it.
 */
constexpr double CORNER_SLOW_FROM{1.05};	// 60 degrees
constexpr double CORNER_SLOW_FULL{2.36};	// 135 degrees
constexpr double CORNER_SLOWEST{0.55};
constexpr double CORNER_SLOW_DISTANCE{30};

[[nodiscard]]
inline double corner_speed(const double bend, const double dist, const double max_speed)
{
	if (bend <= CORNER_SLOW_FROM || dist >= CORNER_SLOW_DISTANCE)
		return max_speed;
	const double w{std::clamp((bend - CORNER_SLOW_FROM) / (CORNER_SLOW_FULL - CORNER_SLOW_FROM), 0.0, 1.0)};
	const double floor{max_speed * (1 - (1 - CORNER_SLOWEST) * w)};
	return std::min(max_speed, std::max(floor, dist * 2));
}

/* Section 9.15: the string pulling looked at most 4 path points ahead
 * (a path has a point at every side and every segment centre passed: 40
 * units), so the long straight flight of the afterburner (BOT_LONG_STRAIGHT,
 * 150 units) was never seen.  Now the point steered at moves on along
 * the path, up to PULL_AHEAD points, at most PULL_PROBES probes a
 * perception tick: from the point steered at last, if it is still
 * reachable, the next ones in turn until one is not; else (the string
 * broke: a wall came between) the nearest PULL_BACK points, from the
 * furthest down (pull_string).
 */
constexpr unsigned PULL_AHEAD{24};
constexpr unsigned PULL_PROBES{4};
constexpr unsigned PULL_BACK{4};

template <typename Reachable>
[[nodiscard]]
std::size_t pull_string_ahead(const std::size_t from, const std::size_t current, const std::size_t count, Reachable &&reachable)
{
	if (from >= count)
		return from;
	std::size_t at{std::max(from, current)};
	const std::size_t last{std::min(count - 1, from + PULL_AHEAD - 1)};
	unsigned probes{PULL_PROBES};
	if (at > from)
	{
		--probes;
		if (at > last || !reachable(at))
		{
			/* The string broke: the nearest points (pull_string). */
			const std::size_t back{std::min(count - 1, from + PULL_BACK - 1)};
			for (std::size_t i{std::min(back, at > from ? at - 1 : from)}; i > from; --i)
				if (reachable(i))
					return i;
			return from;
		}
	}
	while (probes && at < last)
	{
		--probes;
		if (!reachable(at + 1))
			break;
		++at;
	}
	return at;
}

/* Section 9.15: how far the bot flies straight on: to the point it
 * steers at, then along the path while each leg stays within
 * STRAIGHT_MAX_BEND of the direction to that point.
 */
constexpr double STRAIGHT_MAX_BEND{0.35};	// 20 degrees

[[nodiscard]]
inline double straight_ahead(const std::span<const vec3> points, const std::size_t steer, const vec3 &pos)
{
	if (steer >= points.size())
		return 0;
	const auto dir{normalized(points[steer] - pos)};
	double d{distance(pos, points[steer])};
	for (std::size_t i{steer}; i + 1 < points.size(); ++i)
	{
		const auto leg{points[i + 1] - points[i]};
		const double l{length(leg)};
		if (l < 1e-6)
			continue;
		if (dot(leg, dir) < l * std::cos(STRAIGHT_MAX_BEND))
			break;
		d += l;
	}
	return d;
}

}
