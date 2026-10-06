#!/usr/bin/env bash
# Build reference-pinned media tools without changing host /usr/bin. The
# exported native ELF executables use only their task-owned Debian libraries.
set -euo pipefail
source_dir=$(cd -- "$(dirname -- "$0")" && pwd)
if [[ $# != 1 || $1 != /* ]]; then
    echo "usage: $0 ABSOLUTE_NEW_OUTPUT_DIRECTORY" >&2
    exit 2
fi
output=$1
if [[ -e "$output" ]]; then
    echo "output directory already exists: $output" >&2
    exit 2
fi
for tool in docker python3 patchelf tar; do command -v "$tool" >/dev/null; done
mkdir -p -- "$output/rootfs"
image="campfire-pinned-media:8.16.1-7.1.5"
docker build --target pinned-tools -t "$image" "$source_dir" >"$output/build.log" 2>&1
container=$(docker create "$image")
trap 'docker rm "$container" >/dev/null 2>&1 || true' EXIT
docker export "$container" | tar -x -C "$output/rootfs" --wildcards \
    "opt/vips/bin/vips" "opt/vips/lib/*.so*" \
    "opt/ffmpeg/bin/ffmpeg" "opt/ffmpeg/bin/ffprobe" "opt/ffmpeg/lib/*.so*" \
    "usr/lib/x86_64-linux-gnu/*.so*" "usr/local/bin/cf-vips" \
    "usr/local/share/campfire-media/*"
docker image inspect "$image" >"$output/image.json"
docker run --rm "$image" dpkg-query -W >"$output/debian-packages.txt"
python3 - "$output" <<'PY'
import hashlib, json, pathlib, subprocess, sys
out = pathlib.Path(sys.argv[1])
root = out / 'rootfs'
library_dirs = [root / 'opt/vips/lib', root / 'opt/ffmpeg/lib',
                root / 'usr/lib/x86_64-linux-gnu']
loader = root / 'usr/lib/x86_64-linux-gnu/ld-linux-x86-64.so.2'
if not loader.is_file():
    raise SystemExit('native export currently supports Linux x86_64 only')
binaries = [root / 'opt/vips/bin/vips', root / 'opt/ffmpeg/bin/ffmpeg',
            root / 'opt/ffmpeg/bin/ffprobe', root / 'usr/local/bin/cf-vips']
original = {str(p.relative_to(root)): hashlib.sha256(p.read_bytes()).hexdigest()
            for p in binaries}
rpath = ':'.join(map(str, library_dirs))
for directory in library_dirs:
    for p in directory.rglob('*.so*'):
        if p.is_symlink() or not p.is_file() or p.name.startswith('ld-linux'):
            continue
        with p.open('rb') as f:
            if f.read(4) != b'\x7fELF':
                continue
        subprocess.run(['patchelf', '--set-rpath', rpath, str(p)], check=True)
versions = {}
for p in binaries:
    subprocess.run(['patchelf', '--set-interpreter', str(loader),
                    '--set-rpath', rpath, str(p)], check=True)
    flag = '--version' if p.name in ('vips', 'cf-vips') else '-version'
    versions[p.name] = subprocess.check_output([str(p), flag], text=True)
assert versions['vips'].strip() == 'vips-8.16.1'
assert versions['cf-vips'].strip() == 'vips-8.16.1'
assert versions['ffmpeg'].startswith('ffmpeg version 7.1.5-0+deb13u1 ')
assert versions['ffprobe'].startswith('ffprobe version 7.1.5-0+deb13u1 ')
record = {'original_elf_sha256': original, 'versions': versions,
          'native_elf_sha256': {str(p.relative_to(root)): hashlib.sha256(p.read_bytes()).hexdigest()
                               for p in binaries},
          'relocation': 'patchelf interpreter/RPATH only; no shell wrappers or host installation',
          'source_dsc': {'vips': '60205e00d061b9d8072938e04899f2ca2fdac0513068e561d33f7c87fae1ae2e',
                         'ffmpeg': '9ed2ed34cbe7f056eeebbe9045c5e2d15e41b5b053fe7c8ba6979a0b6fb081ce'}}
(out / 'provenance.json').write_text(json.dumps(record, indent=2) + '\n')
for name, path in zip(('VIPS', 'FFMPEG', 'FFPROBE', 'VIPS_ADAPTER'), binaries):
    print(f'CF_PROC_{name}_PATH={path}')
PY
