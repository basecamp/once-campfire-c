# Pinned external media tools

Run `vendor/media/build.sh /absolute/new/output/directory` on Linux x86_64
with Docker, Python 3 and patchelf installed. The build leaves host executables
unchanged. The output contains native executable paths, an exported Debian
root filesystem, versions, source checksums, original/relocated executable
hashes and the installed Debian package inventory. Keep the root filesystem
at that path: the native ELF interpreter and library search paths point to it.

The Dockerfile uses the reference source pins and build flags: libvips
8.16.1-1+deb13u1 and FFmpeg 7:7.1.5-0+deb13u1. `dpkg-source` validates
tarballs against the checksum-pinned DSCs; the image includes the DSCs and
source license notices under `/usr/local/share/campfire-media`. Debian runtime
packages are recorded per build; they are not snapshot-pinned. Passing the
version check alone does not establish reference output byte parity.

Compile the application's subprocess enum with the printed absolute paths
using `CF_PROC_VIPS_PATH`, `CF_PROC_FFMPEG_PATH`, `CF_PROC_FFPROBE_PATH` and
`CF_PROC_VIPS_ADAPTER_PATH`. Paths remain compile-time constants; requests
cannot select executables, and no shell wrapper is used. For container use,
the native executables keep their ordinary `/opt` and `/usr/local` paths.

`cf-vips` links libvips in a separate process and supports:

- `--version`: requires and reports `vips-8.16.1`.
- `analyze INPUT`: sequential load; raw width/height and nullable
  `exif_orientation` JSON. An unreadable image returns `{}`, as the reference
  image analyzer does.
- `transform INPUT OUTPUT FORMAT WIDTH HEIGHT`: loader-valid `page: 0`,
  autorotation, then optional thumbnail downscaling and the reference integer
  convolution mask with scale 24 and offset 0. Zero means an unspecified
  dimension; two zeroes mean conversion only. The output extension must match
  FORMAT. The default saver determines encoded bytes. File output is limited
  to 16 MiB with a bounded saver target; the application owns the process deadline.

Every operation enables `vips_block_untrusted_set(TRUE)` and blocks
`VipsForeignLoadOpenslide`. The parent application never links libvips and
retains its bounded subprocess and media-slot ownership.

For a fresh application output tree with pinned tools:

```sh
vendor/media/build.sh /tmp/campfire-media-tools
export LIBHEIF_PLUGIN_PATH=/tmp/campfire-media-tools/rootfs/usr/lib/x86_64-linux-gnu/libheif/plugins
export CF_PROC_VIPS_PATH=/tmp/campfire-media-tools/rootfs/opt/vips/bin/vips
export CF_PROC_FFMPEG_PATH=/tmp/campfire-media-tools/rootfs/opt/ffmpeg/bin/ffmpeg
export CF_PROC_FFPROBE_PATH=/tmp/campfire-media-tools/rootfs/opt/ffmpeg/bin/ffprobe
export CF_PROC_VIPS_ADAPTER_PATH=/tmp/campfire-media-tools/rootfs/usr/local/bin/cf-vips
CF_MEDIA_LIVE=1 make BUILD_ROOT=/tmp/campfire-media-app test
python3 tests/fixtures/tools/verify_media_vectors.py /tmp/campfire-media-tools/rootfs
```

Choose a fresh `BUILD_ROOT` when changing compile-time tool paths. Set `CF_MEDIA_LIVE=1`
for the strict live application media gate. The vector script tests encoded
bytes independently of application handler tests; both are required.

Native exports need the trusted startup `LIBHEIF_PLUGIN_PATH` above for
Debian HEIC/AVIF decoder modules, whose compiled default path belongs to the
container. The build relocates those plugin libraries too. Container execution
uses Debian's ordinary plugin directory. The current committed byte vectors
cover JPEG, PNG and WebP outputs and the MOV video poster; they do not establish
HEIC/AVIF output byte parity.
