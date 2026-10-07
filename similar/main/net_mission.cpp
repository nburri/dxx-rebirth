/*
 * This file is part of the DXX-Rebirth project <https://github.com/dxx-rebirth/dxx-rebirth/>.
 * It is copyright by its individual contributors, as recorded in the
 * project's Git history.  See COPYING.txt at the top level for license
 * terms and a link to the Git history.
 */
/*
 * Mission transfer (Documentation/network-protocol-v2.md section 8,
 * "Mission transfer"): the game's side of net_v2_mission.h.  The host
 * describes its mission (the .mn2 and .hog, their SHA-256s) in
 * GAME_SETTINGS and sends the files to a client that lacks that version;
 * the client keeps them in missions/downloaded/<hash>/, where the mission
 * list finds them, and plays the mission whose files have the host's
 * bundle hash.  Options: "Accept missions from the host" and "Send
 * missions to players" (Options -> Gameplay).
 */

#include "dxxsconf.h"

#if DXX_USE_MULTIPLAYER

#include <algorithm>
#include <cinttypes>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "net_v2_mission.h"
#include "net_v2_transport.h"
#include "net_v2_game.h"
#include "mission.h"
#include "multi.h"
#include "config.h"
#include "console.h"
#include "physfsx.h"
#include "physfs_list.h"
#include "strutil.h"
#include "timer.h"
#include "game.h"
#include "net_mission.h"

