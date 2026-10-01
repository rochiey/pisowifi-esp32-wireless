# PisoWiFi ESP32 Wireless Bridge (R7.2)

Coin-acceptor bridge for a Piso WiFi vendo. An ESP32 (ESP32 DevKit 30-pin)
watches the coin acceptor pulse line and credits MikroTik hotspot users over
the RouterOS API. The captive portal side lives in
https://github.com/rochiey/mikrotik-hotspot-scripts .

## Hardware

- Board: ESP32 DevKit 30-pin (production unit); the sketch still compiles for NodeMCU/ESP8266.
- Coin pulse input: GPIO27 (idle HIGH, pulses LOW, Allan FAST 20 ms pulse / 40 ms pause).
- Solenoid gate output: GPIO26 (polarity configurable, production uses inverted).
- Talks to the hAP Lite at 10.0.0.1:8728 (RouterOS API).

## Build (exact production recipe)

arduino-cli compile --fqbn "esp32:esp32:esp32:FlashMode=dio" PisoWifiBridge

- ESP32 Arduino core: 2.0.17
- Partition scheme: default 4 MB (app0/app1 1280 KB each, so ArduinoOTA works)
- Board option: FlashMode=dio

## OTA update

python3 tools/espota_newprotocol.py -i 10.0.0.5 -p 3232 -a "<api password>" -f firmware/PisoWifiBridge_R7.2.bin

The OTA server is only serviced while the coin slot is IDLE and no credit is
pending recovery, so an update can never interrupt a customer. If the bridge
booted into the setup portal and later self-healed onto the router, R7.1
starts the OTA server automatically (R7 and older did not - a reboot was
needed). Rollback image: firmware/PisoWifiBridge_R6.bin.

## Field endpoints (device 10.0.0.5)

- GET / - status page (firmware revision, coin slot state, report outbox).
- GET /stats - counters as JSON (includes rev).
- GET /log?key=API_PASSWORD - in-RAM log ring.
- GET /reportTest?key=API_PASSWORD - queue and send one dashboard report.
- GET /setRates?key=API_PASSWORD&price0=1&minutes0=10 - live promo table update.
- GET /debugUsers - dump router hotspot users (limit-uptime / uptime).
- GET /insert?mac=AA:BB:CC:DD:EE:FF - manual coin slot page.
- GET /reboot?key=API_PASSWORD - reboot the bridge (hotspot sessions unaffected).

## What R7 / R7.1 add

- Multi-slot crash-safe pending-credit recovery (8 slots, each with the customer MAC).
  A second failed credit can no longer overwrite a different customer's coins.
  New coin sessions are refused while the store is full, so a coin is never
  taken that cannot be recorded; the rare mid-session overflow merges instead
  of dropping value and raises an audit event.
- Direct sales + audit reporting to https://pisowifi.rochiey.dev/api/pisowifi
  (NVS outbox, retries until 2xx): every finished session posts its own sale
  (source=esp32), plus credit failures, pending saves/recoveries, boots,
  Wi-Fi loss/recovery, coin-slot refusals and 30-minute heartbeats.
- Voucher data-file write retried (keeps returning-customer auto-login working).
- Wi-Fi recovery never reboots or hard-resets the radio while a coin session is
  open or a credit is pending.
- OTA self-heals after a portal-mode boot.

## Revision history

- R1: deterministic per-session ledger, flash-wear protection, OTA.
- R2: per-coin pulse-burst pricing (production: P1=10, P5=60, P10=130 min).
- R3: kick-live-session-before-credit; read-first ordering.
- R4: NVS persistence, deferred failure counter, NTP retry.
- R5: boot-order self-heal (setup portal retries the router every 30 s).
- R6: Done-press closeout delivered synchronously.
- R7: multi-slot pending recovery, direct dashboard reporting, voucher-file retry, safe Wi-Fi recovery.
- R7.1: 8 recovery slots / 16 report slots, coin-slot guard, overflow merge, OTA self-heal.
- R7.2: report outbox raised to 32 items (~2 h of busy sales can queue while the internet is down).

## License

Private project - all rights reserved by the repository owner.