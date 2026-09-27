/*
 * This file is part of the DXX-Rebirth project <https://github.com/dxx-rebirth/dxx-rebirth/>.
 * It is copyright by its individual contributors, as recorded in the
 * project's Git history.  See COPYING.txt at the top level for license
 * terms and a link to the Git history.
 */
/*
 * Multiplayer network protocol v2, stage 2: receiver-side interpolation
 * of remote ships and guided missiles (Documentation/network-protocol-v2.md,
 * sections 2.5 and 5.4).
 *
 * The network layer (net_v2.cpp) feeds timestamped snapshots per remote
 * player; once per frame, before any object moves, net_interp_apply_all
 * writes each remote ship's (and guided missile's) pose at its render
 * time into the object: position, orientation, velocities and segment.
 * The object *is* the interpolated pose, so the renderer, collisions,
 * sounds and HUD all see one position.  object_move_one skips the
 * physics of every object written here this frame (net_interp_drives).
 *
 * The math (rings, Hermite, nlerp, delay estimation) is game-independent
 * and tested on its own: common/main/net_interp.h.
 */

#include "dxxsconf.h"

#if DXX_USE_MULTIPLAYER

#include <algorithm>
#include <array>

#include "net_interp.h"
#include "net_v2_game.h"
#include "multi.h"
#include "object.h"
#include "player.h"
#include "gameseg.h"
#include "segment.h"
#include "physics.h"
#include "newdemo.h"
#include "timer.h"
#include "game.h"
#include "laser.h"
#include "d_levelstate.h"

