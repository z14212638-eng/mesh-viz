#!/usr/bin/env python3
import argparse
import csv
import os
import subprocess
from dataclasses import dataclass
from pathlib import Path
from typing import Dict, List, Tuple


@dataclass
class ModeConfig:
    topology: str  # star | chain
    link_a_wired: bool
    link_b_wired: bool
    name: str


def resolve_mode(mode: int) -> ModeConfig:
    mapping = {
        1: ModeConfig("star", False, False, "L1_STAR_WW"),
        2: ModeConfig("star", True, False, "L1_STAR_EW"),
        3: ModeConfig("star", False, True, "L1_STAR_WE"),
        4: ModeConfig("star", True, True, "L1_STAR_EE"),
        5: ModeConfig("chain", False, False, "L2_CHAIN_WW"),
        6: ModeConfig("chain", True, False, "L2_CHAIN_EW"),
        7: ModeConfig("chain", False, True, "L2_CHAIN_WE"),
        8: ModeConfig("chain", True, True, "L2_CHAIN_EE"),
    }
    if mode not in mapping:
        raise ValueError(f"Invalid mode={mode}, expected 1..8")
    return mapping[mode]


def node_positions(cfg: ModeConfig) -> Dict[str, Tuple[float, float]]:
    pos = {
        "ONT": (0.0, 0.0),
        "AP1": (10.0, 0.0),
    }
    pos["AP2"] = (10.0, 8.0)
    return pos


def topology_links(cfg: ModeConfig) -> List[Tuple[str, str, str]]:
    # (from, to, type)
    if cfg.topology == "star":
        return [
            ("ONT", "AP1", "wired" if cfg.link_a_wired else "wireless"),
            ("ONT", "AP2", "wired" if cfg.link_b_wired else "wireless"),
        ]
    return [
        ("ONT", "AP1", "wired" if cfg.link_a_wired else "wireless"),
        ("AP1", "AP2", "wired" if cfg.link_b_wired else "wireless"),
    ]


def linspace(start: float, end: float, n: int) -> List[float]:
    if n <= 1:
        return [(start + end) * 0.5]
    step = (end - start) / (n - 1)
    return [start + i * step for i in range(n)]


def grid_edges(start: float, end: float, n_cells: int) -> List[float]:
    return linspace(start, end, n_cells + 1)


def grid_centers(start: float, end: float, n_cells: int) -> List[float]:
    edges = grid_edges(start, end, n_cells)
    return [(edges[i] + edges[i + 1]) / 2.0 for i in range(n_cells)]


def default_bounds(cfg: ModeConfig) -> Tuple[float, float, float, float]:
    _ = cfg
    return (-2.0, 12.0, -3.0, 11.0)


def read_last_metrics(csv_path: Path) -> Dict[str, str]:
    with csv_path.open("r", newline="") as f:
        rows = list(csv.DictReader(f))
    if not rows:
        raise RuntimeError(f"No rows found in {csv_path}")
    return rows[-1]


def build_ns3_command(
    root: Path,
    no_build: bool,
    sim_args: str,
) -> str:
    if no_build:
        candidates = [
            root / "build" / "scratch" / "ns3.48-mesh_test_obss_metrics-debug",
            root / "build" / "scratch" / "ns3.48-mesh_test_obss_metrics-default",
            root / "build" / "scratch" / "ns3.48-mesh_test_obss_metrics-optimized",
        ]
        existing = [path for path in candidates if path.exists()]
        if not existing:
            raise FileNotFoundError(
                "Expected built executable not found under build/scratch for mesh_test_obss. "
                "Build scratch/mesh_test_obss_metrics first or rerun without --no-build."
            )
        exe = max(existing, key=lambda path: path.stat().st_mtime)
        return f'"{exe}" {sim_args}'
    return f'./ns3 run "scratch/mesh_test_obss_metrics {sim_args}"'


def color_map(value: float, vmin: float, vmax: float) -> str:
    if vmax <= vmin:
        t = 0.5
    else:
        t = max(0.0, min(1.0, (value - vmin) / (vmax - vmin)))
    # blue -> red
    r = int(40 + 215 * t)
    g = int(100 + 80 * (1 - abs(t - 0.5) * 2))
    b = int(255 - 215 * t)
    return f"rgb({r},{g},{b})"


