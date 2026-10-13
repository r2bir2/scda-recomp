"""Local, offline A/B trace comparator for the v0.7.x-merge SCDA
regression (docs/SCDA_WORKLOG.md, "continued 26").

Reads two scda_ab_trace.log files (OLD: pre-merge checkpoint build, NEW:
merged build; each line "pc esp ebx esi edi ebp", all 8-hex-digit,
written by scda_ab_trace_pc in projects/scda/src/scda_ab_trace.c) and
walks them in lockstep by guest PC. Never loads a full trace into
memory -- both files are read as streaming iterators, and only a small
bounded window around the divergence point is ever held at once.

Output is deliberately minimal:
  - last confirmed-common event (both sides' pc/esp/ebx/esi/edi/ebp)
  - first event where the two sides disagree
  - a few lines of context from each side around that point

Usage:
    py -3 -m tools.debug.ab_trace_diff OLD.log NEW.log [--resync N]
"""
from __future__ import annotations
import argparse
import sys


FIELDS = ("pc", "esp", "ebx", "esi", "edi", "ebp")


def parse_line(line):
    parts = line.split()
    if len(parts) != 6:
        return None
    try:
        return tuple(int(p, 16) for p in parts)
    except ValueError:
        return None


def fmt(event):
    if event is None:
        return "(none)"
    return " ".join(f"{name}=0x{val:08X}" for name, val in zip(FIELDS, event))


def read_events(path):
    """Yields parsed 6-tuples, skipping unparsable lines."""
    with open(path, "r", encoding="ascii", errors="replace") as f:
        for line in f:
            ev = parse_line(line)
            if ev is not None:
                yield ev


def try_resync(old_it, new_it, old_pending, new_pending, window):
    """After a pc mismatch, look up to `window` events ahead on each side
    for the other side's current pc, to tell a trivial single-block skip
    from a genuine control-flow divergence. Returns (resynced, old_skipped,
    new_skipped, old_pending, new_pending) -- old_pending/new_pending are
    the still-unconsumed current events after any resync.
    """
    old_buf = [old_pending]
    new_buf = [new_pending]
    for _ in range(window):
        # Does new's buffer contain old's current pc?
        for i, ev in enumerate(new_buf):
            if ev is not None and old_buf[-1] is not None and ev[0] == old_buf[-1][0]:
                return True, old_buf[:-1], new_buf[:i], old_buf[-1], new_buf[i]
        for i, ev in enumerate(old_buf):
            if ev is not None and new_buf[-1] is not None and ev[0] == new_buf[-1][0]:
                return True, old_buf[:i], new_buf[:-1], old_buf[i], new_buf[-1]
        nxt_old = next(old_it, None)
        nxt_new = next(new_it, None)
        if nxt_old is not None:
            old_buf.append(nxt_old)
        if nxt_new is not None:
            new_buf.append(nxt_new)
        if nxt_old is None and nxt_new is None:
            break
    return False, old_buf, new_buf, old_buf[-1] if old_buf else None, new_buf[-1] if new_buf else None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("old_path")
    ap.add_argument("new_path")
    ap.add_argument("--resync", type=int, default=8,
                     help="lookahead window (events per side) to try resyncing after a pc mismatch")
    ap.add_argument("--max-events", type=int, default=50_000_000,
                     help="safety cap on events walked before giving up")
    args = ap.parse_args()

    old_it = read_events(args.old_path)
    new_it = read_events(args.new_path)

    last_common = None
    count = 0

    while count < args.max_events:
        old_ev = next(old_it, None)
        new_ev = next(new_it, None)
        count += 1

        if old_ev is None and new_ev is None:
            print(f"Reached end of both traces with no divergence found ({count} events each).")
            print(f"Last common event: {fmt(last_common)}")
            return

        if old_ev is None:
            print(f"OLD trace ended first (after {count - 1} common events); NEW continues.")
            print(f"Last common event: {fmt(last_common)}")
            print(f"First NEW-only event: {fmt(new_ev)}")
            return

        if new_ev is None:
            print(f"NEW trace ended first (after {count - 1} common events); OLD continues.")
            print(f"Last common event: {fmt(last_common)}")
            print(f"First OLD-only event: {fmt(old_ev)}")
            return

        if old_ev[0] == new_ev[0]:
            last_common = old_ev  # pc matches; note if registers differ too
            if old_ev != new_ev:
                print(f"NOTE: pc matches but registers differ at event {count}:")
                print(f"  OLD: {fmt(old_ev)}")
                print(f"  NEW: {fmt(new_ev)}")
                print("  (continuing lockstep walk -- this alone is not the divergence point)")
            continue

        # pc mismatch -- attempt a bounded resync before declaring divergence
        resynced, old_skipped, new_skipped, old_resumed, new_resumed = try_resync(
            old_it, new_it, old_ev, new_ev, args.resync)

        if resynced:
            print(f"Resynced after a local skip at event {count}:")
            for ev in old_skipped:
                print(f"  OLD-only (skipped): {fmt(ev)}")
            for ev in new_skipped:
                print(f"  NEW-only (skipped): {fmt(ev)}")
            last_common = old_resumed
            old_ev, new_ev = old_resumed, new_resumed
            # fall through: continue the main loop from here
            # (re-inject by looping manually since old_ev/new_ev consumed this iteration)
            while old_ev[0] == new_ev[0]:
                last_common = old_ev
                old_ev = next(old_it, None)
                new_ev = next(new_it, None)
                count += 1
                if old_ev is None or new_ev is None:
                    break
            if old_ev is not None and new_ev is not None and old_ev[0] == new_ev[0]:
                continue
            if old_ev is None or new_ev is None:
                if old_ev is None and new_ev is None:
                    print(f"Reached end of both traces after resync ({count} events each).")
                elif old_ev is None:
                    print(f"OLD trace ended first after resync; NEW continues.")
                    print(f"First NEW-only event: {fmt(new_ev)}")
                else:
                    print(f"NEW trace ended first after resync; OLD continues.")
                    print(f"First OLD-only event: {fmt(old_ev)}")
                print(f"Last common event: {fmt(last_common)}")
                return
            # another real mismatch after resync -- report it below
            old_pending, new_pending = old_ev, new_ev
        else:
            old_pending, new_pending = old_ev, new_ev

        print("=" * 70)
        print(f"FIRST DIVERGENCE at event #{count} (no resync found within {args.resync} events)")
        print(f"Last confirmed common event: {fmt(last_common)}")
        print(f"OLD: {fmt(old_pending)}")
        print(f"NEW: {fmt(new_pending)}")
        print("Trailing context (up to 5 more events each side):")
        for i in range(5):
            o = next(old_it, None)
            n = next(new_it, None)
            print(f"  OLD[+{i+1}]: {fmt(o)}")
            print(f"  NEW[+{i+1}]: {fmt(n)}")
            if o is None and n is None:
                break
        print("=" * 70)
        return

    print(f"Gave up after {args.max_events} events without finding a divergence or EOF.")
    print(f"Last common event: {fmt(last_common)}")


if __name__ == "__main__":
    main()
