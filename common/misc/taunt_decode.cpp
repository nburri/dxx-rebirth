/*
 * This file is part of the DXX-Rebirth project <https://github.com/dxx-rebirth/dxx-rebirth/>.
 * It is copyright by its individual contributors, as recorded in the
 * project's Git history.  See COPYING.txt at the top level for license
 * terms and a link to the Git history.
 */
/*
 * Taunts: decoding of the player's own file (WAV, Ogg Vorbis, MP3, FLAC)
 * with the vendored decoders in contrib/audio, so that it works the same
 * in every build.  Only the local file is decoded here; a taunt travels
 * as the game's own PCM (taunt_sample.h).
 */

#include <algorithm>
#include <cstdlib>
#include <cmath>
#include <cstring>
#include <limits>
#include "taunt_sample.h"

#define DR_WAV_IMPLEMENTATION
#define DR_WAV_NO_STDIO
#define DR_WAV_NO_WCHAR
#include "contrib/audio/dr_wav.h"
#define DR_MP3_IMPLEMENTATION
#define DR_MP3_NO_STDIO
#include "contrib/audio/dr_mp3.h"
#define DR_FLAC_IMPLEMENTATION
#define DR_FLAC_NO_STDIO
#define DR_FLAC_NO_WCHAR
#define DR_FLAC_NO_SIMD
#include "contrib/audio/dr_flac.h"
#define STB_VORBIS_NO_STDIO
#define STB_VORBIS_NO_PUSHDATA_API
#include "contrib/audio/stb_vorbis.c"

