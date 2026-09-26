# Fork changelog: pull requests of the original nburri/dxx-rebirth repository

The fork was recreated as a standalone repository on 2026-09-26 to rewrite the commit author identity. This file preserves the descriptions and status comments of the pull requests #1–#24 from the original repository, which were used as the log of individual changes.

## #1: Build a Windows package for every pull request

*Status: merged; base `master`; branch `ci-windows-pr-builds`*

### Summary
The existing CI (`ci.yml`) only runs on pushes to `master`, so a change could not be tried in a Windows build before merging.

This adds `.github/workflows/pull-request-windows.yml`. It runs the existing `package-windows.yml` (MSYS2 / MinGW64 / SCons) on every pull request against `master`. It can also be started by hand from the Actions tab.

### Getting the binary
Open the pull request's **Checks** tab, choose the **Pull Request - Windows** run, and download the **DXX-Rebirth-Windows-x86_64** artifact. It contains `D2X-Rebirth` (and `D1X-Rebirth`) with the required DLLs.

### Notes
- Actions must be enabled on the fork (Actions tab → enable workflows) for this to run.
- Existing workflows are unchanged: `ci.yml` still packages all platforms on pushes to `master`, and `release.yml` still publishes zips for `v*.*` tags.

## #2: Sync random velocity of network-respawned powerups

*Status: merged; base `master`; branch `fix-net-powerup-position`*

### Problem
In network deathmatch, powerups that are respawned (used cloaks and invulnerability, consumed missiles and ammo, and so on) end up at a **different position on each player's machine**.

`maybe_drop_net_powerup` (`fireball.cpp`) creates the powerup with `drop_powerup` and sends `MULTI_CREATE_POWERUP` with its type, segment and start position. `drop_powerup` then gives the powerup a random velocity from `d_rand()` of up to ±32 units/s per axis in network mode. The receiver (`multi_do_create_powerup`, `multi.cpp`) calls `drop_powerup` with the same position, but `d_rand()` is not seeded on either side. Each client therefore launches the powerup in a different direction, and it bounces to a different resting place. Nothing re-syncs powerup positions afterwards.

The other drop paths already avoid this: robot drops seed with `d_srand(1245L)`, player-death drops with `d_srand(5483L)`, and spat weapons send a seed in the packet.

### Fix
- New helper `multi_create_powerup_seed(pos)` derives a seed from the powerup position, which both sides already know.
- The sender and the receiver both call `d_srand()` with it right before `drop_powerup`, so both get the same velocity.
- Both sides draw a resume seed from their own random sequence before seeding, and reseed from it after the drop. This keeps each client's other randomness independent, and several respawns in the same frame no longer stack on one spot. (The first version reseeded from `timer_query()`, which only changes once per frame. The code review caught that.)

The packet format is unchanged. **All players need a build with this fix**; unpatched clients behave exactly as before.

