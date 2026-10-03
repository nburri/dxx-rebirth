/*
 * This file is part of the DXX-Rebirth project <https://github.com/dxx-rebirth/dxx-rebirth/>.
 * It is copyright by its individual contributors, as recorded in the
 * project's Git history.  See COPYING.txt at the top level for license
 * terms and a link to the Git history.
 */

/* Optional rendering effects: see ogl_effects.h. */

#include "dxxsconf.h"

#if DXX_USE_OGL && !DXX_USE_OGLES

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <span>
#include <string>
#include <SDL.h>

#include "ogl_init.h"
#include "ogl_extensions.h"
#include "ogl_effects.h"
#include "internal.h"
#include "config.h"
#include "console.h"
#include "gr.h"
#include "args.h"

/* The OpenGL 2.0/3.0 names used below, for headers that only declare
 * OpenGL 1.1 (Windows) or 2.1 (macOS).
 */
#ifndef GL_FRAGMENT_SHADER
#define GL_FRAGMENT_SHADER 0x8B30
#endif
#ifndef GL_VERTEX_SHADER
#define GL_VERTEX_SHADER 0x8B31
#endif
#ifndef GL_COMPILE_STATUS
#define GL_COMPILE_STATUS 0x8B81
#endif
#ifndef GL_LINK_STATUS
#define GL_LINK_STATUS 0x8B82
#endif
#ifndef GL_TEXTURE0
#define GL_TEXTURE0 0x84C0
#endif
#ifndef GL_TEXTURE1
#define GL_TEXTURE1 0x84C1
#endif
#ifndef GL_CLAMP_TO_EDGE
#define GL_CLAMP_TO_EDGE 0x812F
#endif
#ifndef GL_FRAMEBUFFER
#define GL_FRAMEBUFFER 0x8D40
#endif
#ifndef GL_READ_FRAMEBUFFER
#define GL_READ_FRAMEBUFFER 0x8CA8
#endif
#ifndef GL_DRAW_FRAMEBUFFER
#define GL_DRAW_FRAMEBUFFER 0x8CA9
#endif
#ifndef GL_RENDERBUFFER
#define GL_RENDERBUFFER 0x8D41
#endif
#ifndef GL_COLOR_ATTACHMENT0
#define GL_COLOR_ATTACHMENT0 0x8CE0
#endif
#ifndef GL_DEPTH_ATTACHMENT
#define GL_DEPTH_ATTACHMENT 0x8D00
#endif
#ifndef GL_FRAMEBUFFER_COMPLETE
#define GL_FRAMEBUFFER_COMPLETE 0x8CD5
#endif
#ifndef GL_DEPTH_COMPONENT24
#define GL_DEPTH_COMPONENT24 0x81A6
#endif
#ifndef GL_MAX_SAMPLES
#define GL_MAX_SAMPLES 0x8D57
#endif

