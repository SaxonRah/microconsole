from __future__ import print_function
import os, shutil, subprocess, sys, time

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))

def find_picotool():
    for name in ("picotool", "picotool.exe"):
        p = shutil.which(name)
        if p: return p
    base = os.path.join(os.path.expanduser("~"), ".pico-sdk", "picotool")
    if os.path.isdir(base):
        for ver in sorted(os.listdir(base), reverse=True):
            for rel in (("picotool", "picotool.exe"), ("picotool", "picotool")):
                p = os.path.join(base, ver, *rel)
                if os.path.exists(p): return p
    return None

def find_openocd():
    candidates = []
    for name in (os.environ.get("OPENOCD"), shutil.which("openocd"), shutil.which("openocd.exe")):
        if name: candidates.append(name)
    base = os.path.join(os.path.expanduser("~"), ".pico-sdk", "openocd")
    if os.path.isdir(base):
        for ver in sorted(os.listdir(base), reverse=True):
            r = os.path.join(base, ver)
            for rel in ("openocd.exe", os.path.join("bin", "openocd.exe")):
                p = os.path.join(r, rel)
                if os.path.exists(p): candidates.append(p)
    for tool in candidates:
        td = os.path.dirname(os.path.abspath(tool))
        roots = [os.environ.get("OPENOCD_SCRIPTS"), os.path.join(td, "scripts"),
                 os.path.join(td, "share", "openocd", "scripts"),
                 os.path.join(os.path.dirname(td), "scripts"),
                 os.path.join(os.path.dirname(td), "share", "openocd", "scripts")]
        for scripts in roots:
            if scripts and os.path.exists(os.path.join(scripts, "interface", "cmsis-dap.cfg")) and os.path.exists(os.path.join(scripts, "target", "rp2350.cfg")):
                return tool, scripts
    return None, None

def pico_ping(port, baud=115200, timeout=1.5):
    try:
        import serial
    except ImportError:
        return False
    try:
        with serial.Serial(port, baud, timeout=timeout) as ser:
            time.sleep(0.15)
            ser.reset_input_buffer()
            ser.write(b"PING\n")
            ser.flush()
            deadline = time.time() + timeout
            while time.time() < deadline:
                line = ser.readline()
                if b"MWPICO1" in line:
                    return True
    except (OSError, ValueError):
        return False
    return False

def find_pico_port():
    try:
        from serial.tools import list_ports
    except ImportError:
        return None
    ports = list(list_ports.comports())
    ordered = ([p.device for p in ports if p.vid == 0x2E8A] +
               [p.device for p in ports if p.vid != 0x2E8A])
    for dev in ordered:
        if pico_ping(dev):
            return dev
    return None

def wait_for_pico(timeout=20.0):
    deadline = time.time() + timeout
    while time.time() < deadline:
        port = find_pico_port()
        if port:
            return port
        time.sleep(0.5)
    return None

def set_volume(volume):
    try:
        import serial
    except ImportError:
        print("pyserial is required: python -m pip install pyserial")
        return 1
    try:
        volume = max(0, min(100, int(volume)))
    except ValueError:
        print("volume must be 0..100")
        return 2

    port = find_pico_port()
    if not port:
        print("no MWPICO1 Pico found")
        return 1

    try:
        with serial.Serial(port, 115200, timeout=2.0) as ser:
            time.sleep(0.15)
            ser.reset_input_buffer()
            ser.write(("VOL %d\n" % volume).encode("ascii"))
            ser.flush()
            deadline = time.time() + 2.0
            while time.time() < deadline:
                line = ser.readline()
                if b"MWPICO1 vol=" in line:
                    print("%s: %s" % (port, line.decode("ascii", "replace").strip()))
                    return 0
    except (OSError, ValueError) as exc:
        print("serial error:", exc)
        return 1

    print("Pico did not acknowledge VOL")
    return 1

def outputs(device):
    b = os.path.join(ROOT, "pico", "build-" + device.lower())
    return os.path.join(b, "microconsole_demo.elf"), os.path.join(b, "microconsole_demo.uf2")

def swd(elf):
    tool, scripts = find_openocd()
    if not tool:
        print("OpenOCD/CMSIS-DAP setup not found")
        return 1
    e = os.path.abspath(elf).replace("\\", "/")
    cmd = [tool, "-s", scripts, "-f", "interface/cmsis-dap.cfg",
           "-f", "target/rp2350.cfg", "-c",
           "adapter speed 5000; program {%s} verify reset exit" % e]
    print("SWD:", " ".join(cmd))
    return subprocess.call(cmd, cwd=ROOT)

def picotool(uf2):
    tool = find_picotool()
    if not tool:
        print("picotool not found")
        return 1
    return subprocess.call([tool, "load", "-f", "-x", uf2], cwd=ROOT)

def manual(uf2):
    print("Manual BOOTSEL flash:")
    print("  1. Put the Pico Plus 2 into BOOTSEL.")
    print("  2. Copy this UF2 to RPI-RP2:")
    print("     " + uf2)
    return 0

def main(argv):
    if len(argv) >= 2 and argv[1].lower() == "volume":
        if len(argv) < 3:
            print("usage: mc_pico.py volume 0..100")
            return 2
        return set_volume(argv[2])

    if len(argv) < 2 or argv[1].lower() != "flash":
        print("usage: mc_pico.py flash [max98357a|pcm5102a|ns4168] [swd|picotool|manual]")
        print("       mc_pico.py volume 0..100")
        return 2

    device = argv[2].lower() if len(argv) > 2 else "max98357a"
    method = argv[3].lower() if len(argv) > 3 else "swd"
    elf, uf2 = outputs(device)

    if not os.path.exists(uf2):
        print("missing build output:", uf2)
        return 1

    if method == "manual":
        return manual(uf2)
    if method == "picotool":
        rc = picotool(uf2)
    elif method == "swd":
        if not os.path.exists(elf):
            print("missing ELF:", elf)
            return 1
        rc = swd(elf)
    else:
        print("unknown flash method:", method)
        return 2

    if rc != 0:
        return rc

    port = wait_for_pico(20.0)
    if port:
        print("MicroConsole/MicroWave is answering on " + port)
    else:
        print("flash verified, but no MWPICO1 USB serial response was found")
    return 0

if __name__ == "__main__":
    sys.exit(main(sys.argv))
