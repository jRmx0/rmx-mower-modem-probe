# tools/

Diagnostic firmware and scripts for the LilyGo T-SIM7600. All were run on the real board.

| Tool | What it does |
|---|---|
| [`at_bridge/`](at_bridge/) | ESP-IDF app: powers the modem with the board's pin sequence, then bridges USB console ⇄ modem UART (like LilyGo's ATdebug). |
| [`at_session.py`](at_session.py) | Host script: resets the ESP, waits for `at_bridge`, sends AT commands, prints replies. |
| [`serial_log.py`](serial_log.py) | Host script: resets the ESP and prints its log (`COM6 [seconds] [--until TEXT]`). |
| [`ppp_smoke/`](ppp_smoke/) | ESP-IDF app: esp_modem PPP → HTTPS GET `api.rmx.lt/probe/health` → MQTTS to `mqtt.rmx.lt:8883` with a fake login (expects CONNACK 5). The reference for the probe firmware. |

## Build and flash (PowerShell)

```powershell
. C:\Espressif\tools\Microsoft.v6.1.PowerShell_profile.ps1
cd tools\at_bridge          # or tools\ppp_smoke
idf.py set-target esp32     # first time only
idf.py build
idf.py -p COM6 -b 921600 flash
```

Only one app is on the board at a time; flashing one replaces the other.

## AT sessions (with at_bridge flashed)

```powershell
$py = "C:\Espressif\tools\python\v6.1\venv\Scripts\python.exe"
& $py -I -u tools\at_session.py COM6 "AT+CSQ" "AT+CPSI?" "AT+COPS?"
& $py -I -u tools\at_session.py COM6 --file cmds.txt
& $py -I -u tools\at_session.py COM6 --listen 30
```

Command prefixes: `wait=<ms>:` (timeout), `expect=<text>|` (wait for a URC such as
`+CCHOPEN:` instead of `OK`), `raw:` (send bytes without CR; `\r\n` escapes),
`rawfile:<path>` (send a file, e.g. a PEM after `AT+CCERTDOWN`). Example file for a
verified TLS test: see [knowledge/02-sim7600-modem.md](../knowledge/02-sim7600-modem.md).

Bridge commands (type a line starting with `~`): `~status` (STATUS/RING/FLIGHT/DTR levels),
`~pwr` (500 ms PWRKEY pulse — toggles power!), `~pwroff` (3 s), `~flight0`/`~flight1`, `~baud N`.

## Reading the ESP log without `idf.py monitor`

Opening COM6 with pyserial toggles DTR/RTS and **resets the ESP32** (auto-reset circuit).
The modem is not reset. Minimal reader:

```python
import serial, time
s = serial.Serial("COM6", 115200, timeout=0.2)
s.dtr = False; s.rts = True; time.sleep(0.1); s.rts = False   # clean reset
t = time.time()
while time.time() - t < 60:
    print(s.read(4096).decode("utf-8", "replace"), end="")
```
