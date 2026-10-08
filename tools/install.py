#!/usr/bin/env python3
"""Install bundled scratch scripts without overwriting unrelated local edits."""
import argparse
import shutil
from pathlib import Path

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--ns3-root', type=Path, default=Path(__file__).resolve().parents[3])
    args = parser.parse_args()
    root = args.ns3_root.resolve()
    if not (root / 'ns3').is_file():
        parser.error('Expected an ns-3 source root')
    source = Path(__file__).resolve().parents[1] / 'scratch'
    files = [(p, root / 'scratch' / p.relative_to(source)) for p in source.rglob('*') if p.is_file() and '__pycache__' not in p.parts]
    conflicts = [str(dst) for src, dst in files if dst.exists() and dst.read_bytes() != src.read_bytes()]
    if conflicts:
        parser.error('Refusing to overwrite different files; back them up or merge first:\n' + '\n'.join(conflicts))
    for src, dst in files:
        dst.parent.mkdir(parents=True, exist_ok=True)
        if not dst.exists():
            shutil.copy2(src, dst)
    print(f'Installed {len(files)} scratch files in {root / "scratch"}')

if __name__ == '__main__':
    main()
