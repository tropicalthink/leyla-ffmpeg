#!/usr/bin/env bash
# Leyla's ffmpeg for the macOS server (the macOS
# server), built by hand on a Mac:
# jellyfin-ffmpeg at build.sh's pin, through jellyfin's own macOS builder
# (builder/buildmac.sh, builder/Buildmac.md), with what the Linux and
# Windows static builds carry (build-static.sh): leyla-static.patch (no
# network protocol, no fdk-aac), ffprobe's `-leyla_thumb`, `-hls_grid_lead`
# (HLS restarts' A/V sync), `-seek_at_pts` and
# `-skip_nonref_before`. VideoToolbox is jellyfin's, as in its own builds.
#
#   deploy/ffmpeg/build-mac.sh [out dir]
#
# Builds arm64 and then x86_64, each from an empty prefix as Buildmac.md
# asks, and joins them with lipo into one universal ffmpeg and ffprobe:
# <out dir>/ffmpeg-mac-universal.tar.xz (default ~/leyla-ffmpeg-mac).
#
# Needs the full Xcode (the Metal SDK: `xcrun -sdk macosx metal -v`),
# Homebrew, and /opt/ffbuild writable by this user, once:
#   sudo mkdir -p /opt/ffbuild && sudo chown "$USER" /opt/ffbuild
# Runs with macOS's own bash 3.2 and BSD tools.
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
eval "$(grep -E '^JELLYFIN_(TAG|COMMIT)=' "$here/build.sh")"
# The patch helpers build.sh and build-static.sh share.
eval "$(sed -n '/^thumb_patch() {/,/^}/p; /^add_thumb() {/,/^}/p; /^add_grid_lead() {/,/^}/p; /^add_restart_decode() {/,/^}/p' "$here/build.sh")"
out="${1:-$HOME/leyla-ffmpeg-mac}"
mkdir -p "$out"
out="$(cd "$out" && pwd)"
work="$out/work"

[ "$(uname -s)" = Darwin ] || { echo "build-mac.sh runs on a Mac" >&2; exit 1; }
xcrun -sdk macosx metal -v >/dev/null 2>&1 || {
  echo "no Metal SDK: install the full Xcode and run sudo xcode-select --switch /Applications/Xcode.app/Contents/Developer" >&2
  exit 1
}
[ -w /opt/ffbuild ] || {
  echo "/opt/ffbuild is not writable: sudo mkdir -p /opt/ffbuild && sudo chown \"\$USER\" /opt/ffbuild" >&2
  exit 1
}

# Every make and ninja in jellyfin's builder runs `-j$(nproc)`, and macOS
# has no nproc, so each would run `make -j`: as many jobs as there are
# files, on a Mac that overheats and resets past 3 (CLAUDE.md). Here nproc
# answers 3, as CARGO_BUILD_JOBS does for build.sh.
mkdir -p "$work/bin"
printf '#!/bin/sh\necho 3\n' > "$work/bin/nproc"
chmod +x "$work/bin/nproc"
export PATH="$work/bin:$PATH"

for arch in arm64 x86_64; do
  src="$work/src-$arch"
  rm -rf "$src" /opt/ffbuild/prefix
  git clone -q --depth 1 --branch "$JELLYFIN_TAG" https://github.com/jellyfin/jellyfin-ffmpeg.git "$src"
  [ "$(git -C "$src" rev-parse HEAD)" = "$JELLYFIN_COMMIT" ] || {
    echo "$JELLYFIN_TAG no longer points at $JELLYFIN_COMMIT: re-tagged upstream" >&2
    exit 1
  }
  git -C "$src" apply "$here/leyla-static.patch"
  add_thumb "$src"
  add_grid_lead "$src"
  add_restart_decode "$src"
  # ffmpeg's hardened toolchain passes `-z relro,-z now`, which Apple's
  # linker refuses as MinGW's did (build-static.sh); macOS's own defaults
  # keep PIE and the stack protector. The network stays off.
  defaults="$src/builder/variants/defaults-gpl.sh"
  sed 's/ --toolchain=hardened//' "$defaults" > "$defaults.new" && mv "$defaults.new" "$defaults"
  ! grep -q -- '--toolchain=hardened' "$defaults"
  grep -q -- '--disable-network' "$defaults"

  (cd "$src/builder" && ./buildmac.sh "$arch")
  mkdir -p "$work/$arch"
  tar -xJf "$src"/builder/artifacts/jellyfin-ffmpeg_*_portable_mac*-gpl.tar.xz -C "$work/$arch"
  for program in ffmpeg ffprobe; do
    lipo -archs "$work/$arch/$program" | grep -qx "$arch" || {
      echo "$program built for $(lipo -archs "$work/$arch/$program"), not $arch" >&2
      exit 1
    }
    # Nothing linked from outside macOS (Buildmac.md's known issue: a
    # Homebrew libx11 gets linked in, and the program runs on this Mac only).
    outside="$(otool -L "$work/$arch/$program" | tail -n +2 | awk '{print $1}' | grep -v -E '^(/usr/lib/|/System/Library/)' || true)"
    [ -z "$outside" ] || {
      echo "$program ($arch) links outside macOS: $outside" >&2
      exit 1
    }
  done
done

mkdir -p "$work/universal"
for program in ffmpeg ffprobe; do
  lipo -create "$work/arm64/$program" "$work/x86_64/$program" -output "$work/universal/$program"
done
# What Leyla needs of it, before it is kept.
"$work/universal/ffprobe" -leyla_thumb -version 2>/dev/null | grep -q '^leyla_thumb 1 ' || {
  echo "ffprobe has no -leyla_thumb: the thumb patch did not apply" >&2
  exit 1
}
"$work/universal/ffmpeg" -hide_banner -h muxer=hls 2>/dev/null | grep -q -- 'hls_grid_lead' || {
  echo "ffmpeg's hls muxer has no hls_grid_lead: the grid-lead patch did not apply" >&2
  exit 1
}
! "$work/universal/ffmpeg" -hide_banner -protocols 2>/dev/null | grep -qx '  *https\{0,1\}' || {
  echo "ffmpeg was built with a network protocol" >&2
  exit 1
}
tar -cJf "$out/ffmpeg-mac-universal.tar.xz" -C "$work/universal" ffmpeg ffprobe
echo "built $out/ffmpeg-mac-universal.tar.xz ($(lipo -archs "$work/universal/ffmpeg"))"
