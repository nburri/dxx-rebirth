/*
 * This file is part of the DXX-Rebirth project <https://github.com/dxx-rebirth/dxx-rebirth/>.
 * It is copyright by its individual contributors, as recorded in the
 * project's Git history.  See COPYING.txt at the top level for license
 * terms and a link to the Git history.
 */
/*
 * The bot brain, the game-independent part
 * (Documentation/multiplayer-bots.md sections 3.4, 4 and 5):
 *
 * - the tick layers (60 Hz tactics, 20 Hz perception, 5 Hz strategy,
 *   staggered per bot);
 * - the skill and style tables;
 * - the bot's own random numbers;
 * - aiming: the intercept solver with lead, the drifting aim error;
 * - the reaction delay line and the memory of targets;
 * - target scoring with hysteresis;
 * - the steering controller that turns an angle error into a rotation
 *   axis, and the velocity controller for the thrust axes;
 * - strafing, range keeping, trigger discipline and the primary choice.
 *
 * Everything is a pure function of its inputs and the bot's random
 * numbers, and every decision is taken on a fixed tick, so the bot's
 * decisions do not depend on the host's frame rate.  Standard library
 * only (common/unittest/bot_brain.cpp).
 */

#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <numbers>
#include <optional>
#include <random>
#include <span>

#include "bot_vec.h"

