/*
 * This file is part of the DXX-Rebirth project <https://github.com/dxx-rebirth/dxx-rebirth/>.
 * It is copyright by its individual contributors, as recorded in the
 * project's Git history.  See COPYING.txt at the top level for license
 * terms and a link to the Git history.
 */
/*
 * Multiplayer network protocol v2: the transport layer
 * (Documentation/network-protocol-v2.md, sections 2.2 and 3).
 *
 * One `connection` object describes one direction pair (local peer <->
 * remote peer) of a session.  It owns the reliable send queue, the
 * selective-repeat retransmission logic, the receive window, the RTT/RTO
 * estimator and the clock offset estimator.  It never touches a socket
 * and never reads a clock: the caller feeds it datagrams and the current
 * time, and takes the datagrams it builds.  This keeps the transport
 * independent of the game and testable in isolation.
 *
 * Typical use, once per network tick:
 *
 *	c.begin_tick(now);
 *	c.set_unreliable_state(chunk_type::state, bundle);	// optional
 *	for (;;) {
 *		const auto packet{c.build_outgoing(now)};
 *		if (packet.empty())
 *			break;
 *		socket.send(packet);
 *	}
 *
 * and for every datagram read from the socket:
 *
 *	auto report{c.on_receive(datagram, now)};
 *	for (auto &m : report.reliable) ...	// in order, exactly once
 *	for (auto &u : report.unreliable) ...	// newest state, every event
 */

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <span>
#include <vector>

#include "net_v2.h"

