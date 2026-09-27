/*
 * This file is part of the DXX-Rebirth project <https://github.com/dxx-rebirth/dxx-rebirth/>.
 * It is copyright by its individual contributors, as recorded in the
 * project's Git history.  See COPYING.txt at the top level for license
 * terms and a link to the Git history.
 */
/*
 * Multiplayer network protocol v2, stage 3: object authority and pickups
 * (Documentation/network-protocol-v2.md, sections 6.1-6.4 and "Stage 3 as
 * implemented").
 *
 * In a network game every powerup has a net id that is the same on every
 * machine: the level's powerups get their level id at level start, every
 * powerup created later is created by the host and announced with a new
 * id (OBJ_CREATE).  The host decides every pickup: a client that touches
 * a powerup it can use hides it and asks (PICKUP_REQUEST); the host
 * grants the first valid request with PICKUP_GRANT to everyone (which
 * also removes the powerup, or sets the ammunition left in a cannon) and
 * denies the others (PICKUP_DENY, the requester shows the powerup
 * again).  The host's own touches go through the same decision, at once.
 * The host keeps a copy of each client's inventory, from the client's
 * INVENTORY reports and the grants still on their way to it, decides
 * with it, drops the eggs of dead and departed players from it and
 * creates the weapons, flags and orbs players drop (DROP_REQUEST).
 *
 * The rules and tables are game-independent and tested on their own:
 * common/main/net_v2_objects.h, common/unittest/net_v2_authority.cpp.
 */

#include "dxxsconf.h"

#if DXX_USE_MULTIPLAYER

#include <algorithm>
#include <array>
#include <span>

#include "net_v2_objects.h"
#include "net_v2_session.h"
#include "net_v2_game.h"
#include "multi.h"
#include "object.h"
#include "player.h"
#include "powerup.h"
#include "fireball.h"
#include "weapon.h"
#include "laser.h"
#include "collide.h"
#include "gameseg.h"
#include "segment.h"
#include "game.h"
#include "hudmsg.h"
#include "text.h"
#include "digi.h"
#include "sounds.h"
#include "timer.h"
#include "newdemo.h"
#include "vclip.h"
#include "console.h"
#include "d_levelstate.h"

