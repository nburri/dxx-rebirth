/*
 * This file is part of the DXX-Rebirth project <https://github.com/dxx-rebirth/dxx-rebirth/>.
 * It is copyright by its individual contributors, as recorded in the
 * project's Git history.  See COPYING.txt at the top level for license
 * terms and a link to the Git history.
 */
/*
 * Test of the traversal of connected_segment_raw_distances
 * (segment_depths.h, fireball.cpp): on random levels (segments with up
 * to six open sides, some control centre segments), the depths are the
 * shortest routes that do not pass a control centre, and the count at
 * each depth, `max_depth` included, is the number of segments recorded
 * there.  The old traversal's count at `max_depth` wraps on the same
 * levels (the log's "count=655xx ... no segment found at depth 23").
 *
 * Build and run with SCons:
 *
 *	scons sdl2=1 d1x=0 d2x=1 register_runtime_test_plain_link_targets=1 test-segment-depths
 *	build/common/test-segment-depths
 *
 * or directly:
 *
 *	g++ -std=gnu++23 -O2 -Wall -Wextra -Icommon/main common/unittest/segment_depths.cpp -o test-segment-depths
 */

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <optional>
#include <random>
#include <vector>

#include "segment_depths.h"

namespace {

void check_failed(const char *const expr, const char *const file, const int line)
{
	std::fprintf(stderr, "%s:%d: check failed: %s\n", file, line, expr);
	std::exit(1);
}

#define CHECK(cond)	do { if (!(cond)) check_failed(#cond, __FILE__, __LINE__); } while (0)

/* The game's counts: 16 bits, for depths 4 to 30 (fireball.cpp). */
constexpr unsigned COUNT_MIN_DEPTH{4};
constexpr unsigned COUNT_MAX_DEPTH{30};

struct level
{
	std::vector<std::array<int, 6>> children;
	std::vector<bool> controlcen;
};

/* A random level: a chain of rooms (every segment reachable), with extra
 * links (loops, so that shorter routes are found after longer ones).
 */
level random_level(std::minstd_rand &rng, const unsigned n, const unsigned extra_links, const unsigned control_centres)
{
	level l;
	l.children.assign(n, {{-1, -1, -1, -1, -1, -1}});
	l.controlcen.assign(n, false);
	const auto link{[&l](const unsigned a, const unsigned b) {
		if (a == b)
			return;
		unsigned sa{6}, sb{6};
		for (unsigned s = 0; s < 6; ++s)
		{
			if (l.children[a][s] == static_cast<int>(b))
				return;
			if (sa == 6 && l.children[a][s] < 0)
				sa = s;
			if (sb == 6 && l.children[b][s] < 0)
				sb = s;
		}
		if (sa == 6 || sb == 6)
			return;
		l.children[a][sa] = static_cast<int>(b);
		l.children[b][sb] = static_cast<int>(a);
	}};
	for (unsigned i = 1; i < n; ++i)
		link(i, std::uniform_int_distribution<unsigned>(i > 3 ? i - 3 : 0, i - 1)(rng));
	for (unsigned k = 0; k < extra_links; ++k)
		link(std::uniform_int_distribution<unsigned>(0, n - 1)(rng), std::uniform_int_distribution<unsigned>(0, n - 1)(rng));
	for (unsigned k = 0; k < control_centres; ++k)
		l.controlcen[std::uniform_int_distribution<unsigned>(1, n - 1)(rng)] = true;
	return l;
}

/* The game's builder over a test level (fireball.cpp's builder: 0 is
 * `indeterminate`, depth d is d + 1).
 */
struct test_graph
{
	using segment_index = unsigned;
	const level &l;
	uint8_t max_depth;
	std::vector<uint8_t> &biased;
	std::array<uint16_t, COUNT_MAX_DEPTH + 1> &counts;
	std::optional<uint8_t> depth_of(const unsigned i) const
	{
		if (!biased[i])
			return std::nullopt;
		return static_cast<uint8_t>(biased[i] - 1);
	}
	bool excluded(const unsigned i) const
	{
		return l.controlcen[i];
	}
	void exclude(const unsigned i) const
	{
		biased[i] = 1;
	}
	void record(const unsigned i, const uint8_t d) const
	{
		biased[i] = static_cast<uint8_t>(d + 1);
	}
	void count(const uint8_t d, const int delta) const
	{
		if (d < COUNT_MIN_DEPTH || d > COUNT_MAX_DEPTH)
			return;
		if (delta > 0)
			++counts[d];
		else
			--counts[d];
	}
	uint8_t max_depth_of() const
	{
		return max_depth;
	}
	template <typename F>
	void for_each_passable_child(const unsigned i, F &&f) const
	{
		for (const int c : l.children[i])
			if (c >= 0)
				f(static_cast<unsigned>(c));
	}
};

/* The traversal before the fix (fireball.cpp, v0.61-exp-19), for the
 * regression: a segment is counted only below max_depth, but taken off
 * its old depth's count, max_depth included.
 */
void old_visit(const level &l, const uint8_t max_depth, std::vector<uint8_t> &biased, std::array<uint16_t, COUNT_MAX_DEPTH + 1> &counts, const unsigned seg, const uint8_t depth)
{
	const auto valid{[](const unsigned d) {
		return d >= COUNT_MIN_DEPTH && d <= COUNT_MAX_DEPTH;
	}};
	if (biased[seg])
	{
		const unsigned d{biased[seg] - 1u};
		if (d <= depth)
			return;
		if (valid(d))
			--counts[d];
	}
	biased[seg] = static_cast<uint8_t>(depth + 1);
	if (depth >= max_depth)
		return;
	if (l.controlcen[seg])
	{
		biased[seg] = 1;
		return;
	}
	if (valid(depth))
		++counts[depth];
	for (const int c : l.children[seg])
		if (c >= 0)
			old_visit(l, max_depth, biased, counts, static_cast<unsigned>(c), static_cast<uint8_t>(depth + 1));
}

/* The shortest routes that do not pass a control centre (one is
 * reached, not passed), up to max_depth.
 */
std::vector<int> shortest(const level &l, const unsigned start, const unsigned max_depth)
{
	std::vector<int> d(l.children.size(), -1);
	std::deque<unsigned> q;
	d[start] = 0;
	q.push_back(start);
	while (!q.empty())
	{
		const auto i{q.front()};
		q.pop_front();
		if (l.controlcen[i] || static_cast<unsigned>(d[i]) >= max_depth)
			continue;
		for (const int c : l.children[i])
			if (c >= 0 && d[static_cast<unsigned>(c)] < 0)
			{
				d[static_cast<unsigned>(c)] = d[i] + 1;
				q.push_back(static_cast<unsigned>(c));
			}
	}
	return d;
}

void test_counts_match_depths()
{
	std::minstd_rand rng{12345};
	unsigned old_wrapped{0}, levels{0}, at_max{0};
	for (unsigned trial = 0; trial < 400; ++trial)
	{
		const unsigned n{std::uniform_int_distribution<unsigned>(20, 400)(rng)};
		const auto l{random_level(rng, n, n / 3, trial % 3)};
		const unsigned start{std::uniform_int_distribution<unsigned>(0, n - 1)(rng)};
		if (l.controlcen[start])
			continue;
		const auto max_depth{static_cast<uint8_t>(std::uniform_int_distribution<unsigned>(8, 24)(rng))};
		std::vector<uint8_t> biased(n, 0);
		std::array<uint16_t, COUNT_MAX_DEPTH + 1> counts{};
		dcx::visit_segment_depths(test_graph{l, max_depth, biased, counts}, start, 0);
		++levels;
		/* The depths: the shortest routes. */
		const auto want{shortest(l, start, max_depth)};
		std::array<unsigned, COUNT_MAX_DEPTH + 1> tally{};
		for (unsigned i = 0; i < n; ++i)
		{
			if (want[i] < 0)
			{
				CHECK(!biased[i]);
				continue;
			}
			CHECK(biased[i]);
			if (l.controlcen[i])
			{
				/* Never a result. */
				CHECK(biased[i] == 1);
				continue;
			}
			CHECK(biased[i] - 1 == want[i]);
			++tally[static_cast<unsigned>(want[i])];
		}
		/* Each count is the number of segments at its depth,
		 * max_depth included.
		 */
		for (unsigned d = COUNT_MIN_DEPTH; d <= COUNT_MAX_DEPTH; ++d)
			CHECK(counts[d] == tally[d]);
		if (tally[max_depth])
			++at_max;
		/* The old traversal on the same level. */
		std::vector<uint8_t> old_biased(n, 0);
		std::array<uint16_t, COUNT_MAX_DEPTH + 1> old_counts{};
		old_visit(l, max_depth, old_biased, old_counts, start, 0);
		if (old_counts[max_depth] > n)
			++old_wrapped;
	}
	std::printf("segment depths: %u levels, %u with segments at max_depth; the old count wrapped on %u\n", levels, at_max, old_wrapped);
	CHECK(levels > 300);
	CHECK(at_max > 50);
	/* The bug the fix removes: the old count at max_depth wrapped. */
	CHECK(old_wrapped > 10);
}

/* A control centre at max_depth was recorded as a normal segment
 * before; now it is never a result.
 */
void test_control_centre_at_max_depth()
{
	level l;
	/* A chain 0-1-2-3-4-5, the control centre at 5. */
	l.children.assign(6, {{-1, -1, -1, -1, -1, -1}});
	l.controlcen.assign(6, false);
	for (unsigned i = 0; i + 1 < 6; ++i)
	{
		l.children[i][0] = static_cast<int>(i + 1);
		l.children[i + 1][1] = static_cast<int>(i);
	}
	l.controlcen[5] = true;
	std::vector<uint8_t> biased(6, 0);
	std::array<uint16_t, COUNT_MAX_DEPTH + 1> counts{};
	dcx::visit_segment_depths(test_graph{l, 5, biased, counts}, 0u, 0);
	CHECK(biased[5] == 1);
	CHECK(counts[5] == 0);
	CHECK(counts[4] == 1);
	/* max_depth 4: the segment at 4 is counted. */
	std::vector<uint8_t> b2(6, 0);
	std::array<uint16_t, COUNT_MAX_DEPTH + 1> c2{};
	dcx::visit_segment_depths(test_graph{l, 4, b2, c2}, 0u, 0);
	CHECK(c2[4] == 1);
	CHECK(!b2[5]);
}

/* A loop found the long way first: the segment moves to its shorter
 * depth, off the count of the longer one (at max_depth too).
 */
void test_shorter_route_found_later()
{
	level l;
	/* 0-1-2-3-4-5-6 and a shortcut 0-6; max_depth 6.  Depth first
	 * through 1 reaches 6 at depth 6, then 5 at 5 through 6... until the
	 * shortcut: 6 at 1, 5 at 2, 4 at 3.
	 */
	l.children.assign(7, {{-1, -1, -1, -1, -1, -1}});
	l.controlcen.assign(7, false);
	for (unsigned i = 0; i + 1 < 7; ++i)
	{
		l.children[i][0] = static_cast<int>(i + 1);
		l.children[i + 1][1] = static_cast<int>(i);
	}
	l.children[0][2] = 6;
	l.children[6][2] = 0;
	std::vector<uint8_t> biased(7, 0);
	std::array<uint16_t, COUNT_MAX_DEPTH + 1> counts{};
	dcx::visit_segment_depths(test_graph{l, 6, biased, counts}, 0u, 0);
	const std::array<unsigned, 7> want{{0, 1, 2, 3, 3, 2, 1}};
	for (unsigned i = 0; i < 7; ++i)
		CHECK(biased[i] - 1u == want[i]);
	for (unsigned d = COUNT_MIN_DEPTH; d <= 6; ++d)
		CHECK(counts[d] == 0);
	std::vector<uint8_t> ob(7, 0);
	std::array<uint16_t, COUNT_MAX_DEPTH + 1> oc{};
	old_visit(l, 6, ob, oc, 0, 0);
	/* The old count at 6 wrapped: segment 6 was found at 6 first
	 * (uncounted), then at 1.
	 */
	CHECK(oc[6] == 0xffff);
}

}

int main()
{
	test_counts_match_depths();
	test_control_centre_at_max_depth();
	test_shorter_route_found_later();
	std::printf("test-segment-depths: all passed\n");
	return 0;
}
