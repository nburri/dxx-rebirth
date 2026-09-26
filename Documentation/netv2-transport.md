# v2 network transport: library and simulation test (stage 0)

This is stage 0 of the plan in `network-protocol-v2.md` §8: the transport
layer as a self-contained library plus a simulation test. Nothing in the
game uses it yet; stage 1 puts it under the existing UDP socket.

## Files

| File | Content |
|---|---|
| `common/main/net_v2.h` | Wire constants (§3.1–3.7, §2.2), the 34-byte `packet_header`, the `chunk_header`, little-endian helpers, sequence arithmetic. Standard library only. |
| `common/main/net_v2_transport.h` | `dcx::net_v2::connection`, `rtt_estimator`, `clock_sync`, the result/report types and `connection_stats`. |
| `common/main/net_v2_transport.cpp` | Implementation. Compiled into the `common` objects of the game. |
| `common/unittest/net_v2_transport.cpp` | The simulation test (no Boost, plain `main`). |

The library reads no clock and touches no socket: time is a parameter of
every call (`net_clock`, the same 1/65536 s unit as the game's `fix64`),
datagrams go in and out as byte spans. That keeps it free of game state and
lets the test drive two connections through a virtual link on a virtual
clock.

## Building and running the test

With SCons, from the top of the tree (the same options as the game build,
plus the option that registers test link targets; only the requested alias
is built):

```
scons -j"$(nproc)" sdl2=1 d1x=0 d2x=1 register_runtime_test_plain_link_targets=1 test-net-v2-transport
build/common/test-net-v2-transport
```

Without SCons, any C++23 compiler will do:

```
g++ -std=gnu++23 -O2 -g -Wall -Wextra -Icommon/main \
    common/unittest/net_v2_transport.cpp common/main/net_v2_transport.cpp \
    -o test-net-v2-transport && ./test-net-v2-transport
```

Add `-fsanitize=address,undefined` to run the fuzz case under the
sanitizers. The test takes an optional seed as its first argument
(`./test-net-v2-transport 0x1234`); the default seed is fixed, so a run is
reproducible. A failed check prints `file:line: check failed: <expr>` and
exits with status 1; success ends with `all tests passed`.

## What the test covers

| Case | Link | Checks |
|---|---|---|
| header layout | – | Byte offsets of §3.1 round-trip; `proto`, reserved flags, `UNCONNECTED`, session mismatch are rejected in that order. |
| a: lossless | 80 ms | 2 × 2000 reliable messages of random size (0–250 bytes, 2 % at the 1024-byte maximum) arrive exactly once, in order; no retransmission; queue and in-flight drain to 0. |
| b: lossy | 80 ± 20 ms, 20 % loss, 5 % duplication, 20 % reorder | 2 × 3000 reliable messages exactly once, in order; retransmissions well below one per message; loss estimate near 20 %; duplicated packets rejected as such; latest-wins `state`/`input` chunks arrive strictly increasing and converge to the last one sent; `event_u` chunks each at most once. |
| c: RTT/RTO | 80 ± 10 ms, 0 % and 30 % loss | `srtt` within 10 % of 160 ms, `rttvar` small, `rto ≥ srtt`, `rto` always inside [50, 1000] ms; a 1 ms link clamps to 50 ms, a 700 ms link to 1000 ms. |
| d: bounds | 30 ms, acks cut | 1025-byte message rejected at enqueue; the 513th queued message and the 97th 1024-byte message close the connection with `queue_overflow`; with acks cut the sender never exceeds 256 in flight and the receiver delivers exactly 256; when acks return everything arrives in order. |
| e: timeouts | 50 ms | Both sides close with `timeout` 5 s after the link is cut; a peer that talks but never acks makes the sender close with `unacked_timeout` after 10 s; an idle connection sends keepalives and stays up. |
| replay window | – | A repeated packet, and one 65 behind, are rejected; one 64 behind is accepted once; late packets show up in `ack_bits`; packets that fell out of the bitfield count as lost. |
| f: fuzz | – | 200 000 random datagrams are all rejected without effect; 100 000 packets with a valid header and random chunk bytes, and 100 000 mutations of captured real packets, never crash and a malformed packet delivers nothing; 16 malformed packets close the connection with `protocol_error`. |
| g: clock | 50 ± 20 ms, host clock about to wrap, client 1234 s behind | The client's offset target is within 20 ms after 8 packets and within 5 ms after 2 s; the host's estimate is the negative; a 300 ms clock step is followed within 2.5 s; a 20 ms step is slewed (5 ms/s), not jumped. |

The simulated link delivers a packet at its arrival time, as the game does
by reading the socket every frame; packets are *sent* on the 60 Hz tick.

## Using the library

```cpp
using namespace dcx::net_v2;
connection c{{.session_id = sid, .peer_token = tok, .local_player_id = 0, .remote_player_id = 3}, now};

c.enqueue_reliable(msg_type, payload);            // ≤ 1024 bytes, or too_large
c.set_unreliable_state(chunk_type::state, bundle); // latest wins, sent once
c.send_unreliable(chunk_type::event_u, bytes);     // best effort

for (;;) {                                         // once per tick
    const auto packet{c.build_outgoing(now)};
    if (packet.empty()) break;
    sendto(..., packet);
}

auto report{c.on_receive(datagram, now)};          // for every datagram read
for (auto &m : report.reliable) handle(m);         // in order, exactly once
for (auto &u : report.unreliable) apply(u);        // newest state, every event
```

`build_outgoing` produces a packet when there is a state chunk, an event, a
reliable message to send or resend, an ack owed for a received reliable
message, or 100 ms have passed since the last packet (keepalive). The second
call in a tick carries only reliable messages that did not fit beside the
state chunk. `on_receive` applies the checks of §3.7 in order and reports
why a datagram was dropped (`receive_status`). Packets with
`flags.UNCONNECTED` are reported as `unconnected` and left to the session
layer (stage 1), which also decides which `connection` a datagram belongs to
(`packet_header::read` gives it `session_id`, `peer_token` and `player_id`).

`state()` is `connecting` until the first valid packet arrives, then
`connected`, and `closed` with a `close_reason` (`timeout`,
`unacked_timeout`, `queue_overflow`, `protocol_error`, `local`) after which
the connection neither sends nor accepts anything.

## What the stats mean

`connection::stats()` returns a `connection_stats` snapshot:

| Field | Meaning |
|---|---|
| `rtt_valid`, `srtt`, `rttvar`, `rto` | §3.5 estimator, in net time units (1/65536 s; `× 1000 / 65536` for ms). Samples come from `echo_time`/`echo_delay` of every received packet, so they exclude the peer's hold time. `rto` is `clamp(srtt + 4·rttvar, 50 ms, 1 s)` and 1 s before the first sample. This `srtt` is what the HUD will show as ping. |
| `loss_estimate` | Moving average (1/64 per packet) of the fraction of our packets the peer never acknowledged. A packet counts as lost once the peer's `ack` is more than 64 ahead of it, so the value lags by about a second at 60 pps and settles slowly on a link that just became clean. |
| `packets_sent/received/rejected/acked/lost` | Per direction. `rejected` counts every datagram `on_receive` dropped, including duplicates. |
| `messages_enqueued/delivered` | Reliable messages queued here / delivered to the caller from the peer. |
| `message_sends`, `message_resends`, `resends_by_gap`, `resends_by_rto` | Transmissions of reliable messages; the resend split says which rule detected the loss (3 later packets acked, or the RTO). Resends well above the loss rate mean the RTO is too tight for the link. |
| `unreliable_dropped` | State chunks replaced before they were sent, oversize chunks, and events pushed out of the 64-entry event queue. |
| `protocol_errors` | Malformed packets from the peer; 16 within 10 s close the connection. |
| `queue_messages`, `queue_bytes` | Reliable messages queued or in flight, and their payload bytes; the connection closes at 512 messages or 96 KiB. |
| `in_flight` | Sent but not yet acked, at most 256. If it sits at 256 the peer is not acking. |
| `recv_window_pending` | Messages received out of order and held until the gap before them is filled. |
| `clock_offset_valid`, `clock_offset`, `clock_offset_target` | §2.2: peer clock ≈ local clock + `clock_offset`. The target is the offset of the minimum-RTT sample in the last 2 s; the applied value slews toward it at 5 ms/s or jumps if more than 100 ms away (and follows it directly during the first 2 s of a session). Only meaningful on the client (the host is the reference), though both sides compute it. |
| `last_heard` | Local time of the last valid packet from the peer. |

## Deviations from the design text

- Packet sequence numbers skip 0 on wrap so that `ack == 0 && ack_bits == 0`
  always means "nothing received yet".
- RTT samples are taken from `echo_time`/`echo_delay`; the ack-based sample
  (`sent_at` in the packet log) is used only for a packet whose ack arrived
  without a usable echo, because the ack-based value includes the peer's
  hold time up to one tick.
- The 600-byte resend budget is cumulative, but the first resent message of
  a packet may exceed it; otherwise a message above 600 bytes could never be
  retransmitted.
- A malformed packet delivers none of its chunks (not just the ones after
  the bad chunk); its header effects (acks, RTT, `last_heard`) are kept as
  designed.
- The applied clock offset follows the target without slewing during the
  first 2 s after the first sample, so that a session does not start with
  an offset that is up to a jitter's worth wrong for several seconds.
- The library has no Boost dependency and the test is a plain program, as
  requested for stage 0.
