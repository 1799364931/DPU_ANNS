"""Fetch/extract pinned official SDK packages locally; never installs drivers."""
from __future__ import annotations
import hashlib
import json
from pathlib import Path
import subprocess
from urllib.request import urlopen

root = Path(__file__).resolve().parents[1]
wanted = {'doca-sdk-common', 'doca-sdk-dma', 'doca-sdk-comch',
          'libdoca-sdk-common-dev', 'libdoca-sdk-dma-dev', 'libdoca-sdk-comch-dev', 'doca-samples'}
records = []
for arch, system in [('x86_64', 'ubuntu24.04'), ('arm64-dpu', 'ubuntu22.04')]:
    base = f'https://linux.mellanox.com/public/repo/doca/2.9.4/{system}/{arch}/'
    data = (root / 'planning/execution' / f'doca-{arch}-Packages').read_text()
    target = root / 'artifacts/sdk' / arch
    target.mkdir(parents=True, exist_ok=True)
    for block in data.split('\n\n'):
        fields = dict(line.split(': ', 1) for line in block.splitlines() if ': ' in line)
        if fields.get('Package') not in wanted:
            continue
        url = base + fields['Filename'].removeprefix('./')
        path = target / Path(fields['Filename']).name
        payload = path.read_bytes() if path.exists() else urlopen(url, timeout=60).read()
        digest = hashlib.sha256(payload).hexdigest()
        if digest != fields['SHA256']:
            raise ValueError(f'checksum mismatch: {url}')
        path.write_bytes(payload)
        subprocess.run(['dpkg-deb', '-x', str(path), str(target / 'root')], check=True)
        records.append(dict(arch=arch, package=fields['Package'], version=fields['Version'],
                            sha256=digest, url=url))
    print(f'{arch}: SDK extracted', flush=True)
(root / 'planning/execution/sdk_packages.json').write_text(json.dumps(records, indent=2) + '\n')
