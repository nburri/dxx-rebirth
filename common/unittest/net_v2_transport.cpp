/*
 * This file is part of the DXX-Rebirth project <https://github.com/dxx-rebirth/dxx-rebirth/>.
 * It is copyright by its individual contributors, as recorded in the
 * project's Git history.  See COPYING.txt at the top level for license
 * terms and a link to the Git history.
 */
/*
 * Simulation test of the v2 network transport (net_v2_transport.h).
 *
 * Two `connection`s talk over a virtual link with configurable latency,
 * jitter, loss, duplication and reordering, driven by a virtual clock and
 * a seeded generator, so every run is reproducible.  There is no test
 * framework: a failed check prints its location and exits with status 1.
 *
 * Build and run with SCons (see Documentation/netv2-transport.md):
 *
 *	scons sdl2=1 d1x=0 d2x=1 register_runtime_test_plain_link_targets=1 test-net-v2-transport
 *	build/common/test-net-v2-transport
 *
 * or with nothing but a compiler, from the top of the source tree:
 *
 *	g++ -std=gnu++23 -O2 -g -Wall -Wextra -Icommon/main \
 *		common/unittest/net_v2_transport.cpp common/main/net_v2_transport.cpp \
 *		-o test-net-v2-transport && ./test-net-v2-transport
 *
 * Add -fsanitize=address,undefined to the g++ line to run the fuzz case
 * under the sanitizers.  An optional first argument is the seed.
 */

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <span>
#include <string>
#include <vector>

#include "net_v2_transport.h"

using namespace dcx::net_v2;

namespace {

/* Checks */

void check_failed(const char *const expr, const char *const file, const int line, const std::string &detail)
{
	std::fprintf(stderr, "%s:%d: check failed: %s%s%s\n", file, line, expr, detail.empty() ? "" : " -- ", detail.c_str());
	std::exit(1);
}

#define CHECK(cond)	do { if (!(cond)) check_failed(#cond, __FILE__, __LINE__, {}); } while (0)
#define CHECK_MSG(cond, detail)	do { if (!(cond)) check_failed(#cond, __FILE__, __LINE__, (detail)); } while (0)

/* Deterministic generator (splitmix64) */

class rng
{
	std::uint64_t m_state;
public:
	explicit rng(const std::uint64_t seed) :
		m_state{seed}
	{
	}
	std::uint64_t next()
	{
		std::uint64_t z{m_state += 0x9e3779b97f4a7c15ull};
		z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
		z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
		return z ^ (z >> 31);
	}
	/* [0, 1) */
	double uniform()
	{
		return static_cast<double>(next() >> 11) / 9007199254740992.0;
	}
	/* [0, n) */
	std::uint64_t below(const std::uint64_t n)
	{
		return n == 0 ? 0 : next() % n;
	}
	/* [lo, hi] */
	std::int64_t range(const std::int64_t lo, const std::int64_t hi)
	{
		return lo + static_cast<std::int64_t>(below(static_cast<std::uint64_t>(hi - lo + 1)));
	}
	bool chance(const double p)
	{
		return uniform() < p;
	}
};

/* Time helpers */

constexpr net_clock TICK{net_seconds(1) / 60};

double to_ms(const net_clock t)
{
	return static_cast<double>(t) * 1000.0 / 65536.0;
}

/* Virtual link */

struct link_params
{
	net_clock latency{net_milliseconds(80)};
	/* Uniform in [-jitter, +jitter] per packet. */
	net_clock jitter{};
	double loss{};
	double duplication{};
	/* Probability that a packet is held back by an extra
	 * [1, reorder_delay] so that later packets overtake it.
	 */
	double reorder{};
	net_clock reorder_delay{3 * TICK};
};

using datagram = std::vector<std::uint8_t>;
using mangle_function = std::function<void(datagram &)>;

class sim_link
{
	struct queued
	{
		net_clock arrival;
		std::uint64_t order;
		unsigned dest;
		datagram bytes;
	};
	std::vector<queued> m_queue;
	rng &m_rng;
	std::uint64_t m_order{};
public:
	link_params params;
	/* Per direction (indexed by the destination), for the tests that cut
	 * one direction or rewrite what arrives.
	 */
	std::array<bool, 2> blocked{};
	std::array<mangle_function, 2> mangle{};
	std::uint64_t sent{}, dropped{}, duplicated{}, delivered{};

	sim_link(rng &r, const link_params &p) :
		m_rng{r},
		params{p}
	{
	}

	void send(const unsigned from, const std::span<const std::uint8_t> bytes, const net_clock now)
	{
		const unsigned dest{from ^ 1};
		++sent;
		if (blocked[dest] || m_rng.chance(params.loss))
		{
			++dropped;
			return;
		}
		unsigned copies{1};
		if (m_rng.chance(params.duplication))
		{
			++copies;
			++duplicated;
		}
		for (unsigned i{}; i != copies; ++i)
		{
			net_clock delay{params.latency};
			if (params.jitter)
				delay += m_rng.range(-params.jitter, params.jitter);
			if (m_rng.chance(params.reorder))
				delay += m_rng.range(1, params.reorder_delay);
			if (delay < 1)
				delay = 1;
			m_queue.push_back({.arrival = now + delay, .order = m_order++, .dest = dest, .bytes = {bytes.begin(), bytes.end()}});
		}
	}

