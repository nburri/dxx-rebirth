/*
 * This file is part of the DXX-Rebirth project <https://github.com/dxx-rebirth/dxx-rebirth/>.
 * It is copyright by its individual contributors, as recorded in the
 * project's Git history.  See COPYING.txt at the top level for license
 * terms and a link to the Git history.
 */
/*
 * SHA-256 (FIPS 180-4), the content id of a custom ship file
 * (Documentation/custom-ships.md, decision D5).  Depends on the
 * standard library only, so that the converter and the unit tests can
 * use it without the game.
 */

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace dcx {

using sha256_digest = std::array<std::uint8_t, 32>;

class sha256
{
	std::array<std::uint32_t, 8> h;
	std::array<std::uint8_t, 64> block{};
	std::size_t block_used{};
	std::uint64_t total_bytes{};
	void compress(const std::uint8_t *p);
public:
	sha256();
	void update(std::span<const std::uint8_t> data);
	[[nodiscard]]
	sha256_digest finish();
};

[[nodiscard]]
sha256_digest sha256_of(std::span<const std::uint8_t> data);

/* Lower-case hexadecimal, 64 characters. */
[[nodiscard]]
std::string sha256_hex(const sha256_digest &d);

/* Parse 64 hexadecimal characters (either case); false on anything else. */
[[nodiscard]]
bool sha256_from_hex(std::string_view text, sha256_digest &out);

}
