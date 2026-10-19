# 04 — esp_modem + PPP on this board (the chosen data path)

Component: `espressif/esp_modem` **2.1.0** (registry) · source + examples:
`C:\Repos\esp-protocols\components\esp_modem` · docs:
<https://docs.espressif.com/projects/esp-protocols/esp_modem/docs/latest/index.html>.
Working reference code: [`tools/ppp_smoke/main/ppp_smoke.c`](../tools/ppp_smoke/main/ppp_smoke.c).

## Why PPP

The modem becomes a plain IP pipe. lwIP, DNS, TLS (mbedTLS), MQTT (esp-mqtt) and HTTP
(esp_http_client) all run on the ESP32 with standard ESP-IDF APIs. That gives TLS 1.3/1.2,
the Mozilla cert bundle, normal error codes, and code that also works over Wi-Fi.
The modem's own TLS (02, option B) works too but is a fallback.

## Verified result (2026-10-19, 3 runs)

```
module SIMCOM_SIM7600G-H, rssi 19..31
PPP up: ip 10.57.66.246 gw 10.64.64.64 dns 83.181.60.98      (connect ~160 ms after ATD)
HTTPS GET https://api.rmx.lt/probe/health -> 200 in ~4.4 s: {"status":"ok","db":true,"mqtt":true,"version":"0.1.0"}
mqtts://mqtt.rmx.lt:8883, invalid login -> "Connection refused, not authorized" (CONNACK 5) in ~4.3 s
free heap 4.44 MB (incl. PSRAM)
```

Each TLS handshake takes ~4.3 s (LTE round trips + ECDSA P-384 chain verification in
software). Keep connections open (MQTT keepalive, HTTP keep-alive) instead of reconnecting.

Also verified: resetting the ESP32 while the modem was in PPP data mode → the next boot
still connected normally (the `DETECT` branch didn't log a data-mode switch, so the exact
recovery path is not characterized; keep the DETECT + sync + PWRKEY fallback sequence).

## The board-specific config (most important part)

`ESP_MODEM_DTE_DEFAULT_CONFIG()` defaults are **wrong for this board**:
`tx 25, rx 26, rts 27, cts 23`. GPIO25 is FLIGHT and GPIO27 is our TX. Override all four:

```c
esp_modem_dte_config_t dte = ESP_MODEM_DTE_DEFAULT_CONFIG();
dte.uart_config.tx_io_num  = 27;
dte.uart_config.rx_io_num  = 26;
dte.uart_config.rts_io_num = -1;              // not wired
dte.uart_config.cts_io_num = -1;              // not wired
dte.uart_config.flow_control = ESP_MODEM_FLOW_CONTROL_NONE;
dte.uart_config.rx_buffer_size = 8192;        // default 4096
dte.uart_config.tx_buffer_size = 2048;
dte.dte_buffer_size = 4096;
esp_modem_dce_config_t dce_cfg = ESP_MODEM_DCE_DEFAULT_CONFIG("internet");   // APN
esp_netif_config_t ppp_cfg = ESP_NETIF_DEFAULT_PPP();
esp_netif_t *ppp = esp_netif_new(&ppp_cfg);
esp_modem_dce_t *dce = esp_modem_new_dev(ESP_MODEM_DCE_SIM7600, &dte, &dce_cfg, ppp);
```

The same wrong defaults are in the `pppos_client` example's Kconfig (TX 25, RTS 27).

Before esp_modem: GPIO25 (FLIGHT) HIGH, GPIO32 (DTR) LOW, GPIO4 (PWRKEY) LOW.

## Startup sequence (works)

1. `esp_netif_init()`, default event loop, register `IP_EVENT` handler
   (`IP_EVENT_PPP_GOT_IP` / `IP_EVENT_PPP_LOST_IP`).
2. Board pins (above), create DTE/DCE.
3. `esp_modem_set_mode(dce, ESP_MODEM_MODE_DETECT)`; if the mode is DATA → `set_mode(COMMAND)`.
4. `esp_modem_sync()`; if it fails → PWRKEY HIGH 500 ms, retry sync for ~30 s.
5. Wait for `esp_modem_get_signal_quality()` rssi ≠ 99 (registration).
6. `esp_modem_set_mode(dce, ESP_MODEM_MODE_DATA)` → sets PDP context (`AT+CGDCONT=1,"IP","internet"`)
   and dials `ATD*99#`; wait for `IP_EVENT_PPP_GOT_IP` (≤ 60 s).
