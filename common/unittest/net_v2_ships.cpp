/*
 * This file is part of the DXX-Rebirth project <https://github.com/dxx-rebirth/dxx-rebirth/>.
 * It is copyright by its individual contributors, as recorded in the
 * project's Git history.  See COPYING.txt at the top level for license
 * terms and a link to the Git history.
 */
/*
 * Test of the custom ship exchange (net_v2_ships.h): the message layouts
 * and their checks, the assembly of a ship, and a host with clients and
 * a bot over simulated reliable links: everyone ends up with everyone's
 * ship, the host fetching what it lacks from the owner; a refusing
 * client gets nothing; corrupted data, an owner who leaves, data from
 * the wrong peer; the pacing and the queue limit.
 */

#include <cstdio>
#include <cstdlib>
#include <deque>
#include <random>
#include <string>
#include <vector>

#include "net_v2_ships.h"
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

ship_hash sha_of(const std::vector<std::uint8_t> &b)
{
	return dcx::sha256_of(b);
}

asset_key hash_of(const std::vector<std::uint8_t> &b)
{
	return {static_cast<std::uint8_t>(asset_kind::ship), sha_of(b)};
}

std::vector<std::uint8_t> make_ship(const unsigned seed, const std::size_t size)
{
	std::vector<std::uint8_t> b(size);
	std::mt19937 r(seed);
	for (auto &c : b)
		c = static_cast<std::uint8_t>(r());
	return b;
}

void test_messages()
{
	const auto ship{make_ship(1, 5000)};
	ship_info_msg i;
	i.pid = 3;
	i.pyro = false;
	i.size = 5000;
	i.hash = sha_of(ship);
	i.name = "striker";
	std::array<std::uint8_t, ship_info_msg::SIZE> buf;
	i.write(buf.data());
	const auto r{ship_info_msg::read(buf)};
	CHECK(r && r->pid == 3 && !r->pyro && r->size == 5000 && r->hash == i.hash && r->name == "striker");
	/* Malformed: wrong length, unknown flag, bad name, no NUL, a Pyro
	 * with data, a ship without size or over the cap.
	 */
	CHECK(!ship_info_msg::read(std::span(buf).first(ship_info_msg::SIZE - 1)));
	{
		auto b{buf};
		b[1] = 2;
		CHECK(!ship_info_msg::read(b));
	}
	{
		auto b{buf};
		b[6 + SHIP_HASH_SIZE] = 'X';
		CHECK(!ship_info_msg::read(b));
	}
	{
		auto b{buf};
		std::fill_n(&b[6 + SHIP_HASH_SIZE], SHIP_NAME_FIELD, 'a');
		CHECK(!ship_info_msg::read(b));
	}
	{
		auto b{buf};
		b[1] = 1;
		CHECK(!ship_info_msg::read(b));
	}
	{
		auto m{i};
		m.size = SHIP_MAX_SIZE + 1;
		auto b{buf};
		m.write(b.data());
		CHECK(!ship_info_msg::read(b));
		m.size = 0;
		m.write(b.data());
		CHECK(!ship_info_msg::read(b));
	}
	{
		ship_info_msg pyro;
		pyro.pid = 1;
		auto b{buf};
		pyro.write(b.data());
		const auto p{ship_info_msg::read(b)};
		CHECK(p && p->pyro && p->name.empty());
	}
	/* SHIP_DATA */
	{
		const ship_data_msg d{hash_of(ship), 5000, 896, std::span(ship).subspan(896, 896)};
		std::array<std::uint8_t, SHIP_DATA_HEADER + SHIP_DATA_CHUNK> b;
		d.write(b.data());
		const auto r2{ship_data_msg::read(std::span(b).first(d.size()))};
		CHECK(r2 && r2->offset == 896 && r2->total == 5000 && r2->data.size() == 896 && std::equal(r2->data.begin(), r2->data.end(), ship.begin() + 896));
		CHECK(!ship_data_msg::read(std::span(b).first(SHIP_DATA_HEADER)));
		/* Past the total. */
		const ship_data_msg late{hash_of(ship), 1000, 500, std::span(ship).first(600)};
		late.write(b.data());
		CHECK(!ship_data_msg::read(std::span(b).first(late.size())));
		const ship_data_msg big{hash_of(ship), SHIP_MAX_SIZE + 1, 0, std::span(ship).first(10)};
		big.write(b.data());
		CHECK(!ship_data_msg::read(std::span(b).first(big.size())));
		std::vector<std::uint8_t> too_long(SHIP_DATA_HEADER + SHIP_DATA_CHUNK + 1);
		CHECK(!ship_data_msg::read(too_long));
	}
	{
		std::array<std::uint8_t, ship_unavailable_msg::SIZE> b{};
		b[1 + SHIP_HASH_SIZE] = 2;
		/* Kind 0 is none. */
		CHECK(!ship_unavailable_msg::read(b));
		b[0] = 1;
		b[1 + SHIP_HASH_SIZE] = 9;
		CHECK(!ship_unavailable_msg::read(b));
		b[1 + SHIP_HASH_SIZE] = 2;
		CHECK(ship_unavailable_msg::read(b) && ship_unavailable_msg::read(b)->reason == ship_unavailable_reason::owner_left);
		CHECK(!ship_request_msg::read(std::span(b).first(5)));
		/* Per-kind limits: a taunt over 128 KiB is refused, a ship is not. */
		const auto ship{make_ship(3, 2000)};
		std::array<std::uint8_t, SHIP_DATA_HEADER + SHIP_DATA_CHUNK> db;
		ship_data_msg t{{static_cast<std::uint8_t>(asset_kind::taunt), sha_of(ship)}, 200000, 0, std::span(ship).first(100)};
		t.write(db.data());
		CHECK(!ship_data_msg::read(std::span(db).first(t.size())));
		t.key.kind = static_cast<std::uint8_t>(asset_kind::ship);
		t.write(db.data());
		CHECK(ship_data_msg::read(std::span(db).first(t.size())));
		t.key.kind = 7;
		t.write(db.data());
		CHECK(!ship_data_msg::read(std::span(db).first(t.size())));
	}
}

