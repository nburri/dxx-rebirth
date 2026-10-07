/*
 * This file is part of the DXX-Rebirth project <https://github.com/dxx-rebirth/dxx-rebirth/>.
 * It is copyright by its individual contributors, as recorded in the
 * project's Git history.  See COPYING.txt at the top level for license
 * terms and a link to the Git history.
 */
/*
 * Test of the mission transfer (net_v2_mission.h): file names, the HOG
 * and MN2 checks (with a fuzz test of the HOG check), the bundle
 * manifest and its hash, the choice of a local mission by hash, both
 * ends' state machines over a direct link (refusals, corruption, a
 * stall, a resumed download), and the transfer of a 10 MB mission over
 * the real reliable transport (net_v2_transport.h) through a simulated
 * link with a bottleneck, delay and loss, which measures the throughput.
 */

#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <thread>
#ifndef _WIN32
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif
#include <cstdlib>
#include <deque>
#include <map>
#include <random>
#include <string>
#include <vector>

#include "net_v2_mission.h"
#include "net_v2_transport.h"
#include "sha256.h"

namespace {

using namespace dcx::net_v2;

unsigned failures;

void check_failed(const char *const what, const char *const file, const unsigned line)
{
	std::fprintf(stderr, "%s:%u: check failed: %s\n", file, line, what);
	++failures;
}

#define CHECK(cond)	do { if (!(cond)) check_failed(#cond, __FILE__, __LINE__); } while (0)

using bytes = std::vector<std::uint8_t>;

bytes text(const std::string_view s)
{
	return bytes(s.begin(), s.end());
}

/* A HOG of `entries` files of the given sizes, with random contents. */
bytes make_hog(const std::vector<std::pair<std::string, std::size_t>> &entries, const unsigned seed)
{
	std::mt19937 r(seed);
	bytes b{'D', 'H', 'F'};
	for (const auto &[name, size] : entries)
	{
		const auto at{b.size()};
		b.resize(at + 17 + size);
		std::copy_n(name.data(), std::min<std::size_t>(name.size(), 12), &b[at]);
		net_put_le32(&b[at + 13], static_cast<std::uint32_t>(size));
		for (std::size_t i{}; i != size; ++i)
			b[at + 17 + i] = static_cast<std::uint8_t>(r());
	}
	return b;
}

mission_file file_of(const std::string &name, const bytes &b)
{
	return {name, static_cast<std::uint32_t>(b.size()), dcx::sha256_of(b)};
}

const bytes Mn2{text("name = CORONA (Sny)\ntype = normal\nnum_levels = 1\ncorona.rl2\n")};

mission_manifest manifest_of(const bytes &mn2, const bytes &hog)
{
	mission_manifest m;
	m.basename = "Corona";
	m.title = "CORONA (Sny)";
	m.files = {file_of("Corona.MN2", mn2), file_of("Corona.HOG", hog)};
	return m;
}

void test_names()
{
	CHECK(mission_basename_valid("Corona"));
	CHECK(mission_basename_valid("GGC-TOP"));
	CHECK(mission_basename_valid("a_b-1234"));
	CHECK(!mission_basename_valid(""));
	CHECK(!mission_basename_valid("toolongname"));
	CHECK(!mission_basename_valid("../x"));
	CHECK(!mission_basename_valid("a/b"));
	CHECK(!mission_basename_valid("a\\b"));
	CHECK(!mission_basename_valid("a b"));
	CHECK(!mission_basename_valid("a.b"));
	CHECK(!mission_basename_valid("C:"));
	CHECK(mission_file_type_of("Corona.HOG") == mission_file_type::hog);
	CHECK(mission_file_type_of("corona.mn2") == mission_file_type::mn2);
	CHECK(mission_file_type_of("Corona.Mn2") == mission_file_type::mn2);
	CHECK(!mission_file_type_of("Corona.exe"));
	CHECK(!mission_file_type_of("Corona.hog.exe"));
	CHECK(!mission_file_type_of("Corona.ham"));
	CHECK(!mission_file_type_of("../Corona.hog"));
	CHECK(!mission_file_type_of(".hog"));
	CHECK(!mission_file_type_of("Corona"));
	CHECK(!mission_file_type_of("missions/Corona.hog"));
	CHECK(mission_download_dir(mission_hash{{0xab, 0xcd, 0x01, 0x23, 0x45, 0x67, 0x89}}) == "missions/downloaded/abcd01234567");
	std::printf("    names: basenames, extensions, folders\n");
}

void test_hog()
{
	const auto hog{make_hog({{"corona.rl2", 3000}, {"corona.pog", 100}, {"empty.txb", 0}}, 1)};
	CHECK(mission_hog_valid(hog));
	CHECK(mission_hog_valid(make_hog({}, 2)));
	CHECK(!mission_hog_valid(text("DH")));
	CHECK(!mission_hog_valid(text("XHFabc")));
	{
		/* Cut short anywhere after the signature: invalid. */
		unsigned valid_cuts{};
		for (std::size_t n{3}; n < hog.size(); ++n)
			if (mission_hog_valid(std::span(hog).first(n)))
				++valid_cuts;
		/* Only the cuts at an entry boundary are valid HOGs: the bare
		 * signature, after the first and after the second entry.
		 */
		CHECK(valid_cuts == 3);
	}
	{
		auto b{hog};
		net_put_le32(&b[3 + 13], 0xffffffffu);
		CHECK(!mission_hog_valid(b));
		net_put_le32(&b[3 + 13], 3001);
		CHECK(!mission_hog_valid(b));
	}
	{
		/* A name without NUL in its 13 bytes, an empty name, a path. */
		auto b{hog};
		std::fill_n(&b[3], 13, 'a');
		CHECK(!mission_hog_valid(b));
		b = hog;
		b[3] = 0;
		CHECK(!mission_hog_valid(b));
		b = hog;
		b[4] = '/';
		CHECK(!mission_hog_valid(b));
		b = hog;
		b[4] = '\\';
		CHECK(!mission_hog_valid(b));
		b = hog;
		b[4] = 0x01;
		CHECK(!mission_hog_valid(b));
	}
	{
		std::vector<std::pair<std::string, std::size_t>> many(MISSION_HOG_MAX_ENTRIES + 1, {"x.rl2", 0});
		CHECK(!mission_hog_valid(make_hog(many, 3)));
		many.pop_back();
		CHECK(mission_hog_valid(make_hog(many, 3)));
	}
	/* Fuzz: mutated and truncated HOGs never read out of bounds (run
	 * under ASan/UBSan to see that); the check only says yes or no.
	 */
	std::mt19937 r(99);
	unsigned accepted{};
	for (unsigned i{}; i != 20000; ++i)
	{
		auto b{hog};
		const auto flips{1 + r() % 8};
		for (std::uint32_t k{}; k != flips; ++k)
			b[r() % b.size()] = static_cast<std::uint8_t>(r());
		if (r() % 4 == 0)
			b.resize(r() % b.size());
		if (r() % 4 == 0)
			b.resize(b.size() + r() % 40, static_cast<std::uint8_t>(r()));
		accepted += mission_hog_valid(b);
	}
	std::printf("    HOG: entries, cut short, bad sizes and names, %u entries; fuzz: %u of 20000 mutants accepted\n", static_cast<unsigned>(MISSION_HOG_MAX_ENTRIES), accepted);
}

void test_mn2()
{
	CHECK(mission_mn2_valid(Mn2));
	CHECK(mission_mn2_valid(text("\r\n  zname = Vertigo\n")));
	CHECK(mission_mn2_valid(text("!name=x")));
	CHECK(mission_mn2_valid(text("NAME=x")));
	CHECK(!mission_mn2_valid(text("")));
	CHECK(!mission_mn2_valid(text("type = normal\nname = x\n")));
	{
		auto b{Mn2};
		b[10] = 0;
		CHECK(!mission_mn2_valid(b));
	}
	{
		bytes big(MISSION_MN2_MAX + 1, 'x');
		std::ranges::copy(std::string_view{"name=x\n"}, big.begin());
		CHECK(!mission_mn2_valid(big));
		big.resize(MISSION_MN2_MAX);
		CHECK(mission_mn2_valid(big));
	}
	std::printf("    MN2: name line, NUL bytes, 64 KiB cap\n");
}

void test_manifest()
{
	const auto hog{make_hog({{"corona.rl2", 5000}}, 4)};
	const auto m{manifest_of(Mn2, hog)};
	CHECK(m.valid());
	const auto w{mission_manifest_msg::write(m)};
	CHECK(w.size() <= NET_V2_MAX_MESSAGE);
	const auto r{mission_manifest_msg::read(w)};
	CHECK(r && r->basename == "Corona" && r->title == "CORONA (Sny)" && r->files.size() == 2 && r->bundle_hash() == m.bundle_hash());
	CHECK(r && r->total_size() == Mn2.size() + hog.size());
	/* The order of the files and the case of their names do not change
	 * the bundle; their contents do (another version).
	 */
	{
		auto o{m};
		std::swap(o.files[0], o.files[1]);
		o.files[0].name = "CORONA.hog";
		CHECK(o.bundle_hash() == m.bundle_hash());
		auto hog2{hog};
		hog2.back() ^= 1;
		CHECK(manifest_of(Mn2, hog2).bundle_hash() != m.bundle_hash());
		auto mn2b{Mn2};
		mn2b.push_back('\n');
		CHECK(manifest_of(mn2b, hog).bundle_hash() != m.bundle_hash());
		/* The title is not part of it: the .mn2 is. */
		o = m;
		o.title = "Other";
		CHECK(o.bundle_hash() == m.bundle_hash());
	}
	/* Malformed manifests. */
	CHECK(!mission_manifest_msg::read(std::span(w).first(w.size() - 1)));
	{
		auto b{w};
		b[0] ^= 1;	/* the claimed bundle hash */
		CHECK(!mission_manifest_msg::read(b));
		b = w;
		b.back() ^= 1;	/* a file hash: the bundle hash no longer matches */
		CHECK(!mission_manifest_msg::read(b));
		b = w;
		b[32 + 7] = 'x';	/* after the basename's NUL */
		CHECK(!mission_manifest_msg::read(b));
		b = w;
		b[mission_manifest_msg::FIXED - 1] = 3;
		CHECK(!mission_manifest_msg::read(b));
	}
	const auto rejected{[](mission_manifest o) {
		const auto b{mission_manifest_msg::write(o)};
		return !mission_manifest_msg::read(b) && !o.valid();
	}};
	{
		auto o{m};
		o.files[1].name = "Corona.exe";
		CHECK(rejected(o));
		o = m;
		o.files[1].name = "Other.hog";
		CHECK(rejected(o));
		o = m;
		o.files[1].name = "Corona.mn2";
		CHECK(rejected(o));	/* two .mn2 */
		o = m;
		o.files.pop_back();
		CHECK(o.valid());	/* a mission without HOG */
		o = m;
		o.files[1].size = MISSION_FILE_MAX + 1;
		CHECK(rejected(o));
		o = m;
		o.files[0].size = MISSION_MN2_MAX + 1;
		CHECK(rejected(o));
		o = m;
		o.files[1].size = 0;
		CHECK(rejected(o));
		o = m;
		o.basename = "../x";
		CHECK(rejected(o));
		o = m;
		o.title = "bad\ntitle";
		CHECK(rejected(o));
		o = m;
		o.files.push_back(o.files[1]);
		CHECK(rejected(o));
	}
	/* GAME_SETTINGS' announcement. */
	{
		const mission_announcement a{m.bundle_hash(), static_cast<std::uint32_t>(m.total_size()), true};
		std::array<std::uint8_t, mission_announcement::SIZE> b;
		a.write(b.data());
		const auto r2{mission_announcement::read(b.data())};
		CHECK(r2.known() && r2.bundle == a.bundle && r2.size == a.size && r2.sends);
		net_put_le32(&b[32], MISSION_BUNDLE_MAX + 1);
		CHECK(!mission_announcement::read(b.data()).known());
		const mission_announcement none{};
		none.write(b.data());
		CHECK(!mission_announcement::read(b.data()).known());
	}
	std::printf("    manifest: round trip, bundle hash (order, case, versions), %u malformed and invalid ones refused\n", 15u);
}

void test_choice()
{
	const mission_hash want{{1, 2, 3}}, other{{4, 5, 6}}, other2{{7}};
	const mission_announcement a{want, 1000, true};
	const std::vector<mission_candidate> none, mine{{0, other}}, both{{0, other}, {1, other2}, {2, want}};
	auto c{choose_mission(a, both, true)};
	CHECK(c.decision == mission_decision::use_local && c.index == 2);
	/* Same name, another version: download, never the other one. */
	c = choose_mission(a, mine, true);
	CHECK(c.decision == mission_decision::download && c.other_version);
	c = choose_mission(a, none, true);
	CHECK(c.decision == mission_decision::download && !c.other_version);
	/* No download possible: another version is tried, or the reason. */
	c = choose_mission(a, mine, false);
	CHECK(c.decision == mission_decision::use_local && c.index == 0 && c.other_version);
	c = choose_mission(a, none, false);
	CHECK(c.decision == mission_decision::missing_player_refuses);
	c = choose_mission({want, 1000, false}, none, true);
	CHECK(c.decision == mission_decision::missing_host_does_not_send);
	c = choose_mission({want, MISSION_BUNDLE_MAX + 1, true}, none, true);
	CHECK(c.decision == mission_decision::missing_too_large);
	/* Nothing announced (a built-in mission): by name. */
	c = choose_mission({}, both, true);
	CHECK(c.decision == mission_decision::use_by_name);
	std::printf("    choice: by hash among versions, download, refusals\n");
}

/* Both ends over a direct, lossless, in-order link (the reliable
 * stream's guarantees), with a window counted by what the other end has
 * not taken yet.
 */
struct direct_env final : mission_env
{
	std::deque<std::pair<std::uint8_t, bytes>> to_peer;
	std::map<std::string, bytes> files;	/* host: its files; client: stored ones */
	std::vector<std::string> notes;
	unsigned bulk{};
	unsigned host_reads{};
	/* Client: corrupt every DATA chunk it sends with this offset. */
	std::optional<std::uint32_t> corrupt_offset;
	void send(std::uint8_t, const std::uint8_t type, const std::span<const std::uint8_t> payload) override
	{
		bytes b(payload.begin(), payload.end());
		if (corrupt_offset && type == MISSION_MSG_DATA && net_get_le32(&b[37]) == *corrupt_offset)
			b.back() ^= 0x55;
		to_peer.emplace_back(type, std::move(b));
	}
	std::size_t queued_bytes(std::uint8_t) override
	{
		std::size_t n{};
		for (const auto &m : to_peer)
			n += m.second.size();
		return n;
	}
	link_counters link(std::uint8_t) override
	{
		return {};
	}
	void set_bulk(std::uint8_t, const unsigned packets) override
	{
		bulk = packets;
	}
	std::shared_ptr<const std::vector<std::uint8_t>> host_file(const mission_manifest &, const mission_file &f) override
	{
		++host_reads;
		const auto i{files.find(f.name)};
		if (i == files.end() || dcx::sha256_of(i->second) != f.hash)
			return nullptr;
		return std::make_shared<const bytes>(i->second);
	}
	bool client_has_file(const mission_manifest &, const mission_file &f) override
	{
		const auto i{files.find(f.name)};
		return i != files.end() && dcx::sha256_of(i->second) == f.hash;
	}
	bool client_store(const mission_manifest &, const mission_file &f, const std::span<const std::uint8_t> b) override
	{
		files[f.name] = bytes(b.begin(), b.end());
		return true;
	}
	void note(const std::string_view what) override
	{
		notes.emplace_back(what);
	}
};

bool running(const mission_client &c)
{
	return c.state() == mission_client_state::waiting_manifest || c.state() == mission_client_state::downloading;
}

struct direct_pair
{
	direct_env he, ce;
	mission_host host{he};
	mission_client client{ce};
	/* Deliver at most `budget` messages each way. */
	void step(const double seconds, const bool in_level = false, const unsigned budget = 1000)
	{
		host.pump(seconds, in_level);
		client.tick(seconds);
		for (unsigned i{}; i != budget && !he.to_peer.empty(); ++i)
		{
			auto [t, b]{std::move(he.to_peer.front())};
			he.to_peer.pop_front();
			client.receive(0, t, b);
		}
		for (unsigned i{}; i != budget && !ce.to_peer.empty(); ++i)
		{
			auto [t, b]{std::move(ce.to_peer.front())};
			ce.to_peer.pop_front();
			host.receive(1, t, b);
		}
	}
};

void test_direct()
{
	const auto hog{make_hog({{"corona.rl2", 200000}, {"corona.pog", 30000}}, 5)};
	const auto m{manifest_of(Mn2, hog)};
	/* A complete download: the .hog first, the .mn2 last, bulk on and off. */
	{
		direct_pair p;
		p.he.files = {{"Corona.MN2", Mn2}, {"Corona.HOG", hog}};
		p.host.set_manifest(m);
		p.client.begin(m.bundle_hash());
		p.host.client_joined(1);
		bool saw_bulk{}, saw_progress{};
		for (unsigned i{}; i != 400 && p.client.state() != mission_client_state::done; ++i)
		{
			p.step(0.01);
			saw_bulk |= p.he.bulk && p.ce.bulk;
			if (const auto pr{p.host.progress(1)}; pr && *pr > 0 && *pr < 100)
				saw_progress = true;
		}
		CHECK(p.client.state() == mission_client_state::done);
		CHECK(p.ce.files.size() == 2 && p.ce.files["Corona.HOG"] == hog && p.ce.files["Corona.MN2"] == Mn2);
		CHECK(saw_bulk && !p.ce.bulk);
		CHECK(saw_progress && p.client.percent() == 100);
		/* The order of the stores: the .mn2 last. */
		bool hog_first{};
		for (const auto &n : p.ce.notes)
		{
			if (n == "received Corona.HOG")
				hog_first = true;
			if (n == "received Corona.MN2")
				CHECK(hog_first);
		}
		p.step(0.01);
		CHECK(!p.host.busy(1) && !p.he.bulk);
	}
	/* Already has the .hog (an interrupted earlier download): only the
	 * .mn2 comes.
	 */
	{
		direct_pair p;
		p.he.files = {{"Corona.MN2", Mn2}, {"Corona.HOG", hog}};
		p.ce.files = {{"Corona.HOG", hog}};
		p.host.set_manifest(m);
		p.client.begin(m.bundle_hash());
		p.host.client_joined(1);
		for (unsigned i{}; i != 100 && p.client.state() != mission_client_state::done; ++i)
			p.step(0.01);
		CHECK(p.client.state() == mission_client_state::done && p.he.host_reads == 1);
	}
	/* The host does not send missions: no manifest; a request is refused. */
	{
		direct_pair p;
		p.he.files = {{"Corona.MN2", Mn2}, {"Corona.HOG", hog}};
		p.host.set_manifest(m);
		p.host.enabled = false;
		p.client.begin(m.bundle_hash());
		p.host.client_joined(1);
		for (unsigned i{}; i != 1700 && p.client.state() == mission_client_state::waiting_manifest; ++i)
			p.step(0.01);
		CHECK(p.client.state() == mission_client_state::failed && p.client.error().find("did not describe") != std::string::npos);
		std::array<std::uint8_t, MISSION_REQUEST_SIZE> b;
		mission_request_msg{m.files[1].hash, 0}.write(b.data());
		p.host.receive(1, MISSION_MSG_REQUEST, b);
		CHECK(p.he.to_peer.size() == 1 && p.he.to_peer.front().first == MISSION_MSG_UNAVAILABLE && p.he.to_peer.front().second[33] == static_cast<std::uint8_t>(mission_unavailable_reason::refused));
	}
	/* The host announced another bundle than it describes. */
	{
		direct_pair p;
		p.he.files = {{"Corona.MN2", Mn2}, {"Corona.HOG", hog}};
		p.host.set_manifest(m);
		p.client.begin(mission_hash{{9}});
		p.host.client_joined(1);
		p.step(0.01);
		CHECK(p.client.state() == mission_client_state::failed);
	}
	/* The host's file changed on its disk: unreadable. */
	{
		direct_pair p;
		auto hog2{hog};
		hog2[100] ^= 1;
		p.he.files = {{"Corona.MN2", Mn2}, {"Corona.HOG", hog2}};
		p.host.set_manifest(m);
		p.client.begin(m.bundle_hash());
		p.host.client_joined(1);
		for (unsigned i{}; i != 20 && running(p.client); ++i)
			p.step(0.01);
		CHECK(p.client.state() == mission_client_state::failed && p.client.error().find("cannot read") != std::string::npos);
	}
	/* Corrupted data in transit (a host that lies): the SHA-256 refuses it
	 * and nothing is stored.
	 */
	{
		direct_pair p;
		p.he.files = {{"Corona.MN2", Mn2}, {"Corona.HOG", hog}};
		p.he.corrupt_offset = static_cast<std::uint32_t>(MISSION_DATA_CHUNK * 3);
		p.host.set_manifest(m);
		p.client.begin(m.bundle_hash());
		p.host.client_joined(1);
		for (unsigned i{}; i != 400 && running(p.client); ++i)
			p.step(0.01);
		CHECK(p.client.state() == mission_client_state::failed && p.client.error().find("SHA-256") != std::string::npos);
		CHECK(p.ce.files.empty());
	}
	/* A file that hashes right but is no HOG: refused by its check. */
	{
		direct_pair p;
		bytes junk(5000, 'J');
		auto m2{manifest_of(Mn2, junk)};
		p.he.files = {{"Corona.MN2", Mn2}, {"Corona.HOG", junk}};
		p.host.set_manifest(m2);
		p.client.begin(m2.bundle_hash());
		p.host.client_joined(1);
		for (unsigned i{}; i != 100 && running(p.client); ++i)
			p.step(0.01);
		CHECK(p.client.state() == mission_client_state::failed && p.client.error().find("not a valid HOG") != std::string::npos);
		CHECK(p.ce.files.empty());
	}
	/* The host stops (gone silent): the client gives up after the stall
	 * timeout; a new connection resumes where the first stopped.
	 */
	{
		direct_pair p;
		p.he.files = {{"Corona.MN2", Mn2}, {"Corona.HOG", hog}};
		p.host.set_manifest(m);
		p.client.begin(m.bundle_hash());
		p.host.client_joined(1);
		for (unsigned i{}; i != 6; ++i)
			p.step(0.01, false, 40);
		const auto got{p.client.received()};
		CHECK(got > 0 && got < hog.size());
		double t{};
		while (running(p.client) && t < 60)
		{
			p.client.tick(0.5);
			t += 0.5;
		}
		CHECK(p.client.state() == mission_client_state::failed && t >= MISSION_STALL_TIMEOUT && t < MISSION_STALL_TIMEOUT + 1);
		/* Rejoin: a fresh host side (new connection), the client keeps its
		 * part of the .hog and asks from there.
		 */
		p.he.to_peer.clear();
		p.ce.to_peer.clear();
		p.host.slot_cleared(1);
		p.client.begin(m.bundle_hash());
		p.host.client_joined(1);
		/* The manifest only: the client asks from where it stopped. */
		CHECK(p.he.to_peer.size() == 1);
		if (!p.he.to_peer.empty())
		{
			p.client.receive(0, p.he.to_peer.front().first, p.he.to_peer.front().second);
			p.he.to_peer.pop_front();
		}
		CHECK(p.ce.to_peer.size() == 1);
		if (!p.ce.to_peer.empty())
		{
			const auto r{mission_request_msg::read(p.ce.to_peer.front().second)};
			CHECK(r && r->offset == got);
		}
		for (unsigned i{}; i != 400 && p.client.state() != mission_client_state::done; ++i)
			p.step(0.01);
		CHECK(p.client.state() == mission_client_state::done && p.ce.files["Corona.HOG"] == hog);
	}
	/* Pacing: in a level, at most MISSION_RATE_LEVEL. */
	{
		direct_pair p;
		const auto big{make_hog({{"big.rl2", 2000000}}, 6)};
		const auto m3{manifest_of(Mn2, big)};
		p.he.files = {{"Corona.MN2", Mn2}, {"Corona.HOG", big}};
		p.host.set_manifest(m3);
		p.client.begin(m3.bundle_hash());
		p.host.client_joined(1);
		for (unsigned i{}; i != 500; ++i)
			p.step(0.01, true);
		const auto rate{static_cast<double>(p.client.received()) / 5.0};
		CHECK(rate <= MISSION_RATE_LEVEL * 1.1 && rate >= MISSION_RATE_LEVEL * 0.5);
		CHECK(p.he.bulk == MISSION_LEVEL_PACKETS_PER_TICK);
		std::printf("    direct: complete, partial, refused, wrong bundle, unreadable, corrupted, not a HOG, stalled and resumed; in a level %.0f KiB/s\n", rate / 1024);
	}
	/* Messages from anyone but the host, malformed ones: ignored. */
	{
		direct_pair p;
		p.host.set_manifest(m);
		p.client.begin(m.bundle_hash());
		const auto w{mission_manifest_msg::write(m)};
		p.client.receive(2, MISSION_MSG_MANIFEST, w);
		CHECK(p.client.state() == mission_client_state::waiting_manifest);
		p.host.receive(1, MISSION_MSG_REQUEST, w);
		p.host.receive(0, MISSION_MSG_REQUEST, w);
		CHECK(p.he.to_peer.empty());
		CHECK(!mission_data_msg::read(bytes(10, 3)));
		bytes d(MISSION_DATA_HEADER + 10, 0);
		mission_data_msg{mission_hash{}, 5, 0, std::span(d).first(10)}.write(d.data());
		CHECK(!mission_data_msg::read(d));	/* more data than the file has */
		CHECK(!mission_request_msg::read(bytes(MISSION_REQUEST_SIZE, 1)));	/* kind 1 */
	}
}

/* The transport: two real connections through a link with a bottleneck
 * (a queue drained at `bandwidth`, dropping what does not fit in
 * `buffer`), a one-way delay, random loss, both ways.  The host and the
 * client run frames at `fps`.
 */
struct link_sim
{
	double bandwidth;
	std::size_t buffer;
	double delay;
	double loss;
	struct packet
	{
		double at;
		bytes b;
	};
	std::deque<packet> queue;	/* waiting at the bottleneck */
	std::deque<packet> flight;	/* past it, arriving at `at` */
	std::size_t queued{};
	double busy_until{};
	std::mt19937 rng{11};
	unsigned dropped{}, sent{};
	void push(const double now, const std::span<const std::uint8_t> p)
	{
		++sent;
		if (std::uniform_real_distribution<double>(0, 1)(rng) < loss || queued + p.size() > buffer)
		{
			++dropped;
			return;
		}
		queued += p.size();
		queue.push_back({now, bytes(p.begin(), p.end())});
	}
	void advance(const double now)
	{
		while (!queue.empty())
		{
			auto &q{queue.front()};
			const double start{std::max(busy_until, q.at)};
			const double done{start + static_cast<double>(q.b.size() + 28) / bandwidth};
			if (done > now)
				break;
			busy_until = done;
			queued -= q.b.size();
			flight.push_back({done + delay, std::move(q.b)});
			queue.pop_front();
		}
	}
};

struct sim_end final : mission_env
{
	connection *c{};
	std::map<std::string, bytes> files;
	unsigned bulk{};
	void send(std::uint8_t, const std::uint8_t type, const std::span<const std::uint8_t> payload) override
	{
		if (c->enqueue_reliable(type, payload) != enqueue_result::ok)
		{
			++failures;
			std::fprintf(stderr, "cannot queue type %u\n", type);
		}
	}
	std::size_t queued_bytes(std::uint8_t) override
	{
		return c->stats().queue_bytes;
	}
	link_counters link(std::uint8_t) override
	{
		const auto s{c->stats()};
		return {s.message_sends, s.message_resends, s.rtt_valid ? static_cast<double>(s.srtt) / 65536.0 : 0.0};
	}
	void set_bulk(std::uint8_t, const unsigned packets) override
	{
		bulk = packets;
		c->set_max_packets_per_tick(packets ? packets : NET_V2_DEFAULT_MAX_PACKETS_PER_TICK);
	}
	std::shared_ptr<const std::vector<std::uint8_t>> host_file(const mission_manifest &, const mission_file &f) override
	{
		const auto i{files.find(f.name)};
		return i == files.end() ? nullptr : std::make_shared<const bytes>(i->second);
	}
	bool client_has_file(const mission_manifest &, const mission_file &) override
	{
		return false;
	}
	bool client_store(const mission_manifest &, const mission_file &f, const std::span<const std::uint8_t> b) override
	{
		files[f.name] = bytes(b.begin(), b.end());
		return true;
	}
	void note(std::string_view) override
	{
	}
};

struct transfer_result
{
	bool done;
	double seconds;
	double rate;
	std::uint64_t resends, sends;
	unsigned dropped, sent;
};

transfer_result run_transfer(const bytes &hog, const double bandwidth, const std::size_t buffer, const double delay, const double loss, const double fps, const bool in_level = false)
{
	const auto m{manifest_of(Mn2, hog)};
	sim_end he{}, ce{};
	connection hc{connection_config{.session_id = 7, .peer_token = 0x77, .local_player_id = 0, .remote_player_id = 1, .timeout = net_seconds(60), .unacked_timeout = net_seconds(60)}, 0};
	connection cc{connection_config{.session_id = 7, .peer_token = 0x77, .local_player_id = 1, .remote_player_id = 0, .timeout = net_seconds(60), .unacked_timeout = net_seconds(60)}, 0};
	he.c = &hc;
	ce.c = &cc;
	he.files = {{"Corona.MN2", Mn2}, {"Corona.HOG", hog}};
	mission_host host{he};
	mission_client client{ce};
	host.set_manifest(m);
	link_sim down{bandwidth, buffer, delay, loss, {}, {}}, up{bandwidth, buffer, delay, loss, {}, {}};
	up.rng.seed(12);
	client.begin(m.bundle_hash());
	host.client_joined(1);
	const double frame{1.0 / fps};
	double now{}, started{-1};
	const auto to_clock{[](const double t) { return static_cast<net_clock>(t * 65536.0); }};
	while (now < 120 && client.state() != mission_client_state::done && client.state() != mission_client_state::failed)
	{
		now += frame;
		const auto t{to_clock(now)};
		down.advance(now);
		up.advance(now);
		while (!down.flight.empty() && down.flight.front().at <= now)
		{
			const auto r{cc.on_receive(down.flight.front().b, t)};
			for (const auto &msg : r.reliable)
				client.receive(0, msg.type, msg.payload);
			down.flight.pop_front();
		}
		while (!up.flight.empty() && up.flight.front().at <= now)
		{
			const auto r{hc.on_receive(up.flight.front().b, t)};
			for (const auto &msg : r.reliable)
				host.receive(1, msg.type, msg.payload);
			up.flight.pop_front();
		}
		if (started < 0 && client.state() == mission_client_state::downloading)
			started = now;
		host.pump(frame, in_level);
		client.tick(frame);
		for (auto [c, l] : {std::pair{&hc, &down}, std::pair{&cc, &up}})
		{
			c->begin_tick(t);
			for (;;)
			{
				const auto p{c->build_outgoing(t)};
				if (p.empty())
					break;
				l->push(now, p);
			}
		}
		CHECK(hc.state() != connection_state::closed && cc.state() != connection_state::closed);
		if (hc.state() == connection_state::closed || cc.state() == connection_state::closed)
			break;
	}
	const bool done{client.state() == mission_client_state::done && ce.files["Corona.HOG"] == hog && ce.files["Corona.MN2"] == Mn2};
	const double secs{now - std::max(started, 0.0)};
	const auto s{hc.stats()};
	return {done, secs, static_cast<double>(hog.size() + Mn2.size()) / secs, s.message_resends, s.message_sends, down.dropped, down.sent};
}

void test_throughput()
{
	const auto hog{make_hog({{"big.rl2", 10u << 20}}, 8)};
	struct scenario
	{
		const char *name;
		double bandwidth;
		std::size_t buffer;
		double delay, loss, fps;
		double min_rate;
		bool in_level;
	};
	/* min_rate: what each must at least reach (bytes/s). */
	static constexpr scenario scenarios[]{
		{"good link (8 MB/s, 20 ms, 0.5 % loss), 60 fps", 8e6, 512 << 10, 0.020, 0.005, 60, 1.0e6, false},
		{"good link, 30 fps menus", 8e6, 512 << 10, 0.020, 0.005, 30, 1.0e6, false},
		{"good link, 2 % loss", 8e6, 512 << 10, 0.020, 0.02, 60, 0.8e6, false},
		{"far host (50 ms each way), 1 % loss", 8e6, 512 << 10, 0.050, 0.01, 60, 0.8e6, false},
		{"slow uplink (1 MB/s, 64 KB buffer)", 1e6, 64 << 10, 0.020, 0.0, 60, 0.5e6, false},
		{"slow uplink (0.5 MB/s), 1 % loss", 0.5e6, 64 << 10, 0.020, 0.01, 60, 0.25e6, false},
		{"bad link, 5 % loss", 8e6, 512 << 10, 0.030, 0.05, 60, 0.3e6, false},
	};
	for (const auto &sc : scenarios)
	{
		const auto r{run_transfer(hog, sc.bandwidth, sc.buffer, sc.delay, sc.loss, sc.fps, sc.in_level)};
		CHECK(r.done);
		CHECK(r.rate >= sc.min_rate);
		std::printf("    10 MB, %s: %.1f s, %.2f MB/s, %" PRIu64 " of %" PRIu64 " messages resent, %u of %u packets dropped%s\n", sc.name, r.seconds, r.rate / 1e6, r.resends, r.sends, r.dropped, r.sent, r.done && r.rate >= sc.min_rate ? "" : "  <-- FAILED");
	}
	/* During a level (a join in progress): paced at MISSION_RATE_LEVEL. */
	{
		const auto small{make_hog({{"mid.rl2", 1u << 20}}, 9)};
		const auto r{run_transfer(small, 8e6, 512 << 10, 0.020, 0.005, 60, true)};
		CHECK(r.done && r.rate <= MISSION_RATE_LEVEL * 1.15);
		std::printf("    1 MB during a level: %.1f s, %.0f KiB/s\n", r.seconds, r.rate / 1024);
	}
}

/* --missions DIR: every mission of a real missions folder (the group's)
 * through the checks, then the largest over real UDP sockets on the
 * loopback interface, in real time.
 */
std::optional<bytes> slurp(const std::filesystem::path &p)
{
	std::ifstream f(p, std::ios::binary);
	if (!f)
		return std::nullopt;
	return bytes(std::istreambuf_iterator<char>(f), {});
}

struct real_bundle
{
	mission_manifest manifest;
	std::map<std::string, bytes> files;
};

std::vector<real_bundle> read_missions(const std::filesystem::path &dir)
{
	std::map<std::string, real_bundle> by_base;
	for (const auto &e : std::filesystem::directory_iterator(dir))
	{
		const auto name{e.path().filename().string()};
		const auto t{mission_file_type_of(name)};
		if (!t)
			continue;
		const auto base{name.substr(0, name.find('.'))};
		auto &b{by_base[mission_lower(base)]};
		b.manifest.basename = base;
		if (const auto data{slurp(e.path())})
		{
			b.manifest.files.push_back(file_of(name, *data));
			b.files[name] = *data;
		}
	}
	std::vector<real_bundle> out;
	for (auto &[k, b] : by_base)
	{
		/* The title: the .mn2's name line, as the game reads it. */
		for (const auto &[n, d] : b.files)
			if (mission_file_type_of(n) == mission_file_type::mn2)
			{
				const std::string t(d.begin(), std::find(d.begin(), d.end(), '\n'));
				const auto eq{t.find('=')};
				b.manifest.title = eq == std::string::npos ? b.manifest.basename : t.substr(t.find_first_not_of(' ', eq + 1));
				while (!b.manifest.title.empty() && (b.manifest.title.back() == '\r' || b.manifest.title.back() == ' '))
					b.manifest.title.pop_back();
				b.manifest.title.resize(std::min<std::size_t>(b.manifest.title.size(), MISSION_TITLE_FIELD - 1));
			}
		b.manifest.normalise();
		out.push_back(std::move(b));
	}
	return out;
}

#ifndef _WIN32
struct udp_socket
{
	int fd{-1};
	sockaddr_in addr{};
	udp_socket()
	{
		fd = socket(AF_INET, SOCK_DGRAM, 0);
		addr.sin_family = AF_INET;
		addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
		addr.sin_port = 0;
		bind(fd, reinterpret_cast<const sockaddr *>(&addr), sizeof(addr));
		socklen_t l{sizeof(addr)};
		getsockname(fd, reinterpret_cast<sockaddr *>(&addr), &l);
		fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
		/* A large receive buffer, as an operating system's default is. */
		const int size{1 << 20};
		setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &size, sizeof(size));
	}
	~udp_socket()
	{
		close(fd);
	}
	void send_to(const udp_socket &to, const std::span<const std::uint8_t> p) const
	{
		sendto(fd, p.data(), p.size(), 0, reinterpret_cast<const sockaddr *>(&to.addr), sizeof(to.addr));
	}
	std::optional<bytes> receive() const
	{
		bytes b(1500);
		const auto n{recv(fd, b.data(), b.size(), 0)};
		if (n <= 0)
			return std::nullopt;
		b.resize(static_cast<std::size_t>(n));
		return b;
	}
};

