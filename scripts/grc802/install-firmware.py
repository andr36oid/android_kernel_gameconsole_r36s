#!/usr/bin/env python3
"""Stage the report's exact SV6160 firmware into an image root or vendor tree."""
import argparse
import hashlib
from pathlib import Path
import shutil

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('root', type=Path, help='image root to stage')
parser.add_argument('--source', type=Path, default=Path('/andr36oid/work/reports/clone-issue-3/sv6160-rk915/firmware'))
parser.add_argument('--android', action='store_true', help='stage vendor/firmware and a /lib/firmware link')
args = parser.parse_args()
files = {
    'ssv6160-fw.bin': (48056, 'a0e5ed758324e15bc2ed62d0e29f12d8'),
    'ssv6160-patch.bin': (16716, '6c1f8ec692e153ca61617d0d2dd5cd4d'),
}
# Validate every source before modifying the image.
for name, (size, digest) in files.items():
    data = (args.source / name).read_bytes()
    if len(data) != size or hashlib.md5(data).hexdigest() != digest:
        parser.error('firmware differs from the assessed report: ' + name)
dest = args.root / ('vendor/firmware' if args.android else 'lib/firmware')
dest.mkdir(parents=True, exist_ok=True)
for name in files:
    target = dest / name
    shutil.copyfile(args.source / name, target)
    target.chmod(0o644)
    assert hashlib.md5(target.read_bytes()).hexdigest() == files[name][1]
    print(target)
if args.android:
    link = args.root / 'lib/firmware'
    link.parent.mkdir(parents=True, exist_ok=True)
    if not link.is_symlink() and link.exists():
        parser.error('existing lib/firmware is not a symlink: ' + str(link))
    if link.is_symlink():
        if link.readlink() != Path('/vendor/firmware'):
            parser.error('existing lib/firmware points elsewhere: ' + str(link))
    else:
        link.symlink_to('/vendor/firmware')
