/*
 * This file is part of the DXX-Rebirth project <https://github.com/dxx-rebirth/dxx-rebirth/>.
 * It is copyright by its individual contributors, as recorded in the
 * project's Git history.  See COPYING.txt at the top level for license
 * terms and a link to the Git history.
 */
/*
 * Taunts (Documentation/taunts.md): the sample a player's horn plays, its
 * limits and processing, its transfer format, the starter horns, the
 * rate limiter and the network messages.  Depends on the standard
 * library only, so that the unit tests can use it without the game.
 */

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace dcx {

namespace taunt {

/* The sample: mono 16-bit PCM at this rate, at most this long. */
constexpr unsigned SAMPLE_RATE{22050};
constexpr unsigned MAX_SAMPLES{SAMPLE_RATE * 2};			/* 2.0 s */
constexpr unsigned FADE_OUT_SAMPLES{SAMPLE_RATE / 20};		/* 50 ms, where a sample is cut */
constexpr unsigned EDGE_FADE_SAMPLES{SAMPLE_RATE / 500};	/* 2 ms at both ends against clicks */
/* Shorter samples are refused (a click is no taunt). */
constexpr unsigned MIN_SAMPLES{SAMPLE_RATE / 20};			/* 50 ms */

/* Loudness: the peak at most PEAK_TARGET and the RMS at most RMS_TARGET
 * of full scale, so no taunt is louder than a weapon sound; a quiet
 * file is raised by at most MAX_GAIN.
 */
constexpr float PEAK_TARGET{0.70f};
constexpr float RMS_TARGET{0.20f};
constexpr float MAX_GAIN{8.0f};
/* Leading and trailing silence (below this fraction of full scale) is
 * cut before the 2 s are counted.
 */
constexpr float SILENCE_LEVEL{0.01f};

/* The own file: the largest file read, and how much of it is decoded
 * (room for leading silence before the 2 s).
 */
constexpr std::size_t MAX_SOURCE_FILE{16u << 20};
constexpr unsigned MAX_DECODE_SECONDS{8};
constexpr unsigned MIN_SOURCE_RATE{4000};
constexpr unsigned MAX_SOURCE_RATE{192000};
constexpr unsigned MAX_SOURCE_CHANNELS{8};

/* Transfer format ("DXT1"): magic, rate u16, flags u16 (0), count u32,
 * then `count` int16 samples, all little-endian.
 */
constexpr std::array<std::uint8_t, 4> WIRE_MAGIC{{'D', 'X', 'T', '1'}};
constexpr std::size_t WIRE_HEADER_SIZE{12};
constexpr std::size_t MAX_WIRE_SIZE{WIRE_HEADER_SIZE + 2 * MAX_SAMPLES};
static_assert(MAX_WIRE_SIZE <= 100 * 1024);

/* The file names tried for the own sample, in this order. */
constexpr std::array<const char *, 4> OWN_FILE_NAMES{{"taunt.wav", "taunt.mp3", "taunt.ogg", "taunt.flac"}};

/* The player's setting (descent.cfg TauntChoice). */
enum class choice : std::uint8_t
{
	off = 0,
	/* The own file; Horn 1 if there is none or it is unusable. */
	own = 1,
	horn1 = 2,
	horn2 = 3,
	horn3 = 4,
	horn4 = 5,
};
constexpr unsigned STARTER_HORNS{4};

/* Which sample a taunt plays, as the network carries it. */
enum class sample_kind : std::uint8_t
{
	horn1 = 1,
	horn2 = 2,
	horn3 = 3,
	horn4 = 4,
	/* The player's own sample, named by `id` and `size`. */
	custom = 0x10,
};

[[nodiscard]]
constexpr bool valid_sample_kind(const std::uint8_t k)
{
	return (k >= 1 && k <= STARTER_HORNS) || k == static_cast<std::uint8_t>(sample_kind::custom);
}

/* Horn `n` (1 to 4) as a sample kind. */
[[nodiscard]]
constexpr sample_kind horn_kind(const unsigned n)
{
	return static_cast<sample_kind>(n >= 1 && n <= STARTER_HORNS ? n : 1u);
}

[[nodiscard]]
const char *horn_name(unsigned n);

/* The PCM of a sample (mono, SAMPLE_RATE). */
using pcm = std::vector<std::int16_t>;

/* Decoding of the own file (taunt_decode.cpp): the format is told by its
 * content (RIFF/WAVE, OggS, fLaC, an ID3 tag or an MPEG frame).  The
 * channels are mixed to mono; at most MAX_DECODE_SECONDS are decoded.
 */
struct decoded_audio
{
	std::vector<float> samples;	/* mono, -1 to 1 */
	unsigned rate{};
	const char *format{""};
};
[[nodiscard]]
std::optional<decoded_audio> decode_audio_file(std::span<const std::uint8_t> bytes, std::string &error);

/* Processing (taunt_sample.cpp). */
/* Resample mono `in` at `rate` to SAMPLE_RATE (an average over the
 * input span of each output sample when shrinking, linear interpolation
 * when growing).
 */
[[nodiscard]]
std::vector<float> resample(std::span<const float> in, unsigned rate);
/* Cut leading and trailing silence. */
void trim_silence(std::vector<float> &s);
/* Cut to MAX_SAMPLES (with a FADE_OUT_SAMPLES fade-out where cut) and
 * fade both ends by EDGE_FADE_SAMPLES.  True if it was cut.
 */
bool cut_and_fade(std::vector<float> &s);
/* Scale so that the peak is at most PEAK_TARGET and the RMS at most
 * RMS_TARGET; quiet samples are raised by at most MAX_GAIN.  Returns the
 * gain applied.
 */
float normalise(std::vector<float> &s);
[[nodiscard]]
pcm to_pcm(std::span<const float> s);
[[nodiscard]]
std::vector<float> to_float(std::span<const std::int16_t> s);

/* All of the above, from a decoded file: nothing if it is silent or
 * shorter than MIN_SAMPLES after trimming (`error` says why).
 */
[[nodiscard]]
std::optional<pcm> prepare(const decoded_audio &a, std::string &error);
/* A file's bytes to a sample. */
[[nodiscard]]
std::optional<pcm> prepare_file(std::span<const std::uint8_t> bytes, std::string &error);

/* The transfer format. */
[[nodiscard]]
std::vector<std::uint8_t> encode_wire(std::span<const std::int16_t> s);
/* Strict checks (magic, rate, flags, count, exact size); the samples are
 * then cut, faded and normalised again, so a sample from the network
 * obeys the limits whoever made it.
 */
[[nodiscard]]
std::optional<pcm> decode_wire(std::span<const std::uint8_t> bytes);
/* The id of a sample in the transfer format: the SHA-256 of its bytes
 * (the asset key of the transfer, net_v2_ships.h kind 2).
 */
using sample_hash = std::array<std::uint8_t, 32>;
[[nodiscard]]
sample_hash wire_hash(std::span<const std::uint8_t> bytes);

/* The starter horns (our own, synthesised, CC0): `n` 1 to 4. */
[[nodiscard]]
pcm starter_horn(unsigned n);

/* The mixer's format: 44100 Hz, stereo, int16, interleaved. */
[[nodiscard]]
std::vector<std::int16_t> to_mixer_format(std::span<const std::int16_t> s);

/* Spam protection: sliding windows over the times of the last taunts.
 * A taunt is allowed if the player is not locked out, fewer than `burst`
 * taunts fell in the last `burst_window_ms` and fewer than `per_window`
 * in the last `window_ms`.  A taunt over either limit locks the player
 * out for `lockout_ms`.
 */
struct rate_limits
{
	unsigned burst;
	std::uint32_t burst_window_ms;
	unsigned per_window;
	std::uint32_t window_ms;
	std::uint32_t lockout_ms;
};
/* The sender, to tell its player at once. */
constexpr rate_limits SENDER_LIMITS{3, 2000, 4, 10000, 5000};
/* The host: the sender's limits with windows shorter by the jitter the
 * network may add between the sender and the host (300 ms), so that the
 * host does not refuse what the sender allowed and played.
 */
constexpr rate_limits HOST_LIMITS{3, 1700, 4, 9700, 5000};
/* Every receiver again, one taunt more in each window: the network may
 * bunch taunts the host allowed.
 */
constexpr rate_limits RECEIVER_LIMITS{4, 2000, 5, 10000, 5000};

enum class verdict : std::uint8_t
{
	allowed,
	/* Refused: locked out by an earlier excess. */
	locked,
	/* Refused: this one exceeded a limit, locked out from now. */
	exceeded,
};

class rate_limiter
{
	static constexpr std::size_t KEPT{8};
	std::array<std::uint64_t, KEPT> times{};
	std::size_t count{};
	std::uint64_t locked_until{};
	[[nodiscard]]
	unsigned taunts_within(std::uint64_t now_ms, std::uint32_t window_ms) const;
public:
	verdict check(std::uint64_t now_ms, const rate_limits &limits);
	/* Milliseconds of lockout left (0: none). */
	[[nodiscard]]
	std::uint64_t lockout_left(std::uint64_t now_ms) const
	{
		return locked_until > now_ms ? locked_until - now_ms : 0;
	}
	void reset()
	{
		*this = {};
	}
};

/* Network messages (Documentation/network-protocol-v2.md, "Taunts"). */
constexpr std::uint8_t MAX_PLAYER_ID{7};

/* TAUNT_REQUEST, client to host: the client's player taunts. */
struct taunt_request_msg
{
	static constexpr std::size_t SIZE{1 + 4 + 32};
	sample_kind kind{sample_kind::horn1};
	std::uint32_t size{};	/* custom: its size in the transfer format */
	sample_hash hash{};		/* custom: wire_hash of the sample */
	void write(std::span<std::uint8_t, SIZE> buf) const;
	[[nodiscard]]
	static std::optional<taunt_request_msg> read(std::span<const std::uint8_t> buf);
};

/* TAUNT, host to all: player `pid` taunts. */
struct taunt_msg
{
	static constexpr std::size_t SIZE{1 + 1 + 4 + 32};
	std::uint8_t pid{};
	sample_kind kind{sample_kind::horn1};
	std::uint32_t size{};
	sample_hash hash{};
	void write(std::span<std::uint8_t, SIZE> buf) const;
	[[nodiscard]]
	static std::optional<taunt_msg> read(std::span<const std::uint8_t> buf);
};

/* `/mute name` and `/unmute name` (also `/mute` alone: the list). */
struct mute_command
{
	bool mute{};
	std::string_view name;
};
[[nodiscard]]
std::optional<mute_command> parse_mute_command(std::string_view text);

}

}
