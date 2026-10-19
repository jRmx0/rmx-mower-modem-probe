"""Reset the ESP32 on a serial port and print its log for N seconds.

Usage: python -I -u tools/serial_log.py COM6 [seconds] [--until TEXT]
Opening the port resets the ESP32 (auto-reset circuit); the modem keeps running.
"""
import sys
import time

import serial


def main():
    args = sys.argv[1:]
    until = None
    if "--until" in args:
        i = args.index("--until")
        until = args[i + 1]
        del args[i:i + 2]
    port = args[0] if args else "COM6"
    seconds = float(args[1]) if len(args) > 1 else 60
    s = serial.Serial(port, 115200, timeout=0.2)
    s.dtr = False
    s.rts = True
    time.sleep(0.1)
    s.rts = False
    t = time.time()
    seen = ""
    while time.time() - t < seconds:
        chunk = s.read(4096).decode("utf-8", "replace")
        if chunk:
            print(chunk, end="", flush=True)
            seen = (seen + chunk)[-4096:]
            if until and until in seen:
                time.sleep(1)
                print(s.read(4096).decode("utf-8", "replace"), end="")
                break
    s.close()


if __name__ == "__main__":
    main()
