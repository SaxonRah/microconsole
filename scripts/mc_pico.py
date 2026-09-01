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
    if len(argv) < 2 or argv[1] != "flash":
        print("usage: mc_pico.py flash [max98357a|pcm5102a|ns4168] [swd|picotool|manual]")
        return 2
    device = argv[2].lower() if len(argv) > 2 else "max98357a"
    method = argv[3].lower() if len(argv) > 3 else "swd"
    elf, uf2 = outputs(device)
    if not os.path.exists(uf2):
        print("missing build output:", uf2)
        return 1
    if method == "manual": return manual(uf2)
    if method == "picotool": return picotool(uf2)
    if method == "swd":
        if not os.path.exists(elf):
            print("missing ELF:", elf); return 1
        return swd(elf)
    print("unknown flash method:", method)
    return 2

if __name__ == "__main__":
    sys.exit(main(sys.argv))