namespace dcx {

ogl_effects_support ogl_effects_caps;
bool ogl_world_shader_bound;

namespace {

/* Function pointers, named apart from the system headers' prototypes. */
using dxx_GLchar = char;
using pfn_CreateShader = GLuint (APIENTRY *)(GLenum);
using pfn_ShaderSource = void (APIENTRY *)(GLuint, GLsizei, const dxx_GLchar *const *, const GLint *);
using pfn_CompileShader = void (APIENTRY *)(GLuint);
using pfn_GetShaderiv = void (APIENTRY *)(GLuint, GLenum, GLint *);
using pfn_GetShaderInfoLog = void (APIENTRY *)(GLuint, GLsizei, GLsizei *, dxx_GLchar *);
using pfn_DeleteShader = void (APIENTRY *)(GLuint);
using pfn_CreateProgram = GLuint (APIENTRY *)();
using pfn_AttachShader = void (APIENTRY *)(GLuint, GLuint);
using pfn_LinkProgram = void (APIENTRY *)(GLuint);
using pfn_GetProgramiv = void (APIENTRY *)(GLuint, GLenum, GLint *);
using pfn_GetProgramInfoLog = void (APIENTRY *)(GLuint, GLsizei, GLsizei *, dxx_GLchar *);
using pfn_UseProgram = void (APIENTRY *)(GLuint);
using pfn_GetUniformLocation = GLint (APIENTRY *)(GLuint, const dxx_GLchar *);
using pfn_Uniform1i = void (APIENTRY *)(GLint, GLint);
using pfn_Uniform1f = void (APIENTRY *)(GLint, GLfloat);
using pfn_Uniform2f = void (APIENTRY *)(GLint, GLfloat, GLfloat);
using pfn_Uniform4fv = void (APIENTRY *)(GLint, GLsizei, const GLfloat *);
using pfn_ActiveTexture = void (APIENTRY *)(GLenum);
using pfn_GenFramebuffers = void (APIENTRY *)(GLsizei, GLuint *);
using pfn_DeleteFramebuffers = void (APIENTRY *)(GLsizei, const GLuint *);
using pfn_BindFramebuffer = void (APIENTRY *)(GLenum, GLuint);
using pfn_FramebufferTexture2D = void (APIENTRY *)(GLenum, GLenum, GLenum, GLuint, GLint);
using pfn_FramebufferRenderbuffer = void (APIENTRY *)(GLenum, GLenum, GLenum, GLuint);
using pfn_CheckFramebufferStatus = GLenum (APIENTRY *)(GLenum);
using pfn_GenRenderbuffers = void (APIENTRY *)(GLsizei, GLuint *);
using pfn_DeleteRenderbuffers = void (APIENTRY *)(GLsizei, const GLuint *);
using pfn_BindRenderbuffer = void (APIENTRY *)(GLenum, GLuint);
using pfn_RenderbufferStorage = void (APIENTRY *)(GLenum, GLenum, GLsizei, GLsizei);
using pfn_RenderbufferStorageMultisample = void (APIENTRY *)(GLenum, GLsizei, GLenum, GLsizei, GLsizei);
using pfn_BlitFramebuffer = void (APIENTRY *)(GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLbitfield, GLenum);

struct gl_functions
{
	pfn_CreateShader CreateShader;
	pfn_ShaderSource ShaderSource;
	pfn_CompileShader CompileShader;
	pfn_GetShaderiv GetShaderiv;
	pfn_GetShaderInfoLog GetShaderInfoLog;
	pfn_DeleteShader DeleteShader;
	pfn_CreateProgram CreateProgram;
	pfn_AttachShader AttachShader;
	pfn_LinkProgram LinkProgram;
	pfn_GetProgramiv GetProgramiv;
	pfn_GetProgramInfoLog GetProgramInfoLog;
	pfn_UseProgram UseProgram;
	pfn_GetUniformLocation GetUniformLocation;
	pfn_Uniform1i Uniform1i;
	pfn_Uniform1f Uniform1f;
	pfn_Uniform2f Uniform2f;
	pfn_Uniform4fv Uniform4fv;
	pfn_ActiveTexture ActiveTexture;
	pfn_GenFramebuffers GenFramebuffers;
	pfn_DeleteFramebuffers DeleteFramebuffers;
	pfn_BindFramebuffer BindFramebuffer;
	pfn_FramebufferTexture2D FramebufferTexture2D;
	pfn_FramebufferRenderbuffer FramebufferRenderbuffer;
	pfn_CheckFramebufferStatus CheckFramebufferStatus;
	pfn_GenRenderbuffers GenRenderbuffers;
	pfn_DeleteRenderbuffers DeleteRenderbuffers;
	pfn_BindRenderbuffer BindRenderbuffer;
	pfn_RenderbufferStorage RenderbufferStorage;
	pfn_RenderbufferStorageMultisample RenderbufferStorageMultisample;
	pfn_BlitFramebuffer BlitFramebuffer;
};

gl_functions gl;

template <typename F>
bool load_function(F &f, const char *const name, const char *const ext_suffix = nullptr)
{
	f = reinterpret_cast<F>(SDL_GL_GetProcAddress(name));
	if (!f && ext_suffix)
	{
		std::array<char, 64> buf;
		std::snprintf(buf.data(), buf.size(), "%s%s", name, ext_suffix);
		f = reinterpret_cast<F>(SDL_GL_GetProcAddress(buf.data()));
	}
	return f != nullptr;
}

bool has_extension(const char *const extensions, const char *const name)
{
	if (!extensions)
		return false;
	const std::size_t len{std::strlen(name)};
	for (const char *p = extensions; (p = std::strstr(p, name)) != nullptr; p += len)
	{
		if ((p == extensions || p[-1] == ' ') && (p[len] == ' ' || p[len] == 0))
			return true;
	}
	return false;
}

/* The shaders.  GLSL 1.10 with the compatibility built-ins, so that the
 * fixed-function vertex arrays and matrices keep working.
 */
constexpr char world_vertex_source[] =
"#version 110\n"
"varying vec2 v_uv;\n"
"varying vec4 v_col;\n"
"void main()\n"
"{\n"
"	v_uv = gl_MultiTexCoord0.xy;\n"
"	v_col = gl_Color;\n"
"	gl_Position = ftransform();\n"
"}\n";

constexpr char world_fragment_body[] =
"uniform sampler2D u_tex;\n"
"uniform float u_quad;\n"
"uniform vec4 u_corner[4];\n"
"uniform vec2 u_texsize;\n"
"varying vec2 v_uv;\n"
"varying vec4 v_col;\n"
"void main()\n"
"{\n"
"	vec4 col = v_col;\n"
"	if (u_quad > 0.5)\n"
"	{\n"
"		vec2 st = v_col.xy;\n"
"		col = mix(mix(u_corner[0], u_corner[1], st.x), mix(u_corner[3], u_corner[2], st.x), st.y);\n"
"	}\n"
"#ifdef SHARP\n"
	/* Texel coordinates; inside a texel sample its center, across a
	 * texel edge blend over one screen pixel.  Where texels are smaller
	 * than pixels, plain trilinear filtering.  The gradients of the
	 * original coordinates select the mipmap level, so that the
	 * flattened coordinates do not.
	 */
"	vec2 t = v_uv * u_texsize;\n"
"	vec2 w = max(fwidth(t), vec2(1.0e-5));\n"
"	vec2 c = floor(t + 0.5);\n"
"	vec2 sharp_uv = (c + clamp((t - c) / w, -0.5, 0.5)) / u_texsize;\n"
"	vec2 uv = mix(sharp_uv, v_uv, clamp(max(w.x, w.y) * 2.0 - 1.0, 0.0, 1.0));\n"
"	vec4 tex = texture2DGradARB(u_tex, uv, dFdx(v_uv), dFdy(v_uv));\n"
"#else\n"
"	vec4 tex = texture2D(u_tex, v_uv);\n"
"#endif\n"
"	gl_FragColor = tex * col;\n"
"}\n";

/* Post-process passes: a full-target quad, vertices 0..1. */
constexpr char post_vertex_source[] =
"#version 110\n"
"varying vec2 v_uv;\n"
"void main()\n"
"{\n"
"	v_uv = gl_MultiTexCoord0.xy;\n"
"	gl_Position = vec4(gl_Vertex.xy * 2.0 - 1.0, 0.0, 1.0);\n"
"}\n";

/* Downsample with four bilinear taps (a box filter of up to 4x4 source
 * texels); with u_threshold > 0 also keep only the bright part.
 */
constexpr char post_down_fragment_source[] =
"#version 110\n"
"uniform sampler2D u_src;\n"
"uniform vec2 u_off;\n"
"uniform vec2 u_max;\n"
"uniform float u_threshold;\n"
"varying vec2 v_uv;\n"
"vec3 tap(vec2 uv)\n"
"{\n"
"	return texture2D(u_src, min(uv, u_max)).rgb;\n"
"}\n"
"void main()\n"
"{\n"
"	vec3 c = 0.25 * (tap(v_uv + vec2(-u_off.x, -u_off.y)) + tap(v_uv + vec2(u_off.x, -u_off.y)) + tap(v_uv + vec2(-u_off.x, u_off.y)) + tap(v_uv + u_off));\n"
"	if (u_threshold > 0.0)\n"
"	{\n"
"		float m = max(c.r, max(c.g, c.b));\n"
"		float k = clamp((m - u_threshold) / (1.0 - u_threshold), 0.0, 1.0);\n"
"		c *= k * k;\n"
"	}\n"
"	gl_FragColor = vec4(c, 1.0);\n"
"}\n";

/* A 9-tap Gaussian in 5 bilinear taps along u_dir. */
constexpr char post_blur_fragment_source[] =
"#version 110\n"
"uniform sampler2D u_src;\n"
"uniform vec2 u_dir;\n"
"uniform vec2 u_max;\n"
"varying vec2 v_uv;\n"
"vec3 tap(vec2 uv)\n"
"{\n"
"	return texture2D(u_src, clamp(uv, vec2(0.0), u_max)).rgb;\n"
"}\n"
"void main()\n"
"{\n"
"	vec3 c = tap(v_uv) * 0.2270270270;\n"
"	c += (tap(v_uv + u_dir * 1.3846153846) + tap(v_uv - u_dir * 1.3846153846)) * 0.3162162162;\n"
"	c += (tap(v_uv + u_dir * 3.2307692308) + tap(v_uv - u_dir * 3.2307692308)) * 0.0702702703;\n"
"	gl_FragColor = vec4(c, 1.0);\n"
"}\n";

/* The final pass: downsample the scene (render scale), add the bloom,
 * apply gamma and contrast.
 */
constexpr char post_composite_fragment_source[] =
"#version 110\n"
"uniform sampler2D u_scene;\n"
"uniform sampler2D u_bloom;\n"
"uniform vec2 u_off;\n"
"uniform float u_taps4;\n"
"uniform vec2 u_bloom_scale;\n"
"uniform float u_bloom_strength;\n"
"uniform float u_gamma_inv;\n"
"uniform float u_contrast;\n"
"varying vec2 v_uv;\n"
"void main()\n"
"{\n"
"	vec3 c;\n"
"	if (u_taps4 > 0.5)\n"
"		c = 0.25 * (texture2D(u_scene, v_uv + vec2(-u_off.x, -u_off.y)).rgb + texture2D(u_scene, v_uv + vec2(u_off.x, -u_off.y)).rgb + texture2D(u_scene, v_uv + vec2(-u_off.x, u_off.y)).rgb + texture2D(u_scene, v_uv + u_off).rgb);\n"
"	else\n"
"		c = texture2D(u_scene, v_uv).rgb;\n"
"	if (u_bloom_strength > 0.0)\n"
"		c += texture2D(u_bloom, v_uv * u_bloom_scale).rgb * u_bloom_strength;\n"
"	c = clamp(c, 0.0, 1.0);\n"
"	if (u_gamma_inv != 1.0)\n"
"		c = pow(c, vec3(u_gamma_inv));\n"
"	if (u_contrast != 1.0)\n"
"		c = clamp((c - 0.5) * u_contrast + 0.5, 0.0, 1.0);\n"
"	gl_FragColor = vec4(c, 1.0);\n"
"}\n";

GLuint compile_shader(const GLenum type, const std::span<const char *const> sources, const char *const what)
{
	const GLuint s{gl.CreateShader(type)};
	if (!s)
		return 0;
	gl.ShaderSource(s, sources.size(), sources.data(), nullptr);
	gl.CompileShader(s);
	GLint ok{0};
	gl.GetShaderiv(s, GL_COMPILE_STATUS, &ok);
	if (!ok)
	{
		std::array<char, 1024> log{};
		gl.GetShaderInfoLog(s, log.size(), nullptr, log.data());
		con_printf(CON_URGENT, "DXX-Rebirth: OpenGL: the %s shader did not compile: %s", what, log.data());
		gl.DeleteShader(s);
		return 0;
	}
	return s;
}

/* 0 on failure; failures are not retried (see program_slot). */
GLuint link_program(const std::span<const char *const> vertex, const std::span<const char *const> fragment, const char *const what)
{
	const GLuint vs{compile_shader(GL_VERTEX_SHADER, vertex, what)};
	if (!vs)
		return 0;
	const GLuint fs{compile_shader(GL_FRAGMENT_SHADER, fragment, what)};
	if (!fs)
	{
		gl.DeleteShader(vs);
		return 0;
	}
	const GLuint p{gl.CreateProgram()};
	if (p)
	{
		gl.AttachShader(p, vs);
		gl.AttachShader(p, fs);
		gl.LinkProgram(p);
	}
	/* The program keeps the shaders while it lives. */
	gl.DeleteShader(vs);
	gl.DeleteShader(fs);
	if (!p)
		return 0;
	GLint ok{0};
	gl.GetProgramiv(p, GL_LINK_STATUS, &ok);
	if (!ok)
	{
		std::array<char, 1024> log{};
		gl.GetProgramInfoLog(p, log.size(), nullptr, log.data());
		con_printf(CON_URGENT, "DXX-Rebirth: OpenGL: the %s shader did not link: %s", what, log.data());
		return 0;
	}
	con_printf(CON_VERBOSE, "DXX-Rebirth: OpenGL: built the %s shader", what);
	return p;
}

/* A lazily built program: 0 = not tried, failed = tried and failed. */
struct program_slot
{
	GLuint program{0};
	bool failed{false};
};

struct world_program : program_slot
{
	GLint u_quad{-1}, u_corner{-1}, u_texsize{-1};
	/* The uniform values last set, to skip redundant calls. */
	float quad{-1};
	std::array<float, 2> texsize{};
};

struct post_programs
{
	program_slot down, blur, composite;
};

std::array<world_program, 2> world_programs;	/* [sharp] */
post_programs post;
GLuint world_program_current;

bool build_world_program(world_program &wp, const bool sharp)
{
	if (wp.program)
		return true;
	if (wp.failed)
		return false;
	const char *const vertex[]{world_vertex_source};
	const char *const fragment_sharp[]{"#version 110\n#extension GL_ARB_shader_texture_lod : require\n#define SHARP 1\n", world_fragment_body};
	const char *const fragment_plain[]{"#version 110\n", world_fragment_body};
	const auto p{link_program(vertex, sharp ? std::span<const char *const>(fragment_sharp) : std::span<const char *const>(fragment_plain), sharp ? "world (sharp pixels)" : "world")};
	if (!p)
	{
		wp.failed = true;
		return false;
	}
	wp.program = p;
	gl.UseProgram(p);
	gl.Uniform1i(gl.GetUniformLocation(p, "u_tex"), 0);
	wp.u_quad = gl.GetUniformLocation(p, "u_quad");
	wp.u_corner = gl.GetUniformLocation(p, "u_corner");
	wp.u_texsize = gl.GetUniformLocation(p, "u_texsize");
	gl.UseProgram(0);
	world_program_current = 0;
	ogl_world_shader_bound = false;
	return true;
}

bool build_post_program(program_slot &slot, const char *const fragment_source, const char *const what)
{
	if (slot.program)
		return true;
	if (slot.failed)
		return false;
	const char *const vertex[]{post_vertex_source};
	const char *const fragment[]{fragment_source};
	slot.program = link_program(vertex, fragment, what);
	if (!slot.program)
		slot.failed = true;
	return slot.program != 0;
}

world_program *bind_world_program(const bool sharp)
{
	auto &wp{world_programs[sharp]};
	if (!build_world_program(wp, sharp))
		return nullptr;
	if (world_program_current != wp.program)
	{
		gl.UseProgram(wp.program);
		world_program_current = wp.program;
	}
	ogl_world_shader_bound = true;
	return &wp;
}

void set_texsize(world_program &wp, const ogl_texture &tex)
{
	const std::array<float, 2> size{{static_cast<float>(tex.tw), static_cast<float>(tex.th)}};
	if (wp.texsize != size)
	{
		wp.texsize = size;
		gl.Uniform2f(wp.u_texsize, size[0], size[1]);
	}
}

void set_quad(world_program &wp, const float quad)
{
	if (wp.quad != quad)
	{
		wp.quad = quad;
		gl.Uniform1f(wp.u_quad, quad);
	}
}

/* The off-screen targets of the post-process pipeline.  They are made
 * for the whole screen at the render scale; a smaller view uses the
 * lower left part.
 */
struct post_targets
{
	unsigned screen_w{0}, screen_h{0};
	unsigned scale_percent{0};
	unsigned samples{0};
	unsigned w{0}, h{0};			/* scene size */
	GLuint scene_fbo{0}, scene_tex{0}, scene_depth{0};
	GLuint ms_fbo{0}, ms_color{0}, ms_depth{0};	/* when multisampled */
	/* Bloom: [0] half size, [1] and [2] quarter size (ping-pong). */
	std::array<GLuint, 3> bloom_fbo{}, bloom_tex{};
	std::array<unsigned, 3> bloom_w{}, bloom_h{};
	bool broken{false};
};

post_targets targets;

struct post_view
{
	bool active;
	bool world;
	unsigned x, y, w, h;	/* the canvas, in screen pixels (y from the top) */
	unsigned vw, vh;	/* its size in the scene target */
	float scale;
};

post_view view;

void delete_targets()
{
	auto &t{targets};
	if (t.scene_fbo)
		gl.DeleteFramebuffers(1, &t.scene_fbo);
	if (t.ms_fbo)
		gl.DeleteFramebuffers(1, &t.ms_fbo);
	if (t.scene_depth)
		gl.DeleteRenderbuffers(1, &t.scene_depth);
	if (t.ms_color)
		gl.DeleteRenderbuffers(1, &t.ms_color);
	if (t.ms_depth)
		gl.DeleteRenderbuffers(1, &t.ms_depth);
	if (t.scene_tex)
		glDeleteTextures(1, &t.scene_tex);
	for (auto &f : t.bloom_fbo)
		if (f)
			gl.DeleteFramebuffers(1, &f);
	for (auto &x : t.bloom_tex)
		if (x)
			glDeleteTextures(1, &x);
	const bool broken{t.broken};
	t = {};
	t.broken = broken;
}

GLuint make_color_texture(const unsigned w, const unsigned h)
{
	GLuint tex{0};
	glGenTextures(1, &tex);
	glBindTexture(GL_TEXTURE_2D, tex);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
	return tex;
}

bool framebuffer_complete(const char *const what)
{
	const auto status{gl.CheckFramebufferStatus(GL_FRAMEBUFFER)};
	if (status == GL_FRAMEBUFFER_COMPLETE)
		return true;
	con_printf(CON_URGENT, "DXX-Rebirth: OpenGL: the %s framebuffer is incomplete (0x%x); the post-process effects are off", what, static_cast<unsigned>(status));
	return false;
}

unsigned wanted_samples()
{
	unsigned s{CGameCfg.Multisample};
	if (s < 2 || !ogl_effects_caps.fbo_multisample)
		return 0;
	return std::min<unsigned>(s, std::max(ogl_effects_caps.max_samples, 0));
}

unsigned wanted_scale_percent()
{
	const unsigned s{CGameCfg.RenderScale};
	return s <= 100 ? 100 : std::min(s, 200u);
}

/* (Re)make the targets for the current screen and settings. */
bool ensure_targets()
{
	auto &t{targets};
	if (t.broken)
		return false;
	const unsigned sw{grd_curscreen->get_screen_width()}, sh{grd_curscreen->get_screen_height()};
	const unsigned scale{wanted_scale_percent()};
	const unsigned samples{wanted_samples()};
	if (t.scene_fbo && t.screen_w == sw && t.screen_h == sh && t.scale_percent == scale && t.samples == samples)
		return true;
	delete_targets();
	t.screen_w = sw;
	t.screen_h = sh;
	t.scale_percent = scale;
	t.samples = samples;
	t.w = (sw * scale + 99) / 100;
	t.h = (sh * scale + 99) / 100;

	t.scene_tex = make_color_texture(t.w, t.h);
	gl.GenFramebuffers(1, &t.scene_fbo);
	gl.BindFramebuffer(GL_FRAMEBUFFER, t.scene_fbo);
	gl.FramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, t.scene_tex, 0);
	bool ok;
	if (samples)
	{
		/* The scene is drawn into multisampled buffers and resolved
		 * into the texture.
		 */
		ok = framebuffer_complete("resolve");
		gl.GenRenderbuffers(1, &t.ms_color);
		gl.BindRenderbuffer(GL_RENDERBUFFER, t.ms_color);
		gl.RenderbufferStorageMultisample(GL_RENDERBUFFER, samples, GL_RGBA8, t.w, t.h);
		gl.GenRenderbuffers(1, &t.ms_depth);
		gl.BindRenderbuffer(GL_RENDERBUFFER, t.ms_depth);
		gl.RenderbufferStorageMultisample(GL_RENDERBUFFER, samples, GL_DEPTH_COMPONENT24, t.w, t.h);
		gl.GenFramebuffers(1, &t.ms_fbo);
		gl.BindFramebuffer(GL_FRAMEBUFFER, t.ms_fbo);
		gl.FramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, t.ms_color);
		gl.FramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, t.ms_depth);
		ok = ok && framebuffer_complete("multisampled scene");
	}
	else
	{
		gl.GenRenderbuffers(1, &t.scene_depth);
		gl.BindRenderbuffer(GL_RENDERBUFFER, t.scene_depth);
		gl.RenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, t.w, t.h);
		gl.FramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, t.scene_depth);
		ok = framebuffer_complete("scene");
	}
	gl.BindRenderbuffer(GL_RENDERBUFFER, 0);
	for (unsigned i = 0; ok && i < 3; ++i)
	{
		const unsigned div{i ? 4u : 2u};
		t.bloom_w[i] = std::max(1u, sw / div);
		t.bloom_h[i] = std::max(1u, sh / div);
		t.bloom_tex[i] = make_color_texture(t.bloom_w[i], t.bloom_h[i]);
		gl.GenFramebuffers(1, &t.bloom_fbo[i]);
		gl.BindFramebuffer(GL_FRAMEBUFFER, t.bloom_fbo[i]);
		gl.FramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, t.bloom_tex[i], 0);
		ok = framebuffer_complete("bloom");
	}
	gl.BindFramebuffer(GL_FRAMEBUFFER, 0);
	glBindTexture(GL_TEXTURE_2D, 0);
	if (!ok)
	{
		t.broken = true;
		delete_targets();
		return false;
	}
	con_printf(CON_VERBOSE, "DXX-Rebirth: OpenGL: post-process targets %ux%u, %u samples", t.w, t.h, samples);
	return true;
}

