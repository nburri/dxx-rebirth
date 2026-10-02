/*
 * This file is part of the DXX-Rebirth project <https://github.com/dxx-rebirth/dxx-rebirth/>.
 * It is copyright by its individual contributors, as recorded in the
 * project's Git history.  See COPYING.txt at the top level for license
 * terms and a link to the Git history.
 */
/*
 * How a bot moves in a fight, the game-independent part
 * (Documentation/multiplayer-bots.md sections 9.15 and 9.16): the movement mode of
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
	/* The velocity controller along the path: no enemy about (section
	 * 9.16: only for a while after a stuck recovery).
	 */
	path = 1,
	/* The fight's keys: the strafe and the range key. */
	fight_keys = 2,
	/* The path (or a turn to an attacker) flown with keys in a fight
	 * (section 9.16: out of a fight too).
	 */
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
/* Section 9.16: the reverse key of a path braked into every bend (the
 * command's forward part goes below zero while the nose comes round): a
 * sixth of the time on the paths of the -botarena games, where EC held
 * reverse a twelfth of his time and slid through.  Reverse takes more.
 */
constexpr double PATH_KEY_ON_BACK{0.6};
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
 * simulation).  The keys take a quarter of it (section 9.16: three
 * twentieths; with 0.5 the corrections of the velocity's drift as the
 * nose followed a target were a third of the -botarena games' strafe
 * reversals).
 */
constexpr double PATH_KEY_GAIN{0.3};

/* The command the path's keys are taken from (world). */
[[nodiscard]]
inline vec3 path_key_command(const vec3 &wanted, const vec3 &vel, const double max_speed)
{
	if (max_speed <= 0)
		return {};
	return (wanted + (wanted - vel) * PATH_KEY_GAIN) * (1 / max_speed);
}

/* Section 9.16: in the -botarena games the path flown with keys in a
 * fight (60 % of the fight time: collect, retreat, refuel goals, the
 * line of fire blocked, the target out of sight) reversed the strafe 40
 * times a minute: the nose follows the target (it aims while it flies
 * its path), so the path's direction sweeps across the ship's axes and
 * the strafe key went this way and that.  A lateral key let go is not
 * pressed the other way for PATH_KEY_REVERSE_MS unless the command is
 * beyond PATH_KEY_ON_REVERSE (a large correction): a pause of forward
 * flight between the strafe runs, as EC flies.
 */
constexpr double PATH_KEY_ON_REVERSE{0.9};
constexpr unsigned PATH_KEY_REVERSE_MS{400};

class path_keys
{
	std::array<int8_t, 3> m_key{};
	std::array<uint32_t, 3> m_since{};
	/* The lateral keys last held, and when they were let go. */
	std::array<int8_t, 2> m_last{};
	std::array<uint32_t, 2> m_released{};
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
			/* Section 9.16: the other way soon after a lateral key was
			 * let go takes a large command.
			 */
			const auto on_for{[&](const int8_t sign) {
				if (i < 2 && !k)
				{
					const auto recent{[&](const std::size_t a) {
						return m_last[a] && tick - m_released[a] < ticks_from_ms(PATH_KEY_REVERSE_MS);
					}};
					const std::size_t j{1 - i};
					/* The other way on this axis, or (the other axis's key
					 * let go, none held there) a turn to this one.
					 */
					if ((m_last[i] == -sign && recent(i)) || (!m_key[j] && recent(j) && !(m_last[i] == sign && recent(i))))
						return std::max(on[i], PATH_KEY_ON_REVERSE);
				}
				return on[i];
			}};
			if (v > on_for(1))
				want = 1;
			else if (v < -(i == 2 ? std::max(on[i], PATH_KEY_ON_BACK) : on_for(-1)))
				want = -1;
			else if ((k > 0 && v < PATH_KEY_OFF) || (k < 0 && v > -PATH_KEY_OFF))
				want = 0;
			/* A lateral key turned round is let go first. */
			if (i < 2 && k && want == -k)
				want = 0;
			if (want == k)
				continue;
			/* Held long enough (a key not pressed is free). */
			if (k && tick - m_since[i] < ticks_from_ms(PATH_KEY_HOLD_MS))
				continue;
			if (i < 2 && k)
			{
				m_last[i] = k;
				m_released[i] = tick;
			}
			k = want;
			m_since[i] = tick;
		}
		return keys();
	}
	/* Section 9.16: a key the movement holds itself (the strafe run's),
	 * as if this pressed it.
	 */
	void hold(const std::size_t axis, const int8_t key, const uint32_t tick)
	{
		if (axis >= 3 || m_key[axis] == key)
			return;
		if (axis < 2 && m_key[axis])
		{
			m_last[axis] = m_key[axis];
			m_released[axis] = tick;
		}
		m_key[axis] = key;
		m_since[axis] = tick;
	}
	[[nodiscard]]
	thrust_keys keys() const
	{
		thrust_keys out;
		out.sideways = m_key[0];
		out.vertical = m_key[1];
		out.forward = m_key[2];
		return out;
	}
};

