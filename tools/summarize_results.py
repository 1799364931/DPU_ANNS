"""Summarize JSONL output using per-endpoint durations, never cross-endpoint clocks."""
from __future__ import annotations
import argparse
import json
import math
from pathlib import Path
from statistics import mean


def quantile(values: list[int], fraction: float) -> int:
    values=sorted(values)
    return values[max(0,math.ceil(len(values)*fraction)-1)] if values else 0


def summarize(directory: Path) -> dict:
    rows=[json.loads(line) for line in (directory/'queries.jsonl').read_text().splitlines()]
    if not rows:
        raise ValueError('empty query output')
    success=[row for row in rows if row['termination']!='error']
    durations=[row['e2e_ns'] for row in success]
    summary={'queries':len(rows),'successful':len(success),
             'errors':len(rows)-len(success),'offloaded':sum(row['offloaded'] for row in rows),
             'mean_recall':mean(row['recall'] for row in rows),'backend':sorted({row['backend'] for row in rows}),
             'p50_ns':quantile(durations,.50),'p95_ns':quantile(durations,.95),'p99_ns':quantile(durations,.99),
             'stage_durations_overlap':True,'hardware_speedup_claim':False}
    summary['completed_bytes']={purpose:sum(row.get('transfers',{}).get(purpose,{}).get('completed_bytes',0) for row in rows)
                                for purpose in ['state','prefetch','graph_miss','vectors']}
    summary['prefetch_hits']=sum(row.get('soc',{}).get('prefetch_hits',0) for row in rows)
    summary['graph_misses']=sum(row.get('soc',{}).get('graph_misses',0) for row in rows)
    (directory/'report.json').write_text(json.dumps(summary,indent=2)+'\n')
    lines=['# ANNS 查询报告','',f'- 查询数：{len(rows)}；失败：{summary["errors"]}；交接：{summary["offloaded"]}。',
           f'- 平均 Recall@k：{summary["mean_recall"]:.6f}。',
           f'- p50/p95/p99：{summary["p50_ns"]/1e6:.3f}/{summary["p95_ns"]/1e6:.3f}/{summary["p99_ns"]/1e6:.3f} ms。',
           f'- 后端：{summary["backend"]}；这些耗时不能推断硬件加速比。','',
           '阶段耗时可能重叠，不累加为端到端耗时；传输量为应用字节口径。']
    (directory/'report.md').write_text('\n'.join(lines)+'\n')
    return summary


if __name__=='__main__':
    parser=argparse.ArgumentParser();parser.add_argument('directory',type=Path);args=parser.parse_args()
    print(json.dumps(summarize(args.directory),indent=2))
