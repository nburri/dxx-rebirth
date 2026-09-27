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
 * current packet wait for the next one.  At most this many wait; older
 * ones are dropped first.  This is an implementation bound, not a wire
 * constant.
 */
constexpr std::size_t NET_V2_EVENT_QUEUE_MAX{64};

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

/* Result of connection::on_receive, in the order of the checks of
 * section 3.7.  Everything from `bad_length` on means the datagram had no
 * effect at all.
 */
enum class receive_status : std::uint8_t
{
	/* Header applied, every chunk delivered. */
	accepted,
	/* A chunk was malformed.  Nothing of the datagram was applied, not
	 * even the header's acks or echo (a corrupt ack bit would otherwise
	 * acknowledge a message that was never delivered), and it is not
	 * acknowledged, so a conforming peer retransmits.  One protocol
	 * error is counted per distinct `seq`, and an intact copy of the
	 * same `seq` arriving later is accepted normally.
	 */
	malformed_chunk,
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
	/* `ack` names a packet we have not sent.  Nothing is applied; one
	 * protocol error is counted per distinct `seq`, as for a malformed
	 * packet.
	 */
	bad_ack,
	/* Already seen, or older than the 64-packet reorder window. */
	duplicate,
	/* The connection is closed. */
	closed,
};

struct reliable_message
{
	std::uint8_t type{};
	std::vector<std::uint8_t> payload;
};

struct unreliable_chunk
{
	chunk_type type{};
	std::vector<std::uint8_t> payload;
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

struct receive_report
{
	receive_status status{receive_status::bad_length};
	/* Reliable messages that became deliverable, in sequence order. */
	std::vector<reliable_message> reliable;
	/* Every unreliable chunk of the packet, in packet order, except
	 * `state`/`input` chunks from a packet older than one already seen
	 * (latest wins across packets; all chunks of one packet count as
	 * equally new, so a bundle split into two chunks arrives whole).
	 * Views into the caller's datagram, see unreliable_view.
	 */
	std::vector<unreliable_view> unreliable;
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
 * for a lossless packet can arrive up to hold + tick after srtt; without
 * the slack every message on a jitter-free link would be resent as soon
 * as rttvar decays to zero.
 *
 *	rto = clamp(srtt + max(4 rttvar, hold) + tick, NET_V2_RTO_MIN, NET_V2_RTO_MAX)
 */
class rtt_estimator
{
	net_clock m_tick_period;
	net_clock m_hold_period;
	bool m_valid{};
	net_clock m_srtt{};
	net_clock m_rttvar{};
public:
	explicit rtt_estimator(const net_clock tick_period = NET_V2_DEFAULT_TICK_PERIOD, const net_clock hold_period = NET_V2_DEFAULT_TICK_PERIOD) :
		m_tick_period{tick_period},
		m_hold_period{hold_period}
	{
	}
	void add_sample(net_clock r);
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
	/* Per latest-wins chunk type and part: the newest packet that
	 * carried one.
	 */
	struct latest_packet
	{
		bool valid{};
		std::uint16_t seq{};
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
	/* Sequences of malformed packets already counted as a protocol
	 * error; a replay of one counts no further error.  Never used to
	 * reject anything: an intact copy of the sequence is accepted.
	 */
	struct recent_malformed
	{
		std::array<std::uint16_t, 16> seqs{};
		std::size_t count{};
		std::size_t next{};
	};
	struct pending_event
	{
		unreliable_chunk chunk;
		unsigned skipped{};
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
	/* The newest of our packets the peer has echoed; echoes never go
	 * backwards, so an older one is stale or forged.
	 */
	bool m_echo_seen{};
	std::uint16_t m_last_echoed_seq{};
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
	bool m_tick_open{};
	unsigned m_tick_packets{};
	/* Reliable messages were due but did not fit in the last packet. */
	bool m_tick_backlog{};
	bool m_ack_owed{};
	packet_buffer m_outgoing{};
	std::vector<out_msg *> m_carried;
	std::vector<selected_run> m_runs;
	/* The outgoing latest-wins bundles, [0] state and [1] input. */
	std::array<state_bundle, 2> m_state_out{};
	/* Alternation between a pending state part and a head message that
	 * does not fit beside it in the tick's second packet.
	 */
	bool m_yield_to_message{};
	std::deque<pending_event> m_pending_events;