void test_assembler()
{
	const auto ship{make_ship(2, 2000)};
	const auto h{hash_of(ship)};
	{
		ship_assembler a{h, 2000};
		CHECK(a.add({h, 2000, 0, std::span(ship).first(896)}) == ship_assembler::result::more);
		CHECK(a.add({h, 2000, 896, std::span(ship).subspan(896, 896)}) == ship_assembler::result::more);
		CHECK(a.add({h, 2000, 1792, std::span(ship).subspan(1792)}) == ship_assembler::result::complete);
		CHECK(a.take() == ship);
	}
	{
		ship_assembler a{h, 2000};
		CHECK(a.add({h, 2000, 896, std::span(ship).subspan(896, 896)}) == ship_assembler::result::error);
	}
	{
		ship_assembler a{h, 2000};
		CHECK(a.add({h, 1999, 0, std::span(ship).first(896)}) == ship_assembler::result::error);
		asset_key other{h};
		other.hash[0] ^= 1;
		ship_assembler b{h};
		CHECK(b.add({other, 2000, 0, std::span(ship).first(896)}) == ship_assembler::result::error);
	}
}

/* A machine of the simulation. */
struct machine;

struct link_msg
{
	std::uint8_t type;
	std::vector<std::uint8_t> payload;
};

struct world
{
	std::vector<machine *> m;
	/* Per (from, to) the messages in flight, in order. */
	std::map<std::pair<unsigned, unsigned>, std::deque<link_msg>> links;
	/* Bytes a link delivers per step. */
	std::size_t link_rate{8192};
	std::size_t delivered{};
	std::map<unsigned, std::size_t> data_bytes_to;
	bool corrupt_data_from_1{};
};

