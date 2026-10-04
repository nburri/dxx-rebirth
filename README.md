```
                         __________
__________/ DXX-Rebirth /
```

## 0. Introduction:

DXX-Rebirth is based on the late D1X and D2X source ports (which, in turn, were based on the original Descent source and LDescent). The Rebirth Team has spent a lot of time working to improve the source code by fixing old bugs and adding some improvements, while always staying true to our philosophy: Keep it Descent!

It is the goal of DXX-Rebirth to keep Descent 1 & 2 alive and well, updating them for modern PCs while also keeping them the same games you remember playing back in 1995!

> Now Material Defender...Prepare for Descent!

## 1. Features:

DXX-Rebirth has every little feature you may remember from the original Descent 1&2 and much more.

For example:

* Full compatibility with Descent and Descent 2, including all expansions and third-party levels.
* DXX-Rebirth runs on your favourite Operating System. We officially support Linux (and other \*NIXs), Mac OS X and Windows. The source code can also be compiled on many other systems!
* OpenGL provides fast and smooth rendering - even on low-end systems.
* Optionally you can enable several effects like Transparency, Colored Lighting, Texture Filtering, FSAA, etc.
* Thanks to SDL, a wide variety of Joysticks are supported. Also you can use more Joysticks, buttons and axes than you can ever operate in your state of evolution.
* Joystick, Keyboard and Mouse can be used simultaneously.
* The games can display all resolutions and aspects supported by your monitor, including an option for VSync.
* Besides MIDI and CD-Audio (Redbook), you can play your own custom Music from your hard drive or a separate AddOn.
* Both games can utilize special AddOn packs to replace or expand the original game content.
* Multiplayer via UDP protocol provides a fast and easy-to-use LAN and Online action. This includes reliable communication causing less glitches due to lost packets.
* The ingame Demo-recording system has been improved. Demos are less glitchy and smaller while still being backwards-compatible to earlier versions of the games.
* Higher game speed will not cause glitches such as unacceptable fast homing projectiles, incredible high damage caused by several collisions or Fusion cannon, etc.
* Player files, Savegames, Demos and Missions from DOS-Versions of the games can freely be used in DXX-Rebirth.
* On Windows, the StickyKeys, FilterKeys and ToggleKeys shortcut keys (e.g. "press Shift 5 times") are switched off while the game has the focus, unless the feature itself is switched on; the settings are restored on alt-tab, exit and crash. `-keep-accessibility-keys` keeps them.
* Mac Command keys are now working - see F1 Help. Command-Q works much like a normal Mac program
* Even more ...


## 2. Installation:

See [INSTALL.markdown](INSTALL.markdown).


## 3. Multiplayer:

DXX-Rebirth supports Multiplayer over UDP/IP.  Using UDP/IP works over LAN and Internet. 

By default, each game communicates over UDP-Port 42424. This can be changed via the menus, command-line argument or .ini files. 

Please be aware that if you host a game and players want to join your game online via direct IP connection, your specified game port should be forwarded on your router. 

If you host your game on the Online Tracker and other players join via this method, port forwarding should not be necessary.

If you only want to join a game, or host a game on LAN (not online), port forwarding should not be necessary. If you do experience problems, please check your NAT settings and/or Firewall.

### Copying and pasting game addresses

- **Host:** in the screen that waits for players, press **Ctrl+C**; in a running game, open the netgame info with **Shift+Pause** and press **Ctrl+C**. A menu lists the addresses of this computer with the game port (for example `192.168.1.5:42424`) and copies the best one to the clipboard; select another with Enter to copy it instead. Behind a router the game learns your public Internet address once a player has joined from the Internet (each player tells the host the address it reaches it at) or the tracker listed your game: the waiting screen and the netgame info then show it as *Public, seen by N players* (or *seen by the tracker*), and it comes first in the menu. Before that they show your LAN address: give players your router's public IP with the game port, and forward that UDP port to your computer. A client in a game gets the host's address the same way, to pass it on.

### Is the tracker working?

