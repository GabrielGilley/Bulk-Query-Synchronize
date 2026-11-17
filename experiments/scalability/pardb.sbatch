#!/bin/bash

###################
## Slurm parameters

#SBATCH --ntasks-per-node=16
#SBATCH -o log/%j.out
#SBATCH --exclusive
#SBATCH -c 4

#####################
## Cluster parameters

### Script arguments (passed after the script name: sbatch ... pardb.sbatch [N])
# $1: max filter runs per processing pass (optional; empty = pando_pardb default)
MAX_FILTERS_PER_STAGE="${1:-}"

### Pando connection information
echo "outputting"
node=$(echo $SLURM_JOB_NODELIST | cut -d '-' -f1 - | cut -d ',' -f1 - | tr -d '[')
#FIXME this is environment specific
ip=$(host "$node.100g" | awk '{print $4}')
PANDO_MESH_ENTRY="$ip,0"

SCRIPT_PATH=$(scontrol show job $SLURM_JOBID | awk -F= '/Command=/{print $2}')
SCRIPT_DIR=$(dirname $SCRIPT_PATH)
PANDO_PARDB_BIN=$SCRIPT_DIR/../../src/sifs/pando_pardb.sif

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
srun --unbuffered bash pardb.internal $PANDO_MESH_ENTRY $PANDO_PARDB_BIN $PANDO_BIND_IP_FILE "$MAX_FILTERS_PER_STAGE"

echo FINISH
date

# Stop the monitor (it also exits by itself once pardb has been gone a while)
kill "$MEMMON_PID" 2>/dev/null
wait "$MEMMON_PID" 2>/dev/null

