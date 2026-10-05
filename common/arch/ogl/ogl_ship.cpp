/*
 * This file is part of the DXX-Rebirth project <https://github.com/dxx-rebirth/dxx-rebirth/>.
 * It is copyright by its individual contributors, as recorded in the
 * project's Git history.  See COPYING.txt at the top level for license
 * terms and a link to the Git history.
 */
/*
 * The OpenGL draw path of the custom ships (ogl_ship.h).
 */

#include "dxxsconf.h"
#if DXX_USE_OGL
#include <algorithm>
#include <cmath>
#include <set>
#include "ogl_init.h"
#include "ogl_extensions.h"
#include "ogl_ship.h"
#include "internal.h"
#include "config.h"
#include "3d.h"
#include "common/3d/globvars.h"

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#define STBI_NO_LINEAR
#define STBI_NO_HDR
#define STBI_ASSERT(x)
#include "contrib/stb/stb_image.h"

namespace dcx {

namespace ship_gl {

namespace {

/* Every live mesh, so that a lost context can reach their handles.
 * Never destroyed: meshes owned by other files' globals may outlive any
 * global of this file at exit.
 */
std::set<mesh *> &meshes()
{
	static auto *const s{new std::set<mesh *>};
	return *s;
}

[[nodiscard]]
std::uint8_t to_byte(const float v)
{
	return static_cast<std::uint8_t>(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f));
}

/* Upload with mipmaps made by halving (the sizes are powers of two). */
unsigned upload(const rgba_image &img)
{
	GLuint handle{};
	glGenTextures(1, &handle);
	glBindTexture(GL_TEXTURE_2D, handle);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
	rgba_image level{img};
	for (GLint lod = 0;; ++lod)
	{
		glTexImage2D(GL_TEXTURE_2D, lod, GL_RGBA, static_cast<GLsizei>(level.w), static_cast<GLsizei>(level.h), 0, GL_RGBA, GL_UNSIGNED_BYTE, level.px.data());
		if (level.w == 1 && level.h == 1)
			break;
		rgba_image next;
		next.w = std::max(1u, level.w / 2);
		next.h = std::max(1u, level.h / 2);
		next.px.resize(std::size_t{next.w} * next.h * 4);
		for (unsigned y = 0; y < next.h; ++y)
			for (unsigned x = 0; x < next.w; ++x)
				for (unsigned c = 0; c < 4; ++c)
				{
					const unsigned x0{std::min(2 * x, level.w - 1)}, x1{std::min(2 * x + 1, level.w - 1)};
					const unsigned y0{std::min(2 * y, level.h - 1)}, y1{std::min(2 * y + 1, level.h - 1)};
					const auto at = [&level, c](const unsigned px, const unsigned py) -> unsigned {
						return level.px[(std::size_t{py} * level.w + px) * 4 + c];
					};
					next.px[(std::size_t{y} * next.w + x) * 4 + c] = static_cast<std::uint8_t>((at(x0, y0) + at(x1, y0) + at(x0, y1) + at(x1, y1) + 2) / 4);
				}
		level = std::move(next);
	}
	return handle;
}

}

mesh::mesh()
{
	meshes().insert(this);
}

mesh::~mesh()
{
	free_textures();
	meshes().erase(this);
}

void mesh::free_textures()
{
	for (auto &[key, handle] : textures)
	{
		const GLuint h{handle};
		glDeleteTextures(1, &h);
	}
	textures.clear();
}

void textures_lost()
{
	for (const auto m : meshes())
		m->free_textures();
}

std::array<float, 3> tint_from_rgb5(const std::uint8_t r, const std::uint8_t g, const std::uint8_t b)
{
	const float top{static_cast<float>(std::max({r, g, b, std::uint8_t{1}}))};
	return {{r / top, g / top, b / top}};
}

std::unique_ptr<mesh> mesh_create(const dxship::model &m, std::string &error)
{
	auto r{std::make_unique<mesh>()};
	for (const auto &t : m.textures)
	{
		int w, h, comp;
		const auto p{stbi_load_from_memory(t.png.data(), static_cast<int>(t.png.size()), &w, &h, &comp, 4)};
		if (!p)
		{
			error = "a texture does not decode";
			return nullptr;
		}
		/* The header was checked by the reader; the data must agree. */
		if (static_cast<unsigned>(w) != t.info.width || static_cast<unsigned>(h) != t.info.height)
		{
			stbi_image_free(p);
			error = "a texture's size differs from its header";
			return nullptr;
		}
		rgba_image img;
		img.w = t.info.width;
		img.h = t.info.height;
		img.px.assign(p, p + std::size_t{img.w} * img.h * 4);
		stbi_image_free(p);
		r->images.push_back(std::move(img));
	}
	const auto n{m.vertices.size()};
	r->pos.reserve(n * 3);
	r->normal.reserve(n * 3);
	r->uv.reserve(n * 2);
	for (const auto &v : m.vertices)
	{
		r->pos.insert(r->pos.end(), {v.pos.x, v.pos.y, v.pos.z});
		const float nx{dxship::snorm16_to_float(v.normal[0])}, ny{dxship::snorm16_to_float(v.normal[1])}, nz{dxship::snorm16_to_float(v.normal[2])};
		const float len{std::sqrt(nx * nx + ny * ny + nz * nz)};
		const float s{len > 1e-6f ? 1 / len : 0};
		r->normal.insert(r->normal.end(), {nx * s, ny * s, nz * s});
		r->uv.insert(r->uv.end(), {v.u, v.v});
		r->colour.push_back({{v.colour[0] / 255.0f, v.colour[1] / 255.0f, v.colour[2] / 255.0f, v.colour[3] / 255.0f}});
		r->tint.push_back(v.tint / 255.0f);
		r->part.push_back(v.part);
	}
	for (const auto &mat : m.materials)
	{
		mesh::material mm;
		mm.first = mat.first_index;
		mm.count = mat.index_count;
		for (unsigned c = 0; c < 4; ++c)
			mm.base[c] = mat.base_colour[c] / 255.0f;
		mm.albedo = mat.texture == dxship::NO_TEXTURE ? -1 : mat.texture;
		mm.mask = mat.mask == dxship::NO_TEXTURE ? -1 : mat.mask;
		mm.tint = mat.has_flag(dxship::material_flag::tint);
		r->materials.push_back(mm);
	}
	r->parts = m.parts;
	r->radius = m.radius;
	const std::size_t nparts{std::max<std::size_t>(m.parts.size(), 1)};
	r->part_indices.assign(nparts, std::vector<std::vector<std::uint16_t>>(m.materials.size()));
	for (std::size_t mi = 0; mi < m.materials.size(); ++mi)
	{
		const auto &mat{m.materials[mi]};
		for (std::uint32_t i = mat.first_index; i < mat.first_index + mat.index_count; i += 3)
		{
			/* A triangle belongs to the part of its first vertex. */
			const auto p{m.vertices[m.indices[i]].part};
			auto &list{r->part_indices[p < nparts ? p : 0][mi]};
			list.insert(list.end(), {m.indices[i], m.indices[i + 1], m.indices[i + 2]});
		}
	}
	r->colours.resize(n * 4);
	return r;
}

namespace {

/* The albedo with the colour zone tinted, uploaded on first use. */
unsigned texture_for(mesh &m, const mesh::material &mat, const draw_params &p)
{
	const auto key{std::make_tuple(mat.albedo, mat.mask, mat.mask >= 0 ? p.tint_key : 0u)};
	if (const auto f{m.textures.find(key)}; f != m.textures.end())
		return f->second;
	const auto &albedo{m.images[static_cast<std::size_t>(mat.albedo)]};
	unsigned handle;
	if (mat.mask >= 0)
	{
		const auto &mask{m.images[static_cast<std::size_t>(mat.mask)]};
		rgba_image tinted{albedo};
		for (std::size_t i = 0; i < std::size_t{tinted.w} * tinted.h; ++i)
		{
			const float w{mask.px[i * 4] / 255.0f};
			for (unsigned c = 0; c < 3; ++c)
				tinted.px[i * 4 + c] = to_byte(tinted.px[i * 4 + c] / 255.0f * (1 - w + w * p.tint[c]));
			tinted.px[i * 4 + 3] = 255;
		}
		handle = upload(tinted);
	}
	else
	{
		rgba_image opaque{albedo};
		for (std::size_t i = 0; i < std::size_t{opaque.w} * opaque.h; ++i)
			opaque.px[i * 4 + 3] = 255;
		handle = upload(opaque);
	}
	m.textures.emplace(key, handle);
	return handle;
}

}

void draw(mesh &m, const draw_params &p)
{
	/* Model space to OpenGL eye space: the 3D library's rotation (rows
	 * rvec, uvec, fvec of View_matrix, which carries the zoom), z
	 * negated as ogl.cpp does for every vertex, around View_position.
	 */
	const auto &vm{View_matrix};
	const std::array<std::array<float, 3>, 3> rows{{
		{{f2fl(vm.rvec.x), f2fl(vm.rvec.y), f2fl(vm.rvec.z)}},
		{{f2fl(vm.uvec.x), f2fl(vm.uvec.y), f2fl(vm.uvec.z)}},
		{{-f2fl(vm.fvec.x), -f2fl(vm.fvec.y), -f2fl(vm.fvec.z)}},
	}};
	/* A piece drawn around its centre: v - offset - View_position. */
	const std::array<float, 3> vp{{f2fl(View_position.x) + p.offset[0], f2fl(View_position.y) + p.offset[1], f2fl(View_position.z) + p.offset[2]}};
	std::array<GLfloat, 16> mv{};
	for (unsigned r = 0; r < 3; ++r)
	{
		for (unsigned c = 0; c < 3; ++c)
			mv[c * 4 + r] = rows[r][c];
		mv[12 + r] = -(rows[r][0] * vp[0] + rows[r][1] * vp[1] + rows[r][2] * vp[2]);
	}
	mv[15] = 1;
	/* The view direction in model space, for the lighting of
	 * get_noglow_light: 1/4 + 3/4 of the facing; both sides lit.
	 */
	std::array<float, 3> view{{f2fl(vm.fvec.x), f2fl(vm.fvec.y), f2fl(vm.fvec.z)}};
	{
		const float l{std::sqrt(view[0] * view[0] + view[1] * view[1] + view[2] * view[2])};
		if (l > 1e-6f)
			for (auto &c : view)
				c /= l;
	}
	glMatrixMode(GL_MODELVIEW);
	glPushMatrix();
	glMultMatrixf(mv.data());
	const GLboolean cull{glIsEnabled(GL_CULL_FACE)};
	glDisable(GL_CULL_FACE);
	glEnableClientState(GL_VERTEX_ARRAY);
	glEnableClientState(GL_COLOR_ARRAY);
	glVertexPointer(3, GL_FLOAT, 0, m.pos.data());
	glColorPointer(4, GL_FLOAT, 0, m.colours.data());
	glTexCoordPointer(2, GL_FLOAT, 0, m.uv.data());
	glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
	for (std::size_t mi = 0; mi < m.materials.size(); ++mi)
	{
		const auto &mat{m.materials[mi]};
		const bool textured{!p.flat_black && mat.albedo >= 0};
		if (textured)
		{
			glEnableClientState(GL_TEXTURE_COORD_ARRAY);
			OGL_ENABLE(TEXTURE_2D);
			glBindTexture(GL_TEXTURE_2D, texture_for(m, mat, p));
		}
		else
		{
			glDisableClientState(GL_TEXTURE_COORD_ARRAY);
			OGL_DISABLE(TEXTURE_2D);
		}
		for (unsigned part = 0; part < m.part_count(); ++part)
		{
			if (!(p.part_mask & (1u << part)))
				continue;
			const auto &idx{m.part_indices[part][mi]};
			if (idx.empty())
				continue;
			for (const auto i : idx)
			{
				auto *const c{&m.colours[std::size_t{i} * 4]};
				if (p.flat_black)
				{
					c[0] = c[1] = c[2] = 0;
					c[3] = p.alpha;
					continue;
				}
				const auto *const n{&m.normal[std::size_t{i} * 3]};
				const float facing{std::fabs(n[0] * view[0] + n[1] * view[1] + n[2] * view[2])};
				const float shade{0.25f + 0.75f * facing};
				const float w{mat.tint ? 1.0f : m.tint[i]};
				const auto &vc{m.colour[i]};
				/* The light at most 1, as for the polygon models (whose
				 * textures it multiplies): in a bright room an untextured
				 * material keeps its base colour instead of turning white.
				 */
				for (unsigned k = 0; k < 3; ++k)
					c[k] = std::min(p.light[k] * shade, 1.0f) * mat.base[k] * vc[k] * (1 - w + w * p.tint[k]);
				c[3] = p.alpha;
			}
			glDrawElements(GL_TRIANGLES, static_cast<GLsizei>(idx.size()), GL_UNSIGNED_SHORT, idx.data());
		}
	}
	glDisableClientState(GL_TEXTURE_COORD_ARRAY);
	glDisableClientState(GL_COLOR_ARRAY);
	glDisableClientState(GL_VERTEX_ARRAY);
	if (cull)
		glEnable(GL_CULL_FACE);
	glPopMatrix();
}

}

}
#endif
