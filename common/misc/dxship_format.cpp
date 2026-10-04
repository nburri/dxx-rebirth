/*
 * This file is part of the DXX-Rebirth project <https://github.com/dxx-rebirth/dxx-rebirth/>.
 * It is copyright by its individual contributors, as recorded in the
 * project's Git history.  See COPYING.txt at the top level for license
 * terms and a link to the Git history.
 */
/*
 * Reader and writer of the custom ship file (dxship_format.h).
 *
 * Layout, all little-endian:
 *
 *	header	magic "DXSH" | version u16 | flags u16 (0) | file_size u32
 *		| section_count u16 | reserved u16 (0)
 *	section	type u32 (FourCC) | size u32 | payload, zero padded to 4 bytes
 *
 *	MANI	"key=value\n" lines, printable ASCII
 *	BNDS	radius f32, centre f32x3, mins f32x3, maxs f32x3
 *	VERT	count u32, then per vertex: pos f32x3, normal snorm16x3, part u8,
 *		tint u8, uv f32x2, colour RGBA8 (32 bytes)
 *	INDX	count u32 (a multiple of 3), u16 indices
 *	MATL	count u8, 3 zero bytes, per material: first index u32, index
 *		count u32, base colour RGBA8, texture u8, mask u8, flags u8, 0
 *	TEXR	one PNG per section, numbered in file order
 *	PART	count u8, 3 zero bytes, per part: centre f32x3, radius f32
 *	GUNS	8 x {present u8, 3 zero bytes, pos f32x3}
 *
 * Unknown section types are skipped, so that a later version can add
 * sections an older reader ignores.
 */

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include "dxship_format.h"