namespace dcx {

namespace taunt {

namespace {

constexpr std::size_t VORBIS_ARENA_SIZE{8u << 20};

enum class file_format
{
	unknown,
	wav,
	ogg,
	mp3,
	flac,
};

file_format sniff(const std::span<const std::uint8_t> b)
{
	const auto at{[b](const std::size_t offset, const char *magic) {
		const std::size_t n{std::strlen(magic)};
		return b.size() >= offset + n && !std::memcmp(b.data() + offset, magic, n);
	}};
	if ((at(0, "RIFF") || at(0, "RIFX") || at(0, "RF64")) && at(8, "WAVE"))
		return file_format::wav;
	if (at(0, "riff"))		/* Wave64 */
		return file_format::wav;
	if (at(0, "OggS"))
		return file_format::ogg;
	if (at(0, "fLaC"))
		return file_format::flac;
	if (at(0, "ID3"))
		return file_format::mp3;
	/* An MPEG audio frame: 11 sync bits. */
	if (b.size() >= 2 && b[0] == 0xff && (b[1] & 0xe0) == 0xe0)
		return file_format::mp3;
	return file_format::unknown;
}

/* The checks every decoder's stream info passes before anything is
 * decoded.
 */
bool usable_stream(const unsigned channels, const unsigned rate, std::string &error)
{
	if (!channels || channels > MAX_SOURCE_CHANNELS)
	{
		error = "unsupported number of channels " + std::to_string(channels);
		return false;
	}
	if (rate < MIN_SOURCE_RATE || rate > MAX_SOURCE_RATE)
	{
		error = "unsupported sample rate " + std::to_string(rate);
		return false;
	}
	return true;
}

/* Interleaved frames to mono, appended to `out`. */
void mix_down(const float *frames, const std::size_t n, const unsigned channels, std::vector<float> &out)
{
	for (std::size_t i{}; i != n; ++i)
	{
		float sum{};
		for (unsigned c{}; c != channels; ++c)
			sum += frames[i * channels + c];
		out.push_back(sum / static_cast<float>(channels));
	}
}

/* Reads frames in blocks with `read(buffer, frames) -> frames read` until
 * the end or the limit.
 */
template <typename Read>
void read_blocks(const unsigned channels, const unsigned rate, std::vector<float> &out, Read &&read)
{
	constexpr std::size_t block{4096};
	std::vector<float> buffer(block * channels);
	const std::size_t limit{std::size_t{rate} * MAX_DECODE_SECONDS};
	out.reserve(std::min<std::size_t>(limit, 1u << 20));
	while (out.size() < limit)
	{
		const std::size_t want{std::min(block, limit - out.size())};
		const std::size_t got{read(buffer.data(), want)};
		if (!got || got > want)
			break;
		mix_down(buffer.data(), got, channels, out);
	}
}

std::optional<decoded_audio> decode_wav(const std::span<const std::uint8_t> b, std::string &error)
{
	drwav wav;
	if (!drwav_init_memory(&wav, b.data(), b.size(), nullptr))
	{
		error = "not a readable WAV file";
		return std::nullopt;
	}
	std::optional<decoded_audio> r;
	if (usable_stream(wav.channels, wav.sampleRate, error))
	{
		r.emplace();
		r->rate = wav.sampleRate;
		r->format = "WAV";
		read_blocks(wav.channels, wav.sampleRate, r->samples, [&wav](float *buf, const std::size_t n) -> std::size_t {
			return drwav_read_pcm_frames_f32(&wav, n, buf);
		});
	}
	drwav_uninit(&wav);
	return r;
}

std::optional<decoded_audio> decode_mp3(const std::span<const std::uint8_t> b, std::string &error)
{
	drmp3 mp3;
	if (!drmp3_init_memory(&mp3, b.data(), b.size(), nullptr))
	{
		error = "not a readable MP3 file";
		return std::nullopt;
	}
	std::optional<decoded_audio> r;
	if (usable_stream(mp3.channels, mp3.sampleRate, error))
	{
		r.emplace();
		r->rate = mp3.sampleRate;
		r->format = "MP3";
		read_blocks(mp3.channels, mp3.sampleRate, r->samples, [&mp3](float *buf, const std::size_t n) -> std::size_t {
			return drmp3_read_pcm_frames_f32(&mp3, n, buf);
		});
	}
	drmp3_uninit(&mp3);
	return r;
}

std::optional<decoded_audio> decode_flac(const std::span<const std::uint8_t> b, std::string &error)
{
	drflac *const f{drflac_open_memory(b.data(), b.size(), nullptr)};
	if (!f)
	{
		error = "not a readable FLAC file";
		return std::nullopt;
	}
	std::optional<decoded_audio> r;
	if (usable_stream(f->channels, f->sampleRate, error))
	{
		r.emplace();
		r->rate = f->sampleRate;
		r->format = "FLAC";
		read_blocks(f->channels, f->sampleRate, r->samples, [f](float *buf, const std::size_t n) -> std::size_t {
			return drflac_read_pcm_frames_f32(f, n, buf);
		});
	}
	drflac_close(f);
	return r;
}

std::optional<decoded_audio> decode_ogg(const std::span<const std::uint8_t> b, std::string &error)
{
	if (b.size() > static_cast<std::size_t>(std::numeric_limits<int>::max()))
	{
		error = "the file is too large";
		return std::nullopt;
	}
	/* stb_vorbis allocates from this arena: its sizes are checked
	 * against it, and nothing a broken file leaves half set up is ever
	 * passed to free().
	 */
	std::vector<char> arena(VORBIS_ARENA_SIZE);
	stb_vorbis_alloc alloc{arena.data(), static_cast<int>(arena.size())};
	int err{};
	stb_vorbis *const v{stb_vorbis_open_memory(b.data(), static_cast<int>(b.size()), &err, &alloc)};
	if (!v)
	{
		error = "not a readable Ogg Vorbis file (Ogg Opus is not supported)";
		return std::nullopt;
	}
	std::optional<decoded_audio> r;
	const stb_vorbis_info info{stb_vorbis_get_info(v)};
	if (info.channels > 0 && usable_stream(static_cast<unsigned>(info.channels), info.sample_rate, error))
	{
		const int channels{info.channels};
		r.emplace();
		r->rate = info.sample_rate;
		r->format = "Ogg Vorbis";
		read_blocks(static_cast<unsigned>(channels), info.sample_rate, r->samples, [v, channels](float *buf, const std::size_t n) -> std::size_t {
			const int got{stb_vorbis_get_samples_float_interleaved(v, channels, buf, static_cast<int>(n) * channels)};
			return got > 0 ? static_cast<std::size_t>(got) : 0u;
		});
	}
	else if (info.channels <= 0)
		error = "unsupported number of channels";
	stb_vorbis_close(v);
	return r;
}

}

std::optional<decoded_audio> decode_audio_file(const std::span<const std::uint8_t> bytes, std::string &error)
{
	if (bytes.size() > MAX_SOURCE_FILE)
	{
		error = "the file is larger than 16 MiB";
		return std::nullopt;
	}
	std::optional<decoded_audio> r;
	switch (sniff(bytes))
	{
		case file_format::wav:
			r = decode_wav(bytes, error);
			break;
		case file_format::ogg:
			r = decode_ogg(bytes, error);
			break;
		case file_format::mp3:
			r = decode_mp3(bytes, error);
			break;
		case file_format::flac:
			r = decode_flac(bytes, error);
			break;
		case file_format::unknown:
		default:
			error = "not a WAV, Ogg Vorbis, MP3 or FLAC file";
			return std::nullopt;
	}
	if (r)
	{
		/* A decoder's garbage must not reach the processing. */
		for (float &s : r->samples)
			s = std::isfinite(s) ? std::clamp(s, -1.0f, 1.0f) : 0.0f;
		if (r->samples.empty())
		{
			error = std::string{"no audio in the "} + r->format + " file";
			return std::nullopt;
		}
	}
	return r;
}

}

}