namespace dsx {

namespace {

namespace nv = ::dcx::net_v2;
using nv::netid_t;
using nv::NETID_NONE;
using nv::session_msg;

/* A client shows a powerup it asked for again when the host has not
 * answered within this time (the answer is reliable: it only takes this
 * long on a link that is about to time out), and does not ask for a
 * powerup again for a while after a denial.
 */
constexpr fix64 PICKUP_TIMEOUT{F1_0 * 3 / 2};
constexpr fix64 PICKUP_COOLDOWN{F1_0 / 2};
/* A report whose only changes are ammunition, energy and shields waits
 * this long after the previous one.
 */
constexpr fix64 INVENTORY_REPORT_INTERVAL{F1_0 / 10};
constexpr fix64 SPAT_DELAY{F1_0 * 2};
constexpr uint8_t NO_OWNER{0xff};

struct host_object_meta
{
	uint8_t spat_owner{NO_OWNER};
	fix64 spat_until{};
	bool moving{};
};

struct authority_state
{
	nv::netid_table<MAX_OBJECTS> table;
	std::array<host_object_meta, MAX_OBJECTS> meta{};
	std::array<nv::inventory_mirror, MAX_PLAYERS> mirrors{};
	/* The host dropped this player's items; it has not reappeared. */
	std::array<bool, MAX_PLAYERS> dropped{};
	nv::pending_pickups<16> pending;
	/* Client: the grants for this player received (INVENTORY `seq`). */
	uint16_t applied_grants{};
	/* Client: its own lives, to tell a grant for a life that ended. */
	nv::own_life life;
	nv::inventory last_sent{};
	bool have_last_sent{};
	fix64 last_sent_time{};
};

authority_state A;

template <std::size_t N>
void send(const session_msg type, const std::array<uint8_t, N> &buf, const playernum_t exclude = MAX_PLAYERS)
{
	::dsx::net_v2::game_broadcast(static_cast<uint8_t>(type), buf, exclude);
}

template <std::size_t N>
void send_to(const playernum_t slot, const session_msg type, const std::array<uint8_t, N> &buf)
{
	::dsx::net_v2::game_send_to(slot, static_cast<uint8_t>(type), buf);
}

[[nodiscard]]
std::array<int32_t, 3> to_array(const vms_vector &v)
{
	return {{v.x, v.y, v.z}};
}

[[nodiscard]]
vms_vector to_vector(const std::array<int32_t, 3> &a)
{
	return {a[0], a[1], a[2]};
}

/* A player's inventory, as INVENTORY carries it. */
[[nodiscard]]
nv::inventory inventory_of(const object &plr)
{
	auto &pi{plr.ctype.player_info};
	nv::inventory inv;
	inv.primary_flags = pi.primary_weapon_flags;
	inv.laser_level = underlying_value(pi.laser_level);
	for (unsigned i = 0; i < MAX_SECONDARY_WEAPONS; ++i)
		inv.secondary[i] = pi.secondary_ammo[static_cast<secondary_weapon_index>(i)];
	inv.vulcan_ammo = pi.vulcan_ammo;
	inv.powerup_flags = underlying_value(pi.powerup_flags);
	inv.faking_invul = pi.FakingInvul;
#if DXX_BUILD_DESCENT == 2
	inv.orbs = game_mode_hoard(Game_mode) ? pi.hoard.orbs : 0;
	inv.omega_charge = pi.Omega_charge;
#endif
	inv.shields = plr.shields;
	inv.energy = pi.energy;
	return inv;
}

/* Write an inventory into a player's object: the host's copy of a
 * client (with shields and energy, which the state bundle shows), a
 * client's copy of another player (without: its own damage decides
 * nothing there), or the local player's own for a grant that arrived
 * while it was dead.
 */
void write_inventory(object &plr, const nv::inventory &inv, const bool vitals)
{
	auto &pi{plr.ctype.player_info};
	pi.primary_weapon_flags = inv.primary_flags;
	pi.laser_level = static_cast<laser_level>(std::min<unsigned>(inv.laser_level, underlying_value(DXX_MAXIMUM_LASER_LEVEL)));
	for (unsigned i = 0; i < MAX_SECONDARY_WEAPONS; ++i)
		pi.secondary_ammo[static_cast<secondary_weapon_index>(i)] = inv.secondary[i];
	pi.vulcan_ammo = inv.vulcan_ammo;
	pi.powerup_flags = player_flags{inv.powerup_flags};
#if DXX_BUILD_DESCENT == 2
	if (game_mode_hoard(Game_mode))
		pi.hoard.orbs = std::min(inv.orbs, player_info::max_hoard_orbs);
	pi.Omega_charge = std::clamp(inv.omega_charge, 0, MAX_OMEGA_CHARGE);
#endif
	if (vitals)
	{
		plr.shields = inv.shields;
		pi.energy = inv.energy;
	}
}

/* The inventory the pickup rules judge: a player who is only "faking"
 * invulnerability (after a respawn) may take a real one.
 */
[[nodiscard]]
nv::inventory for_rules(nv::inventory inv)
{
	if (inv.faking_invul)
		inv.powerup_flags &= ~underlying_value(player_flag::invulnerable);
	return inv;
}

[[nodiscard]]
nv::inventory_rules rules_for(const playernum_t pnum)
{
	nv::inventory_rules r;
	r.max_energy = MAX_ENERGY;
	r.max_shields = MAX_SHIELDS;
	const auto Difficulty_level{GameUniqueState.Difficulty_level};
	fix boost{3 * F1_0 + 3 * F1_0 * (NDL - underlying_value(Difficulty_level))};
#if DXX_BUILD_DESCENT == 2
	if (Difficulty_level == Difficulty_level_type::_0)
		boost += boost / 2;
#endif
	r.energy_boost = boost;
	r.shield_boost = boost;
	r.max_laser_level = underlying_value(MAX_LASER_LEVEL);
#if DXX_BUILD_DESCENT == 2
	r.max_super_laser_level = underlying_value(MAX_SUPER_LASER_LEVEL);
	r.ammo_rack_bit = underlying_value(player_flag::ammo_rack);
	r.has_team_flag_bit = underlying_value(player_flag::has_team_flag);
	r.max_orbs = player_info::max_hoard_orbs;
	r.capture_mode = game_mode_capture_flag(Game_mode);
	r.hoard_mode = game_mode_hoard(Game_mode);
	r.max_omega_charge = MAX_OMEGA_CHARGE;
#else
	r.max_super_laser_level = r.max_laser_level;
#endif
	r.vulcan_ammo_max = VULCAN_AMMO_MAX;
	for (unsigned i = 0; i < MAX_SECONDARY_WEAPONS; ++i)
		r.secondary_max[i] = Secondary_ammo_max[static_cast<secondary_weapon_index>(i)];
	r.team = underlying_value(multi_get_team_from_player(Netgame, pnum));
	return r;
}

[[nodiscard]]
nv::pickup_desc primary_desc(const primary_weapon_index w)
{
	return {nv::pickup_kind::primary, underlying_value(w), 0, HAS_PRIMARY_FLAG(w)};
}

[[nodiscard]]
nv::pickup_desc secondary_desc(const secondary_weapon_index w, const uint16_t amount)
{
	return {nv::pickup_kind::secondary, underlying_value(w), amount, 0};
}

[[nodiscard]]
nv::pickup_desc flag_desc(const player_flag f)
{
	return {nv::pickup_kind::flag_item, 0, 0, underlying_value(f)};
}

/* What each powerup gives (the cases of do_powerup). */
[[nodiscard]]
nv::pickup_desc desc_of(const powerup_type_t id)
{
	using nv::pickup_kind;
	switch (id)
	{
		case powerup_type_t::POW_EXTRA_LIFE:
			return {pickup_kind::always, 0, 0, 0};
		case powerup_type_t::POW_ENERGY:
			return {pickup_kind::energy, 0, 0, 0};
		case powerup_type_t::POW_SHIELD_BOOST:
			return {pickup_kind::shield, 0, 0, 0};
		case powerup_type_t::POW_LASER:
			return {pickup_kind::laser, 0, 0, 0};
		case powerup_type_t::POW_MISSILE_1:
			return secondary_desc(secondary_weapon_index::concussion, 1);
		case powerup_type_t::POW_MISSILE_4:
			return secondary_desc(secondary_weapon_index::concussion, 4);
		case powerup_type_t::POW_QUAD_FIRE:
			return flag_desc(player_flag::quad_lasers);
		case powerup_type_t::POW_VULCAN_WEAPON:
			return {pickup_kind::vulcan_cannon, underlying_value(primary_weapon_index::vulcan), 0, HAS_PRIMARY_FLAG(primary_weapon_index::vulcan)};
		case powerup_type_t::POW_SPREADFIRE_WEAPON:
			return primary_desc(primary_weapon_index::spreadfire);
		case powerup_type_t::POW_PLASMA_WEAPON:
			return primary_desc(primary_weapon_index::plasma);
		case powerup_type_t::POW_FUSION_WEAPON:
			return primary_desc(primary_weapon_index::fusion);
		case powerup_type_t::POW_PROXIMITY_WEAPON:
			return secondary_desc(secondary_weapon_index::proximity, 4);
		case powerup_type_t::POW_SMARTBOMB_WEAPON:
			return secondary_desc(secondary_weapon_index::smart, 1);
		case powerup_type_t::POW_MEGA_WEAPON:
			return secondary_desc(secondary_weapon_index::mega, 1);
		case powerup_type_t::POW_VULCAN_AMMO:
			return {pickup_kind::vulcan_ammo, 0, VULCAN_AMMO_AMOUNT, 0};
		case powerup_type_t::POW_HOMING_AMMO_1:
			return secondary_desc(secondary_weapon_index::homing, 1);
		case powerup_type_t::POW_HOMING_AMMO_4:
			return secondary_desc(secondary_weapon_index::homing, 4);
		case powerup_type_t::POW_CLOAK:
			return flag_desc(player_flag::cloaked);
		case powerup_type_t::POW_INVULNERABILITY:
			return flag_desc(player_flag::invulnerable);
		case powerup_type_t::POW_MEGAWOW:
			return {pickup_kind::always, 0, 0, 0};
#if DXX_BUILD_DESCENT == 2
		case powerup_type_t::POW_GAUSS_WEAPON:
			return {pickup_kind::vulcan_cannon, underlying_value(primary_weapon_index::gauss), 0, HAS_PRIMARY_FLAG(primary_weapon_index::gauss)};
		case powerup_type_t::POW_HELIX_WEAPON:
			return primary_desc(primary_weapon_index::helix);
		case powerup_type_t::POW_PHOENIX_WEAPON:
			return primary_desc(primary_weapon_index::phoenix);
		case powerup_type_t::POW_OMEGA_WEAPON:
			return {pickup_kind::omega, underlying_value(primary_weapon_index::omega), 0, HAS_PRIMARY_FLAG(primary_weapon_index::omega)};
		case powerup_type_t::POW_SUPER_LASER:
			return {pickup_kind::super_laser, 0, 0, 0};
		case powerup_type_t::POW_FULL_MAP:
			return flag_desc(player_flag::map_all);
		case powerup_type_t::POW_CONVERTER:
			return flag_desc(player_flag::converter);
		case powerup_type_t::POW_AMMO_RACK:
			return flag_desc(player_flag::ammo_rack);
		case powerup_type_t::POW_AFTERBURNER:
			return flag_desc(player_flag::afterburner);
		case powerup_type_t::POW_HEADLIGHT:
			return flag_desc(player_flag::headlight);
		case powerup_type_t::POW_SMISSILE1_1:
			return secondary_desc(secondary_weapon_index::flash, 1);
		case powerup_type_t::POW_SMISSILE1_4:
			return secondary_desc(secondary_weapon_index::flash, 4);
		case powerup_type_t::POW_GUIDED_MISSILE_1:
			return secondary_desc(secondary_weapon_index::guided, 1);
		case powerup_type_t::POW_GUIDED_MISSILE_4:
			return secondary_desc(secondary_weapon_index::guided, 4);
		case powerup_type_t::POW_SMART_MINE:
			return secondary_desc(secondary_weapon_index::smart_mine, 4);
		case powerup_type_t::POW_MERCURY_MISSILE_1:
			return secondary_desc(secondary_weapon_index::mercury, 1);
		case powerup_type_t::POW_MERCURY_MISSILE_4:
			return secondary_desc(secondary_weapon_index::mercury, 4);
		case powerup_type_t::POW_EARTHSHAKER_MISSILE:
			return secondary_desc(secondary_weapon_index::earthshaker, 1);
		/* The blue flag is taken by the red team and the other way round
		 * (player_hit_flag_powerup).
		 */
		case powerup_type_t::POW_FLAG_BLUE:
			return {pickup_kind::team_flag, underlying_value(team_number::red), 0, 0};
		case powerup_type_t::POW_FLAG_RED:
			return {pickup_kind::team_flag, underlying_value(team_number::blue), 0, 0};
		case powerup_type_t::POW_HOARD_ORB:
			return {pickup_kind::orb, 0, 0, 0};
#endif
		default:
			/* Keys (they stay, every player may take one) and the unused
			 * types: not arbitrated.
			 */
			return {};
	}
}

[[nodiscard]]
bool valid_powerup_id(const unsigned id)
{
	return id < MAX_POWERUP_TYPES && id < N_powerup_types;
}

/* The object a net id names here, if it is still the powerup it was
 * given to.
 */
[[nodiscard]]
imobjptridx_t object_of(const netid_t id)
{
	const auto b{A.table.find(id)};
	if (!b)
		return object_none;
	auto &Objects{LevelUniqueObjectState.Objects};
	if (b->objnum > Highest_object_index)
		return object_none;
	const auto &&objp{Objects.vmptridx(objnum_t{b->objnum})};
	if (underlying_value(objp->signature) != b->signature || objp->type != object_type::OBJ_POWERUP || (objp->flags & OF_SHOULD_BE_DEAD))
		return object_none;
	return objp;
}

[[nodiscard]]
netid_t netid_of(const object_base &obj, const objnum_t objnum)
{
	return A.table.netid_of(objnum, underlying_value(obj.signature));
}

void unhide(const nv::pending_pickup &p)
{
	if (!p.hidden)
		return;
	auto &Objects{LevelUniqueObjectState.Objects};
	if (p.objnum > Highest_object_index)
		return;
	auto &obj{*Objects.vmptr(objnum_t{p.objnum})};
	if (underlying_value(obj.signature) == p.signature && obj.type == object_type::OBJ_POWERUP && obj.render_type == render_type::RT_NONE)
		obj.render_type = static_cast<render_type>(p.saved_render);
}

void forget_object(const netid_t id)
{
	A.table.unbind(id);
	if (const auto was{A.pending.erase(id)})
		unhide(*was);
}

/* The local player's inventory as it reports it. */
[[nodiscard]]
nv::inventory own_inventory()
{
	auto &vmobjptr{LevelUniqueObjectState.Objects.vmptr};
	return inventory_of(get_local_plrobj());
}

void send_own_inventory(const bool force)
{
	const auto inv{own_inventory()};
	if (A.have_last_sent && inv == A.last_sent)
		return;
	const fix64 now{timer_query()};
	if (!force && A.have_last_sent && nv::inventory_differs_only_in_consumables(inv, A.last_sent) && now < A.last_sent_time + INVENTORY_REPORT_INTERVAL)
		return;
	nv::inventory_msg m{static_cast<uint8_t>(Player_num), A.applied_grants, inv};
	std::array<uint8_t, nv::inventory_msg::SIZE> buf;
	m.write(buf);
	send(session_msg::inventory, buf);
	A.last_sent = inv;
	A.have_last_sent = true;
	A.last_sent_time = now;
}

void show_cannot_use(const powerup_type_t id, const nv::pickup_desc &d, const nv::inventory &inv)
{
	constexpr auto flags{HM_DEFAULT | HM_REDUNDANT | HM_MAYDUPL};
	switch (d.kind)
	{
		case nv::pickup_kind::energy:
			HUD_init_message(flags, TXT_MAXED_OUT, TXT_ENERGY);
			break;
		case nv::pickup_kind::shield:
			HUD_init_message(flags, TXT_MAXED_OUT, TXT_SHIELD);
			break;
		case nv::pickup_kind::laser:
			HUD_init_message(flags, TXT_MAXED_OUT, TXT_LASER);
			break;
		case nv::pickup_kind::super_laser:
			HUD_init_message_literal(flags, "SUPER LASER MAXED OUT!");
			break;
		case nv::pickup_kind::primary:
		case nv::pickup_kind::omega:
		case nv::pickup_kind::vulcan_cannon:
			{
				const auto w{static_cast<primary_weapon_index>(d.index)};
				HUD_init_message(flags, "%s %s!", TXT_ALREADY_HAVE_THE, PRIMARY_WEAPON_NAMES(w));
			}
			break;
		case nv::pickup_kind::vulcan_ammo:
			HUD_init_message(flags, "%s %u %s!", TXT_ALREADY_HAVE, vulcan_ammo_scale(PLAYER_MAX_AMMO(player_flags{inv.powerup_flags}, VULCAN_AMMO_MAX)), TXT_VULCAN_ROUNDS);
			break;
		case nv::pickup_kind::secondary:
			{
				const auto w{static_cast<secondary_weapon_index>(d.index)};
				HUD_init_message(flags, "%s %i %ss!", TXT_ALREADY_HAVE, inv.secondary[d.index], SECONDARY_WEAPON_NAMES(w));
			}
			break;
		case nv::pickup_kind::flag_item:
			switch (id)
			{
				case powerup_type_t::POW_CLOAK:
					HUD_init_message(flags, "%s %s!", TXT_ALREADY_ARE, TXT_CLOAKED);
					break;
				case powerup_type_t::POW_INVULNERABILITY:
					HUD_init_message(flags, "%s %s!", TXT_ALREADY_ARE, TXT_INVULNERABLE);
					break;
				case powerup_type_t::POW_QUAD_FIRE:
					HUD_init_message(flags, "%s %s!", TXT_ALREADY_HAVE, TXT_QUAD_LASERS);
					break;
#if DXX_BUILD_DESCENT == 2
				case powerup_type_t::POW_FULL_MAP:
					HUD_init_message(flags, "%s %s!", TXT_ALREADY_HAVE, "the FULL MAP");
					break;
				case powerup_type_t::POW_CONVERTER:
					HUD_init_message(flags, "%s %s!", TXT_ALREADY_HAVE, "the Converter");
					break;
				case powerup_type_t::POW_AMMO_RACK:
					HUD_init_message(flags, "%s %s!", TXT_ALREADY_HAVE, "the Ammo rack");
					break;
				case powerup_type_t::POW_AFTERBURNER:
					HUD_init_message(flags, "%s %s!", TXT_ALREADY_HAVE, "the Afterburner");
					break;
				case powerup_type_t::POW_HEADLIGHT:
					HUD_init_message(flags, "%s %s!", TXT_ALREADY_HAVE, "the Headlight boost");
					break;
#endif
				default:
					break;
			}
			break;
		default:
			break;
	}
}

/* The HUD message and sound for another player's flag or orb (the v1
 * MULTI_GOT_FLAG / MULTI_GOT_ORB).
 */
void report_others_pickup(const playernum_t pnum, const powerup_type_t id)
{
#if DXX_BUILD_DESCENT == 2
	if (pnum >= N_players || pnum == Player_num)
		return;
	auto &plr{*vcplayerptr(pnum)};
	auto &Objects{LevelUniqueObjectState.Objects};
	auto &plrobj{*Objects.vmptr(plr.objnum)};
	if ((id == powerup_type_t::POW_FLAG_BLUE || id == powerup_type_t::POW_FLAG_RED) && game_mode_capture_flag(Game_mode))
	{
		digi_start_sound_queued(multi_get_team_from_player(Netgame, pnum) == team_number::blue
			? sound_effect::SOUND_HUD_BLUE_GOT_FLAG
			: sound_effect::SOUND_HUD_RED_GOT_FLAG, F1_0 * 2);
		plrobj.ctype.player_info.powerup_flags |= player_flag::has_team_flag;
		HUD_init_message(HM_MULTI, "%s picked up a flag!", static_cast<const char *>(plr.callsign));
	}
	else if (id == powerup_type_t::POW_HOARD_ORB && game_mode_hoard(Game_mode))
	{
		digi_play_sample(+(Game_mode & GM_TEAM) && multi_get_team_from_player(Netgame, pnum) == multi_get_team_from_player(Netgame, Player_num)
			? sound_effect::SOUND_FRIEND_GOT_ORB
			: sound_effect::SOUND_OPPONENT_GOT_ORB, F1_0 * 2);
		plrobj.ctype.player_info.powerup_flags |= player_flag::has_team_flag;
		HUD_init_message(HM_MULTI, "%s picked up an orb!", static_cast<const char *>(plr.callsign));
	}
#else
	(void)pnum;
	(void)id;
#endif
}

void send_grant(const playernum_t pnum, const netid_t id, const powerup_type_t powerup, const uint32_t count, const uint32_t remaining, const bool removed)
{
	const uint8_t life{pnum < MAX_PLAYERS ? A.mirrors[pnum].life() : uint8_t{}};
	nv::pickup_grant_msg g{static_cast<uint8_t>(pnum), id, static_cast<uint8_t>(powerup), count, remaining, static_cast<uint8_t>(removed ? underlying_value(nv::grant_flag::removed) : 0), life};
	std::array<uint8_t, nv::pickup_grant_msg::SIZE> buf;
	g.write(buf);
	send(session_msg::pickup_grant, buf);
}

struct host_decision
{
	nv::pickup_decision d;
	imobjptridx_t obj{object_none};
	nv::pickup_desc desc;
};

[[nodiscard]]
bool player_alive_for_authority(const playernum_t pnum)
{
	if (pnum >= N_players || pnum >= MAX_PLAYERS || A.dropped[pnum])
		return false;
	auto &plr{*vcplayerptr(pnum)};
	if (plr.connected != player_connection_status::playing)
		return false;
	auto &Objects{LevelUniqueObjectState.Objects};
	if (Objects.vcptr(plr.objnum)->type != object_type::OBJ_PLAYER)
		return false;
	if (pnum == Player_num && Player_dead_state != player_dead_state::no)
		return false;
	return true;
}

/* The host's decision on player `pnum` taking the powerup `id`. */
[[nodiscard]]
host_decision host_decide(const playernum_t pnum, const netid_t id, const uint8_t powerup_id)
{
	host_decision r;
	nv::pickup_object_view o;
	if (const auto &&objp{object_of(id)}; objp != object_none && underlying_value(get_powerup_id(objp)) == powerup_id)
	{
		r.obj = objp;
		r.desc = desc_of(get_powerup_id(objp));
		o.exists = true;
		o.desc = r.desc;
		o.count = static_cast<uint32_t>(std::max(objp->ctype.powerup_info.count, 0));
		const auto &m{A.meta[objp.get_unchecked_index()]};
		o.spat_owner = m.spat_owner;
		o.spat_until = m.spat_until;
	}
	nv::requester_view q{static_cast<uint8_t>(pnum), player_alive_for_authority(pnum), true};
	nv::inventory inv;
	if (pnum == Player_num)
		inv = own_inventory();
	else if (pnum < MAX_PLAYERS)
	{
		inv = A.mirrors[pnum].current();
		if (q.alive && r.obj != object_none)
		{
			auto &Objects{LevelUniqueObjectState.Objects};
			auto &ship{*Objects.vcptr(vcplayerptr(pnum)->objnum)};
			vms_vector pos{ship.pos};
			fix speed{};
			if (!net_interp_newest_position(pnum, pos, speed))
				speed = vm_vec_mag_quick(ship.mtype.phys_info.velocity).d;
			q.in_range = nv::pickup_in_range(vm_vec_dist_quick(pos, r.obj->pos).d, r.obj->size, ship.size, speed);
		}
	}
	r.d = nv::decide_pickup(o, q, for_rules(inv), rules_for(pnum), GameTime64);
	return r;
}

/* The host grants a client's request. */
void host_grant_remote(const playernum_t pnum, const netid_t id, const host_decision &h)
{
	auto &obj{*h.obj};
	const auto powerup{get_powerup_id(obj)};
	auto &mirror{A.mirrors[pnum]};
	mirror.on_grant(rules_for(pnum), h.desc, h.d.outcome);
	auto &Objects{LevelUniqueObjectState.Objects};
	write_inventory(*Objects.vmptr(vcplayerptr(pnum)->objnum), mirror.current(), true);
	const bool removed{h.d.outcome.consumed};
	if (removed)
	{
		obj.flags |= OF_SHOULD_BE_DEAD;
		A.table.unbind(id);
	}
	else
		obj.ctype.powerup_info.count = static_cast<int>(h.d.outcome.remaining);
	send_grant(pnum, id, powerup, h.d.outcome.taken, h.d.outcome.remaining, removed);
	report_others_pickup(pnum, powerup);
	con_printf(CON_VERBOSE, "net: P#%u takes powerup %u (id %04x)", pnum, underlying_value(powerup), id);
}

/* The host's own ship touched a powerup: the same decision, at once; the
 * effect is do_powerup's, as in a game without the network.
 */
void host_touch(const vmobjptridx_t obj, const netid_t id)
{
	const auto powerup{get_powerup_id(obj)};
	const auto h{host_decide(Player_num, id, underlying_value(powerup))};
	if (!h.d.grant)
	{
		if (h.d.reason == nv::deny_reason::cannot_use)
			show_cannot_use(powerup, h.desc, own_inventory());
		return;
	}
	const auto before{obj->ctype.powerup_info.count};
	const auto used{do_powerup(obj, powerup_pickup_mode::granted)};
	const auto after{obj->ctype.powerup_info.count};
	if (!used && after == before)
		/* do_powerup disagreed with the rules: nothing happened. */
		return;
	if (used)
	{
		obj->flags |= OF_SHOULD_BE_DEAD;
		A.table.unbind(id);
	}
	const uint32_t count{h.desc.kind == nv::pickup_kind::omega ? static_cast<uint32_t>(std::max(before, 0)) : static_cast<uint32_t>(std::max(before - after, 0))};
	send_grant(Player_num, id, powerup, count, static_cast<uint32_t>(std::max(after, 0)), used);
}

/* A client's own grant arrived: apply its effect.  A grant that arrives
 * while the player is dead but before its deres (it asked just before
 * dying) still goes into the inventory, so that the report sent with the
 * deres includes it as the host's copy does, and the host drops it with
 * the rest.  A grant for a life whose deres was already sent was in the
 * host's drop of that life: it is not applied at all, least of all to the
 * ship of the next life.
 */
void apply_own_grant(const imobjptridx_t objp, const nv::pickup_grant_msg &g)
{
	if (!A.life.current(g.life))
	{
		con_printf(CON_VERBOSE, "net: grant of powerup %u for an ended life (%u, now %u) ignored", g.powerup_id, g.life, A.life.life);
		return;
	}
	const auto powerup{static_cast<powerup_type_t>(g.powerup_id)};
	const auto desc{desc_of(powerup)};
	const bool alive{Player_dead_state == player_dead_state::no && ConsoleObject->type == object_type::OBJ_PLAYER};
	if (alive && objp != object_none)
	{
		if (desc.kind == nv::pickup_kind::vulcan_cannon || desc.kind == nv::pickup_kind::omega)
			objp->ctype.powerup_info.count = static_cast<int>(g.count);
		if (!do_powerup(objp, powerup_pickup_mode::granted) && objp->ctype.powerup_info.count == static_cast<int>(g.count))
			con_printf(CON_VERBOSE, "net: granted powerup %u was of no use here", g.powerup_id);
		return;
	}
	if (desc.kind == nv::pickup_kind::shield || desc.kind == nv::pickup_kind::energy)
		return;
	auto &vmobjptr{LevelUniqueObjectState.Objects.vmptr};
	auto &plrobj{get_local_plrobj()};
	auto inv{inventory_of(plrobj)};
	nv::apply_pickup(inv, rules_for(Player_num), desc, {true, g.removed(), g.count, g.remaining});
	write_inventory(plrobj, inv, false);
}

void receive_grant(const std::span<const uint8_t> payload)
{
	const auto g{nv::pickup_grant_msg::read(payload)};
	if (!g || !valid_powerup_id(g->powerup_id))
		return;
	const auto &&objp{object_of(g->netid)};
	if (g->pid == Player_num)
	{
		++A.applied_grants;
		if (const auto was{A.pending.erase(g->netid)})
			unhide(*was);
		apply_own_grant(objp, *g);
	}
	else
		report_others_pickup(g->pid, static_cast<powerup_type_t>(g->powerup_id));
	if (objp == object_none)
	{
		if (g->removed())
			forget_object(g->netid);
		return;
	}
	if (g->removed())
	{
		objp->flags |= OF_SHOULD_BE_DEAD;
		forget_object(g->netid);
	}
	else
		objp->ctype.powerup_info.count = static_cast<int>(g->remaining);
}

void receive_deny(const std::span<const uint8_t> payload)
{
	const auto d{nv::pickup_deny_msg::read(payload)};
	if (!d)
		return;
	if (const auto was{A.pending.deny(d->netid, timer_query(), PICKUP_COOLDOWN)})
		unhide(*was);
}

void receive_create(const std::span<const uint8_t> payload)
{
	const auto m{nv::obj_create_msg::read(payload)};
	if (!m || m->type != underlying_value(object_type::OBJ_POWERUP) || !valid_powerup_id(m->id))
		return;
	const auto &&useg{Segments.vmptridx.check_untrusted(segnum_t{m->segnum})};
	if (!useg)
		return;
	const auto &&segnum{*useg};
	/* An id is never reused while bound; a stale binding is dropped. */
	if (const auto &&old{object_of(m->netid)}; old != object_none)
		old->flags |= OF_SHOULD_BE_DEAD;
	forget_object(m->netid);
	const auto id{static_cast<powerup_type_t>(m->id)};
	const auto pos{to_vector(m->pos)};
	const auto &&objp{obj_create(LevelUniqueObjectState, LevelSharedSegmentState, LevelUniqueSegmentState, object_type::OBJ_POWERUP, m->id, segnum, pos, &vmd_identity_matrix, Powerup_info[id].size, object::control_type::powerup, object::movement_type::physics, render_type::RT_POWERUP)};
	if (objp == object_none)
	{
		con_printf(CON_URGENT, "net: cannot create powerup %u (id %04x): no free object", m->id, m->netid);
		return;
	}
	auto &obj{*objp};
	obj.mtype.phys_info.velocity = to_vector(m->velocity);
	obj.mtype.phys_info.drag = 512;
	obj.mtype.phys_info.mass = F1_0;
	obj.mtype.phys_info.flags = PF_BOUNCE;
	obj.rtype.vclip_info.vclip_num = Powerup_info[id].vclip_num;
	obj.rtype.vclip_info.frametime = Vclip[obj.rtype.vclip_info.vclip_num].frame_time;
	obj.rtype.vclip_info.framenum = 0;
	obj.ctype.powerup_info.count = static_cast<int>(m->count);
	/* The host decides when it expires (OBJ_REMOVE). */
	obj.lifeleft = IMMORTAL_TIME;
#if DXX_BUILD_DESCENT == 2
	if (m->has(nv::obj_create_flag::player_dropped))
		obj.flags |= OF_PLAYER_DROPPED;
#endif
	if (m->has(nv::obj_create_flag::spat) && m->owner == Player_num)
		obj.ctype.powerup_info.flags |= PF_SPAT_BY_PLAYER;
	A.table.bind(m->netid, objp.get_unchecked_index(), underlying_value(obj.signature));
	if (m->has(nv::obj_create_flag::appear))
		object_create_explosion_without_damage(Vclip, segnum, pos, i2f(5), vclip_index::powerup_disappearance);
}

void receive_remove(const std::span<const uint8_t> payload)
{
	const auto m{nv::obj_remove_msg::read(payload)};
	if (!m)
		return;
	if (const auto &&objp{object_of(m->netid)}; objp != object_none)
	{
		objp->flags |= OF_SHOULD_BE_DEAD;
		if (m->reason == nv::obj_remove_reason::expired)
		{
			object_create_explosion_without_damage(Vclip, Segments.vmptridx(objp->segnum), objp->pos, F1_0 * 7 / 2, vclip_index::powerup_disappearance);
			if (const auto sound_num{Vclip[vclip_index::powerup_disappearance].sound_num}; sound_num != sound_effect::None)
				digi_link_sound_to_pos(sound_num, Segments.vcptridx(objp->segnum), sidenum_t::WLEFT, objp->pos, 0, F1_0);
		}
	}
	forget_object(m->netid);
}

void receive_settle(const std::span<const uint8_t> payload)
{
	const auto m{nv::obj_settle_msg::read(payload)};
	if (!m)
		return;
	const auto &&objp{object_of(m->netid)};
	if (objp == object_none)
		return;
	const auto &&useg{Segments.vmptridx.check_untrusted(segnum_t{m->segnum})};
	if (!useg)
		return;
	auto &Objects{LevelUniqueObjectState.Objects};
	objp->pos = to_vector(m->pos);
	objp->mtype.phys_info.velocity = {};
	if (objp->segnum != *useg)
		obj_relink(Objects.vmptr, Segments.vmptr, objp, *useg);
}

/* Host: a client's INVENTORY report. */
void host_receive_inventory(const playernum_t from, const std::span<const uint8_t> payload)
{
	const auto m{nv::inventory_msg::read(payload)};
	if (!m || m->pid != from || from >= N_players || from == Player_num)
		return;
	/* A dead player's report (after its items were dropped) waits for
	 * its reappearance.
	 */
	if (A.dropped[from])
		return;
	auto &mirror{A.mirrors[from]};
	mirror.on_report(rules_for(from), m->inv, m->seq);
	auto &Objects{LevelUniqueObjectState.Objects};
	write_inventory(*Objects.vmptr(vcplayerptr(from)->objnum), mirror.current(), true);
	nv::inventory_msg relay{static_cast<uint8_t>(from), m->seq, mirror.current()};
	std::array<uint8_t, nv::inventory_msg::SIZE> buf;
	relay.write(buf);
	send(session_msg::inventory, buf, from);
}

/* Client: another player's inventory, from the host. */
void client_receive_inventory(const std::span<const uint8_t> payload)
{
	const auto m{nv::inventory_msg::read(payload)};
	if (!m || m->pid == Player_num || m->pid >= N_players || m->pid >= MAX_PLAYERS)
		return;
	auto &Objects{LevelUniqueObjectState.Objects};
	write_inventory(*Objects.vmptr(vcplayerptr(m->pid)->objnum), m->inv, false);
}

void host_receive_request(const playernum_t from, const std::span<const uint8_t> payload)
{
	const auto m{nv::pickup_request_msg::read(payload)};
	if (!m || from == Player_num || from >= MAX_PLAYERS)
		return;
	const auto h{host_decide(from, m->netid, m->powerup_id)};
	if (h.d.grant)
	{
		host_grant_remote(from, m->netid, h);
		return;
	}
	nv::pickup_deny_msg d{m->netid, h.d.reason};
	std::array<uint8_t, nv::pickup_deny_msg::SIZE> buf;
	d.write(buf);
	send_to(from, session_msg::pickup_deny, buf);
}

void host_receive_drop(const playernum_t from, const std::span<const uint8_t> payload)
{
	const auto m{nv::drop_request_msg::read(payload)};
	if (!m || from == Player_num || from >= MAX_PLAYERS || !valid_powerup_id(m->powerup_id))
		return;
	if (!player_alive_for_authority(from))
		return;
	const auto id{static_cast<powerup_type_t>(m->powerup_id)};
	const auto desc{desc_of(id)};
	const auto rules{rules_for(from)};
	auto &mirror{A.mirrors[from]};
	if (!nv::evaluate_drop(mirror.current(), rules, desc, m->count))
	{
		con_printf(CON_VERBOSE, "net: P#%u cannot drop powerup %u (count %u)", from, m->powerup_id, m->count);
		return;
	}
	mirror.on_drop(rules, desc, m->count);
	auto &Objects{LevelUniqueObjectState.Objects};
	auto &ship{*Objects.vmptr(vcplayerptr(from)->objnum)};
	write_inventory(ship, mirror.current(), true);
	/* Spat from the ship where its owner had it, not from its delayed
	 * interpolated pose.
	 */
	net_interp_snap_to_newest(from);
	Net_create_loc = 0;
	const auto &&objp{spit_powerup(LevelUniqueObjectState, LevelSharedSegmentState, LevelUniqueSegmentState, Vclip, ship, id, static_cast<unsigned>(d_rand()))};
	if (objp == object_none)
		return;
	if (desc.kind == nv::pickup_kind::vulcan_cannon || desc.kind == nv::pickup_kind::omega)
		objp->ctype.powerup_info.count = static_cast<int>(m->count);
	net_objects_announce(objp, static_cast<uint8_t>(from), false);
}

/* Host, every frame: a powerup that is gone here (expired, destroyed) is
 * removed everywhere; a moving one that came to rest is put at the same
 * place everywhere.  Client: forget ids whose objects are gone.
 */
void scan_objects()
{
	auto &Objects{LevelUniqueObjectState.Objects};
	const bool host{multi_i_am_master()};
	for (unsigned o = 0; o < MAX_OBJECTS; ++o)
	{
		const auto id{A.table.netid_at(static_cast<uint16_t>(o))};
		if (id == NETID_NONE)
			continue;
		const auto b{A.table.find(id)};
		const bool in_use{o <= static_cast<unsigned>(Highest_object_index)};
		const auto &&objp{Objects.vmptridx(static_cast<objnum_t>(in_use ? o : 0))};
		const bool same{in_use && underlying_value(objp->signature) == b->signature && objp->type == object_type::OBJ_POWERUP};
		if (!same || (objp->flags & OF_SHOULD_BE_DEAD))
		{
			if (host)
			{
				const bool expired{same && objp->lifeleft <= 0};
				nv::obj_remove_msg r{id, expired ? nv::obj_remove_reason::expired : nv::obj_remove_reason::gone};
				std::array<uint8_t, nv::obj_remove_msg::SIZE> buf;
				r.write(buf);
				send(session_msg::obj_remove, buf);
			}
			forget_object(id);
			continue;
		}
		if (!host)
			continue;
		auto &m{A.meta[o]};
		auto &velocity{objp->mtype.phys_info.velocity};
		if (vm_vec_mag_quick(velocity).d > nv::SETTLE_SPEED)
			m.moving = true;
		else if (m.moving)
		{
			m.moving = false;
			velocity = {};
			nv::obj_settle_msg s{id, static_cast<uint16_t>(objp->segnum), to_array(objp->pos)};
			std::array<uint8_t, nv::obj_settle_msg::SIZE> buf;
			s.write(buf);
			send(session_msg::obj_settle, buf);
		}
	}
}

}

bool net_objects_active()
{
	return +(Game_mode & GM_NETWORK) && Newdemo_state != ND_STATE_PLAYBACK;
}

void net_objects_level_start()
{
	if (!net_objects_active())
		return;
	A.table.reset();
	A.meta.fill({});
	A.pending.reset();
	A.dropped.fill(false);
	A.applied_grants = 0;
	A.life.reset();
	A.have_last_sent = false;
	auto &Objects{LevelUniqueObjectState.Objects};
	for (unsigned i = 0; i < MAX_PLAYERS; ++i)
	{
		const auto objnum{vcplayerptr(static_cast<playernum_t>(i))->objnum};
		A.mirrors[i].reset(objnum <= Highest_object_index ? inventory_of(*Objects.vcptr(objnum)) : nv::inventory{});
	}
	for (auto &&objp : Objects.vmptridx)
		if (objp->type == object_type::OBJ_POWERUP)
			A.table.bind(nv::level_netid(objp.get_unchecked_index()), objp.get_unchecked_index(), underlying_value(objp->signature));
	con_printf(CON_VERBOSE, "net: %zu level powerups", A.table.size());
}

void net_objects_snapshot_begin()
{
	A.table.reset();
	A.meta.fill({});
	A.pending.reset();
	A.dropped.fill(false);
	A.applied_grants = 0;
	A.life.reset();
	A.have_last_sent = false;
}

void net_objects_snapshot_bind(const vmobjptridx_t obj, const uint16_t netid)
{
	if (netid == NETID_NONE || obj->type != object_type::OBJ_POWERUP)
		return;
	A.table.bind(netid, obj.get_unchecked_index(), underlying_value(obj->signature));
	/* The host decides when it expires. */
	obj->lifeleft = IMMORTAL_TIME;
}

uint16_t net_objects_netid_of(const vcobjptridx_t obj)
{
	if (obj->type != object_type::OBJ_POWERUP)
		return NETID_NONE;
	return netid_of(*obj, obj.get_unchecked_index());
}

void net_objects_host_join(const playernum_t pnum)
{
	if (pnum >= MAX_PLAYERS)
		return;
	/* The joiner (a new player or the one who left this slot) starts
	 * with an empty copy, not with what the slot's ship may still hold:
	 * the items of a player who left were dropped when it left, and the
	 * joiner reports its own inventory in its first frame and before any
	 * request.
	 */
	A.mirrors[pnum].reset({});
	A.dropped[pnum] = false;
}

void net_objects_frame()
{
	if (!net_objects_active() || (Network_status != network_state::playing && Network_status != network_state::endlevel))
		return;
	scan_objects();
	if (!multi_i_am_master())
		A.pending.expire(timer_query(), PICKUP_TIMEOUT, PICKUP_COOLDOWN, [](const nv::pending_pickup &p) { unhide(p); });
	send_own_inventory(false);
}

void net_objects_receive(const playernum_t from, const uint8_t type, const std::span<const uint8_t> payload)
{
	if (!net_objects_active())
		return;
	const auto t{static_cast<session_msg>(type)};
	if (multi_i_am_master())
	{
		switch (t)
		{
			case session_msg::inventory:
				host_receive_inventory(from, payload);
				break;
			case session_msg::pickup_request:
				host_receive_request(from, payload);
				break;
			case session_msg::drop_request:
				host_receive_drop(from, payload);
				break;
			default:
				con_printf(CON_VERBOSE, "net: object message %u from client P#%u ignored", type, from);
				break;
		}
		return;
	}
	switch (t)
	{
		case session_msg::inventory:
			client_receive_inventory(payload);
			break;
		case session_msg::obj_create:
			receive_create(payload);
			break;
		case session_msg::obj_remove:
			receive_remove(payload);
			break;
		case session_msg::obj_settle:
			receive_settle(payload);
			break;
		case session_msg::pickup_grant:
			receive_grant(payload);
			break;
		case session_msg::pickup_deny:
			receive_deny(payload);
			break;
		default:
			con_printf(CON_VERBOSE, "net: object message %u from the host ignored", type);
			break;
	}
}

bool net_objects_touch(const vmobjptridx_t powerup)
{
	if (!net_objects_active())
		return false;
	const auto id{netid_of(*powerup, powerup.get_unchecked_index())};
	if (id == NETID_NONE)
		return false;
	const auto powerup_id{get_powerup_id(powerup)};
	const auto desc{desc_of(powerup_id)};
	if (desc.kind == nv::pickup_kind::none)
	{
		/* Keys: taken locally, and they stay (do_powerup never uses them
		 * up in a multiplayer game).
		 */
		do_powerup(powerup);
		return true;
	}
	if (multi_i_am_master())
	{
		host_touch(powerup, id);
		return true;
	}
	if (powerup->render_type == render_type::RT_NONE)
		return true;
	const fix64 now{timer_query()};
	if (!A.pending.can_request(id, now))
		return true;
	auto &pinfo{powerup->ctype.powerup_info};
	if ((pinfo.flags & PF_SPAT_BY_PLAYER) && pinfo.creation_time > 0 && GameTime64 < pinfo.creation_time + SPAT_DELAY)
		return true;
	const auto inv{own_inventory()};
	if (!nv::evaluate_pickup(for_rules(inv), rules_for(Player_num), desc, static_cast<uint32_t>(std::max(pinfo.count, 0))).usable)
	{
		show_cannot_use(powerup_id, desc, inv);
		return true;
	}
	/* The host decides with the inventory as it is now. */
	send_own_inventory(true);
	nv::pickup_request_msg rq{id, static_cast<uint8_t>(powerup_id)};
	std::array<uint8_t, nv::pickup_request_msg::SIZE> buf;
	rq.write(buf);
	send(session_msg::pickup_request, buf);
	std::optional<nv::pending_pickup> evicted;
	A.pending.add({id, powerup.get_unchecked_index(), underlying_value(powerup->signature), static_cast<uint8_t>(powerup->render_type), true, now, 0}, evicted);
	if (evicted)
		unhide(*evicted);
	powerup->render_type = render_type::RT_NONE;
	return true;
}

void net_objects_announce(const vmobjptridx_t obj, const uint8_t owner, const bool appear)
{
	if (!net_objects_active() || !multi_i_am_master() || obj->type != object_type::OBJ_POWERUP)
		return;
	const auto id{A.table.allocate(0)};
	if (id == NETID_NONE)
	{
		con_printf(CON_URGENT, "net: no free net id for a new powerup");
		return;
	}
	const auto objnum{obj.get_unchecked_index()};
	A.table.bind(id, objnum, underlying_value(obj->signature));
	auto &m{A.meta[objnum]};
	m.spat_owner = owner;
	m.spat_until = owner != NO_OWNER ? GameTime64 + SPAT_DELAY : 0;
	m.moving = vm_vec_mag_quick(obj->mtype.phys_info.velocity).d > nv::SETTLE_SPEED;
	/* The v1 owner, for the snapshot's two passes. */
	map_objnum_local_to_local(objnum);
	uint8_t flags{};
	if (appear)
		flags |= underlying_value(nv::obj_create_flag::appear);
#if DXX_BUILD_DESCENT == 2
	if (obj->flags & OF_PLAYER_DROPPED)
		flags |= underlying_value(nv::obj_create_flag::player_dropped);
#endif
	if (owner != NO_OWNER)
		flags |= underlying_value(nv::obj_create_flag::spat);
	nv::obj_create_msg c{id, underlying_value(obj->type), underlying_value(get_powerup_id(obj)), static_cast<uint16_t>(obj->segnum), to_array(obj->pos), to_array(obj->mtype.phys_info.velocity), static_cast<uint32_t>(std::max(obj->ctype.powerup_info.count, 0)), flags, owner};
	std::array<uint8_t, nv::obj_create_msg::SIZE> buf;
	c.write(buf);
	send(session_msg::obj_create, buf);
}

void net_objects_host_drop_player_eggs(const playernum_t pnum)
{
	if (!net_objects_active() || !multi_i_am_master() || pnum >= MAX_PLAYERS || pnum >= N_players)
		return;
	if (A.dropped[pnum])
		return;
	A.dropped[pnum] = true;
	auto &Objects{LevelUniqueObjectState.Objects};
	const auto &&objp{Objects.vmptridx(vcplayerptr(pnum)->objnum)};
	if (objp->type != object_type::OBJ_PLAYER && objp->type != object_type::OBJ_GHOST)
		return;
	if (pnum != Player_num)
	{
		net_interp_snap_to_newest(pnum);
		write_inventory(*objp, A.mirrors[pnum].current(), true);
	}
	Net_create_loc = 0;
	drop_player_powerup_eggs(objp);
	const auto created{std::min<unsigned>(Net_create_loc, MAX_NET_CREATE_OBJECTS)};
	for (unsigned i = 0; i < created; ++i)
		net_objects_announce(Objects.vmptridx(Net_create_objnums[i]), NO_OWNER, false);
	Net_create_loc = 0;
	if (pnum != Player_num)
	{
		/* The ship carries nothing any more: the items lie in the level
		 * (MultiLevelInv must not count them twice), and a player who
		 * joins this slot later must not start with them.
		 */
		A.mirrors[pnum].clear();
		write_inventory(*objp, A.mirrors[pnum].current(), false);
	}
	con_printf(CON_VERBOSE, "net: P#%u dropped %u powerups", pnum, created);
}

void net_objects_player_reappeared(const playernum_t pnum)
{
	if (pnum >= MAX_PLAYERS)
		return;
	A.dropped[pnum] = false;
	if (pnum == Player_num)
	{
		/* Report the new ship's inventory even if it equals the old. */
		A.have_last_sent = false;
		A.life.on_reappear();
	}
}

void net_objects_own_deres()
{
	if (!net_objects_active())
		return;
	A.life.on_deres();
}

bool net_objects_request_drop(const powerup_type_t id, const uint32_t count)
{
	if (!net_objects_active() || multi_i_am_master())
		return false;
	/* The report of everything before the drop goes first; the one after
	 * the drop follows the request.
	 */
	send_own_inventory(true);
	nv::drop_request_msg m{static_cast<uint8_t>(id), count};
	std::array<uint8_t, nv::drop_request_msg::SIZE> buf;
	m.write(buf);
	send(session_msg::drop_request, buf);
	return true;
}

void net_objects_flush_inventory()
{
	if (!net_objects_active())
		return;
	send_own_inventory(true);
}

void net_objects_send_all_inventories()
{
	if (!net_objects_active() || !multi_i_am_master())
		return;
	for (playernum_t i = 0; i < N_players; ++i)
	{
		if (vcplayerptr(i)->connected != player_connection_status::playing)
			continue;
		const auto inv{i == Player_num ? own_inventory() : A.mirrors[i].current()};
		nv::inventory_msg m{static_cast<uint8_t>(i), 0, inv};
		std::array<uint8_t, nv::inventory_msg::SIZE> buf;
		m.write(buf);
		send(session_msg::inventory, buf);
	}
}

}

#endif
