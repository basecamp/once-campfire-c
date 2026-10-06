#!/usr/bin/env python3
"""Compare pinned external media output with every committed storage byte vector.

This is a strict external-tool parity check, separate from the application
handler tests. Usage: verify_media_vectors.py EXPORTED_ROOTFS
"""
import hashlib
import json
from pathlib import Path
import subprocess
import sys
import tempfile


def main():
    if len(sys.argv) != 2:
        raise SystemExit('usage: verify_media_vectors.py EXPORTED_ROOTFS')
    root = Path(sys.argv[1]).resolve()
    fixtures = Path(__file__).resolve().parents[1]
    adapter = root / 'usr/local/bin/cf-vips'
    ffmpeg = root / 'opt/ffmpeg/bin/ffmpeg'
    ffprobe = root / 'opt/ffmpeg/bin/ffprobe'
    assert subprocess.check_output([adapter, '--version'], text=True).strip() == 'vips-8.16.1'
    for binary in (ffmpeg, ffprobe):
        assert subprocess.check_output([binary, '-version'], text=True).startswith(
            f'{binary.name} version 7.1.5-0+deb13u1 ')
    vectors = json.loads((fixtures / 'vectors/storage.json').read_text())
    results = []
    output_cap = None
    with tempfile.TemporaryDirectory(prefix='cf-media-vector-') as work:
        work = Path(work)
        preview = work / 'alpha-centuri-preview_image.jpg'
        video_args = ['-vf', r'select=eq(n\,0)+eq(key\,1)+gt(scene\,0.015),loop=loop=-1:size=2,trim=start_frame=1',
                      '-frames:v', '1', '-f', 'image2', '-']
        def compare(actual, name):
            expected = fixtures / 'vectors/storage' / name
            data = actual.read_bytes()
            if data != expected.read_bytes():
                raise AssertionError(f'{name}: output differs from reference')
            results.append({'file': name, 'bytes': len(data),
                            'sha256': hashlib.sha256(data).hexdigest()})

        with preview.open('wb') as output:
            subprocess.run([ffmpeg, '-i', fixtures / 'media/alpha-centuri.mov', *video_args],
                           stdout=output, stderr=subprocess.PIPE, check=True, timeout=60)
        compare(preview, preview.name)
        for group in ('messages', 'avatars', 'logos'):
            for item in vectors.get(group, []):
                source = fixtures / 'media' / item['fixture']
                if item['fixture'] == 'alpha-centuri.mov':
                    source = preview
                for variant in item.get('variants', []):
                    pairs = dict(variant['transformations_typed']['hash'])
                    fmt = pairs['format']
                    fmt = fmt.get('str', fmt.get('sym')) if isinstance(fmt, dict) else fmt
                    dims = pairs.get('resize_to_limit', [0, 0])
                    target = work / variant['file']
                    subprocess.run([adapter, 'transform', source, target, fmt,
                                    str(dims[0] or 0), str(dims[1] or 0)],
                                   check=True, timeout=60)
                    compare(target, variant['file'])
        # The large source's conversion exceeds the encoded output budget.
        # The adapter must stop writes before 16 MiB and exit unsuccessfully.
        too_large = work / 'oversize.png'
        failed = subprocess.run([adapter, 'transform', fixtures / 'media/earth.png',
                                 too_large, 'png', '0', '0'],
                                stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=60)
        assert failed.returncode != 0
        assert too_large.stat().st_size <= 16777216
        assert b'encoded output exceeds 16 MiB' in failed.stderr
        output_cap = {'status': 'PASS', 'maximum_bytes': 16777216,
                      'partial_bytes': too_large.stat().st_size,
                      'exit_code': failed.returncode}

    print(json.dumps({'status': 'PASS', 'vectors': results, 'encoded_output_limit': output_cap}, indent=2))


if __name__ == '__main__':
    main()
