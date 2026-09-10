from __future__ import print_function
import os, shutil, subprocess, sys, time

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))


def find_picotool():
    for name in ("picotool", "picotool.exe"):
        p = shutil.which(name)
        if p:
            return p
    base = os.path.join(os.path.expanduser("~"), ".pico-sdk", "picotool")
    if os.path.isdir(base):
        for ver in sorted(os.listdir(base), reverse=True):
            for rel in (("picotool", "picotool.exe"), ("picotool", "picotool")):
                p = os.path.join(base, ver, *rel)
                if os.path.exists(p):
                    return p
    return None


def find_openocd():
    candidates = []
    for name in (os.environ.get("OPENOCD"), shutil.which("openocd"), shutil.which("openocd.exe")):
        if name:
            candidates.append(name)
    base = os.path.join(os.path.expanduser("~"), ".pico-sdk", "openocd")
    if os.path.isdir(base):
        for ver in sorted(os.listdir(base), reverse=True):
            r = os.path.join(base, ver)
            for rel in ("openocd.exe", os.path.join("bin", "openocd.exe")):
                p = os.path.join(r, rel)
                if os.path.exists(p):
                    candidates.append(p)
    for tool in candidates:
        td = os.path.dirname(os.path.abspath(tool))
        roots = [os.environ.get("OPENOCD_SCRIPTS"), os.path.join(td, "scripts"),
                 os.path.join(td, "share", "openocd", "scripts"),
                 os.path.join(os.path.dirname(td), "scripts"),
                 os.path.join(os.path.dirname(td), "share", "openocd", "scripts")]
        for scripts in roots:
            if (scripts and
                    os.path.exists(os.path.join(scripts, "interface", "cmsis-dap.cfg")) and
                    os.path.exists(os.path.join(scripts, "target", "rp2350.cfg"))):
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
                if (b"MWPICO1" in line or b"MCFDPSRAM1" in line or
                    b"MCFDOOM1" in line):
                    return True
    except (OSError, ValueError):
        return False
    return False


def find_pico_port(require_response=False):
    try:
        from serial.tools import list_ports
    except ImportError:
        return None

    ports = list(list_ports.comports())

    # 0x2e8a:0x000c is the CMSIS-DAP probe's own serial interface (COM4 on
    # this setup), not the MicroConsole application CDC port.  Do not waste
    # 1.5 seconds probing it before every command.
    pico_ports = [
        p.device for p in ports
        if p.vid == 0x2E8A and p.pid != 0x000C
    ]

    if not require_response and len(pico_ports) == 1:
        return pico_ports[0]

    for dev in pico_ports:
        if pico_ping(dev):
            return dev

    # Fallback for non-Raspberry-Pi USB/UART bridges used by other targets.
    other_ports = [
        p.device for p in ports
        if not (p.vid == 0x2E8A and p.pid == 0x000C)
        and p.device not in pico_ports
    ]
    for dev in other_ports:
        if pico_ping(dev):
            return dev

    return None


def wait_for_pico(timeout=8.0):
    """Wait only for the application USB CDC device to enumerate.

    Do not open the serial port or send PING here.  FastDoom can spend several
    seconds in WAD/game startup before I_StartTic() begins servicing commands,
    and on Windows opening a just-reset COM port can itself block in
    GetCommState().  OpenOCD has already verified the flash image; this helper
    is only confirming that the application USB device came back.
    """
    deadline = time.time() + timeout
    while time.time() < deadline:
        port = find_pico_port(require_response=False)
        if port:
            return port
        time.sleep(0.1)
    return None


def open_serial(retries=12, retry_delay=0.25):
    try:
        import serial
    except ImportError:
        print("pyserial is required: python -m pip install pyserial")
        return None, None

    port = find_pico_port(require_response=False)
    if not port:
        print("No MicroConsole serial port found")
        return None, None

    last_error = None
    for attempt in range(retries):
        try:
            ser = serial.Serial(port, 115200, timeout=0.15)
            return port, ser
        except (OSError, ValueError) as exc:
            last_error = exc
            if attempt + 1 < retries:
                time.sleep(retry_delay)

    print("serial error:", last_error)
    return None, None