namespace dcx {

namespace net_v2 {

/* Best-effort one-shot chunks (`send_unreliable`) that do not fit in the
 * current packet wait for the next one.  At most this many wait, in a
 * fixed ring of slots (queueing one allocates nothing); older ones are
 * dropped first.  This is an implementation bound, not a wire constant.
 */
constexpr std::size_t NET_V2_EVENT_QUEUE_MAX{64};

/* The largest `event_u` payload `send_unreliable` takes.  Events are
 * cosmetic and small (§6.9 gives each a u8 length); the bound keeps the
 * ring of slots at some 17 KiB per connection instead of 75.  An
 * implementation bound, not a wire constant: the chunk length field
 * still allows NET_V2_MAX_CHUNK_PAYLOAD and a receiver accepts it.
 */
constexpr std::size_t NET_V2_MAX_EVENT{255};

/* A well-formed packet whose `seq` is further ahead of the newest seen
 * than a conforming peer could have sent within NET_V2_TIMEOUT is
 * rejected (`bad_seq`) and counted as a protocol error: accepting it
 * would move the reorder window past every real packet, time the
 * connection out and turn our acks into protocol errors at the peer.
 * The bound follows the peer's tick (set_peer_tick): the ticks in the
 * timeout, times the packets a tick may carry (max_packets_per_tick or
 * a full bundle's parts + 1, whichever is more), times this margin,
 * capped to the half sequence space seq_diff can tell apart.
 */
constexpr net_clock NET_V2_SEQ_JUMP_MARGIN{2};

/* A pending event that did not fit beside the state chunk in this many
 * packets is dropped; later events are not held up by it in the
 * meantime (order between unreliable events is not guaranteed).
 */
constexpr unsigned NET_V2_EVENT_SKIP_MAX{8};

/* A tick period as a ratio of net time units, so that 1/60 s (65536/60,
 * not a whole number of units) is exact and a tick origin advanced by
 * whole periods never drifts against a true 60 Hz caller.
 */
struct tick_period
{
	net_clock numerator{net_seconds(1)};
	net_clock denominator{60};
	/* The period in whole units, rounded up: for slack, never for
	 * scheduling.
	 */
	[[nodiscard]]
	constexpr net_clock units() const
	{
		return (numerator + denominator - 1) / denominator;
	}
};

/* Section 2.3: the default network tick, 60 Hz. */
constexpr tick_period NET_V2_DEFAULT_TICK{};
constexpr net_clock NET_V2_DEFAULT_TICK_PERIOD{NET_V2_DEFAULT_TICK.units()};

/* Section 3.6: packets per tick per connection. */
constexpr unsigned NET_V2_DEFAULT_MAX_PACKETS_PER_TICK{2};

struct connection_config
{
	std::uint32_t session_id{};
	std::uint32_t peer_token{};
	std::uint8_t local_player_id{NET_V2_PLAYER_ID_NONE};
	std::uint8_t remote_player_id{NET_V2_PLAYER_ID_NONE};
	/* The network tick.  A packet budget of max_packets_per_tick opens
	 * once per tick (see begin_tick), and the RTO is padded by the tick
	 * for the peer's ack hold time.  The connection clamps both terms of
	 * the period to at least 1 and the budget to at least 2: with one
	 * packet per tick and a state chunk every tick, a reliable message
	 * larger than the space beside the state could never go out.
	 */
	tick_period tick{};
	unsigned max_packets_per_tick{NET_V2_DEFAULT_MAX_PACKETS_PER_TICK};
	/* The peer's tick: it holds its acks up to this long.  A zero term
	 * means the same as `tick`; the session layer sets it from the
	 * handshake (stage 1).
	 */
	tick_period peer_tick{0, 0};
};

enum class connection_state : std::uint8_t
{
	/* Created, but no valid packet from the peer yet. */
	connecting,
	connected,
	closed,
};

enum class close_reason : std::uint8_t
{
	none,
	/* No valid packet for NET_V2_TIMEOUT. */
	timeout,
	/* The oldest reliable message was unacked for NET_V2_UNACKED_TIMEOUT. */
	unacked_timeout,
	/* Reliable send queue exceeded NET_V2_QUEUE_MAX_MESSAGES or _BYTES. */
	queue_overflow,
	/* NET_V2_PROTOCOL_ERROR_LIMIT malformed packets within the window. */
	protocol_error,
	/* Reliable messages were held out of order for NET_V2_UNACKED_TIMEOUT
	 * without the gap before them ever being filled, although the peer
	 * kept talking: the stream cannot advance.
	 */
	stream_stalled,
	/* connection::close() was called. */
	local,
};

enum class enqueue_result : std::uint8_t
{
	ok,
	/* Payload exceeds NET_V2_MAX_MESSAGE.  Nothing is queued. */
	too_large,
	/* The queue bound was hit.  The connection is now closed. */
	queue_overflow,
	closed,
};

/* Why on_receive dropped a datagram, in the order the checks run (§3.7):
 * the first failing check names the status.  Everything from `bad_length`
 * on means the datagram had no effect at all.
 */
enum class receive_status : std::uint8_t
{
	/* Header applied, every chunk delivered. */
	accepted,
	/* Shorter than the header or longer than NET_V2_MAX_PACKET. */
	bad_length,
	bad_proto,
	bad_flags,
	/* flags.UNCONNECTED is set: not for a connection, handled by the
	 * session layer.
	 */
	unconnected,
	bad_session,
	bad_token,
	bad_player,
	/* The connection is closed.  Checked after the identity checks
	 * (bad_session .. bad_player) and before the replay window, so a
	 * packet for a closed connection is reported as such, not as a
	 * duplicate.
	 */
	closed,
	/* Already seen, or older than the 64-packet reorder window. */
	duplicate,
	/* `seq` is further ahead of the newest seen than the peer could have
	 * sent within the timeout (NET_V2_SEQ_JUMP_MARGIN): a corrupted or
	 * forged sequence, not a stream a conforming peer can produce.
	 * Nothing is applied; one protocol error is counted per distinct
	 * `seq`, and 16 within 10 s close the connection.
	 */
	bad_seq,
	/* `ack` names a packet we have not sent: ahead of our newest, or
	 * further behind it than we have sent packets (an ack forged 32768
	 * or more ahead reads as one far behind; once half the sequence
	 * space has been used every number has been sent, and an honest ack
	 * may lag by any distance after a one-way blackout, so from then on
	 * only "ahead" can be told).  Nothing is applied; one protocol error
	 * is counted per distinct `seq`, as for a malformed packet, and 16
	 * within 10 s close the connection the same way.
	 */
	bad_ack,
	/* A chunk was malformed.  Nothing of the datagram was applied, not
	 * even the header's acks or echo (a corrupt ack bit would otherwise
	 * acknowledge a message that was never delivered), and it is not
	 * acknowledged, so a conforming peer retransmits.  One protocol
	 * error is counted per distinct `seq` (16 within 10 s close the
	 * connection with close_reason::protocol_error), and an intact copy
	 * of the same `seq` arriving later is accepted normally.
	 */
	malformed_chunk,
};

/* A reliable message as delivered: a view, like unreliable_view.  A
 * message delivered while nothing is held out of order points into the
 * datagram the caller passed to on_receive; one that went through the
 * receive window (held itself, or arriving while others were held)
 * points into storage the connection keeps until its next on_receive.
 * Either way the view is valid until the next on_receive at the latest
 * (and no longer than the caller's datagram buffer): copy what must
 * outlive that.
 */
struct reliable_message
{
	std::uint8_t type{};
	std::span<const std::uint8_t> payload;
};

/* An unreliable chunk as delivered: a view into the datagram the caller
 * passed to on_receive, valid only as long as that buffer is, and at the
 * latest until the next on_receive.  For `state`/`input`, `part` and
 * `part_count` are the bundle part (§3.8); the part byte is not in the
 * payload.  Events have part 0 of 1.
 */
struct unreliable_view
{
	chunk_type type{};
	std::uint8_t part{};
	std::uint8_t part_count{1};
	std::span<const std::uint8_t> payload;
};

/* What one on_receive delivered.  Both lists live in connection storage
 * (no allocation per datagram) and, like the views they hold, are valid
 * until the next on_receive on that connection.
 */
struct receive_report
{
	receive_status status{receive_status::bad_length};
	/* Reliable messages that became deliverable, in sequence order.
	 * Views, see reliable_message.
	 */
	std::span<const reliable_message> reliable;
	/* Every unreliable chunk of the packet, in packet order, except a
	 * `state`/`input` part from a packet older than the newest one that
	 * already delivered that (type, part) within the reorder window:
	 * latest wins per part (§3.8), so a bundle whose parts travelled in
	 * different packets is not guaranteed to arrive whole or together;
	 * consumers keep the newest of each part.  All chunks of one packet
	 * count as equally new.  Views into the caller's datagram, see
	 * unreliable_view.
	 */
	std::span<const unreliable_view> unreliable;
};

struct connection_stats
{
	connection_state state{};
	close_reason closed_because{};
	/* Section 3.5.  All in net time units; `rtt_valid` is false before
	 * the first sample, and `rto` is then NET_V2_RTO_MAX.
	 */
	bool rtt_valid{};
	net_clock srtt{};
	net_clock rttvar{};
	net_clock rto{};
	/* Exponential moving average of the fraction of our packets that the
	 * peer never acknowledged, 0..1.
	 */
	double loss_estimate{};
	std::uint64_t packets_sent{};
	std::uint64_t packets_received{};
	std::uint64_t packets_rejected{};
	std::uint64_t packets_acked{};
	std::uint64_t packets_lost{};
	std::uint64_t messages_enqueued{};
	std::uint64_t messages_delivered{};
	std::uint64_t message_sends{};
	std::uint64_t message_resends{};
	std::uint64_t resends_by_gap{};
	std::uint64_t resends_by_rto{};
	std::uint64_t unreliable_dropped{};
	std::uint64_t protocol_errors{};
	/* Reliable messages queued or in flight, and their payload bytes. */
	std::size_t queue_messages{};
	std::size_t queue_bytes{};
	std::size_t in_flight{};
	/* Out-of-order messages held in the receive window. */
	std::size_t recv_window_pending{};
	/* Section 2.2: peer clock ~= local clock + clock_offset. */
	bool clock_offset_valid{};
	net_clock clock_offset{};
	net_clock clock_offset_target{};
	net_clock last_heard{};
};

/* Section 3.5: Jacobson/Karels with RFC 6298 constants, plus slack for
 * the tick.  The peer holds its ack until its next tick (`hold`, its
 * tick period) and we look for losses only at ours (`tick`), so an ack
 * for a lossless packet arrives up to hold + tick after the round trip
 * the echo samples measure; the two holds are therefore added to the
 * variance term rather than weighed against it.  rttvar is floored at
 * hold / 4 so that it cannot collapse to zero on a steady link.
 *
 * A bound (a delayed round trip that did happen, known from a late ack
 * or from the echo of a reordered peer packet) widens rttvar like a
 * sample and leaves srtt alone.  rttvar is a mean deviation, though, and
 * forgets a bound within a handful of samples, while the next packet
 * may well be held just as long; so the largest excess of a bound over
 * srtt is kept for as long as bounds keep coming and for a second after
 * the last (NET_V2_BOUND_HOLD), then fades by 1/16 per tick, and the
 * RTO covers that excess whenever it is more than the variance term: a
 * loss is not declared before a delay that was just seen.
 *
 *	rto = clamp(srtt + max(4 rttvar, excess) + hold + tick, NET_V2_RTO_MIN, NET_V2_RTO_MAX)
 */
class rtt_estimator
{
	net_clock m_tick_period;
	net_clock m_hold_period;
	bool m_valid{};
	net_clock m_srtt{};
	net_clock m_rttvar{};
	/* The largest recent bound above srtt, and the time since the last
	 * bound (in ticks of m_tick_period).
	 */
	net_clock m_bound_excess{};
	net_clock m_bound_age{};
public:
	explicit rtt_estimator(const net_clock tick_period = NET_V2_DEFAULT_TICK_PERIOD, const net_clock hold_period = NET_V2_DEFAULT_TICK_PERIOD) :
		m_tick_period{tick_period},
		m_hold_period{hold_period}
	{
	}
	void add_sample(net_clock r);
	/* A bound on a round trip rather than a measurement of it (the echo
	 * of a reordered peer packet, which carries that packet's own
	 * delay, or the ack of a packet that will never be echoed, which
	 * carries the peer's hold): it widens rttvar, so that the RTO covers
	 * such a delay, never narrows it, and never moves srtt.  Ignored
	 * before the first real sample; the caller checks valid() to know.
	 */
	void add_bound(net_clock r);
	/* Ages the bound excess by this many ticks (every tick granted, not
	 * every grant: a caller at half the tick rate gets two per grant).
	 */
	void advance_ticks(net_clock ticks);
	/* The hold term changed (the peer's tick became known). */
	void set_hold_period(const net_clock hold_period)
	{
		m_hold_period = hold_period;
	}
	[[nodiscard]]
	bool valid() const
	{
		return m_valid;
	}
	[[nodiscard]]
	net_clock srtt() const
	{
		return m_srtt;
	}
	[[nodiscard]]
	net_clock rttvar() const
	{
		return m_rttvar;
	}
	[[nodiscard]]
	net_clock rto() const;
};

/* Section 2.2: offset of the peer's clock relative to ours, from one
 * (rtt, offset) sample per received packet.  The target is the offset of
 * the sample with the smallest RTT in the last NET_V2_CLOCK_SAMPLE_WINDOW;
 * the applied offset slews toward it at NET_V2_CLOCK_SLEW_PER_SECOND, or
 * jumps if it is more than NET_V2_CLOCK_JUMP_THRESHOLD away.  During the
 * first window after the first sample (start of session) the applied
 * offset follows the target directly, since the first few samples are the
 * least accurate and a slew from them would take seconds.
 *
 * The window minimum is kept as a monotonic deque: a sample is dropped as
 * soon as a newer one with a smaller RTT arrives, because the newer one
 * outlives it and beats it.  The front is always the minimum.
 */
class clock_sync
{
	struct sample
	{
		net_clock at;
		net_clock rtt;
		net_clock offset;
	};
	std::deque<sample> m_candidates;
	bool m_valid{};
	net_clock m_offset{};
	net_clock m_target{};
	net_clock m_first_sample{};
	net_clock m_last_update{};
	net_clock m_slew_remainder{};
	/* Returns true if a candidate expired. */
	bool expire(net_clock now);
public:
	void add_sample(net_clock now, net_clock rtt, net_clock offset);
	/* Advance the slew.  Call at least once per tick. */
	void update(net_clock now);
	[[nodiscard]]
	bool valid() const
	{
		return m_valid;
	}
	[[nodiscard]]
	net_clock offset() const
	{
		return m_offset;
	}
	[[nodiscard]]
	net_clock target() const
	{
		return m_target;
	}
	/* Samples still able to become the window minimum. */
	[[nodiscard]]
	std::size_t sample_count() const
	{
		return m_candidates.size();
	}
};

class connection
{
	struct out_msg
	{
		std::uint16_t seq{};
		std::uint8_t type{};
		bool sent{};
		bool acked{};
		bool resend{};
		std::uint16_t in_packet_seq{};
		net_clock first_sent{};
		net_clock last_sent{};
		std::vector<std::uint8_t> payload;
	};
	struct packet_log_entry
	{
		bool valid{};
		bool acked{};
		bool lost{};
		/* Its RTT was sampled: every packet yields one sample at most,
		 * from whichever of the peer's packets echoes it first, or from
		 * its ack if no echo ever comes.
		 */
		bool echoed{};
		std::uint16_t seq{};
		net_clock sent_at{};
		std::vector<std::uint16_t> msg_seqs;
	};
	/* A run of consecutive messages selected for the packet being
	 * built: one RELIABLE chunk.  Indexes m_carried.
	 */
	struct selected_run
	{
		std::size_t first;
		std::size_t count;
	};
	struct recv_slot
	{
		bool filled{};
		std::uint8_t type{};
		std::vector<std::uint8_t> payload;
	};
	/* One pending part of the outgoing latest-wins bundle, copied into a
	 * fixed buffer: no allocation per tick.
	 */
	struct state_part
	{
		bool pending{};
		std::size_t size{};
		std::array<std::uint8_t, NET_V2_MAX_STATE_PART> data{};
	};
	struct state_bundle
	{
		unsigned count{1};
		std::array<state_part, NET_V2_STATE_MAX_PARTS> parts{};
	};

