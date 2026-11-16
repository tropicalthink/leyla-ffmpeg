#!/usr/bin/env bash
# Pass 2 (deploy/ffmpeg/README.md): static ffmpeg/ffprobe from
# jellyfin-ffmpeg's portable builder, with `leyla-static.patch` applied.
# One self-contained binary each starts in ~4.6 ms where the shared build
# takes ~18 ms (measured 2026-10-05), which Leyla pays on every seek,
# probe, trick-play and fingerprint run.
#
#   deploy/ffmpeg/build-static.sh <linux64|linuxarm64|win64> [out dir]
#   deploy/ffmpeg/build-static.sh <linux64|win64>-helper [out dir]
#
# The `-helper` builds are the encode helper's: the same
# ffmpeg with the `http` and `tcp` protocols, so the desktop app's ffmpeg
# reads its job's source from the server's helper route with the seeks the
# job's own input options make, byte for byte as the server's ffmpeg reads
# the file. The server's builds keep no network protocol at all.
#
# Writes ffmpeg-<target>.tar.xz (ffmpeg, and ffprobe with leyla-thumb/'s
# `-leyla_thumb` mode) and source-<target>.tar.gz. VAAPI and QSV
# still load libva, the VAAPI drivers and Intel's runtime at run time;
# those come from the pass-1 package (build.sh), not from here.
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# The same pin as build.sh, read from it so the two cannot drift.
eval "$(grep -E '^JELLYFIN_(TAG|COMMIT)=' "$here/build.sh")"
name="${1:?linux64, linuxarm64 or win64, or linux64-helper or win64-helper}"
target="${name%-helper}"
helper=0
[ "$name" = "$target" ] || helper=1
case "$name" in linux64|linuxarm64|win64|linux64-helper|win64-helper) ;; *) echo "no target $name" >&2; exit 1 ;; esac
out="$(realpath -m "${2:-$HOME/.cache/leyla-ffmpeg-static/out}")"
CPUS="${FFMPEG_BUILD_CPUS:-6}"
src="${FFMPEG_BUILD_DIR:-$HOME/.cache/leyla-ffmpeg-static}/src-$name"

# jellyfin's builder packs the Windows build with the host's zip, after an
# hour of compiling: say so first.
[ "$target" != win64 ] || command -v zip >/dev/null 2>&1 || {
  echo "win64 needs zip on this host (jellyfin's builder packs with it)" >&2
  exit 1
}

# The builder's dependency layers (makeimage.sh's local buildx cache) are
# kept across rebuilds, one cache per platform (a target and its -helper
# build share it): a change to Leyla's quilt patches recompiles ffmpeg, not
# the cross toolchain (40 min) and the libraries first. Each stage's image
# passes through a local registry to the next, so a stage is found again
# only when its parent comes back the same: the registry keeps one port,
# the images' times are the source's commit time, and they carry no build
# attestation.
cache="${FFMPEG_BUILD_DIR:-$HOME/.cache/leyla-ffmpeg-static}/builder-cache-$target"
rm -rf "$src"
mkdir -p "$out" "$cache"
git clone -q --depth 1 --branch "$JELLYFIN_TAG" https://github.com/jellyfin/jellyfin-ffmpeg.git "$src"
ln -s "$cache" "$src/builder/.cache"
[ "$(git -C "$src" rev-parse HEAD)" = "$JELLYFIN_COMMIT" ] || {
  echo "$JELLYFIN_TAG no longer points at $JELLYFIN_COMMIT: re-tagged upstream" >&2
  exit 1
}
git -C "$src" apply "$here/leyla-static.patch"
# `ffprobe -leyla_thumb`, as build.sh adds it (its thumb_patch).
eval "$(sed -n '/^thumb_patch() {/,/^}/p; /^add_thumb() {/,/^}/p; /^add_grid_lead() {/,/^}/p; /^add_restart_decode() {/,/^}/p' "$here/build.sh")"
add_thumb "$src"
# `-hls_grid_lead`, `-seek_at_pts` and `-skip_nonref_before`, as build.sh
# adds them (README.md).
add_grid_lead "$src"
add_restart_decode "$src"
# vpp_amf's HDR to SDR on the GPU (leyla-vpp-amf-hdr.patch, README.md): AMF
# is a Leyla backend on Windows only, so only the Windows builds (the
# server's and the helper's) carry it, and the Linux archives stay the ones
# built without it.
if [ "$target" = win64 ]; then
  cp "$here/leyla-vpp-amf-hdr.patch" "$src/debian/patches/9003-leyla-vpp-amf-hdr.patch"
  echo 9003-leyla-vpp-amf-hdr.patch >> "$src/debian/patches/series"
fi
# ffmpeg's hardened toolchain is ELF's (-z relro/now, PIE, FORTIFY through
# glibc): MinGW's ld refuses `-z` and configure stops at "unable to create
# an executable file" (2026-10-06). The Windows build goes without it and
# keeps what MinGW's binutils turn on by default (ASLR, high-entropy VA,
# DEP). Done here rather than in the patch, whose hash names the Linux
# builds' image.
if [ "$target" = win64 ]; then
  sed -i 's/ --toolchain=hardened//' "$src/builder/variants/defaults-gpl.sh"
  ! grep -q -- '--toolchain=hardened' "$src/builder/variants/defaults-gpl.sh"
fi

# The helper's ffmpeg reads its source over HTTP from the server
# (a caller that passes `-protocol_whitelist http,tcp`).
if [ "$helper" = 1 ]; then
  sed -i 's/ --disable-network --disable-protocols --enable-protocol=file,pipe,fd,concat,data,subfile/ --disable-protocols --enable-protocol=file,pipe,fd,concat,data,subfile,http,tcp/' \
    "$src/builder/variants/defaults-gpl.sh"
  grep -q 'subfile,http,tcp' "$src/builder/variants/defaults-gpl.sh"
fi

cd "$src/builder"
export SOURCE_DATE_EPOCH BUILDX_NO_DEFAULT_ATTESTATIONS=1
SOURCE_DATE_EPOCH="$(git log -1 --format=%ct)"
# makeimage.sh starts this registry on a random port unless it runs.
docker container inspect ffbuildreg >/dev/null 2>&1 ||
  docker run --rm -d -p 127.0.0.1:45015:5000 --name ffbuildreg registry:2 >/dev/null
# The builder's buildx container compiles the dependencies; hold it to
# $CPUS beside production, as build.sh does for its own container.
./makeimage.sh "$target" gpl &
make_pid=$!
for _ in $(seq 60); do
  docker update --cpus "$CPUS" buildx_buildkit_ffbuilder0 >/dev/null 2>&1 && break
  sleep 5
done
wait "$make_pid"
# The ffmpeg compile itself runs in a plain `docker run`; the patch lets
# it take the same limit.
FFBUILD_DOCKER_ARGS="--cpus=$CPUS" ./build.sh "$target" gpl
ext=tar.xz
[ "$target" != win64 ] || ext=zip
mv artifacts/jellyfin-ffmpeg_*_portable_"$target"-gpl."$ext" "$out/ffmpeg-$name.$ext"

# In a container: some pins are SVN revisions, and the host need not have svn.
docker run --rm -v "$src:/tree" -v "$here:/recipe:ro" -v "$out:/out" debian:bookworm bash -c '
  apt-get update -qq >/dev/null
  apt-get install -y -qq git subversion ca-certificates patch >/dev/null
  git config --global --add safe.directory "*"
  /recipe/static-sources.sh /tree "$1" "/out/source-$2.tar.gz"' _ "$target" "$name"
echo "built $(ls "$out"/*"$name".*)"
