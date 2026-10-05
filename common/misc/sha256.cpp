/*
 * This file is part of the DXX-Rebirth project <https://github.com/dxx-rebirth/dxx-rebirth/>.
 * It is copyright by its individual contributors, as recorded in the
 * project's Git history.  See COPYING.txt at the top level for license
 * terms and a link to the Git history.
 */
/*
 * SHA-256 (FIPS 180-4), written from the standard.
 */

#include <algorithm>
#include <string_view>
#include "sha256.h"

namespace dcx {

namespace {

constexpr std::array<std::uint32_t, 64> K{{
	0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
	0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
	0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
	0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
	0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
	0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
	0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
	0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
}};

constexpr std::uint32_t rotr(const std::uint32_t x, const unsigned n)
{
	return (x >> n) | (x << (32 - n));
}

}

sha256::sha256() :
	h{{0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19}}
{
}

void sha256::compress(const std::uint8_t *const p)
{
	std::array<std::uint32_t, 64> w;
	for (unsigned i = 0; i < 16; ++i)
		w[i] = (std::uint32_t{p[4 * i]} << 24) | (std::uint32_t{p[4 * i + 1]} << 16) | (std::uint32_t{p[4 * i + 2]} << 8) | std::uint32_t{p[4 * i + 3]};
	for (unsigned i = 16; i < 64; ++i)
	{
		const auto s0{rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3)};
		const auto s1{rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10)};
		w[i] = w[i - 16] + s0 + w[i - 7] + s1;
	}
	auto a{h[0]}, b{h[1]}, c{h[2]}, d{h[3]}, e{h[4]}, f{h[5]}, g{h[6]}, hh{h[7]};
	for (unsigned i = 0; i < 64; ++i)
	{
		const auto S1{rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25)};
		const auto ch{(e & f) ^ (~e & g)};
		const auto t1{hh + S1 + ch + K[i] + w[i]};
		const auto S0{rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22)};
		const auto maj{(a & b) ^ (a & c) ^ (b & c)};
		const auto t2{S0 + maj};
		hh = g;
		g = f;
		f = e;
		e = d + t1;
		d = c;
		c = b;
		b = a;
		a = t1 + t2;
	}
	h[0] += a;
	h[1] += b;
	h[2] += c;
	h[3] += d;
	h[4] += e;
	h[5] += f;
	h[6] += g;
	h[7] += hh;
}

void sha256::update(std::span<const std::uint8_t> data)
{
	total_bytes += data.size();
	while (!data.empty())
	{
		const auto n{std::min(data.size(), block.size() - block_used)};
		std::copy_n(data.begin(), n, block.begin() + block_used);
		block_used += n;
		data = data.subspan(n);
		if (block_used == block.size())
		{
			compress(block.data());
			block_used = 0;
		}
	}
}

sha256_digest sha256::finish()
{
	const std::uint64_t bits{total_bytes * 8};
	block[block_used++] = 0x80;
	if (block_used > 56)
	{
		std::fill(block.begin() + block_used, block.end(), 0);
		compress(block.data());
		block_used = 0;
	}
	std::fill(block.begin() + block_used, block.begin() + 56, 0);
	for (unsigned i = 0; i < 8; ++i)
		block[56 + i] = static_cast<std::uint8_t>(bits >> (56 - 8 * i));
	compress(block.data());
	sha256_digest out;
	for (unsigned i = 0; i < 8; ++i)
	{
		out[4 * i] = static_cast<std::uint8_t>(h[i] >> 24);
		out[4 * i + 1] = static_cast<std::uint8_t>(h[i] >> 16);
		out[4 * i + 2] = static_cast<std::uint8_t>(h[i] >> 8);
		out[4 * i + 3] = static_cast<std::uint8_t>(h[i]);
	}
	return out;
}

sha256_digest sha256_of(const std::span<const std::uint8_t> data)
{
	sha256 s;
	s.update(data);
	return s.finish();
}

std::string sha256_hex(const sha256_digest &d)
{
	static constexpr char digits[] = "0123456789abcdef";
	std::string r;
	r.reserve(64);
	for (const auto b : d)
	{
		r.push_back(digits[b >> 4]);
		r.push_back(digits[b & 15]);
	}
	return r;
}

bool sha256_from_hex(const std::string_view text, sha256_digest &out)
{
	if (text.size() != 64)
		return false;
	const auto nibble = [](const char c) -> int {
		if (c >= '0' && c <= '9')
			return c - '0';
		if (c >= 'a' && c <= 'f')
			return c - 'a' + 10;
		if (c >= 'A' && c <= 'F')
			return c - 'A' + 10;
		return -1;
	};
	for (std::size_t i = 0; i < 32; ++i)
	{
		const auto hi{nibble(text[2 * i])}, lo{nibble(text[2 * i + 1])};
		if (hi < 0 || lo < 0)
			return false;
		out[i] = static_cast<std::uint8_t>((hi << 4) | lo);
	}
	return true;
}

}