struct machine final : ship_exchange_env
{
	world &w;
	unsigned id;
	std::map<asset_key, std::shared_ptr<const std::vector<std::uint8_t>>> files;
	std::array<std::optional<asset_key>, 8> shown{};
	std::array<bool, 8> clients{};
	ship_exchange ex;
	std::vector<std::string> log;
	machine(world &wd, const unsigned i) :
		w{wd}, id{i}, ex{*this, i == 0, static_cast<std::uint8_t>(i)}
	{
	}
	void add_file(const std::vector<std::uint8_t> &b)
	{
		files[hash_of(b)] = std::make_shared<const std::vector<std::uint8_t>>(b);
	}
	bool has_asset(const asset_key &h) override
	{
		return files.contains(h);
	}
	std::shared_ptr<const std::vector<std::uint8_t>> asset_file(const asset_key &h) override
	{
		const auto f{files.find(h)};
		return f == files.end() ? nullptr : f->second;
	}
	bool store_asset(const asset_key &h, const std::span<const std::uint8_t> b) override
	{
		if (hash_of(std::vector<std::uint8_t>(b.begin(), b.end())) != h)
			return false;
		files[h] = std::make_shared<const std::vector<std::uint8_t>>(b.begin(), b.end());
		return true;
	}
	void player_ship(const std::uint8_t pid, const ship_info_msg *const m) override
	{
		if (const asset_key k{static_cast<std::uint8_t>(asset_kind::ship), m ? m->hash : ship_hash{}}; m && has_asset(k))
			shown[pid] = k;
		else
			shown[pid].reset();
	}
	void send(const std::uint8_t slot, const std::uint8_t type, const std::span<const std::uint8_t> payload) override
	{
		/* A client talks to the host only. */
		const unsigned to{id == 0 ? slot : 0u};
		if (id != 0 && slot != 0)
		{
			++failures;
			std::fprintf(stderr, "client %u sent to slot %u\n", id, slot);
		}
		std::vector<std::uint8_t> p(payload.begin(), payload.end());
		if (type == SHIP_MSG_DATA)
		{
			w.data_bytes_to[to] += p.size();
			if (w.corrupt_data_from_1 && id == 1 && p.size() > SHIP_DATA_HEADER + 10)
				p[SHIP_DATA_HEADER + 5] ^= 0x55;
		}
		w.links[{id, to}].push_back({type, std::move(p)});
	}
	std::size_t queued_bytes(const std::uint8_t slot) override
	{
		const unsigned to{id == 0 ? slot : 0u};
		std::size_t n{};
		for (const auto &m : w.links[{id, to}])
			n += m.payload.size();
		return n;
	}
	bool is_client(const std::uint8_t slot) override
	{
		return slot < clients.size() && clients[slot];
	}
	void note(const std::string_view what, const asset_key &, const std::uint8_t slot) override
	{
		log.push_back(std::string(what) + " " + std::to_string(slot));
	}
};

/* Deliver up to link_rate bytes per link, then let everyone pump. */
void step(world &w, const double seconds, const bool in_level)
{
	for (auto &[key, q] : w.links)
	{
		std::size_t budget{w.link_rate};
		while (!q.empty() && budget)
		{
			auto msg{std::move(q.front())};
			q.pop_front();
			budget -= std::min(budget, msg.payload.size());
			w.delivered += msg.payload.size();
			w.m[key.second]->ex.receive(static_cast<std::uint8_t>(w.m[key.first]->id), msg.type, msg.payload);
		}
	}
	for (const auto m : w.m)
		m->ex.pump(seconds, in_level);
}

bool all_quiet(world &w)
{
	for (const auto &[key, q] : w.links)
		if (!q.empty())
			return false;
	for (const auto m : w.m)
		if (m->ex.busy())
			return false;
	return true;
}

unsigned run(world &w, const unsigned max_steps, const bool in_level = false, const double seconds = 0.05)
{
	unsigned n{};
	for (; n < max_steps; ++n)
	{
		step(w, seconds, in_level);
		if (all_quiet(w))
			break;
	}
	return n;
}

ship_info_msg info_of(const std::vector<std::uint8_t> &b, const char *const name)
{
	ship_info_msg m;
	m.pyro = false;
	m.size = static_cast<std::uint32_t>(b.size());
	m.hash = sha_of(b);
	m.name = name;
	return m;
}