bool post_wanted(const bool world)
{
	if (!ogl_effects_caps.shaders || !ogl_effects_caps.fbo)
		return false;
	return wanted_samples() >= 2 ||
		wanted_scale_percent() > 100 ||
		(world && CGameCfg.Bloom) ||
		CGameCfg.GammaCurve != 100 ||
		CGameCfg.Contrast != 100;
}

/* Draw a quad over the whole target; uv covers [0,u1]x[0,v1]. */
void draw_target_quad(const float u1, const float v1)
{
	const std::array<GLfloat, 8> vertices{{0, 0, 1, 0, 1, 1, 0, 1}};
	const std::array<GLfloat, 8> texcoords{{0, 0, u1, 0, u1, v1, 0, v1}};
	glEnableClientState(GL_VERTEX_ARRAY);
	glEnableClientState(GL_TEXTURE_COORD_ARRAY);
	glVertexPointer(2, GL_FLOAT, 0, vertices.data());
	glTexCoordPointer(2, GL_FLOAT, 0, texcoords.data());
	glDrawArrays(GL_TRIANGLE_FAN, 0, 4);
	glDisableClientState(GL_TEXTURE_COORD_ARRAY);
	glDisableClientState(GL_VERTEX_ARRAY);
}

void set_uniform2(const GLuint p, const char *const name, const float a, const float b)
{
	gl.Uniform2f(gl.GetUniformLocation(p, name), a, b);
}

