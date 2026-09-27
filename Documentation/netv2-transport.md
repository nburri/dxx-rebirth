# v2 network transport: library and simulation test (stage 0)

This is stage 0 of the plan in `network-protocol-v2.md` §8: the transport
layer as a self-contained library plus a simulation test. Nothing in the
game uses it yet; stage 1 puts it under the existing UDP socket.

## Files

| File | Content |
|---|---|
| `common/main/net_v2.h` | Wire constants (§3.1–3.7, §2.2), the 34-byte `packet_header`, the `chunk_header`, the state part byte, little-endian helpers, sequence arithmetic. Standard library only. |
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
| reordered echo | – | Packet 1 arriving after packet 2 does not move the echo fields; packet 2's echo sample is exact and packet 1, acked in the same peer packet, is neither measured nor bounded by its ack: `srtt` and `rttvar` stay exact. |
| long blackout | – | 34 000 packets without an ack, then acks resume while every other packet of ours is lost: 30+ of the 75 lost are counted within 150 packets (the bitfield rule, not slot reuse). |
| malformed replay | – | 20 copies of one corrupted datagram: 1 protocol error, 19 duplicates, connection open, the message arrives by retransmission. |
| latest-wins wrap | – | 33 000 event-only packets, then 200 `state` chunks: none dropped; a reordered older `state` still is. |
| grant closes tick | – | One packet built in tick 0, then exactly 2 (not 3) in tick 1. |
| hostile ack | – | `ack` 5000 after 5 packets sent: `bad_ack`, no packet resolved, no resend, one protocol error; its replay is `bad_ack` again, not counted; `ack` 5 is accepted. |
| event head-of-line | – | A 256-byte event is refused; ten 4-byte events queued behind a 255-byte one all go out beside a 1000-byte state at once; the big one is dropped after 8 packets and fits when sent alone; of 70 queued events the oldest six make room and the 64 newest go out in order. |
| zero tick period | – | `tick_period` 0 and `max_packets_per_tick` 0 are clamped (to 1 and 2) and the connection sends. |
| echo pinning | 30 ms | 600 forged packets over 5 s whose `ack` names one real old packet with its true `send_time` and `echo_delay` 0 (and a variant naming the next packet with the stale time): `srtt`, `rttvar` and the offset target are unchanged. |
| corrupt acks | 40 ms | A packet with reliable messages is lost and the peer's next datagram falsely acks it but is corrupt: nothing of it is applied, the messages are retransmitted, all 200 arrive, one protocol error. |
| stream stalled | – | Message 1 arrives, message 0 never does, the peer keeps sending state: the receiver closes with `stream_stalled` after 10 s. |
| corrupted then intact | – | A corrupted copy of a packet, then the intact copy: the intact one is accepted and its message delivered; one protocol error. |
| held buffers retained | – | Fifty ticks of a message held out of order and released by the next packet: zero allocations inside `on_receive` after a ten-tick warm-up (the test binary counts every `operator new`). |
| sequence jump bound | – | A 240 Hz host sending four-part bundles through a 4.5 s blackout (4320 packets, more than the old fixed 4096): the client, told the peer's tick, resumes without a `bad_seq`. |
| wrapped ack | – | Acks 40000 and 1 on a fresh connection, and 11 and 40000 after ten packets: `bad_ack`; 1 and 10 after ten packets: accepted. |
| sequence wrap | 60 ± 15 ms, 15 % loss, 15 % reorder, 3 % duplication | 140 000 ticks (two packet-sequence wraps per side, four message-sequence wraps): every message delivered in order, nothing rejected but duplicates and malformed-free, no protocol errors, the state stream monotonic. |
| forged seq far ahead | – | A well-formed keepalive with bit 14 of its `seq` flipped: `bad_seq`, one protocol error (a replay counts none), the stream continues and the peer sees no `bad_ack`. |
| clock stall | 50 ms | A 50 ms clock step being slewed, then a 2.5 s stall: the applied offset moves while samples remain in the window and holds once it is empty; the target equals the applied value. |
| corrupted future seq | – | A corrupt datagram whose `seq` byte reads as a future sequence does not prevent the real packets 2, 3 and 4 from being accepted. |
| big message, second packet | – | A 1 KiB message beside an 1100-byte state: the state goes out first, the message in the tick's second packet; `max_packets_per_tick` 1 is clamped to 2. |
| held-back packets | 40 ms, 20 % held 33–50 ms | 1200 messages over a lossless link with a fifth of the packets delayed: at most a dozen resent by the RTO; the held packets, acked after later ones were echoed, bound `rttvar` and the recent excess by their acks, `srtt` stays at the true 80 ms. |
| closed connection | – | `send_unreliable` returns false and `set_unreliable_state` is ignored once the connection is closed. |
| two-part bundle | 30 ms | Two 900-byte parts every tick plus a message every third tick: each part delivered on all 200 ticks, none dropped. |
| bundle with backlog | 30 ms | Two 900-byte parts and a 1 KiB message every tick: both parts on every tick, all messages delivered, three packets per tick at most. |
| slow peer | 60 ms | A 60 Hz host against a 10 Hz peer: host `srtt` within 5 % of the round trip (the peer's ack hold does not leak in). |
| two packets per tick | 30 ms | A host sending two packets per tick against a one-packet peer: `srtt` within 5 %. |
| frame-rate caller | – | 5000 frames at 500 Hz setting the state only when `begin_tick` opened a tick: 600 ticks, 600 states delivered, none dropped. |
| update() first | – | The same pattern with `update()` called before `begin_tick`: 600 ticks still reported, none twice, 600 states delivered. |
| event beside backlog | 30 ms | A 250-byte event per tick beside a standing backlog of 900-byte reliable messages: 60 of 60 events delivered, none dropped. |
| reliable before events | 30 ms | A 1000-byte state and a 250-byte event every tick with a 1 KiB message queued: the message goes out in the first tick (the round-12 event reservation starved it for good), all events delivered, none dropped. |
| bound hold at 30 Hz | – | A caller building every second tick takes a 233 ms bound: the RTO covers it for the second of hold and has faded to less than a quarter half a second later, at wall-clock speed (aged per grant it would still be whole). |
| send_unreliable types | – | `state`, `input`, `reliable`, `session` and an unknown type are refused (false, counted in `unreliable_dropped`); only `event_u` is queued. |
| held message view | – | Message 2 arrives first and is held, its datagram buffer is overwritten, then message 1: both delivered intact, message 1 as a view into its own datagram. |
| to_peer_time wrap | 30 ms | Client clock 2^32 units + 3 s ahead of the host: `to_peer_time` is within 5 ms of the host's wire stamp under `net_time_diff`. |
| echo of an old unechoed packet | 30 ms | 60 pps host against a 10 pps client (five packets in six unechoed): a forged newest client packet echoing the 3 s old packet 61 with its true `send_time` and `echo_delay` 0 leaves `srtt`, `rttvar` and the offset target unchanged. |
| bound excess | – | The estimator alone: 80 ms samples, then a 130 ms bound; the RTO covers 130 ms plus the slack right away, still after 100 more samples, while smaller bounds keep coming and for a second after the last, and is back at its steady value two seconds later; a bound at `srtt` after wide samples leaves `rttvar` unchanged (widen only). |
| parts after the first packet | – | A state and a message in the tick's first packet, then a 2 × 900-byte bundle: the budget follows, both parts go out in the same tick (three packets), the second message on the next. |
| late peer packet | – | The peer's older packet arriving 4 s after its newer one: accepted, its acks honoured, `srtt` unchanged, `rttvar` widened by the delay. |
| late ack of lost packets | – | Acks of packets 1–5 arriving after those of 6–70: the packets are given up and flagged, the late ack takes them back, no message is resent. |
| event beside a big state | 30 ms | A 1000-byte state and a 250-byte event every tick: all 100 events delivered in the tick's second packet, none dropped. |
| gap rule counts acks | – | Six packets, only the sixth acknowledged first: no gap resend; packets 4–6 acknowledged and 1–3 lost: three gap resends. |
| rejected sample | – | A forged echo with `echo_delay` beyond the round trip is rejected without spending the packet's sample; the genuine echo that follows is taken. |
| set_peer_tick | – | Setting the peer's tick to 1/10 s on a live connection grows `rto` by exactly the hold difference and leaves `srtt` alone; zero terms restore the own tick. |
| parts reordered | – | Part 1 arriving before part 0 of one tick: both applied; a stale part 0 is dropped while an older part 1 that is still the newest of its part applies. |
| first grant | – | A connection created at 0 and first built at 3 s gets one tick (2 packets), not a burst. |
| exact 60 Hz caller | – | 36 000 steps of exactly 1/60 s with a permanent backlog: never more than 2 packets per step. |
| bulk transfer + state | 30 ms | 20 × 900-byte messages queued while a 350-byte state is set every tick: 207+ of 210 states delivered, none dropped, all messages in order. |
| peer tick period | 10–85 ms, no jitter | A 60 Hz host (`peer_tick` 1/30) against a 30 Hz client: zero retransmissions over four latencies. |
| 240 Hz caller | – | `build_outgoing` called four times per tick period with a 90 KiB backlog: never more than 2 packets in any 16.7 ms window, everything delivered. |
| window in one packet | – | 300 empty messages queued at once: the first packet carries exactly 256, the receiver accepts it, the remaining 44 follow after the ack. |
| packets per tick | 30 ms | A 90 KiB backlog of 1 KiB messages drains at no more than 2 packets per tick and arrives in order. |
| unaligned peers | 1–100 ms, no jitter, no loss | Six rounds with random tick phases: zero retransmissions, `rto ≥ srtt + tick`. |
| ack blackout | 30 ms, acks zeroed | 500 packets without an ack: more than 200 counted lost once their log slots are reused, loss estimate above 0.9. |
| echo_delay clamp | – | A packet built 100 units before the receive stamp carries `echo_delay` 0 and the peer's RTT sample is the true value. |
| unreliable chunks per packet | – | Two `state` chunks in one packet are both delivered; a reordered older packet's `state` is dropped while its `event_u` is kept; `send_unreliable` refuses every type but `event_u`. |
| f: fuzz | – | 200 000 random datagrams are all rejected without effect; 100 000 packets with a valid header and random chunk bytes, and 100 000 mutations of captured real packets, never crash and a malformed packet delivers nothing; 16 malformed packets close the connection with `protocol_error`. |
| g: clock | 50 ± 20 ms, host clock about to wrap, client 1234 s behind | The client's offset target is within 20 ms after 8 packets and within 5 ms after 2 s; the host's estimate is the negative; a 300 ms clock step is followed within 2.5 s; after a 20 ms step the applied error shrinks by 5 ± 1.5 ms over the second following the window's expiry (slewed, not jumped) and settles. |

The simulated link delivers a packet at its arrival time, as the game does
by reading the socket every frame; packets are *sent* on the 60 Hz tick.
Each peer can tick with its own phase offset (`sim_world::phase`).

## Using the library

```cpp
using namespace dcx::net_v2;
connection c{{.session_id = sid, .peer_token = tok, .local_player_id = 0, .remote_player_id = 3}, now};
// connection_config also has tick (a period as numerator/denominator net
// units, default 65536/60 = exactly 1/60 s), max_packets_per_tick
// (default 2, at least 2) and peer_tick (default: same as tick; stage 1
// sets it from the handshake).  report.reliable and report.unreliable are
// spans into connection storage (no allocation per datagram) holding views:
// into `datagram` (valid while it is; unreliable ones with the §3.8 part
// index/count), or, for a message that had been held out of order, into
// storage the connection keeps until its next on_receive.  Copy what must
// outlive that.  to_peer_time(local) is a wire stamp (net_time): the
// offset is known modulo 2^32 only; compare with net_time_diff.
// begin_tick returns the ticks granted since it was last asked (0 when
// none); update() and build_outgoing grant ticks too but never consume that
// report, so any call order works.  A game loop that runs faster than the
// tick sets the state only when begin_tick returned non-zero, so no state
// is replaced before it was sent.

c.enqueue_reliable(msg_type, payload);            // ≤ 1024 bytes, or too_large
c.set_unreliable_state(chunk_type::state, bundle); // latest wins, sent once
c.set_unreliable_state(chunk_type::state, 1, 2, part_b); // §3.8: part 1 of 2, latest wins per part
c.set_peer_tick({65536, 30});                      // once the handshake tells the peer's tick
c.send_unreliable(chunk_type::event_u, bytes);     // best effort

c.update(now);                                     // optional, any order
if (c.begin_tick(now))                             // once per tick period;
    c.set_unreliable_state(chunk_type::state, bundle); // a frame-rate loop sets
for (;;) {                                         // the state only then
    const auto packet{c.build_outgoing(now)};
    if (packet.empty()) break;
    sendto(..., packet);
}

auto report{c.on_receive(datagram, now)};          // for every datagram read
for (auto &m : report.reliable) handle(m);         // in order, exactly once
for (auto &u : report.unreliable) apply(u);        // newest state, every event
```

`build_outgoing` produces a packet when there is a state part, an event, a
reliable message to send or resend, an ack owed for a received reliable
message, or 100 ms have passed since the last packet (keepalive). No more
than `max_packets_per_tick` (2, §3.6) are built per tick, or one more than a
bundle needs when it needs several (§3.8); a backlog beyond that waits for
later ticks. `begin_tick(now)` grants that budget for every whole period
elapsed since the last grant (in exact rational arithmetic with one unit of
tolerance for the caller's clock rounding, so a true 60 Hz caller never sees
a spurious double grant; the very first grant is a single tick, however long
the connection existed before), and judges the timeouts, the RTO and the
bound hold's age once per granted tick (a retransmission cannot go out more
often anyway), advancing the tick origin by whole periods (not resetting it
to `now`), so a caller at any rate gets exactly one budget per period, a
frame that spans two ticks gets both. A grant replaces whatever was left of
the previous one and is capped at two ticks, so unused ticks never pile up
into a spare and a long stall does not end in a burst.

