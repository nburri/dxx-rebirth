/*
 * This file is part of the DXX-Rebirth project <https://github.com/dxx-rebirth/dxx-rebirth/>.
 * It is copyright by its individual contributors, as recorded in the
 * project's Git history.  See COPYING.txt at the top level for license
 * terms and a link to the Git history.
 */
/*
 * Custom player ships in the game (custom_ship.h).
 */

#include "dxxsconf.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <memory>
#include <random>
#include <string>
#include <vector>

#include "custom_ship.h"
#include "dxship_format.h"
#include "physfsx.h"
#include "physfs_list.h"
#include "console.h"
#include "args.h"
#include "object.h"
#include "player.h"
#include "playsave.h"
#include "gameseg.h"
#include "game.h"
#include "gauges.h"
#include "polyobj.h"
#include "robot.h"
#include "timer.h"
#include "d_levelstate.h"
#include "newdemo.h"
#include "d_enumerate.h"
#if DXX_USE_MULTIPLAYER
#include "multi.h"
#endif
#if DXX_USE_OGL
#include "ogl_init.h"
#include "ogl_ship.h"
#endif

namespace dcx {

namespace custom_ship {

namespace {

std::vector<entry> Entries;
bool Scanned;

std::optional<std::vector<std::uint8_t>> read_whole(const char *const path)
{
	auto file{PHYSFSX_openReadBuffered(path).first};
	if (!file)
		return std::nullopt;
	const auto length{PHYSFS_fileLength(file)};
	if (length <= 0 || length > static_cast<PHYSFS_sint64>(dxship::MAX_FILE_SIZE))
		return std::nullopt;
	std::vector<std::uint8_t> data(static_cast<std::size_t>(length));
	if (PHYSFS_readBytes(file, data.data(), data.size()) != length)
		return std::nullopt;
	return data;
}

/* Check a file and describe it. */
std::optional<entry> examine(const std::string &path, const std::span<const std::uint8_t> bytes, const bool cached, std::string &error)
{
	const auto r{dxship::parse(bytes)};
	if (!r.m)
	{
		error = r.error;
		return std::nullopt;
	}
	entry e;
	e.path = path;
	e.name = r.m->info.name;
	e.title = r.m->info.title.empty() ? r.m->info.name : r.m->info.title;
	e.author = r.m->info.author;
	e.licence = r.m->info.licence;
	e.source = r.m->info.source;
	e.hash = sha256_of(bytes);
	e.size = static_cast<std::uint32_t>(bytes.size());
	e.cached = cached;
	return e;
}

void scan_directory(const char *const dir, const bool cached)
{
	PHYSFSX_uncounted_list files{PHYSFS_enumerateFiles(dir)};
	if (!files)
		return;
	for (const auto name : files)
	{
		const std::string_view n{name};
		if (n.size() < 8 || n.substr(n.size() - 7) != ".dxship")
			continue;
		const std::string path{std::string(dir) + "/" + name};
		const auto bytes{read_whole(path.c_str())};
		if (!bytes)
		{
			con_printf(CON_NORMAL, "ships: %s: cannot read (or larger than 1 MiB)", path.c_str());
			continue;
		}
		std::string error;
		auto e{examine(path, *bytes, cached, error)};
		if (!e)
		{
			con_printf(CON_NORMAL, "ships: %s: invalid: %s", path.c_str(), error.c_str());
			continue;
		}
		/* A cached file is named by its hash; anything else there is
		 * not ours.
		 */
		if (cached && n != sha256_hex(e->hash) + ".dxship")
			continue;
		/* The same ship twice (bundled and cached): the first wins. */
		if (std::ranges::any_of(Entries, [&e](const entry &o) { return o.hash == e->hash; }))
			continue;
		Entries.push_back(std::move(*e));
	}
}

}

void rescan()
{
	Entries.clear();
	scan_directory(SHIPS_DIR, false);
	scan_directory(CACHE_DIR, true);
	std::ranges::sort(Entries, [](const entry &a, const entry &b) {
		/* Own ships first, then the received ones; by title. */
		if (a.cached != b.cached)
			return !a.cached;
		return a.title < b.title;
	});
	Scanned = true;
	con_printf(CON_VERBOSE, "ships: %u custom ships found", static_cast<unsigned>(Entries.size()));
}

const std::vector<entry> &list()
{
	if (!Scanned)
		rescan();
	return Entries;
}

const entry *find_name(const std::string_view name)
{
	for (const auto &e : list())
		if (!e.cached && e.name == name)
			return &e;
	for (const auto &e : list())
		if (e.name == name)
			return &e;
	return nullptr;
}

const entry *find_hash(const sha256_digest &hash)
{
	for (const auto &e : list())
		if (e.hash == hash)
			return &e;
	return nullptr;
}

std::optional<std::vector<std::uint8_t>> read_file(const entry &e)
{
	auto bytes{read_whole(e.path.c_str())};
	if (!bytes || sha256_of(*bytes) != e.hash)
		return std::nullopt;
	return bytes;
}

const entry *store_received(const std::span<const std::uint8_t> bytes, const sha256_digest &expected, std::string &error)
{
	if (bytes.size() > dxship::MAX_FILE_SIZE)
	{
		error = "larger than 1 MiB";
		return nullptr;
	}
	if (sha256_of(bytes) != expected)
	{
		error = "SHA-256 mismatch";
		return nullptr;
	}
	if (const auto e{find_hash(expected)})
		return e;
	const std::string path{std::string(CACHE_DIR) + "/" + sha256_hex(expected) + ".dxship"};
	auto e{examine(path, bytes, true, error)};
	if (!e)
		return nullptr;
	PHYSFS_mkdir(CACHE_DIR);
	{
		auto f{PHYSFSX_openWriteBuffered(path.c_str()).first};
		if (!f || PHYSFS_writeBytes(f, bytes.data(), bytes.size()) != static_cast<PHYSFS_sint64>(bytes.size()))
		{
			error = "cannot write " + path;
			return nullptr;
		}
	}
	Entries.push_back(std::move(*e));
	/* Pointers into Entries stay valid only until the next change. */
	return &Entries.back();
}

}

}

