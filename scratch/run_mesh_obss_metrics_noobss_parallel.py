#!/usr/bin/env python3
import argparse
import subprocess
import time
from concurrent.futures import ThreadPoolExecutor, as_completed
from pathlib import Path
from typing import Dict, List, Tuple

import mesh_test_obss as base
from mesh_test_obss_metrics import (
    average_metric_rows,
    generate_best_assoc_outputs,
    read_last_metrics,
    write_rows,
)


MODES = range(1, 9)
ASSOCS = ["ont", "ap1", "ap2"]


def executable(root: Path) -> Path:
    candidates = [
        root / "build" / "scratch" / "ns3.48-mesh_test_obss_metrics-optimized",
        root / "build" / "scratch" / "ns3.48-mesh_test_obss_metrics-default",
        root / "build" / "scratch" / "ns3.48-mesh_test_obss_metrics-debug",
    ]
    for path in candidates:
        if path.exists():
            return path
    raise FileNotFoundError("Build scratch/mesh_test_obss_metrics first.")


def task_key(mode: int, assoc: str, x: float, y: float, seed: int) -> str:
    return f"mode{mode}_{assoc}_x{x:.4f}_y{y:.4f}_run{seed}".replace("-", "m").replace(".", "p")


def mode_grid(args, mode: int) -> Tuple[List[float], List[float], Tuple[float, float, float, float]]:
    cfg = base.resolve_mode(mode)
    dx_min, dx_max, dy_min, dy_max = base.default_bounds(cfg)
    x_min = dx_min if args.x_min is None else args.x_min
    x_max = dx_max if args.x_max is None else args.x_max
    y_min = dy_min if args.y_min is None else args.y_min
    y_max = dy_max if args.y_max is None else args.y_max
    return base.grid_centers(x_min, x_max, args.nx), base.grid_centers(y_min, y_max, args.ny), (x_min, x_max, y_min, y_max)


def run_task(root: Path, exe: Path, out_dir: Path, args, mode: int, assoc: str, x: float, y: float, seed: int) -> Path:
    raw_dir = out_dir / ".raw_tmp"
    raw_dir.mkdir(exist_ok=True)
    raw_csv = raw_dir / f"{task_key(mode, assoc, x, y, seed)}.csv"
    done = raw_csv.with_suffix(".done")
    if done.exists() and raw_csv.exists():
        return raw_csv
    if raw_csv.exists():
        raw_csv.unlink()

    cmd = [
        str(exe),
        f"--mode={mode}",
        f"--staAssoc={assoc}",
        f"--run={seed}",
        f"--prewarm={args.prewarm}",
        f"--test={args.test}",
        "--useStaPos=1",
        f"--staX={x:.4f}",
        f"--staY={y:.4f}",
        f"--tcpStreams={args.tcp_streams}",
        f"--trafficType={args.traffic_type}",
        f"--appRate={args.app_rate}",
        f"--enableObss={args.enable_obss}",
        f"--enableObss1={args.enable_obss1}",
        f"--enableObss2={args.enable_obss2}",
        f"--obssTargetDuty={args.obss_target_duty}",
        f"--obssLinkRateMbps={args.obss_link_rate_mbps}",
        f"--obssRate={args.obss_rate}",
        "--obssApX=4.0",
        "--obssApY=4.0",
        "--obssStaX=5.0",
        "--obssStaY=4.0",
        "--obssAp2X=8.0",
        "--obssAp2Y=6.0",
        "--obssSta2X=9.0",
        "--obssSta2Y=6.0",
        f"--out={raw_csv}",
    ]
    subprocess.run(cmd, cwd=root, check=True, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, text=True)
    if not raw_csv.exists():
        raise RuntimeError(f"Missing raw CSV after task: {raw_csv}")
    done.write_text("ok\n")
    return raw_csv


