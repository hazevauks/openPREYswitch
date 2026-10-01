<p align="center">
  <img src="assets/img/banner.png" alt="openPREY banner">
</p>

<h1 align="center">openPREY for Nintendo Switch</h1>

<p align="center">
  <b>Prey (2006), running natively on the Nintendo Switch as homebrew.</b><br>
  A port of <a href="https://github.com/themuffinator/OpenPrey">OpenPrey</a>, built with devkitPro and libnx.
</p>

<p align="center">
  <a href="https://github.com/hazevauks/openPREYswitch/releases/latest"><img alt="Latest release" src="https://img.shields.io/github/v/release/hazevauks/openPREYswitch?include_prereleases&label=release"></a>
  <a href="https://github.com/hazevauks/openPREYswitch/releases"><img alt="Downloads" src="https://img.shields.io/github/downloads/hazevauks/openPREYswitch/total"></a>
  <img alt="Platform: Nintendo Switch" src="https://img.shields.io/badge/platform-Nintendo%20Switch-e60012">
  <a href="LICENSE"><img alt="License: GPLv3" src="https://img.shields.io/badge/license-GPLv3-blue"></a>
</p>

<p align="center">
  <a href="#installation">Installation</a> ·
  <a href="#controls">Controls</a> ·
  <a href="#settings-menu">Settings</a> ·
  <a href="#performance-tips">Performance tips</a> ·
  <a href="#known-issues">Known issues</a> ·
  <a href="#support">Support</a> ·
  <a href="#building-from-source">Building</a>
</p>

> [!IMPORTANT]
> openPREY includes no game data. To play, you need your own copy of **Prey (2006) for PC**.

## About

