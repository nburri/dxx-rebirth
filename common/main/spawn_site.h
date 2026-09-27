/*
 * This file is part of the DXX-Rebirth project <https://github.com/dxx-rebirth/dxx-rebirth/>.
 * It is copyright by its individual contributors, as recorded in the
 * project's Git history.  See COPYING.txt at the top level for license
 * terms and a link to the Git history.
 */
/*
 * The game-independent part of choosing a deathmatch spawn site
 * (choose_spawn in gameseq.cpp): rank the sites by their distance to the
 * nearest other ship, then draw among the most secluded ones.  Standard
 * library only, so that test-spawn-site can pin the behaviour: the order
 * of the ranked sites, the result and the number of random numbers drawn.
 */

#pragma once

#include <algorithm>
#include <span>

namespace dcx {

/* `sites` holds one (site index, distance to the nearest other ship)
 * pair per spawn site.  If there are more than `secluded` sites, move
 * the `secluded` farthest ones to the front, farthest first, and return
 * `secluded`; otherwise leave the order alone and return the number of
 * sites.  The result is the number of sites the draw may choose from.
 */
template <typename Site>
unsigned rank_secluded_spawn_sites(const std::span<Site> sites, const unsigned secluded)
{
	if (sites.size() <= secluded)
		return sites.size();
	const auto &&predicate = [](const Site &a, const Site &b) {
		return a.second > b.second;
	};
	const auto b = sites.begin();
	std::partial_sort(b, std::next(b, secluded), sites.end(), predicate);
	return secluded;
}

/* Draw one of the first `usable` (> 0) `sites` with `draw() % usable`,
 * until the drawn site is at least `min_distance` from every other ship
 * or `max_tries` draws were made.  Returns the site index of the last
 * draw.  `draw` returns a non-negative random number (d_rand).
 */
template <typename Site, typename Distance, typename Draw>
auto pick_spawn_site(const std::span<const Site> sites, const unsigned usable, const Distance min_distance, const unsigned max_tries, Draw &&draw)
{
	unsigned tries{0};
	unsigned n;
	do {
		++tries;
		n = draw() % usable;
		if (sites[n].second >= min_distance)
			break;
	} while (tries < max_tries);
	return sites[n].first;
}

}