	/* Receiver side */
	bool m_any_received{};
	std::uint16_t m_highest_seen{};
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
	/* When the window last went from empty to holding out-of-order
	 * messages, or last advanced; the stream_stalled clock.
	 */
	net_clock m_recv_gap_since{};
	std::array<std::array<latest_packet, NET_V2_STATE_MAX_PARTS>, 2> m_latest{};
	recent_malformed m_recent_malformed{};
	std::deque<net_clock> m_protocol_error_times;
	std::vector<parsed_message> m_parsed_messages;
	std::vector<parsed_chunk> m_parsed_chunks;

	/* Estimators */
	rtt_estimator m_rtt;
	clock_sync m_clock;
	double m_loss_estimate{};

	connection_stats m_stats;

	void close_with(close_reason reason);
	void check_timeouts(net_clock now);
	void detect_rto_losses(net_clock now);
	void flag_resend(out_msg &m);
	void process_acks(std::uint16_t ack, std::uint64_t ack_bits);
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
	latest_packet &latest_for(chunk_type type, unsigned part);
	[[nodiscard]]
	bool any_state_pending() const;
	/* Bytes of the pending state parts that the next packet carries: in
	 * order, as far as they fit in an otherwise empty payload.
	 */
	[[nodiscard]]
	std::size_t reserved_state_bytes() const;
	void write_state_parts(std::uint8_t *buf, std::size_t &pos);
	/* §3.7 step 8.  Fills m_parsed_messages and m_parsed_chunks. */
	[[nodiscard]]
	bool validate_chunks(std::span<const std::uint8_t> payload, std::uint8_t flags);
	void deliver_reliable(net_clock now, receive_report &report);
	void deliver_unreliable(std::uint16_t packet_seq, receive_report &report);
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
	 * part numbers, are ignored and counted as dropped.
	 */
	void set_unreliable_state(chunk_type type, std::span<const std::uint8_t> payload);
	void set_unreliable_state(chunk_type type, unsigned part, unsigned part_count, std::span<const std::uint8_t> payload);

	/* A best-effort `event_u` chunk: sent once in the next packet with
	 * room for it, never retransmitted.  An event that does not fit does
	 * not hold up the ones behind it and is dropped after
	 * NET_V2_EVENT_SKIP_MAX packets.  Returns false if it was dropped at
	 * once; `state`/`input` are refused (use set_unreliable_state).
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
	 * config.max_packets_per_tick only while reliable messages remain
	 * that did not fit.  A grant closes the tick that was open, so an
	 * unfinished allowance is never spent on top of the new one.
	 * build_outgoing calls this itself; calling it explicitly at the
	 * tick merely documents the tick.
	 */
	void begin_tick(net_clock now);

	/* Build the next datagram to send, or return an empty span if nothing
	 * is due or the tick's packet budget is spent.  A packet is due when
	 * there is a pending state chunk, a pending event, a reliable message
	 * to send or resend, an ack owed for a received reliable message, or
	 * NET_V2_KEEPALIVE_INTERVAL has passed since the last packet.  Call
	 * repeatedly until it returns empty: the second packet of a tick
	 * carries only reliable messages that did not fit beside the state
	 * chunk.  The span is valid until the next call.
	 */
	[[nodiscard]]
	std::span<const std::uint8_t> build_outgoing(net_clock now);

	/* Validate one received datagram (section 3.7) and apply it. */
	[[nodiscard]]
	receive_report on_receive(std::span<const std::uint8_t> datagram, net_clock now);

	/* Advance the clock slew and, once per tick period, the timeouts
	 * and the RTO loss detection (begin_tick), without building a packet.
	 * build_outgoing does this too.
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

	/* Peer time for a local time, once the clock offset is known. */
	[[nodiscard]]
	net_clock to_peer_time(const net_clock local) const
	{
		return local + m_clock.offset();
	}
};

}

}
