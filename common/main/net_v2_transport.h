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

struct connection_config
{
	std::uint32_t session_id{};
	std::uint32_t peer_token{};
	std::uint8_t local_player_id{NET_V2_PLAYER_ID_NONE};
	std::uint8_t remote_player_id{NET_V2_PLAYER_ID_NONE};
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
	/* Header applied (acks, RTT, last_heard), but a chunk was malformed:
	 * nothing from the first bad chunk on was delivered and a protocol
	 * error was counted.
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

struct receive_report
{
	receive_status status{receive_status::bad_length};
	/* Reliable messages that became deliverable, in sequence order. */
	std::vector<reliable_message> reliable;
	/* `state`/`input` chunks newer than any seen before, and every
	 * `event_u` chunk, in packet order.
	 */
	std::vector<unreliable_chunk> unreliable;
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

/* Section 3.5: Jacobson/Karels with RFC 6298 constants. */
class rtt_estimator
{
	bool m_valid{};
	net_clock m_srtt{};
	net_clock m_rttvar{};
public:
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
 */
class clock_sync
{
	struct sample
	{
		net_clock at;
		net_clock rtt;
		net_clock offset;
	};
	std::deque<sample> m_samples;
	bool m_valid{};
	net_clock m_offset{};
	net_clock m_target{};
	net_clock m_first_sample{};
	net_clock m_last_update{};
	net_clock m_slew_remainder{};
	void expire(net_clock now);
	void retarget();
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
	[[nodiscard]]
	std::size_t sample_count() const
	{
		return m_samples.size();
	}
};

class connection
{
	struct out_msg
	{
		std::uint16_t seq{};
		std::uint8_t type{};
		std::uint8_t sends{};
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
		/* False if the packet carried a retransmission. */
		bool clean{};
		std::uint16_t seq{};
		net_clock sent_at{};
		std::vector<std::uint16_t> msg_seqs;
	};
	struct recv_slot
	{
		bool filled{};
		std::uint8_t type{};
		std::vector<std::uint8_t> payload;
	};
	struct latest_unreliable
	{
		bool valid{};
		std::uint16_t packet_seq{};
		std::vector<std::uint8_t> payload;
	};
	struct parsed_chunk
	{
		chunk_type type;
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
	 * m_held_acked counts those.
	 */
	std::deque<out_msg> m_messages;
	std::size_t m_held_acked{};
	std::size_t m_queue_bytes{};
	std::size_t m_in_flight{};
	std::array<packet_log_entry, NET_V2_RECV_WINDOW> m_packet_log{};
	net_clock m_last_sent{};
	bool m_ack_owed{};
	packet_buffer m_outgoing{};
	std::optional<unreliable_chunk> m_pending_state;
	std::deque<unreliable_chunk> m_pending_events;

	/* Receiver side */
	bool m_any_received{};
	std::uint16_t m_highest_seen{};
	std::uint64_t m_ack_bits{};
	net_clock m_last_heard{};
	net_time m_last_recv_send_time{};
	net_clock m_last_recv_local_time{};
	std::uint16_t m_next_expected{};
	std::array<recv_slot, NET_V2_RECV_WINDOW> m_recv_window{};
	std::size_t m_recv_window_pending{};
	std::array<latest_unreliable, 8> m_latest_unreliable{};
	std::deque<net_clock> m_protocol_error_times;

	/* Estimators */
	rtt_estimator m_rtt;
	clock_sync m_clock;
	double m_loss_estimate{};

	connection_stats m_stats;

	void close_with(close_reason reason);
	void check_timeouts(net_clock now);
	void detect_rto_losses(net_clock now);
	void process_acks(std::uint16_t ack, std::uint64_t ack_bits, net_clock now, bool take_rtt_samples);
	void resolve_packet(packet_log_entry &e, bool acked);
	void pop_acked_messages();
	[[nodiscard]]
	bool window_allows(const out_msg &m) const;
	[[nodiscard]]
	bool any_message_due() const;
	[[nodiscard]]
	bool count_protocol_error(net_clock now);
	[[nodiscard]]
	bool validate_chunks(std::span<const std::uint8_t> payload, std::uint8_t flags, std::vector<parsed_chunk> &chunks) const;
	void deliver_reliable_run(std::span<const std::uint8_t> run, receive_report &report);
	void deliver_unreliable(const parsed_chunk &chunk, std::uint16_t packet_seq, receive_report &report);
	[[nodiscard]]
	std::uint16_t next_local_seq();
public:
	connection(const connection_config &config, net_clock now);

	/* Queue one reliable message.  Payloads above NET_V2_MAX_MESSAGE are
	 * rejected; the queue bounds of section 3.6 close the connection.
	 */
	enqueue_result enqueue_reliable(std::uint8_t type, std::span<const std::uint8_t> payload);

	/* The latest-wins chunk of this tick (`state` or `input`).  It is
	 * carried by the next packet built and then forgotten; calling again
	 * before that replaces it.  Payloads above NET_V2_MAX_CHUNK_PAYLOAD
	 * are ignored and counted as dropped.
	 */
	void set_unreliable_state(chunk_type type, std::span<const std::uint8_t> payload);

	/* A best-effort chunk (`event_u`): sent once in the next packet with
	 * room for it, never retransmitted.  Returns false if it was dropped.
	 */
	bool send_unreliable(chunk_type type, std::span<const std::uint8_t> payload);

	/* Build the next datagram to send, or return an empty span if nothing
	 * is due.  A packet is due when there is a pending state chunk, a
	 * pending event, a reliable message to send or resend, an ack owed for
	 * a received reliable message, or NET_V2_KEEPALIVE_INTERVAL has passed
	 * since the last packet.  Call repeatedly until it returns empty: the
	 * second packet of a tick carries only reliable messages that did not
	 * fit beside the state chunk.  The span is valid until the next call.
	 */
	[[nodiscard]]
	std::span<const std::uint8_t> build_outgoing(net_clock now);

	/* Validate one received datagram (section 3.7) and apply it. */
	[[nodiscard]]
	receive_report on_receive(std::span<const std::uint8_t> datagram, net_clock now);

	/* Run the timeouts and the clock slew without building a packet.
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

	/* The newest `state` or `input` chunk received, if any. */
	[[nodiscard]]
	std::optional<std::span<const std::uint8_t>> latest_received(chunk_type type) const;

	/* Peer time for a local time, once the clock offset is known. */
	[[nodiscard]]
	net_clock to_peer_time(const net_clock local) const
	{
		return local + m_clock.offset();
	}
};

}

}
