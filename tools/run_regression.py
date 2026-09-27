"""Run sequential SIFT baseline comparisons without polling child processes."""
from __future__ import annotations
import argparse
from datetime import datetime, timezone
import json
from pathlib import Path
import subprocess
import time

if __name__ == '__main__':
    parser=argparse.ArgumentParser()
    parser.add_argument('--config',type=Path,required=True)
    parser.add_argument('--binary',type=Path,required=True)
    parser.add_argument('--queries',type=int,default=10000)
    args=parser.parse_args()
    source=args.config.resolve()
    cfg=json.loads(source.read_text())
    cfg['manifest']=str((source.parent/cfg['manifest']).resolve())
    run_id='regression-'+datetime.now(timezone.utc).strftime('%Y%m%d-%H%M%S')
    root=Path(__file__).resolve().parents[1]/'results'/run_id
    root.mkdir()
    summaries=[]
    started=time.monotonic()
    for ef in [64,32,128]:
        config=json.loads(json.dumps(cfg))
        config['search']['ef']=ef
        config['output']=str(root/f'ef{ef}')
        config['query_limit']=args.queries
        config['compare']=1
        config['runtime']['metrics']='basic'
        path=root/f'ef{ef}.config.json'
        path.write_text(json.dumps(config,indent=2)+'\n')
        print(f'START ef={ef}, queries={args.queries}, {root}',flush=True)
        with (root/f'ef{ef}.log').open('w') as log:
            subprocess.run([str(args.binary.resolve()),'--config',str(path)],stdout=log,stderr=log,check=True)
        summary=json.loads((root/f'ef{ef}/summary.json').read_text())
        if summary['queries']!=args.queries or summary['errors']!=0:
            raise RuntimeError('regression incomplete or failed')
        summaries.append(summary)
        print(f'PASS ef={ef}, Recall@10={summary["mean_recall"]:.4f}, elapsed={time.monotonic()-started:.1f}s',flush=True)
    (root/'regression.json').write_text(json.dumps(summaries,indent=2)+'\n')
    print(f'COMPLETE {root}',flush=True)
