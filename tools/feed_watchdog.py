# Watchdog for the live MT5 feed.
#
# The feed (tools/mt5_feed.py) occasionally hangs in its polling loop and stops
# appending closed H1 bars, so the bot silently sees no new bars. This watcher
# restarts the feed if the bar file has not grown for a while (feedStallMs),
# or if the feed process died. It keeps only one feed alive at a time.
#
# Run detached alongside the forward session:
#   python -X utf8 tools\feed_watchdog.py --feed build\mt5_bars.jsonl \
#       --mark build\forward\watchdog.pid --log build\forward\watchdog.log
#
#   (needs a small batch to run detached; see run_forward_live.ps1)

import argparse
import json
import os
import subprocess
import sys
import time

FEED_CMD = [
    sys.executable, "-X", "utf8",
    os.path.join(os.path.dirname(__file__), "mt5_feed.py"),
    "--tf", "H1",
]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--feed", required=True, help="bar jsonl file")
    ap.add_argument("--out", required=True, help="feed output path (same as --feed)")
    ap.add_argument("--mark", required=True, help="pid marker file")
    ap.add_argument("--log", required=True)
    ap.add_argument("--stall-sec", type=float, default=1800.0, help="restart if no growth")
    ap.add_argument("--poll-sec", type=float, default=60.0)
    a = ap.parse_args()

    feed = os.path.abspath(a.feed)
    marker = os.path.abspath(a.mark)

    def log(msg):
        with open(a.log, "a", encoding="utf-8") as f:
            f.write(f"{time.strftime('%Y-%m-%d %H:%M:%S')} {msg}\n")

    def start_feed():
        proc = subprocess.Popen(FEED_CMD + ["--out", a.out], stdout=subprocess.DEVNULL,
                                stderr=subprocess.DEVNULL, creationflags=getattr(subprocess, "DETACHED_PROCESS", 0))
        with open(marker, "w") as f:
            f.write(str(proc.pid) + "\n")
        log(f"started feed pid={proc.pid}")
        return proc

    with open(marker, "a", encoding="utf-8") as f:
        pass  # ensure marker exists

    proc = None
    last_size = -1
    last_change = time.time()
    while True:
        try:
            size = os.path.getsize(feed) if os.path.exists(feed) else -1
        except OSError:
            size = -1
        if size != last_size:
            last_size = size
            last_change = time.time()
        # feed must be running
        if proc is not None and proc.poll() is not None:
            log("feed died, restarting")
            proc = start_feed()
            last_change = time.time()
        elif proc is None:
            proc = start_feed()
            last_change = time.time()
        # feed must be writing
        elif time.time() - last_change > a.stall_sec:
            log(f"feed stalled ({a.stall_sec}s no growth); restarting")
            try:
                proc.kill()
            except Exception:
                pass
            proc = start_feed()
            last_change = time.time()
        else:
            pass
        time.sleep(a.poll_sec)


if __name__ == "__main__":
    main()