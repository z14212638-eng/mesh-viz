#!/usr/bin/env python3
import argparse
import csv
import os
import subprocess
from pathlib import Path
from typing import Dict, List, Tuple

import mesh_test_obss as base


def build_ns3_command(root: Path, no_build: bool, sim_args: str) -> str:
    if no_build:
        candidates = [
            root / "build" / "scratch" / "ns3.48-mesh_test_obss_metrics-optimized",
            root / "build" / "scratch" / "ns3.48-mesh_test_obss_metrics-default",
            root / "build" / "scratch" / "ns3.48-mesh_test_obss_metrics-debug",
        ]
        existing = [path for path in candidates if path.exists()]
        if not existing:
            raise FileNotFoundError(
                "Expected built executable not found under build/scratch for mesh_test_obss_metrics. "
                "Build scratch/mesh_test_obss_metrics first or rerun without --no-build."
            )
        # Prefer optimized/default over debug for the trace-heavy metrics run.
        for path in candidates:
            if path.exists():
                return f'"{path}" {sim_args}'
    return f'./ns3 run "scratch/mesh_test_obss_metrics {sim_args}"'


def read_last_metrics(csv_path: Path) -> Dict[str, str]:
    with csv_path.open("r", newline="") as f:
        rows = list(csv.DictReader(f))
    if not rows:
        raise RuntimeError(f"No rows found in {csv_path}")
    return rows[-1]


def to_float(value: str):
    if value is None or value == "":
        return None
    try:
        return float(value)
    except ValueError:
        return None


def average_metric_rows(rows: List[Dict[str, str]]) -> Dict[str, str]:
    first = rows[0]
    out: Dict[str, str] = {
        "mode": first.get("mode", ""),
        "scenario": first.get("scenario", ""),
        "staAssoc": first.get("staAssoc", ""),
        "x": first.get("staX", ""),
        "y": first.get("staY", ""),
        "runCount": str(len(rows)),
    }

    skip = {"mode", "scenario", "staAssoc", "run", "staX", "staY"}
    for key in first.keys():
        if key in skip:
            continue
        values = [to_float(row.get(key, "")) for row in rows]
        numeric = [v for v in values if v is not None]
        if len(numeric) == len(rows):
            out[key] = f"{sum(numeric) / len(numeric):.6f}"
        else:
            out[key] = first.get(key, "")
    return out


def write_rows(path: Path, rows: List[Dict[str, str]], fieldnames: List[str]) -> None:
    with path.open("w", newline="") as f:
        writer = csv.DictWriter(f, fieldnames=fieldnames)
        writer.writeheader()
        writer.writerows(rows)


def generate_best_assoc_outputs(mode: int,
                                cfg: base.ModeConfig,
                                input_dir: Path,
                                out_csv: Path,
                                out_svg: Path,
                                x_min: float,
                                x_max: float,
                                y_min: float,
                                y_max: float,
                                obss_points: List[Tuple[str, float, float]]) -> None:
    assoc_rows: Dict[str, Dict[Tuple[str, str], Dict[str, str]]] = {}
    for assoc in ["ont", "ap1", "ap2"]:
        path = input_dir / f"mode{mode}_{assoc}.csv"
        with path.open("r", newline="") as f:
            assoc_rows[assoc] = {(row["x"], row["y"]): row for row in csv.DictReader(f)}

    keys = sorted(assoc_rows["ont"].keys(), key=lambda item: (float(item[1]), float(item[0])))
    rows = []
    samples = []
    for key in keys:
        candidates = []
        for assoc in ["ont", "ap1", "ap2"]:
            row = assoc_rows[assoc][key]
            candidates.append((assoc, float(row["endToEndMbps"])))
        best_assoc, best_thpt = max(candidates, key=lambda item: item[1])
        x, y = key
        rows.append({
            "mode": str(mode),
            "scenario": cfg.name,
            "x": x,
            "y": y,
            "bestAssoc": best_assoc,
            "bestThroughputMbps": f"{best_thpt:.3f}",
            "ontThroughputMbps": f"{candidates[0][1]:.3f}",
            "ap1ThroughputMbps": f"{candidates[1][1]:.3f}",
            "ap2ThroughputMbps": f"{candidates[2][1]:.3f}",
        })
        samples.append((float(x), float(y), best_assoc, best_thpt))

    write_rows(out_csv, rows, [
        "mode", "scenario", "x", "y", "bestAssoc", "bestThroughputMbps",
        "ontThroughputMbps", "ap1ThroughputMbps", "ap2ThroughputMbps",
    ])
    base.draw_best_assoc_svg(out_svg, cfg, samples, x_min, x_max, y_min, y_max, obss_points)


