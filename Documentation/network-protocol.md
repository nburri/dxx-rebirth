# DXX-Rebirth multiplayer network protocol (UDP)

This document describes the UDP multiplayer protocol implemented by DXX-Rebirth,
with a focus on D2X-Rebirth (`DXX_BUILD_DESCENT == 2`). Differences in
D1X-Rebirth (`DXX_BUILD_DESCENT == 1`) are called out where they exist.

Everything here is derived from the source code. References use the form
`file:line` or a function name. Line numbers refer to the revision this document
was written against and will drift; the function names are the stable anchors.

Main sources:

| File | Content |
|---|---|
| `similar/main/net_udp.cpp` | UDP transport, packet types (`enum class upid`), session management, reliable delivery, tracker |
| `common/main/net_udp.h` | Ports, queue sizes, `UPID_MAX_SIZE`, `UPID_MDATA_BUF_SIZE`, tracker opcodes |
| `common/main/multiinternal.h` | `multiplayer_command_t` (game message ids) and `command_length` table |
| `common/main/multi.h` | `multiplayer_data_priority`, `kick_player_reason`, `network_state`, `netgame_info`, `MULTI_PROTO_VERSION` |
| `similar/main/multi.cpp` | Game message senders (`multi_send_*`) and handlers (`multi_do_*`), object number mapping |
| `similar/main/multibot.cpp` | Robot control and robot related messages |
| `common/include/byteutil.h` | `PUT_INTEL_*` / `GET_INTEL_*` (little-endian encoding) |

Terminology:

- **host**: the player who created the game. The host is always player number 0
  (`#define multi_i_am_master() (Player_num == 0)`, `multi.h:693`).
- **client**: any other player.
- **UPID packet**: one UDP datagram. The first byte is a `upid` value (section 3).
- **game message**: one `MULTI_*` record inside an MDATA packet (section 5).
  Several game messages can be packed into one MDATA packet.
- `fix`: 16.16 fixed point, sent as a 32-bit little-endian integer.
- `vms_vector`: three `fix` values (x, y, z), 12 bytes.

---

## 1. Overview

### 1.1 Transport and topology

- The protocol runs over UDP datagrams only (`udp_open_socket` creates a
  `SOCK_DGRAM` socket, `net_udp.cpp:1079`).
- The topology is a **star**. Clients only talk to the host; the host relays.
  - Clients send PDATA and MDATA to `Netgame.players[0]` only
    (`net_udp_send_pdata`, `net_udp_send_mdata`).
  - `send_data_direct` refuses to send from a client to anyone except the host
    (`Error("Client sent direct data to non-Host ...")`, `net_udp.cpp:5764`).
  - The host forwards each client's PDATA and MDATA to all other clients
    (`net_udp_process_pdata`, `net_udp_process_mdata`). For MDATA the host keeps
    the original sender's player number in the packet header, so receivers see
    the original sender (see 4.2).
  - Clients accept PDATA and MDATA only if the UDP source address is the host
    address (`net_udp_process_pdata`, `net_udp_process_mdata`).
- The host is a single point of failure: when a client sees the host disconnect,
  it leaves the game ("Host left the game!", `multi_disconnect_player`,
  `multi.cpp:2045`).

### 1.2 Ports and sockets

| Item | Value | Source |
|---|---|---|
| Default game port | `UDP_PORT_DEFAULT = 42424` | `net_udp.h:63` |
| Local port | `-udp_myport` if >= 1024, otherwise 42424 | `reset_UDP_MyPort`, `net_udp.cpp:870` |
| Second socket | If the local port is not 42424, a second socket is opened on 42424 so LAN broadcasts are still received | `net_udp_start_game`, `net_udp.cpp:4925`; `net_udp_list_join_game`, `net_udp.cpp:1699` |
| IPv4 discovery address | `255.255.255.255:42424` (`GBcast`), `SO_BROADCAST` is set on every socket | `net_udp.cpp:553`, `udp_open_socket` |
| IPv6 discovery address | `ff02::1` port 42424 (`GMcast_v6`), only with `DXX_USE_IPv6` | `net_udp.cpp:569` |

Both sockets are polled by `net_udp_listen()`. Receive buffers are
`UPID_MAX_SIZE` (1024) bytes (`net_udp_listen`, `net_udp.cpp:5306`).

### 1.3 IPv4 and IPv6

`_sockaddr` (`multi.h:86`) is a union of `sockaddr`, `sockaddr_in` and (with
`DXX_USE_IPv6`) `sockaddr_in6`. An IPv6 build creates `AF_INET6` sockets and
resolves names with `AI_V4MAPPED | AI_ALL` (`udp_dns_filladdr`,
`net_udp.cpp:1019`), so IPv4 peers appear as v4-mapped addresses. Peer identity
is the complete `_sockaddr`, compared with `memcmp` (`operator==`,
`net_udp.cpp:844`).

### 1.4 Tracker (optional, `DXX_USE_TRACKER`)

