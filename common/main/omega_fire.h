/*
 * This file is part of the DXX-Rebirth project <https://github.com/dxx-rebirth/dxx-rebirth/>.
 * It is copyright by its individual contributors, as recorded in the
 * project's Git history.  See COPYING.txt at the top level for license
 * terms and a link to the Git history.
 */
/*
 * Whether a trigger pull of the Omega cannon makes lightning
 * (do_omega_stuff in similar/main/laser.cpp).
 *
 * The shooter's machine only fires with enough charge: at least
 * MIN_OMEGA_CHARGE, or some charge and no energy left.  In a network
 * game with protocol v1, that check only ever applied on the shooter's
 * own machine: the FIRE message is sent for every trigger pull (twenty
 * a second), and every other machine acted it out without a charge, so
 * the victim, whose machine decided the damage, was hit by every pull.
 * A held Omega therefore kept doing its damage after the charge ran
 * out, at any frame rate.
 *
 * Protocol v2 (stage 4) lets only the shooter's machine report hits, so
 * the pulls without a charge stopped doing damage: after the 18 shots
 * of a full charge, a held Omega fell from 20 to about 1.9 shots a
 * second.  To keep the damage of the classic game, in a network game a
 * pull without the charge still makes its lightning ("uncharged"): it
 * takes no charge and does not delay the recharge, as the remote copies
 * in v1 did not.  Without energy it does nothing (the weapon is then
 * deselected, as in v1).
 *
 * Standard library only, so that the rule is tested outside the game
 * (common/unittest/omega_fire.cpp).
 */

#pragma once

#include <cstdint>

namespace dcx {

enum class omega_shot : std::uint8_t
{
	/* No lightning: the placeholder is deleted. */
	none,
	/* Lightning that takes OMEGA_BASE_TIME of the charge and restarts
	 * the recharge delay.
	 */
	charged,
	/* Network game, charge too low: lightning without a charge. */
	uncharged,
};

/* `charge`, `min_charge`, `energy`: fix (1.0 = 65536). */
[[nodiscard]]
constexpr omega_shot omega_shot_kind(const std::int32_t charge, const std::int32_t min_charge, const std::int32_t energy, const bool network_game)
{
	if (charge >= min_charge || (charge && !energy))
		return omega_shot::charged;
	if (network_game && energy > 0)
		return omega_shot::uncharged;
	return omega_shot::none;
}

}
