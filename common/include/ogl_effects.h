/*
 * This file is part of the DXX-Rebirth project <https://github.com/dxx-rebirth/dxx-rebirth/>.
 * It is copyright by its individual contributors, as recorded in the
 * project's Git history.  See COPYING.txt at the top level for license
 * terms and a link to the Git history.
 */

/* Optional rendering effects for the OpenGL renderer (not OpenGL ES):
 *
 * - the world shader: per-pixel (bilinear) interpolation of the four
 *   corner lights of a wall quad instead of the two Gouraud triangles of
 *   the fixed-function pipeline ("Smooth lighting"), and anti-aliased
 *   "sharp pixels" texture sampling (opengl_texture_filter::sharp);
 * - the post-process pipeline around a 3D view: the view is drawn into
 *   an off-screen framebuffer (optionally multisampled and/or at a
 *   higher render scale), then bright parts are blurred and added back
 *   (bloom), the gamma/contrast curve is applied, and the result is
 *   drawn into the view's place on the screen.
 *
 * Everything needs GLSL 1.10 (OpenGL 2.0) and, for the post-process
 * pipeline, framebuffer objects (OpenGL 3.0, ARB_framebuffer_object or
 * EXT_framebuffer_object/blit/multisample).  Without them the options
 * fall back to the fixed-function renderer.
 */

#pragma once

#include "dxxsconf.h"
#include <array>
#include <cstdint>

#if DXX_USE_OGL
#include "fwd-gr.h"

namespace dcx {

struct ogl_texture;

struct ogl_effects_support
{
	bool shaders;		/* GLSL 1.10 */
	bool texture_lod;	/* ARB_shader_texture_lod (sharp pixels) */
	bool fbo;		/* framebuffer objects with blit */
	bool fbo_multisample;
	int max_samples;
};

#if DXX_USE_OGLES
static constexpr ogl_effects_support ogl_effects_caps{};
static inline void ogl_effects_init() {}
static inline bool ogl_post_begin(const grs_canvas &, bool) { return false; }
static inline void ogl_post_end() {}
static inline bool ogl_post_active() { return false; }
static inline void ogl_post_set_viewport() {}
static inline float ogl_post_scale() { return 1.0f; }
static inline bool ogl_sharp_pixels_active() { return false; }
static inline bool ogl_smooth_lighting_active() { return false; }
static inline bool ogl_world_shader_quad(const ogl_texture &, const std::array<std::array<float, 4>, 4> &) { return false; }
static inline bool ogl_world_shader_plain(const ogl_texture &) { return false; }
static inline void ogl_world_shader_off() {}
#else
extern ogl_effects_support ogl_effects_caps;

/* Called after every gr_set_mode, with the context current. */
void ogl_effects_init();

/* The post-process pipeline around one 3D view (render_frame, the
 * automap).  ogl_post_begin returns whether it redirected drawing into
 * the off-screen framebuffer; then ogl_post_end must follow after
 * g3_end_frame.  `world` enables bloom (not wanted on the automap).
 */
bool ogl_post_begin(const grs_canvas &canvas, bool world);
void ogl_post_end();
bool ogl_post_active();
/* ogl_start_frame: set the viewport of the off-screen view. */
void ogl_post_set_viewport();
/* The render scale of the active view (1 when inactive), for line widths. */
float ogl_post_scale();

/* Sharp pixels: textures are loaded with linear filtering and the
 * shader keeps texel edges crisp.  False if not supported.
 */
bool ogl_sharp_pixels_active();

/* The world shader: bind it for one draw call.  Each returns false when
 * the fixed-function pipeline should draw (shader not wanted or not
 * available); then the shader is unbound.
 *
 * ogl_world_shader_quad: a quad (4 vertices drawn as a triangle fan)
 * whose color array holds the corner coordinates (0,0) (1,0) (1,1)
 * (0,1) in red/green, and whose four corner colors (RGBA, clamped to
 * [0,1]) are `corners`.
 * ogl_world_shader_plain: any other textured draw; the color array
 * holds the colors as usual.  Only binds for sharp pixels.
 */
bool ogl_world_shader_quad(const ogl_texture &tex, const std::array<std::array<float, 4>, 4> &corners);
bool ogl_world_shader_plain(const ogl_texture &tex);
/* Whether smooth lighting wants quads drawn with ogl_world_shader_quad. */
bool ogl_smooth_lighting_active();

extern bool ogl_world_shader_bound;
void ogl_world_shader_unbind();
/* Every fixed-function draw call must make sure the shader is unbound. */
static inline void ogl_world_shader_off()
{
	if (ogl_world_shader_bound) [[unlikely]]
		ogl_world_shader_unbind();
}
#endif

}
#endif
