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

/* Section 5.1.  Distances in game units, angles in degrees. */
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
	/* Strafe direction flips after this many ms (random in range). */
	unsigned strafe_min_ms;
	unsigned strafe_max_ms;
};

inline constexpr std::array<skill_params, BOT_SKILL_COUNT> skill_table{{
	{550, 7.0, 600, 0.0, 0.45, 12, 45, 150, 0, 2000, 0.0, 0, 0, false, 1200, 2000},
	{400, 4.5, 500, 0.4, 0.60, 9, 60, 250, 80, 3000, 0.2, 1, 3, true, 900, 1600},
	{280, 2.8, 400, 0.7, 0.75, 6, 70, 350, 150, 5000, 0.45, 2, 8, true, 400, 1200},
	{200, 1.7, 300, 0.9, 0.90, 4, 80, 450, 250, 7000, 0.7, 3, 15, true, 400, 1200},
	{140, 1.0, 250, 1.0, 1.00, 3, 90, 600, 350, 10000, 0.85, 4, 0xffff, true, 400, 1000},
}};

[[nodiscard]]
constexpr const skill_params &skill_of(const bot_skill s)
{
	const auto i{static_cast<unsigned>(s)};
	return skill_table[i < BOT_SKILL_COUNT ? i : static_cast<unsigned>(BOT_DEFAULT_SKILL)];
}

/* Section 5.2. */
struct style_params
{
	double retreat_shields;
	double engage_weight;
	double collect_weight;
	double range_scale;
};

inline constexpr std::array<style_params, BOT_STYLE_COUNT> style_table{{
	{35, 1.0, 1.0, 1.0},
	{20, 1.5, 0.6, 0.75},
	{55, 0.8, 1.2, 1.25},
	{40, 0.7, 1.8, 1.0},
}};

[[nodiscard]]
constexpr const style_params &style_of(const bot_style s)
{
	const auto i{static_cast<unsigned>(s)};
	return style_table[i < BOT_STYLE_COUNT ? i : 0];
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
 * `pos` moving with `vel`, with lead accuracy `lead` (0: at the target,
 * 1: exact intercept).  Without an intercept, the lead of the time the
 * shot needs to reach the target's current position.
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
};

[[nodiscard]]
inline double target_score(const target_candidate &c, const double awareness_radius)
{
	if (c.excluded)
		return 0;
	double s{c.visible ? 1.0 : c.confidence * 0.5};
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
 * game's rotational thrust).  A direction almost straight behind keeps
 * turning the way it turned (`prefer_heading_sign`), so the bot does not
 * dither between left and right.
 */
struct steer_errors
{
	double pitch{};
	double heading{};
};

[[nodiscard]]
inline steer_errors steer_errors_local(const vec3 &local_dir, const int prefer_heading_sign = 0)
{
	const double horizontal{std::hypot(local_dir.x, local_dir.z)};
	steer_errors e;
	e.pitch = std::atan2(-local_dir.y, horizontal);
	e.heading = std::atan2(local_dir.x, local_dir.z);
	if (prefer_heading_sign && local_dir.z < 0 && std::abs(local_dir.x) < 0.2 * -local_dir.z && (e.heading > 0) != (prefer_heading_sign > 0))
		e.heading = prefer_heading_sign * (std::numbers::pi - std::abs(std::atan2(local_dir.x, -local_dir.z)));
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
 * (critically damped for the ship's time constant), and the axis adds
 * rate feedback to the feed-forward, so the ship neither overshoots nor
 * crawls.  It uses the current state only, so its result for a given
 * state does not depend on the frame length.
 */
[[nodiscard]]
inline double rotation_axis(const double error, const double rate, const turn_response &ship, const double cap)
{
	if (ship.max_rate <= 0)
		return 0;
	constexpr double feedback{3};
	const double tau{std::max(ship.time_constant, 1e-3)};
	const double kp{(1 + feedback) / (4 * tau)};
	const double wmax{ship.max_rate * cap};
	const double wanted{std::clamp(kp * error, -wmax, wmax)};
	const double axis{(wanted + feedback * (wanted - rate)) / ship.max_rate};
	return std::clamp(axis, -cap, cap);
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

}