void set_uniform1(const GLuint p, const char *const name, const float a)
{
	gl.Uniform1f(gl.GetUniformLocation(p, name), a);
}

/* The bloom chain: scene -> half (bright pass) -> quarter -> blur H/V
 * twice.  Leaves the result in bloom_tex[1]; returns false if a
 * program is missing.
 */
bool run_bloom(const float scene_u1, const float scene_v1)
{
	auto &t{targets};
	if (!build_post_program(post.down, post_down_fragment_source, "bloom downsample") ||
		!build_post_program(post.blur, post_blur_fragment_source, "bloom blur"))
		return false;
	const unsigned hw{std::max(1u, view.w / 2)}, hh{std::max(1u, view.h / 2)};
	const unsigned qw{std::max(1u, view.w / 4)}, qh{std::max(1u, view.h / 4)};
	/* Bright pass into the half size target. */
	gl.UseProgram(post.down.program);
	gl.Uniform1i(gl.GetUniformLocation(post.down.program, "u_src"), 0);
	gl.BindFramebuffer(GL_FRAMEBUFFER, t.bloom_fbo[0]);
	glViewport(0, 0, hw, hh);
	glBindTexture(GL_TEXTURE_2D, t.scene_tex);
	/* A quarter of a destination pixel, in source uv. */
	set_uniform2(post.down.program, "u_off", 0.25f * scene_u1 / hw, 0.25f * scene_v1 / hh);
	set_uniform2(post.down.program, "u_max", scene_u1 - 0.5f / t.w, scene_v1 - 0.5f / t.h);
	/* Bloom 1..8: lower threshold for stronger settings. */
	const unsigned strength{std::min<unsigned>(CGameCfg.Bloom, 8)};
	set_uniform1(post.down.program, "u_threshold", 0.80f - 0.03f * strength);
	draw_target_quad(scene_u1, scene_v1);
	/* Half -> quarter. */
	const float h_u1{static_cast<float>(hw) / t.bloom_w[0]}, h_v1{static_cast<float>(hh) / t.bloom_h[0]};
	gl.BindFramebuffer(GL_FRAMEBUFFER, t.bloom_fbo[1]);
	glViewport(0, 0, qw, qh);
	glBindTexture(GL_TEXTURE_2D, t.bloom_tex[0]);
	set_uniform2(post.down.program, "u_off", 0.25f * h_u1 / qw, 0.25f * h_v1 / qh);
	set_uniform2(post.down.program, "u_max", h_u1 - 0.5f / t.bloom_w[0], h_v1 - 0.5f / t.bloom_h[0]);
	set_uniform1(post.down.program, "u_threshold", 0);
	draw_target_quad(h_u1, h_v1);
	/* Blur, ping-pong between [1] and [2]. */
	const float q_u1{static_cast<float>(qw) / t.bloom_w[1]}, q_v1{static_cast<float>(qh) / t.bloom_h[1]};
	gl.UseProgram(post.blur.program);
	gl.Uniform1i(gl.GetUniformLocation(post.blur.program, "u_src"), 0);
	set_uniform2(post.blur.program, "u_max", q_u1 - 0.5f / t.bloom_w[1], q_v1 - 0.5f / t.bloom_h[1]);
	const GLint u_dir{gl.GetUniformLocation(post.blur.program, "u_dir")};
	for (unsigned pass = 0; pass < 2; ++pass)
	{
		gl.BindFramebuffer(GL_FRAMEBUFFER, t.bloom_fbo[2]);
		glBindTexture(GL_TEXTURE_2D, t.bloom_tex[1]);
		gl.Uniform2f(u_dir, (pass + 1.0f) / t.bloom_w[1], 0);
		draw_target_quad(q_u1, q_v1);
		gl.BindFramebuffer(GL_FRAMEBUFFER, t.bloom_fbo[1]);
		glBindTexture(GL_TEXTURE_2D, t.bloom_tex[2]);
		gl.Uniform2f(u_dir, 0, (pass + 1.0f) / t.bloom_h[1]);
		draw_target_quad(q_u1, q_v1);
	}
	return true;
}

}

