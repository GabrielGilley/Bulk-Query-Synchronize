#!/usr/bin/env python3
"""Summarize a memmon.sh run: where memory went, and which node/agent ran out first.

Usage: python3 analyze_memmon.py MON_DIR [--node HOST] [--window SECONDS]

Reads MON_DIR/*.node.csv, *.procs.csv, *.events.log and phases.log (all written
by memmon.sh / par-script.py). Standard library only.
"""

import argparse
import csv
import glob
import os
import statistics
import sys
from collections import defaultdict


def num(v):
    try:
        return float(v)
    except (TypeError, ValueError):
        return None


DAMAGE = []  # (file, description) for files with NUL bytes or broken rows


def read_clean(path):
    """Return the file's lines with NUL bytes removed.

    A node that dies mid-write on NFS leaves runs of NUL bytes where its last
    appends should be; note where they are, since that marks when the node died.
    """
    with open(path, "rb") as f:
        data = f.read()
    nul = data.count(b"\0")
    if nul:
        first = data.index(b"\0")
        line_no = data.count(b"\n", 0, first) + 1
        DAMAGE.append((os.path.basename(path), f"{nul} NUL bytes, first at line {line_no}"))
        data = data.replace(b"\0", b"")
    return data.decode("utf-8", errors="replace").splitlines()


def load_csv(path):
    lines = read_clean(path)
    if not lines:
        return []
    header = next(csv.reader([lines[0]]))
    rows, bad = [], 0
    for fields in csv.reader(lines[1:]):
        if len(fields) != len(header) or num(fields[0]) is None:
            bad += 1
            continue
        rows.append({k: (v if k in ("phase", "state") else num(v)) for k, v in zip(header, fields)})
    if bad:
        DAMAGE.append((os.path.basename(path), f"{bad} malformed rows skipped"))
    rows.sort(key=lambda r: r["ts"])
    return rows


def load_events(path):
    out = []
    if not os.path.exists(path):
        return out
    for line in read_clean(path):
        parts = line.split(" ", 3)
        if len(parts) == 4 and num(parts[0]) is not None:
            out.append((float(parts[0]), parts[3]))
    out.sort(key=lambda e: e[0])
    # Older monitor versions could report one dying pid as GONE many times; keep the first
    seen, dedup = set(), []
    for ts, msg in out:
        if msg.startswith("GONE "):
            pid = msg.split()[1]
            if pid in seen:
                continue
            seen.add(pid)
        dedup.append((ts, msg))
    return dedup


def load_phases(mon_dir):
    path = os.environ.get("PANDO_PHASE_FILE") or os.path.join(mon_dir, "phases.log")
    out = []
    if os.path.exists(path):
        for line in read_clean(path):
            ts, _, name = line.strip().partition(" ")
            if num(ts) is not None:
                out.append((float(ts), name))
    return out


def gib(mib):
    return "   -  " if mib is None else f"{mib / 1024:6.1f}"


def fmt_t(ts, t0):
    if ts is None:
        return "-"
    s = int(ts - t0)
    return f"+{s // 3600:d}:{s % 3600 // 60:02d}:{s % 60:02d}"


def at_or_before(rows, ts):
    best = None
    for r in rows:
        if r["ts"] <= ts:
            best = r
        else:
            break
    return best


def rate(rows, end_ts, window, key):
    """MiB/s change of `key` over the `window` seconds ending at end_ts."""
    end = at_or_before(rows, end_ts)
    # If the run is shorter than the window, use the whole run
    start = at_or_before(rows, end_ts - window) or (rows[0] if rows else None)
    if not end or not start or end is start:
        return None
    a, b = start.get(key), end.get(key)
    if a is None or b is None:
        return None
    return (b - a) / (end["ts"] - start["ts"])


