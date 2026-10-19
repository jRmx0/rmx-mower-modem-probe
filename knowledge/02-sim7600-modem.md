# 02 — SIM7600G-H modem

SIMCom LTE Cat-4 module (150/50 Mbit/s), 3G/2G fallback, GNSS. Local manuals
(PDF): `C:\Repos\LilyGo-Modem-Series\datasheet\SIM7600X\` — AT Command Manual V3.00,
UART, Sleep, GPIO, MQTT(S), HTTP(S), FTP(S), GNSS, FOTA application notes.
SIMCom site: <https://www.simcom.com/product/SIM7600X.html>.

## Our unit (verified 2026-10-19)

| | |
|---|---|
| Model | `SIMCOM_SIM7600G-H` (`ATI`), P/N S2-1097D-Z31E3 |
| Firmware | `LE20B04SIM7600G22` (`AT+SIMCOMATI`). Newer for this P/N: B05, B06 (LilyGo `docs/en/esp32/sim7600-esp32/upgrade/sim7600_upgrade.md`). **Only flash firmware for P/N S2-1097D**, or it bricks. Not needed so far: everything we tested works on B04. |
| UART | 115200 8N1 (`AT+IPR?` = 115200), no flow control (`AT+IFC?` = 0,0) |
| USB PID mode | 9001 (`AT+CUSBPIDSWITCH?`) — only matters in USB modem mode |
| SIM | Tele2 Lithuania prepaid "PILDYK" (MCC/MNC 246-03), `+CPIN: READY`, no PIN |
| APN | `internet` (PDP context 1 is already `"IP","internet"`) |
| Network | LTE, EUTRAN band 1, `+CSQ: 20,99`, `+COPS: 0,0,"Tele2 LT PILDYK",7` |
| IP | private CGNAT address (10.x.x.x). The device can only make **outbound** connections. |
| Clock | NITZ works (`AT+CTZU=1`), `AT+CCLK?` gave correct local time (+12 quarter-hours = UTC+3) after registration. Before registration it shows `80/01/06`. |
| Radio mode | `AT+CNMP=2` (automatic) |

Do not commit IMEI, ICCID or IMSI anywhere (public repos).

## Power and control (board-specific, see 01)

- **On**: if `AT` gets no answer → PWRKEY (GPIO4) HIGH 500 ms → modem boots in ~10–15 s
  (LilyGo waits up to 15 s; `MODEM_START_WAIT_MS`). It prints `RDY`, `+CPIN: READY`,
  `SMS DONE`, `PB DONE` URCs while booting.
- **Off**: `AT+CPOF` (esp_modem `power_down()`), or PWRKEY HIGH ≥ 3 s.
- A PWRKEY pulse on a running modem **turns it off** → always probe with `AT` first.
- FLIGHT (GPIO25) HIGH is required; `AT+CFUN=1` = full function, `AT+CFUN=4` = RF off.
- DTR (GPIO32) LOW = awake. Sleep: `AT+CSCLK=1`, then DTR HIGH lets the modem sleep;
  DTR LOW wakes it. RI (GPIO33) pulses on URCs/SMS/calls.
- The ESP32 resetting does **not** reset the modem. The modem may still be in PPP
  data mode from the previous boot → firmware must detect this
  (`esp_modem_set_mode(ESP_MODEM_MODE_DETECT)`) or send `+++` and wait.

## Registration checklist

```
AT                → OK
AT+CPIN?          → +CPIN: READY          (SIM inserted before power-up!)
AT+CSQ            → +CSQ: <rssi>,<ber>    99 = no signal (antenna on MAIN?)
AT+CEREG?         → +CEREG: 0,1 (home) / 0,5 (roaming); 0,2 searching; 0,4 unknown
AT+CPSI?          → LTE,Online,246-03,…,EUTRAN-BAND1,…
AT+COPS?          → +COPS: 0,0,"Tele2 LT PILDYK",7     (7 = LTE)
AT+CGDCONT?       → 1,"IP","internet"
```

RSSI map: `CSQ` 0 = −113 dBm, 2..30 = −109..−53 dBm (2 dB steps), 31 = ≥ −51 dBm, 99 = unknown.
Without the antenna: `CSQ 99`, `CPSI: NO SERVICE`, a full scan (`AT+COPS=?`, ~1–3 min)
found only one weak foreign GSM cell.

## Two ways to get data through the modem

| | A. PPP (ESP32 does TCP/IP + TLS) | B. Modem-internal stack (AT commands) |
|---|---|---|
| How | `ATD*99#` → PPP; lwIP on the ESP32; esp_modem component | `AT+NETOPEN`, `AT+CCHOPEN` (TLS sockets), `AT+CMQTT*`, `AT+HTTP*` |
| TLS | mbedTLS on the ESP32: TLS 1.2/1.3, full control, cert bundle | modem's TLS: **max TLS 1.2**, SNI **off by default**, CA file must be uploaded |
| MQTT/HTTP libs | esp-mqtt (`espressif/mqtt`), `esp_http_client` — standard, testable | parse AT URCs yourself (TinyGSM does this in Arduino) |
| AT while online | needs CMUX (`AT+CMUX=0`, supported) or pausing PPP | always available |
| Status | **chosen path** for the probe (see 04) | **verified working** as a fallback (below) |

### B. Modem TLS — verified commands (2026-10-19, firmware B04)

Socket service, CA upload, verified TLS 1.2 to `api.rmx.lt:443` and `mqtt.rmx.lt:8883`:

```
AT+NETOPEN                                  → OK, +NETOPEN: 0   (PDP up; AT+IPADDR shows IP)
AT+CDNSGIP="api.rmx.lt"                     → +CDNSGIP: 1,"api.rmx.lt","45.81.252.43"
AT+CCERTDOWN="isrgx1.pem",1935              → >   then send the PEM bytes (exact length) → OK
AT+CCERTLIST                                → +CCERTLIST: "isrgx1.pem"
AT+CCHSTART                                 → +CCHSTART: 0
AT+CSSLCFG="sslversion",0,3                 (3 = TLS 1.2; 4 = all)
AT+CSSLCFG="authmode",0,1                   (1 = verify server with CA)
AT+CSSLCFG="ignorelocaltime",0,1            (1 = don't check validity dates)
AT+CSSLCFG="enableSNI",0,1                  (REQUIRED for Caddy virtual hosts; default 0)
AT+CSSLCFG="cacert",0,"isrgx1.pem"
AT+CCHSSLCFG=0,0                            (session 0 uses SSL context 0)
AT+CCHOPEN=0,"api.rmx.lt",443,2             → OK, +CCHOPEN: 0,0    (2 = TLS)
AT+CCHSEND=0,67                             → >   then exactly 67 bytes of HTTP request
                                            → +CCHRECV: DATA,0,198 … HTTP/1.1 200 OK … {"status":"ok",…}
                                            → +CCH_PEER_CLOSED: 0
AT+CCHCLOSE=0 / AT+CCHSTOP
```

- Negative test: with a wrong CA (`DigiCert Global Root G2`) `+CCHOPEN: 0,15` = handshake
  failed → the modem really verifies the chain.
- The modem handles the ECDSA P-256 leaf + P-384 intermediates of Let's Encrypt's
  Gen-Y chain fine (cipher was ECDHE-ECDSA-AES256-GCM-SHA384 in a PC test at TLS 1.2).
- `AT+CCHSET=1,0` returned ERROR after `CCHSTART` (set it before `CCHSTART` if needed).
  Without it, received data arrives as `+CCHRECV: DATA,<id>,<len>` URCs.
- `isrgx1.pem` is now stored in the modem's file system (survives power cycles).
- The CCHSEND length must be exact; a short length silently truncates the request.

MQTT over the modem (from the MQTT(S) app note + LilyGo TinyGSM `TinyGsmMqttA76xx.h`,
not yet run on our unit):

```
AT+CMQTTSTART                                 → +CMQTTSTART: 0
AT+CMQTTACCQ=0,"<clientid>",1                 (1 = SSL client)
AT+CMQTTSSLCFG=0,0                            (SSL context 0, configured as above)
AT+CMQTTWILLTOPIC=0,<len> / AT+CMQTTWILLMSG=0,<len>,1
AT+CMQTTCONNECT=0,"tcp://mqtt.rmx.lt:8883",60,1,"<user>","<pass>"  → +CMQTTCONNECT: 0,0
AT+CMQTTSUBTOPIC=0,<len>,1 + AT+CMQTTSUB=0     → +CMQTTSUB: 0,0
AT+CMQTTTOPIC=0,<len> + AT+CMQTTPAYLOAD=0,<len> + AT+CMQTTPUB=0,1,60 → +CMQTTPUB: 0,0
RX: +CMQTTRXSTART / +CMQTTRXTOPIC / +CMQTTRXPAYLOAD / +CMQTTRXEND
AT+CMQTTDISC=0,120 / AT+CMQTTREL=0 / AT+CMQTTSTOP
```

Note: the URL is always `tcp://` even for TLS; TLS is selected by ACCQ's 3rd arg + SSLCFG.

## Useful AT commands (tested on our unit unless marked)

| Command | Purpose / our result |
|---|---|
| `ATI`, `AT+SIMCOMATI`, `AT+CGMR` | model / firmware |
| `AT+CBC` | battery voltage: `+CBC: 3.838V` (esp_modem parses this format for SIM7xxx) |
| `AT+CPMUTEMP` | module temperature: 22 °C |
| `AT+CNMP?` / `=2` auto, `=38` LTE only, `=13` GSM only | network mode |
| `AT+CNBP?` | band masks |
| `AT+CMUX=?` | `(0),(0),(1-8),(1-1500),…` — basic mode CMUX supported |
| `AT+CSSLCFG=?` | lists TLS options (sslversion 0-4, authmode 0-3, SNI, ciphersuites 0x002F/0xFFFF) |
| `AT+CGPS=1` / `AT+CGPSINFO` | GNSS on / fix (needs active GPS antenna); `+CGPS: 0,1` = off |
| `AT+CTZU=1`, `AT+CCLK?` | network time |
| `AT+CNTP="pool.ntp.org",0` + `AT+CNTP` | NTP sync in the modem (not tested) |
| `AT&W` | save settings |
| `AT+CRESET` | reboot the modem (not tested) |

## Known quirks

- `+CSQ: 99,99` + `NO SERVICE` = antenna not on MAIN or no coverage.
- SIM hot-plug not detected → power-cycle the modem.
- Some SIMCom firmware versions have MQTT/TLS bugs (LilyGo `docs/simcom_firmware_bug.md`
  lists A7670/SIM7670 ones; none listed for SIM7600G-H).
- LilyGo marks `SecureClient` (TinyGSM SSL client), `HttpsBuiltlnPut`, `ModemGpsStream` etc. as
  skipped for T-SIM7600X (`examples/*/.skip.T-SIM7600X`). The built-in MQTTS and HTTPS GET/POST examples do support it.