void loopback_transfer(const real_bundle &rb)
{
	sim_end he{}, ce{};
	connection hc{connection_config{.session_id = 9, .peer_token = 0x99, .local_player_id = 0, .remote_player_id = 1, .timeout = net_seconds(60), .unacked_timeout = net_seconds(60)}, 0};
	connection cc{connection_config{.session_id = 9, .peer_token = 0x99, .local_player_id = 1, .remote_player_id = 0, .timeout = net_seconds(60), .unacked_timeout = net_seconds(60)}, 0};
	he.c = &hc;
	ce.c = &cc;
	he.files = rb.files;
	mission_host host{he};
	mission_client client{ce};
	host.set_manifest(rb.manifest);
	CHECK(host.current().has_value());
	udp_socket hs, cs;
	client.begin(rb.manifest.bundle_hash());
	host.client_joined(1);
	const auto t0{std::chrono::steady_clock::now()};
	auto last{t0};
	unsigned frames{};
	while (client.state() != mission_client_state::done && client.state() != mission_client_state::failed)
	{
		const auto now_tp{std::chrono::steady_clock::now()};
		const double now{std::chrono::duration<double>(now_tp - t0).count()};
		if (now > 120)
			break;
		const double dt{std::chrono::duration<double>(now_tp - last).count()};
		last = now_tp;
		const auto t{static_cast<net_clock>(now * 65536.0)};
		while (const auto d{cs.receive()})
			for (const auto &m : cc.on_receive(*d, t).reliable)
				client.receive(0, m.type, m.payload);
		while (const auto d{hs.receive()})
			for (const auto &m : hc.on_receive(*d, t).reliable)
				host.receive(1, m.type, m.payload);
		host.pump(dt, false);
		client.tick(dt);
		for (auto [c, from, to] : {std::tuple{&hc, &hs, &cs}, std::tuple{&cc, &cs, &hs}})
		{
			c->begin_tick(t);
			for (;;)
			{
				const auto p{c->build_outgoing(t)};
				if (p.empty())
					break;
				from->send_to(*to, p);
			}
		}
		++frames;
		/* A game frame at some 120 fps. */
		std::this_thread::sleep_until(now_tp + std::chrono::microseconds(8333));
	}
	const double secs{std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count()};
	bool same{client.state() == mission_client_state::done};
	for (const auto &[n, d] : rb.files)
		same = same && ce.files[n] == d;
	CHECK(same);
	const auto s{hc.stats()};
	std::printf("    loopback UDP: '%s' (%.1f MB) in %.1f s real time, %.2f MB/s, %u frames, %" PRIu64 " of %" PRIu64 " messages resent%s\n", rb.manifest.title.c_str(), static_cast<double>(rb.manifest.total_size()) / 1e6, secs, static_cast<double>(rb.manifest.total_size()) / 1e6 / secs, frames, s.message_resends, s.message_sends, same ? "" : "  <-- FAILED");
}
#endif

