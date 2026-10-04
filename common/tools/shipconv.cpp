/*
 * This file is part of the DXX-Rebirth project <https://github.com/dxx-rebirth/dxx-rebirth/>.
 * It is copyright by its individual contributors, as recorded in the
 * project's Git history.  See COPYING.txt at the top level for license
 * terms and a link to the Git history.
 */
/*
 * shipconv: converts a ship model (glTF 2.0 .gltf/.glb, or Wavefront
 * OBJ) into a custom ship file `.dxship` (Documentation/custom-ships.md
 * and custom-ships-authoring.md), and checks such files.
 *
 *	shipconv model.glb -o ships/viper.dxship --name viper
 *		--title "Viper" --author "..." --licence CC0-1.0 [options]
 *	shipconv --check ships/viper.dxship [--preview viper.png]
 *
 * Steps: load, convert the axes to Descent's (x right, y up, z forward),
 * find the player colour zone, the debris parts and the gun markers,
 * re-centre on the bounding sphere and scale it so that its outline
 * from the front and rear equals the Pyro's (decision D3), scale the
 * textures to powers of two of at most
 * 512, write, and read the result back with the game's reader.
 */

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdarg>
#include <cinttypes>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include "dxship_format.h"
#include "sha256.h"
#include "ship_silhouette.h"

#define CGLTF_IMPLEMENTATION
#include "contrib/cgltf/cgltf.h"
#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_PSD
#define STBI_NO_HDR
#define STBI_NO_PIC
#define STBI_NO_PNM
#define STBI_NO_GIF
#include "contrib/stb/stb_image.h"
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "contrib/stb/stb_image_write.h"

/* MinGW checks plain `printf` formats as Microsoft's (no %zu). */
#if defined(__MINGW32__) && !defined(__clang__)
#define SHIPCONV_FORMAT_PRINTF(A, B)	__attribute__((format(gnu_printf, A, B)))
#else
#define SHIPCONV_FORMAT_PRINTF(A, B)	__attribute__((format(printf, A, B)))
#endif

