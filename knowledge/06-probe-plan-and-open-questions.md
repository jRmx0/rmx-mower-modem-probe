# 06 — Plan for the probe firmware, risks, open questions

## Goal

`rmx-mower-modem-probe`: ESP-IDF firmware on the T-SIM7600 that connects over LTE to
rmx-server and talks to **rmx-backend-probe** over **MQTT over TLS** (`mqtt.rmx.lt:8883`) and
**HTTPS** (`api.rmx.lt/probe/…`). It is the device-side rehearsal of the future rmx-mower link.

## What is already proven on the hardware (2026-10-19)

- [x] ESP32 ⇄ SIM7600 UART on GPIO 27 (TX) / 26 (RX), 115200, no flow control
- [x] Modem power/state handling (already-on detection, FLIGHT high, DTR low)
- [x] SIM ready, LTE registration on Tele2 LT (APN `internet`)
- [x] PPP with esp_modem 2.1.0 on ESP-IDF v6.1, IP in ~160 ms after dial
- [x] HTTPS GET `/probe/health` → 200, mbedTLS + cert bundle verification
- [x] MQTTS TLS handshake to 8883 + MQTT CONNECT reaches Mosquitto (fake login → CONNACK 5)
- [x] Recovery after an ESP reset while the modem is in PPP
- [x] Fallback: modem-internal TLS (AT+CCH*) with CA verification to both hosts

### The probe firmware itself (repo root, `main/`) — verified 2026-10-19, 2 runs

- [x] MQTT login `probe-device` over TLS (CONNACK in ~3.6 s incl. handshake), SUBACK on
      `probe/res/probe-device`, retained `probe/status/server` = `online` received
- [x] MQTT `create` on `probe/req/probe-device` → answer on `probe/res/probe-device` in ~240 ms
      (`connection_type: mqtt`), plus the `probe/events/records` event; `list` in ~370 ms
- [x] HTTPS: health 200 (3.7 s incl. TLS), login 200 + `__Secure-probe_session` cookie (~310 ms),
      `POST /api/records` with `Origin: https://api.rmx.lt` + cookie → 201 (`connection_type: https`),
      `GET /api/records` 200 (~310 ms). The keep-alive connection reuses one TLS session.
- [x] ESP reset while the modem was in PPP → next boot connects normally (PPP in ~150 ms)
- [x] 3 min online afterwards: PPP and MQTT stayed up (keepalive 60 s), heap stable
      (4.39 MB free, 284 KB internal)
- App image 833 KB in a 4 MB OTA slot.

### Remote commands over MQTT — verified 2026-10-19

The device subscribes `mower/probe-device/cmd` (QoS 1) and answers on
`mower/probe-device/cmd/ack`: the planned mower command path, rehearsed. Code: `main/remote_cmd.c`;
PC sender: `tools/mqtt_cmd.ps1` (login `app`, the backend role, via `mosquitto_rr`).

```jsonc
// in:  mower/probe-device/cmd
{"id":"pc-1a2b","cmd":"create","value":"hi","expires_at":1792443479}
// out: mower/probe-device/cmd/ack
{"id":"pc-1a2b","cmd":"create","ok":true,"result":{"kind":"record","record_id":41,"connection_type":"mqtt"},"ms":479,"time":1792443419}
{"id":"pc-1a2b","cmd":"create","ok":false,"error":"expired","message":"…","ms":0,"time":…}
```

- Commands: `ping`, `status` (uptime, PPP, rssi, heap, clock), `create`/`list`/`delete` (forwarded
  to the probe server over MQTT), `https_create`/`https_list` (over HTTPS; logs in again once on
  failure), `restart` (acks, then leaves PPP and restarts).
- Errors: `bad_request`, `unknown_cmd`, `expired`, `no_clock`, `timeout`, `failed`, or the probe
  server's own code. Duplicate ids (QoS 1 redelivery; last 8 kept) are ignored without a reply.