def set_volume(volume):
    try:
        volume = max(0, min(100, int(volume)))
    except ValueError:
        print("volume must be 0..100")
        return 2

    port, ser = open_serial()
    if not ser:
        return 1
    try:
        ser.write(("VOL %d\n" % volume).encode("ascii"))
        ser.flush()
        deadline = time.time() + 2.0
        while time.time() < deadline:
            line = ser.readline()
            if (b"MWPICO1 vol=" in line or
                    b"MCFDOOM1 volume=" in line):
                print("%s: %s" % (port, line.decode("ascii", "replace").strip()))
                return 0
    finally:
        ser.close()

    print("Pico did not acknowledge VOL")
    return 1


def set_example(example_id):
    port, ser = open_serial()
    if not ser:
        return 1
    try:
        ser.write(("EXAMPLE %s\n" % example_id).encode("ascii"))
        ser.flush()
        deadline = time.time() + 2.0
        while time.time() < deadline:
            line = ser.readline()
            if not line:
                continue
            text = line.decode("ascii", "replace").strip()
            if "MWPICO1 selected=" in text:
                print("%s: %s" % (port, text))
                return 0
            if "MWPICO1 error=unknown-example" in text:
                print("%s: %s" % (port, text))
                return 2
    finally:
        ser.close()

    print("Pico did not acknowledge EXAMPLE")
    return 1


def list_examples():
    port, ser = open_serial()
    if not ser:
        return 1
    seen_begin = False
    try:
        ser.write(b"LIST\n")
        ser.flush()
        deadline = time.time() + 3.0
        while time.time() < deadline:
            line = ser.readline()
            if not line:
                continue
            text = line.decode("ascii", "replace").strip()
            if "MWPICO1 list-begin" in text:
                seen_begin = True
                print(text)
            elif seen_begin and "MWPICO1 example=" in text:
                print(text)
            elif seen_begin and "MWPICO1 list-end" in text:
                print(text)
                return 0
    finally:
        ser.close()

    print("Pico did not return an example list")
    return 1



def command_response_matches(command, text):
    cmd = command.strip()
    upper = cmd.upper()

    if upper == "PING":
        return text.startswith("MCFDOOM1 state=") or text.startswith("MWPICO1")
    if upper == "STAT":
        return text.startswith("MCFDOOM1 stat ")
    if upper == "MUSIC":
        return text.startswith("MCFDOOM1 music ")
    if upper.startswith("TRACK "):
        return text.startswith("MCFDOOM1 track=")
    if upper == "STACK":
        return text.startswith("MCFDOOM1 stack ")
    if upper == "FS":
        return text.startswith("MCFDOOM1 fs_probe=")
    if upper == "SDRAW":
        return text.startswith("MCFDOOM1 sdraw=")

    # Generic MicroConsole commands may have several response shapes.
    return (text.startswith("MCFDOOM1") or
            text.startswith("MWPICO1") or
            text.startswith("MCFDPSRAM1"))


def read_command_response(ser, port, command, timeout=3.0):
    deadline = time.time() + timeout

    while time.time() < deadline:
        line = ser.readline()
        if not line:
            continue

        text = line.decode("ascii", "replace").strip()
        if not text:
            continue

        # Boot/status chatter may arrive after the command was sent.  Show it,
        # but do not mistake it for the requested command's acknowledgement.
        print("%s: %s" % (port, text))

        if command_response_matches(command, text):
            return 0

    return 1


def command_shell():
    port, ser = open_serial()
    if not ser:
        return 1

    print("MicroConsole command shell on %s" % port)
    print("Commands: PING, STAT, MUSIC, TRACK INTROA|E1M1|E1M5, STACK, FS, SDRAW, VOL n, KEY ...")
    print("Type quit or exit to close the port.")

    try:
        while True:
            try:
                command = input("mc> ").strip()
            except (EOFError, KeyboardInterrupt):
                print()
                break

            if not command:
                continue
            if command.lower() in ("quit", "exit"):
                break

            try:
                ser.reset_input_buffer()
                ser.write((command + "\n").encode("ascii"))
                ser.flush()
            except (OSError, ValueError) as exc:
                print("serial error:", exc)
                return 1

            if read_command_response(ser, port, command, timeout=3.0) != 0:
                print("%s: no matching response for %s" % (port, command))
    finally:
        ser.close()

    return 0