def label_color(value: float, vmin: float, vmax: float) -> str:
    if vmax <= vmin:
        t = 0.5
    else:
        t = max(0.0, min(1.0, (value - vmin) / (vmax - vmin)))
    return "#f8fafc" if t > 0.58 else "#111827"


def draw_svg(
    out_svg: Path,
    cfg: ModeConfig,
    samples: List[Tuple[float, float, float]],
    x_min: float,
    x_max: float,
    y_min: float,
    y_max: float,
    metric_label: str,
    obss_points: List[Tuple[str, float, float]],
) -> None:
    width, height = 1100, 760
    margin_l, margin_r, margin_t, margin_b = 90, 40, 70, 90
    plot_w = width - margin_l - margin_r
    plot_h = height - margin_t - margin_b

    def sx(x: float) -> float:
        return margin_l + (x - x_min) / (x_max - x_min) * plot_w

    def sy(y: float) -> float:
        return margin_t + (y_max - y) / (y_max - y_min) * plot_h

    nodes = node_positions(cfg)
    links = topology_links(cfg)
    throughputs = [t for _, _, t in samples]
    vmin = min(throughputs) if throughputs else 0.0
    vmax = max(throughputs) if throughputs else 1.0
    xs = sorted({x for x, _, _ in samples})
    ys = sorted({y for _, y, _ in samples})
    x_edges = grid_edges(x_min, x_max, len(xs))
    y_edges = grid_edges(y_min, y_max, len(ys))

    lines: List[str] = []
    lines.append(f'<svg xmlns="http://www.w3.org/2000/svg" width="{width}" height="{height}" viewBox="0 0 {width} {height}">')
    lines.append('<rect x="0" y="0" width="100%" height="100%" fill="#ffffff"/>')

    # Title
    lines.append(f'<text x="{width/2}" y="36" text-anchor="middle" font-size="24" font-family="Arial" fill="#1f2937">')
    lines.append(f'MESH Grid Throughput Scan: {cfg.name} ({metric_label})</text>')

    # Plot frame
    lines.append(f'<rect x="{margin_l}" y="{margin_t}" width="{plot_w}" height="{plot_h}" fill="#f8fafc" stroke="#d1d5db"/>')

    # Heatmap cells
    sample_map = {(x, y): t for x, y, t in samples}
    for y_idx, y in enumerate(ys):
        y0 = y_edges[y_idx]
        y1 = y_edges[y_idx + 1]
        for x_idx, x in enumerate(xs):
            x0 = x_edges[x_idx]
            x1 = x_edges[x_idx + 1]
            thpt = sample_map[(x, y)]
            fill = color_map(thpt, vmin, vmax)
            cx = (x0 + x1) / 2.0
            cy = (y0 + y1) / 2.0
            lines.append(
                f'<rect x="{sx(x0):.2f}" y="{sy(y1):.2f}" width="{sx(x1) - sx(x0):.2f}" '
                f'height="{sy(y0) - sy(y1):.2f}" fill="{fill}" fill-opacity="0.78" stroke="#e5e7eb" stroke-width="1"/>'
            )
            lines.append(
                f'<text x="{sx(cx):.2f}" y="{sy(cy) + 4:.2f}" text-anchor="middle" font-size="12" '
                f'font-family="Arial" fill="{label_color(thpt, vmin, vmax)}">{thpt:.1f}</text>'
            )

    # Grid and ticks
    for xt in x_edges:
        xpix = sx(xt)
        lines.append(f'<line x1="{xpix:.2f}" y1="{margin_t}" x2="{xpix:.2f}" y2="{margin_t + plot_h}" stroke="#cbd5e1" stroke-width="1.2"/>')
        lines.append(f'<text x="{xpix:.2f}" y="{margin_t + plot_h + 24}" text-anchor="middle" font-size="13" font-family="Arial" fill="#374151">{xt:.1f}</text>')

    for yt in y_edges:
        ypix = sy(yt)
        lines.append(f'<line x1="{margin_l}" y1="{ypix:.2f}" x2="{margin_l + plot_w}" y2="{ypix:.2f}" stroke="#cbd5e1" stroke-width="1.2"/>')
        lines.append(f'<text x="{margin_l - 10}" y="{ypix + 4:.2f}" text-anchor="end" font-size="13" font-family="Arial" fill="#374151">{yt:.1f}</text>')

    # Axis labels
    lines.append(f'<text x="{margin_l + plot_w/2}" y="{height - 28}" text-anchor="middle" font-size="16" font-family="Arial" fill="#111827">X position (m)</text>')
    lines.append(f'<text x="28" y="{margin_t + plot_h/2}" transform="rotate(-90 28 {margin_t + plot_h/2})" text-anchor="middle" font-size="16" font-family="Arial" fill="#111827">Y position (m)</text>')

    # Topology links
    for a, b, lt in links:
        x1, y1 = nodes[a]
        x2, y2 = nodes[b]
        dash = "7,7" if lt == "wireless" else ""
        color = "#2563eb" if lt == "wireless" else "#6b7280"
        label = "Wireless" if lt == "wireless" else "Wired"
        lines.append(
            f'<line x1="{sx(x1):.2f}" y1="{sy(y1):.2f}" x2="{sx(x2):.2f}" y2="{sy(y2):.2f}" '
            f'stroke="{color}" stroke-width="3" '
            + (f'stroke-dasharray="{dash}" ' if dash else "")
            + '/>'
        )
        mx = (sx(x1) + sx(x2)) / 2
        my = (sy(y1) + sy(y2)) / 2 + 18
        lines.append(f'<text x="{mx:.2f}" y="{my:.2f}" text-anchor="middle" font-size="12" font-family="Arial" fill="{color}">{label}</text>')

    # Node markers
    for name, (x, y) in nodes.items():
        lines.append(f'<circle cx="{sx(x):.2f}" cy="{sy(y):.2f}" r="9" fill="#111827"/>')
        lines.append(f'<text x="{sx(x):.2f}" y="{sy(y)+24:.2f}" text-anchor="middle" font-size="13" font-family="Arial" fill="#111827">{name}</text>')

    # OBSS interferer markers
    for name, x, y in obss_points:
        lines.append(f'<circle cx="{sx(x):.2f}" cy="{sy(y):.2f}" r="7" fill="#dc2626" stroke="#7f1d1d" stroke-width="1.5"/>')
        lines.append(f'<text x="{sx(x):.2f}" y="{sy(y)+22:.2f}" text-anchor="middle" font-size="12" font-family="Arial" fill="#991b1b">{name}</text>')

    # Legend
    lx, ly = width - 230, 84
    lines.append(f'<rect x="{lx}" y="{ly}" width="180" height="96" fill="#ffffff" stroke="#d1d5db"/>')
    lines.append(f'<text x="{lx+10}" y="{ly+20}" font-size="12" font-family="Arial" fill="#111827">{metric_label} (Mbps)</text>')
    for i in range(6):
        t = i / 5.0
        val = vmin + t * (vmax - vmin)
        c = color_map(val, vmin, vmax)
        y0 = ly + 30 + i * 10
        lines.append(f'<rect x="{lx+10}" y="{y0}" width="22" height="8" fill="{c}"/>')
        lines.append(f'<text x="{lx+40}" y="{y0+8}" font-size="10" font-family="Arial" fill="#111827">{val:.1f}</text>')

    lines.append('</svg>')
    out_svg.write_text("\n".join(lines), encoding="utf-8")


