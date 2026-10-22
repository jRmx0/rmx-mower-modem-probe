# Knowledge index — LilyGo T-SIM7600 + ESP-IDF + rmx-server

Start here. Facts marked "verified" were measured on the real board on 2026-10-19.

| # | File | Read it when you need… |
|---|---|---|
| 01 | [01-board-hardware.md](01-board-hardware.md) | pins, DIP switch, power, antennas, flash/PSRAM size, flashing tips |
| 02 | [02-sim7600-modem.md](02-sim7600-modem.md) | modem firmware, power sequence, registration, AT commands, modem-internal TLS/MQTT (verified) |
| 03 | [03-esp-idf-environment.md](03-esp-idf-environment.md) | toolchain paths, build/flash commands, IDF v6 changes, sdkconfig for this board |
| 04 | [04-esp-modem-ppp.md](04-esp-modem-ppp.md) | esp_modem + PPP: correct DTE config, startup sequence, CMUX, robustness |
| 05 | [05-server-contract.md](05-server-contract.md) | endpoints, cert chain, Mosquitto ACL, probe MQTT/HTTPS protocol, cookie + Origin rules |
| 06 | [06-probe-plan-and-open-questions.md](06-probe-plan-and-open-questions.md) | what is proven (incl. the probe firmware), firmware layout, decisions, risks |
| — | [SOURCES.md](SOURCES.md) | where every fact came from (local repos, PDFs, web) |
| — | [../tools/README.md](../tools/README.md) | AT bridge firmware, AT script, PPP smoke test |

## Quick facts (cheat sheet)

```
Board      LilyGo T-SIM7600 (ESP32), ESP32-WROVER-E N16R8: 16 MB flash, 8 MB PSRAM (4 MB mapped)
Modem      SIM7600G-H, fw LE20B04SIM7600G22, 115200 8N1, no RTS/CTS
UART       ESP TX = GPIO27, ESP RX = GPIO26 (UART1)
Control    PWRKEY GPIO4 (HIGH 500 ms = on/off toggle), FLIGHT GPIO25 = HIGH, DTR GPIO32 = LOW,
           RI GPIO33 (in), STATUS GPIO34 (in, 1 = on), LED GPIO12
PC         COM6 (CH9102); ESP-IDF v6.1: . C:\Espressif\tools\Microsoft.v6.1.PowerShell_profile.ps1
SIM/APN    Tele2 LT "PILDYK", APN "internet", LTE band 1, CGNAT IP
Data path  esp_modem 2.1.0 PPP (ESP_MODEM_DCE_SIM7600) + mbedTLS + espressif/mqtt + esp_http_client
Server     https://api.rmx.lt/probe/…  ·  mqtts://mqtt.rmx.lt:8883 (login required, client id := username)
Trust      ISRG Root X1 + X2 (LE Gen-Y ECDSA chain); SNI required for api.rmx.lt
Proven     PPP → HTTPS 200 /probe/health → MQTTS CONNACK (fake login refused) — tools/ppp_smoke
Firmware   main/ (repo root): login probe-device on MQTT + HTTPS, create/list both ways — verified
Logins     MQTT probe-device, web probe-device; passwords only in gitignored sdkconfig
Commands   PC → mower/probe-device/cmd → device → …/cmd/ack (tools/mqtt_cmd.ps1, login app) — verified
OTA        `ota` command → api.rmx.lt/fw/probe/<ver>.bin, signed (RSA-3072, no Secure Boot), rollback — built, not yet on hardware
GNSS       "gps": true on create/https_create; AT via esp_modem_pause_net (~2 s, links survive) — fix and no-fix verified
```

## Local reference material

| Path | Content |
|---|---|
| `C:\Repos\LilyGo-Modem-Series` | vendor repo: docs, schematic `schematic/esp32/T-SIM7600X-V1.3.pdf`, SIMCom PDFs in `datasheet/SIM7600X/`, Arduino examples (`examples/*/utilities.h` has pin maps), TinyGSM fork (`lib/TinyGSM/src/TinyGsmClientSIM7600.h`, `TinyGsmMqttA76xx.h`, `TinyGsmHttpsComm.h` = proven AT sequences) |
| `C:\Repos\esp-protocols\components\esp_modem` | esp_modem source, docs (`docs/`), examples `pppos_client`, `simple_cmux_client`, `modem_console`, `modem_tcp_client` |
| `C:\esp\v6.1\esp-idf` | ESP-IDF; cert bundle `components/mbedtls/esp_crt_bundle/cacrt_all.pem` |
| `C:\Projects\blade-of-grass\rmx-server` | `CLAUDE.md`, `rmx-backend-probe` (server code + protocol), `rmx-vps` (Caddy, Mosquitto config) |
