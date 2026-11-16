# memmon: memory monitoring for pardb runs

`pardb.sbatch` starts `memmon.sh` on every node (`srun --overlap`, one task per node).
`par-script.py` writes phase markers to `$PANDO_PHASE_FILE`, which `run_experiment.sh` sets.
Everything lands in `/cscratch/memmon/<jobid>/`.

After the run, or while it is still going:

    python3 memmon/analyze_memmon.py /cscratch/memmon/<jobid>
    python3 memmon/analyze_memmon.py /cscratch/memmon/<jobid> --node en123 --window 300

Watch a node live:

    tail -f /cscratch/memmon/<jobid>/<host>.events.log
    tail -f /cscratch/memmon/<jobid>/<host>.node.csv | cut -d, -f1-6,11,19-21,33,36

## What is measured (all MiB)

- `HOST.node.csv`: `/proc/meminfo` (anon, page cache, dirty, writeback, shmem, slab,
  page tables), kernel TCP buffer memory, the Slurm **job** cgroup (usage, limit,
  anon/file/dirty, OOM counters), `/scratch` usage, and totals over the pardb processes.
- `HOST.procs.csv`: per pardb process: heap (`RssAnon`), file-backed RSS, written
  BigSpace blocks, CPU seconds (busy vs idle), threads, and every 10th sample
  `smaps_rollup` PSS / Private_Dirty.
- `HOST.events.log`: memory levels crossed (50..98%), pardb processes starting and
  dying (with their last memory numbers), cgroup OOM kills, kernel OOM lines.
- `snapshots/HOST/`: full dumps once memory is above 85% (at most one every 30s), when a
  process dies, and at exit.

## How to read it

BigSpace (`-M300`) is a `MAP_SHARED` file in `/scratch`, so stored entry **values** show
up as page cache/file RSS, not heap. Everything else is heap (anon):

- staged entries (`SeqDB::new_entries_`) until `stage_close`
- ZMQ message queues; HWM is 0, so they are unbounded
- per-entry tags, `c_tags_`, hash slots and the `entries_by_tag_` index

Comparing heap growth with BigSpace growth per agent, plus CPU busy/idle, tells these apart.
