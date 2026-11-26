#!/usr/bin/env python3
"""End-to-end fan test: drive 86Box over its automation socket, record the
ambient stream, then locate the real samples in the recording.

usage: python3 -I fan_e2e.py <86Box binary> <romdir> <assetdir> <workdir>
"""
import json, os, socket, subprocess, sys, time, wave
import numpy as np

exe, roms, assets, work = sys.argv[1:5]
os.makedirs(work, exist_ok=True)
rec = os.path.join(work, "rec"); os.makedirs(rec, exist_ok=True)
sock_path = os.path.join(work, "86box.sock")
with open(os.path.join(work, "86box.cfg"), "w") as f:
    f.write("[Machine]\nmachine = ibmat\nsystem_fan_audio_profile = IBM_PS1_2121\n")
if os.path.exists(sock_path):
    os.unlink(sock_path)

env = dict(os.environ, QT_QPA_PLATFORM="offscreen")
proc = subprocess.Popen([exe, "--automation", "unix:" + sock_path, "--automation-hold",
                         "--rompath", roms, "--assetpath", assets, "--vmpath", work,
                         "--logfile", os.path.join(work, "86box.log")],
                        env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
for _ in range(200):
    if os.path.exists(sock_path):
        break
    time.sleep(0.1)
s = socket.socket(socket.AF_UNIX); s.connect(sock_path); fh = s.makefile("rw")

t0 = time.monotonic()
log = []
def cmd(c):
    fh.write(c + "\n"); fh.flush()
    line = fh.readline()
    r = json.loads(line) if line else {"ok": False, "error": "closed"}
    log.append((round(time.monotonic() - t0, 2), c, r.get("ok"), r.get("ms")))
    return r

cmd("audio start " + rec)
cmd("step 6000 paced")            # power-on: spin-up (3.66 s) then loop
time.sleep(2.0)                   # held 2 s: emulation not running
cmd("step 2000 paced")            # resume
cmd("reset cad")                  # warm reset: fan must not restart
cmd("step 3000 paced")
cmd("reset hard")                 # cold: spin-up again
cmd("step 5000 paced")
cmd("release")
t_off = time.monotonic() - t0
cmd("quit soft")                  # IBM AT has no power button -> plat_power_off()
try:
    proc.wait(timeout=20)
except subprocess.TimeoutExpired:
    proc.kill()
t_exit = time.monotonic() - t0
print("commands:"); [print("  ", *l) for l in log]
print(f"quit soft at {t_off:.2f} s, process exited {t_exit - t_off:.2f} s later (rc {proc.returncode})")

# ---- analysis
path = os.path.join(rec, "ambient.wav")
raw = open(path, "rb").read()
data = np.frombuffer(raw[44:len(raw) - (len(raw) - 44) % 4], "<i2").reshape(-1, 2)[:, 0].astype(float)
print(f"ambient.wav: {len(data) / 48000:.2f} s recorded")

def load(n):
    w = wave.open(os.path.join(assets, "sounds/system/IBM PS1 2121", n))
    return np.frombuffer(w.readframes(w.getnframes()), "<i2").reshape(-1, 2)[:, 0].astype(float)

def find(tmpl, sig, thresh=0.95):
    """Start offsets where tmpl matches sig (normalized cross-correlation)."""
    n = len(tmpl); m = len(sig)
    if m < n: return []
    size = 1 << int(np.ceil(np.log2(m + n)))
    corr = np.fft.irfft(np.fft.rfft(sig, size) * np.conj(np.fft.rfft(tmpl, size)), size)[:m - n + 1]
    c2 = np.concatenate([[0], np.cumsum(sig ** 2)])
    energy = np.sqrt(np.maximum(c2[n:] - c2[:-n], 1e-9) * np.sum(tmpl ** 2))
    ncc = corr / energy
    hits = []
    for i in np.argsort(-ncc):
        if ncc[i] < thresh: break
        if all(abs(i - h) > n // 2 for h, _ in hits): hits.append((int(i), float(ncc[i])))
    return sorted(hits)

up, lp, dn = load("IBM_PS1_2121_POWER_UP.wav"), load("IBM_PS1_2121_POWER_ON.wav"), load("IBM_PS1_2121_POWER_DOWN.wav")
for name, t in [("spin-up (first 1 s)", up[:48000]), ("loop (first 0.5 s)", lp[:24000]), ("spin-down (first 1 s)", dn[:48000])]:
    hits = find(t, data)
    print(f"{name}: " + ", ".join(f"{h / 48000:.2f}s (r={r:.3f})" for h, r in hits))
blk = 960
env = [20 * np.log10(max(np.sqrt(np.mean(data[i:i + blk] ** 2)), 1e-3) / 32768) for i in range(0, len(data), blk)]
print("tail level (dBFS per 20 ms):", [round(x) for x in env[-30:]])
