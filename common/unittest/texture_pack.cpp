/*
 * This file is part of the DXX-Rebirth project <https://github.com/dxx-rebirth/dxx-rebirth/>.
 * It is copyright by its individual contributors, as recorded in the
 * project's Git history.  See COPYING.txt at the top level for license
 * terms and a link to the Git history.
 */

#include "texture_pack.h"

#define BOOST_TEST_DYN_LINK
#define BOOST_TEST_MODULE Rebirth texture pack
#include <boost/test/unit_test.hpp>

using namespace dcx::texture_pack;

namespace {

rgba_image solid(const unsigned w, const unsigned h, const uint8_t r, const uint8_t g, const uint8_t b, const uint8_t a)
{
	rgba_image img;
	img.w = w;
	img.h = h;
	img.px.resize(static_cast<std::size_t>(w) * h * 4);
	for (std::size_t i = 0; i < img.px.size(); i += 4)
		img.px[i] = r, img.px[i + 1] = g, img.px[i + 2] = b, img.px[i + 3] = a;
	return img;
}

const uint8_t *pixel(const rgba_image &img, const unsigned x, const unsigned y)
{
	return &img.px[(static_cast<std::size_t>(y) * img.w + x) * 4];
}

}

BOOST_AUTO_TEST_CASE(mission_directory_is_lower_case_without_extension)
{
	BOOST_TEST(mission_directory("Corona") == "corona");
	BOOST_TEST(mission_directory("Corona.mn2") == "corona");
	BOOST_TEST(mission_directory("missions/Corona.MN2") == "corona");
	BOOST_TEST(mission_directory("") == "");
	BOOST_TEST(mission_directory("..") == "");
}

BOOST_AUTO_TEST_CASE(candidates_mission_first_then_generic)
{
	const std::vector<std::string> expected{
		"textures/corona/Rock021.png",
		"textures/corona/rock021.png",
		"textures/Rock021.png",
		"textures/rock021.png",
	};
	BOOST_TEST(candidate_paths("Rock021", "corona", false) == expected, boost::test_tools::per_element());
}

BOOST_AUTO_TEST_CASE(candidates_animated_frame_keeps_hash)
{
	const std::vector<std::string> expected{
		"textures/lava#3.png",
	};
	BOOST_TEST(candidate_paths("lava#3", "", false) == expected, boost::test_tools::per_element());
}

BOOST_AUTO_TEST_CASE(candidates_custom_art_only_in_mission_directory)
{
	const std::vector<std::string> expected{
		"textures/corona/door01#2.png",
	};
	BOOST_TEST(candidate_paths("door01#2", "corona", true) == expected, boost::test_tools::per_element());
	/* Custom art without a mission directory has no candidate. */
	BOOST_TEST(candidate_paths("door01#2", "", true).empty());
}

BOOST_AUTO_TEST_CASE(candidates_refuse_paths)
{
	BOOST_TEST(candidate_paths("../x", "", false).empty());
	BOOST_TEST(candidate_paths("a/b", "", false).empty());
	BOOST_TEST(candidate_paths("a\\b", "", false).empty());
	BOOST_TEST(candidate_paths("", "", false).empty());
	/* A bad mission name only drops the mission directory. */
	const std::vector<std::string> expected{"textures/x.png"};
	BOOST_TEST(candidate_paths("x", "../m", false) == expected, boost::test_tools::per_element());
}

BOOST_AUTO_TEST_CASE(power_of_two_sizes)
{
	const auto a{to_power_of_two(solid(200, 100, 10, 20, 30, 255), 4096)};
	BOOST_TEST(a.w == 256u);
	BOOST_TEST(a.h == 128u);
	BOOST_TEST(pixel(a, 17, 99)[1] == 20);
	const auto b{to_power_of_two(solid(256, 256, 1, 2, 3, 255), 4096)};
	BOOST_TEST(b.w == 256u);
	const auto c{to_power_of_two(solid(3000, 3000, 1, 2, 3, 255), 1024)};
	BOOST_TEST(c.w == 1024u);
	BOOST_TEST(c.h == 1024u);
}