def failure_time(node_rows, events):
    """First sign of death on a node: a pardb process vanished, an OOM kill, or a kernel OOM line."""
    for ts, msg in events:
        if msg.startswith("GONE") or msg.startswith("CGROUP oom_kill") or (
            msg.startswith("KERNEL") and ("oom" in msg.lower() or "killed process" in msg.lower())
        ):
            return ts, msg
    return None, None


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("mon_dir")
    ap.add_argument("--node", help="analyze this host in detail instead of the first one to fail")
    ap.add_argument("--window", type=int, default=600, help="seconds before the failure to examine (default 600)")
    args = ap.parse_args()

    hosts = sorted(os.path.basename(p)[: -len(".node.csv")] for p in glob.glob(os.path.join(args.mon_dir, "*.node.csv")))
    if not hosts:
        sys.exit(f"no *.node.csv files in {args.mon_dir}")

    nodes, procs, events = {}, {}, {}
    for h in hosts:
        nodes[h] = load_csv(os.path.join(args.mon_dir, f"{h}.node.csv"))
        p = os.path.join(args.mon_dir, f"{h}.procs.csv")
        # Zombie/exiting rows (state Z or X) report 0 memory; drop them
        procs[h] = [r for r in load_csv(p) if r.get("state") not in ("Z", "X")] if os.path.exists(p) else []
        events[h] = load_events(os.path.join(args.mon_dir, f"{h}.events.log"))
    phases = load_phases(args.mon_dir)

    empty = [h for h in hosts if not nodes[h]]
    hosts = [h for h in hosts if nodes[h]]
    if not hosts:
        sys.exit("no usable samples in any *.node.csv")
    if args.node and args.node not in hosts:
        sys.exit(f"--node {args.node}: no samples for that host (have: {', '.join(hosts)})")

    t0 = min(nodes[h][0]["ts"] for h in hosts)

    if DAMAGE or empty:
        print("=" * 78)
        print("DAMAGED FILES (NUL bytes usually mean the node died before its last NFS writes landed)")
        for name, what in DAMAGE:
            print(f"  {name}: {what}")
        for h in empty:
            print(f"  {h}.node.csv: no usable rows")
        for h in hosts:
            if any(n.startswith(h + ".") for n, _ in DAMAGE):
                print(f"  {h}: last good sample at {fmt_t(nodes[h][-1]['ts'], t0)}")
        print()

    # ------------------------------------------------------------ phases
    print("=" * 78)
    print("PHASES (from par-script.py)")
    if phases:
        for i, (ts, name) in enumerate(phases):
            nxt = phases[i + 1][0] if i + 1 < len(phases) else None
            dur = f"{(nxt - ts) / 60:7.1f} min" if nxt else "   (last)"
            print(f"  {fmt_t(ts, t0)}  {dur}  {name}")
    else:
        print("  no phases.log found; phase columns will be '-'")

    # ------------------------------------------------------------ per node
    print()
    print("=" * 78)
    print("PER NODE  (GiB; 'limit' is the Slurm job cgroup limit if set, else MemTotal)")
    print(f"  {'host':<14}{'limit':>7}{'peak':>7}{'peak%':>7}  {'at':>9}  {'failed':>9}  phase at peak")
    fails = {}
    for h in hosts:
        rows = nodes[h]
        if not rows:
            continue
        limit = rows[-1]["cg_limit"] or rows[-1]["mem_total"]

        def usage(r):
            return r["cg_usage"] if r["cg_limit"] else r["used"]

        peak = max(rows, key=usage)
        fts, fmsg = failure_time(rows, events[h])
        fails[h] = (fts, fmsg)
        print(
            f"  {h:<14}{gib(limit)} {gib(usage(peak))}{100 * usage(peak) / limit:6.1f}%"
            f"  {fmt_t(peak['ts'], t0):>9}  {fmt_t(fts, t0) if fts else 'no':>9}  {peak['phase']}"
        )

    # ------------------------------------------------------------ peak per phase across the cluster
    print()
    print("=" * 78)
    print("PEAK PER PHASE  (max over nodes; GiB)")
    print(f"  {'phase':<30}{'node used%':>11}{'cgroup%':>9}{'agents anon':>13}{'bigspace':>10}{'node dirty':>11}")
    by_phase = defaultdict(lambda: [0, 0, 0, 0, 0])
    order = []
    for h in hosts:
        for r in nodes[h]:
            p = r["phase"]
            if p not in by_phase:
                order.append(p)
            b = by_phase[p]
            b[0] = max(b[0], r["used_pct"] or 0)
            b[1] = max(b[1], r["cg_pct"] or 0)
            b[2] = max(b[2], r["agents_anon"] or 0)
            b[3] = max(b[3], r["bigspace_alloc"] or 0)
            b[4] = max(b[4], r["dirty"] or 0)
    for p in order:
        b = by_phase[p]
        print(f"  {p[:29]:<30}{b[0]:10.1f}%{b[1]:8.1f}% {gib(b[2]):>12} {gib(b[3]):>9} {gib(b[4]):>10}")

    # ------------------------------------------------------------ focus node
    failed = sorted((v[0], h) for h, v in fails.items() if v[0])
    if args.node:
        focus = args.node
    elif failed:
        focus = failed[0][1]
    else:
        focus = max(hosts, key=lambda h: max((r["cg_pct"] or r["used_pct"] or 0) for r in nodes[h]))
    rows = nodes[focus]
    fts, fmsg = fails.get(focus, (None, None))
    end_ts = fts if fts else rows[-1]["ts"]
    last = at_or_before(rows, end_ts) or rows[-1]
    limit = last["cg_limit"] or last["mem_total"]
    W = args.window

    print()
    print("=" * 78)
    if fts:
        print(f"FIRST FAILURE: {focus} at {fmt_t(fts, t0)} during phase '{last['phase']}'")
        print(f"  {fmsg[:200]}")
        others = [f"{h} {fmt_t(t, t0)}" for t, h in failed[1:]]
        if others:
            print(f"  later failures: {', '.join(others)}")
    else:
        print(f"NO FAILURE RECORDED. Showing the node closest to its limit: {focus}")

    print()
    print(f"NODE {focus}: last sample before failure ({fmt_t(last['ts'], t0)}), GiB, and rate over the last {W}s / 60s (MiB/s)")
    comps = [
        ("total used (node)", "used"),
        ("job cgroup usage", "cg_usage"),
        ("  anon (heap)", "anon"),
        ("  page cache", "cached"),
        ("    of which dirty", "dirty"),
        ("    of which writeback", "writeback"),
        ("    of which shmem/tmpfs", "shmem"),
        ("  unreclaimable slab", "slab_unrecl"),
        ("  page tables", "pagetables"),
        ("  TCP socket buffers", "tcp_mem"),
        ("pardb anon (sum)", "agents_anon"),
        ("pardb file-backed rss (sum)", "agents_file"),
        ("BigSpace written (sum)", "bigspace_alloc"),
        ("/scratch used", "scratch_used"),
    ]
    for label, k in comps:
        r1, r2 = rate(rows, end_ts, W, k), rate(rows, end_ts, 60, k)
        s1 = f"{r1:+9.1f}" if r1 is not None else "        -"
        s2 = f"{r2:+9.1f}" if r2 is not None else "        -"
        print(f"  {label:<30}{gib(last.get(k))}   {s1} {s2}")
    print(f"  {'limit':<30}{gib(limit)}")
    if last.get("sock_recvq") is not None:
        print(f"  TCP Recv-Q / Send-Q (MiB, last slow sample): {last.get('sock_recvq')} / {last.get('sock_sendq')}")

    # ------------------------------------------------------------ agents on the focus node
    prow = [r for r in procs[focus] if r["ts"] <= end_ts]
    by_pid = defaultdict(list)
    for r in prow:
        by_pid[int(r["pid"])].append(r)
    latest = []
    for pid, rs in by_pid.items():
        lr = rs[-1]
        if lr["ts"] < end_ts - 30:  # not alive near the failure
            continue
        prev = at_or_before(rs, lr["ts"] - 60) or rs[0]
        dt = lr["ts"] - prev["ts"]
        cpu = 100 * (lr["cpu_s"] - prev["cpu_s"]) / dt if dt > 0 else None
        grow = (lr["anon"] - prev["anon"]) / dt if dt > 0 else None
        prev_w = at_or_before(rs, lr["ts"] - W) or rs[0]
        dtw = lr["ts"] - prev_w["ts"]
        grow_w = (lr["anon"] - prev_w["anon"]) / dtw if dtw > 0 else None
        big_w = (lr["bigspace_alloc"] - prev_w["bigspace_alloc"]) / dtw if dtw > 0 else None
        latest.append((lr, cpu, grow, grow_w, big_w))
    latest.sort(key=lambda x: -(x[0]["anon"] or 0))

    print()
    print(f"AGENTS ON {focus} at failure (sorted by heap). cpu% over the last 60s (100% = one core busy)")
    print(f"  {'agent':>5} {'pid':>8} {'st':>2} {'rss':>7} {'anon':>7} {'bigsp':>7} {'hwm':>7} {'cpu%':>6} "
          f"{'anon/s 60s':>10} {f'anon/s {W}s':>11} {f'big/s {W}s':>10}")
    for lr, cpu, grow, grow_w, big_w in latest:
        def f(v, w=6, p=0):
            return f"{v:{w}.{p}f}" if v is not None else " " * (w - 1) + "-"
        print(f"  {lr['agent']:>5.0f} {lr['pid']:>8.0f} {lr['state']:>2} {gib(lr['rss'])} {gib(lr['anon'])} "
              f"{gib(lr['bigspace_alloc'])} {gib(lr['hwm'])} {f(cpu)} {f(grow, 10, 1)} {f(grow_w, 11, 1)} {f(big_w, 10, 1)}")
    gone = [m for t, m in events[focus] if m.startswith("GONE") and t >= end_ts - 5]
    for m in gone[:20]:
        print(f"  {m[:150]}")

    # ------------------------------------------------------------ diagnosis
    print()
    print("=" * 78)
    print("DIAGNOSIS (heuristics; check against the snapshots)")
    notes = []
    if last["cg_limit"] and (last["cg_pct"] or 0) >= 90 and (last["used_pct"] or 0) < 90:
        notes.append(
            f"The job cgroup limit was hit ({last['cg_pct']:.0f}% of {limit / 1024:.0f} GiB) while the node itself "
            f"was {last['used_pct']:.0f}% used. That is Slurm's --mem 360000 killing the job, not the node running "
            "out of RAM. Page cache written by the processes (BigSpace mmap) counts against this limit.")
    share = {k: (last.get(k) or 0) / limit for k in ("anon", "cached", "dirty", "writeback", "shmem", "tcp_mem", "slab_unrecl")}
    dom = max(("anon", "cached", "slab_unrecl", "tcp_mem"), key=lambda k: share[k])
    notes.append(f"Largest component at failure: {dom} = {share[dom] * 100:.0f}% of the limit "
                 f"(anon {share['anon'] * 100:.0f}%, page cache {share['cached'] * 100:.0f}%, "
                 f"dirty {share['dirty'] * 100:.0f}%, shmem {share['shmem'] * 100:.0f}%).")
    if share["shmem"] > 0.2:
        notes.append("Shmem is large: /scratch is probably tmpfs (see HOST.info.txt), so every BigSpace page "
                     "is RAM that can't be reclaimed. Check that bigspace_alloc tracks shmem.")
    if share["dirty"] + share["writeback"] > 0.15:
        notes.append("A lot of dirty page cache: BigSpace mmap writes outpace writeback to /scratch. "
                     "Compare with vm.dirty_* in HOST.info.txt.")
    if share["tcp_mem"] > 0.05:
        notes.append("Kernel TCP buffers are significant. Look at the largest Recv-Q sockets in the snapshots.")
    ra, rb = rate(rows, end_ts, W, "agents_anon"), rate(rows, end_ts, W, "bigspace_alloc")
    if ra is not None and ra > 1:
        if rb is None or rb < 0.1 * ra:
            notes.append(
                f"Over the last {W}s the pardb heap grew {ra:.0f} MiB/s while BigSpace grew "
                f"{(rb or 0):.0f} MiB/s. Data is piling up on the heap and not reaching PandoMap: entries waiting "
                "in SeqDB::new_entries_ (staged until stage_close), and/or messages queued inside ZMQ "
                "(HWM is 0, so queues are unbounded) for agents too busy to receive them.")
        else:
            notes.append(
                f"Heap ({ra:.0f} MiB/s) and BigSpace ({rb:.0f} MiB/s) grew together over the last {W}s: entries are "
                "being inserted (stage_close). Heap per stored entry (tags unordered_set, c_tags, hash slots, "
                "entries_by_tag_ set nodes) is the likely cost; compare heap growth to entry counts.")
    if latest:
        anons = [x[0]["anon"] or 0 for x in latest]
        med = statistics.median(anons)
        if med > 0 and anons[0] > 2 * med:
            top = latest[0]
            notes.append(
                f"Skew: agent {top[0]['agent']:.0f} holds {anons[0] / 1024:.1f} GiB of heap vs median "
                f"{med / 1024:.1f} GiB. One agent receiving far more data (hot keys/ownership) or not draining.")
        busy = [x for x in latest if x[1] is not None and x[1] > 80]
        growing = [x for x in latest if x[2] is not None and x[2] > 5]
        if growing:
            gb = sum(1 for x in growing if x in busy)
            notes.append(
                f"{len(growing)} agents grew >5 MiB/s in the last minute; {gb} of them were CPU-busy (>80%). "
                "Busy agents that are growing fit a backlog building up behind a long filter loop. "
                "Idle agents that are growing fit staging or stage_close.")
    for n in notes:
        print("  - " + n)
    print()
    print(f"Snapshots: {os.path.join(args.mon_dir, 'snapshots', focus)}  (largest mappings, smaps_rollup, top RSS, socket queues)")


if __name__ == "__main__":
    main()