void ogl_effects_init()
{
	const auto version_str{reinterpret_cast<const char *>(glGetString(GL_VERSION))};
	if (!version_str)
		return;
	const auto extensions{reinterpret_cast<const char *>(glGetString(GL_EXTENSIONS))};
	const int major{std::atoi(version_str)};
	const char *const dot{std::strchr(version_str, '.')};
	const int minor{dot ? std::atoi(dot + 1) : 0};
	(void)minor;
	auto &c{ogl_effects_caps};
	const bool had_shaders{c.shaders};
	c = {};
	if (CGameArg.OglNoEffects)
	{
		con_puts(CON_VERBOSE, "DXX-Rebirth: OpenGL: effects disabled by -gl_noeffects");
		return;
	}
	/* GLSL 1.10 */
	if (major >= 2)
	{
		c.shaders =
			load_function(gl.CreateShader, "glCreateShader") &&
			load_function(gl.ShaderSource, "glShaderSource") &&
			load_function(gl.CompileShader, "glCompileShader") &&
			load_function(gl.GetShaderiv, "glGetShaderiv") &&
			load_function(gl.GetShaderInfoLog, "glGetShaderInfoLog") &&
			load_function(gl.DeleteShader, "glDeleteShader") &&
			load_function(gl.CreateProgram, "glCreateProgram") &&
			load_function(gl.AttachShader, "glAttachShader") &&
			load_function(gl.LinkProgram, "glLinkProgram") &&
			load_function(gl.GetProgramiv, "glGetProgramiv") &&
			load_function(gl.GetProgramInfoLog, "glGetProgramInfoLog") &&
			load_function(gl.UseProgram, "glUseProgram") &&
			load_function(gl.GetUniformLocation, "glGetUniformLocation") &&
			load_function(gl.Uniform1i, "glUniform1i") &&
			load_function(gl.Uniform1f, "glUniform1f") &&
			load_function(gl.Uniform2f, "glUniform2f") &&
			load_function(gl.Uniform4fv, "glUniform4fv") &&
			load_function(gl.ActiveTexture, "glActiveTexture", "ARB");
		c.texture_lod = c.shaders && has_extension(extensions, "GL_ARB_shader_texture_lod");
	}
	/* Framebuffer objects: core in 3.0 and ARB_framebuffer_object (same
	 * names), else the EXT extensions (names with "EXT").
	 */
	const bool core_fbo{major >= 3 || has_extension(extensions, "GL_ARB_framebuffer_object")};
	const bool ext_fbo{has_extension(extensions, "GL_EXT_framebuffer_object") && has_extension(extensions, "GL_EXT_framebuffer_blit")};
	if (core_fbo || ext_fbo)
	{
		const char *const sfx{core_fbo ? nullptr : "EXT"};
		const auto L{[sfx](auto &f, const char *const name) {
			return sfx ? load_function(f, (std::string(name) + sfx).c_str()) : load_function(f, name);
		}};
		c.fbo =
			L(gl.GenFramebuffers, "glGenFramebuffers") &&
			L(gl.DeleteFramebuffers, "glDeleteFramebuffers") &&
			L(gl.BindFramebuffer, "glBindFramebuffer") &&
			L(gl.FramebufferTexture2D, "glFramebufferTexture2D") &&
			L(gl.FramebufferRenderbuffer, "glFramebufferRenderbuffer") &&
			L(gl.CheckFramebufferStatus, "glCheckFramebufferStatus") &&
			L(gl.GenRenderbuffers, "glGenRenderbuffers") &&
			L(gl.DeleteRenderbuffers, "glDeleteRenderbuffers") &&
			L(gl.BindRenderbuffer, "glBindRenderbuffer") &&
			L(gl.RenderbufferStorage, "glRenderbufferStorage") &&
			L(gl.BlitFramebuffer, "glBlitFramebuffer");
		if (c.fbo && (core_fbo || has_extension(extensions, "GL_EXT_framebuffer_multisample")) &&
			L(gl.RenderbufferStorageMultisample, "glRenderbufferStorageMultisample"))
		{
			GLint max_samples{0};
			glGetIntegerv(GL_MAX_SAMPLES, &max_samples);
			c.max_samples = max_samples;
			c.fbo_multisample = max_samples >= 2;
		}
	}
#if SDL_MAJOR_VERSION == 1
	/* SDL 1 may make a new context for every video mode. */
	static_cast<void>(had_shaders);
	if (true)
#else
	if (!had_shaders)
#endif
		/* A new context (or the first): nothing built yet. */
		world_programs = {}, post = {}, targets = {}, world_program_current = 0, ogl_world_shader_bound = false;
	con_printf(CON_VERBOSE, "DXX-Rebirth: OpenGL: effects: shaders %s, sharp pixels %s, framebuffers %s, multisampling up to %ix",
		c.shaders ? "yes" : "no", c.texture_lod ? "yes" : "no", c.fbo ? "yes" : "no", c.fbo_multisample ? c.max_samples : 0);
}