	/* Hand every packet for `dest` that has arrived by `now` to `f`, in
	 * arrival order, with its arrival time: the game reads the socket
	 * every frame, so a packet is seen well before the next tick.
	 */
	void deliver(const unsigned dest, const net_clock now, const std::function<void(const datagram &, net_clock)> &f)
	{
		std::vector<queued> due;
		std::erase_if(m_queue, [&](queued &q) {
			if (q.dest != dest || q.arrival > now)
				return false;
			due.push_back(std::move(q));
			return true;
		});
		std::ranges::sort(due, {}, [](const queued &q) { return std::pair{q.arrival, q.order}; });
		for (auto &q : due)
		{
			if (mangle[dest])
				mangle[dest](q.bytes);
			++delivered;
			f(q.bytes, q.arrival);
		}
	}

	void clear()
	{
		m_queue.clear();
	}

	[[nodiscard]]
	std::size_t pending() const
	{
		return m_queue.size();
	}
};

/* Peers and the world */

struct sent_message
{
	std::uint8_t type;
	datagram payload;
};

struct sim_peer
{
	connection conn;
	/* This peer's local clock is the simulation clock plus this bias. */
	net_clock bias;
	std::vector<reliable_message> delivered;
	std::vector<sent_message> sent;
	std::vector<std::uint32_t> states_seen;
	std::vector<std::uint32_t> events_seen;
	std::uint64_t malformed{}, duplicates{}, rejected_other{};

	sim_peer(const connection_config &c, const net_clock bias_, const net_clock now) :
		conn{c, now + bias_},
		bias{bias_}
	{
	}

	[[nodiscard]]
	net_clock clock(const net_clock sim_now) const
	{
		return sim_now + bias;
	}

	void receive(const datagram &d, const net_clock sim_now)
	{
		auto report{conn.on_receive(d, clock(sim_now))};
		switch (report.status)
		{
			case receive_status::accepted:
				break;
			case receive_status::malformed_chunk:
				++malformed;
				break;
			case receive_status::duplicate:
				++duplicates;
				break;
			default:
				++rejected_other;
				break;
		}
		for (auto &m : report.reliable)
			delivered.push_back(std::move(m));
		for (const auto &u : report.unreliable)
		{
			if (u.payload.size() < 4)
				continue;
			const auto value{net_get_le32(u.payload.data())};
			if (u.type == chunk_type::event_u)
				events_seen.push_back(value);
			else
				states_seen.push_back(value);
		}
	}
};

constexpr connection_config host_side{.session_id = 0x12345678, .peer_token = 0xcafef00d, .local_player_id = 0, .remote_player_id = 3};
constexpr connection_config client_side{.session_id = 0x12345678, .peer_token = 0xcafef00d, .local_player_id = 3, .remote_player_id = 0};

class sim_world
{
public:
	rng &random;
	sim_link link;
	std::array<sim_peer, 2> peers;
	net_clock now{};
	std::uint32_t tick_count{};

	sim_world(rng &r, const link_params &p, const std::array<net_clock, 2> biases = {}) :
		random{r},
		link{r, p},
		peers{{
			sim_peer{host_side, biases[0], 0},
			sim_peer{client_side, biases[1], 0},
		}}
	{
	}

	/* One 60 Hz tick: deliver what arrived, let the test act, build and
	 * send.  `act(peer_index)` runs before that peer builds its packets.
	 */
	void tick(const std::function<void(unsigned)> &act = {})
	{
		now += TICK;
		++tick_count;
		for (unsigned i{}; i != 2; ++i)
		{
			auto &p{peers[i]};
			link.deliver(i, now, [&](const datagram &d, const net_clock at) { p.receive(d, at); });
			if (act)
				act(i);
			for (;;)
			{
				const auto packet{p.conn.build_outgoing(p.clock(now))};
				if (packet.empty())
					break;
				CHECK(packet.size() >= NET_V2_HEADER_SIZE && packet.size() <= NET_V2_MAX_PACKET);
				link.send(i, packet, now);
			}
		}
	}

	void run(const unsigned ticks, const std::function<void(unsigned)> &act = {})
	{
		for (unsigned t{}; t != ticks; ++t)
			tick(act);
	}

	/* Queue a reliable message of random size with a self-describing
	 * payload on peer `from`.
	 */
	void enqueue_random_message(const unsigned from, const std::size_t max_size = 250)
	{
		auto &p{peers[from]};
		const auto index{static_cast<std::uint32_t>(p.sent.size())};
		/* Mostly small, occasionally the maximum. */
		const std::size_t size{random.chance(0.02) ? NET_V2_MAX_MESSAGE : random.below(max_size + 1)};
		datagram payload(std::max<std::size_t>(size, 4));
		net_put_le32(payload.data(), index);
		for (std::size_t i{4}; i != payload.size(); ++i)
			payload[i] = static_cast<std::uint8_t>(index * 7 + i);
		const auto type{static_cast<std::uint8_t>(random.range(1, 255))};
		const auto result{p.conn.enqueue_reliable(type, payload)};
		CHECK_MSG(result == enqueue_result::ok, "enqueue failed at message " + std::to_string(index));
		p.sent.push_back({.type = type, .payload = std::move(payload)});
	}

	void set_state(const unsigned from)
	{
		std::array<std::uint8_t, 40> state{};
		net_put_le32(state.data(), tick_count);
		net_put_le32(state.data() + 4, 0xabad1dea);
		peers[from].conn.set_unreliable_state(from == 0 ? chunk_type::state : chunk_type::input, state);
	}

