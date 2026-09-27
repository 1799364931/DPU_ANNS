"""Revalidate all source identities, edges, mappings, PQ rows, and output hashes."""
from __future__ import annotations
import argparse
import json
from pathlib import Path
from index_format import load_sources, validate

if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('manifest', type=Path)
    args = parser.parse_args()
    manifest = json.loads(args.manifest.read_text())
    paths = {key: Path(value['path']) for key, value in manifest['sources'].items()}
    _, ptr, edges, clusters, codes = load_sources(paths)
    validate(args.manifest.parent, manifest, ptr, edges, clusters, codes)
    print('PASS: every edge/order, mapping, cluster, PQ row and checksum')