Within a tick the packets are composed in one fixed priority order:

1. **State/input parts.** The first packet carries every pending part that
   fits, part 0 always; a part that does not fit opens a further packet,
   and a bundle that needs n > 1 packets raises the tick's limit to n + 1
   (judged on every packet of the tick, so parts set after the first packet
   still fit it).
2. **Reliable messages.** Resends first, then the queue in order, each only
   if it fits, stopping at the first that does not (a message that does not
   fit blocks the ones behind it, so a large one cannot be overtaken). A
   head that does not fit beside the parts gets the tick's next packet, by
   itself if need be: no message size can starve, and no message stream
   can displace the state.
3. **Events** (`send_unreliable`) fill whatever is left, in queue order; one
   that does not fit is skipped, not a head-of-line block, and dropped after
   `NET_V2_EVENT_SKIP_MAX` (8) skips. Only the tick's first packet reserves
   room for the head event beside the parts (a head message it displaces
   gets the next packet anyway); later packets put reliable messages first,
   so a message blocked out of the first packet is never blocked again by an
   event. Under a sustained reliable backlog events may therefore be skipped
   and dropped: they are cosmetic.

The first packet of a tick goes out whenever anything is due; a second and
further one only while something is left over (a due message, a pending part
or a pending event), within the limit above. The tick's credit is spent only
when a packet is really built.
`build_outgoing` calls `begin_tick` itself. `on_receive` applies the checks of §3.7
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
packet with that sequence. A well-formed packet whose `seq` is further
ahead of the newest seen than the peer could have sent within the 5 s
timeout (`bad_seq`) is rejected and counted the same way: taking it as the
new highest would reject every real packet that follows as a duplicate until
the timeout and make our acks protocol errors at the peer. The bound follows
the peer's tick (`set_peer_tick`): the ticks in the timeout, times the
packets a tick may carry (`max_packets_per_tick` or a full bundle's parts + 1,
whichever is more), times a margin of `NET_V2_SEQ_JUMP_MARGIN` (2), capped at
32767; 3000 for a 60 Hz peer, 12010 for one at 240 Hz. `bad_ack` covers
acks ahead of our newest packet and, until half the sequence space has been
used, acks further behind it than we have sent packets (a forged ack 32768
or more ahead reads as one far behind); after that an honest ack may lag by
any distance following a one-way blackout, so only "ahead" is an error, and
a far-behind ack is harmless since the packet log is consulted by sequence. Should the receive window hold out-of-order
messages for 10 s without the gap ever being filled while the peer keeps
sending, the connection closes with `stream_stalled` rather than blaming the
peer for a protocol error. Packets with
`flags.UNCONNECTED` are reported as `unconnected` and left to the session
layer (stage 1), which also decides which `connection` a datagram belongs to
(`packet_header::read` gives it `session_id`, `peer_token` and `player_id`).

