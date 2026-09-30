# ESP32-C3 Super Mini + CC1101 433 MHz RF Gateway

Standalone Wi-Fi RF analyzer / recorder / transmitter for **ESP32-C3 Super Mini** and a **CC1101 433 MHz** module.

Features:

- SoftAP captive-portal Wi-Fi provisioning + mDNS (`http://cc1101.local`)
- Dark responsive web UI (no CDN dependencies)
- Stepped RSSI frequency scanner (not an SDR/FFT spectrum)
- Raw async OOK RX with GPIO edge timing capture
- Record Button workflow + pulse clustering analysis
- Persistent saved signals (LittleFS)
- Raw timing replay / TX with radio state lock (IDLE / SCANNING / RX / RECORDING / TX)
- JSON REST API suitable for later Home Assistant use

## Hardware wiring

| CC1101 pad | CC1101 signal | ESP32-C3 Super Mini |
|-----------:|---------------|---------------------|
| 1 | GND | GND |
| 2 | VCC | **3.3V only** |
| 3 | GDO0 | GPIO5 |
| 4 | CSN | GPIO1 |
| 5 | SCK | GPIO4 |
| 6 | MOSI | GPIO3 |
| 7 | MISO | GPIO2 |
| 8 | GDO2 | not connected |

**Important:** the CC1101 is a 3.3 V device. Do not power it from 5 V.

## Why a custom CC1101 driver?

Popular Arduino helpers (notably older SmartRC / ELECHOUSE builds) have known **Init hangs on ESP32-C3 Super Mini** related to nested `SPI.begin()` usage. This project talks to the CC1101 with a **direct SPI register driver** so:

- `PARTNUM` / `VERSION` probing is reliable
- asynchronous serial mode (raw GDO0 RX/TX) is explicit
- frequency / modulation / BW / deviation are under our control

## Capture / TX approach (ESP32-C3)

ESP32-C3 RMT has only **2 RX + 2 TX memory blocks** (~48 symbols/block). That is too small for typical multi-repeat 433 MHz remote bursts (often hundreds of edges).

Therefore this firmware uses:

- **RX:** GPIO `CHANGE` ISR on GDO0 writing timestamps into a preallocated ring buffer (`esp_timer_get_time()`). No heavy work / no heap in the ISR.
- **TX:** CC1101 async TX mode + precise busy-wait replay of signed pulse timings on GDO0, with a hard **5 s** max transmit duration.

## Build requirements

