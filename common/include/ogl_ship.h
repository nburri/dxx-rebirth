/*
 * This file is part of the DXX-Rebirth project <https://github.com/dxx-rebirth/dxx-rebirth/>.
 * It is copyright by its individual contributors, as recorded in the
 * project's Git history.  See COPYING.txt at the top level for license
 * terms and a link to the Git history.
 */
/*
 * The OpenGL draw path of the custom ships (Documentation/custom-ships.md
 * section 6): a `.dxship` model (dxship_format.h) as triangle lists with
 * true-colour textures, lit per vertex like the game's polygon models
 * (1/4 ambient plus 3/4 facing the viewer, times the object's light),
 * with the player colour zone tinted.  Fixed-function OpenGL, client
 * arrays; it needs no game state beyond the 3D library's current
 * instance (View_matrix, View_position).
 */

#pragma once

#include "dxxsconf.h"
#include <array>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <tuple>
#include <vector>

#include "dxship_format.h"

#if DXX_USE_OGL

namespace dcx {

namespace ship_gl {

struct rgba_image
{
	unsigned w{}, h{};
	std::vector<std::uint8_t> px;
};

class mesh
{
public:
	struct material
	{
		std::uint32_t first{}, count{};
		std::array<float, 4> base{{1, 1, 1, 1}};
		int albedo{-1}, mask{-1};
		bool tint{};
	};
	std::vector<float> pos;		/* 3 per vertex, model space (Descent axes) */
	std::vector<float> normal;	/* 3 per vertex, unit */
	std::vector<float> uv;		/* 2 per vertex */
	std::vector<std::array<float, 4>> colour;	/* vertex colour */
	std::vector<float> tint;	/* player colour weight, 0..1 */
	std::vector<std::uint8_t> part;
	/* Per part (0 = the whole ship when it has no parts), per material,
	 * the indices of that part's triangles.
	 */
	std::vector<std::vector<std::vector<std::uint16_t>>> part_indices;
	std::vector<material> materials;
	std::vector<dxship::part> parts;
	std::vector<rgba_image> images;
	float radius{};
	/* Uploaded textures: (albedo image, mask image, tint key) -> handle. */
	std::map<std::tuple<int, int, unsigned>, unsigned> textures;
	/* Scratch for the per-vertex colours of a draw. */
	std::vector<float> colours;
	mesh();
	~mesh();
	mesh(const mesh &) = delete;
	mesh &operator=(const mesh &) = delete;
	void free_textures();
	[[nodiscard]]
	unsigned part_count() const
	{
		return static_cast<unsigned>(part_indices.size());
	}
};

/* Decode the textures and build the arrays; nothing is uploaded until
 * the first draw.  Returns nullptr and sets `error` if a texture does not
 * decode to its header's size.
 */
[[nodiscard]]
std::unique_ptr<mesh> mesh_create(const dxship::model &m, std::string &error);

struct draw_params
{
	/* The object's light (1.0 = full), as the polygon models get it. */
	std::array<float, 3> light{{1, 1, 1}};
	/* The player's colour for the colour zone, and a key naming it (one
	 * set of tinted textures is kept per key).
	 */
	std::array<float, 3> tint{{1, 1, 1}};
	unsigned tint_key{};
	/* Cloak: alpha, and flat black instead of lit and textured. */
	float alpha{1};
	bool flat_black{};
	/* Which parts to draw (bit per part). */
	unsigned part_mask{~0u};
	/* Model space offset added before the instance transform (a debris
	 * piece is drawn around its own centre).
	 */
	std::array<float, 3> offset{{0, 0, 0}};
};

/* Draw at the 3D library's current instance (after
 * g3_start_instance_matrix, as draw_polygon_model does).
 */
void draw(mesh &m, const draw_params &p);

/* The OpenGL context lost its textures (ogl_smash_texture_list_internal):
 * forget every handle; they are uploaded again when next drawn.
 */
void textures_lost();

/* Tint colour of a player colour (player_ship_color) from the HUD's
 * colour (0..31 per channel), brightest channel scaled to 1.
 */
[[nodiscard]]
std::array<float, 3> tint_from_rgb5(std::uint8_t r, std::uint8_t g, std::uint8_t b);

}

}

#endif
