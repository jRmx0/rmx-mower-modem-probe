# rmx-mower-modem-probe

Part of **Blade of Grass** (open-source robotic mower). A learning/diagnostic firmware for
the cellular link of the future rmx-mower: an ESP32 + SIM7600G-H board that connects over LTE
to **rmx-server** and talks to **rmx-backend-probe** with **MQTT over TLS** and **HTTPS**.

- Hardware: LilyGo T-SIM7600 (ESP32-WROVER-E N16R8, SIM7600G-H)
- Framework: ESP-IDF v6.1, esp_modem (PPP), mbedTLS, esp-mqtt, esp_http_client
- Server: `https://api.rmx.lt/probe/…`, `mqtts://mqtt.rmx.lt:8883`

## Status

Probe firmware working on the hardware (2026-10-19): over LTE (PPP) it logs in to the
broker as `probe-device`, sends `create`/`list` on `probe/req/probe-device` and gets the
answers on `probe/res/probe-device`; over HTTPS it logs in (session cookie + `Origin`)
and creates/lists records. Details: [knowledge/06](knowledge/06-probe-plan-and-open-questions.md).

## Build, configure, flash (PowerShell)

```powershell
. C:\Espressif\tools\Microsoft.v6.1.PowerShell_profile.ps1
idf.py set-target esp32     # first time only
idf.py menuconfig           # "rmx probe": APN, MQTT and web logins (passwords stay in sdkconfig)
idf.py build
idf.py -p COM6 -b 921600 flash
python -I -u tools\serial_log.py COM6 120 --until SELF-TEST
```

`sdkconfig` holds the passwords and is gitignored: never commit it.

## Layout

```
main/        the probe firmware: board, cell (esp_modem PPP), mqtt_link, https_api, probe_proto
knowledge/   knowledge base about the board, modem, ESP-IDF, esp_modem and the server contract
             → start with knowledge/INDEX.md
tools/       at_bridge (AT console firmware), at_session.py (AT script), ppp_smoke (PPP + TLS test)
certs/       ISRG Root X1/X2 PEMs (public CA roots for rmx.lt's Let's Encrypt chain)
```

## License

[Apache-2.0](LICENSE).