	/* A queued best-effort event (always `event_u`), in a fixed slot. */
	struct pending_event
	{
		std::uint8_t size{};
		std::uint8_t skipped{};
		std::array<std::uint8_t, NET_V2_MAX_EVENT> data{};
	};
	/* Parsed once by validate_chunks, applied by deliver_*. */
	struct parsed_message
	{
		std::uint16_t seq;
		std::uint8_t type;
		std::span<const std::uint8_t> payload;
	};
	struct parsed_chunk
	{
		chunk_type type;
		std::uint8_t part;
		std::uint8_t part_count;
		std::span<const std::uint8_t> payload;
	};
	/* How the pending bundle parts pack, in send order. */
	struct state_plan
	{
		/* Bytes the next packet carries (the parts that fit first). */
		std::size_t first_packet_bytes;
		/* Packets all pending parts need; 0 if none is pending. */
		unsigned packets;
	};

	connection_config m_config;
	connection_state m_state{connection_state::connecting};
	close_reason m_close_reason{close_reason::none};

	/* Sender side */
	std::uint16_t m_local_seq{};
	std::uint16_t m_next_msg_seq{};
	/* In sequence order.  Messages acked out of order stay in place,
	 * marked acked, until everything before them is acked too;
	 * m_held_acked counts those.  Sent messages form a prefix of the
	 * deque (sending is in order, popping only at the front), so
	 * m_sent_count is also the index of the first unsent message.
	 * m_resend_pending counts sent messages flagged for retransmission.
	 */
	std::deque<out_msg> m_messages;
	std::size_t m_sent_count{};
	std::size_t m_held_acked{};
	std::size_t m_resend_pending{};
	std::size_t m_queue_bytes{};
	std::array<packet_log_entry, NET_V2_RECV_WINDOW> m_packet_log{};
	/* Next packet sequence to examine for having fallen out of the
	 * peer's ack bitfield unacked.
	 */
	std::uint16_t m_lost_scan_seq{1};
	net_clock m_last_sent{};
	/* Tick budget (see begin_tick): the origin of the last granted
	 * period in units scaled by the period's denominator (exact
	 * arithmetic), whether a tick was ever granted (the first grant is a
	 * single tick), the ticks granted but not yet started, and the
	 * packets built in the tick currently open.
	 */
	net_clock m_tick_origin{};
	bool m_tick_granted{};
	unsigned m_tick_credit{};
	/* Ticks granted since begin_tick last reported them. */
	unsigned m_unreported_ticks{};
	bool m_tick_open{};
	unsigned m_tick_packets{};
	/* Packets allowed in the open tick: max_packets_per_tick, or one
	 * more than a pending bundle needs when it needs more than one;
	 * raised on every packet of the tick, so parts set after its first
	 * packet still fit.
	 */
	unsigned m_tick_budget{};
	/* Reliable messages were due but did not fit in the last packet. */
	bool m_tick_backlog{};
	bool m_ack_owed{};
	packet_buffer m_outgoing{};
	std::vector<out_msg *> m_carried;
	std::vector<selected_run> m_runs;
	/* The outgoing latest-wins bundles, [0] state and [1] input. */
	std::array<state_bundle, 2> m_state_out{};
	/* The outgoing events: NET_V2_EVENT_QUEUE_MAX slots, queued in the
	 * order of a ring of slot indices (the oldest at m_events_head,
	 * m_events_count in use); the slots not in the ring are on the free
	 * stack.  Sending or dropping an event frees its slot and closes the
	 * index ring up behind the head; no slot is ever copied.
	 */
	std::array<pending_event, NET_V2_EVENT_QUEUE_MAX> m_event_slots{};
	std::array<std::uint8_t, NET_V2_EVENT_QUEUE_MAX> m_event_order{};
	std::array<std::uint8_t, NET_V2_EVENT_QUEUE_MAX> m_event_free{};
	std::size_t m_event_free_count{};
	std::size_t m_events_head{};
	std::size_t m_events_count{};
	/* The slot index of the i-th queued event. */
	[[nodiscard]]
	std::uint8_t &event_index(const std::size_t i)
	{
		return m_event_order[(m_events_head + i) % NET_V2_EVENT_QUEUE_MAX];
	}
	[[nodiscard]]
	pending_event &event(const std::size_t i)
	{
		return m_event_slots[event_index(i)];
	}
	/* The newest of our packets sampled through an echo, once any was.
	 * Samples progress: a conforming peer echoes the newest packet it
	 * received, so an echo of an older packet in the peer's newest
	 * packet is not a measurement (and a hostile peer's lever otherwise).
	 * A packet acked only after a later one was already echoed arrived
	 * out of order at the peer, so its ack is its one bound.
	 */
	std::optional<std::uint16_t> m_echo_sampled_seq;

