"""Send AT commands through the at_bridge firmware and print the replies.

Usage:  python -I tools/at_session.py COM6 "AT" "ATI" "AT+CSQ"
        python -I tools/at_session.py COM6 --file cmds.txt
        python -I tools/at_session.py COM6 --listen 30     (just print URCs)
Options:
  --boot N     max seconds to wait for the bridge after the reset (default 30)
  --verbose    print the whole ESP boot log
Command prefixes (in this order):
  wait=<ms>:       how long to wait (default 5000)
  expect=<text>|   wait until <text> appears (e.g. a URC like +CCHOPEN:) instead of OK
  raw:             send the bytes as-is, no CR added; \\r \\n \\x1a escapes are decoded
  rawfile:<path>   send a file as-is (e.g. a PEM after AT+CCERTDOWN)
Example: 'wait=60000:expect=+CCHOPEN:|AT+CCHOPEN=0,"api.rmx.lt",443,2'
Use the IDF venv python (it has pyserial):
  C:\\Espressif\\tools\\python\\v6.1\\venv\\Scripts\\python.exe -I -u tools/at_session.py ...
"""
import sys, time, serial

FINAL = (b"\r\nOK\r\n", b"ERROR", b"NO CARRIER", b"CONNECT", b"\r\n> ", b"[bridge]")


def run(s, cmd, wait_ms=5000):
    expect = None
    if cmd.startswith("wait="):
        w, cmd = cmd.split(":", 1)
        wait_ms = int(w[5:])
    if cmd.startswith("expect="):
        e, cmd = cmd.split("|", 1)
        expect = e[7:].encode()
    if cmd.startswith("rawfile:"):
        with open(cmd[8:], "rb") as f:
            data = f.read()
    elif cmd.startswith("raw:"):
        data = cmd[4:].encode().decode("unicode_escape").encode("latin-1")
    else:
        data = cmd.encode() + b"\r"
    s.reset_input_buffer()
    s.write(data)
    buf, t = b"", time.time()
    while time.time() - t < wait_ms / 1000:
        buf += s.read(4096)
        if expect is not None:
            if expect in buf:
                time.sleep(0.3)
                buf += s.read(4096)
                break
        elif cmd.startswith("~"):
            if b"[bridge]" in buf:
                break
        elif any(f in buf[len(data):] for f in FINAL):
            time.sleep(0.15)
            buf += s.read(4096)
            break
    print(f">>> {cmd}\n{buf.decode('utf-8', 'replace').strip()}\n", flush=True)


def main():
    a = sys.argv[1:]
    port = a.pop(0)
    secs = 30.0
    if "--boot" in a:
        i = a.index("--boot")
        secs = float(a[i + 1])
        del a[i:i + 2]
    verbose = "--verbose" in a
    if verbose:
        a.remove("--verbose")

    s = serial.Serial(port, 115200, timeout=0.1)
    # Opening the port toggles DTR/RTS, which resets the ESP32 through the
    # board's auto-reset circuit. So reset deliberately and wait for the
    # bridge to come up (the modem keeps running across ESP resets).
    s.dtr = False
    s.rts = True
    time.sleep(0.1)
    s.rts = False
    buf, t = b"", time.time()
    while time.time() - t < secs and b"[bridge] ready" not in buf:
        buf += s.read(4096)
    lines = buf.decode("utf-8", "replace").splitlines()
    print("\n".join(l for l in lines if verbose or "[bridge]" in l or "at_bridge" in l), flush=True)

    if "--listen" in a:
        secs = float(a[a.index("--listen") + 1])
        t = time.time()
        while time.time() - t < secs:
            d = s.read(4096)
            if d:
                sys.stdout.write(d.decode("utf-8", "replace"))
                sys.stdout.flush()
        return
    cmds = a
    if "--file" in a:
        with open(a[a.index("--file") + 1]) as f:
            cmds = [l.rstrip("\r\n") for l in f if l.strip() and not l.startswith("#")]
    for c in cmds:
        run(s, c)


main()
