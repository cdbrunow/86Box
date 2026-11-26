#!/usr/bin/env python3
"""Power switch end to end.

A: automation "power off" / "power on" with fan + MFM disk: spin-downs play
   with the window open, step is refused while off, power on cold-boots.
B: "Keep window open when powered off" set: a guest power-off ("quit soft")
   leaves 86Box running; "quit" still exits.

usage: python3 -I switch_e2e.py <86Box> <roms> <assets> <workdir>
"""
import json, os, socket, subprocess, sys, time, wave
import numpy as np

exe, roms, assets, work = sys.argv[1:5]

def launch(name, keep_window):
    d = os.path.join(work, name); os.makedirs(os.path.join(d, "rec"), exist_ok=True)
    os.makedirs(os.path.join(d, "xdg", "86Box"), exist_ok=True)
    with open(os.path.join(d, "xdg", "86Box", "86box_global.cfg"), "w") as f:
        f.write("[Emulator]\npower_off_keep_window = %d\n" % keep_window)
    with open(os.path.join(d, "hd.img"), "wb") as f: f.truncate(615 * 4 * 17 * 512)
    with open(os.path.join(d, "86box.cfg"), "w") as f:
        f.write("[Machine]\nmachine = ibmat\nsystem_fan_audio_profile = IBM_PS1_2121\n\n"
                "[Storage controllers]\nhdc_1 = st506_at\n\n"
                "[Hard disks]\nhdd_01_parameters = 17, 4, 615, 0, mfm\nhdd_01_fn = hd.img\n"
                "hdd_01_mfm_channel = 0\nhdd_01_speed = mfm_3600rpm\nhdd_01_audio = IBM_WDL330P_3600RPM\n")
    sock = os.path.join(d, "s.sock")
    env = dict(os.environ, QT_QPA_PLATFORM="offscreen", XDG_CONFIG_HOME=os.path.join(d, "xdg"))
    p = subprocess.Popen([exe, "--automation", "unix:" + sock, "--automation-hold", "--rompath", roms,
                          "--assetpath", assets, "--vmpath", d], env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    for _ in range(200):
        if os.path.exists(sock): break
        time.sleep(0.1)
    s = socket.socket(socket.AF_UNIX); s.connect(sock); fh = s.makefile("rw")
    def cmd(c):
        fh.write(c + "\n"); fh.flush(); line = fh.readline()
        return json.loads(line) if line else {"ok": False, "error": "connection closed"}
    return d, p, cmd

L = lambda path: (lambda w: np.frombuffer(w.readframes(w.getnframes()), "<i2").reshape(-1, w.getnchannels())[:, 0].astype(float))(wave.open(path))
def find(sig, t, thr):
    n = len(t); m = len(sig)
    if m < n: return []
    size = 1 << int(np.ceil(np.log2(m + n)))
    corr = np.fft.irfft(np.fft.rfft(sig, size) * np.conj(np.fft.rfft(t, size)), size)[:m - n + 1]
    c2 = np.concatenate([[0], np.cumsum(sig ** 2)]); ncc = corr / np.sqrt(np.maximum(c2[n:] - c2[:-n], 1e-9) * np.sum(t ** 2))
    hits = []
    for i in np.argsort(-ncc)[:20000]:
        if ncc[i] < thr: break
        if all(abs(i - h) > n // 2 for h, _ in hits): hits.append((int(i), float(ncc[i])))
    return sorted(hits)
H = os.path.join(assets, "sounds/hdd/1990 IBM WDL-330P/1990_IBM_WDL-330P_3600RPM_")
F = os.path.join(assets, "sounds/system/IBM PS1 2121/IBM_PS1_2121_")

# ---- A: the switch, from automation
print("== A: power off / power on")
d, p, cmd = launch("A", 0)
cmd("audio start " + os.path.join(d, "rec"))
cmd("step 12000 paced")
r = cmd("power off"); print("power off:", r.get("ok"))
st = cmd("status"); print("status: paused", st.get("paused"), "| step while off:", cmd("step 100").get("error"))
time.sleep(5.0)                                 # spin-downs play with emulation stopped
print("still running after 5 s:", p.poll() is None)
r = cmd("power on"); print("power on:", r.get("ok"))
cmd("step 12000 paced")
cmd("audio stop"); cmd("quit")
try: p.wait(timeout=20)
except subprocess.TimeoutExpired: p.kill()
hdd, amb = L(os.path.join(d, "rec", "hdd.wav")), L(os.path.join(d, "rec", "ambient.wav"))
for label, sig, path, sl, thr in [("fan power-up", amb, F + "POWER_UP.wav", slice(0, 96000), 0.9),
                                  ("fan power-down", amb, F + "POWER_DOWN.wav", slice(0, 96000), 0.9),
                                  ("HDD spin-up", hdd, H + "SPINUP.wav", slice(0, 96000), 0.95),
                                  ("HDD spin-down", hdd, H + "SPINDOWN.wav", slice(0, 96000), 0.9)]:
    print(f"  {label:15s}: " + (", ".join(f"{i/48000:.2f}s" for i, _ in find(sig, L(path)[sl], thr)) or "-"))

# ---- B: guest power-off with "keep window open"
print("== B: keep window open, guest power-off")
d, p, cmd = launch("B", 1)
cmd("step 3000 paced"); cmd("release")
cmd("quit soft")
time.sleep(4.0)
print("86Box still running after quit soft:", p.poll() is None)
st = cmd("status"); print("status reply:", "ok" if st.get("ok") else st, "| paused", st.get("paused"))
cmd("power on"); r = cmd("step 1000"); print("power on, then step:", r.get("ok"))
t = time.monotonic(); cmd("quit")
try: p.wait(timeout=20); print(f"quit exits: rc {p.returncode} after {time.monotonic() - t:.2f} s")
except subprocess.TimeoutExpired: p.kill(); print("quit did NOT exit")
