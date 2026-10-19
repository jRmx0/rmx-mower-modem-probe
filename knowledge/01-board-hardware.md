# 01 — Board hardware: LilyGo T-SIM7600 (ESP32)

Product: <https://lilygo.cc/products/t-sim7600?variant=42774480224437> ·
Wiki: <https://wiki.lilygo.cc/products/t-sim-series/t-sim7600x/> ·
Vendor docs (local clone): `C:\Repos\LilyGo-Modem-Series\docs\en\esp32\sim7600-esp32\README.MD` ·
Schematic: `C:\Repos\LilyGo-Modem-Series\schematic\esp32\T-SIM7600X-V1.3.pdf`

## What is on our board (verified on the real device, 2026-10-19)

| Item | Value | How verified |
|---|---|---|
| SoC | ESP32-D0WD-V3, rev v3.1, dual core 240 MHz, 40 MHz crystal | `esptool flash-id` |
| Module | ESP32-WROVER-E **N16R8** | user + measurements below |
| Flash | **16 MB** (JEDEC manuf 0x85, device 0x2018), 3.3 V | `esptool flash-id` |
| PSRAM | **8 MB** quad SPI @ 40 MHz, but **only 4 MB is mapped** (ESP32 address-space limit; the rest needs the himem API) | boot log of `tools/at_bridge` |
| USB-UART bridge | WCH **CH9102** (VID 1A86 PID 55D4), shows up as **COM6** on the dev PC | Windows PnP |
| Modem | SIMCom **SIM7600G-H** (global bands), P/N S2-1097D-Z31E3 | `ATI`, `AT+SIMCOMATI` |
| Modem firmware | `LE20B04SIM7600G22` (`Revision: SIM7600G_V2.0.2`) — LilyGo offers B05 and B06 | `AT+SIMCOMATI` |
| Battery | 18650 present, ~3.84 V (`AT+CBC`) | `AT+CBC` |

> ⚠️ LilyGo's docs and wiki say "4 MB flash / WROVER-B N4R8". That is the older/cheaper
> variant. **Ours is 16 MB** (variant Q387/Q427 "SIM7600G-H R2 16MB"). Use
> `CONFIG_ESPTOOLPY_FLASHSIZE_16MB=y`.

## ESP32 ↔ modem wiring (GPIO map)

From LilyGo `examples/*/utilities.h` (`LILYGO_SIM7600X` block) and the README pin table.

| Signal | ESP32 GPIO | Direction (ESP view) | Notes |
|---|---|---|---|
| UART TX → modem RXD | **27** | out | ESP-IDF `tx_io_num = 27` |
| UART RX ← modem TXD | **26** | in | ESP-IDF `rx_io_num = 26` |
| PWRKEY | **4** | out | Driven through a transistor: ESP **HIGH = key pressed**. On: HIGH pulse ≥ 500 ms. Off: HIGH ≥ 3 s (or `AT+CPOF`). |
| FLIGHT (W_DISABLE) | **25** | out | Must be **HIGH** for the radio to work. LOW = flight mode. |
| DTR | **32** | out | **LOW** keeps the modem awake. Used for sleep with `AT+CSCLK=1`. |
| RI (ring) | **33** | in | Ring/URC indicator, can wake the ESP. Reads 1 when idle. |
| STATUS | **34** | in (input-only pin) | 1 = modem running. |
| Board LED (blue) | 12 | out | HIGH = on. Strapping pin (MTDI); keep it low at reset. |
| SD card | SCK 14, MISO 2, MOSI 15, CS 13 | | GPIO2 is a strapping pin: **remove the SD card when flashing** if uploads fail. |
| Battery ADC | 35 | in | Reads 0 V while USB-C is plugged in (divider disconnected). |
| Solar ADC | 36 | in | |
| I2C (free) | SDA 21, SCL 22 | | free for sensors |
| GPS power | modem AUXVDD (`AT+CGPS…`, "GPIO 127" in TinyGSM) | | not an ESP pin |

- **No RTS/CTS is wired to the ESP32.** Use `flow_control = NONE` and pass
  `UART_PIN_NO_CHANGE` / `-1` for RTS/CTS. (`AT+IFC?` reports `0,0`.)
- GPIO 4, 25, 26, 27, 32, 33 are also exposed on the headers. LilyGo: **don't connect anything** to them.
- GPIO ≥ 34 are input-only on ESP32.
- PSRAM on WROVER uses GPIO 16/17 internally — don't use them.
- The modem UART runs at 1.8 V internally; the board level-shifts it. Don't probe it as 3.3 V.

## DIP switch (back of the board)

| Switch | ON | OFF |
|---|---|---|
| 1 | GPIO27 ↔ modem RX connected | disconnected |
| 2 | GPIO26 ↔ modem TX connected | disconnected |
| 3 | NC | NC |
| 4 | Modem powers on by itself (PWRKEY tied) | **PWRKEY controlled by ESP (GPIO4)** |

- **ESP control mode (what we use):** 1 = ON, 2 = ON, 4 = OFF.
- **USB modem mode** (PC talks to the modem through the second USB-C, for firmware
  updates or dial-up): 1 = OFF, 2 = OFF, 4 = ON.

Our board answered AT on GPIO 26/27 → switches 1 and 2 are ON. The modem was
already on when the ESP booted (STATUS = 1); either switch 4 is ON or it stayed
powered from the battery across ESP resets. **Resetting the ESP does not reset the modem.**
Firmware must handle "modem already on" (send `AT` first, pulse PWRKEY only if there is
no answer — a pulse on a running modem turns it **off**).

## Power

| | |
|---|---|
| USB-C | 5 V, must deliver **≥ 2 A peaks** (LTE TX bursts) or the board browns out |
| Battery | one 18650 Li-ion/LiPo, 4.2 V full. **No LiFePO4.** DW01 protection (OVP 4.3 V, UVP 2.5 V, 3 A) |
| Charger | CN3065 (solar-friendly), USB/solar charge max 500 mA |
| Solar | JST 2.0, 5–6 V, charges the battery only (cannot power the board alone) |
| VBUS pin | = USB-C 5 V; the only non-battery power input |
| VBAT pin | battery +; external battery may be connected here |
| PWR button | first battery connection: press PWR to enable battery power |
| Switching USB ↔ battery | may reset the board (no seamless power path) |

Use a USB-A → USB-C cable (LilyGo: USB-C ↔ USB-C, e.g. Mac ports, may not power the board).

## Antennas, SIM, LEDs

- **MAIN** IPEX = LTE main antenna (required). AUX = diversity (optional). GPS = active 3.3 V GNSS antenna.
  Without the MAIN antenna attached we saw `+CSQ: 99,99` / `NO SERVICE`; with it, CSQ 20, LTE band 1.
- Nano SIM. **Insert the SIM before powering up** — hot-insert is not detected.
- LEDs: modem STATUS (red) and NETLIGHT (red) near the modem (cannot be turned off);
  charge (red) / charged (green) near the battery switch; blue user LED on GPIO12.

## Flashing / console

- Console: UART0 (GPIO1/3) via CH9102, 115200 baud. Upload works at 921600.
- Auto-reset: the CH9102 DTR/RTS drive EN/IO0. **Opening the COM port resets the ESP32**
  (pyserial default). The modem keeps running.
- If upload fails: hold IO0 to GND, press RST, release, flash (and remove the SD card).
