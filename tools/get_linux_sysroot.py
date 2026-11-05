"""Unpacks what compiling and linking for 32-bit Linux needs (Debian's i386 C library and its headers) into a folder,
so tools\\linux_check.ps1 can check the Linux code from Windows. Nothing from it ends up in a build for Windows, and the
program it links is not run here.

    python -I tools\\get_linux_sysroot.py <folder>

Each package's SHA-256 is checked against Debian's index, fetched over HTTPS.
"""
import hashlib
import lzma
import os
import shutil
import sys
import tarfile
import urllib.request

MIRROR = 'https://deb.debian.org/debian/'
SUITE = 'trixie'
PACKAGES = ['libc6', 'libc6-dev', 'libgcc-14-dev', 'libgcc-s1', 'linux-libc-dev', 'libcrypt-dev',
            'libgl-dev', 'libgl1', 'libglx-dev', 'libglx0', 'libglvnd0', 'libglvnd-dev', 'mesa-common-dev',
            'libsdl3-0', 'libsdl3-dev', 'libfreetype-dev']     # FreeType's headers only: padicons.c opens the library


def get(url):
    with urllib.request.urlopen(urllib.request.Request(url, headers={'User-Agent': 'get_linux_sysroot'})) as r:
        return r.read()


def index():
    text = lzma.decompress(get(f'{MIRROR}dists/{SUITE}/main/binary-i386/Packages.xz')).decode('utf-8')
    found = {}
    for block in text.split('\n\n'):
        fields = dict(line.split(': ', 1) for line in block.splitlines() if ': ' in line and line[0] != ' ')
        if fields.get('Package') in PACKAGES:
            found[fields['Package']] = fields
    return found


def members(deb):
    """The members of an ar archive."""
    assert deb[:8] == b'!<arch>\n'
    at = 8
    while at + 60 <= len(deb):
        name = deb[at:at + 16].decode().strip().rstrip('/')
        size = int(deb[at + 48:at + 58])
        yield name, deb[at + 60:at + 60 + size]
        at += 60 + size + (size & 1)


def unpack(deb, out):
    """Links become copies, made after the files they point at: Windows may not allow links."""
    links = []
    for name, data in members(deb):
        if not name.startswith('data.tar'):
            continue
        path = os.path.join(out, '_data.tar')
        with open(path, 'wb') as f:
            f.write(lzma.decompress(data) if name.endswith('.xz') else data)
        with tarfile.open(path) as tar:
            for m in tar:
                target = os.path.normpath(os.path.join(out, m.name))
                if not target.startswith(os.path.normpath(out) + os.sep):
                    continue
                if m.isdir():
                    os.makedirs(target, exist_ok=True)
                elif m.issym() or m.islnk():
                    links.append((target, m.linkname, m.issym()))
                elif m.isfile():
                    os.makedirs(os.path.dirname(target), exist_ok=True)
                    with open(target, 'wb') as f:
                        shutil.copyfileobj(tar.extractfile(m), f)
        os.remove(path)
    return links


def main():
    out = os.path.abspath(sys.argv[1])
    os.makedirs(out, exist_ok=True)
    idx = index()
    missing = [p for p in PACKAGES if p not in idx]
    if missing:
        sys.exit(f'not in the index: {missing}')
    links = []
    for p in PACKAGES:
        deb = get(MIRROR + idx[p]['Filename'])
        if hashlib.sha256(deb).hexdigest() != idx[p]['SHA256']:
            sys.exit(f'{p}: the hash does not match')
        print(p, idx[p]['Version'], len(deb))
        links += unpack(deb, out)
    for _ in range(3):              # a link can point at a link
        for target, name, relative in links:
            src = os.path.normpath(os.path.join(os.path.dirname(target), name)) if relative and not name.startswith('/') \
                else os.path.normpath(os.path.join(out, name.lstrip('/')))
            if os.path.isfile(src) and not os.path.exists(target):
                os.makedirs(os.path.dirname(target), exist_ok=True)
                shutil.copyfile(src, target)
    # Debian's /lib is a link to /usr/lib, and libc.so names its files by /lib.
    for name in ('i386-linux-gnu/libc.so.6', 'i386-linux-gnu/ld-linux.so.2', 'ld-linux.so.2'):
        src, dst = os.path.join(out, 'usr/lib', name), os.path.join(out, 'lib', name)
        if os.path.isfile(src):
            os.makedirs(os.path.dirname(dst), exist_ok=True)
            shutil.copyfile(src, dst)
    print('ready:', out)


main()