### Testing
- [x] D2X-Rebirth builds on Linux (g++ 15, the project's `-Werror` flags) with no new warnings.
- [ ] Network game with two patched clients: use a cloak/invulnerability or fire missiles, then confirm the respawned powerup appears in the same place on both screens.

## #3: Let only the host replace used items in network games

*Status: merged; base `master`; branch `net-host-only-respawn`*

### Problem
Large missiles in particular were sometimes duplicated in network deathmatch.

A client that uses an item spawns a replacement itself, via `maybe_drop_net_powerup(..., adjust_cap=1, ...)`:
- firing any missile except concussion (`laser.cpp`),
- using vulcan ammo (`laser.cpp`),
- cloak or invulnerability running out (`game.cpp`),
- returning a CTF flag (`fuelcen.cpp`).

Separately, the host respawns every item that has been missing for about 2 seconds (`MultiLevelInv_Repopulate`, which runs every 0.5 s).

The client announces its replacement with `MULTI_CREATE_POWERUP` over the reliable channel. That channel only delivers packets in order, so a single lost packet stalls it until the resend. If the announcement reaches the host more than about 2 seconds after the host learned the item was used (through the frequent `MULTI_PLAYER_INV` updates), both machines spawn a replacement and the item exists twice.

### Fix
`maybe_drop_net_powerup` returns early on clients for these respawns, so the host is the only machine that replaces items. The host's own immediate respawns and `MultiLevelInv_Repopulate` are unchanged.

### Effects
- When a client uses an item, its replacement now appears within about 2 seconds, placed by the host, instead of immediately.
- No protocol change. Mixed games work: unpatched clients still spawn their own replacements, as before, and an unpatched host still repopulates.

### Testing
- [x] D1X and D2X build on Linux (g++ 15, `-Werror` flags).
- [ ] Network game: a client fires a mega or smart missile, and the replacement appears once, within about 2 seconds, on all machines.

## #4: Keep granted items in the death packet so receivers drop the same items

*Status: merged; base `master`; branch `net-deres-grant-counts`*

### Problem
When the host enables "spawn with" lasers or vulcan ammo, a dead player's dropped items end up out of sync between machines. Items then appear for one player only, picking something up removes a different item elsewhere, and the item that was picked up can be picked up a second time.

`drop_player_eggs` (`collide.cpp`) subtracts the granted laser levels and vulcan ammo from `player_info` **in place**:
1. The dying player's machine calls `drop_player_eggs`, then `multi_send_player_deres`. The packet therefore carries the already reduced values.
2. Each receiver (`multi_do_player_deres`) writes those values to the ship and calls `drop_player_eggs` again, which subtracts the grant a second time.

Example: with a laser grant of 3 and a player at laser level 4, the dying player's machine drops one laser powerup and every other machine drops none.

Receivers pair their drops with the remote object numbers by position (`map_objnum_local_to_remote(Net_create_objnums[i], ...)`). Lasers are dropped first, so every later drop (weapons, missiles, shield, energy) was paired with the wrong object.

### Fix
`drop_player_eggs` still subtracts the grant to decide what to drop, but restores `laser_level` and `vulcan_ammo` at the end. The death packet then carries the real values, and every machine subtracts the grant exactly once. The granted flags are still cleared in place; clearing them twice has no further effect.

- Sender-side fix, no protocol change. Unpatched receivers also drop the right items once the dying player runs the fix.
- The dead ship's `player_info` is not used for anything else before respawn, which resets it. The level inventory count skips ghost ships.

### Testing
- [x] D1X and D2X build on Linux (g++ 15, `-Werror` flags).
- [ ] Network game with a laser grant: a player with more lasers than the grant dies, and the same number of laser powerups appears on every machine. Picking them up removes them everywhere.

## #5: Let the host decide who picks up limited powerups

*Status: open; base `master`; branch `net-host-decided-pickups`*

### Problem
Pickups were decided on each machine. The only guard against two players taking the same powerup was the "is another player closer?" check in `do_powerup`, and it uses other players' positions as last received, which lag behind. Two players reaching a powerup at about the same time could both see themselves as closer, and both collected it. The later `MULTI_REMOVE_OBJECT` found the object already gone and did nothing, so the item was duplicated. This hit contested items like mega, smart and earthshaker missiles hardest.

### Approach (chosen together with the repo owner)
The host decides who gets limited powerups.

```
Client touches a mega missile
  -> MULTI_PICKUP_REQUEST  (client -> host)
Host: powerup still there and not reserved by someone else?
  yes -> reserve it for this player for 5 s, MULTI_PICKUP_REPLY granted=1
  no  ->                                     MULTI_PICKUP_REPLY granted=0
Client, when granted within 2 s of asking: do_powerup()
  used     -> MULTI_REMOVE_OBJECT, as before (removes it everywhere)
  not used -> MULTI_PICKUP_RELEASE (for example, missiles already at maximum)
```

- **Which powerups:** everything except shields, energy, keys, the player's own team flag (and extra lives, which become invulnerability in multiplayer anyway). Shields and energy stay instant. Clients do not ask for a missile or mine they cannot carry more of.
- **Host:** still picks up instantly, but not while a client holds the reservation for that powerup. The "closer player" check still applies to the host's instant pickups; it is skipped for a pickup the host granted.
- **Client throttling:** at most one outstanding request per powerup. It asks again after 2 s without an answer, 1 s after a denial or after taking ammo from a vulcan/gauss cannon, and 5 s after any other grant it could not use.
- **Grant handling:** requests carry a sequence number that the reply repeats. A client uses only the reply to its latest request, only within 2 s of sending it, and not during the exit sequence. A ship that flew through the powerup still gets it. The host starts or extends the 5 s reservation when it receives the request, so at least 3 s remain for the `MULTI_REMOVE_OBJECT` to arrive. The host answers every request, including repeated ones, so a lost reply only delays the pickup (this also matters with packet loss prevention off). A client never sends a release for a powerup that is already gone, so a release cannot overtake its own removal. The release names the grant it gives back, so it cannot end a reservation that a newer request renewed. It travels through the same ordered buffer as `MULTI_VULWPN_AMMO_ADJ`, so the host learns about ammo taken from a cannon before it grants the cannon again. The host denies requests from players who are not playing or not alive, and requests for a different level.
- **Reservations:** matched by object signature, so a reused object slot does not inherit an old reservation. A request must map back to the same object number and owner, so a stale request cannot match a new powerup in the same slot. Reservations are cleared at level start (`reset_network_objects`).
- **Object identification:** the same (object number, owner) pair as `MULTI_REMOVE_OBJECT`, which resolves to the same object on every machine.

### Wire format
New commands, appended after the existing ones so existing command ids do not change:

| Command | Size | Direction | Layout |
|---|---|---|---|
| `MULTI_PICKUP_REQUEST` | 6 | client → host (direct) | `[0]` cmd, `[1..2]` object number (LE16), `[3]` owner, `[4]` sequence number, `[5]` level number |
| `MULTI_PICKUP_REPLY` | 7 | host → requester (direct) | `[0]` cmd, `[1..5]` copied from the request, `[6]` granted (0/1) |
| `MULTI_PICKUP_RELEASE` | 5 | client → host (message buffer, priority 2) | `[0]` cmd, `[1..2]` object number, `[3]` owner, `[4]` sequence number of the grant |

`MULTI_PROTO_VERSION` is raised from 16 to 17. Older builds get the existing "Version mismatch" message instead of joining a game with a different pickup protocol. **Every player must update.**

### Trade-offs
- For clients, weapon, missile and accessory pickups are delayed by one round trip to the host (their ping). The host has no delay.
- If a granted player's `MULTI_REMOVE_OBJECT` takes more than about 3 s to reach the host (very heavy packet loss; the reliable layer disconnects after 5 s), the reservation expires and a second player could be granted the same powerup. This is much rarer than before.

Later commits address the findings of three code reviews and update `Documentation/network-protocol.md`. Like every client message, the host also relays pickup requests and releases to the other clients, which ignore them; they are 5–6 bytes each.

### Testing
- [x] D1X and D2X build on Linux (g++ 15, `-Werror` flags).
- [ ] Two clients fly into the same missile at the same time: exactly one gets it.
- [ ] A client with full missiles touches a missile: it stays, and another player can take it right away.
- [ ] Shields and energy are still collected instantly.

### Comment (2026-09-26)

**Status: on hold, not merged.**

A fourth code review found remaining gaps:
- A stale request can still reserve a new powerup that reused the object slot of a player-owned powerup (slots are reused last-in, first-out).
- A release and the `MULTI_VULWPN_AMMO_ADJ` sent before it are separate reliable packets, so a resend can deliver the release first.
- A client with a round trip over 2 s keeps renewing a reservation it can never use.
- Some smaller issues (5 s back-off after a temporary failure, missing "already have" HUD message for full secondaries on clients).

Decision with the repo owner: first play-test release `v0.61-nb1`, which has the other network fixes (#2, #3, #4, #7, #8). If missiles are still duplicated, continue here. Likely next steps: an object id that is the same on every machine, and sending the release together with the ammo update.

## #6: Document the UDP multiplayer protocol

*Status: merged; base `master`; branch `docs-network-protocol`*

### Summary
Adds `Documentation/network-protocol.md`, a reference for the UDP multiplayer protocol of D2X-Rebirth, noting D1X differences. Every statement is derived from the code and cites a function or `file:line`.

### Contents
1. **Overview:** host-relayed star topology, port 42424, discovery, IPv4/IPv6, tracker, version checks (`MULTI_PROTO_VERSION`), little-endian encoding, size limits.
2. **Session lifecycle:** game list, connecting, level start, mid-game join (admission, object transfer, rejoin sync), timeouts and kicks, level end, leaving.
3. **UDP packet types:** a summary table of all `upid` values, plus byte layouts.
4. **In-game transport:** position packets, the message buffer, priorities, the reliable delivery layer (sequence numbers, ACKs, 250 ms resend, 5 s timeout), direct messages.
5. **Game messages:** all `MULTI_*` messages with id, length, priority and purpose, plus byte layouts for the important ones.
6. **Object number mapping** between machines.
7. **Determinism:** fixed and transmitted random seeds, and the fact that object positions are never resynced.
8. **Known issues:** 10 protocol-level issues confirmed in the code.

The document matches `master` after #2. Later protocol changes (for example the pickup messages in #5) will update it in their own pull requests.

Documentation only, no code changes.

## #7: Read the MULTI_DROP_FLAG seed where the sender writes it

*Status: merged; base `master`; branch `net-drop-flag-seed`*

### Problem
Dropped CTF flags and hoard orbs land in a different place on each machine.

`multi_send_drop_flag` writes the seed for `spit_powerup` at offset 4, directly after the object number. `multi_do_drop_flag` read it at offset 6, copying `MULTI_DROP_WEAPON`, which has a 2-byte ammo field before its seed. That read covered the last 2 bytes of the 8-byte message and 2 bytes beyond it. Receivers therefore seeded `spit_powerup` with a different value than the sender, and the flag or orb was spat in a different direction on each machine.

Found while writing the protocol documentation (#6, known issue 5).

### Fix
- Read the seed at offset 4.
- Update `Documentation/network-protocol.md` to match.

Receiver-only; the wire format is unchanged, so it also works with unpatched senders.

### Testing
- [x] D1X and D2X build on Linux (g++ 15, `-Werror` flags).
- [ ] CTF: the flag carrier drops the flag (or dies), and the flag lands in the same place on every machine.

## #8: Let IPv6 sockets also carry IPv4 traffic on Windows

*Status: merged; base `master`; branch `net-windows-dual-stack`*

### Problem
With the Windows build from CI, joining a game by IP address fails after 10 seconds with:

```
No response by host.
Possible reasons:
* No game on ::ffff:a.b.c.d (anymore)
...
```

Builds with IPv6 support (the default, `ipv6=1`, which the CI uses) open a single `AF_INET6` socket (`udp_open_socket`). `udp_dns_filladdr` resolves IPv4 hosts to IPv4-mapped IPv6 addresses (`AI_V4MAPPED`, hence `::ffff:`), which only works if the socket also carries IPv4 traffic. Linux does this by default (`IPV6_V6ONLY=0`), but **Windows defaults to `IPV6_V6ONLY=1`**. On Windows, packets to IPv4 hosts were therefore never delivered, and a Windows host could not receive packets from IPv4 clients either.

### Fix
Set `IPV6_V6ONLY` to 0 on the socket before `bind`, in IPv6 builds only. No protocol change.

### Testing
- [x] D1X and D2X build on Linux (g++ 15, `-Werror` flags).
- [ ] The Windows CI build compiles (it exercises the `_WIN32` branch).
- [ ] Windows: join a game by IPv4 address, and host a game that an IPv4 client joins.

## #9: Credit kills relayed by the host to the player who died

*Status: merged; base `master`; branch `net-kill-host-victim`*

### Problem
Reported from a deathmatch test of `v0.61-nb1`: when the host kills a player, every client shows that the host killed himself. Only the host counts the kill correctly.

1. When a client dies, it sends `MULTI_KILL_CLIENT` to the host (`multi_send_kill`).
2. The host counts the kill and relays it to everyone as `MULTI_KILL_HOST`, copying the dead player's number into byte 1 (`multi_do_kill_client`).
3. Since upstream commit `5f1877a4d` (2022, "multi_do_kill_*: use player number from network layer"), `multi_do_kill_host` ignores byte 1 and uses the sender of the message as the dead player. For a relayed kill, the sender is always the host.

So on every client, each client death counted as the host dying:
- a kill by the host showed as the host killing himself,
- a kill by another client was credited as that client killing the host,
- the dead player never saw "You were killed by…",
- kill and death counts diverged between machines, and the final score table was wrong.

This is an upstream regression, not caused by the earlier PRs here.

### Fix
`multi_do_kill_host` accepts the message only from the host, and takes the dead player from byte 1 (range-checked against `N_players`). The host fills that byte both for its own deaths and for relayed kills, so the wire format is unchanged. Updated the "obsolete" comment in `multi_send_kill` and the protocol documentation.

The host now also writes the sender's number into byte 1 of the relayed message, instead of copying what the client wrote. That way the host and the clients always count the same death (review finding).

Every player needs the new build: an older client in the same game still shows relayed kills as the host dying, as before. `MULTI_PROTO_VERSION` is not raised, so builds with the standard protocol can still play together.

### Testing
- [x] D1X and D2X build on Linux (g++ 15, `-Werror` flags).
- [ ] Deathmatch with a host and two clients: host kills client A, client B kills client A, and client A dies by suicide. Kill messages and scores match on all three machines.

## #10: Show the git tag and commit in the version of CI builds

*Status: merged; base `master`; branch `ci-version-with-commit`*

### Summary
The main menu and the window title show `D2X-Rebirth <extra_version>`. SConstruct fills `extra_version` from `git describe --tags --abbrev=12`, but `actions/checkout` fetches only the last commit without tags, so CI builds showed just `v0.61`.

- Fetch the full history with tags (`fetch-depth: 0`) in the Windows, Linux and macOS package workflows.
- Windows: mark the checkout as a safe directory for the MSYS2 git. It is owned by a different user than the MSYS2 shell, and git refuses to read it otherwise.

Result:
- a build between releases shows e.g. `D2X-Rebirth v0.61-nb1-3-g95b4859ba123` (last tag, commits since, commit hash; for pull request builds this is GitHub's test merge commit),
- a tagged release shows e.g. `D2X-Rebirth v0.61-nb2`.

No game code changes.

### Testing
- [ ] The Windows artifact of this pull request contains a `v0.61-nb1-…-g…` version string.

### Comment (2026-09-26)

Review note: a release shows its own tag only when it is started by pushing the tag (`git push origin vX.Y-…`), which is how releases are made here. If `release.yml` is started manually with a tag that does not exist yet, the packages are built before the tag is created and show the previous tag plus commit hash instead.

## #11: Initialize game controllers before joysticks so hat presses are not doubled

*Status: merged; base `master`; branch `input-hat-menu-double-step`*

### Problem
Reported by the repo owner: in the menus, each press of the joystick hat (coolie hat) moves the selection by two entries instead of one.

`arch_init` (`similar/arch/sdl/init.cpp`) called `joy_init()` before `gamecontroller_init()`:
- `joy_init()` skips devices for which `SDL_IsGameController()` is true (`common/arch/sdl/joy.cpp`), and opens the rest as plain joysticks.
- The mappings from `gamecontrollerdb.txt`, which the Windows package ships, are only loaded inside `gamecontroller_init()` (`gc_load_controller_db`).

A device that SDL recognizes as a game controller only through that file was therefore opened by **both** layers. SDL then delivers every hat press twice:
- as `SDL_JOYHATMOTION`, which `joy_hat_handler` maps through `joy_key_map` to KEY_UP/KEY_DOWN,
- and as `SDL_CONTROLLERBUTTONDOWN` for the D-pad, which `gc_button_handler` maps through `gc_key_map` to KEY_UP/KEY_DOWN.

The menu handles each of them, so the selection moves twice. (Press/release and centering are not the cause: only button-down events are translated to menu keys.)

### Fix
On SDL2, initialize the game controller layer before the joystick layer, so that the database is loaded when `joy_init()` asks `SDL_IsGameController()`. Each physical device is then handled by exactly one layer. The SDL1 path is unchanged.

### Effects
- Plain joysticks and flight sticks without a controller mapping still use the joystick layer, including hat bindings in the controls configuration.
- A device that is recognized through `gamecontrollerdb.txt` is now only a game controller. Bindings made for it as a joystick (e.g. "J1 H1↑") must be rebound with the controller names.
- If other joysticks are connected at the same time, their saved bindings can shift too: the joystick layer numbers the buttons, hats and axes of all its joysticks in one list, and the skipped device no longer takes the first entries. Check the controls configuration once after updating (review finding).

### Testing
- [x] D1X and D2X build on Linux (SDL2, g++ 15, `-Werror` flags).
- [ ] Windows: one hat press moves the menu selection by one entry. In-game hat/D-pad bindings still work.
- If it still steps twice, the console log shows whether the device appears as both `sdl-joystick` and `gamecontroller`.

## #12: Prefix the version of fork builds with "ggc-"

*Status: merged; base `master`; branch `version-ggc-prefix`*

### Summary
Requested by the repo owner: builds from this fork should be recognizable as such.

The version shown in the main menu, the window title and the console is `D2X-Rebirth <extra_version>` (`g_descent_version` in `vers_id.cpp`). SConstruct now prefixes `extra_version` with `ggc-` (`DXXCommon.VERSION_FORK_PREFIX`), in every case: with git (`git describe`), without git, and with a user-supplied `extra_version`. The build banner shows the prefix as well.

| Build | Shown before | Shown now |
|---|---|---|
| Release tag | `D2X-Rebirth v0.61-nb2` | `D2X-Rebirth ggc-v0.61-nb2` |
| Between releases | `D2X-Rebirth v0.61-nb2-3-g1234567890ab` | `D2X-Rebirth ggc-v0.61-nb2-3-g1234567890ab` |
| No git available | `D2X-Rebirth v0.61` | `D2X-Rebirth ggc-v0.61` |

The numeric version (`DXX_VERSION_STR`, 0.61.0) used by the network version checks and the tracker is unchanged, so this does not affect who can play together.

### Testing
- [x] Local Linux build: `D2X-Rebirth ggc-v0.61-nb2` and `D1X-Rebirth ggc-v0.61-nb2` in the binaries.
- [ ] The Windows artifact of this pull request contains `D2X-Rebirth ggc-v0.61-nb2-…`.

## #13: [500Hz] Build Windows packages for pull requests to release-500hz

*Status: merged; base `release-500hz`; branch `ci-release-500hz-prs`*

Adds `release-500hz` to the branch filter of `pull-request-windows.yml`, so that pull requests into the 500 Hz release branch get a Windows build like pull requests into `master`.

`release-500hz` collects the high frame rate changes (500 fps by default) and is not merged into `master`.

## #14: [500Hz] 500 fps by default, high resolution timer and precise frame pacing

*Status: merged; base `release-500hz`; branch `500hz-frame-cap`*

Part of the **500 Hz release branch** (`release-500hz`, not merged into `master`).

### Changes (final state after three review rounds)
- **500 fps by default:** release builds allow 30–500 fps (`common/main/fwd-game.h`), and `-maxfps` defaults to 500, so no flag is needed. Debug builds stay at 1–1000.
  - The `-maxfps` help text used to print the words "MAXIMUM_FPS"/"MINIMUM_FPS"; it now prints `(default: 500, available: 30-500)`.
- **High resolution game timer:** with SDL2, `timer_update` uses `SDL_GetPerformanceCounter` instead of millisecond `SDL_GetTicks`. It counts from the first reading and clamps readings below it to 0, so counter skew can't make the clock jump.
- **One frame wait for game and automap:** `timer_wait_frame(deadline)` in `common/arch/sdl/timer.cpp`.
  - It sleeps 1 ms while more than a margin remains (2 ms on Windows, 1.5 ms elsewhere), then yields with `SDL_Delay(0)` until the deadline. There's no `<thread>` dependency.
  - It runs `multi_do_frame()` at most once per millisecond while waiting, and at least once per frame.
  - `calc_frame_time` repeats the wait until `FrameTime > 0`, even if `multi_do_frame` resets the timer.
- **VSync:** the buffer swap does the pacing. The safety bound is about 550 fps (`MAXIMUM_FPS + 10%`), so monitors up to 550 Hz are never halved and a swap that doesn't block can't spin at 1000 fps. The wait also sleeps with VSync on.
- **Menus and other screens:** capped at `MENU_MAXIMUM_FPS` (200) inside `timer_delay_bound`, with or without VSync. The elapsed time is checked before sleeping.
- **Automap:** capped at 200 fps only when it pauses the game (single player). In multiplayer the game keeps running behind the automap at the normal rate.

### Trade-off
To hold 500 fps precisely, the wait yields during the last 1.5–2 ms of each frame. At 500 fps that is nearly the whole wait, so one CPU core stays busy yielding between frames. With a lower `-maxfps` it sleeps for most of the wait.

### Testing
- [x] D1X and D2X build on Linux (strict flags); Windows CI build.
- [ ] Windows: the frame counter shows a steady ~500 fps, VSync at 240 Hz gives 240 fps, menus stay at 200, and in multiplayer the game keeps 500 fps with the automap open.

## #15: [500Hz] Make mouse turning independent of the frame rate

*Status: merged; base `release-500hz`; branch `500hz-mouse`*

Part of the **500 Hz release branch** (`release-500hz`, not merged into `master`).

### Problem
In normal (non-flight-sim) mouse mode, `kconfig_end_loop` scaled each frame's mouse counts by that frame's `FrameTime`. After `read_flying_controls` divides by `FrameTime` and the physics multiplies by it again, the total turn per mouse count was proportional to FrameTime. At 500 fps the mouse was only **0.4× as sensitive** as at 200 fps. The guided missile and the automap had the same problem.

Also:
- Frames without a mouse event repeated the last movement for up to 1/30 s. That made sensitivity depend on the mouse's polling rate as well.
- The `MouseOverrun` limit was counted in frames, so it covered less real time at higher frame rates.

### Final design
- **Reference frame time:** mouse counts are scaled by `max(FrameTime, F1_0/200)` (`HIGH_FPS_REFERENCE_FRAMETIME` in `common/main/fwd-game.h`). Below 200 fps the scaling is exactly as before; above 200 fps the mouse behaves as at 200 fps. The overrun bound uses the same reference.
- **No repeat:** frames without a mouse motion event contribute 0. The 1/30 s repeat is removed.
- **Carry:** mouse input that exceeds the per-frame limit is carried into following frames, up to 1/30 s of full-rate turning. So a flick keeps its total rotation even when the mouse reports less often than frames are drawn, and a wheel step still has an effect. The carry expires within 1/30 s even if another input holds the same control at its limit. It is cleared when the axis is disabled (slide/bank modifiers), when the mouse is not a control, and in flight-sim mode.
- **Flight-sim mode:** the axis is recomputed every frame from the stored position, instead of once per mouse event with an old frame time.
- **Initialization:** the carry and overrun fields start at zero, which also covers the automap's own controls.

### Simulation (total rotation in seconds of full-rate turning, MouseSens 8, fixed-point model of `kconfig_end_loop`)
| Case | Mouse | 60 fps pre / now | 144 fps | 200 fps | 500 fps |
|---|---|---|---|---|---|
| Slow (800 counts/s, 200 ms) | 125 Hz | .233 / .233 | .176 / .139 | .175 / .100 | .179 / .100 |
| Slow | 1000 Hz | .233 / .233 | .156 / .139 | .114 / .100 | .047 / .099 |
| Fast (8000 counts/s, 50 ms) | 125 Hz | .067 / .083 | .069 / .075 | .075 / .078 | .074 / .075 |
| Fast | 1000 Hz | .083 / .083 | .083 / .089 | .080 / .083 | .082 / .083 |
| Flick (20000 counts/s, 20 ms) | 125 Hz | .033 / .050 | .042 / .047 | .040 / .048 | .042 / .043 |
| Flick | 1000 Hz | .050 / .067 | .049 / .054 | .050 / .053 | .052 / .053 |
| Wheel step | – | .033 / .050 | .028 / .040 | .030 / .038 | .032 / .035 |

How to read this:
- **At 200 fps and above:** slow, fast and flick movements give the same result at 200 and 500 fps, with 125 or 1000 Hz mice.
- **Before the PR:** a 1000 Hz mouse at 500 fps turned only 0.4× as far.
- **Noticeable change:** a 125 Hz mouse at 144–200 fps turns less for slow movements (0.100 instead of 0.175 at 200 fps). The old repeat bug inflated those values. Raise MouseSens slightly if needed.

### Testing
- [x] D1X and D2X build on Linux (strict flags); fixed-point simulation above.
- [ ] In game: a 180° sweep turns the same at 200 and 500 fps, and the wheel and slide/bank modifiers still work.

## #16: [500Hz] Exact Omega, afterburner, headlight and lava rates at any frame rate

*Status: merged; base `release-500hz`; branch `500hz-accumulators`*

Part of the **500 Hz release branch** (`release-500hz`, not merged into `master`).

### Problem
Several per-frame accumulations divide `FrameTime` by a constant, and the lost remainder grows with the frame rate. That penalized high-fps players:

| Rate, as % of intended | 60 fps | 200 fps | 500 fps |
|---|---|---|---|
| Omega recharge (`FrameTime/4`) | 100.00 | 99.08 | 97.71 |
| Afterburner drain (`/3`) | 100.00 | 100.00 | 98.47 (lasts longer) |
| Afterburner recharge (`/8`) | 99.63 | 97.86 | 97.71 |
| Headlight drain (`*3/8`) | 99.88 | 99.49 | 99.75 |
| Lava / volatile wall damage | **66.7** | 95.4 | 98.1 |

### Fix
1. **Omega** (`ce21d731d`, `laser.cpp`): carry the division remainder to the next frame. The energy cost is still computed from the charge actually gained.
2. **Afterburner and headlight** (`a56b7b5a2`, `controls.cpp`, `game.cpp`): same remainder carry. A remainder is dropped when the value is capped (full charge, empty, not enough energy).
3. **Lava / volatile walls** (`56b2e7819`, `collide.cpp`): damage is still applied at most once per 1/30 s, but it is now scaled by the time actually elapsed since the last application, instead of a fixed 1/30 s. The first touch after a break still applies the old amount. Low-fps players no longer take a third less lava damage.

All of this code runs only for the local player, so file-local statics hold the remainders. The savegame and network formats are unchanged. All rates are now 100% at any frame rate.

Left unchanged, as they are tiny or would change random number sequences: chance-per-frame checks in the robot AI, small halvings in missile acceleration and robot code, and the Omega energy cost rounding (about 0.14%).

### Testing
- [x] D1X and D2X build on Linux (strict flags).

## #17: [500Hz] Cheaper dynamic lighting and segment list in large rooms

*Status: merged; base `release-500hz`; branch `500hz-lighting-perf`*

Part of the **500 Hz release branch** (`release-500hz`, not merged into `master`). Goal: less CPU work per frame in large rooms, where players see micro lags that are worse on weaker PCs. The rendered image is unchanged.

### Changes
1. **Remove dead code** (`84a08dc75`). `use_fcd_lighting` in `lighting.cpp` is never set, so the path-distance branch of `apply_light` never ran. It is removed, together with the parameters that only fed it.
2. **Skip vertices a light cannot reach** (`f1bcaea81`).
   - `set_dynamic_light` lights every rendered vertex with every light-emitting object in the level. That is O(lights × visible vertices), and large rooms have thousands of visible vertices.
   - The vertex list is now split into blocks of 16, each with a bounding box, plus one box for the whole list. For each light, the smallest possible `vm_vec_dist_quick` to a box is computed. If even that distance fails the existing test `(dist >> headlight_shift) < abs(obji_64)`, the whole block, or the whole light, is skipped.
   - The lower bound is conservative, because `vm_vec_mag_quick` never decreases when a component grows. Vertices that are processed use the original code, arithmetic and order. The skipping is disabled wherever 32-bit overflow could matter.
   - A test harness compared old and new code on synthetic rooms, including near-overflow coordinates and 5000 random point clouds with lights and headlights: **all `Dynamic_light` values were bit-identical.**
   - Lighting loop speedup: about 1.1–1.3× in rooms of 20-unit cubes and 2–3.7× in very large rooms, with no loss for unfavourable vertex orders.
3. **Stop `build_segment_list` early** (`8b27b8308`).
   - In OpenGL builds `Render_depth` is 500. Every pass rescans the render list with a hash-map lookup per entry, even after the list has stopped changing.
   - A pass that processes nothing changes no state, so stopping there gives identical results.
   - Measured saving: about 0.1–0.5 ms per frame (100–450 visible segments). That is up to a quarter of a 2 ms frame at 500 fps.

### Found, not changed here
- **Lighting spike:** `set_dynamic_light` runs only at 60 Hz (`light_time`). At 500 fps one frame in about 8 carries the whole lighting cost. That is a periodic frame-time spike and a likely cause of the micro lags. Changing it alters when lighting is updated, so it will be a separate PR.
- `build_object_lists` scans the render list per object (O(objects × list)); small in practice.
- `render_seg_map` is a `std::unordered_map` used throughout the renderer; a flat array would be faster, but that is a larger change.

### Testing
- [x] D1X and D2X build on Linux (strict flags); lighting equivalence harness passes.
- [ ] In game: frame counter in large rooms on a weaker PC, before and after.

## #18: [500Hz] Frame-rate independent network send rates and exact position packet pacing

*Status: merged; base `release-500hz`; branch `500hz-network`*

Part of the **500 Hz release branch** (`release-500hz`, not merged into `master`). Some network messages were sent once per frame, so traffic grew with the frame rate, and position packets drifted below the configured rate. **No wire format change**, so peers on `master` builds are unaffected.

### Final state (after three review rounds)
- **Guided missile positions** (`MULTI_GUIDED`):
  - Sent on their own schedule at `PacketsPerSec` from `do_protocol_frame`, not every frame. At 500 fps that is 30/s instead of 500/s.
  - Forced sends (for example `multi_send_fire`) neither send guided updates nor reset the guided schedule. So a new missile's first update always follows its `MULTI_FIRE`, and updates keep flowing while the player fires.
  - On release, the final position and the release go out together at once. When the missile is destroyed in play, one final position goes out at once. Nothing is sent at level teardown.
  - One shared `multi_send_guided_info(missile, release, priority)` replaces the duplicated code.
- **Heartbeat:** once per second at priority 1 instead of every frame. At level start and after the join extras, one heartbeat is sent at priority 2 (reliable), so a joining player learns the level time immediately.
- **Position packets:** the schedule advances by one interval per send and restarts after a long frame instead of catching up, with at most one packet per frame.

  | fps | pps before (configured 30) | after |
  |---|---|---|
  | 30 | 20 | 29.9 |
  | 60 | 20.1 | 30.0 |
  | 144 | 28.8 | 30 |
  | 200 | 28.6 | 30.0 |
  | 500 | 29.4 | 30.1 |

- **Ping:** only the host sends ping packets. Clients' pings to other clients were always discarded.
- `Documentation/network-protocol.md` is updated.

### Known minor issue
If the reliable level-start or join heartbeat has to be resent, its level time is 250 ms or more stale, so the countdown can briefly step back until the next per-second heartbeat corrects it.

### Testing
- [x] D1X and D2X build on Linux (strict flags); Windows CI build.
- [ ] Network game: a guided missile looks smooth on remote screens, including while firing; position update rate as configured; the time-limit display stays in sync for joining players.

## #19: [500Hz] Smooth the drawing of remote player ships

*Status: open; base `release-500hz`; branch `500hz-remote-smoothing`*

Part of the **500 Hz release branch** (`release-500hz`, not merged into `master`).

### Problem
Remote ships jump: each position packet (10–30 per second) snaps their position and orientation, and physics extrapolates them in between. At 500 fps every correction is a visible jerk.

### Change: smooth only the drawing (`d129cd65e`)
- **Recording the error:** when a position update for a remote player arrives (`net_udp_read_pdata_packet`, `multi_do_position`), the difference between where the ship was drawn and the new authoritative state is stored per player.
- **Decay:** the position error decays with exp(−t/75 ms) of real time, independent of frame rate. The drawn orientation is blended back with the same weight.
- **Drawing only:** a scoped guard in `render_object` puts the smoothed position and orientation in place only around `draw_polygon_object`, so engine glow, headlight model and cloak follow the ship. HUD player names use it too. Physics, collisions, hit detection, sounds, drops, network sends and demo recording keep using the real position.
- **When it snaps instead:** the error is over 20 units or 60°, the new segment is neither the old one nor a neighbour, the ship is dead or a ghost, demo playback is running, or it is the local ship. The error is also cleared in `multi_reset_player_object` (death, respawn, level start, savegame).
- New files: `common/main/remote_smoothing.h`, `similar/main/remote_smoothing.cpp`. The protocol documentation (4.1) is updated. PDATA has no sequence number, so reordered old packets can't be detected on the receiver; this is documented.

No protocol change, and no gameplay authority change.

### Needs testing in a real network game
- The 75 ms, 20 unit and 60° values at 10–30 packets per second with real latency.
- Fast turns and rolls.
- Ships drawn slightly offset when crossing into a neighbouring segment (brief clipping through walls).
- Death, respawn and reconnect.

### Comment (2026-09-26)

**Status: on hold, not part of the first 500 Hz release.**

The latest review found problems with the approach itself, not just the implementation:
- **The drawn ship lags its real position.** Each correction is added to what is left of the previous one. With a ship that keeps accelerating, the drawn ship settles about 2.8× one correction behind its real position (for example about 5.6 units at 30 pps and τ = 75 ms). That is more than a ship's radius, so players aim at a ship that isn't quite where it is.
- **It still jumps at segment crossings.** Clamping the drawn position to the current segment cuts off the part of the offset that lies in the previous segment. Corrections across two or more segments (fast flight through short tunnel segments) snap instead of smoothing.
- Shots, muzzle flashes and sparks appear at the real position, away from the drawn hull.

The plan is to first play-test the 500 Hz release without this change, then decide on a different design, for example velocity-based blending that doesn't lag, a much shorter τ, or an option that can be switched off, based on how remote ships actually look at 500 fps.

## #20: [500Hz] Frame-rate independent ship physics: rotation remainders and exact drag

*Status: merged; base `release-500hz`; branch `500hz-physics`*

Part of the **500 Hz release branch** (`release-500hz`, not merged into `master`). Goal: ship physics that behaves the same at every frame rate, using **today's feel at 200 fps** as the reference.

### Changes
1. **Keep sub-angle rotation remainders** (`ffe64de80`).
   - Each frame, orientation advances by `fixmul(rotvel, FrameTime)` truncated to a 16-bit angle, which loses up to one angle unit per axis. At 500 fps, turns slower than 2.75°/s vanished completely (1.1°/s at 200 fps), and slow turns came in steps.
   - `physics_info` gets a runtime-only `angle_remainder` field, and a helper multiplies in 64-bit and carries the fraction to the next frame. It is used for the orientation update, turn roll, auto-levelling and guided-missile steering.
   - The field is not part of `physics_info_rw`, so savegame, level, demo and network formats are unchanged. It is reset wherever objects are created, loaded or received.
   - Over 10 s, a slow rotation that used to disappear entirely at 500 fps now arrives fully (3999 of 3999 angle units).
2. **Guided-missile earthquake tremor** (`96d582a00`, comment only). The audit said it was 2.5× stronger at 500 fps, but that's wrong: `Seismic_tremor_magnitude` is only set on 30 Hz tick frames and reset every frame, so it is already frame-rate independent. A comment now says not to scale it by FrameTime.
3. **Exact drag and thrust** (`8fbf4acb8`).
   - Above 64 fps, each frame took a linear drag step, so top speed and turn rate depended on the frame rate.
   - The new code takes the per-frame factors that the old code computed at FrameTime 327 (F1_0/200), with the same fixed-point truncation, and applies the closed form for any FrameTime in double precision.
   - A coasting object still comes to rest. Other parts of `do_physics_sim` (collisions, bouncing, sticking) are unchanged.

### Result (Pyro-GX: mass 4.0, drag 0.033)
| fps | top speed before | after | turn rate before | after |
|---|---|---|---|---|
| 60 | 57.29 (−2.1%) | 58.52 | −5.3% | ±0.00% |
| 144 | 58.28 (−0.4%) | 58.52 | −1.2% | ±0.00% |
| 200 | 58.52 | 58.52 | reference | +0.17%* |
| 500 | 58.98 (+0.8%) | 58.52 | +1.2% | ±0.00% |

\*The old per-frame truncation at 200 fps lost 0.17% of turn rate; the closed form does not reproduce that loss. Over the same velocities and drags, one frame at 200 fps differs from the old code by at most 2 fix units.

**Effect on players below 200 fps:** at 60 fps, ships are now 2.1% faster and turn 5.3% faster than before. That puts them level with high-fps players, which is the fairness goal of this branch.

### Testing
- [x] D1X and D2X build on Linux (strict flags); standalone numeric checks (frame splitting, top speed, acceleration, coasting).
- [ ] In game: the feel at 200 fps is unchanged, and slow joystick turns work at 500 fps.

## #21: Build Windows packages only for pull requests labeled playtest (master)

*Status: merged; base `master`; branch `ci-playtest-label-master`*

Pull requests into this branch get a Windows package only when they carry the **`playtest`** label (created in the repo). The build runs when the label is added and on later pushes to that pull request, and `workflow_dispatch` still allows starting it manually. All other pull requests skip the roughly 30-minute Windows build. Pushes to `master` (`ci.yml`) and release tags (`release.yml`) are unchanged.

Requested by the repo owner to save build time.

## #22: Build Windows packages only for pull requests labeled playtest (release-500hz)

*Status: merged; base `release-500hz`; branch `ci-playtest-label-release-500hz`*

Pull requests into this branch get a Windows package only when they carry the **`playtest`** label (created in the repo). The build runs when the label is added and on later pushes to that pull request, and `workflow_dispatch` still allows starting it manually. All other pull requests skip the roughly 30-minute Windows build. Pushes to `master` (`ci.yml`) and release tags (`release.yml`) are unchanged.

Requested by the repo owner to save build time.

## #23: Apply the VSync setting once the OpenGL context exists

*Status: merged; base `master`; branch `fix-vsync-at-startup`*

### Problem
Reported by the repo owner on a 120 Hz monitor: with VSync turned **off** in the options, the game still ran at 120 fps after starting. Leaving the options menu made it run uncapped. It happened again after every restart.

`gr_set_attributes()` sets the swap interval with `SDL_GL_SetSwapInterval`. During start-up (`similar/arch/ogl/gr.cpp`) it is called **before** `SDL_CreateWindow`/`SDL_GL_CreateContext`. The swap interval is a property of the current OpenGL context, so without a context the call has no effect, and the driver's default applied (often VSync on). The options menu calls `gr_set_attributes()` again when it closes, and by then a context exists, which is why leaving the options fixed it.

### Fix
Set the swap interval again right after `SDL_GL_CreateContext` (SDL2 path), and log a message if the driver refuses. The menu path is unchanged.

This is a general bug, so it goes to `master` and will also be brought into `release-500hz`.

### Testing
- [x] D1X and D2X build on Linux (strict flags).
- [ ] Windows: with VSync off, the FPS counter is uncapped right after start; with VSync on, it matches the refresh rate.

## #24: [500Hz] Apply the VSync setting once the OpenGL context exists

*Status: open; base `release-500hz`; branch `500hz-vsync-at-startup`*

Cherry-pick of #23 (merged into `master`) for the 500 Hz release branch.

Reported on `v0.61-500hz-1`: with VSync turned off, the game still ran at the monitor's refresh rate (120 fps on a 120 Hz OLED) after starting. It only reached 500 fps after leaving the options menu, every time.

**Cause:** during start-up, `gr_set_attributes()` calls `SDL_GL_SetSwapInterval` before the OpenGL context exists, so the call has no effect and the driver's default (VSync on) applies. Leaving the options menu calls it again with a context.

**Fix:** set the swap interval again right after `SDL_GL_CreateContext`.

Labeled **playtest**, so this pull request gets a Windows package to verify the fix.