bool ogl_sharp_pixels_active()
{
	return CGameCfg.TexFilt == opengl_texture_filter::sharp && ogl_effects_caps.texture_lod && !world_programs[1].failed;
}

bool ogl_smooth_lighting_active()
{
	return CGameCfg.SmoothLighting && ogl_effects_caps.shaders && !world_programs[0].failed;
}

void ogl_world_shader_unbind()
{
	gl.UseProgram(0);
	world_program_current = 0;
	ogl_world_shader_bound = false;
}

bool ogl_world_shader_quad(const ogl_texture &tex, const std::array<std::array<float, 4>, 4> &corners)
{
	if (!ogl_smooth_lighting_active())
	{
		ogl_world_shader_off();
		return false;
	}
	const bool sharp{ogl_sharp_pixels_active()};
	const auto wp{bind_world_program(sharp)};
	if (!wp)
	{
		ogl_world_shader_off();
		return false;
	}
	set_quad(*wp, 1);
	if (sharp)
		set_texsize(*wp, tex);
	gl.Uniform4fv(wp->u_corner, 4, corners[0].data());
	return true;
}

bool ogl_world_shader_plain(const ogl_texture &tex)
{
	if (!ogl_sharp_pixels_active())
	{
		ogl_world_shader_off();
		return false;
	}
	const auto wp{bind_world_program(true)};
	if (!wp)
	{
		ogl_world_shader_off();
		return false;
	}
	set_quad(*wp, 0);
	set_texsize(*wp, tex);
	return true;
}