namespace dcx::bot {

/* Section 3.4: the brain's tick and its slower layers. */
constexpr unsigned BOT_TICK_RATE{60};
constexpr unsigned PERCEPTION_DIVISOR{3};	// 20 Hz
constexpr unsigned STRATEGY_DIVISOR{12};	// 5 Hz

[[nodiscard]]
constexpr unsigned ticks_from_ms(const unsigned ms)
{
	return (ms * BOT_TICK_RATE + 500) / 1000;
}

/* Layer `divisor` runs on tick `tick` for the bot with stagger `k` (its
 * slot), so that the bots do not all pay for it in the same frame.
 */
[[nodiscard]]
constexpr bool layer_due(const uint32_t tick, const unsigned divisor, const unsigned k)
{
	return (tick + k) % divisor == 0;
}

[[nodiscard]]
constexpr double radians(const double degrees)
{
	return degrees * std::numbers::pi / 180;
}

enum class bot_skill : uint8_t
{
	trainee,
	rookie,
	hotshot,
	ace,
	insane,
};
constexpr unsigned BOT_SKILL_COUNT{5};

enum class bot_style : uint8_t
{
	balanced,
	aggressive,
	cautious,
	collector,
};
constexpr unsigned BOT_STYLE_COUNT{4};

enum class bot_team : uint8_t
{
	automatic,
	blue,
	red,
};
constexpr unsigned BOT_TEAM_COUNT{3};

/* Decision 5 of section 11. */
constexpr bot_skill BOT_DEFAULT_SKILL{bot_skill::hotshot};

inline constexpr std::array<const char *, BOT_SKILL_COUNT> bot_skill_names{{"Trainee", "Rookie", "Hotshot", "Ace", "Insane"}};
inline constexpr std::array<const char *, BOT_STYLE_COUNT> bot_style_names{{"Balanced", "Aggressive", "Cautious", "Collector"}};
inline constexpr std::array<const char *, BOT_TEAM_COUNT> bot_team_names{{"Auto", "Blue", "Red"}};

/* Section 6.2: the built-in names (at most 8 characters, CALLSIGN_LEN). */
inline constexpr std::array<const char *, 16> bot_default_names{{
	"Ravager", "Havoc", "Sparky", "Nomad", "Wraith", "Talon", "Viper", "Blitz",
	"Rook", "Jinx", "Vortex", "Grinder", "Cinder", "Specter", "Brick", "Dart",
}};

/* Section 5.1 (the final table: section 9.7).  Distances in game units,
 * angles in degrees.  Every value is monotonic from Trainee to Insane;
 * none is perfect (no zero reaction, no zero aim error), so Insane plays
 * like a very good human, not a machine.
 */
struct skill_params
{
	unsigned reaction_ms;
	double aim_sigma_deg;
	unsigned aim_drift_ms;
	double lead;
	double turn_cap;
	double fire_cone_deg;
	double fov_half_deg;
	double awareness;
	double hearing;
	unsigned memory_ms;
	double dodge_prob;
	unsigned weapon_smarts;
	/* Map knowledge in path segments; 0xffff is the whole level. */
	unsigned map_knowledge;
	bool strafe;
	/* The strafe takes a new direction after this many ms (random in
	 * range).
	 */
	unsigned strafe_min_ms;
	unsigned strafe_max_ms;
	/* The vertical share of the strafe (0: flat; section 4.6, bobbing). */
	double strafe_vertical;
	/* Section 9.7: the strafe's speed, a share of the top speed (B1:
	 * 0.7 for every skill that strafes).
	 */
	double strafe_speed;
	/* Section 9.7: the share of the time the trigger is held while a
	 * shot is on (fire_burst): a beginner pauses between bursts.
	 */
	double fire_duty;
};

inline constexpr std::array<skill_params, BOT_SKILL_COUNT> skill_table{{
	/* reaction, sigma, drift, lead, turn, cone, fov, aware, hear, memory, dodge, smarts, map, strafe, runs, vertical, strafe speed, duty */
	{550, 7.0, 600, 0.0, 0.45, 12, 45, 150, 0, 2000, 0.0, 0, 0, false, 1200, 2000, 0.0, 0.0, 0.55},
	{400, 4.5, 500, 0.4, 0.60, 9, 60, 250, 80, 3000, 0.2, 1, 3, true, 900, 1800, 0.25, 0.5, 0.8},
	{280, 2.8, 400, 0.7, 0.75, 6, 70, 350, 150, 5000, 0.45, 2, 8, true, 600, 1400, 0.5, 0.7, 1.0},
	{200, 1.7, 300, 0.9, 0.90, 4, 80, 450, 250, 7000, 0.7, 3, 15, true, 500, 1300, 0.8, 0.75, 1.0},
	{140, 1.0, 250, 1.0, 1.00, 3, 90, 600, 350, 10000, 0.85, 4, 0xffff, true, 400, 1100, 1.0, 0.8, 1.0},
}};

[[nodiscard]]
constexpr const skill_params &skill_of(const bot_skill s)
{
	const auto i{static_cast<unsigned>(s)};
	return skill_table[i < BOT_SKILL_COUNT ? i : static_cast<unsigned>(BOT_DEFAULT_SKILL)];
}

/* Section 5.2 (the final table: section 9.7): multipliers and offsets on
 * top of the skill.
 */
struct style_params
{
	double retreat_shields;
	double engage_weight;
	double collect_weight;
	double range_scale;
	/* How long a lost target is hunted: a scale of the skill's memory
	 * (Aggressive chases twice as long).
	 */
	double chase_memory;
	/* Added to the skill's dodge probability (a skill that never dodges
	 * still does not).
	 */
	double dodge_bonus;
	/* A scale of the time between two mines (Aggressive drops fewer,
	 * Cautious more).
	 */
	double mine_interval;
	/* Scales of the strafe speed and of the speed it closes in or backs
	 * off with inside the fight band.
	 */
	double strafe_scale;
	double close_scale;
	/* Not ahead in a fight (fight_advantage below 1), the engage weight
	 * falls toward this share (at an advantage of one half and below).
	 */
	double behind_engage;
	/* Outgunned (advantage below OUTGUNNED_ADVANTAGE), the retreat
	 * threshold rises by this many shields: the bot breaks off.
	 */
	double outgunned_retreat;
	/* Chasing a target further than this lights the afterburner. */
	double burn_chase_distance;
};

inline constexpr std::array<style_params, BOT_STYLE_COUNT> style_table{{
	/* retreat, engage, collect, range, chase, dodge, mines, strafe, close, behind, outgunned, burn */
	{35, 1.0, 1.0, 1.0, 1.0, 0.0, 1.0, 1.0, 1.0, 1.0, 0, 150},	/* balanced */
	{20, 1.5, 0.6, 0.75, 2.0, 0.0, 2.0, 0.9, 1.15, 1.0, 0, 100},	/* aggressive */
	{55, 0.8, 1.2, 1.25, 0.8, 0.1, 0.7, 1.1, 0.85, 0.8, 25, 200},	/* cautious */
	{40, 0.7, 1.8, 1.0, 1.0, 0.05, 1.0, 1.0, 1.0, 0.5, 15, 150},	/* collector */
}};

[[nodiscard]]
constexpr const style_params &style_of(const bot_style s)
{
	const auto i{static_cast<unsigned>(s)};
	return style_table[i < BOT_STYLE_COUNT ? i : 0];
}

/* Section 9.7: how the style modifies the skill. */
[[nodiscard]]
constexpr double effective_dodge(const skill_params &k, const style_params &s)
{
	return k.dodge_prob > 0 ? std::clamp(k.dodge_prob + s.dodge_bonus, 0.0, 0.95) : 0.0;
}

[[nodiscard]]
constexpr unsigned effective_memory_ms(const skill_params &k, const style_params &s)
{
	return static_cast<unsigned>(k.memory_ms * s.chase_memory + 0.5);
}

[[nodiscard]]
constexpr double effective_strafe_speed(const skill_params &k, const style_params &s)
{
	return k.strafe ? std::min(0.9, k.strafe_speed * s.strafe_scale) : 0.0;
}

/* B1's closing speed in the fight band, a share of the top speed. */
constexpr double COMBAT_CLOSE_SPEED{0.8};

[[nodiscard]]
constexpr double effective_close_speed(const style_params &s)
{
	return std::min(1.0, COMBAT_CLOSE_SPEED * s.close_scale);
}

/* How the bot stands against its target: its shields over the target's
 * times its armament over the target's (armament_score, bot_goals.h),
 * the square root of each (a better gun does not make up for no
 * shields), bounded.  1: even; above: ahead.
 */
[[nodiscard]]
inline double fight_advantage(const double own_shields, const double own_armament, const double target_shields, const double target_armament)
{
	const double shields{std::max(own_shields, 1.0) / std::max(target_shields, 1.0)};
	const double arms{std::max(own_armament, 0.1) / std::max(target_armament, 0.1)};
	return std::clamp(std::sqrt(shields) * std::sqrt(arms), 0.1, 10.0);
}

/* Below this advantage a bot is outgunned (outgunned_retreat). */
constexpr double OUTGUNNED_ADVANTAGE{0.6};

/* The style's engage weight factor for an advantage: 1 when ahead or
 * even, falling to behind_engage at an advantage of one half.
 */
[[nodiscard]]
constexpr double style_engage_factor(const style_params &s, const double advantage)
{
	if (advantage >= 1)
		return 1;
	const double t{std::clamp((advantage - 0.5) / 0.5, 0.0, 1.0)};
	return s.behind_engage + (1 - s.behind_engage) * t;
}

/* The style's retreat threshold for an advantage. */
[[nodiscard]]
constexpr double style_retreat_shields(const style_params &s, const double advantage)
{
	return s.retreat_shields + (advantage < OUTGUNNED_ADVANTAGE ? s.outgunned_retreat : 0);
}

/* Section 3.4: each bot's own random numbers, seeded from the session,
 * the slot and the level; the game's d_rand is never used for decisions.
 * The normal deviates are computed here (Box-Muller) rather than by
 * std::normal_distribution, whose algorithm the standard leaves open.
 */
class bot_rng
{
	std::minstd_rand m_engine;
	double m_spare{};
	bool m_have_spare{};
public:
	explicit bot_rng(const uint32_t seed = 1) :
		m_engine{seed ? seed : 1}
	{
	}
	void seed(const uint32_t s)
	{
		m_engine.seed(s ? s : 1);
		m_have_spare = false;
	}
	uint32_t next()
	{
		/* The engine's result type is uint_fast32_t, which is uint32_t on
		 * Windows (where a cast would be useless) and wider elsewhere;
		 * minstd_rand values always fit in 32 bits.
		 */
		const uint32_t r = m_engine();
		return r;
	}
	/* Uniform in [0, 1). */
	double uniform()
	{
		return static_cast<double>(m_engine() - std::minstd_rand::min()) / (static_cast<double>(std::minstd_rand::max() - std::minstd_rand::min()) + 1);
	}
	double uniform(const double lo, const double hi)
	{
		return lo + (hi - lo) * uniform();
	}
	/* Uniform in [0, n). */
	unsigned below(const unsigned n)
	{
		return n ? static_cast<unsigned>(uniform() * n) % n : 0;
	}
	/* Standard normal. */
	double normal()
	{
		if (m_have_spare)
		{
			m_have_spare = false;
			return m_spare;
		}
		double u1;
		do
			u1 = uniform();
		while (u1 <= 0);
		const double u2{uniform()};
		const double r{std::sqrt(-2 * std::log(u1))};
		const double a{2 * std::numbers::pi * u2};
		m_spare = r * std::sin(a);
		m_have_spare = true;
		return r * std::cos(a);
	}
};

/* A seed that differs for every (session, slot, level). */
[[nodiscard]]
constexpr uint32_t bot_seed(const uint32_t session, const unsigned slot, const int level)
{
	uint64_t z{(static_cast<uint64_t>(session) << 16) ^ (static_cast<uint64_t>(slot) << 8) ^ static_cast<uint64_t>(static_cast<uint32_t>(level)) ^ UINT64_C(0x9e3779b97f4a7c15)};
	z = (z ^ (z >> 30)) * UINT64_C(0xbf58476d1ce4e5b9);
	z = (z ^ (z >> 27)) * UINT64_C(0x94d049bb133111eb);
	z ^= z >> 31;
	const auto s{static_cast<uint32_t>(z % 2147483646u) + 1};
	return s;
}

/* Section 4.4: the time `t` > 0 at which a shot of speed `speed` from
 * the origin meets a target at `rel_pos` moving with `rel_vel`:
 * |rel_pos + rel_vel t| = speed t.  None if the target outruns the shot.
 */
[[nodiscard]]
inline std::optional<double> intercept_time(const vec3 &rel_pos, const vec3 &rel_vel, const double speed)
{
	if (speed <= 0)
		return std::nullopt;
	const double a{dot(rel_vel, rel_vel) - speed * speed};
	const double b{2 * dot(rel_pos, rel_vel)};
	const double c{dot(rel_pos, rel_pos)};
	if (c == 0)
		return 0.0;
	if (std::abs(a) < 1e-12)
	{
		/* The target moves as fast as the shot: one root. */
		if (b >= 0)
			return std::nullopt;
		return -c / b;
	}
	const double disc{b * b - 4 * a * c};
	if (disc < 0)
		return std::nullopt;
	const double sq{std::sqrt(disc)};
	const double t1{(-b - sq) / (2 * a)};
	const double t2{(-b + sq) / (2 * a)};
	const double lo{std::min(t1, t2)};
	const double hi{std::max(t1, t2)};
	if (lo > 0)
		return lo;
	if (hi > 0)
		return hi;
	return std::nullopt;
}

/* Where to aim a shot of speed `speed` from `shooter` at a target at
 * `pos` moving with `vel`, with the lead factor `lead` (0: at the
 * target, 1: exact intercept; aim_lead).  Without an intercept, the lead
 * of the time the shot needs to reach the target's current position.
 *
 * `vel` is the target's velocity in the world, not relative to the
 * shooter: a shot does not inherit its shooter's velocity
 * (Laser_create_new: only the mines do).  `shooter` is the gun the shot
 * leaves from; it flies parallel to the ship's nose.
 */
[[nodiscard]]
inline vec3 aim_point(const vec3 &shooter, const vec3 &pos, const vec3 &vel, const double speed, const double lead)
{
	const auto v{vel * lead};
	const auto rel{pos - shooter};
	if (const auto t{intercept_time(rel, v, speed)})
		return pos + v * *t;
	return speed > 0 ? pos + v * (length(rel) / speed) : pos;
}

/* Section 4.4: the aim error, an angular offset in two axes drawn from
 * N(0, sigma), held for the drift period while the aim eases toward it,
 * so that the aim wanders smoothly instead of jittering.
 */
class aim_error
{
	double m_yaw{}, m_pitch{};
	double m_target_yaw{}, m_target_pitch{};
	unsigned m_left{};
public:
	void reset()
	{
		*this = {};
	}
	/* Once per tick.  `sigma` in radians. */
	void update(bot_rng &rng, const double sigma, const unsigned drift_ticks)
	{
		const unsigned period{std::max(1u, drift_ticks)};
		if (!m_left)
		{
			m_target_yaw = sigma * rng.normal();
			m_target_pitch = sigma * rng.normal();
			m_left = period;
		}
		--m_left;
		const double ease{std::min(1.0, 3.0 / period)};
		m_yaw += (m_target_yaw - m_yaw) * ease;
		m_pitch += (m_target_pitch - m_pitch) * ease;
	}
	[[nodiscard]]
	double yaw() const
	{
		return m_yaw;
	}
	[[nodiscard]]
	double pitch() const
	{
		return m_pitch;
	}
	[[nodiscard]]
	double target_yaw() const
	{
		return m_target_yaw;
	}
	[[nodiscard]]
	double target_pitch() const
	{
		return m_target_pitch;
	}
};

/* Section 4.4, lead accuracy: the factor the bot applies to the
 * target's velocity when it leads a shot (aim_point), 1 on average: its
 * error is drawn from N(0, (1 - lead) x LEAD_ERROR_SCALE), held for the
 * aim drift period and eased toward, as the aim error is.  At lead 0
 * (Trainee) the bot does not lead at all.
 *
 * B1 used the fraction `lead` itself (Hotshot 0.7): a systematic
 * under-lead of 30 % of the shot's flight, which misses a steady strafer
 * by 0.3 x its speed x the flight time.  That grows with the flight
 * time, so the slow shots (the blobs of plasma, phoenix, spreadfire,
 * helix) missed a strafing human far more often than the fast ones
 * (Documentation/multiplayer-bots.md, section 9.3).
 */
constexpr double LEAD_ERROR_SCALE{0.35};

class aim_lead
{
	double m_factor{1}, m_target{1};
	unsigned m_left{};
public:
	void reset()
	{
		*this = {};
	}
	/* Once per tick. */
	void update(bot_rng &rng, const double lead, const unsigned drift_ticks)
	{
		if (!(lead > 0))
		{
			m_factor = m_target = 0;
			m_left = 0;
			return;
		}
		const unsigned period{std::max(1u, drift_ticks)};
		if (!m_left)
		{
			const double sigma{std::max(0.0, 1 - lead) * LEAD_ERROR_SCALE};
			m_target = std::max(0.0, 1 + sigma * rng.normal());
			m_left = period;
		}
		--m_left;
		const double ease{std::min(1.0, 3.0 / period)};
		m_factor += (m_target - m_factor) * ease;
	}
	[[nodiscard]]
	double factor() const
	{
		return m_factor;
	}
};

/* Section 9.7: a beginner's trigger.  With a duty below 1 the trigger
 * is held in bursts of FIRE_BURST_MIN_S to FIRE_BURST_MAX_S and released
 * between them for as long as makes the duty on average.  A duty of 1
 * holds it always and draws no random numbers (Hotshot and better play
 * exactly as before).  Every life (reset) starts with a burst, not a
 * pause.
 */
constexpr double FIRE_BURST_MIN_S{0.5};
constexpr double FIRE_BURST_MAX_S{1.1};

class fire_burst
{
	bool m_on{true};
	/* The first burst was drawn (since the reset, at a duty below 1). */
	bool m_started{};
	uint32_t m_left{};
public:
	void reset()
	{
		*this = {};
	}
	/* Once per tick. */
	void update(bot_rng &rng, const double duty)
	{
		if (!(duty < 1))
		{
			m_on = true;
			m_started = false;
			m_left = 0;
			return;
		}
		if (!m_started)
		{
			m_started = true;
			m_on = true;
			m_left = static_cast<uint32_t>(rng.uniform(FIRE_BURST_MIN_S, FIRE_BURST_MAX_S) * BOT_TICK_RATE + 0.5);
			return;
		}
		if (m_left)
		{
			--m_left;
			return;
		}
		const double d{std::clamp(duty, 0.05, 1.0)};
		const double burst{rng.uniform(FIRE_BURST_MIN_S, FIRE_BURST_MAX_S)};
		if (m_on)
		{
			/* The burst is over: the pause that makes the duty. */
			m_on = false;
			m_left = static_cast<uint32_t>(burst * (1 - d) / d * BOT_TICK_RATE + 0.5);
		}
		else
		{
			m_on = true;
			m_left = static_cast<uint32_t>(burst * BOT_TICK_RATE + 0.5);
		}
	}
	[[nodiscard]]
	bool on() const
	{
		return m_on;
	}
};

/* `dir` turned by `yaw` about the up axis and `pitch` about the right
 * axis of a frame whose forward is `dir` and whose up is near `up_hint`.
 */
[[nodiscard]]
inline vec3 apply_aim_offset(const vec3 &dir, const vec3 &up_hint, const double yaw, const double pitch)
{
	const auto f{normalized(dir)};
	auto r{normalized(cross(up_hint, f))};
	if (r == vec3{})
		r = normalized(cross(vec3{0, 0, 1}, f));
	if (r == vec3{})
		r = vec3{1, 0, 0};
	const auto u{cross(f, r)};
	return normalized(f + r * std::tan(yaw) + u * std::tan(pitch));
}

/* Section 4.2: the tactics layer reads what the bot perceived
 * `reaction_ticks` ago.  A ring of the last N perceptions.
 */
template <typename T, std::size_t N>
class delay_line
{
	std::array<T, N> m_items{};
	std::size_t m_head{};
	std::size_t m_count{};
public:
	void clear()
	{
		m_head = 0;
		m_count = 0;
	}
	void push(const T &v)
	{
		m_head = (m_head + 1) % N;
		m_items[m_head] = v;
		if (m_count < N)
			++m_count;
	}
	[[nodiscard]]
	std::size_t size() const
	{
		return m_count;
	}
	/* The item pushed `k` pushes before the newest (0: the newest),
	 * or the oldest one kept if fewer were pushed; nullptr if empty.
	 */
	[[nodiscard]]
	const T *delayed(const std::size_t k) const
	{
		if (!m_count)
			return nullptr;
		const auto back{std::min(k, m_count - 1)};
		return &m_items[(m_head + N - back) % N];
	}
};

/* What a bot knows about one other player. */
struct target_memory
{
	bool valid{};
	vec3 pos;
	vec3 vel;
	uint16_t segment{};
	uint32_t tick{};
};

/* Section 4.2: confidence falls linearly from 1 to 0 over the memory
 * time.
 */
[[nodiscard]]
inline double memory_confidence(const target_memory &m, const uint32_t now, const unsigned memory_ticks)
{
	if (!m.valid || !memory_ticks)
		return 0;
	const double age{static_cast<double>(now - m.tick)};
	return std::clamp(1 - age / memory_ticks, 0.0, 1.0);
}

[[nodiscard]]
inline bool in_field_of_view(const vec3 &fvec, const vec3 &to_target, const double half_angle_deg)
{
	const auto d{normalized(to_target)};
	if (d == vec3{})
		return true;
	return dot(normalized(fvec), d) >= std::cos(radians(half_angle_deg)) - 1e-9;
}

/* Section 4.4: target scoring. */
struct target_candidate
{
	uint8_t id{};
	/* Teammates, ghosts, the bot itself: never a target. */
	bool excluded{};
	bool visible{};
	double confidence{};
	double distance{};
	bool damaged_me_recently{};
	bool low_shields{};
	bool bounty{};
	/* Section 9.10: the target the bot pursues round a corner (out of
	 * sight: it scores at least PURSUIT_TARGET_SCORE, not half its
	 * fading confidence).
	 */
	bool pursued{};
};

/* Section 9.10: an unseen target under pursuit scores this (a visible
 * one 1).  B1 scored every unseen target at half its confidence, so at
 * the moment it broke the line of sight a target lost half its score:
 * any other enemy in sight (1 times its range factor, at least 0.3)
 * beat it, and its hunt was worth half the engagement (the exp-19 log:
 * a hunt of median 1.0 against the grab's 4-6.5).
 */
constexpr double PURSUIT_TARGET_SCORE{0.9};

[[nodiscard]]
inline double target_score(const target_candidate &c, const double awareness_radius)
{
	if (c.excluded)
		return 0;
	double s{c.visible ? 1.0 : c.pursued ? std::max(PURSUIT_TARGET_SCORE, c.confidence * 0.5) : c.confidence * 0.5};
	if (s <= 0)
		return 0;
	const double close{awareness_radius * 0.2};
	const double span{std::max(awareness_radius - close, 1.0)};
	const double range_factor{1 - 0.7 * std::clamp((c.distance - close) / span, 0.0, 1.0)};
	s *= range_factor;
	if (c.damaged_me_recently)
		s *= 1.5;
	if (c.low_shields)
		s *= 1.3;
	if (c.bounty)
		s *= 2;
	return s;
}

/* The best candidate; the current target's score counts `hysteresis`
 * more, so that two close scores do not make the bot flip between
 * targets.  Ties go to the lower id.  None if no candidate scores.
 */
[[nodiscard]]
inline std::optional<uint8_t> choose_target(const std::span<const target_candidate> candidates, const std::optional<uint8_t> current, const double awareness_radius, const double hysteresis = 0.2)
{
	std::optional<uint8_t> best;
	double best_score{0};
	for (const auto &c : candidates)
	{
		double s{target_score(c, awareness_radius)};
		if (s <= 0)
			continue;
		if (current && c.id == *current)
			s *= 1 + hysteresis;
		if (!best || s > best_score || (s == best_score && c.id < *best))
		{
			best = c.id;
			best_score = s;
		}
	}
	return best;
}

/* Section 3.4, steering: the angle errors (radians) toward a direction
 * given in the ship's frame (right, up, forward).  Positive heading
 * turns right, positive pitch turns the nose down (the signs of the
 * game's rotational thrust).
 *
 * The errors are the components of the shortest rotation that brings
 * the nose onto the direction (its angle times its unit axis), not the
 * heading and pitch angles of the direction: those change wildly for a
 * direction near straight up or down (the heading of a direction just
 * above the nose swings by 180 degrees when it passes to the other
 * side), which made the ship shake left and right under a target above
 * it.  A direction almost straight behind keeps turning the way it
 * turned (`prefer_heading_sign`), so the bot does not dither between left
 * and right.
 */
struct steer_errors
{
	double pitch{};
	double heading{};
};

/* Section 9.5, turning round: the ship turns about its pitch and heading
 * axes each at up to its own top rate (physics.cpp applies rotthrust per
 * axis), so a turn about both at once is up to 1.41 times as fast as a
 * turn about one.  For a direction far behind the nose the shortest
 * rotation is ill-defined anyway (a small change of the direction swings
 * its axis round), and B1-B4 often took it about the pitch axis alone:
 * a slow loop in place.  Beyond LARGE_TURN_START the errors blend toward
 * both axes at once (full beyond LARGE_TURN_FULL), in the direction the
 * target lies (or the preferred one, straight behind); as the nose comes
 * round, the shortest rotation takes over again.
 */
constexpr double LARGE_TURN_START{2.0};	// radians, about 115 degrees
constexpr double LARGE_TURN_FULL{2.5};	// about 143 degrees

[[nodiscard]]
inline steer_errors steer_errors_local(const vec3 &local_dir, const int prefer_heading_sign = 0, const int prefer_pitch_sign = 0, const bool combined_large_turns = true);

/* B1's errors: the shortest rotation only (kept for the flight test's
 * comparison).
 */
[[nodiscard]]
inline steer_errors steer_errors_shortest(const vec3 &local_dir, const int prefer_heading_sign = 0)
{
	const auto d{normalized(local_dir)};
	if (d == vec3{})
		return {};
	/* The rotation axis of the nose (0, 0, 1) onto d is (-d.y, d.x, 0):
	 * a right-handed rotation about the right axis lowers the nose
	 * (positive pitch), one about the up axis turns it right (positive
	 * heading).
	 */
	const double angle{std::acos(std::clamp(d.z, -1.0, 1.0))};
	const double side{std::hypot(d.x, d.y)};
	steer_errors e;
	if (side > 1e-9)
	{
		e.pitch = angle * -d.y / side;
		e.heading = angle * d.x / side;
	}
	else if (d.z < 0)
		e.heading = (prefer_heading_sign < 0 ? -1 : 1) * angle;
	if (prefer_heading_sign && d.z < 0 && std::abs(d.x) < 0.2 * -d.z && std::abs(d.y) < 0.2 * -d.z && (e.heading > 0) != (prefer_heading_sign > 0))
	{
		/* Nearly behind: turn the preferred way round, by the angle to go. */
		e.heading = prefer_heading_sign * angle;
		e.pitch = 0;
	}
	return e;
}

/* Section 9.5: the errors of a turn, combined on both axes beyond
 * LARGE_TURN_START (see above).
 */
inline steer_errors steer_errors_local(const vec3 &local_dir, const int prefer_heading_sign, const int prefer_pitch_sign, const bool combined_large_turns)
{
	auto e{steer_errors_shortest(local_dir, prefer_heading_sign)};
	const auto d{normalized(local_dir)};
	const double angle{std::acos(std::clamp(d.z, -1.0, 1.0))};
	if (!combined_large_turns || d == vec3{} || angle <= LARGE_TURN_START)
		return e;
	/* Both axes at once, each by the angle to go over the square root
	 * of two, in the direction the target lies.
	 */
	const double w{std::clamp((angle - LARGE_TURN_START) / (LARGE_TURN_FULL - LARGE_TURN_START), 0.0, 1.0)};
	const int hs{std::abs(d.x) > 0.05 || !prefer_heading_sign ? (d.x < 0 ? -1 : 1) : prefer_heading_sign};
	const int ps{std::abs(d.y) > 0.05 ? (d.y > 0 ? -1 : 1) : (prefer_pitch_sign > 0 ? 1 : -1)};
	const double k{angle / std::numbers::sqrt2};
	e.heading = (1 - w) * e.heading + w * hs * k;
	e.pitch = (1 - w) * e.pitch + w * ps * k;
	return e;
}

/* The ship's rotational response to a full rotation axis: it tends to
 * `max_rate` (radians per second) with time constant `time_constant`
 * (seconds): d(rate)/dt = (axis * max_rate - rate) / time_constant.
 */
struct turn_response
{
	double max_rate{};
	double time_constant{};
};

/* The rotation axis in [-cap, cap] that turns an angle error `error`
 * (radians) away at the current rate `rate` (radians per second).  A
 * cascaded controller: the wanted rate is proportional to the error
 * (critically damped for the ship's time constant) plus `feed_forward`,
 * the rate at which the wanted direction itself turns (radians per
 * second, about the same axis), and the axis adds rate feedback to the
 * feed-forward, so the ship neither overshoots nor crawls.  Without the
 * feed-forward the ship lags a moving target by (its angular rate x the
 * time constant): 9 degrees behind a player strafing at 50 units per
 * second at 60 units, outside every fire cone, so the bots almost never
 * fired.  It uses the current state only, so its result for a given
 * state does not depend on the frame length.
 */
[[nodiscard]]
inline double rotation_axis(const double error, const double rate, const turn_response &ship, const double cap, const double feed_forward = 0)
{
	if (ship.max_rate <= 0)
		return 0;
	constexpr double feedback{3};
	const double tau{std::max(ship.time_constant, 1e-3)};
	const double kp{(1 + feedback) / (4 * tau)};
	const double wmax{ship.max_rate * cap};
	const double wanted{std::clamp(kp * error + feed_forward, -wmax, wmax)};
	const double axis{(wanted + feedback * (wanted - rate)) / ship.max_rate};
	return std::clamp(axis, -cap, cap);
}

/* The angular velocity (radians per second, world axes) of the line
 * from a point to a target at `rel_pos` from it moving at `rel_vel`
 * relative to it: the steering's feed-forward.
 */
[[nodiscard]]
inline vec3 line_of_sight_rate(const vec3 &rel_pos, const vec3 &rel_vel)
{
	const double d2{dot(rel_pos, rel_pos)};
	if (d2 < 1)
		return {};
	return cross(rel_pos, rel_vel) * (1 / d2);
}

/* The thrust command (world direction, length at most 1) that brings the
 * velocity `vel` to `wanted`, for a ship whose full thrust holds
 * `max_speed`.
 */
[[nodiscard]]
inline vec3 velocity_command(const vec3 &wanted, const vec3 &vel, const double max_speed)
{
	if (max_speed <= 0)
		return {};
	auto c{(wanted + (wanted - vel) * 2.0) * (1 / max_speed)};
	const double l{length(c)};
	if (l > 1)
		c *= 1 / l;
	return c;
}

/* Section 3.4: one frame of the steering, from the direction and the
 * thrust the last tick chose and the ship's current state.
 */
struct steer_input
{
	/* The ship's orientation without the turn roll: the frame in which
	 * do_physics_sim_rot applies the rotation.
	 */
	frame3 unrolled;
	/* The orientation with it: the frame in which apply_pilot_controls
	 * applies the thrust.
	 */
	frame3 thrust_frame;
	/* The wanted forward direction and its angular velocity (world). */
	vec3 face_dir{0, 0, 1};
	vec3 face_rate;
	/* The thrust command (world, length at most 1). */
	vec3 move_cmd;
	/* The current pitch and heading rates, radians per second. */
	double pitch_rate{}, heading_rate{};
	/* B1's turn (the flight test's comparison). */
	bool shortest_only{};
};

struct steer_output
{
	double pitch{}, heading{}, forward{}, sideways{}, vertical{};
};

[[nodiscard]]
inline steer_output steer_controls(const steer_input &in, const turn_response &ship, const double cap, int &heading_pref)
{
	steer_output c;
	/* Straight behind, the pitch goes the way it already turns, else
	 * the nose goes up.
	 */
	const int pitch_pref{in.pitch_rate > 0.2 ? 1 : -1};
	const auto e{in.shortest_only ? steer_errors_shortest(in.unrolled.to_local(in.face_dir), heading_pref) : steer_errors_local(in.unrolled.to_local(in.face_dir), heading_pref, pitch_pref)};
	if (std::abs(e.heading) > 0.2)
		heading_pref = e.heading > 0 ? 1 : -1;
	c.pitch = rotation_axis(e.pitch, in.pitch_rate, ship, cap, dot(in.face_rate, in.unrolled.r));
	c.heading = rotation_axis(e.heading, in.heading_rate, ship, cap, dot(in.face_rate, in.unrolled.u));
	const auto thrust{in.thrust_frame.to_local(in.move_cmd)};
	c.sideways = std::clamp(thrust.x, -1.0, 1.0);
	c.vertical = std::clamp(thrust.y, -1.0, 1.0);
	c.forward = std::clamp(thrust.z, -1.0, 1.0);
	return c;
}

/* Section 4.6: the speed toward the target (negative: away) that keeps
 * the distance within [lo, hi].
 */
[[nodiscard]]
inline double range_keeping_speed(const double dist, const double lo, const double hi, const double max_speed)
{
	if (dist > hi)
		return std::min(max_speed, (dist - hi) * 2);
	if (dist < lo)
		return -std::min(max_speed, (lo - dist) * 2);
	return 0;
}

/* Section 4.6: the lateral strafe direction, flipped after a random time
 * in [min_ticks, max_ticks].
 */
class strafe_state
{
	int m_dir{1};
	unsigned m_left{};
public:
	void reset()
	{
		*this = {};
	}
	/* Once per tick; returns -1 or 1. */
	int update(bot_rng &rng, const unsigned min_ticks, const unsigned max_ticks)
	{
		if (!m_left)
		{
			m_dir = -m_dir;
			const unsigned lo{std::max(1u, min_ticks)};
			const unsigned hi{std::max(lo, max_ticks)};
			m_left = lo + rng.below(hi - lo + 1);
		}
		--m_left;
		return m_dir;
	}
	[[nodiscard]]
	int direction() const
	{
		return m_dir;
	}
};

/* Section 4.6, movement in a fight.  B1 first kept still inside the
 * 35-95 unit band and strafed straight left and right, reversing every
 * 0.4-1.2 s: the ship never reached speed before it turned back, so two
 * bots fighting each other shook on one spot.  Now each run of the strafe
 * takes a new direction across the line of sight, at least 90 degrees
 * from the last (so the bot circles, climbs and dives rather than
 * swinging), with a vertical share (`vertical`: 0 flat, 1 as much as
 * sideways), and a new preferred distance inside the band, so the bot
 * also closes in and backs off.
 */
class juke_state
{
	double m_angle{};
	double m_range{};
	unsigned m_left{};
public:
	void reset()
	{
		*this = {};
	}
	/* Once per tick. */
	void update(bot_rng &rng, const unsigned min_ticks, const unsigned max_ticks, const double lo, const double hi)
	{
		if (m_left && m_range >= lo && m_range <= hi)
		{
			--m_left;
			return;
		}
		m_angle = std::remainder(m_angle + rng.uniform(radians(90), radians(270)), 2 * std::numbers::pi);
		m_range = rng.uniform(lo, hi);
		const unsigned a{std::max(1u, min_ticks)};
		const unsigned b{std::max(a, max_ticks)};
		m_left = a + rng.below(b - a + 1);
	}
	/* The angle of the strafe about the line of sight, from the right. */
	[[nodiscard]]
	double angle() const
	{
		return m_angle;
	}
	/* The preferred distance to the target. */
	[[nodiscard]]
	double range() const
	{
		return m_range;
	}
	[[nodiscard]]
	unsigned ticks_left() const
	{
		return m_left;
	}
};

/* The velocity a fighting bot wants: toward or away from the target to
 * reach the juke's preferred distance (at most `approach_speed`), plus
 * the strafe across the line of sight (`strafe_speed`, 0 for none) in the
 * juke's direction, taken in the frame (`right`, `up`) of the ship.
 */
[[nodiscard]]
inline vec3 combat_velocity(const vec3 &to_target, const vec3 &right, const vec3 &up, const juke_state &juke, const double vertical, const double approach_speed, const double strafe_speed)
{
	const auto d{normalized(to_target)};
	if (d == vec3{})
		return {};
	const double dist{length(to_target)};
	const double approach{std::clamp((dist - juke.range()) * 1.5, -approach_speed, approach_speed)};
	/* The ship's right and up, made perpendicular to the line of sight. */
	auto r{normalized(right - d * dot(right, d))};
	if (r == vec3{})
		r = normalized(cross(up, d));
	const auto u{cross(d, r)};
	const auto lateral{normalized(r * std::cos(juke.angle()) + u * (std::sin(juke.angle()) * vertical))};
	return d * approach + lateral * strafe_speed;
}

/* Section 9.5: a bot that turns far round (to face a target behind it)
 * keeps moving, as a human does: a ship that stops to turn is the
 * easiest target there is.  When the nose is more than
 * TURN_MOVE_ANGLE off the wanted direction and the bot wants less than
 * TURN_MOVE_SPEED of the top speed, it slides across the line to the
 * target (`lateral`: the way it already slides, or its own choice) at
 * that speed, keeping what it wanted besides.
 */
constexpr double TURN_MOVE_ANGLE{1.05};	// 60 degrees
constexpr double TURN_MOVE_SPEED{0.6};

[[nodiscard]]
inline vec3 keep_moving_in_turn(const vec3 &wanted, const double face_error, const vec3 &to_target, const vec3 &vel, const vec3 &lateral_hint, const double max_speed)
{
	if (face_error <= TURN_MOVE_ANGLE || max_speed <= 0)
		return wanted;
	const double min_speed{TURN_MOVE_SPEED * max_speed};
	const double have{length(wanted)};
	if (have >= min_speed)
		return wanted;
	const auto los{normalized(to_target)};
	/* Across the line of sight: the way the ship already slides, if it
	 * does, else the hint.
	 */
	auto across{vel - los * dot(vel, los)};
	if (length(across) < 5)
		across = lateral_hint - los * dot(lateral_hint, los);
	across = normalized(across);
	if (across == vec3{})
		return wanted;
	const double need{std::sqrt(std::max(min_speed * min_speed - have * have, 0.0))};
	return wanted + across * need;
}

/* Section 9.8: turning round to a target behind, as a human does it.
 * "During the turn I switch from flying forward to flying backwards (and
 * usually after the turn I boost forward towards the new target)."  The
 * bot keeps (or takes) its momentum away from the target while it turns:
 * it wants to fly away from it, so the thrust that holds that velocity
 * is forward while the nose points away and becomes reverse as the nose
 * comes round (velocity_command works in the world; the ship's axes
 * turn under it), with a little of the slide across the line of sight
 * so that it is not a still target on the line.  Once it faces the
 * target (REVERSE_TURN_FACING) it boosts forward toward it for
 * TURN_BOOST_TICKS (with the afterburner from Hotshot), unless the
 * target is already inside the near edge of its fight band.  It starts
 * only for a target more than REVERSE_TURN_START off the nose; below
 * that keep_moving_in_turn's slide applies.
 */
constexpr double REVERSE_TURN_START{1.92};	// 110 degrees
constexpr double REVERSE_TURN_FACING{0.45};	// 26 degrees
constexpr double REVERSE_TURN_SPEED{0.8};
constexpr double REVERSE_TURN_ACROSS{0.4};
constexpr unsigned REVERSE_TURN_MAX_TICKS{ticks_from_ms(2500)};
constexpr unsigned TURN_BOOST_TICKS{ticks_from_ms(800)};
constexpr double TURN_BOOST_MARGIN{15};

enum class turn_phase : uint8_t
{
	none,
	reversing,
	boost,
};

struct turn_round_state
{
	turn_phase phase{turn_phase::none};
	uint32_t since{};
	uint32_t until{};
	void reset()
	{
		*this = {};
	}
	/* `face_error`: the nose off the target (radians); `distance` to it;
	 * `near_edge`: the near edge of the fight band (no boost inside it).
	 */
	turn_phase update(const double face_error, const uint32_t tick, const double distance, const double near_edge)
	{
		switch (phase)
		{
			case turn_phase::none:
				if (face_error > REVERSE_TURN_START)
				{
					phase = turn_phase::reversing;
					since = tick;
				}
				break;
			case turn_phase::reversing:
				if (face_error <= REVERSE_TURN_FACING)
				{
					if (distance > near_edge + TURN_BOOST_MARGIN)
					{
						phase = turn_phase::boost;
						until = tick + TURN_BOOST_TICKS;
					}
					else
						phase = turn_phase::none;
				}
				else if (tick - since > REVERSE_TURN_MAX_TICKS)
					phase = turn_phase::none;
				break;
			case turn_phase::boost:
				if (tick >= until || face_error > TURN_MOVE_ANGLE || distance <= near_edge)
					phase = turn_phase::none;
				/* A new target behind: turn round again. */
				if (face_error > REVERSE_TURN_START)
				{
					phase = turn_phase::reversing;
					since = tick;
				}
				break;
		}
		return phase;
	}
};

/* The velocity the bot wants in each phase (`wanted` outside them). */
[[nodiscard]]
inline vec3 turn_round_velocity(const turn_phase phase, const vec3 &wanted, const vec3 &to_target, const vec3 &vel, const vec3 &lateral_hint, const double max_speed)
{
	const auto los{normalized(to_target)};
	if (los == vec3{} || max_speed <= 0)
		return wanted;
	switch (phase)
	{
		case turn_phase::none:
			break;
		case turn_phase::reversing:
		{
			auto across{vel - los * dot(vel, los)};
			if (length(across) < 5)
				across = lateral_hint - los * dot(lateral_hint, los);
			across = normalized(across);
			const auto dir{normalized(-los + across * REVERSE_TURN_ACROSS)};
			const double speed{std::max(REVERSE_TURN_SPEED * max_speed, dot(wanted, -los))};
			return dir * std::min(speed, max_speed);
		}
		case turn_phase::boost:
			return los * max_speed;
	}
	return wanted;
}

/* The PR #38 review: flying backwards, the bot does not see the wall
 * behind it; the reverse turn needs REVERSE_TURN_CLEARANCE units clear
 * along the velocity it wants, else it slides round (keep_moving_in_turn)
 * while the nose comes round.
 */
constexpr double REVERSE_TURN_CLEARANCE{25};

[[nodiscard]]
constexpr bool reverse_turn_has_room(const double clearance)
{
	return clearance >= REVERSE_TURN_CLEARANCE;
}

/* Section 9.10, corner clearing: a bot that pursues a target round a
 * corner does not fly straight to the place it lost it (into the line
 * of fire of a target waiting behind the corner).  It "slices the pie":
 * it flies to a point short of the corner (`keep`) and swung wide of it
 * (`swing`, away from the side the target turned to), and faces the
 * corner's exit (the corner plus CORNER_AIM_AHEAD along the target's
 * way), so that the view round the corner opens from a distance, with
 * the guns on it.  Nearer the corner than `keep`, or with no way known
 * (no velocity), it goes on (the path).
 */
constexpr double CORNER_APPROACH_RANGE{110};
constexpr double CORNER_AIM_AHEAD{25};
constexpr double CORNER_SWING_SHARE{0.8};
/* The peek ends when the point is reached or after this long. */
constexpr double CORNER_PEEK_REACHED{8};
constexpr double CORNER_PEEK_SECONDS{1.5};

struct corner_approach
{
	/* Peek: fly to `point` facing `aim`; else go on along the path. */
	bool peek{};
	vec3 point;
	vec3 aim;
};

/* The distance kept from the corner by style (Balanced, Aggressive,
 * Cautious, Collector); from Hotshot (below: 0, straight at it, as a
 * beginner does).
 */
[[nodiscard]]
constexpr double corner_keep(const bot_skill k, const bot_style s)
{
	if (static_cast<unsigned>(k) < static_cast<unsigned>(bot_skill::hotshot))
		return 0;
	constexpr std::array<double, BOT_STYLE_COUNT> by_style{{22, 14, 32, 26}};
	const auto i{static_cast<unsigned>(s)};
	return by_style[i < BOT_STYLE_COUNT ? i : 0];
}

[[nodiscard]]
inline corner_approach corner_approach_point(const vec3 &bot, const vec3 &corner, const vec3 &target_vel, const double keep, const double swing_share = CORNER_SWING_SHARE)
{
	corner_approach r;
	r.point = corner;
	const auto way{normalized(target_vel)};
	r.aim = corner + way * CORNER_AIM_AHEAD;
	const auto to{corner - bot};
	const double d{length(to)};
	if (keep <= 0 || way == vec3{} || d <= keep || d > CORNER_APPROACH_RANGE)
		return r;
	const auto dir{to * (1 / d)};
	/* The side the target turned to, across the bot's approach. */
	const auto across{way - dir * dot(way, dir)};
	const double a{length(across)};
	vec3 swing{};
	if (a > 0.2)
		swing = across * (-keep * swing_share / a);
	r.peek = true;
	r.point = corner - dir * keep + swing;
	return r;
}

/* Section 9.5: a hit from an attacker the bot does not see (behind it,
 * outside its field of view).  B1-B4 learnt the attacker's place from the
 * hit and turned to it after the reaction time, the target choice (5 Hz)
 * and a turn at the skill's rate: standing still meanwhile, the bot died
 * before it faced its attacker.  Now it thrusts away across the line of
 * fire at once (a reflex), turns to the attacker a reaction time later,
 * then fights (the target choice) or, weak, retreats (the goal choice).
 */
enum class hit_reaction : uint8_t
{
	/* It sees the attacker: nothing new, it is fighting it. */
	none,
	/* Evade, then turn to fight. */
	evade_turn,
	/* Evade, then turn only while it flees (weak). */
	evade_flee,
};

struct hit_view
{
	/* The attacker is in sight now and within the field of view. */
	bool attacker_seen{};
	double shields{100};
	double retreat_shields{35};
	bool invulnerable{};
	/* Already evading an earlier hit. */
	bool evading{};
};

[[nodiscard]]
constexpr hit_reaction react_to_hit(const hit_view &v)
{
	if (v.attacker_seen || v.evading)
		return hit_reaction::none;
	if (!v.invulnerable && v.shields < v.retreat_shields)
		return hit_reaction::evade_flee;
	return hit_reaction::evade_turn;
}

/* The evasion: across the line from the attacker, the side the ship
 * already moves to (momentum), else `side` (the bot's roll), with a
 * little away from the attacker.
 */
[[nodiscard]]
inline vec3 evade_direction(const vec3 &from_attacker, const vec3 &vel, const vec3 &side)
{
	const auto away{normalized(from_attacker)};
	if (away == vec3{})
		return normalized(side);
	auto across{vel - away * dot(vel, away)};
	if (length(across) < 5)
		across = side - away * dot(side, away);
	across = normalized(across);
	if (across == vec3{})
		across = normalized(cross(away, vec3{0, 1, 0}));
	if (across == vec3{})
		across = vec3{1, 0, 0};
	return normalized(across + away * 0.3);
}

/* Section 4.6, dodge: a projectile at `rel_pos` from the ship moving at
 * `rel_vel` relative to it passes nearest within `horizon` seconds; if
 * that is closer than `radius`, the direction to thrust (unit, across
 * the projectile's flight, away from where it passes), else none.
 */
[[nodiscard]]
inline std::optional<vec3> dodge_direction(const vec3 &rel_pos, const vec3 &rel_vel, const double horizon, const double radius, const vec3 &fallback)
{
	const double v2{dot(rel_vel, rel_vel)};
	if (v2 < 1)
		return std::nullopt;
	const double t{-dot(rel_pos, rel_vel) / v2};
	if (t <= 0 || t > horizon)
		return std::nullopt;
	const auto closest{rel_pos + rel_vel * t};
	if (length(closest) >= radius)
		return std::nullopt;
	const auto along{rel_vel * (1 / std::sqrt(v2))};
	/* Away from where it passes; straight at the ship: to the side. */
	auto away{normalized(-closest - along * dot(-closest, along))};
	if (away == vec3{} || length(closest) < 0.5)
	{
		away = normalized(fallback - along * dot(fallback, along));
		if (away == vec3{})
			away = normalized(cross(along, vec3{0, 1, 0}));
		if (away == vec3{})
			away = vec3{1, 0, 0};
	}
	return away;
}

/* Section 4.6, dodge: whether a projectile is worth dodging.  Not the
 * bot's own, and not one of a partner's (a teammate in a team game, anyone
 * in cooperative) when friendly fire is off: it cannot hurt the bot.
 */
[[nodiscard]]
constexpr bool shot_worth_dodging(const bool own, const bool from_partner, const bool friendly_fire)
{
	return !own && (friendly_fire || !from_partner);
}

/* Section 4.6, dodge: the bot's roll for one projectile, uniform in
 * [0, 1), a hash of the bot's salt (drawn from its random numbers once per
 * life) and the projectile's signature.  Each projectile thus gets exactly
 * one roll however often it is judged, however many are in the air, with
 * no list of judged projectiles to overflow.
 */
[[nodiscard]]
constexpr double dodge_roll(const uint32_t salt, const uint16_t signature)
{
	uint64_t z{(static_cast<uint64_t>(salt) << 16 | signature) + UINT64_C(0x9e3779b97f4a7c15)};
	z = (z ^ (z >> 30)) * UINT64_C(0xbf58476d1ce4e5b9);
	z = (z ^ (z >> 27)) * UINT64_C(0x94d049bb133111eb);
	z ^= z >> 31;
	return static_cast<double>(z >> 11) * 0x1.0p-53;
}

/* Section 4.3, wall avoidance: a wall that the probe along the velocity
 * meets is the bend the path is about to take, not an obstacle, when the
 * point the bot steers at is nearer than the wall and the bot flies
 * toward that point (within `max_angle`): a velocity that points
 * elsewhere measures a wall that has nothing to do with the steer point.
 */
[[nodiscard]]
inline bool wall_hit_is_bend(const vec3 &pos, const vec3 &vel, const vec3 &steer_point, const double hit_distance, const double max_angle)
{
	const auto to{steer_point - pos};
	const double d{length(to)};
	if (!(d < hit_distance) || d <= 0 || length(vel) <= 0)
		return false;
	return std::acos(std::clamp(dot(to, vel) / (d * length(vel)), -1.0, 1.0)) < max_angle;
}

/* Section 3.4: the time an axis is held for a frame, in fix, as the
 * human's keys give it (FrameTime for a full axis); rounded, not
 * truncated toward minus infinity as fixmul would, so that at 500 fps
 * (FrameTime 131) a small axis is not biased.
 */
[[nodiscard]]
inline int32_t held_axis_time(const double axis, const int32_t frame_time)
{
	return static_cast<int32_t>(std::lround(std::clamp(axis, -1.0, 1.0) * frame_time));
}

/* Section 4.4: a weapon that fires a pattern (spreadfire, helix) hits
 * with its outer shots a little beside the aim: its fire cone is wider
 * by half the pattern's half angle.
 */
[[nodiscard]]
constexpr double fire_cone_with_spread(const double fire_cone, const double spread_half_angle)
{
	return fire_cone + 0.5 * std::max(0.0, spread_half_angle);
}

/* The effective speed of a shot for the lead: a weapon with thrust (the
 * missiles) starts at half its speed and accelerates to it
 * (Laser_create_new), about three quarters of it on average over a
 * fight's distances.
 */
[[nodiscard]]
constexpr double effective_shot_speed(const double speed, const bool thrust)
{
	return thrust ? speed * 0.75 : speed;
}

/* Section 4.4: trigger discipline. */
[[nodiscard]]
inline bool should_fire(const double aim_angle, const double fire_cone, const bool shot_clear, const double dist, const double max_range)
{
	return shot_clear && aim_angle <= fire_cone && dist <= max_range;
}

/* The angle (radians) between two directions. */
[[nodiscard]]
inline double angle_between(const vec3 &a, const vec3 &b)
{
	const auto na{normalized(a)}, nb{normalized(b)};
	if (na == vec3{} || nb == vec3{})
		return 0;
	return std::acos(std::clamp(dot(na, nb), -1.0, 1.0));
}

/* The primary weapons, in the game's order (primary_weapon_index). */
enum class primary : uint8_t
{
	laser,
	vulcan,
	spreadfire,
	plasma,
	fusion,
	super_laser,
	gauss,
	helix,
	phoenix,
	omega,
};

/* What the primary choice looks at (in stage B1: no range bands yet,
 * those come with the weapon tables of B3).
 */
struct primary_view
{
	/* Bit n: primary n owned (HAS_PRIMARY_FLAG). */
	uint16_t owned{1};
	double energy{};
	unsigned vulcan_ammo{};
};

/* Stage B1: the best primary the bot can fire now.  Fusion (it needs
 * the charge the human's trigger gives it) and omega (its charge model
 * is the human's) are left out; the super lasers are the laser (the game
 * selects them by laser level).  Low on energy, the ammunition cannons
 * come first.  The laser if nothing else works.
 */
[[nodiscard]]
inline primary choose_primary(const primary_view &v)
{
	const auto owned{[&](const primary p) {
		return (v.owned >> static_cast<unsigned>(p)) & 1;
	}};
	constexpr double energy_low{10};
	constexpr double energy_min{1};
	const bool ammo{v.vulcan_ammo > 0};
	const bool energy{v.energy >= energy_min};
	if (v.energy < energy_low && ammo)
	{
		if (owned(primary::gauss))
			return primary::gauss;
		if (owned(primary::vulcan))
			return primary::vulcan;
	}
	if (energy)
	{
		for (const auto p : {primary::helix, primary::plasma, primary::spreadfire})
			if (owned(p))
				return p;
	}
	if (ammo)
	{
		if (owned(primary::gauss))
			return primary::gauss;
		if (owned(primary::vulcan))
			return primary::vulcan;
	}
	if (energy && owned(primary::phoenix))
		return primary::phoenix;
	return primary::laser;
}

/* Section 2.3: a player slot as the bots' slot allocation sees it. */
struct slot_view
{
	/* A player is in the slot (connected, or a bot already placed). */
	bool occupied{};
	/* The slot has a connection, even one that is closing (a lobby
	 * player left out of the game, a kicked player's linger): the
	 * connection's end would disconnect whoever is in the slot.
	 */
	bool has_peer{};
	/* A lobby player's callsign still holds the slot. */
	bool reserved{};
};

/* The lowest slot a bot can take below the player limit (slot 0 is the
 * host's), or none.
 */
[[nodiscard]]
constexpr std::optional<unsigned> choose_bot_slot(const std::span<const slot_view> slots, const unsigned limit)
{
	const auto n{std::min<std::size_t>(slots.size(), limit)};
	for (unsigned i = 1; i < n; ++i)
	{
		const auto &v{slots[i]};
		if (!v.occupied && !v.has_peer && !v.reserved)
			return i;
	}
	return std::nullopt;
}

/* The host flies the ship in a slot only if a bot was placed there and
 * no human has a connection for it: a slot with a peer is never a bot.
 */
[[nodiscard]]
constexpr bool slot_flown_by_bot(const bool bot_placed, const bool has_peer)
{
	return bot_placed && !has_peer;
}

}