namespace dsx {

namespace {

namespace nv = ::dcx::net_v2;

std::string hash_text(const nv::mission_hash &h)
{
	return ::dcx::sha256_hex(h).substr(0, 12);
}

std::optional<std::vector<std::uint8_t>> read_file(const char *const path, const std::uint32_t cap)
{
	auto file{PHYSFSX_openReadBuffered(path).first};
	if (!file)
		return std::nullopt;
	const auto length{PHYSFS_fileLength(file)};
	if (length <= 0 || length > static_cast<PHYSFS_sint64>(cap))
		return std::nullopt;
	std::vector<std::uint8_t> data(static_cast<std::size_t>(length));
	if (PHYSFS_readBytes(file, data.data(), data.size()) != length)
		return std::nullopt;
	return data;
}

/* A mission's bundle on this machine: its files, found next to the .mn2
 * (case ignored), and where they are.
 */
struct local_bundle
{
	nv::mission_manifest manifest;
	nv::mission_hash bundle{};
	/* The PhysFS path of each file of the manifest, by name. */
	std::map<std::string, std::string> paths;
};

/* What a file was when it was last hashed. */
struct file_stamp
{
	PHYSFS_sint64 size{-1}, modtime{-1};
	constexpr bool operator==(const file_stamp &) const = default;
};

file_stamp stamp_of(const char *const path)
{
	PHYSFS_Stat st{};
	if (!PHYSFS_stat(path, &st))
		return {};
	return {st.filesize, st.modtime};
}

/* Hashing a 30 MB HOG takes a moment: each file's hash is kept for as
 * long as its size and time do not change.
 */
std::map<std::string, std::pair<file_stamp, nv::mission_hash>> Hash_cache;

std::optional<nv::mission_file> describe_file(const std::string &path, const std::string &name, const std::uint32_t cap)
{
	const auto st{stamp_of(path.c_str())};
	if (st.size <= 0 || st.size > static_cast<PHYSFS_sint64>(cap))
		return std::nullopt;
	if (const auto i{Hash_cache.find(path)}; i != Hash_cache.end() && i->second.first == st)
		return nv::mission_file{name, static_cast<std::uint32_t>(st.size), i->second.second};
	const auto bytes{read_file(path.c_str(), cap)};
	if (!bytes)
		return std::nullopt;
	const auto h{::dcx::sha256_of(*bytes)};
	Hash_cache[path] = {st, h};
	return nv::mission_file{name, static_cast<std::uint32_t>(bytes->size()), h};
}

/* The bundle of the mission at `path` (no extension), or nothing if it
 * cannot be one (a name or size out of bounds, files missing).
 */
std::optional<local_bundle> bundle_of(const std::string &path, const char *const title)
{
	const auto slash{path.rfind('/')};
	const std::string dir{slash == std::string::npos ? std::string{} : path.substr(0, slash)};
	const std::string base{slash == std::string::npos ? path : path.substr(slash + 1)};
	if (!nv::mission_basename_valid(base))
		return std::nullopt;
	local_bundle b;
	b.manifest.basename = base;
	b.manifest.title = std::string{title}.substr(0, nv::MISSION_TITLE_FIELD - 1);
	PHYSFSX_uncounted_list files{PHYSFS_enumerateFiles(dir.empty() ? "." : dir.c_str())};
	if (!files)
		return std::nullopt;
	const auto lower_base{nv::mission_lower(base)};
	for (const auto name : files)
	{
		const std::string n{name};
		const auto t{nv::mission_file_type_of(n)};
		if (!t || nv::mission_lower(n.substr(0, n.find('.'))) != lower_base)
			continue;
		const std::string p{dir.empty() ? n : dir + "/" + n};
		const auto f{describe_file(p, n, *t == nv::mission_file_type::mn2 ? nv::MISSION_MN2_MAX : nv::MISSION_FILE_MAX)};
		if (!f)
			return std::nullopt;
		/* Two files of one type (Corona.hog and CORONA.HOG on a case
		 * sensitive file system): not a bundle anyone can rebuild.
		 */
		if (std::ranges::any_of(b.manifest.files, [&](const nv::mission_file &o) { return o.type() == *t; }))
			return std::nullopt;
		b.manifest.files.push_back(*f);
		b.paths[n] = p;
	}
	b.manifest.normalise();
	if (!b.manifest.valid())
		return std::nullopt;
	b.bundle = b.manifest.bundle_hash();
	return b;
}

struct game_mission_env final : nv::mission_env
{
	/* Host: the paths of the current bundle's files. */
	std::map<std::string, std::string> host_paths;
	/* Host: files read for sending, while transfers need them. */
	std::map<nv::mission_hash, std::shared_ptr<const std::vector<std::uint8_t>>> files;
	void send(const std::uint8_t slot, const std::uint8_t type, const std::span<const std::uint8_t> payload) override
	{
		net_v2::game_send_to(slot, type, payload);
	}
	std::size_t queued_bytes(const std::uint8_t slot) override
	{
		return net_v2::game_queued_bytes(slot);
	}
	link_counters link(const std::uint8_t slot) override
	{
		const auto l{net_v2::game_link(slot)};
		return {l.sends, l.resends, l.rtt};
	}
	void set_bulk(const std::uint8_t slot, const unsigned packets) override
	{
		net_v2::game_set_bulk(slot, packets);
	}
	std::shared_ptr<const std::vector<std::uint8_t>> host_file(const nv::mission_manifest &, const nv::mission_file &f) override
	{
		if (const auto i{files.find(f.hash)}; i != files.end())
			return i->second;
		const auto p{host_paths.find(f.name)};
		if (p == host_paths.end())
			return nullptr;
		auto bytes{read_file(p->second.c_str(), nv::MISSION_FILE_MAX)};
		if (!bytes || bytes->size() != f.size || ::dcx::sha256_of(*bytes) != f.hash)
		{
			con_printf(CON_URGENT, "mission: %s changed since the game started; it cannot be sent", p->second.c_str());
			return nullptr;
		}
		auto s{std::make_shared<const std::vector<std::uint8_t>>(std::move(*bytes))};
		files.emplace(f.hash, s);
		return s;
	}
	bool client_has_file(const nv::mission_manifest &m, const nv::mission_file &f) override
	{
		const auto path{nv::mission_download_dir(m.bundle_hash()) + "/" + f.name};
		const auto d{describe_file(path, f.name, nv::MISSION_FILE_MAX)};
		return d && d->size == f.size && d->hash == f.hash;
	}
	bool client_store(const nv::mission_manifest &m, const nv::mission_file &f, const std::span<const std::uint8_t> bytes) override
	{
		/* Names and sizes were checked with the manifest, the contents
		 * against their SHA-256 and the type's rules.
		 */
		const auto dir{nv::mission_download_dir(m.bundle_hash())};
		const auto path{dir + "/" + f.name};
		PHYSFS_mkdir(dir.c_str());
		auto file{PHYSFSX_openWriteBuffered(path.c_str()).first};
		if (!file || PHYSFS_writeBytes(file, bytes.data(), bytes.size()) != static_cast<PHYSFS_sint64>(bytes.size()))
		{
			con_printf(CON_URGENT, "mission: cannot write %s", path.c_str());
			return false;
		}
		return true;
	}
	void note(const std::string_view what) override
	{
		con_printf(CON_NORMAL, "mission: %.*s", static_cast<int>(what.size()), what.data());
	}
};

game_mission_env Env;
std::optional<nv::mission_host> Host;
nv::mission_client Client{Env};
fix64 Last_pump;
/* Client: what the host announced, what the join decided. */
nv::mission_announcement Announced;
bool Need_download;
std::string Download_title;

bool accepts()
{
	return CGameCfg.AcceptMissions;
}

}

void net_mission_host_start()
{
	Host.emplace(Env);
	Env.host_paths.clear();
	Env.files.clear();
	Host->enabled = CGameCfg.SendMissions;
	Last_pump = timer_query();
	if (!Current_mission || PLAYING_BUILTIN_MISSION)
		return;
	/* Descent 2: Vertigo is a commercial add-on, never sent. */
	if (!d_stricmp(&*Current_mission->filename, "d2x"))
	{
		con_puts(CON_NORMAL, "mission: Vertigo (d2x) is not sent to players");
		return;
	}
	const auto start{timer_query()};
	auto b{bundle_of(std::string{Current_mission->path.c_str()}, Current_mission->mission_name.data())};
	if (!b)
	{
		con_printf(CON_NORMAL, "mission: %s cannot be sent (a name, a size or a file out of bounds)", Current_mission->path.c_str());
		return;
	}
	Env.host_paths = b->paths;
	con_printf(CON_NORMAL, "mission: '%s' is bundle %s, %u files, %" PRIu64 " bytes (hashed in %u ms)%s", b->manifest.title.c_str(), hash_text(b->bundle).c_str(), static_cast<unsigned>(b->manifest.files.size()), b->manifest.total_size(), static_cast<unsigned>((timer_query() - start) * 1000 / F1_0), CGameCfg.SendMissions ? "" : "; not sent (option off)");
	Host->set_manifest(std::move(b->manifest));
}

void net_mission_reset()
{
	Host.reset();
	Env.files.clear();
	Env.host_paths.clear();
	Client.stop();
}

void net_mission_write_announcement(std::uint8_t *const p)
{
	nv::mission_announcement a;
	if (Host && Host->current())
	{
		a.bundle = Host->current()->bundle_hash();
		a.size = static_cast<std::uint32_t>(Host->current()->total_size());
		a.sends = Host->enabled;
	}
	a.write(p);
}

void net_mission_read_announcement(const std::uint8_t *const p)
{
	Announced = nv::mission_announcement::read(p);
}

void net_mission_receive(const playernum_t from, const uint8_t type, const std::span<const uint8_t> payload)
{
	if (multi_i_am_master())
	{
		if (Host)
			Host->receive(static_cast<uint8_t>(from), type, payload);
	}
	else
		Client.receive(static_cast<uint8_t>(from), type, payload);
}

void net_mission_client_joined(const playernum_t slot)
{
	if (Host)
		Host->client_joined(static_cast<uint8_t>(slot));
}

void net_mission_slot_cleared(const playernum_t slot)
{
	if (Host)
		Host->slot_cleared(static_cast<uint8_t>(slot));
}

bool net_mission_host_busy(const playernum_t slot)
{
	return Host && Host->busy(static_cast<uint8_t>(slot));
}

std::optional<unsigned> net_mission_host_progress(const playernum_t slot)
{
	if (!Host)
		return std::nullopt;
	return Host->progress(static_cast<uint8_t>(slot));
}

void net_mission_frame()
{
	const auto now{timer_query()};
	const double seconds{f2fl(static_cast<fix>(std::clamp<fix64>(now - Last_pump, 0, F1_0)))};
	Last_pump = now;
	if (multi_i_am_master())
	{
		if (!Host)
			return;
		Host->enabled = CGameCfg.SendMissions;
		Host->pump(seconds, Network_status == network_state::playing);
		/* Files no transfer needs any more: not kept in memory. */
		bool any{};
		for (playernum_t s{1}; s < MAX_PLAYERS; ++s)
			any |= Host->busy(s);
		if (!any)
			Env.files.clear();
	}
	else
		Client.tick(seconds);
}

join_mission net_mission_prepare_join(const char *const basename, std::string &message)
{
	Need_download = false;
	Client.stop();
	std::vector<nv::mission_candidate> candidates;
	const auto paths{mission_paths_by_basename(basename)};
	std::vector<std::string> usable;
	if (Announced.known())
		for (const auto &p : paths)
			if (const auto b{bundle_of(p, "")})
			{
				candidates.push_back({usable.size(), b->bundle});
				usable.push_back(p);
			}
	const auto c{nv::choose_mission(Announced, candidates, accepts())};
	switch (c.decision)
	{
		case nv::mission_decision::use_by_name:
			return join_mission::by_name;
		case nv::mission_decision::use_local:
			if (c.other_version)
				con_printf(CON_NORMAL, "mission: you have another version of '%s' (the host's is %s) and it cannot be downloaded; trying yours", basename, hash_text(Announced.bundle).c_str());
			else
				con_printf(CON_NORMAL, "mission: '%s' found at %s (bundle %s)", basename, usable[c.index].c_str(), hash_text(Announced.bundle).c_str());
			if (const auto err{load_mission_by_path(usable[c.index].c_str())})
			{
				message = err;
				return join_mission::failed;
			}
			return join_mission::loaded;
		case nv::mission_decision::download:
			Need_download = true;
			Download_title = Netgame.mission_title.data();
			con_printf(CON_NORMAL, "mission: '%s' (bundle %s, %u bytes) %s; it will be downloaded from the host", basename, hash_text(Announced.bundle).c_str(), Announced.size, c.other_version ? "is another version here" : "is not here");
			return join_mission::download;
		case nv::mission_decision::missing_host_does_not_send:
			message = "You do not have this mission,\nand the host does not send missions\n(its option \"Send missions to players\").";
			return join_mission::failed;
		case nv::mission_decision::missing_player_refuses:
			message = "You do not have this mission.\nTurn on \"Accept missions from the host\"\n(Options, Gameplay) to get it from the host.";
			return join_mission::failed;
		case nv::mission_decision::missing_too_large:
			message = "You do not have this mission,\nand it is too large to send.";
			return join_mission::failed;
	}
	return join_mission::failed;
}

void net_mission_client_connected()
{
	if (Need_download)
		Client.begin(Announced.bundle);
}

bool net_mission_download_needed()
{
	return Need_download;
}

client_mission_status net_mission_client_status()
{
	client_mission_status s;
	s.title = Download_title;
	s.percent = Client.percent();
	s.received = Client.received();
	s.total = Client.total() ? Client.total() : Announced.size;
	switch (Client.state())
	{
		case nv::mission_client_state::done:
			s.state = client_mission_status::done;
			break;
		case nv::mission_client_state::failed:
			s.state = client_mission_status::failed;
			s.error = Client.error();
			break;
		case nv::mission_client_state::idle:
			s.state = client_mission_status::failed;
			s.error = "the download stopped";
			break;
		case nv::mission_client_state::waiting_manifest:
		case nv::mission_client_state::downloading:
			s.state = client_mission_status::running;
			break;
	}
	return s;
}

const char *net_mission_client_finish()
{
	Need_download = false;
	const auto &m{Client.current()};
	if (!m)
		return "No mission was received.";
	const auto path{nv::mission_download_dir(m->bundle_hash()) + "/" + m->basename};
	Client.stop();
	con_printf(CON_NORMAL, "mission: '%s' downloaded to %s", m->title.c_str(), path.c_str());
	return load_mission_by_path(path.c_str());
}

void net_mission_client_cancel()
{
	Need_download = false;
	Client.stop();
}

}

#endif