def run_one(args) -> int:
    cfg = base.resolve_mode(args.mode)
    dx_min, dx_max, dy_min, dy_max = base.default_bounds(cfg)
    x_min = dx_min if args.x_min is None else args.x_min
    x_max = dx_max if args.x_max is None else args.x_max
    y_min = dy_min if args.y_min is None else args.y_min
    y_max = dy_max if args.y_max is None else args.y_max

    root = next(p for p in Path(__file__).resolve().parents if (p / "ns3").is_file())
    out_dir = args.out_dir if args.out_dir is not None else root / "scratch" / "mesh_obss_metrics_results"
    out_dir.mkdir(parents=True, exist_ok=True)

    obss_points = []
    if args.enable_obss and args.enable_obss1:
        obss_points.append(("OBSS1", args.obss_ap_x, args.obss_ap_y))
    if args.enable_obss and args.enable_obss2:
        obss_points.append(("OBSS2", args.obss_ap2_x, args.obss_ap2_y))

    if args.best_assoc:
        generate_best_assoc_outputs(
            args.mode,
            cfg,
            args.best_input_dir if args.best_input_dir is not None else out_dir,
            args.csv if args.csv is not None else out_dir / f"mode{args.mode}_best_assoc.csv",
            args.svg if args.svg is not None else out_dir / f"mode{args.mode}_best_assoc.svg",
            x_min,
            x_max,
            y_min,
            y_max,
            obss_points,
        )
        return 0

    seed_runs = [int(item.strip()) for item in args.seed_runs.split(",") if item.strip()]
    xs = base.grid_centers(x_min, x_max, args.nx)
    ys = base.grid_centers(y_min, y_max, args.ny)
    out_csv = args.csv if args.csv is not None else out_dir / f"mode{args.mode}_{args.sta_assoc}.csv"
    out_svg = args.svg if args.svg is not None else out_dir / f"mode{args.mode}_{args.sta_assoc}.svg"
    tmp_dir = out_csv.parent / ".scan_tmp"
    tmp_dir.mkdir(exist_ok=True)

    rows: List[Dict[str, str]] = []
    fieldnames: List[str] = []
    samples: List[Tuple[float, float, float]] = []

    for y in ys:
        for x in xs:
            print(f"[SCAN] mode={args.mode} assoc={args.sta_assoc} x={x:.2f} y={y:.2f}", flush=True)
            seed_rows: List[Dict[str, str]] = []
            for seed_run in seed_runs:
                ns3_csv = tmp_dir / f"metrics_mode{args.mode}_{args.sta_assoc}_run{seed_run}_pid{os.getpid()}.csv"
                if ns3_csv.exists():
                    ns3_csv.unlink()
                rts_arg = f" --rtsCtsThreshold={args.rts_threshold}" if args.rts_threshold is not None else ""
                tcp_arg = f" --tcpStreams={args.tcp_streams}" if args.tcp_streams is not None else ""
                traffic_arg = f" --trafficType={args.traffic_type}" if args.traffic_type else ""
                rate_arg = f" --appRate={args.app_rate}" if args.app_rate is not None else ""
                wired_arg = f" --wiredRate={args.wired_rate}" if args.wired_rate is not None else ""
                ampdu_arg = f" --enableAmpdu={args.enable_ampdu}" if args.enable_ampdu is not None else ""
                amsdu_arg = f" --enableAmsdu={args.enable_amsdu}" if args.enable_amsdu is not None else ""
                obss_rate_arg = f" --obssRate={args.obss_rate}" if args.obss_rate else ""
                obss_arg = (
                    f" --enableObss={args.enable_obss}"
                    f" --enableObss1={args.enable_obss1}"
                    f" --enableObss2={args.enable_obss2}"
                    f" --obssTargetDuty={args.obss_target_duty}"
                    f" --obssLinkRateMbps={args.obss_link_rate_mbps}"
                    f"{obss_rate_arg}"
                    f" --obssApX={args.obss_ap_x} --obssApY={args.obss_ap_y}"
                    f" --obssStaX={args.obss_sta_x} --obssStaY={args.obss_sta_y}"
                    f" --obssAp2X={args.obss_ap2_x} --obssAp2Y={args.obss_ap2_y}"
                    f" --obssSta2X={args.obss_sta2_x} --obssSta2Y={args.obss_sta2_y}"
                )
                sim_args = (
                    f"--mode={args.mode} --staAssoc={args.sta_assoc} "
                    f"--run={seed_run} --prewarm={args.prewarm} --test={args.test} "
                    f"--useStaPos=1 --staX={x:.4f} --staY={y:.4f}"
                    f"{rts_arg}{tcp_arg}{traffic_arg}{rate_arg}{wired_arg}{ampdu_arg}{amsdu_arg}{obss_arg} --out={ns3_csv}"
                )
                subprocess.run(build_ns3_command(root, args.no_build, sim_args), cwd=root, shell=True, check=True)
                seed_rows.append(read_last_metrics(ns3_csv))
                if ns3_csv.exists():
                    ns3_csv.unlink()

            avg_row = average_metric_rows(seed_rows)
            if not fieldnames:
                fieldnames = list(avg_row.keys())
                write_rows(out_csv, [], fieldnames)
            rows.append(avg_row)
            with out_csv.open("a", newline="") as f:
                writer = csv.DictWriter(f, fieldnames=fieldnames)
                writer.writerow(avg_row)
                f.flush()
            samples.append((float(avg_row["x"]), float(avg_row["y"]), float(avg_row["endToEndMbps"])))
            print(f"        endToEndAvg={float(avg_row['endToEndMbps']):.3f} Mbps", flush=True)

    base.draw_svg(out_svg, cfg, samples, x_min, x_max, y_min, y_max, "End-to-End", obss_points)
    try:
        tmp_dir.rmdir()
    except OSError:
        pass
    print(f"[OK] CSV: {out_csv}")
    print(f"[OK] SVG: {out_svg}")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description="Static grid metrics scan for mesh_test_obss_metrics")
    parser.add_argument("--mode", type=int, required=True)
    parser.add_argument("--sta-assoc", type=str, default="ap1", choices=["ont", "ap1", "ap2"])
    parser.add_argument("--best-assoc", action="store_true")
    parser.add_argument("--best-input-dir", type=Path, default=None)
    parser.add_argument("--out-dir", type=Path, default=None)
    parser.add_argument("--nx", type=int, default=7)
    parser.add_argument("--ny", type=int, default=7)
    parser.add_argument("--seed-runs", type=str, default="1,2,3")
    parser.add_argument("--prewarm", type=float, default=1.0)
    parser.add_argument("--test", type=float, default=4.0)
    parser.add_argument("--no-build", action="store_true")
    parser.add_argument("--rts-threshold", type=int, default=None)
    parser.add_argument("--tcp-streams", type=int, default=1)
    parser.add_argument("--traffic-type", type=str, choices=["tcp", "udp"], default="tcp")
    parser.add_argument("--app-rate", type=str, default="20Gbps")
    parser.add_argument("--wired-rate", type=str, default=None)
    parser.add_argument("--enable-ampdu", type=int, choices=[0, 1], default=None)
    parser.add_argument("--enable-amsdu", type=int, choices=[0, 1], default=None)
    parser.add_argument("--enable-obss", type=int, choices=[0, 1], default=1)
    parser.add_argument("--enable-obss1", type=int, choices=[0, 1], default=1)
    parser.add_argument("--enable-obss2", type=int, choices=[0, 1], default=0)
    parser.add_argument("--obss-target-duty", type=float, default=0.15)
    parser.add_argument("--obss-link-rate-mbps", type=float, default=1600.0)
    parser.add_argument("--obss-rate", type=str, default="150Mbps")
    parser.add_argument("--obss-ap-x", type=float, default=4.0)
    parser.add_argument("--obss-ap-y", type=float, default=4.0)
    parser.add_argument("--obss-sta-x", type=float, default=5.0)
    parser.add_argument("--obss-sta-y", type=float, default=4.0)
    parser.add_argument("--obss-ap2-x", type=float, default=8.0)
    parser.add_argument("--obss-ap2-y", type=float, default=6.0)
    parser.add_argument("--obss-sta2-x", type=float, default=9.0)
    parser.add_argument("--obss-sta2-y", type=float, default=6.0)
    parser.add_argument("--x-min", type=float, default=None)
    parser.add_argument("--x-max", type=float, default=None)
    parser.add_argument("--y-min", type=float, default=None)
    parser.add_argument("--y-max", type=float, default=None)
    parser.add_argument("--csv", type=Path, default=None)
    parser.add_argument("--svg", type=Path, default=None)
    args = parser.parse_args()
    return run_one(args)


if __name__ == "__main__":
    raise SystemExit(main())