Unreliable chunks: `state`/`input` chunks carry a part byte (index and count,
§3.8; `set_unreliable_state(type, part, count, bytes)`), and the report
carries every `event_u` chunk and every `state`/`input` part of the packet,
unless the packet is older (by `seq`) than one, still inside the 64-packet
reorder window, whose chunk of that type *and part* was already delivered (a reference older than the window has been superseded
by every packet since and no longer suppresses anything, so a type unseen for
32 768 packets cannot wedge on a wrapped comparison). All chunks of one packet
count as equally new, so a bundle split into two `state` chunks (§3.8)
arrives whole and in order. The transport keeps no copy of the latest state;
the consumer keeps what it needs from the report. Events (`send_unreliable`)
go out in queue order as far as they fit; one that does not fit beside the
state chunk is skipped, not a head-of-line block, and dropped after
`NET_V2_EVENT_SKIP_MAX` (8) packets. An event payload is at most
`NET_V2_MAX_EVENT` (255) bytes (events are cosmetic; §6.9 gives each a u8
length): they wait in `NET_V2_EVENT_QUEUE_MAX` (64) fixed slots queued by a
ring of slot indices, so queueing one allocates nothing and skipping one
copies nothing; when the ring is full the oldest is dropped. Reliable
messages held out of order are copied into window slots whose buffers are
swapped with the retained delivery buffers and never freed, so a held
message allocates nothing either once the connection has warmed up.

