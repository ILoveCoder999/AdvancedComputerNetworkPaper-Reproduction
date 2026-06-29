#!/usr/bin/env bash
# ============================================================
# run_all.sh
# Full experiment suite for SWDC SOCC'11 reproduction
# Usage: bash run_all.sh [ns3-root]
# ============================================================

set -e

NS3_ROOT="${1:-$HOME/ns3}"
RESULTS_DIR="$(pwd)/results"
mkdir -p "$RESULTS_DIR"

echo "======================================================"
echo " SWDC SOCC'11 Reproduction"
echo " NS3 root : $NS3_ROOT"
echo " Results  : $RESULTS_DIR"
echo "======================================================"

# ── Step 0: Copy simulation file into NS3 scratch ──────────
cp "$(dirname "$0")/swdc-simulation.cc" "$NS3_ROOT/scratch/"
cd "$NS3_ROOT"
echo "[1/4] Building NS3 simulation..."
./ns3 build scratch/swdc-simulation 2>&1 | tail -5

OUT="$RESULTS_DIR/swdc_ns3_results.csv"
rm -f "$OUT"

# ── Step 1: Fig. 5 — Packet delivery latency ───────────────
echo ""
echo "[2/4] Running latency experiments (Fig. 5)..."

TOPOS="SW-Ring SW-2DTorus SW-3DHexTorus CamCube CDC"
PKTSIZES="64 1024"
TRAFFICS="uniform local mapreduce"

for TOPO in $TOPOS; do
  for PKT in $PKTSIZES; do
    for TRAFFIC in $TRAFFICS; do
      OVERSUB_ARGS=""
      if [ "$TOPO" = "CDC" ]; then
        # Run three CDC variants
        for TOR_OS in 2 1 1; do
          for AGG_OS in 5 7 5; do
            CDC_TAG="CDC(${TOR_OS},${AGG_OS},1)"
            echo "  $CDC_TAG pkt=${PKT}B traffic=${TRAFFIC}"
            ./ns3 run "swdc-simulation \
              --topo=CDC \
              --traffic=${TRAFFIC} \
              --pktSize=${PKT} \
              --nNodes=512 \
              --simTime=0.5 \
              --nPkts=300 \
              --torOversub=${TOR_OS} \
              --aggOversub=${AGG_OS} \
              --outFile=${OUT}" \
              2>&1 | grep -E "latency|bandwidth|delivery|Results"
            break 2
          done
        done
        # Run all three CDC configs
        for CONFIG in "2 5" "1 7" "1 5"; do
          TOR=$(echo $CONFIG | awk '{print $1}')
          AGG=$(echo $CONFIG | awk '{print $2}')
          echo "  CDC(${TOR},${AGG},1) pkt=${PKT}B traffic=${TRAFFIC}"
          ./ns3 run "swdc-simulation \
            --topo=CDC \
            --traffic=${TRAFFIC} \
            --pktSize=${PKT} \
            --nNodes=512 \
            --simTime=0.5 \
            --nPkts=300 \
            --torOversub=${TOR} \
            --aggOversub=${AGG} \
            --outFile=${OUT}" \
            2>&1 | grep -E "latency|bandwidth|delivery|Results"
        done
      else
        echo "  ${TOPO} pkt=${PKT}B traffic=${TRAFFIC}"
        ./ns3 run "swdc-simulation \
          --topo=${TOPO} \
          --traffic=${TRAFFIC} \
          --pktSize=${PKT} \
          --nNodes=512 \
          --simTime=0.5 \
          --nPkts=300 \
          --outFile=${OUT}" \
          2>&1 | grep -E "latency|bandwidth|delivery|Results"
      fi
    done
  done
done

# ── Step 2: Fig. 6 — Maximum aggregate bandwidth ───────────
echo ""
echo "[3/4] Running bandwidth experiments (Fig. 6)..."

BW_OUT="$RESULTS_DIR/swdc_bw_results.csv"
rm -f "$BW_OUT"

for TOPO in $TOPOS; do
  for PKT in $PKTSIZES; do
    for TRAFFIC in $TRAFFICS; do
      if [ "$TOPO" = "CDC" ]; then
        for CONFIG in "2 5" "1 7" "1 5"; do
          TOR=$(echo $CONFIG | awk '{print $1}')
          AGG=$(echo $CONFIG | awk '{print $2}')
          echo "  CDC(${TOR},${AGG},1) pkt=${PKT}B traffic=${TRAFFIC} [BW]"
          ./ns3 run "swdc-simulation \
            --topo=CDC \
            --traffic=${TRAFFIC} \
            --pktSize=${PKT} \
            --nNodes=512 \
            --simTime=1.0 \
            --nPkts=500 \
            --torOversub=${TOR} \
            --aggOversub=${AGG} \
            --outFile=${BW_OUT}" \
            2>&1 | grep -E "latency|bandwidth|delivery|Results"
        done
      else
        echo "  ${TOPO} pkt=${PKT}B traffic=${TRAFFIC} [BW]"
        ./ns3 run "swdc-simulation \
          --topo=${TOPO} \
          --traffic=${TRAFFIC} \
          --pktSize=${PKT} \
          --nNodes=512 \
          --simTime=1.0 \
          --nPkts=500 \
          --outFile=${BW_OUT}" \
          2>&1 | grep -E "latency|bandwidth|delivery|Results"
      fi
    done
  done
done

# ── Step 3: Fig. 11-12 — Fault tolerance (NS3) ─────────────
echo ""
echo "[4/4] Running fault tolerance experiments (Fig. 11-12)..."

FT_OUT="$RESULTS_DIR/swdc_fault_results.csv"
rm -f "$FT_OUT"

for TOPO in SW-Ring SW-2DTorus SW-3DHexTorus CamCube; do
  for FRAC in 0.0 0.1 0.2 0.3 0.4 0.5; do
    echo "  ${TOPO} fail=${FRAC}"
    ./ns3 run "swdc-simulation \
      --topo=${TOPO} \
      --traffic=uniform \
      --pktSize=1024 \
      --nNodes=512 \
      --simTime=0.5 \
      --nPkts=200 \
      --faultTest=true \
      --faultFrac=${FRAC} \
      --outFile=${FT_OUT}" \
      2>&1 | grep -E "latency|bandwidth|delivery|Failed|Results"
  done
done

echo ""
echo "======================================================"
echo " NS3 simulations complete."
echo " Results in: $RESULTS_DIR"
echo "======================================================"

# ── Step 4: Graph analysis (Python) ────────────────────────
echo ""
echo "Running Python graph analysis (Fig. 3, 4, 11-12)..."
cd "$(dirname "$0")"
python3 swdc_topology.py "$RESULTS_DIR"

echo ""
echo "Plotting results..."
python3 plot_results.py "$RESULTS_DIR"

echo ""
echo "All done! Open $RESULTS_DIR/figures/ for plots."
