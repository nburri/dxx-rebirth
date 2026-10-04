/*
 * This file is part of the DXX-Rebirth project <https://github.com/dxx-rebirth/dxx-rebirth/>.
 * It is copyright by its individual contributors, as recorded in the
 * project's Git history.  See COPYING.txt at the top level for license
 * terms and a link to the Git history.
 */
/*
 * The custom ship file `.dxship` (Documentation/custom-ships.md section
 * 3.2): one file with the manifest, the mesh and the textures of a ship
 * that is drawn instead of the Pyro-GX.  Nothing about it is simulated;
 * it is only drawn.
 *
 * The reader treats every file as untrusted (a ship may come from
 * another player over the network): every count, size, index and float
 * is checked before use, and the result is either a complete model or
 * an error text.  PNG textures are checked only up to their header here
 * (size, depth, colour type); decoding them is the caller's business.
 *
 * Depends on the standard library only, so that the converter
 * (common/tools/shipconv.cpp) and the unit tests can use it without the
 * game.
 */

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace dcx {

namespace dxship {

constexpr std::array<std::uint8_t, 4> FILE_MAGIC{{'D', 'X', 'S', 'H'}};
constexpr std::uint16_t FORMAT_VERSION{1};
constexpr std::size_t HEADER_SIZE{16};
constexpr std::size_t SECTION_HEADER_SIZE{8};

/* Limits (section 7 of the design, decisions D3 and D4). */
constexpr std::size_t MAX_FILE_SIZE{1u << 20};
constexpr unsigned MAX_SECTIONS{24};
constexpr unsigned MAX_VERTICES{16000};
constexpr unsigned MAX_TRIANGLES{10000};
constexpr unsigned MAX_INDICES{3 * MAX_TRIANGLES};
constexpr unsigned MAX_MATERIALS{8};
constexpr unsigned MAX_TEXTURES{4};
constexpr unsigned MAX_TEXTURE_SIZE{512};
/* Part 0 is the body, parts 1.. are debris (decision D6). */
constexpr unsigned MAX_PARTS{10};
constexpr unsigned GUN_POINTS{8};
constexpr std::size_t MAX_NAME{24};
constexpr std::size_t MAX_TITLE{32};
constexpr std::size_t MAX_AUTHOR{48};
constexpr std::size_t MAX_LICENCE{32};
constexpr std::size_t MAX_SOURCE{160};
constexpr std::size_t MAX_DESCRIPTION{160};
constexpr std::size_t MAX_MANIFEST{1024};

/* The Pyro-GX's bounding radius (polymodel `rad` of model 108 in
 * descent2.ham, 0x4bc3a / 65536).  Every ship is scaled to it (decision
 * D3); the reader rejects geometry farther than RADIUS_TOLERANCE times
 * it from the origin.
 */
constexpr float PYRO_RADIUS{4.73518372f};
constexpr float RADIUS_TOLERANCE{1.25f};

constexpr std::uint32_t fourcc(const char a, const char b, const char c, const char d)
{
	return static_cast<std::uint32_t>(static_cast<std::uint8_t>(a)) |
		(static_cast<std::uint32_t>(static_cast<std::uint8_t>(b)) << 8) |
		(static_cast<std::uint32_t>(static_cast<std::uint8_t>(c)) << 16) |
		(static_cast<std::uint32_t>(static_cast<std::uint8_t>(d)) << 24);
}

constexpr std::uint32_t SECTION_MANIFEST{fourcc('M', 'A', 'N', 'I')};
constexpr std::uint32_t SECTION_BOUNDS{fourcc('B', 'N', 'D', 'S')};
constexpr std::uint32_t SECTION_VERTICES{fourcc('V', 'E', 'R', 'T')};
constexpr std::uint32_t SECTION_INDICES{fourcc('I', 'N', 'D', 'X')};
constexpr std::uint32_t SECTION_MATERIALS{fourcc('M', 'A', 'T', 'L')};
constexpr std::uint32_t SECTION_TEXTURE{fourcc('T', 'E', 'X', 'R')};
constexpr std::uint32_t SECTION_PARTS{fourcc('P', 'A', 'R', 'T')};
constexpr std::uint32_t SECTION_GUNS{fourcc('G', 'U', 'N', 'S')};

/* Bytes per vertex in VERT: position f32x3, normal snorm16x3, part u8,
 * tint u8, uv f32x2, colour RGBA8.
 */
constexpr std::size_t VERTEX_SIZE{32};
constexpr std::size_t MATERIAL_SIZE{16};
constexpr std::size_t PART_SIZE{16};
constexpr std::size_t BOUNDS_SIZE{40};
constexpr std::size_t GUNS_SIZE{GUN_POINTS * 16};

constexpr std::uint8_t NO_TEXTURE{0xff};

enum class material_flag : std::uint8_t
{
	/* The whole material is the player's colour zone (decision D2). */
	tint = 1 << 0,
	double_sided = 1 << 1,
};

struct vec3
{
	float x{}, y{}, z{};
	constexpr bool operator==(const vec3 &) const = default;
};

struct vertex
{
	vec3 pos;
	/* Unit normal, stored as signed 16-bit (x / 32767). */
	std::array<std::int16_t, 3> normal{};
	std::uint8_t part{};
	/* Player colour weight of this vertex, 0..255 (colour zone of an
	 * untextured material, or a vertex painted with the zone colour).
	 */
	std::uint8_t tint{};
	float u{}, v{};
	/* Vertex colour, multiplied with the material. */
	std::array<std::uint8_t, 4> colour{{255, 255, 255, 255}};
	constexpr bool operator==(const vertex &) const = default;
};

struct material
{
	std::uint32_t first_index{};
	std::uint32_t index_count{};
	std::array<std::uint8_t, 4> base_colour{{255, 255, 255, 255}};
	/* Index into `textures`, or NO_TEXTURE. */
	std::uint8_t texture{NO_TEXTURE};
	/* A texture whose red channel is the player colour weight per texel
	 * (the colour zone), or NO_TEXTURE.  Its size equals the albedo's.
	 */
	std::uint8_t mask{NO_TEXTURE};
	std::uint8_t flags{};
	[[nodiscard]]
	constexpr bool has_flag(const material_flag f) const
	{
		return flags & static_cast<std::uint8_t>(f);
	}
	constexpr bool operator==(const material &) const = default;
};

struct part
{
	vec3 centre;
	float radius{};
	constexpr bool operator==(const part &) const = default;
};

struct png_info
{
	unsigned width{}, height{};
	std::uint8_t bit_depth{}, colour_type{};
};

struct texture
{
	/* A PNG file, 8 bits per channel, grey, grey+alpha, RGB or RGBA, each
	 * side a power of two of at most MAX_TEXTURE_SIZE.
	 */
	std::vector<std::uint8_t> png;
	png_info info;
	bool operator==(const texture &o) const
	{
		return png == o.png;
	}
};

struct manifest
{
	/* The id: [a-z0-9_-], 1..MAX_NAME characters. */
	std::string name;
	std::string title;
	std::string author;
	/* An SPDX licence id or a short licence text; required. */
	std::string licence;
	std::string source;
	std::string description;
	std::string converter;
	bool operator==(const manifest &) const = default;
};

struct gun_marker
{
	bool present{};
	vec3 pos;
	constexpr bool operator==(const gun_marker &) const = default;
};

struct model
{
	manifest info;
	float radius{};
	vec3 centre, mins, maxs;
	std::vector<vertex> vertices;
	std::vector<std::uint16_t> indices;
	std::vector<material> materials;
	std::vector<texture> textures;
	/* Empty: the whole ship is one part (no authored debris). */
	std::vector<part> parts;
	/* Informational only (the guns are always the Pyro's). */
	std::array<gun_marker, GUN_POINTS> guns{};
	bool operator==(const model &) const = default;
};

struct parse_result
{
	std::optional<model> m;
	/* Why the file was rejected (empty if accepted). */
	std::string error;
};

/* Parse and validate a whole file. */
[[nodiscard]]
parse_result parse(std::span<const std::uint8_t> file);

/* Check a PNG's signature and IHDR chunk against the texture rules. */
[[nodiscard]]
std::optional<png_info> read_png_info(std::span<const std::uint8_t> png, std::string &error);

/* Check the model against every rule the reader enforces (used by the
 * writer, so that nothing invalid is ever written).
 */
[[nodiscard]]
std::string validate(const model &m);

/* Serialise a valid model.  Returns an empty vector and sets `error` if
 * `validate` rejects it or it does not fit MAX_FILE_SIZE.
 */
[[nodiscard]]
std::vector<std::uint8_t> write(const model &m, std::string &error);

[[nodiscard]]
bool valid_name(std::string_view name);

/* Printable ASCII only (0x20..0x7e), at most `limit` characters. */
[[nodiscard]]
bool valid_text(std::string_view text, std::size_t limit);

[[nodiscard]]
constexpr float snorm16_to_float(const std::int16_t v)
{
	return v < -32767 ? -1.0f : static_cast<float>(v) / 32767.0f;
}

[[nodiscard]]
std::int16_t float_to_snorm16(float v);

}

}
