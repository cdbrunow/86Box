#!/usr/bin/env python3
"""End-to-end CD-ROM sound test (Mitsumi interface) over the automation socket.

Drives a Mitsumi CD-ROM (interface at 0x310) directly with port I/O, records the
ambient stream, then finds each real sample in the recording, in order.

usage: python3 -I cd_mitsumi_e2e.py <86Box> <romdir> <assetdir> <workdir>
"""
import json, os, socket, struct, subprocess, sys, time, wave
import numpy as np

exe, roms, assets, work = sys.argv[1:5]
os.makedirs(work, exist_ok=True)
rec = os.path.join(work, "rec"); os.makedirs(rec, exist_ok=True)
sock_path = os.path.join(work, "86box.sock")

# --- media: a data folder (mounted as an ISO) and an audio CUE/BIN
data_dir = os.path.join(work, "datadisc"); os.makedirs(data_dir, exist_ok=True)
with open(os.path.join(data_dir, "big.bin"), "wb") as f:
    f.write(os.urandom(30 * 1024 * 1024))
t = np.arange(44100 * 60) / 44100.0
tone = (np.sin(2 * np.pi * 440 * t) * 8000).astype("<i2")
with open(os.path.join(work, "tone.bin"), "wb") as f:
    f.write(np.repeat(tone, 2).tobytes())
with open(os.path.join(work, "tone.cue"), "w") as f:
    f.write('FILE "tone.bin" BINARY\n  TRACK 01 AUDIO\n    INDEX 01 00:00:00\n')

with open(os.path.join(work, "86box.cfg"), "w") as f:
    f.write("[Machine]\nmachine = ibmat\n\n"
            "[Storage controllers]\ncdrom_interface = mcd\n\n"
            "[Floppy and CD-ROM drives]\ncdrom_01_parameters = 1, mitsumi\n"
            "cdrom_01_audio = GENERIC_ATAPI_IBMULATOR\n")
if os.path.exists(sock_path):
    os.unlink(sock_path)