namespace dsx {

std::optional<unsigned> custom_ship_debug_colour;

namespace {

namespace cs = ::dcx::custom_ship;

#if DXX_USE_OGL
namespace sg = ::dcx::ship_gl;

/* Loaded ships by hash, kept while the game runs (a handful of MiB). */
struct loaded_ship
{
	std::unique_ptr<sg::mesh> mesh;
	bool failed{};
};

std::map<sha256_digest, loaded_ship> Loaded;

sg::mesh *load(const sha256_digest &hash)
{
	auto &l{Loaded[hash]};
	if (l.mesh)
		return l.mesh.get();
	if (l.failed)
		return nullptr;
	const auto e{cs::find_hash(hash)};
	if (!e)
		/* Not here (yet): the Pyro, and look again later. */
		return Loaded.erase(hash), nullptr;
	const auto bytes{cs::read_file(*e)};
	if (!bytes)
	{
		l.failed = true;
		con_printf(CON_URGENT, "ships: %s: cannot read", e->path.c_str());
		return nullptr;
	}
	const auto r{::dcx::dxship::parse(*bytes)};
	std::string error{r.error};
	if (r.m)
		l.mesh = sg::mesh_create(*r.m, error);
	if (!l.mesh)
	{
		l.failed = true;
		con_printf(CON_URGENT, "ships: %s: %s", e->path.c_str(), error.c_str());
		return nullptr;
	}
	con_printf(CON_VERBOSE, "ships: loaded %s (\"%s\" by %s, %s)", e->name.c_str(), e->title.c_str(), e->author.c_str(), e->licence.c_str());
	return l.mesh.get();
}
#endif

per_player_array<std::optional<sha256_digest>> Player_ships;

/* The colour a player is drawn in (the HUD's, the team's in team games). */
unsigned player_colour(const playernum_t pnum)
{
	if (custom_ship_debug_colour && pnum == Player_num)
		return *custom_ship_debug_colour;
#if DXX_USE_MULTIPLAYER
	if (+(Game_mode & GM_MULTI))
		return static_cast<unsigned>(get_player_or_team_color(Netgame, Game_mode, pnum));
#endif
	return static_cast<unsigned>(pnum);
}

std::array<float, 3> tint_of(const unsigned colour)
{
	const auto &c{player_rgb_normal[static_cast<std::size_t>(colour % MAX_PLAYERS)]};
#if DXX_USE_OGL
	return sg::tint_from_rgb5(c.r, c.g, c.b);
#else
	return {{c.r / 31.0f, c.g / 31.0f, c.b / 31.0f}};
#endif
}

/* Debris: the Pyro's debris objects of a custom ship's explosion (not
 * drawn), and the ship's own pieces (drawn, cosmetic only: no object,
 * no collision).
 */
std::map<objnum_t, object_signature_t> Hidden_debris;

#if DXX_USE_OGL
struct piece
{
	sg::mesh *mesh;
	unsigned part;
	unsigned colour;
	std::array<float, 3> pos, vel;
	/* Orientation as rows (rvec, uvec, fvec) and the spin. */
	std::array<std::array<float, 3>, 3> orient;
	std::array<float, 3> spin_axis;
	float spin_rate;
	float age, life;
	std::array<float, 3> light;
	segnum_t segnum;
};

std::vector<piece> Pieces;
fix64 Pieces_time;
std::minstd_rand Piece_random{12345};

float frand(const float lo, const float hi)
{
	return std::uniform_real_distribution<float>(lo, hi)(Piece_random);
}

void rotate_rows(std::array<std::array<float, 3>, 3> &rows, const std::array<float, 3> &axis, const float angle)
{
	/* Rodrigues' rotation of each row about the axis (world space). */
	const float c{std::cos(angle)}, s{std::sin(angle)};
	for (auto &v : rows)
	{
		const float d{axis[0] * v[0] + axis[1] * v[1] + axis[2] * v[2]};
		const std::array<float, 3> x{{axis[1] * v[2] - axis[2] * v[1], axis[2] * v[0] - axis[0] * v[2], axis[0] * v[1] - axis[1] * v[0]}};
		for (unsigned k = 0; k < 3; ++k)
			v[k] = v[k] * c + x[k] * s + axis[k] * d * (1 - c);
	}
	/* Keep the rows orthonormal. */
	auto &r{rows[0]}, &u{rows[1]}, &f{rows[2]};
	const auto norm = [](std::array<float, 3> &v) {
		const float l{std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2])};
		if (l > 1e-6f)
			for (auto &k : v)
				k /= l;
	};
	norm(f);
	r = {{u[1] * f[2] - u[2] * f[1], u[2] * f[0] - u[0] * f[2], u[0] * f[1] - u[1] * f[0]}};
	norm(r);
	u = {{f[1] * r[2] - f[2] * r[1], f[2] * r[0] - f[0] * r[2], f[0] * r[1] - f[1] * r[0]}};
}

