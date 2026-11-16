#!/usr/bin/env bash
# The corresponding source of a static build (GPL-3.0 §6): every library
# jellyfin-ffmpeg's builder compiles in for <target>, at the commit its
# script pins, plus the patched jellyfin-ffmpeg tree itself.
#
#   deploy/ffmpeg/static-sources.sh <patched jellyfin-ffmpeg tree> <target> <out.tar.gz>
#   PINS_ONLY=1 deploy/ffmpeg/static-sources.sh <tree> <target> -   # list, fetch nothing
#
# The builder clones each library inside its own Docker stages and keeps
# no copy, so this reads the same pins (SCRIPT_REPO/SCRIPT_COMMIT, and the
# *2 pair or SVN revision where a script has one) and fetches them again.
set -euo pipefail

tree="$(cd "${1:?patched jellyfin-ffmpeg tree}" && pwd)"
target="${2:?target, e.g. linux64}"
out="$(realpath -m "${3:?output .tar.gz}")"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
mkdir -p "$work/src"

fetch_git() { # repo dir commit
  git init -q "$2"
  # A pin is a sha or a tag. Fetched by name, a tag lands in FETCH_HEAD
  # only, with no local ref for `checkout <tag>` to find.
  if git -C "$2" fetch -q --depth 1 "$1" "$3" 2>/dev/null; then
    git -C "$2" checkout -q FETCH_HEAD
  else
    git -C "$2" fetch -q --tags "$1"  # servers that refuse a fetch by sha
    git -C "$2" checkout -q "$3"
  fi
  git -C "$2" submodule -q update --init --recursive --depth 1 2>/dev/null || true
  rm -rf "$2/.git"
}

cd "$tree/builder"
for script in scripts.d/*.sh scripts.d/*/*.sh; do
  # Each script is sourced in a subshell, as the builder does, so its
  # functions and pins do not leak into the next one.
  (
    TARGET="$target" VARIANT=gpl
    SCRIPT_SKIP="" SCRIPT_REPO="" SCRIPT_COMMIT="" SCRIPT_REPO2="" SCRIPT_COMMIT2="" SCRIPT_REV=""
    # shellcheck disable=SC1090
    source "$script"
    ffbuild_enabled || exit 0
    [ -z "$SCRIPT_SKIP" ] || exit 0
    [ -n "$SCRIPT_REPO" ] || exit 0
    name="$(basename "$script" .sh)"
    dir="$work/src/$name"
    if [ -n "${PINS_ONLY:-}" ]; then
      echo "$name $SCRIPT_REPO ${SCRIPT_COMMIT:-r$SCRIPT_REV}"
      exit 0
    fi
    if [ -n "$SCRIPT_REV" ]; then
      svn export -q "${SCRIPT_REPO}@${SCRIPT_REV}" "$dir"
    else
      fetch_git "$SCRIPT_REPO" "$dir" "$SCRIPT_COMMIT"
      if [ -n "$SCRIPT_REPO2" ] && [ -n "$SCRIPT_COMMIT2" ]; then
        fetch_git "$SCRIPT_REPO2" "$dir-2" "$SCRIPT_COMMIT2"
      fi
    fi
    echo "$name $SCRIPT_REPO ${SCRIPT_COMMIT:-r$SCRIPT_REV}" >> "$work/src/PINS.txt"
  )
done

[ -z "${PINS_ONLY:-}" ] || exit 0
git -C "$tree" archive --format=tar --prefix=jellyfin-ffmpeg/ HEAD | tar -x -C "$work/src"
# The patch applied on top, so the tree in the archive is what was built.
# Leyla's quilt patches count (intent-to-add): every one
# (debian/patches/9000-9003-*.patch, leyla-thumb's among them) is new to
# jellyfin's tree, and a plain `git diff` left them out while the archived
# `series` named them, so the archive did not build (found 2026-10-24).
# Only that folder: the builder's cache link and outputs are not source.
git -C "$tree" add -N debian/patches
git -C "$tree" diff --binary > "$work/src/jellyfin-ffmpeg/leyla.patch"
(cd "$work/src/jellyfin-ffmpeg" && patch -s -p1 < leyla.patch)

tar -C "$work/src" -czf "$out" .
echo "wrote $out ($(du -h "$out" | cut -f1))"