void test_exchange()
{
	const auto ship_a{make_ship(10, 300000)};	/* host's, flown by the host */
	const auto ship_b{make_ship(11, 120000)};	/* only client 1 has it */
	const auto ship_d{make_ship(12, 50000)};	/* host's, flown by its bot */
	world w;
	machine host{w, 0}, c1{w, 1}, c2{w, 2}, c3{w, 3};
	w.m = {&host, &c1, &c2, &c3};
	host.add_file(ship_a);
	host.add_file(ship_d);
	c1.add_file(ship_b);
	c3.ex.accept = false;
	/* The lobby: the host flies A, its bot in slot 5 flies D. */
	host.ex.set_local(0, info_of(ship_a, "aaa"));
	host.ex.set_local(5, info_of(ship_d, "ddd"));
	for (const unsigned c : {1u, 2u, 3u})
	{
		host.clients[c] = true;
		host.ex.client_joined(static_cast<std::uint8_t>(c));
	}
	c1.ex.set_local(1, info_of(ship_b, "bbb"));
	ship_info_msg pyro;
	c2.ex.set_local(2, pyro);
	c3.ex.set_local(3, pyro);
	const auto steps{run(w, 2000)};
	CHECK(steps < 2000);
	/* Everyone who accepts has everything and shows it. */
	for (const auto m : {&host, &c1, &c2})
	{
		CHECK(m->has_asset(hash_of(ship_a)));
		CHECK(m->has_asset(hash_of(ship_b)));
		CHECK(m->has_asset(hash_of(ship_d)));
		CHECK(m->shown[0] == hash_of(ship_a));
		CHECK(m->shown[1] == hash_of(ship_b));
		CHECK(m->shown[5] == hash_of(ship_d));
		CHECK(!m->shown[2] && !m->shown[3]);
	}
	/* The refusing client got nothing and shows Pyros. */
	CHECK(c3.files.empty());
	CHECK(!c3.shown[0] && !c3.shown[1] && !c3.shown[5]);
	CHECK(!w.data_bytes_to[3]);
	/* B went from client 1 to the host once, and from the host to
	 * client 2 once (not to client 1, which has it).
	 */
	CHECK(w.data_bytes_to[0] >= ship_b.size() && w.data_bytes_to[0] < ship_b.size() * 11 / 10);
	CHECK(w.data_bytes_to[2] >= ship_a.size() + ship_b.size() + ship_d.size() && w.data_bytes_to[2] < (ship_a.size() + ship_b.size() + ship_d.size()) * 11 / 10);
	CHECK(w.data_bytes_to[1] >= ship_a.size() + ship_d.size() && w.data_bytes_to[1] < (ship_a.size() + ship_d.size()) * 11 / 10);
	/* A late joiner in slot 4 learns everything and gets what it lacks. */
	machine c4{w, 4};
	w.m.push_back(&c4);
	c4.add_file(ship_a);
	host.clients[4] = true;
	host.ex.client_joined(4);
	c4.ex.set_local(4, pyro);
	CHECK(run(w, 2000) < 2000);
	CHECK(c4.shown[0] == hash_of(ship_a) && c4.shown[1] == hash_of(ship_b) && c4.shown[5] == hash_of(ship_d));
	CHECK(w.data_bytes_to[4] >= ship_b.size() + ship_d.size() && w.data_bytes_to[4] < (ship_b.size() + ship_d.size()) * 11 / 10);
	/* A player who leaves is a Pyro for everyone. */
	host.clients[1] = false;
	host.ex.slot_cleared(1);
	CHECK(!host.shown[1]);
	CHECK(run(w, 100) < 100);
}

