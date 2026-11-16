#!/bin/bash
# memmon.sh -- per-node memory monitor for Pando ParDB runs.
#
# Run one copy per node (pardb.sbatch launches it with srun --overlap).
# It needs only bash, awk, coreutils and procps on the host. It reads /proc and
# the Slurm job cgroup, and never touches the pardb processes themselves.
#
# Usage: memmon.sh OUT_DIR [INTERVAL_SECONDS]
#
# Writes, per node (HOST = short hostname):
#   OUT_DIR/HOST.info.txt     static facts: kernel/vm settings, /scratch fs, cgroup limits
#   OUT_DIR/HOST.node.csv     one row per sample: node meminfo + job cgroup + totals
#   OUT_DIR/HOST.procs.csv    one row per pardb process per sample
#   OUT_DIR/HOST.events.log   threshold crossings, processes disappearing, OOM kernel lines
#   OUT_DIR/snapshots/HOST/   detailed dumps taken when memory is high or a process dies
#
# All memory columns are MiB. Timestamps are Unix epoch seconds.
#
# Environment knobs:
#   PANDO_PHASE_FILE     file par-script.py appends "epoch phase" lines to
#                        (default OUT_DIR/phases.log); the latest phase is
#                        stamped into each node row
#   MEMMON_PROC_NAME     process name to track (default pando_pardb)
#   MEMMON_DANGER_PCT    take detailed snapshots above this % of the limit (default 85)
#   MEMMON_SNAP_GAP      minimum seconds between danger snapshots (default 30)
#   MEMMON_SLOW_EVERY    sample smaps_rollup and socket queues every N samples (default 10)
#   MEMMON_IDLE_EXIT     exit after this many seconds with no pardb processes,
#                        once some were seen (default 300)
#   MEMMON_SCRATCH       where BigSpace files live (default /scratch)

set -u

OUT_DIR=${1:?usage: memmon.sh OUT_DIR [INTERVAL_SECONDS]}
INTERVAL=${2:-2}
HOST=$(hostname -s)
PHASE_FILE=${PANDO_PHASE_FILE:-$OUT_DIR/phases.log}
PROC_NAME=${MEMMON_PROC_NAME:-pando_pardb}
DANGER_PCT=${MEMMON_DANGER_PCT:-85}
SNAP_GAP=${MEMMON_SNAP_GAP:-30}
SLOW_EVERY=${MEMMON_SLOW_EVERY:-10}
IDLE_EXIT=${MEMMON_IDLE_EXIT:-300}
SCRATCH=${MEMMON_SCRATCH:-/scratch}

mkdir -p "$OUT_DIR" "$OUT_DIR/snapshots/$HOST"
INFO=$OUT_DIR/$HOST.info.txt
NODE_CSV=$OUT_DIR/$HOST.node.csv
PROC_CSV=$OUT_DIR/$HOST.procs.csv
EVENTS=$OUT_DIR/$HOST.events.log
SNAP_DIR=$OUT_DIR/snapshots/$HOST
CLK_TCK=$(getconf CLK_TCK 2>/dev/null || echo 100)

log_event() { echo "$(date +%s) $(date '+%F %T') $*" >> "$EVENTS"; }

# ---------------------------------------------------------------- dmesg access
DMESG_CMD=""
if dmesg >/dev/null 2>&1; then DMESG_CMD="dmesg"
elif sudo -n dmesg >/dev/null 2>&1; then DMESG_CMD="sudo -n dmesg"
fi
DMESG_SEEN=0
[[ -n $DMESG_CMD ]] && DMESG_SEEN=$($DMESG_CMD 2>/dev/null | wc -l)

