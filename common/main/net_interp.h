/*
 * This file is part of the DXX-Rebirth project <https://github.com/dxx-rebirth/dxx-rebirth/>.
 * It is copyright by its individual contributors, as recorded in the
 * project's Git history.  See COPYING.txt at the top level for license
 * terms and a link to the Git history.
 */
/*
 * Multiplayer network protocol v2: the game-independent part of the
 * receiver-side interpolation (Documentation/network-protocol-v2.md,
 * sections 2.3 and 5.4, and "Stage 2 as implemented" in section 8):
 *
 * - `snapshot_ring`: the last NET_INTERP_RING_SIZE timestamped poses of
 *   one remote entity, kept in time order;
 * - `sample`: the pose of an entity at a render time (cubic Hermite for
 *   the position, normalised lerp for the orientation, capped constant
 *   velocity extrapolation, snaps on discontinuities);
 * - `delay_estimator`: the interpolation delay of one entity, from the
 *   lateness of its snapshots;
 * - `lag_indicator`: the "lagging" marker, with hysteresis;
 * - `tick_accumulator`: the network tick counter;
 * - `carried_effects`, `to_ship_frame`, `from_ship_frame`: the muzzle
 *   flashes a remote ship carries along.
 *
 * Everything is a pure function of the times passed in, so the result
 * of a frame depends on the time it is rendered at, never on the frame
 * rate.  Times are `host_clock`: the host's clock (net time units,
 * 1/65536 s) widened to 64 bits by `unwrap`.  Positions and velocities
 * are `fix` (net_v2_state.h).  The game side, which writes the objects
 * and finds segments, is similar/main/net_interp.cpp.
 *
 * Standard library only, so that the math is tested outside the game
 * (common/unittest/net_v2_interp.cpp).
 */

#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <vector>

#include "net_v2.h"
#include "net_v2_state.h"