void test_failures()
{
	const auto ship_b{make_ship(21, 40000)};
	/* Corrupted on the way from its owner: the host refuses it and tells
	 * the client who waits.
	 */
	{
		world w;
		machine host{w, 0}, c1{w, 1}, c2{w, 2};
		w.m = {&host, &c1, &c2};
		host.ex.accept = false;	/* fetches only for client 2 */
		c1.add_file(ship_b);
		w.corrupt_data_from_1 = true;
		host.clients[1] = host.clients[2] = true;
		host.ex.client_joined(1);
		host.ex.client_joined(2);
		c1.ex.set_local(1, info_of(ship_b, "bbb"));
		c2.ex.set_local(2, ship_info_msg{});
		CHECK(run(w, 2000) < 2000);
		CHECK(!host.has_asset(hash_of(ship_b)));
		CHECK(!c2.has_asset(hash_of(ship_b)));
		CHECK(!c2.shown[1]);
		CHECK(std::ranges::find(c2.log, "not available 0") != c2.log.end());
	}
	/* The owner leaves while the host fetches: the waiter hears so. */
	{
		world w;
		machine host{w, 0}, c1{w, 1}, c2{w, 2};
		w.m = {&host, &c1, &c2};
		host.ex.accept = false;
		c1.add_file(ship_b);
		host.clients[1] = host.clients[2] = true;
		host.ex.client_joined(1);
		host.ex.client_joined(2);
		c1.ex.set_local(1, info_of(ship_b, "bbb"));
		c2.ex.set_local(2, ship_info_msg{});
		w.link_rate = 2000;
		for (unsigned i = 0; i < 6; ++i)
			step(w, 0.05, false);
		host.clients[1] = false;
		host.ex.slot_cleared(1);
		w.links[{1, 0}].clear();
		w.link_rate = 8192;
		CHECK(run(w, 2000) < 2000);
		CHECK(!c2.has_asset(hash_of(ship_b)));
		CHECK(!host.ex.busy());
	}
	/* Data from someone the host did not ask is ignored; so is a client
	 * speaking for another player.
	 */
	{
		world w;
		machine host{w, 0}, c1{w, 1}, c2{w, 2};
		w.m = {&host, &c1, &c2};
		host.clients[1] = host.clients[2] = true;
		const ship_data_msg d{hash_of(ship_b), static_cast<std::uint32_t>(ship_b.size()), 0, std::span(ship_b).first(500)};
		std::vector<std::uint8_t> buf(d.size());
		d.write(buf.data());
		host.ex.receive(2, SHIP_MSG_DATA, buf);
		CHECK(!host.ex.busy() && host.files.empty());
		auto other{info_of(ship_b, "bbb")};
		other.pid = 1;
		std::array<std::uint8_t, ship_info_msg::SIZE> ib;
		other.write(ib.data());
		host.ex.receive(2, SHIP_MSG_INFO, ib);
		CHECK(!host.ex.info(1));
		/* A client takes ship messages from the host only. */
		c1.ex.receive(2, SHIP_MSG_INFO, ib);
		CHECK(!c1.ex.info(1));
	}
	/* A client that changes its ship again and again: the host takes a
	 * change only every SHIP_CHANGE_INTERVAL, drops what it fetched of
	 * the old one, and holds at most one fetch for it.
	 */
	{
		world w;
		machine host{w, 0}, c1{w, 1};
		w.m = {&host, &c1};
		host.clients[1] = true;
		host.ex.client_joined(1);
		w.link_rate = 0;
		for (unsigned i = 0; i < 50; ++i)
		{
			const auto ship{make_ship(100 + i, 4000)};
			std::array<std::uint8_t, ship_info_msg::SIZE> ib;
			auto m{info_of(ship, "spam")};
			m.pid = 1;
			m.write(ib.data());
			host.ex.receive(1, SHIP_MSG_INFO, ib);
			host.ex.pump(i % 10 == 9 ? 3.5 : 0.1, false);
		}
		/* 50 announcements over about 20 s: 1 + 5 taken. */
		std::size_t requests{};
		for (const auto &msg : w.links[std::make_pair(0u, 1u)])
			if (msg.type == SHIP_MSG_REQUEST)
				++requests;
		CHECK(requests >= 2 && requests <= 7);
		/* Announcing the same again changes nothing. */
		const auto before{w.links[std::make_pair(0u, 1u)].size()};
		std::array<std::uint8_t, ship_info_msg::SIZE> ib;
		auto again{*host.ex.info(1)};
		again.write(ib.data());
		host.ex.receive(1, SHIP_MSG_INFO, ib);
		CHECK(w.links[std::make_pair(0u, 1u)].size() == before);
	}
	/* Nobody has it. */
	{
		world w;
		machine host{w, 0}, c1{w, 1};
		w.m = {&host, &c1};
		host.clients[1] = true;
		host.ex.client_joined(1);
		std::array<std::uint8_t, ship_request_msg::SIZE> rb;
		ship_request_msg{hash_of(ship_b)}.write(rb.data());
		host.ex.receive(1, SHIP_MSG_REQUEST, rb);
		{
			const auto &q{w.links[std::make_pair(0u, 1u)]};
			CHECK(q.size() == 1 && q.front().type == SHIP_MSG_UNAVAILABLE);
		}
	}
}

