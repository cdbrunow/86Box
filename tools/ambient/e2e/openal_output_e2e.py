#!/usr/bin/env python3
"""Check what actually reaches the speakers, not just what 86Box produces.

The automation recorder captures each stream before the audio backend, so a
stream that never gets an OpenAL source still records fine (that bug hid the
fan and CD-ROM sounds until this test). Here OpenAL Soft's "wave" driver
writes its real-time output to a file, and the fan's spin-up and loop are
located in it.

usage: python3 -I openal_output_e2e.py <86Box> <roms> <assets> <workdir>
Needs OpenAL Soft (its wave driver), sox, numpy.
"""
import json, os, socket, subprocess, sys, time, wave
import numpy as np

exe, roms, assets, work = sys.argv[1:5]
os.makedirs(work, exist_ok=True)
out = os.path.join(work, "openal_out.wav")
conf = os.path.join(work, "alsoft.ini")
with open(conf, "w") as f:
    f.write(f"[general]\ndrivers = wave\nfrequency = 48000\n[wave]\nfile = {out}\n")
with open(os.path.join(work, "86box.cfg"), "w") as f:
    f.write("[Machine]\nmachine = ibmat\nsystem_fan_audio_profile = IBM_PS1_2121\n")
sock = os.path.join(work, "s.sock")
p = subprocess.Popen([exe, "--automation", "unix:" + sock, "--automation-hold", "--rompath", roms,
                      "--assetpath", assets, "--vmpath", work],
                     env=dict(os.environ, QT_QPA_PLATFORM="offscreen", ALSOFT_CONF=conf),
                     stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
for _ in range(200):
    if os.path.exists(sock): break
    time.sleep(0.1)
s = socket.socket(socket.AF_UNIX); s.connect(sock); fh = s.makefile("rw")
def cmd(c): fh.write(c + "\n"); fh.flush(); return json.loads(fh.readline() or "{}")
cmd("release"); time.sleep(10); cmd("quit"); p.wait(timeout=30)

pcm = os.path.join(work, "openal_out16.wav")
subprocess.run(["sox", out, "-b", "16", "-e", "signed-integer", pcm], check=True, capture_output=True)
rd = lambda path: (lambda w: np.frombuffer(w.readframes(w.getnframes()), "<i2").reshape(-1, 2)[:, 0].astype(float))(wave.open(path))
# Compare sample-to-sample differences: that ignores the DC offset the main
# stream carries during the BIOS self-test, which would otherwise swamp the
# correlation.
o = np.diff(rd(pcm))
D = os.path.join(assets, "sounds/system/IBM PS1 2121/IBM_PS1_2121_")
up_raw, lp_raw = rd(D + "POWER_UP.wav"), rd(D + "POWER_ON.wav")
up = np.diff(up_raw)
# what the fan should play: spin-up, then the loop over and over (differenced
# as a whole, so the joins stay sample-exact)
exp = np.diff(np.concatenate([up_raw] + [lp_raw] * 8))

def best(t, sig):
    n = len(t); m = len(sig); size = 1 << int(np.ceil(np.log2(m + n)))
    c = np.fft.irfft(np.fft.rfft(sig, size) * np.conj(np.fft.rfft(t, size)), size)[:m - n + 1]
    c2 = np.concatenate([[0], np.cumsum(sig ** 2)])
    ncc = c / np.sqrt(np.maximum(c2[n:] - c2[:-n], 1e-9) * np.sum(t ** 2))
    i = int(np.argmax(ncc)); return i, float(ncc[i])

# The BIOS self-test's noise on the main stream masks parts of a headless
# capture, and moves between runs. So: find a clean slice of the spin-up
# (0.4-0.9 s into it), then check every 0.1 s of the following loop that
# isn't masked (output within 3 dB of the fan's own level), sample-aligned.
sl = slice(int(0.4 * 48000), int(0.9 * 48000))
k, r = best(up[sl], o[:4 * 48000])
i = k - sl.start
print(f"spin-up found in the real output at {i / 48000:.3f} s, r={r:.3f}")
fit = []
for t in np.arange(4.0, 9.5, 0.1):
    a = int(t * 48000); e = exp[a - i:a - i + 4800]; w = o[a:a + 4800]
    g = np.dot(e, w) / max(np.dot(e, e), 1e-9)
    if (g > 0.1) and (np.dot(w, w) <= 2.0 * g * g * np.dot(e, e)):  # fan present, not masked
        fit.append(float(np.dot(e, w) / np.sqrt(np.dot(e, e) * np.dot(w, w) + 1e-9)))
worst = min(fit) if fit else -1.0
print(f"loop after it: {len(fit)} unmasked 0.1 s windows, worst match r={worst:.3f}")
ok = r > 0.95 and len(fit) >= 20 and worst > 0.95
print("PASS" if ok else "FAIL: the fan isn't reaching the audio output intact")
sys.exit(0 if ok else 1)
