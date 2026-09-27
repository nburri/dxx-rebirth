# v2 network transport: library and simulation test (stage 0)

This is stage 0 of the plan in `network-protocol-v2.md` §8: the transport
layer as a self-contained library plus a simulation test. Nothing in the
game uses it yet; stage 1 puts it under the existing UDP socket.

## Files

| File | Content |
|---|---|
| `common/main/net_v2.h` | Wire constants (§3.1–3.7, §2.2), the 36-byte `packet_header` (the design's 34 bytes plus `echo_seq`), the `chunk_header`, little-endian helpers, sequence arithmetic. Standard library only. |
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
| malformed not acked | 40 ms | One packet's chunk length is corrupted in flight: the receiver counts one protocol error and does not ack it, the sender retransmits, all 300 messages arrive in order, exactly one packet counts as lost. |
| hostile echo | – | Extreme `echo_time`/`echo_delay`/`now` combinations (including the int32 overflow case) naming a real packet are accepted without overflow and yield no sample. |
| echo authentication | 30 ms | 76 forged keepalives (real seq with a 9 s old time, unknown seq, an old seq with its true time, seq 0): `srtt`, `rttvar`, `rto` and the offset target are unchanged. |
| tick credit | – | A 100 Hz caller enqueuing one message per frame sends 600 ± 5 packets in 10 s; a frame two ticks after the last gets both ticks (4 packets); a 60-tick stall still releases only 4. |
| reordered echo | – | Packet 1 arriving after packet 2 does not move the echo fields; the peer's RTT sample is exact. |
| long blackout | – | 34 000 packets without an ack, then acks resume while every other packet of ours is lost: 30+ of the 75 lost are counted within 150 packets (the bitfield rule, not slot reuse). |
| malformed replay | – | 20 copies of one corrupted datagram: 1 protocol error, 19 duplicates, connection open, the message arrives by retransmission. |
| latest-wins wrap | – | 33 000 event-only packets, then 200 `state` chunks: none dropped; a reordered older `state` still is. |
| grant closes tick | – | One packet built in tick 0, then exactly 2 (not 3) in tick 1. |
| hostile ack | – | `ack` 5000 after 5 packets sent: `bad_ack`, no packet resolved, no resend, one protocol error; its replay is `bad_ack` again, not counted; `ack` 5 is accepted. |
| event head-of-line | – | Ten 4-byte events queued behind an 1161-byte one all go out beside a 300-byte state at once; the big one is dropped after 8 packets and fits when sent alone. |
| zero tick period | – | `tick_period` 0 and `max_packets_per_tick` 0 are clamped (to 1 and 2) and the connection sends. |
| echo pinning | 30 ms | 600 forged packets over 5 s pinning one real old packet in `echo_seq` with `echo_delay` 0 (with `ack` equal to it, and with `ack` moving on): `srtt`, `rttvar` and the offset target are unchanged. |
| corrupt acks | 40 ms | A packet with reliable messages is lost and the peer's next datagram falsely acks it but is corrupt: nothing of it is applied, the messages are retransmitted, all 200 arrive, one protocol error. |
| stream stalled | – | Message 1 arrives, message 0 never does, the peer keeps sending state: the receiver closes with `stream_stalled` after 10 s. |
| corrupted then intact | – | A corrupted copy of a packet, then the intact copy: the intact one is accepted and its message delivered; one protocol error. |
| corrupted future seq | – | A corrupt datagram whose `seq` byte reads as a future sequence does not prevent the real packets 2, 3 and 4 from being accepted. |
| big message, second packet | – | A 1 KiB message beside an 1100-byte state: the state goes out first, the message in the tick's second packet; `max_packets_per_tick` 1 is clamped to 2. |
| bulk transfer + state | 30 ms | 20 × 900-byte messages queued while a 350-byte state is set every tick: 207+ of 210 states delivered, none dropped, all messages in order. |
| peer tick period | 10–85 ms, no jitter | A 60 Hz host (`peer_tick_period` 1/30) against a 30 Hz client: zero retransmissions over four latencies. |
| 240 Hz caller | – | `build_outgoing` called four times per tick period with a 90 KiB backlog: never more than 2 packets in any 16.7 ms window, everything delivered. |
| window in one packet | – | 300 empty messages queued at once: the first packet carries exactly 256, the receiver accepts it, the remaining 44 follow after the ack. |
| packets per tick | 30 ms | A 90 KiB backlog of 1 KiB messages drains at no more than 2 packets per tick and arrives in order. |
| unaligned peers | 1–100 ms, no jitter, no loss | Six rounds with random tick phases: zero retransmissions, `rto ≥ srtt + tick`. |
| ack blackout | 30 ms, acks zeroed | 500 packets without an ack: more than 200 counted lost once their log slots are reused, loss estimate above 0.9. |
| echo_delay clamp | – | A packet built 100 units before the receive stamp carries `echo_delay` 0 and the peer's RTT sample is the true value. |
| unreliable chunks per packet | – | Two `state` chunks in one packet are both delivered; a reordered older packet's `state` is dropped while its `event_u` is kept. |
| f: fuzz | – | 200 000 random datagrams are all rejected without effect; 100 000 packets with a valid header and random chunk bytes, and 100 000 mutations of captured real packets, never crash and a malformed packet delivers nothing; 16 malformed packets close the connection with `protocol_error`. |
| g: clock | 50 ± 20 ms, host clock about to wrap, client 1234 s behind | The client's offset target is within 20 ms after 8 packets and within 5 ms after 2 s; the host's estimate is the negative; a 300 ms clock step is followed within 2.5 s; a 20 ms step is slewed (5 ms/s), not jumped. |

The simulated link delivers a packet at its arrival time, as the game does
by reading the socket every frame; packets are *sent* on the 60 Hz tick.
Each peer can tick with its own phase offset (`sim_world::phase`).

## Using the library

```cpp
using namespace dcx::net_v2;
connection c{{.session_id = sid, .peer_token = tok, .local_player_id = 0, .remote_player_id = 3}, now};
// connection_config also has tick_period (default 1/60 s),
// max_packets_per_tick (default 2, at least 2) and peer_tick_period
// (default: same as tick_period; stage 1 sets it from the handshake).
// report.unreliable holds views into `datagram`, valid while it is.

c.enqueue_reliable(msg_type, payload);            // ≤ 1024 bytes, or too_large
c.set_unreliable_state(chunk_type::state, bundle); // latest wins, sent once
c.send_unreliable(chunk_type::event_u, bytes);     // best effort

c.begin_tick(now);                                 // once per tick
for (;;) {
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
state chunk, and no more than `max_packets_per_tick` (2, §3.6) are built per
tick; a backlog beyond that waits for later ticks. `begin_tick(now)` grants
that budget for every whole `tick_period` elapsed since the last grant, and
judges the timeouts and the RTO once per grant (a retransmission cannot go out
more often anyway),
advancing the tick origin by whole periods (not resetting it to `now`), so a
caller at any rate gets exactly one budget per period, a frame that spans two
ticks gets both. A grant replaces whatever was left of the previous one and is
capped at two ticks, so unused ticks never pile up into a spare and a long
stall does not end in a burst. Within a tick the first packet goes out
whenever anything is due; a second, up to `max_packets_per_tick` (at least 2),
only while reliable messages remain that did not fit in the previous one. The
first packet of a tick always carries the state chunk; a head message that
does not fit beside it rides the tick's second packet, which has no state left
to carry (and omits one set in between), so no message size can starve and no
message stream can displace the state. The tick's credit is spent only when a
packet is really built. `build_outgoing` calls `begin_tick` itself. `on_receive` applies the checks of §3.7
in order and reports why a datagram was dropped (`receive_status`). A packet
whose header validates but whose chunks do not (`malformed_chunk`) has no
effect at all: not its acks (one corrupt ack bit would otherwise acknowledge a
message that was never delivered and wedge the receiver's window), not its
echo, not `last_heard`; it is not acknowledged, so a conforming peer
retransmits. A packet whose `ack` names a sequence we have not sent
(`bad_ack`) is rejected the same way. Each counts one protocol error per
distinct `seq` (a 16-entry list of recently counted sequences, expired with
the reorder window, suppresses further counts for replays); the list never
rejects anything, so an intact copy of a sequence whose corrupted copy came
first is accepted normally, and a corrupt `seq` byte cannot blackhole the real
packet with that sequence. Should the receive window hold out-of-order
messages for 10 s without the gap ever being filled while the peer keeps
sending, the connection closes with `stream_stalled` rather than blaming the
peer for a protocol error. Packets with
`flags.UNCONNECTED` are reported as `unconnected` and left to the session
layer (stage 1), which also decides which `connection` a datagram belongs to
(`packet_header::read` gives it `session_id`, `peer_token` and `player_id`).

Unreliable chunks: the report carries every `event_u` chunk and every
`state`/`input` chunk of the packet, unless the packet is older (by `seq`)
than one, still inside the 64-packet reorder window, whose chunk of that type
was already delivered (a reference older than the window has been superseded
by every packet since and no longer suppresses anything, so a type unseen for
32 768 packets cannot wedge on a wrapped comparison). All chunks of one packet
count as equally new, so a bundle split into two `state` chunks (§3.8)
arrives whole and in order. The transport keeps no copy of the latest state;
the consumer keeps what it needs from the report. Events (`send_unreliable`)
go out in queue order as far as they fit; one that does not fit beside the
state chunk is skipped, not a head-of-line block, and dropped after
`NET_V2_EVENT_SKIP_MAX` (8) packets.

`state()` is `connecting` until the first valid packet arrives, then
`connected`, and `closed` with a `close_reason` (`timeout`,
`unacked_timeout`, `queue_overflow`, `protocol_error`, `stream_stalled`,
`local`) after which
the connection neither sends nor accepts anything.

## What the stats mean

`connection::stats()` returns a `connection_stats` snapshot:

| Field | Meaning |
|---|---|
| `rtt_valid`, `srtt`, `rttvar`, `rto` | §3.5 estimator, in net time units (1/65536 s; `× 1000 / 65536` for ms). Samples come only from the echo fields, so they exclude the peer's hold time; an echo is taken only if `echo_seq` equals the packet's `ack` (a conforming peer echoes the newest packet it received, which is also its `ack`), names a packet in our log whose recorded `send_time` equals `echo_time`, and is strictly newer than the last echo taken (a repeated `echo_seq` is the same measurement held longer, so it yields no sample); the sample is then `now − sent_at − echo_delay` from our own log. A peer therefore cannot steer the estimate, not even by pinning one real old packet with a small delay. Acks contribute no samples: an ack is held until the peer's next tick and may ride a reordered packet, so it would only add noise on top of what the echo already measures. `rto` is `clamp(srtt + max(4·rttvar, hold) + tick, 50 ms, 1 s)` with `hold = max(tick_period, peer_tick_period)`, and 1 s before the first sample; the slack covers the peer's ack hold (its tick) and our own detection alignment (ours), which the echo-based `srtt` deliberately excludes. This `srtt` is what the HUD will show as ping. |
| `loss_estimate` | Moving average (1/64 per packet) of the fraction of our packets the peer never acknowledged. A packet counts as lost once the peer's `ack` is more than 64 ahead of it, or when its slot in the 256-entry packet log is reused without an ack (no acks at all), so the value lags by one to four seconds at 60 pps and settles slowly on a link that just became clean. |
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
- RTT samples are taken from the echo fields only. In exchange the RTO adds
  `max(4·rttvar, hold) + tick` instead of `4·rttvar`, `hold` being the longer
  of the two tick periods: with hold-free samples `rttvar` decays to zero on
  a steady link and the plain formula would retransmit every message whose
  ack is held for a tick.
- `echo_delay` is clamped to `[0, 65535]`; a caller whose build time is
  behind its receive stamp sends 0 instead of a wrapped value.
- The header has a 36th and 35th byte: `echo_seq` (offset 34), the `seq` of
  the echoed packet, so that echoes can be verified against the packet log
  (§2.2, §3.1 updated). Without it a peer could set `srtt` to seconds with
  a made-up `echo_time`, and the host will rewind hits by that RTT.
- The echo fields follow only the newest packet received; a reordered
  older packet is acknowledged and delivered but not echoed, since echoing
  it would add the reorder delay to the peer's RTT sample.
- Protocol errors are counted once per distinct `seq` through a small list
  (16) of recently counted sequences that expires with the reorder window;
  the list never rejects a datagram, so an intact copy of a sequence whose
  corrupt copy arrived first always gets through.
- `close_reason::stream_stalled` (not in the design) closes a connection
  whose receive window has held out-of-order messages for 10 s without the
  gap being filled while the peer kept sending.
- `max_packets_per_tick` is at least 2: a head message that does not fit
  beside the state chunk rides the tick's second packet, and with one packet
  per tick and a state every tick it could otherwise never go out. The state
  is never displaced from a tick's first packet.
- The packet log resolves an entry as lost when its slot is reused after
  256 packets without an ack, so a total ack blackout still shows as loss.
- The 600-byte resend budget is cumulative, but the first resent message of
  a packet may exceed it; otherwise a message above 600 bytes could never be
  retransmitted.
- A malformed packet has no effect at all: it delivers nothing, is not
  recorded in the replay window (so never acknowledged), and its header's
  acks, echo and `last_heard` are not applied either. The design text
  updates `ack_bits` at step 6 and keeps the header effects after a failed
  chunk walk; both would let a corrupt datagram acknowledge messages that
  were never delivered.
- Acks contribute no RTT samples at all (the design feeds both echo and
  ack samples into one estimator): the echo already measures the round
  trip without the peer's hold, and ack-based samples, even from the packet
  named by `ack`, include that hold and any reorder delay. Karn's rule is
  therefore moot as well.
- `ack` ahead of the last packet sent is a protocol error (`bad_ack`), not
  a silently ignored value: it would otherwise drive the loss scan and the
  gap rule over everything in flight on every packet.
- The constant is `NET_V2_PROTO_VERSION`, since `MULTI_PROTO_VERSION`
  already exists in `multi.h` with the v1 value and both will be visible in
  stage 1 translation units.
- Per-tick packet budgets are opened by `begin_tick` (also called from
  `build_outgoing`) once per full `tick_period`; §3.6's "2 per tick" is
  otherwise meaningless for a caller that builds every frame.
- The applied clock offset follows the target without slewing during the
  first 2 s after the first sample, so that a session does not start with
  an offset that is up to a jitter's worth wrong for several seconds.
- The library has no Boost dependency and the test is a plain program, as
  requested for stage 0.