/* The pacing: at most the level's rate during a level, and never more
 * than the queue limit waiting in the reliable queue.
 */
void test_pacing()
{
	const auto ship{make_ship(31, 200000)};
	world w;
	machine host{w, 0}, c1{w, 1};
	w.m = {&host, &c1};
	host.add_file(ship);
	host.clients[1] = true;
	host.ex.client_joined(1);
	host.ex.set_local(0, info_of(ship, "aaa"));
	c1.ex.set_local(1, ship_info_msg{});
	/* Deliver nothing for a while: the queue limit holds. */
	w.link_rate = 0;
	for (unsigned i = 0; i < 40; ++i)
	{
		/* Control messages through by hand. */
		for (auto &[key, q] : w.links)
			while (!q.empty() && q.front().type != SHIP_MSG_DATA)
			{
				auto msg{std::move(q.front())};
				q.pop_front();
				w.m[key.second]->ex.receive(static_cast<std::uint8_t>(key.first), msg.type, msg.payload);
			}
		for (const auto m : w.m)
			m->ex.pump(0.05, true);
		CHECK(host.queued_bytes(1) <= SHIP_QUEUE_LIMIT + SHIP_DATA_HEADER + SHIP_DATA_CHUNK);
	}
	/* A fast link: the rate holds. */
	w.link_rate = 1 << 20;
	const std::size_t before{w.data_bytes_to[1]};
	const unsigned steps{20};
	for (unsigned i = 0; i < steps; ++i)
		step(w, 0.05, true);
	const double seconds{steps * 0.05};
	const double sent{static_cast<double>(w.data_bytes_to[1] - before)};
	CHECK(sent <= SHIP_RATE_LEVEL * seconds * 1.3 + SHIP_RATE_LEVEL / 4.0);
	CHECK(sent >= SHIP_RATE_LEVEL * seconds * 0.5);
	/* In the lobby it is faster, and it completes. */
	CHECK(run(w, 4000, false) < 4000);
	CHECK(c1.has_asset(hash_of(ship)) && c1.shown[0] == hash_of(ship));
}

/* The same exchange over the real reliable transport (net_v2_transport.h)
 * with 10 % loss and 40 ms one-way delay: a host whose client lacks a
 * 600 KB ship that another client flies.
 */
struct net_machine;

struct net_world
{
	std::vector<net_machine *> m;
	struct packet
	{
		net_clock at;
		unsigned from, to;
		std::vector<std::uint8_t> bytes;
	};
	std::deque<packet> wire;
	std::mt19937 rng{7};
	net_clock now{};
	unsigned dropped{}, sent{};
};

