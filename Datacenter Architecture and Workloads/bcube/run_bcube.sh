#!/usr/bin/env bash
# =====================================================================
# run_bcube.sh  —  BCube SIGCOMM'09 ns-3 experiment runner
#
# Usage:
#   chmod +x run_bcube.sh
#   NS3_HOME=/path/to/ns3  ./run_bcube.sh
#
# If NS3_HOME is not set, the script searches common install locations.
# =====================================================================

set -e

# ── Locate ns-3 ───────────────────────────────────────────────────────
if [ -z "$NS3_HOME" ]; then
    for candidate in \
        "$HOME/ns-3" "$HOME/ns3" \
        "$HOME/workspace/ns-3" \
        "/opt/ns-3" "/usr/local/ns-3"
    do
        if [ -f "$candidate/ns3" ] || [ -f "$candidate/waf" ]; then
            NS3_HOME="$candidate"
            break
        fi
    done
fi

if [ -z "$NS3_HOME" ] || [ ! -d "$NS3_HOME" ]; then
    echo "ERROR: cannot find ns-3. Set NS3_HOME=/path/to/ns3 and retry."
    exit 1
fi
echo "ns-3 home: $NS3_HOME"

# ── Copy simulation file ───────────────────────────────────────────────
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
cp "$SCRIPT_DIR/bcube-sim.cc" "$NS3_HOME/scratch/bcube-sim.cc"
echo "Copied bcube-sim.cc → $NS3_HOME/scratch/"

# ── Detect build command (cmake-based ns3 script or legacy waf) ────────
cd "$NS3_HOME"
if [ -f "./ns3" ]; then
    RUN="./ns3 run"
elif [ -f "./waf" ]; then
    RUN="./waf --run"
else
    echo "ERROR: neither ./ns3 nor ./waf found in $NS3_HOME"
    exit 1
fi

# ── Build ──────────────────────────────────────────────────────────────
echo ""
echo "Building..."
if [ -f "./ns3" ]; then
    ./ns3 build 2>&1 | tail -5
else
    ./waf build 2>&1 | tail -5
fi

# ── Run experiments ────────────────────────────────────────────────────
LOGFILE="$SCRIPT_DIR/bcube_results.txt"
echo "" > "$LOGFILE"

for exp in one-to-one one-to-all all-to-all degrade; do
    echo ""
    echo "============================================================"
    echo " Running: $exp"
    echo "============================================================"
    OUTPUT=$($RUN "scratch/bcube-sim --exp=$exp" 2>&1)
    echo "$OUTPUT"
    echo "$OUTPUT" >> "$LOGFILE"
done

echo ""
echo "All results saved to: $LOGFILE"
echo ""
echo "Expected results (from paper §7.5 and §6):"
echo "  One-to-one  BCube ~1930 Mb/s  Tree  ~990 Mb/s   (2× speedup)"
echo "  One-to-all  BCube ~1600 Mb/s  Tree  ~880 Mb/s   (2× speedup)"
echo "  All-to-all  BCube  ~750 Mb/s  Tree  ~260 Mb/s   (3× speedup)"
echo "  Degrade     BCube graceful, Fat-tree sharp drop at switch failures"
