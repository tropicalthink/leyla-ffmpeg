#!/usr/bin/env bash
# Builds Leyla's ffmpeg: jellyfin-ffmpeg at a pinned tag with
# `leyla.patch` applied (see README.md), for amd64 and arm64, and pushes
#
#   <registry>/tropicalthink/leyla-ffmpeg:<tag>         the two .debs
#   <registry>/tropicalthink/leyla-ffmpeg-source:<tag>  their full source
#
#   deploy/ffmpeg/build.sh tag               # the tag the Dockerfiles name
#   deploy/ffmpeg/build.sh build <arch>      # one package, to try a patch
#   deploy/ffmpeg/build.sh images [arch..]   # both images, local only
#   deploy/ffmpeg/build.sh ensure [arch..]   # push them unless the registry has them
#
# The tag is the jellyfin-ffmpeg version plus a hash of the pins and the
# patch, so changing either names a new image. ci.yml's ci-image job runs
# `ensure`: seconds when the registry has the tag. A first build takes one
# to two hours per architecture, held to $FFMPEG_BUILD_CPUS cores beside
# production, so on a bump build `images` first and load them into
# the CI daemon (README.md); `ensure` then pushes what it finds there.
set -euo pipefail

JELLYFIN_TAG=v8.1.3-1
JELLYFIN_COMMIT=253db2a7b0a8045c54ce68ce33d7f601229b1822
# debian:trixie, as jellyfin-ffmpeg's own `build` script sets it up: the
# runtime image and the CI image are trixie, and the package's Depends name
# that release's libraries.
DISTRO=debian:trixie GCC_VER=14 LLVM_VER=19 LLVMSPIRVLIB_VER=19

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REGISTRY="${CI_REGISTRY:-localhost:5000}"
CPUS="${FFMPEG_BUILD_CPUS:-6}"
WORK="${FFMPEG_BUILD_DIR:-$HOME/.cache/leyla-ffmpeg-build}"

tag() {
  local hash
  # The pins and the patch, not this script's prose: a comment edit must
  # not cost a two-hour rebuild.
  hash="$({ grep -E '^(JELLYFIN_|DISTRO=)' "$here/build.sh"; cat "$here/leyla.patch"; thumb_patch; cat "$here/leyla-hls-grid-lead.patch" "$here/leyla-restart-decode.patch"; } | sha256sum | cut -c1-12)"
  echo "${JELLYFIN_TAG#v}-$hash"
}

# `ffprobe -leyla_thumb` (leyla-thumb/), Leyla's keyframe thumbnailer, as
# one more quilt patch at the end of jellyfin's series: it then lands on top
# of their configure and fftools edits in both builds (this one and
# build-static.sh, which pushes the same series). The C file stays a file
# here, not a hunk inside a patch.
thumb_patch() {
  cat "$here/leyla-thumb/fftools.patch"
  printf -- '--- /dev/null\n+++ b/fftools/leyla_thumb.c\n@@ -0,0 +1,%d @@\n' \
    "$(wc -l < "$here/leyla-thumb/leyla_thumb.c")"
  sed 's/^/+/' "$here/leyla-thumb/leyla_thumb.c"
}

add_thumb() {
  thumb_patch > "$1/debian/patches/9000-leyla-thumb.patch"
  echo 9000-leyla-thumb.patch >> "$1/debian/patches/series"
}

# `-hls_grid_lead` (README.md): a restarted copy keeps the full run's cut
# grid from any segment. In both builds (build-static.sh adds it too).
add_grid_lead() {
  cp "$here/leyla-hls-grid-lead.patch" "$1/debian/patches/9001-leyla-hls-grid-lead.patch"
  echo 9001-leyla-hls-grid-lead.patch >> "$1/debian/patches/series"
}

# `-seek_at_pts` and `-skip_nonref_before` (README.md): a conversion
# started or restarted mid-file reads from the keyframe at or before its
# instant, not one before that, and decodes only the pictures before the
# instant that later ones refer to. In both builds (build-static.sh adds it
# too).
add_restart_decode() {
  cp "$here/leyla-restart-decode.patch" "$1/debian/patches/9002-leyla-restart-decode.patch"
  echo 9002-leyla-restart-decode.patch >> "$1/debian/patches/series"
}

in_registry() { docker manifest inspect --insecure "$1" >/dev/null 2>&1; }