openPREY runs the PC version of Prey (2006) on the Nintendo Switch. It is built on
[OpenPrey](https://github.com/themuffinator/OpenPrey), themuffinator's open-source
source port of Prey's id Tech 4 engine, together with the game's own code from the
Prey SDK. Everything specific to the Switch was written for this port: controller,
touch and gyro input, the system keyboard, OpenGL through the console's Mesa
(nouveau) driver, clock profiles and crash reports.

**Status: first public release.** On real hardware, the campaign has been played
from the start up to the first Spirit Walk puzzles (map `game/feedingtowerc`),
mostly in handheld mode. Later chapters have not been tested yet, and some busy
areas run below 30 fps. Read [Known issues](#known-issues) before you start.

## Features

- Prey's single-player campaign from your own PC files, with saving and loading.
- A 30 fps frame lock and dynamic resolution, up to 1280×720.
- Controls in the style of modern console shooters, plus gyro aiming in handheld
  mode, with a Pro Controller or with a pair of Joy-Con.
- Touch in menus, and the system keyboard for savegame names and console commands.
- A settings menu on the **−** button: FPS counter, frame lock, shadows, dynamic
  resolution, GPU clock profile, gyro, look speed and subtitles.
- The console's official GPU and memory clock profiles, applied by the game itself.
- Logs and crash reports saved to the SD card, ready to attach to a bug report.

## Requirements

- A Nintendo Switch running [Atmosphère](https://github.com/Atmosphere-NX/Atmosphere).
- **Title override.** Open the Homebrew Menu by holding **R** while you start any
  installed game, then launch openPREY from there. Prey needs more memory than
  homebrew gets when started from the Album (applet mode), so it does not run from
  there. Forwarders also get the full memory but have had less testing.
- **Prey (2006) for PC**, for the `.pk4` files in its `base` folder. Tested with the
  retail CD/DVD version (`pak000.pk4` to `pak004.pk4`, plus `game00.pk4`). The file
  layout of some digital releases (`pak_data.pk4`, `pak_sound.pk4`, `pak_en_v.pk4`,
  `pak_en_t.pk4`) is recognized but untested on the Switch.
- **Free space on the SD card** for those files, plus a few GB for the texture
  cache, which grows as you reach new maps.
- **Recommended:** [sys-clk](https://github.com/retronx-team/sys-clk) or
  [Horizon-OC](https://github.com/Horizon-OC/Horizon-OC) to raise the CPU clock
  (see [Performance tips](#performance-tips)).

## Installation

1. Download `openPREY-switch-<version>.zip` from the
   [latest release](https://github.com/hazevauks/openPREYswitch/releases/latest).
2. Extract it to the **root of the SD card**. This creates the folder
   `switch/openprey/`.
3. Copy **every `.pk4` file** from the `base` folder of your PC installation (for
   example `C:\Program Files (x86)\Human Head Studios\Prey\base`) into
   `switch/openprey/base/` on the SD card.
4. Put the SD card back in the console, hold **R** while starting any game, and pick
   **openPREY** in the Homebrew Menu.

The SD card should end up like this:

```text
SD card root
└── switch/
    └── openprey/
        ├── OpenPrey.nro     the game
        ├── basepr/          openPREY's files; your settings, saves, logs and texture cache go here too
        └── base/            your Prey files
            ├── game00.pk4
            ├── pak000.pk4
            ├── pak001.pk4
            ├── pak002.pk4
            ├── pak003.pk4
            └── pak004.pk4
```

The folder must be named `switch/openprey/`: the game always looks for its files
there.

> [!NOTE]
> **The first visit to each map takes a while**, often two minutes or more. The game
> compresses the map's textures and keeps them in `basepr/generated/`, and the
> loading screen can look frozen meanwhile. Later loads read that cache and are much
> faster, so do not delete `basepr/generated/`.

### Updating

Extract the new release over the old one. Your settings, saves and texture cache in
`basepr/` are kept. To back up your progress, copy `switch/openprey/basepr/savegames/`.

## Controls

| Button | Action |
|---|---|
| Left stick | Move |
| Right stick | Look |
| **ZR** | Fire; also presses buttons and uses screens (walk up and aim at them) |
| **ZL** | Alternate fire |
| **A** | Spirit walk |
| **B** | Jump |
| **X** | Next weapon |
| **Y** | Reload |
| **L** | Lighter |
| **R** | Throw grenade |
| **L3** (press the left stick) | Sprint (toggle) |
| **R3** (press the right stick) | Crouch (toggle) |
| D-pad up | Zoom (toggle, with a weapon in hand) |
| D-pad down | Center view |
| D-pad left / right | Previous / next weapon |
| **+** | Game menu (pauses) |
| **−** | openPREY settings menu |

- **Menus:** the left stick moves the cursor, **A** clicks and **B** goes back. You
  can also tap the screen. Selecting a text field, such as a savegame name, opens the
  system keyboard.
- **Gyro aiming** adds to the right stick and is on by default. Turn it off, or use
  it only while **ZL** is held, in the settings menu.
- **Talking to people:** Prey has no talk button; conversations start when you walk
  near people.

## Settings menu

Press **−** to open it, in the menus or during play (the game pauses behind it).
Choose an item with the D-pad up and down, and change it with left and right or
**A**. **B** closes the menu. Changes take effect at once and are saved.

| Setting | Values (default in bold) |
|---|---|
| Show FPS | **Off**, On |
| Frame rate lock | **30 fps**, Off |
| Shadows | **Off**, On |
| Dynamic resolution | Off, **On** |
| GPU clock profile | System default, GPU 384 MHz, GPU 460 MHz, **GPU 460 + RAM 1600** |
| Gyro aiming | Off, **Always**, While aiming (ZL) |
| Gyro sensitivity | 0.25 to 6.00 (**2.00**) |
| Look speed | 60 to 400 degrees per second |
| Invert look | **Off**, On |
| Subtitles | Off, **On** |

**Y** in this menu opens the developer console. There, **A** brings up the keyboard
to type a command, the D-pad up and down recalls earlier commands, and **B** or **−**
closes it.

## Performance tips

- **Raise the CPU clock.** Set the CPU to **1224 MHz** in
  [sys-clk](https://github.com/retronx-team/sys-clk) or
  [Horizon-OC](https://github.com/Horizon-OC/Horizon-OC), or to 1785 MHz for more
  headroom. With title override, openPREY runs as the game you held **R** on, so
  create the profile for that game. The game sets the GPU and memory clocks itself
  (GPU clock profile), so there is no need to overclock the memory.
- **Keep shadows off** (the default). On the Switch they cost a large part of each
  frame in busy scenes.
- **Keep the 30 fps lock and dynamic resolution on** (the defaults). The lock keeps
  frame pacing even, and dynamic resolution lowers the 3D resolution in heavy scenes.
- **Check the numbers** with Show FPS in the settings menu, or with
  [Status Monitor](https://github.com/masagrator/Status-Monitor-Overlay) for clocks
  and load.
- **Do not use `com_fixedTic 1`.** The frame counter goes up only because the game
  then advances one step per frame: below 60 fps, everything runs in slow motion.
- **Do not use `vid_restart`.** It leaves the screen black on the Switch; restart the
  game instead.
- **Exit from the game's menu** when you can. It restores the clocks and closes the
  log cleanly.

Handheld, with the CPU at 1224 MHz and shadows off, the opening maps hold 30 fps,
dipping to about 25 in the busiest moments.

## Known issues

- **Falling through walls in `game/feedingtowerc`.** After you blow up a pod next to
  the flesh wall near the start of the map, going through the small tunnel it opens
  can make Tommy pass through the walls and die, even after loading the autosave.
  This is being investigated. If it happens to you, a log recorded with
  `g_debugPlayerPhysics 1` (typed in the console before entering the tunnel) helps a
  lot.
- **Long first loads** of each map while its texture cache is built (see
  [Installation](#installation)).
- **Some busy areas later in the game drop well below 30 fps.**
- **`vid_restart` leaves the screen black.** Restart the game instead.
- **No multiplayer.**
- **Docked mode** has had less testing than handheld.

## Support

Found a bug? Open an [issue](https://github.com/hazevauks/openPREYswitch/issues) and
include:

- the openPREY version, your system firmware and Atmosphère versions, and how you
  started the game (title override or forwarder);
- handheld or docked, and your clock settings;
- what happened and where in the game (the map name is in the log);
- these files from the SD card, when they exist:

| File | Contents |
|---|---|
| `switch/openprey/basepr/logs/openprey.log` | The log of the last session. It is written line by line, so it survives crashes and freezes. |
| `switch/openprey/basepr/logs/openprey-previous.log` | The session before that. Starting the game renames the last log to this name, so send both if you restarted after the problem. |
| `switch/openprey/openprey_error.txt` | The message of a fatal error. |
| `switch/openprey/openprey_crash.txt` | A crash report (addresses and registers). |
| `atmosphere/crash_reports/` | The newest report, if the console showed an error screen. |

Screenshots or a short video help with graphics and gameplay bugs.

Please do not ask for game files; such requests will be closed.

### Common problems

| Problem | Solution |
|---|---|
| "Required official Prey base pk4 files are missing" or "Couldn't load default.cfg" | The Prey `.pk4` files are not in `switch/openprey/base/`. |
| A crash or an out-of-memory error at startup | Start through title override (hold **R** on a game), not from the Album. |
| A loading screen that looks frozen | It is probably the first visit to that map. Give it a few minutes. |
| A black screen after `vid_restart` | Restart the game. |
| Settings or controls in a bad state | Delete `switch/openprey/basepr/OpenPreyConfig.cfg` to restore the defaults. |

## Building from source

openPREY is built with [devkitPro](https://devkitpro.org/wiki/Getting_Started)
(devkitA64 and libnx) and Meson. Install devkitPro with Switch development
selected, then add the libraries and build tools from its MSYS2 shell:

```sh
pacman -S --needed switch-sdl2 switch-mesa switch-libdrm_nouveau switch-glad \
    switch-openal-soft switch-zlib meson ninja python
```

Configure and build:

```sh
meson setup builddir-switch --cross-file tools/switch/meson/switch-cross.ini -Dbuildtype=release
meson compile -C builddir-switch
```

On Windows, run these from devkitPro's MSYS2 bash started with `MSYSTEM=MSYS` (the
msys `meson` and `python` do not run in MinGW mode), and run
`export MESON_RSP_THRESHOLD=2147483647` first, so that long link lines are not moved
into response files, whose paths MSYS2 does not convert.

The build produces `builddir-switch/OpenPrey.nro`. Link-time optimization is on by
default: a full build takes about 30 minutes, and every rebuild relinks everything
(about 10 minutes). To make the release zip:

```sh
python3 tools/switch/package_release.py
```

It writes `.tmp/release/openPREY-switch-<version>.zip`.

More documentation:

- [docs-dev/switch-port.md](docs-dev/switch-port.md): technical reference (design,
  controls, performance, memory, profiling).
- [docs-dev/switch-port-handbook.md](docs-dev/switch-port-handbook.md): open issues,
  test loop and lessons learned.
- [BUILDING.md](BUILDING.md): desktop builds of OpenPrey (Windows, Linux, macOS).

## Contributing

Bug reports, tests on hardware and pull requests are welcome. Development happens on
the `switch-port` branch, and `main` holds the released state. Engine and game fixes
that help every platform are worth sending upstream to
[OpenPrey](https://github.com/themuffinator/OpenPrey) too.

## Credits

- **hazevauks**: the Switch port.
- **[themuffinator](https://github.com/themuffinator)**:
  [OpenPrey](https://github.com/themuffinator/OpenPrey) and
  [OpenPrey-GameLibs](https://github.com/themuffinator/OpenPrey-GameLibs), which this
  port is built on.
- **Justin Marshall** ([Quake4Doom](https://github.com/jmarshall23/Quake4Doom)) and
  **Robert Beckebans**: engine work that OpenPrey builds on.
- **id Software** (id Tech 4), **Raven Software** (Quake 4) and **Human Head
  Studios** (Prey and the Prey SDK).
- **devkitPro** and **switchbrew**: devkitA64, libnx, and the Switch packages of
  Mesa, OpenAL Soft and zlib.
- The **Mesa** and **nouveau** developers, the **OpenAL Soft** contributors,
  **Sean Barrett** ([stb_vorbis](https://github.com/nothings/stb)) and the **GLEW**
  team.
- Other Switch ports that served as references, among them fgsfdsfgs's
  [dhewm3](https://github.com/fgsfdsfgs/dhewm3) and
  [OpenMW](https://github.com/fgsfdsfgs/openmw) ports and NaGaa95's
  [hl2_nx](https://github.com/NaGaa95/hl2_nx).

## License

The engine is licensed under the [GNU General Public License v3.0](LICENSE). The game
code (`src/game`, `src/Prey` and `src/preyengine`) comes from the Prey SDK through
[OpenPrey-GameLibs](https://github.com/themuffinator/OpenPrey-GameLibs) and is subject
to the original Human Head Studios SDK EULA. Prey's game data belongs to its owners
and is not included.

## Disclaimer

openPREY is an unofficial fan project. It is not affiliated with, endorsed by or
sponsored by Nintendo, Human Head Studios, 2K, Bethesda, ZeniMax, id Software or
Raven Software. PREY is a trademark of ZeniMax Media Inc., and Nintendo Switch is a
trademark of Nintendo. You must own a legitimate copy of Prey (2006) to play.

The software is provided "as is", without warranty of any kind. Use homebrew and
overclocking at your own risk.