	/* Receiver side */
	/* The newest sequence received, once anything was. */
	std::optional<std::uint16_t> m_highest_seen;
	/* The largest forward jump of `seq` still taken as the peer's own
	 * stream; see NET_V2_SEQ_JUMP_MARGIN and set_peer_tick.
	 */
	std::int16_t m_seq_jump_bound{};
	std::uint64_t m_ack_bits{};
	net_clock m_last_heard{};
	/* Of the newest packet received (m_highest_seen), which our headers
	 * echo.
	 */
	net_time m_last_recv_send_time{};
	net_clock m_last_recv_local_time{};
	std::uint16_t m_next_expected{};
	std::array<recv_slot, NET_V2_RECV_WINDOW> m_recv_window{};
	std::size_t m_recv_window_pending{};
	/* Payloads of messages held out of order and delivered by the last
	 * on_receive: the report's views point into the first
	 * m_delivered_held_count entries.  The entries beyond them are the
	 * pool of retained buffers: a window slot takes one when it fills
	 * (instead of allocating) and gives its payload back on delivery, by
	 * swap, and the next on_receive clears the delivered ones back into
	 * the pool.  Buffers are never destroyed, so a held message costs no
	 * allocation once as many have been held at once before.
	 */
	std::vector<std::vector<std::uint8_t>> m_delivered_held;
	std::size_t m_delivered_held_count{};
	/* The lists the last report points into; cleared by the next
	 * on_receive.
	 */
	std::vector<reliable_message> m_report_reliable;
	std::vector<unreliable_view> m_report_unreliable;
	/* When the window last went from empty to holding out-of-order
	 * messages, or last advanced; the stream_stalled clock.
	 */
	net_clock m_recv_gap_since{};
	/* Per latest-wins chunk type and part: the newest packet that
	 * carried one, once any did.
	 */
	std::array<std::array<std::optional<std::uint16_t>, NET_V2_STATE_MAX_PARTS>, 2> m_latest{};
	/* Sequences of malformed packets already counted as a protocol
	 * error, oldest first; a replay of one counts no further error.
	 * Never used to reject anything: an intact copy is accepted.
	 */
	std::deque<std::uint16_t> m_recent_malformed;
	std::deque<net_clock> m_protocol_error_times;
	std::vector<parsed_message> m_parsed_messages;
	std::vector<parsed_chunk> m_parsed_chunks;

