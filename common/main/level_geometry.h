/*
 * This file is part of the DXX-Rebirth project <https://github.com/dxx-rebirth/dxx-rebirth/>.
 * It is copyright by its individual contributors, as recorded in the
 * project's Git history.  See COPYING.txt at the top level for license
 * terms and a link to the Git history.
 */
/*
 * The geometry of a level for the analysis of the movement recordings
 * (Documentation/movement-recording.md section 8.8): a reader of the
 * mission files (.hog, .mn2) and of a level's segments and vertices
 * (.rl2, and .rdl of Descent 1, as gamemine.cpp reads them), and how much
 * room there is around a point: the free distance along a ray through
 * the segments, the room of each segment, the level's character.
 *
 * Standalone (standard C++ only), header-only.  The files are untrusted
 * input: every count, offset and index is checked, and a file that does
 * not make sense gives nothing.  Walls (doors, grates) are not read: a
 * side with a neighbour segment is open.
 */

#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace dcx::movrec::geometry {

using vec3 = std::array<double, 3>;

/* Limits for untrusted files (the game's: 9000 segments). */
constexpr std::size_t MAX_SEGMENTS{20000};
/* A ray crosses at most this many segments (real levels: tens; the cap
 * bounds the work a hostile file can cause).
 */
constexpr std::size_t MAX_RAY_STEPS{512};
constexpr std::size_t MAX_VERTICES{65536};
constexpr std::size_t MAX_HOG_ENTRIES{4096};
constexpr std::uint16_t SEGMENT_NONE{0xffff};
constexpr std::uint16_t SEGMENT_EXIT{0xfffe};
/* The rays stop here (units). */
constexpr double MAX_RAY{4000};
/* A unit in the files: 16.16 fixed point. */
constexpr double FIX_UNIT{65536.0};

/* The vertices of each side (segment.h: Side_to_verts): left, top,
 * right, bottom, back, front.
 */
inline constexpr std::array<std::array<std::uint8_t, 4>, 6> side_verts{{
	{{7, 6, 2, 3}},
	{{0, 4, 7, 3}},
	{{0, 1, 5, 4}},
	{{2, 6, 5, 1}},
	{{4, 5, 6, 7}},
	{{3, 2, 1, 0}},
}};

[[nodiscard]]
inline std::string lower(const std::string_view s)
{
	std::string r{s};
	for (auto &c : r)
		if (c >= 'A' && c <= 'Z')
			c = static_cast<char>(c - 'A' + 'a');
	return r;
}

[[nodiscard]]
inline std::string_view trim(std::string_view s)
{
	while (!s.empty() && static_cast<unsigned char>(s.front()) <= ' ')
		s.remove_prefix(1);
	while (!s.empty() && static_cast<unsigned char>(s.back()) <= ' ')
		s.remove_suffix(1);
	return s;
}

/*
 * Little-endian reads that fail instead of running past the end.
 */
class reader
{
	std::span<const std::uint8_t> in;
	std::size_t pos{};
	bool failed{};
public:
	explicit reader(const std::span<const std::uint8_t> i, const std::size_t at = 0) :
		in{i}, pos{at}
	{
		if (at > i.size())
		{
			failed = true;
			pos = i.size();
		}
	}
	std::uint8_t u8()
	{
		if (pos >= in.size())
		{
			failed = true;
			return 0;
		}
		return in[pos++];
	}
	std::uint16_t u16()
	{
		const std::uint16_t lo{u8()};
		const std::uint16_t hi{u8()};
		return static_cast<std::uint16_t>(lo | (hi << 8));
	}
	std::uint32_t u32()
	{
		const std::uint32_t lo{u16()};
		const std::uint32_t hi{u16()};
		return lo | (hi << 16);
	}
	std::int32_t i32()
	{
		return static_cast<std::int32_t>(u32());
	}
	void skip(const std::size_t n)
	{
		if (n > in.size() - pos)
		{
			failed = true;
			pos = in.size();
			return;
		}
		pos += n;
	}
	[[nodiscard]]
	bool ok() const
	{
		return !failed;
	}
	[[nodiscard]]
	std::size_t position() const
	{
		return pos;
	}
};

/*
 * HOG: "DHF", then entries of a name (13 bytes, NUL padded), a size
 * (u32) and the data.
 */
struct hog_entry
{
	std::string name;
	std::size_t offset{}, size{};
};

