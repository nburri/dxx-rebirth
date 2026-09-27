/*
 * This file is part of the DXX-Rebirth project <https://github.com/dxx-rebirth/dxx-rebirth/>.
 * It is copyright by its individual contributors, as recorded in the
 * project's Git history.  See COPYING.txt at the top level for license
 * terms and a link to the Git history.
 */
/*
 * Multiplayer network protocol v2: transport layer implementation.
 * See net_v2_transport.h and Documentation/network-protocol-v2.md §3.
 */

#include <algorithm>
#include <cstdlib>
#include <utility>

#include "net_v2_transport.h"

namespace dcx {

namespace net_v2 {

namespace {

constexpr std::uint16_t NET_V2_ECHO_DELAY_SATURATED{0xffff};
/* Ticks granted at once, at most (a frame that spanned two periods). */
constexpr unsigned NET_V2_TICK_GRANT_MAX{2};
/* RTT samples above this are noise (wrapped or stalled clocks). */
constexpr net_clock NET_V2_RTT_SAMPLE_MAX{net_seconds(10)};
/* Smoothing of the loss estimate, per resolved packet: 1/64, about one
 * second of packets at 60 pps.
 */
constexpr int NET_V2_LOSS_EWMA_SHIFT{6};

[[nodiscard]]
constexpr std::size_t message_wire_size(const std::size_t payload_size)
{
	return NET_V2_MESSAGE_HEADER_SIZE + payload_size;
}

[[nodiscard]]
constexpr std::size_t chunk_wire_size(const std::size_t payload_size)
{
	return NET_V2_CHUNK_HEADER_SIZE + payload_size;
}

[[nodiscard]]
constexpr bool is_latest_wins(const chunk_type type)
{
	return type == chunk_type::state || type == chunk_type::input;
}

}

/* rtt_estimator (§3.5) */

void rtt_estimator::add_sample(const net_clock r)
{
	if (!m_valid)
	{
		m_valid = true;
		m_srtt = r;
		m_rttvar = r / 2;
		return;
	}
	const auto err{m_srtt > r ? m_srtt - r : r - m_srtt};
	m_rttvar = (3 * m_rttvar + err) / 4;
	m_srtt = (7 * m_srtt + r) / 8;
}

net_clock rtt_estimator::rto() const
{
	if (!m_valid)
		return NET_V2_RTO_MAX;
	return std::clamp(m_srtt + std::max(4 * m_rttvar, m_hold_period) + m_tick_period, NET_V2_RTO_MIN, NET_V2_RTO_MAX);
}

/* clock_sync (§2.2) */

bool clock_sync::expire(const net_clock now)
{
	bool any{};
	while (!m_candidates.empty() && m_candidates.front().at + NET_V2_CLOCK_SAMPLE_WINDOW < now)
	{
		m_candidates.pop_front();
		any = true;
	}
	return any;
}

void clock_sync::add_sample(const net_clock now, const net_clock rtt, const net_clock offset)
{
	/* Older candidates with an RTT no better than this one can never be
	 * the window minimum again: they expire first.
	 */
	while (!m_candidates.empty() && m_candidates.back().rtt >= rtt)
		m_candidates.pop_back();
	m_candidates.push_back({.at = now, .rtt = rtt, .offset = offset});
	expire(now);
	m_target = m_candidates.front().offset;
	if (!m_valid)
	{
		m_valid = true;
		m_offset = m_target;
		m_first_sample = now;
		m_last_update = now;
	}
}

void clock_sync::update(const net_clock now)
{
	if (!m_valid)
		return;
	if (expire(now) && !m_candidates.empty())
		m_target = m_candidates.front().offset;
	const auto elapsed{now - m_last_update};
	if (elapsed <= 0)
		return;
	m_last_update = now;
	const auto diff{m_target - m_offset};
	const auto magnitude{diff < 0 ? -diff : diff};
	if (magnitude > NET_V2_CLOCK_JUMP_THRESHOLD || now - m_first_sample < NET_V2_CLOCK_SAMPLE_WINDOW)
	{
		m_offset = m_target;
		return;
	}
	/* Carry the sub-unit remainder so that short ticks still add up to
	 * the full slew rate.
	 */
	const auto scaled{elapsed * NET_V2_CLOCK_SLEW_PER_SECOND + m_slew_remainder};
	m_slew_remainder = scaled % net_seconds(1);
	const auto step{std::min(magnitude, scaled / net_seconds(1))};
	m_offset += diff < 0 ? -step : step;
}

/* connection */

namespace {

[[nodiscard]]
connection_config sanitized(connection_config config)
{
	/* A zero period would divide by zero in begin_tick; a zero budget
	 * could never send.
	 */
	config.tick_period = std::max<net_clock>(config.tick_period, 1);
	/* Two at least: the second packet of a tick is what carries a
	 * reliable message that does not fit beside the state chunk.
	 */
	config.max_packets_per_tick = std::max(config.max_packets_per_tick, 2u);
	if (config.peer_tick_period < 1)
		config.peer_tick_period = config.tick_period;
	return config;
}

}

connection::connection(const connection_config &config, const net_clock now) :
	m_config{sanitized(config)},
	/* Make the first build_outgoing produce a packet at once. */
	m_last_sent{now - NET_V2_KEEPALIVE_INTERVAL},
	/* One full period behind, so the first begin_tick grants a budget. */
	m_tick_start{now - m_config.tick_period},
	m_last_heard{now},
	/* The ack hold is the longer of the two periods: the peer holds our
	 * packet's ack until its tick, and its ack rides to us in a packet we
	 * only look at on ours.
	 */
	m_rtt{m_config.tick_period, std::max(m_config.tick_period, m_config.peer_tick_period)}
{
}

void connection::close_with(const close_reason reason)
{
	if (m_state == connection_state::closed)
		return;
	m_state = connection_state::closed;
	m_close_reason = reason;
}

void connection::close()
{
	close_with(close_reason::local);
}

std::uint16_t connection::next_local_seq()
{
	/* Sequence 0 is never used, so that `ack == 0 && ack_bits == 0`
	 * means "nothing received yet" (§3.1 starts at 1).
	 */
	if (++m_local_seq == 0)
		++m_local_seq;
	return m_local_seq;
}

enqueue_result connection::enqueue_reliable(const std::uint8_t type, const std::span<const std::uint8_t> payload)
{
	if (m_state == connection_state::closed)
		return enqueue_result::closed;
	if (payload.size() > NET_V2_MAX_MESSAGE)
		return enqueue_result::too_large;
	if (m_messages.size() - m_held_acked >= NET_V2_QUEUE_MAX_MESSAGES || m_queue_bytes + payload.size() > NET_V2_QUEUE_MAX_BYTES)
	{
		close_with(close_reason::queue_overflow);
		return enqueue_result::queue_overflow;
	}
	auto &m{m_messages.emplace_back()};
	m.seq = m_next_msg_seq++;
	m.type = type;
	m.payload.assign(payload.begin(), payload.end());
	m_queue_bytes += payload.size();
	++m_stats.messages_enqueued;
	return enqueue_result::ok;
}

void connection::set_unreliable_state(const chunk_type type, const std::span<const std::uint8_t> payload)
{
	if (payload.size() > NET_V2_MAX_CHUNK_PAYLOAD)
	{
		++m_stats.unreliable_dropped;
		return;
	}
	if (m_state_pending)
		/* Replaced before it was sent. */
		++m_stats.unreliable_dropped;
	m_state_pending = true;
	m_state_type = type;
	m_state_size = payload.size();
	std::ranges::copy(payload, m_state_buffer.data());
}

bool connection::send_unreliable(const chunk_type type, const std::span<const std::uint8_t> payload)
{
	if (payload.size() > NET_V2_MAX_CHUNK_PAYLOAD)
	{
		++m_stats.unreliable_dropped;
		return false;
	}
	if (m_pending_events.size() >= NET_V2_EVENT_QUEUE_MAX)
	{
		m_pending_events.pop_front();
		++m_stats.unreliable_dropped;
	}
	m_pending_events.push_back({.chunk = {.type = type, .payload = {payload.begin(), payload.end()}}});
	return true;
}

bool connection::window_allows(const out_msg &m) const
{
	/* The queue head is the oldest message that is unacked or unsent
	 * (acked heads are popped).  The peer's window starts at its next
	 * expected sequence, which is at or after that, so staying within
	 * the window of the head is always safe, and it also bounds what one
	 * packet can select when nothing is in flight yet.
	 */
	return seq_diff(m.seq, m_messages.front().seq) < static_cast<std::int16_t>(NET_V2_MAX_IN_FLIGHT);
}

bool connection::any_message_due() const
{
	if (m_resend_pending != 0)
		return true;
	return m_sent_count != m_messages.size() && window_allows(m_messages[m_sent_count]);
}

void connection::check_timeouts(const net_clock now)
{
	if (m_state == connection_state::closed)
		return;
	if (now - m_last_heard >= NET_V2_TIMEOUT)
	{
		close_with(close_reason::timeout);
		return;
	}
	/* Only the oldest unacked message matters. */
	for (const auto &m : m_messages)
	{
		if (m.acked)
			continue;
		if (m.sent && now - m.first_sent >= NET_V2_UNACKED_TIMEOUT)
			close_with(close_reason::unacked_timeout);
		break;
	}
	/* The receive side of the same limit: messages held out of order
	 * whose gap the peer never fills although it keeps sending.
	 */
	if (m_recv_window_pending != 0 && now - m_recv_gap_since >= NET_V2_UNACKED_TIMEOUT)
		close_with(close_reason::stream_stalled);
}

void connection::flag_resend(out_msg &m)
{
	if (m.resend)
		return;
	m.resend = true;
	++m_resend_pending;
}

void connection::detect_rto_losses(const net_clock now)
{
	const auto rto{m_rtt.rto()};
	for (std::size_t i{}; i != m_sent_count; ++i)
	{
		auto &m{m_messages[i]};
		if (m.acked || m.resend)
			continue;
		if (now - m.last_sent >= rto)
		{
			flag_resend(m);
			++m_stats.resends_by_rto;
		}
	}
}

void connection::update(const net_clock now)
{
	m_clock.update(now);
	begin_tick(now);
}

void connection::begin_tick(const net_clock now)
{
	const auto elapsed{now - m_tick_start};
	if (elapsed < m_config.tick_period)
		return;
	const auto ticks{elapsed / m_config.tick_period};
	m_tick_start += ticks * m_config.tick_period;
	m_tick_credit = static_cast<unsigned>(std::min<net_clock>(ticks, NET_V2_TICK_GRANT_MAX));
	/* Whatever was left of the previous tick is not carried on top. */
	m_tick_open = false;
	m_tick_packets = 0;
	/* Timeouts and the RTO are judged once per tick: a retransmission
	 * cannot go out more often anyway.
	 */
	check_timeouts(now);
	detect_rto_losses(now);
}

std::span<const std::uint8_t> connection::build_outgoing(const net_clock now)
{
	update(now);
	if (m_state == connection_state::closed)
		return {};
	/* §3.6: one packet per tick; a further one, up to
	 * max_packets_per_tick, only while reliable messages remain that did
	 * not fit.  Otherwise the next packet needs the next tick (update()
	 * above has run begin_tick).  The tick is opened, and its credit
	 * spent, only once a packet is really built.
	 */
	const bool header_due{m_state_pending || !m_pending_events.empty() || m_ack_owed || now - m_last_sent >= NET_V2_KEEPALIVE_INTERVAL};
	const bool messages_due{any_message_due()};
	if (!header_due && !messages_due)
		return {};
	const bool opens_tick{!m_tick_open || m_tick_packets >= m_config.max_packets_per_tick || !m_tick_backlog};
	if (opens_tick && m_tick_credit == 0)
		return {};
	const unsigned packets_in_tick{opens_tick ? 0u : m_tick_packets};

	/* Select the reliable messages: resends first, then new ones, each
	 * only if it fits, stopping at the first that does not (§3.4).  The
	 * selection is recorded as runs of consecutive sequences, one
	 * RELIABLE chunk each, so that the budget and the layout agree.
	 */
	std::size_t budget{};
	std::size_t resend_bytes{};
	const auto try_add{[&](out_msg &m, const bool is_resend) {
		const auto msg_size{message_wire_size(m.payload.size())};
		/* The resend budget is cumulative; the first resent message of a
		 * packet may exceed it, or a message above 600 bytes could never
		 * be retransmitted.
		 */
		if (is_resend && resend_bytes != 0 && resend_bytes + msg_size > NET_V2_RESEND_BUDGET)
			return false;
		const bool new_run{m_runs.empty() || seq_diff(m.seq, m_carried.back()->seq) != 1 || m_runs.back().count == NET_V2_MAX_RUN_COUNT};
		const auto cost{msg_size + (new_run ? NET_V2_CHUNK_HEADER_SIZE + NET_V2_RELIABLE_RUN_HEADER_SIZE : 0)};
		if (cost > budget)
			return false;
		budget -= cost;
		if (is_resend)
			resend_bytes += msg_size;
		if (new_run)
			m_runs.push_back({.first = m_carried.size(), .count = 1});
		else
			++m_runs.back().count;
		m_carried.push_back(&m);
		return true;
	}};
	const auto select{[&](const std::size_t reserved) {
		budget = NET_V2_MAX_PACKET - NET_V2_HEADER_SIZE - reserved;
		resend_bytes = 0;
		m_carried.clear();
		m_runs.clear();
		if (m_resend_pending != 0)
		{
			for (std::size_t i{}; i != m_sent_count; ++i)
			{
				auto &m{m_messages[i]};
				if (m.acked || !m.resend)
					continue;
				if (!try_add(m, true))
					break;
			}
		}
		for (auto i{m_sent_count}; i != m_messages.size(); ++i)
		{
			auto &m{m_messages[i]};
			if (!window_allows(m) || !try_add(m, false))
				break;
		}
	}};
	const std::size_t state_size{m_state_pending ? chunk_wire_size(m_state_size) : 0};
	select(state_size);
	/* The first packet of a tick always carries the state chunk.  A head
	 * message that does not fit beside it rides the tick's second packet
	 * (max_packets_per_tick is at least 2), which normally has no state
	 * left to carry; only if a state was set again in between does that
	 * packet omit it, so that the message is never starved.
	 */
	const bool omit_state{m_carried.empty() && messages_due && state_size != 0 && packets_in_tick != 0};
	if (omit_state)
		select(0);
	if (!header_due && m_carried.empty())
		/* Something is due but did not fit this time; never emit an
		 * empty packet for it, or the caller's send loop would not end.
		 */
		return {};
	if (opens_tick)
	{
		--m_tick_credit;
		m_tick_open = true;
		m_tick_packets = 0;
	}

	/* Header is written last; chunks first. */
	auto *const buf{m_outgoing.data()};
	std::size_t pos{NET_V2_HEADER_SIZE};
	const auto seq{next_local_seq()};
	auto &log{m_packet_log[seq % NET_V2_RECV_WINDOW]};
	if (log.valid && !log.acked && !log.lost)
		/* 256 packets later and never acked: lost. */
		resolve_packet(log, false);
	log.msg_seqs.clear();
	/* The loss scan cannot usefully look further back than the log
	 * holds; keep it in range so that a long ack blackout never leaves
	 * it more than half a sequence space behind, where seq_diff would
	 * turn negative and stop it for good.
	 */
	const auto oldest_logged{static_cast<std::uint16_t>(seq - (NET_V2_RECV_WINDOW - 1))};
	if (seq_diff(m_lost_scan_seq, oldest_logged) < 0)
	{
		m_lost_scan_seq = oldest_logged;
		if (m_lost_scan_seq == 0)
			++m_lost_scan_seq;
	}
	for (const auto &run : m_runs)
	{
		const auto chunk_at{pos};
		pos += NET_V2_CHUNK_HEADER_SIZE;
		net_put_le16(buf + pos, m_carried[run.first]->seq);
		buf[pos + 2] = static_cast<std::uint8_t>(run.count);
		pos += NET_V2_RELIABLE_RUN_HEADER_SIZE;
		for (auto j{run.first}; j != run.first + run.count; ++j)
		{
			auto &m{*m_carried[j]};
			buf[pos] = m.type;
			net_put_le16(buf + pos + 1, static_cast<std::uint16_t>(m.payload.size()));
			pos += NET_V2_MESSAGE_HEADER_SIZE;
			std::ranges::copy(m.payload, buf + pos);
			pos += m.payload.size();
			if (m.sent)
			{
				/* A retransmission: it was flagged. */
				m.resend = false;
				--m_resend_pending;
				++m_stats.message_resends;
			}
			else
			{
				m.sent = true;
				m.first_sent = now;
				++m_sent_count;
			}
			m.in_packet_seq = seq;
			m.last_sent = now;
			++m_stats.message_sends;
			log.msg_seqs.push_back(m.seq);
		}
		chunk_header{.type = static_cast<std::uint8_t>(chunk_type::reliable), .length = static_cast<std::uint16_t>(pos - chunk_at - NET_V2_CHUNK_HEADER_SIZE)}.write(buf + chunk_at);
	}
	if (m_state_pending && !omit_state)
	{
		chunk_header{.type = static_cast<std::uint8_t>(m_state_type), .length = static_cast<std::uint16_t>(m_state_size)}.write(buf + pos);
		pos += NET_V2_CHUNK_HEADER_SIZE;
		std::copy_n(m_state_buffer.data(), m_state_size, buf + pos);
		pos += m_state_size;
		m_state_pending = false;
	}
	/* Events: every one that fits, in queue order; one that does not fit
	 * is skipped (not a head-of-line block) and dropped once it has been
	 * skipped NET_V2_EVENT_SKIP_MAX times.
	 */
	std::erase_if(m_pending_events, [&](pending_event &e) {
		const auto &c{e.chunk};
		if (chunk_wire_size(c.payload.size()) > NET_V2_MAX_PACKET - pos)
		{
			if (++e.skipped < NET_V2_EVENT_SKIP_MAX)
				return false;
			++m_stats.unreliable_dropped;
			return true;
		}
		chunk_header{.type = static_cast<std::uint8_t>(c.type), .length = static_cast<std::uint16_t>(c.payload.size())}.write(buf + pos);
		pos += NET_V2_CHUNK_HEADER_SIZE;
		std::ranges::copy(c.payload, buf + pos);
		pos += c.payload.size();
		return true;
	});

	packet_header h;
	h.session_id = m_config.session_id;
	h.peer_token = m_config.peer_token;
	h.player_id = m_config.local_player_id;
	if (!m_carried.empty())
		h.flags |= static_cast<std::uint8_t>(packet_flag::has_reliable);
	if (pos == NET_V2_HEADER_SIZE)
		h.flags |= static_cast<std::uint8_t>(packet_flag::keepalive);
	h.seq = seq;
	h.send_time = to_net_time(now);
	if (m_any_received)
	{
		h.ack = m_highest_seen;
		h.ack_bits = m_ack_bits;
		h.echo_seq = m_highest_seen;
		h.echo_time = m_last_recv_send_time;
		/* A caller whose build time is behind its receive time (clock
		 * read before the socket) must not wrap into a huge delay.
		 */
		const auto delay{std::clamp<net_clock>(now - m_last_recv_local_time, 0, NET_V2_ECHO_DELAY_SATURATED)};
		h.echo_delay = static_cast<std::uint16_t>(delay);
	}
	h.write(buf);

	log.valid = true;
	log.acked = false;
	log.lost = false;
	log.seq = seq;
	log.sent_at = now;
	m_last_sent = now;
	++m_tick_packets;
	/* Judged now, not at the next call: messages queued in between
	 * belong to the next tick.
	 */
	m_tick_backlog = any_message_due();
	m_ack_owed = false;
	++m_stats.packets_sent;
	return {buf, pos};
}

void connection::resolve_packet(packet_log_entry &e, const bool acked)
{
	e.acked = acked;
	e.lost = !acked;
	m_loss_estimate += ((acked ? 0.0 : 1.0) - m_loss_estimate) / (1 << NET_V2_LOSS_EWMA_SHIFT);
	if (!acked)
	{
		++m_stats.packets_lost;
		return;
	}
	++m_stats.packets_acked;
	if (m_messages.empty())
		return;
	const auto front_seq{m_messages.front().seq};
	for (const auto s : e.msg_seqs)
	{
		const auto d{seq_diff(s, front_seq)};
		if (d < 0)
			continue;
		const auto idx{static_cast<std::size_t>(d)};
		if (idx >= m_messages.size())
			continue;
		auto &m{m_messages[idx]};
		if (m.seq != s || m.acked || !m.sent)
			continue;
		m.acked = true;
		if (m.resend)
		{
			m.resend = false;
			--m_resend_pending;
		}
		++m_held_acked;
		m_queue_bytes -= m.payload.size();
	}
}

void connection::pop_acked_messages()
{
	while (!m_messages.empty() && m_messages.front().acked)
	{
		m_messages.pop_front();
		--m_held_acked;
		/* Acked implies sent. */
		--m_sent_count;
	}
}

/* The caller has checked that `ack` names a packet we sent.  No RTT is
 * taken from acks: the ack for a packet is held until the peer's next
 * tick and may ride a reordered packet, so only the echo fields, which
 * account for the hold, feed the estimator.
 */
void connection::process_acks(const std::uint16_t ack, const std::uint64_t ack_bits)
{
	if (ack == 0 && ack_bits == 0)
		/* The peer has not received anything from us yet. */
		return;
	for (unsigned i{}; i <= NET_V2_ACK_BITS; ++i)
	{
		if (i != 0 && !(ack_bits & (std::uint64_t{1} << (i - 1))))
			continue;
		const auto s{static_cast<std::uint16_t>(ack - i)};
		auto &e{m_packet_log[s % NET_V2_RECV_WINDOW]};
		if (!e.valid || e.seq != s || e.acked || e.lost)
			continue;
		resolve_packet(e, true);
	}
	/* Packets that fell out of the ack bitfield unacked are lost.  Scan
	 * forward from where the previous ack left off, never past the last
	 * packet sent.
	 */
	while (seq_diff(ack, m_lost_scan_seq) > static_cast<std::int16_t>(NET_V2_ACK_BITS) && seq_diff(m_local_seq, m_lost_scan_seq) >= 0)
	{
		auto &e{m_packet_log[m_lost_scan_seq % NET_V2_RECV_WINDOW]};
		if (e.valid && e.seq == m_lost_scan_seq && !e.acked && !e.lost)
			resolve_packet(e, false);
		if (++m_lost_scan_seq == 0)
			++m_lost_scan_seq;
	}
	pop_acked_messages();
	/* Gap rule: a message whose packet is 3 or more behind the highest
	 * acked packet is retransmitted without waiting for the RTO.
	 */
	for (std::size_t i{}; i != m_sent_count; ++i)
	{
		auto &m{m_messages[i]};
		if (m.acked || m.resend)
			continue;
		if (seq_diff(ack, m.in_packet_seq) >= static_cast<std::int16_t>(NET_V2_GAP_LOSS_THRESHOLD))
		{
			flag_resend(m);
			++m_stats.resends_by_gap;
		}
	}
}

bool connection::count_protocol_error(const std::uint16_t seq, const net_clock now)
{
	auto &r{m_recent_malformed};
	/* Sequences older than the reorder window cannot be replayed into
	 * acceptance anyway; forget them so the list follows the stream.
	 */
	for (std::size_t i{}; i != r.count;)
	{
		if (m_any_received && seq_diff(m_highest_seen, r.seqs[i]) > static_cast<std::int16_t>(NET_V2_ACK_BITS))
		{
			r.seqs[i] = r.seqs[--r.count];
			r.next = r.count % r.seqs.size();
		}
		else
			++i;
	}
	for (std::size_t i{}; i != r.count; ++i)
		if (r.seqs[i] == seq)
			/* A replay of a packet already counted. */
			return false;
	r.seqs[r.next] = seq;
	r.next = (r.next + 1) % r.seqs.size();
	if (r.count != r.seqs.size())
		++r.count;
	++m_stats.protocol_errors;
	m_protocol_error_times.push_back(now);
	while (m_protocol_error_times.front() + NET_V2_PROTOCOL_ERROR_WINDOW <= now)
		m_protocol_error_times.pop_front();
	return m_protocol_error_times.size() >= NET_V2_PROTOCOL_ERROR_LIMIT;
}

/* §3.7 step 8: every chunk must fit, every reliable message must fit its
 * chunk and its sequence must be inside the receive window, the flags
 * must describe the content.  Nothing is applied here; the parsed
 * records are kept for deliver_reliable / deliver_unreliable.
 */
bool connection::validate_chunks(const std::span<const std::uint8_t> payload, const std::uint8_t flags)
{
	m_parsed_messages.clear();
	m_parsed_chunks.clear();
	std::size_t pos{};
	unsigned reliable_chunks{};
	unsigned chunks{};
	while (pos != payload.size())
	{
		if (payload.size() - pos < NET_V2_CHUNK_HEADER_SIZE)
			return false;
		const auto ch{chunk_header::read(payload.data() + pos)};
		pos += NET_V2_CHUNK_HEADER_SIZE;
		if (ch.length > payload.size() - pos)
			return false;
		const std::span<const std::uint8_t> body{payload.data() + pos, ch.length};
		pos += ch.length;
		++chunks;
		const auto type{static_cast<chunk_type>(ch.type)};
		switch (type)
		{
			case chunk_type::reliable:
			{
				if (body.size() < NET_V2_RELIABLE_RUN_HEADER_SIZE)
					return false;
				auto seq{net_get_le16(body.data())};
				const unsigned count{body[2]};
				if (count == 0)
					return false;
				std::size_t at{NET_V2_RELIABLE_RUN_HEADER_SIZE};
				for (unsigned i{}; i != count; ++i, ++seq)
				{
					if (body.size() - at < NET_V2_MESSAGE_HEADER_SIZE)
						return false;
					const auto msg_type{body[at]};
					const auto len{net_get_le16(body.data() + at + 1)};
					at += NET_V2_MESSAGE_HEADER_SIZE;
					if (len > NET_V2_MAX_MESSAGE || len > body.size() - at)
						return false;
					if (seq_diff(seq, m_next_expected) >= static_cast<std::int16_t>(NET_V2_RECV_WINDOW))
						/* Beyond the window: a conforming sender never
						 * does this (§3.4).
						 */
						return false;
					m_parsed_messages.push_back({.seq = seq, .type = msg_type, .payload = body.subspan(at, len)});
					at += len;
				}
				if (at != body.size())
					return false;
				++reliable_chunks;
				break;
			}
			case chunk_type::state:
			case chunk_type::input:
			case chunk_type::event_u:
				/* Opaque to the transport; record sizes are checked by
				 * the consumer (stage 2).
				 */
				m_parsed_chunks.push_back({.type = type, .payload = body});
				break;
			case chunk_type::session:
				/* Only valid with flags.UNCONNECTED, which never reaches
				 * a connection.
				 */
				return false;
			default:
				return false;
		}
	}
	const bool has_reliable{(flags & static_cast<std::uint8_t>(packet_flag::has_reliable)) != 0};
	const bool keepalive{(flags & static_cast<std::uint8_t>(packet_flag::keepalive)) != 0};
	if (has_reliable != (reliable_chunks != 0))
		return false;
	if (keepalive != (chunks == 0))
		return false;
	return true;
}

void connection::deliver_reliable(const net_clock now, receive_report &report)
{
	if (m_parsed_messages.empty())
		return;
	const bool had_pending{m_recv_window_pending != 0};
	for (const auto &pm : m_parsed_messages)
	{
		if (seq_diff(pm.seq, m_next_expected) < 0)
			/* Already delivered. */
			continue;
		auto &slot{m_recv_window[pm.seq % NET_V2_RECV_WINDOW]};
		if (slot.filled)
			continue;
		slot.filled = true;
		slot.type = pm.type;
		slot.payload.assign(pm.payload.begin(), pm.payload.end());
		++m_recv_window_pending;
	}
	for (;;)
	{
		auto &slot{m_recv_window[m_next_expected % NET_V2_RECV_WINDOW]};
		if (!slot.filled)
			break;
		report.reliable.push_back({.type = slot.type, .payload = std::move(slot.payload)});
		slot.payload.clear();
		slot.filled = false;
		--m_recv_window_pending;
		++m_next_expected;
		++m_stats.messages_delivered;
	}
	/* The stall clock restarts whenever the window was empty before or
	 * the stream advanced now.
	 */
	if (!had_pending || !report.reliable.empty())
		m_recv_gap_since = now;
	m_ack_owed = true;
}

connection::latest_packet &connection::latest_for(const chunk_type type)
{
	return type == chunk_type::state ? m_latest_state : m_latest_input;
}

void connection::deliver_unreliable(const std::uint16_t packet_seq, receive_report &report)
{
	for (const auto &c : m_parsed_chunks)
	{
		if (is_latest_wins(c.type))
		{
			auto &latest{latest_for(c.type)};
			/* The last packet that carried this type is a reference only
			 * while it is inside the reorder window; anything older has
			 * been superseded by every packet since, and its sequence
			 * may even have wrapped (a type not seen for 32 768 packets
			 * would otherwise be dropped for the next 32 768).
			 */
			const auto age{latest.valid ? seq_diff(m_highest_seen, latest.seq) : std::int16_t{-1}};
			const bool reference{age >= 0 && age <= static_cast<std::int16_t>(NET_V2_ACK_BITS)};
			if (reference && seq_diff(packet_seq, latest.seq) < 0)
				/* From a packet older than one already applied:
				 * reordered, drop.  Chunks of the same packet are all
				 * delivered.
				 */
				continue;
			latest.valid = true;
			latest.seq = packet_seq;
		}
		report.unreliable.push_back({.type = c.type, .payload = c.payload});
	}
}

receive_report connection::on_receive(const std::span<const std::uint8_t> datagram, const net_clock now)
{
	receive_report report;
	const auto reject{[&](const receive_status status) {
		++m_stats.packets_rejected;
		report.status = status;
		return std::move(report);
	}};
	/* §3.7 steps 1–5 */
	if (datagram.size() < NET_V2_HEADER_SIZE || datagram.size() > NET_V2_MAX_PACKET)
		return reject(receive_status::bad_length);
	const auto h{*packet_header::read(datagram)};
	if (h.proto != NET_V2_PROTO_VERSION)
		return reject(receive_status::bad_proto);
	if (h.flags & static_cast<std::uint8_t>(packet_flag::reserved_mask))
		return reject(receive_status::bad_flags);
	if (h.has_flag(packet_flag::unconnected))
		return reject(receive_status::unconnected);
	if (h.session_id != m_config.session_id)
		return reject(receive_status::bad_session);
	if (h.peer_token != m_config.peer_token)
		return reject(receive_status::bad_token);
	if (h.player_id != m_config.remote_player_id)
		return reject(receive_status::bad_player);
	if (m_state == connection_state::closed)
		return reject(receive_status::closed);
	/* Step 6: replay / reorder window.  Decide here, record below, only
	 * once the chunks have validated: a packet whose content we cannot
	 * deliver must not be acknowledged.
	 */
	std::uint64_t new_ack_bits{};
	if (m_any_received)
	{
		const auto d{seq_diff(h.seq, m_highest_seen)};
		if (d > 0)
		{
			const auto shift{static_cast<unsigned>(d)};
			if (shift > NET_V2_ACK_BITS)
				new_ack_bits = 0;
			else if (shift == NET_V2_ACK_BITS)
				new_ack_bits = std::uint64_t{1} << (NET_V2_ACK_BITS - 1);
			else
				new_ack_bits = (m_ack_bits << shift) | (std::uint64_t{1} << (shift - 1));
		}
		else
		{
			if (d == 0 || d < -static_cast<std::int16_t>(NET_V2_ACK_BITS))
				return reject(receive_status::duplicate);
			const auto bit{std::uint64_t{1} << (static_cast<unsigned>(-d) - 1)};
			if (m_ack_bits & bit)
				return reject(receive_status::duplicate);
			new_ack_bits = m_ack_bits | bit;
		}
	}
	/* An ack of a packet we have not sent is a protocol error, and it
	 * would drive the loss scan and the gap rule over everything in
	 * flight on every packet.  Counted once per sequence; a replay of
	 * the datagram is rejected the same way but counts no more.
	 */
	if (!(h.ack == 0 && h.ack_bits == 0) && seq_diff(h.ack, m_local_seq) > 0)
	{
		if (count_protocol_error(h.seq, now))
			close_with(close_reason::protocol_error);
		return reject(receive_status::bad_ack);
	}
	/* Step 8: walk the chunks.  Nothing of the datagram is applied
	 * before it is proven well-formed: a corrupt datagram's ack bits
	 * could otherwise acknowledge messages that were never delivered.
	 */
	if (!validate_chunks(datagram.subspan(NET_V2_HEADER_SIZE), h.flags))
	{
		if (count_protocol_error(h.seq, now))
			close_with(close_reason::protocol_error);
		return reject(receive_status::malformed_chunk);
	}
	/* Step 7: header effects */
	if (m_state == connection_state::connecting)
		m_state = connection_state::connected;
	m_last_heard = now;
	++m_stats.packets_received;
	if (h.echo_seq != 0 && h.echo_delay != NET_V2_ECHO_DELAY_SATURATED)
	{
		/* Only an echo of a packet we really sent, with the send_time
		 * we really wrote, may feed the estimators: the game will rewind
		 * hits by this RTT.  A conforming peer echoes the packet it also
		 * names in `ack` (both are its newest), and every packet of ours
		 * is echoed once: a repeated echo_seq is the same measurement
		 * held longer, not a new one, so it yields no sample.  Together
		 * these stop a peer from pinning one old packet with a small
		 * delay to steer srtt and the clock.  The sample itself comes
		 * from our own log, in 64 bits.
		 */
		const auto &e{m_packet_log[h.echo_seq % NET_V2_RECV_WINDOW]};
		if (h.echo_seq == h.ack && e.valid && e.seq == h.echo_seq && to_net_time(e.sent_at) == h.echo_time && (!m_echo_seen || seq_diff(h.echo_seq, m_last_echoed_seq) > 0))
		{
			const net_clock rtt{now - e.sent_at - h.echo_delay};
			if (rtt >= 0 && rtt <= NET_V2_RTT_SAMPLE_MAX)
			{
				m_echo_seen = true;
				m_last_echoed_seq = h.echo_seq;
				m_rtt.add_sample(rtt);
				const net_time peer_now{h.send_time + static_cast<net_time>(rtt / 2)};
				m_clock.add_sample(now, rtt, net_time_diff(peer_now, to_net_time(now)));
			}
		}
	}
	process_acks(h.ack, h.ack_bits);
	/* Step 9: record and apply.  Only the newest packet moves the echo
	 * fields: echoing a late, reordered packet would inflate the peer's
	 * RTT sample by the reorder delay.
	 */
	const bool newest{!m_any_received || seq_diff(h.seq, m_highest_seen) > 0};
	m_any_received = true;
	m_ack_bits = new_ack_bits;
	if (newest)
	{
		m_highest_seen = h.seq;
		m_last_recv_send_time = h.send_time;
		m_last_recv_local_time = now;
	}
	deliver_reliable(now, report);
	deliver_unreliable(h.seq, report);
	report.status = receive_status::accepted;
	return report;
}

connection_stats connection::stats() const
{
	auto s{m_stats};
	s.state = m_state;
	s.closed_because = m_close_reason;
	s.rtt_valid = m_rtt.valid();
	s.srtt = m_rtt.srtt();
	s.rttvar = m_rtt.rttvar();
	s.rto = m_rtt.rto();
	s.loss_estimate = m_loss_estimate;
	s.queue_messages = m_messages.size() - m_held_acked;
	s.queue_bytes = m_queue_bytes;
	/* Sent messages form the deque's prefix; the acked ones among them
	 * are held in place until popped.
	 */
	s.in_flight = m_sent_count - m_held_acked;
	s.recv_window_pending = m_recv_window_pending;
	s.clock_offset_valid = m_clock.valid();
	s.clock_offset = m_clock.offset();
	s.clock_offset_target = m_clock.target();
	s.last_heard = m_last_heard;
	return s;
}

}

}
