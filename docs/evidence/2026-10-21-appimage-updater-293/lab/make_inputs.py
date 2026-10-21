"""Lab inputs for the #342 acceptance: an AppImage-only lab release per tag (#293 phase 3).

Synthetic images (an ELF runtime stub with the lab update information and 8 MiB of random
payload) and a zsync 0.6.2 control file naming the lab mirror URL, beside their SHA256SUMS.
"""
import hashlib
import os
from pathlib import Path
import sys

SRC = Path(sys.argv[1])
OUT = Path(sys.argv[2])
sys.path.insert(0, str(SRC / 'tests/releases'))
from appimage_fixture import make_appimage, make_zsync  # noqa: E402

for tag in sys.argv[3:]:
    version = tag[1:]
    name = f'putgd-{version}-x86_64.AppImage'
    directory = OUT / tag
    directory.mkdir(parents=True, exist_ok=False)
    image = directory / name
    make_appimage(image, 'zsync|https://pkg.put.gd/lab/dl/nightly/appimage.zsync', os.urandom(8 * 1024 * 1024))
    make_zsync(directory / (name + '.zsync'), image, name, f'https://pkg.put.gd/lab/releases/{tag}/{name}')
    sums = ''.join(f'{hashlib.sha256(path.read_bytes()).hexdigest()}  {path.name}\n'
                   for path in sorted(directory.iterdir()))
    (directory / 'SHA256SUMS').write_text(sums)
    print(tag, name, hashlib.sha256(image.read_bytes()).hexdigest())
