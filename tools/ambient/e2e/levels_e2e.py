import json, os, socket, subprocess, sys, time, wave
import numpy as np
exe, roms, assets, work = sys.argv[1:5]
def run(name, extra):
    d = os.path.join(work, name); os.makedirs(os.path.join(d, "rec"), exist_ok=True)
    open(os.path.join(d, "hd.img"), "wb").truncate(615 * 4 * 17 * 512)
    open(os.path.join(d, "86box.cfg"), "w").write("[General]\n" + extra + "\n[Machine]\nmachine = ibmat\nsystem_fan_audio_profile = IBM_PS1_2121\n\n[Storage controllers]\nhdc_1 = st506_at\n\n[Hard disks]\nhdd_01_parameters = 17, 4, 615, 0, mfm\nhdd_01_fn = hd.img\nhdd_01_mfm_channel = 0\nhdd_01_speed = mfm_3600rpm\nhdd_01_audio = IBM_WDL330P_3600RPM\n")
    sock = os.path.join(d, "s.sock")
    p = subprocess.Popen([exe, "--automation", "unix:" + sock, "--automation-hold", "--rompath", roms, "--assetpath", assets, "--vmpath", d], env=dict(os.environ, QT_QPA_PLATFORM="offscreen"), stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    for _ in range(200):
        if os.path.exists(sock): break
        time.sleep(0.1)
    s = socket.socket(socket.AF_UNIX); s.connect(sock); fh = s.makefile("rw")
    def cmd(c): fh.write(c + "\n"); fh.flush(); return json.loads(fh.readline() or "{}")
    cmd("audio start " + os.path.join(d, "rec")); cmd("step 8000 paced"); cmd("audio stop"); cmd("quit")
    p.wait(timeout=30)
    L = lambda f: (lambda w: np.frombuffer(w.readframes(w.getnframes()), "<i2").astype(float))(wave.open(os.path.join(d, "rec", f)))
    rms = lambda a: 20 * np.log10(max(np.sqrt(np.mean(a[:48000*6*2] ** 2)), 1e-3) / 32768)
    return rms(L("ambient.wav")), rms(L("hdd.wav"))
a = run("default", "")
b = run("scaled", "fan_sound_gain = 50\nhdd_sound_gain = 25\n")
print(f"fan:  default {a[0]:.2f} dBFS, at 50 %  {b[0]:.2f} dBFS -> {b[0]-a[0]:+.2f} dB (expect -6.02)")
print(f"HDD:  default {a[1]:.2f} dBFS, at 25 %  {b[1]:.2f} dBFS -> {b[1]-a[1]:+.2f} dB (expect -12.04)")
