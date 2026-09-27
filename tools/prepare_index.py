"""Create a stable cluster-contiguous graph without modifying source artifacts."""
from __future__ import annotations
import argparse
import json
from pathlib import Path
import shutil
import tempfile
import time
import numpy as np
from index_format import load_sources, sha256, source_paths, validate, index_identity


def prepare(config: Path) -> Path:
    paths = source_paths(config.resolve())
    output = paths['output']
    if output.exists():
        raise FileExistsError(f'refusing to overwrite existing index: {output}')
    info, ptr, edges, clusters, codes = load_sources(paths)
    output.parent.mkdir(parents=True, exist_ok=True)
    staging = Path(tempfile.mkdtemp(prefix=output.name + '.building-', dir=output.parent))
    started = time.monotonic()
    try:
        n, c, r = info['n'], info['c'], info['rmax']
        g2d = np.argsort(clusters, kind='stable').astype('<u4')
        d2g = np.empty(n, dtype='<u4')
        d2g[g2d] = np.arange(n, dtype='<u4')
        offsets = np.concatenate(([0], np.cumsum(np.bincount(clusters, minlength=c)))).astype('<u8')
        records = np.memmap(staging / 'graph.u32', dtype='<u4', mode='w+', shape=(n, r + 1))
        records[:] = np.uint32(0xffffffff)
        for begin in range(0, n, 65536):
            end = min(n, begin + 65536)
            original = g2d[begin:end]
            degree = ptr[original + 1] - ptr[original]
            records[begin:end, 0] = degree
            for slot in range(r):
                active = degree > slot
                records[begin:end, slot + 1][active] = d2g[edges[ptr[original[active]] + slot]]
        records.flush()
        del records
        files = {'graph': 'graph.u32', 'graph_to_dataset': 'graph_to_dataset.u32',
                 'dataset_to_graph': 'dataset_to_graph.u32', 'graph_to_cluster': 'graph_to_cluster.u32',
                 'cluster_offsets': 'cluster_offsets.u64', 'pq_codes': 'pq_codes.u8',
                 'pq_centroids': 'pq_centroids.f32'}
        for values, key in [(g2d, 'graph_to_dataset'), (d2g, 'dataset_to_graph'),
                            (clusters[g2d].astype('<u4'), 'graph_to_cluster'),
                            (offsets, 'cluster_offsets'), (codes[g2d], 'pq_codes')]:
            values.tofile(staging / files[key])
        shutil.copyfile(paths['pq_centroids'], staging / files['pq_centroids'])
        manifest = dict(info, format_version=1, byte_order='little', pq_parts=32,
                        entry_graph=int(d2g[info['entry_dataset']]),
                        vector_payload_offset=0, vector_stride=info['d'] * 4,
                        sources={k: dict(path=str(v), bytes=v.stat().st_size, sha256=sha256(v))
                                 for k, v in paths.items() if k != 'output'},
                        files={k: dict(path=v, bytes=(staging / v).stat().st_size,
                                       sha256=sha256(staging / v)) for k, v in files.items()})
        manifest['index_id'] = index_identity(manifest)
        validate(staging, manifest, ptr, edges, clusters, codes)
        manifest['validation'] = 'full_edges_order_mappings_pq_checksums'
        manifest['prepare_seconds'] = time.monotonic() - started
        (staging / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
        staging.rename(output)
    except BaseException:
        shutil.rmtree(staging)
        raise
    return output / 'manifest.json'


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--config', type=Path, required=True)
    args = parser.parse_args()
    print(prepare(args.config), flush=True)
