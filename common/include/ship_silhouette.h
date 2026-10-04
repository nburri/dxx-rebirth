/*
 * This file is part of the DXX-Rebirth project <https://github.com/dxx-rebirth/dxx-rebirth/>.
 * It is copyright by its individual contributors, as recorded in the
 * project's Git history.  See COPYING.txt at the top level for license
 * terms and a link to the Git history.
 */
/*
 * The size rule of custom ships (decision D3 of
 * Documentation/custom-ships.md): players see each other mostly head-on
 * or from behind, so a ship's outline seen from the front and from the
 * rear (40 % each) and from the side and from above (10 % each) is made
 * equal to the Pyro-GX's, while its outermost point stays within the
 * radius the game's reader accepts.  Used by the converter
 * (common/tools/shipconv.cpp) and its test.
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

/* The Pyro-GX's outline in square units, measured with `projected_area`
 * and `mean_area` below from model 108 of descent2.ham: seen along its
 * axis (front and rear are mirror images, the same area), from the side,
 * from above, and averaged over all directions.  Numbers only; no retail
 * geometry is used.
 */
constexpr double PYRO_FRONT_AREA{8.557};
constexpr double PYRO_SIDE_AREA{13.712};
constexpr double PYRO_TOP_AREA{35.817};
constexpr double PYRO_MEAN_AREA{23.548};

/* How much each view counts. */
constexpr double WEIGHT_FRONT{0.4};
constexpr double WEIGHT_REAR{0.4};
constexpr double WEIGHT_SIDE{0.1};
constexpr double WEIGHT_TOP{0.1};

/* The band the converter aims for.  A ship that stays below BAND_LOW
 * even with its outermost point at RADIUS_LIMIT is "too thin": it is
 * converted, with a warning, but not bundled.
 */
constexpr double BAND_LOW{0.9};
constexpr double BAND_HIGH{1.1};

/* The outermost point at most RADIUS_LIMIT Pyro radii from the centre,
 * just inside the reader's limit (RADIUS_TOLERANCE); the collision sphere
 * stays the Pyro's.
 */
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

/* The outline of a model centred on the origin, seen from the front (or
 * the rear), the side and above.
 */
struct view_areas
{
	double front, side, top;
	/* The weighted area the size rule compares. */
	constexpr double weighted() const
	{
		return (WEIGHT_FRONT + WEIGHT_REAR) * front + WEIGHT_SIDE * side + WEIGHT_TOP * top;
	}
	/* Each relative to the Pyro's. */
	constexpr view_areas relative() const
	{
		return {front / PYRO_FRONT_AREA, side / PYRO_SIDE_AREA, top / PYRO_TOP_AREA};
	}
};

constexpr double PYRO_VIEW_AREA{view_areas{PYRO_FRONT_AREA, PYRO_SIDE_AREA, PYRO_TOP_AREA}.weighted()};

inline view_areas axis_areas(const std::span<const vec3> pos, const std::span<const triangle> tris)
{
	float extent{};
	for (const auto &t : tris)
		for (const auto i : t)
			extent = std::max(extent, std::sqrt(pos[i].x * pos[i].x + pos[i].y * pos[i].y + pos[i].z * pos[i].z));
	if (!(extent > 0))
		return {};
	extent *= 1.001f;
	return {projected_area(pos, tris, {0, 0, 1}, extent), projected_area(pos, tris, {1, 0, 0}, extent), projected_area(pos, tris, {0, 1, 0}, extent)};
}

/* The weighted outline relative to the Pyro's. */
inline double view_ratio(const std::span<const vec3> pos, const std::span<const triangle> tris)
{
	return axis_areas(pos, tris).weighted() / PYRO_VIEW_AREA;
}

struct fair_size
{
	/* The factor to apply to a ship whose outermost point is at the
	 * Pyro's radius.
	 */
	double scale;
	/* The resulting weighted outline relative to the Pyro's. */
	double ratio;
	/* RADIUS_LIMIT stopped the ship short of the Pyro's outline. */
	bool radius_capped;
	/* ... and below BAND_LOW: too thin to be fair. */
	bool too_thin;
};

/* The square root, without std::sqrt being constexpr. */
constexpr double newton_sqrt(const double x)
{
	double s{x > 1 ? x : 1};
	for (unsigned i = 0; i < 100; ++i)
		s = 0.5 * (s + x / s);
	return s;
}

/* The size rule.  `ratio` is the ship's weighted outline relative to the
 * Pyro's when its outermost point is at the Pyro's radius; it grows with
 * the square of the (uniform) scale.  Scale so that the ratio becomes 1,
 * but with the outermost point at most RADIUS_LIMIT Pyro radii out.
 */
constexpr fair_size fair_scale(const double ratio)
{
	if (!(ratio > 0))
		return {1, 0, false, true};
	const auto s{newton_sqrt(1 / ratio)};
	if (s <= RADIUS_LIMIT)
		return {s, ratio * s * s, false, false};
	const auto r{ratio * RADIUS_LIMIT * RADIUS_LIMIT};
	return {RADIUS_LIMIT, r, true, r < BAND_LOW};
}

}