vms_vector to_vms(const std::array<float, 3> &v)
{
	return {fl2f(v[0]), fl2f(v[1]), fl2f(v[2])};
}

void update_pieces()
{
	const auto now{GameTime64};
	if (now < Pieces_time || now - Pieces_time > F1_0)
		Pieces_time = now;
	const float dt{f2fl(static_cast<fix>(now - Pieces_time))};
	Pieces_time = now;
	if (dt <= 0)
		return;
	for (auto &p : Pieces)
	{
		p.age += dt;
		const float drag{std::exp(-0.7f * dt)};
		for (unsigned k = 0; k < 3; ++k)
		{
			p.vel[k] *= drag;
			p.pos[k] += p.vel[k] * dt;
		}
		rotate_rows(p.orient, p.spin_axis, p.spin_rate * dt);
		/* A piece that leaves the mine is gone. */
		const auto pos{to_vms(p.pos)};
		const auto seg{find_point_seg(LevelSharedSegmentState, LevelUniqueSegmentState, pos, Segments.vmptridx(p.segnum) DXX_lighting_hack_pass_parameter)};
		if (seg == segment_none)
			p.age = p.life;
		else
			p.segnum = seg;
	}
	std::erase_if(Pieces, [](const piece &p) { return p.age >= p.life; });
}
#endif

}

void custom_ship_set_player(const playernum_t pnum, const sha256_digest *const hash)
{
	if (pnum >= Player_ships.size())
		return;
	const std::optional<sha256_digest> next{hash ? std::optional<sha256_digest>{*hash} : std::nullopt};
	if (Player_ships[pnum] == next)
		return;
	Player_ships[pnum] = next;
	/* A demo being recorded keeps the ships in its side file. */
	newdemo_record_ships();
}

std::optional<sha256_digest> custom_ship_of_player(const playernum_t pnum)
{
	if (pnum >= Player_ships.size())
		return std::nullopt;
	return Player_ships[pnum];
}

void custom_ship_clear_players()
{
	for (auto &s : Player_ships)
		s.reset();
}

