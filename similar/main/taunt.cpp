/*
 * This file is part of the DXX-Rebirth project <https://github.com/dxx-rebirth/dxx-rebirth/>.
 * It is copyright by its individual contributors, as recorded in the
 * project's Git history.  See COPYING.txt at the top level for license
 * terms and a link to the Git history.
 */
/*
 * Taunts (Documentation/taunts.md): the key, the own sample, the
 * playback from the taunting ship, the network (TAUNT_REQUEST to the
 * host, TAUNT to everyone), the spam protection, the bots, muting.
 */

#include "dxxsconf.h"
#include <algorithm>
#include <array>
#include <cinttypes>
#include <cstring>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "taunt.h"
#include "taunt_sample.h"

#if DXX_BUILD_DESCENT == 2
#include "config.h"
#include "console.h"
#include "digi.h"
#include "game.h"
#include "hudmsg.h"
#include "movement_record.h"
#include "movement_record_format.h"
#include "multi.h"
#include "newdemo.h"
#include "object.h"
#include "physfsx.h"
#include "player.h"
#include "maths.h"
#include "segment.h"
#include "strutil.h"
#include "timer.h"
#include "d_levelstate.h"
#if DXX_USE_MULTIPLAYER
#include "bot.h"
#endif
#if DXX_USE_UDP
#include "net_v2_session.h"
#include "net_v2_game.h"
#endif

