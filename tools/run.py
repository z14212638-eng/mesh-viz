#!/usr/bin/env python3
"""Run one mesh simulation, then open exactly that run in the offline viewer."""
import argparse
import datetime
import os
from pathlib import Path
import subprocess

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--ns3-root', type=Path, default=Path(__file__).resolve().parents[3])
    parser.add_argument('--output', type=Path, help='New result directory (must not exist)')
    parser.add_argument('--no-gui', action='store_true')
    parser.add_argument('--no-build', action='store_true')
    parser.add_argument('simulation_args', nargs=argparse.REMAINDER, help='Arguments after -- go to the scratch scenario')
    args = parser.parse_args()
    root = args.ns3_root.resolve()
    if not (root/'ns3').is_file(): parser.error('ns-3 root not found')
    output = (args.output or root/'meshviz-results'/datetime.datetime.now().strftime('%Y%m%d-%H%M%S-%f')).resolve()
    simargs = args.simulation_args
    if simargs[:1] == ['--']: simargs = simargs[1:]
    if any(x.split('=')[0] in ('--out', '--meshviz') for x in simargs): parser.error('--out / --meshviz are managed by this runner; use --output')
    if not args.no_build:
        subprocess.run(['./ns3', 'build', 'mesh_test_obss_metrics', '-j', '4'], cwd=root, check=True)
        if not args.no_gui: subprocess.run(['cmake','--build','cmake-cache','--target','meshviz-viewer','-j','2'],cwd=root,check=True)
    version = (root/'VERSION').read_text().strip()
    candidates = [root/'build'/'scratch'/f'ns{version}-mesh_test_obss_metrics-{p}' for p in ('optimized','default','debug','release')]
    exe = next((p for p in candidates if p.is_file()), None)
    if exe is None: parser.error('Build mesh_test_obss_metrics first')
    output.mkdir(parents=True, exist_ok=False)
    # Practical single-run defaults. Later explicit options override these.
    cmd = [str(exe), '--mode=5','--staAssoc=ap2','--prewarm=1.2','--test=0.8','--tcpStreams=1','--appRate=20Mbps'] + simargs
    cmd += [f'--out={output / "metrics.csv"}', f'--meshviz={output / "run.jsonl"}']
    print('Running:', ' '.join(cmd), flush=True)
    subprocess.run(cmd,cwd=root,check=True)
    print(f'Results: {output}', flush=True)
    if not args.no_gui:
        viewer=root/'build/contrib/meshviz/meshviz-viewer'
        if not viewer.is_file(): parser.error('Qt viewer not built; build meshviz-viewer or use --no-gui')
        subprocess.run([str(viewer),str(output/'run.jsonl')],cwd=root,check=True)

if __name__ == '__main__':
    main()
