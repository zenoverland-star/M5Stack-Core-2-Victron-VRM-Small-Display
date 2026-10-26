# Victron GX → M5Stack Core2 wall display

A small wall-mounted monitor for a Victron ESS. An **M5Stack Core2** subscribes to
the Victron **GX device's local MQTT broker** and shows live system data — no
internet or VRM cloud required. Everything runs on your LAN.

**Shows:**
- **Battery SOC** — large, colour-coded (green / amber / red), always on top
- **Solar** (cycling page) — combined PV total, each MPPT, and today's kWh
- **Load** (cycling page) — total AC consumption

The lower half auto-rotates between the Solar and Load pages; **tap the screen**
to jump to the next page. Rendering is flicker-free via an off-screen canvas.

---

## Hardware

- **M5Stack Core2** (ESP32) — 2.0" 320×240 touch display, enclosed
- USB-C power supply for permanent operation (the Core2 ships with a cable, not a plug)

That's it — the Core2 is a finished enclosed unit. A wall mount or a custom
fascia is optional.

---

## Software setup

### 1. Arduino IDE + board support
1. Install the **Arduino IDE** (2.x).
2. Boards Manager → install **esp32 by Espressif Systems**.
3. Tools → Board → select **M5Core2** (may appear as **M5Stack-Core2**).

### 2. Libraries (Library Manager)
- **M5Unified** (by M5Stack) — accept its dependencies (M5GFX) when prompted
- **PubSubClient** (by Nick O'Leary) — MQTT
- **ArduinoJson** (by Benoit Blanchon) — JSON parsing

### 3. Enable MQTT on the Victron GX
- GX settings → enable **MQTT on LAN (plaintext)**.
- Note the GX device's **LAN IP** and your **VRM Portal ID**
  (Settings → VRM online portal).

### 4. Configure the sketch
Edit the `CONFIGURE ME` block at the top:
- WiFi SSID + password
- GX IP, MQTT user (usually `root`) + password (leave `""` if your broker has no auth)
- VRM Portal ID
- Your two solar-charger instance numbers (see below)

### 5. Flash
Select the COM port that appears when you plug in the Core2, then Upload.
Keep the sketch in its own folder (one `.ino` per folder).

---

## Finding your MPPT instances

The sketch reads each MPPT separately. Instance numbers are system-specific.
In a tool like **MQTT Explorer**, connect to the GX broker and expand:

```
N/<your-portal-id>/solarcharger/
```

The numeric children (e.g. `277`, `278`) are your instances. Put them in
`MPPT_W` / `MPPT_E`. If you have one MPPT, or more than two, adjust the topic
list and drawing code to match.

---

## Topics used

All values arrive as JSON like `{"value": 12.3}` — the sketch reads `value`.

| Data | Topic (under `N/<portal>/`) |
|------|------------------------------|
| Battery SOC (%) | `system/0/Dc/Battery/Soc` |
| AC load (W) | `system/0/Ac/Consumption/L1/Power` |
| MPPT power (W) | `solarcharger/<instance>/Yield/Power` |
| MPPT today (kWh) | `solarcharger/<instance>/History/Daily/0/Yield` |

> `Daily/0` is today; `Daily/1` is yesterday, and so on. The GX keeps **no
> combined cross-MPPT daily total**, so the sketch sums the per-MPPT figures
> on the device.

---

## Two things that will trip you up

1. **Keepalive is mandatory.** The GX broker stops publishing ~60s after the
   last keepalive. The sketch publishes an empty message to
   `R/<portal>/keepalive` every 30s. Without it you get data for a minute,
   then silence.

2. **ArduinoJson v6 vs v7.** The sketch uses **v7** syntax (`JsonDocument doc;`).
   If Library Manager gave you v6, swap that one line to
   `StaticJsonDocument<128> doc;` (noted in a comment in the code).

---

## Notes

- **Single-phase** AC load uses `L1` only. Three-phase: sum `L1`+`L2`+`L3`.
- **Font8** (the big SOC digits) — if SOC shows blank, swap `&fonts::Font8`
  to `&fonts::Font7` in `banner()`.
- **Black screen on boot?** The off-screen canvas may have failed to allocate —
  add `canvas.setColorDepth(8);` before `createSprite`.
- Runs entirely on the LAN, so it keeps working with the internet disconnected.
  (Only the GX's clock relies on NTP — without a time source, "today's kWh"
  may roll over at the wrong moment.)

Provided as-is, no warranty. Tested on one 2-MPPT, single-phase system.
