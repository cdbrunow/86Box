#!/usr/bin/env python3
"""End-to-end CD-ROM sound test over 86Box's automation socket.

Drives an ATAPI CD-ROM (primary slave) directly with port I/O, records the
ambient stream, then finds each real sample in the recording, in order.

usage: python3 -I cd_atapi_e2e.py <86Box> <romdir> <assetdir> <workdir>
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
            "[Storage controllers]\nhdc_1 = ide_isa\n\n"
            "[Floppy and CD-ROM drives]\ncdrom_01_parameters = 1, atapi\n"
            "cdrom_01_ide_channel = 0:1\ncdrom_01_audio = GENERIC_ATAPI_IBMULATOR\n")
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
def ev(name):
    events.append(name); print("event:", name)

cmd("audio start " + rec)
step(1500)                                             # BIOS POST
ev("host load data disc -> close_tray")
cmd(f"cd load 0 {data_dir}"); step(2500); packet([0x00]*6); packet([0x03, 0, 0, 0, 18, 0], 18)
ev("READ 16 -> spin_start, spin"); read10(16); step(3000)
ev("READ 17 (sequential) -> nothing"); read10(17); step(500)
ev("READ 100 (83 sectors) -> seek_step"); read10(100); step(800)
ev("READ 12000 -> seek_out"); read10(12000); step(800)
ev("SEEK 50 -> seek_in"); seek10(50); step(800)
ev("idle 61 s emulated -> spin_stop"); step(61000, False); step(2500)
ev("READ 60 -> spin_start again"); read10(60); step(2500)
ev("STOP UNIT -> spin_stop"); start_stop(0x00); step(2500)
ev("guest eject (LoEj) -> open_tray"); start_stop(0x02); step(2500)
ev("guest load -> close_tray"); start_stop(0x03); step(2500)
ev("host eject -> open_tray"); cmd("cd eject 0"); step(2500)
ev("host load audio disc -> close_tray"); cmd(f"cd load 0 {os.path.join(work, 'tone.cue')}"); step(2500); packet([0x00]*6); packet([0x03, 0, 0, 0, 18, 0], 18)
ev("PLAY AUDIO 0:02-0:50 -> spin_start, spin; no idle stop while playing")
play_msf(0, 2, 0, 0, 50, 0); step(3000); step(40000, False); step(1000)
ev("PAUSE, idle 61 s -> spin_stop"); pause(0); step(61000, False); step(2500)
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