	/* Estimators */
	rtt_estimator m_rtt;
	clock_sync m_clock;
	double m_loss_estimate{};

	connection_stats m_stats;

	void close_with(close_reason reason);
	/* The period arithmetic behind begin_tick: grant the ticks elapsed,
	 * judge the timeouts and the RTO once per grant.
	 */
	void grant_ticks(net_clock now);
	void check_timeouts(net_clock now);
	void detect_rto_losses(net_clock now);
	void flag_resend(out_msg &m);
	void process_acks(std::uint16_t ack, std::uint64_t ack_bits, net_clock now, bool newest_carrier, std::optional<std::uint16_t> echo_before);
	void resolve_packet(packet_log_entry &e, bool acked);
	void pop_acked_messages();
	[[nodiscard]]
	bool window_allows(const out_msg &m) const;
	[[nodiscard]]
	bool any_message_due() const;
	/* Count a protocol error for `seq` unless one was already counted
	 * for it; returns true if the limit is reached.
	 */
	[[nodiscard]]
	bool count_protocol_error(std::uint16_t seq, net_clock now);
	[[nodiscard]]
	std::optional<std::uint16_t> &latest_for(chunk_type type, unsigned part);
	[[nodiscard]]
	bool any_state_pending() const;
	[[nodiscard]]
	state_plan plan_state_parts() const;
	void write_state_parts(std::uint8_t *buf, std::size_t &pos);
	/* §3.7 step 8.  Fills m_parsed_messages and m_parsed_chunks. */
	[[nodiscard]]
	bool validate_chunks(std::span<const std::uint8_t> payload, std::uint8_t flags);
	void deliver_reliable(net_clock now);
	void deliver_unreliable(std::uint16_t packet_seq);
	[[nodiscard]]
	std::uint16_t next_local_seq();
public:
	connection(const connection_config &config, net_clock now);