def aggregate(out_dir: Path, args) -> None:
    seeds = [int(item.strip()) for item in args.seed_runs.split(",") if item.strip()]
    obss_points: List[Tuple[str, float, float]] = []
    if args.enable_obss and args.enable_obss1:
        obss_points.append(("OBSS1", 4.0, 4.0))
    if args.enable_obss and args.enable_obss2:
        obss_points.append(("OBSS2", 8.0, 6.0))

    for mode in MODES:
        cfg = base.resolve_mode(mode)
        xs, ys, bounds = mode_grid(args, mode)
        for assoc in ASSOCS:
            rows: List[Dict[str, str]] = []
            samples: List[Tuple[float, float, float]] = []
            for y in ys:
                for x in xs:
                    seed_rows = [
                        read_last_metrics(out_dir / ".raw_tmp" / f"{task_key(mode, assoc, x, y, seed)}.csv")
                        for seed in seeds
                    ]
                    avg = average_metric_rows(seed_rows)
                    rows.append(avg)
                    samples.append((float(avg["x"]), float(avg["y"]), float(avg["endToEndMbps"])))
            csv_path = out_dir / f"mode{mode}_{assoc}.csv"
            svg_path = out_dir / f"mode{mode}_{assoc}.svg"
            write_rows(csv_path, rows, list(rows[0].keys()))
            base.draw_svg(svg_path, cfg, samples, bounds[0], bounds[1], bounds[2], bounds[3], "End-to-End", obss_points)

        generate_best_assoc_outputs(
            mode,
            cfg,
            out_dir,
            out_dir / f"mode{mode}_best_assoc.csv",
            out_dir / f"mode{mode}_best_assoc.svg",
            bounds[0],
            bounds[1],
            bounds[2],
            bounds[3],
            obss_points,
        )


def main() -> int:
    parser = argparse.ArgumentParser(description="Parallel scan for mesh_test_obss_metrics.")
    parser.add_argument("--out-dir", type=Path, required=True)
    parser.add_argument("--nx", type=int, default=7)
    parser.add_argument("--ny", type=int, default=7)
    parser.add_argument("--seed-runs", type=str, default="1,2,3")
    parser.add_argument("--prewarm", type=float, default=1.0)
    parser.add_argument("--test", type=float, default=4.0)
    parser.add_argument("--tcp-streams", type=int, default=1)
    parser.add_argument("--traffic-type", type=str, choices=["tcp", "udp"], default="tcp")
    parser.add_argument("--app-rate", type=str, default="20Gbps")
    parser.add_argument("--enable-obss", type=int, choices=[0, 1], default=0)
    parser.add_argument("--enable-obss1", type=int, choices=[0, 1], default=0)
    parser.add_argument("--enable-obss2", type=int, choices=[0, 1], default=0)
    parser.add_argument("--obss-target-duty", type=float, default=0.15)
    parser.add_argument("--obss-link-rate-mbps", type=float, default=1600.0)
    parser.add_argument("--obss-rate", type=str, default="150Mbps")
    parser.add_argument("--jobs", type=int, default=12)
    parser.add_argument("--x-min", type=float, default=None)
    parser.add_argument("--x-max", type=float, default=None)
    parser.add_argument("--y-min", type=float, default=None)
    parser.add_argument("--y-max", type=float, default=None)
    args = parser.parse_args()

    root = next(p for p in Path(__file__).resolve().parents if (p / "ns3").is_file())
    exe = executable(root)
    out_dir = args.out_dir
    out_dir.mkdir(parents=True, exist_ok=True)

    seeds = [int(item.strip()) for item in args.seed_runs.split(",") if item.strip()]
    tasks = []
    for mode in MODES:
        xs, ys, _ = mode_grid(args, mode)
        tasks.extend(
            (mode, assoc, x, y, seed)
            for assoc in ASSOCS
            for y in ys
            for x in xs
            for seed in seeds
        )

    print(f"out_dir={out_dir}")
    print(f"exe={exe}")
    print(f"tasks={len(tasks)} jobs={args.jobs}")
    start = time.time()
    completed = 0
    with ThreadPoolExecutor(max_workers=args.jobs) as pool:
        futures = [pool.submit(run_task, root, exe, out_dir, args, *task) for task in tasks]
        for future in as_completed(futures):
            future.result()
            completed += 1
            if completed % 50 == 0 or completed == len(tasks):
                elapsed = time.time() - start
                rate = completed / elapsed if elapsed > 0 else 0.0
                eta = (len(tasks) - completed) / rate if rate > 0 else 0.0
                print(f"[PROGRESS] {completed}/{len(tasks)} elapsed={elapsed/60:.1f}m eta={eta/60:.1f}m", flush=True)

    aggregate(out_dir, args)
    subprocess.run(
        [
            "python3",
            "scratch/mesh_obss_metrics_full_20260604_metrics_full/build_metric_excel_tables.py",
            "--input-dir",
            str(out_dir),
            "--output-dir",
            str(out_dir),
        ],
        cwd=root,
        check=True,
    )
    print(out_dir)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
