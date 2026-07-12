# Auto Brightness | WLED usermod (BH1750FVI / VEML7700 / analog photocell)
**Author:** Austin St. Aubin <austinsaintaubin@gmail.com> | **License:** MIT
[![build](https://github.com/AustinSaintAubin/wled-usermod-auto-brightness/actions/workflows/build.yml/badge.svg)](https://github.com/AustinSaintAubin/wled-usermod-auto-brightness/actions/workflows/build.yml)

A community [WLED](https://github.com/wled/WLED) usermod that **drives WLED's overall
brightness from ambient light**, with a perceptual (logarithmic) lux→brightness map,
smoothing, a relative manual offset, an optional "Off When Dark" function, and optional
MQTT / Home Assistant publishing.

| Light source  | Type                       | Address / pin |
|---------------|----------------------------|---------------|
| **BH1750FVI** | I²C digital lux sensor     | `0x23` (or `0x5C` with ADDR high) |
| **VEML7700**  | I²C digital lux sensor     | `0x10`        |
| **Analog**    | Photocell/LDR divider or analog lux sensor | any ADC-capable GPIO |

The I²C addresses don't collide, so **Auto** mode simply probes BH1750 first, then
VEML7700, and uses whichever it finds — plug-and-play. The analog source is never
auto-selected (it needs an explicit pin) and is calibrated to lux via two raw→lux points.

> This usermod is the auto-brightness feature **broken out of**
> [wled-usermod-sensors-i2c](https://github.com/AustinSaintAubin/wled-usermod-sensors-i2c)
> (which keeps its environmental sensor readings, including BH1750 lux). Both usermods
> can be installed together and can even share the same BH1750.

> **Note:** this usermod (code, settings UI, and docs) was developed with **AI assistance**
> and validated by building against WLED. Review before use and verify on your own hardware.

> **Community usermod — use at your own risk.** This is a third-party usermod and is **not**
> reviewed, tested, endorsed, or supported by the WLED project. Usermods compile into your
> firmware with full access to your device. Read the source before flashing.

## Features

- **Selectable or auto-detected light source**: BH1750FVI, VEML7700 (both I²C on WLED's
  globally configured `i2c_sda`/`i2c_scl` pins) or an analog photocell/LDR.
- **Perceptual brightness mapping**: `log10(lux)` mapped onto a configurable
  Lux Min/Max → Brightness Min/Max range, exponentially smoothed against flicker.
- **Manual offset**: adjust brightness by hand and the difference is tracked as a
  relative offset on top of the automatic value (clearable with one button).
- **Off When Dark**: switch the strip fully off below a lux threshold, back on above a
  second one (hysteresis), with a deliberate-override escape hatch.
- **Analog calibration**: two raw→lux calibration points (dark and bright), interpolated
  in log-lux space — inverted wiring (dark = higher ADC value) works without extra config.
- **MQTT / Home Assistant** (optional): publishes the lux value as an illuminance sensor
  and an **Auto Brightness switch** via HA MQTT discovery.
- A sensor that drops off the bus is **re-probed automatically** (every 30 s) and recovers.
- **Self-contained** — makes no changes to `wled00/` and uses a local usermod id, so it
  drops in cleanly as an out-of-tree module.

## Hardware / wiring

**I²C sensor (BH1750FVI or VEML7700):**
1. Wire `SDA`/`SCL` to your ESP32's I²C pins, plus `3V3` and `GND`.
2. In WLED, set the **I²C GPIOs** under *Config → LED Preferences* (or via the
   `I2CSDAPIN` / `I2CSCLPIN` build flags). With an I²C source selected (including
   Auto), the usermod refuses to start if I²C pins are not configured.

**Analog photocell/LDR:**
1. Build a voltage divider: 3V3 → LDR → *ADC pin* → fixed resistor (e.g. 10 kΩ) → GND
   (or the other way around — the calibration handles inverted wiring).
2. Use an **ADC1 pin** (classic ESP32: GPIO 32–39; ADC2 pins don't work with WiFi on).
3. Set *Source* to **Analog** and enter the pin on the Usermods settings page, then
   calibrate (see below).

> Targeted at **ESP32**. (ESP8266 analog `A0` support may come later.)

## Install / Build

This is an **out-of-tree** usermod, consumed via WLED's git-URL `custom_usermods` mechanism —
you don't copy it into the WLED source tree. See the WLED docs:
[Writing a usermod → Share it via git URL](https://kno.wled.ge/advanced/custom-features/#writing-a-usermod).

1. Get the [WLED](https://github.com/wled/WLED) source.
2. In a `platformio_override.ini` at the WLED repo root, reference this repo by URL in your build
   environment's `custom_usermods`:
   ```ini
   custom_usermods = https://github.com/AustinSaintAubin/wled-usermod-auto-brightness.git#main
   ```
   PlatformIO fetches it automatically — no manual copy and no git submodule needed. The `wled-`
   library name is auto-recognized as a usermod. Pin a release with `#v1.0.0` instead of `#main`
   if you prefer a fixed version. For local development you can instead point at a checkout:
   `custom_usermods = symlink:///absolute/path/to/wled-usermod-auto-brightness`.

   To combine with other usermods — e.g. the companion
   [wled-usermod-sensors-i2c](https://github.com/AustinSaintAubin/wled-usermod-sensors-i2c) —
   use the **multiline** form (one entry per indented line; mixing a bare name and a URL on a
   single line breaks parsing):
   ```ini
   custom_usermods =
     https://github.com/AustinSaintAubin/wled-usermod-auto-brightness.git#main
     https://github.com/AustinSaintAubin/wled-usermod-sensors-i2c.git#main
   ```
3. Build & flash for your ESP32.

The required sensor libraries (BH1750, Adafruit VEML7700) are listed as `dependencies` in
`library.json` and installed **automatically** by PlatformIO — no manual `lib_deps`.

A ready-to-copy [`platformio_override.sample.ini`](examples/platformio_override.sample.ini) is included
(the git-URL `custom_usermods` line, the I²C pin flags, OTA upload env, and size-trim
flags) — copy it to the WLED repo root as `platformio_override.ini` and adjust.

## Settings (Config → Usermods → "Auto Brightness")

The section starts with the master **Enabled** checkbox. Directly below it, a **Live
table** (Light Source / Ambient Light / Ambient Light Raw / Brightness Control) with a
**↻ Refresh** button takes a **fresh sensor reading** (via the JSON `read` command below)
and re-fetches the values from `/json/info` — this is also how you read the raw ADC
values off the analog sensor for calibration.

**Light Sensor**

| Setting          | Default | Notes |
|------------------|---------|-------|
| Source           | Auto    | Auto (probes BH1750, then VEML7700) / BH1750FVI / VEML7700 / Analog |
| BH1750 Address   | 0x23    | `0x23`, or `0x5C` if its ADDR pin is high (Auto probes both) |
| Analog Pin       | -1      | ADC-capable GPIO for the Analog source (classic ESP32: 32–39); -1 = unset |
| Cal Dark Raw/Lux | 200 → 1 lx | Calibration point: ADC reading in the dark and the lux it represents |
| Cal Bright Raw/Lux | 3800 → 1000 lx | Calibration point: ADC reading in bright light and the lux it represents |

The four calibration fields render as a small **Dark/Bright × ADC Raw|Lux table**; they
only matter for the Analog source. To calibrate: darken the room, read *Ambient Light Raw*
from the Live table, enter it as *Cal Dark Raw* with your estimate of the true lux; repeat
in bright light for the *Bright* row. Swapped raw values (dark reads **higher** than
bright — the usual photocell-to-GND divider) are handled automatically.

**Brightness**

| Setting               | Default | Notes |
|-----------------------|---------|-------|
| Control Enabled       | off     | Ambient control on/off (this is what the HA switch and the `on` JSON command toggle) |
| Lux Min               | 1       | Lux value mapped to *Brightness Min* |
| Lux Max               | 1000    | Lux value mapped to *Brightness Max* |
| Brightness Min        | 5       | Brightness at/below Lux Min (0–255) |
| Brightness Max        | 255     | Brightness at/above Lux Max (0–255) |
| Smoothing             | 70 %    | Exponential smoothing (0 = instant, higher = smoother) |
| Update Interval       | 2 s     | How often the sensor is read / brightness is recomputed |
| Allow Manual Offset   | on      | See "Manual adjustments" below |
| Reset Offset          | button  | Instantly clears the manual offset (sends the `resetOffset` JSON command — no Save needed) |

**Off When Dark** (own sub-section; the two lux fields render as a small table)

| Setting       | Default | Notes |
|---------------|---------|-------|
| Enabled       | off     | Master switch: turn the LEDs fully off in darkness |
| Off Below Lux | 5       | Lux below which the LEDs switch off |
| On Above Lux  | 20      | Lux at/above which normal auto-brightness resumes; kept ≥ *Off Below Lux* (set higher for hysteresis) |

**MQTT & Home Assistant** (the page header displays as "MQTT Home Assistant" — WLED strips
punctuation from group titles)

| Setting                  | Default | Notes |
|--------------------------|---------|-------|
| Publish Illuminance      | on      | Publish the lux value over MQTT / HA. Turn **off** if wled-usermod-sensors-i2c already publishes the same BH1750 and you don't want a second illuminance entity |
| Publish Changes Only     | on      | Only publish lux when it changes (a refresh still goes out every 5 min so the HA entity never expires) |
| Home Assistant Discovery | off     | Publish HA MQTT discovery configs |

## Auto-brightness behaviour

Brightness is derived from the lux reading using a **logarithmic** map,
which matches how the eye perceives the very wide ambient range:

```
target = map( log10(lux) , log10(luxMin) , log10(luxMax) , briMin , briMax )
```

The target is **exponentially smoothed** to avoid flicker, then applied to WLED's
global brightness via `stateUpdated(CALL_MODE_NO_NOTIFY)`. This keeps the active
preset / effect / colors intact — only overall brightness changes — and does not
broadcast to sync peers.

Turning the LEDs **off** pauses auto-brightness (it will never switch them back on);
control resumes automatically the next time you turn them on. Nightlight fades are
likewise left alone.

With **Off When Dark** enabled, the strip is switched fully off when ambient light
drops below **Off Below Lux** — for rooms where even *Brightness Min* would glow —
and normal control resumes once lux reaches **On Above Lux**. Set *On Above Lux*
higher than *Off Below Lux* to add hysteresis so the lights don't flap around a
single boundary (equal values give a plain threshold; the pair is auto-corrected so
it can never invert).

While it's dark, **darkness wins**: adjusting brightness only updates your manual
offset and the strip switches back off on the next update. The one exception is
explicitly turning the lights **on** from the dark-off state — that is honored
(lights stay on) until the room reaches *On Above Lux*, after which darkness can win
again. The current state is visible on the info page as `dark-off` /
`dark-off (overridden)`. Note that normal auto-brightness never dims to 0 on its
own — only *Off When Dark* switches the strip off.

### Manual adjustments (relative offset)

With *Allow Manual Offset* on, if you manually change brightness (UI, app, etc.)
the difference from the current auto value is captured as an **offset** and added
to all future auto values, so the system keeps tracking ambient light but shifted
to your preference. Clear it with *Reset Offset* (or the JSON command below).

### Analog raw→lux calibration

The two calibration points are interpolated **linearly in log10(lux)** — an LDR
divider's voltage is roughly linear in log-lux, and the brightness mapping is
logarithmic anyway, so the combined ADC→brightness relation stays simple and
monotonic. Readings outside the calibrated raw range are clamped to the nearest
calibration point.

## External access (Home Assistant & similar)

- **Info page / `/json/info`** — `Light Source`, `Ambient Light` (lx),
  `Ambient Light Raw` (Analog source only), and a `Brightness Control` status line
  (applied brightness, target, offset, dark-off state).
- **MQTT** — published under your WLED device topic:

  ```
  <mqttDeviceTopic>/autobri          (control state, ON/OFF, retained)
  <mqttDeviceTopic>/autobri/set      (command topic: ON/OFF or 1/0)
  <mqttDeviceTopic>/autobri/lux      (illuminance value)
  ```

- **Home Assistant** — with *Home Assistant Discovery* on and MQTT connected, an
  **Ambient Light** illuminance sensor and an **Auto Brightness switch** auto-register
  under the WLED device, so ambient control can be toggled straight from HA. Both
  entities use WLED's `/status` LWT as their availability topic, so they show
  *unavailable* whenever the device itself is offline.
- **`/json/state`** — exposes `{"AutoBri":{"on":<bool>,"offset":<int>}}`.

## Controlling auto-brightness from a preset / API

The usermod accepts commands in the JSON state under the `AutoBri` key, which
is also processed when a **preset is applied**. Create a preset of type
**"API command"** (or send the JSON to `/json/state`):

```json
{ "AutoBri": { "on": true } }                      // re-engage automatic control
{ "AutoBri": { "resetOffset": true } }             // clear the manual offset
{ "AutoBri": { "on": true, "resetOffset": true } } // both at once
{ "AutoBri": { "on": false } }                     // hand brightness back to manual
{ "AutoBri": { "read": true } }                    // take a fresh sensor reading now
```

This makes it easy to bind a button / schedule / scene to "return to automatic
brightness".

> **Migrating from wled-usermod-sensors-i2c ≤ v1.x:** the old commands/topics move here —
> `{"SensorsI2C":{"autoBri":…}}` → `{"AutoBri":{"on":…}}`,
> `{"SensorsI2C":{"resetOffset":true}}` → `{"AutoBri":{"resetOffset":true}}`; the MQTT
> `<deviceTopic>/autobri` + `/autobri/set` topics are unchanged (this usermod takes them over).
> Auto-brightness settings are not migrated — re-enter them once on the settings page.

## Notes / limitations

- **VEML7700** runs at fixed gain 1/8 × 100 ms integration (≈0.23 lx resolution,
  ~15 klx ceiling — direct sunlight clips, which the mapping clamps anyway). Auto-ranging
  is deliberately not used because it blocks WLED's loop.
- **Sharing one BH1750 with wled-usermod-sensors-i2c** works (continuous mode reads are
  idempotent); worst case is one stale reading right after the other usermod re-probes.
  Consider pointing this usermod at a VEML7700 (or the other BH1750 address) if you want
  full isolation, and turn off *Publish Illuminance* here to avoid a duplicate HA entity.
- ESP32 only (Analog needs an ADC1 pin with WiFi on; no ESP8266 support yet).
- Uses `USERMOD_ID_AUTO_BRIGHTNESS` defined locally in the `.cpp` (defaults to `901`) so
  the module needs no edit to `wled00/const.h`.

## License

[MIT](LICENSE) © 2026 Austin St. Aubin