	/* Queue one reliable message.  Payloads above NET_V2_MAX_MESSAGE are
	 * rejected; the queue bounds of section 3.6 close the connection.
	 */
	enqueue_result enqueue_reliable(std::uint8_t type, std::span<const std::uint8_t> payload);

	/* The latest-wins chunk of this tick (`state` or `input`), as one
	 * part (0 of 1) or as part `part` of a bundle of `part_count` parts
	 * (§3.8, at most NET_V2_STATE_MAX_PARTS).  Each part is carried by
	 * the next packet with room for it and then forgotten; setting the
	 * same part again before that replaces it.  Parts that do not fit
	 * beside each other open the tick's second packet, like a reliable
	 * backlog does.  Payloads above NET_V2_MAX_STATE_PART, and invalid
	 * part numbers, are ignored and counted as dropped; on a closed
	 * connection nothing is taken.
	 */
	void set_unreliable_state(chunk_type type, std::span<const std::uint8_t> payload);
	void set_unreliable_state(chunk_type type, unsigned part, unsigned part_count, std::span<const std::uint8_t> payload);

	/* A best-effort `event_u` chunk: sent once in the next packet with
	 * room for it, never retransmitted.  An event that does not fit does
	 * not hold up the ones behind it and is dropped after
	 * NET_V2_EVENT_SKIP_MAX packets.  Returns false if it was dropped at
	 * once, or refused: only `event_u` of at most NET_V2_MAX_EVENT bytes
	 * is accepted (`state`/`input` go through set_unreliable_state, and
	 * any other type would be rejected by the peer as malformed).
	 */
	bool send_unreliable(chunk_type type, std::span<const std::uint8_t> payload);

