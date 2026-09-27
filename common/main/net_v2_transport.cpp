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
#include <bit>
#include <cassert>
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
/* How long the RTO keeps covering the largest delay a bound reported
 * after the last bound, before that excess fades.
 */
constexpr net_clock NET_V2_BOUND_HOLD{net_seconds(1)};
/* Smoothing of the loss estimate, per resolved packet: 1/64, about one
 * second of packets at 60 pps.
 */
constexpr int NET_V2_LOSS_EWMA_SHIFT{6};
/* Malformed sequences remembered as already counted. */
constexpr std::size_t NET_V2_RECENT_MALFORMED_MAX{16};

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

/* Index into the per-type bundle arrays, and its inverse. */
[[nodiscard]]
constexpr std::size_t state_index(const chunk_type type)
{
	return type == chunk_type::input ? 1 : 0;
}

[[nodiscard]]
constexpr chunk_type state_type(const std::size_t index)
{
	return index == 1 ? chunk_type::input : chunk_type::state;
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
	}
	else
	{
		const auto err{m_srtt > r ? m_srtt - r : r - m_srtt};
		m_rttvar = (3 * m_rttvar + err) / 4;
		m_srtt = (7 * m_srtt + r) / 8;
	}
	/* Never quite zero: a link that has been perfectly steady still
	 * needs room for its next hiccup.
	 */
	m_rttvar = std::max(m_rttvar, m_hold_period / 4);
}

void rtt_estimator::add_bound(const net_clock r)
{
	if (!m_valid)
		return;
	/* Widen only: a bound is a delay that happened, not a measurement of
	 * how steady the link is, so a small one never narrows rttvar.
	 */
	const auto err{m_srtt > r ? m_srtt - r : r - m_srtt};
	m_rttvar = std::max({m_rttvar, (3 * m_rttvar + err) / 4, m_hold_period / 4});
	/* Any bound, however small, says that delays are still being seen:
	 * the largest of them stays covered.
	 */
	if (r > m_srtt)
		m_bound_excess = std::max(m_bound_excess, r - m_srtt);
	m_bound_age = 0;
}

void rtt_estimator::advance_ticks(net_clock ticks)
{
	/* Bounded: however long the stall, the excess is gone after the
	 * hold plus some 150 ticks of decay, and the loop ends with it.
	 */
	for (; ticks > 0 && m_bound_excess != 0; --ticks)
	{
		m_bound_age += m_tick_period;
		if (m_bound_age <= NET_V2_BOUND_HOLD)
			continue;
		m_bound_excess -= m_bound_excess / 16;
		if (m_bound_excess < 16)
			m_bound_excess = 0;
	}
}


net_clock rtt_estimator::rto() const
{
	if (!m_valid)
		return NET_V2_RTO_MAX;
	/* The samples exclude the two tick holds, so they are added to the
	 * variance term, not weighed against it.  A delay seen lately is
	 * covered even once rttvar has forgotten it.
	 */
	return std::clamp(m_srtt + std::max(4 * m_rttvar, m_bound_excess) + m_hold_period + m_tick_period, NET_V2_RTO_MIN, NET_V2_RTO_MAX);
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
	if (expire(now))
		/* A stall that empties the window leaves nothing to aim for:
		 * the target freezes where the applied offset is, rather than
		 * keeping the slew running toward a sample that has expired.
		 * The next sample sets a fresh target.
		 */
		m_target = m_candidates.empty() ? m_offset : m_candidates.front().offset;
	const auto elapsed{now - m_last_update};
	if (elapsed <= 0)
		return;
	m_last_update = now;
	const auto diff{m_target - m_offset};
	const auto magnitude{diff < 0 ? -diff : diff};
	if (magnitude == 0)
	{
		m_slew_remainder = 0;
		return;
	}
	if (magnitude > NET_V2_CLOCK_JUMP_THRESHOLD || now - m_first_sample < NET_V2_CLOCK_SAMPLE_WINDOW)
	{
		m_offset = m_target;
		m_slew_remainder = 0;
		return;
	}
	/* Carry the sub-unit remainder so that short ticks still add up to
	 * the full slew rate; a step clamped at the target leaves nothing to
	 * carry.
	 */
	const auto scaled{elapsed * NET_V2_CLOCK_SLEW_PER_SECOND + m_slew_remainder};
	auto step{scaled / net_seconds(1)};
	if (step >= magnitude)
	{
		step = magnitude;
		m_slew_remainder = 0;
	}
	else
		m_slew_remainder = scaled % net_seconds(1);
	m_offset += diff < 0 ? -step : step;
}