namespace dcx {

namespace dxship {

namespace {

[[nodiscard]]
std::uint16_t get16(const std::uint8_t *const p)
{
	return static_cast<std::uint16_t>(p[0] | (p[1] << 8));
}

[[nodiscard]]
std::uint32_t get32(const std::uint8_t *const p)
{
	return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) | (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
}

[[nodiscard]]
float getf(const std::uint8_t *const p)
{
	return std::bit_cast<float>(get32(p));
}

[[nodiscard]]
std::uint32_t get32be(const std::uint8_t *const p)
{
	return (static_cast<std::uint32_t>(p[0]) << 24) | (static_cast<std::uint32_t>(p[1]) << 16) | (static_cast<std::uint32_t>(p[2]) << 8) | static_cast<std::uint32_t>(p[3]);
}

struct out_buffer
{
	std::vector<std::uint8_t> b;
	void u8(const std::uint8_t v)
	{
		b.push_back(v);
	}
	void u16(const std::uint16_t v)
	{
		b.push_back(static_cast<std::uint8_t>(v));
		b.push_back(static_cast<std::uint8_t>(v >> 8));
	}
	void u32(const std::uint32_t v)
	{
		for (unsigned i = 0; i < 4; ++i)
			b.push_back(static_cast<std::uint8_t>(v >> (8 * i)));
	}
	void f32(const float v)
	{
		u32(std::bit_cast<std::uint32_t>(v));
	}
	void vec(const vec3 &v)
	{
		f32(v.x);
		f32(v.y);
		f32(v.z);
	}
	void bytes(const std::span<const std::uint8_t> s)
	{
		b.insert(b.end(), s.begin(), s.end());
	}
	void pad4()
	{
		while (b.size() & 3)
			b.push_back(0);
	}
};

[[nodiscard]]
bool finite(const vec3 &v)
{
	return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}

[[nodiscard]]
float length(const vec3 &v)
{
	return std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
}

[[nodiscard]]
vec3 read_vec(const std::uint8_t *const p)
{
	return {getf(p), getf(p + 4), getf(p + 8)};
}

[[nodiscard]]
bool is_power_of_two(const unsigned v)
{
	return v && !(v & (v - 1));
}

constexpr float MAX_UV{1024.0f};

/* The manifest keys this version knows, with their length limits. */
struct manifest_key
{
	std::string_view key;
	std::string manifest::*field;
	std::size_t limit;
};

constexpr std::array<manifest_key, 7> Manifest_keys{{
	{"name", &manifest::name, MAX_NAME},
	{"title", &manifest::title, MAX_TITLE},
	{"author", &manifest::author, MAX_AUTHOR},
	{"licence", &manifest::licence, MAX_LICENCE},
	{"source", &manifest::source, MAX_SOURCE},
	{"description", &manifest::description, MAX_DESCRIPTION},
	{"converter", &manifest::converter, MAX_TITLE},
}};

[[nodiscard]]
std::string parse_manifest(const std::span<const std::uint8_t> s, manifest &out)
{
	if (s.size() > MAX_MANIFEST)
		return "manifest too large";
	const std::string_view text{reinterpret_cast<const char *>(s.data()), s.size()};
	std::array<bool, Manifest_keys.size()> seen{};
	std::size_t pos{};
	while (pos < text.size())
	{
		const auto eol{text.find('\n', pos)};
		if (eol == text.npos)
			return "manifest line without newline";
		const auto line{text.substr(pos, eol - pos)};
		pos = eol + 1;
		const auto eq{line.find('=')};
		if (eq == line.npos || eq == 0)
			return "manifest line without key";
		const auto key{line.substr(0, eq)}, value{line.substr(eq + 1)};
		if (!std::ranges::all_of(key, [](const char c) { return (c >= 'a' && c <= 'z') || c == '_'; }))
			return "bad manifest key";
		const auto known{std::ranges::find(Manifest_keys, key, &manifest_key::key)};
		if (known == Manifest_keys.end())
		{
			/* A key of a later version: checked, then ignored. */
			if (!valid_text(value, MAX_DESCRIPTION))
				return "bad manifest value";
			continue;
		}
		const auto i{static_cast<std::size_t>(known - Manifest_keys.begin())};
		if (seen[i])
			return "duplicate manifest key";
		seen[i] = true;
		if (!valid_text(value, known->limit))
			return "bad manifest value for " + std::string(key);
		out.*(known->field) = std::string(value);
	}
	if (!valid_name(out.name))
		return "missing or bad ship name";
	if (out.licence.empty())
		return "missing licence";
	return {};
}

void write_manifest(out_buffer &o, const manifest &m)
{
	for (const auto &k : Manifest_keys)
	{
		const auto &value{m.*(k.field)};
		if (value.empty())
			continue;
		o.bytes({reinterpret_cast<const std::uint8_t *>(k.key.data()), k.key.size()});
		o.u8('=');
		o.bytes({reinterpret_cast<const std::uint8_t *>(value.data()), value.size()});
		o.u8('\n');
	}
}

}

std::int16_t float_to_snorm16(const float v)
{
	if (!std::isfinite(v))
		return 0;
	const auto c{std::clamp(v, -1.0f, 1.0f)};
	return static_cast<std::int16_t>(std::lround(c * 32767.0f));
}

bool valid_name(const std::string_view name)
{
	return !name.empty() && name.size() <= MAX_NAME && std::ranges::all_of(name, [](const char c) {
		return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
	});
}

bool valid_text(const std::string_view text, const std::size_t limit)
{
	return text.size() <= limit && std::ranges::all_of(text, [](const char c) {
		return c >= 0x20 && c <= 0x7e;
	});
}

std::optional<png_info> read_png_info(const std::span<const std::uint8_t> png, std::string &error)
{
	static constexpr std::array<std::uint8_t, 8> signature{{0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'}};
	/* Signature, IHDR length and type, 13 bytes of IHDR, its CRC. */
	if (png.size() < 8 + 8 + 13 + 4)
	{
		error = "texture too short";
		return std::nullopt;
	}
	if (!std::equal(signature.begin(), signature.end(), png.begin()))
	{
		error = "texture is not a PNG";
		return std::nullopt;
	}
	const auto p{png.data() + 8};
	if (get32be(p) != 13 || std::memcmp(p + 4, "IHDR", 4))
	{
		error = "PNG without IHDR";
		return std::nullopt;
	}
	png_info r;
	const auto w{get32be(p + 8)}, h{get32be(p + 12)};
	r.bit_depth = p[16];
	r.colour_type = p[17];
	const auto compression{p[18]}, filter{p[19]}, interlace{p[20]};
	if (!is_power_of_two(w) || !is_power_of_two(h) || w > MAX_TEXTURE_SIZE || h > MAX_TEXTURE_SIZE)
	{
		error = "texture size not a power of two up to 512";
		return std::nullopt;
	}
	if (r.bit_depth != 8 || (r.colour_type != 0 && r.colour_type != 2 && r.colour_type != 4 && r.colour_type != 6) || compression || filter || interlace > 1)
	{
		error = "texture must be an 8-bit grey, RGB or RGBA PNG";
		return std::nullopt;
	}
	r.width = w;
	r.height = h;
	return r;
}

std::string validate(const model &m)
{
	if (!valid_name(m.info.name))
		return "bad ship name (a-z, 0-9, _ and -, at most 24 characters)";
	if (m.info.licence.empty())
		return "missing licence";
	for (const auto &k : Manifest_keys)
		if (!valid_text(m.info.*(k.field), k.limit))
			return "bad manifest value for " + std::string(k.key);
	if (!std::isfinite(m.radius) || std::fabs(m.radius - PYRO_RADIUS) > PYRO_RADIUS * 0.01f)
		return "radius is not the Pyro's";
	if (!finite(m.centre) || !finite(m.mins) || !finite(m.maxs) || m.mins.x > m.maxs.x || m.mins.y > m.maxs.y || m.mins.z > m.maxs.z)
		return "bad bounds";
	const auto vcount{m.vertices.size()};
	if (vcount < 3 || vcount > MAX_VERTICES)
		return "vertex count out of range";
	const auto nparts{std::max<std::size_t>(m.parts.size(), 1)};
	if (m.parts.size() == 1 || m.parts.size() > MAX_PARTS)
		return "part count out of range";
	const float max_extent{PYRO_RADIUS * RADIUS_TOLERANCE};
	bool zone{};
	for (const auto &v : m.vertices)
	{
		if (!finite(v.pos) || length(v.pos) > max_extent)
			return "vertex outside the ship's radius";
		if (!std::isfinite(v.u) || !std::isfinite(v.v) || std::fabs(v.u) > MAX_UV || std::fabs(v.v) > MAX_UV)
			return "bad texture coordinate";
		if (v.part >= nparts)
			return "vertex part out of range";
		if (v.tint)
			zone = true;
	}
	const auto icount{m.indices.size()};
	if (icount < 3 || icount > MAX_INDICES || icount % 3)
		return "index count out of range";
	for (const auto i : m.indices)
		if (i >= vcount)
			return "index out of range";
	if (m.materials.empty() || m.materials.size() > MAX_MATERIALS)
		return "material count out of range";
	if (m.textures.size() > MAX_TEXTURES)
		return "too many textures";
	std::vector<png_info> infos;
	for (const auto &t : m.textures)
	{
		std::string e;
		const auto i{read_png_info(t.png, e)};
		if (!i)
			return e;
		infos.push_back(*i);
	}
	for (const auto &mat : m.materials)
	{
		if (mat.first_index % 3 || mat.index_count % 3 || !mat.index_count || mat.first_index > icount || mat.index_count > icount - mat.first_index)
			return "material index range out of range";
		if (mat.flags & ~std::uint8_t{3})
			return "unknown material flags";
		if (mat.texture != NO_TEXTURE && mat.texture >= m.textures.size())
			return "material texture out of range";
		if (mat.mask != NO_TEXTURE)
		{
			if (mat.mask >= m.textures.size() || mat.texture == NO_TEXTURE)
				return "material mask out of range";
			const auto &a{infos[mat.texture]}, &b{infos[mat.mask]};
			if (a.width != b.width || a.height != b.height)
				return "mask size differs from its texture";
			zone = true;
		}
		if (mat.has_flag(material_flag::tint))
			zone = true;
	}
	if (!zone)
		return "no player colour zone";
	for (const auto &p : m.parts)
		if (!finite(p.centre) || length(p.centre) > max_extent || !std::isfinite(p.radius) || p.radius < 0 || p.radius > 2 * max_extent)
			return "bad part";
	for (const auto &g : m.guns)
		if (g.present ? !finite(g.pos) || length(g.pos) > 2 * max_extent : g.pos != vec3{})
			return "bad gun marker";
	return {};
}

std::vector<std::uint8_t> write(const model &m, std::string &error)
{
	error = validate(m);
	if (!error.empty())
		return {};
	out_buffer o;
	o.bytes(FILE_MAGIC);
	o.u16(FORMAT_VERSION);
	o.u16(0);
	o.u32(0);	/* file size, patched below */
	o.u16(0);	/* section count, patched below */
	o.u16(0);
	unsigned sections{};
	const auto section = [&](const std::uint32_t type, auto &&fill) {
		o.u32(type);
		const auto size_at{o.b.size()};
		o.u32(0);
		const auto start{o.b.size()};
		fill();
		const auto size{static_cast<std::uint32_t>(o.b.size() - start)};
		for (unsigned i = 0; i < 4; ++i)
			o.b[size_at + i] = static_cast<std::uint8_t>(size >> (8 * i));
		o.pad4();
		++sections;
	};
	section(SECTION_MANIFEST, [&] { write_manifest(o, m.info); });
	section(SECTION_BOUNDS, [&] {
		o.f32(m.radius);
		o.vec(m.centre);
		o.vec(m.mins);
		o.vec(m.maxs);
	});
	section(SECTION_VERTICES, [&] {
		o.u32(static_cast<std::uint32_t>(m.vertices.size()));
		for (const auto &v : m.vertices)
		{
			o.vec(v.pos);
			for (const auto n : v.normal)
				o.u16(static_cast<std::uint16_t>(n));
			o.u8(v.part);
			o.u8(v.tint);
			o.f32(v.u);
			o.f32(v.v);
			o.bytes(v.colour);
		}
	});
	section(SECTION_INDICES, [&] {
		o.u32(static_cast<std::uint32_t>(m.indices.size()));
		for (const auto i : m.indices)
			o.u16(i);
	});
	section(SECTION_MATERIALS, [&] {
		o.u8(static_cast<std::uint8_t>(m.materials.size()));
		o.u8(0);
		o.u16(0);
		for (const auto &mat : m.materials)
		{
			o.u32(mat.first_index);
			o.u32(mat.index_count);
			o.bytes(mat.base_colour);
			o.u8(mat.texture);
			o.u8(mat.mask);
			o.u8(mat.flags);
			o.u8(0);
		}
	});
	for (const auto &t : m.textures)
		section(SECTION_TEXTURE, [&] { o.bytes(t.png); });
	if (!m.parts.empty())
		section(SECTION_PARTS, [&] {
			o.u8(static_cast<std::uint8_t>(m.parts.size()));
			o.u8(0);
			o.u16(0);
			for (const auto &p : m.parts)
			{
				o.vec(p.centre);
				o.f32(p.radius);
			}
		});
	if (std::ranges::any_of(m.guns, &gun_marker::present))
		section(SECTION_GUNS, [&] {
			for (const auto &g : m.guns)
			{
				o.u8(g.present);
				o.u8(0);
				o.u16(0);
				o.vec(g.pos);
			}
		});
	if (o.b.size() > MAX_FILE_SIZE)
	{
		error = "file larger than 1 MiB";
		return {};
	}
	const auto size{static_cast<std::uint32_t>(o.b.size())};
	for (unsigned i = 0; i < 4; ++i)
		o.b[8 + i] = static_cast<std::uint8_t>(size >> (8 * i));
	o.b[12] = static_cast<std::uint8_t>(sections);
	o.b[13] = static_cast<std::uint8_t>(sections >> 8);
	return std::move(o.b);
}

parse_result parse(const std::span<const std::uint8_t> file)
{
	parse_result r;
	const auto fail = [&r](std::string e) -> parse_result {
		r.m.reset();
		r.error = std::move(e);
		return std::move(r);
	};
	if (file.size() < HEADER_SIZE)
		return fail("file too short");
	if (file.size() > MAX_FILE_SIZE)
		return fail("file larger than 1 MiB");
	const auto base{file.data()};
	if (!std::equal(FILE_MAGIC.begin(), FILE_MAGIC.end(), base))
		return fail("not a ship file");
	if (get16(base + 4) != FORMAT_VERSION)
		return fail("unsupported ship file version");
	if (get16(base + 6) || get16(base + 14))
		return fail("reserved header fields set");
	if (get32(base + 8) != file.size())
		return fail("file size mismatch");
	const unsigned nsections{get16(base + 12)};
	if (nsections > MAX_SECTIONS)
		return fail("too many sections");
	model m;
	struct seen_sections
	{
		bool manifest{}, bounds{}, vertices{}, indices{}, materials{}, parts{}, guns{};
	} seen;
	std::size_t pos{HEADER_SIZE};
	for (unsigned s = 0; s < nsections; ++s)
	{
		if (file.size() - pos < SECTION_HEADER_SIZE)
			return fail("section header past the end");
		const auto type{get32(base + pos)};
		const std::size_t size{get32(base + pos + 4)};
		pos += SECTION_HEADER_SIZE;
		if (size > file.size() - pos)
			return fail("section past the end");
		const std::span<const std::uint8_t> payload{base + pos, size};
		const auto p{payload.data()};
		pos += size;
		const auto padded{(size + 3) & ~std::size_t{3}};
		if (padded - size > file.size() - pos)
			return fail("section padding past the end");
		pos += padded - size;
		const auto once = [](bool &flag) -> bool {
			if (flag)
				return false;
			flag = true;
			return true;
		};
		switch (type)
		{
			case SECTION_MANIFEST:
				if (!once(seen.manifest))
					return fail("duplicate section");
				if (auto e{parse_manifest(payload, m.info)}; !e.empty())
					return fail(std::move(e));
				break;
			case SECTION_BOUNDS:
				if (!once(seen.bounds))
					return fail("duplicate section");
				if (size != BOUNDS_SIZE)
					return fail("bad bounds section");
				m.radius = getf(p);
				m.centre = read_vec(p + 4);
				m.mins = read_vec(p + 16);
				m.maxs = read_vec(p + 28);
				break;
			case SECTION_VERTICES:
				{
					if (!once(seen.vertices))
						return fail("duplicate section");
					if (size < 4)
						return fail("bad vertex section");
					const auto count{get32(p)};
					if (count < 3 || count > MAX_VERTICES || size != 4 + std::size_t{count} * VERTEX_SIZE)
						return fail("bad vertex count");
					m.vertices.resize(count);
					auto q{p + 4};
					for (auto &v : m.vertices)
					{
						v.pos = read_vec(q);
						for (unsigned i = 0; i < 3; ++i)
							v.normal[i] = static_cast<std::int16_t>(get16(q + 12 + 2 * i));
						v.part = q[18];
						v.tint = q[19];
						v.u = getf(q + 20);
						v.v = getf(q + 24);
						std::copy_n(q + 28, 4, v.colour.begin());
						q += VERTEX_SIZE;
					}
				}
				break;
			case SECTION_INDICES:
				{
					if (!once(seen.indices))
						return fail("duplicate section");
					if (size < 4)
						return fail("bad index section");
					const auto count{get32(p)};
					if (count < 3 || count > MAX_INDICES || count % 3 || size != 4 + std::size_t{count} * 2)
						return fail("bad index count");
					m.indices.resize(count);
					for (std::size_t i = 0; i < count; ++i)
						m.indices[i] = get16(p + 4 + 2 * i);
				}
				break;
			case SECTION_MATERIALS:
				{
					if (!once(seen.materials))
						return fail("duplicate section");
					if (size < 4)
						return fail("bad material section");
					const unsigned count{p[0]};
					if (!count || count > MAX_MATERIALS || p[1] || p[2] || p[3] || size != 4 + count * MATERIAL_SIZE)
						return fail("bad material count");
					m.materials.resize(count);
					auto q{p + 4};
					for (auto &mat : m.materials)
					{
						mat.first_index = get32(q);
						mat.index_count = get32(q + 4);
						std::copy_n(q + 8, 4, mat.base_colour.begin());
						mat.texture = q[12];
						mat.mask = q[13];
						mat.flags = q[14];
						if (q[15])
							return fail("bad material padding");
						q += MATERIAL_SIZE;
					}
				}
				break;
			case SECTION_TEXTURE:
				{
					if (m.textures.size() >= MAX_TEXTURES)
						return fail("too many textures");
					std::string e;
					const auto info{read_png_info(payload, e)};
					if (!info)
						return fail(std::move(e));
					m.textures.push_back({std::vector<std::uint8_t>(payload.begin(), payload.end()), *info});
				}
				break;
			case SECTION_PARTS:
				{
					if (!once(seen.parts))
						return fail("duplicate section");
					if (size < 4)
						return fail("bad part section");
					const unsigned count{p[0]};
					if (count < 2 || count > MAX_PARTS || p[1] || p[2] || p[3] || size != 4 + count * PART_SIZE)
						return fail("bad part count");
					m.parts.resize(count);
					auto q{p + 4};
					for (auto &pt : m.parts)
					{
						pt.centre = read_vec(q);
						pt.radius = getf(q + 12);
						q += PART_SIZE;
					}
				}
				break;
			case SECTION_GUNS:
				{
					if (!once(seen.guns))
						return fail("duplicate section");
					if (size != GUNS_SIZE)
						return fail("bad gun section");
					auto q{p};
					for (auto &g : m.guns)
					{
						if (q[0] > 1 || q[1] || q[2] || q[3])
							return fail("bad gun marker");
						g.present = q[0];
						g.pos = read_vec(q + 4);
						q += 16;
					}
				}
				break;
			default:
				/* A section of a later version. */
				break;
		}
	}
	if (pos != file.size())
		return fail("data after the last section");
	if (!seen.manifest || !seen.bounds || !seen.vertices || !seen.indices || !seen.materials)
		return fail("missing section");
	if (auto e{validate(m)}; !e.empty())
		return fail(std::move(e));
	r.m = std::move(m);
	return r;
}

}

}