def draw_best_assoc_svg(
    out_svg: Path,
    cfg: ModeConfig,
    samples: List[Tuple[float, float, str, float]],
    x_min: float,
    x_max: float,
    y_min: float,
    y_max: float,
    obss_points: List[Tuple[str, float, float]],
) -> None:
    width, height = 1100, 760
    margin_l, margin_r, margin_t, margin_b = 90, 40, 70, 90
    plot_w = width - margin_l - margin_r
    plot_h = height - margin_t - margin_b

    def sx(x: float) -> float:
        return margin_l + (x - x_min) / (x_max - x_min) * plot_w

    def sy(y: float) -> float:
        return margin_t + (y_max - y) / (y_max - y_min) * plot_h

    nodes = node_positions(cfg)
    links = topology_links(cfg)
    throughputs = [t for _, _, _, t in samples]
    vmin = min(throughputs) if throughputs else 0.0
    vmax = max(throughputs) if throughputs else 1.0
    xs = sorted({x for x, _, _, _ in samples})
    ys = sorted({y for _, y, _, _ in samples})
    x_edges = grid_edges(x_min, x_max, len(xs))
    y_edges = grid_edges(y_min, y_max, len(ys))
    sample_map = {(x, y): (assoc, t) for x, y, assoc, t in samples}

    assoc_label = {"ont": "ONT", "ap1": "AP1", "ap2": "AP2"}
    lines: List[str] = []
    lines.append(f'<svg xmlns="http://www.w3.org/2000/svg" width="{width}" height="{height}" viewBox="0 0 {width} {height}">')
    lines.append('<rect x="0" y="0" width="100%" height="100%" fill="#ffffff"/>')
    lines.append(f'<text x="{width/2}" y="36" text-anchor="middle" font-size="24" font-family="Arial" fill="#1f2937">')
    lines.append(f'MESH Best Association: {cfg.name}</text>')
    lines.append(f'<rect x="{margin_l}" y="{margin_t}" width="{plot_w}" height="{plot_h}" fill="#f8fafc" stroke="#d1d5db"/>')

    for y_idx, y in enumerate(ys):
        y0 = y_edges[y_idx]
        y1 = y_edges[y_idx + 1]
        for x_idx, x in enumerate(xs):
            x0 = x_edges[x_idx]
            x1 = x_edges[x_idx + 1]
            assoc, thpt = sample_map[(x, y)]
            fill = color_map(thpt, vmin, vmax)
            text_color = label_color(thpt, vmin, vmax)
            cx = (x0 + x1) / 2.0
            cy = (y0 + y1) / 2.0
            lines.append(
                f'<rect x="{sx(x0):.2f}" y="{sy(y1):.2f}" width="{sx(x1) - sx(x0):.2f}" '
                f'height="{sy(y0) - sy(y1):.2f}" fill="{fill}" fill-opacity="0.78" stroke="#e5e7eb" stroke-width="1"/>'
            )
            lines.append(
                f'<text x="{sx(cx):.2f}" y="{sy(cy)-5:.2f}" text-anchor="middle" font-size="12" '
                f'font-family="Arial" font-weight="700" fill="{text_color}">{assoc_label.get(assoc, assoc.upper())}</text>'
            )
            lines.append(
                f'<text x="{sx(cx):.2f}" y="{sy(cy)+11:.2f}" text-anchor="middle" font-size="12" '
                f'font-family="Arial" fill="{text_color}">{thpt:.1f}</text>'
            )

    for xt in x_edges:
        xpix = sx(xt)
        lines.append(f'<line x1="{xpix:.2f}" y1="{margin_t}" x2="{xpix:.2f}" y2="{margin_t + plot_h}" stroke="#cbd5e1" stroke-width="1.2"/>')
        lines.append(f'<text x="{xpix:.2f}" y="{margin_t + plot_h + 24}" text-anchor="middle" font-size="13" font-family="Arial" fill="#374151">{xt:.1f}</text>')

    for yt in y_edges:
        ypix = sy(yt)
        lines.append(f'<line x1="{margin_l}" y1="{ypix:.2f}" x2="{margin_l + plot_w}" y2="{ypix:.2f}" stroke="#cbd5e1" stroke-width="1.2"/>')
        lines.append(f'<text x="{margin_l - 10}" y="{ypix + 4:.2f}" text-anchor="end" font-size="13" font-family="Arial" fill="#374151">{yt:.1f}</text>')

    lines.append(f'<text x="{margin_l + plot_w/2}" y="{height - 28}" text-anchor="middle" font-size="16" font-family="Arial" fill="#111827">X position (m)</text>')
    lines.append(f'<text x="28" y="{margin_t + plot_h/2}" transform="rotate(-90 28 {margin_t + plot_h/2})" text-anchor="middle" font-size="16" font-family="Arial" fill="#111827">Y position (m)</text>')

    for a, b, lt in links:
        x1, y1 = nodes[a]
        x2, y2 = nodes[b]
        dash = "7,7" if lt == "wireless" else ""
        color = "#2563eb" if lt == "wireless" else "#6b7280"
        label = "Wireless" if lt == "wireless" else "Wired"
        lines.append(
            f'<line x1="{sx(x1):.2f}" y1="{sy(y1):.2f}" x2="{sx(x2):.2f}" y2="{sy(y2):.2f}" '
            f'stroke="{color}" stroke-width="3" '
            + (f'stroke-dasharray="{dash}" ' if dash else "")
            + '/>'
        )
        mx = (sx(x1) + sx(x2)) / 2
        my = (sy(y1) + sy(y2)) / 2 + 18
        lines.append(f'<text x="{mx:.2f}" y="{my:.2f}" text-anchor="middle" font-size="12" font-family="Arial" fill="{color}">{label}</text>')

    for name, (x, y) in nodes.items():
        lines.append(f'<circle cx="{sx(x):.2f}" cy="{sy(y):.2f}" r="9" fill="#111827"/>')
        lines.append(f'<text x="{sx(x):.2f}" y="{sy(y)+24:.2f}" text-anchor="middle" font-size="13" font-family="Arial" fill="#111827">{name}</text>')

    for name, x, y in obss_points:
        lines.append(f'<circle cx="{sx(x):.2f}" cy="{sy(y):.2f}" r="7" fill="#dc2626" stroke="#7f1d1d" stroke-width="1.5"/>')
        lines.append(f'<text x="{sx(x):.2f}" y="{sy(y)+22:.2f}" text-anchor="middle" font-size="12" font-family="Arial" fill="#991b1b">{name}</text>')

    lx, ly = width - 230, 84
    lines.append(f'<rect x="{lx}" y="{ly}" width="180" height="96" fill="#ffffff" stroke="#d1d5db"/>')
    lines.append(f'<text x="{lx+10}" y="{ly+20}" font-size="12" font-family="Arial" fill="#111827">Best Throughput (Mbps)</text>')
    for i in range(6):
        t = i / 5.0
        val = vmin + t * (vmax - vmin)
        c = color_map(val, vmin, vmax)
        y0 = ly + 30 + i * 10
        lines.append(f'<rect x="{lx+10}" y="{y0}" width="22" height="8" fill="{c}"/>')
        lines.append(f'<text x="{lx+40}" y="{y0+8}" font-size="10" font-family="Arial" fill="#111827">{val:.1f}</text>')

    lines.append('</svg>')
    out_svg.write_text("\n".join(lines), encoding="utf-8")


