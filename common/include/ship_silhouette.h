/*
 * This file is part of the DXX-Rebirth project <https://github.com/dxx-rebirth/dxx-rebirth/>.
 * It is copyright by its individual contributors, as recorded in the
 * project's Git history.  See COPYING.txt at the top level for license
 * terms and a link to the Git history.
 */
/*
 * The size rule of custom ships (decision D3 of
 * Documentation/custom-ships.md): a ship's mean silhouette, the area of
 * its outline averaged over view directions all round, is made equal to
 * the Pyro-GX's, while its outermost point stays within the radius the
 * game's reader accepts.  Used by the converter (common/tools/shipconv.cpp)
 * and its test.
 */

#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "dxship_format.h"

namespace dcx::ship_silhouette {

/* The Pyro-GX's mean silhouette in square units, measured with
 * `mean_area` below from model 108 of descent2.ham.  Numbers only; no
 * retail geometry is used.
 */
constexpr double PYRO_MEAN_AREA{23.548};

/* The band the converter aims for, and the floor below which a ship is
 * reported as too small to be fair.
 */
constexpr double BAND_LOW{0.9};
constexpr double BAND_HIGH{1.1};
constexpr double FLOOR{0.85};

/* The outermost point should be at most RADIUS_CAP Pyro radii from the
 * centre (the collision sphere stays the Pyro's).  A ship that is still
 * below FLOOR there grows further, up to RADIUS_LIMIT, just inside the
 * reader's limit (RADIUS_TOLERANCE).
 */
constexpr double RADIUS_CAP{1.3};
constexpr double RADIUS_LIMIT{::dcx::dxship::RADIUS_TOLERANCE * 0.98};

/* View directions: a Fibonacci spiral over a hemisphere (a silhouette
 * seen from behind is the mirror image of the one from the front).
 */
constexpr unsigned DIRECTIONS{64};
/* Grid cells across the ship's diameter, in each view. */
constexpr unsigned GRID{160};

using vec3 = ::dcx::dxship::vec3;
using triangle = std::array<std::uint32_t, 3>;

/* The area of the outline seen along `dir` (unit length), on a grid of
 * GRID x GRID cells spanning +-extent.
 */
inline double projected_area(const std::span<const vec3> pos, const std::span<const triangle> tris, const vec3 &dir, const float extent)
{
	/* An axis not parallel to `dir`, then the two image axes. */
	const vec3 helper{std::fabs(dir.x) < 0.9f ? vec3{1, 0, 0} : vec3{0, 1, 0}};
	const auto cross = [](const vec3 &a, const vec3 &b) {
		return vec3{a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
	};
	auto u{cross(dir, helper)};
	const auto ul{std::sqrt(u.x * u.x + u.y * u.y + u.z * u.z)};
	u = {u.x / ul, u.y / ul, u.z / ul};
	const auto v{cross(dir, u)};
	const float cell{2 * extent / GRID};
	std::vector<std::uint8_t> grid(GRID * GRID);
	for (const auto &t : tris)
	{
		std::array<std::array<float, 2>, 3> p;
		for (unsigned i = 0; i < 3; ++i)
		{
			const auto &q{pos[t[i]]};
			p[i] = {{q.x * u.x + q.y * u.y + q.z * u.z, q.x * v.x + q.y * v.y + q.z * v.z}};
		}
		const auto &a{p[0]}, &b{p[1]}, &c{p[2]};
		const auto d{(b[0] - a[0]) * (c[1] - a[1]) - (c[0] - a[0]) * (b[1] - a[1])};
		if (std::fabs(d) < 1e-12f)
			continue;
		const auto to_cell = [&](const float x) {
			return static_cast<int>(std::floor((x + extent) / cell));
		};
		const int last{static_cast<int>(GRID) - 1};
		const int x0{std::max(0, to_cell(std::min({a[0], b[0], c[0]})))}, x1{std::min(last, to_cell(std::max({a[0], b[0], c[0]})))};
		const int y0{std::max(0, to_cell(std::min({a[1], b[1], c[1]})))}, y1{std::min(last, to_cell(std::max({a[1], b[1], c[1]})))};
		for (int gy = y0; gy <= y1; ++gy)
			for (int gx = x0; gx <= x1; ++gx)
			{
				const float px{(static_cast<float>(gx) + 0.5f) * cell - extent}, py{(static_cast<float>(gy) + 0.5f) * cell - extent};
				const auto w0{((b[0] - px) * (c[1] - py) - (c[0] - px) * (b[1] - py)) / d};
				const auto w1{((c[0] - px) * (a[1] - py) - (a[0] - px) * (c[1] - py)) / d};
				if (w0 >= 0 && w1 >= 0 && 1 - w0 - w1 >= 0)
					grid[static_cast<std::size_t>(gy) * GRID + static_cast<std::size_t>(gx)] = 1;
			}
	}
	return static_cast<double>(std::count(grid.begin(), grid.end(), 1)) * cell * cell;
}

/* The mean silhouette in square units of a model centred on the origin. */
inline double mean_area(const std::span<const vec3> pos, const std::span<const triangle> tris)
{
	float extent{};
	for (const auto &t : tris)
		for (const auto i : t)
			extent = std::max(extent, std::sqrt(pos[i].x * pos[i].x + pos[i].y * pos[i].y + pos[i].z * pos[i].z));
	if (!(extent > 0))
		return 0;
	extent *= 1.001f;
	constexpr double golden_angle{2.39996322972865332};
	double sum{};
	for (unsigned i = 0; i < DIRECTIONS; ++i)
	{
		const double z{(i + 0.5) / DIRECTIONS}, r{std::sqrt(1 - z * z)}, phi{golden_angle * i};
		const vec3 dir{static_cast<float>(r * std::cos(phi)), static_cast<float>(r * std::sin(phi)), static_cast<float>(z)};
		sum += projected_area(pos, tris, dir, extent);
	}
	return sum / DIRECTIONS;
}

struct fair_size
{
	/* The factor to apply to a ship whose outermost point is at the
	 * Pyro's radius.
	 */
	double scale;
	/* The resulting mean silhouette relative to the Pyro's. */
	double ratio;
	/* RADIUS_CAP stopped the ship short of the Pyro's silhouette. */
	bool radius_capped;
	/* The ship grows beyond RADIUS_CAP to reach FLOOR (or RADIUS_LIMIT). */
	bool beyond_cap;
};

/* The square root, without std::sqrt being constexpr. */
constexpr double newton_sqrt(const double x)
{
	double s{x > 1 ? x : 1};
	for (unsigned i = 0; i < 100; ++i)
		s = 0.5 * (s + x / s);
	return s;
}

/* The size rule.  `ratio` is the ship's mean silhouette relative to the
 * Pyro's when its outermost point is at the Pyro's radius.  The
 * silhouette grows with the square of the scale.
 *  1. Scale so that the ratio becomes 1, but with the outermost point at
 *     most RADIUS_CAP Pyro radii out.
 *  2. A long, thin ship that ends below FLOOR there grows on until it
 *     reaches FLOOR, but at most to RADIUS_LIMIT: being seen matters
 *     more than the fit of the collision sphere.
 */
constexpr fair_size fair_scale(const double ratio)
{
	if (!(ratio > 0))
		return {1, 0, false, false};
	const auto s{newton_sqrt(1 / ratio)};
	if (s <= RADIUS_CAP)
		return {s, ratio * s * s, false, false};
	if (ratio * RADIUS_CAP * RADIUS_CAP >= FLOOR)
		return {RADIUS_CAP, ratio * RADIUS_CAP * RADIUS_CAP, true, false};
	const auto floor_scale{newton_sqrt(FLOOR / ratio)};
	const auto g{floor_scale < RADIUS_LIMIT ? floor_scale : RADIUS_LIMIT};
	return {g, ratio * g * g, true, true};
}

}
