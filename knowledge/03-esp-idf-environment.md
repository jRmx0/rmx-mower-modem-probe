# 03 — ESP-IDF environment on the dev PC

## Installed toolchain (verified 2026-10-19)

| | |
|---|---|
| ESP-IDF | **v6.1** at `C:\esp\v6.1\esp-idf` (installed with EIM, `eim_idf.json` in `C:\Espressif\tools`) |
| Tools | `C:\Espressif\tools` (xtensa-esp-elf, cmake 4.0.3, ninja, ccache, openocd) |
| Python venv | `C:\Espressif\tools\python\v6.1\venv\Scripts\python.exe` (has esptool 5.5.0, pyserial) |
| Activation (PowerShell) | `. C:\Espressif\tools\Microsoft.v6.1.PowerShell_profile.ps1` |
| Serial port | **COM6** = CH9102 (ESP32 console/flash) |
| Other repos | `C:\Repos\esp-protocols` (esp_modem source + examples), `C:\Repos\LilyGo-Modem-Series` |

The activation script sets `IDF_COMPONENT_LOCAL_STORAGE_URL=file://C:\Espressif\tools`
(a local mirror under `C:\Espressif\tools\components`, which has `espressif/mqtt 1.1.0`
and `espressif/cjson 1.7.19~2` but **not** `esp_modem`). The component manager still
falls back to the online registry: `espressif/esp_modem 2.1.0` downloaded fine.
Offline alternative: `override_path: "C:/Repos/esp-protocols/components/esp_modem"`.

### Build / flash / monitor (PowerShell)

```powershell
. C:\Espressif\tools\Microsoft.v6.1.PowerShell_profile.ps1
cd <project>
idf.py set-target esp32        # once (creates sdkconfig from sdkconfig.defaults)
idf.py build
idf.py -p COM6 -b 921600 flash
idf.py -p COM6 monitor         # interactive; agents: use a pyserial script instead
```

Agents can't use the interactive monitor. Read the log with pyserial (opening the port
resets the ESP32 — see `tools/README.md`). Pipe `idf.py` output to a file and read it;
`| Select-Object -Last N` buffers until the command ends.

Read-only chip check: `python -m esptool --port COM6 flash-id`.

## ESP-IDF v6 changes that matter here

- **MQTT is no longer in IDF.** Add `espressif/mqtt` (`idf.py add-dependency espressif/mqtt`
  or in `main/idf_component.yml`). Headers/API (`mqtt_client.h`, `esp_mqtt_client_config_t`) unchanged.
- **cJSON (`json` component) is no longer in IDF.** Use `espressif/cjson` (`^1.7.19`).
  Remove `json` from `REQUIRES`.
- Drivers are split (`esp_driver_uart`, `esp_driver_gpio`, …): list them in
  `PRIV_REQUIRES`.
- Source: IDF v6.0 migration guide, protocols section
  (<https://docs.espressif.com/projects/esp-idf/en/release-v6.0/esp32/migration-guides/release-6.x/6.0/protocols.html>).

## sdkconfig.defaults for this board

```
CONFIG_IDF_TARGET="esp32"
CONFIG_ESPTOOLPY_FLASHSIZE_16MB=y
CONFIG_ESPTOOLPY_FLASHMODE_QIO=y        # verified: bootloader enables QIO, 80 MHz
CONFIG_ESPTOOLPY_FLASHFREQ_80M=y
CONFIG_SPIRAM=y                          # 8 MB found, 4 MB mapped into the heap
CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ_240=y
CONFIG_ESP_MAIN_TASK_STACK_SIZE=8192
# PPP (esp_modem)
CONFIG_LWIP_PPP_SUPPORT=y
CONFIG_LWIP_PPP_NOTIFY_PHASE_SUPPORT=y
CONFIG_LWIP_PPP_PAP_SUPPORT=y
CONFIG_LWIP_PPP_ENABLE_IPV6=n
CONFIG_LWIP_TCPIP_TASK_STACK_SIZE=4096
CONFIG_COMPILER_CXX_EXCEPTIONS=y        # esp_modem C++ core
# TLS roots
CONFIG_MBEDTLS_CERTIFICATE_BUNDLE=y
CONFIG_MBEDTLS_CERTIFICATE_BUNDLE_DEFAULT_FULL=y
```

Notes:
- The default partition table has a **1 MB** factory app. The smoke test
  (esp_modem + PPP + esp_http_client + esp-mqtt + full cert bundle) is already
  **808 KB** (`idf.py size`). For the real firmware use a custom partition table
  (16 MB flash allows two 3–4 MB OTA slots + NVS + storage).
- PSRAM: "Virtual address not enough for PSRAM, map as much as we can. 4MB is mapped" is
  normal on ESP32. Use `CONFIG_SPIRAM_BANKSWITCH_ENABLE` + himem API only if > 4 MB is needed.
- After PPP + HTTPS + MQTTS the smoke test had ~4.4 MB free heap (mostly PSRAM).
  Internal RAM is what matters for TLS buffers; `CONFIG_SPIRAM_TRY_ALLOCATE_WIFI_LWIP`
  / `CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC` can move mbedTLS buffers to PSRAM if internal RAM runs low.
- `CONFIG_MBEDTLS_HAVE_TIME_DATE` is off by default → mbedTLS does not check certificate
  validity dates. Good enough for the probe; production should sync time (SNTP over PPP,
  or the modem clock `AT+CCLK?`) and enable it.
- UART0 (GPIO1/3) is the console. The modem uses UART1 (any free UART works; esp_modem default is UART_NUM_1).