def generate_best_assoc_outputs(
    mode: int,
    cfg: ModeConfig,
    input_dir: Path,
    out_csv: Path,
    out_svg: Path,
    x_min: float,
    x_max: float,
    y_min: float,
    y_max: float,
    obss_points: List[Tuple[str, float, float]],
) -> None:
    assoc_rows: Dict[str, Dict[Tuple[float, float], float]] = {}
    scenario = cfg.name
    for assoc in ["ont", "ap1", "ap2"]:
        csv_path = input_dir / f"mode{mode}_{assoc}.csv"
        if not csv_path.exists():
            raise FileNotFoundError(f"Missing association CSV: {csv_path}")
        with csv_path.open("r", newline="") as f:
            rows = list(csv.DictReader(f))
        assoc_rows[assoc] = {
            (float(row["x"]), float(row["y"])): float(row["endToEndMbpsAvg"])
            for row in rows
        }
        if rows:
            scenario = rows[0].get("scenario", scenario)

    common_points = set.intersection(*(set(values.keys()) for values in assoc_rows.values()))
    if not common_points:
        raise RuntimeError(f"No common grid points found for mode {mode}")

    samples: List[Tuple[float, float, str, float]] = []
    out_csv.parent.mkdir(parents=True, exist_ok=True)
    with out_csv.open("w", newline="") as f:
        writer = csv.writer(f)
        writer.writerow([
            "mode",
            "scenario",
            "x",
            "y",
            "bestAssoc",
            "bestThroughputMbps",
            "ontThroughputMbps",
            "ap1ThroughputMbps",
            "ap2ThroughputMbps",
        ])
        for x, y in sorted(common_points, key=lambda item: (item[1], item[0])):
            values = {assoc: assoc_rows[assoc][(x, y)] for assoc in ["ont", "ap1", "ap2"]}
            best_assoc, best_value = max(values.items(), key=lambda item: item[1])
            samples.append((x, y, best_assoc, best_value))
            writer.writerow([
                mode,
                scenario,
                f"{x:.4f}",
                f"{y:.4f}",
                best_assoc,
                f"{best_value:.3f}",
                f"{values['ont']:.3f}",
                f"{values['ap1']:.3f}",
                f"{values['ap2']:.3f}",
            ])

    draw_best_assoc_svg(out_svg, cfg, samples, x_min, x_max, y_min, y_max, obss_points)


