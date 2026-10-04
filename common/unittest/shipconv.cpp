/*
 * This file is part of the DXX-Rebirth project <https://github.com/dxx-rebirth/dxx-rebirth/>.
 * It is copyright by its individual contributors, as recorded in the
 * project's Git history.  See COPYING.txt at the top level for license
 * terms and a link to the Git history.
 */
/*
 * Test of the ship converter (common/tools/shipconv.cpp): small
 * synthetic models (OBJ with a material library and a PNG texture, glTF
 * with debris parts and gun markers) through the real program, and the
 * results read with the game's reader.
 *
 * Usage: test-shipconv [path to shipconv] (default build/common/shipconv)
 */

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <ctime>
#include <optional>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "dxship_format.h"

namespace {

namespace ds = dcx::dxship;
namespace fs = std::filesystem;

unsigned failures;

void check_failed(const char *const what, const char *const file, const unsigned line)
{
	std::fprintf(stderr, "%s:%u: check failed: %s\n", file, line, what);
	++failures;
}

#define CHECK(cond)	do { if (!(cond)) check_failed(#cond, __FILE__, __LINE__); } while (0)

std::string Tool;
fs::path Dir;

void write_text(const fs::path &p, const std::string &text)
{
	std::ofstream(p, std::ios::binary) << text;
}

void write_bytes(const fs::path &p, const std::vector<std::uint8_t> &b)
{
	std::ofstream(p, std::ios::binary).write(reinterpret_cast<const char *>(b.data()), static_cast<std::streamsize>(b.size()));
}

std::vector<std::uint8_t> read_bytes(const fs::path &p)
{
	std::ifstream f(p, std::ios::binary);
	return std::vector<std::uint8_t>(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
}

/* A minimal PNG writer: stored (uncompressed) deflate blocks. */
std::uint32_t crc32(const std::uint8_t *p, std::size_t n, std::uint32_t c = 0)
{
	c = ~c;
	while (n--)
	{
		c ^= *p++;
		for (unsigned k = 0; k < 8; ++k)
			c = (c >> 1) ^ (0xedb88320u & (0u - (c & 1)));
	}
	return ~c;
}

void be32(std::vector<std::uint8_t> &o, const std::uint32_t v)
{
	for (int i = 3; i >= 0; --i)
		o.push_back(static_cast<std::uint8_t>(v >> (8 * i)));
}

void chunk(std::vector<std::uint8_t> &o, const char *const type, const std::vector<std::uint8_t> &data)
{
	be32(o, static_cast<std::uint32_t>(data.size()));
	std::vector<std::uint8_t> td(type, type + 4);
	td.insert(td.end(), data.begin(), data.end());
	o.insert(o.end(), td.begin(), td.end());
	be32(o, crc32(td.data(), td.size()));
}

/* RGB image, row-major. */
std::vector<std::uint8_t> png_rgb(const unsigned w, const unsigned h, const std::vector<std::uint8_t> &rgb)
{
	std::vector<std::uint8_t> raw;
	for (unsigned y = 0; y < h; ++y)
	{
		raw.push_back(0);
		raw.insert(raw.end(), rgb.begin() + static_cast<std::ptrdiff_t>(y * w * 3), rgb.begin() + static_cast<std::ptrdiff_t>((y + 1) * w * 3));
	}
	std::vector<std::uint8_t> z{0x78, 0x01};
	std::size_t pos{};
	do
	{
		const auto n{std::min<std::size_t>(raw.size() - pos, 65535)};
		z.push_back(pos + n == raw.size() ? 1 : 0);
		z.push_back(static_cast<std::uint8_t>(n));
		z.push_back(static_cast<std::uint8_t>(n >> 8));
		z.push_back(static_cast<std::uint8_t>(~n));
		z.push_back(static_cast<std::uint8_t>(~n >> 8));
		z.insert(z.end(), raw.begin() + static_cast<std::ptrdiff_t>(pos), raw.begin() + static_cast<std::ptrdiff_t>(pos + n));
		pos += n;
	} while (pos < raw.size());
	std::uint32_t a{1}, b{};
	for (const auto c : raw)
	{
		a = (a + c) % 65521;
		b = (b + a) % 65521;
	}
	be32(z, (b << 16) | a);
	std::vector<std::uint8_t> o{0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
	std::vector<std::uint8_t> ihdr;
	be32(ihdr, w);
	be32(ihdr, h);
	ihdr.insert(ihdr.end(), {8, 2, 0, 0, 0});
	chunk(o, "IHDR", ihdr);
	chunk(o, "IDAT", z);
	chunk(o, "IEND", {});
	return o;
}

/* Run the converter; its exit status. */
int run(const std::string &args, const char *const log = "log.txt")
{
	const auto cmd{"\"" + Tool + "\" " + args + " > \"" + (Dir / log).string() + "\" 2>&1"};
	const auto r{std::system(cmd.c_str())};
	return r;
}

std::string read_text(const fs::path &p)
{
	std::ifstream f(p);
	return std::string(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
}

std::optional<ds::model> load(const fs::path &p)
{
	const auto r{ds::parse(read_bytes(p))};
	if (!r.m)
		std::fprintf(stderr, "%s: %s\n", p.string().c_str(), r.error.c_str());
	return r.m;
}

float max_radius(const ds::model &m)
{
	float r{};
	for (const auto &v : m.vertices)
		r = std::max(r, std::sqrt(v.pos.x * v.pos.x + v.pos.y * v.pos.y + v.pos.z * v.pos.z));
	return r;
}

/* A box from `lo` to `hi` as OBJ faces (vertex numbers from `base`). */
std::string obj_box(const float lx, const float ly, const float lz, const float hx, const float hy, const float hz, const unsigned base, const std::string &top_material, const std::string &other_material)
{
	std::string s;
	char line[160];
	for (unsigned i = 0; i < 8; ++i)
	{
		std::snprintf(line, sizeof(line), "v %g %g %g\n", i & 1 ? hx : lx, i & 2 ? hy : ly, i & 4 ? hz : lz);
		s += line;
	}
	static constexpr std::array<std::array<unsigned, 4>, 6> faces{{{{6, 7, 3, 2}}, {{1, 5, 4, 0}}, {{3, 7, 5, 1}}, {{4, 6, 2, 0}}, {{5, 7, 6, 4}}, {{2, 3, 1, 0}}}};
	for (unsigned f = 0; f < 6; ++f)
	{
		s += "usemtl " + (f == 0 ? top_material : other_material) + "\n";
		std::snprintf(line, sizeof(line), "f %u/1 %u/2 %u/3 %u/4\n", base + faces[f][0], base + faces[f][1], base + faces[f][2], base + faces[f][3]);
		s += line;
	}
	return s;
}

const std::string Common{" --author tester --licence CC0-1.0 --source https://example.invalid"};

void test_obj()
{
	/* 8x8 texture: left half red, right half grey. */
	std::vector<std::uint8_t> rgb;
	for (unsigned y = 0; y < 8; ++y)
		for (unsigned x = 0; x < 8; ++x)
			rgb.insert(rgb.end(), x < 4 ? std::initializer_list<std::uint8_t>{200, 30, 30} : std::initializer_list<std::uint8_t>{120, 120, 120});
	write_bytes(Dir / "tex.png", png_rgb(8, 8, rgb));
	write_text(Dir / "box.mtl", "newmtl Hull\nKd 1 1 1\nmap_Kd tex.png\nnewmtl Accent\nKd 0.2 0.3 0.9\nnewmtl Plain\nKd 0.5 0.5 0.5\n");
	const auto uv{std::string("vt 0 0\nvt 1 0\nvt 1 1\nvt 0 1\n")};
	/* The accent material on the top face of a flat box: 1/6 of the
	 * faces, but a small share of the area.
	 */
	write_text(Dir / "box.obj", "mtllib box.mtl\n" + uv + obj_box(-2, -0.5f, -4, 2, 0.5f, 4, 1, "Accent", "Hull"));
	const auto out{Dir / "box.dxship"};
	CHECK(run("\"" + (Dir / "box.obj").string() + "\" -o \"" + out.string() + "\" --name box" + Common) == 0);
	if (const auto m{load(out)})
	{
		CHECK(m->info.name == "box");
		CHECK(m->info.title == "box");
		CHECK(m->info.licence == "CC0-1.0");
		CHECK(m->indices.size() == 36);
		CHECK(std::fabs(max_radius(*m) - ds::PYRO_RADIUS) < 0.02f);
		CHECK(std::ranges::any_of(m->materials, [](const ds::material &mat) { return mat.has_flag(ds::material_flag::tint); }));
		CHECK(m->textures.size() == 1);
		/* The long axis is z (forward). */
		CHECK(m->maxs.z - m->mins.z > m->maxs.x - m->mins.x);
		/* Computed normals and the winding point out of the box. */
		bool outward{true};
		for (const auto &v : m->vertices)
			if (v.normal[0] * v.pos.x + v.normal[1] * v.pos.y + v.normal[2] * v.pos.z <= 0)
				outward = false;
		for (std::size_t i = 0; i + 2 < m->indices.size(); i += 3)
		{
			const auto &a{m->vertices[m->indices[i]].pos}, &b{m->vertices[m->indices[i + 1]].pos}, &c{m->vertices[m->indices[i + 2]].pos};
			const float ux{b.x - a.x}, uy{b.y - a.y}, uz{b.z - a.z}, vx{c.x - a.x}, vy{c.y - a.y}, vz{c.z - a.z};
			const float nx{uy * vz - uz * vy}, ny{uz * vx - ux * vz}, nz{ux * vy - uy * vx};
			const float cx{a.x + b.x + c.x}, cy{a.y + b.y + c.y}, cz{a.z + b.z + c.z};
			if (nx * cx + ny * cy + nz * cz <= 0)
				outward = false;
		}
		CHECK(outward);
		/* No debris_* parts: the automatic split (left, body, right). */
		CHECK(m->parts.size() == 3);
	}
	CHECK(run("\"" + (Dir / "box.obj").string() + "\" -o \"" + (Dir / "nodebris.dxship").string() + "\" --name box --no-debris" + Common) == 0);
	if (const auto m{load(Dir / "nodebris.dxship")})
		CHECK(m->parts.empty());
	CHECK(run("--check \"" + out.string() + "\"") == 0);
	/* The colour zone by texel colour: the red half of the texture. */
	write_text(Dir / "box2.obj", "mtllib box.mtl\n" + uv + obj_box(-2, -0.5f, -4, 2, 0.5f, 4, 1, "Hull", "Hull"));
	CHECK(run("\"" + (Dir / "box2.obj").string() + "\" -o \"" + (Dir / "box2.dxship").string() + "\" --name box2 --colour-key C81E1E:10" + Common) == 0);
	if (const auto m{load(Dir / "box2.dxship")})
	{
		CHECK(m->textures.size() == 2);
		CHECK(m->materials.size() == 1 && m->materials[0].mask == 1);
	}
	/* No colour zone at all. */
	write_text(Dir / "plain.obj", "mtllib box.mtl\n" + uv + obj_box(-2, -0.5f, -4, 2, 0.5f, 4, 1, "Plain", "Plain"));
	CHECK(run("\"" + (Dir / "plain.obj").string() + "\" -o \"" + (Dir / "plain.dxship").string() + "\" --name plain" + Common) != 0);
	CHECK(read_text(Dir / "log.txt").find("colour zone") != std::string::npos);
	CHECK(!fs::exists(Dir / "plain.dxship"));
	/* The zone named on the command line. */
	CHECK(run("\"" + (Dir / "plain.obj").string() + "\" -o \"" + (Dir / "plain.dxship").string() + "\" --name plain --colour-material Plain" + Common) == 0);
	/* No licence. */
	CHECK(run("\"" + (Dir / "box.obj").string() + "\" -o \"" + (Dir / "nolic.dxship").string() + "\" --name nolic") != 0);
	/* A bad name. */
	CHECK(run("\"" + (Dir / "box.obj").string() + "\" -o \"" + (Dir / "bad.dxship").string() + "\" --name \"Bad Name\"" + Common) != 0);
	/* The other way round: the model faces -z. */
	CHECK(run("\"" + (Dir / "box.obj").string() + "\" -o \"" + (Dir / "rot.dxship").string() + "\" --name rot --forward +x" + Common) == 0);
	if (const auto m{load(Dir / "rot.dxship")})
		CHECK(m->maxs.x - m->mins.x > m->maxs.z - m->mins.z);
	/* A corrupted file fails --check. */
	auto bytes{read_bytes(out)};
	bytes[20] ^= 0x40;
	write_bytes(Dir / "broken.dxship", bytes);
	CHECK(run("--check \"" + (Dir / "broken.dxship").string() + "\"") != 0);
}

void test_limits()
{
	/* 10001 triangles: a strip of thin quads. */
	std::string s{"mtllib box.mtl\nusemtl Accent\n"};
	char line[96];
	const unsigned quads{5001};
	for (unsigned i = 0; i <= quads; ++i)
	{
		std::snprintf(line, sizeof(line), "v %g 0 0\nv %g 1 0\n", i * 0.01, i * 0.01);
		s += line;
	}
	for (unsigned i = 0; i < quads; ++i)
	{
		std::snprintf(line, sizeof(line), "f %u %u %u %u\n", 2 * i + 1, 2 * i + 3, 2 * i + 4, 2 * i + 2);
		s += line;
	}
	write_text(Dir / "many.obj", s);
	CHECK(run("\"" + (Dir / "many.obj").string() + "\" -o \"" + (Dir / "many.dxship").string() + "\" --name many" + Common) != 0);
	CHECK(read_text(Dir / "log.txt").find("triangles") != std::string::npos);
	/* A flat plate much wider than the Pyro seen from above: shrunk. */
	write_text(Dir / "plate.obj", "mtllib box.mtl\nvt 0 0\nvt 1 0\nvt 1 1\nvt 0 1\n" + obj_box(-4, -0.05f, -4, 4, 0.05f, 4, 1, "Accent", "Accent"));
	CHECK(run("\"" + (Dir / "plate.obj").string() + "\" -o \"" + (Dir / "plate.dxship").string() + "\" --name plate" + Common) == 0);
	CHECK(read_text(Dir / "log.txt").find("shrunk") != std::string::npos);
	if (const auto m{load(Dir / "plate.dxship")})
	{
		/* Top view: a square of side a covers a^2 / (2R)^2 <= 1.15 * 0.3973. */
		const auto side{m->maxs.x - m->mins.x};
		CHECK(side * side / (4 * ds::PYRO_RADIUS * ds::PYRO_RADIUS) <= 1.15 * 0.3973 + 0.01);
		CHECK(max_radius(*m) < ds::PYRO_RADIUS);
	}
}

/* glTF: a body, two debris parts, a gun marker, a mirrored node. */
void test_gltf()
{
	/* One cube, 8 vertices, 12 triangles; position accessor then index
	 * accessor in one buffer.
	 */
	std::vector<std::uint8_t> buf;
	const auto f32 = [&buf](const float v) {
		std::uint32_t u;
		std::memcpy(&u, &v, 4);
		for (unsigned i = 0; i < 4; ++i)
			buf.push_back(static_cast<std::uint8_t>(u >> (8 * i)));
	};
	for (unsigned i = 0; i < 8; ++i)
	{
		f32(i & 1 ? 1.0f : -1.0f);
		f32(i & 2 ? 1.0f : -1.0f);
		f32(i & 4 ? 1.0f : -1.0f);
	}
	static constexpr std::array<std::uint16_t, 36> idx{{2, 3, 7, 2, 7, 6, 0, 4, 5, 0, 5, 1, 1, 5, 7, 1, 7, 3, 0, 2, 6, 0, 6, 4, 4, 6, 7, 4, 7, 5, 0, 1, 3, 0, 3, 2}};
	for (const auto i : idx)
	{
		buf.push_back(static_cast<std::uint8_t>(i));
		buf.push_back(static_cast<std::uint8_t>(i >> 8));
	}
	write_bytes(Dir / "cube.bin", buf);
	const std::string gltf{R"({
"asset": {"version": "2.0"},
"scene": 0,
"scenes": [{"nodes": [0]}],
"nodes": [
 {"name": "ship", "children": [1, 2, 3, 4], "mesh": 0, "scale": [2, 0.5, 4]},
 {"name": "debris_wing_left", "mesh": 1, "translation": [-1.5, 0, 0], "scale": [0.5, 0.5, 0.5]},
 {"name": "Debris_Wing_Right", "mesh": 1, "translation": [1.5, 0, 0], "scale": [-0.5, 0.5, 0.5]},
 {"name": "gun0", "translation": [0.5, -0.4, 0.2]},
 {"name": "antenna", "mesh": 1, "translation": [0, 1.2, 0], "scale": [0.1, 0.5, 0.1]}
],
"meshes": [
 {"primitives": [{"attributes": {"POSITION": 0}, "indices": 1, "material": 0}]},
 {"primitives": [{"attributes": {"POSITION": 0}, "indices": 1, "material": 1}]}
],
"materials": [
 {"name": "hull", "pbrMetallicRoughness": {"baseColorFactor": [0.3, 0.3, 0.35, 1]}},
 {"name": "Player Colour", "pbrMetallicRoughness": {"baseColorFactor": [1, 1, 1, 1]}}
],
"accessors": [
 {"bufferView": 0, "componentType": 5126, "count": 8, "type": "VEC3", "min": [-1, -1, -1], "max": [1, 1, 1]},
 {"bufferView": 1, "componentType": 5123, "count": 36, "type": "SCALAR"}
],
"bufferViews": [
 {"buffer": 0, "byteOffset": 0, "byteLength": 96},
 {"buffer": 0, "byteOffset": 96, "byteLength": 72}
],
"buffers": [{"uri": "cube.bin", "byteLength": 168}]
})"};
	write_text(Dir / "ship.gltf", gltf);
	CHECK(run("\"" + (Dir / "ship.gltf").string() + "\" -o \"" + (Dir / "ship.dxship").string() + "\" --name gltf-ship --title \"glTF Ship\"" + Common) == 0);
	if (const auto m{load(Dir / "ship.dxship")})
	{
		CHECK(m->info.title == "glTF Ship");
		/* Body plus two debris parts (the antenna belongs to the body). */
		CHECK(m->parts.size() == 3);
		CHECK(m->indices.size() == 4 * 36);
		CHECK(m->guns[0].present && !m->guns[1].present);
		/* glTF's +x is the ship's left: the gun marker at x +0.5 ends up
		 * on Descent's left (negative x).
		 */
		CHECK(m->guns[0].pos.x < 0);
		bool left{}, right{};
		for (const auto &v : m->vertices)
		{
			if (v.part == 1)
				left = left || v.pos.x > 0;
			if (v.part == 2)
				right = right || v.pos.x < 0;
		}
		CHECK(left && right);
		CHECK(std::ranges::any_of(m->materials, [](const ds::material &mat) { return mat.has_flag(ds::material_flag::tint); }));
	}
	CHECK(run("--check \"" + (Dir / "ship.dxship").string() + "\" --preview \"" + (Dir / "ship.png").string() + "\"") == 0);
	CHECK(fs::exists(Dir / "ship.png"));
}

}

int main(const int argc, char **const argv)
{
	Tool = argc > 1 ? argv[1] : "build/common/shipconv";
	if (!fs::exists(Tool))
	{
		std::fprintf(stderr, "test-shipconv: %s not found (build it with `scons shipconv`)\n", Tool.c_str());
		return 1;
	}
	Tool = fs::absolute(Tool).string();
	Dir = fs::temp_directory_path() / ("test-shipconv-" + std::to_string(std::rand()) + "-" + std::to_string(static_cast<unsigned long>(std::time(nullptr))));
	fs::create_directories(Dir);
	test_obj();
	test_limits();
	test_gltf();
	if (failures)
	{
		std::fprintf(stderr, "test-shipconv: %u failures (files kept in %s)\n", failures, Dir.string().c_str());
		return 1;
	}
	fs::remove_all(Dir);
	std::puts("test-shipconv: all checks passed");
	return 0;
}
