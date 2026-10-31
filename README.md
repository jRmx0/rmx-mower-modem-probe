# rmx-mower-modem-probe

Part of **Blade of Grass** (open-source robotic mower). A learning/diagnostic firmware for
the cellular link of the future rmx-mower: an ESP32 + SIM7600G-H board (LilyGo T-SIM7600)
that connects over LTE to **rmx-server** and talks to **rmx-backend-probe** with
**MQTT over TLS** and **HTTPS**. ESP-IDF v6.1, esp_modem (PPP), mbedTLS, esp-mqtt.

- **Docs:** [Modem-Probe](https://github.com/rmx-lt/blade-of-grass/wiki/Modem-Probe) on the wiki
  (remote commands, OTA, error codes) and the
  [knowledge index](https://github.com/rmx-lt/blade-of-grass/wiki/Modem-Knowledge)
  (board, modem, ESP-IDF, PPP, server contract, plan).
- **Work:** issues on the [Blade of Grass project](https://github.com/orgs/rmx-lt/projects/1).

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
main/        the probe firmware: board, cell (esp_modem PPP), mqtt_link, https_api, probe_proto, remote_cmd, gnss, ota
tools/       at_bridge (AT console firmware), at_session.py (AT script), ppp_smoke (PPP + TLS test),
             mqtt_cmd.ps1 (remote commands), release.ps1 (signed OTA release)
version.txt  firmware version (PROJECT_VER), bump it for every OTA release
certs/       ISRG Root X1/X2 PEMs (public CA roots for rmx.lt's Let's Encrypt chain)
```

## License

[Apache-2.0](LICENSE).