check_dmesg() {
    [[ -z $DMESG_CMD ]] && return
    local total new
    total=$($DMESG_CMD 2>/dev/null | wc -l)
    (( total <= DMESG_SEEN )) && { DMESG_SEEN=$total; return; }
    new=$($DMESG_CMD 2>/dev/null | tail -n $((total - DMESG_SEEN)) \
          | grep -iE 'oom|out of memory|killed process|memory cgroup|page allocation failure')
    DMESG_SEEN=$total
    if [[ -n $new ]]; then
        while IFS= read -r l; do log_event "KERNEL $l"; done <<< "$new"
    fi
}

# ---------------------------------------------------------------- cgroup
# Find the Slurm job-level memory cgroup: walk up from our own cgroup until we
# reach job_<id>. All steps (pardb and this monitor) share that limit.
CG_VER=0; CG_DIR=""
find_cgroup() {
    local base rel d
    if [[ -f /sys/fs/cgroup/cgroup.controllers ]]; then
        CG_VER=2; base=/sys/fs/cgroup
        rel=$(awk -F: '$1=="0"{print $3}' /proc/self/cgroup)
    else
        CG_VER=1; base=/sys/fs/cgroup/memory
        rel=$(awk -F: '$2 ~ /(^|,)memory(,|$)/{print $3}' /proc/self/cgroup)
    fi
    d=$base$rel
    while [[ $d != "$base" && $d != "/" && $(basename "$d") != "job_${SLURM_JOB_ID:-none}" ]]; do
        d=$(dirname "$d")
    done
    if [[ $d == "$base" || $d == "/" ]]; then d=$base$rel; fi   # not under Slurm: use our own
    [[ -d $d ]] && CG_DIR=$d || CG_VER=0
}
find_cgroup

# Prints: usage,limit,anon,file,dirty,writeback,shmem,sock,oom,oom_kill (MiB except counters)
read_cgroup() {
    if (( CG_VER == 2 )); then
        local cur max
        cur=$(cat "$CG_DIR/memory.current" 2>/dev/null || echo 0)
        max=$(cat "$CG_DIR/memory.max" 2>/dev/null || echo max)
        [[ $max == max ]] && max=0
        awk -v cur="$cur" -v max="$max" -v ev="$CG_DIR/memory.events" '
            {s[$1]=$2}
            END {
                while ((getline l < ev) > 0) { split(l, a, " "); e[a[1]]=a[2] }
                M=1048576
                printf "%d,%d,%d,%d,%d,%d,%d,%d,%d,%d", cur/M, max/M, s["anon"]/M, s["file"]/M,
                    s["file_dirty"]/M, s["file_writeback"]/M, s["shmem"]/M, s["sock"]/M,
                    e["oom"]+0, e["oom_kill"]+0
            }' "$CG_DIR/memory.stat" 2>/dev/null
    elif (( CG_VER == 1 )); then
        local cur max
        cur=$(cat "$CG_DIR/memory.usage_in_bytes" 2>/dev/null || echo 0)
        max=$(cat "$CG_DIR/memory.limit_in_bytes" 2>/dev/null || echo 0)
        # "no limit" in v1 is a huge number; treat anything over 1 EiB as none
        awk -v m="$max" 'BEGIN{exit !(m > 2^60)}' && max=0
        awk -v cur="$cur" -v max="$max" -v oc="$CG_DIR/memory.oom_control" -v fc="$CG_DIR/memory.failcnt" '
            {s[$1]=$2}
            END {
                while ((getline l < oc) > 0) { split(l, a, " "); o[a[1]]=a[2] }
                getline f < fc
                M=1048576
                # v1 has no sock counter; oom column = failcnt (times the limit was hit)
                printf "%d,%d,%d,%d,%d,%d,%d,%d,%d,%d", cur/M, max/M, s["total_rss"]/M, s["total_cache"]/M,
                    s["total_dirty"]/M, s["total_writeback"]/M, s["total_shmem"]/M, 0,
                    f+0, o["oom_kill"]+0
            }' "$CG_DIR/memory.stat" 2>/dev/null
    else
        echo "0,0,0,0,0,0,0,0,0,0"
    fi
}