namespace dsx {

namespace {

using ::dcx::net_interp::carried_effects;
using ::dcx::net_interp::entity_track;
using ::dcx::net_interp::host_clock;
using ::dcx::net_interp::lag_indicator;
using ::dcx::net_interp::pose;
using ::dcx::net_interp::pose_kind;
using ::dcx::net_interp::ship_axes;
using ::dcx::net_interp::snapshot;
using ::dcx::net_v2::net_clock;

#if DXX_BUILD_DESCENT == 2
struct guided_track
{
	entity_track track;
	/* The missile the snapshots describe. */
	::dcx::net_interp::guided_identity who;
	/* The local copy the snapshots were last compared with. */
	objnum_t local{object_none};
};
#endif

/* Where a driven ship was before this frame's pose was written: the start
 * of its object collision sweep (phys_sweep_objects).
 */
struct sweep_start
{
	vms_vector pos;
	segnum_t segment{segment_none};
	bool valid{};
};

struct interp_state
{
	/* The estimated host clock is the local clock plus `offset` (zero on
	 * the host), once `clock_valid`.
	 */
	bool clock_valid{};
	net_clock offset{};
	net_clock tick_period{::dcx::net_v2::net_seconds(1) / 60};
	per_player_array<entity_track> ships{};
	per_player_array<lag_indicator> lag{};
	per_player_array<net_clock> lag_age{};
	/* The objects written this frame; object_move_one leaves them alone. */
	per_player_array<objnum_t> driven_ship{};
	per_player_array<sweep_start> sweep_from{};
	/* The pose a ship had before net_interp_snap_to_newest moved it this
	 * frame, so that the sweep starts from where the ship was drawn.
	 */
	per_player_array<sweep_start> before_snap{};
	/* The newest snapshot of a ship whose ghost record arrived while it
	 * was still a ship here (before MULTI_PLAYER_DERES): the deres puts it
	 * there again, with its velocity, for the explosion and the eggs.
	 */
	per_player_array<std::optional<snapshot>> ghosted{};
	/* The muzzle flashes each remote ship carries along. */
	per_player_array<carried_effects> carried{};
#if DXX_BUILD_DESCENT == 2
	per_player_array<guided_track> guided{};
	per_player_array<objnum_t> driven_guided{};
#endif
};

interp_state I;

void clear_driven()
{
	I.driven_ship.fill(object_none);
	I.sweep_from.fill({});
#if DXX_BUILD_DESCENT == 2
	I.driven_guided.fill(object_none);
#endif
}

[[nodiscard]]
bool point_in_segment(fvcvertptr &vcvertptr, const vms_vector &pos, const shared_segment &seg)
{
	return get_seg_masks(vcvertptr, pos, seg, 0).centermask == sidemask_t{};
}

/* Section 5.4, step 2: the segment containing `pos`, looked for in
 * `start` and in the segments at most two sides away from it.  An
 * interpolated ship moves at most a few units per snapshot, so it can
 * only be that close to the segment of the nearer snapshot; the bound
 * keeps the search cheap at any frame rate and keeps a point outside the
 * mine (between snapshots on either side of a corner) from being traced
 * into some far segment.  Short tunnel segments are the reason for depth
 * 2 rather than 1: a ship that crosses a segment shorter than one
 * snapshot's travel is two sides away from both snapshots' segments.
 */
[[nodiscard]]
segnum_t find_segment_near(const vms_vector &pos, const segnum_t start)
{
	auto &LevelSharedVertexState = LevelSharedSegmentState.get_vertex_state();
	auto &vcvertptr = LevelSharedVertexState.get_vertices().vcptr;
	auto &vcsegptr = LevelSharedSegmentState.get_segments().vcptr;
	const shared_segment &s0 = *vcsegptr(start);
	if (point_in_segment(vcvertptr, pos, s0))
		return start;
	for (const auto c1 : s0.children)
		if (IS_CHILD(c1) && point_in_segment(vcvertptr, pos, *vcsegptr(c1)))
			return c1;
	for (const auto c1 : s0.children)
	{
		if (!IS_CHILD(c1))
			continue;
		for (const auto c2 : vcsegptr(c1)->shared_segment::children)
			if (IS_CHILD(c2) && c2 != start && point_in_segment(vcvertptr, pos, *vcsegptr(c2)))
				return c2;
	}
	return segment_none;
}

/* Write a pose into an object.  Returns false if the object was left
 * where it was (an extrapolation that left the mine).
 */
bool write_pose(const vmobjptridx_t obj, const pose &p)
{
	const vms_vector pos{p.pos.x, p.pos.y, p.pos.z};
	auto seg{find_segment_near(pos, segnum_t{p.segment})};
	if (seg == segment_none && p.other_segment != p.segment)
		seg = find_segment_near(pos, segnum_t{p.other_segment});
	if (seg == segment_none && obj->segnum != segnum_t{p.segment} && obj->segnum != segnum_t{p.other_segment})
		seg = find_segment_near(pos, obj->segnum);
	if (seg == segment_none)
	{
		/* Step 3: an extrapolation never goes where no segment is
		 * (through a wall the ship never touched); it stops.
		 */
		if (p.kind == pose_kind::extrapolated || p.kind == pose_kind::stale)
		{
			obj->mtype.phys_info.velocity = {};
			obj->mtype.phys_info.rotvel = {};
			return false;
		}
		/* A snapshot's own position may lie a little outside its segment
		 * (the owner's physics allows the ship's centre past a side
		 * while it scrapes); the nearer snapshot's segment is what the
		 * owner said.
		 */
		seg = segnum_t{p.segment};
	}
	obj->pos = pos;
	obj->orient = vms_matrix_from_quaternion(vms_quaternion{p.orient.w, p.orient.x, p.orient.y, p.orient.z});
	obj->mtype.phys_info.velocity = vms_vector{p.vel.x, p.vel.y, p.vel.z};
	obj->mtype.phys_info.rotvel = vms_vector{p.rotvel.x, p.rotvel.y, p.rotvel.z};
	if (obj->segnum != seg)
	{
		auto &Objects = LevelUniqueObjectState.Objects;
		obj_relink(Objects.vmptr, vmsegptr, obj, vmsegptridx(seg));
	}
	return true;
}

[[nodiscard]]
::dcx::net_v2::net_vec to_net_vec(const vms_vector &v)
{
	return {v.x, v.y, v.z};
}

[[nodiscard]]
ship_axes axes_of(const object_base &obj)
{
	return {to_net_vec(obj.orient.rvec), to_net_vec(obj.orient.uvec), to_net_vec(obj.orient.fvec)};
}

/* Put the muzzle flashes of player `pnum`'s ship back on its guns, where
 * the ship is now.  A flash that is gone, or whose place on the ship is
 * outside the mine (it is left where it is), is forgotten.
 */
void carry_effects(const playernum_t pnum, const vcobjptr_t ship)
{
	auto &c = I.carried[pnum];
	if (c.empty())
		return;
	if (ship->type != object_type::OBJ_PLAYER)
	{
		c.clear();
		return;
	}
	auto &Objects = LevelUniqueObjectState.Objects;
	const auto axes{axes_of(*ship)};
	c.remove_if([&](const carried_effects::entry &e) {
		const auto &&fx = Objects.vmptridx(objnum_t{e.object});
		if (fx->type != object_type::OBJ_FIREBALL || fx->signature != object_signature_t{e.signature} || (fx->flags & OF_SHOULD_BE_DEAD))
			return true;
		const auto off{::dcx::net_interp::from_ship_frame(axes, e.local)};
		const vms_vector pos{ship->pos.x + off.x, ship->pos.y + off.y, ship->pos.z + off.z};
		auto seg{find_segment_near(pos, fx->segnum)};
		if (seg == segment_none)
			seg = find_segment_near(pos, ship->segnum);
		if (seg == segment_none)
			return true;
		fx->pos = pos;
		if (fx->segnum != seg)
			obj_relink(Objects.vmptr, vmsegptr, fx, vmsegptridx(seg));
		return false;
	});
}

[[nodiscard]]
bool segment_valid(const uint16_t s)
{
	return s < LevelSharedSegmentState.get_segments().get_count();
}

[[nodiscard]]
host_clock est_host_now(const net_clock now)
{
	return now + I.offset;
}

/* Player `pnum`'s ship object, if it is one (not a ghost). */
[[nodiscard]]
imobjptridx_t ship_object(const playernum_t pnum)
{
	if (pnum >= N_players || pnum == Player_num || !(Game_mode & GM_NETWORK) || Newdemo_state == ND_STATE_PLAYBACK)
		return object_none;
	auto &Objects = LevelUniqueObjectState.Objects;
	const auto objnum{vcplayerptr(pnum)->objnum};
	if (objnum == object_none)
		return object_none;
	const auto &&obj = Objects.vmptridx(objnum);
	if (obj->type != object_type::OBJ_PLAYER)
		return object_none;
	return obj;
}

/* Put player `pnum`'s ship at snapshot `s`, now, rather than at its
 * render time.
 */
void place_ship(const playernum_t pnum, const vmobjptridx_t obj, const snapshot &s)
{
	auto &b = I.before_snap[pnum];
	if (!b.valid)
		b = {obj->pos, obj->segnum, true};
	if (write_pose(obj, ::dcx::net_interp::pose_at(s, pose_kind::interpolated)))
		set_thrust_from_velocity(obj);
}

}

namespace net_v2 {

namespace interp {

void set_clock(const bool valid, const net_clock offset, const net_clock tick_period)
{
	I.clock_valid = valid;
	I.offset = offset;
	if (tick_period != I.tick_period)
	{
		I.tick_period = tick_period;
		for (auto &t : I.ships)
			t.delay.set_base(tick_period);
#if DXX_BUILD_DESCENT == 2
		for (auto &g : I.guided)
			g.track.delay.set_base(tick_period);
#endif
	}
}

void receive_ship(const playernum_t pnum, const snapshot &s, const net_clock now)
{
	if (pnum >= MAX_PLAYERS || !segment_valid(s.segment))
		return;
	I.ghosted[pnum].reset();
	I.ships[pnum].receive(s, now, est_host_now(now));
}

void receive_ghost(const playernum_t pnum)
{
	if (pnum >= MAX_PLAYERS)
		return;
	/* A dead or unspawned player: the next record starts afresh (and the
	 * ship is placed there directly), however far it is.  The delay
	 * estimate survives.
	 */
	auto &ring = I.ships[pnum].ring;
	if (ring.empty())
		return;
	/* The owner's ship exploded (its records stay live through the death
	 * sequence).  If the MULTI_PLAYER_DERES is not here yet, the ship
	 * waits for it where the owner had it, stopped: without snapshots it
	 * would coast on by physics with its last thrust.
	 */
	if (const auto &&obj = ship_object(pnum); obj != object_none)
	{
		const auto &newest{ring.newest()};
		I.ghosted[pnum] = newest;
		place_ship(pnum, obj, newest);
		obj->mtype.phys_info.velocity = {};
		obj->mtype.phys_info.rotvel = {};
		obj->mtype.phys_info.thrust = {};
		obj->mtype.phys_info.rotthrust = {};
	}
	ring.clear();
}

void receive_guided(const playernum_t pnum, const uint16_t id, const uint8_t gen, const snapshot &s, const net_clock now)
{
#if DXX_BUILD_DESCENT == 2
	if (pnum >= MAX_PLAYERS || !segment_valid(s.segment))
		return;
	auto &g = I.guided[pnum];
	if (g.who.receive(id, gen))
		g.track.ring.clear();
	g.track.receive(s, now, est_host_now(now));
#else
	(void)pnum;
	(void)id;
	(void)gen;
	(void)s;
	(void)now;
#endif
}

void set_lag_age(const playernum_t pnum, const net_clock age)
{
	if (pnum < MAX_PLAYERS)
		I.lag_age[pnum] = age;
}

net_clock view_delay()
{
	/* The delay of the host's own ship: the host-stamped entities are
	 * shown this far in the past.
	 */
	return multi_i_am_master() ? 0 : I.ships[0].delay.delay();
}

void reset_player(const playernum_t pnum)
{
	if (pnum >= MAX_PLAYERS)
		return;
	I.ships[pnum].reset(I.tick_period);
	I.lag[pnum].reset();
	I.lag_age[pnum] = 0;
	I.ghosted[pnum].reset();
	I.before_snap[pnum] = {};
	I.carried[pnum].clear();
#if DXX_BUILD_DESCENT == 2
	I.guided[pnum] = {};
	I.guided[pnum].track.reset(I.tick_period);
#endif
}

}

}

void net_interp_reset()
{
	for (playernum_t i = 0; i < MAX_PLAYERS; ++i)
		net_v2::interp::reset_player(i);
	clear_driven();
}

bool net_interp_drives(const vcobjidx_t obj)
{
	const objnum_t o{obj};
	if (std::ranges::find(I.driven_ship, o) != I.driven_ship.end())
		return true;
#if DXX_BUILD_DESCENT == 2
	if (std::ranges::find(I.driven_guided, o) != I.driven_guided.end())
		return true;
#endif
	return false;
}

void net_interp_snap_to_newest(const playernum_t pnum)
{
	if (pnum >= MAX_PLAYERS)
		return;
	const auto &&obj = ship_object(pnum);
	if (obj == object_none)
		return;
	const auto &ring{I.ships[pnum].ring};
	if (!ring.empty())
		place_ship(pnum, obj, ring.newest());
	else if (const auto &g{I.ghosted[pnum]})
		place_ship(pnum, obj, *g);
}

bool net_interp_newest_position(const playernum_t pnum, vms_vector &pos, fix &speed)
{
	if (pnum >= MAX_PLAYERS)
		return false;
	const auto &ring{I.ships[pnum].ring};
	const snapshot *s{nullptr};
	if (!ring.empty())
		s = &ring.newest();
	else if (const auto &g{I.ghosted[pnum]})
		s = &*g;
	if (!s)
		return false;
	pos = {s->pos.x, s->pos.y, s->pos.z};
	speed = vm_vec_mag_quick(vms_vector{s->vel.x, s->vel.y, s->vel.z}).d;
	return true;
}

void net_interp_sweep_driven(const d_robot_info_array &Robot_info, const vmobjptridx_t obj)
{
	const objnum_t o{obj};
	for (playernum_t i = 0; i < MAX_PLAYERS; ++i)
	{
		if (I.driven_ship[i] != o)
			continue;
		auto &from = I.sweep_from[i];
		if (!from.valid || obj->type != object_type::OBJ_PLAYER)
			return;
		from.valid = false;
		if (!::dcx::net_interp::is_sweepable_move({from.pos.x, from.pos.y, from.pos.z}, {obj->pos.x, obj->pos.y, obj->pos.z}))
			return;
		phys_sweep_objects(Robot_info, obj, from.pos, from.segment);
		return;
	}
}

void net_interp_carry_flash(const vcobjptridx_t ship, const vcobjptridx_t flash)
{
	if (!(Game_mode & GM_NETWORK) || Newdemo_state == ND_STATE_PLAYBACK)
		return;
	if (ship->type != object_type::OBJ_PLAYER || flash->type != object_type::OBJ_FIREBALL)
		return;
	const auto pnum{get_player_id(ship)};
	if (pnum >= N_players || pnum >= MAX_PLAYERS || pnum == Player_num || vcplayerptr(pnum)->objnum != ship.get_unchecked_index())
		return;
	const auto offset{vm_vec_build_sub(flash->pos, ship->pos)};
	I.carried[pnum].add({
		.object = flash.get_unchecked_index(),
		.signature = static_cast<uint16_t>(flash->signature),
		.local = ::dcx::net_interp::to_ship_frame(axes_of(*ship), to_net_vec(offset)),
	});
}

bool net_interp_player_lagging(const playernum_t pnum)
{
	return pnum < MAX_PLAYERS && pnum != Player_num && I.lag[pnum].lagging();
}

void net_interp_apply_all()
{
	clear_driven();
	if (!(Game_mode & GM_NETWORK) || Newdemo_state == ND_STATE_PLAYBACK || !I.clock_valid)
		return;
	if (Network_status != network_state::playing && Network_status != network_state::endlevel)
		return;
	auto &Objects = LevelUniqueObjectState.Objects;
	auto &vmobjptridx = Objects.vmptridx;
	const net_clock now{timer_query()};
	const host_clock est{est_host_now(now)};
	const unsigned n{std::min<unsigned>(N_players, MAX_PLAYERS)};
	for (playernum_t i = 0; i < n; ++i)
	{
		I.lag[i].update(I.lag_age[i]);
		if (i == Player_num)
			continue;
		auto &plr = *vcplayerptr(i);
		if (plr.connected != player_connection_status::playing)
		{
			I.carried[i].clear();
			continue;
		}
		auto &t = I.ships[i];
		t.delay.update(now);
		if (!t.ring.empty())
		{
			const auto &&obj = vmobjptridx(plr.objnum);
			if (obj->type == object_type::OBJ_PLAYER)
				if (const auto p{sample(t.ring, t.render_time(est))})
				{
					auto &b = I.before_snap[i];
					const sweep_start from{b.valid ? b : sweep_start{obj->pos, obj->segnum, true}};
					if (write_pose(obj, *p))
					{
						set_thrust_from_velocity(obj);
						I.sweep_from[i] = from;
					}
					I.driven_ship[i] = obj;
				}
		}
		I.before_snap[i] = {};
		/* The flashes of the shots it fired since the last frame were
		 * made at the gun where it was then (and drawn): put them back
		 * on the gun where it is now, before this frame draws them.
		 */
		if (plr.objnum != object_none)
			carry_effects(i, vmobjptridx(plr.objnum));
		else
			I.carried[i].clear();
#if DXX_BUILD_DESCENT == 2
		auto &g = I.guided[i];
		g.track.delay.update(now);
		const auto &&gim = LevelUniqueObjectState.Guided_missile.get_player_active_guided_missile(vmobjptridx, i);
		const objnum_t local{gim == nullptr ? objnum_t{object_none} : gim.get_unchecked_index()};
		if (g.local != local)
		{
			g.local = local;
			/* The copy is gone (it exploded or was released): its
			 * snapshots with it.  A new copy keeps the snapshots that
			 * arrived before its fire message, if they are its own
			 * (same generation, checked below).
			 */
			if (local == object_none)
			{
				g.who.active = false;
				g.track.ring.clear();
			}
		}
		if (gim == nullptr || !g.who.active || g.track.ring.empty())
			continue;
		/* Only the missile the snapshots describe (MULTI_FIRE_BOMB and
		 * MULTI_FIRE_TRACK map the owner's object number to the copy and
		 * set its generation): the records of the next missile, in the
		 * same object slot, that arrive before its fire message are not
		 * applied to the previous copy.
		 */
		if (local != objnum_remote_to_local(g.who.id, static_cast<int8_t>(i)) || !g.who.describes(g.who.id, multi_guided_generation(i)))
			continue;
		const auto p{sample(g.track.ring, g.track.render_time(est))};
		/* Before its first snapshot (just launched) and after the records
		 * stopped for longer than the extrapolation limit (the owner's
		 * missile hit something, or the release is on its way), the copy
		 * flies by its own physics, which also lets it hit what the
		 * owner's missile hit.
		 */
		if (!p || p->kind == pose_kind::early || p->kind == pose_kind::stale)
			continue;
		const vmobjptridx_t missile = gim;
		write_pose(missile, *p);
		I.driven_guided[i] = missile;
#endif
	}
}

}

#endif