/* Section 9.16: engaged with a pickup on its path (collect, refuel), the
 * bot aimed at its target and flew the path with keys, whatever the
 * angle between them: three fifths of the fight time on the path's keys,
 * which turned this way and that as the nose followed the target across
 * the path's way (two thirds of the strafe reversals), slowly (the path
 * often behind the nose: reverse a fifth of the time), and aimed worse
 * than in the fight's keys.  With the target more than ASIDE_START off
 * the path's way the bot fights with the fight's keys (the pickup waits),
 * back to the path within ASIDE_END, or when the pickup is nearer than
 * ASIDE_NEAR along the path.
 */
constexpr double ASIDE_START{1.31};	// 75 degrees
constexpr double ASIDE_END{0.96};	// 55 degrees
constexpr double ASIDE_NEAR{50};

class pickup_aside
{
	bool m_on{};
public:
	void reset()
	{
		m_on = false;
	}
	/* Once per engaged tick with such a goal: `off` the angle between the
	 * path's way and the target, `left` the path's length left.  Whether
	 * the bot fights instead of flying the path.
	 */
	[[nodiscard]]
	bool update(const double off, const double left)
	{
		if (left < ASIDE_NEAR)
			m_on = false;
		else if (!m_on && off > ASIDE_START)
			m_on = true;
		else if (m_on && off < ASIDE_END)
			m_on = false;
		return m_on;
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
 * until the wall is close.  Section 9.16: the forward key with a wall
 * ahead nearer than that too.
 */
constexpr double JUKE_WALL_ROOM{18};
constexpr double JUKE_RELEASE_ROOM{8};

struct fight_room
{
	double left{JUKE_WALL_ROOM}, right{JUKE_WALL_ROOM}, down{JUKE_WALL_ROOM}, up{JUKE_WALL_ROOM}, back{JUKE_WALL_ROOM};
	/* Section 9.16: ahead too (the forward key let go short of a wall). */
	double front{JUKE_WALL_ROOM};
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
	if (k.forward > 0 && r.front < JUKE_RELEASE_ROOM)
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

/* Section 9.16: the dodge pressed its key against the strafe key held
 * (a reversal of the strafe after every shot dodged: a sixth to a
 * quarter of the bots' reversals in the -botarena games).  A strafe key
 * held is kept; the dodge adds the other lateral axis's key its way if
 * it lies DODGE_ADD_SHARE along it and the two keys together still push
 * along the dodge (the run goes on, diagonally); else the dodge turns it.
 */
constexpr double DODGE_ADD_SHARE{0.25};

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
	if (k.sideways && !k.vertical && ay > DODGE_ADD_SHARE * l && k.sideways * dodge_local.x + ay > 0)
	{
		k.sideways = k.sideways < 0 ? -1 : 1;
		k.vertical = dodge_local.y < 0 ? -1 : 1;
		return k;
	}
	if (k.vertical && !k.sideways && ax > DODGE_ADD_SHARE * l && k.vertical * dodge_local.y + ax > 0)
	{
		k.vertical = k.vertical < 0 ? -1 : 1;
		k.sideways = dodge_local.x < 0 ? -1 : 1;
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
 * fight near a wall flipped its strafe at the tick rate.  Section 9.16:
 * turned only for a push of 0.8 (was 0.5; EC touched the walls 13 times
 * a minute and slid on).
 */
constexpr double AVOID_KEY_SHARE{0.15};
constexpr double AVOID_FLIP_SHARE{0.8};
/* Section 9.16: a free key pressed off every wall the probe saw was a
 * fifth to a third of the bots' strafe reversals in the -botarena games
 * (the push was a run of its own, the other way of the last).  A free
 * key is pressed only for a push of AVOID_PRESS_SHARE (the wall is
 * near); a lesser push only lets go of a key into the wall.
 */
constexpr double AVOID_PRESS_SHARE{0.8};

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
		else if (!key && std::abs(a) >= AVOID_PRESS_SHARE)
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
 * Section 9.16: 0.75 within 20 units (was 0.55 within 30): the keys
 * slide through a bend, and the reverse key braked into every one.
 * Section 9.18: 0.9 (the humans of the group flew 7 to 10 units/s
 * faster than the bots on every map; the bots slowed into bends a human
 * slides through).
 */
constexpr double CORNER_SLOW_FROM{1.05};	// 60 degrees
constexpr double CORNER_SLOW_FULL{2.36};	// 135 degrees
constexpr double CORNER_SLOWEST{0.9};
constexpr double CORNER_SLOW_DISTANCE{20};
/* Section 9.18: the speed toward the end of a path that is not a pickup
 * (a hunt's or a roam's place): this much per unit of distance left (was
 * 1.5: from 39 units on the bot braked).
 */
constexpr double PATH_END_SLOW_GAIN{3};

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

/* The review of PR #74: the points the string skips are not flown past
 * (the bot cuts the corner, often further than 3 `reach` from them), so
 * advance_along never passed them: the path's point stayed behind, the
 * remaining length (the stuck detector's progress) grew as the bot flew
 * on, the string could not be pulled beyond PULL_AHEAD points of it, and
 * a broken string went back to points behind the bot.  In the -botarena
 * games the stuck recoveries went up 1.7 to 3.8 times.  The bot's point
 * on the path is now the skipped point nearest to it (never back).
 */
[[nodiscard]]
inline std::size_t advance_skipped(const std::span<const vec3> points, const std::size_t index, const std::size_t steer, const vec3 &pos)
{
	if (steer <= index || steer >= points.size())
		return index;
	std::size_t best{index};
	double best_d{distance(pos, points[index])};
	for (std::size_t i{index + 1}; i < steer; ++i)
		if (const double d{distance(pos, points[i])}; d < best_d)
		{
			best_d = d;
			best = i;
		}
	return best;
}

/* Section 9.16: EC flies his ways with forward and a strafe key held
 * together (the nose a little off the way, the thrust of both keys
 * along it), a third to a half of his time at 58-64 units/s, with the
 * afterburner 86-96; the bots flew their paths with the nose along the
 * way and the forward key alone (the top speed, 58, at best: 35 on
 * average).  On a straight of RUN_MIN_STRAIGHT ahead or more, with the
 * nose along its path (not aiming), the bot turns the nose RUN_ANGLE
 * off the way and holds forward and the strafe key of that side, until
 * the straight ahead is shorter than RUN_END_STRAIGHT (a bend).  The
 * next run keeps the side (RUN_SAME_SIDE: a run the other way right after
 * a bend is a strafe reversal).  With the afterburner lit the nose is
 * along the way (the afterburner pushes along the nose).
 */
constexpr double RUN_ANGLE{0.7};	// 40 degrees
constexpr double RUN_MIN_STRAIGHT{70};
constexpr double RUN_END_STRAIGHT{35};
constexpr double RUN_MIN_SPEED{20};

constexpr double RUN_SAME_SIDE{0.75};

class strafe_run
{
	int8_t m_side{}, m_last{};
public:
	void reset()
	{
		if (m_side)
			m_last = m_side;
		m_side = 0;
	}
	/* Once per tick of a path flown facing it: `straight` the straight
	 * flight ahead, `speed` the ship's.  The side of the run (-1 left,
	 * 1 right), 0 for none.
	 */
	[[nodiscard]]
	int update(bot_rng &rng, const double straight, const double speed)
	{
		if (!m_side && straight >= RUN_MIN_STRAIGHT && speed >= RUN_MIN_SPEED)
			m_side = static_cast<int8_t>(m_last && rng.uniform() < RUN_SAME_SIDE ? m_last : rng.uniform() < 0.5 ? -1 : 1);
		else if (m_side && straight < RUN_END_STRAIGHT)
			reset();
		return m_side;
	}
	[[nodiscard]]
	int side() const
	{
		return m_side;
	}
};

/* The nose of a run: `way` (unit) turned RUN_ANGLE away from the side
 * whose key is held (`right` the ship's right axis), so that forward and
 * that strafe key push along the way.
 */
[[nodiscard]]
inline vec3 strafe_run_face(const vec3 &way, const vec3 &right, const int side)
{
	const auto r{normalized(right - way * dot(right, way))};
	if (r == vec3{} || !side)
		return way;
	return normalized(way * std::cos(RUN_ANGLE) - r * (side * std::sin(RUN_ANGLE)));
}

/* Section 9.16: the path is flown with keys everywhere now (the velocity
 * controller held the top speed at best, and reversed the strafe at
 * every correction).  Keys are coarse: in a tight bend of Earth Shaker
 * (segments 28 and 29) a bot stuck 70-140 times in a game (two of the
 * -botarena games), the keys' corrections too late for the turn.
 * After a stuck recovery the bot flies its path with the velocity
 * controller for this long.
 */
constexpr unsigned PRECISE_AFTER_STUCK_MS{4000};

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