# ---------------------------------------------------------------- static info
{
    echo "== memmon start $(date '+%F %T') host=$HOST job=${SLURM_JOB_ID:-} interval=${INTERVAL}s"
    echo "== uname";            uname -a
    echo "== cpus";             nproc
    echo "== free -m";          free -m
    echo "== /proc/meminfo";    cat /proc/meminfo
    echo "== vm sysctls"
    for k in overcommit_memory overcommit_ratio dirty_ratio dirty_background_ratio dirty_bytes \
             dirty_background_bytes dirty_expire_centisecs swappiness max_map_count min_free_kbytes; do
        printf '%s = %s\n' "vm.$k" "$(cat /proc/sys/vm/$k 2>/dev/null)"
    done
    echo "== transparent hugepages: $(cat /sys/kernel/mm/transparent_hugepage/enabled 2>/dev/null)"
    echo "== /scratch and /tmp filesystems (tmpfs here means BigSpace lives in RAM)"
    df -PTh "$SCRATCH" /tmp /cscratch 2>&1
    findmnt -T "$SCRATCH" 2>&1
    echo "== cgroup v$CG_VER dir: $CG_DIR"
    if (( CG_VER == 2 )); then
        for f in memory.max memory.high memory.swap.max memory.current memory.events; do
            echo "-- $f"; cat "$CG_DIR/$f" 2>&1
        done
    elif (( CG_VER == 1 )); then
        for f in memory.limit_in_bytes memory.memsw.limit_in_bytes memory.usage_in_bytes memory.oom_control; do
            echo "-- $f"; cat "$CG_DIR/$f" 2>&1
        done
    fi
    echo "== dmesg access: ${DMESG_CMD:-none (OOM kernel lines will not be captured)}"
    echo "== ulimit -a";        ulimit -a
    echo "== numa";             command -v numactl >/dev/null && numactl -H
    echo "== slurm env";        env | grep -E '^SLURM_(JOB_ID|JOB_NUM_NODES|NODELIST|MEM|CPUS|NTASKS)' | sort
} > "$INFO" 2>&1

# ---------------------------------------------------------------- CSV headers
NODE_HDR="ts,phase,mem_total,mem_avail,used,used_pct,free,cached,dirty,writeback,anon,mapped,shmem,slab_unrecl,pagetables,swap_used,committed_as,tcp_mem,cg_usage,cg_limit,cg_pct,cg_anon,cg_file,cg_dirty,cg_writeback,cg_shmem,cg_sock,cg_oom,cg_oom_kill,scratch_used,n_agents,agents_rss,agents_anon,agents_file,agents_shmem,bigspace_alloc,sock_recvq,sock_sendq"
PROC_HDR="ts,pid,agent,state,cpu_s,threads,rss,anon,file,shmem,swap,hwm,bigspace_alloc,pss,private_dirty"
[[ -s $NODE_CSV ]] || echo "$NODE_HDR" > "$NODE_CSV"
[[ -s $PROC_CSV ]] || echo "$PROC_HDR" > "$PROC_CSV"

# ---------------------------------------------------------------- process tracking
declare -A AGENT_OF     # pid -> agent id (the ",ID" in the bind address)
declare -A BIGFILE_OF   # pid -> its BigSpace backing file(s), space separated
declare -A LAST_ROW     # pid -> last proc row (for the "gone" report)
declare -A DEAD         # pids already reported gone; never tracked again

register_pid() {
    local pid=$1 args agent
    args=$(tr '\0' ' ' < /proc/$pid/cmdline 2>/dev/null) || return
    # argv[1] is "ip,ID"
    agent=$(awk '{ n = split($2, a, ","); print (n > 1 ? a[n] : "?") }' <<< "$args")
    AGENT_OF[$pid]=$agent
    BIGFILE_OF[$pid]=""
    log_event "NEW pid=$pid agent=$agent cmd=$args"
}