- `expires_at` (unix s, optional): refused when past, or when the clock isn't set. The clock comes
  from SNTP (`pool.ntp.org`) over PPP right after boot (worked first try, < 4 s).
- Measured PC → device → PC round trip (incl. a fresh TLS connect of `mosquitto_rr`): ~0.5–1 s;
  `create` ~0.5 s on the device, `https_create` ~3.7 s (new TLS handshake, the HTTPS client is
  closed after the self-test). After `restart` the device was back and answering in ~30 s.
- Commands run on their own task (queue of 4): the MQTT task must never block, because a
  forwarded request waits for its response on that same task.
- `"gps": true` on `create` / `https_create` (`tools/mqtt_cmd.ps1 -Gps`, verified 2026-10-20): the value
  sent is the GNSS location as a JSON string, e.g.
  `{"gps":"fix","lat":…,"lon":…,"alt":…,"utc":"2026-10-20T09:19:18Z","sats":9,"hdop":0.8,"mode":"3D"}`,
  or `gps unavailable: <why>` (no fix (searching N s), GNSS was off / turned on now, AT+CGPS? / AT+CGNSSINFO
  error|timeout, PPP down, modem busy, +++ failed). The ack `result` gets `"gps"` (the status) and
  `"gps_fix"`. `gps` together with `value` → `bad_request`. Code: `main/gnss.c`; AT access by pausing PPP
  for ~2 s (04). GNSS is turned on at boot (`gnss_start()`).
  Verified: no-fix path over MQTT (record 50, 52) and HTTPS (record 51), records on the server hold
  `gps unavailable: no fix (searching 20 s)`; MQTT/PPP stayed up; `create` with gps ~2.2–2.3 s on the
  device, `https_create` ~5.6 s (new TLS). Fix path verified the same day with the board at a
  window: `create` → record 60, `https_create` → record 61, both with the JSON location (12 sats,
  HDOP 1.0), ack `"gps_fix":true,"gps":"fix (2D, 12 sats, <utc>)"`, ~2.2 s / ~5.8 s on the device.
  Indoors (antenna connected): no fix after 18 min (record 54).
- `mosquitto_rr` 2.0.20 has no `-f`; it silently sent an empty payload. Use `-m` (quote-escaped).
- Using `app` from the PC: on 8883 its client id is forced to `app`, so a real backend on that
  login would be kicked while the script runs (no backend runs yet).

### Firmware updates (OTA) — built 2026-10-22, NOT yet tested on the board

- `main/ota.c` + the `ota` command (README "Firmware updates"): esp_https_ota from
  `https://api.rmx.lt/fw/probe/<version>.bin` (Caddy, basic auth), resumed with HTTP Range
  after a drop (3 retries), header check (project, version, not the running one), size check.
- Rollback: `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE`. A new image confirms itself on the first
  MQTT CONNACK; without it within `CONFIG_PROBE_OTA_CONFIRM_S` (300 s) it rolls back.
- Signed apps without Secure Boot (decided by the user 2026-10-22: no Secure Boot, no eFuses):
  RSA-3072, key in gitignored `keys/ota_signing_key.pem`. Needs `CONFIG_ESP32_REV_MIN_3`
  (the board is v3.1). Verified in a build only: `espsecure verify-signature` passes with the key
  and fails with another one.
- Build 2026-10-22 (IDF v6.1): app 0xd1000 bytes (signed, 856 KB), **bootloader 0x6d20 bytes,
  only 0x2e0 (736 B) free** below the partition table at 0x8000. Any more bootloader features
  would need a lower bootloader log level or moving the partition table (= new layout, USB).
- To verify on the board: good update, bad image rolls back, crash on boot rolls back, link
  lost mid-download resumes, wrong key refused, power cut while writing.

Not yet done: reconnect handling verified under real signal loss (the code redials and
power-cycles the modem, untested), CMUX, sleep/power saving, modem MQTT AT path,
OTA on the hardware, secrets in NVS instead of sdkconfig, SNTP + certificate date checks.

