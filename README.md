# ultnbalive99: NBA Live 99 (PC) on modern Windows

NBA Live 99 for Windows (1998) was built for Windows 95/98, DirectX 6 and 3dfx Voodoo cards, and installs
with a 16-bit setup program that no longer runs on 64-bit Windows. This project keeps the game playable on
current PCs without changing any of its files: a launcher and a set of DLLs sit next to the game and fix it
in memory as it runs.

Release 0.1 (stage 1): the game runs as shipped on Windows 11.

- **No installer, no disc, no registry:** a plain copy of the disc's game folder runs. The CD check is
  skipped in memory, the install paths the game expects are supplied by our core, and the game's registry
  key is answered from memory. Nothing is written to the registry.
- **Graphics:** the game's own 3dfx driver (EA's `3rash\voodooa.dll`) draws through our Glide 2 replacement
  on Direct3D 11, in borderless full screen at the desktop resolution with the picture centred in 4:3. The 3D
  is drawn at full resolution, including the arena behind the menus. The desktop display mode is never
  changed.
- **Menus on Windows 10 and 11:** the menu labels show their proper words (Windows 10 and 11 garbled them),
  and Alt-Tab out of the game and back no longer freezes it.
- **Keys:** Page Down saves a screenshot to `screenshots\` in the game folder; Ctrl+F11 shows or hides the
  frame rate.
- **Settings:** `nba99fix.ini` (the game keeps its own settings in `nba99.ini`).

Later releases are planned to add faster loading from network drives, sound on SDL3, gamepads, smooth frame
pacing, an in-game options menu and the rosters.

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
4. **Copy our release files** into the game folder: download a release zip (Main for playing, Debug for
   testing and bug reports) and copy all of its files and folders next to `NBAWIN.EXE`.
5. Start the game with **`NBALive99.exe`**.

The game's own files are never changed. The install paths the game reads (its `install.dat`) are written as
a copy in `%TEMP%\NBALive99\`. To uninstall our files, delete the ones you copied in step 4 (and
`nba99fix.ini`, `logs\` and `crash\` if they are there).

The shipped 1998-99 rosters stay on the disc (`\NBA99\ORIGDB` and `\NBA99\DBDATA`); the patch replaces some
of the roster tables in `ORIGDB`. A later release explains how to use the shipped rosters as well.

## Known limitations in 0.1

- Loading is slow when the game folder is on a network drive (around 25 seconds for the database at start
  and again when a match loads). A local drive is much faster. A later release fixes this.
- Sound is EA's own DirectSound driver as shipped.
- The arena behind the menus is drawn at full resolution; the menus themselves are the game's own 640x480
  pictures, scaled.

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

## Licence

CC0 1.0 Universal (public domain dedication). See LICENSE.