# BigSpace files are created lazily after start-up; look them up until found
find_bigfile() {
    local pid=$1
    [[ -n ${BIGFILE_OF[$pid]} ]] && return
    BIGFILE_OF[$pid]=$(grep -oE "$SCRATCH/bigspace\.[0-9]+/space" /proc/$pid/maps 2>/dev/null | sort -u | tr '\n' ' ')
}

# ---------------------------------------------------------------- snapshots
LAST_SNAP=0
snapshot() {
    local reason=$1 f now pid
    now=$(date +%s)
    f=$SNAP_DIR/$(date +%Y%m%d-%H%M%S)_$reason.txt
    {
        echo "== snapshot $reason at $(date '+%F %T') phase=$(current_phase)"
        echo "== /proc/meminfo"; cat /proc/meminfo
        echo "== cgroup ($CG_DIR) memory.stat"; cat "$CG_DIR/memory.stat" 2>/dev/null
        echo "== /proc/net/sockstat"; cat /proc/net/sockstat
        echo "== top processes by RSS"
        ps -eo pid,ppid,stat,rss,vsz,nlwp,pcpu,comm,args --sort=-rss 2>/dev/null | head -30 | cut -c1-220
        for pid in "${!AGENT_OF[@]}"; do
            [[ -d /proc/$pid ]] || continue
            echo "== pid $pid agent ${AGENT_OF[$pid]} status"
            grep -E '^(State|VmPeak|VmSize|VmHWM|VmRSS|RssAnon|RssFile|RssShmem|VmData|VmSwap|Threads)' /proc/$pid/status
            echo "-- smaps_rollup"; cat /proc/$pid/smaps_rollup 2>/dev/null
            echo "-- 5 largest mappings by Rss (kB)"
            awk '/^[0-9a-f]+-[0-9a-f]+ /{m=$0} /^Rss:/{print $2, m}' /proc/$pid/smaps 2>/dev/null \
                | sort -rn | head -5
        done
        echo "== TCP sockets with the largest queues (Recv-Q Send-Q local peer)"
        ss -tnH 2>/dev/null | awk '{print $2, $3, $4, $5}' | sort -rn | head -15
    } > "$f" 2>&1
    LAST_SNAP=$now
}

current_phase() {
    if [[ -s $PHASE_FILE ]]; then
        tail -n 1 "$PHASE_FILE" 2>/dev/null | cut -d' ' -f2- | tr -d ',\r'
    else
        echo "-"
    fi
}

finish() {
    log_event "EXIT ($1)"
    snapshot "exit"
    check_dmesg
    [[ -n $DMESG_CMD ]] && $DMESG_CMD -T 2>/dev/null | tail -n 200 > "$SNAP_DIR/dmesg_tail.txt"
    exit 0
}
trap 'finish SIGTERM' TERM
trap 'finish SIGINT' INT

# ---------------------------------------------------------------- main loop
log_event "START interval=${INTERVAL}s cgroup_v$CG_VER=$CG_DIR danger=${DANGER_PCT}%"
SAMPLE=0
SEEN_ANY=0
LAST_SEEN=$(date +%s)
LAST_LEVEL=0           # highest threshold (as %) reported so far
PREV_OOM_KILL=""
RECVQ=""; SENDQ=""