void test_missions_dir(const char *const dir)
{
	const auto bundles{read_missions(dir)};
	unsigned valid{}, hogs_ok{}, hogs{};
	const real_bundle *largest{};
	for (const auto &b : bundles)
	{
		for (const auto &[n, d] : b.files)
			if (mission_file_type_of(n) == mission_file_type::hog)
			{
				++hogs;
				hogs_ok += mission_hog_valid(d);
				if (!mission_hog_valid(d))
					std::printf("    %s: not a valid HOG\n", n.c_str());
			}
		const bool ok{b.manifest.valid()};
		valid += ok;
		if (!ok)
			std::printf("    %s: not a bundle (%u files)\n", b.manifest.basename.c_str(), static_cast<unsigned>(b.manifest.files.size()));
		else
		{
			const auto w{mission_manifest_msg::write(b.manifest)};
			const auto r{mission_manifest_msg::read(w)};
			CHECK(r && r->bundle_hash() == b.manifest.bundle_hash());
			if (!largest || b.manifest.total_size() > largest->manifest.total_size())
				largest = &b;
		}
	}
	std::printf("    %s: %u missions, %u valid bundles, %u of %u HOGs valid; largest '%s' %.1f MiB (cap %u MiB)\n", dir, static_cast<unsigned>(bundles.size()), valid, hogs_ok, hogs, largest ? largest->manifest.title.c_str() : "-", largest ? static_cast<double>(largest->manifest.total_size()) / (1 << 20) : 0.0, MISSION_BUNDLE_MAX >> 20);
	CHECK(hogs_ok == hogs);
#ifndef _WIN32
	if (largest)
		loopback_transfer(*largest);
#endif
}

}

int main(const int argc, char **const argv)
{
	std::printf("test-net-v2-mission:\n");
	if (argc == 3 && !std::strcmp(argv[1], "--missions"))
	{
		test_missions_dir(argv[2]);
		std::printf("test-net-v2-mission: %s\n", failures ? "checks failed" : "all checks passed");
		return failures ? EXIT_FAILURE : EXIT_SUCCESS;
	}
	test_names();
	test_hog();
	test_mn2();
	test_manifest();
	test_choice();
	test_direct();
	test_throughput();
	if (failures)
	{
		std::printf("test-net-v2-mission: %u checks failed\n", failures);
		return EXIT_FAILURE;
	}
	std::printf("test-net-v2-mission: all checks passed\n");
	return EXIT_SUCCESS;
}