proc = subprocess.Popen([exe, "--automation", "unix:" + sock_path, "--automation-hold",
                         "--rompath", roms, "--assetpath", assets, "--vmpath", work,
                         "--logfile", os.path.join(work, "86box.log")],
                        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
for _ in range(200):
    if os.path.exists(sock_path): break
    time.sleep(0.1)
s = socket.socket(socket.AF_UNIX); s.connect(sock_path); fh = s.makefile("rw")

def cmd(c):
    fh.write(c + "\n"); fh.flush()
    r = json.loads(fh.readline())
    if not r.get("ok"):
        print("  !!", c, r)
    return r

def step(ms, paced=True):
    cmd(f"step {ms}" + (" paced" if paced else ""))

def status():
    return cmd("ior 0x1f7 1")["values"][0]

def wait_not_busy(limit=2000):
    for _ in range(limit):
        st = status()
        if not st & 0x80:
            return st
        step(1, False)
    raise RuntimeError("drive stayed busy")

def packet(cdb, read_bytes=0):
    """Send a 12-byte ATAPI packet; read and discard `read_bytes` of data."""
    cdb = bytes(cdb) + bytes(12 - len(cdb))
    cmd("iow 0x3f6 1 0x02")                  # nIEN: no interrupts
    cmd("iow 0x1f6 1 0xb0")                  # select the slave
    wait_not_busy()
    cmd("iow 0x1f1 1 0")                     # PIO
    cmd("iow 0x1f4 1 0x00"); cmd("iow 0x1f5 1 0x08")  # byte count 2048
    cmd("iow 0x1f7 1 0xa0")                  # PACKET
    for _ in range(2000):
        st = status()
        if (st & 0x88) == 0x08: break
        step(1, False)
    for w in struct.unpack("<6H", cdb):
        cmd(f"iow 0x1f0 2 {w}")
    got = 0
    for _ in range(4000):
        st = wait_not_busy()
        if st & 0x08 and got < read_bytes:   # data ready: drain it word by word
            n = cmd("ior 0x1f4 1")["values"][0] | (cmd("ior 0x1f5 1")["values"][0] << 8)
            for _ in range(n // 2):
                cmd("ior 0x1f0 2")
            got += n
            continue
        break
    return status()

def packet_retry(cdb, read_bytes=0):
    """Clear a UNIT ATTENTION (e.g. after a disc change) and retry once."""
    st = packet(cdb, read_bytes)
    if st & 0x01:
        packet([0x03, 0, 0, 0, 18, 0], 18)   # REQUEST SENSE
        st = packet(cdb, read_bytes)
    if st & 0x01:
        print("  !! packet failed:", bytes(cdb[:1]).hex(), "status", hex(st))
    return st

def read10(lba):   return packet_retry([0x28, 0, *lba.to_bytes(4, "big"), 0, 0, 1, 0], 2048)
def seek10(lba):   return packet_retry([0x2B, 0, *lba.to_bytes(4, "big")])
def start_stop(b4): return packet_retry([0x1B, 0, 0, 0, b4, 0])
def play_msf(m0, s0, f0, m1, s1, f1): return packet_retry([0x47, 0, 0, m0, s0, f0, m1, s1, f1, 0])
def pause(resume): return packet_retry([0x4B, 0, 0, 0, 0, 0, 0, 0, resume, 0])

events = []

BASE = 0x310
def m_flags(): return cmd(f"ior {BASE + 1} 1")["values"][0]
def m_cmd(op, args=(), drain=0):
    cmd(f"iow {BASE} 1 {op}")
    for a in args: cmd(f"iow {BASE} 1 {a}")
    step(5, False)
    for _ in range(400):                       # wait for status or data
        if (m_flags() & 0x06) != 0x06: break   # NOSTAT|NODATA both set = nothing yet
        step(2, False)
    got = 0
    while got < drain and not (m_flags() & 0x02):  # data available (NODATA clear)
        cmd(f"ior {BASE} 1"); got += 1
    st = []
    while not (m_flags() & 0x04):               # status bytes (NOSTAT clear)
        st.append(cmd(f"ior {BASE} 1")["values"][0])
    return st
def bcd(x): return ((x // 10) << 4) | (x % 10)
def m_read(lba):
    a = lba + 150
    args = (bcd(a // 4500), bcd(a // 75 % 60), bcd(a % 75), 0, 0, 1)
    st = m_cmd(0xc1, args, drain=2048)
    print(f"    read {lba}: status {st[:1]}")
    if st and (st[0] & 0x01):                  # command check (e.g. disc changed): retry once
        st = m_cmd(0xc1, args, drain=2048)
    return st

def ev(name):
    events.append(name); print("event:", name)

cmd("audio start " + rec)
step(1500)
ev("host load data disc -> open_tray, close_tray"); cmd(f"cd load 0 {data_dir}"); step(3500)
print("  stat:", m_cmd(0x40), m_cmd(0x40))
ev("READ 16 until accepted (Mitsumi refuses reads for a while after a disc change) -> spin_start")
for attempt in range(50):
    st = m_cmd(0xc1, (0, 2, 0x16, 0, 0, 1), drain=2048)
    if st and not (st[0] & 0x01):
        print(f"    accepted after {attempt} retries"); break
    step(100, False)
step(3000)
ev("READ 17 -> nothing"); m_read(17); step(500)
ev("READ 100 -> seek_step"); m_read(100); step(800)
ev("READ 12000 -> seek_out"); m_read(12000); step(800)
ev("READ 50 -> seek_in"); m_read(50); step(800)
ev("idle 61 s -> spin_stop"); step(61000, False); step(2500)
ev("guest EJECT -> open_tray"); print("  ", m_cmd(0xf6)); step(2500)
ev("guest CLOSE -> close_tray"); print("  ", m_cmd(0xf8)); step(3000)
r = cmd("audio stop")
cmd("release"); cmd("quit")
try: proc.wait(timeout=20)
except subprocess.TimeoutExpired: proc.kill()

# ---- analysis: find each sample in the recording, report in time order
def mono(path, skip_hdr=True):
    w = wave.open(path)
    d = np.frombuffer(w.readframes(w.getnframes()), "<i2").reshape(-1, w.getnchannels())
    return d[:, 0].astype(float), w.getframerate()

data, rate = mono(os.path.join(rec, "ambient.wav"))
print(f"\nambient.wav: {len(data) / rate:.1f} s")

def find(tmpl, sig, thresh):
    n = len(tmpl); m = len(sig); size = 1 << int(np.ceil(np.log2(m + n)))
    corr = np.fft.irfft(np.fft.rfft(sig, size) * np.conj(np.fft.rfft(tmpl, size)), size)[:m - n + 1]
    c2 = np.concatenate([[0], np.cumsum(sig ** 2)])
    ncc = corr / np.sqrt(np.maximum(c2[n:] - c2[:-n], 1e-9) * np.sum(tmpl ** 2))
    hits = []
    for i in np.argsort(-ncc)[:5000]:
        if ncc[i] < thresh: break
        if all(abs(i - h) > n // 2 for h, _ in hits): hits.append((int(i), float(ncc[i])))
    return hits

D = os.path.join(assets, "sounds/cdrom/Generic ATAPI (IBMulator)/GENERIC_ATAPI_")
found = []
for name, frames, thr in [("spin_start", 48000, 0.9), ("spin_stop", 48000, 0.9), ("seek_step", 0, 0.8),
                          ("seek_out", 0, 0.8), ("seek_in", 0, 0.8), ("open_tray", 48000, 0.9),
                          ("close_tray", 48000, 0.9)]:
    t, _ = mono(D + name.upper() + ".wav")
    t = t[:frames] if frames else t
    for pos, r in find(t, data, thr):
        found.append((pos / rate, name, r))
print("\nsounds found, in order:")
for tm, name, r in sorted(found):
    print(f"  {tm:7.2f} s  {name:11s} r={r:.3f}")
cd, crate = mono(os.path.join(rec, "cd.wav")) if os.path.exists(os.path.join(rec, "cd.wav")) else (np.zeros(1), 1)
print(f"\ncd.wav (Red Book audio): {len(cd) / crate:.1f} s, peak {int(np.abs(cd).max())}")