/* connection */

namespace {

[[nodiscard]]
connection_config sanitized(connection_config config)
{
	/* A zero term would divide by zero in begin_tick; a zero budget
	 * could never send.
	 */
	config.tick.numerator = std::max<net_clock>(config.tick.numerator, 1);
	config.tick.denominator = std::max<net_clock>(config.tick.denominator, 1);
	/* Two at least: the second packet of a tick is what carries a
	 * reliable message that does not fit beside the state chunk.
	 */
	config.max_packets_per_tick = std::max(config.max_packets_per_tick, 2u);
	/* peer_tick is sanitized by set_peer_tick, from the constructor. */
	return config;
}

}

connection::connection(const connection_config &config, const net_clock now) :
	m_config{sanitized(config)},
	/* Make the first build_outgoing produce a packet at once. */
	m_last_sent{now - NET_V2_KEEPALIVE_INTERVAL},
	/* One full period behind, so the first begin_tick grants a budget. */
	m_tick_origin{now * m_config.tick.denominator - m_config.tick.numerator},
	m_last_heard{now},
	m_rtt{m_config.tick.units(), m_config.tick.units()}
{
	set_peer_tick(config.peer_tick);
}

/* The one place that knows the hold rule: the ack hold is the longer of
 * the two periods, since the peer holds our packet's ack until its tick
 * and its ack rides to us in a packet we only look at on ours.  Zero
 * terms mean "same as ours".
 */
void connection::set_peer_tick(tick_period peer_tick)
{
	if (peer_tick.numerator < 1 || peer_tick.denominator < 1)
		peer_tick = m_config.tick;
	m_config.peer_tick = peer_tick;
	m_rtt.set_hold_period(std::max(m_config.tick.units(), peer_tick.units()));
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
	set_unreliable_state(type, 0, 1, payload);
}

void connection::set_unreliable_state(const chunk_type type, const unsigned part, const unsigned part_count, const std::span<const std::uint8_t> payload)
{
	if (m_state == connection_state::closed)
		return;
	if (!is_latest_wins(type) || part_count < 1 || part_count > NET_V2_STATE_MAX_PARTS || part >= part_count || payload.size() > NET_V2_MAX_STATE_PART)
	{
		++m_stats.unreliable_dropped;
		return;
	}
	auto &bundle{m_state_out[state_index(type)]};
	if (bundle.count != part_count)
	{
		/* A new layout: parts of the old one still pending are stale. */
		for (auto &p : bundle.parts)
			if (p.pending)
			{
				p.pending = false;
				++m_stats.unreliable_dropped;
			}
		bundle.count = part_count;
	}
	auto &p{bundle.parts[part]};
	if (p.pending)
		/* Replaced before it was sent. */
		++m_stats.unreliable_dropped;
	p.pending = true;
	p.size = payload.size();
	std::ranges::copy(payload, p.data.data());
}

namespace {

/* The one walk over the pending bundle parts, in send order (state
 * before input, part index ascending): f(type, part index, count, part).
 * Stops when f returns false.
 */
template <typename Bundles, typename F>
void for_each_pending_part(Bundles &bundles, F &&f)
{
	for (std::size_t t{}; t != bundles.size(); ++t)
	{
		auto &bundle{bundles[t]};
		for (unsigned i{}; i != bundle.count; ++i)
		{
			auto &p{bundle.parts[i]};
			if (p.pending && !f(state_type(t), i, bundle.count, p))
				return;
		}
	}
}

[[nodiscard]]
constexpr std::size_t state_part_wire_size(const std::size_t payload_size)
{
	return chunk_wire_size(NET_V2_STATE_PART_HEADER_SIZE + payload_size);
}

}