[[nodiscard]]
inline std::optional<std::vector<hog_entry>> read_hog(const std::span<const std::uint8_t> bytes)
{
	if (bytes.size() < 3 || bytes[0] != 'D' || bytes[1] != 'H' || bytes[2] != 'F')
		return std::nullopt;
	std::vector<hog_entry> out;
	std::size_t at{3};
	while (at < bytes.size())
	{
		if (bytes.size() - at < 17 || out.size() == MAX_HOG_ENTRIES)
			return std::nullopt;
		std::string name;
		for (std::size_t i{}; i != 13 && bytes[at + i]; ++i)
			name += static_cast<char>(bytes[at + i]);
		reader r{bytes, at + 13};
		const std::size_t size{r.u32()};
		at += 17;
		if (size > bytes.size() - at)
			return std::nullopt;
		out.push_back({std::move(name), at, size});
		at += size;
	}
	return out;
}

/* The entry of that name (not case sensitive), or nothing. */
[[nodiscard]]
inline std::optional<std::span<const std::uint8_t>> hog_file(const std::span<const std::uint8_t> bytes, const std::span<const hog_entry> dir, const std::string_view name)
{
	const auto want{lower(name)};
	for (const auto &e : dir)
		if (lower(e.name) == want)
			return bytes.subspan(e.offset, e.size);
	return std::nullopt;
}

/*
 * MN2 (and MSN): "name = ...", "num_levels = N" and N level files,
 * "num_secrets = N" and N "file,level" lines.
 */
struct mission_file
{
	std::string name;
	std::vector<std::string> levels, secret_levels;
};

[[nodiscard]]
inline mission_file parse_mission(const std::string_view text)
{
	mission_file m;
	std::vector<std::string_view> lines;
	for (std::size_t at{}; at < text.size();)
	{
		auto end{text.find('\n', at)};
		if (end == std::string_view::npos)
			end = text.size();
		lines.push_back(text.substr(at, end - at));
		at = end + 1;
	}
	const auto key_is{[](const std::string_view line, const std::string_view key) {
		return line.size() >= key.size() && lower(line.substr(0, key.size())) == key;
	}};
	const auto value{[](const std::string_view line) {
		const auto eq{line.find('=')};
		return eq == std::string_view::npos ? std::string_view{} : trim(line.substr(eq + 1));
	}};
	/* A level line: the file name up to a space, comma or comment. */
	const auto file_of{[](std::string_view line) {
		line = trim(line);
		const auto end{line.find_first_of(" \t,;")};
		return std::string{line.substr(0, end)};
	}};
	const auto count{[&value](const std::string_view line) {
		unsigned n{};
		for (const char c : value(line))
		{
			if (c < '0' || c > '9')
				break;
			n = n * 10 + static_cast<unsigned>(c - '0');
			if (n > 255)
				return 0u;
		}
		return n;
	}};
	for (std::size_t i{}; i != lines.size(); ++i)
	{
		const auto line{trim(lines[i])};
		if (m.name.empty() && (key_is(line, "name") || key_is(line, "xname") || key_is(line, "zname") || key_is(line, "!name")))
			m.name = std::string{value(line)};
		else if (key_is(line, "num_levels") || key_is(line, "num_secrets"))
		{
			auto &list{key_is(line, "num_levels") ? m.levels : m.secret_levels};
			for (unsigned n{count(line)}; n && i + 1 < lines.size(); --n)
				if (auto f{file_of(lines[++i])}; !f.empty())
					list.push_back(std::move(f));
		}
	}
	return m;
}

/*
 * The level: vertices and segments (gamemine.cpp, load_mine_data_compiled).
 */
struct segment
{
	std::array<std::uint16_t, 8> verts{};
	/* SEGMENT_NONE: a wall; SEGMENT_EXIT: the mine's exit. */
	std::array<std::uint16_t, 6> children{};
};

struct level
{
	std::vector<vec3> vertices;
	std::vector<segment> segments;
	int version{};
};

