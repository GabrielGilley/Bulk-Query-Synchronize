#!/bin/bash

# if [[ $# -ne 5 ]]; then
#   echo "Usage: $0 EXPERIMENT_NAME FILTER_LIST_IN_TXT_FILE DATA_DIR RESULTS_DIR NODE_COUNT"
#   echo "Ex: $0 shuffle_scalability filters/shuffle.txt /cscratch/btc-export results 8"
#   exit 1
# fi

TEST_NAME="kmer_inv_idx"
# FILTERS="filters/dagify.txt"
FILTERS="filters/overlap_canonical.txt"
# DATA_DIR="/cscratch/eth-pre-dag-full/"
# DATA_DIR="/cscratch/fastq/pando/pando"
# DATA_DIR="/cscratch/pando-few"
DATA_DIR="/cscratch/pando-chr13-R1"
RESULTS_DIR="/cscratch/results/"
NODE_COUNT=8
# Filter runs per processing pass before the stage closes (bounds memory).
# Empty = pando_pardb default (unlimited; its memory limits bound memory instead).
MAX_FILTERS_PER_STAGE=5000
# Write batch flush size in bytes per destination agent (empty = pando_pardb default of 262144, 0 = off)
WRITE_BATCH_BYTES=
# How each processing phase ends: once (one sweep per phase) or until-done (sweep
# until nothing runs; pando_pardb's own default). This experiment uses once: in the
# k-mer overlap pipeline (filters/overlap_canonical.txt) no filter creates or tags
# entries that another filter runs on in the same phase. Use until-done (or leave
# it empty) for pipelines whose filters chain within a phase, e.g. the BTC filters.
SWEEP=once
# Keys the filter scan fetches from the map per request (empty = pando_pardb default of 4096)
SCAN_BLOCK_KEYS=
# Store each distinct tag once per agent instead of in every entry: on or off
# (empty = pando_pardb default, on)
INTERN_TAGS=
# Memory limits: a processing pass ends early (and asks every agent to) before
# memory runs out. Empty = pando_pardb defaults.
#   MEM_LIMIT_PCT: keep memory below this % of the node's memory, or of this job's
#                  --mem if lower (default 90; 0 turns the limit off)
#   MEM_PASS_BUDGET_MB: also emit at most this many MiB of output per pass (default none)
#   MEM_BUDGET_FRACTION: share of the free memory the agents on a node may emit per
#                        pass, together (default 0.25)
#   AGENTS_PER_NODE: agents sharing a node's memory (default: Slurm's tasks per node)
MEM_LIMIT_PCT=
MEM_PASS_BUDGET_MB=
MEM_BUDGET_FRACTION=
AGENTS_PER_NODE=

mkdir -p "$RESULTS_DIR"
touch "$RESULTS_DIR/$TEST_NAME.csv"

AVAILABLE_NODE_COUNT=$(sinfo -N -p compute | grep idle | awk '{print $1}' | wc -l) 
AVAILABLE_NODES=$(sinfo -N -p compute | grep idle | awk '{print $1}')              

# Check if available nodes are less than requested nodes
if [ "$AVAILABLE_NODE_COUNT" -lt "$NODE_COUNT" ]; then
    echo "Error: Requested $NODE_COUNT nodes when only $AVAILABLE_NODE_COUNT nodes are available."
    exit 1
fi


# Calculate half the number of available nodes
# HALF_NODE_COUNT=$(($AVAILABLE_NODE_COUNT / 2))
# Get the first half of the available nodes
# FIRST_HALF_NODES=$(echo "$AVAILABLE_NODES" | head -n "$HALF_NODE_COUNT")

# Get the second half of the available nodes
# SECOND_HALF_NODES=$(echo "$AVAILABLE_NODES" | tail -n +"$((HALF_NODE_COUNT + 1))")

# srun -N "$HALF_NODE_COUNT" bash -c 'rm -rf /scratch/bigspace* /tmp/elga*' --nodelist="$FIRST_HALF_NODES" &
# srun -N "$((NODE_COUNT - HALF_NODE_COUNT))" bash -c 'rm -rf /scratch/bigspace* /tmp/elga*' --nodelist="$SECOND_HALF_NODES" &


# Submit batch job
SLURM_OUTPUT=$(sbatch -N "$NODE_COUNT" --mem 390000 pardb.sbatch "$MAX_FILTERS_PER_STAGE" "$WRITE_BATCH_BYTES" "$SWEEP" "$SCAN_BLOCK_KEYS" "$INTERN_TAGS" \
    "$MEM_LIMIT_PCT" "$MEM_PASS_BUDGET_MB" "$MEM_BUDGET_FRACTION" "$AGENTS_PER_NODE")
JOB_ID=$(echo "$SLURM_OUTPUT" | awk '{print $4}')
echo "Batch job submitted with $NODE_COUNT nodes. Job ID: $JOB_ID"

sleep 1

# Get mesh
FIRST_NODE="en$(scontrol show job $JOB_ID | grep "NodeList=" | awk 'NR==2' | awk -F'=' '{print $2}' | tr -d ' ' | awk -F',' '{print $1}' | awk -F'[' '{print $2}' | awk -F'-' '{print $1}' | sed 's/[[:space:]]//g')"
HOST_OUTPUT=$(host "$FIRST_NODE.100g")
IP_ADDRESS=$(echo "$HOST_OUTPUT" | awk '{print $NF}')
MESH="${IP_ADDRESS},0"

echo "MESH: $MESH"

# Memory monitor output (written by memmon.sh from pardb.sbatch). par-script.py
# appends phase markers here so each memory sample is tagged with its phase.
export MEMMON_DIR="/cscratch/memmon/$JOB_ID"
mkdir -p "$MEMMON_DIR"
export PANDO_PHASE_FILE="$MEMMON_DIR/phases.log"
echo "Memory monitor: $MEMMON_DIR (analyze: python3 memmon/analyze_memmon.py $MEMMON_DIR)"

echo "Beginning Experiment..."
echo "Params: $TEST_NAME $MESH $FILTERS $DATA_DIR $RESULTS_DIR max_filters_per_stage=${MAX_FILTERS_PER_STAGE:-default} write_batch_bytes=${WRITE_BATCH_BYTES:-default} sweep=${SWEEP:-default} scan_block_keys=${SCAN_BLOCK_KEYS:-default} intern_tags=${INTERN_TAGS:-default} mem_limit_pct=${MEM_LIMIT_PCT:-default} mem_pass_budget_mb=${MEM_PASS_BUDGET_MB:-default} mem_budget_fraction=${MEM_BUDGET_FRACTION:-default} agents_per_node=${AGENTS_PER_NODE:-default}"
# singularity run -B /cscratch -B /scratch ./../../src/sifs/pando_python.sif python3 dagify-script.py -n $TEST_NAME -m $MESH -f $FILTERS -d $DATA_DIR -r $RESULTS_DIR
singularity run -B /cscratch -B /scratch ./../../src/sifs/pando_python.sif python3 par-script.py -n $TEST_NAME -m $MESH -f $FILTERS -d $DATA_DIR -r $RESULTS_DIR

echo "Experiment Complete"
echo "Canceling Job $JOB_ID"
scancel $JOB_ID

# HERE WE WANT TO RUN THE SNAKEFILE, MOVE THE OUTPUT FILES (OTHERWISE SNAKE WILL SEE THEY ALREADY EXIST AND TERMINATE AFTER FIRST RUN)
# RUN THE FILE, MOVE THE OUTPUT, RUN THE FILE. 
# snakemake --cores all --config experiment_name="$1" filters="$2" nodes="$3"