namespace dsx {

namespace {

namespace tt = ::dcx::taunt;
namespace mr = ::dcx::movrec;

/* Taunts are heard as far as a weapon's sound (digi_link_sound_to_object). */
constexpr vm_distance TAUNT_DISTANCE{F1_0 * 256};

/* What a taunt plays: a starter horn, or a player's own sample named by
 * its id and size.
 */
struct sample_ref
{
	tt::sample_kind kind{tt::sample_kind::horn1};
	std::uint32_t id{};
	std::uint32_t size{};
};

/* The own sample, read once from the user's directory (again after a
 * change in the sound options).
 */
struct own_sample
{
	bool read{};
	std::vector<std::uint8_t> wire;
	std::vector<std::int16_t> mixer;
	std::uint32_t id{};
	std::size_t samples{};
	std::string status;
};

/* A taunt playing: the signature of the ship it follows. */
struct voice
{
	object_signature_t signature{};
};

struct taunt_state
{
	own_sample own;
	std::array<std::vector<std::int16_t>, tt::STARTER_HORNS> horns;
	std::array<voice, MAX_PLAYERS> voices{};
	/* Host: each sender's taunts; client: each player's taunts as
	 * relayed.
	 */
	std::array<tt::rate_limiter, MAX_PLAYERS> received{};
	/* This machine's own key presses. */
	tt::rate_limiter local;
	/* Callsigns muted with `/mute` (for the session). */
	std::vector<callsign_t> muted;
};

taunt_state T;

/* A slot of its own for each player, and one for the preview. */
constexpr unsigned PREVIEW_SLOT{MAX_PLAYERS};
static_assert(PREVIEW_SLOT < ::dcx::DIGI_CUSTOM_SLOTS);

std::uint64_t now_ms()
{
	const fix64 t{timer_query()};
	return t > 0 ? static_cast<std::uint64_t>(t) * 1000 / F1_0 : 0;
}

/* The first usable one of the own files; an unusable one is reported
 * and the next one tried.
 */
void read_own_sample()
{
	auto &o{T.own};
	o = {};
	o.read = true;
	std::string problems;
	const auto refuse{[&problems](const char *const name, const std::string &why) {
		con_printf(CON_URGENT, "taunt: %s: %s", name, why.c_str());
		if (problems.empty())
			problems = std::string{name} + ": " + why;
	}};
	for (const char *const name : tt::OWN_FILE_NAMES)
	{
		auto f{PHYSFSX_openReadBuffered(name).first};
		if (!f)
			continue;
		const auto length{PHYSFS_fileLength(f)};
		if (length < 0 || static_cast<std::uint64_t>(length) > tt::MAX_SOURCE_FILE)
		{
			refuse(name, "larger than 16 MiB");
			continue;
		}
		std::vector<std::uint8_t> bytes(static_cast<std::size_t>(length));
		if (PHYSFSX_readBytes(f, bytes.data(), bytes.size()) != static_cast<PHYSFS_sint64>(bytes.size()))
		{
			refuse(name, "cannot be read");
			continue;
		}
		std::string error;
		const auto pcm{tt::prepare_file(bytes, error)};
		if (!pcm)
		{
			refuse(name, error);
			continue;
		}
		o.wire = tt::encode_wire(*pcm);
		o.id = tt::wire_id(o.wire);
		o.samples = pcm->size();
		o.mixer = tt::to_mixer_format(*pcm);
		char text[96];
		std::snprintf(text, sizeof(text), "%s, %.1f s", name, static_cast<double>(o.samples) / tt::SAMPLE_RATE);
		o.status = text;
		con_printf(CON_NORMAL, "taunt: own sample %s (%zu bytes to send, id %08" PRIx32 ")", text, o.wire.size(), o.id);
		return;
	}
	o.status = problems.empty() ? std::string{"no taunt.wav/.mp3/.ogg/.flac found"} : problems + "; Horn 1";
}

const own_sample &own()
{
	if (!T.own.read)
		read_own_sample();
	return T.own;
}

const std::vector<std::int16_t> &horn_samples(const tt::sample_kind kind)
{
	const unsigned n{static_cast<unsigned>(kind) >= 1 && static_cast<unsigned>(kind) <= tt::STARTER_HORNS ? static_cast<unsigned>(kind) : 1u};
	auto &h{T.horns[n - 1]};
	if (h.empty())
		h = tt::to_mixer_format(tt::starter_horn(n));
	return h;
}

/* What the local player's taunt plays. */
sample_ref own_ref()
{
	const auto choice{static_cast<tt::choice>(CGameCfg.TauntChoice)};
	switch (choice)
	{
		case tt::choice::horn1:
		case tt::choice::horn2:
		case tt::choice::horn3:
		case tt::choice::horn4:
			return {tt::horn_kind(static_cast<unsigned>(choice) - 1), 0, 0};
		case tt::choice::own:
			if (const auto &o{own()}; !o.wire.empty())
				return {tt::sample_kind::custom, o.id, static_cast<std::uint32_t>(o.wire.size())};
			[[fallthrough]];
		case tt::choice::off:
		default:
			return {};
	}
}

/* The samples of `r` as player `pnum` taunts it: a player's own sample
 * is known here only for the local player (Documentation/taunts.md:
 * the others hear Horn 1 until their sample arrives).
 */
const std::vector<std::int16_t> &samples_of(const playernum_t pnum, const sample_ref &r)
{
	if (r.kind == tt::sample_kind::custom)
	{
		if (pnum == Player_num)
			if (const auto &o{own()}; !o.mixer.empty() && o.id == r.id)
				return o.mixer;
		return horn_samples(tt::sample_kind::horn1);
	}
	return horn_samples(r.kind);
}

/* This machine does not play player `pnum`'s taunts (the own ones
 * always sound).
 */
bool muted(const playernum_t pnum)
{
	if (pnum == Player_num)
		return false;
	if (!CGameCfg.TauntsHeard)
		return true;
	const auto &name{vcplayerptr(pnum)->callsign};
	return std::ranges::any_of(T.muted, [&name](const callsign_t &m) { return !d_stricmp(static_cast<const char *>(m), static_cast<const char *>(name)); });
}

/* The ship of player `pnum`, if it is in the level. */
const object *ship_of(const playernum_t pnum)
{
	if (pnum >= MAX_PLAYERS)
		return nullptr;
	const auto objnum{vcplayerptr(pnum)->objnum};
	if (objnum == object_none)
		return nullptr;
	auto &Objects{LevelUniqueObjectState.Objects};
	const auto &&obj{Objects.vcptridx(objnum)};
	if (obj->type != OBJ_PLAYER || get_player_id(*obj) != pnum)
		return nullptr;
	return &*obj;
}

/* Player `pnum` taunts: its horn sounds from its ship.  Returns false if
 * this machine does not play it.
 */
bool play(const playernum_t pnum, const sample_ref &r)
{
	if (pnum >= MAX_PLAYERS || muted(pnum))
		return false;
	const auto ship{ship_of(pnum)};
	if (!ship)
		return false;
	const auto &&[volume, pan]{digi_sound_location(ship->pos, vcsegptridx(ship->segnum), F1_0, TAUNT_DISTANCE)};
	/* A new taunt of this player stops its last one. */
	::dcx::digi_stop_custom(pnum);
	if (volume <= 0)
	{
		con_printf(CON_VERBOSE, "taunt: P#%u's horn out of earshot", pnum);
		return false;
	}
	const auto channel{::dcx::digi_play_custom(pnum, samples_of(pnum, r), volume, pan)};
	con_printf(CON_VERBOSE, "taunt: P#%u's horn (sample %u) at volume %i, channel %u", pnum, static_cast<unsigned>(r.kind), volume, static_cast<unsigned>(channel));
	if (channel == sound_channel::None)
		return false;
	T.voices[pnum] = {ship->signature};
	return true;
}

void record(const playernum_t pnum, const sample_ref &r, const bool played)
{
	movement_record_level_event(mr::level_event_kind::taunt, pnum, static_cast<int>(r.kind), played ? 0 : 1);
}

#if DXX_USE_MULTIPLAYER && DXX_USE_UDP
namespace nv = ::dcx::net_v2;

/* The host: player `pnum`'s taunt to everyone but `exclude`. */
void host_relay(const playernum_t pnum, const sample_ref &r, const playernum_t exclude)
{
	const tt::taunt_msg m{static_cast<std::uint8_t>(pnum), r.kind, r.id, r.size};
	std::array<std::uint8_t, tt::taunt_msg::SIZE> buf;
	m.write(buf);
	::dsx::net_v2::game_broadcast(static_cast<std::uint8_t>(nv::session_msg::taunt), buf, exclude);
}

void send_request(const sample_ref &r)
{
	const tt::taunt_request_msg m{r.kind, r.id, r.size};
	std::array<std::uint8_t, tt::taunt_request_msg::SIZE> buf;
	m.write(buf);
	::dsx::net_v2::game_broadcast(static_cast<std::uint8_t>(nv::session_msg::taunt_request), buf);
}
#endif

/* The player whose callsign starts with `name` (case-insensitive; an
 * exact match first).
 */
std::optional<playernum_t> find_player(const std::string_view name)
{
	if (name.empty())
		return std::nullopt;
	std::optional<playernum_t> prefix;
	for (playernum_t i{0}; i < N_players; ++i)
	{
		const auto &plr{*vcplayerptr(i)};
		if (plr.connected == player_connection_status::disconnected)
			continue;
		const char *const cs{plr.callsign};
		if (std::strlen(cs) == name.size() && !d_strnicmp(cs, name.data(), name.size()))
			return i;
		if (!prefix && !d_strnicmp(cs, name.data(), name.size()))
			prefix = i;
	}
	return prefix;
}

}

void taunt_key_pressed()
{
	if (Newdemo_state == ND_STATE_PLAYBACK)
		return;
	if (static_cast<tt::choice>(CGameCfg.TauntChoice) == tt::choice::off)
	{
		HUD_init_message_literal(HM_DEFAULT, "Your horn is off (Sound options)");
		return;
	}
	const auto now{now_ms()};
	if (T.local.check(now, tt::SENDER_LIMITS) != tt::verdict::allowed)
	{
		const auto left{T.local.lockout_left(now)};
		HUD_init_message(HM_DEFAULT, "Horn cooling down (%u s)", static_cast<unsigned>((left + 999) / 1000));
		return;
	}
	const auto r{own_ref()};
	record(Player_num, r, play(Player_num, r));
#if DXX_USE_MULTIPLAYER && DXX_USE_UDP
	if (+(Game_mode & GM_NETWORK))
	{
		if (multi_i_am_master())
			host_relay(Player_num, r, MAX_PLAYERS);
		else
			send_request(r);
	}
#endif
}

void taunt_frame()
{
	for (playernum_t pnum{0}; pnum < MAX_PLAYERS; ++pnum)
	{
		const auto channel{::dcx::digi_custom_channel(pnum)};
		if (channel == sound_channel::None)
			continue;
		auto &v{T.voices[pnum]};
		const auto ship{ship_of(pnum)};
		/* The ship died or left: its horn stops with it. */
		if (!ship || ship->signature != v.signature || muted(pnum))
		{
			::dcx::digi_stop_custom(pnum);
			continue;
		}
		const auto &&[volume, pan]{digi_sound_location(ship->pos, vcsegptridx(ship->segnum), F1_0, TAUNT_DISTANCE)};
		if (volume <= 0)
		{
			::dcx::digi_stop_custom(pnum);
			continue;
		}
		digi_set_channel_volume(channel, volume);
		digi_set_channel_pan(channel, pan);
	}
}

std::string taunt_own_status(const bool reload)
{
	if (reload)
		T.own.read = false;
	return own().status;
}

void taunt_preview()
{
	if (static_cast<tt::choice>(CGameCfg.TauntChoice) == tt::choice::off)
	{
		::dcx::digi_stop_custom(PREVIEW_SLOT);
		return;
	}
	::dcx::digi_play_custom(PREVIEW_SLOT, samples_of(Player_num, own_ref()), F1_0, sound_pan{F1_0 / 2});
}

void taunt_level_start()
{
	for (auto &r : T.received)
		r.reset();
	T.local.reset();
	(void)own();
	for (unsigned n{1}; n <= tt::STARTER_HORNS; ++n)
		(void)horn_samples(tt::horn_kind(n));
}

void taunt_bot_kill(const playernum_t pnum)
{
#if DXX_USE_MULTIPLAYER && DXX_USE_UDP
	if (!(Game_mode & GM_NETWORK) || !multi_i_am_master() || pnum >= MAX_PLAYERS || !bots_taunt() || !bot_is_local(pnum))
		return;
	/* After about every other kill, within the same limits as a human. */
	if (d_rand() & 1)
		return;
	if (T.received[pnum].check(now_ms(), tt::HOST_LIMITS) != tt::verdict::allowed)
		return;
	const sample_ref r{tt::horn_kind(1u + pnum % tt::STARTER_HORNS), 0, 0};
	record(pnum, r, play(pnum, r));
	host_relay(pnum, r, MAX_PLAYERS);
#else
	(void)pnum;
#endif
}

void net_taunt_receive(const playernum_t from, const std::uint8_t type, const std::span<const std::uint8_t> payload)
{
#if DXX_USE_MULTIPLAYER && DXX_USE_UDP
	if (!(Game_mode & GM_NETWORK))
		return;
	const auto now{now_ms()};
	if (static_cast<nv::session_msg>(type) == nv::session_msg::taunt_request)
	{
		if (!multi_i_am_master() || from >= N_players || from == Player_num)
			return;
		const auto m{tt::taunt_request_msg::read(payload)};
		if (!m)
		{
			con_printf(CON_VERBOSE, "taunt: malformed TAUNT_REQUEST from P#%u", from);
			return;
		}
		if (const auto v{T.received[from].check(now, tt::HOST_LIMITS)}; v != tt::verdict::allowed)
		{
			con_printf(CON_VERBOSE, "taunt: P#%u's taunt refused (%s)", from, v == tt::verdict::locked ? "locked out" : "too many, locked out for 5 s");
			return;
		}
		const sample_ref r{m->kind, m->id, m->size};
		record(from, r, play(from, r));
		host_relay(from, r, from);
	}
	else if (static_cast<nv::session_msg>(type) == nv::session_msg::taunt)
	{
		if (multi_i_am_master())
			return;
		const auto m{tt::taunt_msg::read(payload)};
		if (!m || m->pid >= N_players || m->pid == Player_num)
			return;
		if (T.received[m->pid].check(now, tt::RECEIVER_LIMITS) != tt::verdict::allowed)
		{
			con_printf(CON_VERBOSE, "taunt: P#%u's relayed taunt refused here", m->pid);
			return;
		}
		const sample_ref r{m->kind, m->id, m->size};
		record(m->pid, r, play(m->pid, r));
	}
#else
	(void)from;
	(void)type;
	(void)payload;
#endif
}

bool taunt_chat_command(const char *const text)
{
	const auto c{tt::parse_mute_command(text)};
	if (!c)
		return false;
	if (c->name.empty())
	{
		if (!c->mute)
			HUD_init_message_literal(HM_MULTI, "Usage: /unmute <name>");
		else if (T.muted.empty())
			HUD_init_message_literal(HM_MULTI, "Nobody's horn is muted. Usage: /mute <name>");
		else
		{
			std::string list;
			for (const auto &m : T.muted)
			{
				if (!list.empty())
					list += ", ";
				list += static_cast<const char *>(m);
			}
			HUD_init_message(HM_MULTI, "Muted horns: %s", list.c_str());
		}
		return true;
	}
	const auto pnum{find_player(c->name)};
	if (!pnum || *pnum == Player_num)
	{
		HUD_init_message(HM_MULTI, "No other player called %.*s", static_cast<int>(std::min<std::size_t>(c->name.size(), CALLSIGN_LEN)), c->name.data());
		return true;
	}
	const auto &name{vcplayerptr(*pnum)->callsign};
	const auto same{[&name](const callsign_t &m) { return !d_stricmp(static_cast<const char *>(m), static_cast<const char *>(name)); }};
	std::erase_if(T.muted, same);
	if (c->mute)
	{
		T.muted.push_back(name);
		::dcx::digi_stop_custom(*pnum);
		HUD_init_message(HM_MULTI, "%s's horn is muted", static_cast<const char *>(name));
	}
	else
		HUD_init_message(HM_MULTI, "%s's horn is heard again", static_cast<const char *>(name));
	return true;
}

}
#endif
