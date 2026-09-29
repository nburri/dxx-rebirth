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
 *
 * In a network deathmatch the host assigns every spawn (its own, its
 * bots' and, on request, its clients'; Documentation/network-protocol-v2.md
 * section 8, "Host-assigned spawns"), and remembers the sites it assigned
 * in the last few seconds (spawn_reservations): until the ship that was
 * sent there shows up in the host's view, the site counts as a ship for
 * the ranking of the others (count_reserved_spawn_sites) and is left out
 * while a free site remains (partition_free_spawn_sites).
 */

#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
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

/* The sites assigned recently, each until a time (any clock; the game
 * uses timer_query), and to whom.  `N` is the number of sites
 * (MAX_PLAYERS).
 */
template <std::size_t N>
class spawn_reservations
{
	std::array<std::int64_t, N> until{};
	std::array<unsigned, N> owner{};
	std::array<bool, N> held{};
public:
	/* No owner: a reservation release() never drops. */
	static constexpr unsigned no_owner{~0u};
	void reset()
	{
		held.fill(false);
	}
	void reserve(const unsigned site, const std::int64_t now, const std::int64_t duration, const unsigned for_owner = no_owner)
	{
		if (site >= N)
			return;
		until[site] = now + duration;
		owner[site] = for_owner;
		held[site] = true;
	}
	/* Drop the reservations of `for_owner`: it asks again, so it no
	 * longer goes to (or already left) the site it was given.
	 */
	void release(const unsigned for_owner)
	{
		if (for_owner == no_owner)
			return;
		for (std::size_t i = 0; i < N; ++i)
			if (owner[i] == for_owner)
				held[i] = false;
	}
	[[nodiscard]]
	bool reserved(const unsigned site, const std::int64_t now) const
	{
		return site < N && held[site] && now < until[site];
	}
};

/* Count every reserved site as a ship standing there: the distance of
 * each site in `sites` (site index, distance to the nearest other ship)
 * becomes at most its distance to every other reserved site.
 * `reserved(site)` says whether a site is reserved; `site_distance(a, b)`
 * is the distance between two sites, negative if there is no path (such
 * a site is ignored, as an unreachable ship is).
 */
template <typename Site, typename Reserved, typename SiteDistance>
void count_reserved_spawn_sites(const std::span<Site> sites, Reserved &&reserved, SiteDistance &&site_distance)
{
	for (auto &s : sites)
		for (const auto &r : sites)
		{
			if (r.first == s.first || !reserved(r.first))
				continue;
			const auto d{site_distance(s.first, r.first)};
			if (d >= 0 && s.second > d)
				s.second = d;
		}
}

/* Move the free (not reserved) sites to the front, keeping their order,
 * and return their number; if every site is reserved, return the number
 * of sites (all remain candidates, and count_reserved_spawn_sites made the
 * ranking prefer the one farthest from the others).
 */
template <typename Site, typename Reserved>
unsigned partition_free_spawn_sites(const std::span<Site> sites, Reserved &&reserved)
{
	const auto free_end{std::stable_partition(sites.begin(), sites.end(), [&reserved](const Site &s) {
		return !reserved(s.first);
	})};
	if (free_end == sites.begin())
		return sites.size();
	return static_cast<unsigned>(std::distance(sites.begin(), free_end));
}

}
