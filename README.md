# leyla-ffmpeg

The FFmpeg that ships with [Leyla](https://leyla.media), a self-hosted media
server. It is [jellyfin-ffmpeg](https://github.com/jellyfin/jellyfin-ffmpeg)
at a pinned tag (`JELLYFIN_TAG` and `JELLYFIN_COMMIT` in `build.sh`) with the
patches in this repository applied. Jellyfin's own patches are kept: the
`tonemapx` filter, the conservative ffnvcodec pin and the bundled VAAPI
drivers.

This repository is the corresponding source offer for the `ffmpeg` and
`ffprobe` binaries in Leyla's Docker image, Linux packages, Windows app and
macOS server. It is GPL-3.0-or-later, like the build it produces.
Everything in the binaries that is not in this repository is upstream:
FFmpeg, jellyfin-ffmpeg and the libraries listed by `static-sources.sh`.

## What is different from jellyfin-ffmpeg

| Change | Why |
|---|---|
| No `libfdk_aac` | Its licence cannot be combined with the GPL encoders (x264, x265) in a binary someone else receives. Leyla encodes with ffmpeg's native `aac`. |
| No Vulkan, libplacebo, shaderc, OpenCL | About 60 MB. Leyla tone-maps on the CPU with `tonemapx`. |
| No libtheora or libwebp encoders, no libopenmpt | Not used. ffmpeg's own decoders still read Theora and WebP. |
| No network: `--disable-network`, protocols `file`, `pipe`, `fd`, `concat`, `data` and `subfile` only; no GnuTLS, OpenSSL or SRT | The server hands ffmpeg local files and pipes, so a crafted playlist cannot make it fetch anything. |
| `--toolchain=hardened` | Stack protector, `_FORTIFY_SOURCE`, PIE and full RELRO for a binary that parses untrusted files. |
| chromaprint on its bundled KissFFT | One library fewer. |
| `-Jellyfin-Layla` version suffix | `ffmpeg -version` says which build is running. |
| `ffprobe -leyla_thumb` (`leyla-thumb/`) | A long-lived thumbnail helper that decodes one keyframe for a scrub preview and answers with a JPEG over stdin and stdout. It is a mode of `ffprobe`, so a static build links every codec once. |
| `-hls_grid_lead` (`leyla-hls-grid-lead.patch`, quilt 9001) | Sets the HLS muxer's cut grid back from a pass's first picture, so a copy restarted at any boundary cuts where the full run did. |
| `-seek_at_pts` and `-skip_nonref_before` (`leyla-restart-decode.patch`, quilt 9002) | A conversion restarted mid-file decodes less preroll: it lands on the keyframe at or before the instant and skips pictures no later picture refers to. Pictures from the instant on are identical. |
| `vpp_amf` HDR to SDR (`leyla-vpp-amf-hdr.patch`, quilt 9003, static Windows builds) | Makes AMD's `vpp_amf` tone-map HDR to SDR on the GPU after a d3d11va decode, with a `tonemap` option. |
| `libvmaf` 3.2.1 with the v0.6.1, NEG and 4K models (Docker image only) | Quality measurement with the same binary the image ships. |

Every decoder, demuxer and parser stays, along with x264, x265, libvpx,
SVT-AV1, NVENC, VAAPI, QSV, chromaprint, libass, zvbi and libbluray.

## Building

- `build.sh` builds the two `.deb` packages and a source image (pass 1). `build.sh tag` prints the tag a
  build is named by.
- `build-static.sh <linux64|linuxarm64|win64|win64-helper> <out>` runs
  jellyfin-ffmpeg's portable builder with `leyla-static.patch` and produces
  one file linked only against glibc (or the Windows equivalents).
- `build-mac.sh` builds the macOS universal binary by hand on a Mac.
- `static-sources.sh` archives the source of everything the static builder
  compiles in, from the same pins (`PINS_ONLY=1` lists them).

Builds take one to two hours per architecture and need Docker. Set
`FFMPEG_BUILD_CPUS` to limit the compile.

## Bumping jellyfin-ffmpeg

Set `JELLYFIN_TAG` and `JELLYFIN_COMMIT` in `build.sh`, clone that tag,
`git apply --3way leyla.patch`, fix what conflicts and save `git diff` back
here.

## Licence

GPL-3.0-or-later (`LICENSE`). FFmpeg's own licence texts apply to FFmpeg's
code. Leyla's server is a separate program that runs this ffmpeg as a child
process and talks to it over pipes.