namespace {

namespace ds = dcx::dxship;
using ds::vec3;

constexpr const char *CONVERTER_VERSION{"shipconv 1"};

namespace ss = dcx::ship_silhouette;
/* The Pyro's gun points (Player_ship->gun_points), for the warnings on
 * gun markers only; the game always fires from its own table.
 */
constexpr std::array<vec3, 8> PYRO_GUNS{{
	{2.23f, -0.91f, 0.55f}, {-2.25f, -0.91f, 0.53f}, {3.39f, -1.81f, 2.26f}, {-3.41f, -1.80f, 2.26f},
	{2.33f, 0.0f, -1.39f}, {-2.39f, 0.0f, -1.39f}, {0.02f, -1.34f, 2.82f}, {-0.02f, -1.34f, -2.91f},
}};
/* Decision D2 and design section 8: the colour zone's share of the
 * surface.
 */
constexpr double ZONE_MIN{0.05};
constexpr double ZONE_WARN{0.10};
/* Grey level the colour zone's texels are normalised to; the game
 * multiplies it with the player's colour.
 */
constexpr float ZONE_GREY{0.85f};

/* vector helpers */

vec3 operator+(const vec3 &a, const vec3 &b)
{
	return {a.x + b.x, a.y + b.y, a.z + b.z};
}

vec3 operator-(const vec3 &a, const vec3 &b)
{
	return {a.x - b.x, a.y - b.y, a.z - b.z};
}

vec3 operator*(const vec3 &a, const float s)
{
	return {a.x * s, a.y * s, a.z * s};
}

float dot(const vec3 &a, const vec3 &b)
{
	return a.x * b.x + a.y * b.y + a.z * b.z;
}

vec3 cross(const vec3 &a, const vec3 &b)
{
	return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}

float length(const vec3 &a)
{
	return std::sqrt(dot(a, a));
}

vec3 normalised(const vec3 &a)
{
	const auto l{length(a)};
	return l > 1e-12f ? a * (1.0f / l) : vec3{0, 0, 1};
}

/* Messages */

unsigned Warnings;

SHIPCONV_FORMAT_PRINTF(1, 2)
void warn(const char *const fmt, ...)
{
	++Warnings;
	std::fputs("shipconv: warning: ", stderr);
	va_list ap;
	va_start(ap, fmt);
	std::vfprintf(stderr, fmt, ap);
	va_end(ap);
	std::fputc('\n', stderr);
}

[[noreturn]] SHIPCONV_FORMAT_PRINTF(1, 2)
void fatal(const char *const fmt, ...)
{
	std::fputs("shipconv: error: ", stderr);
	va_list ap;
	va_start(ap, fmt);
	std::vfprintf(stderr, fmt, ap);
	va_end(ap);
	std::fputc('\n', stderr);
	std::exit(1);
}

std::optional<std::vector<std::uint8_t>> read_file(const std::string &path)
{
	std::ifstream f(path, std::ios::binary);
	if (!f)
		return std::nullopt;
	return std::vector<std::uint8_t>(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
}

bool write_file(const std::string &path, const std::span<const std::uint8_t> data)
{
	std::ofstream f(path, std::ios::binary);
	if (!f)
		return false;
	f.write(reinterpret_cast<const char *>(data.data()), static_cast<std::streamsize>(data.size()));
	return static_cast<bool>(f);
}

std::string lower(std::string s)
{
	for (auto &c : s)
		c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
	return s;
}

std::string directory_of(const std::string &path)
{
	const auto slash{path.find_last_of("/\\")};
	return slash == std::string::npos ? std::string{} : path.substr(0, slash + 1);
}

/* Images */

struct image
{
	unsigned w{}, h{};
	/* RGBA8, row-major, top row first. */
	std::vector<std::uint8_t> rgba;
	std::string name;
	[[nodiscard]]
	std::uint8_t *at(const unsigned x, const unsigned y)
	{
		return &rgba[(std::size_t{y} * w + x) * 4];
	}
	[[nodiscard]]
	const std::uint8_t *at(const unsigned x, const unsigned y) const
	{
		return &rgba[(std::size_t{y} * w + x) * 4];
	}
};

std::optional<image> decode_image(const std::span<const std::uint8_t> bytes, std::string name)
{
	int w, h, comp;
	const auto p{stbi_load_from_memory(bytes.data(), static_cast<int>(bytes.size()), &w, &h, &comp, 4)};
	if (!p)
		return std::nullopt;
	image r;
	r.w = static_cast<unsigned>(w);
	r.h = static_cast<unsigned>(h);
	r.rgba.assign(p, p + std::size_t{r.w} * r.h * 4);
	r.name = std::move(name);
	stbi_image_free(p);
	return r;
}

std::optional<image> load_image_file(const std::string &path)
{
	const auto bytes{read_file(path)};
	if (!bytes)
		return std::nullopt;
	return decode_image(*bytes, path);
}

unsigned floor_power_of_two(const unsigned v)
{
	unsigned r{1};
	while (r * 2 <= v)
		r *= 2;
	return r;
}

/* Area-average resampling of a float plane (any size to any smaller
 * or equal size).
 */
std::vector<float> resample(const std::vector<float> &src, const unsigned sw, const unsigned sh, const unsigned channels, const unsigned dw, const unsigned dh)
{
	std::vector<float> out(std::size_t{dw} * dh * channels);
	for (unsigned y = 0; y < dh; ++y)
	{
		const unsigned y0{y * sh / dh}, y1{std::max(y0 + 1, (y + 1) * sh / dh)};
		for (unsigned x = 0; x < dw; ++x)
		{
			const unsigned x0{x * sw / dw}, x1{std::max(x0 + 1, (x + 1) * sw / dw)};
			for (unsigned c = 0; c < channels; ++c)
			{
				double sum{};
				for (unsigned yy = y0; yy < y1; ++yy)
					for (unsigned xx = x0; xx < x1; ++xx)
						sum += src[(std::size_t{yy} * sw + xx) * channels + c];
				out[(std::size_t{y} * dw + x) * channels + c] = static_cast<float>(sum / ((y1 - y0) * (x1 - x0)));
			}
		}
	}
	return out;
}

std::vector<std::uint8_t> encode_png(const unsigned w, const unsigned h, const unsigned comp, const std::vector<std::uint8_t> &pixels)
{
	std::vector<std::uint8_t> out;
	stbi_write_png_compression_level = 9;
	stbi_write_png_to_func([](void *const ctx, void *const data, const int size) {
		auto &o{*static_cast<std::vector<std::uint8_t> *>(ctx)};
		const auto p{static_cast<const std::uint8_t *>(data)};
		o.insert(o.end(), p, p + size);
	}, &out, static_cast<int>(w), static_cast<int>(h), static_cast<int>(comp), pixels.data(), static_cast<int>(w * comp));
	return out;
}

/* The source scene, in the source's axes until `convert_axes`. */

struct src_vertex
{
	vec3 pos;
	vec3 normal;
	float u{}, v{};
	std::array<float, 4> colour{{1, 1, 1, 1}};
	std::uint8_t part{};
	std::uint8_t tint{};
};

struct src_material
{
	std::string name;
	std::array<float, 4> base{{1, 1, 1, 1}};
	/* Index into `scene::images`, or -1. */
	int image{-1};
	bool double_sided{};
	bool tint{};
};

struct src_triangle
{
	std::array<std::uint32_t, 3> v;
	unsigned material{};
};

struct scene
{
	std::vector<src_vertex> vertices;
	std::vector<src_triangle> triangles;
	std::vector<src_material> materials;
	std::vector<image> images;
	bool has_normals{true};
	std::vector<std::string> part_names;
	std::array<std::optional<vec3>, 8> guns;
};

/* The part of a node: debris_* (decision D6) names a part; children
 * inherit it.
 */
std::uint8_t part_for_name(scene &s, const std::string &name, const std::uint8_t inherited)
{
	const auto l{lower(name)};
	if (l.rfind("debris", 0) != 0)
		return inherited;
	const auto found{std::ranges::find(s.part_names, l)};
	if (found != s.part_names.end())
		return static_cast<std::uint8_t>(1 + (found - s.part_names.begin()) % (ds::MAX_PARTS - 1));
	s.part_names.push_back(l);
	if (s.part_names.size() > ds::MAX_PARTS - 1)
		warn("more than %u debris parts; \"%s\" is merged with another part", ds::MAX_PARTS - 1, name.c_str());
	return static_cast<std::uint8_t>(1 + (s.part_names.size() - 1) % (ds::MAX_PARTS - 1));
}

std::optional<unsigned> gun_marker_index(const std::string &name)
{
	const auto l{lower(name)};
	if (l.size() == 4 && l.rfind("gun", 0) == 0 && l[3] >= '0' && l[3] <= '7')
		return static_cast<unsigned>(l[3] - '0');
	return std::nullopt;
}

/* glTF */

int gltf_image(scene &s, std::map<const cgltf_image *, int> &cache, const cgltf_image *const img, const std::string &dir)
{
	if (!img)
		return -1;
	if (const auto f{cache.find(img)}; f != cache.end())
		return f->second;
	std::optional<image> decoded;
	const std::string name{img->name ? img->name : img->uri ? img->uri : "embedded image"};
	if (img->buffer_view)
	{
		const auto data{cgltf_buffer_view_data(img->buffer_view)};
		if (data)
			decoded = decode_image({data, img->buffer_view->size}, name);
	}
	else if (img->uri)
	{
		const std::string uri{img->uri};
		if (uri.rfind("data:", 0) == 0)
		{
			const auto comma{uri.find(',')};
			if (comma != std::string::npos && uri.find(";base64") < comma)
			{
				const auto b64{uri.substr(comma + 1)};
				std::size_t padding{};
				for (auto i{b64.size()}; i > 0 && b64[i - 1] == '='; --i)
					++padding;
				const auto size{b64.size() / 4 * 3 - padding};
				void *out{};
				cgltf_options options{};
				if (cgltf_load_buffer_base64(&options, size, b64.c_str(), &out) == cgltf_result_success)
				{
					decoded = decode_image({static_cast<const std::uint8_t *>(out), size}, name);
					std::free(out);
				}
			}
		}
		else
		{
			std::string path{uri};
			cgltf_decode_uri(path.data());
			path.resize(std::strlen(path.c_str()));
			decoded = load_image_file(dir + path);
		}
	}
	if (!decoded)
		fatal("cannot read the texture \"%s\"", name.c_str());
	s.images.push_back(std::move(*decoded));
	const int index{static_cast<int>(s.images.size() - 1)};
	cache[img] = index;
	return index;
}

void gltf_primitive(scene &s, const cgltf_primitive &prim, const std::array<float, 16> &world, const unsigned material, const std::uint8_t part)
{
	if (prim.type != cgltf_primitive_type_triangles && prim.type != cgltf_primitive_type_triangle_strip && prim.type != cgltf_primitive_type_triangle_fan)
	{
		warn("a primitive that is not made of triangles is skipped");
		return;
	}
	const cgltf_accessor *pos{}, *nrm{}, *uv{}, *col{};
	for (cgltf_size i = 0; i < prim.attributes_count; ++i)
	{
		const auto &a{prim.attributes[i]};
		switch (a.type)
		{
			case cgltf_attribute_type_position:
				pos = a.data;
				break;
			case cgltf_attribute_type_normal:
				nrm = a.data;
				break;
			case cgltf_attribute_type_texcoord:
				if (a.index == 0)
					uv = a.data;
				break;
			case cgltf_attribute_type_color:
				if (a.index == 0)
					col = a.data;
				break;
			default:
				break;
		}
	}
	if (!pos)
	{
		warn("a primitive without positions is skipped");
		return;
	}
	if (!nrm)
		s.has_normals = false;
	const auto transform = [&world](const vec3 &p, const float w) -> vec3 {
		return {
			world[0] * p.x + world[4] * p.y + world[8] * p.z + world[12] * w,
			world[1] * p.x + world[5] * p.y + world[9] * p.z + world[13] * w,
			world[2] * p.x + world[6] * p.y + world[10] * p.z + world[14] * w,
		};
	};
	/* Normals go through the inverse transpose; for the rotations and
	 * uniform scales of a model the matrix itself, renormalised, is
	 * enough, but a mirroring (negative determinant) flips the winding.
	 */
	const auto det{
		world[0] * (world[5] * world[10] - world[9] * world[6]) -
		world[4] * (world[1] * world[10] - world[9] * world[2]) +
		world[8] * (world[1] * world[6] - world[5] * world[2])};
	const auto base{static_cast<std::uint32_t>(s.vertices.size())};
	for (cgltf_size i = 0; i < pos->count; ++i)
	{
		src_vertex v;
		std::array<float, 4> f{};
		cgltf_accessor_read_float(pos, i, f.data(), 3);
		v.pos = transform({f[0], f[1], f[2]}, 1);
		if (nrm)
		{
			cgltf_accessor_read_float(nrm, i, f.data(), 3);
			v.normal = normalised(transform({f[0], f[1], f[2]}, 0));
		}
		if (uv)
		{
			cgltf_accessor_read_float(uv, i, f.data(), 2);
			v.u = f[0];
			v.v = f[1];
		}
		if (col)
		{
			f = {{1, 1, 1, 1}};
			cgltf_accessor_read_float(col, i, f.data(), cgltf_num_components(col->type));
			v.colour = f;
		}
		v.part = part;
		s.vertices.push_back(v);
	}
	std::vector<std::uint32_t> idx;
	if (prim.indices)
		for (cgltf_size i = 0; i < prim.indices->count; ++i)
			idx.push_back(base + static_cast<std::uint32_t>(cgltf_accessor_read_index(prim.indices, i)));
	else
		for (cgltf_size i = 0; i < pos->count; ++i)
			idx.push_back(base + static_cast<std::uint32_t>(i));
	const auto add = [&](std::uint32_t a, std::uint32_t b, std::uint32_t c) {
		if (det < 0)
			std::swap(b, c);
		s.triangles.push_back({{{a, b, c}}, material});
	};
	if (prim.type == cgltf_primitive_type_triangles)
		for (std::size_t i = 0; i + 2 < idx.size(); i += 3)
			add(idx[i], idx[i + 1], idx[i + 2]);
	else if (prim.type == cgltf_primitive_type_triangle_strip)
		for (std::size_t i = 0; i + 2 < idx.size(); ++i)
			if (i & 1)
				add(idx[i + 1], idx[i], idx[i + 2]);
			else
				add(idx[i], idx[i + 1], idx[i + 2]);
	else
		for (std::size_t i = 1; i + 1 < idx.size(); ++i)
			add(idx[0], idx[i], idx[i + 1]);
}

void gltf_node(scene &s, std::map<const cgltf_material *, unsigned> &materials, std::map<const cgltf_image *, int> &images, const cgltf_node &node, const std::string &dir, const std::uint8_t inherited_part)
{
	const std::string name{node.name ? node.name : ""};
	std::array<float, 16> world;
	cgltf_node_transform_world(&node, world.data());
	if (const auto g{gun_marker_index(name)})
	{
		s.guns[*g] = vec3{world[12], world[13], world[14]};
		return;
	}
	const auto part{part_for_name(s, name, inherited_part)};
	if (node.mesh)
	{
		if (node.skin)
			warn("node \"%s\" is skinned; its rest pose is used", name.c_str());
		for (cgltf_size i = 0; i < node.mesh->primitives_count; ++i)
		{
			const auto &prim{node.mesh->primitives[i]};
			const auto key{prim.material};
			auto found{materials.find(key)};
			if (found == materials.end())
			{
				src_material m;
				if (key)
				{
					m.name = key->name ? key->name : "";
					m.double_sided = key->double_sided;
					if (key->has_pbr_metallic_roughness)
					{
						const auto &pbr{key->pbr_metallic_roughness};
						std::copy_n(pbr.base_color_factor, 4, m.base.begin());
						if (pbr.base_color_texture.texture)
						{
							if (pbr.base_color_texture.texcoord)
								warn("material \"%s\" uses texture coordinate set %d; set 0 is used", m.name.c_str(), pbr.base_color_texture.texcoord);
							if (pbr.base_color_texture.has_transform)
								warn("material \"%s\" has a texture transform, which is ignored", m.name.c_str());
							m.image = gltf_image(s, images, pbr.base_color_texture.texture->image, dir);
						}
					}
					if (key->alpha_mode != cgltf_alpha_mode_opaque)
						warn("material \"%s\" is drawn opaque", m.name.c_str());
				}
				s.materials.push_back(m);
				found = materials.emplace(key, static_cast<unsigned>(s.materials.size() - 1)).first;
			}
			gltf_primitive(s, prim, world, found->second, part);
		}
	}
	for (cgltf_size i = 0; i < node.children_count; ++i)
		gltf_node(s, materials, images, *node.children[i], dir, part);
}

scene load_gltf(const std::string &path)
{
	cgltf_options options{};
	cgltf_data *data{};
	if (cgltf_parse_file(&options, path.c_str(), &data) != cgltf_result_success)
		fatal("cannot parse the glTF file %s", path.c_str());
	if (cgltf_load_buffers(&options, data, path.c_str()) != cgltf_result_success)
	{
		cgltf_free(data);
		fatal("cannot load the buffers of %s", path.c_str());
	}
	if (cgltf_validate(data) != cgltf_result_success)
	{
		cgltf_free(data);
		fatal("%s is not a valid glTF file", path.c_str());
	}
	scene s;
	std::map<const cgltf_material *, unsigned> materials;
	std::map<const cgltf_image *, int> images;
	const auto dir{directory_of(path)};
	if (data->scene || data->scenes_count)
	{
		const auto &sc{data->scene ? *data->scene : data->scenes[0]};
		for (cgltf_size i = 0; i < sc.nodes_count; ++i)
			gltf_node(s, materials, images, *sc.nodes[i], dir, 0);
	}
	else
		for (cgltf_size i = 0; i < data->nodes_count; ++i)
			if (!data->nodes[i].parent)
				gltf_node(s, materials, images, data->nodes[i], dir, 0);
	cgltf_free(data);
	return s;
}

/* Wavefront OBJ (with MTL: Kd, map_Kd; vertex colours after the
 * position; `o` and `g` names for debris parts and gun markers).
 */

std::map<std::string, src_material> load_mtl(scene &s, const std::string &path, std::map<std::string, int> &image_cache)
{
	std::map<std::string, src_material> out;
	std::ifstream f(path);
	if (!f)
	{
		warn("cannot read the material library %s", path.c_str());
		return out;
	}
	const auto dir{directory_of(path)};
	std::string line;
	src_material *cur{};
	while (std::getline(f, line))
	{
		std::istringstream ls(line);
		std::string k;
		ls >> k;
		if (k == "newmtl")
		{
			std::string n;
			std::getline(ls >> std::ws, n);
			cur = &out[n];
			cur->name = n;
		}
		else if (!cur)
			continue;
		else if (k == "Kd")
			ls >> cur->base[0] >> cur->base[1] >> cur->base[2];
		else if (k == "map_Kd")
		{
			std::string rest, file;
			std::getline(ls >> std::ws, rest);
			/* The file name is the last word (options come first). */
			const auto sp{rest.find_last_of(' ')};
			file = sp == std::string::npos ? rest : rest.substr(sp + 1);
			while (!file.empty() && (file.back() == '\r' || file.back() == ' '))
				file.pop_back();
			const auto full{dir + file};
			if (const auto c{image_cache.find(full)}; c != image_cache.end())
				cur->image = c->second;
			else
			{
				auto img{load_image_file(full)};
				if (!img)
					fatal("cannot read the texture %s", full.c_str());
				s.images.push_back(std::move(*img));
				cur->image = static_cast<int>(s.images.size() - 1);
				image_cache[full] = cur->image;
			}
		}
	}
	return out;
}

scene load_obj(const std::string &path)
{
	std::ifstream f(path);
	if (!f)
		fatal("cannot read %s", path.c_str());
	scene s;
	std::vector<vec3> pos, nrm;
	std::vector<std::array<float, 4>> col;
	std::vector<std::array<float, 2>> tex;
	std::map<std::string, src_material> library;
	std::map<std::string, int> image_cache;
	std::map<std::string, unsigned> material_index;
	unsigned current_material{};
	bool have_material{};
	std::uint8_t part{}, object_part{};
	std::optional<unsigned> gun;
	std::map<std::array<long, 4>, std::uint32_t> corner_cache;
	const auto use_material = [&](const std::string &name) {
		if (const auto f{material_index.find(name)}; f != material_index.end())
		{
			current_material = f->second;
			return;
		}
		auto m{library.contains(name) ? library[name] : src_material{}};
		m.name = name;
		s.materials.push_back(m);
		current_material = material_index[name] = static_cast<unsigned>(s.materials.size() - 1);
	};
	std::string line;
	while (std::getline(f, line))
	{
		if (!line.empty() && line.back() == '\r')
			line.pop_back();
		std::istringstream ls(line);
		std::string k;
		ls >> k;
		if (k == "v")
		{
			vec3 p;
			ls >> p.x >> p.y >> p.z;
			pos.push_back(p);
			std::array<float, 4> c{{1, 1, 1, 1}};
			if (ls >> c[0] >> c[1] >> c[2])
				col.push_back(c);
			else
				col.push_back({{1, 1, 1, 1}});
			if (gun)
				s.guns[*gun] = p;
		}
		else if (k == "vn")
		{
			vec3 n;
			ls >> n.x >> n.y >> n.z;
			nrm.push_back(normalised(n));
		}
		else if (k == "vt")
		{
			std::array<float, 2> t{};
			ls >> t[0] >> t[1];
			tex.push_back(t);
		}
		else if (k == "mtllib")
		{
			std::string n;
			std::getline(ls >> std::ws, n);
			for (auto &[name, m] : load_mtl(s, directory_of(path) + n, image_cache))
				library[name] = m;
		}
		else if (k == "usemtl")
		{
			std::string n;
			std::getline(ls >> std::ws, n);
			use_material(n);
			have_material = true;
		}
		else if (k == "o" || k == "g")
		{
			/* An object's debris part holds for its groups, unless a
			 * group names a part of its own.
			 */
			std::string n;
			std::getline(ls >> std::ws, n);
			gun = gun_marker_index(n);
			if (k == "o")
				part = object_part = part_for_name(s, n, 0);
			else
				part = part_for_name(s, n, object_part);
		}
		else if (k == "f" && !gun)
		{
			if (!have_material)
			{
				use_material("");
				have_material = true;
			}
			std::vector<std::uint32_t> corners;
			std::string c;
			while (ls >> c)
			{
				std::array<long, 3> ix{{0, 0, 0}};
				std::size_t start{};
				for (unsigned j = 0; j < 3 && start <= c.size(); ++j)
				{
					const auto slash{c.find('/', start)};
					const auto field{c.substr(start, slash == std::string::npos ? std::string::npos : slash - start)};
					if (!field.empty())
						ix[j] = std::strtol(field.c_str(), nullptr, 10);
					if (slash == std::string::npos)
						break;
					start = slash + 1;
				}
				const auto resolve = [](const long i, const std::size_t n) -> long {
					return i < 0 ? static_cast<long>(n) + i : i - 1;
				};
				const auto pi{resolve(ix[0], pos.size())}, ti{ix[1] ? resolve(ix[1], tex.size()) : -1}, ni{ix[2] ? resolve(ix[2], nrm.size()) : -1};
				if (pi < 0 || static_cast<std::size_t>(pi) >= pos.size() || (ti >= 0 && static_cast<std::size_t>(ti) >= tex.size()) || (ni >= 0 && static_cast<std::size_t>(ni) >= nrm.size()))
					fatal("%s: face index out of range", path.c_str());
				const std::array<long, 4> key{{pi, ti, ni, static_cast<long>(current_material) * 16 + part}};
				auto found{corner_cache.find(key)};
				if (found == corner_cache.end())
				{
					src_vertex v;
					v.pos = pos[static_cast<std::size_t>(pi)];
					v.colour = col[static_cast<std::size_t>(pi)];
					if (ti >= 0)
					{
						v.u = tex[static_cast<std::size_t>(ti)][0];
						/* OBJ's v runs upwards, glTF's downwards. */
						v.v = 1.0f - tex[static_cast<std::size_t>(ti)][1];
					}
					if (ni >= 0)
						v.normal = nrm[static_cast<std::size_t>(ni)];
					else
						s.has_normals = false;
					v.part = part;
					s.vertices.push_back(v);
					found = corner_cache.emplace(key, static_cast<std::uint32_t>(s.vertices.size() - 1)).first;
				}
				corners.push_back(found->second);
			}
			for (std::size_t i = 1; i + 1 < corners.size(); ++i)
				s.triangles.push_back({{{corners[0], corners[i], corners[i + 1]}}, current_material});
		}
	}
	return s;
}

/* Conversion steps */

struct options
{
	std::string input, output, preview;
	ds::manifest info;
	unsigned texture_size{ds::MAX_TEXTURE_SIZE};
	std::string forward{"+z"}, up{"+y"};
	std::vector<std::string> colour_materials;
	std::optional<std::array<int, 4>> colour_key;
	std::string colour_variant, colour_mask;
	bool auto_debris{true};
	bool check_only{};
};

vec3 axis_vector(const std::string &a)
{
	if (a.size() != 2 || (a[0] != '+' && a[0] != '-'))
		fatal("bad axis \"%s\" (use +x, -x, +y, -y, +z or -z)", a.c_str());
	const float s{a[0] == '-' ? -1.0f : 1.0f};
	switch (a[1])
	{
		case 'x':
			return {s, 0, 0};
		case 'y':
			return {0, s, 0};
		case 'z':
			return {0, 0, s};
		default:
			fatal("bad axis \"%s\"", a.c_str());
	}
}

/* From the source's right-handed axes (glTF: +y up, the model facing +z)
 * to Descent's (x right, y up, z forward).  Seen from behind the ship,
 * right is forward x up in a right-handed system.
 */
void convert_axes(scene &s, const options &opt)
{
	const auto f{axis_vector(opt.forward)}, u{axis_vector(opt.up)};
	if (std::fabs(dot(f, u)) > 0.5f)
		fatal("the forward and up axes must differ");
	const auto r{cross(f, u)};
	const auto map = [&](const vec3 &p) -> vec3 {
		return {dot(p, r), dot(p, u), dot(p, f)};
	};
	for (auto &v : s.vertices)
	{
		v.pos = map(v.pos);
		v.normal = map(v.normal);
	}
	for (auto &g : s.guns)
		if (g)
			g = map(*g);
	/* The format's winding: cross(b - a, c - a) points out of the ship.
	 * The source's does in its own axes; a mapping that mirrors (always,
	 * from a right-handed source to Descent's left-handed axes) reverses
	 * it.
	 */
	if (dot(cross(r, u), f) < 0)
		for (auto &t : s.triangles)
			std::swap(t.v[1], t.v[2]);
}

void compute_normals(scene &s)
{
	std::vector<vec3> acc(s.vertices.size());
	for (const auto &t : s.triangles)
	{
		const auto &a{s.vertices[t.v[0]].pos}, &b{s.vertices[t.v[1]].pos}, &c{s.vertices[t.v[2]].pos};
		const auto n{cross(b - a, c - a)};
		for (const auto i : t.v)
			acc[i] = acc[i] + n;
	}
	for (std::size_t i = 0; i < s.vertices.size(); ++i)
		s.vertices[i].normal = normalised(acc[i]);
}

struct sphere
{
	vec3 centre;
	float radius{};
};

/* Ritter's bounding sphere, compared with the box's; the smaller wins. */
sphere bounding_sphere(const std::vector<vec3> &pts)
{
	const auto farthest = [&pts](const vec3 &from) {
		const vec3 *best{&pts[0]};
		float d{-1};
		for (const auto &p : pts)
			if (const auto l{length(p - from)}; l > d)
			{
				d = l;
				best = &p;
			}
		return *best;
	};
	const auto y{farthest(pts[0])}, z{farthest(y)};
	sphere s{(y + z) * 0.5f, length(z - y) * 0.5f};
	for (const auto &p : pts)
		if (const auto d{length(p - s.centre)}; d > s.radius)
		{
			const auto r{(s.radius + d) * 0.5f};
			s.centre = s.centre + (p - s.centre) * ((r - s.radius) / d);
			s.radius = r;
		}
	vec3 lo{pts[0]}, hi{pts[0]};
	for (const auto &p : pts)
	{
		lo = {std::min(lo.x, p.x), std::min(lo.y, p.y), std::min(lo.z, p.z)};
		hi = {std::max(hi.x, p.x), std::max(hi.y, p.y), std::max(hi.z, p.z)};
	}
	sphere b{(lo + hi) * 0.5f, 0};
	for (const auto &p : pts)
		b.radius = std::max(b.radius, length(p - b.centre));
	return b.radius < s.radius ? b : s;
}

/* Colour zone masks: one float plane per image, at the image's size. */
using mask_plane = std::vector<float>;

bool is_zone_colour(const std::array<float, 4> &c)
{
	return c[0] > 0.9f && c[1] < 0.1f && c[2] > 0.9f;
}

/* Mark the texels a triangle covers in UV space (wrapped), with a margin
 * of `dilate` texels.
 */
void rasterise_uv(mask_plane &m, const image &img, const std::array<std::array<float, 2>, 3> &uv, const int dilate)
{
	const float w{static_cast<float>(img.w)}, h{static_cast<float>(img.h)};
	std::array<std::array<float, 2>, 3> p;
	/* Shift the triangle so that its first corner lies in [0, 1). */
	const float su{std::floor(uv[0][0])}, sv{std::floor(uv[0][1])};
	for (unsigned i = 0; i < 3; ++i)
		p[i] = {{(uv[i][0] - su) * w, (uv[i][1] - sv) * h}};
	const auto d{(p[1][0] - p[0][0]) * (p[2][1] - p[0][1]) - (p[2][0] - p[0][0]) * (p[1][1] - p[0][1])};
	const int x0{static_cast<int>(std::floor(std::min({p[0][0], p[1][0], p[2][0]}))) - dilate}, x1{static_cast<int>(std::ceil(std::max({p[0][0], p[1][0], p[2][0]}))) + dilate};
	const int y0{static_cast<int>(std::floor(std::min({p[0][1], p[1][1], p[2][1]}))) - dilate}, y1{static_cast<int>(std::ceil(std::max({p[0][1], p[1][1], p[2][1]}))) + dilate};
	if (static_cast<long>(x1 - x0) * (y1 - y0) > 4l * img.w * img.h)
		return;
	const auto inside = [&](const float px, const float py) {
		if (std::fabs(d) < 1e-9f)
			return false;
		const auto w0{((p[1][0] - px) * (p[2][1] - py) - (p[2][0] - px) * (p[1][1] - py)) / d};
		const auto w1{((p[2][0] - px) * (p[0][1] - py) - (p[0][0] - px) * (p[2][1] - py)) / d};
		return w0 >= 0 && w1 >= 0 && 1 - w0 - w1 >= 0;
	};
	for (int y = y0; y <= y1; ++y)
		for (int x = x0; x <= x1; ++x)
		{
			bool hit{inside(static_cast<float>(x) + 0.5f, static_cast<float>(y) + 0.5f)};
			for (int dy = -dilate; !hit && dy <= dilate; ++dy)
				for (int dx = -dilate; !hit && dx <= dilate; ++dx)
					hit = inside(static_cast<float>(x + dx) + 0.5f, static_cast<float>(y + dy) + 0.5f);
			/* Degenerate (a line or a point in UV space): its texels. */
			if (!hit && std::fabs(d) < 1e-9f)
				for (const auto &c : p)
					if (std::floor(c[0]) == static_cast<float>(x) && std::floor(c[1]) == static_cast<float>(y))
						hit = true;
			if (!hit)
				continue;
			const auto wx{static_cast<unsigned>(((x % static_cast<int>(img.w)) + static_cast<int>(img.w)) % static_cast<int>(img.w))};
			const auto wy{static_cast<unsigned>(((y % static_cast<int>(img.h)) + static_cast<int>(img.h)) % static_cast<int>(img.h))};
			m[std::size_t{wy} * img.w + wx] = 1;
		}
}

struct zone_result
{
	std::vector<mask_plane> masks;
	std::vector<bool> has_mask;
};

zone_result find_colour_zone(scene &s, const options &opt)
{
	zone_result z;
	z.masks.resize(s.images.size());
	z.has_mask.assign(s.images.size(), false);
	for (std::size_t i = 0; i < s.images.size(); ++i)
		z.masks[i].assign(std::size_t{s.images[i].w} * s.images[i].h, 0);
	/* Materials named as the zone. */
	for (auto &m : s.materials)
	{
		const auto l{lower(m.name)};
		const bool named{l.find("accent") != std::string::npos || l.find("player") != std::string::npos || l.find("colour") != std::string::npos || l.find("color") != std::string::npos};
		const bool chosen{std::ranges::find(opt.colour_materials, m.name) != opt.colour_materials.end()};
		if (named || chosen)
		{
			m.tint = true;
			std::fprintf(stderr, "shipconv: material \"%s\" is the player colour zone\n", m.name.c_str());
		}
	}
	for (const auto &name : opt.colour_materials)
		if (std::ranges::find(s.materials, name, &src_material::name) == s.materials.end())
			fatal("no material named \"%s\"", name.c_str());
	/* Textured zone materials and zone-coloured vertices become texel
	 * masks; untextured ones keep the material flag or the vertex tint.
	 */
	for (const auto &t : s.triangles)
	{
		auto &mat{s.materials[t.material]};
		const bool vertex_zone{std::ranges::all_of(t.v, [&](const std::uint32_t i) { return is_zone_colour(s.vertices[i].colour); })};
		if (mat.image >= 0 && (mat.tint || vertex_zone))
		{
			const auto ii{static_cast<std::size_t>(mat.image)};
			std::array<std::array<float, 2>, 3> uv;
			for (unsigned k = 0; k < 3; ++k)
				uv[k] = {{s.vertices[t.v[k]].u, s.vertices[t.v[k]].v}};
			rasterise_uv(z.masks[ii], s.images[ii], uv, 2);
			z.has_mask[ii] = true;
		}
		else if (vertex_zone)
			for (const auto i : t.v)
				s.vertices[i].tint = 255;
	}
	for (auto &m : s.materials)
		if (m.image >= 0 && m.tint)
			m.tint = false;	/* now a texel mask */
	for (auto &v : s.vertices)
		if (is_zone_colour(v.colour))
			v.colour = {{ZONE_GREY, ZONE_GREY, ZONE_GREY, 1}};
	/* Options that pick texels. */
	std::optional<image> variant, explicit_mask;
	if (!opt.colour_variant.empty() && !(variant = load_image_file(opt.colour_variant)))
		fatal("cannot read %s", opt.colour_variant.c_str());
	if (!opt.colour_mask.empty() && !(explicit_mask = load_image_file(opt.colour_mask)))
		fatal("cannot read %s", opt.colour_mask.c_str());
	for (std::size_t i = 0; i < s.images.size(); ++i)
	{
		const auto &img{s.images[i]};
		auto &mask{z.masks[i]};
		const auto sample = [](const image &o, const unsigned x, const unsigned y, const image &like) {
			return o.at(x * o.w / like.w, y * o.h / like.h);
		};
		if (opt.colour_key || variant || explicit_mask)
			z.has_mask[i] = true;
		for (unsigned y = 0; y < img.h; ++y)
			for (unsigned x = 0; x < img.w; ++x)
			{
				const auto p{img.at(x, y)};
				auto &m{mask[std::size_t{y} * img.w + x]};
				if (opt.colour_key)
				{
					const auto &k{*opt.colour_key};
					if (std::abs(p[0] - k[0]) <= k[3] && std::abs(p[1] - k[1]) <= k[3] && std::abs(p[2] - k[2]) <= k[3])
						m = 1;
				}
				if (variant)
				{
					const auto q{sample(*variant, x, y, img)};
					if (std::abs(p[0] - q[0]) + std::abs(p[1] - q[1]) + std::abs(p[2] - q[2]) > 24)
						m = 1;
				}
				if (explicit_mask)
					m = std::max(m, static_cast<float>(sample(*explicit_mask, x, y, img)[0]) / 255.0f);
			}
	}
	return z;
}

/* Grey the zone texels, keeping their relative brightness, so that the
 * game's multiplication with the player's colour shows that colour.
 */
void grey_zone(image &img, const mask_plane &mask)
{
	double sum{}, weight{};
	for (std::size_t i = 0; i < mask.size(); ++i)
		if (mask[i] > 0)
		{
			const auto p{&img.rgba[i * 4]};
			sum += mask[i] * (0.299 * p[0] + 0.587 * p[1] + 0.114 * p[2]) / 255.0;
			weight += mask[i];
		}
	if (weight <= 0)
		return;
	const auto mean{std::max(sum / weight, 0.05)};
	for (std::size_t i = 0; i < mask.size(); ++i)
		if (const auto m{mask[i]}; m > 0)
		{
			const auto p{&img.rgba[i * 4]};
			const auto l{(0.299 * p[0] + 0.587 * p[1] + 0.114 * p[2]) / 255.0};
			const auto grey{std::clamp(ZONE_GREY * l / mean, 0.0, 1.0) * 255.0};
			for (unsigned c = 0; c < 3; ++c)
				p[c] = static_cast<std::uint8_t>(std::lround(p[c] * (1 - m) + grey * m));
		}
}

/* The automatic split (decision D6, fallback B): the outer thirds of the
 * span, left and right; if that leaves a side nearly empty, front and
 * back instead.
 */
unsigned automatic_parts(scene &s, const float radius)
{
	const auto centroid = [&s](const src_triangle &t) {
		return (s.vertices[t.v[0]].pos + s.vertices[t.v[1]].pos + s.vertices[t.v[2]].pos) * (1.0f / 3.0f);
	};
	const auto count = [&](auto &&pred) {
		return static_cast<std::size_t>(std::ranges::count_if(s.triangles, [&](const src_triangle &t) { return pred(centroid(t)); }));
	};
	const float cut{radius / 3};
	const auto n{s.triangles.size()};
	const auto min_part{std::max<std::size_t>(n / 20, 1)};
	std::function<std::uint8_t(const vec3 &)> assign;
	if (count([&](const vec3 &c) { return c.x < -cut; }) >= min_part && count([&](const vec3 &c) { return c.x > cut; }) >= min_part)
		assign = [cut](const vec3 &c) -> std::uint8_t { return c.x < -cut ? 1 : c.x > cut ? 2 : 0; };
	else if (count([&](const vec3 &c) { return c.z < -cut; }) >= min_part && count([&](const vec3 &c) { return c.z > cut; }) >= min_part)
		assign = [cut](const vec3 &c) -> std::uint8_t { return c.z > cut ? 1 : c.z < -cut ? 2 : 0; };
	else
		return 0;
	/* A vertex shared across the cut is duplicated per part. */
	std::map<std::pair<std::uint32_t, std::uint8_t>, std::uint32_t> copies;
	for (auto &t : s.triangles)
	{
		const auto p{assign(centroid(t))};
		for (auto &i : t.v)
		{
			if (s.vertices[i].part == p)
			{
				if (!copies.contains({i, p}))
					copies[{i, p}] = i;
				continue;
			}
			const auto key{std::make_pair(i, p)};
			auto f{copies.find(key)};
			if (f == copies.end())
			{
				auto v{s.vertices[i]};
				v.part = p;
				s.vertices.push_back(v);
				f = copies.emplace(key, static_cast<std::uint32_t>(s.vertices.size() - 1)).first;
			}
			i = f->second;
		}
	}
	return 2;
}

/* Preview: three views, the colour zone in red, lit like the game. */

void render_preview(const ds::model &m, const std::vector<image> &albedo, const std::vector<image> &masks, const std::string &path)
{
	constexpr unsigned S{320};
	image out;
	out.w = S * 3;
	out.h = S;
	out.rgba.assign(std::size_t{out.w} * out.h * 4, 0);
	for (std::size_t i = 0; i < out.rgba.size(); i += 4)
	{
		out.rgba[i] = out.rgba[i + 1] = out.rgba[i + 2] = 40;
		out.rgba[i + 3] = 255;
	}
	constexpr std::array<float, 3> zone_colour{{0.95f, 0.25f, 0.2f}};
	/* The whole ship in view: it may reach beyond the Pyro's radius (D3). */
	float extent{ds::PYRO_RADIUS};
	for (const auto &v : m.vertices)
		extent = std::max(extent, length(v.pos));
	/* View directions: from front-right-above, from the side, from above. */
	const std::array<std::array<vec3, 3>, 3> views{{
		{{normalised({-0.8f, 0, 0.6f}), normalised({-0.15f, 0.9f, -0.2f}), normalised({-0.55f, -0.45f, -0.7f})}},
		{{{0, 0, -1}, {0, 1, 0}, {-1, 0, 0}}},
		{{{1, 0, 0}, {0, 0, 1}, {0, -1, 0}}},
	}};
	for (unsigned view = 0; view < 3; ++view)
	{
		vec3 right{views[view][0]}, up{views[view][1]};
		up = normalised(up - right * dot(up, right));
		const auto fwd{cross(right, up)};
		std::vector<float> depth(S * S, 1e30f);
		const float scale{static_cast<float>(S) * 0.45f / extent};
		struct pv
		{
			float x, y, z;
		};
		std::vector<pv> proj(m.vertices.size());
		for (std::size_t i = 0; i < m.vertices.size(); ++i)
		{
			const auto &p{m.vertices[i].pos};
			proj[i] = {static_cast<float>(S) / 2 + dot(p, right) * scale, static_cast<float>(S) / 2 - dot(p, up) * scale, dot(p, fwd)};
		}
		for (const auto &mat : m.materials)
			for (std::uint32_t k = mat.first_index; k < mat.first_index + mat.index_count; k += 3)
			{
				const std::array<std::uint16_t, 3> t{{m.indices[k], m.indices[k + 1], m.indices[k + 2]}};
				const auto &a{proj[t[0]]}, &b{proj[t[1]]}, &c{proj[t[2]]};
				const auto d{(b.x - a.x) * (c.y - a.y) - (c.x - a.x) * (b.y - a.y)};
				if (std::fabs(d) < 1e-9f)
					continue;
				const int x0{std::max(0, static_cast<int>(std::floor(std::min({a.x, b.x, c.x}))))}, x1{std::min(static_cast<int>(S) - 1, static_cast<int>(std::ceil(std::max({a.x, b.x, c.x}))))};
				const int y0{std::max(0, static_cast<int>(std::floor(std::min({a.y, b.y, c.y}))))}, y1{std::min(static_cast<int>(S) - 1, static_cast<int>(std::ceil(std::max({a.y, b.y, c.y}))))};
				for (int y = y0; y <= y1; ++y)
					for (int x = x0; x <= x1; ++x)
					{
						const float px{static_cast<float>(x) + 0.5f}, py{static_cast<float>(y) + 0.5f};
						const auto w0{((b.x - px) * (c.y - py) - (c.x - px) * (b.y - py)) / d};
						const auto w1{((c.x - px) * (a.y - py) - (a.x - px) * (c.y - py)) / d};
						const auto w2{1 - w0 - w1};
						if (w0 < 0 || w1 < 0 || w2 < 0)
							continue;
						const auto z{w0 * a.z + w1 * b.z + w2 * c.z};
						auto &dz{depth[static_cast<std::size_t>(y) * S + static_cast<std::size_t>(x)]};
						if (z >= dz)
							continue;
						dz = z;
						const auto &va{m.vertices[t[0]]}, &vb{m.vertices[t[1]]}, &vc{m.vertices[t[2]]};
						const auto nv = [](const ds::vertex &v) {
							return vec3{ds::snorm16_to_float(v.normal[0]), ds::snorm16_to_float(v.normal[1]), ds::snorm16_to_float(v.normal[2])};
						};
						const auto n{normalised(nv(va) * w0 + nv(vb) * w1 + nv(vc) * w2)};
						/* The game's model light: 1/4 ambient, 3/4 facing the viewer. */
						const auto light{0.25f + 0.75f * std::fabs(dot(n, fwd))};
						std::array<float, 3> colour{{1, 1, 1}};
						float tint{(va.tint * w0 + vb.tint * w1 + vc.tint * w2) / 255.0f};
						if (mat.texture != ds::NO_TEXTURE)
						{
							const auto &img{albedo[mat.texture]};
							const auto u{va.u * w0 + vb.u * w1 + vc.u * w2}, v{va.v * w0 + vb.v * w1 + vc.v * w2};
							const auto tx{static_cast<unsigned>(static_cast<long>(std::floor((u - std::floor(u)) * static_cast<float>(img.w))) % static_cast<long>(img.w))};
							const auto ty{static_cast<unsigned>(static_cast<long>(std::floor((v - std::floor(v)) * static_cast<float>(img.h))) % static_cast<long>(img.h))};
							const auto p{img.at(tx, ty)};
							for (unsigned ch = 0; ch < 3; ++ch)
								colour[ch] = p[ch] / 255.0f;
							if (mat.mask != ds::NO_TEXTURE)
								tint = std::max(tint, masks[mat.mask].at(tx, ty)[0] / 255.0f);
						}
						if (mat.has_flag(ds::material_flag::tint))
							tint = 1;
						for (unsigned ch = 0; ch < 3; ++ch)
						{
							const auto vcol{(va.colour[ch] * w0 + vb.colour[ch] * w1 + vc.colour[ch] * w2) / 255.0f};
							const auto c{colour[ch] * vcol * (1 - tint + tint * zone_colour[ch]) * light};
							out.at(view * S + static_cast<unsigned>(x), static_cast<unsigned>(y))[ch] = static_cast<std::uint8_t>(std::clamp(c * 255.0f, 0.0f, 255.0f));
						}
					}
			}
	}
	if (!write_file(path, encode_png(out.w, out.h, 4, out.rgba)))
		fatal("cannot write %s", path.c_str());
}

/* Decode the textures of a written file (for the preview). */
void decode_textures(const ds::model &m, std::vector<image> &albedo, std::vector<image> &masks)
{
	for (const auto &t : m.textures)
	{
		auto img{decode_image(t.png, "texture")};
		if (!img)
			fatal("cannot decode a texture of the ship");
		albedo.push_back(*img);
		masks.push_back(std::move(*img));
	}
}

void print_summary(const ds::model &m, const std::span<const std::uint8_t> file)
{
	std::printf("name        %s\n", m.info.name.c_str());
	std::printf("title       %s\n", m.info.title.c_str());
	std::printf("author      %s\n", m.info.author.c_str());
	std::printf("licence     %s\n", m.info.licence.c_str());
	if (!m.info.source.empty())
		std::printf("source      %s\n", m.info.source.c_str());
	std::printf("size        %zu bytes\n", file.size());
	std::printf("sha256      %s\n", dcx::sha256_hex(dcx::sha256_of(file)).c_str());
	std::printf("triangles   %zu\n", m.indices.size() / 3);
	std::printf("vertices    %zu\n", m.vertices.size());
	std::printf("materials   %zu\n", m.materials.size());
	for (const auto &t : m.textures)
		std::printf("texture     %ux%u, %zu bytes\n", t.info.width, t.info.height, t.png.size());
	std::printf("parts       %zu\n", m.parts.empty() ? std::size_t{1} : m.parts.size());
	std::printf("bounds      %.2f..%.2f x %.2f..%.2f x %.2f..%.2f (radius %.3f)\n", m.mins.x, m.maxs.x, m.mins.y, m.maxs.y, m.mins.z, m.maxs.z, m.radius);
	std::vector<ss::triangle> tris;
	for (std::size_t i = 0; i + 2 < m.indices.size(); i += 3)
		tris.push_back({{m.indices[i], m.indices[i + 1], m.indices[i + 2]}});
	std::vector<vec3> pos;
	float outer{};
	for (const auto &v : m.vertices)
	{
		pos.push_back(v.pos);
		outer = std::max(outer, length(v.pos));
	}
	const auto areas{ss::axis_areas(pos, tris)};
	const auto rel{areas.relative()};
	std::printf("outline     %.2f x the Pyro's from the front and rear, weighted (front %.2f, side %.2f, top %.2f; all round %.2f); outermost point %.2f x the Pyro's radius\n", areas.weighted() / ss::PYRO_VIEW_AREA, rel.front, rel.side, rel.top, ss::mean_area(pos, tris) / ss::PYRO_MEAN_AREA, static_cast<double>(outer / ds::PYRO_RADIUS));
}

int check(const options &opt)
{
	const auto bytes{read_file(opt.input)};
	if (!bytes)
		fatal("cannot read %s", opt.input.c_str());
	const auto r{ds::parse(*bytes)};
	if (!r.m)
	{
		std::fprintf(stderr, "shipconv: %s: invalid: %s\n", opt.input.c_str(), r.error.c_str());
		return 1;
	}
	print_summary(*r.m, *bytes);
	if (!opt.preview.empty())
	{
		std::vector<image> albedo, masks;
		decode_textures(*r.m, albedo, masks);
		render_preview(*r.m, albedo, masks, opt.preview);
	}
	return 0;
}

int convert(options opt)
{
	const auto ext{lower(opt.input.substr(opt.input.find_last_of('.') + 1))};
	scene s{ext == "obj" ? load_obj(opt.input) : ext == "gltf" || ext == "glb" ? load_gltf(opt.input) : (fatal("unknown input type \"%s\" (use .glb, .gltf or .obj)", ext.c_str()), scene{})};
	if (s.triangles.empty())
		fatal("%s has no triangles", opt.input.c_str());
	std::fprintf(stderr, "shipconv: %s: %zu triangles, %zu vertices, %zu materials, %zu textures\n", opt.input.c_str(), s.triangles.size(), s.vertices.size(), s.materials.size(), s.images.size());
	if (s.triangles.size() > ds::MAX_TRIANGLES)
		fatal("%zu triangles; at most %u are allowed", s.triangles.size(), ds::MAX_TRIANGLES);
	{
		/* Texture coordinates the format cannot hold (dxship_format.h). */
		unsigned bad{};
		for (auto &v : s.vertices)
			if (!std::isfinite(v.u) || !std::isfinite(v.v) || std::fabs(v.u) > 1024 || std::fabs(v.v) > 1024 || !std::isfinite(v.pos.x) || !std::isfinite(v.pos.y) || !std::isfinite(v.pos.z))
			{
				++bad;
				v.u = v.v = 0;
				if (!std::isfinite(v.pos.x) || !std::isfinite(v.pos.y) || !std::isfinite(v.pos.z))
					fatal("the model has a vertex that is not a finite number");
			}
		if (bad)
			warn("%u vertices had texture coordinates beyond +-1024 or not finite; set to 0", bad);
	}
	convert_axes(s, opt);
	if (!s.has_normals)
		compute_normals(s);
	/* Bounding sphere (of the vertices that triangles use), re-centred and
	 * scaled to the Pyro's radius.
	 */
	std::vector<vec3> pts;
	{
		std::vector<bool> used(s.vertices.size());
		for (const auto &t : s.triangles)
			for (const auto i : t.v)
				used[i] = true;
		for (std::size_t i = 0; i < s.vertices.size(); ++i)
			if (used[i])
				pts.push_back(s.vertices[i].pos);
	}
	const auto bs{bounding_sphere(pts)};
	if (bs.radius <= 0)
		fatal("the model has no extent");
	if (length(bs.centre) > 0.1f * bs.radius)
		warn("the origin is %.0f %% of the radius off the bounding sphere's centre; the ship is re-centred", 100.0 * length(bs.centre) / bs.radius);
	float scale{ds::PYRO_RADIUS / bs.radius};
	for (auto &v : s.vertices)
		v.pos = (v.pos - bs.centre) * scale;
	for (auto &g : s.guns)
		if (g)
			g = (*g - bs.centre) * scale;
	/* Size (decision D3): the outline seen from the front and the rear
	 * (with a little of the side and top views) equal to the Pyro's, the
	 * outermost point at most ss::RADIUS_LIMIT Pyro radii out.
	 */
	{
		std::vector<ss::triangle> tris;
		for (const auto &t : s.triangles)
			tris.push_back(t.v);
		pts.clear();
		for (const auto &v : s.vertices)
			pts.push_back(v.pos);
		const auto before{ss::view_ratio(pts, tris)};
		const auto fair{ss::fair_scale(before)};
		const auto grow{static_cast<float>(fair.scale)};
		for (auto &v : s.vertices)
			v.pos = v.pos * grow;
		for (auto &g : s.guns)
			if (g)
				g = *g * grow;
		pts.clear();
		float outer{};
		for (const auto &v : s.vertices)
		{
			pts.push_back(v.pos);
			outer = std::max(outer, length(v.pos));
		}
		const auto areas{ss::axis_areas(pts, tris)};
		const auto after{areas.weighted() / ss::PYRO_VIEW_AREA};
		const auto rel{areas.relative()};
		std::fprintf(stderr, "shipconv: size: outline (weighted) %.2f x the Pyro's at the Pyro's radius; scaled by %.3f to %.2f x (front %.2f, side %.2f, top %.2f), outermost point %.2f x the Pyro's radius%s\n", before, fair.scale, after, rel.front, rel.side, rel.top, static_cast<double>(outer / ds::PYRO_RADIUS), fair.radius_capped ? " (radius limit)" : "");
		if (fair.too_thin)
			warn("too thin: the outline from the front and rear is only %.2f x the Pyro's even at %.2f x its radius (below %.2f); the ship is harder to see and hit than a Pyro and should not be bundled", after, ss::RADIUS_LIMIT, ss::BAND_LOW);
		else if (after < ss::BAND_LOW - 0.005 || after > ss::BAND_HIGH + 0.005)
			warn("the outline from the front and rear is %.2f x the Pyro's, outside %.2f..%.2f", after, ss::BAND_LOW, ss::BAND_HIGH);
	}
	for (unsigned i = 0; i < 8; ++i)
		if (const auto &g{s.guns[i]})
			if (const auto d{length(*g - PYRO_GUNS[i])}; d > 1)
				warn("gun marker gun%u is %.1f units from the Pyro's gun %u, where the shots come from", i, static_cast<double>(d), i);
	/* Debris parts. */
	unsigned debris{static_cast<unsigned>(std::min<std::size_t>(s.part_names.size(), ds::MAX_PARTS - 1))};
	if (!debris && opt.auto_debris)
	{
		debris = automatic_parts(s, ds::PYRO_RADIUS);
		std::fprintf(stderr, "shipconv: no debris_* parts; automatic split into %u debris parts\n", debris);
	}
	/* Colour zone. */
	auto zone{find_colour_zone(s, opt)};
	/* Textures: power of two, at most the limit; the zone greyed. */
	ds::model m;
	std::vector<int> albedo_index(s.images.size(), -1), mask_index(s.images.size(), -1), mask_out_index(s.images.size(), -1);
	std::vector<image> albedo_out, mask_out;
	std::vector<bool> image_used(s.images.size());
	for (const auto &mat : s.materials)
		if (mat.image >= 0)
			image_used[static_cast<std::size_t>(mat.image)] = true;
	for (std::size_t i = 0; i < s.images.size(); ++i)
	{
		if (!image_used[i])
			continue;
		auto &img{s.images[i]};
		const auto tw{std::min(opt.texture_size, floor_power_of_two(img.w))}, th{std::min(opt.texture_size, floor_power_of_two(img.h))};
		std::vector<float> planes(std::size_t{img.w} * img.h * 4);
		if (zone.has_mask[i])
			grey_zone(img, zone.masks[i]);
		for (std::size_t k = 0; k < planes.size(); ++k)
			planes[k] = img.rgba[k];
		const auto scaled{resample(planes, img.w, img.h, 4, tw, th)};
		image a;
		a.w = tw;
		a.h = th;
		a.rgba.resize(scaled.size());
		/* Opaque: the alpha channel is dropped from the file. */
		std::vector<std::uint8_t> rgb;
		rgb.reserve(scaled.size() / 4 * 3);
		for (std::size_t k = 0; k < scaled.size(); ++k)
		{
			a.rgba[k] = (k & 3) == 3 ? 255 : static_cast<std::uint8_t>(std::clamp(std::lround(scaled[k]), 0l, 255l));
			if ((k & 3) != 3)
				rgb.push_back(a.rgba[k]);
		}
		if (img.w != tw || img.h != th)
			std::fprintf(stderr, "shipconv: texture %s scaled from %ux%u to %ux%u\n", img.name.c_str(), img.w, img.h, tw, th);
		const auto png{encode_png(tw, th, 3, rgb)};
		m.textures.push_back({png, {}});
		albedo_index[i] = static_cast<int>(m.textures.size() - 1);
		albedo_out.push_back(a);
		if (zone.has_mask[i])
		{
			const auto ms{resample(zone.masks[i], img.w, img.h, 1, tw, th)};
			std::vector<std::uint8_t> grey(ms.size());
			for (std::size_t k = 0; k < ms.size(); ++k)
				grey[k] = static_cast<std::uint8_t>(std::clamp(std::lround(ms[k] * 255.0f), 0l, 255l));
			if (std::ranges::any_of(grey, [](const std::uint8_t g) { return g != 0; }))
			{
				m.textures.push_back({encode_png(tw, th, 1, grey), {}});
				mask_index[i] = static_cast<int>(m.textures.size() - 1);
				image mi;
				mi.w = tw;
				mi.h = th;
				for (const auto g : grey)
					mi.rgba.insert(mi.rgba.end(), {g, g, g, 255});
				mask_out.push_back(std::move(mi));
				mask_out_index[i] = static_cast<int>(mask_out.size() - 1);
			}
		}
	}
	if (m.textures.size() > ds::MAX_TEXTURES)
		fatal("%zu textures (albedo and colour masks); at most %u are allowed", m.textures.size(), ds::MAX_TEXTURES);
	/* Vertices: base colour and vertex colour baked into the vertex; the
	 * zone tint of untextured materials as the material flag.
	 */
	struct mat_key
	{
		std::uint8_t texture, mask, flags;
		auto operator<=>(const mat_key &) const = default;
	};
	std::map<mat_key, std::vector<std::uint32_t>> groups;
	std::vector<ds::vertex> verts(s.vertices.size());
	std::vector<std::uint8_t> vertex_set(s.vertices.size());
	double zone_area{}, total_area{};
	for (const auto &t : s.triangles)
	{
		const auto &mat{s.materials[t.material]};
		const auto img{mat.image};
		mat_key key{ds::NO_TEXTURE, ds::NO_TEXTURE, static_cast<std::uint8_t>((mat.tint ? 1 : 0) | (mat.double_sided ? 2 : 0))};
		if (img >= 0)
		{
			key.texture = static_cast<std::uint8_t>(albedo_index[static_cast<std::size_t>(img)]);
			if (const auto mi{mask_index[static_cast<std::size_t>(img)]}; mi >= 0)
				key.mask = static_cast<std::uint8_t>(mi);
		}
		auto &g{groups[key]};
		for (const auto i : t.v)
		{
			g.push_back(i);
			if (vertex_set[i])
				continue;
			vertex_set[i] = 1;
			const auto &sv{s.vertices[i]};
			auto &v{verts[i]};
			v.pos = sv.pos;
			const auto n{normalised(sv.normal)};
			v.normal = {{ds::float_to_snorm16(n.x), ds::float_to_snorm16(n.y), ds::float_to_snorm16(n.z)}};
			v.part = sv.part;
			v.tint = sv.tint;
			v.u = sv.u;
			v.v = sv.v;
			for (unsigned c = 0; c < 4; ++c)
			{
				auto value{sv.colour[c] * mat.base[c]};
				if (mat.tint && c < 3)
					value = ZONE_GREY;
				v.colour[c] = static_cast<std::uint8_t>(std::clamp(std::lround(value * 255.0f), 0l, 255l));
			}
		}
		/* The zone's share of the surface (design section 8). */
		const auto &a{verts[t.v[0]].pos}, &b{verts[t.v[1]].pos}, &c{verts[t.v[2]].pos};
		const double area{0.5 * length(cross(b - a, c - a))};
		double w{mat.tint ? 1.0 : (verts[t.v[0]].tint + verts[t.v[1]].tint + verts[t.v[2]].tint) / (3.0 * 255.0)};
		if (key.mask != ds::NO_TEXTURE)
		{
			const auto &mi{mask_out[static_cast<std::size_t>(mask_out_index[static_cast<std::size_t>(img)])]};
			double sum{};
			static constexpr std::array<std::array<double, 3>, 4> samples{{{{1 / 3., 1 / 3., 1 / 3.}}, {{2 / 3., 1 / 6., 1 / 6.}}, {{1 / 6., 2 / 3., 1 / 6.}}, {{1 / 6., 1 / 6., 2 / 3.}}}};
			for (const auto &bw : samples)
			{
				const auto u{bw[0] * verts[t.v[0]].u + bw[1] * verts[t.v[1]].u + bw[2] * verts[t.v[2]].u};
				const auto v{bw[0] * verts[t.v[0]].v + bw[1] * verts[t.v[1]].v + bw[2] * verts[t.v[2]].v};
				const auto tx{static_cast<unsigned>(static_cast<long>(std::floor((u - std::floor(u)) * mi.w)) % static_cast<long>(mi.w))};
				const auto ty{static_cast<unsigned>(static_cast<long>(std::floor((v - std::floor(v)) * mi.h)) % static_cast<long>(mi.h))};
				sum += mi.at(tx, ty)[0] / 255.0;
			}
			w = std::max(w, sum / samples.size());
		}
		zone_area += w * area;
		total_area += area;
	}
	const auto zone_share{total_area > 0 ? zone_area / total_area : 0};
	std::fprintf(stderr, "shipconv: player colour zone: %.0f %% of the surface\n", 100 * zone_share);
	if (zone_share < ZONE_MIN)
		fatal("the player colour zone covers %.1f %% of the surface; at least %.0f %% are required (name a material \"accent\", or use --colour-material, --colour-key, --colour-variant or --colour-mask)", 100 * zone_share, 100 * ZONE_MIN);
	if (zone_share < ZONE_WARN)
		warn("the player colour zone covers only %.0f %% of the surface", 100 * zone_share);
	/* Unused and duplicate vertices out, indices renumbered. */
	std::map<std::vector<std::uint8_t>, std::uint16_t> unique;
	std::vector<std::int32_t> remap(verts.size(), -1);
	for (std::size_t i = 0; i < verts.size(); ++i)
	{
		if (!vertex_set[i])
			continue;
		std::vector<std::uint8_t> keybytes(sizeof(ds::vertex));
		const auto &v{verts[i]};
		std::memcpy(keybytes.data(), &v.pos, sizeof(v.pos));
		std::memcpy(keybytes.data() + 12, v.normal.data(), 6);
		keybytes[18] = v.part;
		keybytes[19] = v.tint;
		std::memcpy(keybytes.data() + 20, &v.u, 4);
		std::memcpy(keybytes.data() + 24, &v.v, 4);
		std::memcpy(keybytes.data() + 28, v.colour.data(), 4);
		keybytes.resize(32);
		auto f{unique.find(keybytes)};
		if (f == unique.end())
		{
			if (m.vertices.size() >= ds::MAX_VERTICES)
				fatal("more than %u vertices", ds::MAX_VERTICES);
			m.vertices.push_back(v);
			f = unique.emplace(std::move(keybytes), static_cast<std::uint16_t>(m.vertices.size() - 1)).first;
		}
		remap[i] = f->second;
	}
	if (groups.size() > ds::MAX_MATERIALS)
		fatal("%zu distinct materials; at most %u are allowed", groups.size(), ds::MAX_MATERIALS);
	for (const auto &[key, idx] : groups)
	{
		ds::material mat;
		mat.first_index = static_cast<std::uint32_t>(m.indices.size());
		mat.index_count = static_cast<std::uint32_t>(idx.size());
		mat.texture = key.texture;
		mat.mask = key.mask;
		mat.flags = key.flags;
		for (const auto i : idx)
			m.indices.push_back(static_cast<std::uint16_t>(remap[i]));
		m.materials.push_back(mat);
	}
	/* Parts: centre and radius of each. */
	if (debris)
	{
		m.parts.resize(debris + 1);
		for (unsigned p = 0; p <= debris; ++p)
		{
			vec3 lo{1e9f, 1e9f, 1e9f}, hi{-1e9f, -1e9f, -1e9f};
			bool any{};
			for (const auto &v : m.vertices)
				if (v.part == p)
				{
					any = true;
					lo = {std::min(lo.x, v.pos.x), std::min(lo.y, v.pos.y), std::min(lo.z, v.pos.z)};
					hi = {std::max(hi.x, v.pos.x), std::max(hi.y, v.pos.y), std::max(hi.z, v.pos.z)};
				}
			auto &part{m.parts[p]};
			if (!any)
			{
				warn("debris part %u is empty", p);
				continue;
			}
			part.centre = (lo + hi) * 0.5f;
			for (const auto &v : m.vertices)
				if (v.part == p)
					part.radius = std::max(part.radius, length(v.pos - part.centre));
		}
	}
	for (unsigned i = 0; i < 8; ++i)
		if (const auto &g{s.guns[i]})
		{
			if (length(*g) > 2 * ds::PYRO_RADIUS * ds::RADIUS_TOLERANCE)
				warn("gun marker gun%u is far outside the ship and is left out", i);
			else
				m.guns[i] = {true, *g};
		}
	m.info = opt.info;
	m.info.converter = CONVERTER_VERSION;
	m.radius = ds::PYRO_RADIUS;
	m.mins = m.maxs = m.vertices[0].pos;
	for (const auto &v : m.vertices)
	{
		m.mins = {std::min(m.mins.x, v.pos.x), std::min(m.mins.y, v.pos.y), std::min(m.mins.z, v.pos.z)};
		m.maxs = {std::max(m.maxs.x, v.pos.x), std::max(m.maxs.y, v.pos.y), std::max(m.maxs.z, v.pos.z)};
	}
	std::string error;
	const auto file{ds::write(m, error)};
	if (file.empty())
		fatal("cannot write the ship: %s", error.c_str());
	/* Read it back with the game's reader. */
	const auto back{ds::parse(file)};
	if (!back.m)
		fatal("the game's reader rejects the result: %s", back.error.c_str());
	if (!write_file(opt.output, file))
		fatal("cannot write %s", opt.output.c_str());
	print_summary(*back.m, file);
	if (!opt.preview.empty())
	{
		std::vector<image> albedo, masks;
		decode_textures(*back.m, albedo, masks);
		render_preview(*back.m, albedo, masks, opt.preview);
	}
	if (Warnings)
		std::fprintf(stderr, "shipconv: %u warning%s\n", Warnings, Warnings == 1 ? "" : "s");
	return 0;
}

void usage()
{
	std::puts(
"usage: shipconv MODEL -o SHIP.dxship --name NAME --licence LICENCE [options]\n"
"       shipconv --check SHIP.dxship [--preview PNG]\n"
"\n"
"Converts a ship model (glTF 2.0 .glb/.gltf, or .obj with .mtl) into a\n"
"custom ship for D2X-Rebirth (Documentation/custom-ships-authoring.md).\n"
"\n"
"  --name ID              the ship's id: a-z, 0-9, _ and -, at most 24\n"
"  --title TEXT           the name players see (at most 32)\n"
"  --author TEXT          (at most 48)\n"
"  --licence TEXT         an SPDX id such as CC0-1.0 or CC-BY-4.0 (required)\n"
"  --source URL           where the model comes from\n"
"  --description TEXT\n"
"  --forward AXIS         the model's forward axis (default +z, as in glTF)\n"
"  --up AXIS              the model's up axis (default +y)\n"
"  --texture-size N       largest texture side, a power of two <= 512\n"
"  --colour-material NAME this material is the player colour zone\n"
"                         (also any material whose name contains\n"
"                         accent, player, colour or color)\n"
"  --colour-key RRGGBB[:T] texels of this colour (tolerance T, default 24)\n"
"  --colour-variant PNG   texels that differ in this recoloured texture\n"
"  --colour-mask PNG      red channel = the player colour weight\n"
"  --no-debris            no automatic debris split\n"
"  --preview PNG          write three views (the colour zone in red)\n"
"\n"
"Vertex colours of pure magenta (#FF00FF) also mark the colour zone.\n"
"Nodes named debris_* are debris parts; nodes named gun0..gun7 are gun\n"
"markers (informational: shots always come from the Pyro's guns).");
}

}

int main(const int argc, char **const argv)
{
	options opt;
	const auto next = [&](int &i) -> std::string {
		if (i + 1 >= argc)
			fatal("%s needs a value", argv[i]);
		return argv[++i];
	};
	for (int i = 1; i < argc; ++i)
	{
		const std::string a{argv[i]};
		if (a == "-o" || a == "--output")
			opt.output = next(i);
		else if (a == "--check")
		{
			opt.check_only = true;
			opt.input = next(i);
		}
		else if (a == "--name")
			opt.info.name = next(i);
		else if (a == "--title")
			opt.info.title = next(i);
		else if (a == "--author")
			opt.info.author = next(i);
		else if (a == "--licence" || a == "--license")
			opt.info.licence = next(i);
		else if (a == "--source")
			opt.info.source = next(i);
		else if (a == "--description")
			opt.info.description = next(i);
		else if (a == "--forward")
			opt.forward = next(i);
		else if (a == "--up")
			opt.up = next(i);
		else if (a == "--texture-size")
		{
			const auto v{std::strtoul(next(i).c_str(), nullptr, 10)};
			if (v < 1 || v > ds::MAX_TEXTURE_SIZE || (v & (v - 1)))
				fatal("--texture-size must be a power of two up to %u", ds::MAX_TEXTURE_SIZE);
			opt.texture_size = static_cast<unsigned>(v);
		}
		else if (a == "--colour-material" || a == "--color-material")
			opt.colour_materials.push_back(next(i));
		else if (a == "--colour-key" || a == "--color-key")
		{
			const auto v{next(i)};
			unsigned rgb{}, tol{24};
			if (std::sscanf(v.c_str(), "%6x:%u", &rgb, &tol) < 1 || v.size() < 6)
				fatal("--colour-key wants RRGGBB[:tolerance]");
			opt.colour_key = std::array<int, 4>{{static_cast<int>(rgb >> 16), static_cast<int>((rgb >> 8) & 255), static_cast<int>(rgb & 255), static_cast<int>(tol)}};
		}
		else if (a == "--colour-variant" || a == "--color-variant")
			opt.colour_variant = next(i);
		else if (a == "--colour-mask" || a == "--color-mask")
			opt.colour_mask = next(i);
		else if (a == "--no-debris")
			opt.auto_debris = false;
		else if (a == "--preview")
			opt.preview = next(i);
		else if (a == "-h" || a == "--help")
		{
			usage();
			return 0;
		}
		else if (a[0] == '-' && a.size() > 1)
		{
			usage();
			return 2;
		}
		else
			opt.input = a;
	}
	if (opt.check_only)
		return check(opt);
	if (opt.input.empty() || opt.output.empty())
	{
		usage();
		return 2;
	}
	if (!ds::valid_name(opt.info.name))
		fatal("--name must be 1 to 24 characters of a-z, 0-9, _ and -");
	if (opt.info.licence.empty())
		fatal("--licence is required (for example CC0-1.0 or CC-BY-4.0)");
	if (opt.info.title.empty())
		opt.info.title = opt.info.name;
	return convert(std::move(opt));
}
