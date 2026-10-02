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

- **Host:** in the screen that waits for players, press **Ctrl+C**; in a running game, open the netgame info with **Shift+Pause** and press **Ctrl+C**. A menu lists the addresses of this computer with the game port (for example `192.168.1.5:42424`) and copies the best one to the clipboard; select another with Enter to copy it instead. The game cannot see your public Internet address when you are behind a router: give players your router's public IP with the game port, and forward that UDP port to your computer. A client in a game gets the host's address the same way, to pass it on.
- **Joining:** in *Join game manually*, **Ctrl+V** (or Shift+Insert) in the address field pastes the address; a pasted `address:port` or `[IPv6]:port` also fills the game port field.
- In every text field of the menus and in the chat line (F8), **Ctrl+V** / **Shift+Insert** paste (the first line of the clipboard, trimmed, as far as it fits) and, in menu fields, **Ctrl+C** copies the field's text. On macOS, Cmd+V and Cmd+C work too.


## 4. Legal stuff:

See [COPYING.txt](COPYING.txt) and [GPL-3.txt](GPL-3.txt)


## 5. Contact:

- https://github.com/dxx-rebirth/dxx-rebirth/

## 6. Issue Reporting

Use GitHub issues tab report a [new issue](https://github.com/dxx-rebirth/dxx-rebirth/issues/new), be sure to add as much detail as possible in the template provided.