/* `name` tells the old format (.sdl, Descent 1 shareware) from the new. */
[[nodiscard]]
inline std::optional<level> read_level(const std::span<const std::uint8_t> bytes, const std::string_view name = {})
{
	reader h{bytes};
	/* "LVLP" */
	if (h.u32() != 0x504c564cu)
		return std::nullopt;
	level out;
	out.version = h.i32();
	const auto mine_offset{h.u32()};
	if (!h.ok() || out.version < 0 || out.version > 100 || mine_offset >= bytes.size())
		return std::nullopt;
	const bool new_format{!(name.size() >= 4 && lower(name.substr(name.size() - 4)) == ".sdl")};
	reader r{bytes, mine_offset};
	r.u8();	/* compiled version */
	const std::size_t nv{new_format ? r.u16() : r.u32()};
	const std::size_t ns{new_format ? r.u16() : r.u32()};
	if (!r.ok() || nv > MAX_VERTICES || ns > MAX_SEGMENTS || !ns)
		return std::nullopt;
	out.vertices.resize(nv);
	for (auto &v : out.vertices)
		for (auto &c : v)
			c = r.i32() / FIX_UNIT;
	out.segments.resize(ns);
	const auto special{[&r](const unsigned mask) {
		if (mask & (1u << 6))
			r.skip(4);
	}};
	const auto children{[&r, ns](segment &s, const unsigned mask) {
		for (unsigned k{}; k != 6; ++k)
		{
			auto c{SEGMENT_NONE};
			if (mask & (1u << k))
			{
				c = r.u16();
				if (c != SEGMENT_EXIT && c >= ns)
					c = SEGMENT_NONE;
			}
			s.children[k] = c;
		}
	}};
	const auto verts{[&r, nv](segment &s) {
		for (auto &v : s.verts)
		{
			v = r.u16();
			if (v >= nv)
				return false;
		}
		return true;
	}};
	for (auto &s : out.segments)
	{
		const unsigned mask{new_format ? r.u8() : 0x7fu};
		if (out.version == 5)
		{
			special(mask);
			if (!verts(s))
				return std::nullopt;
			children(s, mask);
		}
		else
		{
			children(s, mask);
			if (!verts(s))
				return std::nullopt;
			if (out.version <= 1)
				special(mask);
		}
		if (out.version <= 5)
			r.skip(2);	/* static light */
		const unsigned wall_mask{new_format ? r.u8() : 0x3fu};
		std::array<bool, 6> wall{};
		for (unsigned k{}; k != 6; ++k)
			if (wall_mask & (1u << k))
				wall[k] = r.u8() != 255;
		for (unsigned k{}; k != 6; ++k)
		{
			if (s.children[k] != SEGMENT_NONE && !wall[k])
				continue;
			const auto tmap{r.u16()};
			if (!new_format || (tmap & 0x8000))
				r.skip(2);
			r.skip(4 * 6);	/* uvls */
		}
		if (!r.ok())
			return std::nullopt;
	}
	return out;
}

/*
 * Rays through the segments.
 */

[[nodiscard]]
inline vec3 sub(const vec3 &a, const vec3 &b)
{
	return {{a[0] - b[0], a[1] - b[1], a[2] - b[2]}};
}

[[nodiscard]]
inline vec3 cross(const vec3 &a, const vec3 &b)
{
	return {{a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]}};
}