while true; do
    SAMPLE=$((SAMPLE + 1))
    TS=$(date +%s.%N | cut -c1-14)
    SLOW=$(( SAMPLE % SLOW_EVERY == 1 ))

    # --- which pardb processes exist now
    # A dying (OOM-killed) process can sit in state Z/X, or flicker in and out
    # of pgrep while its memory is torn down. Count it as gone the first time
    # and never pick it up again.
    declare -A NOW=()
    for pid in $(pgrep -x "$PROC_NAME" 2>/dev/null); do
        [[ -n ${DEAD[$pid]+x} ]] && continue
        read -r pstat 2>/dev/null < /proc/$pid/stat || continue
        pstat=${pstat##*) }
        [[ ${pstat%% *} == [ZX] ]] && continue
        NOW[$pid]=1
        [[ -n ${AGENT_OF[$pid]+x} ]] || register_pid "$pid"
    done

    # --- processes that disappeared since the last sample
    for pid in "${!AGENT_OF[@]}"; do
        if [[ -z ${NOW[$pid]+x} ]]; then
            log_event "GONE pid=$pid agent=${AGENT_OF[$pid]} phase=$(current_phase) last_row=${LAST_ROW[$pid]:-none}"
            unset "AGENT_OF[$pid]" "BIGFILE_OF[$pid]" "LAST_ROW[$pid]"
            DEAD[$pid]=1
            GONE_NOW=1
        fi
    done

    # --- BigSpace allocated (written) blocks per file, in one stat call
    declare -A BIGALLOC=()
    files=()
    for pid in "${!NOW[@]}"; do
        find_bigfile "$pid"
        for bf in ${BIGFILE_OF[$pid]:-}; do files+=("$bf"); done
    done
    if (( ${#files[@]} > 0 )); then
        while read -r name blocks bsize; do
            BIGALLOC[$name]=$(( blocks * bsize / 1048576 ))
        done < <(stat -c '%n %b %B' "${files[@]}" 2>/dev/null)
    fi

    # --- per-process rows
    n=0; t_rss=0; t_anon=0; t_file=0; t_shm=0; t_big=0
    for pid in "${!NOW[@]}"; do
        st=$(awk '/^VmRSS/{r=$2} /^RssAnon/{a=$2} /^RssFile/{f=$2} /^RssShmem/{s=$2}
                  /^VmSwap/{w=$2} /^VmHWM/{h=$2} /^Threads/{t=$2}
                  END{printf "%d %d %d %d %d %d %d", t, r/1024, a/1024, f/1024, s/1024, w/1024, h/1024}' \
             /proc/$pid/status 2>/dev/null) || continue
        [[ -z $st ]] && continue
        read -r thr rss anon file shm swap hwm <<< "$st"

        read -r statline 2>/dev/null < /proc/$pid/stat || continue
        rest=${statline##*) }
        read -r -a f <<< "$rest"
        pstate=${f[0]}
        [[ $pstate == [ZX] ]] && continue
        cpu_s=$(( (f[11] + f[12]) / CLK_TCK ))

        big=0
        for bf in ${BIGFILE_OF[$pid]:-}; do big=$(( big + ${BIGALLOC[$bf]:-0} )); done

        pss=""; pdirty=""
        if (( SLOW )); then
            read -r pss pdirty < <(awk '/^Pss:/{p=$2} /^Private_Dirty:/{d=$2} END{printf "%d %d", p/1024, d/1024}' \
                                   /proc/$pid/smaps_rollup 2>/dev/null)
        fi

        row="$TS,$pid,${AGENT_OF[$pid]:-?},$pstate,$cpu_s,$thr,$rss,$anon,$file,$shm,$swap,$hwm,$big,$pss,$pdirty"
        echo "$row" >> "$PROC_CSV"
        LAST_ROW[$pid]="rss=${rss}M anon=${anon}M file=${file}M shmem=${shm}M bigspace=${big}M"

        n=$((n + 1)); t_rss=$((t_rss + rss)); t_anon=$((t_anon + anon))
        t_file=$((t_file + file)); t_shm=$((t_shm + shm)); t_big=$((t_big + big))
    done

    # --- node-level numbers
    mem=$(awk '{v[$1]=$2}
        END{M=1024
            used=v["MemTotal:"]-v["MemAvailable:"]
            printf "%d,%d,%d,%.1f,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d",
              v["MemTotal:"]/M, v["MemAvailable:"]/M, used/M, 100*used/v["MemTotal:"], v["MemFree:"]/M,
              v["Cached:"]/M, v["Dirty:"]/M, v["Writeback:"]/M, v["AnonPages:"]/M, v["Mapped:"]/M,
              v["Shmem:"]/M, v["SUnreclaim:"]/M, v["PageTables:"]/M,
              (v["SwapTotal:"]-v["SwapFree:"])/M, v["Committed_AS:"]/M}' /proc/meminfo)
    used_pct=$(cut -d, -f4 <<< "$mem")
    tcp_mem=$(awk -v pg="$(getconf PAGESIZE)" '/^TCP:/{for(i=1;i<NF;i++) if($i=="mem") printf "%d", $(i+1)*pg/1048576}' /proc/net/sockstat)
    cg=$(read_cgroup)
    [[ -z $cg ]] && cg="0,0,0,0,0,0,0,0,0,0"
    IFS=, read -r cg_usage cg_limit _ _ _ _ _ _ _ cg_oom_kill <<< "$cg"
    cg_pct=0
    (( cg_limit > 0 )) && cg_pct=$(awk -v u="$cg_usage" -v l="$cg_limit" 'BEGIN{printf "%.1f", 100*u/l}')
    IFS=, read -r cg_u cg_l cg_rest <<< "$cg"
    scratch_used=$(df -Pk "$SCRATCH" 2>/dev/null | awk 'NR==2{printf "%d", $3/1024}')

    if (( SLOW )); then
        read -r RECVQ SENDQ < <(ss -tnH 2>/dev/null | awk '{r+=$2; s+=$3} END{printf "%d %d", r/1048576, s/1048576}')
    fi

    phase=$(current_phase)
    echo "$TS,$phase,$mem,${tcp_mem:-0},$cg_u,$cg_l,$cg_pct,$cg_rest,${scratch_used:-0},$n,$t_rss,$t_anon,$t_file,$t_shm,$t_big,${RECVQ:-},${SENDQ:-}" >> "$NODE_CSV"
    RECVQ=""; SENDQ=""

    # --- events and danger snapshots
    pct=$(awk -v a="$used_pct" -v b="$cg_pct" 'BEGIN{printf "%d", (a > b ? a : b)}')
    for lvl in 50 70 80 85 90 95 98; do
        if (( pct >= lvl && LAST_LEVEL < lvl )); then
            log_event "LEVEL ${lvl}% reached: node_used=${used_pct}% cgroup=${cg_pct}% phase=$phase agents_anon=${t_anon}M agents_file=${t_file}M bigspace=${t_big}M"
            LAST_LEVEL=$lvl
        fi
    done
    # Re-arm the lower levels once memory drops well below them (e.g. between phases)
    while (( LAST_LEVEL > 0 && pct < LAST_LEVEL - 15 )); do
        case $LAST_LEVEL in 98) LAST_LEVEL=95;; 95) LAST_LEVEL=90;; 90) LAST_LEVEL=85;;
            85) LAST_LEVEL=80;; 80) LAST_LEVEL=70;; 70) LAST_LEVEL=50;; *) LAST_LEVEL=0;; esac
    done
    now=$(date +%s)
    if (( pct >= DANGER_PCT && now - LAST_SNAP >= SNAP_GAP )); then
        snapshot "high${pct}pct"
    fi
    if [[ -n $PREV_OOM_KILL && $cg_oom_kill != "$PREV_OOM_KILL" ]]; then
        log_event "CGROUP oom_kill counter $PREV_OOM_KILL -> $cg_oom_kill"
        GONE_NOW=1
    fi
    PREV_OOM_KILL=$cg_oom_kill
    if [[ -n ${GONE_NOW:-} ]]; then
        snapshot "process_gone"
        check_dmesg
        unset GONE_NOW
    elif (( SAMPLE % 30 == 0 )); then
        check_dmesg
    fi

    # --- exit when the run is over
    if (( n > 0 )); then SEEN_ANY=1; LAST_SEEN=$now; fi
    if (( SEEN_ANY && n == 0 && now - LAST_SEEN >= IDLE_EXIT )); then
        finish "no $PROC_NAME processes for ${IDLE_EXIT}s"
    fi

    sleep "$INTERVAL" &
    wait $!
done