## Firmware structure (implemented)

```
main/
  board.c/.h        pins, modem power on/off, STATUS
  cell.c/.h         esp_modem DTE/DCE, registration wait, PPP up/down, reconnect loop, events
  mqtt_link.c/.h    esp-mqtt client: mqtts://mqtt.rmx.lt:8883, LWT, subscribe, request/response
  https_api.c/.h    esp_http_client: login (cookie + Origin), records CRUD
  gnss.c/.h         SIM7600 GNSS over AT (via cell_with_command_mode): on at boot, read on demand
  remote_cmd.c/.h   commands from mower/<user>/cmd, acks on …/cmd/ack
  ota.c/.h          esp_https_ota download, image checks, first-boot confirm / rollback
  probe_proto.c/.h  JSON envelopes (espressif/cjson), request_id generation
  app_main.c        sequence: board → cell → https test → mqtt test → loop
idf_component.yml   espressif/esp_modem ^2.1, espressif/mqtt ^1.1, espressif/cjson ^1.7.19
Kconfig.projbuild   APN, MQTT user/password, HTTPS user/password (values in sdkconfig, NOT committed)
partitions.csv      16 MB layout with OTA slots
```

Start from [`tools/ppp_smoke/main/ppp_smoke.c`](../tools/ppp_smoke/main/ppp_smoke.c).

MQTT client settings to start with:
- `broker.address.uri = "mqtts://mqtt.rmx.lt:8883"`, `broker.verification.crt_bundle_attach = esp_crt_bundle_attach`
  (or `certificate = <isrg-root-x1+x2 PEM>` to trust only those roots)
- `credentials.username/password` from config; client id is forced to the username by the broker
- `session.keepalive = 60`, `session.last_will` → a status topic (for the probe: none of
  `probe/…` is writable by `probe-web` except `probe/req/+`; agree a topic with the user)
- `buffer.size` ≥ 16–64 KB if the device sends `list`
- QoS 1 for requests; match responses by `request_id`

## Decisions (answered by the user 2026-10-19)

1. **MQTT login**: a dedicated Mosquitto user **`probe-device`** (rmx-vps branch
   `probe-device-login`). ACL: write `probe/req/probe-device`; read `probe/res/probe-device`,
   `probe/events/#`, `probe/status/#` (plus the `mower/probe-device/#` pattern every login gets).
2. **HTTPS login**: a dedicated web user **`probe-device`** (`probe-server create-user`).
3. **Secrets**: both passwords live in the gitignored `sdkconfig` (`CONFIG_PROBE_MQTT_PASSWORD`,
   `CONFIG_PROBE_WEB_PASSWORD`); plain-text copies only in the VPS `credentials.txt` files.
   Copy them into `sdkconfig` with ssh without printing them. NVS later.
4. Status / last will: `mower/probe-device/status` (retained `online` / `offline`), the only
   status topic the ACL lets `probe-device` write.
5. Remote commands from the PC use the existing `app` login (no new VPS user).

Still open: modem firmware update B04 → B06 (not needed so far); whether the probe should
exercise the real `mower/mower-01/…` topics.

## Risks / gotchas to remember

- esp_modem default pins hit FLIGHT (25) and our TX (27) — always override.
- ESP and modem reset independently; PWRKEY pulse on a running modem turns it off.
- TLS handshakes cost ~4.3 s each over LTE: reuse connections.
- The default 1 MB app partition is nearly full with this stack (808 KB).
- CGNAT: no inbound connections; MQTT keepalive must be shorter than the carrier's NAT
  idle timeout (often minutes) — 60 s is a safe start.
- Let's Encrypt chain changes: keep ISRG Root X1 + X2.
- Mosquitto silently ignores forbidden SUBSCRIBE/PUBLISH: if nothing arrives, check the ACL first.
- SIM must be in before power-up; antenna on MAIN (CSQ 99 = no antenna).
