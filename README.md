# ultnbalive99: NBA Live 99 (PC) on modern Windows

NBA Live 99 for Windows (1998) was built for Windows 95/98, DirectX 6 and 3dfx Voodoo cards, and installs
with a 16-bit setup program that no longer runs on 64-bit Windows. This project keeps the game playable on
current PCs without changing any of its files: a launcher and a set of DLLs sit next to the game and fix it
in memory as it runs.

Release 0.4: the game runs as shipped on Windows 10 and 11, with our own sound driver, fast loading from
network drives, smooth 60 fps matches, and modern gamepads with rumble.

- **No installer, no disc, no registry:** a plain copy of the disc's game folder runs. The CD check is
  skipped in memory, the install paths the game expects are supplied by our core, and the game's registry
  key is answered from memory. Nothing is written to the registry.
- **Graphics:** the game's own 3dfx driver (EA's `3rash\voodooa.dll`) draws through our Glide 2 replacement
  on Direct3D 11, in borderless full screen at the desktop resolution with the picture centred in 4:3. The 3D
  is drawn at full resolution, including the arena behind the menus. The desktop display mode is never
  changed.
- **Menus on Windows 10 and 11:** the menu labels show their proper words (Windows 10 and 11 garbled them),
  and Alt-Tab out of the game and back no longer freezes it.
- **Sound (0.2):** our `eacsnd.dll` replaces EA's DirectSound driver and plays the game's sound through SDL3
  on any output device (stereo, 5.1 or 7.1 outputs), with no DirectSound needed.
- **Fast loading from network drives (0.3):** drive information is cached, the game's temporary roster files
  stay in memory and the commentary banks are read into memory once, so a network drive plays as smoothly as
  a local one.
- **Smooth frame pacing (0.3):** every frame of a match shows exactly one step of the game, and the game clock
  is matched to the screen, so animation and camera movement are even. Works on 60 Hz and 120 Hz screens, and
  with G-Sync or FreeSync at other rates.
- **Gamepads (0.4):** Xbox, PlayStation, Nintendo and other pads work through SDL3, up to the game's four
  controllers, and can be plugged in and out while the game runs. Each pad shows its own button names in the
  game's Configure Controller screen, and starts with the PlayStation version's layout (turbo on the right
  trigger). The D-pad, left stick and both triggers work; the game's own Analog Mode setting steers with the
  stick between the eight directions. In the menus A (Cross) accepts and B (Circle) goes back; Start skips the
  intro movies and auto replays. Your button changes are kept as usual in `devcfgs.ini`.
- **Rumble (0.4):** the pad of the player concerned vibrates on dunks, blocks, steals, contact, knock-downs,
  hanging on the rim, injuries, jump balls and baskets; never in the menus or for players the computer
  controls. Medium strength by default, with low, high and off, and a strength for each event.
