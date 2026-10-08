#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$(readlink -f "$0")")"
while [[ ! -f ns3 && "$PWD" != / ]]; do cd ..; done
[[ -f ns3 ]] || { echo "ns-3 root not found" >&2; exit 1; }

STAMP="${STAMP:-$(date +%Y%m%d_%H%M%S)}"
OUT="${OUT:-scratch/mesh_obss_metrics_full_${STAMP}}"
MAX_JOBS="${MAX_JOBS:-4}"

if [[ -e "$OUT" ]]; then
  echo "[ERROR] Output path already exists: $OUT" >&2
  echo "        Set OUT to a new directory or remove the old one explicitly." >&2
  exit 1
fi

mkdir -p "$OUT/logs"

echo "[INFO] Output: $OUT"
echo "[INFO] MAX_JOBS: $MAX_JOBS"

./ns3 build scratch/mesh_test_obss_metrics

run_one() {
  local mode="$1"
  local assoc="$2"
  local name="mode${mode}_${assoc}"
  echo "[START] $name"
  python3 scratch/mesh_test_obss_metrics.py \
    --mode "$mode" \
    --sta-assoc "$assoc" \
    --nx 7 \
    --ny 7 \
    --seed-runs 1,2,3 \
    --prewarm 1 \
    --test 4 \
    --app-rate 20Gbps \
    --enable-obss 1 \
    --enable-obss1 1 \
    --enable-obss2 0 \
    --obss-rate 150Mbps \
    --no-build \
    --csv "$OUT/${name}.csv" \
    --svg "$OUT/${name}.svg" \
    > "$OUT/logs/${name}.log" 2>&1
  echo "[DONE] $name"
}

for mode in 1 2 3 4 5 6 7 8; do
  for assoc in ont ap1 ap2; do
    run_one "$mode" "$assoc" &
    while (( $(jobs -rp | wc -l) >= MAX_JOBS )); do
      sleep 5
    done
  done
done
wait

for mode in 1 2 3 4 5 6 7 8; do
  echo "[BEST] mode${mode}"
  python3 scratch/mesh_test_obss_metrics.py \
    --mode "$mode" \
    --best-assoc \
    --best-input-dir "$OUT" \
    --enable-obss 1 \
    --enable-obss1 1 \
    --enable-obss2 0 \
    --obss-rate 150Mbps \
    --csv "$OUT/mode${mode}_best_assoc.csv" \
    --svg "$OUT/mode${mode}_best_assoc.svg" \
    > "$OUT/logs/mode${mode}_best_assoc.log" 2>&1
done

echo "[ALL DONE] $OUT"