bool ogl_post_begin(const grs_canvas &canvas, const bool world)
{
	view.active = false;
	if (!post_wanted(world) || !ensure_targets())
		return false;
	ogl_world_shader_off();
	auto &t{targets};
	const auto &bm{canvas.cv_bitmap};
	view.world = world;
	view.x = bm.bm_x;
	view.y = bm.bm_y;
	view.w = std::min<unsigned>(bm.bm_w, t.screen_w);
	view.h = std::min<unsigned>(bm.bm_h, t.screen_h);
	if (!view.w || !view.h)
		return false;
	view.scale = t.scale_percent / 100.0f;
	view.vw = std::min<unsigned>(t.w, static_cast<unsigned>(std::lround(view.w * view.scale)));
	view.vh = std::min<unsigned>(t.h, static_cast<unsigned>(std::lround(view.h * view.scale)));
	gl.BindFramebuffer(GL_FRAMEBUFFER, t.samples ? t.ms_fbo : t.scene_fbo);
	glViewport(0, 0, view.vw, view.vh);
	/* OGL_VIEWPORT must set the viewport again after the view. */
	last_width = last_height = ~0u;
	glClearColor(0, 0, 0, 0);
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
	view.active = true;
	return true;
}

bool ogl_post_active()
{
	return view.active;
}

