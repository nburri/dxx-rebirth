/*
 * This file is part of the DXX-Rebirth project <https://github.com/dxx-rebirth/dxx-rebirth/>.
 * It is copyright by its individual contributors, as recorded in the
 * project's Git history.  See COPYING.txt at the top level for license
 * terms and a link to the Git history.
 */
/*
 * Test of the taunts (taunt_sample.h): the rate limiter, the decoding of
 * WAV, Ogg Vorbis, MP3 and FLAC files, the trimming, the fade and the
 * loudness limits, broken and hostile input, the transfer format, the
 * starter horns and the messages.  The argument is the directory of the
 * test files (default common/unittest/data, from the top of the tree).
 */

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <random>
#include <string>
#include <vector>

#include "taunt_sample.h"

using namespace dcx::taunt;

namespace {

void check_failed(const char *const expr, const char *const file, const int line)
{
	std::fprintf(stderr, "%s:%d: check failed: %s\n", file, line, expr);
	std::exit(1);
}

#define CHECK(cond)	do { if (!(cond)) check_failed(#cond, __FILE__, __LINE__); } while (0)

std::string data_dir{"common/unittest/data"};

std::vector<std::uint8_t> read_file(const std::string &name)
{
	std::ifstream f{data_dir + "/" + name, std::ios::binary};
	if (!f)
	{
		std::fprintf(stderr, "cannot read %s/%s\n", data_dir.c_str(), name.c_str());
		std::exit(1);
	}
	return {std::istreambuf_iterator<char>{f}, std::istreambuf_iterator<char>{}};
}

struct levels
{
	float peak{};
	float rms{};
};

levels measure(const pcm &s)
{
	levels l;
	double e{};
	for (const auto v : s)
	{
		const float f{static_cast<float>(v) / 32768.0f};
		l.peak = std::max(l.peak, std::fabs(f));
		e += static_cast<double>(f) * f;
	}
	if (!s.empty())
		l.rms = static_cast<float>(std::sqrt(e / static_cast<double>(s.size())));
	return l;
}

/* Every sample the game would play obeys the limits. */
void check_limits(const pcm &s)
{
	CHECK(s.size() >= MIN_SAMPLES && s.size() <= MAX_SAMPLES);
	const auto l{measure(s)};
	CHECK(l.peak <= PEAK_TARGET + 0.001f);
	CHECK(l.rms <= RMS_TARGET + 0.001f);
	/* The ends are faded: no click. */
	CHECK(std::abs(s.front()) < 400 && std::abs(s.back()) < 400);
}

/* Zero crossings per second of the first `n` samples. */
double crossings_per_second(const pcm &s, const std::size_t n)
{
	unsigned c{};
	for (std::size_t i{1}; i < n && i < s.size(); ++i)
		if ((s[i - 1] < 0) != (s[i] < 0))
			++c;
	return c * double{SAMPLE_RATE} / static_cast<double>(std::min(n, s.size()));
}

void put16(std::vector<std::uint8_t> &v, const unsigned x)
{
	v.push_back(static_cast<std::uint8_t>(x));
	v.push_back(static_cast<std::uint8_t>(x >> 8));
}

void put32(std::vector<std::uint8_t> &v, const std::uint32_t x)
{
	put16(v, x & 0xffff);
	put16(v, x >> 16);
}

/* A WAV file of `seconds` of a `freq` sine at `amp` (after `silence`
 * seconds of silence), PCM of `bits` (8, 16, 24) or float (32 with
 * `is_float`).
 */
std::vector<std::uint8_t> make_wav(const unsigned rate, const unsigned channels, const unsigned bits, const bool is_float, const double seconds, const double freq, const double amp, const double silence = 0)
{
	const std::size_t frames{static_cast<std::size_t>((seconds + silence) * rate)};
	const unsigned bytes_per_sample{bits / 8};
	std::vector<std::uint8_t> data;
	for (std::size_t i{}; i != frames; ++i)
	{
		const double t{static_cast<double>(i) / rate};
		const double v{t < silence ? 0.0 : amp * std::sin(2 * 3.14159265358979 * freq * t)};
		for (unsigned c{}; c != channels; ++c)
		{
			if (is_float)
			{
				const float f{static_cast<float>(v)};
				std::uint32_t u;
				std::memcpy(&u, &f, 4);
				put32(data, u);
			}
			else if (bits == 8)
				data.push_back(static_cast<std::uint8_t>(128 + std::lround(v * 127)));
			else
			{
				const long s{std::lround(v * ((1L << (bits - 1)) - 1))};
				for (unsigned b{}; b != bytes_per_sample; ++b)
					data.push_back(static_cast<std::uint8_t>(static_cast<unsigned long>(s) >> (8 * b)));
			}
		}
	}
	std::vector<std::uint8_t> f{'R', 'I', 'F', 'F'};
	put32(f, static_cast<std::uint32_t>(36 + data.size()));
	for (const char c : {'W', 'A', 'V', 'E', 'f', 'm', 't', ' '})
		f.push_back(static_cast<std::uint8_t>(c));
	put32(f, 16);
	put16(f, is_float ? 3 : 1);
	put16(f, channels);
	put32(f, rate);
	put32(f, rate * channels * bytes_per_sample);
	put16(f, channels * bytes_per_sample);
	put16(f, bits);
	for (const char c : {'d', 'a', 't', 'a'})
		f.push_back(static_cast<std::uint8_t>(c));
	put32(f, static_cast<std::uint32_t>(data.size()));
	f.insert(f.end(), data.begin(), data.end());
	return f;
}

void test_rate_limiter()
{
	rate_limiter r;
	/* A burst of three within 2 s. */
	CHECK(r.check(1000, HOST_LIMITS) == verdict::allowed);
	CHECK(r.check(1300, HOST_LIMITS) == verdict::allowed);
	CHECK(r.check(1600, HOST_LIMITS) == verdict::allowed);
	/* The fourth: over the limit, locked out for 5 s. */
	CHECK(r.check(1900, HOST_LIMITS) == verdict::exceeded);
	CHECK(r.lockout_left(1900) == 5000);
	CHECK(r.check(3000, HOST_LIMITS) == verdict::locked);
	CHECK(r.check(6899, HOST_LIMITS) == verdict::locked);
	/* After the lockout: one more fits in the 10 s window (3 so far). */
	CHECK(r.check(6900, HOST_LIMITS) == verdict::allowed);
	CHECK(r.lockout_left(6900) == 0);
	/* Four within 10 s: the fifth is refused. */
	CHECK(r.check(9500, HOST_LIMITS) == verdict::exceeded);
	/* Steady taunting at 4 per 10 s goes on for ever. */
	rate_limiter s;
	for (unsigned i{}; i != 40; ++i)
		CHECK(s.check(100000 + i * 2600, HOST_LIMITS) == verdict::allowed);
	/* Faster than that is caught. */
	rate_limiter f;
	unsigned allowed{};
	for (unsigned i{}; i != 100; ++i)
		allowed += f.check(i * 1000, HOST_LIMITS) == verdict::allowed;
	CHECK(allowed <= 4 * 10 + 4);
	/* Receivers allow one more in each window. */
	rate_limiter rx;
	for (const unsigned t : {0u, 100u, 200u, 300u})
		CHECK(rx.check(t, RECEIVER_LIMITS) == verdict::allowed);
	CHECK(rx.check(400, RECEIVER_LIMITS) == verdict::exceeded);
	/* A host-allowed sequence, bunched by the network, passes the
	 * receivers.
	 */
	rate_limiter host, receiver;
	std::mt19937 rng{7};
	std::uniform_int_distribution<unsigned> gap{0, 4000}, delay{0, 300};
	std::uint64_t t{}, last_arrival{};
	for (unsigned i{}; i != 2000; ++i)
	{
		t += gap(rng);
		if (host.check(t, HOST_LIMITS) != verdict::allowed)
			continue;
		/* Arrival in order, each delayed 0-300 ms, bunching the sends. */
		last_arrival = std::max(last_arrival, t + delay(rng));
		CHECK(receiver.check(last_arrival, RECEIVER_LIMITS) == verdict::allowed);
	}
	/* What the sender allowed and played, delayed by up to 300 ms on
	 * its way, passes the host.
	 */
	{
		rate_limiter sender, at_host;
		std::mt19937 jitter{11};
		std::uint64_t ts{}, arrival{};
		for (unsigned i{}; i != 3000; ++i)
		{
			ts += gap(jitter);
			if (sender.check(ts, SENDER_LIMITS) != verdict::allowed)
				continue;
			arrival = std::max(arrival, ts + delay(jitter));
			CHECK(at_host.check(arrival, HOST_LIMITS) == verdict::allowed);
		}
	}
		/* The clock starting at 0 works. */
	rate_limiter z;
	CHECK(z.check(0, HOST_LIMITS) == verdict::allowed);
	z.reset();
	CHECK(z.lockout_left(0) == 0);
}

void test_processing()
{
	/* Resampling keeps the pitch: 1 kHz at 48 kHz and at 8 kHz. */
	for (const unsigned rate : {48000u, 44100u, 8000u, 11025u, 96000u})
	{
		std::string error;
		const auto s{prepare_file(make_wav(rate, 1, 16, false, 1.0, 1000, 0.5), error)};
		CHECK(s);
		CHECK(std::abs(static_cast<long>(s->size()) - static_cast<long>(SAMPLE_RATE)) < 300);
		const double c{crossings_per_second(*s, s->size())};
		CHECK(c > 1960 && c < 2040);
		check_limits(*s);
	}
	/* Formats of WAV: 8, 16, 24 bit, float, stereo. */
	for (const auto &[bits, is_float] : {std::pair{8u, false}, {16u, false}, {24u, false}, {32u, true}})
	{
		std::string error;
		const auto s{prepare_file(make_wav(44100, 2, bits, is_float, 0.5, 500, 0.6), error)};
		CHECK(s);
		check_limits(*s);
		const double c{crossings_per_second(*s, s->size())};
		CHECK(c > 960 && c < 1040);
	}
	/* Leading silence is cut; a long sound is cut to 2.0 s with a fade. */
	{
		std::string error;
		const auto s{prepare_file(make_wav(22050, 1, 16, false, 3.0, 300, 0.5, 1.0), error)};
		CHECK(s);
		CHECK(s->size() == MAX_SAMPLES);
		check_limits(*s);
		/* Loud at once (the silence is gone)... */
		const auto head{measure(pcm(s->begin() + 400, s->begin() + 2000))};
		CHECK(head.peak > 0.25f);
		/* ...and fading over the last 50 ms. */
		const auto tail{measure(pcm(s->end() - 300, s->end()))};
		const auto before{measure(pcm(s->end() - static_cast<long>(FADE_OUT_SAMPLES) - 600, s->end() - static_cast<long>(FADE_OUT_SAMPLES) - 300))};
		CHECK(tail.peak < before.peak * 0.4f);
	}
	/* Loudness: a full-scale square wave is brought down to the limits;
	 * a quiet one is raised by at most MAX_GAIN.
	 */
	{
		std::vector<float> loud(SAMPLE_RATE);
		for (std::size_t i{}; i != loud.size(); ++i)
			loud[i] = (i / 25) & 1 ? 1.0f : -1.0f;
		const float g{normalise(loud)};
		CHECK(g > 0 && g <= RMS_TARGET + 0.001f);
		std::vector<float> quiet(SAMPLE_RATE);
		for (std::size_t i{}; i != quiet.size(); ++i)
			quiet[i] = 0.02f * std::sin(static_cast<float>(i) * 0.1f);
		CHECK(normalise(quiet) == MAX_GAIN);
	}
	/* Silent and too short files are refused. */
	{
		std::string error;
		CHECK(!prepare_file(make_wav(22050, 1, 16, false, 1.0, 300, 0.0), error));
		CHECK(!error.empty());
		CHECK(!prepare_file(make_wav(22050, 1, 16, false, 0.02, 300, 0.5), error));
	}
	/* A NaN from a float file does not survive. */
	{
		auto f{make_wav(22050, 1, 32, true, 0.5, 300, 0.5)};
		const float nan{std::nanf("")};
		std::memcpy(&f[44 + 4 * 1000], &nan, 4);
		std::string error;
		const auto s{prepare_file(f, error)};
		CHECK(s);
		check_limits(*s);
	}
}

void test_files()
{
	for (const char *const name : {"taunt-test.ogg", "taunt-test.mp3", "taunt-test.flac"})
	{
		const auto bytes{read_file(name)};
		std::string error;
		const auto a{decode_audio_file(bytes, error)};
		if (!a)
			std::fprintf(stderr, "%s: %s\n", name, error.c_str());
		CHECK(a);
		CHECK(a->rate >= 16000 && a->rate <= 48000);
		const auto s{prepare(*a, error)};
		CHECK(s);
		/* 2.4 s of tone after 0.3 s of silence: cut to 2.0 s. */
		CHECK(s->size() == MAX_SAMPLES);
		check_limits(*s);
		/* The tone was at the RMS limit's level: the RMS lands there. */
		CHECK(measure(*s).rms > RMS_TARGET * 0.9f);
		/* The first 20 ms are already the tone (the silence is gone);
		 * the decoders' leading delay (MP3) is short.
		 */
		CHECK(measure(pcm(s->begin() + 600, s->begin() + 1100)).peak > 0.2f);
		std::printf("taunt: %s decoded as %s at %u Hz, %zu samples\n", name, a->format, a->rate, s->size());
	}
}

void test_hostile_files()
{
	std::mt19937 rng{1};
	const auto pick{[&rng](const std::size_t n) { return std::uniform_int_distribution<std::size_t>{0, n - 1}(rng); }};
	std::string error;
	/* Nothing, garbage, unknown formats. */
	CHECK(!decode_audio_file({}, error));
	std::vector<std::uint8_t> garbage(5000);
	for (auto &b : garbage)
		b = static_cast<std::uint8_t>(rng());
	garbage[0] = 'X';
	CHECK(!decode_audio_file(garbage, error));
	/* Absurd WAV headers. */
	{
		auto f{make_wav(22050, 1, 16, false, 0.5, 300, 0.5)};
		auto bad{f};
		bad[24] = 0x40; bad[25] = 0x42; bad[26] = 0x0f; bad[27] = 0;	/* 1 MHz */
		CHECK(!prepare_file(bad, error));
		bad = f;
		bad[22] = 200;	/* 200 channels */
		CHECK(!prepare_file(bad, error));
	}
	/* Truncated and mutated files of every format: whatever comes out
	 * obeys the limits, and nothing crashes.
	 */
	std::vector<std::vector<std::uint8_t>> inputs{read_file("taunt-test.ogg"), read_file("taunt-test.mp3"), read_file("taunt-test.flac"), make_wav(44100, 2, 16, false, 0.5, 440, 0.5)};
	for (const auto &input : inputs)
	{
		for (const std::size_t cut : {std::size_t{0}, std::size_t{4}, std::size_t{12}, std::size_t{44}, std::size_t{100}, std::size_t{1000}, input.size() / 2, input.size() - 1})
		{
			const std::span<const std::uint8_t> t{input.data(), std::min(cut, input.size())};
			if (const auto s{prepare_file(t, error)})
				check_limits(*s);
		}
		for (unsigned round{}; round != 150; ++round)
		{
			auto m{input};
			const std::size_t flips{1 + pick(40)};
			for (std::size_t k{}; k != flips; ++k)
			{
				/* Bias toward the headers. */
				const std::size_t at{pick(2) ? pick(std::min<std::size_t>(m.size(), 200)) : pick(m.size())};
				m[at] ^= static_cast<std::uint8_t>(1u << pick(8));
			}
			if (const auto s{prepare_file(m, error)})
				check_limits(*s);
		}
	}
}

void test_wire()
{
	std::string error;
	const auto s{prepare_file(make_wav(22050, 1, 16, false, 1.5, 600, 0.5), error)};
	CHECK(s);
	const auto w{encode_wire(*s)};
	CHECK(w.size() == WIRE_HEADER_SIZE + 2 * s->size());
	CHECK(w.size() <= MAX_WIRE_SIZE && MAX_WIRE_SIZE <= 100 * 1024);
	const auto d{decode_wire(w)};
	CHECK(d);
	CHECK(d->size() == s->size());
	/* Equal but for the ends, which the receiver fades once more. */
	for (std::size_t i{EDGE_FADE_SAMPLES}; i != s->size() - EDGE_FADE_SAMPLES; ++i)
		CHECK(std::abs((*d)[i] - (*s)[i]) <= 40);
	/* The longest sample fits. */
	CHECK(encode_wire(pcm(MAX_SAMPLES + 100, 100)).size() == MAX_WIRE_SIZE);
	/* Broken transfers. */
	const auto broken{[&w](const std::size_t at, const std::uint8_t value) {
		auto b{w};
		b[at] = value;
		return !decode_wire(b);
	}};
	CHECK(broken(0, 'X'));						/* magic */
	CHECK(broken(4, 0x44));						/* rate */
	CHECK(broken(6, 1));						/* flags */
	CHECK(broken(8, static_cast<std::uint8_t>(w[8] + 1)));	/* count against size */
	CHECK(broken(11, 1));						/* huge count */
	CHECK(!decode_wire(std::span<const std::uint8_t>{w}.first(w.size() - 1)));
	CHECK(!decode_wire(std::span<const std::uint8_t>{w}.first(WIRE_HEADER_SIZE - 1)));
	CHECK(!decode_wire(encode_wire(pcm(MIN_SAMPLES - 1, 1000))));
	CHECK(!decode_wire(encode_wire(pcm(MIN_SAMPLES, 0))));	/* silent */
	/* A hostile peer's full-scale sample is brought to the limits. */
	{
		pcm loud(MAX_SAMPLES);
		for (std::size_t i{}; i != loud.size(); ++i)
			loud[i] = (i / 20) & 1 ? 32767 : -32768;
		const auto l{decode_wire(encode_wire(loud))};
		CHECK(l);
		check_limits(*l);
	}
	/* The id: CRC-32. */
	const std::uint8_t check[]{'1', '2', '3', '4', '5', '6', '7', '8', '9'};
	CHECK(wire_id(check) == 0xcbf43926u);
	CHECK(wire_id(w) != wire_id(encode_wire(starter_horn(1))));
}

void test_horns()
{
	std::vector<pcm> horns;
	for (unsigned n{1}; n <= STARTER_HORNS; ++n)
	{
		const auto h{starter_horn(n)};
		check_limits(h);
		/* Loud enough to be heard: within 6 dB of the limits. */
		const auto l{measure(h)};
		CHECK(l.peak > PEAK_TARGET * 0.5f || l.rms > RMS_TARGET * 0.5f);
		/* Through the transfer format unchanged in length. */
		CHECK(decode_wire(encode_wire(h))->size() == h.size());
		for (const auto &other : horns)
			CHECK(other != h);
		horns.push_back(h);
		CHECK(std::strlen(horn_name(n)) > 0);
	}
	/* The same every time (every machine makes its own). */
	CHECK(starter_horn(3) == horns[2]);
	CHECK(starter_horn(0) == horns[0]);
}

void test_mixer_format()
{
	const pcm s{0, 100, -100};
	const auto m{to_mixer_format(s)};
	CHECK(m.size() == 12);
	CHECK(m[0] == 0 && m[1] == 0 && m[2] == 50 && m[3] == 50);
	CHECK(m[4] == 100 && m[6] == 0);
	CHECK(m[8] == -100 && m[10] == -100);
}

void test_messages()
{
	{
		const taunt_request_msg m{sample_kind::custom, 0x12345678u, 50000};
		std::array<std::uint8_t, taunt_request_msg::SIZE> buf;
		m.write(buf);
		const auto r{taunt_request_msg::read(buf)};
		CHECK(r && r->kind == sample_kind::custom && r->id == 0x12345678u && r->size == 50000);
		const taunt_request_msg h{sample_kind::horn3, 0, 0};
		h.write(buf);
		CHECK(taunt_request_msg::read(buf)->kind == sample_kind::horn3);
		/* A horn names no sample; an own sample has a possible size. */
		buf[1] = 1;
		CHECK(!taunt_request_msg::read(buf));
		taunt_request_msg{sample_kind::custom, 1, 11}.write(buf);
		CHECK(!taunt_request_msg::read(buf));
		taunt_request_msg{sample_kind::custom, 1, MAX_WIRE_SIZE + 2}.write(buf);
		CHECK(!taunt_request_msg::read(buf));
		taunt_request_msg{sample_kind::custom, 1, WIRE_HEADER_SIZE + 2 * MIN_SAMPLES + 1}.write(buf);
		CHECK(!taunt_request_msg::read(buf));
		for (const std::uint8_t k : {0, 5, 0x0f, 0x11, 0xff})
		{
			buf[0] = k;
			CHECK(!taunt_request_msg::read(buf));
		}
		CHECK(!taunt_request_msg::read(std::span<const std::uint8_t>{buf}.first(8)));
	}
	{
		const taunt_msg m{5, sample_kind::custom, 0xdeadbeefu, MAX_WIRE_SIZE};
		std::array<std::uint8_t, taunt_msg::SIZE> buf;
		m.write(buf);
		const auto r{taunt_msg::read(buf)};
		CHECK(r && r->pid == 5 && r->kind == sample_kind::custom && r->id == 0xdeadbeefu && r->size == MAX_WIRE_SIZE);
		taunt_msg{7, sample_kind::horn1, 0, 0}.write(buf);
		CHECK(taunt_msg::read(buf));
		buf[0] = 8;
		CHECK(!taunt_msg::read(buf));
		buf[0] = 0;
		buf[1] = 9;
		CHECK(!taunt_msg::read(buf));
		std::array<std::uint8_t, taunt_msg::SIZE + 1> longer{};
		CHECK(!taunt_msg::read(longer));
	}
	CHECK(horn_kind(2) == sample_kind::horn2 && horn_kind(9) == sample_kind::horn1);
}

void test_mute_command()
{
	const auto m{parse_mute_command("/mute Bob")};
	CHECK(m && m->mute && m->name == "Bob");
	const auto u{parse_mute_command("/UNMUTE:  alice  ")};
	CHECK(u && !u->mute && u->name == "alice");
	const auto l{parse_mute_command("/mute")};
	CHECK(l && l->mute && l->name.empty());
	CHECK(!parse_mute_command("/mutex"));
	CHECK(!parse_mute_command("hello /mute x"));
	CHECK(!parse_mute_command("/kick: x"));
}

}

int main(const int argc, char **const argv)
{
	if (argc > 1)
		data_dir = argv[1];
	test_rate_limiter();
	test_processing();
	test_files();
	test_hostile_files();
	test_wire();
	test_horns();
	test_mixer_format();
	test_messages();
	test_mute_command();
	std::puts("taunt: all checks passed");
	return 0;
}