- **Keys:** Page Down saves a screenshot to `screenshots\` in the game folder; Ctrl+F11 shows or hides the
  frame rate.
- **Settings:** `nba99fix.ini` (the game keeps its own settings in `nba99.ini`).

Later releases are planned to add the rosters (including the shipped 1998-99 set), an in-game options menu,
visual options and multiplayer.

**Unofficial.** Not affiliated with or endorsed by Electronic Arts. You need your own copy of the
game. No EA code, data or art is included in this project or its downloads.

## Requirements

- Windows 10 or 11 (also runs under Wine).
- An NBA Live 99 for Windows disc (or an image of it).
- EA's public v1.1 patch for NBA Live 99, `nbalive99patch_english.exe`. The v1.1 `NBAWIN.EXE` is
  2,488,320 bytes, SHA-256 `3563b929b75379df3d93468d1c957b742f17b2a708098b7cfbeeac3fda51287d`. The launcher
  refuses the disc's v1.0 exe and changed (no-CD) copies, and says what to do.
- A graphics card with Direct3D 11.

## Install

The disc's `SETUP\ENGLISH\SETUP.EXE` is a 16-bit InstallShield launcher and does not run on 64-bit Windows.
It is not needed: the disc's `\NBA99\` folder is the complete game, so the install is a copy.

1. **Copy from the disc** (or the mounted image) the folder `\NBA99\` to a folder of your choice, for example
   `C:\Games\NBA Live 99`. It holds the folders `3RASH`, `ART`, `CSV`, `DBDATA`, `DITTY`, `MOVIES`, `ORIGDB`,
   `SONGS`, `SOUND`, `SPEECH` and the files `NBAWIN.EXE`, `CTRLS.DLL`, `EACSND.DLL`, `FONTRES.DLL`,
   `INPDEVS.INI` and `MOUSE.FSH`. If the disc folder also has `SHLWAPI.DLL` or `WININET.DLL`, do not copy
   them (they are 1998 Windows files the installer put in the Windows folder).
2. **Make the empty folders** the installer would have made, inside the game folder:
   `saves`, `art\ingame\players\user`, `art\frontend\foregrnd\logos\user`, `speech\gamespch\user` and
   `speech\playspch\user`.
3. **Apply EA's v1.1 patch:** run `nbalive99patch_english.exe` (a 32-bit self-extracting zip, fine on
   Windows 11) and unzip it into the game folder, replacing the files there, or open it with 7-Zip and
   extract it into the game folder. Check that `NBAWIN.EXE` is now 2,488,320 bytes.
4. **Keep EA's sound driver:** rename the game's `EACSND.DLL` to `eacsnd_ea.dll`. Our `eacsnd.dll` takes its
   place; EA's copy is only used by the Debug build's sound log, and renaming it back restores EA's sound.
5. **Copy our release files** into the game folder: download a release zip (Main for playing, Debug for
   testing and bug reports) and copy all of its files and folders next to `NBAWIN.EXE`.
6. Start the game with **`NBALive99.exe`**.

Apart from the sound driver in step 4, the game's own files are never changed. The install paths the game
reads (its `install.dat`) are written as a copy in `%TEMP%\NBALive99\`. To uninstall our files, delete the
ones you copied in step 5 (and `nba99fix.ini`, `logs\` and `crash\` if they are there), then rename
`eacsnd_ea.dll` back to `EACSND.DLL`.

The shipped 1998-99 rosters stay on the disc (`\NBA99\ORIGDB` and `\NBA99\DBDATA`); the patch replaces some
of the roster tables in `ORIGDB`. A later release explains how to use the shipped rosters as well.

## Known limitations in 0.4

- Screens whose refresh rate is not a multiple of 60 Hz (75, 100, 144 Hz) without G-Sync or FreeSync show
  matches unevenly; set the desktop to 60 or 120 Hz, or turn on variable refresh.
- Auto replays run at 15 frames a second, as the game was made.
- EA's licence screen at start waits about 8 seconds unless a key or a pad button is pressed.
- Gamepads: the menus cannot be moved through with the pad (the game's menus are mouse driven; A and B accept
  and go back). Up to four controllers, as the game was made: pads 3 and 4 take the places of the two keyboard
  players. Movement follows the stick in the game's own steps, not fully analogue.
- Gamepads: tested on an Xbox Wireless Controller; PlayStation, Nintendo, Bluetooth and several pads at once are
  supported but not yet tested on real hardware.
- The menus themselves are the game's own 640x480 pictures, scaled.
- The game mixes all its sound to stereo; positioned (3D) sound around surround speakers is not active yet.

## Settings

Settings are in `nba99fix.ini`; each setting is described there. Releases ship `nba99fix.ini.default`, and
the launcher makes your `nba99fix.ini` from it on the first start, so copying a newer release over the old
one keeps your settings. Delete `nba99fix.ini` to go back to the defaults.

## Building

The sources are in `src/`. You need mingw-w64 (i686), Python 3 and zip. On Linux or WSL:

```
cd src
./build.sh            # both releases and the tools; ./build.sh main, debug or tools for one
```

The releases are written to `src/dist/` (`dist/Main`, `dist/Debug` and a zip of each). `src/README.md`
has the technical notes for every part.

## What we ship

We do not ship EA assets or EA intellectual property: no game files, art, audio, video, text or database
records, and nothing made from them. Anything that needs the game's own material is generated on the
user's machine from their own files. See [CONTRIBUTING.md](CONTRIBUTING.md) for the full rule.

## Third-party components

| Component | Licence | Licence file |
|---|---|---|
| DOSBox Staging CRT shaders crt-hyllian and vga-1080p, ported to HLSL (in `glide2x.dll`) | MIT | `src/third_party/CRT-SHADERS-LICENSE.txt` |
| SDL 3.2.26 (`SDL3.dll`, used by `eacsnd.dll` for sound and `nba99core.dll` for gamepads and rumble) | zlib | `src/third_party/SDL3-LICENSE.txt` |

## Licence

CC0 1.0 Universal (public domain dedication). See LICENSE.