void custom_ship_apply_local_choice()
{
	const std::string_view name{PlayerCfg.ShipName.data()};
	const auto e{name.empty() ? nullptr : cs::find_name(name)};
	if (!name.empty() && !e)
		con_printf(CON_NORMAL, "ships: your ship \"%s\" is not in the ships folder; flying the Pyro-GX", PlayerCfg.ShipName.data());
	custom_ship_set_player(Player_num, e ? &e->hash : nullptr);
	/* -shipfor pid:name,...: ships for other players or bots, locally (a
	 * test aid; the network announces the real choices).
	 */
	const std::string_view spec{CGameArg.DbgShipFor};
	std::size_t start{};
	while (start < spec.size())
	{
		const auto comma{spec.find(',', start)};
		const auto item{spec.substr(start, comma == spec.npos ? spec.npos : comma - start)};
		start = comma == spec.npos ? spec.size() : comma + 1;
		const auto colon{item.find(':')};
		if (colon == item.npos)
			continue;
		const auto pid{static_cast<playernum_t>(std::strtoul(std::string(item.substr(0, colon)).c_str(), nullptr, 10))};
		const auto ship{item.substr(colon + 1)};
		if (ship == "pyro")
			custom_ship_set_player(pid, nullptr);
		else if (const auto s{cs::find_name(ship)})
			custom_ship_set_player(pid, &s->hash);
		else
			con_printf(CON_NORMAL, "ships: -shipfor: no ship \"%.*s\"", static_cast<int>(ship.size()), ship.data());
	}
	custom_ship_preload();
}

void custom_ship_preload()
{
#if DXX_USE_OGL
	/* Decode now, not at the first sight in the middle of a fight. */
	for (const auto &s : Player_ships)
		if (s)
			load(*s);
#endif
}

void custom_ship_level_start()
{
	Hidden_debris.clear();
#if DXX_USE_OGL
	Pieces.clear();
	Pieces_time = GameTime64;
#endif
}

bool custom_ship_draw_player(grs_canvas &, const object_base &obj, const g3s_lrgb &light, const float alpha, const bool flat_black)
{
#if DXX_USE_OGL
	if (obj.type != object_type::OBJ_PLAYER)
		return false;
	const auto pnum{get_player_id(obj)};
	const auto &hash{custom_ship_of_player(pnum)};
	if (!hash)
		return false;
	const auto m{load(*hash)};
	if (!m)
		return false;
	sg::draw_params p;
	p.light = {{f2fl(light.r), f2fl(light.g), f2fl(light.b)}};
	const auto colour{player_colour(pnum)};
	p.tint = tint_of(colour);
	p.tint_key = colour;
	p.alpha = alpha;
	p.flat_black = flat_black;
	/* After the Pyro's explosion only its centre is drawn (subobj_flags
	 * 1): the ship's body, its debris parts fly on their own.
	 */
	if (obj.rtype.pobj_info.subobj_flags)
		p.part_mask = 1;
	auto &&ctx{g3_start_instance_matrix(obj.pos, obj.orient)};
	sg::draw(*m, p);
	g3_done_instance(ctx);
	return true;
#else
	(void)obj, (void)light, (void)alpha, (void)flat_black;
	return false;
#endif
}

bool custom_ship_hides_debris(const objnum_t objnum, const object_signature_t signature)
{
	if (Hidden_debris.empty())
		return false;
	const auto f{Hidden_debris.find(objnum)};
	if (f == Hidden_debris.end())
		return false;
	if (f->second == signature)
		return true;
	Hidden_debris.erase(f);
	return false;
}

void custom_ship_debris_created(const object_base &obj, const objnum_t debris, const object_signature_t signature)
{
#if DXX_USE_OGL
	if (obj.type != object_type::OBJ_PLAYER)
		return;
	/* Only when the ship's own pieces fly instead. */
	const auto &hash{custom_ship_of_player(get_player_id(obj))};
	if (!hash)
		return;
	const auto m{load(*hash)};
	if (!m || m->parts.empty())
		return;
	if (Hidden_debris.size() > 256)
		Hidden_debris.clear();
	Hidden_debris[debris] = signature;
#else
	(void)obj, (void)debris, (void)signature;
#endif
}

