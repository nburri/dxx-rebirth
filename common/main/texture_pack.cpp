/*
 * This file is part of the DXX-Rebirth project <https://github.com/dxx-rebirth/dxx-rebirth/>.
 * It is copyright by its individual contributors, as recorded in the
 * project's Git history.  See COPYING.txt at the top level for license
 * terms and a link to the Git history.
 */
/*
 * Texture packs: names and image operations (texture_pack.h).
 */

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstddef>
#include "texture_pack.h"

namespace dcx::texture_pack {

namespace {

char lower(const char c)
{
	return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

std::string lowered(const std::string_view s)
{
	std::string r{s};
	std::transform(r.begin(), r.end(), r.begin(), lower);
	return r;
}

/* Names come from game and mission files: refuse anything that is not
 * a plain file name.
 */
bool usable_name(const std::string_view s)
{
	if (s.empty() || s == "." || s.find("..") != s.npos)
		return false;
	for (const char c : s)
		if (c == '/' || c == '\\' || c == ':' || static_cast<unsigned char>(c) < 32)
			return false;
	return true;
}

void add_unique(std::vector<std::string> &v, std::string s)
{
	if (std::find(v.begin(), v.end(), s) == v.end())
		v.push_back(std::move(s));
}

std::size_t offset(const rgba_image &img, const unsigned x, const unsigned y)
{
	return (static_cast<std::size_t>(y) * img.w + x) * 4;
}

bool has_holes(const rgba_image &img)
{
	for (std::size_t i = 0; i + 3 < img.px.size(); i += 4)
		if (is_hole(&img.px[i]))
			return true;
	return false;
}

rgba_image scale_bilinear_wrap(const rgba_image &in, const unsigned w, const unsigned h)
{
	rgba_image out;
	out.w = w;
	out.h = h;
	out.px.resize(static_cast<std::size_t>(w) * h * 4);
	const double fx{static_cast<double>(in.w) / w}, fy{static_cast<double>(in.h) / h};
	for (unsigned y = 0; y < h; ++y)
	{
		const double sy{(y + 0.5) * fy - 0.5};
		const double y0f{std::floor(sy)};
		const double ty{sy - y0f};
		const unsigned y0{static_cast<unsigned>((static_cast<long>(y0f) % static_cast<long>(in.h) + in.h) % in.h)};
		const unsigned y1{(y0 + 1) % in.h};
		for (unsigned x = 0; x < w; ++x)
		{
			const double sx{(x + 0.5) * fx - 0.5};
			const double x0f{std::floor(sx)};
			const double tx{sx - x0f};
			const unsigned x0{static_cast<unsigned>((static_cast<long>(x0f) % static_cast<long>(in.w) + in.w) % in.w)};
			const unsigned x1{(x0 + 1) % in.w};
			const uint8_t *const p[4]{&in.px[offset(in, x0, y0)], &in.px[offset(in, x1, y0)], &in.px[offset(in, x0, y1)], &in.px[offset(in, x1, y1)]};
			const double wt[4]{(1 - tx) * (1 - ty), tx * (1 - ty), (1 - tx) * ty, tx * ty};
			/* Premultiplied, so that transparent texels do not tint
			 * their neighbours.
			 */
			double a{0}, c[3]{0, 0, 0};
			for (unsigned k = 0; k < 4; ++k)
			{
				const double pa{p[k][3] * wt[k]};
				a += pa;
				for (unsigned j = 0; j < 3; ++j)
					c[j] += p[k][j] * pa;
			}
			uint8_t *const o{&out.px[offset(out, x, y)]};
			if (a > 0)
				for (unsigned j = 0; j < 3; ++j)
					o[j] = static_cast<uint8_t>(std::clamp(c[j] / a + 0.5, 0.0, 255.0));
			else
				for (unsigned j = 0; j < 3; ++j)
					o[j] = p[0][j];
			o[3] = static_cast<uint8_t>(std::clamp(a + 0.5, 0.0, 255.0));
		}
	}
	return out;
}

}

std::string mission_directory(const std::string_view mission_filename)
{
	auto name{mission_filename};
	if (const auto slash{name.find_last_of("/\\")}; slash != name.npos)
		name.remove_prefix(slash + 1);
	if (const auto dot{name.rfind('.')}; dot != name.npos && dot > 0)
		name = name.substr(0, dot);
	if (!usable_name(name))
		return {};
	return lowered(name);
}

std::vector<std::string> candidate_paths(const std::string_view bitmap_name, const std::string_view mission_dir, const bool mission_custom)
{
	std::vector<std::string> r;
	if (!usable_name(bitmap_name))
		return r;
	const std::string name{bitmap_name}, lname{lowered(bitmap_name)};
	if (!mission_dir.empty() && usable_name(mission_dir))
	{
		const std::string dir{"textures/" + std::string{mission_dir} + "/"};
		add_unique(r, dir + name + ".png");
		add_unique(r, dir + lname + ".png");
	}
	if (!mission_custom)
	{
		add_unique(r, "textures/" + name + ".png");
		add_unique(r, "textures/" + lname + ".png");
	}
	return r;
}

rgba_image scale_nearest(const rgba_image &in, const unsigned w, const unsigned h)
{
	if (in.w == w && in.h == h)
		return in;
	rgba_image out;
	out.w = w;
	out.h = h;
	out.px.resize(static_cast<std::size_t>(w) * h * 4);
	for (unsigned y = 0; y < h; ++y)
	{
		const unsigned sy{static_cast<unsigned>(static_cast<uint64_t>(y) * in.h / h)};
		for (unsigned x = 0; x < w; ++x)
		{
			const unsigned sx{static_cast<unsigned>(static_cast<uint64_t>(x) * in.w / w)};
			std::copy_n(&in.px[offset(in, sx, sy)], 4, &out.px[offset(out, x, y)]);
		}
	}
	return out;
}

rgba_image to_power_of_two(const rgba_image &in, const unsigned max_size)
{
	if (in.empty())
		return {};
	const unsigned cap{std::bit_floor(std::max(max_size, 1u))};
	const unsigned w{std::min(std::bit_ceil(in.w), cap)}, h{std::min(std::bit_ceil(in.h), cap)};
	if (w == in.w && h == in.h)
		return in;
	return has_holes(in) ? scale_nearest(in, w, h) : scale_bilinear_wrap(in, w, h);
}

rgba_image composite(const rgba_image &base_in, const rgba_image &top_in, const unsigned orient, const bool supertransparent)
{
	if (base_in.empty() || top_in.empty())
		return {};
	const unsigned s{std::max({base_in.w, base_in.h, top_in.w, top_in.h})};
	const auto base{scale_nearest(base_in, s, s)};
	const auto top{scale_nearest(top_in, s, s)};
	rgba_image out;
	out.w = out.h = s;
	out.px.resize(static_cast<std::size_t>(s) * s * 4);
	const unsigned m{s - 1};
	for (unsigned y = 0; y < s; ++y)
		for (unsigned x = 0; x < s; ++x)
		{
			/* The same rotation as texmerge.cpp merge_texture_N. */
			unsigned tx, ty;
			switch (orient & 3)
			{
				default:
					ty = y, tx = x;
					break;
				case 1:
					ty = x, tx = m - y;
					break;
				case 2:
					ty = m - y, tx = m - x;
					break;
				case 3:
					ty = m - x, tx = y;
					break;
			}
			const uint8_t *const t{&top.px[offset(top, tx, ty)]};
			const uint8_t *const b{&base.px[offset(base, x, y)]};
			uint8_t *const o{&out.px[offset(out, x, y)]};
			if (is_hole(t))
			{
				/* The base's color, so that filtering does not bleed
				 * magenta into the edges of the hole.
				 */
				std::copy_n(b, 3, o);
				o[3] = supertransparent ? 0 : b[3];
				continue;
			}
			const unsigned ta{t[3]}, ba{b[3]};
			const unsigned oa{ta + ba * (255 - ta) / 255};
			for (unsigned j = 0; j < 3; ++j)
				o[j] = oa ? static_cast<uint8_t>((t[j] * ta + b[j] * ba * (255 - ta) / 255 + oa / 2) / oa) : b[j];
			o[3] = static_cast<uint8_t>(oa);
		}
	return out;
}

}