	/* Grant one tick for every full period that has elapsed since the
	 * last grant, keeping the tick phase (the origin advances by whole
	 * periods in exact rational arithmetic, it is not reset to `now`),
	 * so a caller at any rate gets exactly one tick per period, a true
	 * 60 Hz caller never sees a double grant from a truncated period
	 * (one unit of tolerance covers its own clock rounding), and a frame
	 * that spans two ticks gets both.  The very first grant is a
	 * single tick, however long the connection existed before.  A grant
	 * replaces whatever was left of the previous one and is capped at two
	 * ticks, so unused ticks never pile up into a spare and a long stall
	 * does not end in a burst.
	 * Within a tick, §3.6 allows one packet, and further ones up to
	 * config.max_packets_per_tick only while reliable messages or bundle
	 * parts remain that did not fit; a bundle that needs several packets
	 * raises that limit to one more than it needs, so a blocked head
	 * message still gets a packet of its own.  A grant closes the tick
	 * that was open, so an unfinished allowance is never spent on top of
	 * the new one.
	 *
	 * Returns the ticks granted since the caller last asked (0 if none).
	 * update() and build_outgoing() grant ticks as well but never consume
	 * this report, so the order of calls does not matter and a second
	 * call within the same tick returns 0.  A caller running faster than
	 * the tick (a game loop at frame rate) sets the state chunk only when
	 * it returned non-zero, so that a state is never replaced before it
	 * was sent:
	 *
	 *	c.update(now);	// optional
	 *	if (c.begin_tick(now))
	 *		c.set_unreliable_state(chunk_type::state, bundle);
	 *	while (!(packet = c.build_outgoing(now)).empty())
	 *		send(packet);
	 */
	unsigned begin_tick(net_clock now);

