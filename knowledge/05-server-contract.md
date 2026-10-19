# 05 — What the device talks to: rmx-server / rmx-backend-probe

Sources (local): `C:\Projects\blade-of-grass\README.md` (architecture),
`C:\Projects\blade-of-grass\rmx-server\CLAUDE.md` (server notes),
`rmx-server\rmx-backend-probe\README.md` + `crates/probe-protocol/src/lib.rs` (wire protocol),
`rmx-server\rmx-vps\services\mosquitto\config\{mosquitto.conf,acl}`, `…\caddy\Caddyfile`.
Re-read those before coding: they are the source of truth and may have changed.

## Endpoints

| Endpoint | What | TLS |
|---|---|---|
| `https://api.rmx.lt/probe/…` | rmx-backend-probe REST (Rust/axum behind Caddy) | Caddy, Let's Encrypt, TLS 1.2/1.3, HTTP/1.1 + h2 |
| `mqtts://mqtt.rmx.lt:8883` | Mosquitto 2.1, MQTT 3.1.1/5 over TLS (min TLS 1.2) | LE cert copied from Caddy |
| `wss://mqtt.rmx.lt/mqtt` | MQTT over WebSockets (browsers) | Caddy |

- VPS `45.81.252.43` (`api`, `mqtt` are CNAMEs to `rmx.lt`). IPv4 only.
- **The probe is off by default on the VPS** (owner starts it when needed). Off → `/probe/*`
  returns 502. On 2026-10-19 it was on: `GET /probe/health` → `200 {"status":"ok","db":true,"mqtt":true,"version":"0.1.0"}`
  (one earlier request that day got 401 — probably during a redeploy).
- Mosquitto on 8883 runs always.

## TLS certificate chain (checked 2026-10-19 with openssl)

Both hosts serve Let's Encrypt's new **Gen-Y ECDSA** chain:

```
CN=api.rmx.lt / CN=mqtt.rmx.lt     ECDSA P-256 leaf, valid 2026-10-15 … 2027-01-13 (90 days, auto-renewed)
  └ CN=YE2 / CN=YE1 (Let's Encrypt) ECDSA P-384, to 2028-09-02
    └ CN=Root YE (ISRG)             ECDSA P-384, cross-signed by
      └ ISRG Root X2                ECDSA P-384, cross-signed by
        └ ISRG Root X1              RSA 4096 (self-signed root)
```

- Trust **ISRG Root X1** and **ISRG Root X2** (both are in ESP-IDF's cert bundle
  `cacrt_all.pem`; Root YE is not, which is fine because the server sends the cross-signs).
  Copies: [`certs/isrg-root-x1.pem`](../certs/isrg-root-x1.pem),
  [`certs/isrg-root-x2.pem`](../certs/isrg-root-x2.pem),
  [`certs/rmx-ca-bundle.pem`](../certs/rmx-ca-bundle.pem) (both). openssl verifies both hosts with either root alone.
- Verified on the device: mbedTLS + `esp_crt_bundle_attach` (PPP path) and the modem's
  TLS with `isrgx1.pem` (AT path) both validate the chain.
- Don't pin the leaf or the intermediate (they rotate every ~60–90 days). Pin roots only.
- Let's Encrypt chains change (they did in 2025–26); keep both roots, re-check with
  `openssl s_client -connect mqtt.rmx.lt:8883 -servername mqtt.rmx.lt -showcerts`.
- **SNI is required** for `api.rmx.lt` (Caddy serves several names on one IP). mbedTLS sends
  it automatically; the modem needs `AT+CSSLCFG="enableSNI",0,1`.

## MQTT broker rules (Mosquitto, `mosquitto.conf` + `acl`)

- `allow_anonymous false`: every connection needs username + password.
- Listener 8883: TLS (`tls_version tlsv1.2` = minimum), **`use_username_as_clientid true`** —
  the broker replaces the client ID with the username. Topic schemes keyed on the client ID
  must use the username.
- ACL:
  - `pattern readwrite mower/%u/#` — **every login** may use only `mower/<its username>/#`.
  - `user app` → `mower/#` (backend).
  - `user probe-app` → `probe/#` (probe server).
  - `user probe-web` → write `probe/req/+`, read `probe/res/+`, `probe/events/#`, `probe/status/#`.
- Logins that exist: `app`, `mower-01`, `probe-app`, `probe-web`. Passwords live only on the
  server (`services/mosquitto/data/credentials.txt`, root 0600). **Never print or commit them.**
  The user must provide the device's password (e.g. via `menuconfig`/NVS, not committed).
- Mosquitto accepts a SUBSCRIBE to a forbidden topic but never delivers to it (no error!).
  A PUBLISH to a forbidden topic is silently dropped (MQTT 3.1.1).
- Bad login → CONNACK 5 "not authorized" (verified from the device).

### Planned mower topics (architecture, not implemented by any server yet)

| Topic | Direction | Notes |
|---|---|---|
| `mower/<id>/telemetry` | mower → server | |
| `mower/<id>/status` | mower → server, retained | also the Last Will (`offline`) |
| `mower/<id>/cmd` | server → mower | command with ID and expiry |
| `mower/<id>/cmd/ack` | mower → server | |