[[nodiscard]]
inline double dot(const vec3 &a, const vec3 &b)
{
	return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

/* Where the ray o + t d meets the triangle (a, b, c), t > 0; or -1. */
[[nodiscard]]
inline double ray_triangle(const vec3 &o, const vec3 &d, const vec3 &a, const vec3 &b, const vec3 &c)
{
	constexpr double edge_slack{1e-6};
	const auto e1{sub(b, a)}, e2{sub(c, a)};
	const auto p{cross(d, e2)};
	const double det{dot(e1, p)};
	if (std::abs(det) < 1e-12)
		return -1;
	const double inv{1 / det};
	const auto s{sub(o, a)};
	const double u{dot(s, p) * inv};
	if (u < -edge_slack || u > 1 + edge_slack)
		return -1;
	const auto q{cross(s, e1)};
	const double v{dot(d, q) * inv};
	if (v < -edge_slack || u + v > 1 + edge_slack)
		return -1;
	const double t{dot(e2, q) * inv};
	return t > 1e-4 ? t : -1;
}

/* The free distance from `from` (in segment `seg`) along the unit
 * vector `dir` to the first side without a neighbour, at most `limit`.
 * The ray leaves each segment through the side it meets furthest (a
 * point a little outside its segment still finds the way out; a side
 * that is not flat is tried split both ways), through at most
 * MAX_RAY_STEPS segments.  0 if `seg` is no segment of the level.
 */
[[nodiscard]]
inline double free_distance(const level &l, std::size_t seg, vec3 from, const vec3 &dir, const double limit = MAX_RAY)
{
	if (seg >= l.segments.size())
		return 0;
	double travelled{};
	for (std::size_t step{}; step != MAX_RAY_STEPS; ++step)
	{
		const auto &s{l.segments[seg]};
		double best{-1};
		std::size_t side{};
		for (std::size_t k{}; k != 6; ++k)
		{
			const auto &sv{side_verts[k]};
			const auto &a{l.vertices[s.verts[sv[0]]]}, &b{l.vertices[s.verts[sv[1]]]}, &c{l.vertices[s.verts[sv[2]]]}, &d{l.vertices[s.verts[sv[3]]]};
			for (const double t : {ray_triangle(from, dir, a, b, c), ray_triangle(from, dir, a, c, d), ray_triangle(from, dir, a, b, d), ray_triangle(from, dir, b, c, d)})
				if (t > best)
				{
					best = t;
					side = k;
				}
		}
		if (best < 0)
			return travelled;
		travelled += best;
		if (travelled >= limit)
			return limit;
		const auto next{s.children[side]};
		if (next == SEGMENT_NONE || next == SEGMENT_EXIT)
			return travelled;
		for (std::size_t k{}; k != 3; ++k)
			from[k] += dir[k] * best;
		seg = next;
	}
	return travelled;
}

/*
 * The room of each segment and the level's character.
 */

/* Room classes by the room of the segment (the median free distance
 * from its centre over ROOM_DIRECTIONS directions): tight below
 * ROOM_TIGHT, open from ROOM_OPEN.
 */
/* The room is about half the width of the space (a corridor of the
 * standard 20 unit segments gives 10 to 15).
 */
constexpr double ROOM_TIGHT{15};
constexpr double ROOM_OPEN{35};
/* Long sight lines: the 90th percentile of the segments' longest free
 * distance from this.
 */
constexpr double LONG_SIGHT{200};
enum class room_class : std::uint8_t
{
	tight,
	medium,
	open,
};
constexpr std::size_t ROOM_CLASSES{3};
inline constexpr std::array<const char *, ROOM_CLASSES> room_class_names{{"tight", "medium", "open"}};

[[nodiscard]]
inline room_class class_of(const double room)
{
	return room < ROOM_TIGHT ? room_class::tight : room < ROOM_OPEN ? room_class::medium : room_class::open;
}

struct segment_room
{
	vec3 centre{};
	/* The median and the longest free distance from the centre over
	 * ROOM_DIRECTIONS directions.
	 */
	double room{}, longest{};
	double volume{};
	room_class kind{};
};

struct level_geometry
{
	level mesh;
	std::vector<segment_room> rooms;
	/* Shares of the volume by room class; the volume-weighted median
	 * room; the 90th percentile of the longest free distances; the
	 * whole volume.
	 */
	std::array<double, ROOM_CLASSES> volume_share{};
	double median_room{}, long_lines{}, volume{};
	/* For people: the level ("level 1 \"SnyTek: Pyroglyphic\" of
	 * \"Pyroglyphic (Sny)\"") and where it came from ("PYGL.HOG:
	 * pygl_132.rl2").
	 */
	std::string name, source;
	[[nodiscard]]
	const segment_room *room_at(const std::size_t seg) const
	{
		return seg < rooms.size() ? &rooms[seg] : nullptr;
	}
	/* The level in a few words. */
	[[nodiscard]]
	std::string character() const;
};

namespace detail {

[[nodiscard]]
inline double tet_volume(const vec3 &a, const vec3 &b, const vec3 &c, const vec3 &d)
{
	return std::abs(dot(sub(b, a), cross(sub(c, a), sub(d, a)))) / 6;
}

/* A hexahedron as five tetrahedra (vertex 0..3 the front, 4..7 the
 * back, as segment.h numbers them).
 */
[[nodiscard]]
inline double segment_volume(const level &l, const segment &s)
{
	const auto v{[&](const unsigned i) -> const vec3 & { return l.vertices[s.verts[i]]; }};
	return tet_volume(v(0), v(1), v(3), v(4)) + tet_volume(v(1), v(2), v(3), v(6)) + tet_volume(v(1), v(4), v(5), v(6)) + tet_volume(v(3), v(4), v(6), v(7)) + tet_volume(v(1), v(3), v(4), v(6));
}

}

/* The directions the room of a segment is measured in: spread evenly
 * over the sphere (a Fibonacci spiral), none along an axis or a
 * diagonal: in a level built of cubes those meet the vertices and edges
 * exactly.
 */
constexpr std::size_t ROOM_DIRECTIONS{26};

[[nodiscard]]
inline std::vector<vec3> room_directions()
{
	std::vector<vec3> d;
	const double golden{3.14159265358979323846 * (3 - std::sqrt(5.0))};
	for (std::size_t i{}; i != ROOM_DIRECTIONS; ++i)
	{
		const double y{1 - (static_cast<double>(i) + 0.5) * 2 / ROOM_DIRECTIONS};
		const double r{std::sqrt(1 - y * y)};
		const double a{golden * static_cast<double>(i) + 0.3};
		d.push_back({{r * std::cos(a), y, r * std::sin(a)}});
	}
	return d;
}

[[nodiscard]]
inline level_geometry measure(level mesh, std::string name = {}, std::string source = {})
{
	level_geometry g;
	g.mesh = std::move(mesh);
	g.name = std::move(name);
	g.source = std::move(source);
	const auto &l{g.mesh};
	const auto dirs{room_directions()};
	std::vector<std::pair<double, double>> by_room;
	std::vector<double> longest;
	for (std::size_t i{}; i != l.segments.size(); ++i)
	{
		const auto &s{l.segments[i]};
		segment_room r;
		for (const auto v : s.verts)
			for (std::size_t k{}; k != 3; ++k)
				r.centre[k] += l.vertices[v][k] / 8;
		/* A little off the centre: not on a plane of symmetry. */
		const vec3 from{{r.centre[0] + 0.137, r.centre[1] + 0.071, r.centre[2] + 0.093}};
		std::vector<double> d;
		for (const auto &dir : dirs)
			d.push_back(free_distance(l, i, from, dir));
		std::sort(d.begin(), d.end());
		r.room = (d[d.size() / 2 - 1] + d[d.size() / 2]) / 2;
		r.longest = d.back();
		r.volume = detail::segment_volume(l, s);
		r.kind = class_of(r.room);
		g.volume += r.volume;
		g.volume_share[static_cast<std::size_t>(r.kind)] += r.volume;
		by_room.emplace_back(r.room, r.volume);
		longest.push_back(r.longest);
		g.rooms.push_back(r);
	}
	if (g.volume > 0)
		for (auto &v : g.volume_share)
			v /= g.volume;
	std::sort(by_room.begin(), by_room.end());
	double cum{};
	for (const auto &[room, volume] : by_room)
		if ((cum += volume) >= g.volume / 2)
		{
			g.median_room = room;
			break;
		}
	if (!longest.empty())
	{
		std::sort(longest.begin(), longest.end());
		g.long_lines = longest[std::min(longest.size() - 1, longest.size() * 9 / 10)];
	}
	return g;
}

inline std::string level_geometry::character() const
{
	const auto share{[this](const room_class c) { return volume_share[static_cast<std::size_t>(c)]; }};
	const double open_share{share(room_class::open)}, tight{share(room_class::tight)};
	const double medium{share(room_class::medium)};
	const bool long_views{long_lines >= LONG_SIGHT};
	const char *const kind{open_share >= 0.5 ? (long_views ? "large open spaces with long sight lines" : "large open spaces")
		: tight >= 0.5 ? (long_views ? "tight corridors with long sight lines" : "tight corridors")
		: medium >= 0.5 ? (long_views ? "wide tunnels and rooms with long sight lines" : "wide corridors and small rooms")
		: "a mix of tight corridors and open rooms"};
	const unsigned segments = mesh.segments.size();
	std::array<char, 256> buf;
	std::snprintf(buf.data(), buf.size(), "%s: %.0f%% of the volume open, %.0f%% medium, %.0f%% tight; room %.0f units (volume-weighted median), the longest free line from a segment %.0f units (90th percentile); %u segments, %.0f thousand cubic units",
		kind, 100 * open_share, 100 * medium, 100 * tight, median_room, long_lines, segments, volume / 1000);
	return buf.data();
}

/*
 * Finding a recorded level among the missions of a folder.
 */

/* A mission of the folder: its .mn2 (or .msn) and its .hog, which
 * find_level's `load_hog` reads when a level of it is asked for (it may
 * stay empty).
 */
struct mission_entry
{
	std::string stem;
	std::string hog_name;
	mission_file info;
	bool hog_read{};
	std::vector<std::uint8_t> hog;
	std::vector<hog_entry> dir;
};

/* What the recording says of the level (level_record). */
struct level_query
{
	std::string mission, mission_file, level_file;
	int level_num{};
	std::size_t segments{};
};

struct level_match
{
	/* The mission and the level file it was found in; empty if none. */
	const mission_entry *mission{};
	std::string level_file;
	std::optional<level> mesh;
	/* How it was found, or why not. */
	std::string note;
};

/* The game keeps this much of a mission's name (mission.h). */
constexpr std::size_t MISSION_NAME_KEPT{25};

/* The level among `missions`: by the mission's file name if the
 * recording has it (format minor 3; it is cut to `file_name_kept`), else
 * by the mission's name and the level number; it must have the
 * recorded number of segments.  Several missions that fit with the same
 * geometry are one; with different geometry, ambiguous: nothing.
 * `load_hog` reads a mission's .hog into `hog` and `dir` (once).
 */
template <typename hog_loader>
[[nodiscard]]
inline level_match find_level(std::vector<mission_entry> &missions, const level_query &q, hog_loader &&load_hog, const std::size_t file_name_kept = 20)
{
	level_match out;
	std::vector<mission_entry *> candidates;
	const bool by_file{!q.mission_file.empty()};
	const auto want_file{lower(q.mission_file)};
	const auto want_name{lower(trim(q.mission))};
	for (auto &m : missions)
	{
		if (by_file)
		{
			const auto stem{lower(m.stem)};
			if (stem == want_file || (want_file.size() >= file_name_kept && stem.starts_with(want_file)))
				candidates.push_back(&m);
		}
		else if (const auto name{lower(trim(m.info.name))}; !want_name.empty() && (name == want_name || (want_name.size() >= MISSION_NAME_KEPT && name.starts_with(want_name))))
			candidates.push_back(&m);
	}
	if (candidates.empty())
	{
		out.note = by_file ? "no mission file \"" + q.mission_file + "\" in the folder" : "no mission named \"" + q.mission + "\" in the folder";
		return out;
	}
	struct fit
	{
		mission_entry *m;
		std::string file;
		level mesh;
	};
	std::vector<fit> fits;
	std::string why;
	for (const auto m : candidates)
	{
		std::string file{q.level_file};
		if (file.empty())
		{
			if (q.level_num > 0 && static_cast<std::size_t>(q.level_num) <= m->info.levels.size())
				file = m->info.levels[static_cast<std::size_t>(q.level_num - 1)];
			else if (q.level_num < 0 && static_cast<std::size_t>(-q.level_num) <= m->info.secret_levels.size())
				file = m->info.secret_levels[static_cast<std::size_t>(-q.level_num - 1)];
		}
		if (file.empty())
		{
			why += "; " + m->stem + " has no level " + std::to_string(q.level_num);
			continue;
		}
		if (!m->hog_read)
		{
			m->hog_read = true;
			load_hog(*m);
		}
		if (m->dir.empty())
		{
			why += "; " + m->stem + ": no readable .hog";
			continue;
		}
		const auto bytes{hog_file(m->hog, m->dir, file)};
		if (!bytes)
		{
			why += "; " + m->hog_name + " has no " + file;
			continue;
		}
		auto mesh{read_level(*bytes, file)};
		if (!mesh)
		{
			why += "; " + m->hog_name + ": " + file + " cannot be read";
			continue;
		}
		if (q.segments && mesh->segments.size() != q.segments)
		{
			why += "; " + m->hog_name + ": " + file + " has " + std::to_string(mesh->segments.size()) + " segments, the recording " + std::to_string(q.segments);
			continue;
		}
		fits.push_back({m, std::move(file), std::move(*mesh)});
	}
	if (fits.empty())
	{
		out.note = "not found" + why;
		return out;
	}
	const auto same{[](const level &a, const level &b) {
		return a.vertices == b.vertices && a.segments.size() == b.segments.size() && std::equal(a.segments.begin(), a.segments.end(), b.segments.begin(), [](const segment &x, const segment &y) { return x.verts == y.verts && x.children == y.children; });
	}};
	std::string names;
	for (const auto &f : fits)
		names += (names.empty() ? "" : ", ") + f.m->stem;
	if (fits.size() > 1)
	{
		if (!std::all_of(fits.begin() + 1, fits.end(), [&](const fit &f) { return same(f.mesh, fits.front().mesh); }))
		{
			out.note = "ambiguous: " + names + " fit the mission name, level number and segment count, with different geometry (a recording of format minor 3 names the file)";
			return out;
		}
		out.note = "the same level in " + names + "; ";
	}
	auto &f{fits.front()};
	out.note += std::string{by_file ? "by the mission file" : "by the mission name and level number"} + ": " + f.m->hog_name + ", " + f.file;
	out.mission = f.m;
	out.level_file = std::move(f.file);
	out.mesh = std::move(f.mesh);
	return out;
}

}