def send_command(command):
    port, ser = open_serial()
    if not ser:
        return 1

    try:
        ser.reset_input_buffer()
        ser.write((command + "\n").encode("ascii"))
        ser.flush()

        rc = read_command_response(ser, port, command, timeout=3.0)
        if rc != 0:
            print("%s: no matching response for %s" % (port, command))
        return rc
    finally:
        ser.close()


def outputs(device, image="demo"):
    b = os.path.join(ROOT, "pico", "build-" + device.lower())
    if image == "examples":
        stem = "microconsole_examples"
    elif image == "psram":
        stem = "microconsole_fastdoom_psram_probe"
    elif image == "fastdoom":
        stem = "microconsole_fastdoom_pico"
    else:
        stem = "microconsole_demo"
    return os.path.join(b, stem + ".elf"), os.path.join(b, stem + ".uf2")


def swd(elf):
    tool, scripts = find_openocd()
    if not tool:
        print("OpenOCD/CMSIS-DAP setup not found")
        return 1
    e = os.path.abspath(elf).replace("\\", "/")
    cmd = [tool, "-s", scripts, "-f", "interface/cmsis-dap.cfg",
           "-f", "target/rp2350.cfg", "-c",
           ("adapter speed 5000; "
            "rp2350.dap.core0 cortex_m reset_config sysresetreq; "
            "rp2350.dap.core1 cortex_m reset_config sysresetreq; "
            "program {%s} verify reset exit" % e)]
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


def flash(device, method, image):
    elf, uf2 = outputs(device, image)

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

    port = wait_for_pico(8.0)
    if port:
        print("MicroConsole USB enumerated on " + port +
              " (firmware may still be starting)")
    else:
        print("flash verified; MicroConsole USB has not enumerated yet")
    return 0


def usage():
    print("usage: mc_pico.py flash [max98357a|pcm5102a|ns4168] [swd|picotool|manual]")
    print("       mc_pico.py flash-examples [max98357a|pcm5102a|ns4168] [swd|picotool|manual]")
    print("       mc_pico.py flash-psram [max98357a|pcm5102a|ns4168] [swd|picotool|manual]")
    print("       mc_pico.py flash-fastdoom [max98357a|pcm5102a|ns4168] [swd|picotool|manual]")
    print("       mc_pico.py volume 0..100")
    print("       mc_pico.py example ID")
    print("       mc_pico.py list-examples")
    print('       mc_pico.py command "PING|STAT|MUSIC|TRACK E1M1|STACK|FS|SDRAW|KEY LEFT DOWN|..."')
    print("       mc_pico.py shell")


def main(argv):
    if len(argv) < 2:
        usage()
        return 2

    cmd = argv[1].lower()

    if cmd == "volume":
        if len(argv) < 3:
            usage()
            return 2
        return set_volume(argv[2])

    if cmd == "example":
        if len(argv) < 3:
            usage()
            return 2
        return set_example(argv[2])

    if cmd == "list-examples":
        return list_examples()

    if cmd == "command":
        if len(argv) < 3:
            usage()
            return 2
        return send_command(" ".join(argv[2:]))

    if cmd == "shell":
        return command_shell()

    if cmd not in ("flash", "flash-examples", "flash-psram", "flash-fastdoom"):
        usage()
        return 2

    device = argv[2].lower() if len(argv) > 2 else "max98357a"
    method = argv[3].lower() if len(argv) > 3 else "swd"
    if cmd == "flash-examples":
        image = "examples"
    elif cmd == "flash-psram":
        image = "psram"
    elif cmd == "flash-fastdoom":
        image = "fastdoom"
    else:
        image = "demo"
    return flash(device, method, image)


if __name__ == "__main__":
    sys.exit(main(sys.argv))