7. Use sockets/TLS/MQTT/HTTP normally.
8. Before a planned reset: `esp_modem_set_mode(dce, ESP_MODEM_MODE_COMMAND)`.

## SIM7600 in esp_modem

`ESP_MODEM_DCE_SIM7600` (`command/src/esp_modem_modules.cpp`) adds over the generic DCE:
`get_battery_status` (`AT+CBC`, SIM7xxx "3.838V" format), `set_network_bands` (`AT+CNBP`),
`set_gnss_power_mode`, `power_down` (`AT+CPOF`). Data mode uses `ATD*99#` (generic).
Useful C API calls: `esp_modem_sync`, `esp_modem_get_signal_quality`, `esp_modem_get_imsi`,
`esp_modem_get_imei`, `esp_modem_get_module_name`, `esp_modem_get_operator_name`,
`esp_modem_read_pin`/`set_pin`, `esp_modem_at(dce, "AT+CPSI?", out, timeout)` (raw AT),
`esp_modem_pause_net(dce, true/false)` (temporarily leave PPP to send AT), `esp_modem_destroy`.

## AT commands while online: `esp_modem_pause_net()` (chosen, verified 2026-10-20)

The probe firmware uses `esp_modem_pause_net(dce, true)` → AT commands → `esp_modem_pause_net(dce, false)`,
wrapped in `cell_with_command_mode()` (`main/cell.c`) with a mutex shared with the redial watchdog.
What it does inside esp_modem 2.1.0: stop feeding lwIP frames to the UART, wait 1 s (escape guard
time), send `+++` (expects `OK`/`NO CARRIER`), then later `ATO` (expects `CONNECT`) and resume.

Measured: `+++` ~1.9–2.0 s (incl. the 1 s guard), GNSS AT commands 30–50 ms, `ATO` ~10 ms.
PPP resumed every time (4 sessions); the MQTT link and the HTTPS keep-alive connection survived:
the probe-server request right after the pause answered in ~400 ms. No reconnect in the log.
If `ATO` failed, the code puts the DCE into command mode and lets the watchdog redial (untested).
`esp_modem_at()` returns the last response line before `OK` (truncated to
`CONFIG_ESP_MODEM_C_API_STR_MAX` = 128 chars).

Why not CMUX: untested on this board, it changes how the whole DTE/redial path works
(mode transitions, exit issues reported on SIMCom 76xx), and GPS is read only on demand, so a
2 s pause per read is cheap. Revisit CMUX if AT is needed often (e.g. periodic GNSS + signal).

## AT commands while online: CMUX

`AT+CMUX=?` → `(0),(0),(1-8),(1-1500),…` — basic-mode CMUX works on the SIM7600.
`esp_modem_set_mode(dce, ESP_MODEM_MODE_CMUX)` gives one virtual channel for PPP and one
for AT (signal quality, GNSS) at the same time. Example: `examples/simple_cmux_client`.
Not tested on our board yet. Known reports: CMUX exit issues on A7670, truncated AT
answers under heavy PPP load on SIMCom 76xx (esp32.com forum). Without CMUX, the only
way to send AT while online is `esp_modem_pause_net()` (there is one UART, no second AT port).

## Robustness notes (for the real firmware)

- Handle `IP_EVENT_PPP_LOST_IP`: go to command mode, check registration, redial. Cap retries
  and power-cycle the modem (PWRKEY off/on) as the last resort.
- The ESP and modem reset independently. Never assume the modem state at boot.
- No hardware flow control: keep the baud at 115200 (fine for MQTT telemetry). Higher
  rates without RTS/CTS risk UART overruns; enable `CONFIG_UART_ISR_IN_IRAM` if raising it.
- `CONFIG_ESP_MODEM_ADD_DEBUG_LOGS=y` logs all UART traffic (debugging only).
- The cellular IP is CGNAT: inbound connections are impossible; MQTT (device-initiated) is the right model.
