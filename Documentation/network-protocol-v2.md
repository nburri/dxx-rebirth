# DXX-Rebirth multiplayer network protocol v2 (design)

This document specifies a replacement for the UDP multiplayer protocol described
in `Documentation/network-protocol.md` ("v1" below). It targets D2X-Rebirth
(`DXX_BUILD_DESCENT == 2`), 2–8 players over the internet, mostly anarchy and
team anarchy. D1X-Rebirth uses the same design with the D1 message subset; D1
differences are called out only where they change a wire layout.

v1 sections are cited as "v1 §n". Line numbers in code references are for the
`experimental-netcode` branch at the time of writing; function names are the
stable anchors.

Conventions:

- All multi-byte integers are little-endian (as in v1, `PUT_INTEL_*` /
  `serial::writer::le_bytebuffer`).
- `fix` = 16.16 fixed point, sent as `i32`. `vms_vector` = three `fix` (12 bytes).
- `u8/u16/u32/u64/i16/i32` have their usual meaning.
- "host" = player slot 0 (`multi_i_am_master()`), "client" = any other player.
- "tick" = one step of the host's fixed-rate network loop (default 60 Hz).
- "net time" = the host's clock, in `fix` units (1/65536 s), transmitted as the
  low 32 bits (`u32`, wraps after 18.2 h; all comparisons use wrapping `i32`
  differences).

---

## 1. Goals and non-goals

### 1.1 Goals

1. **One authority.** The host decides everything that must agree between
   machines: pickups, damage, kills, object creation and removal, powerup
   respawns, spawn points, level flow and every random number that matters.
   Clients send their own ship state and their intents; they receive
   authoritative state.
2. **Smooth remote ships that are where they appear to be.** Remote entities are
   interpolated between timestamped snapshots on the receiver, and hits are
   judged by the host against a position history rewound to the moment the
   shooter saw. The drawn hull and the hull that can be hit are the same object.
3. **A real reliable layer.** Per-packet sequence numbers, piggybacked
   acknowledgements with an ack bitfield, RTT-derived retransmission timeouts,
   selective retransmission, and two delivery classes. No stop-and-wait, no
   fixed 250 ms timer, no packet dropped because an earlier one is missing.
4. **Robustness.** Every packet is validated (length, ranges, session and peer
   identity), replay and reorder are handled by sequence numbers, players are
   identified by a per-session token and not only by their address.
5. **Bounded resources.** Every queue has a size limit and a defined behaviour
   on overflow and on timeout. Every UDP payload is at most 1200 bytes.
6. **Keep what works.** UDP, LAN discovery, the tracker (registration, game
   list, hole punching), the join/game-list menus, and the gameplay semantics
   of the `MULTI_*` messages are kept where the authority model allows it.

### 1.2 Non-goals

- Backwards compatibility with v1. `MULTI_PROTO_VERSION` becomes **100**; v1
  builds see a different first byte and refuse each other (§3.1).
- Host migration. When the host leaves, the game ends (as in v1).
- Deterministic lockstep or host re-simulation of client ship physics. The
  client keeps simulating its own ship locally (Descent's physics on the 500 Hz
  branch carries per-object remainders that are not reproducible on another
  machine); the host validates the reported state instead of recomputing it.
- Cheat-proofing beyond plausibility checks. The checks in §5.5 and §6 stop
  common desynchronisation bugs and casual manipulation; they are not an
  anti-cheat system.
- Changing the tracker program. Tracker packets keep their v1 layouts (v1
  §3.3); only the embedded game info blob changes.

### 1.3 v1 problems this fixes

| v1 "Known issue" (v1 §8) | v2 answer |
|---|---|
| 1. Reliable delivery is strictly in order with a fixed 250 ms resend, a gap drops all later packets, 5 s → kick | §3.4: selective repeat with a receive window; a lost packet delays only the messages it carried, resent after an RTT-based RTO (min 50 ms); acks are on every packet. Timeouts and queue limits in §3.6. |
| 2. Many gameplay messages are unreliable (`MULTI_FIRE`, chat, `MULTI_GUIDED` release, heartbeat, …) | §3.3: every event that changes game state travels on the ordered-reliable class. Only state snapshots and cosmetic effects are unreliable. |
| 3. Object sync for mid-game joins is not retransmitted, tolerates 10 missing objects, misparses on allocation failure | §4.4: the level snapshot is a sequence of reliable messages with explicit counts and a final checksum; parse errors abort the join with a reason. |
| 4. Host-sent messages lose the player they are about (`MULTI_FLAGS`, `MULTI_MARKER`, formerly `MULTI_KILL_HOST`) | §6: every v2 message that is *about* a player carries that player explicitly; the transport-level sender is only used for authorisation. |
| 5. `MULTI_DROP_FLAG` layout mismatch | Gone: spat objects are created by the host with an explicit initial velocity (§6.3); no seeds are shared for object placement. |
| 6. D1/D2 request id not checked | §4.2: the join request carries the game id and program version; both are checked and a mismatch is answered with a reason. |
| 7. Limited input validation | §3.7: fixed validation order for every packet; every chunk and message has an explicit length; every index is range-checked before use. |
| 8. Quitting is not confirmed | §4.6: `LEAVE` is reliable and the leaver waits for the ack (bounded by 1 s). |
| 9. Unwritten bytes in `MULTI_PLAYER_DERES` | Gone: the message is replaced (§6.4). |
| 10. No host migration | Still a non-goal. |

Pain points from the fork changelog (`Documentation/fork-changelog.md`):

