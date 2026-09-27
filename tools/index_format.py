"""ANNS v1 index preparation and full equivalence checks (NumPy only)."""
from __future__ import annotations
import hashlib
import json
from pathlib import Path
import numpy as np


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open('rb') as stream:
        for chunk in iter(lambda: stream.read(8 * 1024 * 1024), b''):
            digest.update(chunk)
    return digest.hexdigest()


def require(condition: bool, message: str) -> None:
    if not condition:
        raise ValueError(message)


def source_paths(config: Path) -> dict[str, Path]:
    values = json.loads(config.read_text())
    return {key: (config.parent / value).resolve() for key, value in values.items()}


def matrix_shape(path: Path, dtype: str) -> tuple[int, int]:
    shape = np.fromfile(path, dtype='<u4', count=2)
    require(len(shape) == 2 and min(shape) > 0, f'bad matrix header: {path}')
    rows, cols = map(int, shape)
    require(path.stat().st_size == 8 + rows * cols * np.dtype(dtype).itemsize,
            f'bad matrix payload: {path}')
    return rows, cols


def load_sources(paths: dict[str, Path]) -> tuple[dict, np.ndarray, np.ndarray, np.ndarray, np.ndarray]:
    graph = json.loads(paths['graph_manifest'].read_text())
    data = json.loads(paths['data_manifest'].read_text())
    pq = json.loads(paths['pq_manifest'].read_text())['models']['PQ32']
    cluster = json.loads(paths['cluster_manifest'].read_text())
    n, d = int(data['shapes']['base_count']), int(data['shapes']['dimension'])
    c = int(cluster['signature']['c'])
    require(graph['node_count'] == n and cluster['signature']['base_count'] == n,
            'graph/cluster dataset size mismatch')
    require(d % 32 == 0 and pq['parts'] == 32 and pq['bits'] == 8,
            'only PQ32 with eight bits is supported')
    require(cluster['signature']['dimension'] == d and
            cluster['signature']['base_sha256'] == data['outputs']['base_bin']['sha256'],
            'cluster dataset identity mismatch')
    expected = {'indptr': graph['indptr_sha256'], 'neighbors': graph['neighbors_sha256'],
                'pq_codes': pq['code_sha256'], 'pq_centroids': pq['centroid_sha256'],
                'clusters': cluster['files']['vector_to_cluster.i32']['sha256'],
                'base': data['outputs']['base_raw']['sha256'],
                'queries': data['outputs']['query_bin']['sha256'],
                'groundtruth': data['outputs']['groundtruth_bin']['sha256']}
    for name, digest in expected.items():
        require(sha256(paths[name]) == digest, f'source checksum mismatch: {name}')
    ptr = np.fromfile(paths['indptr'], dtype='<u8')
    edges = np.memmap(paths['neighbors'], dtype='<u4', mode='r')
    clusters = np.fromfile(paths['clusters'], dtype='<i4')
    codes = np.memmap(paths['pq_codes'], dtype='u1', mode='r', shape=(n, 32))
    require(ptr.size == n + 1 and ptr[0] == 0 and ptr[-1] == edges.size and
            bool(np.all(ptr[1:] >= ptr[:-1])), 'bad CSR boundaries')
    require(clusters.size == n and bool(np.all((clusters >= 0) & (clusters < c))),
            'bad cluster assignment')
    require(edges.size == graph['edge_count'] and bool(np.all(edges < n)), 'bad graph edges')
    require(paths['pq_codes'].stat().st_size == n * 32, 'PQ payload size mismatch')
    require(paths['pq_centroids'].stat().st_size == 256 * d * 4, 'PQ centroid shape mismatch')
    require(paths['base'].stat().st_size == n * d * 4, 'raw FP32 payload size mismatch')
    qn, qd = matrix_shape(paths['queries'], '<f4')
    tn, tw = matrix_shape(paths['groundtruth'], '<u4')
    require(qd == d and qn == tn and tw >= 10, 'query/groundtruth dimensions mismatch')
    degree = ptr[1:] - ptr[:-1]
    rmax = int(degree.max())
    require(rmax == graph['degree_max'] and 0 <= graph['entry_point'] < n,
            'graph metadata mismatch')
    info = dict(n=n, d=d, c=c, rmax=rmax, stride=4 * (rmax + 1),
                entry_dataset=int(graph['entry_point']), query_count=qn, truth_width=tw)
    return info, ptr, edges, clusters, codes


def index_identity(manifest: dict) -> str:
    fields = {key: manifest[key] for key in
              ('n', 'd', 'c', 'rmax', 'stride', 'entry_dataset', 'entry_graph',
               'format_version', 'pq_parts', 'byte_order', 'vector_stride', 'vector_payload_offset')}
    fields['files'] = {key: value['sha256'] for key, value in manifest['files'].items()}
    fields['sources'] = {key: value['sha256'] for key, value in manifest['sources'].items()}
    return hashlib.sha256(json.dumps(fields, sort_keys=True).encode()).hexdigest()


def validate(directory: Path, manifest: dict, ptr: np.ndarray, edges: np.ndarray,
             clusters: np.ndarray, original_codes: np.ndarray) -> None:
    require(manifest['index_id'] == index_identity(manifest), 'manifest identity mismatch')
    n, c, r = manifest['n'], manifest['c'], manifest['rmax']
    files = manifest['files']
    def array(name: str, dtype: str) -> np.ndarray:
        return np.fromfile(directory / files[name]['path'], dtype=dtype)
    g2d = array('graph_to_dataset', '<u4')
    d2g = array('dataset_to_graph', '<u4')
    g2c = array('graph_to_cluster', '<u4')
    offsets = array('cluster_offsets', '<u8')
    require(g2d.size == n and d2g.size == n and bool(np.all(g2d < n)), 'bad mappings')
    require(bool(np.array_equal(d2g[g2d], np.arange(n))), 'mappings are not inverse permutations')
    require(offsets.size == c + 1 and offsets[0] == 0 and offsets[-1] == n and
            bool(np.all(offsets[1:] >= offsets[:-1])), 'bad cluster boundaries')
    require(bool(np.array_equal(g2c, clusters[g2d])), 'cluster mapping mismatch')
    require(bool(np.array_equal(g2c, np.repeat(np.arange(c), np.diff(offsets).astype('i8')))),
            'cluster ranges do not cover ordered graph IDs')
    require(manifest['entry_graph'] == int(d2g[manifest['entry_dataset']]), 'entry mismatch')
    records = np.memmap(directory / files['graph']['path'], dtype='<u4', mode='r', shape=(n, r + 1))
    codes = np.memmap(directory / files['pq_codes']['path'], dtype='u1', mode='r', shape=(n, 32))
    # Check every edge and its order, in bounded batches. Padding is deliberately not interpreted.
    for begin in range(0, n, 65536):
        end = min(n, begin + 65536)
        original = g2d[begin:end]
        degree = ptr[original + 1] - ptr[original]
        require(bool(np.array_equal(records[begin:end, 0], degree)), 'degree mismatch')
        require(bool(np.array_equal(codes[begin:end], original_codes[original])), 'PQ row mismatch')
        for slot in range(r):
            active = degree > slot
            expected = d2g[edges[ptr[original[active]] + slot]]
            require(bool(np.array_equal(records[begin:end, slot + 1][active], expected)),
                    f'edge/order mismatch at slot {slot}')
    for spec in files.values():
        path = directory / spec['path']
        require(path.stat().st_size == spec['bytes'] and sha256(path) == spec['sha256'],
                f'output checksum mismatch: {path}')
