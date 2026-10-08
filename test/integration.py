#!/usr/bin/env python3
"""Real ns-3.48 integration checks: topology, MAC identity, packet lineage and metrics."""
import argparse
import collections
import concurrent.futures
import csv
import json
from pathlib import Path
import subprocess
import tempfile


def read_trace(path):
    rows = [json.loads(line) for line in path.read_text().splitlines()]
    by_type = collections.defaultdict(list)
    for row in rows:
        by_type[row['type']].append(row)
    assert len(by_type['run']) == 1 and len(by_type['end']) == 1
    return by_type


def validate(trace, csv_path, expected_hops):
    d = read_trace(trace)
    devices = {v['mac']: v for v in d['device']}
    assert len(devices) == len(d['device']), 'MAC aliases / loopback recorded as real devices'
    assert all(v['mac'] != 0 for v in d['device'])
    ppdus = {p['id']: p for p in d['ppdu']}
    assert len(ppdus) == len(d['ppdu']) > 0
    for p in ppdus.values():
        assert p['sender'] in devices
        assert devices[p['sender']]['node'] == p['node']
        assert devices[p['sender']]['device'] == p['device']
        assert p['endNs'] > p['startNs']
    for rx in d['rx'] + d['rx-drop']:
        assert rx['ppdu'] in ppdus
        assert rx['timeNs'] >= ppdus[rx['ppdu']]['startNs']
        if rx['type'] == 'rx':
            assert rx['timeNs'] >= ppdus[rx['ppdu']]['endNs']
        if rx['ok'] == 0:
            assert rx['snrDb'] is None, 'ns-3 failure SNR must remain unknown'
    paths = collections.defaultdict(set)
    for h in d['hop']:
        if h['event'] == 'RX':
            paths[h['packet']].add(h['hop'])
            assert h['delayMs'] >= 0
    assert paths and max(map(len, paths.values())) == expected_hops, 'Missing complete cross-hop path'
    linked = collections.defaultdict(set)
    for p in ppdus.values():
        for packet in p['packets']:
            linked[packet].add(p['node'])
    assert linked, 'PPDUs lost byte tags during aggregation'
    links = {e['id']: e for e in d['link']}
    complete = [packet for packet, hops in paths.items() if len(hops) == expected_hops]
    assert any(len(linked[p]) >= sum(links[h]['wireless'] for h in paths[p]) for p in complete), 'No complete PPDU lineage'
    summary = list(csv.DictReader(csv_path.open()))[-1]
    start, end = d['run'][0]['startNs'], d['run'][0]['endNs']
    def average(rows, key):
        return sum(r[key] * min(100_000_000, end - r['timeNs']) for r in rows) / (end - start)
    goodput = average(d['total'], 'mbps')
    assert goodput > 0
    assert abs(goodput - float(summary['endToEndMbps'])) < 0.002
    names = {n['node']: n['name'].replace('STA1','STA') for n in d['node']}
    for i in range(1, expected_hops + 1):
        name = summary[f'hop{i}Name']
        h = next(e['id'] for e in d['link'] if names[e['from']]+'->'+names[e['to']] == name)
        rate = average([r for r in d['metric'] if r['hop'] == h], 'mbps')
        assert rate > 0
        assert abs(rate - float(summary[f'hop{i}Mbps'])) < 0.002, (name, rate, summary[f'hop{i}Mbps'])
    return {'ppdus':len(ppdus),'packets':len(paths),'goodputMbps':round(goodput,4),'hops':expected_hops}


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--ns3-root',type=Path,default=Path(__file__).resolve().parents[3])
    parser.add_argument('--work-dir',type=Path,default=Path.home()/'Work')
    parser.add_argument('--report',type=Path)
    args=parser.parse_args()
    root=args.ns3_root.resolve()
    exe=next((root/'build/scratch').glob('ns3.48-mesh_test_obss_metrics-*'))
    args.work_dir.mkdir(parents=True,exist_ok=True)
    results=[]
    with tempfile.TemporaryDirectory(prefix='meshviz-test-',dir=args.work_dir) as temp:
        folder=Path(temp)
        def run_case(mode,assoc,protocol,extra=(),suffix=''):
            case=f'{protocol}-mode{mode}-{assoc}{suffix}'
            out=folder/case;out.mkdir()
            cmd=[str(exe),f'--mode={mode}',f'--staAssoc={assoc}',f'--trafficType={protocol}',
                 '--prewarm=1.3','--test=0.4','--tcpStreams=1','--appRate=10Mbps',
                 f'--out={out / "metrics.csv"}',f'--meshviz={out / "run.jsonl"}',*extra]
            proc=subprocess.run(cmd,cwd=root,text=True,capture_output=True,timeout=120)
            assert proc.returncode==0,case+': '+proc.stderr
            hops=1 if assoc=='ont' else (3 if mode>=5 and assoc=='ap2' else 2)
            result=validate(out/'run.jsonl',out/'metrics.csv',hops)
            return {'case':case,**result},out
        cases=[(m,a,p) for m in range(1,9) for a in ('ont','ap1','ap2') for p in ('tcp','udp')]
        with concurrent.futures.ThreadPoolExecutor(max_workers=2) as pool:
            futures=[pool.submit(run_case,*c) for c in cases]
            for f in concurrent.futures.as_completed(futures):
                result,_=f.result();results.append(result);print('PASS',result,flush=True)
        for proto in ('tcp','udp'):
            result,_=run_case(5,'ap2',proto,('--enableObss=1','--obssRate=1Mbps','--prewarm=2.0'),'-obss')
            results.append(result);print('PASS',result,flush=True)
        result, high=run_case(5,'ap2','tcp',('--appRate=1Gbps','--prewarm=1.5','--test=0.5','--captureDuration=0.06'),'-aggregation')
        high_trace=read_trace(high/'run.jsonl')
        assert max(len(p['packets']) for p in high_trace['ppdu']) > 1
        assert max(p['mpdus'] for p in high_trace['ppdu']) > 1
        results.append(result);print('PASS',result,flush=True)
        result,_=run_case(6,'ap2','udp',('--enableAmpdu=0','--enableAmsdu=0','--test=0.35'),'-unaggregated-partial-bin')
        results.append(result);print('PASS',result,flush=True)
        # Capture limiting must not bias the full-window metrics or alter the simulation.
        base=folder/'udp-mode5-ap2'
        cap=folder/'cap';cap.mkdir()
        common=[str(exe),'--mode=5','--staAssoc=ap2','--trafficType=udp','--prewarm=1.3','--test=0.4','--tcpStreams=1','--appRate=10Mbps']
        subprocess.run(common+[f'--out={cap / "metrics.csv"}',f'--meshviz={cap / "run.jsonl"}','--maxPpdus=2'],cwd=root,check=True,capture_output=True)
        a,b=read_trace(base/'run.jsonl'),read_trace(cap/'run.jsonl')
        assert len(b['ppdu'])==2 and b['end'][0]['skippedPpdus']>0
        assert a['metric']==b['metric'] and a['total']==b['total']
        off=folder/'off.csv'
        subprocess.run(common+[f'--out={off}'],cwd=root,check=True,capture_output=True)
        assert off.read_text()==(base/'metrics.csv').read_text(), 'Instrumentation changed simulation results'
        print('PASS capture cap and recording-on/off invariance',flush=True)
    report={'baseline':'ns-3.48','cases':sorted(results,key=lambda r:r['case']),'captureCap':'passed','instrumentationInvariance':'passed'}
    if args.report:
        args.report.parent.mkdir(parents=True,exist_ok=True)
        args.report.write_text(json.dumps(report,indent=2)+'\n')
    print(f'PASS {len(results)} scenarios + cap/invariance checks')

if __name__=='__main__':main()