The game writes what happens with the tracker to `gamelog.txt`, in lines starting with `[Tracker]`: the address the tracker's name resolves to, the registration of a hosted game, the tracker's ACKs (or "No response from game tracker" after 25 seconds), the address the tracker lists the game at, and the games a game list request (F4 in the game list) brought. If the name does not resolve, check it with `nslookup tracker.dxx-rebirth.com` (or the name given with `-tracker_hostaddr`) in a command prompt.
- **Joining:** in *Join game manually*, **Ctrl+V** (or Shift+Insert) in the address field pastes the address; a pasted `address:port` or `[IPv6]:port` also fills the game port field.
- In every text field of the menus and in the chat line (F8), **Ctrl+V** / **Shift+Insert** paste (the first line of the clipboard, trimmed, as far as it fits) and, in menu fields, **Ctrl+C** copies the field's text. On macOS, Cmd+V and Cmd+C work too.

### Capture the flag (Classic)

A variant of capture the flag that the host picks in the game setup (*Capture the flag (Classic)*, next to *Capture the flag*). Each team's flag starts in the team's own goal instead of where the level designer put it, and a captured flag goes back to its own goal: you know where the enemy flag is, but you have to fight your way through the enemy team to get it. As before, you score by bringing the enemy flag into your own goal.

The host sets three flag rules under *Advanced Options* → *CTF Classic: flag rules*. They are saved with the host's game settings, and players see them in the netgame info (Shift+Pause):

- **Dropped flag returns home at once.** When a carrier is killed or leaves, the flag goes straight home. If off, it stays where it fell. Default: off.
- **Own team returns its flag by touch.** Fly through your own team's flag lying away from home to send it home ("Red flag returned by …"). Default: on.
- **Score only with own flag home.** A capture counts only while your own flag sits in your goal. Otherwise you see "Your flag must be home to score". Default: on.

When *score only with own flag home* is on but *own team returns its flag by touch* is off, a flag lying away from home goes home by itself after 30 seconds. Without this, a team whose flag was dropped somewhere could never score again. The level needs a goal for each team. When a team has several goal segments, the flag goes to the largest one. All players need a build with this mode (network protocol 111).


### Taunts (horn)

Press **V** (Controls → *Taunt / Horn*; it can also be put on a joystick or mouse button) and your ship sounds its horn. Everyone near your ship hears it from where you are, like a weapon's sound.

- **Your own sound:** put a file named `taunt.wav`, `taunt.mp3`, `taunt.ogg` or `taunt.flac` in the folder with your pilot files (`.plr`) and `descent.cfg`, and choose *Own file* in *Options → Sound Effects & Music* (the default). The menu shows whether the file was found. The game uses at most the first 2 seconds (after any silence at the start) and evens out the volume, so no horn is louder than a weapon.
- **Starter horns:** the same menu offers a car horn, a bike bell, an air horn and a beep, and *Off*. Choosing one plays it.
- **Limits:** up to 3 horns within 2 seconds and 4 within 10 seconds; more and your horn cools down for 5 seconds ("Horn cooling down"). The host enforces this for everyone.
- **Muting:** uncheck *Hear other players' horns* in the same menu, or type `/mute name` in the chat (F8) to mute one player for this session (`/unmute name`, `/mute` lists them).
- **Bots:** the host can let bots honk after some of their kills (*Bots taunt after kills* on the Bots screens, default off).
- Until a later version sends the files, the other players hear your own file as the car horn. All players need a build with network protocol 112. Details: [Documentation/taunts.md](Documentation/taunts.md).

## 4. Legal stuff:

See [COPYING.txt](COPYING.txt) and [GPL-3.txt](GPL-3.txt)


## 5. Contact:

- https://github.com/dxx-rebirth/dxx-rebirth/

## 6. Issue Reporting

Use GitHub issues tab report a [new issue](https://github.com/dxx-rebirth/dxx-rebirth/issues/new), be sure to add as much detail as possible in the template provided.

### Log files

The game writes what it does to `gamelog.txt` in the folder it writes the pilot files to (`~/.d2x-rebirth` on Linux). Each log starts with a line giving the date and time the game was started and its version. The logs of the last 5 runs are kept: `gamelog.txt` is the current (or last) run, `gamelog.1.txt` the run before, up to `gamelog.4.txt`; so a log that shows a problem is still there after a restart. `-gamelog-keep <n>` keeps <n> runs instead (0 overwrites `gamelog.txt` every time). After a crash, attach the crash report the game names and the `gamelog.txt` (or, if you started the game again since, the matching `gamelog.N.txt`) to the issue.