build_arch() {
  local arch="$1" src="$WORK/src-$1" out="$WORK/out/$1"
  rm -rf "$src" "$out"
  mkdir -p "$out"
  git clone -q --depth 1 --branch "$JELLYFIN_TAG" https://github.com/jellyfin/jellyfin-ffmpeg.git "$src"
  [ "$(git -C "$src" rev-parse HEAD)" = "$JELLYFIN_COMMIT" ] || {
    echo "$JELLYFIN_TAG no longer points at $JELLYFIN_COMMIT: re-tagged upstream" >&2
    exit 1
  }
  git -C "$src" apply "$here/leyla.patch"
  add_thumb "$src"
  add_grid_lead "$src"
  add_restart_decode "$src"
  sed "s/DISTRO/$DISTRO/; s/BUILD_ARCHITECTURE/$arch/; s/GCC_RELEASE_VERSION/$GCC_VER/; \
       s/LLVM_RELEASE_VERSION/$LLVM_VER/; s/LLVMSPIRVLIB_RELEASE_VERSION/$LLVMSPIRVLIB_VER/" \
    "$src/Dockerfile.in" > "$src/Dockerfile"
  docker build -q -t "leyla-ffmpeg-build-$arch" "$src"
  # The recipe runs `make -j$(nproc)`: --cpus is what keeps it off
  # production's cores.
  # No bind mount: in CI the daemon is dind, whose filesystem is not this
  # one. The output is copied out of the stopped container instead.
  docker rm -f "leyla-ffmpeg-build-$arch" >/dev/null 2>&1 || true
  docker run --name "leyla-ffmpeg-build-$arch" --cpus="$CPUS" -e "RELEASE=$DISTRO" \
    "leyla-ffmpeg-build-$arch"
  docker cp "leyla-ffmpeg-build-$arch:/dist/." "$out"
  docker rm "leyla-ffmpeg-build-$arch" >/dev/null
  mv "$out"/deb/jellyfin-ffmpeg8_*_"$arch".deb "$WORK/dist/ffmpeg-$arch.deb"
  mv "$out/source-$arch.tar.gz" "$WORK/source/source-$arch.tar.gz"
  rm -rf "$src" "$out"
}

images() {
  # Both images for this tag, built locally; packages already built for it
  # are reused.
  local arch
  mkdir -p "$WORK/dist" "$WORK/source"
  for arch in "${arches[@]}"; do
    [ -f "$WORK/dist/ffmpeg-$arch.deb" ] || build_arch "$arch"
  done
  cp "$here/Dockerfile.dist" "$WORK/dist/Dockerfile"
  cp "$here/Dockerfile.dist" "$WORK/source/Dockerfile"
  docker build -q -t "$image" "$WORK/dist"
  docker build -q -t "$source_image" "$WORK/source"
}

cmd="${1:-}"
[ "$#" -gt 0 ] && shift
if [ "$#" -gt 0 ]; then arches=("$@"); else arches=(amd64 arm64); fi
t="$(tag)"
# Per tag, so a package from another patch is never picked up.
WORK="$WORK/$t"
image="$REGISTRY/tropicalthink/leyla-ffmpeg:$t"
source_image="$REGISTRY/tropicalthink/leyla-ffmpeg-source:$t"

case "$cmd" in
  tag) echo "$t" ;;
  build)
    mkdir -p "$WORK/dist" "$WORK/source"
    build_arch "${arches[0]}"
    echo "built $WORK/dist/ffmpeg-${arches[0]}.deb"
    ;;
  images) images; echo "built $image and $source_image" ;;
  ensure)
    if in_registry "$image" && in_registry "$source_image"; then
      # On this daemon too: the image builds that copy the package out of
      # it (docker-build, the CI images) run with no registry login.
      docker image inspect "$image" >/dev/null 2>&1 || docker pull -q "$image"
      echo "$image is in the registry"
      exit 0
    fi
    # Built ahead on the same daemon (`images`, then loaded into dind): push
    # those rather than build for hours again.
    if ! docker image inspect "$image" "$source_image" >/dev/null 2>&1; then
      images
    fi
    docker push "$image"
    docker push "$source_image"
    echo "pushed $image"
    ;;
  *) sed -n '2,11p' "$0" | sed 's/^# \{0,1\}//'; exit 2 ;;
esac