bool connection::any_state_pending() const
{
	bool any{};
	for_each_pending_part(m_state_out, [&](chunk_type, unsigned, unsigned, const state_part &) {
		any = true;
		return false;
	});
	return any;
}

connection::state_plan connection::plan_state_parts() const
{
	/* Greedy, in order: a part that does not fit starts the next packet
	 * and everything behind it waits.  The same walk write_state_parts
	 * makes, so the reservation and the layout agree.
	 */
	const std::size_t available{NET_V2_MAX_PACKET - NET_V2_HEADER_SIZE};
	state_plan plan{.first_packet_bytes = 0, .packets = 0};
	std::size_t filled{};
	for_each_pending_part(m_state_out, [&](chunk_type, unsigned, unsigned, const state_part &p) {
		const auto wire{state_part_wire_size(p.size)};
		if (plan.packets == 0 || filled + wire > available)
		{
			++plan.packets;
			filled = 0;
		}
		filled += wire;
		if (plan.packets == 1)
			plan.first_packet_bytes = filled;
		return true;
	});
	return plan;
}

void connection::write_state_parts(std::uint8_t *const buf, std::size_t &pos)
{
	for_each_pending_part(m_state_out, [&](const chunk_type type, const unsigned part, const unsigned count, state_part &p) {
		const auto wire{state_part_wire_size(p.size)};
		if (wire > NET_V2_MAX_PACKET - pos)
			/* The hard bound: whatever the reservation said, nothing is
			 * written past the buffer.  The rest waits.
			 */
			return false;
		chunk_header{.type = static_cast<std::uint8_t>(type), .length = static_cast<std::uint16_t>(NET_V2_STATE_PART_HEADER_SIZE + p.size)}.write(buf + pos);
		pos += NET_V2_CHUNK_HEADER_SIZE;
		buf[pos++] = net_state_part_byte(part, count);
		std::copy_n(p.data.data(), p.size, buf + pos);
		pos += p.size;
		p.pending = false;
		return true;
	});
}