	void send_event(const unsigned from, const std::uint32_t value)
	{
		std::array<std::uint8_t, 6> event{};
		net_put_le32(event.data(), value);
		peers[from].conn.send_unreliable(chunk_type::event_u, event);
	}
};

/* Every message sent by `from` was delivered to the other side exactly
 * once, in order, unchanged.
 */
void check_delivery(const sim_world &w, const unsigned from)
{
	const auto &sent{w.peers[from].sent};
	const auto &got{w.peers[from ^ 1].delivered};
	CHECK_MSG(got.size() == sent.size(), "sent " + std::to_string(sent.size()) + ", delivered " + std::to_string(got.size()));
	for (std::size_t i{}; i != sent.size(); ++i)
	{
		CHECK_MSG(got[i].type == sent[i].type, "type mismatch at " + std::to_string(i));
		CHECK_MSG(got[i].payload == sent[i].payload, "payload mismatch at " + std::to_string(i));
	}
}

void print_stats(const char *const label, const connection_stats &s)
{
	std::printf("    %s: sent %llu recv %llu rejected %llu acked %llu lost %llu | msgs %llu delivered %llu sends %llu resends %llu (gap %llu, rto %llu) | srtt %.1f ms rttvar %.1f ms rto %.1f ms loss %.3f | queue %zu (%zu B) in-flight %zu window-pending %zu | offset %.2f ms (target %.2f)\n",
		label,
		static_cast<unsigned long long>(s.packets_sent),
		static_cast<unsigned long long>(s.packets_received),
		static_cast<unsigned long long>(s.packets_rejected),
		static_cast<unsigned long long>(s.packets_acked),
		static_cast<unsigned long long>(s.packets_lost),
		static_cast<unsigned long long>(s.messages_enqueued),
		static_cast<unsigned long long>(s.messages_delivered),
		static_cast<unsigned long long>(s.message_sends),
		static_cast<unsigned long long>(s.message_resends),
		static_cast<unsigned long long>(s.resends_by_gap),
		static_cast<unsigned long long>(s.resends_by_rto),
		to_ms(s.srtt), to_ms(s.rttvar), to_ms(s.rto), s.loss_estimate,
		s.queue_messages, s.queue_bytes, s.in_flight, s.recv_window_pending,
		to_ms(s.clock_offset), to_ms(s.clock_offset_target));
}

void begin(const char *const name)
{
	std::printf("[%s]\n", name);
	std::fflush(stdout);
}

/* (a) Lossless ordered delivery */

void test_lossless(const std::uint64_t seed)
{
	begin("a: lossless ordered delivery");
	rng r{seed};
	sim_world w{r, link_params{.latency = net_milliseconds(80)}};
	constexpr unsigned messages_per_side{2000};
	w.run(600, [&](const unsigned i) {
		for (unsigned k{}; k != 5 && w.peers[i].sent.size() < messages_per_side; ++k)
			w.enqueue_random_message(i);
		w.set_state(i);
	});
	for (unsigned i{}; i != 2; ++i)
	{
		check_delivery(w, i);
		const auto s{w.peers[i].conn.stats()};
		print_stats(i ? "client" : "host", s);
		CHECK(s.state == connection_state::connected);
		CHECK_MSG(s.message_resends == 0, "no retransmission on a lossless link");
		CHECK(s.in_flight == 0);
		CHECK(s.queue_messages == 0);
		CHECK(s.packets_lost == 0);
		CHECK(s.loss_estimate < 0.001);
	}
}

/* (b) Loss, reorder, duplication */

void test_lossy(const std::uint64_t seed)
{
	begin("b: 20% loss, 5% duplication, 20% reorder, 20 ms jitter");
	rng r{seed};
	sim_world w{r, link_params{.latency = net_milliseconds(80), .jitter = net_milliseconds(20), .loss = 0.20, .duplication = 0.05, .reorder = 0.20}};
	constexpr unsigned messages_per_side{3000};
	std::uint32_t events_sent{};
	w.run(1200, [&](const unsigned i) {
		for (unsigned k{}; k != 4 && w.peers[i].sent.size() < messages_per_side; ++k)
			w.enqueue_random_message(i);
		w.set_state(i);
		if (i == 0)
			w.send_event(0, events_sent++);
	});
	/* The loss estimate is meaningful while the loss lasts; it lags by
	 * about a second and decays on a clean link.
	 */
	for (unsigned i{}; i != 2; ++i)
	{
		const auto loss{w.peers[i].conn.stats().loss_estimate};
		CHECK_MSG(loss > 0.08 && loss < 0.35, "loss estimate " + std::to_string(loss));
	}
	/* Drain on a clean link so that the last state is not simply lost. */
	w.link.params.loss = 0;
	w.link.params.duplication = 0;
	w.link.params.reorder = 0;
	w.run(60, [&](const unsigned i) { w.set_state(i); });
	const auto final_tick{w.tick_count};
	/* Let the last state chunks arrive (keepalives only from here). */
	w.run(30);
	for (unsigned i{}; i != 2; ++i)
	{
		check_delivery(w, i);
		const auto s{w.peers[i].conn.stats()};
		print_stats(i ? "client" : "host", s);
		CHECK(s.state == connection_state::connected);
		CHECK(s.in_flight == 0);
		CHECK(s.queue_messages == 0);
		CHECK(s.recv_window_pending == 0);
		CHECK_MSG(s.message_resends > 0, "loss must cause retransmissions");
		/* 20% loss needs about 0.25 resends per message; reordering adds
		 * some spurious ones.  Far more would mean the sender misjudges
		 * loss.
		 */
		CHECK_MSG(s.message_resends < messages_per_side * 8 / 10, "resends " + std::to_string(s.message_resends));
		CHECK(s.loss_estimate < 0.35);
		CHECK_MSG(w.peers[i].duplicates > 0, "duplicated packets must be rejected as such");
		CHECK(w.peers[i].malformed == 0);
		CHECK(w.peers[i].rejected_other == 0);
		/* Latest-wins state: strictly increasing, and it converges. */
		const auto &states{w.peers[i].states_seen};
		CHECK(!states.empty());
		CHECK(std::ranges::adjacent_find(states, std::greater_equal<>{}) == states.end());
		CHECK_MSG(states.back() == final_tick, "last state " + std::to_string(states.back()) + " vs tick " + std::to_string(final_tick));
		const auto latest{w.peers[i].conn.latest_received(i == 0 ? chunk_type::input : chunk_type::state)};
		CHECK(latest.has_value() && net_get_le32(latest->data()) == final_tick);
	}
	/* Events: each at most once, most of them arrive. */
	auto events{w.peers[1].events_seen};
	std::ranges::sort(events);
	CHECK(std::ranges::adjacent_find(events) == events.end());
	CHECK(events.size() <= events_sent);
	CHECK_MSG(events.size() > events_sent * 7 / 10, "events delivered " + std::to_string(events.size()) + " of " + std::to_string(events_sent));
	std::printf("    events: %zu of %u delivered, link: %llu sent, %llu dropped, %llu duplicated\n", events.size(), events_sent,
		static_cast<unsigned long long>(w.link.sent), static_cast<unsigned long long>(w.link.dropped), static_cast<unsigned long long>(w.link.duplicated));
}

/* (c) RTT and RTO estimates */

void test_rtt(const std::uint64_t seed)
{
	begin("c: RTT/RTO estimation");
	constexpr net_clock latency{net_milliseconds(80)};
	for (const double loss : {0.0, 0.30})
	{
		rng r{seed};
		sim_world w{r, link_params{.latency = latency, .jitter = net_milliseconds(10), .loss = loss}};
		bool rto_in_range{true};
		w.run(180, [&](const unsigned i) {
			w.set_state(i);
			if (i == 0 && w.random.chance(0.5))
				w.enqueue_random_message(0, 100);
			const auto s{w.peers[i].conn.stats()};
			if (s.rto < NET_V2_RTO_MIN || s.rto > NET_V2_RTO_MAX)
				rto_in_range = false;
		});
		CHECK(rto_in_range);
		std::printf("  loss %.0f%%:\n", loss * 100);
		for (unsigned i{}; i != 2; ++i)
		{
			const auto s{w.peers[i].conn.stats()};
			print_stats(i ? "client" : "host", s);
			CHECK(s.rtt_valid);
			const auto expected{2 * latency};
			const auto err{s.srtt > expected ? s.srtt - expected : expected - s.srtt};
			CHECK_MSG(err * 10 < expected, "srtt " + std::to_string(to_ms(s.srtt)) + " ms vs " + std::to_string(to_ms(expected)) + " ms");
			CHECK(s.rto >= s.srtt);
			CHECK(s.rto >= NET_V2_RTO_MIN && s.rto <= NET_V2_RTO_MAX);
			CHECK(s.rttvar < net_milliseconds(30));
		}
	}
	/* The clamp: a 2 ms link must give RTO_MIN, a 700 ms link RTO_MAX. */
	for (const auto &[one_way, expected_rto] : {std::pair{net_milliseconds(1), NET_V2_RTO_MIN}, std::pair{net_milliseconds(700), NET_V2_RTO_MAX}})
	{
		rng r{seed};
		sim_world w{r, link_params{.latency = one_way}};
		w.run(300, [&](const unsigned i) { w.set_state(i); });
		const auto s{w.peers[0].conn.stats()};
		CHECK_MSG(s.rto == expected_rto, "rto " + std::to_string(to_ms(s.rto)) + " ms for one-way " + std::to_string(to_ms(one_way)) + " ms");
	}
	std::printf("    RTO clamps to [%.0f, %.0f] ms\n", to_ms(NET_V2_RTO_MIN), to_ms(NET_V2_RTO_MAX));
}

/* (d) Window and queue bounds */

void test_bounds(const std::uint64_t seed)
{
	begin("d: window and queue bounds");
	{
		connection c{host_side, 0};
		const std::vector<std::uint8_t> big(NET_V2_MAX_MESSAGE + 1);
		CHECK(c.enqueue_reliable(1, big) == enqueue_result::too_large);
		CHECK(c.enqueue_reliable(1, std::span{big}.first(NET_V2_MAX_MESSAGE)) == enqueue_result::ok);
		CHECK(c.stats().queue_messages == 1);
		/* A pending state chunk above the chunk limit is dropped, not sent. */
		c.set_unreliable_state(chunk_type::state, std::vector<std::uint8_t>(NET_V2_MAX_CHUNK_PAYLOAD + 1));
		CHECK(c.stats().unreliable_dropped == 1);
		std::printf("    oversize message rejected at enqueue\n");
	}
	{
		/* Message count bound */
		connection c{host_side, 0};
		const std::array<std::uint8_t, 8> small{};
		for (unsigned i{}; i != NET_V2_QUEUE_MAX_MESSAGES; ++i)
			CHECK(c.enqueue_reliable(1, small) == enqueue_result::ok);
		CHECK(c.state() == connection_state::connected || c.state() == connection_state::connecting);
		CHECK(c.enqueue_reliable(1, small) == enqueue_result::queue_overflow);
		CHECK(c.state() == connection_state::closed);
		CHECK(c.closed_because() == close_reason::queue_overflow);
		CHECK(c.enqueue_reliable(1, small) == enqueue_result::closed);
		CHECK(c.build_outgoing(TICK).empty());
		std::printf("    queue overflow at %u messages closes the connection\n", NET_V2_QUEUE_MAX_MESSAGES + 1);
	}
	{
		/* Byte bound */
		connection c{host_side, 0};
		const std::vector<std::uint8_t> big(NET_V2_MAX_MESSAGE);
		const auto fit{NET_V2_QUEUE_MAX_BYTES / NET_V2_MAX_MESSAGE};
		for (std::size_t i{}; i != fit; ++i)
			CHECK(c.enqueue_reliable(1, big) == enqueue_result::ok);
		CHECK(c.enqueue_reliable(1, big) == enqueue_result::queue_overflow);
		CHECK(c.closed_because() == close_reason::queue_overflow);
		std::printf("    queue overflow at %zu bytes closes the connection\n", NET_V2_QUEUE_MAX_BYTES + NET_V2_MAX_MESSAGE);
	}
	{
		/* Receive window: with the acks cut, the sender stalls at 256 in
		 * flight and the receiver delivers exactly those.
		 */
		rng r{seed};
		sim_world w{r, link_params{.latency = net_milliseconds(30)}};
		w.run(6, [&](const unsigned i) { w.set_state(i); });
		w.link.blocked[0] = true;	/* client -> host */
		for (unsigned k{}; k != 400; ++k)
			w.enqueue_random_message(0, 16);
		std::size_t max_in_flight{};
		w.run(120, [&](const unsigned i) {
			w.set_state(i);
			max_in_flight = std::max(max_in_flight, w.peers[0].conn.stats().in_flight);
		});
		auto s{w.peers[0].conn.stats()};
		print_stats("host (acks cut)", s);
		CHECK_MSG(max_in_flight == NET_V2_MAX_IN_FLIGHT, "max in flight " + std::to_string(max_in_flight));
		CHECK(s.in_flight == NET_V2_MAX_IN_FLIGHT);
		CHECK(s.queue_messages == 400);
		CHECK(w.peers[1].delivered.size() == NET_V2_MAX_IN_FLIGHT);
		CHECK(w.peers[1].conn.stats().recv_window_pending == 0);
		CHECK(s.state == connection_state::connected);
		/* Reopen the acks: everything arrives, in order. */
		w.link.blocked[0] = false;
		w.run(120, [&](const unsigned i) { w.set_state(i); });
		s = w.peers[0].conn.stats();
		print_stats("host (acks back)", s);
		check_delivery(w, 0);
		CHECK(s.in_flight == 0);
		CHECK(s.queue_messages == 0);
	}
}

/* (e) Timeout and disconnect detection */

void test_timeouts(const std::uint64_t seed)
{
	begin("e: timeouts");
	{
		/* Silence in both directions: both sides close after 5 s. */
		rng r{seed};
		sim_world w{r, link_params{.latency = net_milliseconds(50)}};
		w.run(60, [&](const unsigned i) { w.set_state(i); });
		CHECK(w.peers[0].conn.state() == connection_state::connected);
		CHECK(w.peers[1].conn.state() == connection_state::connected);
		w.link.blocked = {{true, true}};
		const auto cut_at{w.now};
		net_clock closed_at{};
		while (w.peers[0].conn.state() != connection_state::closed)
		{
			w.tick([&](const unsigned i) { w.set_state(i); });
			closed_at = w.now;
			CHECK_MSG(w.now - cut_at < net_seconds(7), "host did not time out");
		}
		CHECK(w.peers[0].conn.closed_because() == close_reason::timeout);
		const auto elapsed{closed_at - cut_at};
		/* Packets in flight at the cut still arrive one latency later. */
		CHECK_MSG(elapsed >= NET_V2_TIMEOUT && elapsed < NET_V2_TIMEOUT + w.link.params.latency + 2 * TICK, "timeout after " + std::to_string(to_ms(elapsed)) + " ms");
		w.run(10, [&](const unsigned i) { w.set_state(i); });
		CHECK(w.peers[1].conn.state() == connection_state::closed);
		CHECK(w.peers[1].conn.closed_because() == close_reason::timeout);
		/* A closed connection sends nothing and takes nothing. */
		CHECK(w.peers[0].conn.build_outgoing(w.peers[0].clock(w.now) + TICK).empty());
		std::printf("    silence: both sides closed %.0f ms after the cut\n", to_ms(elapsed));
	}
	{
		/* A peer that keeps talking but never acks: 10 s unacked limit. */
		rng r{seed};
		sim_world w{r, link_params{.latency = net_milliseconds(50)}};
		w.run(30, [&](const unsigned i) { w.set_state(i); });
		w.link.mangle[0] = [](datagram &d) {
			/* Rewrite the client's ack fields to "nothing received". */
			if (d.size() >= NET_V2_HEADER_SIZE)
				std::fill(d.begin() + 14, d.begin() + 24, 0);
		};
		w.enqueue_random_message(0, 16);
		const auto start{w.now};
		net_clock closed_at{};
		while (w.peers[0].conn.state() != connection_state::closed)
		{
			w.tick([&](const unsigned i) { w.set_state(i); });
			closed_at = w.now;
			CHECK_MSG(w.now - start < net_seconds(12), "host did not give up on the unacked message");
		}
		CHECK(w.peers[0].conn.closed_because() == close_reason::unacked_timeout);
		const auto elapsed{closed_at - start};
		CHECK_MSG(elapsed >= NET_V2_UNACKED_TIMEOUT && elapsed < NET_V2_UNACKED_TIMEOUT + 3 * TICK, "unacked timeout after " + std::to_string(to_ms(elapsed)) + " ms");
		CHECK(w.peers[1].conn.state() == connection_state::connected);
		const auto s{w.peers[0].conn.stats()};
		print_stats("host", s);
		CHECK(s.rto == NET_V2_RTO_MAX || s.resends_by_rto > 0);
		std::printf("    unacked message: host closed after %.0f ms\n", to_ms(elapsed));
	}
	{
		/* Idle: keepalives every 100 ms keep the connection alive. */
		rng r{seed};
		sim_world w{r, link_params{.latency = net_milliseconds(50)}};
		w.run(30, [&](const unsigned i) { w.set_state(i); });
		const auto before{w.peers[0].conn.stats().packets_sent};
		w.run(600);
		const auto s{w.peers[0].conn.stats()};
		const auto keepalives{s.packets_sent - before};
		CHECK(s.state == connection_state::connected);
		/* At least 100 ms of silence rounds up to 7 ticks of 16.7 ms. */
		CHECK_MSG(keepalives >= 80 && keepalives <= 105, "keepalives in 10 s: " + std::to_string(keepalives));
		CHECK(s.in_flight == 0);
		std::printf("    idle: %llu keepalives in 10 s, still connected\n", static_cast<unsigned long long>(keepalives));
	}
}

/* Replay and reorder window (§3.7 step 6) */

void test_replay(const std::uint64_t seed)
{
	begin("replay/reorder window");
	rng r{seed};
	connection a{host_side, 0};
	connection b{client_side, 0};
	std::vector<datagram> packets;
	const std::array<std::uint8_t, 4> state{{1, 2, 3, 4}};
	for (unsigned i{}; i != 70; ++i)
	{
		a.set_unreliable_state(chunk_type::state, state);
		const auto p{a.build_outgoing(net_clock{i + 1} * TICK)};
		CHECK(!p.empty());
		packets.emplace_back(p.begin(), p.end());
	}
	const auto now{net_clock{100} * TICK};
	CHECK(b.on_receive(packets[69], now).status == receive_status::accepted);
	CHECK(b.on_receive(packets[69], now).status == receive_status::duplicate);
	CHECK_MSG(b.on_receive(packets[4], now).status == receive_status::duplicate, "65 behind is outside the window");
	CHECK_MSG(b.on_receive(packets[5], now).status == receive_status::accepted, "64 behind is inside the window");
	CHECK(b.on_receive(packets[5], now).status == receive_status::duplicate);
	CHECK(b.on_receive(packets[40], now).status == receive_status::accepted);
	CHECK(b.on_receive(packets[40], now).status == receive_status::duplicate);
	/* Late packets are reported in ack_bits; the sender sees them acked. */
	b.set_unreliable_state(chunk_type::input, state);
	const auto ack_packet{b.build_outgoing(now)};
	const auto h{*packet_header::read(ack_packet)};
	CHECK(h.ack == 70);
	CHECK((h.ack_bits & (std::uint64_t{1} << (70 - 1 - 41))) != 0);	/* packet 41 */
	CHECK((h.ack_bits & (std::uint64_t{1} << (70 - 1 - 6))) != 0);	/* packet 6 */
	CHECK((h.ack_bits & (std::uint64_t{1} << (70 - 1 - 7))) == 0);	/* packet 7 was never received */
	CHECK(a.on_receive(ack_packet, now).status == receive_status::accepted);
	const auto s{a.stats()};
	CHECK(s.packets_acked == 3);
	CHECK(s.packets_lost == 5);	/* packets 1..5 fell out of the bitfield */
	std::printf("    late packets inside 64 accepted once, older and repeated ones rejected\n");
}

/* (f) Fuzz */

void test_fuzz(const std::uint64_t seed)
{
	begin("f: fuzz");
	rng r{seed};
	connection target{host_side, 0};
	/* Random bytes: never accepted. */
	std::uint64_t random_packets{};
	for (unsigned i{}; i != 200000; ++i)
	{
		datagram d(r.below(1400));
		for (auto &byte : d)
			byte = static_cast<std::uint8_t>(r.next());
		const auto report{target.on_receive(d, net_clock{i} * 100)};
		CHECK(report.status != receive_status::accepted && report.status != receive_status::malformed_chunk);
		CHECK(report.reliable.empty() && report.unreliable.empty());
		++random_packets;
	}
	/* Random chunk bodies behind a header that passes steps 1-6: the chunk
	 * walk must reject or deliver without ever crashing, and a rejected
	 * packet delivers nothing.
	 */
	std::uint64_t structured{}, structured_accepted{}, structured_malformed{};
	{
		connection victim{host_side, 0};
		std::uint16_t seq{};
		for (unsigned i{}; i != 100000; ++i)
		{
			packet_header h;
			h.session_id = host_side.session_id;
			h.peer_token = host_side.peer_token;
			h.player_id = host_side.remote_player_id;
			h.flags = static_cast<std::uint8_t>(r.below(8));
			h.seq = ++seq;
			h.ack = static_cast<std::uint16_t>(r.next());
			h.ack_bits = r.next();
			h.send_time = static_cast<net_time>(r.next());
			h.echo_time = static_cast<net_time>(r.next());
			h.echo_delay = static_cast<std::uint16_t>(r.next());
			datagram d(NET_V2_HEADER_SIZE + r.below(NET_V2_MAX_PACKET - NET_V2_HEADER_SIZE + 8));
			h.write(d.data());
			for (std::size_t k{NET_V2_HEADER_SIZE}; k != d.size(); ++k)
				/* Bias toward plausible chunk types and small lengths. */
				d[k] = static_cast<std::uint8_t>(r.chance(0.5) ? r.below(8) : r.next());
			const auto report{victim.on_receive(d, net_clock{i} * TICK)};
			++structured;
			if (report.status == receive_status::accepted)
				++structured_accepted;
			else if (report.status == receive_status::malformed_chunk)
			{
				++structured_malformed;
				CHECK(report.reliable.empty() && report.unreliable.empty());
			}
			if (victim.state() == connection_state::closed)
			{
				CHECK(victim.closed_because() == close_reason::protocol_error || victim.closed_because() == close_reason::timeout);
				victim = connection{host_side, net_clock{i} * TICK};
				seq = 0;
			}
		}
	}
	/* Mutations of real traffic. */
	std::uint64_t mutated{}, mutated_accepted{};
	{
		sim_world w{r, link_params{.latency = net_milliseconds(40), .loss = 0.1}};
		std::vector<datagram> captured;
		w.link.mangle[1] = [&](datagram &d) { captured.push_back(d); };
		w.run(240, [&](const unsigned i) {
			w.enqueue_random_message(i, 60);
			w.set_state(i);
			w.send_event(i, w.tick_count);
		});
		w.link.mangle[1] = {};
		CHECK(captured.size() > 150);
		connection victim{client_side, w.peers[1].clock(w.now)};
		for (unsigned i{}; i != 100000; ++i)
		{
			datagram d{captured[r.below(captured.size())]};
			switch (r.below(4))
			{
				case 0:
					d.resize(r.below(d.size() + 40));
					break;
				case 1:
					d.insert(d.begin() + static_cast<std::ptrdiff_t>(r.below(d.size() + 1)), static_cast<std::uint8_t>(r.next()));
					break;
				default:
					for (unsigned k{}, n{static_cast<unsigned>(r.range(1, 8))}; k != n && !d.empty(); ++k)
						d[r.below(d.size())] = static_cast<std::uint8_t>(r.next());
					break;
			}
			const auto report{victim.on_receive(d, w.peers[1].clock(w.now) + net_clock{i} * 50)};
			++mutated;
			if (report.status == receive_status::accepted)
				++mutated_accepted;
			if (victim.state() == connection_state::closed)
				victim = connection{client_side, w.peers[1].clock(w.now) + net_clock{i} * 50};
		}
	}
	std::printf("    %llu random datagrams rejected; %llu structured: %llu accepted, %llu malformed; %llu mutated: %llu accepted; no crash\n",
		static_cast<unsigned long long>(random_packets),
		static_cast<unsigned long long>(structured), static_cast<unsigned long long>(structured_accepted), static_cast<unsigned long long>(structured_malformed),
		static_cast<unsigned long long>(mutated), static_cast<unsigned long long>(mutated_accepted));
	/* Protocol errors: 16 malformed packets in 10 s close the connection. */
	{
		connection victim{host_side, 0};
		std::uint16_t seq{};
		for (unsigned i{}; i != NET_V2_PROTOCOL_ERROR_LIMIT; ++i)
		{
			packet_header h;
			h.session_id = host_side.session_id;
			h.peer_token = host_side.peer_token;
			h.player_id = host_side.remote_player_id;
			h.flags = static_cast<std::uint8_t>(packet_flag::has_reliable);
			h.seq = ++seq;
			datagram d(NET_V2_HEADER_SIZE + NET_V2_CHUNK_HEADER_SIZE + NET_V2_RELIABLE_RUN_HEADER_SIZE + NET_V2_MESSAGE_HEADER_SIZE);
			h.write(d.data());
			auto *p{d.data() + NET_V2_HEADER_SIZE};
			chunk_header{.type = static_cast<std::uint8_t>(chunk_type::reliable), .length = NET_V2_RELIABLE_RUN_HEADER_SIZE + NET_V2_MESSAGE_HEADER_SIZE}.write(p);
			/* A message 300 sequences beyond the receive window. */
			net_put_le16(p + 3, 300);
			p[5] = 1;
			p[6] = 7;
			net_put_le16(p + 7, 0);
			CHECK(victim.state() != connection_state::closed);
			CHECK(victim.on_receive(d, net_clock{i} * TICK).status == receive_status::malformed_chunk);
		}
		CHECK(victim.state() == connection_state::closed);
		CHECK(victim.closed_because() == close_reason::protocol_error);
		std::printf("    %u malformed packets close the connection (protocol_error)\n", NET_V2_PROTOCOL_ERROR_LIMIT);
	}
}

/* (g) Clock offset */

void test_clock(const std::uint64_t seed)
{
	begin("g: clock offset estimation");
	rng r{seed};
	/* The host clock is about to wrap its low 32 bits; the client clock
	 * is 1234.5678 s behind.
	 */
	constexpr net_clock host_bias{0xffff0000};
	constexpr net_clock client_bias{host_bias - 80906240};
	sim_world w{r, link_params{.latency = net_milliseconds(50), .jitter = net_milliseconds(20)}, {{host_bias, client_bias}}};
	const auto truth{[&]() { return w.peers[0].bias - w.peers[1].bias; }};
	const auto error{[&]() {
		const auto s{w.peers[1].conn.stats()};
		CHECK(s.clock_offset_valid);
		const auto e{s.clock_offset_target - truth()};
		return e < 0 ? -e : e;
	}};
	/* The first sample needs a full round trip plus the tick alignment
	 * of both peers: about 8 ticks here.  After 15 the estimate rests on
	 * a handful of samples and must already be close.
	 */
	w.run(15, [&](const unsigned i) { w.set_state(i); });
	const auto early{error()};
	std::printf("    after 15 ticks (%zu samples): target error %.2f ms\n", static_cast<std::size_t>(w.peers[1].conn.stats().packets_received), to_ms(early));
	CHECK_MSG(early < net_milliseconds(20), "early target error " + std::to_string(to_ms(early)) + " ms");
	w.run(105, [&](const unsigned i) { w.set_state(i); });
	const auto settled{error()};
	auto s{w.peers[1].conn.stats()};
	std::printf("    after 2 s: target error %.2f ms, applied error %.2f ms, host sees client offset %.2f ms\n",
		to_ms(settled), to_ms(s.clock_offset - truth()), to_ms(w.peers[0].conn.stats().clock_offset));
	CHECK_MSG(settled < net_milliseconds(5), "settled target error " + std::to_string(to_ms(settled)) + " ms");
	const auto applied{s.clock_offset - truth()};
	CHECK((applied < 0 ? -applied : applied) < net_milliseconds(5));
	/* Symmetric: the host's estimate of the client is the negative. */
	const auto host_offset{w.peers[0].conn.stats().clock_offset};
	const auto host_err{host_offset + truth()};
	CHECK((host_err < 0 ? -host_err : host_err) < net_milliseconds(5));
	CHECK(w.peers[1].conn.to_peer_time(w.peers[1].clock(w.now)) - w.peers[0].clock(w.now) < net_milliseconds(5));
	/* A 300 ms step of the client clock is followed within one window. */
	w.peers[1].bias += net_milliseconds(300);
	w.run(150, [&](const unsigned i) { w.set_state(i); });
	s = w.peers[1].conn.stats();
	const auto after_step{s.clock_offset - truth()};
	std::printf("    300 ms step: applied error %.2f ms after 2.5 s\n", to_ms(after_step));
	CHECK((after_step < 0 ? -after_step : after_step) < net_milliseconds(5));
	/* A small drift is slewed, not jumped: after 20 ms of drift the
	 * applied offset is still moving toward the target.
	 */
	w.peers[1].bias += net_milliseconds(20);
	w.run(60, [&](const unsigned i) { w.set_state(i); });
	s = w.peers[1].conn.stats();
	const auto during_slew{s.clock_offset - truth()};
	std::printf("    20 ms step: applied error %.2f ms after 1 s (slewing at %.0f ms/s)\n", to_ms(during_slew), to_ms(NET_V2_CLOCK_SLEW_PER_SECOND));
	CHECK(during_slew < 0 ? -during_slew > net_milliseconds(10) : during_slew > net_milliseconds(10));
	w.run(240, [&](const unsigned i) { w.set_state(i); });
	s = w.peers[1].conn.stats();
	const auto after_slew{s.clock_offset - truth()};
	CHECK((after_slew < 0 ? -after_slew : after_slew) < net_milliseconds(5));
	std::printf("    settled again: applied error %.2f ms\n", to_ms(after_slew));
}

/* Wire layout of the header (§3.1) */

void test_header_layout()
{
	begin("header layout");
	static_assert(NET_V2_HEADER_SIZE == 34);
	static_assert(NET_V2_MAX_CHUNK_PAYLOAD == 1163);
	packet_header h;
	h.session_id = 0x04030201;
	h.peer_token = 0x08070605;
	h.player_id = 7;
	h.flags = 0x03;
	h.seq = 0x0a09;
	h.ack = 0x0c0b;
	h.ack_bits = 0x14131211100f0e0dull;
	h.send_time = 0x18171615;
	h.echo_time = 0x1c1b1a19;
	h.echo_delay = 0x1e1d;
	std::array<std::uint8_t, NET_V2_HEADER_SIZE> buf{};
	h.write(buf.data());
	const std::array<std::uint8_t, NET_V2_HEADER_SIZE> expected{{
		100, 0,
		1, 2, 3, 4,
		5, 6, 7, 8,
		7,
		3,
		9, 10,
		11, 12,
		13, 14, 15, 16, 17, 18, 19, 20,
		21, 22, 23, 24,
		25, 26, 27, 28,
		29, 30,
	}};
	CHECK(buf == expected);
	const auto back{packet_header::read(buf)};
	CHECK(back.has_value());
	CHECK(back->proto == 100 && back->session_id == h.session_id && back->peer_token == h.peer_token && back->player_id == 7 && back->flags == 3 && back->seq == h.seq && back->ack == h.ack && back->ack_bits == h.ack_bits && back->send_time == h.send_time && back->echo_time == h.echo_time && back->echo_delay == h.echo_delay);
	CHECK(!packet_header::read(std::span{buf}.first(33)).has_value());
	/* A packet that a v1 build would parse: first byte 100 is not a
	 * valid upid.  A v2 build drops anything without proto 100.
	 */
	buf[1] = 1;
	connection c{host_side, 0};
	CHECK(c.on_receive(buf, 0).status == receive_status::bad_proto);
	buf[1] = 0;
	buf[11] = 0x0a;	/* keepalive + reserved bit */
	CHECK(c.on_receive(buf, 0).status == receive_status::bad_flags);
	buf[11] = 0x04;
	CHECK(c.on_receive(buf, 0).status == receive_status::unconnected);
	buf[11] = 0x02;
	CHECK(c.on_receive(buf, 0).status == receive_status::bad_session);
	std::printf("    34-byte header round-trips with the documented offsets\n");
}

}

int main(const int argc, char **const argv)
{
	const std::uint64_t seed{argc > 1 ? std::strtoull(argv[1], nullptr, 0) : 0x5eed'0000'0001ull};
	std::printf("net_v2 transport simulation test, seed 0x%llx, tick %.2f ms\n", static_cast<unsigned long long>(seed), to_ms(TICK));
	test_header_layout();
	test_lossless(seed);
	test_lossy(seed);
	test_rtt(seed);
	test_bounds(seed);
	test_timeouts(seed);
	test_replay(seed);
	test_fuzz(seed);
	test_clock(seed);
	std::printf("all tests passed\n");
	return 0;
}
