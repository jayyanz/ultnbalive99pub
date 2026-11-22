# Contributing to ultnbalive99

## The rule: no EA assets or EA intellectual property

Everything in this project is informed by studying the game, so "derived from the game" is not the test.
The test is whether a file contains EA's own material.

- **Not allowed:**
  - EA's art, audio, video, text and data files, or bytes copied from them. That includes game files,
    database records, archive contents and byte patterns taken from EA files. Code that must recognise an
    EA file checks its size and a hash instead.
  - EA's rating system, or a model that reproduces it.
  - Anything made from those assets: spliced or decoded speech, upscaled art, converted pictures.
  - These are only ever generated on the user's machine, from the user's own files.
- **Allowed:** our own code, addresses and format knowledge, values we determined ourselves, public facts,
  and lists of function names (`tools/watcom_names.py` writes its list on the user's machine from their own
  exe; the list itself is not committed).

## Never commit

`.gitignore` blocks these; check the file list by hand as well.

- The game folders and images: `Test Environment/`, `ISO/`, `Patch/`, and any game exe.
- `Personal Art/`, `mj.dat`, and anything made from personal or EA art.
- Game-format and derived files: `*.BMP`, `*.FSH`, `*.VIV`, `*.DBF`, `*.CDX`, `*.DCT`, `*.ASF`, `*.BNK`, `*.MAD`,
  `*.CSV`, `*.LAY`, speech banks (`*.HDR`, game `*.DAT`), decoded or spliced speech (`*.wav`), and screenshots and
  frame captures.
- Runtime output: `logs/`, `crash/` (`*.dmp`), `nbatemp/`, `nba99data/`, `screenshots/`.
- Built output: `dist/`, `build/`, release zips and built DLLs. Releases are published separately.
- Output generated from the user's exe: `src/tools/out/`.

## Third-party code

Third-party code or binaries keep their own licence file next to them, and get a row in the
"Third-party components" table in `README.md` and in the release notes.

## Before every push

1. List every file in the commit and check each one against the rule and the never-commit list above.
2. Show the maintainer the file list and wait for approval.
3. Push to a branch, never to `main`. The maintainer reviews the branch on GitHub before it goes into `main`.