Safety: the mower stops on its own when the link or acks are lost; never rely on the server.

## rmx-backend-probe protocol (the probe target)

The probe server is a CRUD demo on a `records(time, connection_type, value)` table, over
HTTPS REST, WSS and MQTT. Every change is pushed to all clients.

### MQTT (topic prefix `probe`)

| Topic | Direction | Payload |
|---|---|---|
| `probe/req/<client-id>` | client → server | request envelope (JSON) |
| `probe/res/<client-id>` | server → client | response envelope |
| `probe/events/records` | server → all | change event |
| `probe/status/server` | server → all, **retained** | `online` / `offline` (server's LWT) |

- The server subscribes `probe/req/+` (QoS 1), answers on `probe/res/<same id>` (QoS 1).
  Client id: 1–64 chars `[A-Za-z0-9_-]`.
- **Which login can a device use?** Only `probe-web` (or `probe-app`) may touch `probe/#`.
  On 8883 the client id is forced to the username, so with `probe-web` the device
  publishes on `probe/req/probe-web` and subscribes `probe/res/probe-web`,
  `probe/events/records`, `probe/status/server`. (`mower-01` can't reach `probe/#`.)
  ⚠️ Open decision for the user: reuse `probe-web` or add a dedicated device login
  (e.g. a `probe-device` user with the same ACL as `probe-web`) on the VPS.
- Requests (`crates/probe-protocol`):

```jsonc
{ "request_id": "<any string, e.g. uuid>", "request": { "op": "list" } }
{ "request_id": "r2", "request": { "op": "create", "value": "hello from mower" } }
{ "request_id": "r3", "request": { "op": "update", "id": 7, "value": null } }
{ "request_id": "r4", "request": { "op": "delete", "id": 7 } }
```

- Responses:

```jsonc
{ "request_id": "r2", "result": { "status": "ok", "data": { "kind": "record", "record": {
    "id": 7, "time": "2026-10-18T12:00:00Z", "connection_type": "mqtt", "value": "hi",
    "updated_at": "2026-10-18T12:00:00Z" } } } }
{ "request_id": "r1", "result": { "status": "ok", "data": { "kind": "records", "records": [ … up to 500 ] } } }
{ "request_id": "r4", "result": { "status": "ok", "data": { "kind": "deleted", "id": 7 } } }
{ "request_id": "x",  "result": { "status": "error", "error": { "code": "not_found", "message": "…" } } }
```

  Error codes: `bad_request`, `not_found`, `internal`, `unauthorized`, `forbidden`, `too_many_requests`.
  `value` ≤ 1000 characters (or null). A `list` response can be large (≤ 500 records):
  set the MQTT client's `buffer.size` big enough (e.g. 64 KB, in PSRAM) or avoid `list`.
- Events: `{"op":"create","record":{…}}`, `{"op":"update","record":{…}}`,
  `{"op":"delete","id":7,"connection_type":"mqtt"}`.

### HTTPS REST

| Method | Path | Body | Response |
|---|---|---|---|
| GET | `/probe/health` | — | public; 200/503 `{"status","db","mqtt","version"}` |
| POST | `/probe/api/auth/login` | `{"username","password"}` | 200 `{"username"}` + `Set-Cookie`; 401; 429 + `Retry-After` |
| GET | `/probe/api/records` | — | `RecordRow[]` (newest 500) |
| POST | `/probe/api/records` | `{"value": string\|null}` | 201 `RecordRow` |
| PUT | `/probe/api/records/{id}` | `{"value": …}` | 200 / 404 |
| DELETE | `/probe/api/records/{id}` | — | 204 / 404 |
| GET | `/probe/api/auth/me` | — | 200 / 401 |
| POST | `/probe/api/auth/logout` | — | 204 |

Device rules for HTTPS (the API was built for browsers):
1. **Login** with a web user (created on the VPS with `probe-server create-user …`; the user
   provides the credentials). Response header:
   `Set-Cookie: __Secure-probe_session=<64 hex>; Path=/probe; Max-Age=2592000; HttpOnly; Secure; SameSite=Strict`.
   esp_http_client does **not** keep cookies: read the header in `HTTP_EVENT_ON_HEADER`
   and send `Cookie: __Secure-probe_session=<token>` yourself on later requests.
2. **Origin header**: every non-GET request (including login) must carry exactly one
   `Origin` from the server's `ALLOWED_ORIGINS` (`https://api.rmx.lt`) or it gets 403.
   Add `Origin: https://api.rmx.lt` on POST/PUT/DELETE.
3. Send the cookie only once (a duplicated cookie name counts as logged out).
4. Sessions: idle timeout 7 days (sliding), absolute 30 days. Logging in on every boot is
   fine, but failed logins back off (5 free failures per username, 20 per IP, then 1 s … 15 min;
   all devices on a carrier CGNAT may share an IP).
5. `Content-Type: application/json` for bodies. Errors come as `{"code","message"}`.
6. Server records the transport: HTTPS-created rows have `connection_type: "https"`,
   MQTT ones `"mqtt"`. That's how the probe shows which path the device used.
