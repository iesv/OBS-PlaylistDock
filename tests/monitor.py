"""Record a virtual WASAPI endpoint while the native continuity test switches media.

Requires SoundCard and NumPy. Uses only the isolated test process's monitor setting.
"""
import argparse
from collections import deque
import os
from pathlib import Path
import subprocess
import threading
import time

import numpy as np
import soundcard as sc

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--executable", type=Path, required=True)
parser.add_argument("--obs", type=Path, required=True)
inputs = parser.add_mutually_exclusive_group(required=True)
inputs.add_argument("--media", type=Path)
inputs.add_argument("--files", nargs="+", type=Path)
parser.add_argument("--device", default="CABLE Input (VB-Audio Virtual Cable)")
parser.add_argument("--native", action="store_true")
args = parser.parse_args()
speaker = next(d for d in sc.all_speakers() if d.name == args.device)
microphone = sc.get_microphone(speaker.id, include_loopback=True)
blocks = deque(maxlen=2000)
lock = threading.Lock()
ready = threading.Event()
finished = threading.Event()
errors = []


def record():
    try:
        with microphone.recorder(samplerate=48000, blocksize=480) as recorder:
            ready.set()
            while not finished.is_set():
                data = recorder.record(numframes=480)
                values = np.max(np.abs(data), axis=1)
                windows = values[:len(values) // 48 * 48].reshape(-1, 48)
                audible = int(np.count_nonzero(np.max(windows, axis=1) > 0.00001)) * 48
                with lock:
                    blocks.append((time.monotonic(), len(values), audible, float(np.max(values))))
    except Exception as error:
        errors.append(error)
        ready.set()


thread = threading.Thread(target=record, daemon=True)
thread.start()
assert ready.wait(10), "WASAPI recorder did not start"
assert not errors, errors
env = os.environ.copy()
env["PLAYLISTDOCK_MONITOR_DEVICE"] = args.device
files = list(map(str, args.files)) if args.files else sorted(str(p) for p in args.media.iterdir() if p.is_file())
mode = "--native-continuity" if args.native else "--mixed-real" if args.files else "--continuity"
command = [str(args.executable), mode, str(args.obs / "obs-plugins/64bit/vlc-video.dll"), str(args.obs / "data/obs-plugins/vlc-video"), *files]
checks = 0
failures = 0
try:
    with subprocess.Popen(command, env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, encoding="utf-8", errors="replace") as process:
        for line in process.stdout:
            print(line, end="", flush=True)
            if not line.startswith(("AUDIO_CONTINUITY", "MIXED_AUDIO")):
                continue
            now = time.monotonic()
            with lock:
                samples = [block for block in blocks if now - 2.5 < block[0] < now - 0.1]
            frames = sum(b[1] for b in samples)
            audible = sum(b[2] for b in samples)
            peak = max((b[3] for b in samples), default=0)
            coverage = frames / (2.4 * 48000)
            # Measure missing millisecond windows, allowing ordinary zero crossings.
            audible_ratio = audible / max(frames, 1)
            passed = coverage > 0.95 and audible_ratio > 0.95 and not errors
            print(f"MONITOR_LOOPBACK check={checks+1} coverage={coverage:.4f} audible_ratio={audible_ratio:.4f} peak={peak:.6f} {'PASS' if passed else 'FAIL'}", flush=True)
            checks += 1
            failures += not passed
        exit_code = process.wait()
finally:
    finished.set()
    thread.join(timeout=5)
assert not errors, errors
assert exit_code == 0 and checks >= 8 and failures == 0, (exit_code, checks, failures)
print(f"MONITOR_LOOPBACK checks={checks} actual_wasapi_output=PASS")
