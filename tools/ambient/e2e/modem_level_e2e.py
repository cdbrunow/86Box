import json, os, socket, subprocess, sys, time, wave
import numpy as np
exe, roms, assets, work = sys.argv[1:5]
def run(name, extra):
    d = os.path.join(work, name); os.makedirs(os.path.join(d, "rec"), exist_ok=True)
    open(os.path.join(d, "86box.cfg"), "w").write("[General]\n" + extra + "\n[Machine]\nmachine = ibmat\n\n[Ports (COM & LPT)]\nserial1_enabled = 1\nserial1_device = char_modem\n")
    sock = os.path.join(d, "s.sock")
    p = subprocess.Popen([exe, "--automation", "unix:" + sock, "--automation-hold", "--rompath", roms, "--assetpath", assets, "--vmpath", d], env=dict(os.environ, QT_QPA_PLATFORM="offscreen"), stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    for _ in range(200):
        if os.path.exists(sock): break
        time.sleep(0.1)
    s = socket.socket(socket.AF_UNIX); s.connect(sock); fh = s.makefile("rw")
    def cmd(c): fh.write(c + "\n"); fh.flush(); return json.loads(fh.readline() or "{}")
    cmd("step 2000")
    for port, val in [(0x3fb, 0x80), (0x3f8, 12), (0x3f9, 0), (0x3fb, 0x03), (0x3fc, 0x03)]:
        cmd(f"iow {port} 1 {val}")
    cmd("audio start " + os.path.join(d, "rec"))
    for ch in "ATDT5551234\r":
        cmd(f"iow 0x3f8 1 {ord(ch)}"); cmd("step 3")
    cmd("step 4000 paced")
    cmd("audio stop"); cmd("quit"); p.wait(timeout=30)
    w = wave.open(os.path.join(d, "rec", "main.wav")); a = np.frombuffer(w.readframes(w.getnframes()), "<i2").astype(float)
    return 20 * np.log10(max(np.sqrt(np.mean(a ** 2)), 1e-3) / 32768), len(a) / 2 / w.getframerate()
a = run("m100", ""); b = run("m50", "modem_sound_gain = 50\n")
print(f"modem speaker: 100 % {a[0]:.2f} dBFS ({a[1]:.1f} s), 50 % {b[0]:.2f} dBFS -> {b[0]-a[0]:+.2f} dB (expect -6.02)")