	/* Build the next datagram to send, or return an empty span if nothing
	 * is due or the tick's packet budget is spent.  A packet is due when
	 * there is a pending state part, a pending event, a reliable message
	 * to send or resend, an ack owed for a received reliable message, or
	 * NET_V2_KEEPALIVE_INTERVAL has passed since the last packet.  Call
	 * repeatedly until it returns empty.
	 *
	 * A tick is composed in one fixed priority order:
	 *  (a) state/input parts (§3.8): the first packet carries every part
	 *      that fits, part 0 always; parts that do not fit open further
	 *      packets, and a bundle that needs n > 1 packets may use n + 1
	 *      in the tick;
	 *  (b) reliable messages: resends first, then the queue in order,
	 *      each only if it fits, stopping at the first that does not.  A
	 *      head that does not fit beside the parts gets the next packet
	 *      of the tick, by itself if need be, so no message size starves;
	 *  (c) best-effort events fill whatever is left, in queue order; one
	 *      that does not fit is skipped, not a head-of-line block, and
	 *      dropped after NET_V2_EVENT_SKIP_MAX skips.  Only the tick's
	 *      first packet reserves room for the head event beside the
	 *      parts; later packets put reliable messages first, so a head
	 *      message blocked out of the first packet is never blocked
	 *      again by an event.  Under a sustained reliable backlog events
	 *      may therefore be skipped and dropped: they are cosmetic.
	 * A second and further packet is built only while something is left
	 * over (a due message, a pending part or a pending event), up to
	 * max_packets_per_tick (at least 2) or the bundle's n + 1.  The span
	 * is valid until the next call.
	 */
	[[nodiscard]]
	std::span<const std::uint8_t> build_outgoing(net_clock now);

	/* Validate one received datagram (section 3.7) and apply it. */
	[[nodiscard]]
	receive_report on_receive(std::span<const std::uint8_t> datagram, net_clock now);

	/* The peer's tick became known (stage 1 learns it in the handshake):
	 * the RTO's hold term and the sequence jump bound follow.  Zero terms
	 * mean the same as `tick`.
	 */
	void set_peer_tick(tick_period peer_tick);

	/* Advance the clock slew and, once per tick period, the timeouts
	 * and the RTO loss detection, without building a packet and without
	 * consuming begin_tick's report.  build_outgoing does this too.
	 */
	void update(net_clock now);

	void close();

	[[nodiscard]]
	connection_state state() const
	{
		return m_state;
	}
	[[nodiscard]]
	close_reason closed_because() const
	{
		return m_close_reason;
	}
	[[nodiscard]]
	const connection_config &config() const
	{
		return m_config;
	}
	[[nodiscard]]
	connection_stats stats() const;

	/* The peer's clock at a local time, once the offset is known, as a
	 * wire timestamp (net_time, wrapping every 2^32 units = 65536 s).
	 * The offset is learnt from the peer's 32-bit `send_time` stamps and
	 * is therefore known modulo 2^32 only: a full-width result would be
	 * off by some multiple of 2^32 whenever the peer's clock does not
	 * share the local epoch, and a stage 1 consumer comparing it with a
	 * wire stamp would be wrong without noticing.  Compare with
	 * net_time_diff against `send_time` or the stamps in the bundle.
	 */
	[[nodiscard]]
	net_time to_peer_time(const net_clock local) const
	{
		return to_net_time(local + m_clock.offset());
	}
};

}

}