struct net_machine final : ship_exchange_env
{
	net_world &w;
	unsigned id;
	std::map<unsigned, connection> conns;
	std::map<asset_key, std::shared_ptr<const std::vector<std::uint8_t>>> files;
	std::array<std::optional<asset_key>, 8> shown{};
	ship_exchange ex;
	net_machine(net_world &wd, const unsigned i) :
		w{wd}, id{i}, ex{*this, i == 0, static_cast<std::uint8_t>(i)}
	{
	}
	void connect(const unsigned peer)
	{
		conns.emplace(std::piecewise_construct, std::forward_as_tuple(peer), std::forward_as_tuple(connection_config{.session_id = 0x1234, .peer_token = 0x5000 + (id ? id : peer), .local_player_id = static_cast<std::uint8_t>(id), .remote_player_id = static_cast<std::uint8_t>(peer)}, w.now));
	}
	bool has_asset(const asset_key &k) override
	{
		return files.contains(k);
	}
	std::shared_ptr<const std::vector<std::uint8_t>> asset_file(const asset_key &k) override
	{
		const auto f{files.find(k)};
		return f == files.end() ? nullptr : f->second;
	}
	bool store_asset(const asset_key &k, const std::span<const std::uint8_t> b) override
	{
		if (sha_of(std::vector<std::uint8_t>(b.begin(), b.end())) != k.hash)
			return false;
		files[k] = std::make_shared<const std::vector<std::uint8_t>>(b.begin(), b.end());
		return true;
	}
	void player_ship(const std::uint8_t pid, const ship_info_msg *const m) override
	{
		if (const asset_key k{static_cast<std::uint8_t>(asset_kind::ship), m ? m->hash : ship_hash{}}; m && has_asset(k))
			shown[pid] = k;
		else
			shown[pid].reset();
	}
	void send(const std::uint8_t slot, const std::uint8_t type, const std::span<const std::uint8_t> payload) override
	{
		const auto c{conns.find(id ? 0u : slot)};
		if (c == conns.end() || c->second.enqueue_reliable(type, payload) != enqueue_result::ok)
		{
			++failures;
			std::fprintf(stderr, "net_machine %u: cannot queue type %u (%zu bytes)\n", id, type, payload.size());
		}
	}
	std::size_t queued_bytes(const std::uint8_t slot) override
	{
		const auto c{conns.find(id ? 0u : slot)};
		return c == conns.end() ? 0 : c->second.stats().queue_bytes;
	}
	bool is_client(const std::uint8_t slot) override
	{
		return id == 0 && conns.contains(slot);
	}
	void note(std::string_view, const asset_key &, std::uint8_t) override
	{
	}
	void pump()
	{
		ex.pump(net_milliseconds(10) / 65536.0, false);
		for (auto &[peer, c] : conns)
		{
			c.begin_tick(w.now);
			for (;;)
			{
				const auto p{c.build_outgoing(w.now)};
				if (p.empty())
					break;
				++w.sent;
				if (std::uniform_int_distribution<unsigned>(0, 9)(w.rng) == 0)
				{
					++w.dropped;
					continue;
				}
				w.wire.push_back({w.now + net_milliseconds(40), id, peer, std::vector<std::uint8_t>(p.begin(), p.end())});
			}
		}
	}
	void receive(const unsigned from, const std::vector<std::uint8_t> &d)
	{
		const auto c{conns.find(from)};
		if (c == conns.end())
			return;
		const auto r{c->second.on_receive(d, w.now)};
		for (const auto &m : r.reliable)
			ex.receive(static_cast<std::uint8_t>(from), m.type, m.payload);
	}
};

void test_transport()
{
	const auto ship_b{make_ship(41, 600000)};
	net_world w;
	net_machine host{w, 0}, c1{w, 1}, c2{w, 2};
	w.m = {&host, &c1, &c2};
	for (const unsigned c : {1u, 2u})
	{
		host.connect(c);
		w.m[c]->connect(0);
	}
	c1.files[hash_of(ship_b)] = std::make_shared<const std::vector<std::uint8_t>>(ship_b);
	host.ex.set_local(0, ship_info_msg{});
	host.ex.client_joined(1);
	host.ex.client_joined(2);
	c1.ex.set_local(1, info_of(ship_b, "bbb"));
	c2.ex.set_local(2, ship_info_msg{});
	unsigned steps{};
	for (; steps < 6000; ++steps)
	{
		w.now += net_milliseconds(10);
		for (const auto m : w.m)
			m->pump();
		while (!w.wire.empty() && w.wire.front().at <= w.now)
		{
			const auto pk{std::move(w.wire.front())};
			w.wire.pop_front();
			w.m[pk.to]->receive(pk.from, pk.bytes);
		}
		if (c2.shown[1] && host.shown[1] && !host.ex.busy() && !c2.ex.busy())
			break;
	}
	CHECK(c2.has_asset(hash_of(ship_b)) && c2.shown[1] == hash_of(ship_b));
	CHECK(host.has_asset(hash_of(ship_b)) && host.shown[1] == hash_of(ship_b));
	CHECK(w.dropped > 0);
	/* Two hops at the lobby's 96 KiB/s: about 13 s; loss adds a little. */
	CHECK(steps < 3000);
	std::printf("test-net-v2-ships: 600 KB relayed over the transport in %.1f s, %u of %u packets dropped\n", steps * 0.01, w.dropped, w.sent);
}

}

int main()
{
	test_messages();
	test_assembler();
	test_exchange();
	test_failures();
	test_pacing();
	test_transport();
	if (failures)
	{
		std::fprintf(stderr, "test-net-v2-ships: %u failures\n", failures);
		return 1;
	}
	std::puts("test-net-v2-ships: all checks passed");
	return 0;
}