`state()` is `connecting` until the first valid packet arrives, then
`connected`, and `closed` with a `close_reason` (`timeout`,
`unacked_timeout`, `queue_overflow`, `protocol_error`, `stream_stalled`,
`local`) after which
the connection neither sends nor accepts anything.

## What the stats mean

`connection::stats()` returns a `connection_stats` snapshot:

| Field | Meaning |
|---|---|
| `rtt_valid`, `srtt`, `rttvar`, `rto` | §3.5 estimator, in net time units (1/65536 s; `× 1000 / 65536` for ms). `srtt` is measured only by the echo fields of the newest packet received, so it excludes the peer's hold time: an echo describes the packet the header's `ack` names, and it is taken only if that packet is in our log with the recorded `send_time` equal to `echo_time`, has not been sampled before (each of our packets yields one sample at most; a repeated echo yields none) and is newer than the packet last sampled: a conforming peer echoes the newest packet it received, so an echo of an older one is never genuine and is ignored (a 60 pps host against a 10 pps client leaves five packets in six unechoed, which a hostile client could otherwise name with `echo_delay` 0 to drive `srtt` to seconds); the sample is `now − sent_at − echo_delay` from our own log. A reordered older peer packet carries its own reorder delay in every sample it could give; that delay is exactly what a late ack costs, so its echo widens `rttvar` only (a bound) and never moves `srtt`, while its acks are honoured. A peer therefore cannot steer `srtt` at all: not by pinning one real old packet with a small delay, not by naming an old unechoed one, and not by holding ack bits back, because an ack is never a sample. A packet that is first acked, by the peer's newest packet, only after a later-sent packet of ours had already been echoed arrived out of order at the peer and will never be echoed; that packet's ack widens `rttvar` (once, a bound: an ack includes the peer's hold), so that its long round trip reaches the RTO. A packet left unechoed merely because the peer sends fewer packets than we do is not bounded by its ack, which would carry the peer's hold into the RTO twice. Bounds are taken only once the estimator has a sample, and a packet keeps its one sample until then. `rto` is `clamp(srtt + max(4·rttvar, excess) + hold + tick, 50 ms, 1 s)` with `tick` the own period rounded up to whole units, `hold` the larger of the own and the peer's, and `excess` the largest bound above `srtt` seen since bounds last stopped coming (kept for a second after the last bound, then fading by a sixteenth per tick: `rttvar` forgets a bound within a few samples, but the next packet may be held just as long), and 1 s before the first sample; the two holds are added because the samples deliberately exclude them, and `rttvar` is floored at a quarter of the hold. This `srtt` is what the HUD will show as ping. |
| `loss_estimate` | Moving average (1/64 per packet) of the fraction of our packets the peer never acknowledged. A packet counts as lost once the peer's `ack` is more than 64 ahead of it, or when its slot in the 256-entry packet log is reused without an ack (no acks at all), so the value lags by one to four seconds at 60 pps and settles slowly on a link that just became clean. |
| `packets_sent/received/rejected/acked/lost` | Per direction. `rejected` counts every datagram `on_receive` dropped, including duplicates. A packet given up as lost is taken back if its ack arrives late (the counts and the loss estimate follow), and its messages are then not resent. |
| `messages_enqueued/delivered` | Reliable messages queued here / delivered to the caller from the peer. |
| `message_sends`, `message_resends`, `resends_by_gap`, `resends_by_rto` | Transmissions of reliable messages; the resend split says which rule detected the loss: the gap rule (three packets sent after the message's own acknowledged, counted from `ack` and the bits between, so a lone reordered ack far ahead resends nothing) or the RTO. Resends well above the loss rate mean the RTO is too tight for the link. |
| `unreliable_dropped` | Latest-wins data that never went out: a state part set again before it was sent, the parts of an old layout when a bundle's part count changes, a payload above `NET_V2_MAX_STATE_PART` or an invalid part number, an event above `NET_V2_MAX_EVENT` (255 bytes) or of a type other than `event_u`, an event pushed out of the 64-slot ring, and an event skipped in `NET_V2_EVENT_SKIP_MAX` (8) consecutive packets. |
| `protocol_errors` | Malformed packets from the peer; 16 within 10 s close the connection. |
| `queue_messages`, `queue_bytes` | Reliable messages queued or in flight, and their payload bytes; the connection closes at 512 messages or 96 KiB. |
| `in_flight` | Sent but not yet acked, at most 256. If it sits at 256 the peer is not acking. |
| `recv_window_pending` | Messages received out of order and held until the gap before them is filled. |
| `clock_offset_valid`, `clock_offset`, `clock_offset_target` | §2.2: peer clock ≈ local clock + `clock_offset`. The target is the offset of the minimum-RTT sample in the last 2 s; the applied value slews toward it at 5 ms/s or jumps if more than 100 ms away (and follows it directly during the first 2 s of a session). When a stall empties the window the target freezes at the applied value until a fresh sample arrives, so the slew never runs on toward an expired sample. Only meaningful on the client (the host is the reference), though both sides compute it. |
| `last_heard` | Local time of the last valid packet from the peer. |

## Deviations from the design text

- Packet sequence numbers skip 0 on wrap so that `ack == 0 && ack_bits == 0`
  always means "nothing received yet".
- RTT samples are taken from the echo fields only, and only from echoes
  that progress (each newer than the packet last echoed); acks, and the
  echoes of reordered older peer packets, are bounds: they widen `rttvar`
  alone and set the recent excess the RTO covers. In exchange the RTO adds
  `hold + tick` to `max(4·rttvar, excess)`, `hold` being the longer of the
  two tick periods, and floors `rttvar` at `hold/4`: the samples exclude the
  holds, and with them `rttvar` would decay to zero on a steady link and the
  plain formula would retransmit every message whose ack is held for a
  tick.
- `echo_delay` is clamped to `[0, 65535]`; a caller whose build time is
  behind its receive stamp sends 0 instead of a wrapped value.
- The echo is verified against the packet log: the header's `ack` names the
  echoed packet (a conforming peer echoes the newest packet it received,
  which is also its `ack`), and a sample is taken only if that packet's
  recorded `send_time` equals `echo_time` and it was not sampled before.
  Without this a peer could set `srtt` to seconds with a made-up
  `echo_time`, and the host will rewind hits by that RTT.
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
- `state`/`input` chunk payloads start with a part byte (index low nibble,
  count high nibble, at most 4 parts; `net_v2.h`), so that a bundle split
  over two packets (§3.8) is latest-wins per part; the design leaves the
  parts self-describing at the application layer only, which the transport
  cannot key on. A pending part opens a tick's second packet, and a bundle
  that needs several packets raises the tick's packet limit to one more than
  it needs, so a blocked head message never displaces a part.
- The tick period is a rational (`tick_period{numerator, denominator}`), so
  1/60 s is exact; the design's integer `fix` accumulator would drift by
  0.24 ms/s.
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
- The design text as first written fed every ack into the estimator; those
  samples include the peer's hold, which the echo excludes, and a peer can
  hold ack bits back at will. Acks are therefore never samples: an ack
  bounds `rttvar` (once) only for a packet the echo can no longer measure
  (acked after a later packet was echoed, so it arrived out of order at
  the peer), so that a delayed packet's long round trip is still seen; see
  the previous bullet for the bound excess. Karn's rule is moot: packets
  are never retransmitted, only messages are.
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