def main() -> int:
    parser = argparse.ArgumentParser(description="Static grid throughput scan for mesh_test_obss")
    parser.add_argument("--mode", type=int, required=True, help="Scenario mode 1..8")
    parser.add_argument("--sta-assoc", type=str, default="ap1", choices=["ont", "ap1", "ap2"])
    parser.add_argument("--best-assoc", action="store_true", help="Read modeX_ont/ap1/ap2 CSVs and generate a best-association summary CSV/SVG")
    parser.add_argument("--best-input-dir", type=Path, default=None, help="Directory containing modeX_ont.csv, modeX_ap1.csv, and modeX_ap2.csv")
    parser.add_argument("--nx", type=int, default=6, help="Grid points in X")
    parser.add_argument("--ny", type=int, default=6, help="Grid points in Y")
    parser.add_argument("--run", type=int, default=1)
    parser.add_argument("--seed-runs", type=str, default="1,2,3", help="Comma-separated RNG run values to average")
    parser.add_argument("--prewarm", type=float, default=10.0)
    parser.add_argument("--test", type=float, default=10.0)
    parser.add_argument("--no-build", action="store_true", help="Pass --no-build to ./ns3 run to avoid concurrent configure/build races")
    parser.add_argument("--rts-threshold", type=int, default=None, help="Optional RTS/CTS threshold passed to MESH_TEST")
    parser.add_argument("--tcp-streams", type=int, default=1, help="TCP stream count passed to mesh_test_obss")
    parser.add_argument("--app-rate", type=str, default="20Gbps", help="Per-flow application rate passed to mesh_test_obss")
    parser.add_argument("--wired-rate", type=str, default=None, help="Optional wired backhaul rate passed to mesh_test_obss, e.g. 20Gbps")
    parser.add_argument("--enable-ampdu", type=int, choices=[0, 1], default=None, help="Optional BE A-MPDU switch passed to MESH_TEST_2")
    parser.add_argument("--enable-amsdu", type=int, choices=[0, 1], default=None, help="Optional BE A-MSDU switch passed to MESH_TEST_2")
    parser.add_argument("--enable-obss", type=int, choices=[0, 1], default=1, help="Enable OBSS interference")
    parser.add_argument("--enable-obss1", type=int, choices=[0, 1], default=1, help="Enable OBSS1 interferer")
    parser.add_argument("--enable-obss2", type=int, choices=[0, 1], default=0, help="Enable OBSS2 interferer")
    parser.add_argument("--obss-target-duty", type=float, default=0.15, help="Target aggregate OBSS airtime duty")
    parser.add_argument("--obss-link-rate-mbps", type=float, default=1600.0, help="Estimated OBSS link rate used to derive offered rate")
    parser.add_argument("--obss-rate", type=str, default="150Mbps", help="Explicit aggregate OBSS UDP offered rate, e.g. 150Mbps. Empty uses obss-target-duty conversion.")
    parser.add_argument("--obss-ap-x", type=float, default=4.0)
    parser.add_argument("--obss-ap-y", type=float, default=4.0)
    parser.add_argument("--obss-sta-x", type=float, default=5.0)
    parser.add_argument("--obss-sta-y", type=float, default=4.0)
    parser.add_argument("--obss-ap2-x", type=float, default=8.0)
    parser.add_argument("--obss-ap2-y", type=float, default=6.0)
    parser.add_argument("--obss-sta2-x", type=float, default=9.0)
    parser.add_argument("--obss-sta2-y", type=float, default=6.0)
    parser.add_argument("--metric", type=str, default="endToEnd", choices=["endToEnd", "hop1", "hop2", "hop3"], help="Which throughput metric to visualize in the SVG")
    parser.add_argument("--x-min", type=float, default=None)
    parser.add_argument("--x-max", type=float, default=None)
    parser.add_argument("--y-min", type=float, default=None)
    parser.add_argument("--y-max", type=float, default=None)
    parser.add_argument("--csv", type=Path, default=None, help="Output CSV for sampled points")
    parser.add_argument("--svg", type=Path, default=None, help="Output SVG figure path")
    args = parser.parse_args()

    cfg = resolve_mode(args.mode)

    dx_min, dx_max, dy_min, dy_max = default_bounds(cfg)
    x_min = dx_min if args.x_min is None else args.x_min
    x_max = dx_max if args.x_max is None else args.x_max
    y_min = dy_min if args.y_min is None else args.y_min
    y_max = dy_max if args.y_max is None else args.y_max
    root = next(p for p in Path(__file__).resolve().parents if (p / "ns3").is_file())
    out_dir = root / "scratch" / "mesh_obss_results"
    out_dir.mkdir(exist_ok=True)
    if args.best_assoc:
        out_csv = args.csv if args.csv is not None else out_dir / f"mode{args.mode}_best_assoc.csv"
        out_svg = args.svg if args.svg is not None else out_dir / f"mode{args.mode}_best_assoc.svg"
    else:
        out_csv = args.csv if args.csv is not None else out_dir / f"grid_scan_mode{args.mode}_{args.sta_assoc}.csv"
        out_svg = args.svg if args.svg is not None else out_dir / f"grid_scan_mode{args.mode}_{args.sta_assoc}.svg"

    obss_points = []
    if args.enable_obss and args.enable_obss1:
        obss_points.append(("OBSS1", args.obss_ap_x, args.obss_ap_y))
    if args.enable_obss and args.enable_obss2:
        obss_points.append(("OBSS2", args.obss_ap2_x, args.obss_ap2_y))

    if args.best_assoc:
        input_dir = args.best_input_dir if args.best_input_dir is not None else out_dir
        generate_best_assoc_outputs(args.mode, cfg, input_dir, out_csv, out_svg, x_min, x_max, y_min, y_max, obss_points)
        print(f"\n[OK] best-association CSV: {out_csv}")
        print(f"[OK] best-association SVG: {out_svg}")
        return 0

    seed_runs = [int(item.strip()) for item in args.seed_runs.split(",") if item.strip()]
    if not seed_runs:
        raise ValueError("seed-runs must contain at least one run index")

    tmp_dir = out_csv.parent / ".scan_tmp"
    tmp_dir.mkdir(exist_ok=True)

    xs = grid_centers(x_min, x_max, args.nx)
    ys = grid_centers(y_min, y_max, args.ny)

    samples: List[Tuple[float, float, float]] = []
    header = [
        "mode",
        "scenario",
        "staAssoc",
        "x",
        "y",
        "endToEndMbpsAvg",
        "hop1Name",
        "hop1MbpsAvg",
        "hop2Name",
        "hop2MbpsAvg",
        "hop3Name",
        "hop3MbpsAvg",
        "obssEnabled",
        "obssTargetDuty",
        "obssMeasuredDutyAvg",
        "obssRate",
    ]
    for seed_run in seed_runs:
        header.extend(
            [
                f"endToEndRun{seed_run}Mbps",
                f"hop1Run{seed_run}Name",
                f"hop1Run{seed_run}Mbps",
                f"hop2Run{seed_run}Name",
                f"hop2Run{seed_run}Mbps",
                f"hop3Run{seed_run}Name",
                f"hop3Run{seed_run}Mbps",
            ]
        )

    with out_csv.open("w", newline="") as f:
        writer = csv.writer(f)
        writer.writerow(header)
        f.flush()

    for y in ys:
        for x in xs:
            print(f"[SCAN] x={x:.2f}, y={y:.2f}")
            seed_metrics: List[Dict[str, str]] = []
            for seed_run in seed_runs:
                # 每个任务/seed 只复用一个临时 CSV，避免在 .scan_tmp 下留下大量分点文件。
                ns3_csv = tmp_dir / f"mesh_scan_mode{args.mode}_{args.sta_assoc}_run{seed_run}_pid{os.getpid()}.csv"
                if ns3_csv.exists():
                    ns3_csv.unlink()
                rts_arg = f" --rtsCtsThreshold={args.rts_threshold}" if args.rts_threshold is not None else ""
                tcp_arg = f" --tcpStreams={args.tcp_streams}" if args.tcp_streams is not None else ""
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
                    f'--mode={args.mode} --staAssoc={args.sta_assoc} '
                    f'--run={seed_run} --prewarm={args.prewarm} --test={args.test} '
                    f'--useStaPos=1 --staX={x:.4f} --staY={y:.4f}'
                    f'{rts_arg}{tcp_arg}{rate_arg}{wired_arg}{ampdu_arg}{amsdu_arg}{obss_arg} --out={ns3_csv}'
                )
                cmd = build_ns3_command(root, args.no_build, sim_args)
                subprocess.run(cmd, cwd=root, shell=True, check=True)
                seed_metrics.append(read_last_metrics(ns3_csv))
                if ns3_csv.exists():
                    ns3_csv.unlink()

            end_to_end_values = [float(item["endToEndMbps"]) for item in seed_metrics]
            hop1_values = [float(item["hop1Mbps"]) for item in seed_metrics if item["hop1Mbps"]]
            hop2_values = [float(item["hop2Mbps"]) for item in seed_metrics if item["hop2Mbps"]]
            hop3_values = [float(item["hop3Mbps"]) for item in seed_metrics if item["hop3Mbps"]]
            hop1_name = seed_metrics[0].get("hop1Name", "")
            hop2_name = seed_metrics[0].get("hop2Name", "")
            hop3_name = seed_metrics[0].get("hop3Name", "")
            obss_values = [float(item.get("obssMeasuredDuty", "0") or 0.0) for item in seed_metrics]
            obss_measured_avg = sum(obss_values) / len(obss_values)
            obss_enabled = seed_metrics[0].get("obssEnabled", str(args.enable_obss))
            obss_rate = seed_metrics[0].get("obssRate", "")

            end_to_end_avg = sum(end_to_end_values) / len(end_to_end_values)
            hop1_avg = sum(hop1_values) / len(hop1_values) if hop1_values else 0.0
            hop2_avg = sum(hop2_values) / len(hop2_values) if hop2_values else 0.0
            hop3_avg = sum(hop3_values) / len(hop3_values) if hop3_values else 0.0

            metric_value = {
                "endToEnd": end_to_end_avg,
                "hop1": hop1_avg,
                "hop2": hop2_avg,
                "hop3": hop3_avg,
            }[args.metric]
            samples.append((x, y, metric_value))

            row = [
                str(args.mode),
                cfg.name,
                args.sta_assoc,
                f"{x:.4f}",
                f"{y:.4f}",
                f"{end_to_end_avg:.3f}",
                hop1_name,
                f"{hop1_avg:.3f}" if hop1_name else "",
                hop2_name,
                f"{hop2_avg:.3f}" if hop2_name else "",
                hop3_name,
                f"{hop3_avg:.3f}" if hop3_name else "",
                obss_enabled,
                f"{args.obss_target_duty:.3f}",
                f"{obss_measured_avg:.6f}",
                obss_rate,
            ]
            for item in seed_metrics:
                row.extend(
                    [
                        f'{float(item["endToEndMbps"]):.3f}',
                        item.get("hop1Name", ""),
                        f'{float(item["hop1Mbps"]):.3f}' if item.get("hop1Mbps") else "",
                        item.get("hop2Name", ""),
                        f'{float(item["hop2Mbps"]):.3f}' if item.get("hop2Mbps") else "",
                        item.get("hop3Name", ""),
                        f'{float(item["hop3Mbps"]):.3f}' if item.get("hop3Mbps") else "",
                    ]
                )
            with out_csv.open("a", newline="") as f:
                writer = csv.writer(f)
                writer.writerow(row)
                f.flush()
            seeds_text = ", ".join(f"{value:.1f}" for value in end_to_end_values)
            print(f"        endToEndAvg={end_to_end_avg:.3f} Mbps from [{seeds_text}]")

    metric_label = {
        "endToEnd": "End-to-End",
        "hop1": "Hop1",
        "hop2": "Hop2",
        "hop3": "Hop3",
    }[args.metric]
    draw_svg(out_svg, cfg, samples, x_min, x_max, y_min, y_max, metric_label, obss_points)

    # 最后一轮读完后，顺手清理空的临时目录；目录非空也不报错。
    try:
        tmp_dir.rmdir()
    except OSError:
        pass

    print(f"\n[OK] sample CSV: {out_csv}")
    print(f"[OK] figure SVG: {out_svg}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
