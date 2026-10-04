/*
 * This file is part of the DXX-Rebirth project <https://github.com/dxx-rebirth/dxx-rebirth/>.
 * It is copyright by its individual contributors, as recorded in the
 * project's Git history.  See COPYING.txt at the top level for license
 * terms and a link to the Git history.
 */
/*
 * Taunts: the processing of a sample, its transfer format, the starter
 * horns, the rate limiter and the messages (taunt_sample.h).
 */

#include <algorithm>
#include <cmath>
#include <numbers>
#include <utility>
#include "taunt_sample.h"
#include "sha256.h"

namespace dcx {

namespace taunt {

namespace {

void put_u16(std::uint8_t *p, const unsigned v)
{
	p[0] = static_cast<std::uint8_t>(v);
	p[1] = static_cast<std::uint8_t>(v >> 8);
}

void put_u32(std::uint8_t *p, const std::uint32_t v)
{
	p[0] = static_cast<std::uint8_t>(v);
	p[1] = static_cast<std::uint8_t>(v >> 8);
	p[2] = static_cast<std::uint8_t>(v >> 16);
	p[3] = static_cast<std::uint8_t>(v >> 24);
}

unsigned get_u16(const std::uint8_t *p)
{
	return p[0] | (unsigned{p[1]} << 8);
}

std::uint32_t get_u32(const std::uint8_t *p)
{
	return p[0] | (std::uint32_t{p[1]} << 8) | (std::uint32_t{p[2]} << 16) | (std::uint32_t{p[3]} << 24);
}

/* Whether `id` and `size` suit `kind`: zero for a horn, a possible
 * transfer size for an own sample.
 */
bool valid_sample_ref(const sample_kind kind, const sample_hash &hash, const std::uint32_t size)
{
	if (kind != sample_kind::custom)
		return size == 0 && hash == sample_hash{};
	return size >= WIRE_HEADER_SIZE + 2 * MIN_SAMPLES && size <= MAX_WIRE_SIZE && !((size - WIRE_HEADER_SIZE) & 1);
}

constexpr float two_pi{2 * std::numbers::pi_v<float>};

/* A band-limited sawtooth-like tone: harmonics 1/k below the Nyquist
 * frequency.
 */
float harmonic_tone(const float phase, const float freq, const unsigned max_harmonics)
{
	float v{};
	for (unsigned k{1}; k <= max_harmonics && freq * static_cast<float>(k) < SAMPLE_RATE / 2.0f; ++k)
		v += std::sin(phase * static_cast<float>(k)) / static_cast<float>(k);
	return v;
}

/* Linear attack and release. */
float envelope(const float t, const float length, const float attack, const float release)
{
	if (t < 0 || t >= length)
		return 0;
	if (t < attack)
		return t / attack;
	if (t > length - release)
		return (length - t) / release;
	return 1;
}

std::vector<float> synth_car_horn()
{
	/* Two detuned brass-like tones, the classic two-note car horn. */
	constexpr float length{0.55f};
	std::vector<float> s(static_cast<std::size_t>(length * SAMPLE_RATE));
	for (std::size_t i{}; i != s.size(); ++i)
	{
		const float t{static_cast<float>(i) / SAMPLE_RATE};
		const float v{harmonic_tone(two_pi * 415.0f * t, 415.0f, 10) + harmonic_tone(two_pi * 494.0f * t, 494.0f, 10)};
		s[i] = std::tanh(1.6f * v) * envelope(t, length, 0.015f, 0.04f);
	}
	return s;
}

std::vector<float> synth_bike_bell()
{
	/* Two strikes of a bell: inharmonic partials, each decaying. */
	constexpr float length{0.9f};
	constexpr std::array<std::array<float, 3>, 4> partials{{
		{{2650.0f, 1.0f, 0.35f}},
		{{3975.0f, 0.5f, 0.25f}},
		{{5410.0f, 0.3f, 0.15f}},
		{{1325.0f, 0.2f, 0.30f}},
	}};
	std::vector<float> s(static_cast<std::size_t>(length * SAMPLE_RATE));
	for (std::size_t i{}; i != s.size(); ++i)
	{
		const float t{static_cast<float>(i) / SAMPLE_RATE};
		float v{};
		for (const float strike : {0.0f, 0.16f})
		{
			const float ts{t - strike};
			if (ts < 0)
				continue;
			for (const auto &p : partials)
				v += p[1] * std::sin(two_pi * p[0] * ts) * std::exp(-ts / p[2]);
		}
		s[i] = v * envelope(t, length, 0.001f, 0.05f);
	}
	return s;
}

std::vector<float> synth_air_horn()
{
	/* A truck's air horn: a low chord of three reeds, a short rise in
	 * pitch at the start, a little vibrato, overdriven.
	 */
	constexpr float length{1.0f};
	constexpr std::array<float, 3> notes{{233.0f, 277.0f, 349.0f}};
	std::vector<float> s(static_cast<std::size_t>(length * SAMPLE_RATE));
	std::array<float, 3> phase{};
	for (std::size_t i{}; i != s.size(); ++i)
	{
		const float t{static_cast<float>(i) / SAMPLE_RATE};
		const float rise{t < 0.08f ? 0.96f + 0.04f * t / 0.08f : 1.0f};
		const float vibrato{1.0f + 0.004f * std::sin(two_pi * 5.5f * t)};
		float v{};
		for (std::size_t n{}; n != notes.size(); ++n)
		{
			const float f{notes[n] * rise * vibrato};
			phase[n] = std::fmod(phase[n] + two_pi * f / SAMPLE_RATE, two_pi);
			v += harmonic_tone(phase[n], f, 14);
		}
		s[i] = std::tanh(1.2f * v) * envelope(t, length, 0.03f, 0.08f);
	}
	return s;
}

std::vector<float> synth_beep_beep()
{
	/* Two short beeps, a soft square wave. */
	constexpr float beep{0.13f}, gap{0.09f}, length{2 * beep + gap};
	std::vector<float> s(static_cast<std::size_t>(length * SAMPLE_RATE));
	for (std::size_t i{}; i != s.size(); ++i)
	{
		const float t{static_cast<float>(i) / SAMPLE_RATE};
		const float tb{t < beep ? t : t - beep - gap};
		const float e{envelope(tb, beep, 0.005f, 0.015f)};
		if (e <= 0)
			continue;
		const float ph{two_pi * 880.0f * tb};
		s[i] = e * (std::sin(ph) + std::sin(3 * ph) / 3 + std::sin(5 * ph) / 5);
	}
	return s;
}

}

const char *horn_name(const unsigned n)
{
	switch (n)
	{
		case 1: return "Car horn";
		case 2: return "Bike bell";
		case 3: return "Air horn";
		case 4: return "Beep beep";
		default: return "Horn";
	}
}

std::vector<float> resample(const std::span<const float> in, const unsigned rate)
{
	if (rate == SAMPLE_RATE || in.empty() || !rate)
		return {in.begin(), in.end()};
	const double ratio{static_cast<double>(rate) / SAMPLE_RATE};
	const std::size_t n_out{static_cast<std::size_t>(static_cast<double>(in.size()) / ratio)};
	std::vector<float> out(n_out);
	for (std::size_t i{}; i != n_out; ++i)
	{
		const double pos{static_cast<double>(i) * ratio};
		if (ratio > 1)
		{
			/* Shrinking: the average of the input this output sample
			 * spans (a box filter against aliasing).
			 */
			const std::size_t first{static_cast<std::size_t>(pos)};
			const std::size_t last{std::min(in.size(), std::max(first + 1, static_cast<std::size_t>(pos + ratio)))};
			double sum{};
			for (std::size_t k{first}; k != last; ++k)
				sum += in[k];
			out[i] = static_cast<float>(sum / static_cast<double>(last - first));
		}
		else
		{
			const std::size_t k{static_cast<std::size_t>(pos)};
			const float frac{static_cast<float>(pos - static_cast<double>(k))};
			const float a{in[k]}, b{k + 1 < in.size() ? in[k + 1] : a};
			out[i] = a + (b - a) * frac;
		}
	}
	return out;
}

void trim_silence(std::vector<float> &s)
{
	const auto loud{[](const float v) { return std::fabs(v) >= SILENCE_LEVEL; }};
	const auto first{std::find_if(s.begin(), s.end(), loud)};
	if (first == s.end())
	{
		s.clear();
		return;
	}
	const auto last{std::find_if(s.rbegin(), s.rend(), loud).base()};
	/* Keep 5 ms before the first loud sample: the attack. */
	constexpr std::ptrdiff_t lead{SAMPLE_RATE / 200};
	const auto start{std::distance(s.begin(), first) > lead ? first - lead : s.begin()};
	s.erase(last, s.end());
	s.erase(s.begin(), start);
}

bool cut_and_fade(std::vector<float> &s)
{
	bool cut{};
	if (s.size() > MAX_SAMPLES)
	{
		s.resize(MAX_SAMPLES);
		cut = true;
		for (std::size_t i{}; i != FADE_OUT_SAMPLES; ++i)
			s[MAX_SAMPLES - 1 - i] *= static_cast<float>(i) / FADE_OUT_SAMPLES;
	}
	if (s.size() > 2 * EDGE_FADE_SAMPLES)
		for (std::size_t i{}; i != EDGE_FADE_SAMPLES; ++i)
		{
			const float g{static_cast<float>(i) / EDGE_FADE_SAMPLES};
			s[i] *= g;
			s[s.size() - 1 - i] *= g;
		}
	return cut;
}

float soft_limit(const float v)
{
	const float a{std::fabs(v)};
	if (!(a > LIMIT_KNEE))
		return v;
	constexpr float room{PEAK_TARGET - LIMIT_KNEE};
	return std::copysign(LIMIT_KNEE + room * std::tanh((a - LIMIT_KNEE) / room), v);
}

float loudness(const std::span<const float> s)
{
	if (s.empty())
		return 0;
	/* The energy (sum of squares) and length of each window. */
	std::vector<std::pair<double, std::size_t>> windows;
	windows.reserve(s.size() / LOUDNESS_WINDOW + 1);
	double loudest{};
	for (std::size_t i{}; i < s.size(); i += LOUDNESS_WINDOW)
	{
		const auto w{s.subspan(i, std::min<std::size_t>(LOUDNESS_WINDOW, s.size() - i))};
		double e{};
		for (const float v : w)
			e += static_cast<double>(v) * v;
		windows.emplace_back(e, w.size());
		loudest = std::max(loudest, e / static_cast<double>(w.size()));
	}
	/* The gate is on the RMS: the mean energy is its square.  Each window
	 * counts by its length (a short last one hardly).
	 */
	const double gate{loudest * LOUDNESS_GATE * LOUDNESS_GATE};
	double sum{};
	std::size_t n{};
	for (const auto &[e, len] : windows)
		if (e / static_cast<double>(len) >= gate)
		{
			sum += e;
			n += len;
		}
	return n ? static_cast<float>(std::sqrt(sum / static_cast<double>(n))) : 0.0f;
}

float normalise(std::vector<float> &s)
{
	float peak{};
	for (const float v : s)
	{
		if (!std::isfinite(v))
			return 0;
		peak = std::max(peak, std::fabs(v));
	}
	const float level{loudness(s)};
	if (peak <= 0 || !(level > 0))
		return 0;
	/* Already within the limits (a sample normalised before, as a
	 * receiver gets it): left as it is.
	 */
	if (peak <= PEAK_TARGET && level >= LOUDNESS_TARGET * 0.97f && level <= LOUDNESS_TARGET * 1.005f)
		return 1;
	/* The limiter takes some loudness off what it bends: the gain is
	 * found again from the original a few times, so the result lands on
	 * the target unless MAX_GAIN or MAX_LIMITING stop it.
	 */
	const float max_gain{std::min(MAX_GAIN, PEAK_TARGET * MAX_LIMITING / peak)};
	const std::vector<float> original{s};
	float gain{std::min(LOUDNESS_TARGET / level, max_gain)};
	for (unsigned pass{};; ++pass)
	{
		std::ranges::transform(original, s.begin(), [gain](const float v) { return soft_limit(v * gain); });
		if (pass == 3 || gain >= max_gain)
			break;
		const float now{loudness(s)};
		if (!(now > 0) || now >= LOUDNESS_TARGET * 0.995f)
			break;
		gain = std::min(gain * LOUDNESS_TARGET / now, max_gain);
	}
	return gain;
}

pcm to_pcm(const std::span<const float> s)
{
	pcm out(s.size());
	std::ranges::transform(s, out.begin(), [](const float v) {
		const float c{std::clamp(std::isfinite(v) ? v : 0.0f, -1.0f, 1.0f)};
		return static_cast<std::int16_t>(std::lround(c * 32767.0f));
	});
	return out;
}

std::vector<float> to_float(const std::span<const std::int16_t> s)
{
	std::vector<float> out(s.size());
	std::ranges::transform(s, out.begin(), [](const std::int16_t v) { return static_cast<float>(v) / 32768.0f; });
	return out;
}

std::optional<pcm> prepare(const decoded_audio &a, std::string &error)
{
	if (a.rate < MIN_SOURCE_RATE || a.rate > MAX_SOURCE_RATE)
	{
		error = "unsupported sample rate " + std::to_string(a.rate);
		return std::nullopt;
	}
	auto s{resample(a.samples, a.rate)};
	trim_silence(s);
	if (s.size() < MIN_SAMPLES)
	{
		error = s.empty() ? "the file is silent" : "the sound is shorter than 50 ms";
		return std::nullopt;
	}
	cut_and_fade(s);
	if (!normalise(s))
	{
		error = "the file is silent or broken";
		return std::nullopt;
	}
	return to_pcm(s);
}

std::optional<pcm> prepare_file(const std::span<const std::uint8_t> bytes, std::string &error)
{
	if (bytes.size() > MAX_SOURCE_FILE)
	{
		error = "the file is larger than 16 MiB";
		return std::nullopt;
	}
	const auto a{decode_audio_file(bytes, error)};
	if (!a)
		return std::nullopt;
	return prepare(*a, error);
}

std::vector<std::uint8_t> encode_wire(std::span<const std::int16_t> s)
{
	if (s.size() > MAX_SAMPLES)
		s = s.first(MAX_SAMPLES);
	std::vector<std::uint8_t> out(WIRE_HEADER_SIZE + 2 * s.size());
	std::ranges::copy(WIRE_MAGIC, out.begin());
	put_u16(&out[4], SAMPLE_RATE);
	put_u16(&out[6], 0);
	put_u32(&out[8], static_cast<std::uint32_t>(s.size()));
	auto *p{&out[WIRE_HEADER_SIZE]};
	for (const auto v : s)
	{
		put_u16(p, static_cast<std::uint16_t>(v));
		p += 2;
	}
	return out;
}

std::optional<pcm> decode_wire(const std::span<const std::uint8_t> bytes)
{
	if (bytes.size() < WIRE_HEADER_SIZE || bytes.size() > MAX_WIRE_SIZE)
		return std::nullopt;
	if (!std::ranges::equal(bytes.first(4), WIRE_MAGIC))
		return std::nullopt;
	if (get_u16(&bytes[4]) != SAMPLE_RATE || get_u16(&bytes[6]) != 0)
		return std::nullopt;
	const std::uint32_t count{get_u32(&bytes[8])};
	if (count < MIN_SAMPLES || count > MAX_SAMPLES || bytes.size() != WIRE_HEADER_SIZE + 2 * std::size_t{count})
		return std::nullopt;
	pcm raw(count);
	for (std::size_t i{}; i != count; ++i)
		raw[i] = static_cast<std::int16_t>(get_u16(&bytes[WIRE_HEADER_SIZE + 2 * i]));
	auto s{to_float(raw)};
	cut_and_fade(s);
	if (!normalise(s))
		return std::nullopt;
	return to_pcm(s);
}

sample_hash wire_hash(const std::span<const std::uint8_t> bytes)
{
	return ::dcx::sha256_of(bytes);
}

pcm starter_horn(const unsigned n)
{
	std::vector<float> s;
	switch (n)
	{
		case 2: s = synth_bike_bell(); break;
		case 3: s = synth_air_horn(); break;
		case 4: s = synth_beep_beep(); break;
		default: s = synth_car_horn(); break;
	}
	cut_and_fade(s);
	normalise(s);
	return to_pcm(s);
}

std::vector<std::int16_t> to_mixer_format(const std::span<const std::int16_t> s)
{
	std::vector<std::int16_t> out(s.size() * 4);
	for (std::size_t i{}; i != s.size(); ++i)
	{
		const int a{s[i]}, b{i + 1 < s.size() ? s[i + 1] : a};
		const auto mid{static_cast<std::int16_t>((a + b) / 2)};
		out[4 * i] = out[4 * i + 1] = s[i];
		out[4 * i + 2] = out[4 * i + 3] = mid;
	}
	return out;
}

unsigned rate_limiter::taunts_within(const std::uint64_t now_ms, const std::uint32_t window_ms) const
{
	unsigned n{};
	for (std::size_t i{}; i != count; ++i)
		if (times[i] <= now_ms && now_ms - times[i] < window_ms)
			++n;
	return n;
}

verdict rate_limiter::check(const std::uint64_t now_ms, const rate_limits &limits)
{
	if (locked_until > now_ms)
		return verdict::locked;
	/* `times` keeps the newest KEPT taunts: enough for either window as
	 * long as the limits stay below KEPT.
	 */
	if (taunts_within(now_ms, limits.burst_window_ms) >= limits.burst || taunts_within(now_ms, limits.window_ms) >= limits.per_window)
	{
		locked_until = now_ms + limits.lockout_ms;
		return verdict::exceeded;
	}
	if (count == KEPT)
	{
		std::shift_left(times.begin(), times.end(), 1);
		--count;
	}
	times[count++] = now_ms;
	return verdict::allowed;
}

void taunt_request_msg::write(const std::span<std::uint8_t, SIZE> buf) const
{
	buf[0] = static_cast<std::uint8_t>(kind);
	put_u32(&buf[1], size);
	std::ranges::copy(hash, &buf[5]);
}

std::optional<taunt_request_msg> taunt_request_msg::read(const std::span<const std::uint8_t> buf)
{
	if (buf.size() != SIZE || !valid_sample_kind(buf[0]))
		return std::nullopt;
	taunt_request_msg m;
	m.kind = static_cast<sample_kind>(buf[0]);
	m.size = get_u32(&buf[1]);
	std::ranges::copy(buf.subspan(5, 32), m.hash.begin());
	if (!valid_sample_ref(m.kind, m.hash, m.size))
		return std::nullopt;
	return m;
}

void taunt_msg::write(const std::span<std::uint8_t, SIZE> buf) const
{
	buf[0] = pid;
	buf[1] = static_cast<std::uint8_t>(kind);
	put_u32(&buf[2], size);
	std::ranges::copy(hash, &buf[6]);
}

std::optional<taunt_msg> taunt_msg::read(const std::span<const std::uint8_t> buf)
{
	if (buf.size() != SIZE || buf[0] > MAX_PLAYER_ID || !valid_sample_kind(buf[1]))
		return std::nullopt;
	taunt_msg m;
	m.pid = buf[0];
	m.kind = static_cast<sample_kind>(buf[1]);
	m.size = get_u32(&buf[2]);
	std::ranges::copy(buf.subspan(6, 32), m.hash.begin());
	if (!valid_sample_ref(m.kind, m.hash, m.size))
		return std::nullopt;
	return m;
}

std::optional<mute_command> parse_mute_command(std::string_view text)
{
	const auto starts_with_nocase{[](const std::string_view t, const std::string_view prefix) {
		return t.size() >= prefix.size() && std::ranges::equal(t.substr(0, prefix.size()), prefix, [](const char a, const char b) {
			return (a >= 'A' && a <= 'Z' ? a - 'A' + 'a' : a) == b;
		});
	}};
	mute_command c;
	if (starts_with_nocase(text, "/unmute"))
		text.remove_prefix(7);
	else if (starts_with_nocase(text, "/mute"))
	{
		c.mute = true;
		text.remove_prefix(5);
	}
	else
		return std::nullopt;
	/* The command ends here, or a separator follows ("/mutex" is no
	 * command).
	 */
	if (!text.empty() && text.front() != ' ' && text.front() != ':')
		return std::nullopt;
	if (!text.empty() && text.front() == ':')
		text.remove_prefix(1);
	while (!text.empty() && text.front() == ' ')
		text.remove_prefix(1);
	while (!text.empty() && text.back() == ' ')
		text.remove_suffix(1);
	c.name = text;
	return c;
}

}

}
