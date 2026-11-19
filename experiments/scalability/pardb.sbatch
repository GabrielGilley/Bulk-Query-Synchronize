#!/bin/bash

###################
## Slurm parameters

#SBATCH --ntasks-per-node=16
#SBATCH -o log/%j.out
#SBATCH --exclusive
#SBATCH -c 4

#####################
## Cluster parameters

### Script arguments (passed after the script name: sbatch ... pardb.sbatch [N] [BYTES] [SWEEP] [KEYS] [INTERN] [PCT] [MB] [FRACTION] [AGENTS])
# $1: max filter runs per processing pass (optional; empty = pando_pardb default)
MAX_FILTERS_PER_STAGE="${1:-}"
# $2: write batch flush size in bytes (optional; empty = pando_pardb default, 0 = off)
WRITE_BATCH_BYTES="${2:-}"
# $3: how a process request ends, until-done or once (optional; empty = pando_pardb default, until-done)
SWEEP="${3:-}"
# $4: keys the filter scan fetches per request (optional; empty = pando_pardb default, 4096)
SCAN_BLOCK_KEYS="${4:-}"
# $5: store each distinct tag once per process, on or off (optional; empty = pando_pardb default, on)
INTERN_TAGS="${5:-}"
# $6-$9: memory limits (optional; empty = pando_pardb defaults): percent of the
# node's or job's memory to stay below, fixed output budget per pass in MiB,
# share of the headroom per pass, agents sharing a node's memory
MEM_LIMIT_PCT="${6:-}"
MEM_PASS_BUDGET_MB="${7:-}"
MEM_BUDGET_FRACTION="${8:-}"
AGENTS_PER_NODE="${9:-}"

### Pando connection information
echo "outputting"
node=$(echo $SLURM_JOB_NODELIST | cut -d '-' -f1 - | cut -d ',' -f1 - | tr -d '[')
#FIXME this is environment specific
ip=$(host "$node.100g" | awk '{print $4}')
PANDO_MESH_ENTRY="$ip,0"

# Command= also lists the script's arguments; keep only the script path
SCRIPT_PATH=$(scontrol show job "$SLURM_JOBID" | awk -F= '/Command=/{print $2}' | awk '{print $1}')
SCRIPT_DIR=$(dirname "$SCRIPT_PATH")
PANDO_PARDB_BIN="$SCRIPT_DIR/../../src/sifs/pando_pardb.sif"

PANDO_BIND_IP_FILE=/opt/installer/ip
echo REMOVING FILES
srun bash -c 'sudo rm -rf /tmp/elga.ipc* /scratch/bigspace*'
srun bash -c 'rm -rf /tmp/elga.ipc* /scratch/bigspace*'
echo CHECKING SCRATCH SPACE
srun bash -c 'ls /scratch'
srun bash -c 'ls /tmp'

### Memory monitor: one copy per node, alongside the pardb tasks.
# --overlap lets it share the CPUs the pardb step already holds.
# Output: $MEMMON_DIR (also where par-script.py writes phases.log)
MEMMON_DIR=${MEMMON_DIR:-/cscratch/memmon/$SLURM_JOB_ID}
mkdir -p "$MEMMON_DIR"
echo "MEMMON_DIR=$MEMMON_DIR"
srun --overlap -N "$SLURM_JOB_NUM_NODES" --ntasks-per-node=1 -c 1 --kill-on-bad-exit=0 \
    bash "$SCRIPT_DIR/memmon/memmon.sh" "$MEMMON_DIR" "${MEMMON_INTERVAL:-2}" &
MEMMON_PID=$!

echo START
date
echo $PANDO_MESH_ENTRY 
echo $PANDO_PARDB_BIN
echo $PANDO_BIND_IP_FILE
echo "MAX_FILTERS_PER_STAGE=${MAX_FILTERS_PER_STAGE:-default}"
echo "WRITE_BATCH_BYTES=${WRITE_BATCH_BYTES:-default}"
echo "SWEEP=${SWEEP:-default}"
echo "SCAN_BLOCK_KEYS=${SCAN_BLOCK_KEYS:-default}"
echo "INTERN_TAGS=${INTERN_TAGS:-default}"
echo "MEM_LIMIT_PCT=${MEM_LIMIT_PCT:-default} MEM_PASS_BUDGET_MB=${MEM_PASS_BUDGET_MB:-default} MEM_BUDGET_FRACTION=${MEM_BUDGET_FRACTION:-default} AGENTS_PER_NODE=${AGENTS_PER_NODE:-default}"
srun --unbuffered bash pardb.internal "$PANDO_MESH_ENTRY" "$PANDO_PARDB_BIN" "$PANDO_BIND_IP_FILE" "$MAX_FILTERS_PER_STAGE" "$WRITE_BATCH_BYTES" "$SWEEP" "$SCAN_BLOCK_KEYS" "$INTERN_TAGS" \
    "$MEM_LIMIT_PCT" "$MEM_PASS_BUDGET_MB" "$MEM_BUDGET_FRACTION" "$AGENTS_PER_NODE"

echo FINISH
date

# Stop the monitor (it also exits by itself once pardb has been gone a while)
kill "$MEMMON_PID" 2>/dev/null
wait "$MEMMON_PID" 2>/dev/null

