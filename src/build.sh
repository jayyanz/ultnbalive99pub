#!/bin/sh
# Builds the Main and Debug releases with mingw-w64 (i686, 32-bit).
#   ./build.sh            both builds
#   ./build.sh main       Main only
#   ./build.sh debug      Debug only
# Output: dist/Main/ and dist/Debug/ (complete game-folder file sets: launcher,
# DLLs, nba99fix.ini.default) and dist/NBALive99Fix-<version>-<Main|Debug>.zip.
# Releases never contain nba99fix.ini: the launcher makes it from
# nba99fix.ini.default on the first start, so copying a new release over an
# install keeps the user's settings.
# The Debug build also carries 3rash\voodoologa.dll (thrash_log: logs every call
# the game makes to EA's 3D driver; off unless [nba99] ThrashLog=1).
# Tools that are not part of a release (glide2x_log) are built to dist/Tools/.
set -e
cd "$(dirname "$0")"
SRC=$(pwd)
CC=i686-w64-mingw32-g++   # C++ in the restricted subset (README "C++ rules")
CCC=i686-w64-mingw32-gcc  # C, for glide2x_log (a tool, not part of a release)
RC=i686-w64-mingw32-windres
VERSION=$(cat VERSION)
MAJ=$(echo $VERSION | cut -d. -f1); MIN=$(echo $VERSION | cut -d. -f2); PAT=$(echo $VERSION | cut -d. -f3)
STAMP=$(date -u +%Y%m%d.%H%M)
python3 tools/gen_glide_stubs.py glide2x
python3 tools/gen_thrash_thunks.py thrash_log

build() {   # $1 = Main or Debug
    V=$1
    if [ $V = Debug ]; then DBG=1; OPT="-O1 -g -fno-omit-frame-pointer"; STRIP=""; else DBG=0; OPT="-O2"; STRIP="-s"; fi
    DEFS="-DNBA99_DEBUG=$DBG -DNBA99_VERSION_MAJOR=$MAJ -DNBA99_VERSION_MINOR=$MIN -DNBA99_VERSION_PATCH=$PAT -DNBA99_BUILD_STAMP=\"$STAMP\""
    B=$SRC/build/$V; O=$SRC/dist/$V
    rm -rf $B $O; mkdir -p $B $O
    LINE="$VERSION $V ($STAMP)"
    res() {   # $1 name, $2 description, $3 file type (1 exe, 2 dll)
        sed -e "s|@DESC@|$2|" -e "s|@NAME@|$1|" -e "s|@TYPE@|$3|" -e "s|@LINE@|$LINE|" -e "s|@BUILD@|$V|" \
            -e "s|@MAJ@|$MAJ|g" -e "s|@MIN@|$MIN|g" -e "s|@PAT@|$PAT|g" -e "s|@DEBUGFLAG@|$( [ $DBG = 1 ] && echo 1 || echo 0 )|" \
            common/version.rc.in > $B/$1.rc
        $RC -O coff -o $B/$1.res $B/$1.rc
    }
    CF="$OPT -std=gnu++17 -fno-exceptions -fno-rtti -fno-threadsafe-statics -Wall -I$SRC/common $DEFS $STRIP -static-libgcc"
    res NBALive99.exe "NBA Live 99 launcher" 1
    $CC $CF -mwindows -o $O/NBALive99.exe launcher/launcher.cpp $B/NBALive99.exe.res -lversion
    res nba99core.dll "NBA Live 99 core fixes" 2
    $CC $CF -shared -o $O/nba99core.dll nba99core/nba99core.cpp $B/nba99core.dll.res -lwinmm -lversion
    res glide2x.dll "NBA Live 99 Glide 2 on Direct3D 11" 2
    $CC $CF -Wno-unused-function -shared -o $O/glide2x.dll glide2x/gl_api.cpp glide2x/gl_d3d.cpp glide2x/gl_win.cpp \
        glide2x/gl_stubs.cpp glide2x/glide2x.def $B/glide2x.dll.res -ld3d11 -ldxgi -luuid -lgdi32
    if [ $V = Debug ]; then
        mkdir -p $O/3rash
        res voodoologa.dll "NBA Live 99 THRASH call log (Debug tool)" 2
        $CC $CF -shared -o $O/3rash/voodoologa.dll thrash_log/thrash_log.cpp \
            thrash_log/thrash.def $B/voodoologa.dll.res -Wl,--enable-stdcall-fixup
    fi
    cp third_party/CRT-SHADERS-LICENSE.txt $O/              # MIT notice for the CRT shaders in glide2x.dll
    python3 tools/make_ini.py common/nba99fix.ini.in $V > $O/nba99fix.ini.default   # never nba99fix.ini (user settings)
    # Only these files may be in a release: no EA files, no personal art, and never
    # nba99fix.ini (a release that carried it would overwrite the user's settings).
    for f in $(cd $O && find . -type f | sed 's|^\./||'); do
        case "$f" in
            NBALive99.exe|nba99core.dll|glide2x.dll|nba99fix.ini.default|CRT-SHADERS-LICENSE.txt) ;;
            3rash/voodoologa.dll) [ $V = Debug ] || { echo "build: thrash_log is Debug only"; exit 1; } ;;
            *) echo "build: refusing to package $f (not a release file)"; exit 1 ;;
        esac
    done
    rm -f dist/NBALive99Fix-$VERSION-$V.zip
    (cd $O && zip -q -9 -r ../NBALive99Fix-$VERSION-$V.zip .)
    echo "$V $VERSION ($STAMP): $(cd $O && find . -type f | sed 's|^\./||' | tr '\n' ' ')"
}

tools() {   # not part of a release
    T=$SRC/dist/Tools; mkdir -p $T/glide2x_log
    python3 tools/gen_glide_thunks.py glide2x_log
    $CCC -O2 -Wall -shared -static-libgcc -o $T/glide2x_log/glide2x.dll glide2x_log/glide2x_log.c glide2x_log/glide2x.def \
        -Wl,--enable-stdcall-fixup
    cp glide2x_log/glide2x_log.ini $T/glide2x_log/
}

case "${1:-all}" in
    main) build Main ;;
    debug) build Debug ;;
    tools) tools ;;
    all) build Main; build Debug; tools ;;
    *) echo "usage: $0 [main|debug|tools|all]"; exit 1 ;;
esac
