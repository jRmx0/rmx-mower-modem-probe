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
and creates/lists records. It also takes remote commands from the PC on
`mower/probe-device/cmd` and replies on `…/cmd/ack`. Details: [knowledge/06](knowledge/06-probe-plan-and-open-questions.md).

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

## Send a command from the PC

```powershell
tools\mqtt_cmd.ps1 ping
tools\mqtt_cmd.ps1 status
tools\mqtt_cmd.ps1 create -Value "hello from the PC"
tools\mqtt_cmd.ps1 https_create -Value "via HTTPS"
tools\mqtt_cmd.ps1 create -Gps          # value = GPS location, or "gps unavailable: <why>"
tools\mqtt_cmd.ps1 https_create -Gps
tools\mqtt_cmd.ps1 delete -RecordId 41
tools\mqtt_cmd.ps1 restart
tools\mqtt_cmd.ps1 ota -Version 0.2.1 -Size 851234   # see "Firmware updates"
```

Commands: `ping status create list delete https_create https_list ota restart`. Each carries
`expires_at` (now + `-TtlS`, default 60 s). `-Gps` (create / https_create) sends the device's
GNSS location instead of a value; without a fix it sends `gps unavailable: …` and says why.
Needs the Mosquitto clients for Windows and `ssh rmx-vps`.

## Firmware updates (OTA)

The device downloads a new image over HTTPS into the spare app slot (`partitions.csv`: two
4 MB slots), checks it, boots it, and keeps it only if it reaches the MQTT broker within
`CONFIG_PROBE_OTA_CONFIRM_S` (300 s). Otherwise the previous image boots again. Code:
[`main/ota.c`](main/ota.c).

- **Signed images.** OTA images must be signed with our RSA-3072 key (Secure Boot V2 scheme,
  `CONFIG_SECURE_SIGNED_APPS_NO_SECURE_BOOT`). The running firmware only accepts an update
  signed with the same key as itself. There is no hardware Secure Boot: no eFuses are burned,
  and USB flashing works as before.
- **Hosting.** `https://api.rmx.lt/fw/probe/<version>.bin` on the VPS (Caddy, HTTP basic
  auth, rmx-vps README). Only URLs under `CONFIG_PROBE_OTA_URL_PREFIX` are accepted.
- **Retries.** A dropped download resumes where it stopped (HTTP Range), up to 3 times; the
  running firmware is untouched until the whole image checks out.

One-time setup (the bootloader can't be updated over the air, so this needs USB):

```powershell
mkdir keys
espsecure generate-signing-key --version 2 --scheme rsa3072 keys\ota_signing_key.pem
# back up keys\ota_signing_key.pem offline: without it, no more OTA (only a USB flash with a new key)
idf.py menuconfig           # "rmx probe": firmware download password (Caddy basic auth login probe-device)
idf.py -p COM6 -b 921600 flash
```

An existing `sdkconfig` keeps its old values: add the OTA block from `sdkconfig.defaults`
to it (or set the same options in menuconfig) before that build.

Each release:

```powershell
# bump version.txt (the device refuses the version it already runs)
tools\release.ps1 -Send    # build + sign, upload to the VPS, send the ota command
tools\mqtt_cmd.ps1 status  # result.version, result.ota (state, progress, error), rolled_back_from
```

The `ota` command: `{"id":"…","cmd":"ota","url":"https://api.rmx.lt/fw/probe/0.2.1.bin","version":"0.2.1","size":851234}`.
It acks `started` at once, then `done` before the restart, or an error: `busy`, `same_version`,
`low_signal`, `bad_image` (wrong project or version, bad signature or hash), `bad_size`,
`download_failed`, `failed`. One update is about 0.9 MB of mobile data.

## Layout

```
main/        the probe firmware: board, cell (esp_modem PPP), mqtt_link, https_api, probe_proto, remote_cmd, gnss, ota
knowledge/   knowledge base about the board, modem, ESP-IDF, esp_modem and the server contract
             → start with knowledge/INDEX.md
tools/       at_bridge (AT console firmware), at_session.py (AT script), ppp_smoke (PPP + TLS test),
             mqtt_cmd.ps1 (remote commands), release.ps1 (signed OTA release)
version.txt  firmware version (PROJECT_VER), bump it for every OTA release
certs/       ISRG Root X1/X2 PEMs (public CA roots for rmx.lt's Let's Encrypt chain)
```

## License

[Apache-2.0](LICENSE).
