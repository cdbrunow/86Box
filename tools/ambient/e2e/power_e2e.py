#!/usr/bin/env python3
"""Power-off end to end: fan + MFM disk (WDL-330P) + ATAPI CD-ROM, then soft-off.
Also checks HDD short vs long seeks (driven with WD1003 READs)."""
import json, os, socket, struct, subprocess, sys, time, wave
import numpy as np
exe, roms, assets, work = sys.argv[1:5]
os.makedirs(work, exist_ok=True); rec = os.path.join(work, "rec"); os.makedirs(rec, exist_ok=True)
sock_path = os.path.join(work, "86box.sock")
with open(os.path.join(work, "hd.img"), "wb") as f: f.truncate(615 * 4 * 17 * 512)
dd = os.path.join(work, "datadisc"); os.makedirs(dd, exist_ok=True)
open(os.path.join(dd, "a.bin"), "wb").write(os.urandom(4 << 20))
with open(os.path.join(work, "86box.cfg"), "w") as f:
    f.write("[Machine]\nmachine = ibmat\nsystem_fan_audio_profile = IBM_PS1_2121\n\n"
            "[Storage controllers]\nhdc_1 = st506_at\ncdrom_interface = mcd\n\n"
            "[Hard disks]\nhdd_01_parameters = 17, 4, 615, 0, mfm\nhdd_01_fn = hd.img\n"
            "hdd_01_mfm_channel = 0\nhdd_01_speed = mfm_3600rpm\nhdd_01_audio = IBM_WDL330P_3600RPM\n\n"
            "[Floppy and CD-ROM drives]\ncdrom_01_parameters = 1, mitsumi\n"
            "cdrom_01_audio = GENERIC_ATAPI_IBMULATOR\n")
proc = subprocess.Popen([exe, "--automation", "unix:" + sock_path, "--automation-hold", "--rompath", roms,
                         "--assetpath", assets, "--vmpath", work, "--logfile", os.path.join(work, "86box.log")],
                        env=dict(os.environ, QT_QPA_PLATFORM="offscreen"), stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
for _ in range(200):
    if os.path.exists(sock_path): break
    time.sleep(0.1)
s = socket.socket(socket.AF_UNIX); s.connect(sock_path); fh = s.makefile("rw")
def cmd(c):
    fh.write(c + "\n"); fh.flush(); line = fh.readline()
    return json.loads(line) if line else {"ok": False}
def step(ms, paced=True): cmd(f"step {ms}" + (" paced" if paced else ""))
def st(port): return cmd(f"ior {port} 1")["values"][0]

def mfm_read(cyl):            # WD1003 READ SECTORS, 1 sector at cyl/head 0/sector 1
    cmd("iow 0x3f6 1 0x02")
    for port, val in [(0x1f2, 1), (0x1f3, 1), (0x1f4, cyl & 0xff), (0x1f5, cyl >> 8), (0x1f6, 0xa0), (0x1f7, 0x20)]:
        cmd(f"iow {port} 1 {val}")
    for _ in range(500):
        if not st(0x1f7) & 0x80: break
        step(1, False)
    if st(0x1f7) & 0x08:
        for _ in range(256): cmd("ior 0x1f0 2")
    return st(0x1f7)

BASE = 0x310
def m_flags(): return cmd(f"ior {BASE + 1} 1")["values"][0]
def m_cmd(op, args=(), drain=0):
    cmd(f"iow {BASE} 1 {op}")
    for a in args: cmd(f"iow {BASE} 1 {a}")
    step(5, False)
    for _ in range(400):
        if (m_flags() & 0x06) != 0x06: break
        step(2, False)
    got = 0
    while got < drain and not (m_flags() & 0x02):
        cmd(f"ior {BASE} 1"); got += 1
    out = []
    while not (m_flags() & 0x04):
        out.append(cmd(f"ior {BASE} 1")["values"][0])
    return out
def cd_read16():
    for attempt in range(50):
        r = m_cmd(0xc1, (0, 2, 0x16, 0, 0, 1), drain=2048)
        if r and not (r[0] & 0x01): return f"ok after {attempt} retries"
        step(100, False)
    return "failed"

cmd("audio start " + rec)
step(12000)                                   # fan + HDD spin-up (HDD: 10.9 s)
print("mfm reads:", [hex(mfm_read(c)) for c in (0,)])
step(300)
for c, what in [(10, "short"), (400, "long"), (380, "short"), (20, "long")]:
    r = mfm_read(c); print(f"  HDD read cyl {c} ({what}) status {r:#x}"); step(400)
cmd(f"cd load 0 {dd}"); step(3500)
print("cd read:", cd_read16()); step(3000)
cmd("release"); t = time.monotonic(); cmd("quit soft")
try: proc.wait(timeout=20)
except subprocess.TimeoutExpired: proc.kill()
print(f"quit soft -> exit after {time.monotonic() - t:.2f} s (rc {proc.returncode})")

L = lambda p: (lambda w: np.frombuffer(w.readframes(w.getnframes()), "<i2").reshape(-1, w.getnchannels())[:, 0].astype(float))(wave.open(p))
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
C = os.path.join(assets, "sounds/cdrom/Generic ATAPI (IBMulator)/GENERIC_ATAPI_")
hdd, amb = L(os.path.join(rec, "hdd.wav")), L(os.path.join(rec, "ambient.wav"))
print(f"\nhdd.wav {len(hdd)/48000:.1f} s, ambient.wav {len(amb)/48000:.1f} s")
for label, sig, path, sl, thr in [
        ("HDD spin-up", hdd, H + "SPINUP.wav", slice(0, 96000), 0.95),
        ("HDD seek (short)", hdd, H + "SEEK_1TRACK.wav", slice(None), 0.9),
        ("HDD seek (long)", hdd, H + "SEEK_LONG.wav", slice(None), 0.9),
        ("HDD spin-down", hdd, H + "SPINDOWN.wav", slice(0, 96000), 0.9),
        ("fan power-down", amb, F + "POWER_DOWN.wav", slice(0, 96000), 0.9),
        ("CD spin_start", amb, C + "SPIN_START.wav", slice(-38400, None), 0.9),
        ("CD spin_stop", amb, C + "SPIN_STOP.wav", slice(0, 48000), 0.6)]:
    t = L(path)[sl]
    print(f"{label:17s}: " + (", ".join(f"{i/48000:.2f}s r={r:.2f}" for i, r in find(sig, t, thr)) or "-"))