namespace dcx {

namespace net_interp {

using ::dcx::net_v2::net_clock;
using ::dcx::net_v2::net_time;
using ::dcx::net_v2::net_seconds;
using ::dcx::net_v2::net_milliseconds;
using ::dcx::net_v2::net_vec;
using ::dcx::net_v2::net_quat;
using host_clock = std::int64_t;

constexpr std::size_t NET_INTERP_RING_SIZE{16};
/* Section 5.4, step 3. */
constexpr net_clock NET_INTERP_EXTRAPOLATE_MAX{net_milliseconds(100)};
/* Section 5.4, step 4: 20 units. */
constexpr std::int64_t NET_INTERP_SNAP_DISTANCE{20 * 65536};
/* Between snapshots further apart than this (loss), the velocities no
 * longer describe the path between them: interpolate linearly.
 */
constexpr net_clock NET_INTERP_HERMITE_MAX_GAP{net_milliseconds(250)};
/* Section 5.4: the lateness window, the recomputation interval and the
 * slew of the interpolation delay.  The design's "1 ms per frame" depends
 * on the frame rate; it is 1 ms per 60 Hz frame here, as a rate.
 */
constexpr net_clock NET_INTERP_LATENESS_WINDOW{net_seconds(2)};
constexpr net_clock NET_INTERP_DELAY_UPDATE_INTERVAL{net_seconds(1)};
constexpr net_clock NET_INTERP_DELAY_SLEW_PER_SECOND{net_milliseconds(60)};
/* The largest lateness margin an entity's delay covers.  The design caps
 * a single, global jitter margin at 50 ms; the delay is per entity here
 * (the relayed ship of another client is older than the host's by that
 * client's uplink), so a larger cap only slows the display of the one
 * late entity and keeps it smooth instead of extrapolating it.
 */
constexpr net_clock NET_INTERP_LATENESS_MAX{net_milliseconds(400)};
/* The lag marker: shown when a player's state is older than ON at the
 * host, hidden again below OFF.
 */
constexpr net_clock NET_INTERP_LAG_ON{net_milliseconds(250)};
constexpr net_clock NET_INTERP_LAG_OFF{net_milliseconds(200)};

/* Widen a wire time to the 64-bit clock, as the value nearest to `ref`
 * (valid while the two are less than 2^31 units, 9 hours, apart).
 */
[[nodiscard]]
constexpr host_clock unwrap(const net_time t, const host_clock ref)
{
	return ref + ::dcx::net_v2::net_time_diff(t, ::dcx::net_v2::to_net_time(ref));
}

struct snapshot
{
	host_clock time{};
	net_vec pos;
	net_quat orient;
	std::uint16_t segment{};
	net_vec vel;
	net_vec rotvel;
	/* Section 5.4, step 4: a change of `alive` is a discontinuity. */
	bool alive{true};
};

enum class insert_result : std::uint8_t
{
	/* Inserted and now the newest. */
	newest,
	/* Inserted between older and newer ones (reordered). */
	inserted,
	/* A snapshot with the same time is already there. */
	duplicate,
	/* Older than every snapshot of a full ring. */
	too_old,
};

/* The snapshots of one entity, oldest first.  A full ring drops its
 * oldest one to make room.
 */
class snapshot_ring
{
	std::array<snapshot, NET_INTERP_RING_SIZE> m_buf{};
	std::size_t m_count{};
public:
	insert_result insert(const snapshot &s)
	{
		std::size_t i{m_count};
		while (i > 0 && m_buf[i - 1].time > s.time)
			--i;
		if (i > 0 && m_buf[i - 1].time == s.time)
			return insert_result::duplicate;
		if (m_count == m_buf.size())
		{
			if (i == 0)
				return insert_result::too_old;
			/* Drop the oldest: the older ones move down one, which opens
			 * the slot just below `i` for the new one.
			 */
			std::move(m_buf.begin() + 1, m_buf.begin() + i, m_buf.begin());
			m_buf[i - 1] = s;
			return i == m_count ? insert_result::newest : insert_result::inserted;
		}
		std::move_backward(m_buf.begin() + i, m_buf.begin() + m_count, m_buf.begin() + m_count + 1);
		m_buf[i] = s;
		++m_count;
		return i + 1 == m_count ? insert_result::newest : insert_result::inserted;
	}
	void clear()
	{
		m_count = 0;
	}
	[[nodiscard]]
	std::size_t size() const
	{
		return m_count;
	}
	[[nodiscard]]
	bool empty() const
	{
		return !m_count;
	}
	[[nodiscard]]
	const snapshot &operator[](const std::size_t i) const
	{
		return m_buf[i];
	}
	[[nodiscard]]
	const snapshot &oldest() const
	{
		return m_buf[0];
	}
	[[nodiscard]]
	const snapshot &newest() const
	{
		return m_buf[m_count - 1];
	}
};

enum class pose_kind : std::uint8_t
{
	/* Before the oldest snapshot: the oldest one's pose. */
	early,
	/* Between two snapshots (or exactly on one). */
	interpolated,
	/* At a discontinuity, before the newer snapshot takes effect: the
	 * older one's pose.
	 */
	held,
	/* Past the newest snapshot, by at most NET_INTERP_EXTRAPOLATE_MAX. */
	extrapolated,
	/* Past the extrapolation limit: held where the limit put it, with
	 * zero velocity.
	 */
	stale,
};

struct pose
{
	pose_kind kind{};
	net_vec pos;
	net_quat orient;
	net_vec vel;
	net_vec rotvel;
	/* Section 5.4, step 2: the segment to look in first (the nearer
	 * snapshot's) and the other snapshot's segment.
	 */
	std::uint16_t segment{};
	std::uint16_t other_segment{};
	bool alive{true};
};

namespace detail {

[[nodiscard]]
inline std::int32_t round_fix(const double v)
{
	const double r{std::nearbyint(v)};
	return static_cast<std::int32_t>(std::clamp(r, -2147483648.0, 2147483647.0));
}

[[nodiscard]]
inline net_vec lerp(const net_vec &a, const net_vec &b, const double s)
{
	return {
		round_fix(a.x + (static_cast<double>(b.x) - a.x) * s),
		round_fix(a.y + (static_cast<double>(b.y) - a.y) * s),
		round_fix(a.z + (static_cast<double>(b.z) - a.z) * s),
	};
}

[[nodiscard]]
inline double distance(const net_vec &a, const net_vec &b)
{
	const double dx{static_cast<double>(b.x) - a.x}, dy{static_cast<double>(b.y) - a.y}, dz{static_cast<double>(b.z) - a.z};
	return std::sqrt(dx * dx + dy * dy + dz * dz);
}

}

/* Cubic Hermite between p0 (velocity v0) and p1 (velocity v1), `dt`
 * seconds apart, at fraction s of the way.  Velocities are fix per
 * second.
 */
[[nodiscard]]
inline net_vec hermite(const net_vec &p0, const net_vec &v0, const net_vec &p1, const net_vec &v1, const double dt, const double s)
{
	const double s2{s * s}, s3{s2 * s};
	const double h00{2 * s3 - 3 * s2 + 1}, h10{s3 - 2 * s2 + s}, h01{-2 * s3 + 3 * s2}, h11{s3 - s2};
	const auto axis{[&](const double a0, const double b0, const double a1, const double b1) {
		return detail::round_fix(h00 * a0 + h10 * dt * b0 + h01 * a1 + h11 * dt * b1);
	}};
	return {
		axis(p0.x, v0.x, p1.x, v1.x),
		axis(p0.y, v0.y, p1.y, v1.y),
		axis(p0.z, v0.z, p1.z, v1.z),
	};
}

/* Normalised lerp of two engine quaternions, along the shorter arc.  The
 * result has the magnitude of a unit quaternion in engine units (32767),
 * which vms_matrix_from_quaternion does not need but keeps the value in
 * range.
 */
[[nodiscard]]
inline net_quat nlerp(const net_quat &a, const net_quat &b, const double s)
{
	const double dot{static_cast<double>(a.w) * b.w + static_cast<double>(a.x) * b.x + static_cast<double>(a.y) * b.y + static_cast<double>(a.z) * b.z};
	const double sign{dot < 0 ? -1.0 : 1.0};
	const double w{a.w + (sign * b.w - a.w) * s};
	const double x{a.x + (sign * b.x - a.x) * s};
	const double y{a.y + (sign * b.y - a.y) * s};
	const double z{a.z + (sign * b.z - a.z) * s};
	const double n{std::sqrt(w * w + x * x + y * y + z * z)};
	if (n < 1)
		return a;
	const double k{32767.0 / n};
	const auto c{[k](const double v) {
		return static_cast<std::int16_t>(std::clamp(std::nearbyint(v * k), -32767.0, 32767.0));
	}};
	return {c(w), c(x), c(y), c(z)};
}

[[nodiscard]]
inline pose pose_at(const snapshot &s, const pose_kind kind)
{
	return pose{
		.kind = kind,
		.pos = s.pos,
		.orient = s.orient,
		.vel = s.vel,
		.rotvel = s.rotvel,
		.segment = s.segment,
		.other_segment = s.segment,
		.alive = s.alive,
	};
}

/* Section 5.4, steps 1 to 4: the pose of the entity at `render_time`.
 * Nothing if the ring is empty.
 */
[[nodiscard]]
inline std::optional<pose> sample(const snapshot_ring &ring, const host_clock render_time)
{
	if (ring.empty())
		return std::nullopt;
	const auto &first{ring.oldest()};
	if (render_time <= first.time)
		return pose_at(first, render_time == first.time ? pose_kind::interpolated : pose_kind::early);
	const auto &last{ring.newest()};
	if (render_time > last.time)
	{
		/* Step 3: constant velocity from the newest snapshot, orientation
		 * held, for at most NET_INTERP_EXTRAPOLATE_MAX; then hold there.
		 */
		const auto ahead{render_time - last.time};
		const bool capped{ahead > NET_INTERP_EXTRAPOLATE_MAX};
		const double t{static_cast<double>(capped ? NET_INTERP_EXTRAPOLATE_MAX : ahead) / 65536.0};
		auto p{pose_at(last, capped ? pose_kind::stale : pose_kind::extrapolated)};
		p.pos = {
			detail::round_fix(last.pos.x + last.vel.x * t),
			detail::round_fix(last.pos.y + last.vel.y * t),
			detail::round_fix(last.pos.z + last.vel.z * t),
		};
		p.rotvel = {};
		if (capped)
			p.vel = {};
		return p;
	}
	std::size_t i{1};
	while (ring[i].time < render_time)
		++i;
	const auto &a{ring[i - 1]};
	const auto &b{ring[i]};
	if (render_time == b.time)
		return pose_at(b, pose_kind::interpolated);
	/* Step 4: a jump or a change of `alive` is not interpolated; `a`
	 * holds until `b` takes effect at its own time.
	 */
	if (a.alive != b.alive || detail::distance(a.pos, b.pos) > NET_INTERP_SNAP_DISTANCE)
		return pose_at(a, pose_kind::held);
	const auto gap{b.time - a.time};
	const double s{static_cast<double>(render_time - a.time) / static_cast<double>(gap)};
	pose p;
	p.kind = pose_kind::interpolated;
	p.pos = gap <= NET_INTERP_HERMITE_MAX_GAP
		? hermite(a.pos, a.vel, b.pos, b.vel, static_cast<double>(gap) / 65536.0, s)
		: detail::lerp(a.pos, b.pos, s);
	p.orient = nlerp(a.orient, b.orient, s);
	p.vel = detail::lerp(a.vel, b.vel, s);
	p.rotvel = detail::lerp(a.rotvel, b.rotvel, s);
	/* Step 2: the nearer snapshot's segment first. */
	const bool past_mid{s >= 0.5};
	p.segment = past_mid ? b.segment : a.segment;
	p.other_segment = past_mid ? a.segment : b.segment;
	p.alive = a.alive;
	return p;
}

/* Which guided missile the snapshots of one player's guided track
 * describe: the owner's object number `id` and generation `gen` (the
 * owner's count of guided missiles fired, which the fire message and
 * every guided record carry).  A new missile that reuses the previous
 * one's object slot has the same `id`, so without `gen` its records,
 * arriving before its fire message, would be applied to the previous
 * missile's copy.
 */
struct guided_identity
{
	std::uint16_t id{};
	std::uint8_t gen{};
	bool active{};
	/* A record of missile (`rid`, `rgen`) arrived: true if the snapshots
	 * held so far are of another missile (or none), so that the ring
	 * must be cleared before this record goes in.
	 */
	bool receive(const std::uint16_t rid, const std::uint8_t rgen)
	{
		const bool changed{!active || id != rid || gen != rgen};
		id = rid;
		gen = rgen;
		active = true;
		return changed;
	}
	/* Whether the snapshots describe the local copy that was fired as
	 * the owner's object `copy_id` with generation `copy_gen`.
	 */
	[[nodiscard]]
	bool describes(const std::uint16_t copy_id, const std::uint8_t copy_gen) const
	{
		return active && id == copy_id && gen == copy_gen;
	}
};

/* Whether a ship moved from `a` to `b` along a path its collisions can be
 * swept on, rather than jumping (a discontinuity, the first record after
 * a respawn): at most NET_INTERP_SNAP_DISTANCE.
 */
[[nodiscard]]
inline bool is_sweepable_move(const net_vec &a, const net_vec &b)
{
	return detail::distance(a, b) <= NET_INTERP_SNAP_DISTANCE;
}

/* A ship's axes: the right, up and forward unit vectors (fix), the rows
 * of the engine's orientation matrix.
 */
struct ship_axes
{
	net_vec rvec, uvec, fvec;
};

/* `offset`, a vector in the world (from the ship's centre), in the ship's
 * own frame: its right, up and forward components (vm_vec_rotate).
 */
[[nodiscard]]
inline net_vec to_ship_frame(const ship_axes &a, const net_vec &offset)
{
	const auto dot{[&offset](const net_vec &v) {
		return detail::round_fix((static_cast<double>(v.x) * offset.x + static_cast<double>(v.y) * offset.y + static_cast<double>(v.z) * offset.z) / 65536);
	}};
	return {dot(a.rvec), dot(a.uvec), dot(a.fvec)};
}

/* The inverse of to_ship_frame: a vector given in the ship's frame, in the
 * world.
 */
[[nodiscard]]
inline net_vec from_ship_frame(const ship_axes &a, const net_vec &local)
{
	const auto axis{[&local](const std::int32_t r, const std::int32_t u, const std::int32_t f) {
		return detail::round_fix((static_cast<double>(r) * local.x + static_cast<double>(u) * local.y + static_cast<double>(f) * local.z) / 65536);
	}};
	return {
		axis(a.rvec.x, a.uvec.x, a.fvec.x),
		axis(a.rvec.y, a.uvec.y, a.fvec.y),
		axis(a.rvec.z, a.uvec.z, a.fvec.z),
	};
}

constexpr std::size_t NET_INTERP_CARRIED_MAX{16};

/* The effects a remote ship carries along: the muzzle flashes of its
 * shots.  A muzzle flash is an object that does not move, which is right
 * for a ship that fires at the end of its frame and is drawn there (the
 * local player's, which makes no flash for its own view anyway, or a
 * robot, which is slow), but a remote ship is moved on by the
 * interpolation every frame after its fire message made the flash, so
 * a flash that stays put is left behind it, the further the faster it
 * flies (several ship lengths over a flash's life with the afterburner).
 * Each entry is the flash's object number and signature (the object may
 * be gone and its slot reused) and its offset from the ship's centre in
 * the ship's frame at the moment it was made: the gun it came from.
 * When the list is full, the oldest entry is dropped (that flash then
 * stays where it is).
 */
class carried_effects
{
public:
	struct entry
	{
		std::uint16_t object{};
		std::uint16_t signature{};
		net_vec local;
	};
private:
	std::array<entry, NET_INTERP_CARRIED_MAX> m_buf{};
	std::size_t m_count{};
public:
	void add(const entry &e)
	{
		/* A new object in the slot of one that is gone. */
		remove_if([&e](const entry &o) { return o.object == e.object; });
		if (m_count == m_buf.size())
		{
			std::move(m_buf.begin() + 1, m_buf.end(), m_buf.begin());
			--m_count;
		}
		m_buf[m_count++] = e;
	}
	/* Remove every entry for which `pred` is true, keeping the order. */
	template <typename P>
	void remove_if(P &&pred)
	{
		const auto end{std::remove_if(m_buf.begin(), m_buf.begin() + m_count, pred)};
		m_count = static_cast<std::size_t>(end - m_buf.begin());
	}
	void clear()
	{
		m_count = 0;
	}
	[[nodiscard]]
	std::size_t size() const
	{
		return m_count;
	}
	[[nodiscard]]
	bool empty() const
	{
		return !m_count;
	}
	[[nodiscard]]
	const entry &operator[](const std::size_t i) const
	{
		return m_buf[i];
	}
};

/* Section 5.4: the interpolation delay of one entity,
 *
 *	delay = base + clamp(p90(lateness over the last 2 s), 0, NET_INTERP_LATENESS_MAX)
 *
 * with `base` one tick period, and as the lateness sample, taken when a
 * snapshot arrives that is newer than all before it, the age the
 * previous newest snapshot had reached by then (estimated host time now
 * minus its time).  That is the longest the entity went without a newer
 * snapshot, which is exactly what the delay must cover for the render
 * time to stay behind the newest snapshot.  On a steady link it is one
 * tick plus the arrival's one-way lateness, so the delay is the design's
 * `2 * tick_period + jitter_margin`; but it also covers a host whose
 * frame rate is below the tick rate (it sends one bundle per frame), and
 * the one-way latency, which the design's margin of 50 ms at most does
 * not (see "Stage 2 as implemented").  For another client's ship relayed
 * by the host it also contains that client's uplink, which is why the
 * delay is kept per entity.
 *
 * The target is recomputed once per NET_INTERP_DELAY_UPDATE_INTERVAL; the
 * applied delay follows it at NET_INTERP_DELAY_SLEW_PER_SECOND of elapsed
 * time (so the same at any frame rate), except that the first target is
 * taken at once.
 */
class delay_estimator
{
	struct lateness_sample
	{
		net_clock at;
		net_clock lateness;
	};
	std::deque<lateness_sample> m_samples;
	net_clock m_base;
	net_clock m_target;
	net_clock m_delay;
	net_clock m_next_update{};
	net_clock m_last_update{};
	net_clock m_slew_remainder{};
	bool m_started{};
	void expire(const net_clock now)
	{
		while (!m_samples.empty() && m_samples.front().at + NET_INTERP_LATENESS_WINDOW < now)
			m_samples.pop_front();
	}
	void recompute()
	{
		net_clock margin{};
		if (!m_samples.empty())
		{
			std::vector<net_clock> v;
			v.reserve(m_samples.size());
			for (const auto &s : m_samples)
				v.push_back(s.lateness);
			/* The 90th percentile: the smallest value that at least 90 %
			 * of the samples do not exceed.
			 */
			const std::size_t k{(v.size() * 9 + 9) / 10 - 1};
			std::nth_element(v.begin(), v.begin() + static_cast<std::ptrdiff_t>(k), v.end());
			margin = std::clamp<net_clock>(v[k], 0, NET_INTERP_LATENESS_MAX);
		}
		m_target = m_base + margin;
	}
public:
	delay_estimator() :
		delay_estimator{0}
	{
	}
	explicit delay_estimator(const net_clock base) :
		m_base{base}, m_target{base}, m_delay{base}
	{
	}
	/* Forget the samples and fall back to the base delay (a new level, a
	 * player who left).
	 */
	void reset(const net_clock base)
	{
		m_samples.clear();
		m_base = m_target = m_delay = base;
		m_slew_remainder = 0;
		m_started = false;
	}
	/* The tick rate changed: the base follows at once, the margin stays. */
	void set_base(const net_clock base)
	{
		m_target += base - m_base;
		m_delay += base - m_base;
		m_base = base;
	}
	void add_sample(const net_clock now, const net_clock lateness)
	{
		m_samples.push_back({now, lateness < 0 ? 0 : lateness});
		if (!m_started)
		{
			m_started = true;
			m_last_update = now;
			m_next_update = now + NET_INTERP_DELAY_UPDATE_INTERVAL;
			recompute();
			m_delay = m_target;
		}
	}
	/* Call once per frame, before `delay`. */
	void update(const net_clock now)
	{
		if (!m_started)
			return;
		expire(now);
		if (now >= m_next_update)
		{
			recompute();
			/* Keep the phase of the schedule, so that the recomputation
			 * happens at the same moments (to within a frame) at any
			 * frame rate.
			 */
			do
				m_next_update += NET_INTERP_DELAY_UPDATE_INTERVAL;
			while (m_next_update <= now);
		}
		const auto elapsed{now - m_last_update};
		if (elapsed <= 0)
			return;
		m_last_update = now;
		const auto diff{m_target - m_delay};
		if (!diff)
		{
			m_slew_remainder = 0;
			return;
		}
		const auto scaled{elapsed * NET_INTERP_DELAY_SLEW_PER_SECOND + m_slew_remainder};
		auto step{scaled / net_seconds(1)};
		const auto magnitude{diff < 0 ? -diff : diff};
		if (step >= magnitude)
		{
			step = magnitude;
			m_slew_remainder = 0;
		}
		else
			m_slew_remainder = scaled % net_seconds(1);
		m_delay += diff < 0 ? -step : step;
	}
	[[nodiscard]]
	net_clock delay() const
	{
		return m_delay;
	}
	[[nodiscard]]
	net_clock target() const
	{
		return m_target;
	}
	[[nodiscard]]
	net_clock base() const
	{
		return m_base;
	}
	[[nodiscard]]
	bool started() const
	{
		return m_started;
	}
};

/* The "lagging" marker of one player (section 5.2, `input_age`): on when
 * the age of the player's newest state at the host exceeds
 * NET_INTERP_LAG_ON, off again below NET_INTERP_LAG_OFF, so that a ship
 * near the threshold does not blink.
 */
class lag_indicator
{
	bool m_lagging{};
public:
	bool update(const net_clock age)
	{
		if (m_lagging ? age < NET_INTERP_LAG_OFF : age > NET_INTERP_LAG_ON)
			m_lagging = !m_lagging;
		return m_lagging;
	}
	void reset()
	{
		m_lagging = false;
	}
	[[nodiscard]]
	bool lagging() const
	{
		return m_lagging;
	}
};

/* One remote entity: its snapshots, its delay and its lag marker. */
struct entity_track
{
	snapshot_ring ring;
	delay_estimator delay;
	/* Insert a received snapshot.  `now` is the local clock, `est_host_now`
	 * the estimated host clock at the same moment.  A snapshot newer than
	 * all the others is a lateness sample (see delay_estimator).
	 */
	insert_result receive(const snapshot &s, const net_clock now, const host_clock est_host_now)
	{
		const bool had{!ring.empty()};
		const host_clock previous{had ? ring.newest().time : 0};
		const auto r{ring.insert(s)};
		if (r == insert_result::newest)
			delay.add_sample(now, est_host_now - (had ? previous : s.time));
		return r;
	}
	/* The render time of this entity at `est_host_now`, after
	 * `delay.update(now)`.
	 */
	[[nodiscard]]
	host_clock render_time(const host_clock est_host_now) const
	{
		return est_host_now - delay.delay();
	}
	void reset(const net_clock base)
	{
		ring.clear();
		delay.reset(base);
	}
};

/* Section 2.3: the network tick counter.  `advance(now)` counts the tick
 * boundaries passed since the previous call, in exact rational arithmetic
 * (the phase accumulates `elapsed * rate` against one second, so 1/60 s,
 * which is not a whole number of units, never drifts), like calc_d_tick
 * does for the game's 30 Hz tick.  Tick numbers are u32 and start at 0.
 * A long frame returns every tick it spans; the caller sends one state
 * for all of them (the newest), so a stall never ends in a burst.
 */
class tick_accumulator
{
	unsigned m_rate;
	net_clock m_phase{};
	net_clock m_last{};
	std::uint32_t m_tick{};
	bool m_started{};
public:
	tick_accumulator() :
		tick_accumulator{60}
	{
	}
	explicit tick_accumulator(const unsigned rate) :
		m_rate{rate ? rate : 60}
	{
	}
	void reset(const net_clock now)
	{
		m_phase = 0;
		m_last = now;
		m_tick = 0;
		m_started = true;
	}
	/* A new rate keeps the fraction of the current tick. */
	void set_rate(const unsigned rate)
	{
		if (!rate || rate == m_rate)
			return;
		m_phase = m_phase * rate / m_rate;
		m_rate = rate;
	}
	unsigned advance(const net_clock now)
	{
		if (!m_started)
		{
			reset(now);
			return 0;
		}
		const auto elapsed{now - m_last};
		if (elapsed <= 0)
			return 0;
		m_last = now;
		m_phase += elapsed * m_rate;
		/* One unit of time of tolerance (`m_rate` phase units), as in
		 * connection::begin_tick: a 60 Hz caller whose integer clock
		 * rounds the period down by a unit is on time, not late.  The
		 * phase keeps the exact remainder, possibly slightly negative,
		 * so the tolerance never accumulates.
		 */
		const auto ticks{(m_phase + m_rate) / net_seconds(1)};
		m_phase -= ticks * net_seconds(1);
		m_tick += static_cast<std::uint32_t>(ticks);
		return static_cast<unsigned>(ticks);
	}
	[[nodiscard]]
	std::uint32_t tick() const
	{
		return m_tick;
	}
	[[nodiscard]]
	unsigned rate() const
	{
		return m_rate;
	}
	/* The tick period, rounded up to whole units. */
	[[nodiscard]]
	net_clock period() const
	{
		return (net_seconds(1) + m_rate - 1) / m_rate;
	}
};

}

}
