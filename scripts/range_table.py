#!/usr/bin/env python3
"""
Live per-tag / per-anchor range statistics table, read straight from the
boards' serial $RNG streams (same multi-port ingestion and frame_index/slot
based labeling as stream_ranges.py -- see that file for why labels are
derived from position rather than node_id).

For every (tag, anchor) pair, over a rolling time window:
    rate_hz   samples in the window / window length
    mean_m    mean of range_m in the window
    std_m     population stddev of range_m in the window (blank until >=2
              samples)
    n         sample count in the window

A serial device can only be held open by one process at a time on macOS, so
this cannot open the same boards stream_ranges.py already has open -- doing
so would silently steal a subset of the ports instead of sharing the data
(this is exactly what happened the first time these two tools were run
side by side: each grabbed whichever ports it reached first, so each only
ever showed half the fleet). By default this instead tails the fan-out file
stream_ranges.py writes (/tmp/uwb_ranges.jsonl), so both tools can run at
once, each in its own terminal, reading the boards exactly once between
them. Pass --serial to open the ports directly instead (only when
stream_ranges.py is NOT already running against them).

Usage:
    ./scripts/stream_ranges.py &              # must be running first
    ./scripts/range_table.py                  # tails its fan-out file
    ./scripts/range_table.py -w 5             # 5s window instead of 10s
    ./scripts/range_table.py --serial          # open ports directly (standalone)
    ./scripts/range_table.py --serial /dev/cu.usbserial-AAAA
"""

import sys
import glob
import json
import time
import queue
import threading
import statistics
from collections import defaultdict, deque

import serial

BAUD = 921600
DEFAULT_WINDOW_S = 10.0
REFRESH_S = 1.0
FANOUT_PATH = "/tmp/uwb_ranges.jsonl"


def tag_label(frame_idx):
    return "T%02d" % (frame_idx + 1)


def anchor_label(slot):
    return "A%02d" % (slot + 1)


def parse_args():
    window = DEFAULT_WINDOW_S
    ports = []
    use_serial = False
    args = sys.argv[1:]
    i = 0
    while i < len(args):
        a = args[i]
        if a in ("-w", "--window"):
            i += 1
            window = float(args[i])
        elif a == "--serial":
            use_serial = True
        elif not a.startswith("-"):
            ports.append(a)
            use_serial = True
        i += 1
    return ports, window, use_serial


def find_ports(explicit):
    if explicit:
        return explicit
    ports = sorted(glob.glob("/dev/cu.usbserial-*"))
    if not ports:
        sys.exit("Error: no ESP32 serial ports found at /dev/cu.usbserial-*")
    return ports


def reader(port, out_q, stop_evt):
    try:
        s = serial.Serial(port, BAUD, timeout=1, dsrdtr=False, rtscts=False)
    except Exception as e:
        print(f"# {port}: open failed: {e}", file=sys.stderr)
        return

    # See the matching comment in stream_ranges.py: a clean reset-and-release
    # on every open avoids depending on whatever DTR/RTS state a previous
    # connection to this port left behind.
    s.setDTR(False)
    s.setRTS(True)
    time.sleep(0.1)
    s.setRTS(False)
    time.sleep(0.1)

    print(f"# {port}: listening", file=sys.stderr)
    try:
        while not stop_evt.is_set():
            line = s.readline().decode("ascii", errors="replace").strip()
            if not line.startswith("$RNG"):
                continue
            parts = line.split(",")
            if len(parts) < 12:
                continue
            try:
                frame_idx = int(parts[2])
                flags = int(parts[10], 0)
                d_mm = int(parts[6])
                slot = int(parts[5])
            except ValueError:
                continue

            if (flags & 0x04) != 0 or d_mm <= 0:
                continue

            out_q.put((tag_label(frame_idx), anchor_label(slot), d_mm / 1000.0, time.time()))
    except (OSError, serial.SerialException):
        pass
    finally:
        try:
            s.close()
        except Exception:
            pass


def tail_fanout(path, out_q, stop_evt):
    """Follow stream_ranges.py's fan-out file (tail -f style) instead of
    opening any serial port. Records are already fully parsed JSON, so this
    just re-derives (tag, anchor, range_m, ts) from each line."""
    f = None
    warned_missing = False
    while not stop_evt.is_set():
        if f is None:
            try:
                f = open(path, "r")
            except OSError:
                if not warned_missing:
                    print(f"# waiting for {path} -- is stream_ranges.py running?",
                          file=sys.stderr)
                    warned_missing = True
                time.sleep(1.0)
                continue
            print(f"# tailing {path}", file=sys.stderr)

        line = f.readline()
        if not line:
            time.sleep(0.1)
            continue
        try:
            rec = json.loads(line)
            out_q.put((rec["tag_label"], rec["anchor_label"], rec["range_m"], time.time()))
        except (json.JSONDecodeError, KeyError):
            continue


def render(samples, window):
    now = time.time()
    keys = sorted(samples.keys())

    lines = []
    lines.append(f"live range stats -- {window:.0f}s window, refresh {REFRESH_S:.0f}s"
                  f"  ({time.strftime('%H:%M:%S')})")
    lines.append("")
    header = f"{'TAG':<6}{'ANCHOR':<8}{'RATE(Hz)':>10}{'MEAN(m)':>10}{'N':>6}"
    lines.append(header)
    lines.append("-" * len(header))

    if not keys:
        lines.append("(no samples yet)")
    else:
        for k in keys:
            dq = samples[k]
            while dq and (now - dq[0][0]) > window:
                dq.popleft()
            vals = [v for (_, v) in dq]
            n = len(vals)
            if n == 0:
                continue
            rate = n / window
            mean = statistics.mean(vals)
            tag, anc = k
            lines.append(f"{tag:<6}{anc:<8}{rate:>10.2f}{mean:>10.3f}{n:>6d}")

    lines.append("")
    lines.append("Ctrl-C to quit")
    sys.stdout.write("\x1b[2J\x1b[H")
    sys.stdout.write("\n".join(lines) + "\n")
    sys.stdout.flush()


def main():
    explicit_ports, window, use_serial = parse_args()

    out_q: "queue.Queue" = queue.Queue()
    stop_evt = threading.Event()

    if use_serial:
        ports = find_ports(explicit_ports)
        threads = [
            threading.Thread(target=reader, args=(p, out_q, stop_evt), daemon=True)
            for p in ports
        ]
    else:
        threads = [
            threading.Thread(target=tail_fanout, args=(FANOUT_PATH, out_q, stop_evt), daemon=True)
        ]
    for t in threads:
        t.start()

    samples = defaultdict(deque)  # (tag,anchor) -> deque[(t, range_m)]
    next_render = time.time()

    try:
        while True:
            timeout = max(0.0, next_render - time.time())
            try:
                tag, anc, rng, ts = out_q.get(timeout=timeout)
                samples[(tag, anc)].append((ts, rng))
            except queue.Empty:
                pass

            if time.time() >= next_render:
                render(samples, window)
                next_render = time.time() + REFRESH_S
    except KeyboardInterrupt:
        pass
    finally:
        stop_evt.set()


if __name__ == "__main__":
    main()
