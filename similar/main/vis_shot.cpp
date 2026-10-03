/*
 * This file is part of the DXX-Rebirth project <https://github.com/dxx-rebirth/dxx-rebirth/>.
 * It is copyright by its individual contributors, as recorded in the
 * project's Git history.  See COPYING.txt at the top level for license
 * terms and a link to the Git history.
 */
/*
 * -visshot: pictures and frame times of the visual quality presets
 * (vis_shot.h).
 */

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <memory>
#include <string>
#include <string_view>
#include <vector>
#include "vis_shot.h"
#include "config.h"
#include "console.h"
#include "fireball.h"
#include "game.h"
#include "gameseg.h"
#include "gameseq.h"
#include "hudmsg.h"
#include "kconfig.h"
#include "laser.h"
#include "mission.h"
#include "object.h"
#include "player.h"
#include "playsave.h"
#include "segment.h"
#include "timer.h"
#include "vclip.h"
#include "d_levelstate.h"
#include "gr.h"
#if DXX_USE_OGL
#include "ogl_init.h"
#include "ogl_effects.h"
#endif

namespace dsx {

namespace {

int exit_status = 1;

#if DXX_USE_OGL
struct vis_preset
{
	const char *name;
	const char *description;
	opengl_texture_filter filter;
	uint8_t aniso, samples, scale, bloom, gamma, contrast;
	bool smooth_lighting;
};

/* Each option alone against "original", then the combinations. */
constexpr std::array<vis_preset, 12> presets{{
	{"original", "Classic textures, no effects (the look before this branch)", opengl_texture_filter::classic, 0, 0, 100, 0, 100, 100, false},
	{"smooth", "Texture filter Smooth (trilinear), no anisotropic filtering", opengl_texture_filter::trilinear, 0, 0, 100, 0, 100, 100, false},
	{"aniso16", "Smooth + anisotropic filtering 16x", opengl_texture_filter::trilinear, 16, 0, 100, 0, 100, 100, false},
	{"blocky", "Blocky Filtered (CPU 4x upscale) + anisotropic 16x", opengl_texture_filter::upscale, 16, 0, 100, 0, 100, 100, false},
	{"sharp", "Sharp Pixels (shader) + anisotropic 16x", opengl_texture_filter::sharp, 16, 0, 100, 0, 100, 100, false},
	{"msaa4", "Classic + anti-aliasing MSAA 4x", opengl_texture_filter::classic, 0, 4, 100, 0, 100, 100, false},
	{"scale200", "Classic + render scale 200% (supersampling)", opengl_texture_filter::classic, 0, 0, 200, 0, 100, 100, false},
	{"smoothlight", "Classic + smooth (per-pixel bilinear) lighting", opengl_texture_filter::classic, 0, 0, 100, 0, 100, 100, true},
	{"bloom", "Classic + bloom 3", opengl_texture_filter::classic, 0, 0, 100, 3, 100, 100, false},
	{"gamma", "Classic + gamma 1.15, contrast 1.10", opengl_texture_filter::classic, 0, 0, 100, 0, 115, 110, false},
	{"recommended", "Sharp Pixels, aniso 16x, MSAA 4x, smooth lighting, bloom 3", opengl_texture_filter::sharp, 16, 4, 100, 3, 100, 100, true},
	{"max", "Sharp Pixels, aniso 16x, MSAA 8x, render scale 200%, smooth lighting, bloom 3", opengl_texture_filter::sharp, 16, 8, 200, 3, 100, 100, true},
}};

struct viewpoint
{
	vms_vector pos;
	vms_matrix orient;
	segnum_t segnum;
	vms_vector dir;	/* unit, view direction */
	double run;	/* length of the straight view, units */
};

unsigned frames_seen;

double length(const vms_vector &v)
{
	const double x{f2fl(v.x)}, y{f2fl(v.y)}, z{f2fl(v.z)};
	return std::sqrt(x * x + y * y + z * z);
}

vms_vector scaled(const vms_vector &v, const double f)
{
	return {fl2f(f2fl(v.x) * f), fl2f(f2fl(v.y) * f), fl2f(f2fl(v.z) * f)};
}

/* Long straight views: from a segment through one side, follow the
 * children that keep the direction.
 */
std::vector<viewpoint> choose_viewpoints(const unsigned wanted)
{
	auto &LevelSharedVertexState = LevelSharedSegmentState.get_vertex_state();
	auto &Vertices = LevelSharedVertexState.get_vertices();
	auto &vcvertptr = Vertices.vcptr;
	const std::size_t count{static_cast<std::size_t>(Segments.get_count())};
	std::vector<vms_vector> centre(count);
	for (const auto &&segp : vcsegptridx)
		if (const std::size_t n{segp.get_unchecked_index()}; n < count)
			centre[n] = compute_segment_center(vcvertptr, segp);
	std::vector<viewpoint> candidates;
	for (const auto &&segp : vcsegptridx)
	{
		const std::size_t n{segp.get_unchecked_index()};
		if (n >= count)
			continue;
		for (const auto side : MAX_SIDES_PER_SEGMENT)
		{
			const auto child{segp->shared_segment::children[side]};
			if (!IS_CHILD(child) || child >= count)
				continue;
			auto d{vm_vec_build_sub(centre[child], centre[n])};
			const double first{length(d)};
			if (first < 1)
				continue;
			const auto dir{scaled(d, 1 / first)};
			double run{first};
			std::size_t cur{child};
			for (unsigned step = 0; step < 12; ++step)
			{
				double best_dot{0.92};
				std::size_t best{count};
				double best_len{0};
				for (const auto s2 : MAX_SIDES_PER_SEGMENT)
				{
					const auto c2{vcsegptr(static_cast<segnum_t>(cur))->children[s2]};
					if (!IS_CHILD(c2) || c2 >= count)
						continue;
					const auto d2{vm_vec_build_sub(centre[c2], centre[cur])};
					const double l2{length(d2)};
					if (l2 < 1)
						continue;
					const double dot{(f2fl(d2.x) * f2fl(dir.x) + f2fl(d2.y) * f2fl(dir.y) + f2fl(d2.z) * f2fl(dir.z)) / l2};
					if (dot > best_dot)
						best_dot = dot, best = c2, best_len = l2;
				}
				if (best == count)
					break;
				run += best_len;
				cur = best;
			}
			viewpoint vp;
			vp.segnum = static_cast<segnum_t>(n);
			vp.dir = dir;
			vp.run = run;
			/* Back a little from the centre, inside the segment. */
			vp.pos = vm_vec_build_sub(centre[n], scaled(d, 0.25));
			vp.orient = vm_vector_to_matrix(dir);
			candidates.push_back(vp);
		}
	}
	std::sort(candidates.begin(), candidates.end(), [](const viewpoint &a, const viewpoint &b) { return a.run > b.run; });
	std::vector<viewpoint> chosen;
	for (const auto &c : candidates)
	{
		if (chosen.size() >= wanted)
			break;
		bool distant{true};
		for (const auto &o : chosen)
			if (length(vm_vec_build_sub(c.pos, o.pos)) < 120)
				distant = false;
		if (distant)
			chosen.push_back(c);
	}
	return chosen;
}

/* Things that glow: an explosion, a flare and some shots ahead. */
void spawn_effects(const viewpoint &vp)
{
	auto &Objects = LevelUniqueObjectState.Objects;
	const double ahead{std::min(vp.run * 0.35, 32.0)};
	const auto seg_of{[](const vms_vector &p, const segnum_t start) {
		return find_point_seg(LevelSharedSegmentState, LevelUniqueSegmentState, p, Segments.vmptridx(start) DXX_lighting_hack_pass_parameter);
	}};
	const auto p_expl{vm_vec_build_add(vp.pos, scaled(vp.dir, ahead))};
	if (const auto s{seg_of(p_expl, vp.segnum)}; s != segment_none)
		object_create_explosion_without_damage(Vclip, s, p_expl, i2f(7), vclip_index::small_explosion);
	const auto console{Objects.vmptridx(ConsoleObject)};
	/* Units ahead, near enough to be seen. */
	const std::array<std::pair<double, weapon_id_type>, 4> shots{{
		{12, weapon_id_type::FLARE_ID},
		{16, weapon_id_type::PLASMA_ID},
		{22, weapon_id_type::LASER_ID_L4},
		{28, weapon_id_type::FUSION_ID},
	}};
	const auto side{vp.orient.rvec};
	for (std::size_t i = 0; i < shots.size(); ++i)
	{
		const auto &[f, id] = shots[i];
		const double off{i % 2 ? -4.0 : 4.0};
		const auto p{vm_vec_build_add(vm_vec_build_add(vp.pos, scaled(vp.dir, std::min(vp.run * 0.8, f))), scaled(side, off))};
		if (const auto s{seg_of(p, vp.segnum)}; s != segment_none)
			Laser_create_new(vp.dir, p, s, console, id, weapon_sound_flag::silent);
	}
}

void place_camera(const vms_vector &pos, const vms_matrix &orient, const segnum_t segnum)
{
	auto &Objects = LevelUniqueObjectState.Objects;
	const auto console{Objects.vmptridx(ConsoleObject)};
	console->pos = pos;
	console->orient = orient;
	const auto s{find_point_seg(LevelSharedSegmentState, LevelUniqueSegmentState, pos, Segments.vmptridx(segnum) DXX_lighting_hack_pass_parameter)};
	if (s != segment_none)
		obj_relink(Objects.vmptr, Segments.vmptr, console, s);
	else
		obj_relink(Objects.vmptr, Segments.vmptr, console, Segments.vmptridx(segnum));
	Viewer = ConsoleObject;
}

/* Every picture at the same game time, so that each preset sees the same
 * flicker and glow; a new GameTime64 and a FrameTime of 1/60 s make
 * set_dynamic_light compute the light of the new view.
 */
fix64 picture_time;

void render_one(const unsigned step)
{
	GameTime64 = picture_time + step;
	FrameTime = F1_0 / 60;
	HUD_clear_messages();
	game_render_frame(LevelSharedRobotInfoState.Robot_info, Controls);
}

bool write_ppm(const std::string &path, const unsigned w, const unsigned h)
{
	std::vector<unsigned char> buf(static_cast<std::size_t>(w) * h * 3);
	glPixelStorei(GL_PACK_ALIGNMENT, 1);
	glReadPixels(0, 0, w, h, GL_RGB, GL_UNSIGNED_BYTE, buf.data());
	const std::unique_ptr<FILE, int (*)(FILE *)> f{std::fopen(path.c_str(), "wb"), &std::fclose};
	if (!f)
	{
		con_printf(CON_URGENT, "visshot: cannot write %s", path.c_str());
		return false;
	}
	glPixelStorei(GL_PACK_ALIGNMENT, 4);
	std::fprintf(f.get(), "P6\n%u %u\n255\n", w, h);
	for (unsigned y = h; y-- > 0;)
		std::fwrite(&buf[static_cast<std::size_t>(y) * w * 3], 1, static_cast<std::size_t>(w) * 3, f.get());
	return true;
}

/* Returns whether the textures must be loaded again. */
bool apply_preset(const vis_preset &p)
{
	const bool reload{CGameCfg.TexFilt != p.filter || CGameCfg.TexAnisotropy != p.aniso};
	CGameCfg.TexFilt = p.filter;
	CGameCfg.TexAnisotropy = p.aniso;
	CGameCfg.Multisample = p.samples;
	CGameCfg.RenderScale = p.scale;
	CGameCfg.Bloom = p.bloom;
	CGameCfg.GammaCurve = p.gamma;
	CGameCfg.Contrast = p.contrast;
	CGameCfg.SmoothLighting = p.smooth_lighting;
	return reload;
}

bool preset_wanted(const std::string_view name)
{
	const std::string_view list{CGameArg.DbgVisShotPresets};
	if (list.empty())
		return true;
	std::size_t start{0};
	while (start <= list.size())
	{
		const auto comma{list.find(',', start)};
		const auto item{list.substr(start, comma == list.npos ? list.npos : comma - start)};
		if (item == name)
			return true;
		if (comma == list.npos)
			break;
		start = comma + 1;
	}
	return false;
}

void take_pictures()
{
	const std::string dir{CGameArg.DbgVisShotDir};
	const unsigned w{grd_curscreen->get_screen_width()}, h{grd_curscreen->get_screen_height()};
	auto &Objects = LevelUniqueObjectState.Objects;
	const auto console{Objects.vmptr(ConsoleObject)};
	std::vector<viewpoint> vps;
	{
		viewpoint start;
		start.pos = console->pos;
		start.orient = console->orient;
		start.segnum = console->segnum;
		start.dir = console->orient.fvec;
		start.run = 60;
		vps.push_back(start);
	}
	for (const auto &vp : choose_viewpoints(3))
		vps.push_back(vp);
	for (const auto &vp : vps)
		spawn_effects(vp);
	con_printf(CON_URGENT, "visshot: %u viewpoints, %ux%u, effects: shaders %d, sharp %d, fbo %d, max samples %d",
		static_cast<unsigned>(vps.size()), w, h, ogl_effects_caps.shaders, ogl_effects_caps.texture_lod, ogl_effects_caps.fbo, ogl_effects_caps.max_samples);

	const std::unique_ptr<FILE, int (*)(FILE *)> timing{std::fopen((dir + "/timing.txt").c_str(), "w"), &std::fclose};
	const unsigned frames{CGameArg.DbgVisShotFrames ? CGameArg.DbgVisShotFrames : 60u};
	picture_time = GameTime64;
	if (timing)
		std::fprintf(timing.get(), "# preset ms_per_frame fps (%u frames per viewpoint, %u viewpoints, %ux%u) | description\n", frames, static_cast<unsigned>(vps.size()), w, h);
	for (const auto &p : presets)
	{
		if (!preset_wanted(p.name))
			continue;
		if (apply_preset(p))
		{
			gr_set_attributes();
			gr_set_mode(Game_screen_mode);
		}
		reset_cockpit();
		double total_ms{0};
		for (std::size_t i = 0; i < vps.size(); ++i)
		{
			const auto &vp{vps[i]};
			place_camera(vp.pos, vp.orient, vp.segnum);
			/* The first frames load textures, build shaders and let the
			 * light of the new view settle.
			 */
			for (unsigned step = 1; step < 4; ++step)
			{
				render_one(step);
				gr_flip();
			}
			render_one(4);
			glFinish();
			std::array<char, 64> name;
			std::snprintf(name.data(), name.size(), "/vp%u-%s.ppm", static_cast<unsigned>(i), p.name);
			write_ppm(dir + name.data(), w, h);
			gr_flip();
			glFinish();
			const auto t0{std::chrono::steady_clock::now()};
			for (unsigned f = 0; f < frames; ++f)
			{
				render_one(5 + f);
				gr_flip();
			}
			glFinish();
			total_ms += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
		}
		const double ms{total_ms / (frames * vps.size())};
		con_printf(CON_URGENT, "visshot: %-12s %7.3f ms/frame (%6.0f fps)  %s", p.name, ms, 1000 / ms, p.description);
		if (timing)
			std::fprintf(timing.get(), "%s %.3f %.0f | %s\n", p.name, ms, 1000 / ms, p.description);
	}
}
#endif

}

bool vis_shot_start()
{
	if (!InterfaceUniqueState.PilotName[0u])
	{
		new_player_config();
		InterfaceUniqueState.PilotName.copy(std::span<const char>("visshot", 7));
	}
	PlayerCfg.CockpitMode[0] = PlayerCfg.CockpitMode[1] = cockpit_mode_t::full_screen;
	CGameCfg.FPSIndicator = false;
	const auto &mission{CGameArg.DbgVisShotMission};
	if (const auto err{load_mission_by_file_or_title(mission.c_str())})
	{
		con_printf(CON_URGENT, "visshot: mission \"%s\": %s", mission.c_str(), err);
		return false;
	}
	const unsigned level{CGameArg.DbgVisShotLevel};
	if (level < 1 || level > static_cast<unsigned>(Current_mission->last_level))
	{
		con_printf(CON_URGENT, "visshot: mission \"%s\" has %u levels, not %u", Current_mission->mission_name.data(), static_cast<unsigned>(Current_mission->last_level), level);
		return false;
	}
	GameUniqueState.Difficulty_level = Difficulty_level_type::_0;
	/* The same game every run: a simulated clock (the level's random
	 * seed comes from it) at 100 frames per second.
	 */
	timer_use_simulated_clock(F1_0, F1_0 / 100);
	StartNewGame(level);
	return true;
}

window_event_result vis_shot_frame()
{
#if DXX_USE_OGL
	if (++frames_seen < 4)
		return window_event_result::ignored;
	take_pictures();
	exit_status = 0;
#else
	con_puts(CON_URGENT, "visshot: needs an OpenGL build");
#endif
	return window_event_result::close;
}

int vis_shot_exit_status()
{
	return exit_status;
}

}