void custom_ship_player_exploded(const object_base &obj)
{
#if DXX_USE_OGL
	if (obj.type != object_type::OBJ_PLAYER)
		return;
	const auto pnum{get_player_id(obj)};
	const auto &hash{custom_ship_of_player(pnum)};
	if (!hash)
		return;
	const auto m{load(*hash)};
	if (!m || m->parts.empty())
		return;
	if (Pieces.empty())
		Pieces_time = GameTime64;
	const std::array<std::array<float, 3>, 3> orient{{
		{{f2fl(obj.orient.rvec.x), f2fl(obj.orient.rvec.y), f2fl(obj.orient.rvec.z)}},
		{{f2fl(obj.orient.uvec.x), f2fl(obj.orient.uvec.y), f2fl(obj.orient.uvec.z)}},
		{{f2fl(obj.orient.fvec.x), f2fl(obj.orient.fvec.y), f2fl(obj.orient.fvec.z)}},
	}};
	const std::array<float, 3> base{{f2fl(obj.pos.x), f2fl(obj.pos.y), f2fl(obj.pos.z)}};
	const std::array<float, 3> velocity{{f2fl(obj.mtype.phys_info.velocity.x), f2fl(obj.mtype.phys_info.velocity.y), f2fl(obj.mtype.phys_info.velocity.z)}};
	/* Every part flies, the body too, slower. */
	for (unsigned part = 0; part < m->parts.size() && Pieces.size() < 64; ++part)
	{
		const auto &pc{m->parts[part].centre};
		piece p{};
		p.mesh = m;
		p.part = part;
		p.colour = player_colour(pnum);
		/* Where the part's centre is in the world. */
		std::array<float, 3> out{};
		for (unsigned k = 0; k < 3; ++k)
		{
			p.pos[k] = base[k] + orient[0][k] * pc.x + orient[1][k] * pc.y + orient[2][k] * pc.z;
			out[k] = p.pos[k] - base[k];
		}
		float l{std::sqrt(out[0] * out[0] + out[1] * out[1] + out[2] * out[2])};
		if (l < 0.5f)
		{
			out = {{frand(-1, 1), frand(-1, 1), frand(-1, 1)}};
			l = std::max(1e-3f, std::sqrt(out[0] * out[0] + out[1] * out[1] + out[2] * out[2]));
		}
		const float speed{part ? frand(14, 26) : frand(3, 6)};
		for (unsigned k = 0; k < 3; ++k)
			p.vel[k] = velocity[k] + out[k] / l * speed + frand(-4, 4);
		p.orient = orient;
		std::array<float, 3> axis{{frand(-1, 1), frand(-1, 1), frand(-1, 1)}};
		const float al{std::max(1e-3f, std::sqrt(axis[0] * axis[0] + axis[1] * axis[1] + axis[2] * axis[2]))};
		for (auto &k : axis)
			k /= al;
		p.spin_axis = axis;
		p.spin_rate = part ? frand(2, 7) : frand(0.5f, 1.5f);
		p.life = part ? frand(2.0f, 3.0f) : frand(1.2f, 1.8f);
		p.light = {{1.6f, 1.3f, 1.0f}};
		p.segnum = obj.segnum;
		Pieces.push_back(p);
	}
#else
	(void)obj;
#endif
}

void custom_ship_draw_pieces(grs_canvas &)
{
#if DXX_USE_OGL
	if (Pieces.empty())
		return;
	update_pieces();
	for (const auto &p : Pieces)
	{
		/* Hot at first, then charred. */
		const float t{std::clamp(p.age / 0.8f, 0.0f, 1.0f)};
		sg::draw_params dp;
		for (unsigned k = 0; k < 3; ++k)
			dp.light[k] = p.light[k] * (1 - t) + 0.45f * t;
		dp.tint = tint_of(p.colour);
		dp.tint_key = p.colour;
		dp.part_mask = 1u << p.part;
		const auto &c{p.mesh->parts[p.part].centre};
		dp.offset = {{c.x, c.y, c.z}};
		vms_matrix orient;
		orient.rvec = to_vms(p.orient[0]);
		orient.uvec = to_vms(p.orient[1]);
		orient.fvec = to_vms(p.orient[2]);
		auto &&ctx{g3_start_instance_matrix(to_vms(p.pos), orient)};
		sg::draw(*p.mesh, dp);
		g3_done_instance(ctx);
	}
#endif
}

void custom_ship_draw_preview(grs_canvas &canvas, const cs::entry *const e, const vms_angvec &angles, const unsigned colour)
{
	auto &Polygon_models{LevelSharedPolygonModelState.Polygon_models};
	const auto &pyro{Polygon_models[Player_ship->model_num.dsx]};
	g3_start_frame(canvas);
	g3_set_view_matrix(vms_vector{}, vmd_identity_matrix, 0x9000);
	vms_vector pos{};
	const fix rad{pyro.rad ? pyro.rad : fl2f(::dcx::dxship::PYRO_RADIUS)};
	/* As draw_model_picture (polyobj.cpp) places a model. */
	pos.z = fixmuldiv(0x60000, rad, 0x28000);
	const auto orient{vm_angles_2_matrix(angles)};
#if DXX_USE_OGL
	if (e)
		if (const auto m{load(e->hash)})
		{
			sg::draw_params p;
			p.tint = tint_of(colour);
			p.tint_key = colour;
			auto &&ctx{g3_start_instance_matrix(pos, orient)};
			sg::draw(*m, p);
			g3_done_instance(ctx);
			g3_end_frame();
			return;
		}
#endif
	draw_polygon_model(canvas, draw_tmap, pos, orient, nullptr, pyro, 0, g3s_lrgb{F1_0, F1_0, F1_0}, nullptr, alternate_textures{});
	g3_end_frame();
}

}
