"""Extract an Ubuntu 22.04 Arm reference sysroot locally for clang cross builds."""
from __future__ import annotations
import gzip
import hashlib
import json
from pathlib import Path
import subprocess
from urllib.request import urlopen
from urllib.error import URLError

def fetch(url: str) -> bytes:
    last_error = None
    for candidate in [url, url.replace("https://ports.ubuntu.com", "http://ports.ubuntu.com")]:
        try:
            return urlopen(candidate, timeout=30).read()
        except URLError as error:
            last_error = error
    raise RuntimeError(f"download failed: {url}") from last_error
root=Path(__file__).resolve().parents[1]
base='https://ports.ubuntu.com/ubuntu-ports/'
wanted={'libc6','libc6-dev','linux-libc-dev','libstdc++6','libstdc++-11-dev','libgcc-s1','libgcc-11-dev',
        'libjson-c-dev','libjson-c5','libibverbs1','ibverbs-providers','libnl-3-200','libnl-route-3-200'}
dest=root/'artifacts/toolchain/arm22'
packages=root/'artifacts/toolchain/packages'
dest.mkdir(parents=True,exist_ok=True)
packages.mkdir(parents=True,exist_ok=True)
index=gzip.decompress(fetch(base+'dists/jammy/main/binary-arm64/Packages.gz')).decode()
records=[]
for block in index.split('\n\n'):
    f=dict(line.split(': ',1) for line in block.splitlines() if ': ' in line)
    if f.get('Package') not in wanted:
        continue
    path=packages/Path(f['Filename']).name
    payload=path.read_bytes() if path.exists() else fetch(base+f['Filename'])
    digest=hashlib.sha256(payload).hexdigest()
    if digest!=f['SHA256']:
        raise ValueError('checksum mismatch: '+f['Package'])
    path.write_bytes(payload)
    subprocess.run(['dpkg-deb','-x',str(path),str(dest)],check=True)
    records.append(dict(package=f['Package'],version=f['Version'],sha256=digest,url=base+f['Filename']))
missing=wanted-{r['package'] for r in records}
if missing:
    raise ValueError('missing packages: '+str(missing))
(root/'planning/execution/arm_sysroot_packages.json').write_text(json.dumps(records,indent=2)+'\n')
print('Ubuntu 22.04 Arm reference sysroot extracted',flush=True)