BOOST_AUTO_TEST_CASE(power_of_two_keeps_holes_sharp)
{
	auto img{solid(3, 3, 0, 0, 0, 255)};
	img.px[(1 * 3 + 1) * 4 + 0] = 255;
	img.px[(1 * 3 + 1) * 4 + 1] = 0;
	img.px[(1 * 3 + 1) * 4 + 2] = 255;
	img.px[(1 * 3 + 1) * 4 + 3] = 0;
	const auto r{to_power_of_two(img, 64)};
	BOOST_TEST(r.w == 4u);
	unsigned holes{0};
	for (std::size_t i = 0; i < r.px.size(); i += 4)
		holes += is_hole(&r.px[i]);
	BOOST_TEST(holes > 0u);
	for (std::size_t i = 0; i < r.px.size(); i += 4)
		BOOST_TEST((is_hole(&r.px[i]) || r.px[i + 3] == 255));
}

BOOST_AUTO_TEST_CASE(composite_blends_and_punches_holes)
{
	const auto base{solid(2, 2, 100, 100, 100, 255)};
	auto top{solid(2, 2, 0, 0, 0, 0)};
	/* (0,0) opaque red, (1,0) hole, (0,1) transparent, (1,1) half green. */
	top.px[0] = 255, top.px[3] = 255;
	top.px[4] = 255, top.px[5] = 0, top.px[6] = 255, top.px[7] = 0;
	top.px[12 + 1] = 200, top.px[12 + 3] = 128;
	const auto r{composite(base, top, 0, true)};
	BOOST_TEST(r.w == 2u);
	BOOST_TEST(pixel(r, 0, 0)[0] == 255);
	BOOST_TEST(pixel(r, 0, 0)[3] == 255);
	BOOST_TEST(pixel(r, 1, 0)[3] == 0);
	BOOST_TEST(pixel(r, 1, 0)[0] == 100);
	BOOST_TEST(pixel(r, 0, 1)[0] == 100);
	BOOST_TEST(pixel(r, 0, 1)[3] == 255);
	BOOST_TEST(pixel(r, 1, 1)[1] > 140);
	BOOST_TEST(pixel(r, 1, 1)[1] < 160);
	/* Without supertransparency a hole shows the base. */
	const auto n{composite(base, top, 0, false)};
	BOOST_TEST(pixel(n, 1, 0)[3] == 255);
}

BOOST_AUTO_TEST_CASE(composite_rotation_matches_texmerge)
{
	/* A 2x2 overlay with one opaque texel at row 0, column 1. */
	const auto base{solid(2, 2, 0, 0, 0, 255)};
	auto top{solid(2, 2, 0, 0, 0, 0)};
	top.px[4] = 255, top.px[7] = 255;
	/* texmerge: dest(y, x) = top[wh * x + (wh - 1 - y)] for orient 1, so
	 * top(0, 1) lands at y = 0, x = 0.
	 */
	const auto r1{composite(base, top, 1, false)};
	BOOST_TEST(pixel(r1, 0, 0)[0] == 255);
	/* orient 2: dest(y, x) = top(1 - y, 1 - x): top(0, 1) at (1, 0). */
	const auto r2{composite(base, top, 2, false)};
	BOOST_TEST(pixel(r2, 0, 1)[0] == 255);
	/* orient 3: dest(y, x) = top(1 - x, y): top(0, 1) at y = 1, x = 1. */
	const auto r3{composite(base, top, 3, false)};
	BOOST_TEST(pixel(r3, 1, 1)[0] == 255);
}

BOOST_AUTO_TEST_CASE(composite_scales_to_the_larger_image)
{
	const auto r{composite(solid(64, 64, 1, 1, 1, 255), solid(256, 256, 0, 0, 0, 0), 0, true)};
	BOOST_TEST(r.w == 256u);
	BOOST_TEST(r.h == 256u);
	BOOST_TEST(pixel(r, 255, 255)[0] == 1);
}
