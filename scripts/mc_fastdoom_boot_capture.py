#!/usr/bin/env python3
import os
import subprocess
import sys
import time

import serial
from serial.tools import list_ports

VID = 0x2E8A
APP_PID = 0x0009

OPENOCD = os.path.expandvars(
    r"%USERPROFILE%\.pico-sdk\openocd\0.12.0+dev\openocd.exe"
)
OPENOCD_SCRIPTS = os.path.expandvars(
    r"%USERPROFILE%\.pico-sdk\openocd\0.12.0+dev\scripts"
)

def find_app_port():
    for p in list_ports.comports():
        if p.vid == VID and p.pid == APP_PID:
            return p.device
    return None

def start_reset():
    cmd = [
        OPENOCD,
        "-s", OPENOCD_SCRIPTS,
        "-f", "interface/cmsis-dap.cfg",
        "-f", "target/rp2350.cfg",
        "-c", "adapter speed 5000; init; reset run; shutdown",
    ]
    print("SWD reset:", " ".join(cmd), flush=True)
    return subprocess.Popen(
        cmd,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
    )

def try_open(port):
    try:
        s = serial.Serial(port, 115200, timeout=0.02)
        print(f"opened {port}", flush=True)
        return s
    except serial.SerialException:
        return None

def main():
    log_name = "fastdoom-boot-serial.log"
    print(f"Capturing Pico application USB serial to {log_name}", flush=True)

    ser = None
    port_name = None

    # Open the already-enumerated application port before reset if possible.
    initial = find_app_port()
    if initial:
        ser = try_open(initial)
        if ser is not None:
            port_name = initial
            try:
                ser.reset_input_buffer()
            except serial.SerialException:
                pass

    reset_proc = start_reset()
    deadline = time.monotonic() + 20.0

    with open(log_name, "w", encoding="utf-8", errors="replace") as log:
        while time.monotonic() < deadline:
            # Reap and display OpenOCD output without blocking serial capture.
            if reset_proc.poll() is not None and reset_proc.stdout is not None:
                ocd = reset_proc.stdout.read()
                if ocd:
                    print(ocd, end="", flush=True)
                reset_proc.stdout = None

            port = find_app_port()

            if port is None:
                if ser is not None:
                    try:
                        ser.close()
                    except Exception:
                        pass
                    ser = None
                    port_name = None
                time.sleep(0.02)
                continue

            if ser is None or port != port_name:
                if ser is not None:
                    try:
                        ser.close()
                    except Exception:
                        pass
                ser = try_open(port)
                port_name = port if ser is not None else None
                if ser is None:
                    time.sleep(0.02)
                    continue

            try:
                data = ser.read(4096)
            except serial.SerialException:
                try:
                    ser.close()
                except Exception:
                    pass
                ser = None
                port_name = None
                time.sleep(0.02)
                continue

            if data:
                text = data.decode("utf-8", errors="replace")
                print(text, end="", flush=True)
                log.write(text)
                log.flush()

        if ser is not None:
            try:
                ser.close()
            except Exception:
                pass

    if reset_proc.poll() is None:
        reset_proc.terminate()
        try:
            reset_proc.wait(timeout=2)
        except subprocess.TimeoutExpired:
            reset_proc.kill()

    print(f"\nCapture complete: {log_name}", flush=True)

if __name__ == "__main__":
    main()
