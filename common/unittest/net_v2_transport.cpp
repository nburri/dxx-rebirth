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
#include <iterator>
#include <span>
#include <string>
#include <tuple>
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

/* One 60 Hz period rounded up (1093 units): stepping a connection by TICK
 * always covers a full period.  The simulation itself runs on the exact
 * period, tick_count * 65536 / 60.
 */
constexpr net_clock TICK{NET_V2_DEFAULT_TICK.units()};

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
	 * [reorder_min, reorder_delay] so that later packets overtake it.
	 */
	double reorder{};
	net_clock reorder_min{1};
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
				delay += m_rng.range(params.reorder_min, params.reorder_delay);
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
		/* Move the due packets to the back, take them out, sort them. */
		const auto due_range{std::ranges::stable_partition(m_queue, [&](const queued &q) { return q.dest != dest || q.arrival > now; })};
		std::vector<queued> due(std::make_move_iterator(due_range.begin()), std::make_move_iterator(due_range.end()));
		m_queue.erase(due_range.begin(), due_range.end());
		std::ranges::sort(due, {}, [](const queued &q) { return std::pair{q.arrival, q.order}; });
		for (auto &q : due)
		{
			if (mangle[dest])
				mangle[dest](q.bytes);
			if (q.bytes.empty())
			{
				/* A mangler that empties the datagram drops it. */
				++dropped;
				continue;
			}
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
	/* Copies: the report's messages are views that end with the next
	 * on_receive.
	 */
	std::vector<sent_message> delivered;
	std::vector<sent_message> sent;
	std::vector<std::uint32_t> states_seen;
	std::array<unsigned, NET_V2_STATE_MAX_PARTS> part_counts{};
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
		for (const auto &m : report.reliable)
			delivered.push_back({.type = m.type, .payload = {m.payload.begin(), m.payload.end()}});
		for (const auto &u : report.unreliable)
		{
			if (u.payload.size() < 4)
				continue;
			const auto value{net_get_le32(u.payload.data())};
			if (u.type == chunk_type::event_u)
				events_seen.push_back(value);
			else
			{
				states_seen.push_back(value);
				++part_counts[u.part];
			}
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
	/* Each peer ticks this much after the simulation tick, so that the
	 * two are not phase-aligned (they never are in reality).
	 */
	std::array<net_clock, 2> phase{};
	net_clock now{};
	std::uint32_t tick_count{};

	/* Peer i ticks every tick_every[i] simulation ticks (a 30 Hz client
	 * against the 60 Hz simulation is {1, 2}).
	 */
	std::array<unsigned, 2> tick_every{{1, 1}};

	sim_world(rng &r, const link_params &p, const std::array<net_clock, 2> biases = {}, const std::array<connection_config, 2> &configs = {{host_side, client_side}}) :
		random{r},
		link{r, p},
		peers{{
			sim_peer{configs[0], biases[0], 0},
			sim_peer{configs[1], biases[1], 0},
		}}
	{
	}

	/* One 60 Hz tick: deliver what arrived, let the test act, build and
	 * send.  `act(peer_index)` runs before that peer builds its packets.
	 */
	void tick(const std::function<void(unsigned)> &act = {})
	{
		++tick_count;
		now = net_clock{tick_count} * net_seconds(1) / 60;
		for (unsigned i{}; i != 2; ++i)
		{
			if (tick_count % tick_every[i] != 0)
				continue;
			auto &p{peers[i]};
			const auto peer_now{now + phase[i]};
			link.deliver(i, peer_now, [&](const datagram &d, const net_clock at) { p.receive(d, at); });
			if (act)
				act(i);
			p.conn.begin_tick(p.clock(peer_now));
			for (;;)
			{
				const auto packet{p.conn.build_outgoing(p.clock(peer_now))};
				if (packet.empty())
					break;
				CHECK(packet.size() >= NET_V2_HEADER_SIZE && packet.size() <= NET_V2_MAX_PACKET);
				link.send(i, packet, peer_now);
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

	/* A bundle of `count` parts of `size` bytes each, tagged with the
	 * tick and the part number.
	 */
	void set_state_parts(const unsigned from, const unsigned count, const std::size_t size)
	{
		datagram part(size);
		for (unsigned i{}; i != count; ++i)
		{
			net_put_le32(part.data(), tick_count);
			part[4] = static_cast<std::uint8_t>(i);
			peers[from].conn.set_unreliable_state(from == 0 ? chunk_type::state : chunk_type::input, i, count, part);
		}
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
			/* Within 10 %, with or without loss: a lost echo carrier does
			 * not make its packet's ack a measurement.
			 */
			CHECK_MSG(err * 10 < expected, "srtt " + std::to_string(to_ms(s.srtt)) + " ms vs " + std::to_string(to_ms(expected)) + " ms at " + std::to_string(loss * 100) + "% loss");
			CHECK(s.rto >= s.srtt);
			CHECK(s.rto >= NET_V2_RTO_MIN && s.rto <= NET_V2_RTO_MAX);
			CHECK(s.rttvar < net_milliseconds(30));
		}
	}
	/* The formula and its clamp: a 700 ms link gives RTO_MAX; a 1 ms
	 * link gives the formula's value (about 52 ms with the floor and the
	 * two tick holds), clamped from below at RTO_MIN.
	 */
	for (const auto &[one_way, expected_rto] : {std::pair{net_milliseconds(1), net_clock{}}, std::pair{net_milliseconds(700), NET_V2_RTO_MAX}})
	{
		rng r{seed};
		sim_world w{r, link_params{.latency = one_way}};
		w.run(300, [&](const unsigned i) { w.set_state(i); });
		const auto s{w.peers[0].conn.stats()};
		const auto tick{w.peers[0].conn.config().tick.units()};
		const auto formula{std::clamp(s.srtt + 4 * s.rttvar + tick + tick, NET_V2_RTO_MIN, NET_V2_RTO_MAX)};
		CHECK_MSG(s.rto == formula, "rto " + std::to_string(to_ms(s.rto)) + " ms vs formula " + std::to_string(to_ms(formula)) + " ms for one-way " + std::to_string(to_ms(one_way)) + " ms");
		if (expected_rto != 0)
			CHECK(s.rto == expected_rto);
		else
			CHECK(s.rto >= NET_V2_RTO_MIN && s.rto < net_milliseconds(60) && s.rttvar == tick / 4);
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
		/* The state part boundary: the maximum is taken, one more is not. */
		connection s{host_side, 0};
		s.set_unreliable_state(chunk_type::state, std::vector<std::uint8_t>(NET_V2_MAX_STATE_PART));
		CHECK(s.stats().unreliable_dropped == 0);
		CHECK(s.build_outgoing(0).size() == NET_V2_MAX_PACKET);
		s.set_unreliable_state(chunk_type::state, std::vector<std::uint8_t>(NET_V2_MAX_STATE_PART + 1));
		CHECK(s.stats().unreliable_dropped == 1);
		CHECK(s.build_outgoing(TICK).empty());
		std::printf("    oversize message rejected at enqueue; state part of %zu bytes taken, %zu dropped\n", NET_V2_MAX_STATE_PART, NET_V2_MAX_STATE_PART + 1);
	}
	{
		/* Message count bound */
		connection c{host_side, 0};
		const std::array<std::uint8_t, 8> small{};
		for (unsigned i{}; i != NET_V2_QUEUE_MAX_MESSAGES; ++i)
			CHECK(c.enqueue_reliable(1, small) == enqueue_result::ok);
		CHECK(c.state() != connection_state::closed);
		CHECK(c.closed_because() == close_reason::none);
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

/* Review findings: one case each. */

/* 1. One packet never selects more than the 256-message window, even
 * when nothing is in flight yet.
 */
void test_window_in_one_packet()
{
	begin("window bound within one packet");
	connection a{host_side, 0};
	connection b{client_side, 0};
	for (unsigned i{}; i != 300; ++i)
		CHECK(a.enqueue_reliable(static_cast<std::uint8_t>(i), {}) == enqueue_result::ok);
	const auto p1{a.build_outgoing(TICK)};
	CHECK(!p1.empty());
	CHECK_MSG(a.stats().in_flight == NET_V2_MAX_IN_FLIGHT, "in flight " + std::to_string(a.stats().in_flight));
	const auto r1{b.on_receive(p1, TICK + 1)};
	CHECK(r1.status == receive_status::accepted);
	CHECK(r1.reliable.size() == NET_V2_MAX_IN_FLIGHT);
	/* The rest waits for acks, not for the next packet. */
	CHECK(a.build_outgoing(TICK).empty());
	const auto ack{b.build_outgoing(2 * TICK)};
	CHECK(a.on_receive(ack, 2 * TICK).status == receive_status::accepted);
	const auto p2{a.build_outgoing(3 * TICK)};
	const auto r2{b.on_receive(p2, 3 * TICK + 1)};
	CHECK(r2.status == receive_status::accepted);
	CHECK(r2.reliable.size() == 300 - NET_V2_MAX_IN_FLIGHT);
	for (std::size_t i{}; i != r2.reliable.size(); ++i)
		CHECK(r2.reliable[i].type == static_cast<std::uint8_t>(NET_V2_MAX_IN_FLIGHT + i));
	std::printf("    300 queued messages: first packet carries %u, receiver accepts, rest follows after the ack\n", NET_V2_MAX_IN_FLIGHT);
}

/* 2. A backlog drains at no more than max_packets_per_tick per tick. */
void test_packets_per_tick(const std::uint64_t seed)
{
	begin("packets per tick");
	rng r{seed};
	sim_world w{r, link_params{.latency = net_milliseconds(30)}};
	w.run(6, [&](const unsigned i) { w.set_state(i); });
	datagram big(NET_V2_MAX_MESSAGE);
	for (unsigned k{}; k != 90; ++k)
	{
		big[0] = static_cast<std::uint8_t>(k);
		CHECK(w.peers[0].conn.enqueue_reliable(9, big) == enqueue_result::ok);
		w.peers[0].sent.push_back({.type = 9, .payload = big});
	}
	CHECK(w.peers[0].conn.stats().queue_bytes == 90 * NET_V2_MAX_MESSAGE);
	std::uint64_t max_per_tick{}, ticks_with_two{};
	auto before{w.peers[0].conn.stats().packets_sent};
	w.run(120, [&](const unsigned i) {
		w.set_state(i);
		if (i == 1)
		{
			const auto now_sent{w.peers[0].conn.stats().packets_sent};
			max_per_tick = std::max(max_per_tick, now_sent - before);
			if (now_sent - before == 2)
				++ticks_with_two;
			before = now_sent;
		}
	});
	check_delivery(w, 0);
	CHECK_MSG(max_per_tick == NET_V2_DEFAULT_MAX_PACKETS_PER_TICK, "max packets in one tick " + std::to_string(max_per_tick));
	CHECK_MSG(ticks_with_two >= 40, "ticks with two packets " + std::to_string(ticks_with_two));
	std::printf("    90 KiB backlog: at most %llu packets per tick, %llu ticks used the second packet\n",
		static_cast<unsigned long long>(max_per_tick), static_cast<unsigned long long>(ticks_with_two));
}

/* 3. Peers that tick with an arbitrary phase offset on a jitter-free,
 * lossless link never retransmit: the RTO covers the ack hold.
 */
void test_unaligned_peers(const std::uint64_t seed)
{
	begin("unaligned peers, no spurious RTO resends");
	rng r{seed};
	for (unsigned round{}; round != 6; ++round)
	{
		sim_world w{r, link_params{.latency = net_milliseconds(round == 0 ? 1 : 20 * round)}};
		w.phase = {{static_cast<net_clock>(r.below(TICK)), static_cast<net_clock>(r.below(TICK))}};
		w.run(300, [&](const unsigned i) {
			w.set_state(i);
			for (unsigned k{}; k != 2 && w.peers[i].sent.size() < 500; ++k)
				w.enqueue_random_message(i, 60);
		});
		for (unsigned i{}; i != 2; ++i)
		{
			check_delivery(w, i);
			const auto s{w.peers[i].conn.stats()};
			CHECK_MSG(s.message_resends == 0, "round " + std::to_string(round) + (i ? " client" : " host") + " resends " + std::to_string(s.message_resends) + " (rto " + std::to_string(s.resends_by_rto) + ")");
			CHECK(s.rto >= s.srtt + w.peers[i].conn.config().tick.units());
		}
		std::printf("    phases %.1f/%.1f ms, latency %.0f ms: 0 resends, srtt %.1f ms, rto %.1f ms\n",
			to_ms(w.phase[0]), to_ms(w.phase[1]), to_ms(w.link.params.latency), to_ms(w.peers[0].conn.stats().srtt), to_ms(w.peers[0].conn.stats().rto));
	}
}

/* 4. With every ack blacked out, packets are still counted lost once
 * their log slot is reused, so the loss estimate rises toward 1.
 */
void test_ack_blackout_loss(const std::uint64_t seed)
{
	begin("loss estimate under an ack blackout");
	rng r{seed};
	sim_world w{r, link_params{.latency = net_milliseconds(30)}};
	w.run(30, [&](const unsigned i) { w.set_state(i); });
	CHECK(w.peers[0].conn.stats().loss_estimate < 0.01);
	w.link.mangle[0] = [](datagram &d) {
		if (d.size() >= NET_V2_HEADER_SIZE)
			std::fill(d.begin() + 14, d.begin() + 24, 0);
	};
	w.run(500, [&](const unsigned i) { w.set_state(i); });
	const auto s{w.peers[0].conn.stats()};
	print_stats("host", s);
	CHECK(s.state == connection_state::connected);
	CHECK_MSG(s.packets_lost > 200, "packets lost " + std::to_string(s.packets_lost));
	CHECK_MSG(s.loss_estimate > 0.9, "loss estimate " + std::to_string(s.loss_estimate));
	std::printf("    500 unacked packets: %llu counted lost, loss estimate %.3f\n", static_cast<unsigned long long>(s.packets_lost), s.loss_estimate);
}

/* 5. A build time earlier than the receive stamp gives echo_delay 0, not
 * a wrapped value, and the peer's RTT sample stays sane.
 */
void test_echo_delay_clamp()
{
	begin("echo_delay clamp");
	connection a{host_side, 0};
	connection b{client_side, 0};
	const std::array<std::uint8_t, 4> x{{1, 2, 3, 4}};
	b.set_unreliable_state(chunk_type::input, x);
	const auto pb{b.build_outgoing(500)};
	CHECK(a.on_receive(pb, 1000).status == receive_status::accepted);
	a.set_unreliable_state(chunk_type::state, x);
	const auto pa{a.build_outgoing(900)};
	const auto h{*packet_header::read(pa)};
	CHECK(h.echo_time == 500);
	CHECK_MSG(h.echo_delay == 0, "echo_delay " + std::to_string(h.echo_delay));
	CHECK(b.on_receive(pa, 1700).status == receive_status::accepted);
	const auto s{b.stats()};
	CHECK(s.rtt_valid);
	CHECK_MSG(s.srtt == 1200, "srtt " + std::to_string(s.srtt));
	std::printf("    build 100 units before the receive stamp: echo_delay 0, peer RTT sample %lld units\n", static_cast<long long>(s.srtt));
}

/* 6. Every unreliable chunk of a packet is delivered; latest-wins applies
 * across packets only.
 */
void test_unreliable_chunks_per_packet()
{
	begin("unreliable chunks per packet");
	connection a{host_side, 0};
	connection b{client_side, 0};
	const std::array<std::uint8_t, 3> part0{{0, 0, 0}}, part1{{1, 1, 1}}, later{{2, 2, 2}}, event{{9, 9, 9}};
	a.set_unreliable_state(chunk_type::state, 0, 2, part0);
	a.set_unreliable_state(chunk_type::state, 1, 2, part1);
	/* Only events through send_unreliable: state/input have their own
	 * path, and anything else the peer would reject as malformed.
	 */
	CHECK(!a.send_unreliable(chunk_type::state, part1));
	CHECK(!a.send_unreliable(chunk_type::input, part1));
	CHECK(!a.send_unreliable(chunk_type::reliable, part1));
	CHECK(!a.send_unreliable(chunk_type::session, part1));
	CHECK(!a.send_unreliable(static_cast<chunk_type>(200), part1));
	CHECK(a.stats().unreliable_dropped == 5);
	const auto p1{a.build_outgoing(TICK)};
	const auto r1{b.on_receive(p1, TICK + 1)};
	CHECK(r1.status == receive_status::accepted);
	CHECK_MSG(r1.unreliable.size() == 2, "chunks delivered " + std::to_string(r1.unreliable.size()));
	CHECK(r1.unreliable[0].type == chunk_type::state && std::ranges::equal(r1.unreliable[0].payload, part0));
	CHECK(r1.unreliable[1].type == chunk_type::state && std::ranges::equal(r1.unreliable[1].payload, part1));
	/* Two more packets, delivered in reverse: the older state is dropped,
	 * but its event still arrives.
	 */
	a.set_unreliable_state(chunk_type::state, part1);
	CHECK(a.send_unreliable(chunk_type::event_u, event));
	const datagram p2{[&] { const auto p{a.build_outgoing(2 * TICK)}; return datagram{p.begin(), p.end()}; }()};
	a.set_unreliable_state(chunk_type::state, later);
	const datagram p3{[&] { const auto p{a.build_outgoing(3 * TICK)}; return datagram{p.begin(), p.end()}; }()};
	const auto r3{b.on_receive(p3, 3 * TICK + 1)};
	CHECK(r3.status == receive_status::accepted && r3.unreliable.size() == 1 && std::ranges::equal(r3.unreliable[0].payload, later));
	const auto r2{b.on_receive(p2, 3 * TICK + 2)};
	CHECK(r2.status == receive_status::accepted);
	CHECK_MSG(r2.unreliable.size() == 1 && r2.unreliable[0].type == chunk_type::event_u, "reordered packet delivered " + std::to_string(r2.unreliable.size()) + " chunks");
	std::printf("    two STATE chunks in one packet both delivered; an older packet's STATE dropped, its EVENT_U kept\n");
}

/* Fifteenth review round. */

/* 1. A well-formed packet whose seq is far ahead (a corrupted or forged
 * bit) is rejected and counted, not taken as the new highest: the stream
 * continues, and our acks stay valid at the peer.
 */
void test_forged_seq_far_ahead()
{
	begin("forged seq far ahead");
	connection a{host_side, 0};
	connection b{client_side, 0};
	const std::array<std::uint8_t, 2> x{};
	const auto exchange{[&](const unsigned i) {
		const net_clock t{net_clock{i} * TICK};
		a.set_unreliable_state(chunk_type::state, x);
		const auto p{a.build_outgoing(t)};
		CHECK(!p.empty());
		CHECK_MSG(b.on_receive(p, t + 1).status == receive_status::accepted, "real packet at tick " + std::to_string(i) + " rejected");
		b.set_unreliable_state(chunk_type::input, x);
		const auto q{b.build_outgoing(t + 2)};
		CHECK(!q.empty());
		CHECK_MSG(a.on_receive(q, t + 3).status == receive_status::accepted, "peer's packet at tick " + std::to_string(i) + " rejected");
	}};
	for (unsigned i{}; i != 5; ++i)
		exchange(i);
	/* A keepalive (100 ms after the last packet) with bit 14 of its seq
	 * flipped.
	 */
	const net_clock t{11 * TICK};
	datagram k{[&] { const auto p{a.build_outgoing(t)}; return datagram{p.begin(), p.end()}; }()};
	auto h{*packet_header::read(k)};
	CHECK(h.has_flag(packet_flag::keepalive));
	h.seq = static_cast<std::uint16_t>(h.seq ^ (1u << 14));
	h.write(k.data());
	CHECK(b.on_receive(k, t + 1).status == receive_status::bad_seq);
	CHECK(b.stats().protocol_errors == 1);
	/* A replay of it counts no further error. */
	CHECK(b.on_receive(k, t + 2).status == receive_status::bad_seq);
	CHECK(b.stats().protocol_errors == 1);
	/* The stream continues, and the peer sees no bad_ack. */
	for (unsigned i{12}; i != 18; ++i)
		exchange(i);
	CHECK(a.stats().protocol_errors == 0 && b.stats().packets_received == 11);
	CHECK(a.state() == connection_state::connected && b.state() == connection_state::connected);
	std::printf("    keepalive with seq bit 14 flipped: bad_seq, one protocol error, the stream continues, no bad_ack at the peer\n");
}

/* 2. When the 2 s sample window empties (a stall), the clock target
 * freezes at the applied offset instead of the slew running on toward an
 * expired sample.
 */
void test_clock_stall_freezes_offset(const std::uint64_t seed)
{
	begin("clock target freezes in a stall");
	rng r{seed};
	sim_world w{r, link_params{.latency = net_milliseconds(50)}, {{0, net_seconds(100)}}};
	w.run(120, [&](const unsigned i) { w.set_state(i); });
	CHECK(w.peers[1].conn.stats().clock_offset_valid);
	/* A 50 ms step: below the jump threshold, so it is slewed at 5 ms/s
	 * once the old samples have left the window.
	 */
	w.peers[1].bias += net_milliseconds(50);
	w.run(130, [&](const unsigned i) { w.set_state(i); });
	const auto before_stall{w.peers[1].conn.stats()};
	CHECK_MSG(before_stall.clock_offset_target != before_stall.clock_offset, "slew not in progress: target " + std::to_string(before_stall.clock_offset_target) + " applied " + std::to_string(before_stall.clock_offset));
	/* Nothing gets through for 2.5 s (the timeout is 5 s). */
	w.link.blocked = {{true, true}};
	w.run(60, [&](const unsigned i) { w.set_state(i); });
	const auto slewing{w.peers[1].conn.stats().clock_offset};
	CHECK_MSG(slewing != before_stall.clock_offset, "the slew stopped while samples were still in the window");
	w.run(72, [&](const unsigned i) { w.set_state(i); });
	const auto frozen{w.peers[1].conn.stats()};
	w.run(18, [&](const unsigned i) { w.set_state(i); });
	const auto later{w.peers[1].conn.stats()};
	CHECK_MSG(later.clock_offset == frozen.clock_offset, "applied offset still moving after the window emptied: " + std::to_string(frozen.clock_offset) + " -> " + std::to_string(later.clock_offset));
	CHECK(later.clock_offset_target == later.clock_offset);
	CHECK(later.state == connection_state::connected);
	const auto truth{w.peers[0].bias - w.peers[1].bias};
	std::printf("    50 ms step, then a 2.5 s stall: the applied offset slews until the window empties, then holds %.2f ms from the truth\n", to_ms(later.clock_offset - truth));
}

/* Fourteenth review round. */

/* 1. Priority within a tick: state parts, then reliable messages, then
 * events fill what is left.  Only the tick's first packet reserves room
 * for the head event; a message blocked out of it is never blocked again
 * by an event (the round-12 reservation starved a 1 KiB message behind a
 * 700-byte state and a 700-byte event per tick for good).
 */
void test_reliable_before_events(const std::uint64_t seed)
{
	begin("reliable messages before events");
	rng r{seed};
	sim_world w{r, link_params{.latency = net_milliseconds(30)}};
	const datagram big(NET_V2_MAX_MESSAGE), event(700);
	std::uint32_t sent{};
	bool queued{};
	/* The host's turn (act runs per peer): a state, an event, and once
	 * the message.
	 */
	const auto tick{[&](const unsigned peer) {
		if (peer != 0)
			return;
		w.set_state_parts(0, 1, 700);
		datagram e{event};
		net_put_le32(e.data(), sent++);
		CHECK(w.peers[0].conn.send_unreliable(chunk_type::event_u, e));
		if (!queued)
		{
			queued = true;
			CHECK(w.peers[0].conn.enqueue_reliable(9, big) == enqueue_result::ok);
			w.peers[0].sent.push_back({.type = 9, .payload = big});
		}
	}};
	/* The message goes out in the first tick and arrives 30 ms later. */
	w.run(3, tick);
	CHECK_MSG(w.peers[1].delivered.size() == 1, "message delivered after 3 ticks: " + std::to_string(w.peers[1].delivered.size()));
	w.run(60, tick);
	w.run(5, [&](const unsigned peer) {
		if (peer == 0)
			w.set_state_parts(0, 1, 700);
	});
	check_delivery(w, 0);
	const auto host{w.peers[0].conn.stats()};
	CHECK(host.state == connection_state::connected);
	CHECK_MSG(host.unreliable_dropped == 0, "events dropped " + std::to_string(host.unreliable_dropped));
	CHECK_MSG(w.peers[1].events_seen.size() >= 60, "events delivered " + std::to_string(w.peers[1].events_seen.size()) + " of " + std::to_string(sent));
	std::printf("    700-byte state and event per tick, 1 KiB message queued: sent in the first tick, %zu of %u events delivered, none dropped\n", w.peers[1].events_seen.size(), sent);
}

/* 2. The bound hold ages by granted ticks, not by grants: a caller at
 * 30 Hz (two ticks per grant) forgets a bound at the same wall-clock
 * speed as one at 60 Hz.
 */
void test_bound_hold_ages_by_ticks()
{
	begin("30 Hz caller ages the bound hold at wall-clock speed");
	connection a{host_side, 0};
	connection b{client_side, 0};
	const std::array<std::uint8_t, 4> x{};
	const net_clock latency{1000};
	/* One exchange: a builds (every second tick, so each grant is two
	 * ticks), b answers at once with the echo.  Returns b's packet so
	 * that it may be held back.
	 */
	const auto exchange{[&](const net_clock t, const bool deliver) {
		a.set_unreliable_state(chunk_type::state, x);
		const auto p{a.build_outgoing(t)};
		CHECK(!p.empty());
		CHECK(b.on_receive(p, t + latency).status == receive_status::accepted);
		b.set_unreliable_state(chunk_type::input, x);
		const auto q{b.build_outgoing(t + latency + 100)};
		CHECK(!q.empty());
		if (deliver)
			CHECK(a.on_receive(q, t + 2 * latency + 100).status == receive_status::accepted);
		return datagram{q.begin(), q.end()};
	}};
	net_clock t{};
	for (unsigned i{}; i != 60; ++i, t += 2 * TICK)
		exchange(t, true);
	const auto steady{a.stats()};
	CHECK_MSG(steady.rtt_valid && steady.srtt == 2 * latency, "srtt " + std::to_string(steady.srtt));
	/* b's answer to this packet is held back ... */
	const auto held{exchange(t, false)};
	t += 2 * TICK;
	/* ... its successor arrives normally ... */
	exchange(t, true);
	/* ... and the held one 200 ms late: a bound of 2 ticks + 200 ms over
	 * srtt, held for a second.
	 */
	const net_clock extra{net_milliseconds(200)};
	CHECK(a.on_receive(held, t + 2 * latency + 100 + extra).status == receive_status::accepted);
	const net_clock bound_at{t};
	const auto bounded{a.stats()};
	const net_clock excess{2 * TICK + extra};
	CHECK_MSG(bounded.rto >= bounded.srtt + excess, "rto " + std::to_string(bounded.rto) + " right after the bound");
	/* Just short of a second later the excess is still covered. */
	for (t += 2 * TICK; t < bound_at + net_seconds(1) - 2 * TICK; t += 2 * TICK)
		exchange(t, true);
	const auto held_stats{a.stats()};
	CHECK_MSG(held_stats.rto >= held_stats.srtt + excess, "rto " + std::to_string(held_stats.rto) + " inside the hold");
	/* Half a second after the hold (30 ticks in 15 grants) it has faded
	 * to a seventh; aged per grant it would still be whole.
	 */
	for (; t < bound_at + net_seconds(1) + net_milliseconds(500); t += 2 * TICK)
		exchange(t, true);
	const auto faded{a.stats()};
	CHECK_MSG(faded.rto < faded.srtt + excess / 4 + 2 * TICK + 1000, "rto " + std::to_string(faded.rto) + " half a second after the hold, srtt " + std::to_string(faded.srtt));
	std::printf("    bound of %.1f ms over srtt: rto %.1f ms inside the hold, %.1f ms half a second after it (30 Hz caller)\n", to_ms(excess), to_ms(held_stats.rto), to_ms(faded.rto));
}

/* Thirteenth review round. */

/* 5. Delivered reliable messages are views: a message delivered while
 * nothing is held views the datagram, one that went through the receive
 * window views storage the connection keeps until its next on_receive,
 * so the datagram it came in may be gone by then.
 */
void test_held_message_view_lifetime()
{
	begin("held message delivered as a view");
	connection a{host_side, 0};
	connection b{client_side, 0};
	const std::array<std::uint8_t, 6> x1{{1, 1, 1, 1, 1, 1}}, x2{{2, 2, 2, 2, 2, 2}};
	CHECK(a.enqueue_reliable(1, x1) == enqueue_result::ok);
	const datagram p1{[&] { const auto p{a.build_outgoing(0)}; return datagram{p.begin(), p.end()}; }()};
	CHECK(a.enqueue_reliable(2, x2) == enqueue_result::ok);
	datagram p2{[&] { const auto p{a.build_outgoing(TICK)}; return datagram{p.begin(), p.end()}; }()};
	/* Packet 2 first: its message is held. */
	const auto r2{b.on_receive(p2, TICK + 1)};
	CHECK(r2.status == receive_status::accepted && r2.reliable.empty());
	/* The datagram buffer is reused before packet 1 arrives. */
	std::ranges::fill(p2, std::uint8_t{0xff});
	const auto r1{b.on_receive(p1, TICK + 2)};
	CHECK(r1.status == receive_status::accepted);
	CHECK_MSG(r1.reliable.size() == 2, "delivered " + std::to_string(r1.reliable.size()));
	CHECK(r1.reliable[0].type == 1 && std::ranges::equal(r1.reliable[0].payload, x1));
	CHECK(r1.reliable[1].type == 2 && std::ranges::equal(r1.reliable[1].payload, x2));
	/* Nothing held any more: the next message is a view into its own
	 * datagram.
	 */
	const std::array<std::uint8_t, 6> x3{{3, 3, 3, 3, 3, 3}};
	CHECK(a.enqueue_reliable(3, x3) == enqueue_result::ok);
	const datagram p3{[&] { const auto p{a.build_outgoing(2 * TICK)}; return datagram{p.begin(), p.end()}; }()};
	const auto r3{b.on_receive(p3, 2 * TICK + 1)};
	CHECK(r3.status == receive_status::accepted && r3.reliable.size() == 1 && std::ranges::equal(r3.reliable[0].payload, x3));
	CHECK(r3.reliable[0].payload.data() >= p3.data() && r3.reliable[0].payload.data() < p3.data() + p3.size());
	std::printf("    message 2 held, its datagram overwritten, then message 1: both delivered intact; message 3, nothing held, is a view into its datagram\n");
}

/* 6. to_peer_time is a wire timestamp: the offset is known modulo 2^32,
 * so a peer whose clock is 2^32 units ahead is still read correctly
 * against its stamps.
 */
void test_to_peer_time_wraps(const std::uint64_t seed)
{
	begin("to_peer_time modulo 2^32");
	rng r{seed};
	const net_clock bias{(net_clock{1} << 32) + net_seconds(3)};
	sim_world w{r, link_params{.latency = net_milliseconds(30)}, {{0, bias}}};
	w.run(180, [&](const unsigned i) { w.set_state(i); });
	CHECK(w.peers[1].conn.stats().clock_offset_valid);
	const auto client_view{w.peers[1].conn.to_peer_time(w.peers[1].clock(w.now))};
	const auto host_stamp{to_net_time(w.peers[0].clock(w.now))};
	const auto err{net_time_diff(client_view, host_stamp)};
	CHECK_MSG((err < 0 ? -err : err) < net_milliseconds(5), "peer time error " + std::to_string(err) + " units");
	std::printf("    client clock 2^32 + 3 s ahead of the host: to_peer_time within %.2f ms of the host's stamp\n", to_ms(err));
}

/* Twelfth review round. */

/* 1. The peer's newest packet echoing a packet of ours older than the one
 * last echoed is no measurement: a conforming peer echoes the newest
 * packet it received.  A 60 pps host against a 10 pps client leaves five
 * packets in six unechoed; a hostile client naming a seconds-old one with
 * echo_delay 0 must not move srtt (nor rttvar, nor the clock).
 */
void test_echo_of_old_unechoed_packet()
{
	begin("echo of an old unechoed packet");
	connection_config hc{host_side}, cc{client_side};
	hc.peer_tick = {net_seconds(1), 10};
	cc.tick = {net_seconds(1), 10};
	connection a{hc, 0};
	connection b{cc, 0};
	const std::array<std::uint8_t, 8> x{};
	const net_clock latency{net_milliseconds(30)};
	std::vector<net_clock> sent_at;
	std::uint16_t client_seq{};
	/* 4 s: the host sends every tick, the client every sixth. */
	for (unsigned i{}; i != 240; ++i)
	{
		const net_clock t{net_clock{i} * TICK};
		a.set_unreliable_state(chunk_type::state, x);
		const auto p{a.build_outgoing(t)};
		CHECK(!p.empty());
		CHECK(packet_header::read(p)->seq == sent_at.size() + 1);
		sent_at.push_back(t);
		CHECK(b.on_receive(p, t + latency).status == receive_status::accepted);
		if (i % 6 == 5)
		{
			b.set_unreliable_state(chunk_type::input, x);
			const auto q{b.build_outgoing(t + latency + 100)};
			CHECK(!q.empty());
			client_seq = packet_header::read(q)->seq;
			CHECK(a.on_receive(q, t + 2 * latency + 100).status == receive_status::accepted);
		}
	}
	const auto before{a.stats()};
	CHECK_MSG(before.rtt_valid && before.srtt == 2 * latency, "srtt " + std::to_string(before.srtt));
	/* Packet 61 went out 3 s ago and was never echoed (the client echoed
	 * 60 and 66).  A forged newest client packet names it with its true
	 * send_time and echo_delay 0.
	 */
	const std::uint16_t old_seq{61};
	CHECK((old_seq - 1) % 6 != 5);
	const net_clock now{net_clock{240} * TICK};
	packet_header h;
	h.session_id = host_side.session_id;
	h.peer_token = host_side.peer_token;
	h.player_id = host_side.remote_player_id;
	h.flags = static_cast<std::uint8_t>(packet_flag::keepalive);
	h.seq = static_cast<std::uint16_t>(client_seq + 1);
	h.send_time = to_net_time(now);
	h.ack = old_seq;
	h.ack_bits = 0;
	h.echo_time = to_net_time(sent_at[old_seq - 1]);
	h.echo_delay = 0;
	std::array<std::uint8_t, NET_V2_HEADER_SIZE> d{};
	h.write(d.data());
	CHECK(a.on_receive(d, now).status == receive_status::accepted);
	const auto after{a.stats()};
	CHECK_MSG(after.srtt == before.srtt, "srtt " + std::to_string(to_ms(after.srtt)) + " ms after the forged echo, was " + std::to_string(to_ms(before.srtt)));
	CHECK_MSG(after.rttvar == before.rttvar, "rttvar " + std::to_string(after.rttvar) + " vs " + std::to_string(before.rttvar));
	CHECK(after.clock_offset_target == before.clock_offset_target);
	std::printf("    60 pps host, 10 pps client: a forged echo of the 3 s old unechoed packet 61 leaves srtt at %.1f ms\n", to_ms(after.srtt));
}

/* 2. A bound (a delayed round trip reported by a late ack) is covered by
 * the RTO for as long as bounds keep coming and for a second after the
 * last, however many samples rttvar has had to forget it; then it fades
 * and the RTO returns to its steady value.  srtt never moves on a bound.
 */
void test_bound_excess()
{
	begin("bound excess in the RTO");
	rtt_estimator e;
	const net_clock hold{NET_V2_DEFAULT_TICK_PERIOD}, slack{2 * hold};
	const net_clock rtt{net_milliseconds(80)}, held{net_milliseconds(130)};
	for (unsigned i{}; i != 100; ++i)
		e.add_sample(rtt);
	const auto steady{e.rto()};
	CHECK_MSG(steady == rtt + 4 * (hold / 4) + slack, "steady rto " + std::to_string(steady));
	e.add_bound(held);
	CHECK_MSG(e.rto() >= held + slack, "rto " + std::to_string(e.rto()) + " right after the bound");
	/* 100 samples later rttvar has forgotten the bound; the excess has
	 * not, and srtt has not moved.
	 */
	for (unsigned i{}; i != 100; ++i)
		e.add_sample(rtt);
	CHECK_MSG(e.rto() >= held + slack, "rto " + std::to_string(e.rto()) + " 100 samples after the bound");
	/* Two seconds of smaller bounds every 10 ticks, the last on the last
	 * tick: the largest stays.
	 */
	for (unsigned i{}; i != 120; ++i)
	{
		e.advance_ticks(1);
		e.add_sample(rtt);
		if (i % 10 == 9)
			e.add_bound(net_milliseconds(100));
	}
	CHECK_MSG(e.rto() >= held + slack, "rto " + std::to_string(e.rto()) + " while smaller bounds keep coming");
	/* Just short of a second after the last bound: still covered. */
	for (unsigned i{}; i != 59; ++i)
	{
		e.advance_ticks(1);
		e.add_sample(rtt);
	}
	CHECK_MSG(e.rto() >= held + slack, "rto " + std::to_string(e.rto()) + " a second after the last bound");
	/* A bound never shrinks rttvar: after wide samples a bound at srtt
	 * (no error at all) leaves rttvar where it was.
	 */
	rtt_estimator wide;
	for (unsigned i{}; i != 20; ++i)
		wide.add_sample(i % 2 ? net_milliseconds(60) : net_milliseconds(100));
	const auto rttvar_before{wide.rttvar()};
	CHECK(rttvar_before > hold / 4);
	wide.add_bound(wide.srtt());
	CHECK_MSG(wide.rttvar() == rttvar_before, "rttvar " + std::to_string(wide.rttvar()) + " after a bound at srtt, was " + std::to_string(rttvar_before));
	/* Two seconds more: faded, back to the steady value. */
	for (unsigned i{}; i != 120; ++i)
	{
		e.advance_ticks(1);
		e.add_sample(rtt);
	}
	CHECK_MSG(e.rto() == steady, "rto " + std::to_string(e.rto()) + " after the excess faded, steady " + std::to_string(steady));
	std::printf("    80 ms samples, a 130 ms bound: rto covers it through 100 samples and a second of quiet, then returns to %.1f ms\n", to_ms(steady));
}

/* 4. Parts set after the tick's first packet still fit the tick: the
 * packet budget follows the bundle plan on every packet, not only on the
 * tick's first.
 */
void test_parts_after_first_packet()
{
	begin("parts set after the tick's first packet");
	connection a{host_side, 0};
	connection b{client_side, 0};
	const datagram big(NET_V2_MAX_MESSAGE), small(40), part(900);
	CHECK(a.enqueue_reliable(9, big) == enqueue_result::ok);
	CHECK(a.enqueue_reliable(9, big) == enqueue_result::ok);
	a.set_unreliable_state(chunk_type::state, small);
	/* Packet 1: the state and the first message; the second is backlog. */
	const auto p1{a.build_outgoing(0)};
	CHECK(!p1.empty() && packet_header::read(p1)->has_flag(packet_flag::has_reliable));
	CHECK(b.on_receive(p1, 1).status == receive_status::accepted);
	/* A two-part bundle appears: two packets more than the budget of two
	 * allows for, and the message does not fit beside a part.
	 */
	a.set_unreliable_state(chunk_type::state, 0, 2, part);
	a.set_unreliable_state(chunk_type::state, 1, 2, part);
	unsigned parts{};
	for (unsigned k{}; k != 2; ++k)
	{
		const auto p{a.build_outgoing(0)};
		CHECK_MSG(!p.empty(), "packet " + std::to_string(k + 2) + " of the tick was not built");
		const auto report{b.on_receive(p, 1)};
		CHECK(report.status == receive_status::accepted);
		for (const auto &u : report.unreliable)
			if (u.type == chunk_type::state && u.part_count == 2)
				++parts;
	}
	CHECK_MSG(parts == 2, "parts delivered in the tick " + std::to_string(parts));
	/* The tick is spent; the second message waits for the next one. */
	CHECK(a.build_outgoing(0).empty());
	const auto p4{a.build_outgoing(TICK)};
	CHECK(!p4.empty() && packet_header::read(p4)->has_flag(packet_flag::has_reliable));
	CHECK(a.stats().unreliable_dropped == 0);
	std::printf("    state + message, then a 2 x 900-byte bundle: three packets in the tick, both parts delivered, the second message on the next tick\n");
}

/* Eleventh review round. */

/* 1. The documented frame-rate pattern works in any call order: update()
 * first, then begin_tick, still reports the tick.
 */
void test_frame_rate_caller_update_first()
{
	begin("500 Hz caller, update() before begin_tick");
	connection a{host_side, 0};
	connection b{client_side, 0};
	const std::array<std::uint8_t, 40> state{};
	unsigned ticks{}, states{}, doubles{};
	for (unsigned f{}; f != 5000; ++f)
	{
		const net_clock now{net_clock{f} * net_seconds(1) / 500};
		a.update(now);
		if (a.begin_tick(now))
		{
			++ticks;
			a.set_unreliable_state(chunk_type::state, state);
			/* Asking again in the same tick reports nothing. */
			if (a.begin_tick(now))
				++doubles;
		}
		for (;;)
		{
			const auto p{a.build_outgoing(now)};
			if (p.empty())
				break;
			const auto report{b.on_receive(p, now + 1)};
			CHECK(report.status == receive_status::accepted);
			states += static_cast<unsigned>(report.unreliable.size());
		}
		if (f % 8 == 7)
		{
			const auto ack{b.build_outgoing(now + 2)};
			if (!ack.empty())
				CHECK(a.on_receive(ack, now + 3).status == receive_status::accepted);
		}
	}
	CHECK_MSG(a.stats().unreliable_dropped == 0, "states dropped " + std::to_string(a.stats().unreliable_dropped));
	CHECK_MSG(ticks >= 599 && ticks <= 601, "ticks " + std::to_string(ticks));
	CHECK(states == ticks && doubles == 0);
	std::printf("    5000 frames with update() first: %u ticks reported, %u states delivered, none dropped, no double report\n", ticks, states);
}

/* 2. Events keep their room beside a standing reliable backlog. */
void test_event_beside_backlog(const std::uint64_t seed)
{
	begin("event beside a reliable backlog");
	rng r{seed};
	sim_world w{r, link_params{.latency = net_milliseconds(30)}};
	const datagram big(900);
	std::uint32_t sent{};
	w.run(60, [&](const unsigned i) {
		w.set_state(i);
		if (i == 0)
		{
			while (w.peers[0].conn.stats().queue_messages < 4)
			{
				CHECK(w.peers[0].conn.enqueue_reliable(9, big) == enqueue_result::ok);
				w.peers[0].sent.push_back({.type = 9, .payload = big});
			}
			datagram e(300);
			net_put_le32(e.data(), sent++);
			CHECK(w.peers[0].conn.send_unreliable(chunk_type::event_u, e));
		}
	});
	w.run(10, [&](const unsigned i) { w.set_state(i); });
	check_delivery(w, 0);
	const auto host{w.peers[0].conn.stats()};
	CHECK_MSG(host.unreliable_dropped == 0, "events dropped " + std::to_string(host.unreliable_dropped));
	CHECK_MSG(w.peers[1].events_seen.size() == 60, "events delivered " + std::to_string(w.peers[1].events_seen.size()) + " of 60");
	std::printf("    300-byte event per tick beside 900-byte reliable messages: 60 of 60 delivered, none dropped\n");
}

/* 3. A reordered older peer packet neither samples the RTT nor lets its
 * acks measure a packet: the peer->us reorder delay stays out of srtt.
 */
void test_late_peer_packet_no_sample()
{
	begin("late peer packet gives no sample");
	connection a{host_side, 0};
	connection b{client_side, 0};
	const std::array<std::uint8_t, 2> x{};
	a.set_unreliable_state(chunk_type::state, x);
	const auto p1{a.build_outgoing(0)};
	CHECK(b.on_receive(p1, 1000).status == receive_status::accepted);
	b.set_unreliable_state(chunk_type::input, x);
	const datagram q1{[&] { const auto p{b.build_outgoing(1100)}; return datagram{p.begin(), p.end()}; }()};
	a.set_unreliable_state(chunk_type::state, x);
	const auto p2{a.build_outgoing(TICK)};
	CHECK(b.on_receive(p2, TICK + 1000).status == receive_status::accepted);
	b.set_unreliable_state(chunk_type::input, x);
	const datagram q2{[&] { const auto p{b.build_outgoing(TICK + 1100)}; return datagram{p.begin(), p.end()}; }()};
	/* Q2 arrives on time and measures packet 2: 2100 units. */
	CHECK(a.on_receive(q2, TICK + 2200).status == receive_status::accepted);
	const auto s1{a.stats()};
	CHECK_MSG(s1.rtt_valid && s1.srtt == 2100, "srtt " + std::to_string(s1.srtt));
	/* Q1 arrives 4 s late: accepted, its acks honoured, srtt untouched;
	 * its delay only widens rttvar (that is what a late ack costs).
	 */
	CHECK(a.on_receive(q1, TICK + 2200 + 4 * net_seconds(1)).status == receive_status::accepted);
	const auto s2{a.stats()};
	CHECK_MSG(s2.srtt == s1.srtt, "srtt " + std::to_string(s2.srtt) + " after the late packet");
	CHECK_MSG(s2.rttvar > s1.rttvar, "rttvar " + std::to_string(s2.rttvar) + " vs " + std::to_string(s1.rttvar));
	CHECK(s2.packets_acked == 2);
	std::printf("    Q2 on time (srtt 2100 units), Q1 four seconds late: srtt unchanged, rttvar widened, both acks honoured\n");
}

/* 4. A late ack of packets already given up as lost still acknowledges
 * their messages: they are not resent, and the loss estimate takes them
 * back.
 */
void test_late_ack_of_lost_packets()
{
	begin("late ack of packets marked lost");
	connection a{host_side, 0};
	connection b{client_side, 0};
	const std::array<std::uint8_t, 4> x{{1, 2, 3, 4}};
	/* 70 packets in 35 ticks (two 1 KiB messages per tick, one packet
	 * each), well inside the initial 1 s RTO, so only the late ack is
	 * under test.
	 */
	const datagram big(NET_V2_MAX_MESSAGE);
	std::vector<datagram> packets;
	for (unsigned i{}; i != 35; ++i)
	{
		CHECK(a.enqueue_reliable(1, big) == enqueue_result::ok);
		CHECK(a.enqueue_reliable(1, big) == enqueue_result::ok);
		for (unsigned k{}; k != 2; ++k)
		{
			const auto p{a.build_outgoing(net_clock{i + 1} * TICK)};
			CHECK(!p.empty());
			packets.emplace_back(p.begin(), p.end());
		}
	}
	const net_clock t{36 * TICK};
	/* The peer acks packets 1-5 first (A1), then 6-70 (A2). */
	for (unsigned i{}; i != 5; ++i)
		CHECK(b.on_receive(packets[i], t + i).status == receive_status::accepted);
	b.set_unreliable_state(chunk_type::input, x);
	const datagram a1{[&] { const auto p{b.build_outgoing(t + 10)}; return datagram{p.begin(), p.end()}; }()};
	for (unsigned i{5}; i != 70; ++i)
		CHECK(b.on_receive(packets[i], t + 20 + i).status == receive_status::accepted);
	b.set_unreliable_state(chunk_type::input, x);
	const datagram a2{[&] { const auto p{b.build_outgoing(t + TICK)}; return datagram{p.begin(), p.end()}; }()};
	/* A2 arrives first: packets 1-5 fall out of the bitfield and are
	 * given up, their messages flagged for retransmission.
	 */
	CHECK(a.on_receive(a2, t + TICK + 1).status == receive_status::accepted);
	auto s{a.stats()};
	CHECK_MSG(s.packets_lost == 5 && s.resends_by_gap == 5, "lost " + std::to_string(s.packets_lost) + ", gap resends flagged " + std::to_string(s.resends_by_gap));
	/* A1 arrives late, before the next build: the messages were
	 * delivered, so nothing is resent.
	 */
	CHECK(a.on_receive(a1, t + TICK + 2).status == receive_status::accepted);
	s = a.stats();
	CHECK_MSG(s.packets_lost == 0 && s.packets_acked == 70, "lost " + std::to_string(s.packets_lost) + ", acked " + std::to_string(s.packets_acked));
	CHECK(s.in_flight == 0 && s.queue_messages == 0);
	(void)a.build_outgoing(38 * TICK);
	CHECK(a.stats().message_resends == 0);
	CHECK(b.stats().messages_delivered == 70);
	std::printf("    acks of packets 1-5 arrive after those of 6-70: given up, then taken back, no message resent\n");
}

/* Tenth review round. */

/* 1. An event that does not fit beside the state chunk gets the tick's
 * second packet instead of being skipped to death.
 */
void test_event_beside_big_state(const std::uint64_t seed)
{
	begin("event beside a big state");
	rng r{seed};
	sim_world w{r, link_params{.latency = net_milliseconds(30)}};
	const datagram event(700);
	std::uint32_t sent{};
	w.run(100, [&](const unsigned i) {
		if (i == 0)
		{
			w.set_state_parts(0, 1, 700);
			datagram e{event};
			net_put_le32(e.data(), sent++);
			CHECK(w.peers[0].conn.send_unreliable(chunk_type::event_u, e));
		}
		else
			w.set_state(1);
	});
	w.run(5);
	const auto host{w.peers[0].conn.stats()};
	const auto &events{w.peers[1].events_seen};
	CHECK_MSG(host.unreliable_dropped == 0, "events dropped " + std::to_string(host.unreliable_dropped));
	CHECK_MSG(events.size() == 100, "events delivered " + std::to_string(events.size()) + " of 100");
	CHECK(w.peers[1].part_counts[0] == 100);
	std::printf("    700-byte state and 700-byte event every tick: %zu of 100 events delivered, none dropped\n", events.size());
}

/* 2. The gap rule counts acknowledged packets after the message's own,
 * not the distance to the highest ack: one reordered ack far ahead
 * acknowledges one packet and resends nothing.
 */
void test_gap_rule_counts_acks()
{
	begin("gap rule counts acknowledged packets");
	connection a{host_side, 0};
	connection b{client_side, 0};
	const std::array<std::uint8_t, 4> x{{1, 2, 3, 4}};
	std::vector<datagram> packets;
	for (unsigned i{}; i != 6; ++i)
	{
		CHECK(a.enqueue_reliable(static_cast<std::uint8_t>(i + 1), x) == enqueue_result::ok);
		const auto p{a.build_outgoing(net_clock{i + 1} * TICK)};
		CHECK(!p.empty());
		packets.emplace_back(p.begin(), p.end());
	}
	/* Only the sixth packet reaches the peer for now. */
	const net_clock t{7 * TICK};
	CHECK(b.on_receive(packets[5], t).status == receive_status::accepted);
	b.set_unreliable_state(chunk_type::input, x);
	CHECK(a.on_receive(b.build_outgoing(t + 1), t + 2).status == receive_status::accepted);
	auto s{a.stats()};
	CHECK_MSG(s.resends_by_gap == 0, "gap resends after a lone ack of packet 6: " + std::to_string(s.resends_by_gap));
	CHECK(s.in_flight == 5);
	/* Now the rest arrives; nothing was ever resent. */
	for (unsigned i{}; i != 5; ++i)
		CHECK(b.on_receive(packets[i], t + 3 + i).status == receive_status::accepted);
	b.set_unreliable_state(chunk_type::input, x);
	/* A tick later: the peer's budget for this tick is spent. */
	CHECK(a.on_receive(b.build_outgoing(t + TICK), t + TICK + 1).status == receive_status::accepted);
	s = a.stats();
	CHECK(s.in_flight == 0 && s.message_resends == 0 && s.resends_by_gap == 0);
	CHECK(b.stats().messages_delivered == 6);
	/* And a genuine gap still fires: packets 1..3 of a fresh set lost,
	 * 4..6 acknowledged.
	 */
	connection c{host_side, 0};
	connection d{client_side, 0};
	std::vector<datagram> second;
	for (unsigned i{}; i != 6; ++i)
	{
		CHECK(c.enqueue_reliable(1, x) == enqueue_result::ok);
		const auto p{c.build_outgoing(net_clock{i + 1} * TICK)};
		second.emplace_back(p.begin(), p.end());
	}
	for (unsigned i{3}; i != 6; ++i)
		CHECK(d.on_receive(second[i], t + i).status == receive_status::accepted);
	d.set_unreliable_state(chunk_type::input, x);
	CHECK(c.on_receive(d.build_outgoing(t + 20), t + 21).status == receive_status::accepted);
	CHECK_MSG(c.stats().resends_by_gap == 3, "gap resends with three later packets acked: " + std::to_string(c.stats().resends_by_gap));
	std::printf("    lone ack of packet 6: 0 gap resends; packets 4-6 acked, 1-3 lost: 3 gap resends\n");
}

/* 3. A rejected sample (hostile echo_delay beyond the round trip) does
 * not spend the packet's one sample: the real echo still counts.
 */
void test_rejected_sample_keeps_packet()
{
	begin("rejected sample keeps the packet's sample");
	connection a{host_side, 0};
	connection b{client_side, 0};
	const std::array<std::uint8_t, 2> x{};
	a.set_unreliable_state(chunk_type::state, x);
	const auto p1{a.build_outgoing(0)};
	/* b's first sequence goes to waste, so a forged seq 1 stays unique. */
	b.set_unreliable_state(chunk_type::input, x);
	CHECK(!b.build_outgoing(500).empty());
	CHECK(b.on_receive(p1, 1000).status == receive_status::accepted);
	packet_header h;
	h.session_id = client_side.session_id;
	h.peer_token = client_side.peer_token;
	h.player_id = client_side.local_player_id;
	h.flags = static_cast<std::uint8_t>(packet_flag::keepalive);
	h.seq = 1;
	h.ack = 1;
	h.echo_time = 0;	/* p1's true send_time */
	h.echo_delay = 0xfffe;	/* far beyond the round trip: rtt < 0 */
	std::array<std::uint8_t, NET_V2_HEADER_SIZE> d{};
	h.write(d.data());
	CHECK(a.on_receive(d, 2000).status == receive_status::accepted);
	CHECK(!a.stats().rtt_valid);
	/* The genuine echo of packet 1 follows and is taken. */
	b.set_unreliable_state(chunk_type::input, x);
	const auto pb{b.build_outgoing(2500)};
	CHECK(packet_header::read(pb)->ack == 1 && packet_header::read(pb)->echo_delay == 1500);
	CHECK(a.on_receive(pb, 3000).status == receive_status::accepted);
	const auto s{a.stats()};
	CHECK_MSG(s.rtt_valid && s.srtt == 1500, "srtt " + std::to_string(s.srtt) + " units, expected 1500");
	std::printf("    hostile echo_delay rejected, the real echo of the same packet still sampled (1500 units)\n");
}

/* 4. The peer's tick can be set on a live connection; the RTO's hold
 * term follows.
 */
void test_set_peer_tick()
{
	begin("set_peer_tick on a live connection");
	connection a{host_side, 0};
	connection b{client_side, 0};
	const std::array<std::uint8_t, 2> x{};
	a.set_unreliable_state(chunk_type::state, x);
	CHECK(b.on_receive(a.build_outgoing(0), 500).status == receive_status::accepted);
	b.set_unreliable_state(chunk_type::input, x);
	CHECK(a.on_receive(b.build_outgoing(600), 1000).status == receive_status::accepted);
	const auto before{a.stats()};
	CHECK(before.rtt_valid && before.rto > NET_V2_RTO_MIN && before.rto < NET_V2_RTO_MAX);
	a.set_peer_tick({net_seconds(1), 10});
	const auto after{a.stats()};
	const auto expected{tick_period{net_seconds(1), 10}.units() - NET_V2_DEFAULT_TICK.units()};
	CHECK_MSG(after.rto - before.rto == expected, "rto grew by " + std::to_string(to_ms(after.rto - before.rto)) + " ms, expected " + std::to_string(to_ms(expected)));
	CHECK(after.srtt == before.srtt);
	CHECK(a.config().peer_tick.denominator == 10);
	/* Zero terms mean "same as our tick". */
	a.set_peer_tick({0, 0});
	CHECK(a.stats().rto == before.rto);
	std::printf("    peer tick 1/60 -> 1/10 s: rto +%.1f ms, srtt unchanged; back to own tick restores it\n", to_ms(expected));
}

/* Ninth review round. */

/* 1a. A peer that sends far fewer packets than we do (keepalives at
 * 10 Hz against our 60 Hz) must not inflate our srtt with its ack hold.
 */
void test_slow_peer_srtt(const std::uint64_t seed)
{
	begin("slow peer, srtt unbiased");
	rng r{seed};
	connection_config h{host_side}, c{client_side};
	h.peer_tick = {net_seconds(1), 10};
	c.tick = {net_seconds(1), 10};
	c.peer_tick = {net_seconds(1), 60};
	sim_world w{r, link_params{.latency = net_milliseconds(60)}, {}, {{h, c}}};
	w.tick_every = {{1, 6}};
	w.run(600, [&](const unsigned i) { w.set_state(i); });
	const auto s{w.peers[0].conn.stats()};
	const auto expected{2 * net_milliseconds(60)};
	const auto err{s.srtt > expected ? s.srtt - expected : expected - s.srtt};
	CHECK_MSG(err * 20 < expected, "host srtt " + std::to_string(to_ms(s.srtt)) + " ms vs " + std::to_string(to_ms(expected)) + " ms");
	CHECK(s.state == connection_state::connected && s.message_resends == 0);
	std::printf("    60 Hz host, 10 Hz peer, 60 ms link: host srtt %.1f ms (rto %.1f ms)\n", to_ms(s.srtt), to_ms(s.rto));
}

/* 1b. Two packets per tick against a one-packet peer: the unechoed one
 * of each pair is not measured by its ack.
 */
void test_two_packets_per_tick_srtt(const std::uint64_t seed)
{
	begin("two packets per tick, srtt unbiased");
	rng r{seed};
	sim_world w{r, link_params{.latency = net_milliseconds(30)}};
	const datagram big(NET_V2_MAX_MESSAGE);
	w.run(600, [&](const unsigned i) {
		if (i == 0)
		{
			/* A state too big to share a packet with a 1 KiB message. */
			w.set_state_parts(0, 1, 300);
			if (w.peers[0].conn.stats().queue_messages < 4)
			{
				CHECK(w.peers[0].conn.enqueue_reliable(9, big) == enqueue_result::ok);
				w.peers[0].sent.push_back({.type = 9, .payload = big});
			}
		}
		else
			w.set_state(1);
	});
	const auto s{w.peers[0].conn.stats()};
	const auto expected{2 * net_milliseconds(30)};
	const auto err{s.srtt > expected ? s.srtt - expected : expected - s.srtt};
	CHECK_MSG(err * 20 < expected, "host srtt " + std::to_string(to_ms(s.srtt)) + " ms vs " + std::to_string(to_ms(expected)) + " ms");
	CHECK_MSG(s.packets_sent >= 1100, "packets " + std::to_string(s.packets_sent));
	std::printf("    %llu host packets in 600 ticks: srtt %.1f ms\n", static_cast<unsigned long long>(s.packets_sent), to_ms(s.srtt));
}

/* 2. A two-part bundle with a standing reliable backlog: both parts every
 * tick, the messages drain, at most three packets per tick.
 */
void test_bundle_with_backlog(const std::uint64_t seed)
{
	begin("two-part bundle with a standing backlog");
	rng r{seed};
	sim_world w{r, link_params{.latency = net_milliseconds(30)}};
	const datagram big(NET_V2_MAX_MESSAGE);
	std::uint64_t worst{};
	auto before{w.peers[0].conn.stats().packets_sent};
	w.run(200, [&](const unsigned i) {
		if (i == 0)
		{
			w.set_state_parts(0, 2, 900);
			CHECK(w.peers[0].conn.enqueue_reliable(9, big) == enqueue_result::ok);
			w.peers[0].sent.push_back({.type = 9, .payload = big});
		}
		else
		{
			w.set_state(1);
			const auto sent{w.peers[0].conn.stats().packets_sent};
			worst = std::max(worst, sent - before);
			before = sent;
		}
	});
	/* Drain without new parts, so that the last ones arrive. */
	w.run(10);
	check_delivery(w, 0);
	const auto host{w.peers[0].conn.stats()};
	const auto &counts{w.peers[1].part_counts};
	CHECK_MSG(host.unreliable_dropped == 0, "parts dropped " + std::to_string(host.unreliable_dropped));
	CHECK_MSG(counts[0] == 200 && counts[1] == 200, "parts delivered: " + std::to_string(counts[0]) + " / " + std::to_string(counts[1]));
	CHECK_MSG(worst == 3, "most packets in one tick " + std::to_string(worst));
	std::printf("    2 x 900-byte parts and a 1 KiB message every tick: parts %u/%u, 200 messages delivered, %llu packets per tick at most\n", counts[0], counts[1], static_cast<unsigned long long>(worst));
}

/* 3. A game loop at 500 fps sets the state only when begin_tick opened a
 * tick, so no state is ever replaced before it was sent.
 */
void test_frame_rate_caller()
{
	begin("500 Hz caller with begin_tick");
	connection a{host_side, 0};
	connection b{client_side, 0};
	const std::array<std::uint8_t, 40> state{};
	unsigned ticks{}, states{};
	for (unsigned f{}; f != 5000; ++f)
	{
		const net_clock now{net_clock{f} * net_seconds(1) / 500};
		if (a.begin_tick(now))
		{
			++ticks;
			a.set_unreliable_state(chunk_type::state, state);
		}
		for (;;)
		{
			const auto p{a.build_outgoing(now)};
			if (p.empty())
				break;
			const auto report{b.on_receive(p, now + 1)};
			CHECK(report.status == receive_status::accepted);
			states += static_cast<unsigned>(report.unreliable.size());
		}
		if (f % 8 == 7)
		{
			const auto ack{b.build_outgoing(now + 2)};
			if (!ack.empty())
				CHECK(a.on_receive(ack, now + 3).status == receive_status::accepted);
		}
	}
	CHECK_MSG(a.stats().unreliable_dropped == 0, "states dropped " + std::to_string(a.stats().unreliable_dropped));
	CHECK_MSG(ticks >= 599 && ticks <= 601, "ticks " + std::to_string(ticks));
	CHECK(states == ticks);
	std::printf("    5000 frames, %u ticks opened, %u states delivered, none dropped\n", ticks, states);
}

/* Eighth review round. */

/* 1. Packets held back (not lost) must not be resent by the RTO: acked
 * only after later packets were echoed, they are never echoed themselves,
 * and their acks bound rttvar once each.
 */
void test_held_packets(const std::uint64_t seed)
{
	begin("held-back packets");
	rng r{seed};
	sim_world w{r, link_params{.latency = net_milliseconds(40), .reorder = 0.20, .reorder_min = net_milliseconds(33), .reorder_delay = net_milliseconds(50)}};
	w.run(600, [&](const unsigned i) {
		w.set_state(i);
		if (i == 0)
			for (unsigned k{}; k != 2 && w.peers[0].sent.size() < 1200; ++k)
				w.enqueue_random_message(0, 60);
	});
	w.run(20, [&](const unsigned i) { w.set_state(i); });
	check_delivery(w, 0);
	const auto s{w.peers[0].conn.stats()};
	print_stats("host", s);
	CHECK_MSG(s.resends_by_rto <= 12, "RTO resends " + std::to_string(s.resends_by_rto) + " of 1200 messages");
	CHECK(s.rttvar >= w.peers[0].conn.config().tick.units() / 4);
	std::printf("    lossless 40 ms link, 20%% of packets held 33-50 ms: %llu of 1200 messages resent by RTO (%llu by the gap rule), srtt %.1f ms, rttvar %.1f ms\n",
		static_cast<unsigned long long>(s.resends_by_rto), static_cast<unsigned long long>(s.resends_by_gap), to_ms(s.srtt), to_ms(s.rttvar));
}

/* 3. A closed connection takes no unreliable data. */
void test_closed_refuses_unreliable()
{
	begin("closed connection refuses unreliable data");
	connection a{host_side, 0};
	a.close();
	const std::array<std::uint8_t, 4> x{{1, 2, 3, 4}};
	CHECK(!a.send_unreliable(chunk_type::event_u, x));
	a.set_unreliable_state(chunk_type::state, x);
	a.set_unreliable_state(chunk_type::state, 1, 2, x);
	CHECK(a.enqueue_reliable(1, x) == enqueue_result::closed);
	CHECK(a.build_outgoing(TICK).empty());
	CHECK(a.stats().unreliable_dropped == 0);
	std::printf("    send_unreliable false, set_unreliable_state ignored, nothing built\n");
}

/* Seventh review round. */

/* 1. A two-part bundle (§3.8) leaves the transport every tick: the
 * second part opens the tick's second packet.
 */
void test_state_parts(const std::uint64_t seed)
{
	begin("two-part state bundle every tick");
	rng r{seed};
	sim_world w{r, link_params{.latency = net_milliseconds(30)}};
	w.run(200, [&](const unsigned i) {
		if (i == 0)
		{
			w.set_state_parts(0, 2, 900);
			if (w.tick_count % 3 == 0)
			{
				/* Messages that fit beside a part.  (A 1 KiB message
				 * beside 900-byte parts would make a part and the
				 * message alternate, by design.)
				 */
				datagram m(100, static_cast<std::uint8_t>(w.tick_count));
				CHECK(w.peers[0].conn.enqueue_reliable(7, m) == enqueue_result::ok);
				w.peers[0].sent.push_back({.type = 7, .payload = std::move(m)});
			}
		}
		else
			w.set_state(1);
	});
	w.run(5);
	check_delivery(w, 0);
	const auto host{w.peers[0].conn.stats()};
	const auto &counts{w.peers[1].part_counts};
	CHECK_MSG(host.unreliable_dropped == 0, "parts dropped " + std::to_string(host.unreliable_dropped));
	CHECK_MSG(counts[0] == 200 && counts[1] == 200, "parts delivered: " + std::to_string(counts[0]) + " / " + std::to_string(counts[1]));
	CHECK(host.packets_sent >= 400);
	std::printf("    2 x 900-byte parts per tick (plus messages): part 0 delivered %u times, part 1 %u times, none dropped\n", counts[0], counts[1]);
}

/* 1b. Latest-wins is keyed per part: a late part 0 with a lower seq than
 * the newest part 1 is applied; a stale part never overrides a newer one
 * of the same part.
 */
void test_state_parts_reorder()
{
	begin("state parts reordered");
	connection a{host_side, 0};
	connection b{client_side, 0};
	const auto build_pair{[&](const net_clock t, const std::uint8_t tag) {
		datagram part(900, tag);
		a.set_unreliable_state(chunk_type::state, 0, 2, part);
		part[0] = static_cast<std::uint8_t>(tag + 1);
		a.set_unreliable_state(chunk_type::state, 1, 2, part);
		const auto p0{a.build_outgoing(t)};
		CHECK(p0.size() == NET_V2_HEADER_SIZE + 3 + 1 + 900);
		const datagram d0{p0.begin(), p0.end()};
		const auto p1{a.build_outgoing(t)};
		CHECK(p1.size() == NET_V2_HEADER_SIZE + 3 + 1 + 900);
		CHECK(a.build_outgoing(t).empty());
		return std::pair{d0, datagram{p1.begin(), p1.end()}};
	}};
	const auto [a0, b0]{build_pair(0, 10)};
	/* Part 1 (the later packet) first, then part 0: both applied. */
	auto r{b.on_receive(b0, 100)};
	CHECK(r.status == receive_status::accepted && r.unreliable.size() == 1 && r.unreliable[0].part == 1 && r.unreliable[0].part_count == 2 && r.unreliable[0].payload.size() == 900 && r.unreliable[0].payload[0] == 11);
	r = b.on_receive(a0, 101);
	CHECK(r.status == receive_status::accepted && r.unreliable.size() == 1 && r.unreliable[0].part == 0 && r.unreliable[0].payload[0] == 10);
	/* Two more ticks; deliver A2, B1, A1, B2: the old part 0 (A1) is
	 * stale behind A2 and dropped, the old part 1 (B1) is still the
	 * newest of its part and applied.
	 */
	const auto [a1, b1]{build_pair(TICK, 20)};
	const auto [a2, b2]{build_pair(2 * TICK, 30)};
	r = b.on_receive(a2, 200);
	CHECK(r.unreliable.size() == 1 && r.unreliable[0].part == 0 && r.unreliable[0].payload[0] == 30);
	r = b.on_receive(b1, 201);
	CHECK(r.unreliable.size() == 1 && r.unreliable[0].part == 1 && r.unreliable[0].payload[0] == 21);
	r = b.on_receive(a1, 202);
	CHECK(r.status == receive_status::accepted && r.unreliable.empty());
	r = b.on_receive(b2, 203);
	CHECK(r.unreliable.size() == 1 && r.unreliable[0].part == 1 && r.unreliable[0].payload[0] == 31);
	std::printf("    part 1 before part 0 of one tick: both applied; a stale part 0 dropped while an older part 1 still applies\n");
}

/* 4. The first grant is a single tick, however early the connection was
 * created.
 */
void test_first_grant_single()
{
	begin("first grant is one tick");
	connection a{host_side, 0};
	const datagram big(NET_V2_MAX_MESSAGE);
	for (unsigned i{}; i != 6; ++i)
		CHECK(a.enqueue_reliable(9, big) == enqueue_result::ok);
	unsigned n{};
	while (!a.build_outgoing(net_seconds(3)).empty())
		++n;
	CHECK_MSG(n == NET_V2_DEFAULT_MAX_PACKETS_PER_TICK, "first build 3 s after creation: " + std::to_string(n) + " packets");
	std::printf("    created at 0, first build at 3 s: %u packets, not a burst\n", n);
}

/* 5. A caller at exactly 60 Hz for ten minutes never gets a double grant
 * from a truncated period.
 */
void test_exact_caller_no_double_grant()
{
	begin("exact 60 Hz caller, no double grant");
	connection a{host_side, 0};
	connection b{client_side, 0};
	const datagram big(NET_V2_MAX_MESSAGE);
	std::uint64_t total{}, worst{};
	for (unsigned k{}; k != 36000; ++k)
	{
		const net_clock now{net_clock{k} * net_seconds(1) / 60};
		while (a.stats().queue_messages < 6)
			CHECK(a.enqueue_reliable(9, big) == enqueue_result::ok);
		unsigned n{};
		for (;;)
		{
			const auto p{a.build_outgoing(now)};
			if (p.empty())
				break;
			++n;
			CHECK(b.on_receive(p, now + 1).status == receive_status::accepted);
		}
		worst = std::max<std::uint64_t>(worst, n);
		total += n;
		const auto ack{b.build_outgoing(now + 2)};
		if (!ack.empty())
			CHECK(a.on_receive(ack, now + 3).status == receive_status::accepted);
	}
	CHECK_MSG(worst == NET_V2_DEFAULT_MAX_PACKETS_PER_TICK, "most packets in one step: " + std::to_string(worst));
	CHECK_MSG(total >= 2 * 36000 - 4, "packets in 10 min: " + std::to_string(total));
	std::printf("    36000 steps of exactly 1/60 s with a backlog: never more than %llu packets per step, %llu in all\n",
		static_cast<unsigned long long>(worst), static_cast<unsigned long long>(total));
}

/* Fifth review round. */

/* 1. Pinning one real old packet in echo_seq with echo_delay 0 forever
 * must not steer the estimators.
 */
void test_echo_pinning(const std::uint64_t seed)
{
	begin("echo pinning");
	rng r{seed};
	sim_world w{r, link_params{.latency = net_milliseconds(30)}};
	w.run(120, [&](const unsigned i) { w.set_state(i); });
	auto &a{w.peers[0].conn};
	const auto before{a.stats()};
	CHECK(before.rtt_valid);
	const auto latest_seq{static_cast<std::uint16_t>(before.packets_sent)};
	const auto pinned{static_cast<std::uint16_t>(latest_seq - 5)};
	const auto pinned_time{to_net_time(w.peers[0].clock(net_clock{pinned} * TICK))};
	std::uint16_t forged_seq{3000};
	const auto forge{[&](const std::uint16_t ack, const net_clock at) {
		packet_header h;
		h.session_id = host_side.session_id;
		h.peer_token = host_side.peer_token;
		h.player_id = host_side.remote_player_id;
		h.flags = static_cast<std::uint8_t>(packet_flag::keepalive);
		h.seq = ++forged_seq;
		h.send_time = to_net_time(at);
		h.ack = ack;
		h.echo_time = pinned_time;
		h.echo_delay = 0;
		std::array<std::uint8_t, NET_V2_HEADER_SIZE> d{};
		h.write(d.data());
		CHECK(a.on_receive(d, at).status == receive_status::accepted);
	}};
	/* Pinned for 5 s of "growing" RTT: real seq, true time, delay 0. */
	for (unsigned i{}; i != 300; ++i)
	{
		const auto at{w.peers[0].clock(w.now) + net_clock{i + 1} * TICK};
		forge(pinned, at);
		/* Also the variant where ack moves on but the echo time stays
		 * (the ack names a packet long acked and its send_time does not
		 * match, so it measures nothing either).
		 */
		forge(static_cast<std::uint16_t>(pinned + 1), at);
	}
	const auto after{a.stats()};
	CHECK_MSG(after.srtt == before.srtt && after.rttvar == before.rttvar, "srtt " + std::to_string(to_ms(after.srtt)) + " ms vs " + std::to_string(to_ms(before.srtt)));
	CHECK(after.clock_offset_target == before.clock_offset_target);
	CHECK(after.state == connection_state::connected);
	std::printf("    600 packets pinning one old echo over 5 s: srtt stays %.1f ms, offset target unchanged\n", to_ms(after.srtt));
}

/* 2a. The acks of a corrupt datagram are not applied: a lost packet
 * whose message a corrupt packet falsely acks is still retransmitted.
 */
void test_corrupt_ack_not_applied(const std::uint64_t seed)
{
	begin("corrupt datagram's acks not applied");
	rng r{seed};
	sim_world w{r, link_params{.latency = net_milliseconds(40)}};
	w.run(10, [&](const unsigned i) { w.set_state(i); });
	std::optional<std::uint16_t> dropped_seq;
	bool forged{};
	w.link.mangle[1] = [&](datagram &d) {
		if (dropped_seq || d.size() < NET_V2_HEADER_SIZE)
			return;
		const auto h{*packet_header::read(d)};
		if (!h.has_flag(packet_flag::has_reliable))
			return;
		dropped_seq = h.seq;
		d.clear();	/* lost on the wire */
	};
	w.link.mangle[0] = [&](datagram &d) {
		if (!dropped_seq || forged || d.size() < NET_V2_HEADER_SIZE + NET_V2_CHUNK_HEADER_SIZE)
			return;
		/* The client's next packet: falsely ack the lost packet and
		 * corrupt its chunk so that it is detectably malformed.
		 */
		net_put_le16(d.data() + 14, *dropped_seq);
		d[NET_V2_HEADER_SIZE + 1] = 0xff;
		d[NET_V2_HEADER_SIZE + 2] = 0x0f;
		forged = true;
	};
	w.run(300, [&](const unsigned i) {
		w.set_state(i);
		if (i == 0)
			for (unsigned k{}; k != 2 && w.peers[0].sent.size() < 200; ++k)
				w.enqueue_random_message(0, 40);
	});
	CHECK(dropped_seq && forged);
	check_delivery(w, 0);
	const auto host{w.peers[0].conn.stats()};
	const auto client{w.peers[1].conn.stats()};
	CHECK_MSG(host.protocol_errors == 1, "host protocol errors " + std::to_string(host.protocol_errors));
	CHECK_MSG(host.message_resends >= 1, "the lost packet's messages must be resent");
	CHECK(host.state == connection_state::connected && client.state == connection_state::connected);
	CHECK(client.recv_window_pending == 0 && host.in_flight == 0);
	std::printf("    lost packet %u falsely acked by a corrupt datagram: %llu message(s) resent, all 200 delivered\n", *dropped_seq, static_cast<unsigned long long>(host.message_resends));
}

/* 2b. A window that never advances while the peer keeps talking closes
 * as stream_stalled, not as the peer's protocol error.
 */
void test_stream_stalled()
{
	begin("stream stalled");
	connection a{host_side, 0};
	connection b{client_side, 0};
	/* A hand-made packet with message 1 but never message 0. */
	packet_header h;
	h.session_id = host_side.session_id;
	h.peer_token = host_side.peer_token;
	h.player_id = host_side.local_player_id;
	h.flags = static_cast<std::uint8_t>(packet_flag::has_reliable);
	/* Just below the wrap, so that a's real packets (seq 1, 2, ...) are
	 * a plausible continuation (within NET_V2_MAX_SEQ_JUMP).
	 */
	h.seq = 65000;
	datagram d(NET_V2_HEADER_SIZE + NET_V2_CHUNK_HEADER_SIZE + NET_V2_RELIABLE_RUN_HEADER_SIZE + NET_V2_MESSAGE_HEADER_SIZE);
	h.write(d.data());
	auto *p{d.data() + NET_V2_HEADER_SIZE};
	chunk_header{.type = static_cast<std::uint8_t>(chunk_type::reliable), .length = NET_V2_RELIABLE_RUN_HEADER_SIZE + NET_V2_MESSAGE_HEADER_SIZE}.write(p);
	net_put_le16(p + 3, 1);
	p[5] = 1;
	p[6] = 7;
	net_put_le16(p + 7, 0);
	const auto r1{b.on_receive(d, TICK)};
	CHECK(r1.status == receive_status::accepted && r1.reliable.empty());
	CHECK(b.stats().recv_window_pending == 1);
	/* Both peers keep talking, but the gap is never filled. */
	net_clock t{TICK};
	while (b.state() != connection_state::closed)
	{
		t += TICK;
		CHECK_MSG(t < 12 * net_seconds(1), "did not close");
		a.set_unreliable_state(chunk_type::state, std::array<std::uint8_t, 1>{{1}});
		const auto pa{a.build_outgoing(t)};
		CHECK(!pa.empty());
		CHECK(b.on_receive(pa, t + 1).status == receive_status::accepted);
		b.set_unreliable_state(chunk_type::input, std::array<std::uint8_t, 1>{{2}});
		const auto pb{b.build_outgoing(t + 2)};
		if (!pb.empty())
			CHECK(a.on_receive(pb, t + 3).status == receive_status::accepted);
	}
	CHECK(a.state() == connection_state::connected);
	CHECK(b.closed_because() == close_reason::stream_stalled);
	CHECK_MSG(t >= NET_V2_UNACKED_TIMEOUT + TICK && t < NET_V2_UNACKED_TIMEOUT + 3 * TICK, "closed at " + std::to_string(to_ms(t)) + " ms");
	std::printf("    gap never filled while the peer keeps sending: stream_stalled after %.0f ms\n", to_ms(t - TICK));
}

/* 3a. An intact copy of a sequence whose corrupted copy came first is
 * accepted.
 */
void test_malformed_then_intact()
{
	begin("corrupted then intact copy");
	connection a{host_side, 0};
	connection b{client_side, 0};
	const std::array<std::uint8_t, 4> x{{1, 2, 3, 4}};
	CHECK(a.enqueue_reliable(5, x) == enqueue_result::ok);
	const datagram intact{[&] { const auto p{a.build_outgoing(TICK)}; return datagram{p.begin(), p.end()}; }()};
	datagram corrupt{intact};
	corrupt[NET_V2_HEADER_SIZE + 1] = 0xff;
	corrupt[NET_V2_HEADER_SIZE + 2] = 0x0f;
	CHECK(b.on_receive(corrupt, TICK + 1).status == receive_status::malformed_chunk);
	const auto r{b.on_receive(intact, TICK + 2)};
	CHECK(r.status == receive_status::accepted);
	CHECK(r.reliable.size() == 1 && r.reliable[0].type == 5 && std::ranges::equal(r.reliable[0].payload, x));
	CHECK(b.stats().protocol_errors == 1);
	std::printf("    corrupted copy counted once, the intact copy delivered\n");
}

/* 3b. A corrupted seq byte pointing at a future sequence does not
 * blackhole the real packet with that sequence.
 */
void test_corrupted_future_seq()
{
	begin("corrupted future seq");
	connection a{host_side, 0};
	connection b{client_side, 0};
	const std::array<std::uint8_t, 2> x{};
	a.set_unreliable_state(chunk_type::state, x);
	datagram corrupt{[&] { const auto p{a.build_outgoing(TICK)}; return datagram{p.begin(), p.end()}; }()};
	net_put_le16(corrupt.data() + 12, 3);	/* seq 1 read as 3 */
	corrupt[NET_V2_HEADER_SIZE + 1] = 0xff;
	CHECK(b.on_receive(corrupt, TICK + 1).status == receive_status::malformed_chunk);
	for (std::uint16_t s{2}; s != 5; ++s)
	{
		a.set_unreliable_state(chunk_type::state, x);
		const auto p{a.build_outgoing(net_clock{s} * TICK)};
		CHECK(packet_header::read(p)->seq == s);
		CHECK_MSG(b.on_receive(p, net_clock{s} * TICK + 1).status == receive_status::accepted, "real packet " + std::to_string(s) + " rejected");
	}
	std::printf("    corrupt datagram claiming seq 3: the real packets 2, 3 and 4 all accepted\n");
}

/* 4. A head message that does not fit beside the state chunk rides the
 * tick's second packet; the state is never displaced from the first, and
 * the budget is at least two packets per tick.
 */
void test_big_message_rides_second_packet()
{
	begin("big head message rides the second packet");
	connection_config h{host_side};
	h.max_packets_per_tick = 1;
	connection a{h, 0};
	CHECK(a.config().max_packets_per_tick == 2);
	const datagram big(NET_V2_MAX_MESSAGE), state(1100);
	CHECK(a.enqueue_reliable(9, big) == enqueue_result::ok);
	a.set_unreliable_state(chunk_type::state, state);
	/* Built at the construction time, so exactly one tick is granted. */
	const auto p1{a.build_outgoing(0)};
	CHECK_MSG(p1.size() == NET_V2_HEADER_SIZE + 3 + NET_V2_STATE_PART_HEADER_SIZE + 1100, "first packet " + std::to_string(p1.size()) + " bytes");
	CHECK(!packet_header::read(p1)->has_flag(packet_flag::has_reliable));
	const auto p2{a.build_outgoing(0)};
	CHECK_MSG(p2.size() == NET_V2_HEADER_SIZE + 6 + 3 + NET_V2_MAX_MESSAGE, "second packet " + std::to_string(p2.size()) + " bytes");
	CHECK(packet_header::read(p2)->has_flag(packet_flag::has_reliable));
	CHECK(a.build_outgoing(0).empty());
	CHECK(a.stats().unreliable_dropped == 0);
	std::printf("    1 KiB message beside an 1100-byte state: state first, the message in the tick's second packet\n");
}

/* 4b. A stream of large reliable messages never displaces the per-tick
 * state chunk (regression: the round-5 omission starved it).
 */
void test_state_survives_bulk_transfer(const std::uint64_t seed)
{
	begin("state chunk survives a bulk transfer");
	rng r{seed};
	sim_world w{r, link_params{.latency = net_milliseconds(30)}};
	w.run(10, [&](const unsigned i) { w.set_state(i); });
	datagram big(900);
	for (unsigned k{}; k != 20; ++k)
	{
		big[0] = static_cast<std::uint8_t>(k);
		CHECK(w.peers[0].conn.enqueue_reliable(9, big) == enqueue_result::ok);
		w.peers[0].sent.push_back({.type = 9, .payload = big});
	}
	w.run(200, [&](const unsigned i) { w.set_state(i); });
	check_delivery(w, 0);
	const auto host{w.peers[0].conn.stats()};
	const auto &states{w.peers[1].states_seen};
	CHECK_MSG(host.unreliable_dropped == 0, "states dropped " + std::to_string(host.unreliable_dropped));
	CHECK_MSG(states.size() >= 207, "states delivered " + std::to_string(states.size()) + " of 210");
	CHECK(std::ranges::adjacent_find(states, std::greater_equal<>{}) == states.end());
	std::printf("    20 x 900-byte messages with a 350-byte state every tick: %zu of 210 states delivered, none dropped\n", states.size());
}

/* 2. The RTO covers the peer's ack hold, which is the peer's tick, not
 * ours: a 60 Hz host talking to a 30 Hz client never resends on a
 * jitter-free link.
 */
void test_peer_tick_period(const std::uint64_t seed)
{
	begin("peer tick period");
	rng r{seed};
	connection_config h{host_side}, c{client_side};
	h.peer_tick = {net_seconds(1), 30};
	c.tick = {net_seconds(1), 30};
	c.peer_tick = {net_seconds(1), 60};
	for (unsigned round{}; round != 4; ++round)
	{
		sim_world w{r, link_params{.latency = net_milliseconds(10 + 25 * round)}, {}, {{h, c}}};
		w.tick_every = {{1, 2}};
		w.phase = {{static_cast<net_clock>(r.below(TICK)), static_cast<net_clock>(r.below(TICK))}};
		w.run(400, [&](const unsigned i) {
			w.set_state(i);
			for (unsigned k{}; k != (i == 0 ? 2u : 1u) && w.peers[i].sent.size() < 400; ++k)
				w.enqueue_random_message(i, 60);
		});
		/* Let the last messages arrive. */
		w.run(12, [&](const unsigned i) { w.set_state(i); });
		for (unsigned i{}; i != 2; ++i)
		{
			check_delivery(w, i);
			const auto s{w.peers[i].conn.stats()};
			CHECK_MSG(s.message_resends == 0, "round " + std::to_string(round) + (i ? " client" : " host") + " resends " + std::to_string(s.message_resends));
		}
		const auto s{w.peers[0].conn.stats()};
		std::printf("    60 Hz host, 30 Hz client, latency %.0f ms: 0 resends, host srtt %.1f ms, rto %.1f ms\n", to_ms(w.link.params.latency), to_ms(s.srtt), to_ms(s.rto));
	}
}

/* Fourth review round: the coordinator's probe cases. */

/* 1. Latest-wins does not wedge after 32 768 packets without that
 * chunk type.
 */
void test_latest_wins_wrap()
{
	begin("latest-wins after a sequence wrap");
	connection_config h{host_side}, c{client_side};
	h.tick = {1, 1};
	c.tick = {1, 1};
	connection a{h, 0};
	connection b{c, 0};
	const std::array<std::uint8_t, 4> x{{1, 2, 3, 4}};
	net_clock t{1};
	a.set_unreliable_state(chunk_type::state, x);
	CHECK(b.on_receive(a.build_outgoing(t), t).unreliable.size() == 1);
	for (unsigned i{}; i != 33000; ++i)
	{
		++t;
		CHECK(a.send_unreliable(chunk_type::event_u, x));
		const auto p{a.build_outgoing(t)};
		CHECK(!p.empty());
		CHECK(b.on_receive(p, t).status == receive_status::accepted);
	}
	unsigned dropped{};
	for (unsigned i{}; i != 200; ++i)
	{
		++t;
		a.set_unreliable_state(chunk_type::state, x);
		const auto r{b.on_receive(a.build_outgoing(t), t)};
		CHECK(r.status == receive_status::accepted);
		if (r.unreliable.empty())
			++dropped;
	}
	CHECK_MSG(dropped == 0, "state chunks dropped after the wrap: " + std::to_string(dropped));
	/* And a genuinely reordered older state is still dropped. */
	a.set_unreliable_state(chunk_type::state, x);
	const datagram p1{[&] { const auto p{a.build_outgoing(++t)}; return datagram{p.begin(), p.end()}; }()};
	a.set_unreliable_state(chunk_type::state, x);
	const datagram p2{[&] { const auto p{a.build_outgoing(++t)}; return datagram{p.begin(), p.end()}; }()};
	CHECK(b.on_receive(p2, t).unreliable.size() == 1);
	CHECK(b.on_receive(p1, t).unreliable.empty());
	std::printf("    33000 event-only packets, then 200 states: all delivered; a reordered older state still dropped\n");
}

/* 2. A grant closes the tick that was still open. */
void test_grant_closes_tick()
{
	begin("grant closes the open tick");
	connection a{host_side, 0};
	const datagram big(NET_V2_MAX_MESSAGE);
	for (unsigned i{}; i != 6; ++i)
		CHECK(a.enqueue_reliable(9, big) == enqueue_result::ok);
	/* The caller builds once in tick 0 and stops. */
	CHECK(!a.build_outgoing(0).empty());
	unsigned n1{};
	while (!a.build_outgoing(TICK).empty())
		++n1;
	CHECK_MSG(n1 == NET_V2_DEFAULT_MAX_PACKETS_PER_TICK, "packets in tick 1: " + std::to_string(n1));
	std::printf("    one packet in tick 0, then %u (not 3) in tick 1\n", n1);
}

/* 3. An ack of a packet we never sent is rejected as a protocol error
 * and touches nothing.
 */
void test_hostile_ack()
{
	begin("hostile ack");
	connection a{host_side, 0};
	const std::array<std::uint8_t, 4> x{{1, 2, 3, 4}};
	for (unsigned i{}; i != 5; ++i)
	{
		CHECK(a.enqueue_reliable(1, x) == enqueue_result::ok);
		CHECK(!a.build_outgoing(net_clock{i + 1} * TICK).empty());
	}
	const auto before{a.stats()};
	packet_header h;
	h.session_id = host_side.session_id;
	h.peer_token = host_side.peer_token;
	h.player_id = host_side.remote_player_id;
	h.flags = static_cast<std::uint8_t>(packet_flag::keepalive);
	h.seq = 1;
	h.ack = 5000;
	std::array<std::uint8_t, NET_V2_HEADER_SIZE> d{};
	h.write(d.data());
	CHECK(a.on_receive(d, 10 * TICK).status == receive_status::bad_ack);
	const auto after{a.stats()};
	CHECK(after.packets_lost == before.packets_lost && after.resends_by_gap == before.resends_by_gap && after.packets_acked == before.packets_acked);
	CHECK(after.loss_estimate == before.loss_estimate);
	CHECK(after.protocol_errors == 1 && after.packets_received == before.packets_received);
	CHECK(after.state == connection_state::connecting);
	/* A replay of it is rejected the same way but not counted again. */
	CHECK(a.on_receive(d, 11 * TICK).status == receive_status::bad_ack);
	CHECK(a.stats().protocol_errors == 1);
	/* An ack of exactly the last packet we sent is fine. */
	h.seq = 2;
	h.ack = 5;
	h.write(d.data());
	CHECK(a.on_receive(d, 12 * TICK).status == receive_status::accepted);
	CHECK(a.stats().packets_acked == 1);
	std::printf("    ack 5000 with 5 packets sent: bad_ack, nothing resolved or resent; ack 5 accepted\n");
}

/* 4. An event that does not fit does not block the events behind it. */
void test_event_head_of_line()
{
	begin("event head-of-line");
	connection a{host_side, 0};
	const datagram huge(NET_V2_MAX_CHUNK_PAYLOAD), tiny(4), state(300);
	CHECK(a.send_unreliable(chunk_type::event_u, huge));
	for (unsigned i{}; i != 10; ++i)
		CHECK(a.send_unreliable(chunk_type::event_u, tiny));
	a.set_unreliable_state(chunk_type::state, state);
	const auto p0{a.build_outgoing(0)};
	CHECK_MSG(p0.size() == NET_V2_HEADER_SIZE + (3 + NET_V2_STATE_PART_HEADER_SIZE + 300) + 10 * (3 + 4), "first packet " + std::to_string(p0.size()) + " bytes");
	CHECK(a.stats().unreliable_dropped == 0);
	unsigned ticks_until_dropped{};
	for (unsigned t{1}; t != 20 && a.stats().unreliable_dropped == 0; ++t)
	{
		a.set_unreliable_state(chunk_type::state, state);
		CHECK(!a.build_outgoing(net_clock{t} * TICK).empty());
		ticks_until_dropped = t;
	}
	CHECK(a.stats().unreliable_dropped == 1);
	CHECK_MSG(ticks_until_dropped == NET_V2_EVENT_SKIP_MAX - 1, "dropped after " + std::to_string(ticks_until_dropped + 1) + " packets");
	/* Without a state chunk the huge event would have fitted. */
	CHECK(a.send_unreliable(chunk_type::event_u, huge));
	const auto p{a.build_outgoing(net_clock{30} * TICK)};
	CHECK(p.size() == NET_V2_MAX_PACKET);
	std::printf("    10 tiny events go out beside the state at once; the oversize one is dropped after %u packets, and fits alone\n", NET_V2_EVENT_SKIP_MAX);
}

/* 6. A zero tick period does not divide by zero. */
void test_zero_tick_period()
{
	begin("zero tick period");
	connection_config h{host_side};
	h.tick = {0, 0};
	h.max_packets_per_tick = 0;
	connection a{h, 0};
	CHECK(a.config().tick.numerator == 1 && a.config().tick.denominator == 1 && a.config().max_packets_per_tick == 2);
	a.set_unreliable_state(chunk_type::state, std::array<std::uint8_t, 1>{{1}});
	CHECK(!a.build_outgoing(1).empty());
	std::printf("    tick_period 0 and max_packets_per_tick 0 are clamped to 1 and 2\n");
}

/* Third review round. */

/* 1. Echo fields that do not name one of our packets with its true
 * send_time, or that go backwards, leave the estimators untouched.
 */
void test_echo_authentication(const std::uint64_t seed)
{
	begin("echo authentication");
	rng r{seed};
	sim_world w{r, link_params{.latency = net_milliseconds(30)}};
	w.run(120, [&](const unsigned i) { w.set_state(i); });
	const auto before{w.peers[0].conn.stats()};
	CHECK(before.rtt_valid && before.clock_offset_valid);
	auto &a{w.peers[0].conn};
	const auto now{w.peers[0].clock(w.now)};
	/* Forge client packets: valid header, new seq, hostile echo. */
	std::uint16_t forged_seq{2000};
	const auto forge{[&](const std::uint16_t echo_seq, const net_time echo_time, const std::uint16_t echo_delay) {
		packet_header h;
		h.session_id = host_side.session_id;
		h.peer_token = host_side.peer_token;
		h.player_id = host_side.remote_player_id;
		h.flags = static_cast<std::uint8_t>(packet_flag::keepalive);
		h.seq = ++forged_seq;
		h.send_time = to_net_time(now);
		h.ack = echo_seq;
		h.echo_time = echo_time;
		h.echo_delay = echo_delay;
		std::array<std::uint8_t, NET_V2_HEADER_SIZE> d{};
		h.write(d.data());
		CHECK(a.on_receive(d, now).status == receive_status::accepted);
	}};
	const auto latest_seq{static_cast<std::uint16_t>(before.packets_sent)};	/* seqs start at 1 and never skipped here */
	for (unsigned i{}; i != 19; ++i)
	{
		/* The probe: a real (and long acked) seq with a send_time 9 s in
		 * the past.
		 */
		forge(static_cast<std::uint16_t>(latest_seq - 6), to_net_time(now - net_seconds(9)), 0);
		/* A real seq, its true time, but named twice (see the pinning
		 * test for the full case).
		 */
		forge(static_cast<std::uint16_t>(latest_seq - 5), to_net_time(w.peers[0].clock(net_clock{latest_seq - 5} * TICK)), 0);
		/* Our very first packet, correct send_time, but older than the
		 * echo already seen.
		 */
		forge(1, to_net_time(w.peers[0].clock(TICK)), 0);
		/* ack 0 with a plausible time: no packet named. */
		forge(0, to_net_time(now - net_milliseconds(60)), 0);
	}
	const auto after{a.stats()};
	CHECK_MSG(after.srtt == before.srtt && after.rttvar == before.rttvar && after.rto == before.rto, "srtt " + std::to_string(to_ms(after.srtt)) + " ms vs " + std::to_string(to_ms(before.srtt)));
	CHECK(after.clock_offset_target == before.clock_offset_target);
	CHECK(after.state == connection_state::connected);
	CHECK(after.packets_received == before.packets_received + 19 * 4);
	std::printf("    76 forged echoes: srtt stays %.1f ms, offset target unchanged\n", to_ms(after.srtt));
}

/* 2. The tick origin advances by whole periods: a caller at another
 * rate gets one budget per period, and a frame spanning two ticks gets
 * both.
 */
void test_tick_credit()
{
	begin("tick credit");
	{
		connection a{host_side, 0};
		connection b{client_side, 0};
		const net_clock frame{net_milliseconds(10)};
		const std::array<std::uint8_t, 16> msg{};
		for (unsigned f{}; f != 1000; ++f)
		{
			const net_clock now{f * frame};
			CHECK(a.enqueue_reliable(1, msg) == enqueue_result::ok);
			for (;;)
			{
				const auto p{a.build_outgoing(now)};
				if (p.empty())
					break;
				CHECK(b.on_receive(p, now + 1).status == receive_status::accepted);
			}
			const auto ack{b.build_outgoing(now + 1)};
			if (!ack.empty())
				CHECK(a.on_receive(ack, now + 2).status == receive_status::accepted);
		}
		const auto sent{a.stats().packets_sent};
		CHECK_MSG(sent >= 595 && sent <= 605, "100 Hz caller sent " + std::to_string(sent) + " packets in 10 s");
		std::printf("    100 Hz caller, 60 Hz tick: %llu packets in 10 s\n", static_cast<unsigned long long>(sent));
	}
	{
		connection a{host_side, 0};
		const datagram big(NET_V2_MAX_MESSAGE);
		for (unsigned k{}; k != 6; ++k)
			CHECK(a.enqueue_reliable(9, big) == enqueue_result::ok);
		unsigned first{};
		while (!a.build_outgoing(0).empty())
			++first;
		CHECK(first == NET_V2_DEFAULT_MAX_PACKETS_PER_TICK);
		/* A hitch: the next frame comes two ticks later. */
		unsigned after_hitch{};
		while (!a.build_outgoing(2 * TICK).empty())
			++after_hitch;
		CHECK_MSG(after_hitch == 2 * NET_V2_DEFAULT_MAX_PACKETS_PER_TICK, "after a two-tick hitch: " + std::to_string(after_hitch));
		/* But a long stall does not release a burst beyond two ticks. */
		for (unsigned k{}; k != 10; ++k)
			CHECK(a.enqueue_reliable(9, big) == enqueue_result::ok);
		unsigned after_stall{};
		while (!a.build_outgoing(60 * TICK).empty())
			++after_stall;
		CHECK_MSG(after_stall == 2 * NET_V2_DEFAULT_MAX_PACKETS_PER_TICK, "after a 1 s stall: " + std::to_string(after_stall));
		std::printf("    hitch of two ticks: %u packets; stall of 60 ticks: %u packets\n", after_hitch, after_stall);
	}
}

/* 4. A reordered (older) packet does not move the echo fields, so the
 * peer's RTT sample is not inflated by the reorder delay.
 */
void test_reordered_echo()
{
	begin("reordered packet does not move the echo");
	connection a{host_side, 0};
	connection b{client_side, 0};
	const std::array<std::uint8_t, 2> x{};
	a.set_unreliable_state(chunk_type::state, x);
	const datagram p1{[&] { const auto p{a.build_outgoing(0)}; return datagram{p.begin(), p.end()}; }()};
	a.set_unreliable_state(chunk_type::state, x);
	const datagram p2{[&] { const auto p{a.build_outgoing(TICK)}; return datagram{p.begin(), p.end()}; }()};
	CHECK(b.on_receive(p2, TICK + 1000).status == receive_status::accepted);
	CHECK(b.on_receive(p1, TICK + 1500).status == receive_status::accepted);
	b.set_unreliable_state(chunk_type::input, x);
	const auto ack{b.build_outgoing(TICK + 2000)};
	const auto h{*packet_header::read(ack)};
	CHECK_MSG(h.ack == 2, "echoed (acked) seq " + std::to_string(h.ack));
	CHECK(h.echo_time == to_net_time(TICK) && h.echo_delay == 1000);
	CHECK(a.on_receive(ack, TICK + 3000).status == receive_status::accepted);
	const auto s{a.stats()};
	CHECK(s.rtt_valid);
	/* Packet 2's echo is the exact 2000.  Packet 1 is acked in the same
	 * peer packet, so nothing of ours had been echoed before it was
	 * acked: its ack is no bound either (that would carry the peer's
	 * hold), and srtt and rttvar stay exact.
	 */
	CHECK_MSG(s.srtt == 2000 && s.rttvar == 1000, "srtt " + std::to_string(s.srtt) + " units, expected 2000, rttvar " + std::to_string(s.rttvar));
	std::printf("    late packet 1 after packet 2: echo names 2, srtt exact (2000 units)\n");
}

/* 5. After a long ack blackout the loss scan is still in range: packets
 * that fall out of the bitfield are counted within 64 packets.
 */
void test_long_blackout_scan()
{
	begin("loss scan after a long blackout");
	connection a{host_side, 0};
	connection b{client_side, 0};
	const std::array<std::uint8_t, 2> x{};
	net_clock t{};
	const auto step{[&](const bool acks, const bool deliver) {
		t += TICK;
		a.set_unreliable_state(chunk_type::state, x);
		const auto pa{a.build_outgoing(t)};
		CHECK(!pa.empty());
		if (deliver)
			CHECK(b.on_receive(pa, t + 1).status == receive_status::accepted);
		b.set_unreliable_state(chunk_type::input, x);
		const auto pb{b.build_outgoing(t + 2)};
		CHECK(!pb.empty());
		datagram d{pb.begin(), pb.end()};
		if (!acks)
			std::fill(d.begin() + 14, d.begin() + 24, 0);
		CHECK(a.on_receive(d, t + 3).status == receive_status::accepted);
	}};
	for (unsigned i{}; i != 200; ++i)
		step(true, true);
	/* More than half the sequence space without a single ack. */
	for (unsigned i{}; i != 34000; ++i)
		step(false, true);
	for (unsigned i{}; i != 20; ++i)
		step(true, true);
	const auto lost_before{a.stats().packets_lost};
	/* Now every other packet of ours is lost while the peer's acks keep
	 * coming: the bitfield rule must count the lost ones within 64
	 * packets, long before their log slots are reused.
	 */
	for (unsigned i{}; i != 150; ++i)
		step(true, i % 2 == 0);
	const auto lost_after{a.stats().packets_lost};
	CHECK_MSG(lost_after - lost_before >= 30, "lost counted after the blackout: " + std::to_string(lost_after - lost_before));
	CHECK(a.state() == connection_state::connected);
	std::printf("    34000 unacked packets, then acks resume with 50%% loss: %llu of 75 lost packets counted within 150 packets\n",
		static_cast<unsigned long long>(lost_after - lost_before));
}

/* 6. Replays of one malformed datagram count one protocol error. */
void test_malformed_replay()
{
	begin("malformed replay");
	connection a{host_side, 0};
	connection b{client_side, 0};
	const std::array<std::uint8_t, 4> x{{1, 2, 3, 4}};
	CHECK(a.enqueue_reliable(5, x) == enqueue_result::ok);
	datagram d{[&] { const auto p{a.build_outgoing(TICK)}; return datagram{p.begin(), p.end()}; }()};
	d[NET_V2_HEADER_SIZE + 1] = 0xff;
	d[NET_V2_HEADER_SIZE + 2] = 0x0f;
	CHECK(b.on_receive(d, TICK + 1).status == receive_status::malformed_chunk);
	for (unsigned i{}; i != 19; ++i)
		CHECK(b.on_receive(d, TICK + 2 + i).status == receive_status::malformed_chunk);
	const auto s{b.stats()};
	CHECK(s.protocol_errors == 1);
	CHECK(s.packets_received == 0);
	CHECK(s.packets_rejected == 20);
	CHECK(s.state == connection_state::connecting);
	/* The retransmission in a fresh packet is still taken. */
	CHECK(a.on_receive(b.build_outgoing(2 * TICK), 2 * TICK).status == receive_status::accepted);
	unsigned resent{};
	for (net_clock t{3 * TICK}; resent == 0 && t < 100 * TICK; t += TICK)
	{
		const auto p{a.build_outgoing(t)};
		if (p.empty())
			continue;
		const auto report{b.on_receive(p, t + 1)};
		CHECK(report.status == receive_status::accepted);
		resent += static_cast<unsigned>(report.reliable.size());
	}
	CHECK(resent == 1);
	std::printf("    20 copies of one corrupted datagram: 1 protocol error, message arrives by retransmission\n");
}

/* Second review round. */

/* 1. A packet with a malformed chunk is not acknowledged, so the sender
 * retransmits its messages and the receiver stays in order.
 */
void test_malformed_not_acked(const std::uint64_t seed)
{
	begin("malformed packet is not acked");
	rng r{seed};
	sim_world w{r, link_params{.latency = net_milliseconds(40)}};
	w.run(10, [&](const unsigned i) { w.set_state(i); });
	unsigned corrupted{};
	w.link.mangle[1] = [&](datagram &d) {
		if (corrupted != 0 || d.size() < NET_V2_HEADER_SIZE + NET_V2_CHUNK_HEADER_SIZE)
			return;
		const auto h{*packet_header::read(d)};
		if (!h.has_flag(packet_flag::has_reliable))
			return;
		/* The first RELIABLE chunk's length: make it overrun the packet. */
		d[NET_V2_HEADER_SIZE + 1] = 0xff;
		d[NET_V2_HEADER_SIZE + 2] = 0x0f;
		++corrupted;
	};
	w.run(300, [&](const unsigned i) {
		w.set_state(i);
		if (i == 0)
			for (unsigned k{}; k != 3 && w.peers[0].sent.size() < 300; ++k)
				w.enqueue_random_message(0, 40);
	});
	CHECK(corrupted == 1);
	check_delivery(w, 0);
	const auto host{w.peers[0].conn.stats()};
	const auto client{w.peers[1].conn.stats()};
	print_stats("host", host);
	print_stats("client", client);
	CHECK(w.peers[1].malformed == 1);
	CHECK(client.protocol_errors == 1);
	CHECK_MSG(host.message_resends >= 1, "the corrupted packet's messages must be resent");
	CHECK(host.state == connection_state::connected && client.state == connection_state::connected);
	CHECK(host.in_flight == 0 && host.queue_messages == 0);
	CHECK(client.recv_window_pending == 0);
	/* Exactly one packet went unacked: the corrupted one. */
	CHECK_MSG(host.packets_lost == 1, "packets lost " + std::to_string(host.packets_lost));
	std::printf("    one corrupted chunk length: %llu message(s) resent, all 300 delivered in order, both sides connected\n",
		static_cast<unsigned long long>(host.message_resends));
}

/* 2. Hostile echo fields must not overflow the RTT arithmetic. */
void test_hostile_echo()
{
	begin("hostile echo fields");
	connection c{host_side, 0};
	/* One real packet, so that ack 1 names a logged packet. */
	c.set_unreliable_state(chunk_type::state, std::array<std::uint8_t, 1>{{1}});
	CHECK(!c.build_outgoing(0).empty());
	std::uint16_t seq{};
	for (const auto &[echo_time, echo_delay, now] : {
		std::tuple{net_time{0x7fffffff}, std::uint16_t{100}, net_clock{0}},
		std::tuple{net_time{0x80000001}, std::uint16_t{0}, net_clock{0}},
		std::tuple{net_time{0xffffffff}, std::uint16_t{0xfffe}, net_clock{0}},
		std::tuple{net_time{1}, std::uint16_t{0xfffe}, net_clock{0x7fffffffffffffff}},
		std::tuple{net_time{0x12345678}, std::uint16_t{7}, net_clock{-0x7fffffffffffffff}},
	})
	{
		packet_header h;
		h.session_id = host_side.session_id;
		h.peer_token = host_side.peer_token;
		h.player_id = host_side.remote_player_id;
		h.flags = static_cast<std::uint8_t>(packet_flag::keepalive);
		h.seq = ++seq;
		h.ack = 1;
		h.echo_time = echo_time;
		h.echo_delay = echo_delay;
		std::array<std::uint8_t, NET_V2_HEADER_SIZE> d{};
		h.write(d.data());
		const auto report{c.on_receive(d, now)};
		CHECK(report.status == receive_status::accepted);
	}
	const auto s{c.stats()};
	/* None of the forged echo values got in, and packet 1's ack is not a
	 * measurement either (nothing later was echoed before it).
	 */
	CHECK_MSG(!s.rtt_valid, "srtt " + std::to_string(s.srtt));
	std::printf("    extreme echo_time/echo_delay/now combinations: no overflow, no bogus sample\n");
}

/* 3. A caller that builds every frame at 240 Hz still gets one packet
 * budget per 60 Hz tick.
 */
void test_high_rate_caller()
{
	begin("240 Hz caller, 60 Hz tick budget");
	connection a{host_side, 0};
	connection b{client_side, 0};
	const datagram big(NET_V2_MAX_MESSAGE);
	for (unsigned k{}; k != 90; ++k)
		CHECK(a.enqueue_reliable(9, big) == enqueue_result::ok);
	const net_clock frame{TICK / 4};
	std::vector<unsigned> per_tick;
	std::size_t delivered{};
	std::uint64_t frames_with_packets{};
	for (unsigned f{}; f != 4 * 200; ++f)
	{
		const net_clock now{f * frame};
		/* Windows of four frames (a period, within the one unit of
		 * rounding tolerance the scheduler allows).
		 */
		const auto tick_index{static_cast<std::size_t>(f / 4)};
		if (per_tick.size() <= tick_index)
			per_tick.resize(tick_index + 1);
		bool any{};
		/* No begin_tick: the budget must open by itself once per period. */
		for (;;)
		{
			const auto p{a.build_outgoing(now)};
			if (p.empty())
				break;
			any = true;
			++per_tick[tick_index];
			const auto report{b.on_receive(p, now + 1)};
			CHECK(report.status == receive_status::accepted);
			delivered += report.reliable.size();
		}
		if (any)
			++frames_with_packets;
		if (f % 4 == 3)
		{
			b.begin_tick(now + 2);
			const auto ack{b.build_outgoing(now + 2)};
			if (!ack.empty())
				CHECK(a.on_receive(ack, now + 3).status == receive_status::accepted);
		}
	}
	const auto worst{std::ranges::max(per_tick)};
	CHECK_MSG(worst <= NET_V2_DEFAULT_MAX_PACKETS_PER_TICK, "packets in one tick period: " + std::to_string(worst));
	CHECK_MSG(delivered == 90, "delivered " + std::to_string(delivered));
	CHECK(a.stats().in_flight == 0);
	std::printf("    %zu tick periods, at most %u packets in any of them, %llu of 800 frames sent, 90 KiB delivered\n",
		per_tick.size(), worst, static_cast<unsigned long long>(frames_with_packets));
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
	const auto peer_time_err{net_time_diff(w.peers[1].conn.to_peer_time(w.peers[1].clock(w.now)), to_net_time(w.peers[0].clock(w.now)))};
	CHECK((peer_time_err < 0 ? -peer_time_err : peer_time_err) < net_milliseconds(5));
	/* A 300 ms step of the client clock is followed within one window. */
	w.peers[1].bias += net_milliseconds(300);
	w.run(150, [&](const unsigned i) { w.set_state(i); });
	s = w.peers[1].conn.stats();
	const auto after_step{s.clock_offset - truth()};
	std::printf("    300 ms step: applied error %.2f ms after 2.5 s\n", to_ms(after_step));
	CHECK((after_step < 0 ? -after_step : after_step) < net_milliseconds(5));
	/* A small drift is slewed, not jumped.  The min-RTT window keeps the
	 * old target for up to 2 s; once it has expired the applied offset
	 * moves toward the new one at NET_V2_CLOCK_SLEW_PER_SECOND, so over
	 * the following second the error shrinks by about 5 ms.
	 */
	w.peers[1].bias += net_milliseconds(20);
	const auto applied_error{[&] {
		const auto e{w.peers[1].conn.stats().clock_offset - truth()};
		return e < 0 ? -e : e;
	}};
	w.run(120, [&](const unsigned i) { w.set_state(i); });
	const auto e1{applied_error()};
	w.run(60, [&](const unsigned i) { w.set_state(i); });
	const auto e2{applied_error()};
	std::printf("    20 ms step: applied error %.2f ms after 2 s, %.2f ms after 3 s (slew %.0f ms/s)\n", to_ms(e1), to_ms(e2), to_ms(NET_V2_CLOCK_SLEW_PER_SECOND));
	CHECK_MSG(e1 >= net_milliseconds(8), "error after 2 s " + std::to_string(to_ms(e1)) + " ms");
	CHECK_MSG(e1 - e2 >= net_milliseconds(7) / 2 && e1 - e2 <= net_milliseconds(13) / 2, "slewed " + std::to_string(to_ms(e1 - e2)) + " ms in 1 s");
	/* 20 ms at 5 ms/s is 4 s; allow for the estimate noise on top. */
	w.run(300, [&](const unsigned i) { w.set_state(i); });
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
	CHECK(!packet_header::read(std::span{buf}.first(NET_V2_HEADER_SIZE - 1)).has_value());
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
	test_forged_seq_far_ahead();
	test_clock_stall_freezes_offset(seed);
	test_reliable_before_events(seed);
	test_bound_hold_ages_by_ticks();
	test_held_message_view_lifetime();
	test_to_peer_time_wraps(seed);
	test_echo_of_old_unechoed_packet();
	test_bound_excess();
	test_parts_after_first_packet();
	test_frame_rate_caller_update_first();
	test_event_beside_backlog(seed);
	test_late_peer_packet_no_sample();
	test_late_ack_of_lost_packets();
	test_event_beside_big_state(seed);
	test_gap_rule_counts_acks();
	test_rejected_sample_keeps_packet();
	test_set_peer_tick();
	test_slow_peer_srtt(seed);
	test_two_packets_per_tick_srtt(seed);
	test_bundle_with_backlog(seed);
	test_frame_rate_caller();
	test_held_packets(seed);
	test_closed_refuses_unreliable();
	test_state_parts(seed);
	test_state_parts_reorder();
	test_first_grant_single();
	test_exact_caller_no_double_grant();
	test_echo_pinning(seed);
	test_corrupt_ack_not_applied(seed);
	test_stream_stalled();
	test_malformed_then_intact();
	test_corrupted_future_seq();
	test_big_message_rides_second_packet();
	test_state_survives_bulk_transfer(seed);
	test_peer_tick_period(seed);
	test_latest_wins_wrap();
	test_grant_closes_tick();
	test_hostile_ack();
	test_event_head_of_line();
	test_zero_tick_period();
	test_echo_authentication(seed);
	test_tick_credit();
	test_reordered_echo();
	test_long_blackout_scan();
	test_malformed_replay();
	test_malformed_not_acked(seed);
	test_hostile_echo();
	test_high_rate_caller();
	test_window_in_one_packet();
	test_packets_per_tick(seed);
	test_unaligned_peers(seed);
	test_ack_blackout_loss(seed);
	test_echo_delay_clamp();
	test_unreliable_chunks_per_packet();
	test_fuzz(seed);
	test_clock(seed);
	std::printf("all tests passed\n");
	return 0;
}