| Problem | v2 answer |
|---|---|
| Duplicated pickups: each machine decided its own pickups with stale positions (#5, on hold) | §6.2: the host grants every pickup, judging the request against the requester's rewound position. Objects have a single network id, so stale requests cannot match a reused slot. |
| Client-side respawn of used items duplicated powerups (#3) | §6.3: only the host creates objects. |
| Death drops diverged because of double grant subtraction (#4) | §6.4: the host computes and creates the drops from its authoritative inventory; receivers create nothing themselves. |
| Relayed kills credited to the host (#9) | §6.5: `PLAYER_KILLED` names victim and killer explicitly and is only ever sent by the host. |
| Remote ship smoothing put on hold because the drawn ship lagged its hitbox by ~2.8 corrections (#19) | §2.5 and §5.4: there is no separate "real" position for remote ships. The interpolated position *is* the object position, and the host judges hits against its history at the shooter's view time. |
| Guided missile position pacing, forced sends, heartbeat priorities (#18) | §5.3: guided missiles are part of the host state bundle; level time is in the bundle header; no "forced" sends exist. |

---

## 2. Architecture

### 2.1 Roles

- **Host** (slot 0): runs the authoritative simulation for everything except
  the other players' ship movement. It owns all objects (§6.1), applies all
  damage, decides all pickups and spawns, drives level flow and the shared
  clock. It sends one state bundle per tick to every client and relays
  events. It is also a player, with zero latency to itself.
- **Client**: simulates its own ship locally (prediction), renders remote
  entities by interpolation, sends its ship state every tick, sends intents
  (fire, hit reports, pickup requests, door/trigger reports), and applies
  authoritative events and corrections from the host.
- Topology stays a **star**: clients talk only to the host. A client never
  learns other clients' addresses.

### 2.2 Clock

- The host's clock is `timer_query()` (`fix64`, 1/65536 s). Net time is its
  low 32 bits. The host's `GameTime64` and `ThisLevelTime` are derived from the
  same source, so the bundle header can carry level time as a tick-aligned
  value for free (§5.2).
- Every packet header carries `send_time`, `echo_time` and `echo_delay` (§3.1).
  From each received host packet, a client computes

  ```
  rtt     = now - echo_time - echo_delay          (all in fix, wrapping i32)
  offset  = send_time + rtt/2 - now               (host_time ≈ local + offset)
  ```

  and keeps the last 2 s of samples. The target offset is the sample with the
  smallest `rtt` in that window (minimum filter: queueing delay only ever adds
  to RTT, so the smallest sample is closest to the true one-way delay). The
  applied offset slews toward the target at most 5 ms per second, or jumps if
  the difference exceeds 100 ms (start of session, route change). This is a
  minimal NTP-style estimator: one sample per packet, 60 per second, no extra
  ping traffic.
- The host does the same computation to get each client's RTT, which replaces
  the v1 `ping`/`pong` packets; RTTs are shown to everyone through the bundle
  (§5.2).

### 2.3 Tick model

- The host runs a network tick at `Netgame.TickRate` Hz (default 60; allowed
  30, 60, 120; replaces `PacketsPerSec`, `MIN_PPS`/`MAX_PPS`). The tick is
  driven from `do_protocol_frame` with a `fix` accumulator exactly like
  `calc_d_tick` (`game.cpp:620`), so it is independent of the frame rate. Tick
  numbers are `u32`, count from 0 at session start and never reset on level
  change.
- On each tick the host: (1) processes received client packets, (2) records
  every player's position in the lag-compensation history (§6.6), (3) runs
  pending authoritative decisions (pickup grants, respawns, repopulation), (4)
  builds and sends one bundle to each client, containing the reliable messages
  that are due plus the state chunk.
- Clients send one packet per host tick period as well (their own accumulator
  at the same rate). Clients do not need to be phase-aligned with the host;
  everything is timestamped.
- The 30 Hz `d_tick_step` of the game (`DESIGNATED_GAME_FPS`) is unchanged and
  unrelated; nothing in v2 depends on it.

### 2.4 Authority model

| Thing | Who decides | How it reaches others |
|---|---|---|
| Own ship position, orientation, velocity | The owning client (predicted locally, validated by the host, §5.5) | Client → host in `INPUT`; host → all in the state bundle |
| Everything about weapons in flight (existence, origin, direction, seed) | Shooter proposes in `FIRE`; host validates and broadcasts | Reliable `FIRE` from host |
| Hits and damage to players, robots, reactor, walls, monitors | Shooter reports in `WEAPON_HIT`; host validates against rewound history and applies | Reliable `DAMAGE`, `PLAYER_KILLED`, `ROBOT_KILLED`, `WALL_STATE`, … from host |
| Shields, energy, inventory (weapons, ammo, keys, flags, orbs) | Host | Bundle (shields/energy), reliable `INVENTORY` on change |
| Object creation and removal (powerups, eggs, mines, flags, robots) | Host | Reliable `OBJ_CREATE` / `OBJ_REMOVE` |
| Pickups | Host, on client request | Reliable `PICKUP_GRANT` (+ `OBJ_REMOVE`) |
| Death, respawn point, invulnerability on appear | Host | Reliable `PLAYER_KILLED`, `PLAYER_SPAWN` |
| Doors, triggers, reactor, lights, walls | Host (on client report) | Reliable `WALL_STATE`, `TRIGGER`, `REACTOR_DESTROYED` |
| Robots (robot anarchy, coop) | Host simulates all robots | Bundle (positions, round-robin), reliable robot events |
| Level time, countdown, kill matrix, scores, team vector, bounty target | Host | Bundle header (time), reliable `SCORE_UPDATE`, `GAME_MODE_STATE` |
| Level start/end, join, leave, kick | Host | Reliable session messages |
| Chat, typing indicator, markers | Sender; host relays | Reliable |
| Cosmetic effects (sounds, sparks, afterburner blobs) | Sender; host relays | Unreliable event chunk |

Randomness: the only shared random values are those the host puts into
messages (weapon speed variance seed in `FIRE`, spawn point index in
`PLAYER_SPAWN`, `ShufflePowerupSeed`). Receivers seed `d_rand` from the
message immediately before the call that consumes it and never rely on the
global sequence staying in step (removes the fragility described in v1 §7).

### 2.5 What runs where

- **Own ship on a client**: as today. `object_move_one` → `do_physics_sim` with
  local controls, collisions with walls and with *interpolated* remote objects
  (bumping is cosmetic; damage from bumps is host-side, §6.5). The client sends
  the resulting state every tick. The host validates it (§5.5) and either
  accepts it as authoritative or sends a correction, which the client applies
  as a soft snap (§5.6).
- **Remote ships, guided missiles and robots on any machine**: their
  `movement_source` is set to `object::movement_type::None` while they are
  network-driven, so `do_physics_sim` no longer touches them. Each frame,
  before rendering and before collision detection of local objects, the
  interpolation step (§5.4) writes `pos`, `orient`, `segnum` (with
  `obj_relink`) and `velocity` (for effects and for extrapolation) from the
  snapshot buffer. Because this writes the *object*, everything that reads an
  object (renderer, `fvi`, sounds, HUD names, the shooter's local collision
  detection) sees the same position. That is why the "drawn ship lags its
  hitbox" problem cannot come back: there is exactly one position.
- **Weapons in flight**: simulated on every machine from the same `FIRE`
  parameters (origin, direction, seed, fire time). They can drift slightly
  between machines, which is why hits are *reported by the shooter* and
  *validated by the host* (§6.5) instead of being detected independently.
- **Host**: additionally runs the position history, the validators, the
  inventory and level-inventory accounting (`MultiLevelInv_*`, now exact
  because the host sees every pickup), robots, and all `multi_do_*` side
  effects that create or remove objects.

---

## 3. Transport layer

Files (new): `common/main/net_v2.h` (constants, wire structs, message ids),
`common/main/net_v2_transport.h` + `similar/main/net_v2_transport.cpp`
(connection, reliability, clock; no dependency on game state so it can be unit
tested), `similar/main/net_v2.cpp` (socket I/O, session, packet dispatch;
replaces most of `net_udp.cpp`).

### 3.1 Packet header (34 bytes)

Every UDP datagram, connected or not, starts with this header.

| Offset | Size | Field | Notes |
|---|---|---|---|
| 0 | 2 | `proto` | `MULTI_PROTO_VERSION` = 100. A v1 build reads byte 0 as `upid` 100, which is not a valid `upid` and is dropped (`build_upid_from_untrusted`). A v2 build drops anything whose first two bytes are not `100, 0`. |
| 2 | 4 | `session_id` | Random `u32` chosen by the host when the game is created (replaces `GameID`). 0 in discovery packets (`GAME_INFO_LITE_REQ`, `GAME_INFO_LITE`, `GAME_INFO_REQ`, `GAME_INFO`). |
| 6 | 4 | `peer_token` | Per-connection random `u32` assigned by the host in `JOIN_ACCEPT`. Client → host: the client's own token. Host → client: that client's token. 0 before a token is assigned (join handshake, discovery). |
| 10 | 1 | `player_id` | Sender's slot (0–7). `0xFF` when not yet assigned. Redundant with `peer_token`; used as a cheap consistency check and for logging. |
| 11 | 1 | `flags` | Bit 0 `HAS_RELIABLE`: at least one `RELIABLE` chunk follows. Bit 1 `KEEPALIVE`: no chunks, header only. Bit 2 `UNCONNECTED`: discovery or join packet (session/token rules relaxed as described in §4). Bits 3–7 reserved, must be 0. |
| 12 | 2 | `seq` | Sender's packet sequence number for this connection. Starts at 1, wraps `u16`. Every packet, including keepalives, consumes a number. |
| 14 | 2 | `ack` | Highest `seq` received from the peer (in wrapping order). |
| 16 | 8 | `ack_bits` | Bit `i` (0–63) set: packet `ack - 1 - i` was received. 64 bits cover 1.07 s at 60 pps and 0.53 s at 120 pps, which is more than any RTO in §3.5, so a retransmission is never triggered by a bitfield too short to report a late ack. |
| 24 | 4 | `send_time` | Sender's local clock (net time units) when the packet was built. |
| 28 | 4 | `echo_time` | `send_time` of the most recent packet received from the peer, or 0. |
| 32 | 2 | `echo_delay` | Time between receiving that packet and sending this one, in net time units, saturated at 65535 (1 s). Peers send at least every 100 ms (§3.6), so saturation only happens on a stalled link and the RTT sample is then discarded. |

Maximum UDP payload: `NET_V2_MAX_PACKET` = 1200 bytes (header included). This
is below the 1280-byte IPv6 minimum MTU minus headers, so no path in practice
fragments it. The receive buffer is 1500 bytes; anything longer than 1200 is
dropped before parsing.

### 3.2 Chunks

After the header, the payload is a sequence of chunks until the end of the
datagram:

| Offset | Size | Field |
|---|---|---|
| 0 | 1 | `chunk_type` |
| 1 | 2 | `chunk_len` (bytes that follow, 0–1163) |
| 3 | `chunk_len` | chunk payload |

A chunk that does not fit in the remaining bytes invalidates the whole packet
(§3.7). Chunk types:

| Id | Name | Class | Content |
|---|---|---|---|
| 0x01 | `RELIABLE` | R | A run of consecutive reliable messages (§3.4). |
| 0x02 | `STATE` | U | Host state bundle, whole or part (§5.2). |
| 0x03 | `INPUT` | U | Client ship state (§5.3). |
| 0x04 | `EVENT_U` | U | Best-effort cosmetic events (§6.9). |
| 0x05 | `SESSION` | – | Unconnected session messages (§4.2, §4.3). Only valid with `flags.UNCONNECTED`. |

Unknown chunk types make the packet invalid (a receiver cannot skip what it
cannot bound-check semantically; a length field alone is not enough to trust
the rest).

### 3.3 Delivery classes

| Class | Name | Guarantees | Used for |
|---|---|---|---|
| R | ordered-reliable | Every message is delivered exactly once, in the order the sender queued it, per direction of a connection. | Every event that changes game state: object create/remove, fire, hits, kills, pickups, doors, triggers, session and level messages, chat, inventory. |
| U | unreliable | Sent once, never retransmitted, may be lost, duplicated or reordered. Consumers keep only the newest (`STATE`, `INPUT`) or apply idempotently (`EVENT_U`). | Ship and object state, cosmetic effects. |

There is no "reliable-unordered" class. Ordering costs nothing extra with
selective repeat (a message only waits for the gaps *before* it), and every
game event either depends on an earlier one (a `WEAPON_HIT` on a `FIRE`, an
`OBJ_REMOVE` on an `OBJ_CREATE`) or is cheap to delay by one RTO.

### 3.4 Reliable messages and the `RELIABLE` chunk

Each direction of a connection has a message sequence `msg_seq` (`u16`, wraps).
A `RELIABLE` chunk carries a run of messages with consecutive sequence numbers:

| Offset | Size | Field |
|---|---|---|
| 0 | 2 | `first_seq` |
| 2 | 1 | `count` (1–255) |
| 3 | … | `count` messages |

Each message:

| Offset | Size | Field |
|---|---|---|
| 0 | 1 | `msg_type` (§6.10) |
| 1 | 2 | `msg_len` (0–1024) |
| 3 | `msg_len` | payload |

A single reliable message never exceeds `NET_V2_MAX_MESSAGE` = 1024 bytes and
is never split across packets. Larger transfers are split by the sender into
numbered messages at the application layer (§4.4). One packet may contain
several `RELIABLE` chunks (a fresh run and one or more retransmitted runs).

The host multiplexes: a message that must reach all clients is queued once per
connection (7 queues); each connection has its own `msg_seq`. This is what v1
does with per-recipient `pkt_num`, minus the shared 1024-entry store.

#### Sender

```
struct out_msg { u16 seq; u8 type; bytes payload; u16 in_packet_seq; fix64 first_sent; fix64 last_sent; u8 sends; }
send_queue:  deque<out_msg>   // queued, not yet sent, or marked for resend
in_flight:   map<msg seq -> out_msg>
packet_log:  ring[256] of { u16 packet_seq; fix64 sent_at; vector<u16> msg_seqs; bool acked }

on_tick(build packet):
    header.seq = ++local_seq; header.ack/ack_bits = receiver state (below)
    budget = 1200 - 34 - space reserved for the STATE/INPUT chunk of this tick
    first put messages marked RESEND (oldest first), then new messages from send_queue,
    each message only if 3 + msg_len fits in budget; stop at the first that does not fit
    (order is preserved: a message that does not fit blocks later ones, so a large
     message cannot be overtaken)
    group consecutive seqs into RELIABLE chunks
    record packet in packet_log with the msg_seqs it carries
    for each carried message: in_packet_seq = header.seq, last_sent = now, sends++

on_ack(header.ack, header.ack_bits):
    for each packet_seq in {ack} ∪ {ack-1-i | bit i set}, not yet acked:
        mark acked; rtt sample from its sent_at (only if sends of all its messages == 1,
        Karn's rule: no RTT from retransmissions)
        for each msg_seq in it: erase from in_flight
    for each in_flight message m not acked:
        lost_by_gap = (ack - m.in_packet_seq) >= 3 wrapping      // 3 later packets acked
        lost_by_rto = now - m.last_sent >= rto
        if lost_by_gap or lost_by_rto: mark RESEND (kept in in_flight, same msg seq)
```

Retransmissions are bounded per packet: at most `NET_V2_RESEND_BUDGET` = 600
bytes of resent messages per packet, so a burst of loss cannot starve the
state chunk; the remainder waits for the next tick (16.7 ms), which is far
below any RTO.

#### Receiver

```
recv_window: bitset[256] + storage, base = next_expected_seq
on RELIABLE chunk (first_seq, count, msgs):
    for each (seq, msg):
        d = seq - next_expected (wrapping i16)
        if d < 0:              duplicate, drop (already delivered)
        elif d >= 256:         beyond window: packet is invalid (§3.7), stop
        else: store msg at slot d if empty
    while slot 0 filled: deliver, pop, next_expected++
```

Delivery is to the game layer in order (`multi_process_data` equivalent). The
sender never has more than 256 messages in flight per connection (it stops
sending new messages when `in_flight` holds 256 or `send_queue` overflows,
§3.6), so a well-behaved peer never exceeds the receive window.

Acks are the packet header of every packet the receiver sends. `ack` and
`ack_bits` are updated when a packet passes validation (§3.7), regardless of
its content, so a peer sending only `STATE`/`INPUT` still acknowledges. If a
peer has nothing to send for 100 ms it sends a `KEEPALIVE` packet (header
only), so acks flow even when idle (menus, kill matrix screen).

### 3.5 RTT and RTO

Per connection (Jacobson/Karels, RFC 6298 constants):

```
first sample:   srtt = r;  rttvar = r/2
later samples:  rttvar = 3/4 rttvar + 1/4 |srtt - r|;  srtt = 7/8 srtt + 1/8 r
rto = clamp(srtt + 4 rttvar, NET_V2_RTO_MIN = 50 ms, NET_V2_RTO_MAX = 1000 ms)
```

- RTT samples come from acked packets (`sent_at` in `packet_log`) and from the
  `echo_time`/`echo_delay` of each received packet (§2.2). Both feed the same
  estimator.
- A message is retransmitted by the gap rule (3 later packets acked) *or* by
  the RTO, whichever comes first. With 60 packets per second the gap rule fires
  about 50 ms after the loss; the RTO is the fallback for the tail of a burst.
- No exponential backoff on repeated loss of the same message (the connection
  timeout in §3.6 bounds the damage; backing off would only make an unusable
  link fail more slowly).
- Values shown as "ping" in the HUD are the host's `srtt` for that player
  (`Netgame.players[i].ping`), distributed in the bundle.

### 3.6 Rate limits, queue bounds, connection lifecycle

| Limit | Value | On violation |
|---|---|---|
| Packet size | 1200 bytes | Sender: never built; receiver: dropped. |
| Packets per tick per connection | 1 normally; 2 if the reliable backlog does not fit next to the state chunk (the second packet carries reliable chunks only) | – |
| Reliable send queue (queued + in flight) per connection | 512 messages or 96 KiB | Host: kick that client, `kick_player_reason::queue_overflow` (new reason). Client: leave the game with the message "Connection to host too slow". |
| Messages in flight | 256 (receiver window) | Sender stops taking new messages from the queue until acks arrive. |
| Oldest unacked reliable message | 10 s | Same as queue overflow (this replaces the v1 `pkttimeout`; a message unacked for 10 s means the link is dead or unusable). |
| No valid packet received | `NET_V2_TIMEOUT` = 5 s (v1 `UDP_TIMEOUT`) | Host: disconnect the player, broadcast `PLAYER_LEFT(timeout)`. Client: "Host left the game", return to menu. |
| Keepalive | header-only packet after 100 ms of silence | – |
| Level snapshot pacing (§4.4) | 32 KiB/s per joining client | – |
| `WEAPON_HIT` / `PICKUP_REQUEST` / `FIRE` from one client | 64 per second combined; excess messages are dropped by the host with a console warning | Protects the host from a flood; a legitimate client sends at most ~25/s (vulcan 20/s + hits). |
| Discovery replies (`GAME_INFO_LITE`) | 8/s total, `GAME_INFO` 2/s per requester (v1 behaviour) | Excess requests ignored. |

Connection states (per peer, on both ends):

```
none → (JOIN_REQUEST / JOIN_ACCEPT) → connected(joining) → connected(playing)
connected → LEAVE/KICK acked, or timeout → closed (state kept for 2 s to drop stragglers, then forgotten)
```

A connection is created on the host when it sends `JOIN_ACCEPT` (token
assigned), and on the client when it receives it. The address of a connection
may change: if a packet arrives with a valid `session_id`, `peer_token` and a
`seq` within 64 of the expected one but from a new address, the host adopts the
new address (NAT rebinding) and logs it. A packet with a known token from a new
address with an implausible `seq` is dropped.

### 3.7 Validation order (every received datagram)

1. Length ≥ 34 and ≤ 1200, else drop.
2. `proto == 100`, else drop silently (a v1 build or noise).
3. `flags` reserved bits are 0, else drop.
4. If `flags.UNCONNECTED`: `session_id` must be 0 (discovery) or the local
   session (join), `peer_token` must be 0, and the only chunk allowed is one
   `SESSION` chunk. Handled by §4. Rate limited.
5. Otherwise `session_id` must equal the local session and `peer_token` must
   name a live connection whose `player_id` equals the header's, else drop.
   Clients additionally require the source address to be the host's address
   *or* the token to match with a plausible `seq` (the host may also rebind).
6. Replay/reorder: `d = seq - highest_seen` (wrapping `i16`). `d > 0`: new
   highest, shift `ack_bits`. `-64 ≤ d ≤ 0` and bit not yet set: old but new
   to us, set the bit, process. Otherwise (duplicate or older than 64
   packets): drop. This is done before any chunk is parsed, so a replayed
   packet never reaches the game layer twice.
7. Update the peer's `last_heard`, RTT sample from `echo_*` if
   `echo_time != 0` and `echo_delay < 65535`.
8. Walk the chunks. Every chunk must fit; every `RELIABLE` message must fit
   its chunk; every `STATE`/`INPUT` record must have the exact size for its
   flags. The first violation drops the *rest* of the packet but keeps the
   header effects (acks, RTT) already applied, and increments a per-peer error
   counter; 16 invalid packets within 10 s from one peer disconnect it
   (`kick_player_reason::protocol_error`, new).
9. Only then are messages delivered and the header's `ack`/`ack_bits`
   applied to the sender state.

All indices inside messages (player ids `< N_players`, net ids that resolve
to an object, segment numbers `≤ Highest_segment_index`, wall/trigger numbers
within their arrays, weapon ids within the tables) are checked by the message
handler before use; a failed check drops that message and counts as one
protocol error for the peer.

### 3.8 Fragmentation policy

- There is no transport-level fragmentation. Every reliable message is ≤ 1024
  bytes and fits in one packet with room for the header and a chunk header.
- The state bundle may exceed one packet only in robot games (§5.2). It is
  then split into two `STATE` chunks in two packets, each self-describing
  (its own `player_mask` / record counts), and each applied independently;
  the receiver does not wait for both.
- Application-level splitting (level snapshot, §4.4) uses explicit part
  numbers and totals in the messages themselves.
- Why not IP fragmentation or a fragment chunk: a lost fragment loses the
  whole datagram, doubling the effective loss rate for large packets, and
  nothing in the protocol needs more than 1 KiB at once except the snapshot,
  which is a stream anyway.

---

## 4. Session layer

Session messages travel either as the single `SESSION` chunk of an
`UNCONNECTED` packet (discovery, join) or as ordered-reliable messages once a
connection exists. Both use the same message framing as §3.4 (`msg_type`,
`msg_len`, payload) so one parser serves both.

### 4.1 Game discovery and game list

Unchanged flow (v1 §2.2), new layouts. The client broadcasts
`GAME_INFO_LITE_REQ` to `255.255.255.255:42424` and `ff02::1` and asks the
tracker; hosts reply with `GAME_INFO_LITE`; the host also broadcasts
`GAME_INFO_LITE` every 10 s and on player list changes.

`GAME_INFO_LITE_REQ` (0x01), 10 bytes:

| Offset | Size | Field |
|---|---|---|
| 0 | 4 | `game_id`: `"D2XR"` (D1: `"D1XR"`). Checked on receive (fixes v1 §8.6). |
| 4 | 2 | program major |
| 6 | 2 | program minor |
| 8 | 2 | program micro |

`GAME_INFO_LITE` (0x02), 26 + names:

| Offset | Size | Field |
|---|---|---|
| 0 | 4 | `game_id` |
| 4 | 6 | program major, minor, micro |
| 10 | 4 | `session_id` |
| 14 | 4 | `levelnum` (i32) |
| 18 | 1 | `gamemode` (`network_game_type`) |
| 19 | 1 | `RefusePlayers` |
| 20 | 1 | `difficulty` |
| 21 | 1 | effective `game_status` (`network_state`, v1 §2.1) |
| 22 | 1 | `numconnected` |
| 23 | 1 | `max_numplayers` |
| 24 | 1 | `game_flag` (`netgame_rule_flags`, incl. the D2 hoard bits) |
| 25 | 1 | `tick_rate` (30/60/120) |
| 26 | ≤ 26 | `game_name`, NUL-terminated, at most 25 characters + NUL |
| … | ≤ 26 | `mission_title`, NUL-terminated |
| … | ≤ 9 | `mission_name`, NUL-terminated |

Strings are always NUL-terminated (v1 omitted the NUL for full-length strings,
which made the parser position-dependent). The receiver requires exactly three
NULs within the declared length.

A game list entry is keyed by `session_id`. An entry whose `numconnected` is 0
or that has not been refreshed for 30 s is removed.

`GAME_INFO_REQ` (0x03, same payload as `GAME_INFO_LITE_REQ`) and `GAME_INFO`
(0x04, payload = the `GAME_SETTINGS` block of §4.5 followed by the
`PLAYER_LIST` block) give the join screen its details. Rate limits as in v1
(§3.6). A v2 host answers a request with a different program version with
`JOIN_DENY(version)` (below) instead of v1's `version_deny`.

Tracker: opcodes 21–26 and their outer layouts stay as in v1 §3.3 (the tracker
program dictates them). The version string in `UPID_TRACKER_REGISTER` and
`UPID_TRACKER_REQGAMES` becomes `"D2XR<major>.<minor>.<micro>.100"`, so a v2
client never receives v1 games from the tracker and vice versa. The `z=` blob
in `UPID_TRACKER_REGISTER` and `tracker_gameinfo` is a complete v2
`GAME_INFO_LITE` datagram (34-byte header with `UNCONNECTED`, one `SESSION`
chunk). Hole punching (opcode 26) is unchanged. This assumes the tracker
stores the blob opaquely, which is how the v1 client parses it (it looks for
`z=` and hands the rest to the normal packet parser); see §9.

### 4.2 Join handshake

```
client                                   host
  |-- JOIN_REQUEST (session_id in header) -->|  validate; allocate slot + token
  |<-- JOIN_ACCEPT --------------------------|  connection exists on both ends now
  |  (or JOIN_DENY)                          |
  |<== GAME_SETTINGS, PLAYER_LIST ==========>|  reliable, in order
  |<== LEVEL_START ==========================|
  |   load level                             |
  |== LEVEL_READY(checksum) ================>|
  |<== SNAPSHOT_* … SNAPSHOT_END ============|  only when joining a level in progress
  |== CLIENT_READY =========================>|
  |<== LEVEL_GO / first STATE bundle ========|  status playing; PLAYER_JOINED to others
```

`JOIN_REQUEST` (0x05), 32 bytes, header: `UNCONNECTED`, `session_id` = target
game, `peer_token` = 0, `player_id` = 0xFF.

| Offset | Size | Field |
|---|---|---|
| 0 | 4 | `game_id` |
| 4 | 6 | program major, minor, micro |
| 10 | 4 | `client_nonce` (random `u32`, identifies this join attempt across retries) |
| 14 | 9 | callsign (NUL-padded, lower-cased by the receiver) |
| 23 | 1 | rank (`player_rank`, 0–9) |
| 24 | 4 | `Current_level_num` of the client (i32; used only to pick the right kick reason) |
| 28 | 4 | `client_time` (client's local clock, for the first RTT sample) |

The client sends it every 500 ms until it receives `JOIN_ACCEPT` or
`JOIN_DENY`, for at most 10 s ("No response by host"). After 4 s it also asks
the tracker for a hole punch, as in v1.

Host admission (`net_v2_welcome_player`, replaces `net_udp_welcome_player`;
same rules as v1 §2.5.1 unless noted):

| Condition | Result |
|---|---|
| `game_id`, version or `proto` mismatch | `JOIN_DENY(version)` with the host's values |
| Same `client_nonce` and callsign as an existing connection | Resend the same `JOIN_ACCEPT` (retry of a request whose accept was lost) |
| `RefusePlayers` set | Prompt as in v1 (`net_udp_do_refuse_stuff`); no answer until accepted; `JOIN_DENY(dork)` after 8 s |
| Host in `endlevel` or reactor destroyed | `JOIN_DENY(endlevel)` |
| Callsign matches a `disconnected` slot | Rejoin into that slot (keeps scores) |
| Callsign matches a connected slot | `JOIN_DENY(duplicate_callsign)` (new reason; v1 silently ignored, which looked like a hang) |
| Game closed | `JOIN_DENY(closed)` |
| Free slot, or a disconnected slot to take over (oldest `LastPacketTime`) | Accept |
| Otherwise | `JOIN_DENY(full)` |

The v1 refusal "host is already sending objects to someone else" disappears:
snapshots are ordinary reliable streams and several can be in progress.

`JOIN_ACCEPT` (0x06), 22 bytes, header: `UNCONNECTED`, `session_id`,
`peer_token` = the new token (so the client learns it from the header),
`player_id` = 0 (sender is the host):

| Offset | Size | Field |
|---|---|---|
| 0 | 4 | `client_nonce` echoed |
| 4 | 1 | assigned `player_id` (1–7) |
| 5 | 1 | `tick_rate` |
| 6 | 4 | `host_time` at send |
| 10 | 4 | current `tick` |
| 14 | 4 | `client_time` echoed (first RTT sample) |
| 18 | 4 | `session_id` (redundant with the header, kept for logging) |

`JOIN_DENY` (0x07), 14 bytes: `client_nonce` (4), `reason` (1,
`kick_player_reason`), host program major/minor/micro (6), host `proto` (2),
1 reserved. `kick_player_reason` gains `duplicate_callsign`, `queue_overflow`,
`protocol_error`, `checksum`, `snapshot_failed` and drops `pkttimeout` and the
never-used `connected`.

From the first `JOIN_ACCEPT` on, all traffic is connected (header §3.1) and
the client's transport is initialised with `seq = 1`, expecting host `seq` 1.

### 4.3 Game setup (`starting`) and level start

- `starting` (host collecting players before the first level): joins are
  accepted as above; the host sends `PLAYER_LIST` to everyone on every change
  (reliable), and `GAME_INFO_LITE` broadcasts. A client cancels with
  `LEAVE(cancelled)`.
- Team games: after the host confirms the player list, `GAME_SETTINGS` carries
  the team vector and names (reliable), replacing the v1 second `game_info`.
- `LEVEL_START` (0x10), reliable, host → all:

  | Offset | Size | Field |
  |---|---|---|
  | 0 | 4 | `levelnum` (i32) |
  | 4 | 4 | `tick` at which the level clock starts |
  | 8 | 4 | `ShufflePowerupSeed` |
  | 12 | 8 | `locations[i]`: start position index per slot (host shuffles, as v1 `net_udp_send_sync`) |
  | 20 | 4 | `level_time` (fix; 0 for a fresh level, the current time for a join in progress) |
  | 24 | 4 | `control_invul_time` |
  | 28 | 1 | `start_flags`: bit 0 = level in progress (snapshot follows) |

- The client loads the level (`StartNewLevel` → `level_sync()`), computes
  `my_segments_checksum`, and sends `LEVEL_READY` (0x11): `levelnum` (4),
  `segments_checksum` (2). A mismatch is answered with `KICK(checksum)`;
  v1 showed `TXT_NETLEVEL_NMATCH` on the client, which stays.
- Fresh level: the host waits until every connected player is `ready` (or
  disconnected), then sends `LEVEL_GO` (0x12): `tick` (4), `host_time` (4).
  Every player places its ship at `Player_init[locations[i]]` and the host
  starts sending bundles. Joins that arrive while the host itself is still
  loading are answered after the host is ready.
- Level in progress: the host sends the snapshot (§4.4), the client replies
  `CLIENT_READY` (0x13, empty) after applying it, the host marks the slot
  `playing`, broadcasts `PLAYER_JOINED`, and includes the player in bundles.
  The joining player's ship is a ghost until its first `PLAYER_SPAWN` (§6.5),
  which the host issues immediately after `CLIENT_READY`.

### 4.4 Level snapshot for a join in progress

The host serialises the complete level state at one tick into memory and
queues all parts at once on the joining connection, so events that happen
later are ordered after the snapshot by the reliable stream itself. Nothing
restarts when an object changes during the transfer (v1 `objnum_is_past` is
gone). The transport paces the connection at 32 KiB/s (§3.6); a typical level
(≈ 150 objects) transfers in about 1.3 s.

| Message | Payload |
|---|---|
| `SNAPSHOT_BEGIN` (0x14) | `object_count` u16, `part_count` u16 (parts of all kinds that follow) |
| `SNAPSHOT_OBJECTS` (0x15), ≤ 3 objects | `part` u16, `n` u8 (1–3), then `n` × { `netid` u16 (§6.1), `object_rw` 264 bytes as in v1 `object_data`, converted with `multi_object_to_object_rw` } |
| `SNAPSHOT_WALLS` (0x16), ≤ 100 walls | `part` u16, `n` u8, then `n` × { `wallnum` u16, `type` u8, `flags` u8, `state` u8, `hps` fix } (merges v1 `MULTI_WALL_STATUS`, `MULTI_DOOR_OPEN`, `MULTI_HOSTAGE_DOOR`) |
| `SNAPSHOT_TRIGGERS` (0x17) | `part` u16, bitmap of disabled triggers (`MAX_TRIGGERS/8` bytes) (v1 `MULTI_START_TRIGGER`) |
| `SNAPSHOT_LIGHTS` (0x18), ≤ 60 entries | `part` u16, `n` u8, `n` × { `segnum` u16, `sidemask` u8, 6 × `tmap_num2` u16 } (v1 `MULTI_LIGHT`) |
| `SNAPSHOT_MARKERS` (0x19) | `part` u16, `n` u8, `n` × { `owner` u8, `index` u8, `pos` 12, `text` 40 } |
| `SNAPSHOT_GAME` (0x1A) | `part` u16, then: kill matrix 8×8 u16, `killed[8]` u16, `player_kills[8]` u16, `player_score[8]` i32, `team_kills[2]` i16, `team_vector` u8, `Bounty_target` u8, `KillGoalCount[8]` u8, `monitor_vector` u32, `Countdown_seconds_left` i16 (−1 if not running), thief `Stolen_items` 10 × u8, `level_time` fix, per-player `connected` state 8 × u8 |
| `SNAPSHOT_INVENTORY` (0x1B), one per player | `part` u16, `pid` u8, then the `INVENTORY` payload of §6.2 |
| `SNAPSHOT_END` (0x1C) | `object_count` u16, `part_count` u16, `crc32` u32 over all `SNAPSHOT_*` payloads in order |

Client rules: parts must arrive with consecutive `part` numbers (guaranteed by
the ordered stream; a mismatch is a protocol error). Objects with a level
`netid` (§6.1) are placed at the same object number as on the host
(`init_objects` first, as v1); player-created objects are allocated locally
and mapped. If `obj_allocate` fails, the join is aborted with
`LEAVE(snapshot_failed)` and the menu shows `TXT_NET_SYNC_FAILED` (v1 silently
misparsed). `SNAPSHOT_END` counts and `crc32` must match; otherwise the same
abort. The host also aborts (with `KICK(snapshot_failed)`) if `CLIENT_READY`
does not arrive within 30 s.

### 4.5 `GAME_SETTINGS` and `PLAYER_LIST`

`GAME_SETTINGS` (0x08), reliable host → all and inside `GAME_INFO`; fixed
part 60 bytes (D2), then three NUL-terminated strings as in `GAME_INFO_LITE`:

| Offset | Size | Field |
|---|---|---|
| 0 | 4 | `levelnum` |
| 4 | 1 | `gamemode` |
| 5 | 1 | `RefusePlayers` |
| 6 | 1 | `difficulty` |
| 7 | 1 | `game_status` |
| 8 | 1 | `numplayers` |
| 9 | 1 | `max_numplayers` |
| 10 | 1 | `numconnected` |
| 11 | 1 | `game_flag` |
| 12 | 1 | `team_vector` |
| 13 | 1 | `tick_rate` |
| 14 | 4 | `AllowedItems` (u32; D1 sends its u16 zero-extended) |
| 18 | 4 | `ShufflePowerupSeed` |
| 22 | 1 | `SecludedSpawns` |
| 23 | 2 | `SpawnGrantedItems.mask` |
| 25 | 2 | `DuplicatePowerups` packed |
| 27 | 1 | `Allow_marker_view` (D2) |
| 28 | 1 | `AlwaysLighting` (D2) |
| 29 | 1 | `ThiefModifierFlags` (D2) |
| 30 | 1 | `AllowGuidebot` (D2) |
| 31 | 1 | `ShowEnemyNames` |
| 32 | 1 | `BrightPlayers` |
| 33 | 1 | `InvulAppear` |
| 34 | 1 | `NoFriendlyFire` |
| 35 | 1 | `MouselookFlags` |
| 36 | 1 | `PitchLockFlags` |
| 37 | 1 | reserved (was `PacketLossPrevention`; always reliable now) |
| 38 | 4 | `KillGoal` |
| 42 | 4 | `PlayTimeAllowed` (fix seconds) |
| 46 | 2 × 9 | `team_name[blue]`, `team_name[red]` (fixed 9 bytes each) |
| 64 | … | `game_name`, `mission_title`, `mission_name`, each NUL-terminated |

D1: the four D2-only bytes are sent as 0 so the layout is shared.

`PLAYER_LIST` (0x09): 8 × { callsign 9, `connected` u8, `rank` u8, `team` u8 }
= 96 bytes. Sent whenever a slot changes. Replaces the per-player part of v1
heavy game info and the v1 `addplayer` packet; `PLAYER_JOINED` (0x0A: `pid`,
callsign 9, rank, team) and `PLAYER_LEFT` (0x0B: `pid`, `reason`) are the
incremental forms used during play.

### 4.6 Leaving, kicking, timeouts

- **Client leaves**: it sends `LEAVE` (0x0C, `reason` u8) reliable, keeps the
  socket open until the message is acked or 1 s has passed, then closes. The
  host, on `LEAVE`, runs the disconnect (ghost the ship, drop eggs by host
  rule §6.4, release nothing — robots are host-owned), broadcasts
  `PLAYER_LEFT(quit)`. The v1 `MULTI_QUIT` + `MULTI_PLAYER_DERES(deres_drop)`
  pair is gone; the host creates the drops itself.
- **Host kicks**: `KICK` (0x0D, `reason` u8) reliable; the host keeps the
  connection for 1 s to retransmit, then closes it, and broadcasts
  `PLAYER_LEFT(kicked)`. Manual kick via chat `/kick:` unchanged.
- **Timeout**: no valid packet for 5 s → host treats it as `PLAYER_LEFT(timeout)`;
  the slot becomes `disconnected` and can be rejoined by the same callsign.
- **Host leaves**: `HOST_SHUTDOWN` (0x0E) reliable to all, wait up to 1 s for
  acks, `GAME_INFO_LITE` broadcast with `numconnected = 0`, tracker unregister.
  Clients show "Host left the game!" (as v1).

### 4.7 Level end

- Reactor destroyed: host broadcasts `REACTOR_DESTROYED` (§6.7) and thereafter
  `countdown` in `SNAPSHOT_GAME`-style `LEVEL_STATUS` (0x1D: `countdown` i16,
  `reason` u8) once per second (reliable). Clients no longer run their own
  countdown authority; they display the host's value and advance it locally
  between updates.
- A player escaping sends `ESCAPED` (0x1E, `secret_flag` u8 for D1). The host
  marks the slot `escape_tunnel`/`end_menu` in the bundle flags.
- `LEVEL_END` (0x1F): `reason` u8 (reactor, exit, time limit, kill goal),
  followed by `SCORES` (§6.8). Every client shows the kill matrix from
  authoritative data; the v1 `endlevel_h`/`endlevel_c` exchange is gone. The
  kill matrix screen keeps calling the protocol frame so keepalives flow.
- The next level starts with `LEVEL_START` (§4.3). Reliable queues are *not*
  cleared at level change (they are ordered streams; everything before
  `LEVEL_START` is by construction from the old level and handlers ignore
  object references from a previous level by checking `levelnum` where it
  matters).

---

## 5. State synchronisation

### 5.1 Quantisation

| Quantity | Encoding | Precision | Range | Why |
|---|---|---|---|---|
| Position | 3 × `fix` (12 bytes), exact | 1/65536 unit | level extent | Exact positions keep the segment/point relation intact for `obj_relink` and for the host's rewind checks; saving 6 bytes per player with 16-bit deltas is not worth the extra failure modes on packet loss (a delta needs its base). |
| Orientation | `vms_quaternion` 4 × i16 (8 bytes), as v1 | ~3 × 10⁻⁵ | unit quaternion | Existing `vms_quaternion_from_matrix` / `vms_matrix_from_quaternion`. |
| Velocity | 3 × i16, `fix >> 10` | 1/64 unit/s | ±512 unit/s | Ship speeds are < 100 unit/s even with afterburner and bumps; 1/64 is below anything visible in one tick (0.0003 unit). |
| Rotational velocity | 3 × i16, `fix >> 8` | 1/256 | ±128 (about 20 rev/s) | Only used for extrapolation and engine effects. |
| Shields, energy | u16, `fix >> 8` | 1/256 | 0–255.99 | Max is 200; HUD shows integers. |
| Segment | u16 | – | `MAX_SEGMENTS` 9000 | As v1. |
| Times | u32 net time (`fix` low 32 bits) | 15 µs | 18.2 h wrap | Same unit as the engine clock. |
| Ping in bundle | u8, 4 ms steps | 4 ms | 0–1020 ms | Display only. |

Dequantisation is `value << shift` (sign-extended). Senders round to nearest.

### 5.2 Host state bundle (`STATE` chunk, host → each client, every tick)

Chunk header, 18 bytes:

| Offset | Size | Field |
|---|---|---|
| 0 | 4 | `tick` |
| 4 | 4 | `host_time` of this tick (the time stamp of every record in the chunk) |
| 8 | 4 | `level_time` (`ThisLevelTime`, fix) — replaces `MULTI_HEARTBEAT` |
| 12 | 2 | `your_input_ack`: `input_seq` of the newest `INPUT` the host accepted from the recipient |
| 14 | 1 | `your_flags`: bit 0 `CORRECTION` (apply your own record as authoritative, §5.6), bit 1 `HAS_PINGS` (8 ping bytes at the end), bit 2 `COUNTDOWN` (reactor countdown running), bits 3–7 reserved |
| 15 | 1 | `player_mask`: bit `i` set = a record for player `i` follows, in ascending order |
| 16 | 1 | `n_guided` (0–8) |
| 17 | 1 | `n_robots` (0–12) |

Player record, 41 bytes (or 1 byte when bit 7 of `flags` is set):

| Offset | Size | Field |
|---|---|---|
| 0 | 1 | `flags`: bit 0 alive (ship exists), bit 1 cloaked, bit 2 invulnerable, bit 3 afterburner active, bit 4 headlight on, bit 5 carrying flag / has orbs, bit 6 dying (explosion sequence), bit 7 **ghost/unspawned: record is this byte only** |
| 1 | 8 | quaternion w, x, y, z |
| 9 | 12 | position |
| 21 | 2 | segment |
| 23 | 6 | velocity (quantised) |
| 29 | 6 | rotational velocity (quantised) |
| 35 | 2 | shields |
| 37 | 2 | energy |
| 39 | 1 | `weapon`: bits 0–2 laser level, bit 3 quad lasers, bits 4–7 selected primary (for remote engine/gun visuals and HUD "enemy names" details) |
| 40 | 1 | `input_age`: host ticks since this player's last accepted `INPUT` (saturates at 255). Clients use it to show a "lagging" marker and to stop extrapolating stale ships. |

For the recipient's own slot the record is the host's *accepted* copy of its
state (normally identical to what it sent, at `your_input_ack`); its shields
and energy fields are authoritative and are always applied (§5.6).

Guided missile record, 31 bytes each (one per active guided missile; only in
D2):

| Offset | Size | Field |
|---|---|---|
| 0 | 1 | owner `pid` |
| 1 | 2 | `netid` of the missile |
| 3 | 8 | quaternion |
| 11 | 12 | position |
| 23 | 2 | segment |
| 25 | 6 | velocity |

The owner steers its guided missile locally and reports it in `INPUT` (§5.3);
the host copies the accepted values here. This replaces `MULTI_GUIDED`
positions; release is a reliable `GUIDED_RELEASE` (§6.10).

Robot record, 31 bytes each (robot anarchy / coop only): `netid` u16,
quaternion 8, position 12, segment 2, velocity 6, `flags` u8 (bit 0 cloaked,
bit 1 boss gating). The host sends at most 12 per tick, choosing robots that
moved since their last record first, nearest to any player first, and every
robot at least once per second. Robot animation and AI state are local
(driven by `ai_frame` running in "remote" mode, as robots do for non-controllers
today). Robots that do not move are simply not re-sent.

Trailer: if `HAS_PINGS`, 8 × u8 RTT per slot (set once per second).

Size: 18 + Σ records. 8 live players: 18 + 328 = 346 bytes; with 3 guided
missiles 439. Only robot games can exceed one packet (18 + 328 + 12 × 31 =
718 still fits; the cap of 12 robots keeps it so). Records for players that
are `disconnected` are omitted from the mask.

### 5.3 Client state (`INPUT` chunk, client → host, every tick), 46 bytes (+31)

| Offset | Size | Field |
|---|---|---|
| 0 | 2 | `input_seq` (u16, increments per sent chunk) |
| 2 | 4 | `sample_time`: the client's estimate of host time when this state was sampled (§2.2) |
| 6 | 4 | `view_time`: the host time the client's interpolation was displaying for remote entities at that moment (`sample_time − interp_delay`, §5.4). The host uses it for rewinds (§6.6). |
| 10 | 1 | `flags`: bit 0 alive, bit 1 afterburner active, bit 2 headlight on, bit 3 `WANT_RESPAWN` (fire pressed while dead), bit 4 guided record follows, bits 5–7 reserved |
| 11 | 8 | quaternion |
| 19 | 12 | position |
| 31 | 2 | segment |
| 33 | 6 | velocity |
| 39 | 6 | rotational velocity |
| 45 | 1 | `weapon`: bits 0–3 selected primary, bits 4–7 selected secondary |
| 46 | 31 | optional guided missile record (same layout as §5.2, `pid` = sender) |

The chunk is sent every tick while the client is in the level (also while
dead, with bit 0 clear, so `WANT_RESPAWN` and keepalive-by-content work).
Nothing else about the ship is sent: inventory, shields and energy are the
host's.

### 5.4 Receiver-side interpolation

Per remote entity (player ship, guided missile, robot) the client keeps a ring
of 16 snapshots `{host_time, pos, quat, seg, vel, rotvel, flags}`. A `STATE`
chunk whose `tick` is older than the newest applied one is dropped (latest
wins); older-but-newer-than-buffer-tail chunks are inserted in time order.

Render time:

```
interp_delay = 2 * tick_period + jitter_margin
jitter_margin = p90 over the last 2 s of |arrival_local_time - (host_time + offset)|, clamped to [0, 50 ms]
render_time   = est_host_time - interp_delay        (est_host_time = local + offset, §2.2)
```

`interp_delay` is recomputed once per second and slewed by at most 1 ms per
frame so the remote world does not jump when the margin changes. At 60 Hz and
a clean link it is 33 ms; with 20 ms jitter 53 ms.

Each frame, before `object_move_all` runs local physics and before collision
tests:

1. Find snapshots `a`, `b` with `a.host_time ≤ render_time ≤ b.host_time`.
   Position: cubic Hermite between `a` and `b` using their velocities (correct
   curvature in turns; linear interpolation of a ship in a tight circle cuts
   corners by up to `v·Δt²/8` ≈ 0.02 unit — Hermite is nearly free and
   removes even that). Orientation: normalised lerp of the quaternions (angles
   between two 60 Hz snapshots are small). Velocity: linear.
2. Segment: `b.seg` if `render_time` is past the midpoint, else `a.seg`, then
   confirmed with `find_point_seg` starting from that segment (depth 2); if
   the point is not found, keep `a.seg`. `obj_relink` only when the segment
   changes.
3. If `render_time > b.host_time` (no newer snapshot yet): extrapolate from
   `b` with constant velocity for at most 100 ms, orientation held; beyond
   that hold the last position (and the record's `input_age` will show the
   ship as lagging). Extrapolation uses `b.seg` + `find_point_seg`, never
   `do_physics_sim`, so a ship cannot bounce off walls it never touched.
4. Discontinuities: if `|b.pos − a.pos| > 20 units` or the `alive` flag
   differs, snap to `b` at `b.host_time` (respawn, teleport, death).
5. Flags (cloak, invulnerable, dying, ghost) apply from the newest snapshot
   immediately, not interpolated.

The interpolation writes `obj->pos`, `orient`, `segnum` (relink) and
`mtype.phys_info.velocity`; the object's `movement_source` is `None` so
physics leaves it alone. Bumps between the local ship and a remote ship are
computed by the local ship's physics against this position (cosmetic; the
host applies bump damage, §6.5).

Why this fixes "drawn ship lags hitbox": there is no second, "real" position
that the renderer smooths toward. The interpolated pose is the object. The
shooter's projectiles collide with that pose, the shooter reports the hit
with `view_time`, and the host checks the report against its history at
`view_time`, which is (up to interpolation error, ≤ 0.5 unit at 30 unit/s and
60 Hz) exactly the pose the shooter saw.

### 5.5 Host validation of `INPUT`

For each `INPUT` from player `p` (only the newest per packet; older
`input_seq` than the last accepted are dropped):

1. `sample_time` must be within `[host_now − 1 s, host_now + 100 ms]`; else
   drop (clock not yet synchronised or a stale packet).
2. `segment ≤ Highest_segment_index`; the position must be inside that segment
   (`get_seg_masks(...).centermask == 0` with a 1-unit tolerance) or inside a
   segment reachable within 2 hops (`find_point_seg`); else **reject**.
3. `|velocity| ≤ NET_V2_MAX_SHIP_SPEED` = 150 unit/s (about 3 × afterburner
   top speed; generous so legitimate bumps pass); else **reject**.
4. Displacement since the last accepted state:
   `|Δpos| ≤ 150 unit/s × Δt + 5 units`; else **reject** (teleport).
5. If `p` is dead or a ghost on the host, position fields are ignored (the
   record stays a ghost record); only `WANT_RESPAWN` is read.
6. Accepted: stored as `p`'s authoritative state with `host_time =
   clamp(sample_time, last_stored_time, host_now)` in the history ring (§6.6)
   and broadcast in the next bundle. `your_input_ack` = this `input_seq`.

On **reject** the host keeps the last accepted state, sets `CORRECTION` in
`p`'s next bundle and puts the last accepted position with zero velocity in
`p`'s own record. Three rejections within one second also log a console
warning on the host; there is no automatic kick (bugs in the validator must
not eject players).

### 5.6 The local player's own ship

- Prediction: unchanged local physics, immediately responsive.
- Authoritative fields from the own record every tick: shields, energy, flags
  bits 1–2 and 6–7 (cloak, invulnerable, dying, ghost). The HUD therefore
  shows shields one one-way latency late; that is the price of host-side
  damage and it is invisible at typical 20–60 ms.
- Soft correction: when `CORRECTION` is set, the client replaces `pos`,
  `orient`, `segnum`, `velocity` with the record (snap; corrections only
  happen when the host rejected the state, which in a working game means a
  desync such as flying through a door that is closed on the host) and resets
  the physics remainders (`reset_remainders`, as `extract_quaternionpos` does).
  No visual smoothing: a snap once in a game is acceptable, a smoothed
  correction would re-introduce a drawn/real divergence.
- Death: the client never kills itself. When the host sends `PLAYER_KILLED`
  for the local player (§6.5), the client runs its death sequence
  (`start_player_death_sequence` semantics) from that message; local shield
  arithmetic in `apply_damage_to_player` is disabled for network games.

---

## 6. Authoritative events

All messages in this section are ordered-reliable (class R) unless marked
`EVENT_U`. "Host → all" includes the host applying the message to itself in
the same code path (`multi_do_*`), so host and clients run identical handlers.

### 6.1 Network object ids

v1 identifies an object by `(owner, remote objnum)` (v1 §6). v2 collapses that
into one `netid` (u16) that is the same on every machine:

| Bits | Meaning |
|---|---|
| 15 = 1 | Level object: bits 0–14 = object number at level load (identical everywhere, as v1 `owner_none`). |
| 15 = 0 | Dynamic object: bits 12–14 = creator slot (0–7), bits 0–11 = creator's running counter (wraps at 4096, far above `MAX_OBJECTS` = 350, so a live object never shares an id with a new one). |

Only the host creates persistent objects (powerups, eggs, flags, orbs, robots,
markers), so their creator slot is 0. Clients create ids only for their own
weapons (`FIRE`, §6.5) and mines. The existing tables
`local_to_remote[]` / `remote_to_local[][]` become `netid_of[local]` and
`local_of[netid]` (a 64 K entry `objnum_t` array, 128 KiB, reset on
`reset_network_objects`). `objnum_local_to_remote` / `objnum_remote_to_local`
keep their names with the new type. Pairing by creation order
(`Net_create_objnums`) is gone: every created object is announced with its id.

### 6.2 Pickups

```
client's ship touches powerup P (collide_player_and_powerup, local player)
  → client hides P locally (render off, no further collisions) and sends
    PICKUP_REQUEST { netid(P), view_time }
host:
  P exists, is a powerup/hostage/flag/orb, not already granted?
  requester alive, same level?
  requester's rewound position at view_time (§6.6) within radius(P) + radius(ship) + 2 units of P.pos?
  do_powerup semantics say the requester can use it (not full, right team for flags, …)?
    yes → apply pickup to host-side inventory of requester
          broadcast OBJ_REMOVE{netid}  and  PICKUP_GRANT{pid, netid, powerup id, count}
          (+ INVENTORY{pid} when the pickup changed weapons/ammo/keys)
    no  → PICKUP_DENY{netid} to the requester only
client on PICKUP_GRANT for itself: run do_powerup's effects (sound, HUD text, inventory from INVENTORY)
client on PICKUP_DENY, or no answer within 1 s: show P again
```

- The host itself picks up instantly (it is the authority) and broadcasts the
  same messages.
- All powerups go through this, including shields and energy. A client sees
  the effect one RTT after touching (20–100 ms). The maintainer may want
  shields/energy client-instant instead (§9).
- Vulcan/gauss ammo taken from a cannon the player already has: the grant
  says `count`; if the cannon stays (partial ammo), the host sends
  `OBJ_AMMO{netid, remaining}` instead of `OBJ_REMOVE` (replaces
  `MULTI_VULWPN_AMMO_ADJ`).
- Radius tolerance 2 units covers interpolation error plus one tick of the
  requester's own movement.

`INVENTORY` (host → all, 24 bytes): `pid` u8, `primary_weapon_flags` u16,
`laser_level` u8, `secondary_ammo[10]` u8 (D1: 5), `vulcan_ammo` u16,
`powerup_flags` u32, `hoard_orbs` u8, `has_flag` u8, reserved 1. Sent to
everyone on every change (a few per second at most) and in the snapshot. It
replaces `MULTI_PLAYER_INV` (periodic), `MULTI_FLAGS`, `MULTI_GOT_FLAG`,
`MULTI_GOT_ORB` and the inventory half of `MULTI_PLAYER_DERES`. The level
inventory accounting (`MultiLevelInv_*`) runs on the host only and is exact
because every pickup and every drop passes through it.

### 6.3 Object creation and removal (host only)

`OBJ_CREATE` (host → all, 36 bytes):

| Offset | Size | Field |
|---|---|---|
| 0 | 2 | `netid` |
| 2 | 1 | `type` (`object_type`: powerup, robot, hostage, marker, weapon for mines) |
| 3 | 1 | `id` (powerup type / robot id / weapon id) |
| 4 | 2 | segment |
| 6 | 12 | position |
| 18 | 6 | initial velocity (quantised) |
| 24 | 8 | orientation quaternion (identity for powerups) |
| 32 | 2 | `count` (vulcan ammo in a cannon, orbs in a hoard powerup; 0 otherwise) |
| 34 | 1 | `pflags` (`PF_SPAT_BY_PLAYER` etc.) |
| 35 | 1 | `owner` pid for spat objects (0xFF none) |

`OBJ_REMOVE` (host → all, 3 bytes): `netid` u16, `reason` u8 (picked up,
expired, exploded, level cleanup). Receivers remove only if the type matches
one that the host may remove this way; weapons in flight are not removed by
message (their lifetime is simulated) except mines (`reason` = exploded).

Uses: powerup respawn (`MultiLevelInv_Repopulate`, host), used-item
replacement (`maybe_drop_net_powerup`, host only — as the fork's #3 already
does), death drops (§6.4), dropped weapons (`DROP_WEAPON` request → host
`OBJ_CREATE`), CTF flags and hoard orbs spat on death or drop, robot eggs
(`CREATE_ROBOT_POWERUPS` → n × `OBJ_CREATE`), matcen robots, boss gating,
markers. The explicit velocity replaces every shared seed
(`multi_create_powerup_seed`, `spit_powerup` seeds, `d_srand(5483)`,
`d_srand(1245)`); receivers pass the given velocity to `drop_powerup` /
`obj_create` and do not randomise.

### 6.4 Death drops and disconnect drops

When the host kills a player or a player leaves, the host runs
`drop_player_eggs` on its authoritative inventory copy of that player and
announces every created object with `OBJ_CREATE` (at most
`MAX_NET_CREATE_OBJECTS` = 40 messages, ≈ 1.4 KiB, well within one or two
packets). Clients do not run `drop_player_eggs` in network games. The grant
subtraction happens once, on the host. `MULTI_PLAYER_DERES` disappears; the
"explode" half becomes the `dying` flag in the bundle plus `PLAYER_KILLED`.

### 6.5 Firing, hits, damage, kills

**Firing.** `FIRE` (shooter → host, host → all; 40 bytes):

| Offset | Size | Field |
|---|---|---|
| 0 | 1 | `pid` shooter (set by the host on relay; clients ignore what a client wrote) |
| 1 | 4 | `fire_time`: host time at which the shot left the gun (shooter's `sample_time` clock) |
| 5 | 1 | weapon: primary index, `MISSILE_ADJUST` + secondary index, or `FLARE_ADJUST` (as v1) |
| 6 | 1 | laser level |
| 7 | 1 | flags (v1 semantics: quad, fusion charge, alternate gun) |
| 8 | 12 | origin: the shooter's ship position at `fire_time` |
| 20 | 12 | direction (`orient.fvec`) |
| 32 | 2 | `seed`: shooter-chosen; receivers `d_srand(seed)` before `do_laser_firing` so vulcan/gauss spread and `speedvar` come out identical (they matter for where the projectile is, since the shooter reports hits on it) |
| 34 | 2 | `netid` of the first projectile (consecutive ids for multi-projectile shots: spread, quad, fusion pair, helix) |
| 36 | 2 | `track_netid` (homing target) or 0xFFFF |
| 38 | 2 | reserved |

Host checks: shooter alive; weapon available with ammo/energy in the host's
inventory (rejected shots are answered with `INVENTORY` so the client resyncs
its HUD); rate: `fire_time` at least 0.8 × the weapon's fire delay after the
previous accepted shot; `fire_time` within `[host_now − 500 ms, host_now +
50 ms]`; origin within 5 units of the shooter's rewound position at
`fire_time`. Accepted → ammo/energy deducted on the host (broadcast in
`INVENTORY` only for ammo, energy comes with the bundle), relayed to all
other clients.

Receivers spawn the projectile at `origin` along `direction`, then advance it
by `est_host_time − fire_time` (capped at 250 ms) with the weapon's own
physics step, so a shot that arrives 40 ms late appears 40 ms along its path
instead of at the muzzle. The shooter itself spawned the projectile at fire
time already and does nothing on the relay.

Which machine's fire origin counts: the shooter's. It is the only machine
that knows exactly where its ship was at the moment of firing, and the host
verifies it against the shooter's own reported history.

**Hits.** Every machine simulates every projectile. Damage, however, is
decided only from the shooter's report. `WEAPON_HIT` (shooter → host, 26
bytes):

| Offset | Size | Field |
|---|---|---|
| 0 | 2 | weapon `netid` |
| 2 | 4 | `hit_time`: the shooter's `view_time` at the frame of the hit |
| 6 | 1 | `target_kind`: 0 player, 1 robot, 2 reactor, 3 wall (blastable), 4 monitor/switch side, 5 none (explosion in the open / on a wall) |
| 7 | 2 | target: `pid` (u8 + pad) / robot `netid` / reactor `netid` / `wallnum` / segment (for kind 4, side in the next byte) |
| 9 | 1 | side (kind 4) or 0 |
| 10 | 12 | hit point |
| 22 | 2 | `damage_hint`: the shooter's computed damage (fix >> 8), for logging only; the host recomputes |
| 24 | 2 | reserved |

Host validation and application:

1. The weapon exists, belongs to the sender (creator slot in `netid`), was
   fired ≤ 10 s ago, has not already been consumed (persistent weapons —
   fusion, omega, phoenix bounces — may hit several targets once each, as
   `hitobj_list` does today).
2. `hit_time` within `[fire_time, host_now]`.
3. Plausible flight: `|hit_point − origin| ≤ weapon_speed_max × (hit_time −
   fire_time) + 10 units` (weapon speed from `Weapon_info`, with `speedvar`
   variance and parent speed included).
4. Line of sight: `find_vector_intersection` from the projectile's position
   one tick before (reconstructed from origin, direction and time; for
   homing weapons from the host's own copy of the projectile) to `hit_point`
   must not hit a wall first, unless `target_kind` is wall/monitor.
5. Target check with **rewind**: for kind 0, the victim's position from the
   history at `hit_time` (§6.6) must be within `victim.size + 2 units` of
   `hit_point`. For robots, the host's robot position at `hit_time` (host
   history for robots too, 64 entries each). For the reactor/walls/monitors
   the geometry is static; only step 4 applies.
6. Apply: `apply_damage_to_player` / `apply_damage_to_robot` /
   reactor / wall on the host with the weapon's damage computed by the host
   (`weapon->shields × multiplier × multi_damage_scale`, friendly fire rule
   from `Netgame.NoFriendlyFire`). Area damage (mega, smart, mines,
   earthshaker): the host explodes the weapon at `hit_point` and applies
   radius damage to every player at its rewound position at `hit_time` and to
   robots at their current host position.
7. Broadcast `DAMAGE{victim pid, attacker pid, weapon id, amount, hit point}`
   (hit sounds, palette flash on the victim, hit sparks on everyone) and, if
   shields dropped below 0, `PLAYER_KILLED`.

Bumps between ships: the host detects them from its own copies (interpolated
positions of the two ships at the same host time) and applies bump damage
(`collide_player_and_player`); no report needed. Robot melee and robot
weapons: the host fires and detects (it is the shooter), applying rewind to
the victim at `host_now − one_way_latency(victim)` (half the victim's `srtt`)
so a robot shot lands where the victim saw itself.

Shooter feedback: the shooter's client shows its own hit effects immediately
(sparks, sound) when its projectile collides locally; kill messages and score
changes wait for the host.

**Kills.** `PLAYER_KILLED` (host → all, 12 bytes): `victim` u8, `killer` u8
(0xFF none/environment), `killer_kind` u8 (player, robot, reactor, mine, self),
weapon id u8, `team_vector` u8, `Bounty_target` u8, kill matrix cell update
`kills[killer][victim]` u16, `victim_killed_total` u16, `killer_kills_total`
u16. The handler is `multi_compute_kill` with explicit parameters; the victim
runs its death sequence on receipt. `MULTI_KILL_CLIENT`/`MULTI_KILL_HOST`
disappear.

**Respawn.** The dead client sets `WANT_RESPAWN` in `INPUT` once its death
sequence has ended (`DoPlayerDead` path). The host chooses the spawn point
(`InitPlayerPosition` logic with `SecludedSpawns`, host `d_rand`) and sends
`PLAYER_SPAWN` (host → all, 24 bytes): `pid` u8, `spawn_index` u8,
`invul_time` fix (from `InvulAppear`), position 12, quaternion 8, then the
client places its ship there and becomes alive; everyone else un-ghosts the
ship (`multi_do_reappear` semantics). Granted spawn items come in the following
`INVENTORY`.

### 6.6 Position history and rewind

The host keeps, per player and per robot, a ring of `NET_V2_HISTORY` = 64
entries `{host_time, pos, seg, size}` written once per tick from the accepted
state (§5.5) — 1.07 s at 60 Hz, 2.1 s at 30 Hz. `rewind(entity, t)` returns
the linear interpolation between the two entries around `t`; `t` older than
the ring yields the oldest entry and marks the report as "stale" (still
applied if the other checks pass — a player with > 1 s of latency is playing
badly anyway, but not cheating). `t` in the future is clamped to now.

Which time is rewound to: the shooter's `view_time`. The shooter saw remote
ships at `render_time = est_host_time − interp_delay`; that is what `view_time`
is (§5.3), and `hit_time` in `WEAPON_HIT` is the `view_time` of the frame in
which the hit happened. The host's history is written with the *victim's*
`sample_time`, i.e. where the victim really was at that host time, so the
comparison is apples to apples. Interpolation error on the shooter's side is
bounded by one tick of the victim's motion (≤ 0.5 unit) and is covered by the
2-unit tolerance.

Consequence for the victim: it can be hit "around a corner" by up to one
one-way latency of the shooter plus `interp_delay` (typically 50–100 ms) —
the standard trade-off of shooter-favouring lag compensation, and the same
one every client already lives with today (v1 damage is applied by the victim
from projectiles that were fired at stale positions).

### 6.7 Doors, triggers, walls, reactor, lights

- Doors: the local player touching a door keeps opening it locally at once
  (`collide_player_and_wall` → `wall_open_door`) for responsiveness, and sends
  `WALL_REQUEST{segnum, side, kind=open}` to the host. The host checks
  distance (rewound requester within 10 units of the side) and keys
  (host-side inventory) and broadcasts `WALL_STATE{wallnum, type, flags,
  state, hps}`. Since keys are host-granted, a client can only open a locked
  door locally if the host will agree, so the prediction is safe. Weapon-
  opened doors follow from `WEAPON_HIT` kind 3.
- Triggers: `TRIGGER_REQUEST{trigger, by_shot}` from the player who flew
  through or shot the switch; host validates the requester's position or the
  `WEAPON_HIT` and broadcasts `TRIGGER{trigger, pid}`; only then do all
  machines (including the requester) run `check_trigger` effects. A one-RTT
  delay on switches is acceptable and avoids double-application.
- Blastable walls, monitors and switches: via `WEAPON_HIT` kinds 3–4 →
  `WALL_STATE` / `EFFECT_BLOWUP{segnum, side, point}` from the host.
- Reactor: damage via `WEAPON_HIT` kind 2; the host destroys it and
  broadcasts `REACTOR_DESTROYED{netid, pid}` and starts the countdown (§4.7).
  Reactor fire (`MULTI_CONTROLCEN_FIRE`) becomes a host `FIRE` with the
  reactor as owner (`pid` 0xFE), hits on players by rewind as for robots.
- Lights (`MULTI_LIGHT`) and `SEISMIC` are host broadcasts triggered by the
  host's own handling of the hit / earthshaker.

### 6.8 Game modes

| Mode | v2 handling |
|---|---|
| Anarchy, team anarchy | Kills and team vector in `PLAYER_KILLED`; `GAME_MODE_STATE{team_vector, Bounty_target, KillGoalCount[8]}` (replaces `MULTI_GMODE_UPDATE`, `MULTI_KILLGOALS`, `MULTI_DO_BOUNTY`) sent on change and every 5 s; kill goal / time limit end decided by the host → `LEVEL_END`. |
| Bounty | `Bounty_target` chosen by the host on every kill/disconnect, in `PLAYER_KILLED` and `GAME_MODE_STATE`. |
| CTF (D2) | Flags are powerups: pickup via §6.2 (host checks team). Capture: the host tests the carrier's rewound/current position against the goal segment (`fuelcen_check_for_goal` on the host, using the carrier's latest accepted state) → `CAPTURE{pid}` (score +5) and flag reset `OBJ_CREATE`. Drop on death: host `OBJ_CREATE` with spit velocity. `DROP_FLAG` request from a carrier → host creates. |
| Hoard / team hoard (D2) | Orbs are powerups (`INVENTORY.hoard_orbs`). Scoring: host detects the carrier in the goal (`fuelcen_check_for_hoard_goal`) → `ORB_BONUS{pid, orbs}`. Drop on death handled as flags. |
| Cooperative | Robots host-simulated (§5.2); keys granted to all (host applies the coop rule); `SCORE_UPDATE{pid, score}` from the host; `SAVE_GAME`/`RESTORE_GAME` as host messages (unchanged payloads). |
| Robot anarchy | Same robot handling; robot kills credited by the host. |

Robots: the v1 claim/release/priority system (`multi_can_move_robot`,
`MULTI_ROBOT_CLAIM`, `MULTI_ROBOT_RELEASE`, `MAX_ROBOTS_CONTROLLED`) is removed.
The host runs `ai_frame` for every robot; clients run robots as "remote"
(animation only, positions from the bundle). Robot fire is a host `FIRE` with
`pid` = 0xFD and the robot `netid` in `track_netid`'s place (reuse as
`owner_netid`); robot death is `ROBOT_KILLED{netid, killer pid}` (host) after a
validated `WEAPON_HIT`; boss actions (`BOSS_TELEPORT`, `BOSS_CLOAK`,
`BOSS_GATE`) and matcen `CREATE_ROBOT` become host `OBJ_CREATE`/robot events
with the same payloads as v1 minus the sender field. The thief's stolen items
live in `SNAPSHOT_GAME` and `STOLEN_ITEMS` (host).

### 6.9 Cosmetic events (`EVENT_U`)

Chunk payload: `n` u8, then `n` events of `{type u8, len u8, payload}`. Sent
once, relayed once by the host, never retransmitted. Receivers apply them if
the referenced player exists. Types: `PLAY_SOUND` (v1 `MULTI_PLAY_SOUND`
payload + `pid`), `CREATE_EXPLOSION` (`pid`), `DROP_BLOB` (`pid`),
`SOUND_FUNCTION` (`pid`, function, sound), `TYPING_STATE` (`pid`, state).
Chat (`MESSAGE`) is reliable because a lost chat line is noticed.

### 6.10 Mapping of every v1 `MULTI_*` message

| v1 message (id) | v2 | Notes |
|---|---|---|
| `MULTI_POSITION` (0) | gone | Position is `INPUT` → bundle. The "send position before an important event" pattern is unnecessary: every event carries its own coordinates. |
| `MULTI_REAPPEAR` (1) | `PLAYER_SPAWN` (host) | Host picks the spawn point. |
| `MULTI_FIRE` (2), `FIRE_TRACK` (3), `FIRE_BOMB` (4) | `FIRE` | One message; `track_netid` and projectile `netid` fields cover all three. |
| `MULTI_REMOVE_OBJECT` (5) | `OBJ_REMOVE` (host) | Clients never remove. |
| `MULTI_MESSAGE` (6) | `MESSAGE` | Reliable now; `pid` explicit. Same 35-byte text. |
| `MULTI_QUIT` (7) | `LEAVE` / `PLAYER_LEFT` / `HOST_SHUTDOWN` | Acked. |
| `MULTI_PLAY_SOUND` (8) | `EVENT_U PLAY_SOUND` | |
| `MULTI_CONTROLCEN` (9) | `REACTOR_DESTROYED` (host) | |
| `MULTI_ROBOT_CLAIM` (10), `ROBOT_RELEASE` (21) | gone | Host owns robots. |
| `MULTI_CLOAK` (11), `DECLOAK` (16) | bundle flag bit 1 + `INVENTORY` | Cloak start time = first tick with the bit set. |
| `MULTI_ENDLEVEL_START` (12) | `ESCAPED` | |
| `MULTI_CREATE_EXPLOSION` (13) | `EVENT_U CREATE_EXPLOSION` | |
| `MULTI_CONTROLCEN_FIRE` (14) | `FIRE` with `pid` 0xFE | Host only. |
| `MULTI_CREATE_POWERUP` (15) | `OBJ_CREATE` (host) | No client-side spawns; explicit velocity. |
| `MULTI_ROBOT_POSITION` (17) | robot records in the bundle | |
| `MULTI_PLAYER_DERES` (18) | `PLAYER_KILLED` + `dying` flag + host `OBJ_CREATE` × n + `INVENTORY` | |
| `MULTI_DOOR_OPEN` (19), `WALL_STATUS` (49), `HOSTAGE_DOOR` (32) | `WALL_STATE` (host); `WALL_REQUEST` (client) | One wall message. |
| `MULTI_ROBOT_EXPLODE` (20) | `ROBOT_KILLED` (host) | |
| `MULTI_ROBOT_FIRE` (22) | `FIRE` with `pid` 0xFD | Host only. |
| `MULTI_SCORE` (23) | `SCORE_UPDATE` (host) | |
| `MULTI_CREATE_ROBOT` (24), `BOSS_CREATE_ROBOT` (30) | `OBJ_CREATE` type robot (host) | |
| `MULTI_TRIGGER` (25) | `TRIGGER` (host); `TRIGGER_REQUEST` (client) | |
| `MULTI_BOSS_TELEPORT` (26), `BOSS_CLOAK` (27), `BOSS_START_GATE` (28), `BOSS_STOP_GATE` (29) | `BOSS_ACTION{netid, action, segnum}` (host) | |
| `MULTI_CREATE_ROBOT_POWERUPS` (31) | n × `OBJ_CREATE` (host) | |
| `MULTI_SAVE_GAME` (33), `RESTORE_GAME` (34) | same payloads, host only | |
| `MULTI_HEARTBEAT` (35) | bundle header `level_time` | 60 Hz instead of 1 Hz. |
| `MULTI_KILLGOALS` (36), `DO_BOUNTY` (37), `GMODE_UPDATE` (39) | `GAME_MODE_STATE` (host) | |
| `MULTI_TYPING_STATE` (38) | `EVENT_U TYPING_STATE` | |
| `MULTI_KILL_HOST` (40), `KILL_CLIENT` (41) | `PLAYER_KILLED` (host) | Clients never report their own death; the host knows. |
| `MULTI_RANK` (42) | `RANK{pid, rank}` | |
| `MULTI_DROP_WEAPON` (43) | `DROP_WEAPON_REQUEST{primary}` → host `OBJ_CREATE` + `INVENTORY` | No seed. |
| `MULTI_VULWPN_AMMO_ADJ` (44) | `OBJ_AMMO` (host) | |
| `MULTI_PLAYER_INV` (45) | `INVENTORY` (host, on change) | Not periodic. |
| `MULTI_MARKER` (46) | `MARKER_REQUEST{index, text}` → host `OBJ_CREATE` type marker + `MARKER_TEXT{pid, index, text}` | Owner explicit (fixes v1 §8.4). |
| `MULTI_GUIDED` (47) | guided record in `INPUT`/bundle; `GUIDED_RELEASE{pid}` reliable | |
| `MULTI_STOLEN_ITEMS` (48) | `STOLEN_ITEMS` (host) + snapshot | |
| `MULTI_SEISMIC` (50) | `SEISMIC` (host) | |
| `MULTI_LIGHT` (51) | `LIGHT_STATE` (host) + snapshot | |
| `MULTI_START_TRIGGER` (52) | `SNAPSHOT_TRIGGERS` / `TRIGGER_DISABLE` (host) | |
| `MULTI_FLAGS` (53), `GOT_FLAG` (57), `GOT_ORB` (61) | `INVENTORY` | `pid` explicit (fixes v1 §8.4). |
| `MULTI_DROP_BLOB` (54), `SOUND_FUNCTION` (55) | `EVENT_U` | |
| `MULTI_CAPTURE_BONUS` (56) | `CAPTURE` (host) | |
| `MULTI_DROP_FLAG` (58) | `DROP_FLAG_REQUEST` → host `OBJ_CREATE` | No seed (fixes v1 §8.5 for good). |
| `MULTI_FINISH_GAME` (59) | `LEVEL_END(reason=finish)` (host) | |
| `MULTI_ORB_BONUS` (60) | `ORB_BONUS` (host) | |
| `MULTI_EFFECT_BLOWUP` (62) | `EFFECT_BLOWUP` (host) after `WEAPON_HIT` kind 4 | |
| `MULTI_UPDATE_BUDDY_STATE` (63) | `BUDDY_STATE` (host; guide-bot is host-owned) | |
| fork's `PICKUP_REQUEST`/`REPLY`/`RELEASE` (#5) | `PICKUP_REQUEST`, `PICKUP_GRANT`, `PICKUP_DENY` | Release is unnecessary: the host decides usability before granting. |
| v1 `pdata` | `INPUT` / bundle | |
| v1 `ping`/`pong` | header `send_time`/`echo_*` | |
| v1 `endlevel_h`/`endlevel_c` | `LEVEL_STATUS`, `LEVEL_END`, `SCORES` | |
| v1 `object_data` | `SNAPSHOT_*` | |
| v1 `sync`, `game_info`, `addplayer`, `request`, `dump`, `quit_joining`, `version_deny` | `GAME_SETTINGS`, `PLAYER_LIST`, `LEVEL_START`, `LEVEL_GO`, `PLAYER_JOINED`, `JOIN_REQUEST`, `KICK`/`JOIN_DENY`, `LEAVE(cancelled)` | |

Message type numbering: session 0x01–0x1F (§4), `INVENTORY` 0x20,
`OBJ_CREATE` 0x21, `OBJ_REMOVE` 0x22, `OBJ_AMMO` 0x23, `PICKUP_REQUEST` 0x24,
`PICKUP_GRANT` 0x25, `PICKUP_DENY` 0x26, `FIRE` 0x27, `WEAPON_HIT` 0x28,
`DAMAGE` 0x29, `PLAYER_KILLED` 0x2A, `PLAYER_SPAWN` 0x2B, `GUIDED_RELEASE`
0x2C, `WALL_REQUEST` 0x2D, `WALL_STATE` 0x2E, `TRIGGER_REQUEST` 0x2F,
`TRIGGER` 0x30, `TRIGGER_DISABLE` 0x31, `EFFECT_BLOWUP` 0x32, `LIGHT_STATE`
0x33, `REACTOR_DESTROYED` 0x34, `SEISMIC` 0x35, `GAME_MODE_STATE` 0x36,
`SCORE_UPDATE` 0x37, `SCORES` 0x38, `CAPTURE` 0x39, `ORB_BONUS` 0x3A,
`DROP_WEAPON_REQUEST` 0x3B, `DROP_FLAG_REQUEST` 0x3C, `MARKER_REQUEST` 0x3D,
`MARKER_TEXT` 0x3E, `MESSAGE` 0x3F, `RANK` 0x40, `ROBOT_KILLED` 0x41,
`BOSS_ACTION` 0x42, `STOLEN_ITEMS` 0x43, `BUDDY_STATE` 0x44, `SAVE_GAME` 0x45,
`RESTORE_GAME` 0x46, `ESCAPED` 0x1E, `LEVEL_STATUS` 0x1D, `LEVEL_END` 0x1F.
The table lives in `net_v2.h` as a `for_each_net_v2_message(VALUE)` macro
with `(NAME, id, min_len, max_len, allowed_sender)` so the length and
direction checks of §3.7 are table-driven like v1's `command_length`.

---

## 7. Bandwidth estimate (8 players, 60 Hz, D2 anarchy)

Sizes from §3.1 (header 34), §3.2 (chunk header 3), §5.2 (bundle header 18,
player record 41), §5.3 (`INPUT` 46). "On wire" adds 28 bytes IPv4+UDP (48
for IPv6).

**Host → one client, per tick** (8 live players, no guided missiles, no
reliable messages due):

| Part | Bytes |
|---|---|
| Packet header | 34 |
| `STATE` chunk header | 3 |
| Bundle header | 18 |
| 8 × player record | 328 |
| **Payload** | **383** |
| On wire (IPv4) | 411 |

Per second: 383 × 60 = 22 980 B/s payload; 411 × 60 = 24 660 B/s ≈ 197 kbit/s
on wire per client. Host upstream to 7 clients: 172 620 B/s ≈ 169 KiB/s ≈
**1.38 Mbit/s**. With the per-second ping trailer (+8 bytes once a second) and
one guided missile in flight (+31 bytes per tick) this rises by < 2 %.

Reliable events on top, worst case (everyone firing vulcan at 20 shots/s):
8 × 20 × (3 + 40) = 6 880 B/s relayed to each client ≈ 55 kbit/s; typical
play (a few shots per second per player, occasional hits, pickups, kills) is
under 1 500 B/s per client. Retransmissions add the loss rate times the
reliable volume (e.g. 5 % loss → +75 B/s).

**Client → host, per tick:**

| Part | Bytes |
|---|---|
| Packet header | 34 |
| `INPUT` chunk header | 3 |
| `INPUT` record | 46 |
| **Payload** | **83** |
| On wire | 111 |

Per second: 83 × 60 = 4 980 B/s payload, 111 × 60 = 6 660 B/s ≈ 53 kbit/s
upstream per client. Host downstream from 7 clients: 46 620 B/s ≈ 46 KiB/s ≈
373 kbit/s. A firing client adds ≤ 20 × 43 = 860 B/s; hits ≤ 20 × 29 = 580 B/s.

**Scaling:**

| Players | Tick | Host up (on wire) | Per client down |
|---|---|---|---|
| 8 | 60 Hz | 1.38 Mbit/s | 197 kbit/s |
| 8 | 30 Hz | 0.69 Mbit/s | 99 kbit/s |
| 4 | 60 Hz | 3 × (34+3+18+164+28) × 60 = 44 460 B/s ≈ 356 kbit/s | 119 kbit/s |
| 2 | 60 Hz | (34+3+18+82+28) × 60 = 9 900 B/s ≈ 79 kbit/s | 79 kbit/s |

Comparison with v1 at 30 pps (default) and 8 players: the host relays 7
`pdata` (49 + 28 bytes) per tick to each of 7 clients plus its own, i.e.
7 × 8 × 77 × 30 = 129 360 B/s ≈ 1.03 Mbit/s, plus MDATA. v1 at 40 pps
(`MAX_PPS`) is 1.38 Mbit/s. So v2 at 60 Hz costs about what v1 costs at its
maximum setting while delivering 60 Hz state with timestamps, and v2 at 30 Hz
is a third cheaper than v1's default. The 1200-byte packet cap is never
approached in non-robot games (max 439 + reliable backlog).

The host's per-tick CPU work is 7 packet builds (memcpy-scale), 7 validations
(one `find_point_seg` each), the history writes and pending validators; it is
negligible next to rendering.

---

## 8. Staged implementation plan

Each stage builds on the previous one, compiles for D1X and D2X with the
project's `-Werror` flags, and leaves the game playable so that it can be
play-tested on its own. `MULTI_PROTO_VERSION` is set to 100 in stage 1 and
bumped by one per later stage that changes the wire format (101, 102, …) so
that mismatched playtest builds refuse each other instead of misbehaving; it is
set to a final value at stage 7.

### Stage 0 — Transport library and simulation test

- **New files**: `common/main/net_v2.h` (constants, header/chunk layouts,
  `for_each_net_v2_message`), `common/main/net_v2_transport.h`,
  `common/main/net_v2_transport.cpp` (namespace `dcx::net_v2`:
  `connection` with `build_packet(now, budget, state_chunk) → span`,
  `receive_packet(now, span) → validation result + delivered messages`,
  `queue_reliable(type, payload)`, RTT/RTO estimator, `clock_sync` offset
  estimator; all I/O-free and game-free, so it compiles once in `common`).
- **Test**: `common/unittest/net_v2_transport.cpp`, registered in `SConstruct`
  next to `test-serial` (`RuntimeTest('test-net-v2-transport', (...))`,
  Boost.Test like the existing tests). It contains a `sim_link` with
  configurable loss, duplication, reorder probability, base latency, jitter
  and bandwidth cap driven by a virtual clock, two `connection`s ticking at 60
  Hz, and these cases:
  1. 10 000 reliable messages of random sizes under 30 % loss, 5 %
     duplication, 20 % reorder: delivered exactly once, in order, within the
     RTO bounds; no message resent more than `1 + loss × k` times on average.
  2. Idle link: keepalives every 100 ms, acks keep `in_flight` empty.
  3. Receiver window: a sender stalled by lost acks never exceeds 256 in
     flight; queue overflow (512) fires the overflow callback once.
  4. Replay/reorder: a captured packet re-injected later is rejected; a packet
     64+ behind is rejected; out-of-order within 64 is accepted once.
  5. RTT estimator: with 80 ± 20 ms link, `srtt` settles within 10 % in 2 s;
     `rto` stays within [50, 1000] ms; Karn's rule holds under loss.
  6. Clock sync: offset error < 5 ms after 2 s at 20 ms jitter; a 300 ms clock
     step is followed within one window.
  7. Fuzz: 1 000 000 random and mutated packets never crash or read out of
     bounds (run under ASan in CI), and never deliver a message.
- **Risk**: low. **Play-test**: none.

### Stage 1 — New transport and session under the existing gameplay messages

- Replace the `upid` packet types with the v2 header, discovery, join
  handshake (§4.1–4.3, §4.6), keepalive/timeout, kick and leave. The
  gameplay layer keeps sending `MULTI_*` records: `dispatch_table::send_data`
  maps priority 2 to a reliable message `LEGACY_MDATA` (0x7F, payload = the
  v1 message bytes) and priorities 0/1 to an `EVENT_U LEGACY` event;
  `send_data_direct` becomes a reliable message to one connection; `pdata`
  travels as `INPUT` (client → host) and as a transitional bundle of raw
  `quaternionpos` records (host → clients), applied exactly as today
  (`extract_quaternionpos`). Level sync, `object_data` and the endlevel
  packets keep their v1 payloads but ride on reliable messages
  (`LEGACY_SYNC`, `LEGACY_OBJECTS`, `LEGACY_ENDLEVEL`), which alone fixes v1
  issues 3 and 8.
- **Files**: `similar/main/net_udp.cpp` → most of it moves to
  `similar/main/net_v2.cpp` (sockets `udp_open_socket`, `net_udp_listen`,
  `net_udp_process_packet` → `net_v2_receive`, `do_protocol_frame`,
  `net_udp_welcome_player` → `net_v2_welcome_player`, `net_udp_game_connect`,
  `net_udp_timeout_check`, tracker functions with the new version string; the
  `net_udp_noloss_*` functions, `UDP_mdata_*`, `ping`/`pong` are deleted).
  The menus (`net_udp_list_join_game`, `net_udp_setup_game`,
  `netgame_list_game_menu`) stay in place with their entry points.
  `common/main/net_udp.h` (constants), `common/main/multi.h`
  (`MULTI_PROTO_VERSION` = 100, `kick_player_reason` additions,
  `netplayer_info` gains `peer_token`, `netgame_info` gains `session_id` and
  `TickRate`), `similar/main/multi.cpp` (`multi_send_data*` unchanged API).
- **Shippable**: yes — same gameplay, better transport.
- **Risk**: medium (large mechanical move; join/leave edge cases).
- **Play-test**: LAN discovery and tracker listing; join, leave, kick, rejoin
  after `kill -9`; 4-player anarchy for 20 minutes with `tc qdisc … netem
  loss 10% delay 80ms 20ms` on one client; no "failed sending important
  packets" kicks.

### Stage 2 — Clock, tick, state bundle, interpolation

- Implement §2.2, §2.3, §5.2–§5.4 and §5.6 (without validation: the host
  accepts every `INPUT`). Remove `MULTI_POSITION`, `MULTI_GUIDED` positions,
  `MULTI_HEARTBEAT`, `MULTI_ROBOT_POSITION` for the thief (host-owned later;
  transitional: keep sending as legacy). Add the `TickRate` setting to the
  game setup menu in place of `PacketsPerSec`.
- **Files**: `similar/main/net_v2.cpp` (tick accumulator in
  `do_protocol_frame`, `build_state_bundle`, `apply_state_bundle`,
  `build_input`, `apply_input`), new `common/main/net_interp.h` +
  `similar/main/net_interp.cpp` (snapshot rings, Hermite/nlerp, `render_time`,
  `interp_delay` estimator), `similar/main/object.cpp` (`object_move_one`:
  skip physics for objects with `movement_source == None` that are
  network-driven; call `net_interp_apply_all()` from `object_move_all` before
  the loop), `similar/main/multi.cpp` (`multi_send_position`,
  `multi_do_position`, `multi_send_guided_*`, `multi_do_guided`,
  `multi_send_heartbeat`, `multi_do_heartbeat` removed; `multi_do_frame` loses
  the periodic sends), `similar/main/gameseg.cpp` (`extract_quaternionpos`
  reused for snaps), `similar/main/gauges.cpp` (ping from bundle), the v1
  `remote_smoothing.*` from the on-hold branch is *not* merged.
- **Shippable**: yes.
- **Risk**: medium. The visible change is large (remote ships now move at 60 Hz
  timestamps); segment relinking of interpolated ships through short tunnel
  segments needs care (`find_point_seg` depth 2 rule).
- **Play-test**: at 500 fps, remote ships smooth in fast turns and through
  tunnels; with 100 ms netem delay the ship you aim at is where your shots
  land (still client-detected damage at this stage, so this is a visual
  check); guided missiles smooth on remote screens; a client with 300 ms
  extra latency shows the lag marker and does not warp others.

### Stage 3 — Object authority and pickups

- Implement §6.1–§6.4: `netid`, `OBJ_CREATE`/`OBJ_REMOVE`/`OBJ_AMMO`,
  `PICKUP_REQUEST`/`GRANT`/`DENY`, `INVENTORY`, host-side inventory, host-only
  death drops. Delete `MULTI_CREATE_POWERUP`, `MULTI_REMOVE_OBJECT` (client
  side), `MULTI_DROP_WEAPON`, `MULTI_DROP_FLAG`, `MULTI_VULWPN_AMMO_ADJ`,
  `MULTI_PLAYER_INV`, `MULTI_FLAGS`, `MULTI_GOT_FLAG`, `MULTI_GOT_ORB`,
  `MULTI_CREATE_ROBOT_POWERUPS`, the object list half of `MULTI_PLAYER_DERES`.
- **Files**: `similar/main/multi.cpp` (`objnum_local_to_remote`,
  `objnum_remote_to_local`, `map_objnum_*` → netid tables;
  `multi_send_create_powerup`/`multi_do_create_powerup` → `OBJ_CREATE`;
  `multi_send_remobj`/`multi_do_remobj`; `multi_send_drop_weapon`,
  `multi_send_drop_flag`; `multi_send_player_inventory`/`multi_do_player_inventory`
  → `INVENTORY`; `MultiLevelInv_*` host-only; `multi_send_player_deres`
  removed), `similar/main/powerup.cpp` (`do_powerup` split into
  `powerup_request(obj)` on clients and `powerup_apply(pid, obj)` on grant;
  the "closer player" check deleted), `similar/main/collide.cpp`
  (`collide_player_and_powerup` → request; `drop_player_eggs` host-only in
  network games, returns the created list), `similar/main/fireball.cpp`
  (`maybe_drop_net_powerup` host-only; `drop_powerup` takes an explicit
  velocity), `similar/main/weapon.cpp` (`spit_powerup` velocity out-param
  instead of seed), `similar/main/laser.cpp` (mines get a netid at creation),
  `similar/main/multibot.cpp` (robot egg creation host-only).
- **Shippable**: yes.
- **Risk**: high — this changes how pickups feel (one RTT delay) and touches
  every object-creating code path.
- **Play-test**: two ships hitting one mega at the same moment: exactly one
  gets it, everywhere; vulcan cannon partial ammo; flags and orbs land in the
  same place on all machines; death drops identical everywhere including with
  spawn grants; used items respawn once; pickup delay acceptable at 60 ms
  RTT; deny path (touch a missile while full) restores the object.

### Stage 4 — Firing, hits, damage, kills, respawn with lag compensation

- Implement §6.5 and §6.6: new `FIRE` with origin/time/seed/netids and
  catch-up spawn, `WEAPON_HIT`, position history, `DAMAGE`, `PLAYER_KILLED`,
  `PLAYER_SPAWN`, `WANT_RESPAWN`, host-side `apply_damage_to_player`, §5.5
  validation and `CORRECTION`. Delete `MULTI_KILL_HOST`, `MULTI_KILL_CLIENT`,
  `MULTI_REAPPEAR`, `MULTI_FIRE_TRACK`, `MULTI_FIRE_BOMB`.
- **Files**: `similar/main/laser.cpp` (`do_laser_firing` seeds `d_rand` from
  the message; `Laser_player_fire_spread_delay` takes the origin; new
  `laser_catch_up(obj, dt)`), `similar/main/multi.cpp` (`multi_send_fire`,
  `multi_do_fire` new layout; `multi_send_kill` removed; `multi_compute_kill`
  takes explicit victim/killer), `similar/main/collide.cpp`
  (`collide_player_and_weapon`: shooter → `WEAPON_HIT` report, host → apply;
  `collide_robot_and_weapon`, `collide_weapon_and_controlcen`,
  `collide_weapon_and_wall` likewise; `apply_damage_to_player` host-only;
  `explode_badass_weapon` radius damage with rewind on the host;
  `collide_player_and_player` bump damage host-only), new
  `common/main/net_history.h` + `similar/main/net_history.cpp` (rings,
  `rewind`), `similar/main/object.cpp` (death sequence started from
  `PLAYER_KILLED`; `dead_player_frame` sends `WANT_RESPAWN`),
  `similar/main/gameseq.cpp` (`InitPlayerPosition` host chooses →
  `PLAYER_SPAWN`; `DoPlayerDead` no local kill), `similar/main/net_v2.cpp`
  (validators, rate limits of §3.6).
- **Shippable**: yes.
- **Risk**: high — this is the core of "did my shot hit". Persistent weapons
  (fusion, omega) and splash damage have several edge cases; the catch-up
  spawn must not tunnel through walls (step with the weapon's normal physics,
  never teleport).
- **Play-test**: with 100–200 ms netem delay, shots that visibly hit a remote
  ship register (compare hit counts shooter vs. victim HUD); kill and death
  counters identical on all machines after 50 kills; suicide by own mega;
  friendly fire off honoured; omega on a moving target; respawn points vary
  and `InvulAppear` works; a deliberately teleporting client (debug key)
  gets corrected and does not warp on others' screens.

### Stage 5 — Join in progress, level flow, level end

- Implement §4.3–§4.5, §4.7: `LEVEL_START`/`READY`/`GO`, `SNAPSHOT_*`,
  `CLIENT_READY`, `LEVEL_STATUS`, `LEVEL_END`, `SCORES`, `ESCAPED`. Delete the
  legacy `LEGACY_SYNC`, `LEGACY_OBJECTS`, `LEGACY_ENDLEVEL`, the extras
  sequence (`net_udp_send_extras`), `objnum_is_past` and the restart logic.
- **Files**: `similar/main/net_v2.cpp` (`net_v2_build_snapshot`,
  `net_v2_apply_snapshot`, `level_sync`, `end_current_level`,
  `send_endlevel_packet` → `LEVEL_STATUS`), `similar/main/kmatrix.cpp`
  (authoritative matrix, keepalive polling), `similar/main/multi.cpp`
  (`multi_prep_level_objects`, `multi_prep_level_player`,
  `multi_send_endlevel_start` → `ESCAPED`), `similar/main/gameseq.cpp`
  (`StartNewLevel` waits for `LEVEL_GO`).
- **Shippable**: yes.
- **Risk**: medium; the snapshot touches `object_rw` conversion and object
  allocation on the joiner.
- **Play-test**: join a level with ~200 objects mid-fight; join during the
  reactor countdown (refused with the right reason); level change with 8
  players and 15 % loss; rejoin after a timeout keeps the score; checksum
  mismatch (edited level) is reported, not a hang.

### Stage 6 — Doors, triggers, reactor, game modes, robots

- Implement §6.7 and §6.8: `WALL_REQUEST`/`WALL_STATE`, `TRIGGER_REQUEST`/
  `TRIGGER`, `EFFECT_BLOWUP`, `REACTOR_DESTROYED`, `GAME_MODE_STATE`, `CAPTURE`,
  `ORB_BONUS`, `SCORE_UPDATE`, markers, and host-simulated robots with the
  claim system removed. Robot support can be split off as stage 6b if the
  deathmatch stages need to ship first (§9).
- **Files**: `similar/main/wall.cpp` (`wall_open_door` request path),
  `similar/main/switch.cpp` (`check_trigger` → request; effects on
  `TRIGGER`), `similar/main/cntrlcen.cpp` (`do_controlcen_destroyed_stuff`
  from message; reactor fire as host `FIRE`), `similar/main/fuelcen.cpp`
  (`fuelcen_check_for_goal`, `fuelcen_check_for_hoard_goal` host-only),
  `similar/main/collide.cpp` (`check_effect_blowup` remote flag),
  `similar/main/multibot.cpp` (delete `multi_can_move_robot`,
  `multi_add_controlled_robot`, claim/release; `multi_send_robot_frame` →
  bundle records; robot fire/explode host-only), `similar/main/ai.cpp`
  (remote-animation mode for clients), `similar/main/multi.cpp` (game mode
  messages, `multi_check_for_killgoal_winner` host-only).
- **Shippable**: yes.
- **Risk**: low for doors/triggers/reactor; medium-high for robots (host CPU
  for `ai_frame` on all robots is what single player does anyway; the risk is
  in animation/AI state on clients).
- **Play-test**: switch-heavy level (doors open once, everywhere); CTF and
  hoard full rounds with captures and drops; bounty target changes; coop
  level with matcens and a boss; robot anarchy with 8 players.

### Stage 7 — Cleanup and documentation

- Delete the v1 remnants (`enum class upid`, `UDP_mdata_*`,
  `PacketLossPrevention` option and UI, `PacketsPerSec`, `command_length`
  table entries that no longer exist), set the final `MULTI_PROTO_VERSION`,
  update `Documentation/network-protocol.md` (mark as historical, point here)
  and rewrite this document's tables from the code as v1's was.
- **Risk**: low.

### Cross-cutting test tooling

- A `-netsim loss=<pct>,delay=<ms>,jitter=<ms>` command line option (debug
  builds only) that wraps the socket send/receive in the same `sim_link`
  used by the unit test, so latency and loss can be reproduced on one machine
  with several instances (`-udp_myport`), without root or `netem`.
- A `-netlog` option writing one line per packet (time, direction, seq, ack,
  size, chunk types) for post-mortem analysis of playtests.

---

## 9. Open questions for the maintainer

1. **Default tick rate.** 60 Hz costs the host ≈ 1.4 Mbit/s upstream with 8
   players (§7), 30 Hz half of that. Options: (a) 60 Hz default, host setting
   30/60/120; (b) 60 Hz tick but a per-client `state_divisor` the host lowers
   automatically when that client's link shows sustained loss (state every
   second tick to that client only). (a) is simpler; (b) helps the one player
   on a bad line without slowing everyone. Recommendation: (a) now, (b) later
   if playtests show it is needed.
2. **Shields and energy pickups.** Host-granted (§6.2) means a one-RTT delay
   (20–100 ms) before the HUD reacts and before the object disappears
   locally (it is hidden immediately, so only the number lags). The
   alternative — client-instant for shields/energy, as the fork's #5 did —
   keeps a small duplication window for those two types. Recommendation:
   host-granted for everything, revisit only if playtesters notice.
3. **Robot games in v2.** Host-simulated robots (stage 6) are the clean
   answer but the most work outside deathmatch. Options: implement in stage 6
   as planned, or ship v2 refusing robot anarchy/coop (the setup menu greys
   them out) and add them later. Recommendation: defer to a 6b that follows
   the first v2 release, since the fork's focus is deathmatch.
4. **Trigger latency.** Switches and fly-through triggers take effect only on
   the host's `TRIGGER` (one RTT), doors open locally at once. Local
   prediction of triggers would need rollback of arbitrary level effects.
   Recommendation: accept the RTT for triggers; door prediction as designed.
5. **Position exactness vs. bandwidth.** Positions are sent as exact 12-byte
   `fix` triples. A segment-relative 16-bit encoding (1/256 unit, as v1
   `shortpos`) would cut the bundle by 6 bytes per player (≈ 15 % of host
   upload) at the cost of a small placement error and a dependency on the
   receiver having the same segment geometry. Recommendation: exact first;
   measure, then decide.
6. **D1X.** The design is shared, but every stage doubles the test matrix if
   D1X is kept in step. Options: keep D1X compiling and functional at every
   stage (the code is `similar/`, so most of it comes for free) but play-test
   only D2X until stage 5; or drop D1X multiplayer from the fork.
   Recommendation: keep it compiling, play-test D2X only.
