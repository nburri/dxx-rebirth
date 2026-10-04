/*
 * This file is part of the DXX-Rebirth project <https://github.com/dxx-rebirth/dxx-rebirth/>.
 * It is copyright by its individual contributors, as recorded in the
 * project's Git history.  See COPYING.txt at the top level for license
 * terms and a link to the Git history.
 */
/*
 * Test of the custom ship file (dxship_format.h) and of SHA-256
 * (sha256.h): the FIPS test vectors, round trips, every rule of the
 * reader, and a fuzz test: every truncation of a valid file, random bit
 * flips, byte mutations, section header damage and random garbage must
 * be rejected or yield a model that passes `validate` - never crash or
 * read out of bounds (run it under -fsanitize=address,undefined too).
 *
 * Usage: test-dxship [iterations] [ship files...]; files given are
 * checked and fuzzed as well.
 */

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <fstream>
#include <limits>
#include <random>
#include <string>
#include <vector>

#include "dxship_format.h"
#include "sha256.h"

namespace {

using namespace dcx;
namespace ds = dcx::dxship;

unsigned failures;

void check_failed(const char *const what, const char *const file, const unsigned line)
{
	std::fprintf(stderr, "%s:%u: check failed: %s\n", file, line, what);
	++failures;
}

#define CHECK(cond)	do { if (!(cond)) check_failed(#cond, __FILE__, __LINE__); } while (0)

std::vector<std::uint8_t> bytes_of(const std::string_view s)
{
	return {s.begin(), s.end()};
}

void test_sha256()
{
	CHECK(sha256_hex(sha256_of({})) == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
	CHECK(sha256_hex(sha256_of(bytes_of("abc"))) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
	CHECK(sha256_hex(sha256_of(bytes_of("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"))) == "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
	CHECK(sha256_hex(sha256_of(bytes_of("abcdefghbcdefghicdefghijdefghijkefghijklfghijklmghijklmnhijklmnoijklmnopjklmnopqklmnopqrlmnopqrsmnopqrstnopqrstu"))) == "cf5b16a778af8380036ce59e7b0492370b249b11e8f07a51afac45037afee9d1");
	/* One million 'a', fed in uneven pieces. */
	sha256 s;
	const std::vector<std::uint8_t> chunk(997, 'a');
	std::size_t left{1000000};
	while (left)
	{
		const auto n{std::min(left, chunk.size())};
		s.update(std::span(chunk).first(n));
		left -= n;
	}
	CHECK(sha256_hex(s.finish()) == "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
	/* Every length around the block and padding boundaries. */
	for (std::size_t n = 50; n < 140; ++n)
	{
		const std::vector<std::uint8_t> v(n, static_cast<std::uint8_t>(n));
		sha256 a;
		a.update(std::span(v).first(n / 3));
		a.update(std::span(v).subspan(n / 3));
		CHECK(a.finish() == sha256_of(v));
	}
	sha256_digest d;
	CHECK(sha256_from_hex("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", d));
	CHECK(d == sha256_of(bytes_of("abc")));
	CHECK(sha256_from_hex("BA7816BF8F01CFEA414140DE5DAE2223B00361A396177A9CB410FF61F20015AD", d));
	CHECK(!sha256_from_hex("ba7816bf", d));
	CHECK(!sha256_from_hex("xa7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", d));
}

/* A PNG header the reader accepts (it decodes nothing). */
std::vector<std::uint8_t> fake_png(const unsigned w, const unsigned h, const std::uint8_t colour_type = 6)
{
	std::vector<std::uint8_t> p{0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n', 0, 0, 0, 13, 'I', 'H', 'D', 'R'};
	for (const auto v : {w, h})
		for (int i = 3; i >= 0; --i)
			p.push_back(static_cast<std::uint8_t>(v >> (8 * i)));
	p.insert(p.end(), {8, colour_type, 0, 0, 0, 1, 2, 3, 4, 0, 0, 0, 0, 'I', 'E', 'N', 'D', 0xae, 0x42, 0x60, 0x82});
	return p;
}

/* A cube: 24 vertices, 12 triangles, two materials (the top a texture
 * with a colour mask, the rest flat with the tint flag), two parts.
 */
ds::model make_cube()
{
	ds::model m;
	m.info.name = "test-cube_1";
	m.info.title = "Test Cube";
	m.info.author = "nobody";
	m.info.licence = "CC0-1.0";
	m.info.source = "https://example.invalid/cube";
	m.info.converter = "test";
	const float h{3.0f};
	m.radius = ds::PYRO_RADIUS;
	m.mins = {-h, -h, -h};
	m.maxs = {h, h, h};
	const std::array<std::array<float, 3>, 6> normals{{{{0, 1, 0}}, {{0, -1, 0}}, {{1, 0, 0}}, {{-1, 0, 0}}, {{0, 0, 1}}, {{0, 0, -1}}}};
	for (unsigned f = 0; f < 6; ++f)
	{
		const auto &n{normals[f]};
		/* Two axes in the face plane. */
		const std::array<float, 3> a{{n[1] != 0 ? 1.0f : 0.0f, n[1] != 0 ? 0.0f : 1.0f, 0}};
		const std::array<float, 3> b{{n[1] * a[2] - n[2] * a[1], n[2] * a[0] - n[0] * a[2], n[0] * a[1] - n[1] * a[0]}};
		for (unsigned c = 0; c < 4; ++c)
		{
			const float sa{c & 1 ? 1.0f : -1.0f}, sb{c & 2 ? 1.0f : -1.0f};
			ds::vertex v;
			v.pos = {h * (n[0] + sa * a[0] + sb * b[0]), h * (n[1] + sa * a[1] + sb * b[1]), h * (n[2] + sa * a[2] + sb * b[2])};
			v.normal = {{ds::float_to_snorm16(n[0]), ds::float_to_snorm16(n[1]), ds::float_to_snorm16(n[2])}};
			v.u = sa * 0.5f + 0.5f;
			v.v = sb * 0.5f + 0.5f;
			v.part = f >= 4 ? 1 : 0;
			v.tint = f == 5 ? 128 : 0;
			v.colour = {{200, static_cast<std::uint8_t>(40 * f), 10, 255}};
			m.vertices.push_back(v);
		}
	}
	for (std::uint16_t f = 0; f < 6; ++f)
	{
		const std::uint16_t b{static_cast<std::uint16_t>(f * 4)};
		m.indices.insert(m.indices.end(), {b, static_cast<std::uint16_t>(b + 1), static_cast<std::uint16_t>(b + 3), b, static_cast<std::uint16_t>(b + 3), static_cast<std::uint16_t>(b + 2)});
	}
	ds::material top;
	top.first_index = 0;
	top.index_count = 6;
	top.texture = 0;
	top.mask = 1;
	ds::material rest;
	rest.first_index = 6;
	rest.index_count = 30;
	rest.base_colour = {{10, 20, 30, 255}};
	rest.flags = static_cast<std::uint8_t>(ds::material_flag::tint) | static_cast<std::uint8_t>(ds::material_flag::double_sided);
	m.materials = {top, rest};
	m.textures.push_back({fake_png(64, 32), {}});
	m.textures.push_back({fake_png(64, 32, 0), {}});
	m.parts = {{{0, 0, 0}, 4}, {{0, 0, 2}, 3}};
	m.guns[0] = {true, {1, 2, 3}};
	m.guns[7] = {true, {-1, -2, -3}};
	return m;
}

std::vector<std::uint8_t> write_ok(const ds::model &m)
{
	std::string e;
	auto f{ds::write(m, e)};
	if (!e.empty())
		std::fprintf(stderr, "write: %s\n", e.c_str());
	CHECK(e.empty());
	CHECK(!f.empty());
	return f;
}

void test_round_trip()
{
	const auto m{make_cube()};
	const auto f{write_ok(m)};
	CHECK(f.size() % 4 == 0);
	const auto r{ds::parse(f)};
	if (!r.m)
		std::fprintf(stderr, "parse: %s\n", r.error.c_str());
	CHECK(r.m);
	CHECK(r.error.empty());
	if (r.m)
	{
		CHECK(*r.m == m);
		CHECK(r.m->textures[0].info.width == 64 && r.m->textures[0].info.height == 32);
		/* Writing what was read gives the same bytes, so the hash of a
		 * ship is stable.
		 */
		CHECK(write_ok(*r.m) == f);
	}
	/* Without parts and gun markers. */
	auto plain{m};
	plain.parts.clear();
	for (auto &v : plain.vertices)
		v.part = 0;
	plain.guns = {};
	const auto f2{write_ok(plain)};
	const auto r2{ds::parse(f2)};
	CHECK(r2.m && *r2.m == plain);
}

void expect_invalid(const ds::model &m, const char *const what)
{
	std::string e;
	const auto f{ds::write(m, e)};
	if (e.empty() || !f.empty())
	{
		std::fprintf(stderr, "accepted, but should not: %s\n", what);
		++failures;
	}
}

void test_rules()
{
	{
		auto m{make_cube()};
		m.info.licence.clear();
		expect_invalid(m, "no licence");
	}
	for (const char *name : {"", "Upper", "with space", "a/b", "abcdefghijklmnopqrstuvwxy"})
	{
		auto m{make_cube()};
		m.info.name = name;
		expect_invalid(m, "bad name");
	}
	{
		auto m{make_cube()};
		m.info.title = "tab\there";
		expect_invalid(m, "control character");
		m.info.title = std::string(33, 'x');
		expect_invalid(m, "title too long");
	}
	{
		auto m{make_cube()};
		m.materials[0].mask = ds::NO_TEXTURE;
		m.materials[1].flags = 0;
		for (auto &v : m.vertices)
			v.tint = 0;
		expect_invalid(m, "no colour zone");
		m.vertices[3].tint = 1;
		std::string e;
		CHECK(!ds::write(m, e).empty());
	}
	{
		auto m{make_cube()};
		m.indices[5] = static_cast<std::uint16_t>(m.vertices.size());
		expect_invalid(m, "index out of range");
	}
	{
		auto m{make_cube()};
		m.vertices[2].pos.x = ds::PYRO_RADIUS * ds::RADIUS_TOLERANCE * 1.01f;
		expect_invalid(m, "vertex outside the radius");
		m.vertices[2].pos.x = std::numeric_limits<float>::quiet_NaN();
		expect_invalid(m, "NaN position");
	}
	{
		auto m{make_cube()};
		m.vertices[1].u = std::numeric_limits<float>::infinity();
		expect_invalid(m, "infinite uv");
	}
	{
		auto m{make_cube()};
		m.radius *= 1.1f;
		expect_invalid(m, "radius not the Pyro's");
	}
	{
		auto m{make_cube()};
		m.textures[0].png = fake_png(48, 32);
		expect_invalid(m, "texture not a power of two");
		m.textures[0].png = fake_png(1024, 32);
		expect_invalid(m, "texture too large");
		m.textures[0].png = fake_png(64, 32);
		m.textures[0].png[24] = 16;
		expect_invalid(m, "16-bit texture");
		m.textures[0].png = fake_png(64, 32, 3);
		expect_invalid(m, "palette texture");
		m.textures[0].png = fake_png(64, 32);
		m.textures[0].png[1] = 'Q';
		expect_invalid(m, "not a PNG");
	}
	{
		auto m{make_cube()};
		m.textures[1].png = fake_png(32, 32, 0);
		expect_invalid(m, "mask size differs");
	}
	{
		auto m{make_cube()};
		m.materials[0].texture = 7;
		expect_invalid(m, "texture index");
		m.materials[0].texture = ds::NO_TEXTURE;
		expect_invalid(m, "mask without texture");
	}
	{
		auto m{make_cube()};
		m.materials[1].index_count = 33;
		expect_invalid(m, "material range past the indices");
		m.materials[1].index_count = 29;
		expect_invalid(m, "material range not whole triangles");
		m.materials[1].index_count = 30;
		m.materials[1].flags = 4;
		expect_invalid(m, "unknown material flag");
	}
	{
		auto m{make_cube()};
		m.materials.resize(9, m.materials[0]);
		expect_invalid(m, "too many materials");
	}
	{
		auto m{make_cube()};
		m.textures.resize(5, m.textures[0]);
		expect_invalid(m, "too many textures");
	}
	{
		auto m{make_cube()};
		m.vertices[0].part = 2;
		expect_invalid(m, "part out of range");
		m.vertices[0].part = 0;
		m.parts.resize(1);
		expect_invalid(m, "a single part");
		m.parts.resize(11, m.parts[0]);
		expect_invalid(m, "too many parts");
	}
	{
		auto m{make_cube()};
		m.guns[3].pos = {1, 0, 0};
		expect_invalid(m, "absent gun with a position");
	}
	{
		/* The limits: 10000 triangles and 16000 vertices pass, one more
		 * does not.
		 */
		auto m{make_cube()};
		while (m.vertices.size() < ds::MAX_VERTICES)
			m.vertices.push_back(m.vertices[m.vertices.size() % 24]);
		m.indices.resize(ds::MAX_INDICES);
		for (std::size_t i = 0; i < m.indices.size(); ++i)
			m.indices[i] = static_cast<std::uint16_t>(i % ds::MAX_VERTICES);
		m.materials[1].index_count = ds::MAX_INDICES - 6;
		std::string e;
		const auto f{ds::write(m, e)};
		CHECK(e.empty());
		CHECK(ds::parse(f).m);
		auto bigger{m};
		bigger.vertices.push_back(m.vertices[0]);
		expect_invalid(bigger, "too many vertices");
		bigger = m;
		bigger.indices.insert(bigger.indices.end(), {0, 1, 2});
		expect_invalid(bigger, "too many triangles");
	}
	{
		/* A texture that makes the file larger than 1 MiB. */
		auto m{make_cube()};
		m.textures[0].png.resize(ds::MAX_FILE_SIZE);
		expect_invalid(m, "file over 1 MiB");
	}
}

void put32(std::vector<std::uint8_t> &f, const std::size_t at, const std::uint32_t v)
{
	for (unsigned i = 0; i < 4; ++i)
		f[at + i] = static_cast<std::uint8_t>(v >> (8 * i));
}

/* Every section start in a valid file. */
std::vector<std::size_t> section_offsets(const std::vector<std::uint8_t> &f)
{
	std::vector<std::size_t> r;
	std::size_t pos{ds::HEADER_SIZE};
	const unsigned n{static_cast<unsigned>(f[12] | (f[13] << 8))};
	for (unsigned s = 0; s < n; ++s)
	{
		r.push_back(pos);
		const std::size_t size{static_cast<std::size_t>(f[pos + 4]) | (static_cast<std::size_t>(f[pos + 5]) << 8) | (static_cast<std::size_t>(f[pos + 6]) << 16) | (static_cast<std::size_t>(f[pos + 7]) << 24)};
		pos += ds::SECTION_HEADER_SIZE + ((size + 3) & ~std::size_t{3});
	}
	return r;
}

void test_damaged_headers(const std::vector<std::uint8_t> &good)
{
	CHECK(ds::parse(good).m);
	{
		auto f{good};
		f[0] = 'X';
		CHECK(!ds::parse(f).m);
	}
	{
		auto f{good};
		f[4] = 2;
		CHECK(!ds::parse(f).m);
	}
	{
		auto f{good};
		put32(f, 8, static_cast<std::uint32_t>(f.size() + 4));
		CHECK(!ds::parse(f).m);
		f.insert(f.end(), {0, 0, 0, 0});
		CHECK(!ds::parse(f).m);
	}
	{
		auto f{good};
		f[12] = 0xff;
		f[13] = 0xff;
		CHECK(!ds::parse(f).m);
	}
	for (const auto at : section_offsets(good))
	{
		for (const std::uint32_t size : {0u, 1u, 3u, 0x7fffffffu, 0xffffffffu, 0xfffffff8u})
		{
			auto f{good};
			put32(f, at + 4, size);
			CHECK(!ds::parse(f).m);
		}
		/* The count inside a section, where there is one. */
		for (const std::uint32_t count : {0u, 1u, 2u, 0x10000u, 0x7fffffffu, 0xffffffffu, 0x40000000u})
		{
			auto f{good};
			put32(f, at + 8, count);
			const auto r{ds::parse(f)};
			if (r.m)
				CHECK(ds::validate(*r.m).empty());
		}
		/* A duplicate section: the section's type copied over the next. */
		auto f{good};
		const auto offsets{section_offsets(good)};
		const auto it{std::ranges::find(offsets, at)};
		if (it + 1 != offsets.end())
		{
			std::copy_n(good.begin() + static_cast<std::ptrdiff_t>(at), 4, f.begin() + static_cast<std::ptrdiff_t>(*(it + 1)));
			const auto r{ds::parse(f)};
			if (r.m)
				CHECK(ds::validate(*r.m).empty());
		}
	}
	{
		/* An unknown section is skipped. */
		auto f{good};
		const std::vector<std::uint8_t> extra{'X', 'T', 'R', 'A', 3, 0, 0, 0, 1, 2, 3, 0};
		f.insert(f.end(), extra.begin(), extra.end());
		put32(f, 8, static_cast<std::uint32_t>(f.size()));
		f[12] = static_cast<std::uint8_t>(f[12] + 1);
		const auto r{ds::parse(f)};
		CHECK(r.m);
	}
}

/* The reader must survive anything. */
unsigned accepted_mutations;

void parse_survives(const std::vector<std::uint8_t> &f)
{
	const auto r{ds::parse(f)};
	if (r.m)
	{
		++accepted_mutations;
		CHECK(r.error.empty());
		CHECK(ds::validate(*r.m).empty());
		/* An accepted file always writes back into one the reader accepts. */
		std::string e;
		const auto again{ds::write(*r.m, e)};
		CHECK(e.empty() && ds::parse(again).m);
	}
	else
		CHECK(!r.error.empty());
}

void fuzz(const std::vector<std::uint8_t> &good, const unsigned iterations, const unsigned seed)
{
	/* Every truncation. */
	for (std::size_t n = 0; n < good.size(); ++n)
		CHECK(!ds::parse(std::span(good).first(n)).m);
	std::mt19937 rng(seed);
	const auto pick = [&rng](const std::size_t n) {
		return std::uniform_int_distribution<std::size_t>(0, n - 1)(rng);
	};
	const auto offsets{section_offsets(good)};
	for (unsigned it = 0; it < iterations; ++it)
	{
		auto f{good};
		switch (it % 6)
		{
			case 0:
				/* One bit. */
				f[pick(f.size())] ^= static_cast<std::uint8_t>(1u << pick(8));
				break;
			case 1:
				/* A few random bytes. */
				for (unsigned k = 0, n = 1 + static_cast<unsigned>(pick(8)); k < n; ++k)
					f[pick(f.size())] = static_cast<std::uint8_t>(rng());
				break;
			case 2:
				/* Bytes near a section header (sizes, counts). */
				{
					const auto at{offsets[pick(offsets.size())] + pick(16)};
					if (at < f.size())
						f[at] = static_cast<std::uint8_t>(rng());
				}
				break;
			case 3:
				/* A run of 0xff or 0x00. */
				{
					const auto at{pick(f.size())}, n{std::min<std::size_t>(1 + pick(12), f.size() - at)};
					std::fill_n(f.begin() + static_cast<std::ptrdiff_t>(at), n, it & 8 ? 0xff : 0x00);
				}
				break;
			case 4:
				/* Cut and keep the header's size consistent. */
				{
					f.resize(ds::HEADER_SIZE + pick(f.size() - ds::HEADER_SIZE));
					put32(f, 8, static_cast<std::uint32_t>(f.size()));
				}
				break;
			default:
				/* A float made NaN or huge. */
				{
					const auto at{(pick(f.size() / 4)) * 4};
					put32(f, at, it & 16 ? 0x7fc00000u : 0x7f7fffffu);
				}
				break;
		}
		parse_survives(f);
	}
	/* Garbage with a valid header. */
	for (unsigned it = 0; it < iterations / 10; ++it)
	{
		std::vector<std::uint8_t> f(ds::HEADER_SIZE + pick(4096));
		for (auto &b : f)
			b = static_cast<std::uint8_t>(rng());
		std::copy(ds::FILE_MAGIC.begin(), ds::FILE_MAGIC.end(), f.begin());
		f[4] = 1;
		f[5] = f[6] = f[7] = f[14] = f[15] = 0;
		put32(f, 8, static_cast<std::uint32_t>(f.size()));
		f[13] = 0;
		f[12] = static_cast<std::uint8_t>(pick(ds::MAX_SECTIONS + 2));
		parse_survives(f);
	}
}

std::vector<std::uint8_t> read_file(const char *const path)
{
	std::ifstream f(path, std::ios::binary);
	return std::vector<std::uint8_t>(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
}

}

int main(const int argc, char **const argv)
{
	const unsigned iterations{argc > 1 ? static_cast<unsigned>(std::strtoul(argv[1], nullptr, 10)) : 200000u};
	test_sha256();
	test_round_trip();
	test_rules();
	const auto good{write_ok(make_cube())};
	test_damaged_headers(good);
	fuzz(good, iterations, 1);
	for (int i = 2; i < argc; ++i)
	{
		const auto f{read_file(argv[i])};
		const auto r{ds::parse(f)};
		if (!r.m)
		{
			std::fprintf(stderr, "%s: rejected: %s\n", argv[i], r.error.c_str());
			++failures;
			continue;
		}
		test_damaged_headers(f);
		fuzz(f, iterations / 4, static_cast<unsigned>(i));
	}
	std::printf("test-dxship: %u mutated files accepted (and valid)\n", accepted_mutations);
	if (failures)
	{
		std::fprintf(stderr, "test-dxship: %u failures\n", failures);
		return 1;
	}
	std::puts("test-dxship: all checks passed");
	return 0;
}
