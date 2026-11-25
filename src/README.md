# NBA Live 99 PC: rebuild source

Plan and findings: project docs `nba99-plan-and-exe-findings.md` and `nba99-stage1-brief.md`. The method, the
build and most of the code come from ultnbalive98 0.7.19 (its `src/README.md` has the history of each part).
Reference exe: EA's v1.1 `NBAWIN.EXE`, 2,488,320 bytes, SHA-256 `3563b929...fda51287d`, PE timestamp
0x36C878D2. Every address here refers to that file; function names are the game's own (see Function names).

## Building: Main and Debug releases

`./build.sh` (mingw-w64 i686 g++ and gcc, Python 3, zip) builds both releases from the same source and the tools;
`./build.sh main`, `debug` or `tools` builds one. The version is in `VERSION`; every build is stamped with the UTC
build time, e.g. `0.1.0 Debug (20261110.1048)`. Output:
- `dist/Main/` and `dist/Debug/`: the full file set for the game folder (`NBALive99.exe`, `nba99core.dll`,
  `glide2x.dll`, `eacsnd.dll`, `SDL3.dll`, `SDL3-LICENSE.txt`, `nba99fix.ini.default`, `CRT-SHADERS-LICENSE.txt`; Debug also `3rash\voodoologa.dll` and `eacsnd_log.dll`), plus
  `dist/NBALive99Fix-<version>-<build>.zip`. A release never contains `nba99fix.ini` or any file not on that
  list (the build refuses), so an update cannot overwrite the user's settings.
- `dist/Tools/glide2x_log/`: the Glide call logger; `dist/Tools/eacsnd_test/sndtest.exe`: sound test for our
  `eacsnd.dll` (neither is part of a release). `./build.sh tools` also runs `eacsnd/test/mixtest.cpp` (3D maths).
- `build/` holds intermediate files only.