void ogl_post_set_viewport()
{
	glViewport(0, 0, view.vw, view.vh);
	last_width = last_height = ~0u;
}

float ogl_post_scale()
{
	return view.active ? view.scale : 1.0f;
}

void ogl_post_end()
{
	if (!view.active)
		return;
	view.active = false;
	ogl_world_shader_off();
	auto &t{targets};
	if (t.samples)
	{
		gl.BindFramebuffer(GL_READ_FRAMEBUFFER, t.ms_fbo);
		gl.BindFramebuffer(GL_DRAW_FRAMEBUFFER, t.scene_fbo);
		gl.BlitFramebuffer(0, 0, view.vw, view.vh, 0, 0, view.vw, view.vh, GL_COLOR_BUFFER_BIT, GL_NEAREST);
	}
	glPushAttrib(GL_ENABLE_BIT | GL_COLOR_BUFFER_BIT | GL_TEXTURE_BIT | GL_VIEWPORT_BIT | GL_DEPTH_BUFFER_BIT);
	glDisable(GL_BLEND);
	glDisable(GL_ALPHA_TEST);
	glDisable(GL_DEPTH_TEST);
	glDisable(GL_CULL_FACE);
	glDepthMask(GL_FALSE);
	gl.ActiveTexture(GL_TEXTURE0);
	glEnable(GL_TEXTURE_2D);

	const float scene_u1{static_cast<float>(view.vw) / t.w}, scene_v1{static_cast<float>(view.vh) / t.h};
	const bool bloom{view.world && CGameCfg.Bloom && run_bloom(scene_u1, scene_v1)};

	gl.BindFramebuffer(GL_FRAMEBUFFER, 0);
	const unsigned screen_h{grd_curscreen->get_screen_height()};
	glViewport(view.x, screen_h - view.y - view.h, view.w, view.h);
	if (build_post_program(post.composite, post_composite_fragment_source, "post-process composite"))
	{
		const GLuint p{post.composite.program};
		gl.UseProgram(p);
		gl.Uniform1i(gl.GetUniformLocation(p, "u_scene"), 0);
		gl.Uniform1i(gl.GetUniformLocation(p, "u_bloom"), 1);
		glBindTexture(GL_TEXTURE_2D, t.scene_tex);
		const bool taps4{view.scale > 1.01f};
		set_uniform1(p, "u_taps4", taps4 ? 1.0f : 0.0f);
		set_uniform2(p, "u_off", 0.25f * scene_u1 / view.w, 0.25f * scene_v1 / view.h);
		if (bloom)
		{
			gl.ActiveTexture(GL_TEXTURE1);
			glBindTexture(GL_TEXTURE_2D, t.bloom_tex[1]);
			gl.ActiveTexture(GL_TEXTURE0);
			/* scene uv -> quarter target uv */
			const unsigned qw{std::max(1u, view.w / 4)}, qh{std::max(1u, view.h / 4)};
			set_uniform2(p, "u_bloom_scale", (static_cast<float>(qw) / t.bloom_w[1]) / scene_u1, (static_cast<float>(qh) / t.bloom_h[1]) / scene_v1);
			set_uniform1(p, "u_bloom_strength", 0.25f + 0.15f * std::min<unsigned>(CGameCfg.Bloom, 8));
		}
		else
			set_uniform1(p, "u_bloom_strength", 0);
		set_uniform1(p, "u_gamma_inv", 100.0f / std::clamp<unsigned>(CGameCfg.GammaCurve, 50, 200));
		set_uniform1(p, "u_contrast", std::clamp<unsigned>(CGameCfg.Contrast, 50, 200) / 100.0f);
		draw_target_quad(scene_u1, scene_v1);
		if (bloom)
		{
			gl.ActiveTexture(GL_TEXTURE1);
			glBindTexture(GL_TEXTURE_2D, 0);
			gl.ActiveTexture(GL_TEXTURE0);
		}
		gl.UseProgram(0);
	}
	else
	{
		/* No composite shader: copy the view as it is. */
		gl.BindFramebuffer(GL_READ_FRAMEBUFFER, t.scene_fbo);
		gl.BindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
		gl.BlitFramebuffer(0, 0, view.vw, view.vh, view.x, screen_h - view.y - view.h, view.x + view.w, screen_h - view.y, GL_COLOR_BUFFER_BIT, GL_LINEAR);
		gl.BindFramebuffer(GL_FRAMEBUFFER, 0);
	}
	world_program_current = 0;
	glPopAttrib();
	/* The rest of the frame (HUD, cockpit) draws on the whole screen. */
	const unsigned screen_w{grd_curscreen->get_screen_width()};
	glViewport(0, 0, screen_w, screen_h);
	last_width = screen_w;
	last_height = screen_h;
}

}

#endif