bool connection::send_unreliable(const chunk_type type, const std::span<const std::uint8_t> payload)
{
	if (m_state == connection_state::closed)
		return false;
	/* Only events: the peer rejects any other type in this position as
	 * malformed, and state/input have their own path.
	 */
	if (type != chunk_type::event_u || payload.size() > NET_V2_MAX_CHUNK_PAYLOAD)
	{
		++m_stats.unreliable_dropped;
		return false;
	}
	if (m_events_count == NET_V2_EVENT_QUEUE_MAX)
	{
		/* Full: the oldest goes. */
		m_events_head = (m_events_head + 1) % NET_V2_EVENT_QUEUE_MAX;
		--m_events_count;
		++m_stats.unreliable_dropped;
	}
	auto &e{event_slot(m_events_count)};
	e.size = payload.size();
	e.skipped = 0;
	std::ranges::copy(payload, e.data.begin());
	++m_events_count;
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
	/* Only the oldest unacked message matters, and it is the front:
	 * acked messages are popped from the front as soon as they are
	 * acked (pop_acked_messages), the ones held behind an unacked front
	 * are not the oldest.
	 */
	if (!m_messages.empty())
	{
		const auto &m{m_messages.front()};
		assert(!m.acked);
		if (m.sent && now - m.first_sent >= NET_V2_UNACKED_TIMEOUT)
			close_with(close_reason::unacked_timeout);
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
	grant_ticks(now);
}

unsigned connection::begin_tick(const net_clock now)
{
	grant_ticks(now);
	return std::exchange(m_unreported_ticks, 0u);
}

void connection::grant_ticks(const net_clock now)
{
	/* Exact: times scaled by the period's denominator, periods in units
	 * of the numerator.
	 */
	const auto &tick{m_config.tick};
	/* One unit of tolerance: a caller whose integer clock rounds the
	 * period down by a unit (floor(k * 65536 / 60) does, 14 steps in 15)
	 * is on time, not late; without it that step would grant nothing and
	 * the next one two.  The origin still advances by exact periods, so
	 * the tolerance never accumulates.
	 */
	const auto elapsed{now * tick.denominator - m_tick_origin + tick.denominator};
	if (elapsed < tick.numerator)
		return;
	auto ticks{elapsed / tick.numerator};
	m_tick_origin += ticks * tick.numerator;
	if (!m_tick_granted)
	{
		/* However long the connection existed before its first tick, it
		 * is not owed a burst for that time.
		 */
		m_tick_granted = true;
		ticks = 1;
	}
	m_tick_credit = static_cast<unsigned>(std::min<net_clock>(ticks, NET_V2_TICK_GRANT_MAX));
	m_unreported_ticks = std::min(m_unreported_ticks + m_tick_credit, NET_V2_TICK_GRANT_MAX);
	/* Whatever was left of the previous tick is not carried on top. */
	m_tick_open = false;
	m_tick_packets = 0;
	/* Timeouts and the RTO are judged once per tick: a retransmission
	 * cannot go out more often anyway.
	 */
	check_timeouts(now);
	m_rtt.advance_ticks(ticks);
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
	const bool header_due{any_state_pending() || m_events_count != 0 || m_ack_owed || now - m_last_sent >= NET_V2_KEEPALIVE_INTERVAL};
	const bool messages_due{any_message_due()};
	if (!header_due && !messages_due)
		return {};
	const bool opens_tick{!m_tick_open || m_tick_packets >= m_tick_budget || !m_tick_backlog};
	if (opens_tick && m_tick_credit == 0)
		return {};

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
	/* The tick's priority order: (a) the state parts that fit go first in
	 * every packet; (b) reliable messages; a head that does not fit
	 * beside the parts rides a later packet of the tick, which the
	 * budget below always leaves room for; (c) events fill what is left.
	 * Only the tick's first packet reserves room for the head event
	 * beside the parts (a head message it displaces gets the next packet
	 * anyway); a later packet never reserves it, so a message blocked
	 * out of the first packet is not blocked again, by an event, out of
	 * the packet that is meant for it.  Events may be skipped and
	 * dropped under a sustained backlog: they are cosmetic.
	 */
	const auto plan{plan_state_parts()};
	std::size_t reserved{plan.first_packet_bytes};
	if (opens_tick && m_events_count != 0)
	{
		const auto event_size{chunk_wire_size(event_slot(0).size)};
		if (reserved + event_size <= NET_V2_MAX_PACKET - NET_V2_HEADER_SIZE)
			reserved += event_size;
	}
	select(reserved);
	if (!header_due && m_carried.empty())
		/* Something is due but did not fit this time; never emit an
		 * empty packet for it, or the caller's send loop would not end.
		 */
		return {};
	/* Header is written last; chunks first.  The reliable runs are laid
	 * out before anything is committed: the selection guarantees that
	 * they fit, and should layout and selection ever disagree (a bug,
	 * never an overflow) the packet is abandoned with nothing changed,
	 * rather than some messages marked sent and others not.
	 */
	auto *const buf{m_outgoing.data()};
	std::size_t pos{NET_V2_HEADER_SIZE};
	for (const auto &run : m_runs)
	{
		const auto chunk_at{pos};
		if (NET_V2_CHUNK_HEADER_SIZE + NET_V2_RELIABLE_RUN_HEADER_SIZE > NET_V2_MAX_PACKET - pos)
		{
			assert(!"reliable run selected beyond the packet");
			return {};
		}
		pos += NET_V2_CHUNK_HEADER_SIZE + NET_V2_RELIABLE_RUN_HEADER_SIZE;
		for (auto j{run.first}; j != run.first + run.count; ++j)
		{
			const auto &m{*m_carried[j]};
			if (message_wire_size(m.payload.size()) > NET_V2_MAX_PACKET - pos)
			{
				assert(!"reliable message selected beyond the packet");
				return {};
			}
			buf[pos] = m.type;
			net_put_le16(buf + pos + 1, static_cast<std::uint16_t>(m.payload.size()));
			pos += NET_V2_MESSAGE_HEADER_SIZE;
			std::ranges::copy(m.payload, buf + pos);
			pos += m.payload.size();
		}
		chunk_header{.type = static_cast<std::uint8_t>(chunk_type::reliable), .length = static_cast<std::uint16_t>(pos - chunk_at - NET_V2_CHUNK_HEADER_SIZE)}.write(buf + chunk_at);
		net_put_le16(buf + chunk_at + NET_V2_CHUNK_HEADER_SIZE, m_carried[run.first]->seq);
		buf[chunk_at + NET_V2_CHUNK_HEADER_SIZE + 2] = static_cast<std::uint8_t>(run.count);
	}

	/* From here on the packet is committed. */
	/* §3.6 allows max_packets_per_tick; a bundle that needs more than
	 * one packet (§3.8) gets one packet more than it needs, so that a
	 * blocked head message never displaces a part.  Judged on every
	 * packet of the tick, not only its first, so parts set after the
	 * first packet still fit the tick; the limit only ever grows within
	 * a tick.
	 */
	const auto needed{plan.packets};
	const auto budget_now{std::max(m_config.max_packets_per_tick, needed > 1 ? needed + 1 : 0u)};
	if (opens_tick)
	{
		--m_tick_credit;
		m_tick_open = true;
		m_tick_packets = 0;
		m_tick_budget = budget_now;
	}
	else
		m_tick_budget = std::max(m_tick_budget, budget_now);

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
	for (auto *const carried : m_carried)
	{
		auto &m{*carried};
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
	write_state_parts(buf, pos);
	/* Events: every one that fits, in queue order; one that does not fit
	 * is skipped (not a head-of-line block) and dropped once it has been
	 * skipped NET_V2_EVENT_SKIP_MAX times.  Sent and dropped ones leave
	 * the ring; the kept ones close up behind the head (a slot copy
	 * each, and few are ever kept).
	 */
	std::size_t kept{};
	for (std::size_t i{}; i != m_events_count; ++i)
	{
		auto &e{event_slot(i)};
		if (chunk_wire_size(e.size) > NET_V2_MAX_PACKET - pos)
		{
			if (++e.skipped < NET_V2_EVENT_SKIP_MAX)
			{
				if (kept != i)
					event_slot(kept) = e;
				++kept;
			}
			else
				++m_stats.unreliable_dropped;
			continue;
		}
		chunk_header{.type = static_cast<std::uint8_t>(chunk_type::event_u), .length = static_cast<std::uint16_t>(e.size)}.write(buf + pos);
		pos += NET_V2_CHUNK_HEADER_SIZE;
		std::copy_n(e.data.data(), e.size, buf + pos);
		pos += e.size;
	}
	m_events_count = kept;

	packet_header h;
	h.session_id = m_config.session_id;
	h.peer_token = m_config.peer_token;
	h.player_id = m_config.local_player_id;
	if (!log.msg_seqs.empty())
		h.flags |= static_cast<std::uint8_t>(packet_flag::has_reliable);
	if (pos == NET_V2_HEADER_SIZE)
		h.flags |= static_cast<std::uint8_t>(packet_flag::keepalive);
	h.seq = seq;
	h.send_time = to_net_time(now);
	if (m_any_received)
	{
		h.ack = m_highest_seen;
		h.ack_bits = m_ack_bits;
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
	log.echoed = false;
	log.seq = seq;
	log.sent_at = now;
	m_last_sent = now;
	++m_tick_packets;
	/* Judged now, not at the next call: messages queued in between
	 * belong to the next tick.
	 */
	/* Skipped events count as backlog too: one that did not fit beside
	 * the state chunk gets the tick's second packet, like a blocked
	 * reliable head.
	 */
	m_tick_backlog = any_message_due() || any_state_pending() || m_events_count != 0;
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

/* The caller has checked that `ack` names a packet we sent.  The echo
 * fields are the RTT measurement (they account for the peer's hold); an
 * ack bounds the round trip of a packet that no echo will ever measure,
 * and only bounds it: srtt comes from echoes alone.
 */
void connection::process_acks(const std::uint16_t ack, const std::uint64_t ack_bits, const net_clock now, const bool newest_carrier, const bool echo_before_valid, const std::uint16_t echo_before)
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
		if (!e.valid || e.seq != s || e.acked)
			continue;
		if (e.lost)
		{
			/* Given up on too early: a late ack still acknowledges the
			 * messages (they were delivered), and the loss estimate takes
			 * the packet back.
			 */
			e.lost = false;
			--m_stats.packets_lost;
			m_loss_estimate = std::max(0.0, m_loss_estimate - 1.0 / (1 << NET_V2_LOSS_EWMA_SHIFT));
		}
		if (!e.echoed && newest_carrier && echo_before_valid && seq_diff(echo_before, s) > 0)
		{
			/* A later packet of ours was already echoed before this one
			 * was first acknowledged: it arrived out of order at the
			 * peer, which echoes only its newest, so no echo will ever
			 * measure it.  Its ack is the one word about that long round
			 * trip, exactly what the RTO must cover, so it widens rttvar
			 * (once).  It is a bound, not a sample: an ack includes the
			 * peer's hold, and a peer may hold ack bits back at will, so
			 * it never moves srtt (the HUD ping and the host's rewind).
			 * A packet merely left unechoed because the peer sends fewer
			 * packets than we do is not bounded either, nor one whose
			 * first ack rides a reordered older peer packet, which
			 * carries that packet's own delay.
			 */
			const auto r{now - e.sent_at};
			if (r >= 0 && r <= NET_V2_RTT_SAMPLE_MAX && m_rtt.valid())
			{
				e.echoed = true;
				m_rtt.add_bound(r);
			}
		}
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
	/* Gap rule: a message is retransmitted without waiting for the RTO
	 * once three packets sent after its own are acknowledged: `ack`
	 * itself and the set bits between the message's packet and `ack`.
	 * A lone reordered ack far ahead acknowledges one packet, not three.
	 */
	const auto acked_after{[&](const std::uint16_t packet) -> unsigned {
		const auto d{seq_diff(ack, packet)};
		if (d <= 0)
			return 0;
		const auto between{static_cast<unsigned>(d) - 1};
		const std::uint64_t mask{between >= NET_V2_ACK_BITS ? ~std::uint64_t{} : (std::uint64_t{1} << between) - 1};
		return 1 + static_cast<unsigned>(std::popcount(ack_bits & mask));
	}};
	for (std::size_t i{}; i != m_sent_count; ++i)
	{
		auto &m{m_messages[i]};
		if (m.acked || m.resend)
			continue;
		if (acked_after(m.in_packet_seq) >= NET_V2_GAP_LOSS_THRESHOLD)
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
	 * acceptance anyway; forget them, so that the same number, when the
	 * stream has wrapped round to it, is counted again.  Every entry is
	 * judged (there are at most 16): a corrupt packet with a far-future
	 * sequence would otherwise sit at the front for the rest of the
	 * wrap and shield the entries behind it.
	 */
	if (m_any_received)
		std::erase_if(r, [this](const std::uint16_t s) { return seq_diff(m_highest_seen, s) > static_cast<std::int16_t>(NET_V2_ACK_BITS); });
	if (std::ranges::find(r, seq) != r.end())
		/* A replay of a packet already counted. */
		return false;
	r.push_back(seq);
	if (r.size() > NET_V2_RECENT_MALFORMED_MAX)
		r.pop_front();
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
			{
				/* The part byte (§3.8); the records behind it are opaque
				 * to the transport and checked by the consumer (stage 2).
				 */
				if (body.size() < NET_V2_STATE_PART_HEADER_SIZE)
					return false;
				const unsigned part{static_cast<unsigned>(body[0] & 0x0f)};
				const unsigned count{static_cast<unsigned>(body[0] >> 4)};
				if (count < 1 || count > NET_V2_STATE_MAX_PARTS || part >= count)
					return false;
				m_parsed_chunks.push_back({.type = type, .part = static_cast<std::uint8_t>(part), .part_count = static_cast<std::uint8_t>(count), .payload = body.subspan(NET_V2_STATE_PART_HEADER_SIZE)});
				break;
			}
			case chunk_type::event_u:
				m_parsed_chunks.push_back({.type = type, .part = 0, .part_count = 1, .payload = body});
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

void connection::deliver_reliable(const net_clock now)
{
	if (m_parsed_messages.empty())
		return;
	const bool had_pending{m_recv_window_pending != 0};
	for (const auto &pm : m_parsed_messages)
	{
		if (seq_diff(pm.seq, m_next_expected) < 0)
			/* Already delivered. */
			continue;
		if (m_recv_window_pending == 0 && pm.seq == m_next_expected)
		{
			/* The common case, in order with nothing held: straight
			 * into the report as a view into the datagram, not through
			 * a window slot.
			 */
			m_report_reliable.push_back({.type = pm.type, .payload = pm.payload});
			++m_next_expected;
			++m_stats.messages_delivered;
			continue;
		}
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
		/* Held storage: kept until the next on_receive, so the view
		 * outlives this call like the ones into the datagram.
		 */
		const auto &held{m_delivered_held.emplace_back(std::move(slot.payload))};
		m_report_reliable.push_back({.type = slot.type, .payload = held});
		slot.payload.clear();
		slot.filled = false;
		--m_recv_window_pending;
		++m_next_expected;
		++m_stats.messages_delivered;
	}
	/* The stall clock restarts whenever the window was empty before or
	 * the stream advanced now.
	 */
	if (!had_pending || !m_report_reliable.empty())
		m_recv_gap_since = now;
	m_ack_owed = true;
}

connection::latest_packet &connection::latest_for(const chunk_type type, const unsigned part)
{
	return m_latest[state_index(type)][part];
}

void connection::deliver_unreliable(const std::uint16_t packet_seq)
{
	for (const auto &c : m_parsed_chunks)
	{
		if (is_latest_wins(c.type))
		{
			auto &latest{latest_for(c.type, c.part)};
			/* The last packet that carried this part is a reference only
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
		m_report_unreliable.push_back({.type = c.type, .part = c.part, .part_count = c.part_count, .payload = c.payload});
	}
}

receive_report connection::on_receive(const std::span<const std::uint8_t> datagram, const net_clock now)
{
	/* The previous report ends here: its lists and its views into held
	 * storage.  The vectors keep their capacity, so a datagram costs no
	 * allocation once the connection has warmed up.
	 */
	m_delivered_held.clear();
	m_report_reliable.clear();
	m_report_unreliable.clear();
	receive_report report;
	const auto reject{[&](const receive_status status) {
		++m_stats.packets_rejected;
		report.status = status;
		return std::move(report);
	}};
	/* §3.7 steps 1–5.  read() fails on a datagram shorter than the
	 * header; that is the lower length check.
	 */
	if (datagram.size() > NET_V2_MAX_PACKET)
		return reject(receive_status::bad_length);
	const auto header{packet_header::read(datagram)};
	if (!header)
		return reject(receive_status::bad_length);
	const auto &h{*header};
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
		if (d > NET_V2_MAX_SEQ_JUMP)
		{
			/* A corrupted or forged sequence far ahead: taking it as the
			 * new highest would reject every real packet that follows as
			 * a duplicate until the timeout, and make our acks protocol
			 * errors at the peer.  A protocol error here, once per
			 * sequence, nothing applied.
			 */
			if (count_protocol_error(h.seq, now))
				close_with(close_reason::protocol_error);
			return reject(receive_status::bad_seq);
		}
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
	const bool echo_before_valid{m_echo_sampled_any};
	const auto echo_before{m_echo_sampled_seq};
	/* Only the newest packet received measures the round trip: a
	 * reordered older peer packet carries its own reorder delay in every
	 * sample it could give.  That delay is exactly what a late ack costs
	 * us, so such an echo widens rttvar (a bound) and never moves srtt;
	 * the packet's acks are honoured below.
	 */
	const bool newest{!m_any_received || seq_diff(h.seq, m_highest_seen) > 0};
	if (!(h.ack == 0 && h.ack_bits == 0) && h.echo_delay != NET_V2_ECHO_DELAY_SATURATED)
	{
		/* The echo describes the packet `ack` names (the newest the peer
		 * received).  Only an echo of a packet we really sent, with the
		 * send_time we really wrote, may feed the estimators: the game
		 * will rewind hits by this RTT.  Each packet of ours yields one
		 * sample at most, whichever of the peer's packets carries its
		 * echo first: a reordered echo still counts once, a repeated one
		 * (the same measurement held longer) not at all, which stops a
		 * peer from pinning one old packet with a small delay to steer
		 * srtt and the clock.  The sample itself comes from our own log,
		 * in 64 bits.
		 */
		auto &e{m_packet_log[h.ack % NET_V2_RECV_WINDOW]};
		if (e.valid && e.seq == h.ack && !e.echoed && to_net_time(e.sent_at) == h.echo_time)
		{
			const net_clock rtt{now - e.sent_at - h.echo_delay};
			if (rtt < 0 || rtt > NET_V2_RTT_SAMPLE_MAX)
			{
				/* Out of range (a hostile or absurd echo_delay): rejected
				 * without spending the packet's one sample or moving the
				 * echo mark, which would make older packets look
				 * reordered.
				 */
			}
			else if (!newest)
			{
				/* A bound needs a sample to bound; the packet keeps its
				 * sample until there is one.
				 */
				if (m_rtt.valid())
				{
					e.echoed = true;
					m_rtt.add_bound(rtt);
				}
			}
			else if (!m_echo_sampled_any || seq_diff(h.ack, m_echo_sampled_seq) > 0)
			{
				e.echoed = true;
				m_echo_sampled_any = true;
				m_echo_sampled_seq = h.ack;
				m_rtt.add_sample(rtt);
				const net_time peer_now{h.send_time + static_cast<net_time>(rtt / 2)};
				m_clock.add_sample(now, rtt, net_time_diff(peer_now, to_net_time(now)));
			}
			/* Else the peer's newest packet echoes a packet older than
			 * the one last echoed.  A conforming peer echoes the newest
			 * packet it received, so this is no measurement at all, and
			 * it is ignored: five packets in six go unechoed against a
			 * peer sending a sixth of our rate, and a hostile peer could
			 * otherwise name a seconds-old one with echo_delay 0 and
			 * drive srtt to seconds.
			 */
		}
	}
	process_acks(h.ack, h.ack_bits, now, newest, echo_before_valid, echo_before);
	/* Step 9: record and apply.  Only the newest packet moves the echo
	 * fields: echoing a late, reordered packet would inflate the peer's
	 * RTT sample by the reorder delay.
	 */
	m_any_received = true;
	m_ack_bits = new_ack_bits;
	if (newest)
	{
		m_highest_seen = h.seq;
		m_last_recv_send_time = h.send_time;
		m_last_recv_local_time = now;
	}
	deliver_reliable(now);
	deliver_unreliable(h.seq);
	report.reliable = m_report_reliable;
	report.unreliable = m_report_unreliable;
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