| | Main (players) | Debug (testing) |
|---|---|---|
| Logs (`launcher.log`, `nba99core.log`, `glide2x.log`, `timing.log`, `eacsnd.log`) | off (`Log=1` in a section turns one on) | on |
| Crash report | none (normal Windows behaviour) | log entry, `crash\nba99_<date>_<time>.txt` and `.dmp`, game closed cleanly, launcher shows where the report is |
| System report at start (versions of our files, sizes of the game's DLLs, Windows or Wine version, CPUs, RAM, GPUs, displays, nba99fix.ini settings) | no | yes |
| Diagnostics: `CrashTest`, `CrashDumpFull`, `LogFileOpens`, `ThreadDump`, `WatchAddr`, `ProfileStalls`, `ThrashLog`, `EacsndLog`, `[sound] LogEffects` | compiled out | available |
| Compiler | -O2, stripped | -O1 -g, frame pointers, symbols kept |

Logs go to a folder per game start, `logs\<YYYY-MM-DD_HHMMSS>_<version>_<Main|Debug>_<computer>\`;
`logs\latest.txt` names the newest (`common/nba99_build.h`, environment variable `NBA99_LOG_SESSION`).

Shared pieces: `common/nba99_build.h` (version, build name, log defaults, log folders), `common/nba99_cxx.h`,
`common/nba99_stats.h` (timing figures shared by the core and glide2x), `common/version.rc.in` (version resource
in every file), `common/nba99fix.ini.in` (one settings template; `;@debug` / `;@main` blocks per build, made by
`tools/make_ini.py`).

The launcher reads the version resource of `nba99core.dll` and `glide2x.dll` and warns (with a choice to start
anyway) if a file is missing or from another version or build.

## C++ rules (from ultnbalive98 0.7.15)

The release code is C++ compiled with mingw-w64 g++ (i686, `g++-mingw-w64-i686`, the win32 thread model), in a
restricted subset so the files stay msvcrt-only and keep the Windows 98 path open:
- `-std=gnu++17 -fno-exceptions -fno-rtti -fno-threadsafe-statics`, `-static-libgcc`. No `std::thread`, no
  iostreams, little or no STL. The built files must not need libstdc++-6.dll, libgcc_s or libwinpthread-1.dll.
- **Imports must not grow:** every file keeps the same DLL list, and no post-Windows-98 function becomes a static
  import (look newer APIs up at run time). **Exports must not change:** glide2x.dll's Glide names (stdcall
  decoration, `glide2x.def`) and nba99core.dll's names. Compare both tables against the last release after every
  change.
- Every export and every symbol the inline asm thunks use by name has C linkage. Each source file includes
  `common/nba99_cxx.h` first and puts its own code between `NBA99_C_BEGIN` and `NBA99_C_END` (`extern "C"`).
- Files: everything in a release is C++. The compiled sources are `.cpp` (`launcher/launcher.cpp`,
  `nba99core/nba99core.cpp`, `glide2x/gl_api.cpp`, `gl_d3d.cpp`, `gl_win.cpp`, `gl_stubs.cpp` (generated), and
  `thrash_log/thrash_log.cpp` in the Debug build); the files #included into them are `.inc`. Only `glide2x_log/`
  (a tool built to `dist/Tools`, never in a release) and the `test/` programs are C. thrash_log moved to C++ on
  22 Nov 2026 with its 34 exports and its imports (KERNEL32, USER32, msvcrt) unchanged.

Tables in 0.1.0: `NBALive99.exe` imports KERNEL32, USER32, VERSION, msvcrt (no ADVAPI32: the exe check uses our own
SHA-256). `nba99core.dll` imports ADVAPI32, KERNEL32, USER32, VERSION, WINMM, msvcrt and exports
`nba99core_version`, `nba99core_stats`, `nba99core_set_clock_rate`. `glide2x.dll`: 159 Glide exports, imports
unchanged from 98 0.7.19 apart from IsBadReadPtr (judder log, removed). The launcher no longer imports DeleteFileA
(restart loop, removed).

## Function names (tools/watcom_names.py)

The exe was built with Watcom's routine-name option: every function is preceded by its name and one byte with
the name's length (`... check_for_cd 0C | 53 51 52 ...`). `python3 tools/watcom_names.py NBAWIN.EXE` checks the
exe (size and SHA-256), scans the code section (AUTO) and writes to `tools/out/` (not committed):
`nba99_functions.txt` (`0x004013d0 check_for_cd`, one per line) and `nba99_name_functions.py`, a Ghidra script
(Script Manager, run on the loaded exe) that creates and names each function. 4,538 names; the spot checks
(`check_for_cd` 0x4013D0, `camera_setframerate` 0x405890, `drawshadows3rash` 0x4AE0E0) are checked before
anything is written. Two- and three-letter names are only taken after Watcom's padding or the end of the previous
function, because code bytes often look like a short name and its length: the earlier count of 4,558 included
about 20 such false names (`A4`, `Pj`, ...). The runtime library (from about 0x545800) has no names; parts of it
are named in the notes below by what they do.

## Launcher (launcher/launcher.cpp, NBALive99.exe)

Starts `NBAWIN.EXE` from its own folder, suspended, with nba99core.dll loaded by an APC before any game code (as
98). Before that it checks the exe by size and SHA-256 (our own SHA-256, no new imports):
- v1.1 (2,488,320 bytes, `3563b929...`): started.
- v1.0 from the disc (2,478,592 bytes, `bd180729...`): refused, "apply EA's v1.1 patch first".
- The known no-CD copy (`c8a63d26...`): refused. It is the v1.1 exe with one byte changed: `check_for_cd`
  starts with `ret`.
- Anything else: a warning with the choice to start anyway (the core then applies only the general fixes).

Command line (check_commandline, 0x401D00): the arguments are joined and split again, each compared without
case: `NOSOUND`, `WINDOW` (windowed mode flag), `3DFX`, `IDENTIFY` (shows the version), `LOADDUMP[:n]`, or a
driver name `xxx` (if `3rash\xxx.dll` exists it becomes the driver). More arguments can come from the environment
variable `NBALIVE`. With `3dfx` the game does not call getregistry3dsetting and keeps its built-in driver path
`3rash\voodooa.dll` (0x5FD0C4). Without it, the registry value "Thrash Driver" picks `3rash\<value>a.dll`,
falling back to `3rash\softtria.dll`. A driver name starting with `voodoo` is the Voodoo path, `soft` software,
`d3d` Direct3D (getcardtype reads "3D Card" and "Thrash Driver"). The launcher passes `3dfx` (`[launcher] Args`).
`THRASH_DRIVERNAME` cannot choose the driver: main (0x402400) deletes it from the environment at start.

The launcher waits for the game to close and logs its exit code (Debug: a crash message). NBA Live 98's restart
after a settings change (`nba99data\restart.req`) was removed on 22 Nov 2026 (98 leftovers audit); a later
settings screen brings it back if it needs it.

## Core (nba99core/nba99core.cpp, nba99core.dll)

- **Exe check:** PE timestamp 0x36C878D2, the strings `GLOBALTXT_INSERT_CD` (0x5D3510), `install.dat` (0x5D78DC),
  `3rash\voodooa.dll` (0x5FD0C4) and the first bytes of `check_for_cd`. Only then are code patches applied.
- **CD check:** `check_for_cd` (0x4013D0) has no result: its only caller, init_system (call at 0x40193F), ignores
  EAX and calls startthrash next. It takes the drive letter of the CD folder from install.dat (0x67C425),
  needs GetDriveTypeA = CD-ROM (else GLOBALTXT_INVALID_CDROM_DRIVE and exit) and `<CD folder>nbawin.exe` (else
  GLOBALTXT_INSERT_CD, Retry/Cancel). Its first byte becomes `ret` (it has pushed nothing yet), which is exactly
  "CD found". So the repack's `ret` was a correct fix; what was wrong with the repack is that the exe file was
  changed. No other disc check: GetDriveTypeA is called only there; GetVolumeInformationA and
  GetLogicalDriveStringsA are imported but never called from game code.
- **install.dat:** file_checkinstalldat (0x474770) needs `<exe folder>install.dat` (the install root 0x67C525 is the
  exe's own folder, from argv[0], not the registry), checked with access() (GetFileAttributesA). file_init
  (0x473EA0) then reads `install.dat` from the current folder with `fscanf("%s %s %s")`: whitespace-separated
  tokens, so unlike 98 there are no fixed columns. Each line is `HD|CD name folder`; `END` ends the list and is
  followed by the CD folder (copied to 0x67C425) and the language (GERMAN, FRENCH, SPANISH, ITALIAN, else
  English). The speech `update` folders (`updateplayspch`, `updategamespch` and the g/f/s/i language variants) and
  the text-art `updatetextart` are added by file_init itself, after the file. CD lines are read from
  `<CD folder><folder>` (file_getvpath, 0x474850).
  The core writes `%TEMP%\NBALive99\INSTALL.DAT` (the 28 retail entries as HD lines in the retail layout, then
  `END`, `.\`, `english`) and redirects the exe's CreateFileA, GetFileAttributesA and FindFirstFileA for any
  path ending in `install.dat`. The CD folder must not contain spaces (it is one token): `.\` (the game
  folder, relative) is used, which is also where UserInput_LoadConfig reads `inpdevs.ini` in one of its cases.
  A quoted install root after END, as in the repack's file, is read as the CD folder and a path with spaces
  breaks into several tokens; that only worked because the repack's exe skipped the CD check.
- **Registry:** the exe calls the registry in three places: getregistry3dsetting (HKLM `SOFTWARE\EA Sports\NBA
  Live 99`, "Thrash Driver"; skipped with `3dfx`), getcardtype ("3D Card" and "Thrash Driver"; only for a d3d
  driver) and NET_setautodial (HKCU `Software\Microsoft\Windows\CurrentVersion\Internet Settings`, reads and
  writes EnableAutodial). The strings "Install Dir", "CD Drive" and "Thrash Resolution" are not in the exe at all:
  the repack's "Thrash Resolution" = 800x600 did nothing. The core hooks RegOpenKeyExA, RegQueryValueExA,
  RegSetValueExA, RegFlushKey and RegCloseKey in the exe: the game's key (any case, with or without
  WOW6432Node) is a virtual key (Install Dir, CD Drive, Language, Thrash Driver, 3D Card, 3D Device Description,
  Group, Hardware Acceleration, Triple Buffer, D3D Device); writes to it stay in memory for the session. Other
  keys are read normally; writes to them are logged and not made unless `[nba99] RegistryWrites=1`. Every open,
  read and write is logged with its caller.
- **Memory:** GlobalMemoryStatus clamped to 512 MB.
- **DirectDraw (KeepDesktopMode=1):** the exe imports DirectDrawCreate and DirectDrawEnumerateA but never calls
  them. VOODOOA.DLL and CTRLS.DLL (the front-end control library) import DirectDrawCreate. The core hooks the
  exe's LoadLibraryA; every module the game loads gets its DirectDrawCreate import (and its own LoadLibraryA)
  hooked, and the DirectDraw interface is patched as in 98 (normal cooperative level, no SetDisplayMode). Under
  Wine VOODOOA calls SetCooperativeLevel(0x11, fullscreen + exclusive), made normal; no SetDisplayMode call was
  seen.
- **LoadLibrary log:** shows which file each name found, e.g. `LoadLibrary("glide2x.dll") from voodooa.dll+0x1046:
  <game folder>\glide2x.dll`: VOODOOA loads glide2x.dll by name, so the game folder (the exe's folder) is searched
  first and ours is used.
- **Threads:** every CreateThread from the exe and every timeSetEvent callback is logged. The game's threads start
  through the runtime's thread starter (entry 0x566ED0, the real function in the argument block) and
  EACT_CreateThread / Playoff_CreateThread. 98's CPU pinning (`PinGameThreads`, `GameCPU`) and safe
  termination (`SafeTerminate`) were carried over switched off and removed in 0.3.0 (maintainer's decision, 25 Nov 2026): in every
  99 log (both test PCs, up to 7.7 hours) the game never called TerminateThread (its one call site, 0x5671C1,
  is a runtime helper), never set an affinity itself and ran on all CPUs without a freeze. TerminateThread calls
  are still logged, and the process gets all CPUs back if a compatibility setting started it on one (seen on the
  handheld test PC).
- **nbatemp:** the stale `nbatemp\` cleanup from 98 (the game uses the same folder name).
- **Timing:** `timing.inc`: the first timer callback is counted as the game clock (Wine: 120 ticks/s, callback
  0x547E50, EA's timer server as in 98: a one-shot timeSetEvent re-armed from GetTickCount), the per-second sampler
  and `timing.log`, and the machine report. PreciseClock is on by default since 0.3.0 (see Frame pacing below).
  The shared stats struct (`common/nba99_stats.h`) restarted at version 1 on 22 Nov 2026; version 2 (0.3.0) adds
  `r_spf`.
- **Simulation steps and input frames (0.3.0, counting only):** 99's match loop has the same shape as 98's.
  `normalgameloop` (0x4382C0) runs one simulation step, `singleailoop` (0x437C80), for every input frame
  (`ctrlvalsready`, then draws with `drawview`). Its first five bytes (push ebx, ecx, esi, edi, ebp) jump to a thunk
  that counts the step and does the same pushes. The input task `ctrlinterrupt` (0x40D050) counts timer ticks in
  0x5FE4D8 and samples the controllers every 2 ticks (4 online), resetting it to 0, like 98's 0x42E4A0; its
  registration at 0x40CECB and removal at 0x40CF00 (`mov eax, 0x40D050`) load our wrapper, which counts a frame when
  0x5FE4D8 is 0 after the task. Both are checked byte for byte. `timing.log` has `steps` and `inp` per second and
  the frames that showed 0/1/2/3+ new steps; the overlay is three lines again (steps per second; steps per frame)
  with bars coloured by steps (grey 0, green 1, yellow 2, red 3+).
- **File totals (0.3.0, Debug, profile.log):** once a minute a "minute" block with every file call the exe made
  (main thread and other threads, count and time, KB read and written) and the 12 files opened most often, for the
  network-drive measurements. The exe has the same file patterns as 98: EA's file library resolves names with
  GetFullPathName, GetVolumeInformation and FindFirstFile (0x590300), and the game uses `nbatemp\` and the speech
  `*DATA.VIV` / `*HDRS.VIV` banks; the 98 fixes (drive info cache, temp file buffer, speech in memory) are ported only
  if these figures show the same costs.
- **ThrashLog=1 (Debug):** the driver path at 0x5FD0C4 becomes `3rash\voodoologa.dll` (still starting with
  `voodoo`), and the virtual registry answers "Thrash Driver" = `voodoolog`.

## Front-end mouse (nba99core/mouse.inc, MouseMap=1)

The 99 front end is built from real Win32 child windows: eacthdlr_create* (CreateWindowExA, classes such as
NBABUTTON) places each control in the game window's client area in 640x480 game pixels; CTRLS.DLL draws them
(_RenderButton) and hit-tests them (_SetButtonHitFlags, from the message's lParam and wParam). EA's mouse hook,
EACT_MouseHookProc (WH_MOUSE, set in EACT_Initialize), takes the cursor in screen pixels, uses ScreenToClient, keeps
the cursor in 0..636 x 0..476 (MapWindowPoints and SetCursorPos), finds the control with WindowFromPoint and
ChildWindowFromPoint, and tracks the control under the mouse through MOUSEHOOKSTRUCT.hwnd. Button messages it lets
through to the window Windows picked. With the game window desktop-sized and the picture scaled (glide2x), the real
cursor no longer lines up with the controls: the cursor was held in the top-left 640x480 and clicks missed.

The core keeps the game in real screen pixels (as glide2x's ClientToScreen already does for the game window) and
moves points where they cross into the 640x480 world, in the exe and CTRLS.DLL: ScreenToClient, MapWindowPoints from
the screen and WindowFromPoint (picture -> game pixels); ClientToScreen and MapWindowPoints to the screen (game
pixels -> picture). The hook procedure runs through a wrapper: the game's hook sees the real point but, as the
window, the control under the matching picture point (or the control with the capture). If it lets a mouse message
through, the wrapper posts it to that control in its client pixels (wParam from the message itself: the key state
still showed the button down on WM_LBUTTONUP, so the first version never released the button) and drops the
original. Nothing changes with a 640x480 client. Debug: the first 12 hook calls and the first clicks are logged with
the mapped point and the window there; `MouseTrace=N` logs N hook calls after the first click.

Wine (1920x1080, picture 1440x1080 at 240,0): Options, Controller Setup, Starting Lineup, the pause menu, Quit Game,
and Exit Game on the main menu all work by mouse. For comparison, a 640x480 window (glide2x Windowed=1,
Width=640) needs no mapping and gives the same message sequence.

## Front-end labels (nba99core/wintext.inc, WindowTextFix=1)

On Windows 10/11 many menu labels showed garbled words ("P\xC2Q", "I") while Wine drew them correctly (second and
third Windows runs, 11 Nov 2026; not the full-resolution pause feature). The labels are NBATEXTCONTROL and
NBAMULTIBUTTON child windows (EACT_TextControlProc 0x51C250, 0x51E080). The layout code sets their text with
SetWindowTextA (Layman_SetText 0x4C5370) and the string from the CSV language tables (Language_LoadString). On
WM_SETTEXT a label without its own buffer keeps the lParam pointer (0x51D06A: mov [data+0x28], lParam) and draws
from it later. "P\xC2Q" is the bytes of 0x0051C250 (50 C2 51 00): the pointer ends up on memory that later holds
the control's own window procedure address.

The text is set from another thread than the one that owns the controls. Windows 98 and Wine pass the caller's
pointer; Windows NT copies WM_SETTEXT into a temporary buffer for the receiving thread and frees it after the
message, so the label keeps a stale pointer. The core hooks the exe's SetWindowTextA, SendMessageA and
SendNotifyMessageA: a WM_SETTEXT to a window whose procedure is in NBAWIN.EXE or CTRLS.DLL is given to that
procedure with CallWindowProcA and the game's pointer, directly on the same thread, or through a hidden
message-only helper window of the owning thread (WM_APP messages are never copied; the send still waits, like the
original). The exe's CreateWindowExA is hooked to make one helper for each thread that creates windows. Debug logs
the first 8 redirected messages and any passed on unchanged.

## Alt-Tab in the menus (nba99core/alttab.inc, AltTabFix=1)

Alt-Tab froze the game in the menus (desktop test PC, 22 Nov 2026). Focus log: on Alt-Tab Windows minimised the borderless,
top-most game window (WM_WINDOWPOSCHANGED to -32000,-32000 with a state change; no ShowWindow or system command from
the game) and sent WM_ACTIVATEAPP 0; on the way back it was restored and WM_ACTIVATEAPP 1 arrived. Thread dumps: the
window thread idle in its GetMessageA loop (0x54A93E), the main thread waiting from ferender_drawframe+0x186 for good.

ferender_drawframe (0x487780) posts 0x5FE to the game window each frame; the window thread answers in
eacthdlr_renderupdate (0x516400) by setting one of two events (0x6218A0 draw, 0x6218A4 skip) and the main thread
waits for either (WaitForMultipleObjectsEx, INFINITE). When IsIconic(game window) is true or the game is not the
active app (dword 0x6E466C = 0) it skips the post and only waits. The window's WM_ACTIVATEAPP handler (0x549D40,
registered for 0x1C at 0x54AC3C) only sets 0x6E466C to wParam; eacthdlr_activateapp (0x515DA0), which also sets the
skip event, has no reference in the exe. So losing the focus stops the front end for good.

The core hooks the exe's IsIconic and acts on the one call from ferender_drawframe (return address 0x48779B, v1.1
exe only): minimised and active, it answers FALSE so the front end keeps drawing; not the active app, it waits 15 ms
and posts 0x5FE itself, so the window thread sets the skip event and the front end idles until WM_ACTIVATEAPP 1. The
other callers keep the real answer: the window thread (0x54AB0B) calls SetForegroundWindow only when the window is
not minimised, which must not pull the game back in front of the player; 0x5504A5 is network code. First build
(IsIconic only) still hung after Alt-Tab back: the main thread had already stopped on 0x6E466C = 0.

## glide2x.dll (from 98 0.7.19)

Started from 98's code (Glide 2.60 on Direct3D 11, 159 exports, borderless 4:3 output, mouse mapping, overlay, CRT
filter, screenshots), renamed, with the 99 changes below. The wrapper is specific to NBA Live 99.

**98 leftovers audit (22 Nov 2026, project doc `nba99-98-leftovers-audit.md`).** Removed: the judder log and
presentation statistics (`gl_judder.inc`, `JudderLog`; they read 98's camera and object table), widescreen
(`Widescreen`, the 4:3-area layer, the "ws probe"; read 98's screen state byte), the gamepad menu box (`nav_draw`,
`ps_navbox`), the options menu's live CRT switch, the distance detail feed (`render_h`), `game_v11`, `CullInvert`,
the steps line and step colours in the overlay, and `glide2x_log/test/aff_test.c`. Nothing in glide2x reads game
memory now. Kept: the frame pacing locks (`StepLock`, `DisplayLock`, `QueueSkip`, with the core's `PreciseClock`),
switched on in 0.3.0 once the core counted 99's steps and input frames (see Frame pacing below).

Changes for 99:
- **NBA Live 98's full-resolution pause pictures removed (FullResPause).** The 99 front end draws its menus over a
  live 3D arena: every frame the game reads the 640x480 picture back (LFB read of the GPU-rendered buffer) and writes
  the composed menu frame (LFB write). The 98 feature kept the full-resolution frame at the read and, at the write,
  showed it wherever a 640x480 pixel came back unchanged. It cannot work for 99's opaque menus: wherever a menu pixel
  happens to equal the arena pixel under it (black boxes over dark arena, for example) the full-resolution arena shows
  through as moving polygon edges (the shimmer, seen on the handheld test PC and again at 4K on 22 Nov 2026). Its
  darkened-background test added more false matches inside dark panels, and large panels (DETAILS changes 55 to 65% of
  the picture) fell back to 640x480 anyway. The 99 wrapper shows the game's own 640x480 menu picture, scaled; the
  `FullResPause` key is gone.
- **Menu layer (glide2x/gl_menulayer.inc, `[glide] MenuFullRes=1`), 22 Nov 2026.** The 99 replacement: the arena
  stays at full resolution and only the menus come from the game's 640x480 picture. At the LFB read the full-resolution
  frame is kept and the picture handed to the game is marked: the lowest bit of R, G and B is set to a fixed pattern per
  pixel (one of 001 to 110, never all 0 or all 1), so black, white and other plain menu colours always differ from the
  arena under them. The pattern flips to its opposite on every read, so any menu colour comes back changed at least
  every other frame, even over a still arena (with a fixed pattern a few dark menu pixels matched a still, dark roof
  and showed its lights as white dots, 22 Nov 2026). At the LFB write a pixel the game changed counts as menu until
  it has come back unchanged for 30 frames in a row. More than 85% changed
  is a new picture and is shown whole. Debug logs the first 20 writes and every 600th (pixels changed, menu,
  full-resolution).
- **Keys through a keyboard hook.** Page Down (screenshot to `screenshots\NBA99_<date>_<time>.png`), Ctrl+F11
  (overlay) and Ctrl+F10 (Debug draw trace) were caught in the game window's procedure; in the 99 front end the
  focused child control gets the key messages, so they did nothing in the menus. A WH_KEYBOARD hook on the game
  window's thread now catches them for every window of that thread and keeps them from the game. glide2x.dll's
  imports gain SetWindowsHookExA, UnhookWindowsHookEx and CallNextHookEx (USER32, Windows 95 functions).
- **glide2x stays loaded until the game exits.** At quit VOODOOA calls grSstWinClose and grGlideShutdown and
  frees glide2x.dll while the game window still exists and its thread still dispatches messages. In the second
  Windows run (ThrashLog=1, 11 Nov 2026) that crashed after THRASH_restore: access violation in USER32 dispatch to
  an address in the unloaded glide2x (it was missing from the minidump's module list). `win_attach` now takes one
  extra reference on glide2x.dll (LoadLibraryA of its own file, never released; not GetModuleHandleEx with PIN,
  which is Windows XP and later), and `win_detach` puts the original window procedure back if ours is still on
  top; if another procedure was put in front of ours, ours stays in the chain and passes every message straight to
  the original.

Under VOODOOA (Wine): VOODOOA loads glide2x.dll twice (grGlideInit, grGlideShutdown, unload, then load again),
then calls grSstWinOpen with the game's window, res 7 (640x480), and draws. Every Glide name VOODOOA asks for (60,
including grSstScreenWidth/Height, grSstVRetraceOn, grBufferNumPending, grSstControl and
grTexDownloadMipMapLevelPartial) is implemented in glide2x.

## THRASH call log (thrash_log/, 3rash\voodoologa.dll, Debug)

Sits between the exe and EA's THRASH driver and forwards every call unchanged. `[nba99] ThrashLog=1` selects it; it
loads `3rash\voodooa.dll` (`[thrashlog] Real=`) and writes `thrash_log.txt` in the session's log folder:
- every call to the setup functions (about, init, is, restore, selectdisplay, setvideomode, window, clip) with
  arguments, result and the exe address that called it;
- the first 40 calls of every other function (`[thrashlog] First=`; lockwindow, unlockwindow, readrect, writerect
  200), the same way;
- a TICK line for every second with calls in it (calls per function, pageflip interval min/avg/max ms);
- call counts at exit.
The thunks (`tools/gen_thrash_thunks.py` from `thrash_exports.txt`) log the call, call the real driver with the same
stack, log the result and return to the game; the game's return address is kept on a small per-thread stack.

First Wine log: setstate, about, init, `THRASH_setvideomode(1, 2, 1)` from 0x58B148 (the THRASH library's own
mode numbers, from the call 640, 480, 16 in startthrash at 0x401731/0x401748), window 1/2/0 with lockwindow and
unlockwindow, then clearwindow, settexture, drawtri, flushwindow, sync and readrect (RunAlphaTests at 0x4BFD90).

## Sound DLL call log (eacsnd_log/, eacsnd_log.dll, Debug)

Sits between the exe and the sound DLL and passes every call on unchanged; first step of 0.2.0 (sound on SDL3).
`[nba99] EacsndLog=1` makes the core redirect the exe's `LoadLibraryA("eacsnd.dll")` (0x56AD31, exact name only) to
`eacsnd_log.dll` in the game folder; the proxy loads `eacsnd_ea.dll` (EA's DLL, copied to that name before ours
replaced it) or, without that file, `eacsnd.dll` (`[eacsndlog] Real=`), by full path on the first call, and writes
`eacsnd_log.txt` in the session's log folder:
- every call to dllversion, setfunctions, caps, start, stop and the record functions, with arguments, result, the
  exe address that called and the thread;
- the first 400 calls (`[eacsndlog] First=`) of vol, pos3d, rate, create3dbuf, removebuf, play3d, stopbuf and of the
  callbacks voice_ended, position (with the x y z it returned) and decode, the same way; every message callback;
- a TICK line for every second with calls in it: counts per function and callback, mixer frames, serve gap
  min/avg/max ms;
- call counts at exit.

EA's DLL (99, 42,496 bytes, Watcom, imports only `DSOUND!DirectSoundCreate`) has 16 stdcall exports:
`_iSNDdllversion@0` (returns 0x00100003), `_iSNDdirectsetfunctions@32`, `_iSNDdirectcaps@4` (format probe, bit
0x800 = 22050 Hz), `_iSNDdirectstart@8`, `_iSNDdirectstop@0`, `_iSNDdirectrecordstart@16` / `recordpacket@4` /
`recordstop@0` (stubs: -1, 0, 0), `_iSNDdirectserve@0`, and the 3D set `vol@8`, `pos3d@12`, `rate@8`,
`create3dbuf@28`, `removebuf@4`, `play3d@28`, `stopbuf@4`. serve works as in 98: it fills the stream from callback 1
(the game's mixer) and calls callback 5 (the game's tick) every rate/100 samples. The 3D voices (up to 16) are
DirectSound 3D buffers made from sample sources (up to 64) that the game's decoder (callback 8) writes; pos3d gets
the position from callback 7 and scales it by 1/65536; the listener is never set. Effects played through the
game's own mixer never reach the DLL (the effect log for 0.4.0 needs an exe hook for those).

## eacsnd.dll on SDL3 (eacsnd/, 0.2.0)

Our replacement for EA's `eacsnd.dll`: the same 16 exports, names and ordinals (checked against EA's export table),
on an SDL3 audio stream, so DirectSound is no longer loaded. Needs `SDL3.dll` (32-bit, SDL 3.2.26, zlib licence,
`third_party/`, in both releases) next to the exe. Settings `[sound]`: `LatencyMs` (40), `DeviceFrames` (0),
`Device` (part of a device name; empty = the Windows default, followed when it changes), `Volume` (100),
`Mode3D` (speakers, stereo, off), `Log`, `LogEffects` (Debug).

- **Stream (as 98):** all mixing happens in `serve()`, which the game calls from its timer (0x56B174) under its sound
  lock. The queue is kept `LatencyMs` ahead with the game's mixer (callback 1), and the game's 100 Hz tick
  (callback 5) is called between mix chunks at EA's sample-accurate spacing (`rate * n / 100`, rounded down to 16
  frames, the rounding carried, counters reset after 30000). Format as the game asks: it passes the caps bits
  back to start (logged with EA's DLL on Windows 11: 0x808), so 22050 Hz stereo 16-bit (EA's order of preference
  is kept). The output is float at the game's rate with the
  device's channel count (up to 8); the game's stereo goes to front left and right; SDL converts to the device.
- **3D voices:** EA's DLL reports 0x20000 only when DirectSound has hardware 3D buffers; on Windows 11 it has
  none, so with EA's DLL the game used no 3D voices (logged: caps 0x3E0F, start 0x808, no 3D call). Ours reports
  0x20000, but the first game run with it (25 Nov 2026, caps 0x23E0F) still started with 0x808 and made no 3D
  call: caps alone do not turn the 3D voices on, and the game mixes every sound into its own stereo stream. What
  enables them (a game setting or another start flag) is still to be found; the 3D path below is ready for it. Ours keeps up to 63 sources (16-bit mono, written by the game's decoder, callback 8, exactly
  as EA's DLL asked for them) and 16 voices, mixed in `serve()` after the game's mix: linear resampling at the voice's
  rate, the game's volume 0..127 as a linear amplitude (EA's table is 20·log10(v/127) in hundredths of a dB),
  gain changes ramped over 64 frames. Position: callback 7 turns the game's two angles into a direction (EA's
  DLL scaled it by 1/65536 and passed it to SetPosition; the listener was never set), so it is a unit vector in
  DirectSound's axes (+x right, +y up, +z ahead) and only the direction counts; distance would follow
  DirectSound's default rolloff (1/d beyond 1). `eacsnd/mix3d.inc` pans it: layouts with rear speakers (4.0, 5.0,
  5.1, 6.1, 7.1) get VBAP between the two speakers either side; front-only layouts get a constant-power
  left/right pan from the sideways component (a sound behind is heard on its side). `Mode3D=stereo` uses the
  left/right pan on any layout; `Mode3D=off` puts every 3D sound in the centre. Finished voices are stopped and
  reported to the game (callback 2) from `serve()`, as EA's DLL did after a fill; `stopbuf` also reports, as EA's.
  Game code is never called while our voice lock is held.
- **Recording:** `recordstart` -1, `recordpacket` 0, `recordstop` 0, exactly as EA's DLL (never finished by EA).
- **Not ported from EA's DLL:** the abort callback (3) on errors; ours logs the error (first 200) and returns
  the failure value instead of stopping the game.
- **Logs:** `eacsnd.log` (Debug, or `Log=1`): settings, output devices, device and format, per minute the serve
  calls, underruns, longest gap between serves, 3D voices started and the most at once. `effects.log` (Debug,
  `LogEffects=1`): every 3D source loaded (id, the game's two numbers, sample pointer, length, rate) and every 3D
  sound started (voice, source, rate, volume, loop, the game's angles and the direction in degrees), for the
  0.4.0 rumble work. Effects the game mixes itself (callback 1) never reach the DLL; logging those needs an exe hook
  (to do once the call log shows which).
- **Tests:** `eacsnd/test/mixtest.cpp` (built and run by `./build.sh tools`): panning on every layout, constant
  power, Mode3D, distance, volume curve, resampling, looping, end of sound, gain ramp. `dist/Tools/eacsnd_test/
  sndtest.exe` (Windows, in the game folder): version, caps, start, a sound placed front, right, behind and left
  and circling, the end-of-sound report, and 100 ticks per second of sound.
- The launcher's version check now includes `eacsnd.dll` (EA's has no version resource, so a release copied
  without it is warned about).

## File speed (0.3.0: CacheDriveInfo, BufferTempFiles, SpeechInMemory)

Ported from NBA Live 98 after measuring 99 (`profile.log` minute blocks, network drive against local drive, 22 Nov
2026). EA's file library opens every file under a per-drive lock (0x590A8D) and asks for the drive's free space
inside it (0x590ADC); the streaming thread reopens the commentary banks for every line; the team load builds
`nbatemp\$nbalst*.tmp` in about 950 small writes. From a network drive that gave a 100 to 1000 ms frame every few
seconds in matches (the main thread waiting on the lock) and a 30 s team load; from a local drive no slow frames in
play. All three default to on in both builds:
- `fscache.inc` (`CacheDriveInfo`): free space kept 5 s per drive root, volume details and drive type for the session.
- `tempbuf.inc` (`BufferTempFiles`): files in `nbatemp\` opened for writing are kept in memory while open and written
  in one go on close (as 98; an overlapped or unbuffered temp open, not seen, is logged once and left alone).
- `spchmem.inc` (`SpeechInMemory`): every file in `speech\<folder>\`, its `update\` and `user\` (about 142 MB for
  the English install) is read by an idle-priority thread 3 s after start; read-only opens of a file in memory get
  an event handle and every file call on it is answered from memory (overlapped reads complete at once; 13 calls
  hooked, as 98 plus GetOverlappedResult). New for 99: read-only opens that Windows answered "not found" under
  `speech\` or of a `.dat`/`.hdr` name in the game folder (the per-player `user\`, `update\` and bare-name look-ups)
  are answered "not found" from a list; any write open, delete or move clears the list.
The counts are logged in `nba99core.log` when the game closes.

NBA Live 99 also maps some speech banks into memory (EA's file library, 0x590BA4: CreateFileMappingA with
`PAGE_READONLY | SEC_COMMIT`, then MapViewOfFile `FILE_MAP_READ` of the whole file), which 98 never did. Refusing
the mapping aborted the match load ("typeofbigfile - NO BIG FILE SET", 23 Nov 2026). The first mapping of a bank
now moves its memory copy into an unnamed page-file section (moved, not doubled) and every mapping gets a duplicate
handle of that section.

## Frame pacing (0.3.0: PreciseClock, StepLock, DisplayLock, QueueSkip)

Measured on 99 before switching on (timing.log, 22 to 25 Nov 2026, handheld test PC at 59 Hz and desktop test PC at 60 Hz): the
game clock ticks about 120 times a second but 1 to 21 ms apart (EA's timer server re-arms a one-shot timeSetEvent
from GetTickCount against a 16.16 deadline at 0x6E4010, as in 98); matches run 60 steps and 60 input frames a second;
the frames shown drift against the steps, so in some seconds every frame showed one step and in others 15 to 20 a
second showed none and as many showed two (one 25 Nov quarter: 11,683 frames with one step, 920 with none, 735 with
two or more). It showed as judder in animation and camera movement. 99's timer and match loop have the same shape
as 98's, so the 98 locks are switched on unchanged, all default 1:
- `[nba99] PreciseClock`: the exe's GetTickCount is a QueryPerformanceCounter millisecond clock, timer resolution
  1 ms; the clock speed can be steered (`nba99core_set_clock_rate`, 0.97 to 1.03).
- `[glide] StepLock`: in a match each swap waits for one more input frame than the last (at most 50 ms), so every
  frame shows exactly one step; 2 shows every other step (30 frames a second).
- `[glide] DisplayLock`: steers the game clock to the display (DWM timing) so presents fall halfway between two
  refreshes: 59.94 Hz runs the game 0.1% slower; 120 Hz shows each frame for two refreshes. Other rates (75, 100,
  144 Hz) leave the lock off: the step lock still presents an even 60 frames a second, which a variable refresh
  screen (G-Sync, FreeSync) shows as they come; a fixed-refresh screen at such a rate shows them unevenly.
  When the lock starts (match start) or the averaged phase drifts more than 0.2 from the middle, it steers 20 times
  harder (at most 1.5%) until the phase is within 0.08: about half a second instead of 1 to 2 s near a refresh.
- `[glide] QueueSkip`: with VSync=1, after 30 blocked presents one frame is not shown to empty the display queue.

## glide2x_log (dist/Tools/glide2x_log)

As in 98: a stand-in glide2x.dll that logs and forwards to `glide2x_real.dll`. Not needed for 0.1 (thrash_log
covers the layer above), kept for comparing with other wrappers.

## Tests (Wine 9.0, Xvfb 1920x1080, software Direct3D)

- Launcher: v1.1 accepted; v1.0 and the no-CD exe refused with their messages.
- Core: all hooks in place, install.dat generated and read (once by file_checkinstalldat, then file_init), CD check
  skipped, VOODOOA loaded, glide2x.dll found in the game folder, DirectDraw cooperative level made normal.
- First light: the game's own error screens drawn through VOODOOA and glide2x, borderless at 1920x1080 with the
  picture at 1440x1080, centred.
- thrash_log: 34 of 34 functions found and forwarded; the call pattern above.
- Full install (from the clean test install): legal screen, intro movie, main menu (Game Setup) centred 4:3,
  Options by mouse, Controller Setup, Starting Lineup, an exhibition match at 1440x1080 (12 fps under software
  rendering), pause menu (Esc), Quit Game, Exit Game: the game closed with exit code 0, `nbatemp` removed, only the
  game's own `nba99.ini` written. Sound ran into a null device (the container has none).
- After the Windows run: 98 full-resolution pause pictures off (later removed), Page Down saves screenshots in the
  intro and in the menus.
- The white bar to the right of screen titles in the Debug screenshots is the game's own title banner, half covered
  by the overlay panel.

## First Windows 11 run (0.1.0, handheld PC, Ryzen Z1 Extreme, 10 Nov 2026)

Install on a network drive. Launcher, exe check, install.dat, CD skip, virtual registry, VOODOOA with our
glide2x.dll and the desktop mode all as under Wine; exit code 0. Menus and matches at 60 fps (59 Hz desktop).
From the stall profile:
- File access on the network drive dominates the waits: database load at start (DBF/CDX reads about 4 ms each,
  about 25 s), the match loading (nbatemp\$nbalst*.tmp writes, opens each followed by GetDiskFreeSpace, about 25 s),
  and deleting nbatemp at the end. These are the costs 98's CacheDriveInfo and BufferTempFiles fixed; the brief's
  condition for bringing them into 99 is met (import-table hooks, no addresses).
- 15 fps stretches are the game's own loops: the instant replay (doreplay, replaytiming, speech_update) and the
  intro movie; 9 s of full CPU on the legal screen (license_screen waiting in any_input) and in
  netspeech_dowelcometogame (event_intro) at the start of a match.
- The game only touched the registry for NET_setautodial (read; no write that session).

## Windows 11 runs at 4K (0.1.0, desktop PC, Ryzen 7 9800X3D, RTX 5090, 3840x2160, 22 Nov 2026)

Install on a network drive, launcher renamed (the name does not matter). Builds 20261111.1218 to
20261122.0118: front-end labels correct (every WM_SETTEXT through the helper window, none passed on); the 98
full-resolution pause pictures shimmered and were removed; the menu layer gives the arena at 2880x2160 behind the
menus, no shimmer or stray pixels in the screenshots, menu area steady at 206,520 pixels a frame on Game Setup
(DETAILS); exit code 0.

Release checks (22 Nov 2026): every file in a release is C++ (thrash_log moved from C); against 98 0.7.19 built from
source, no file imports a new DLL and every new import is a Windows 98 function (glide2x's GetModuleHandleExA from the
quit fix replaced); exports unchanged (nba99core 3, glide2x 159, voodoologa 34).
