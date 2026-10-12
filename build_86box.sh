#!/bin/bash
# Build the 86Box 6.0 fork used by the tests: release tarball + patches/86box.patch (test control FIFO,
# RTL8139 boot ROM socket, SLiRP TFTP). Output: build/86box/out/src/86Box (SDL frontend, tests);
# GUI=1 also builds build/86box/out-qt/src/86Box (Qt6 GUI, run_86box.sh).
# The patch is the source of truth: a missing tree is recreated from it, and it is re-exported after
# each build so it always matches the built binary.
set -euo pipefail
nb=$(dirname "$(readlink -f "$0")")
src=$nb/build/86box
if [ ! -d "$src/.git" ]; then
  "$nb/fetch_sources.sh"
  mkdir -p "$nb/build"
  tar xzf "$nb/sources/86Box-6.0.tar.gz" -C "$nb/build"
  mv "$nb/build/86Box-6.0" "$src"
  git -C "$src" init -q && git -C "$src" add -A && git -C "$src" apply "$nb/patches/86box.patch"
fi
build() {  # build dir, cmake options
  [ -f "$src/$1/build.ninja" ] || cmake -S "$src" -B "$src/$1" -G Ninja -DCMAKE_BUILD_TYPE=Release -DRELEASE=ON \
    -DDISCORD=OFF -DMUNT=OFF -DSOUNDCANVAS=OFF -DFLUIDSYNTH=OFF -DRTMIDI=OFF -DOPENAL=OFF "${@:2}" > /dev/null
  ninja -C "$src/$1" > "$src/$1/build.log" 2>&1 || { tail -30 "$src/$1/build.log"; echo "86BOX BUILD FAILED" >&2; exit 1; }
}
build out -DQT=OFF
[ -z "${GUI:-}" ] || build out-qt -DQT=ON -DUSE_QT6=ON
git -C "$src" add -N src
git -C "$src" diff > "$nb/patches/86box.patch"
echo "$src/out/src/86Box"
