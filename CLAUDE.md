# rmx-mower-modem-probe — notes for agents

Goal: ESP-IDF firmware on a LilyGo T-SIM7600 (ESP32 + SIM7600G-H) that reaches rmx-server over
LTE and talks to rmx-backend-probe with MQTT over TLS (`mqtt.rmx.lt:8883`) and HTTPS
(`api.rmx.lt/probe/…`).

**Read the wiki page `Modem-Knowledge` first** (local clone `C:\Projects\blade-of-grass.wiki`,
`git pull` it; online: https://github.com/rmx-lt/blade-of-grass/wiki/Modem-Knowledge). It indexes
verified facts about the board, the modem, the ESP-IDF setup, esp_modem/PPP, and the server contract
(pages `Modem-*`). The probe firmware is in `main/` (verified on hardware); the minimal reference is
`tools/ppp_smoke/main/ppp_smoke.c`. Update the wiki pages when you learn something new or find a fact
wrong (mark what you verified on hardware and when), then commit and push the wiki.

Work and open questions are GitHub issues in this repo, on the Blade of Grass project
(https://github.com/orgs/rmx-lt/projects/1), not notes in markdown files.

Reusable board, chip and ESP-IDF knowledge also lives in the central hub `C:\Repos\esp-knowledge`
(private repo `jRmx0/esp-knowledge`; read its `INDEX.md`). Its `boards/t-sim7600.md` has the header pinout and
free GPIOs; put new board or chip facts there, and keep probe-specific knowledge in the wiki (`Modem-*` pages).

## Environment

- Windows, PowerShell primary. ESP-IDF v6.1: `. C:\Espressif\tools\Microsoft.v6.1.PowerShell_profile.ps1`, then `idf.py`.
- Board on **COM6** (CH9102). Opening the port resets the ESP32 (not the modem).
- No interactive `idf.py monitor` for agents: read the serial log with a pyserial script (see the wiki page `Modem-Tools`).
- Related repos: `C:\Repos\esp-knowledge`, `C:\Repos\LilyGo-Modem-Series`, `C:\Repos\esp-protocols`, `C:\esp\v6.1\esp-idf`,
  `C:\Projects\blade-of-grass\rmx-server` (has its own `CLAUDE.md` with server rules).

## Must-know pitfalls

- esp_modem's default DTE pins (tx 25, rx 26, rts 27, cts 23) are wrong here: use TX 27, RX 26, no RTS/CTS.
- FLIGHT (GPIO25) HIGH, DTR (GPIO32) LOW. PWRKEY (GPIO4) HIGH 500 ms toggles power: send `AT` first.
- MQTT and cJSON are not in ESP-IDF v6: use `espressif/mqtt`, `espressif/cjson`.
- Broker forces client id = username; ACL limits each login to its topics. Forbidden SUB/PUB fail silently.
- HTTPS API needs `Origin: https://api.rmx.lt` on non-GET requests and a manually handled session cookie.

## Rules

- This repo is public (Apache-2.0): never commit passwords, tokens, IMEI/ICCID/IMSI, or personal data.
  Device credentials go in a gitignored `sdkconfig` / NVS. Server secrets stay on the VPS; never print them.
- VPS changes (new MQTT users, ACL) follow `rmx-server/CLAUDE.md` and need the user's OK.
- Flashing the board is fine (it's the dev board for this project); say what you flashed.
