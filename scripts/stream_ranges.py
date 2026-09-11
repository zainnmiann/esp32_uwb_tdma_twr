#!/usr/bin/env python3
"""
Stream live UWB ranges in JSON format matching the fleet telemetry schema:
{"tag_label":"T02","anchor_label":"A01","range_m":5.40,"rssi":-69.0,"timestamp_sent":92349830,"timestamp_received":1788869810.876}

Reads every /dev/cu.usbserial-* port found (or an explicit list given on the
command line), one reader thread per port, and merges their $RNG lines into
one combined JSON stream on stdout. Only tags ever emit $RNG (anchors are
silent by protocol design), so pointing this at anchor ports too is harmless
-- those threads just never produce a record.

tag_label / anchor_label are derived from frame_index / slot -- the same
position-based labeling the OLED display uses (display.cpp) -- not from
node_id. node_id is now derived from each ESP32's factory MAC (see
radio_esp32.cpp) specifically to guarantee uniqueness across boards, so it is
an arbitrary 16-bit value with no pattern; a fixed node_id -> label table
(the previous implementation here) matches nothing on any board provisioned
after that change. frame_index and slot are both carried directly in every
$RNG line and are exactly how each board was provisioned ($SET,frame_index,N
/ $SET,slot_index,N), so deriving labels from them is always correct with no
separate mapping table to keep in sync.
"""

import sys
import glob
import time
import json
import queue
import threading
import serial

BAUD = 921600


def tag_label(frame_idx):
    return "T%02d" % (frame_idx + 1)


def anchor_label(slot):
    return "A%02d" % (slot + 1)


RESCAN_S = 2.0


def explicit_ports():
    """Ports pinned on the command line, or None to auto-discover/rescan."""
    explicit = [a for a in sys.argv[1:] if not a.startswith("-")]
    return explicit or None


def candidate_ports():
    fixed = explicit_ports()
    if fixed is not None:
        return fixed
    return sorted(glob.glob("/dev/cu.usbserial-*"))


def reader(port, out_q, stop_evt):
    # One attempt per call -- a board that isn't reachable right now (mid
    # reset from a $RESET, or physically not plugged in yet) is not retried
    # in a loop here. Instead the caller's periodic rescan notices this
    # thread died and starts a fresh one for the same port later, which
    # naturally covers both "wasn't up yet at open time" and "got unplugged
    # mid-run, then plugged back in" the same way.
    try:
        s = serial.Serial(port, BAUD, timeout=1, dsrdtr=False, rtscts=False)
    except Exception as e:
        print(f"# {port}: open failed: {e}", file=sys.stderr)
        return

    # These boards' auto-reset circuit (DTR/RTS -> EN/GPIO0) can leave the
    # ESP32 held in reset or the ROM bootloader depending on whatever
    # DTR/RTS state was left behind by whoever last had this port open --
    # observed directly: a board that was already running fine went
    # silent the moment a previous process holding this same port was
    # killed, and stayed silent until explicitly reset. A clean
    # reset-and-release on every open makes this deterministic instead of
    # depending on leftover line state.
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
            # $RNG,sfn,frame_idx,tag_id,anchor_id,slot,d_mm,tof_ps,rxp,fpp,flags,t_local_us*CS
            if len(parts) < 12:
                continue
            try:
                frame_idx = int(parts[2])
                flags = int(parts[10], 0)
                d_mm = int(parts[6])
                slot = int(parts[5])
            except ValueError:
                continue

            # RESP_LOST (bit2/0x04) means no anchor response at all -- not a
            # measurement. SS-TWR fallback (bit0/0x01) is still a real range,
            # just lower accuracy, so it stays in.
            if (flags & 0x04) != 0 or d_mm <= 0:
                continue

            try:
                rxp = round(float(parts[8]), 1)
            except ValueError:
                rxp = 0.0

            rec = {
                "tag_label": tag_label(frame_idx),
                "anchor_label": anchor_label(slot),
                "range_m": round(d_mm / 1000.0, 2),
                "rssi": rxp,
                "timestamp_sent": int(parts[11].split("*")[0]),
                "timestamp_received": round(time.time(), 3),
            }
            out_q.put(rec)
    except (OSError, serial.SerialException):
        pass
    finally:
        try:
            s.close()
        except Exception:
            pass


FANOUT_PATH = "/tmp/uwb_ranges.jsonl"


def main():
    out_q: "queue.Queue" = queue.Queue()
    stop_evt = threading.Event()
    live: "dict[str, threading.Thread]" = {}

    def rescan():
        for p in candidate_ports():
            th = live.get(p)
            if th is None or not th.is_alive():
                t = threading.Thread(target=reader, args=(p, out_q, stop_evt), daemon=True)
                live[p] = t
                t.start()

    rescan()

    # macOS holds a serial device open for exactly one process. Any other
    # tool that wants this same data (range_table.py) cannot also open these
    # ports -- it would silently steal a subset of them instead of sharing.
    # So this process, as the one that owns the ports, also fans every
    # record out to a well-known file that other local tools can tail
    # instead of touching the serial devices themselves.
    try:
        fanout = open(FANOUT_PATH, "w", buffering=1)
        print(f"# fanout: {FANOUT_PATH}", file=sys.stderr)
    except OSError as e:
        fanout = None
        print(f"# fanout disabled: {e}", file=sys.stderr)

    next_rescan = time.time() + RESCAN_S
    try:
        while True:
            timeout = max(0.0, next_rescan - time.time())
            try:
                rec = out_q.get(timeout=timeout)
                line = json.dumps(rec, separators=(",", ":"))
                print(line, flush=True)
                if fanout is not None:
                    try:
                        fanout.write(line + "\n")
                    except OSError:
                        pass
            except queue.Empty:
                pass

            if time.time() >= next_rescan:
                # Picks up a board plugged in after startup, and retries any
                # port whose reader thread died (open failed, or the board
                # was unplugged mid-run) -- see the comment in reader().
                rescan()
                next_rescan = time.time() + RESCAN_S
    except (KeyboardInterrupt, BrokenPipeError):
        pass
    finally:
        stop_evt.set()
        if fanout is not None:
            fanout.close()


if __name__ == "__main__":
    main()
