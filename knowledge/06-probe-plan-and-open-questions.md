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

Not yet done: real MQTT login + pub/sub on `probe/…`, HTTPS login + CRUD, CMUX, reconnect
handling under signal loss, sleep/power saving, modem MQTT AT path, GNSS.

## Suggested firmware structure

```
main/
  board.c/.h        pins, modem power on/off, STATUS
  cell.c/.h         esp_modem DTE/DCE, registration wait, PPP up/down, reconnect loop, events
  mqtt_link.c/.h    esp-mqtt client: mqtts://mqtt.rmx.lt:8883, LWT, subscribe, request/response
  https_api.c/.h    esp_http_client: login (cookie + Origin), records CRUD
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

## Open questions for the user

1. **MQTT login for the device**: reuse `probe-web` (password exists on the server) or create a
   new user (e.g. `probe-device`) with an ACL entry on the VPS? (VPS change → rmx-vps CHANGELOG.)
2. **HTTPS login**: which web user should the device use (create one with
   `probe-server create-user` on the VPS)?
3. **Secrets on the device**: sdkconfig (gitignored) for now, NVS later? Never commit them.
4. Should the probe also exercise the planned `mower/mower-01/…` topics (already allowed by the ACL)?
5. Modem firmware update B04 → B06: not needed so far; only if bugs show up.
6. Is the probe running on the VPS when testing? (It is off by default → 502.)

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