- [PlatformIO](https://platformio.org/) Core 6+
- USB data cable
- ESP32-C3 Super Mini board

## Build

```bash
cd esp32-rf433gw
pio run
```

## Upload firmware

```bash
pio run -t upload
```

If the port is not auto-detected:

```bash
pio run -t upload --upload-port /dev/cu.usbmodem*
```

## Upload LittleFS web assets

The UI lives in `data/` (`index.html`, `app.js`, `style.css`) and must be uploaded separately:

```bash
pio run -t uploadfs
```

Or both firmware + filesystem:

```bash
pio run -t upload -t uploadfs
```

## Serial monitor

```bash
pio device monitor -b 115200
```

Expected boot lines include:

```text
ESP32-C3 CC1101 RF Gateway
[CC1101] detected PARTNUM=0x00 VERSION=0x14
[WiFi] ...
[Web] HTTP server on :80
```

## First boot / Wi-Fi provisioning

1. Power the board. If no Wi-Fi credentials are stored, it starts SoftAP:
   - SSID: `CC1101-Gateway-XXXX` (XXXX from MAC)
   - AP IP: `192.168.4.1`
2. Join the SoftAP. A captive portal / web UI should open (or browse to `http://192.168.4.1`).
3. Scan networks, pick SSID, enter password, **Save & Reboot**.
4. After reboot the device joins your LAN.
5. Open `http://cc1101.local` or the printed IP address.

If STA association fails for ~30 seconds, the firmware falls back to SoftAP again.

Clear Wi-Fi from **Settings → Clear Wi-Fi & Reboot**.

## Testing procedure (milestones)

### Milestone 1 — bring-up

1. Firmware boots, Serial at 115200 works.
2. Log shows SPI init and CC1101 `PARTNUM` / `VERSION`.
3. Frequency defaults to **433.920 MHz**; RSSI sample prints.
4. SoftAP / STA provisioning works; web UI loads from LittleFS.

### Milestone 2 — scanner

1. Open **Spectrum**.
2. Start scan over 433.00–434.79 MHz, 25 kHz step.
3. Chart updates with RSSI points; peak frequency shown.
4. Enable **Find signal**, transmit a remote nearby, confirm detection banner.

### Milestone 3 — live RX / record / replay

1. **Live RX → Start RX**: pulses and waveform update over WebSocket.
2. **Recorder → Record Button**, press remote: burst captured, clusters + heuristic decode shown.
3. **Save** signal, then **Replay** from Saved Signals / Transmitter.
4. Confirm radio refuses simultaneous scan + TX (state lock).

## REST API (JSON)

| Method | Path | Description |
|--------|------|-------------|
| GET | `/api/status` | Wi-Fi + CC1101 + RF status |
| GET | `/api/signals` | List saved signals |
| GET | `/api/signals/{id}` | Get one signal (with pulses) |
| DELETE | `/api/signals/{id}` | Delete signal |
| POST | `/api/signals/{id}/replay` | Replay saved signal |
| POST | `/api/scan/start` | Start stepped RSSI scan |
| POST | `/api/scan/stop` | Stop scan |
| GET | `/api/scan/data` | Scan points / peaks |
| POST | `/api/rx/start` | Start raw RX |
| POST | `/api/rx/stop` | Stop raw RX |
| POST | `/api/record/start` | Start Record Button workflow |
| POST | `/api/record/stop` | Stop / finalize |
| POST | `/api/record/save` | Persist last recording (`{"name":"..."}`) |
| POST | `/api/tx` | Transmit pulses / CSV / saved id |
| GET/POST | `/api/settings` | Read / apply RF settings |
| POST | `/api/settings/rf/reset` | Reset RF defaults |
| GET | `/api/wifi/scan` | Scan Wi-Fi networks |
| POST | `/api/wifi/save` | Save credentials + reboot |
| POST | `/api/wifi/clear` | Clear credentials + reboot |

WebSocket: `ws://<host>/ws` — live status / pulse window.

## Apple HomeKit

Native HomeKit via [HomeSpan](https://github.com/HomeSpan/HomeSpan) (no Homebridge).

1. Connect the hub to your 2.4 GHz Wi-Fi (STA mode).
2. Pair signals in **Recorder**, then open **Remotes**.
3. Create a remote, add buttons linked to saved signals, save (hub reboots).
4. On iPhone: **Home → Add Accessory → More options… → Enter code**

Setup code: **231-14-081**

Each remote appears as one HomeKit accessory with one switch per button. Tapping a switch transmits that RF signal (momentary).

Web UI stays on port 80; HomeKit HAP uses port **1201**.

## Project layout

```text
src/
  main.cpp
  radio/          CC1101 + capture/scan/analyze/TX + state manager
  network/        Wi-Fi SoftAP / STA / captive DNS / mDNS
  web/            Async HTTP + WebSocket API
  storage/        LittleFS signal store
  models/         RFSignal structures
data/             Web UI (LittleFS)
include/Config.h  Pins + defaults
```

## Troubleshooting

### CC1101 NOT DETECTED

- Confirm **3.3 V** power (not 5 V)
- Common ground with the ESP32
- Recheck the pin table (SCK=4, MOSI=3, MISO=2, CSN=1, GDO0=5)
- Short Dupont wires; reseat breadboard contacts
- Most modules use a **26 MHz** crystal (assumed by the driver)

### Web UI missing / 404

Upload the filesystem: `pio run -t uploadfs`

### SoftAP works but STA never joins

- Wrong password / 5 GHz-only AP (ESP32-C3 is 2.4 GHz only)
- Clear credentials and reprovision

### Capture overflow

- Strong continuous noise on the band fills the ring buffer
- Stop RX, move away from noise sources, or raise the record RSSI threshold

### No Serial output on Super Mini

`platformio.ini` enables USB CDC:

```ini
-DARDUINO_USB_MODE=1
-DARDUINO_USB_CDC_ON_BOOT=1
```

Select the correct `cu.usbmodem*` port.

## Safety notes

- Max raw TX duration defaults to **5 seconds**
- Only one RF owner at a time (no accidental RX+TX)
- Frequency values are validated against CC1101 bands
- Heuristic binary decode is labeled as guessed — do not treat it as protocol certainty

## License

Project code is provided as-is for personal / research use. Comply with local RF regulations when transmitting.