A tracker is a separate program ([dxx-tracker](https://github.com/Mako88/dxx-tracker/)).
Its default address is `tracker.dxx-rebirth.com:9999` (`TRACKER_ADDR_DEFAULT`,
`TRACKER_PORT_DEFAULT`, `net_udp.h:68-70`). The host registers its game
every 10 seconds, clients request a game list, and the tracker can help with
NAT hole punching. The tracker opcodes are listed in section 3.

### 1.5 Protocol and version checks

The protocol has two version identifiers:

- The program version `DXX_VERSION_MAJORi / MINORi / MICROi`, built from
  `SConstruct`.
- `MULTI_PROTO_VERSION` (currently `16`, `multi.h:151`).

In addition, request packets carry a 4-byte game identifier `UDP_REQ_ID`: `"D1XR"`
in D1X and `"D2XR"` in D2X (`net_udp.cpp:77-81`).

The checks work like this:

| Packet | Check | On mismatch |
|---|---|---|
| `game_info_lite_req` (host side) | major, minor and micro version must match (`net_udp_check_game_info_request`, `net_udp.cpp:912`). The protocol version is not checked. | No reply |
| `game_info_req` (host side) | `MULTI_PROTO_VERSION`, then major, minor and micro (`net_udp.cpp:924`) | Host replies with `version_deny` |
| `game_info_lite` (client side) | major, minor and micro must match (`net_udp_process_game_info_light`, `net_udp.cpp:3096`) | Entry silently ignored |
| `version_deny` (client side) | Stores the host's version in `Netgame.protocol.udp.program_iver` and sets `valid = -1` (`net_udp_process_version_deny`) | `net_udp_game_connect` shows "Version mismatch! Cannot join Game." with both versions |
| `sync` (client side) | `Netgame.segments_checksum` must equal the local `my_segments_checksum` (`net_udp_read_sync_packet`, `net_udp.cpp:4635`) | Shows `TXT_NETLEVEL_NMATCH` and throws `multi::level_checksum_mismatch` |

The request packets contain `UDP_REQ_ID`. The helper that compares it
(`net_udp_check_game_info_request(const upid_rspan<id>)`, `net_udp.cpp:2631`)
is a one-argument overload. `net_udp_process_packet` calls only the
two-argument overloads (`net_udp.cpp:3390`, `3422`), which compare version
numbers only. The `game_info_request_result::id_mismatch` branches are
therefore not reached with the current call sites (see section 8).

`game_info_lite` replies and broadcasts carry no D1/D2 identifier. The only
filter a client applies to them is the program version.

### 1.6 Byte order and data encoding

- All multi-byte integers are **little-endian**. They are written with
  `PUT_INTEL_SHORT` / `PUT_INTEL_INT` and read with `GET_INTEL_SHORT` /
  `GET_INTEL_INT` (`byteutil.h:112-167`). These convert through `INTEL_SHORT` /
  `INTEL_INT`, which swap only on big-endian hosts. They use `memcpy` when
  `DXX_WORDS_NEED_ALIGNMENT` is set, so unaligned fields are safe.
- Vectors are written with `multi_put_vector` and read with `multi_get_vector`
  as three little-endian `int32` values (`multi.cpp:232-257`).
- Some game messages are defined as serialized structs
  (`DEFINE_MULTIPLAYER_SERIAL_MESSAGE`, `multiinternal.h:19`). They use
  `serial::writer::le_bytebuffer` / `serial::reader::le_bytebuffer`, which are
  also little-endian (`multi_serialize_write` and `multi_serialize_read`,
  `multiinternal.h:158-178`). Fields are packed without padding.
- Strings (`ntstring<N>`) in game info packets have variable length. If the
  string is shorter than `N`, it is followed by a NUL. If it is exactly `N`
  characters long, no NUL is sent (`ntstring::copy_out`; the receiver side is
  `copy_to_ntstring`, `net_udp.cpp:862`).
- Callsigns in sequence and game info packets are sent as a fixed
  `CALLSIGN_LEN + 1` = 9 bytes.
- Exceptions:
  - The `ping` packet contains the host's `fix64` timestamp copied with
    `memcpy` in host byte order. The client only echoes it back, so the host
    always reads its own byte order (`net_udp_ping_frame`, `net_udp.cpp:6132`).
  - `object_data` carries raw `object_rw` structs (264 bytes,
    `object.h:542`). `multi_object_to_object_rw` converts every
    multi-byte field with `INTEL_*` before copying (`multi.cpp` after
    `multi_process_data`).
  - Tracker packets are partly ASCII text (section 3.3).

### 1.7 Size limits

| Constant | Value | Meaning | Source |
|---|---|---|---|
| `UPID_MAX_SIZE` | 1024 | Receive buffer size, and the maximum size of `object_data` packets | `net_udp.h:86` |
| `UPID_MDATA_BUF_SIZE` | 454 | Maximum payload (concatenated game messages) of one MDATA packet | `net_udp.h:87` |
| `sizeof(UDP_mdata_info)` | at least 462 | Upper bound for a received MDATA packet (`net_udp_process_mdata`) and the size of MDATA send buffers. This is a local struct, not the wire layout. | `net_udp.h:128` |
| `upid_length<upid::game_info>` | `sizeof(netgame_info)` | Only used as an upper bound when accepting `game_info` / `sync` (`net_udp_process_packet`) | `net_udp.cpp:149` |
| `UDP_MAX_NETGAMES` | 900 | Size of the client's game list | `net_udp.h:75` |
| `MAX_PLAYERS` | 8 | | `fwd-player.h:75` |
| `MAX_NET_CREATE_OBJECTS` | 40 (D2), 20 (D1) | Maximum objects listed in one `MULTI_PLAYER_DERES` | `multi.h:160-163` |

No explicit path-MTU handling exists. The largest packets are `object_data`
(at most 5 + 9 + 3 × 273 = 833 bytes, for the first packet with its reset marker; see 2.5.2), `game_info` / `sync`
(at most 469 bytes in D2, see 3.2.3) and MDATA (at most 6 + 454 = 460 bytes).

---

## 2. Session lifecycle

### 2.1 Network states

`Network_status` (`enum class network_state`, `multi.h:556`) is sent on the
wire in game info packets:

| Value | Name | Meaning |
|---|---|---|
| 0 | `menu` | Not in a game |
| 1 | `playing` | In a level |
| 2 | `browsing` | Looking at the game list (D2 sets this in `net_udp_do_join_game`; tracker game entries are only accepted in this state) |
| 3 | `waiting` | Level loaded, waiting for the host's `sync` |
| 4 | `starting` | Host is collecting players before the first level |
| 5 | `endlevel` | Between levels |

`get_effective_netgame_status` (`net_udp.cpp:825`) reports `endlevel` in game
info if the reactor is destroyed or fewer than 30 seconds of play time remain.

### 2.2 Discovering games

1. When the game list opens (and on F4/F5/F6), the client sends a
   `game_info_lite_req` to the IPv4 broadcast address and to the IPv6 multicast
   address, and `UPID_TRACKER_REQGAMES` to the tracker
   (`netgame_list_game_menu::event_handler`, `net_udp.cpp:1476`).
2. The host answers each valid request with `game_info_lite`, at most 8 times
   per second across all requesters (`last_lite_req_time`, `net_udp.cpp:3419`).
3. While a game runs, the host broadcasts `game_info_lite` every 10 seconds
   (`do_protocol_frame`, `net_udp.cpp:5495`). The host also broadcasts it
   when the game starts, when the player list changes during `starting`, and
   when the host leaves (with `numplayers = 0`).
4. The client keeps entries in `Active_udp_games`. An entry is matched by game
   name and `GameID`. An update with `numconnected == 0` removes the entry
   (`net_udp_process_game_info_light`).

`GameID` is `d_rand()` after `d_srand(timer_query())` when the host starts the
game (`net_udp_start_game`, `net_udp.cpp:4940`).

D2 only: if `HOARD.HAM` is present, the host puts hoard information into the
upper bits of `game_flag` (`hoard`, `team_hoard`, `really_endlevel`,
`really_forming`) (`net_udp_update_netgame`, `net_udp.cpp:2759`). The client
decodes this in `net_udp_process_game_info_light`.

### 2.3 Connecting to a host

`net_udp_game_connect` (`net_udp.cpp:1299`) is used both for "join from list"
and for manual connect:

1. Every second it sends `game_info_req` to the host. With a tracker game ID,
   it also asks the tracker for hole punching after 4 seconds
   (`udp_tracker_request_holepunch`).
2. The host answers at most twice per second (`last_full_req_time`,
   `net_udp.cpp:3387`) with `game_info` (heavy info, section 3.2.3), or with
   `version_deny`.
3. The client stores the heavy info in `Netgame` and sets
   `Netgame.protocol.udp.valid = 1`. It also stores the sender address as the
   host address: `Netgame.players[0].protocol.udp.addr = game_addr`
   (`net_udp_process_game_info_heavy`, `net_udp.cpp:3163`).
4. After 10 seconds without an answer, the client gives up ("No response by
   host").
5. The user sees the game info and chooses to join. The client then requests
   fresh game info again and calls `net_udp_do_join_game` (`net_udp.cpp:5111`).
   That function checks, all on the client side:
   - the mission can be loaded,
   - D2 OEM or Mac shareware level limits,
   - `HOARD.HAM` is present for hoard games,
   - `net_udp_can_join_netgame` (`net_udp.cpp:1857`):
     - `starting`: allowed.
     - `playing`: allowed only if the game is not closed and has capacity or a
       disconnected slot, or if this callsign is already in the player list at
       `your_index`.
     - Otherwise, refused.
6. The client sets `Player_num = 1` for now and starts the level
   (`StartNewLevel`). The level start calls `multi::dispatch->level_sync()`
   (`gameseq.cpp:2023`).

### 2.4 Game setup and level start (`starting` → `playing`)

Host side (`net_udp_setup_game` → `net_udp_start_game` → `net_udp_select_players`):

1. The host opens its sockets and sets `Network_status = starting`.
2. Each `request` received in `starting` state adds the sender to
   `Netgame.players` (`net_udp_add_player`, `net_udp.cpp:2669`). The host then
   sends `game_info` to every player and broadcasts `game_info_lite`
   (`net_udp_send_netgame_update`).
3. A `quit_joining` packet in `starting` state removes the sender
   (`net_udp_remove_player`).
4. When the host accepts the player list:
   - Players that were not selected get `dump` with reason `dork`.
   - An abort sends `dump` with reason `aborted` plus `game_info` with
     `numplayers = 0`.
   - Team games show the team selection menu.
5. The host calls `StartNewLevel(Netgame.levelnum)`, which reaches
   `level_sync()`.

Level sync (`dispatch_table::level_sync`, `net_udp.cpp:5076`) runs at the start
of every level:

- **Host**: `net_udp_wait_for_requests` sets `Network_status = waiting` and
  waits until every player is either `playing` or `disconnected`. A `request`
  received in `waiting` state marks the sender as `playing`
  (`net_udp_process_request`, `net_udp.cpp:3343`). Then `net_udp_send_sync`
  (`net_udp.cpp:4721`):
  - checks that the level has enough start positions (otherwise sends `dump`
    `aborted` to everyone),
  - shuffles `Netgame.locations` in non-cooperative games (`std::minstd_rand`
    seeded with the timer),
  - sets `game_status = playing` and `segments_checksum`,
  - sends `sync` (heavy game info with UPID 10) to every connected client,
  - applies the same data locally.
- **Client**: `net_udp_wait_for_sync` sets `Network_status = waiting` and sends a
  `request`. `net_udp_sync_poll` resends the `request` every 2 seconds until a
  `sync` arrives (`net_udp.cpp:3663`). On `sync`, `net_udp_read_sync_packet`
  (`net_udp.cpp:4621`):
  - finds its own player number: the slot `i == your_index` whose callsign
    matches the local pilot name,
  - checks the segment checksum,
  - copies scores, kills and connection states,
  - places ships at `Player_init[Netgame.locations[i]]` (not on rejoin),
  - sets `Network_status = playing`.
- If a client leaves the waiting dialog, it sends `quit_joining`.

### 2.5 Joining a game in progress

#### 2.5.1 Request and admission

A `request` received by the host in `playing` state is handled by
`net_udp_welcome_player` (`net_udp.cpp:1981`). If `Netgame.RefusePlayers` is
set, the host first asks the user (`net_udp_do_refuse_stuff`,
`net_udp.cpp:6212`):

- F6 (or, in team games, Alt-1 / Alt-2) accepts the player.
- A request that arrives more than `REFUSE_INTERVAL` (8 s) after the prompt,
  without acceptance, is answered with `dump` `dork`.

Admission rules in `net_udp_welcome_player`:

| Condition | Result |
|---|---|
| Host is in `endlevel`, or the reactor is destroyed | `dump` `endlevel` |
| Host is already sending objects or extras to another player | Silently ignored (the client retries every 2 s) |
| `PacketLossPrevention` is on and fewer than `UDP_MDATA_STOR_MIN_FREE_2JOIN` (384) reliable queue slots are free | Silently ignored |
| Client's `Current_level_num` differs from the host's | `dump` `level` |
| Callsign and address match an existing slot that is `disconnected` | Rejoin into that slot |
| Callsign and address match an existing slot that is still connected | Silently ignored |
| New player, game closed (`netgame_rule_flags::closed`) | `dump` `closed` |
| New player, `N_players < max_numplayers` | New slot `N_players` |
| New player, game full but some slots disconnected | Take over the slot that has been disconnected the longest (oldest `LastPacketTime`) |
| New player, all slots connected | `dump` `full` |

When the player is admitted, the host stores the player in `UDP_sync_player`,
sets `Network_send_objects = 1` and starts `net_udp_send_objects`.

#### 2.5.2 Object transfer

`net_udp_send_objects` (`net_udp.cpp:2322`) is called from every
`do_protocol_frame` (with `listen`) and sends at most one `object_data` packet
every 1/50 second.

- The first packet starts with a marker entry (object number `-1`) that
  contains the joining player's number.
- The host then sends, in two passes over `0..Highest_object_index`, every
  object of type powerup, player, reactor, ghost, robot or hostage (D2: also
  live proximity mines, `PMINE_ID`):
  - Pass 0 (`Network_send_object_mode == 0`): objects with owner `-1` or owned
    by the joining player.
  - Pass 1: objects owned by other players.
- Each entry contains the host's local object number, the owner, the owner's
  remote object number and an `object_rw`. At most 3 objects fit in one packet.
- After the last object, a trailer entry with object number `0xfffffffe`
  (`network_checksum_marker_object`, `net_udp.cpp:551`) carries the total count.
- If an object that was already sent changes during the transfer, the transfer
  restarts from the beginning: `Network_send_objnum = -1`, see
  `objnum_is_past` (`net_udp.cpp:2135`) and its callers in `multi.cpp` /
  `multibot.cpp`.

Client side (`net_udp_read_object_packet`, `net_udp.cpp:2476`), only in state
`waiting`:

- The `-1` marker clears the object array (`init_objects`), sets
  `Network_rejoined = 1` and adopts the player number from the marker
  (`change_playernum_to`).
- Objects owned by `-1` or by the joining player go to the same object number
  as on the host.
- Other objects are allocated locally with `obj_allocate` and mapped with
  `map_objnum_local_to_remote` (section 6).
- The trailer triggers `net_udp_verify_objects`, which accepts the transfer if
  at most 10 objects are missing and at least `max_numplayers` player or ghost
  objects exist. Otherwise it shows `TXT_NET_SYNC_FAILED` and returns to the menu.

`object_data` packets are not acknowledged and not retransmitted. Once
`Network_rejoined` is set, the client also stops resending its `request`
(`net_udp_sync_poll`).

#### 2.5.3 Rejoin sync and extras

After the trailer, `net_udp_send_rejoin_sync` (`net_udp.cpp:2571`):

1. marks the slot `playing`,
2. for a new (not returning) player, sends `addplayer` to every other connected
   client,
3. sends a `sync` (heavy game info) to the joining player with the current kill
   matrix, scores, `level_time` and `monitor_vector` (blown-up monitors,
   `net_udp_create_monitor_vector`),
4. D1 only: sends door states immediately (`net_udp_send_door_updates`).

The host sets `VerifyPlayerJoined = player_num`. Until the first `pdata` from
that player arrives (`net_udp_read_pdata_packet`), the host resends the `sync`
every second (`net_udp_resend_sync_due_to_packet_loss`, called from
`do_protocol_frame`).

Then the host sends "extras" (`net_udp_send_extras`, `net_udp.cpp:6372`), one
step per call and at most one step every 1/50 s, counting
`Network_sending_extras` down:

| Step | D2 | D1 |
|---|---|---|
| 9 | Disabled fly-through triggers: `MULTI_START_TRIGGER` direct | – |
| 8 | Door and wall states: `MULTI_DOOR_OPEN` / `MULTI_WALL_STATUS` direct, `MULTI_HOSTAGE_DOOR` | – |
| 7 | All markers: `MULTI_MARKER` | – |
| 6 | Thief's stolen items: `MULTI_STOLEN_ITEMS` (robot games only) | – |
| 5 / 3 | Kill goal counts: `MULTI_KILLGOALS` (if there is a kill goal or a time limit) | step 3 |
| 4 | Smashed lights: `MULTI_LIGHT` direct | – |
| 3 | `MULTI_FLAGS` for every player | – |
| 2 | Host inventory: `MULTI_PLAYER_INV`, priority 1 | step 2 |
| 1 | `MULTI_DO_BOUNTY` (bounty games) | step 1 |

"Direct" means `send_data_direct` to the joining player only (4.5). The other
messages are broadcast to everyone.

After the last step, the host sends `MULTI_HEARTBEAT` in its next frame in
time-limited games (`multi_schedule_heartbeat`), because the sync data does not
contain the level time.

#### 2.5.4 Other clients

Other clients learn about the new player from `addplayer`
(`net_udp_new_player`, `net_udp.cpp:1921`). A client learns that a
disconnected player came back when that player's relayed `pdata` arrives with
`connected == playing` (`net_udp_read_pdata_packet`, `net_udp.cpp:6044`).

### 2.6 Periodic activity while playing

| What | Interval | Where |
|---|---|---|
| `pdata` (own ship) | `F1_0 / Netgame.PacketsPerSec` (default 30, allowed 5–40: `MIN_PPS` / `MAX_PPS`, `multi.h:155`). The schedule advances by one interval per send (restarting from the current time after a forced send, or when it is still one or more intervals behind after advancing, so that a backlog after a long frame is not caught up with packets in consecutive frames), so the average rate matches `PacketsPerSec` at any frame rate at or above it; before, it was reset to the send time, and at 60 fps 30 pps gave only 20 packets per second. Also forced by `multi_send_fire` (at most 20/s) and by `multi_send_effect_blowup`. | `do_protocol_frame`, `net_udp.cpp:5440` |
| D2 thief position (`MULTI_ROBOT_POSITION`) | Same tick as `pdata` | `multi_send_thief_frame`, `multibot.cpp:445` |
| D2 `MULTI_GUIDED` position | Same tick as `pdata`, while the local player has an active guided missile and `Network_status` is `playing`. It is queued with priority 0 and sent at once in the same mdata packet as the thief position (priority 1), or flushed on its own (`net_udp_send_mdata`) if there is none. The final position is also sent when the missile is released (priority 0, immediately followed by the release message with priority 1, so both go out at once in one packet), and at priority 1 when the missile is removed in play (marked `OF_SHOULD_BE_DEAD`, `Network_status` `playing`; not by `clear_transient_objects` at a level change). Before this pacing, the position was sent every frame (priority 0) from `read_flying_controls`. | `multi_send_guided_frame` (called from `do_protocol_frame`), `multi_send_guided_release` (from `release_local_guided_missile`), `multi_send_guided_final_position` (from `obj_delete`) |
| Robot frame and MDATA flush (unreliable) | Every 1/10 s | `net_udp.cpp:5473` |
| Reliable queue processing | Every protocol frame | `net_udp_noloss_process_queue` |
| `ping` (host to clients) | Every second, host only (before this change, clients also sent it to their entries for players 1–7, whose addresses a client does not know, and every receiver discarded it) | `net_udp_ping_frame` |
| Player timeout check | Every second (only when `listen` is set) | `net_udp_timeout_check` |
| `endlevel_h` / `endlevel_c` | Every second while the reactor is destroyed; also in the kill matrix screen | `do_protocol_frame`, `kmatrix.cpp:423` |
| `game_info_lite` broadcast and tracker register (host) | Every 10 s | `do_protocol_frame` |
| `MULTI_PLAYER_INV` (priority 0) | 3 times per second | `multi_do_frame`, `multi.cpp:1118` |
| `MULTI_GMODE_UPDATE` (host, team or bounty games) | Every 2 s | `multi_do_frame` |
| `MULTI_HEARTBEAT` (priority 1) | Once per second (each frame in which the whole-second value of `ThisLevelTime` changed; before this change, every frame, with priority 0), sent by the lowest-numbered connected player, only if there is a time limit. Also in the first frame of a level (`multi_prep_level_player`), and in the frame after the host has sent the extras to a joining player (`net_udp_send_extras`), both through `multi_schedule_heartbeat`. | `multi_do_frame`, `multi.cpp:1095` |
| Powerup respawn (`MultiLevelInv_Repopulate`) | Every 1/2 s, host only, non-coop | `multi.cpp:5544` |

`do_protocol_frame` is called from `multi_do_frame` every game frame with
`listen = 1`. It is called with `force = 1` by `multi_send_fire`,
`multi_send_effect_blowup` and `leave_game`.

### 2.7 Timeouts, disconnects and kicks

- `UDP_TIMEOUT` = 5 s (`net_udp.h:79`). Every second, each peer checks every
  other connected player. If `Netgame.players[i].LastPacketTime` is older than
  5 s, it calls `multi_disconnect_player(i)` (`net_udp_timeout_check`,
  `net_udp.cpp:5383`).
- `LastPacketTime` is refreshed only by `pdata` (`net_udp_read_pdata_packet`),
  by endlevel packets, and by join handling. MDATA does not refresh it. On
  clients, the time for other clients is refreshed by the `pdata` relayed by
  the host.
- `multi_disconnect_player` (`multi.cpp:2045`):
  - turns the ship into a ghost and releases its robots,
  - picks a new bounty target if needed (host),
  - clears the reliable-delivery trace for that player
    (`dispatch_table::disconnect_player`).
  - If the disconnected player is the host, the client shows "Host left the
    game!" and quits.
- **Kick**: `dispatch_table::kick_player` (`net_udp.cpp:2725`) sends
  `dump(reason)`. When the host calls it, the host also disconnects the player
  locally. The host kicks manually with the chat command `/kick: name` or
  `/kick: #n` (`multi_send_message_end`, `multi.cpp:1454`).
- A client acts on `dump` only if it comes from the host address and the client
  is in `waiting` or `playing` state (`net_udp_process_dump`,
  `net_udp.cpp:3270`):
  - `kicked` / `pkttimeout`: shows a message and leaves the game.
  - Any other reason: shows the reason and returns to the menu.

`kick_player_reason` values (`multi.h:127`):

| Value | Name | Sent when |
|---|---|---|
| 0 | `closed` | Game is closed to new players |
| 1 | `full` | No free slot |
| 2 | `endlevel` | Host is between levels, or endlevel started during object transfer |
| 3 | `dork` | Not selected by the host, or the refuse prompt timed out |
| 4 | `aborted` | Host aborted the game setup |
| 5 | `connected` | Never sent ("never used") |
| 6 | `level` | Client is on a different level |
| 7 | `kicked` | Kicked by the host |
| 8 | `pkttimeout` | Player did not acknowledge reliable data in time (4.4) |

### 2.8 Level end

- A player who escapes sends `MULTI_ENDLEVEL_START` (priority 2), sets itself
  to `escape_tunnel`, and sends an endlevel packet (`multi_send_endlevel_start`,
  `multi.cpp:2680`).
- While the reactor is destroyed, every peer sends endlevel packets every
  second. The host sends `endlevel_h` to each client, a client sends
  `endlevel_c` to the host (`dispatch_table::send_endlevel_packet`,
  `net_udp.cpp:2801`). These packets carry connection states, the countdown
  and the kill matrix. When the receiver is not in `playing` state, a lower
  countdown value in the packet replaces the local countdown. The host
  also requires the sending client to be `playing` (`net_udp_read_endlevel_packet`,
  `net_udp.cpp:3558`).
- The kill matrix screen (`kmatrix.cpp`) keeps calling `do_protocol_frame` and
  sends endlevel packets every second. It waits until all connected players are
  in `end_menu` or `died_in_mine` state.
- `dispatch_table::end_current_level` (`net_udp.cpp:1815`) sets
  `Network_status = endlevel`, sends three endlevel packets and updates
  `Netgame`. D1 only: it decides whether to go to the secret level.
- The next level begins with `level_sync()` (2.4). The reliable queue is
  cleared there (`net_udp_noloss_init_mdata_queue`).

### 2.9 Leaving

Client (`multi_leave_game`, `multi.cpp:1174`):

1. Sends `MULTI_POSITION` and drops its eggs.
2. Sends `MULTI_PLAYER_DERES` (`deres_drop`) and `MULTI_QUIT`, both priority 2.
3. `dispatch_table::leave_game` (`net_udp.cpp:5234`) runs one forced
   `do_protocol_frame`, flushes and closes the sockets.

Host:

1. Finishes sending pending extras.
2. Sends `MULTI_QUIT`.
3. Sends `game_info` with `numplayers = 0` to every client and broadcasts
   `game_info_lite`.
4. Unregisters from the tracker.

Clients handle the host's `MULTI_QUIT` through `multi_do_quit` →
`multi_disconnect_player(0)` → "Host left the game!".

---

## 3. UDP packet types (`upid`)

### 3.1 Summary

The first byte of every datagram is the `upid` (`enum class upid`,
`net_udp.cpp:87`). Unknown values are dropped (`build_upid_from_untrusted`).
Packets whose length is checked with `build_upid_rspan` must have exactly the
listed length.

| Id | Name | Direction | Length (bytes) | Purpose |
|---|---|---|---|---|
| 1 | `version_deny` | host → requester | 9 (exact) | Reply to `game_info_req` with a version mismatch |
| 2 | `game_info_req` | client → host | 13 (exact) | Request full game info |
| 3 | `game_info` | host → client | variable, at most `sizeof(netgame_info)` | Full ("heavy") game info |
| 4 | `game_info_lite_req` | client → broadcast / host | 13 (exact) | Discover games |
| 5 | `game_info_lite` | host → requester / broadcast | variable, at most `sizeof(UDP_netgame_info_lite)` | Short game info for the game list |
| 6 | `dump` | host → client | 2 (exact) | Join refused or kicked; carries `kick_player_reason` |
| 7 | `addplayer` | host → clients | 12 (exact) | A new player joined a running game |
| 8 | `request` | client → host | 12 (exact) | Join request, or "I have loaded the level, send sync" |
| 9 | `quit_joining` | client → host | 1 (exact) | Client cancels joining |
| 10 | `sync` | host → client | same as `game_info` | Start or rejoin: full game state |
| 11 | `object_data` | host → joining client | at most 1024 | Object transfer for a mid-game join |
| 12 | `ping` | host → clients | 37 (exact) | Host time stamp and ping list |
| 13 | `pong` | client → host | 10 (exact) | Echo of the ping time stamp |
| 14 | `endlevel_h` | host → clients | 170 | End of level state from the host |
| 15 | `endlevel_c` | client → host | 24 | End of level state from a client |
| 16 | `pdata` | client → host, host → clients | 49 (exact) | Ship position (unreliable) |
| 17 | `mdata_pnorm` | client → host, host → clients | 2 + payload | Game messages, not acknowledged |
| 18 | `mdata_pneedack` | client → host, host → clients | 6 + payload | Game messages, acknowledged, in order |
| 19 | `mdata_ack` | receiver → sender | 7 (exact) | Acknowledges one `mdata_pneedack` |
| 20 | – | | | Not used |
| 21 | `UPID_TRACKER_REGISTER` | host → tracker | variable | Register or refresh a game |
| 22 | `UPID_TRACKER_REMOVE` | host → tracker | 1 | Remove the game |
| 23 | `UPID_TRACKER_REQGAMES` | client → tracker | variable | Request the game list |
| 24 | `tracker_gameinfo` | tracker → client | variable | One game from the tracker |
| 25 | `tracker_ack` | tracker → host | 2 | Registration acknowledgement |
| 26 | `tracker_holepunch` | client → tracker, tracker → host, host → client | 3 / variable / 1 | NAT hole punching |

Ids 21–26 exist only with `DXX_USE_TRACKER`. 21–23 are `#define`s in
`net_udp.h:89-91`, 24–26 are `upid` enumerators. Their numbers are fixed by the
tracker program (comment at `net_udp.cpp:112`).

Receiver-side role filters (`net_udp_process_packet`, `net_udp.cpp:3360`):

- Only the host processes 2, 4, 8, 9, 13, 15 and 25.
- Only clients process 1, 3, 5, 6, 7, 10, 11, 12 and 14.
- 16–19 are processed by everyone.

### 3.2 Packet layouts

In the tables, "u8/u16/u32/i32" are little-endian integers.

#### 3.2.1 `game_info_req` (2) and `game_info_lite_req` (4)

Built from `udp_request_game_info_template` (`net_udp.cpp:479`).

| Offset | Size | Field |
|---|---|---|
| 0 | 1 | upid (2 or 4) |
| 1 | 4 | `UDP_REQ_ID`: `"D2XR"` (D1: `"D1XR"`) |
| 5 | 2 | `DXX_VERSION_MAJORi` |
| 7 | 2 | `DXX_VERSION_MINORi` |
| 9 | 2 | `DXX_VERSION_MICROi` |
| 11 | 2 | `MULTI_PROTO_VERSION` (checked only for `game_info_req`) |

#### 3.2.2 `version_deny` (1)

`udp_response_version_deny` (`net_udp.cpp:753`); read in
`net_udp_process_version_deny`.

| Offset | Size | Field |
|---|---|---|
| 0 | 1 | upid = 1 |
| 1 | 2 | Host major version |
| 3 | 2 | Host minor version |
| 5 | 2 | Host micro version |
| 7 | 2 | Host `MULTI_PROTO_VERSION` |

#### 3.2.3 `game_info` (3) and `sync` (10): heavy game info

Written by `net_udp_prepare_heavy_game_info` (`net_udp.cpp:2900`). Read by
`net_udp_process_game_info_heavy` (`net_udp.cpp:3159`). The two functions
agree field by field. Offsets are for D2; the D1 differences are listed below
the table.

| Offset (D2) | Size | Field | Notes |
|---|---|---|---|
| 0 | 1 | upid | 3 = `game_info`, 10 = `sync` |
| 1 | 2 | major version | Stored, not compared, on receive |
| 3 | 2 | minor version | |
| 5 | 2 | micro version | |
| 7 | 1 | `your_index` | Index of the slot whose address equals the recipient's address, or `MULTI_PNUM_UNDEF` (0xcc) |
| 8 | 8 × 11 | `players[i]`: callsign (9), `connected` (1), `rank` (1) | `player_connection_status`: 0 disconnected, 1 playing, 2 waiting, 3 died_in_mine, 4 found_secret, 5 escape_tunnel, 6 end_menu |
| 96 | 4 | `levelnum` (i32) | D1: negative means secret level |
| 100 | 1 | `gamemode` | `network_game_type`: 0 anarchy, 1 team_anarchy, 2 robot_anarchy, 3 cooperative, 4 capture_flag, 5 hoard, 6 team_hoard, 7 bounty |
| 101 | 1 | `RefusePlayers` | |
| 102 | 1 | `difficulty` | |
| 103 | 1 | `game_status` | Effective `network_state` (2.1) |
| 104 | 1 | `numplayers` | |
| 105 | 1 | `max_numplayers` | |
| 106 | 1 | `numconnected` | |
| 107 | 1 | `game_flag` | `netgame_rule_flags`: 1 closed, 4 show all players on automap, 8/16/32/64 D2 hoard bits |
| 108 | 1 | `team_vector` | Bit `i` set = player `i` is on the red team |
| 109 | 4 | `AllowedItems` | `netflag_flag` (u32 in D2, u16 in D1 but sent as 4 bytes) |
| 113 | 4 | `ShufflePowerupSeed` | Sent as 0 in cooperative games |
| 117 | 1 | `SecludedSpawns` | |
| 118 | 2 | `SpawnGrantedItems.mask` | D1: 1 byte |
| 120 | 2 | `DuplicatePowerups` packed field | D1: 1 byte |
| 122 | 1 | `Allow_marker_view` | D2 only |
| 123 | 1 | `AlwaysLighting` | D2 only |
| 124 | 1 | `ThiefModifierFlags` | D2 only |
| 125 | 1 | `AllowGuidebot` | D2 only |
| 126 | 1 | `ShowEnemyNames` | |
| 127 | 1 | `BrightPlayers` | |
| 128 | 1 | `InvulAppear` | |
| 129 | 2 × 9 | `team_name[blue]`, `team_name[red]` | |
| 147 | 8 × 4 | `locations[i]` | Start position index per player |
| 179 | 8 × 8 × 2 | `kills[i][j]` | Kill matrix |
| 307 | 2 | `segments_checksum` | |
| 309 | 2 | `team_kills[blue]` | |
| 311 | 2 | `team_kills[red]` | |
| 313 | 8 × 2 | `killed[i]` | |
| 329 | 8 × 2 | `player_kills[i]` | |
| 345 | 4 | `KillGoal` | |
| 349 | 4 | `PlayTimeAllowed` (fix) | |
| 353 | 4 | `level_time` | |
| 357 | 4 | `control_invul_time` | |
| 361 | 4 | `monitor_vector` | Bit mask of blown-up monitors |
| 365 | 8 × 4 | `player_score[i]` | |
| 397 | 8 × 1 | `net_player_flags[i]` | Low 8 bits of `player_flags` only |
| 405 | 1 | `PacketsPerSec` | |
| 406 | 1 | 0 | Formerly the high byte of `PacketsPerSec` |
| 407 | 1 | `PacketLossPrevention` | |
| 408 | 1 | `NoFriendlyFire` | |
| 409 | 1 | `MouselookFlags` | |
| 410 | 1 | `PitchLockFlags` | |
| 411 | ≤ 25 | `game_name` | Variable length string (1.6) |
| … | ≤ 25 | `mission_title` | |
| … | ≤ 8 | `mission_name` | |

D1 differences: `SpawnGrantedItems` and `DuplicatePowerups` are 1 byte each,
and the four D2-only bytes are missing. All offsets from `ShowEnemyNames`
onward are 6 lower (`ShowEnemyNames` at 120). The maximum length is 469 bytes in
D2 and 463 in D1.

The receiver checks only that the packet is not longer than
`sizeof(netgame_info)`. It also rejects an invalid `game_status`.

#### 3.2.4 `game_info_lite` (5)

Written by `net_udp_prepare_light_game_info` (`net_udp.cpp:2876`). Read by
`net_udp_process_game_info_light` (`net_udp.cpp:3076`).

| Offset | Size | Field |
|---|---|---|
| 0 | 1 | upid = 5 |
| 1 | 2 | major version |
| 3 | 2 | minor version |
| 5 | 2 | micro version |
| 7 | 4 | `GameID` |
| 11 | 4 | `levelnum` |
| 15 | 1 | `gamemode` |
| 16 | 1 | `RefusePlayers` |
| 17 | 1 | `difficulty` |
| 18 | 1 | effective `game_status` |
| 19 | 1 | `numconnected` |
| 20 | 1 | `max_numplayers` |
| 21 | 1 | `game_flag` |
| 22 | ≤ 25 / ≤ 25 / ≤ 8 | `game_name`, `mission_title`, `mission_name` (variable length) |

#### 3.2.5 `dump` (6)

| Offset | Size | Field |
|---|---|---|
| 0 | 1 | upid = 6 |
| 1 | 1 | `kick_player_reason` (2.7) |

#### 3.2.6 `request` (8) and `addplayer` (7)

Written by `net_udp_send_sequence_packet` (`net_udp.cpp:1725`, `1736`). Read by
`build_udp_sequence_from_untrusted` and `build_from_untrusted`
(`net_udp.cpp:1747-1770`).

| Offset | Size | `request` | `addplayer` |
|---|---|---|---|
| 0 | 1 | upid = 8 | upid = 7 |
| 1 | 9 | callsign (lowercased on receive) | callsign |
| 10 | 1 | `Current_level_num` of the requester (i8) | slot number of the new player |
| 11 | 1 | rank (`player_rank`, 0–9) | rank |

#### 3.2.7 `quit_joining` (9)

A single byte, the upid (`net_udp_wait_for_sync`). In `starting` state the host
removes the player. In `playing` state it stops an object transfer to that
address (`net_udp_stop_resync`).

#### 3.2.8 `object_data` (11)

Written by `net_udp_send_objects` (`net_udp.cpp:2322`). Read by
`net_udp_read_object_packet` (`net_udp.cpp:2476`).

Header:

| Offset | Size | Field |
|---|---|---|
| 0 | 1 | upid = 11 |
| 1 | 4 | Number of entries in this packet (i32) |
| 5 | … | Entries |

Entry:

| Size | Field | Notes |
|---|---|---|
| 4 | Object number on the host (i32) | `-1`: reset marker. `0xfffffffe`: trailer. |
| 1 | Owner (i8) | For the reset marker: the joining player's number. For the trailer: the joining player's number. |
| 4 | Remote object number (i32) | For the reset marker: unused. For the trailer: total number of objects sent. |
| 264 | `object_rw` | Only for normal entries, not for the reset marker or the trailer |

The trailer is sent as its own 14-byte packet (count = 1).

#### 3.2.9 `ping` (12) and `pong` (13)

`net_udp_ping_frame`, `net_udp_process_ping`, `net_udp_process_pong`
(`net_udp.cpp:6132-6210`).

| Offset | Size | `ping` |
|---|---|---|
| 0 | 1 | upid = 12 |
| 1 | 8 | Host `fix64` time (host byte order) |
| 9 | 7 × 4 | Ping in ms of players 1–7 (i32) |

| Offset | Size | `pong` |
|---|---|---|
| 0 | 1 | upid = 13 |
| 1 | 1 | Client player number |
| 2 | 8 | Echoed time stamp |

The host computes the ping as the elapsed time in ms, clamped to 0–9999.

Only the host sends `ping` (`do_protocol_frame`). The host ignores `ping`, and a
client ignores a `ping` that does not come from the host's address and answers
only the host. Only the host processes `pong`.

#### 3.2.10 `endlevel_h` (14) and `endlevel_c` (15)

`dispatch_table::send_endlevel_packet` (`net_udp.cpp:2801`),
`net_udp_read_endlevel_packet` (`net_udp.cpp:3558`).

`endlevel_h` (170 bytes):

| Offset | Size | Field |
|---|---|---|
| 0 | 1 | upid = 14 |
| 1 | 1 | `Countdown_seconds_left` |
| 2 | 8 × 5 | Per player: `connected` (1), `net_kills_total` (2), `net_killed_total` (2). The receiver skips its own entry. |
| 42 | 8 × 8 × 2 | Kill matrix. The receiver skips its own row. |

`endlevel_c` (24 bytes):

| Offset | Size | Field |
|---|---|---|
| 0 | 1 | upid = 15 |
| 1 | 1 | Sender player number (must match the sender address) |
| 2 | 1 | `connected` |
| 3 | 1 | `Countdown_seconds_left` |
| 4 | 2 | `net_kills_total` |
| 6 | 2 | `net_killed_total` |
| 8 | 8 × 2 | Sender's row of the kill matrix |

A `connected` value of `disconnected` makes the receiver call
`multi_disconnect_player`.

#### 3.2.11 `pdata` (16)

See section 4.1.

#### 3.2.12 `mdata_pnorm` (17), `mdata_pneedack` (18), `mdata_ack` (19)

See section 4.2 and 4.4.

### 3.3 Tracker packets

These layouts come from the game's code only. The tracker is maintained
separately.

| Id | Layout | Source |
|---|---|---|
| 21 register | `[21]` + ASCII `"b=" UDP_REQ_ID DXX_VERSION_STR ".<MULTI_PROTO_VERSION>,z="` (no NUL) + a complete `game_info_lite` packet (3.2.4) | `udp_tracker_register`, `net_udp.cpp:6539` |
| 22 remove | `[22]` | `udp_tracker_unregister` |
| 23 request games | `[23]` + ASCII `UDP_REQ_ID DXX_VERSION_STR ".<MULTI_PROTO_VERSION>"` (no NUL) | `udp_tracker_reqgames` |
| 24 game info | Text containing `a=<ip>/<port>`, one separator byte, `c=` + 2-byte little-endian tracker game id, and later `z=` + a `game_info_lite` packet. The receiver computes the offset of the lite packet relative to `a=` and so assumes that `a=` starts at byte 1. Accepted only from the tracker address and only in `browsing` state. | `udp_tracker_process_game`, `net_udp.cpp:6572` |
| 25 ack | `[25][kind]`: kind 0 = acknowledgement on the same socket, 1 = acknowledgement from another port (the game port is reachable from outside) | `udp_tracker_process_ack` |
| 26 hole punch | client → tracker: `[26][game id u16]`. tracker → host: `[26]` + `"<ip>/<port>"` + NUL. host → client: `[26]`. A client treats a 1-byte packet 26 as the reply. | `udp_tracker_request_holepunch`, `udp_tracker_process_holepunch` |

If no acknowledgement has arrived 10 s after registration, the host warns the
user (`udp_tracker_verify_ack_timeout`).

---

## 4. In-game data transport

### 4.1 PDATA: ship position

`net_udp_send_pdata` (`net_udp.cpp:5923`). `net_udp_process_pdata` /
`net_udp_read_pdata_packet` (`net_udp.cpp:5968`, `6025`).

| Offset | Size | Field |
|---|---|---|
| 0 | 1 | upid = 16 |
| 1 | 1 | Player number |
| 2 | 1 | `connected` (`player_connection_status`) |
| 3 | 2 | Orientation quaternion `w` (i16) |
| 5 | 2 | quaternion `x` |
| 7 | 2 | quaternion `y` |
| 9 | 2 | quaternion `z` |
| 11 | 12 | Position (`vms_vector`) |
| 23 | 2 | Segment number (u16, validated on receive) |
| 25 | 12 | Velocity |
| 37 | 12 | Rotational velocity |

This is the 46-byte `quaternionpos` encoding (`object.h:225`) built with
`build_quaternionpos` and applied with `extract_quaternionpos`.

- Rate: one packet per `F1_0 / Netgame.PacketsPerSec` (2.6). Sent only while
  the local player is `playing` and `Network_status` is `playing` or `endlevel`.
- A client sends to the host. The host sends its own `pdata` to every client
  that is not disconnected.
- The host relays a client's `pdata` unchanged to all clients except the sender
  that are not `disconnected` or `waiting`. It relays only if the sender slot is
  in `1..N_players` and `playing`.
- The receiver requires the source address to be the slot's address (host) or
  the host's address (client).
- `pdata` is never acknowledged. It is also the only packet type that keeps
  `LastPacketTime` fresh during play (2.7).

### 4.2 MDATA: game message transport

Game messages (section 5) are appended to a send buffer, `UDP_MData.mbuf`
(`UPID_MDATA_BUF_SIZE` = 454 bytes), by `dispatch_table::send_data`
(`net_udp.cpp:5337`). If the new message does not fit, the buffer is first sent
unreliably.

`net_udp_send_mdata` (`net_udp.cpp:5803`) wraps the buffer:

| Offset | Size | Field |
|---|---|---|
| 0 | 1 | upid: 17 (`mdata_pnorm`) or 18 (`mdata_pneedack`) |
| 1 | 1 | Player number of the originator |
| 2 | 4 | `pkt_num` (u32). Only in upid 18. The value is different for each recipient. |
| 2 or 6 | n ≤ 454 | Concatenated game messages |

Behaviour:

- The host sends to every `playing` client. A client sends to the host.
- On receive (`net_udp_process_mdata`, `net_udp.cpp:5858`):
  - The packet is dropped if the player number is ≥ `MAX_PLAYERS` or if the
    packet is longer than `sizeof(UDP_mdata_info)`.
  - The source address must match: on the host, the address of the slot in
    byte 1; on a client, the host address.
  - For upid 18, the sequence number is validated first (4.4).
  - The host then relays the packet to every other `playing` client. For upid
    18 it rewrites bytes 2–5 with that recipient's next `pkt_num`. Byte 1 (the
    originator) is not changed.
  - The messages are processed only in state `playing`, `endlevel` or
    `waiting`, by `multi_process_bigdata` (5.1). The player number passed to
    each handler is byte 1 of the MDATA header.

### 4.3 Priorities

`enum class multiplayer_data_priority` (`multi.h:117`) is the second argument of
`multi_send_data`.

| Priority | Effect in `send_data` |
|---|---|
| `_0` | Append only. The buffer is sent unreliably by the next 1/10 s flush in `do_protocol_frame`, or earlier together with the next priority 1 or 2 message. |
| `_1` | Append and send the buffer at once as `mdata_pnorm` (unreliable). |
| `_2` | Append and send the buffer at once as `mdata_pneedack` (reliable), if `Netgame.PacketLossPrevention` is set. Otherwise as `mdata_pnorm`. |

A flush always sends the whole buffer. Priority 0 messages that are still
waiting are therefore sent with the delivery guarantee of the message that
triggered the flush. Some code relies on this. For example, `multi_send_position`
sends the same `MULTI_POSITION` twice, first with priority 1 and then with
priority 0, so that the second copy goes out with the next reliable message
(`multi.cpp:2834`). This is used before `PLAYER_DERES`, `REAPPEAR`,
`CREATE_POWERUP` and `DROP_WEAPON`.

### 4.4 Reliable delivery ("packet loss prevention", `mdata_pneedack`)

Code: `net_udp.cpp:5541-5791`. It is enabled when `Netgame.PacketLossPrevention`
is set (default 1, `net_udp.cpp:4463`).

State:

- `UDP_mdata_queue`: up to `UDP_MDATA_STOR_QUEUE_SIZE` (1024) sent packets. Each
  entry stores the payload, the originator, a `pkt_num` and time stamp per
  recipient, a per-player acknowledgement mask, and the time of the first send.
- `UDP_mdata_trace[p]`, per peer: `pkt_num_tosend` (next number to send to
  `p`), `pkt_num_torecv` (next number expected from `p`), and a ring of the
  last 1024 received numbers for duplicate detection.
- A client keeps all of its receive state under index 0, because everything it
  receives comes from the host (`net_udp_noloss_validate_mdata`).
- Numbers start at `UDP_MDATA_PKT_NUM_MIN` (1) and wrap after
  `UDP_MDATA_PKT_NUM_MAX` (102400) (`net_udp.h:82-83`).

Sending (`net_udp_noloss_add_queue_pkt`):

- Every `mdata_pneedack` is stored in the queue.
- For each recipient that must acknowledge, the current `pkt_num_tosend` is
  recorded and then incremented.

Receiving (`net_udp_noloss_validate_mdata`, `net_udp.cpp:5556`):

- The source address must be the expected peer.
- If `pkt_num == pkt_num_torecv`, the receiver sends an ACK, stores the number
  in the ring, increments `pkt_num_torecv` and processes the packet.
- If the number is in the ring (a duplicate), the receiver sends the ACK again
  and drops the packet.
- Any other number (a gap, meaning an earlier packet was lost) is **dropped
  without an ACK**. The receiver waits for the sender to resend the missing
  packet. The protocol has no receive buffer for out-of-order packets.

`mdata_ack` (19), 7 bytes:

| Offset | Size | Field |
|---|---|---|
| 0 | 1 | upid = 19 |
| 1 | 1 | Player number of the acknowledging peer |
| 2 | 1 | Originator player number of the acknowledged packet (MDATA byte 1) |
| 3 | 4 | Acknowledged `pkt_num` |

`net_udp_noloss_got_ack` marks the queue entry whose originator and
per-recipient `pkt_num` match.

Resend and timeout (`net_udp_noloss_process_queue`, `net_udp.cpp:5651`), run
every protocol frame:

- Recipients that are not `playing`, the local player, and (on clients)
  everyone except the host are treated as acknowledged.
- Any recipient that has not acknowledged and was last sent the packet at least
  **F1_0/4 (250 ms)** ago gets it again, with its own `pkt_num` and the original
  originator in byte 1.
- Resending stops for the current frame once about `UPID_MAX_SIZE / 2` (512)
  bytes have been resent.
- An entry is removed when everyone has acknowledged it, or `UDP_TIMEOUT`
  (5 s) after the first send.
- If the timeout expires and someone has still not acknowledged:
  - The **host** kicks each missing client with `dump` `pkttimeout`.
  - A **client** turns `PacketLossPrevention` off, shows "You left the game.
    You failed sending important packets." and leaves.
- If the queue is full (1024 entries), the host kicks every client that has not
  acknowledged the oldest entry and drops that entry. A client leaves the game
  (`net_udp_noloss_add_queue_pkt`).
- The queue and all traces are reset in `net_udp_init` and at every
  `level_sync`. The trace for one player is reset when that player connects or
  disconnects.

### 4.5 Direct messages

`dispatch_table::send_data_direct` (`net_udp.cpp:5755`) sends a single message
to one player at once, without the send buffer:

- Layout: the MDATA header plus one message.
- `needack` selects upid 18 (when `PacketLossPrevention` is on) or 17.
- Clients may send direct messages to the host only.

It is used for `MULTI_KILL_CLIENT` (client → host) and for the per-player
extras sent to a joining player (`MULTI_DOOR_OPEN`, `MULTI_WALL_STATUS`,
`MULTI_LIGHT`, `MULTI_START_TRIGGER`). The `multi_send_data_direct` template
wrapper (`multi.cpp:1165`) passes `2` as `needack`.

---

## 5. Game messages (`multiplayer_command_t`)

### 5.1 Framing

- A game message is a 1-byte `multiplayer_command_t` followed by a fixed
  number of bytes. The total length comes from `command_length<C>`
  (`multiinternal.h:136`) and, on receive, from the `message_length[]` table
  (`multi.cpp:385`). Both are generated from `for_each_multiplayer_command`
  (`multiinternal.h:26`).
- There is **no length field** on the wire. Both peers must agree on the table,
  which is why `MULTI_PROTO_VERSION` must change when it changes.
- `multi_process_bigdata` (`multi.cpp:2533`) walks the payload message by
  message. It stops at the first unknown type or truncated message and ignores
  the rest of the packet.
- `multi_process_data` (`multi.cpp:5841`) dispatches each message to its
  `multi_do_*` handler with `pnum` = MDATA originator (4.2).
- Many messages also carry a player number in byte 1. Several senders mark it
  "Obsolete - reclaim player number field on next multiplayer protocol version
  bump". Unless noted otherwise below, **receivers use the MDATA originator
  and ignore byte 1**.

### 5.2 Summary

Ids are the enumerator order in `for_each_multiplayer_command`. D1X has ids
0–45 only. "Prio" is the priority used by the sender in the current code
("direct" = `send_data_direct` with ACK). "Any" means any player can send the
message.

| Id | Name | Len D2 | Len D1 | Prio | Sender | Purpose |
|---|---|---|---|---|---|---|
| 0 | `MULTI_POSITION` | 47 | 47 | 1, then a copy at 0 | any | Own ship position (before important events) |
| 1 | `MULTI_REAPPEAR` | 4 | 4 | 2 | any | Ship reappears after death |
| 2 | `MULTI_FIRE` | 17 | 17 | 1 | any | Weapon fired |
| 3 | `MULTI_FIRE_TRACK` | 20 | 20 | 1 | any | Homing weapon fired, with target |
| 4 | `MULTI_FIRE_BOMB` | 19 | 19 | 1 | any | Bomb or mine fired, with its object number |
| 5 | `MULTI_REMOVE_OBJECT` | 4 | 4 | 2 | any | Powerup or hostage picked up |
| 6 | `MULTI_MESSAGE` | 37 | 37 | 0 | any | Chat message |
| 7 | `MULTI_QUIT` | 2 | 2 | 2 | any | Leaving the game |
| 8 | `MULTI_PLAY_SOUND` | 8 | 8 | 0 | any | Play a sound on the sender's ship |
| 9 | `MULTI_CONTROLCEN` | 4 | 4 | 2 | any | Reactor destroyed |
| 10 | `MULTI_ROBOT_CLAIM` | 5 | 5 | 2 | any | Take control of a robot |
| 11 | `MULTI_CLOAK` | 2 | 2 | 2 | any | Sender cloaked |
| 12 | `MULTI_ENDLEVEL_START` | 2 | 3 | 2 | any | Sender escaped (D1: or found the secret exit) |
| 13 | `MULTI_CREATE_EXPLOSION` | 2 | 2 | 0 | any | Small explosion on the sender's ship |
| 14 | `MULTI_CONTROLCEN_FIRE` | 16 | 16 | 0 | reactor controller | Reactor fires |
| 15 | `MULTI_CREATE_POWERUP` | 19 | 19 | 2 | any | Powerup created (used items, respawn by host) |
| 16 | `MULTI_DECLOAK` | 2 | 2 | 2 | any | Sender decloaked |
| 17 | `MULTI_ROBOT_POSITION` | 51 | 51 | 0 or 1 | robot controller | Robot position |
| 18 | `MULTI_PLAYER_DERES` | 108 | 58 | 2 | any | Sender died or left: inventory and dropped objects |
| 19 | `MULTI_DOOR_OPEN` | 5 | 4 | 2 / direct | any / host | Door opened or wall blasted |
| 20 | `MULTI_ROBOT_EXPLODE` | 7 | 7 | 2 | any | Robot destroyed |
| 21 | `MULTI_ROBOT_RELEASE` | 5 | 5 | 2 | robot controller | Give up control of a robot |
| 22 | `MULTI_ROBOT_FIRE` | 18 | 18 | 1 | any | Robot fires |
| 23 | `MULTI_SCORE` | 6 | 6 | 0 | any (coop) | Sender's score |
| 24 | `MULTI_CREATE_ROBOT` | 6 | 6 | 2 | any | Robot created by a robot generator (matcen) |
| 25 | `MULTI_TRIGGER` | 3 | 3 | 2 | any | Trigger activated |
| 26 | `MULTI_BOSS_TELEPORT` | 5 | 5 | 2 | boss controller | Boss teleported |
| 27 | `MULTI_BOSS_CLOAK` | 3 | 3 | 2 | boss controller | Boss cloaked |
| 28 | `MULTI_BOSS_START_GATE` | 3 | 3 | 2 | boss controller | Boss gating effect started |
| 29 | `MULTI_BOSS_STOP_GATE` | 3 | 3 | 2 | boss controller | Boss gating effect stopped |
| 30 | `MULTI_BOSS_CREATE_ROBOT` | 8 | 8 | 2 | boss controller | Boss gated in a robot |
| 31 | `MULTI_CREATE_ROBOT_POWERUPS` | 27 | 27 | 2 | robot controller / host | Powerups dropped by a destroyed robot |
| 32 | `MULTI_HOSTAGE_DOOR` | 7 | 7 | 0 | host | Hit points of a blastable wall (rejoin) |
| 33 | `MULTI_SAVE_GAME` | 26 | 26 | 2 | host | Save the game |
| 34 | `MULTI_RESTORE_GAME` | 6 | 6 | 2 | host | Restore a saved game |
| 35 | `MULTI_HEARTBEAT` | 5 | 5 | 1 | lowest-numbered connected player | Level time (time-limited games) |
| 36 | `MULTI_KILLGOALS` | 9 | 9 | 2 | host | Kill goal counters |
| 37 | `MULTI_DO_BOUNTY` | 2 | 2 | 2 | host | Bounty target |
| 38 | `MULTI_TYPING_STATE` | 3 | 3 | 2 | any | Chat typing indicator |
| 39 | `MULTI_GMODE_UPDATE` | 3 | 3 | 0 | host | Team vector and bounty target, every 2 s |
| 40 | `MULTI_KILL_HOST` | 7 | 7 | 2 | host | A kill, with game mode data |
| 41 | `MULTI_KILL_CLIENT` | 5 | 5 | direct | client → host | Client reports its own death |
| 42 | `MULTI_RANK` | 3 | 3 | 2 | any | Sender's rank changed |
| 43 | `MULTI_DROP_WEAPON` | 10 | 10 | 2 | any | Sender dropped a primary weapon |
| 44 | `MULTI_VULWPN_AMMO_ADJ` | 6 | 6 | 2 | any | Remaining ammo in a vulcan/gauss powerup |
| 45 | `MULTI_PLAYER_INV` | 21 | 15 | 0 (periodic), 1 (extras) | any | Sender's inventory |
| 46 | `MULTI_MARKER` | 55 | – | 2 | any | Marker dropped |
| 47 | `MULTI_GUIDED` | 26 | – | 0 then flushed at once (paced position, final position before release), 1 (release, final position on removal) | any | Guided missile position or release |
| 48 | `MULTI_STOLEN_ITEMS` | 11 | – | 2 | host | Thief's stolen items (rejoin) |
| 49 | `MULTI_WALL_STATUS` | 6 | – | direct | host | Wall state (rejoin) |
| 50 | `MULTI_SEISMIC` | 5 | – | 2 | any | Earthshaker disturbance duration |
| 51 | `MULTI_LIGHT` | 16 | – | direct | host | Smashed lights of a segment (rejoin) |
| 52 | `MULTI_START_TRIGGER` | 2 | – | direct | host | Disable a trigger (rejoin) |
| 53 | `MULTI_FLAGS` | 6 | – | 2 | any; host in extras | Player powerup flags (headlight, flag, orb) |
| 54 | `MULTI_DROP_BLOB` | 2 | – | 0 | any | Afterburner blobs |
| 55 | `MULTI_SOUND_FUNCTION` | 4 | – | 2 | any | Afterburner sound start/stop |
| 56 | `MULTI_CAPTURE_BONUS` | 2 | – | 2 | any | Sender scored in CTF |
| 57 | `MULTI_GOT_FLAG` | 2 | – | 2 | any | Sender picked up a flag |
| 58 | `MULTI_DROP_FLAG` | 8 | – | 2 | any | Sender dropped a flag or an orb |
| 59 | `MULTI_FINISH_GAME` | 2 | – | 2 | any | Final boss killed |
| 60 | `MULTI_ORB_BONUS` | 3 | – | 2 | any | Sender scored in hoard |
| 61 | `MULTI_GOT_ORB` | 2 | – | 2 | any | Sender picked up an orb |
| 62 | `MULTI_EFFECT_BLOWUP` | 17 | – | 0 | any | Monitor or switch destroyed (sent before `MULTI_TRIGGER`) |
| 63 | `MULTI_UPDATE_BUDDY_STATE` | 7 | – | 2 | any | Guide-bot goal |

`DXX_MP_SIZE_BEGIN_SYNC` (`multiinternal.h:79`, `86`) is defined but not used
by any message.

### 5.3 Message layouts

Offsets include the type byte at 0. "(ignored)" means the receiver does not
read the byte.

#### Movement

**`MULTI_POSITION` (0), 47 bytes.** `multi_send_position` (`multi.cpp:2815`),
`multi_do_position` (`multi.cpp:1753`). Applies to the originator's ship.

| Offset | Size | Field |
|---|---|---|
| 1 | 8 | Quaternion w, x, y, z (i16 each) |
| 9 | 12 | Position |
| 21 | 2 | Segment (u16, validated) |
| 23 | 12 | Velocity |
| 35 | 12 | Rotational velocity |

**`MULTI_REAPPEAR` (1), 4 bytes.** `multi_send_reappear` / `multi_do_reappear`.

| Offset | Size | Field |
|---|---|---|
| 1 | 1 | Player number (ignored) |
| 2 | 2 | Sender's player object number. The receiver checks that the object is a player or ghost with id = originator. |

#### Weapons

**`MULTI_FIRE` (2) / `MULTI_FIRE_TRACK` (3) / `MULTI_FIRE_BOMB` (4).**
`multi_send_fire` (`multi.cpp:2573`), `multi_do_fire` (`multi.cpp:1659`),
dispatch at `multi.cpp:5860`.

| Offset | Size | Field |
|---|---|---|
| 1 | 1 | Player number (ignored) |
| 2 | 1 | Weapon: primary index; `MISSILE_ADJUST` (100) + secondary index; or `FLARE_ADJUST` (127) for flares |
| 3 | 1 | Laser level (primaries) |
| 4 | 1 | Flags. For lasers: `LASER_QUAD`. For fusion: charge (`Fusion_charge = flags << 12`). For secondaries: bit 0 selects the alternate gun. |
| 5 | 12 | Firing direction (`orient.fvec`) |
| 17 | 2 | `FIRE_TRACK`: target's remote object number. `FIRE_BOMB`: sender's local object number of the bomb. |
| 19 | 1 | `FIRE_TRACK` only: target's owner |

For `FIRE_BOMB` the receiver maps the created bomb to (object number, originator)
with `map_objnum_local_to_remote`. `multi_send_fire` also forces a protocol frame
(at most 20 times per second) so a `pdata` goes out before the shot.

**`MULTI_CONTROLCEN_FIRE` (14), 16 bytes.** `multi_send_controlcen_fire` /
`multi_do_controlcen_fire`.

| Offset | Size | Field |
|---|---|---|
| 1 | 12 | Direction to target |
| 13 | 1 | Gun number |
| 14 | 2 | Reactor object number (level object, not mapped) |

**`MULTI_DROP_WEAPON` (43), 10 bytes.** `multi_send_drop_weapon`
(`multi.cpp:3857`), `multi_do_drop_weapon` (`multi.cpp:3889`).

| Offset | Size | Field |
|---|---|---|
| 1 | 1 | Powerup id |
| 2 | 2 | Sender's local object number of the powerup |
| 4 | 2 | Ammo count |
| 6 | 4 | Random seed for `spit_powerup` |

The receiver creates the powerup with `spit_powerup` from its copy of the
originator's ship, using the seed, and maps it to (object number, originator).

**`MULTI_VULWPN_AMMO_ADJ` (44), 6 bytes.**

| Offset | Size | Field |
|---|---|---|
| 1 | 2 | Remote object number |
| 3 | 1 | Owner |
| 4 | 2 | New ammo count |

#### Kills and death

**`MULTI_KILL_CLIENT` (41), 5 bytes, and `MULTI_KILL_HOST` (40), 7 bytes.**
`multi_send_kill` (`multi.cpp:2842`), `multi_do_kill_client` (`multi.cpp:1934`),
`multi_do_kill_host` (`multi.cpp:1917`).

| Offset | Size | Field |
|---|---|---|
| 1 | 1 | Killed player number. `KILL_CLIENT`: not used; the host takes the sender as the killed player. `KILL_HOST`: set by the host (to the sender of a relayed `KILL_CLIENT`) and used by clients |
| 2 | 2 | Killer's remote object number (`0xffff` if none) |
| 4 | 1 | Killer's owner (`-1` if none) |
| 5 | 1 | `KILL_HOST` only: `Netgame.team_vector` |
| 6 | 1 | `KILL_HOST` only: `Bounty_target` |

Flow:

- A **client** that dies sends `KILL_CLIENT` directly to the host and does not
  count the kill yet.
- The host's `multi_do_kill_client` copies bytes 2–4 into a new `KILL_HOST`,
  sets byte 1 to the sender, adds the team vector and bounty target, broadcasts it with priority 2, and
  computes the kill locally with killed = the client.
- When the **host** dies, it computes the kill and broadcasts `KILL_HOST`
  itself.
- `multi_do_kill_host` runs only on clients and accepts the message only from
  the host. It takes the killed player from byte 1, because for a relayed kill
  the MDATA originator is the host, and applies the team vector and bounty
  target from the packet.

**`MULTI_PLAYER_DERES` (18), 108 bytes D2 / 58 bytes D1.**
`multi_send_player_deres` (`multi.cpp:2700`), `multi_do_player_deres`
(`multi.cpp:1811`).

| Offset D2 | Offset D1 | Size | Field |
|---|---|---|---|
| 1 | 1 | 1 | Player number (ignored) |
| 2 | 2 | 1 | Type: 0 `deres_explode`, 1 `deres_drop` |
| 3 | 3 | 2 (D1: 1) | `primary_weapon_flags` |
| 5 | 4 | 1 | `laser_level` |
| 6 | 5 | 1 | Hoard orbs (0 when not hoard) |
| 7 | 6 | 10 (D1: 5) | Secondary ammo: homing, concussion, smart, mega, proximity, and in D2 also flash, guided, smart mine, mercury, earthshaker |
| 17 | 11 | 2 | Vulcan ammo |
| 19 | 13 | 4 | `powerup_flags` |
| 23 | 17 | 1 | `Net_create_loc`: number of objects the sender created while dropping its eggs |
| 24 | 18 | 2 × 40 (D1: 2 × 20) | Sender's object numbers of those objects (`-1` = unused) |
| 104 | – | 4 | D2 only: not written by the sender |

The receiver sets the ship's inventory and calls `drop_player_eggs`
(deterministic, see section 7). It maps its created objects in order to the
sender's object numbers. Local objects beyond the sender's count get
`OF_SHOULD_BE_DEAD`. Then it either explodes the ship and makes it a ghost
(`deres_explode`) or shows the appearance effect (`deres_drop`).

**`MULTI_PLAYER_INV` (45), 21 bytes D2 / 15 bytes D1.**
`multi_send_player_inventory` (`multi.cpp:5239`), `multi_do_player_inventory`.

| Offset D2 | Offset D1 | Size | Field |
|---|---|---|---|
| 1 | 1 | 1 | Player number (ignored) |
| 2 | 2 | 2 (D1: 1) | `primary_weapon_flags` |
| 4 | 3 | 1 | `laser_level` |
| 5 | 4 | 10 (D1: 5) | Secondary ammo (same order as DERES) |
| 15 | 9 | 2 | Vulcan ammo |
| 17 | 11 | 4 | `powerup_flags` |

The host uses this data to count the level's powerup inventory
(`MultiLevelInv_Recount`) and respawn missing powerups.

#### Objects and powerups

**`MULTI_REMOVE_OBJECT` (5), 4 bytes.** `multi_send_remobj` / `multi_do_remobj`.

| Offset | Size | Field |
|---|---|---|
| 1 | 2 | Remote object number |
| 3 | 1 | Owner |

The receiver removes the object only if it is a powerup or a hostage.

**`MULTI_CREATE_POWERUP` (15), 19 bytes.** `multi_send_create_powerup`
(`multi.cpp:3023`), `multi_do_create_powerup` (`multi.cpp:2240`).

| Offset | Size | Field |
|---|---|---|
| 1 | 1 | Player number (ignored) |
| 2 | 1 | Powerup type |
| 3 | 2 | Segment |
| 5 | 2 | Sender's local object number |
| 7 | 12 | Position |

The receiver seeds `d_rand` with `multi_create_powerup_seed(position)`
(`pos.x ^ pos.y ^ pos.z`, `multi.cpp:266`), calls `drop_powerup` with zero
initial velocity, then reseeds from values it drew from its own sequence
beforehand. It maps the object to (object number, originator). The sender
(`maybe_drop_net_powerup`, `fireball.cpp:881-884`) seeds in the same way. The message is ignored during endlevel
or after the reactor is destroyed.

**`MULTI_CREATE_ROBOT_POWERUPS` (31), 27 bytes.**
`multi_send_create_robot_powerups` (`multibot.cpp:705`),
`multi_do_create_robot_powerups` (`multibot.cpp:1247`).

| Offset | Size | Field |
|---|---|---|
| 1 | 1 | Player number (ignored) |
| 2 | 1 | `contains.count` |
| 3 | 1 | `contains.type` |
| 4 | 1 | `contains.id` |
| 5 | 2 | Segment |
| 7 | 12 | Position |
| 19 | 4 × 2 | Sender's object numbers of the created powerups (`-1` = unused) |

#### Player state

**`MULTI_CLOAK` (11) / `MULTI_DECLOAK` (16), 2 bytes.** Byte 1: player number
(ignored).

- `CLOAK` sets the originator's cloaked flag, starts the cloak time at the
  receiver's `GameTime64` and releases the originator's robots.
- `DECLOAK` only records a demo event (`multi_do_decloak`, `multi.cpp:2162`).

**`MULTI_FLAGS` (53, D2), 6 bytes.** `multi_send_flags` (`multi.cpp:4268`),
`multi_do_flags` (`multi.cpp:4257`).

| Offset | Size | Field |
|---|---|---|
| 1 | 1 | Player number whose flags are sent (ignored by the receiver) |
| 2 | 4 | `powerup_flags` |

The receiver applies the flags to the originator's ship.

**`MULTI_RANK` (42), 3 bytes.** Byte 1: player number (ignored). Byte 2: new rank.

**`MULTI_SCORE` (23), 6 bytes.** Byte 1: player number (ignored). Bytes 2–5:
score (i32). Sent only in cooperative games.

**`MULTI_ENDLEVEL_START` (12), 2 bytes D2 / 3 bytes D1.** Byte 1: player
number. D1 byte 2: `multi_endlevel_type` (1 = secret exit).
`multi_do_escape` (`multi.cpp:1981`) uses the originator for the message and
connection state, but passes **byte 1** to `multi_make_player_ghost`.

**`MULTI_QUIT` (7), 2 bytes.** Byte 1: player number (ignored). The receiver
calls `multi_disconnect_player(originator)`.

#### Chat

**`MULTI_MESSAGE` (6), 37 bytes.** `multi_send_message` (`multi.cpp:2785`),
`multi_do_message` (`multi.cpp:1708`).

| Offset | Size | Field |
|---|---|---|
| 1 | 1 | Player number (ignored) |
| 2 | 35 | Text (`Network_message`, NUL-terminated if shorter) |

A message of the form `name: text` is shown only to the named player or team.
The address can be a callsign prefix, a team name, or a team number `1`/`2` in
team games.

**`MULTI_TYPING_STATE` (38), 3 bytes.** Byte 1: player number (ignored).
Byte 2: `msgsend_state` (0 none, 1 typing, 2 automap).

#### Level and environment

**`MULTI_DOOR_OPEN` (19), 5 bytes D2 / 4 bytes D1.** `multi_send_door_open`
(`multi.cpp:2953`), `multi_do_door_open` (`multi.cpp:2176`).

| Offset | Size | Field |
|---|---|---|
| 1 | 2 | Segment |
| 3 | 1 | Side |
| 4 | 1 | D2 only: `wall_flags`, copied to the wall |

**`MULTI_TRIGGER` (25), 3 bytes.** Byte 1: player number (ignored). Byte 2:
trigger number. The receiver rejects triggers whose originator is itself.

**`MULTI_CONTROLCEN` (9), 4 bytes.** `multi_send_destroy_controlcen`
(`multi.cpp:2625`), `multi_do_controlcen_destroy` (`multi.cpp:1963`).

| Offset | Size | Field |
|---|---|---|
| 1 | 2 | Reactor object number (level object, not mapped; `object_none` if the reactor was inside another object) |
| 3 | 1 | Player who destroyed it |

**`MULTI_HEARTBEAT` (35), 5 bytes.** Bytes 1–4: `ThisLevelTime` (fix). The
receiver overwrites its own level time, which it then keeps advancing by its
own frame time (`GameProcessFrame`), so the message only corrects drift. It is
sent with priority 1, so that the value is not up to 1/10 s old when it
arrives. The sync data does not contain the level time, so a joining player
learns it from the first heartbeat after the extras.

**`MULTI_HOSTAGE_DOOR` (32), 7 bytes.** Bytes 1–2: wall number. Bytes 3–6:
hit points (fix). The receiver damages the wall down to that value.

**`MULTI_SAVE_GAME` (33), 26 bytes / `MULTI_RESTORE_GAME` (34), 6 bytes.**
Byte 1: slot. Bytes 2–5: game id (u32). `SAVE_GAME` bytes 6–25: description (20).

D2 only:

| Message | Layout |
|---|---|
| `MULTI_WALL_STATUS` (49) | 1–2 wall number, 3 type, 4 flags, 5 state |
| `MULTI_LIGHT` (51) | 1–2 segment, 3 side mask, 4–15 six `tmap_num2` values (u16) |
| `MULTI_START_TRIGGER` (52) | 1 trigger number (the receiver sets `disabled`) |
| `MULTI_SEISMIC` (50) | 1–4 duration (fix) |
| `MULTI_EFFECT_BLOWUP` (62) | 1 player number (ignored), 2–3 segment, 4 side, 5–16 hit point |
| `MULTI_MARKER` (46) | 1 player number (ignored by the receiver), 2 marker index, 3–14 position, 15–54 text (40) |
| `MULTI_GUIDED` (47) | serialized: 1 player number (ignored), 2 release flag, 3–25 `shortpos` (`bytemat[9]`, `xo`, `yo`, `zo`, `segment`, `velx`, `vely`, `velz`, 16-bit each). The receiver warps its copy of the sender's active guided missile to the `shortpos` and lets physics move it until the next update. With the release flag set, it ignores the `shortpos` and only releases the missile (`multi_do_guided`). |
| `MULTI_STOLEN_ITEMS` (48) | 1–10 powerup ids |
| `MULTI_DROP_BLOB` (54) | 1 player number (ignored) |
| `MULTI_SOUND_FUNCTION` (55) | 1 player number (ignored), 2 function (0 = stop, 3 = start afterburner loop), 3 sound |
| `MULTI_FINISH_GAME` (59) | 1 player number (ignored) |
| `MULTI_UPDATE_BUDDY_STATE` (63) | serialized: 1 `Looking_for_marker`, 2 `Escort_special_goal`, 3–6 `Last_buddy_key` (i32) |

#### Robots

Robot control (`multibot.cpp`):

- Each player simulates at most `MAX_ROBOTS_CONTROLLED` (5) robots at a time.
- A robot becomes controlled through `multi_can_move_robot` and
  `multi_add_controlled_robot`, which sends `ROBOT_CLAIM`.
- A controlled robot is released after `ROBOT_TIMEOUT` without updates (D2 2 s,
  D1 3 s), when a more agitated robot needs the slot (after
  `MIN_CONTROL_TIME`: D2 1 s, D1 2 s), or on death or disconnect.
- Conflicting claims are resolved with `MULTI_ROBOT_PRIORITY(objnum, pnum) =
  ((objnum % 4) + pnum) % N_players` (`multibot.cpp:103`).
- Pending positions and fire events are sent in `multi_send_robot_frame`, every
  1/10 s.

| Message | Layout | Notes |
|---|---|---|
| `MULTI_ROBOT_CLAIM` (10), 5 | serialized: 1 player number, 2 owner (i8), 3–4 remote object number | Note the field order differs from `RELEASE` |
| `MULTI_ROBOT_RELEASE` (21), 5 | 1 player number, 2–3 remote object number, 4 owner | Accepted only from the current controller |
| `MULTI_ROBOT_POSITION` (17), 51 | 1 player number, 2–3 remote object number, 4 owner, 5–50 quaternionpos (as `MULTI_POSITION` bytes 1–46) | A position from a non-owner is accepted after the robot's `REMOTE_SLOT_NUM` counts up to `MAX_ROBOTS_CONTROLLED` ("claim packet must have gotten lost") |
| `MULTI_ROBOT_FIRE` (22), 18 | 1 player number, 2–3 remote object number, 4 owner, 5 gun number, 6–17 vector | For proximity and smart-mine gun numbers the vector is added to the robot position |
| `MULTI_ROBOT_EXPLODE` (20), 7 | serialized: 1–2 killer remote object number (i16), 3–4 robot remote object number (i16), 5 killer owner, 6 robot owner | No sender player number |
| `MULTI_CREATE_ROBOT` (24), 6 | 1 player number (ignored), 2 robot generator station number, 3–4 sender's object number, 5 robot type | Mapped to (object number, originator) |
| `MULTI_BOSS_TELEPORT` (26), 5 | serialized: 1–2 boss object number, 3–4 segment | Boss object numbers are level objects and are not mapped |
| `MULTI_BOSS_CLOAK` / `START_GATE` / `STOP_GATE` (27–29), 3 | serialized: 1–2 boss object number | |
| `MULTI_BOSS_CREATE_ROBOT` (30), 8 | serialized: 1–2 boss object number, 3–4 sender's object number of the new robot, 5–6 segment, 7 robot type | Mapped to (object number, originator) |

#### Game mode specific

| Message | Layout | Notes |
|---|---|---|
| `MULTI_KILLGOALS` (36), 9 | 1–8 `KillGoalCount` for each player (1 byte) | |
| `MULTI_DO_BOUNTY` (37), 2 | 1 new bounty target | Host only; ignored by the host |
| `MULTI_GMODE_UPDATE` (39), 3 | 1 `team_vector`, 2 `Bounty_target` | Host only, every 2 s in team or bounty games. A new team vector re-colors ships. |
| `MULTI_CAPTURE_BONUS` (56, D2), 2 | 1 player number (ignored) | +5 team kills and kills for the originator |
| `MULTI_GOT_FLAG` (57, D2), 2 | 1 player number (ignored) | Sets `has_team_flag` on the originator. Followed by `MULTI_FLAGS`. |
| `MULTI_DROP_FLAG` (58, D2), 8 | 1 powerup id, 2–3 sender's object number, 4–7 seed. (Before the fix in section 8, item 5, receivers read the seed at 6–9.) | Used for CTF flags and hoard orbs; `spit_powerup` with the seed |
| `MULTI_ORB_BONUS` (60, D2), 3 | 1 player number (ignored), 2 orb count | Bonus `n(n+1)/2` |
| `MULTI_GOT_ORB` (61, D2), 2 | 1 player number (ignored) | |

---

## 6. Object numbers across machines

Each machine allocates objects independently, so the same logical object can
have different local numbers on different machines. On the wire, an object is
identified by the pair **(owner, remote object number)**
(`struct owned_remote_objnum`, `multi.h:441`):

- `owner == -1` (`owner_none`, `multi.cpp:95`): an object that existed when the
  level was loaded (robots, reactor, level powerups, and so on). These have the
  same number on every machine, and the remote number equals the local number.
- `owner == p`: an object that player `p` created. The remote number is the
  object number on `p`'s machine.

Tables (`multi.cpp:160-161`, `object_owner` at `multi.cpp:314`):

- `object_owner[local]`: owner of each local object (`-1` for level objects).
- `local_to_remote[local]`: remote number of each local object.
- `remote_to_local[owner][remote]`: the reverse mapping.

Functions (`multi.cpp:481-553`):

| Function | Purpose |
|---|---|
| `objnum_local_to_remote(local)` | Returns `{owner, remote}`. For `owner_none` returns `{-1, local}`. For objects above `Highest_object_index` returns `{-1, 0xffff}`. Throws on an illegal owner or a missing mapping. |
| `objnum_remote_to_local(remote, owner)` | `owner == -1`: returns `remote`. Owner out of range: returns `remote` (with `Int3`). `remote >= MAX_OBJECTS`: returns `object_none`. Otherwise looks up `remote_to_local`. |
| `map_objnum_local_to_remote(local, remote, owner)` | Records an object created on behalf of another player |
| `map_objnum_local_to_local(local)` | Records an object the local player created: owner = `Player_num`, remote = local |
| `reset_network_objects()` | Clears all mappings |

Typical pattern:

1. The creator calls `map_objnum_local_to_local(obj)` and sends its own object
   number. Examples: `multi_send_create_powerup`, `multi_send_drop_weapon`,
   `multi_send_fire` for bombs, `multi_send_create_robot`,
   `multi_send_player_deres` for eggs.
2. Each receiver creates its own object and calls
   `map_objnum_local_to_remote(new_local, sender_objnum, originator)`.
3. Later references (`REMOVE_OBJECT`, `VULWPN_AMMO_ADJ`, `FIRE_TRACK`, robot
   messages, kill messages) send `objnum_local_to_remote(...)` and the receiver
   resolves them with `objnum_remote_to_local(...)`.

Objects created by the same deterministic code on every machine are matched by
creation order. `drop_player_eggs` and robot egg creation fill
`Net_create_objnums[]` (`fireball.cpp`, `drop_powerup`). The sender sends that
list. Receivers pair it index by index with their own `Net_create_objnums`
(`multi_do_player_deres`, `multi_do_create_robot_powerups`).

For mid-game joins, the owner and remote number of each object are sent in
`object_data` and restored with the same functions (2.5.2).

---

## 7. Determinism

Several events are not transmitted as results. Each peer recomputes them
locally and relies on identical pseudo-random numbers.

`d_rand` is a global linear congruential generator:
`seed = seed * 0x41c64e6d + 0x3039`, returning `(seed >> 16) & 0x7fff`
(`common/maths/rand.cpp:33-43`). If `NO_WATCOM_RAND` is defined it uses the C
library `rand()` instead (`rand.cpp:19`), whose sequence can differ between
platforms. Because the generator is global, peers stay in step only if they
seed right before use and make the same calls afterwards.

| Event | Seed | Where |
|---|---|---|
| Player eggs (inventory dropped on death or quit) | `d_srand(5483)` in every multiplayer game, then `drop_player_eggs`. Receivers run the same function on `PLAYER_DERES` with the inventory from the packet. | `collide.cpp:1994`, `multi_do_player_deres` |
| Robot contents dropped on death | `d_srand(1245)` before `object_create_robot_egg`, both on the sender and on receivers of `CREATE_ROBOT_POWERUPS` | `multibot.cpp:1319`, `1340` (sender), `1271` (receiver) |
| Whether a robot drops random contents, and how many | `d_srand(timer_query())` on the controlling machine; the result is transmitted in `CREATE_ROBOT_POWERUPS` | `multibot.cpp:1329` |
| Dropped weapons, flags and orbs (`spit_powerup`) | Seed chosen by the sender and sent in `DROP_WEAPON` / `DROP_FLAG` | `weapon.cpp:1500` |
| Powerups created with `MULTI_CREATE_POWERUP` (host respawns, used items) | `multi_create_powerup_seed(pos)` = `pos.x ^ pos.y ^ pos.z`, derived from the position in the packet, on the sender and on receivers. Each machine then reseeds from its own sequence. | `multi.cpp:266`, `fireball.cpp:881-884`, `multi_do_create_powerup` |
| Smart missile children | `d_srand(8321)` in multiplayer. The target list is built from the local object array. | `laser.cpp:2144` |
| Powerup shuffle at level start (anarchy) | `Netgame.ShufflePowerupSeed` from game info, used with `std::minstd_rand` and `std::uniform_int_distribution` | `powerup_shuffle_state::shuffle`, `multi.cpp:3723` |

Notes:

- `std::uniform_int_distribution` is implementation-defined in the C++
  standard. Different standard libraries (for example libstdc++ and libc++) can
  produce different shuffles from the same seed.
- For robot powerups, the sender passes the robot's velocity as the initial
  velocity (`object_create_robot_egg(Robot_info, del_obj)`, `fireball.cpp:1336`).
  The receiver passes a zero vector (`multi_do_create_robot_powerups`). The
  velocity randomization in `drop_powerup` scales with the initial velocity
  magnitude (`fireball.cpp:1100`, `1140`), so resulting velocities can differ
  even with the same seed.
- For `MULTI_CREATE_POWERUP` the velocity is not transmitted. It is
  reproduced from the position-derived seed. Both sides pass a zero initial
  velocity.
- Player eggs are created at the receiver's copy of the dead ship's position
  and velocity. `multi_send_player_deres` sends `MULTI_POSITION` first so this
  copy is as fresh as possible (4.3).
- **Positions are not resynchronized afterwards.** Only player ships (`pdata`,
  `MULTI_POSITION`) and controlled robots (`MULTI_ROBOT_POSITION`) have
  position updates. The positions of powerups and other objects are sent only
  once, in `object_data`, to a player who joins mid-game. Any divergence in
  their movement stays until they are picked up or removed.

---

## 8. Known issues

These are protocol-level limitations and inconsistencies that can be confirmed
by reading the current code.

1. **Reliable delivery is strictly in order with a fixed resend interval.** A
   `mdata_pneedack` packet that arrives after a gap is dropped without an ACK
   (`net_udp_noloss_validate_mdata`), so one lost packet delays everything after
   it by at least one resend. The resend interval is a fixed 250 ms, and resends
   are capped at about 512 bytes per frame (`net_udp_noloss_process_queue`).
   After 5 s without an acknowledgement, the host kicks the client (`pkttimeout`)
   or the client leaves the game.

2. **Many messages are not reliable.** Priority 0 and 1 messages are sent as
   `mdata_pnorm`. They are delivered reliably only if a priority 2 message
   happens to flush them first (4.3). Examples: `MULTI_FIRE`,
   `MULTI_ROBOT_FIRE`, `MULTI_ROBOT_POSITION`, `MULTI_PLAY_SOUND`,
   `MULTI_CREATE_EXPLOSION`, `MULTI_CONTROLCEN_FIRE`, `MULTI_MESSAGE` (chat),
   `MULTI_SCORE`, `MULTI_HEARTBEAT`, `MULTI_GUIDED`, `MULTI_DROP_BLOB`,
   `MULTI_GMODE_UPDATE`, and `MULTI_HOSTAGE_DOOR`, which is also used for the
   rejoin door state (`net_udp_send_door_updates`). With
   `PacketLossPrevention` off, every message is unreliable.

3. **Object sync for mid-game joins has no retransmission.** `object_data`
   packets are sent once (`net_udp_send_objects`). After the first packet the
   client stops resending its `request` (`Network_rejoined`,
   `net_udp_sync_poll`). `net_udp_verify_objects` accepts up to 10 missing
   objects without an error. If `obj_allocate` fails on the receiver, `loc` is
   not advanced past the `object_rw`, so the rest of that packet is parsed at
   the wrong offset (`net_udp_read_object_packet`, `net_udp.cpp:2538-2549`).
   Only the `sync` packet is resent, every second, until the new player's first
   `pdata` arrives.

4. **Some host-sent messages lose the player they are about.** Game message
   handlers receive the MDATA originator as `pnum`, and these handlers ignore
   the player number inside the message:
   - Fixed: `MULTI_KILL_HOST` relayed by the host for a client's death
     (`multi_do_kill_client`). Clients used the sender, which is the host, as
     the killed player, so every client death appeared as the host dying.
     `multi_do_kill_host` now accepts the message only from the host and takes
     the killed player from byte 1.
   - `MULTI_FLAGS` sent by the host for every player during rejoin extras
     (`net_udp_send_player_flags` → `multi_send_flags(i)`): receivers apply all
     of them to the host's ship (`multi_do_flags`, `multi.cpp:4257`).
   - `MULTI_MARKER` sent by the host for every player's markers during rejoin
     extras (`multi_send_markers`): `multi_do_drop_marker` uses `pnum` = host
     for the marker slot (`multi.cpp:2375`).

5. **Fixed: `MULTI_DROP_FLAG` send and receive layouts disagreed.**
   `multi_send_drop_flag` writes the seed at offset 4. `multi_do_drop_flag`
   used to read it at offset 6, which covered the last 2 bytes of the message
   and 2 bytes beyond its 8-byte length. Receivers therefore used a different
   seed in `spit_powerup` than the sender, and a dropped flag or orb moved
   differently on each machine. The receiver now reads offset 4. The wire
   format is unchanged, so this also works with unpatched senders.

6. **The D1/D2 request identifier is not checked.** The overload that compares
   `UDP_REQ_ID` (`net_udp.cpp:2631`) has no callers. `net_udp_process_packet`
   calls the version-only overloads (1.5). `game_info_lite` replies carry no
   identifier at all.

7. **Limited input validation.**
   - `game_info` and `sync` are only checked against a maximum length. A client
     accepts `game_info` from any address and stores that address as the host
     address (`net_udp_process_game_info_heavy`, `net_udp.cpp:3163`).
   - `endlevel_h` / `endlevel_c` have no length check. On the host, the player
     number in `endlevel_c` byte 1 is used as an array index for the address
     comparison, without a range check (`net_udp_read_endlevel_packet`).
   - `mdata_ack` is not checked against the sender address, and its player
     number fields are not range-checked (`net_udp_noloss_got_ack`).
   - `net_udp_process_mdata` has no minimum length check.

   Because all reads are from the 1024-byte receive buffer, short packets lead
   to stale data being read rather than reads past the buffer in these cases.

8. **Quitting is not confirmed.** `MULTI_QUIT` is priority 2. However,
   `dispatch_table::leave_game` runs one protocol frame and then closes the
   sockets, so the message is sent once and never resent. If it is lost, the
   other players notice the departure only through the 5 s timeout.

9. **Unwritten bytes.** In D2, `MULTI_PLAYER_DERES` is 108 bytes, but
   `multi_send_player_deres` writes only bytes 0–103. `multi_command` sets only
   the type byte in its constructor (`multiinternal.h:144-150`), so bytes
   104–107 are not initialized by the protocol code.

10. **The protocol has no host migration.** Every client leaves when the host
    leaves or times out (`multi_disconnect_player`).
