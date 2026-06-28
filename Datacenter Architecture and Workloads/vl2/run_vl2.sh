#!/usr/bin/env bash
# =============================================================================
# run_vl2.sh  –  Build and run all VL2 experiments
#
# Usage:
#   cd <ns-3-source-root>          # e.g. ~/ns-3-dev  or  ~/ns-allinone-3.XX/ns-3.XX
#   bash /path/to/run_vl2.sh
#
# The script:
#   1. Copies vl2_simulation.cc into scratch/
#   2. Builds with ./ns3 build
#   3. Runs experiments 1-4 and saves .dat + .xml output files
#   4. Prints a reminder to plot with plot_vl2.py
# =============================================================================

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SIM_SRC="$SCRIPT_DIR/vl2_simulation.cc"

# ── sanity checks ──────────────────────────────────────────────────────────────
if [[ ! -f "$SIM_SRC" ]]; then
    echo "ERROR: vl2_simulation.cc not found at $SIM_SRC"
    exit 1
fi

if [[ ! -f "./ns3" ]] && [[ ! -f "./waf" ]]; then
    echo "ERROR: Run this script from inside your ns-3 source directory."
    echo "       (Expected to find ./ns3 or ./waf there.)"
    exit 1
fi

# Detect build tool (ns-3.36+ uses CMake/ns3; older uses waf)
if [[ -f "./ns3" ]]; then
    BUILD_CMD="./ns3 build"
    RUN_PREFIX="./ns3 run"
else
    BUILD_CMD="./waf build"
    RUN_PREFIX="./waf --run"
fi

# ── copy source ────────────────────────────────────────────────────────────────
echo ">>> Copying vl2_simulation.cc to scratch/"
cp "$SIM_SRC" scratch/vl2_simulation.cc

# ── build ──────────────────────────────────────────────────────────────────────
echo ">>> Building..."
$BUILD_CMD 2>&1 | tail -5

# ── output directory ───────────────────────────────────────────────────────────
OUTDIR="$SCRIPT_DIR/results"
mkdir -p "$OUTDIR"

run_exp() {
    local N=$1
    local DESC=$2
    local EXTRA_ARGS="${3:-}"
    echo ""
    echo "================================================================"
    echo " Experiment $N: $DESC"
    echo "================================================================"
    # ns3 run puts output files in the ns-3 root; move them to results/
    $RUN_PREFIX "vl2-simulation --experiment=$N $EXTRA_ARGS" 2>&1
    for f in vl2_exp${N}_goodput.dat vl2_exp${N}_flows.xml; do
        [[ -f "$f" ]] && mv "$f" "$OUTDIR/" && echo "  Saved $OUTDIR/$f"
    done
}

# ── run experiments ────────────────────────────────────────────────────────────

# Exp 1: All-to-all shuffle (20 servers, 50 MB per flow)
run_exp 1 "All-to-all data shuffle"

# Exp 2: VLB fairness (same topology, DC-like flow mix)
run_exp 2 "VLB fairness"

# Exp 3: Performance isolation (two services)
run_exp 3 "Performance isolation"

# Exp 4: Link failure convergence
run_exp 4 "Link failure convergence"

echo ""
echo "================================================================"
echo " All experiments complete.  Results in: $OUTDIR"
echo " Plot with:  python3 $SCRIPT_DIR/plot_vl2.py --results $OUTDIR"
echo "================================================================"
