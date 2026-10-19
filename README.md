# rmx-mower-modem-probe

Part of **Blade of Grass** (open-source robotic mower). A learning/diagnostic firmware for
the cellular link of the future rmx-mower: an ESP32 + SIM7600G-H board that connects over LTE
to **rmx-server** and talks to **rmx-backend-probe** with **MQTT over TLS** and **HTTPS**.

- Hardware: LilyGo T-SIM7600 (ESP32-WROVER-E N16R8, SIM7600G-H)
- Framework: ESP-IDF v6.1, esp_modem (PPP), mbedTLS, esp-mqtt, esp_http_client
- Server: `https://api.rmx.lt/probe/…`, `mqtts://mqtt.rmx.lt:8883`

## Status

Research phase done. Verified on the hardware: PPP over LTE, HTTPS to `/probe/health`
(200), MQTT over TLS to the broker (TLS + CONNECT OK). The probe firmware itself is next.

## Layout

```
knowledge/   knowledge base about the board, modem, ESP-IDF, esp_modem and the server contract
             → start with knowledge/INDEX.md
tools/       at_bridge (AT console firmware), at_session.py (AT script), ppp_smoke (PPP + TLS test)
certs/       ISRG Root X1/X2 PEMs (public CA roots for rmx.lt's Let's Encrypt chain)
```

## License

[Apache-2.0](LICENSE).
