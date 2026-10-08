#!/usr/bin/env python3
"""Exercise ns3 CLI recording and shell-free automatic viewer launch."""
import argparse
import json
from pathlib import Path
import shutil
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--ns3-root', type=Path, default=Path(__file__).resolve().parents[3])
    args = parser.parse_args()
    root = args.ns3_root.resolve()
    work = Path.home() / 'Work'
    work.mkdir(exist_ok=True)
    common = ['--mode=1', '--staAssoc=ap1', '--trafficType=udp', '--tcpStreams=1',
              '--appRate=10Mbps', '--prewarm=1.3', '--test=0.3', '--captureDuration=0.03']
    with tempfile.TemporaryDirectory(prefix='meshviz-cli-', dir=work) as temp:
        folder = Path(temp)
        program = ' '.join(['mesh_test_obss_metrics', *common,
                            '--enableMeshviz=1', '--openMeshviz=0'])
        for _ in range(2):
            subprocess.run([str(root/'ns3'), 'run', program, '--no-build', '--cwd', str(folder)],
                           cwd=root, check=True, capture_output=True, text=True, timeout=60)
        runs = list((folder/'meshviz-results').glob('*/run.jsonl'))
        assert len(runs) == 2, 'Repeated runs must not overwrite each other'
        for run in runs:
            rows = [json.loads(line) for line in run.read_text().splitlines()]
            assert rows[-1]['type'] == 'end'
            assert any(row['type'] == 'ppdu' for row in rows)
            assert (run.parent/'metrics.csv').is_file()
            edges = [r for r in rows if r['type'] == 'link']
            assert len([r for r in edges if r['from'] == 0]) == 2, 'Expected star'
        executable = next((root/'build/scratch').glob('ns3.48-mesh_test_obss_metrics-*'))
        # Copy only the executable; its existing build RPATH resolves ns-3 libraries.
        # A short-lived viewer stand-in validates arguments and completed output.
        sandbox = folder/'build with spaces'
        simulation = sandbox/'scratch'/executable.name
        simulation.parent.mkdir(parents=True)
        shutil.copy2(executable, simulation)
        viewer = sandbox/'contrib/meshviz/meshviz-viewer'
        viewer.parent.mkdir(parents=True)
        viewer.write_text('#!/usr/bin/env python3\nimport json,sys\nfrom pathlib import Path\n'
                          'p=Path(sys.argv[1])\n'
                          'assert len(sys.argv)==2\n'
                          'assert json.loads(p.read_text().splitlines()[-1])["type"]=="end"\n'
                          'p.with_suffix(".opened").write_text(str(p))\n')
        viewer.chmod(0o755)
        trace = folder/'result with spaces; $literal'/"run's.jsonl"
        csv = trace.with_name('metric results.csv')
        subprocess.run([str(simulation), *common, '--enableMeshviz=1', f'--meshviz={trace}', f'--out={csv}'],
                       cwd=folder, check=True, capture_output=True, text=True, timeout=60)
        assert csv.is_file()
        assert trace.with_suffix('.opened').exists()
        assert trace.with_suffix('.opened').read_text() == str(trace)
        off = folder/'disabled'
        off.mkdir()
        subprocess.run([str(simulation), *common, '--enableMeshviz=0'], cwd=off,
                       check=True, capture_output=True, text=True, timeout=60)
        assert not (off/'meshviz-results').exists()
        assert not list(off.glob('*.jsonl'))
        print('PASS: ns3 star CLI, unique per-run outputs, completed trace before viewer, '
              'literal paths with spaces/metacharacters, disabled capture')


if __name__ == '__main__':
    main()
