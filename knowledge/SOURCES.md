# Sources

## Measured on the hardware (2026-10-19)

- `esptool flash-id` on COM6: chip, flash size.
- `tools/at_bridge` boot log: PSRAM; AT session (`tools/at_session.py`): modem identity,
  firmware, SIM, registration, TLS tests (`AT+CCH*`), negative CA test.
- `tools/ppp_smoke`: PPP, HTTPS, MQTTS results (3 runs incl. reset during PPP).
- `openssl s_client` from the PC against `api.rmx.lt:443` and `mqtt.rmx.lt:8883`: chain, TLS 1.2 cipher.

## Local repositories

- LilyGo-Modem-Series (`C:\Repos\LilyGo-Modem-Series`, HEAD 51b988a):
  `docs/en/esp32/sim7600-esp32/README.MD`, `docs/en/esp32/sim7600-esp32/upgrade/sim7600_upgrade.md`,
  `docs/simcom_firmware_bug.md`, `examples/ATdebug/{ATdebug.ino,utilities.h}`,
  `examples/MqttsBuiltlnSSL`, `lib/TinyGSM/src/*`, `schematic/esp32/T-SIM7600X-V1.3.pdf`,
  `datasheet/SIM7600X/*.pdf` (AT manual V3.00, UART, Sleep, MQTT(S), HTTP(S) app notes).
- esp-protocols (`C:\Repos\esp-protocols`, HEAD 5c4650d0): `components/esp_modem` 2.1.0 —
  `include/esp_modem_config.h` (DTE defaults), `src/esp_modem_command_library.cpp`,
  `command/src/esp_modem_modules.cpp`, `examples/pppos_client`, `examples/simple_cmux_client`, `Kconfig`.
- ESP-IDF v6.1 (`C:\esp\v6.1\esp-idf`): component list (no `mqtt`, no `json`), cert bundle.
- Blade of Grass: `C:\Projects\blade-of-grass\README.md`, `rmx-server\CLAUDE.md`,
  `rmx-backend-probe` (README, `crates/probe-protocol/src/lib.rs`, `crates/probe-server/src/{mqtt.rs,http.rs,auth/*}`),
  `rmx-vps/services/{mosquitto/config/*,caddy/Caddyfile}`.

## Web

- LilyGo product page: <https://lilygo.cc/products/t-sim7600?variant=42774480224437> (variants incl. 16 MB G-H R2)
- LilyGo wiki: <https://wiki.lilygo.cc/products/t-sim-series/t-sim7600x/> (pin map, says WROVER-B N4R8 — outdated for our unit)
- LilyGo repo: <https://github.com/Xinyuan-LilyGO/LilyGo-Modem-Series>
- esp_modem docs: <https://docs.espressif.com/projects/esp-protocols/esp_modem/docs/latest/README.html>
- ESP-IDF v6.0 protocols migration (MQTT, cJSON moved to the registry):
  <https://docs.espressif.com/projects/esp-idf/en/release-v6.0/esp32/migration-guides/release-6.x/6.0/protocols.html>
- Espressif DevCon23 modem talk: <https://dl.espressif.com/public/DevCon23_Talk_Modems_Final.pdf>
- Forum reports (no confirmed fixes; background on pitfalls): esp32.com
  <https://esp32.com/viewtopic.php?p=103856> (UART timeouts on LilyGo GPIO26/27),
  <https://esp32.com/viewtopic.php?p=117488>, <https://esp32.com/viewtopic.php?p=146549> (CMUX truncation),
  esp-protocols issues <https://github.com/espressif/esp-protocols/issues>.
- SIMCom SIM7600 product page: <https://www.simcom.com/product/SIM7600X.html>
