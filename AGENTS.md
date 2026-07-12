# Agent working notes — Auto Brightness (WLED usermod)

Operational knowledge for AI agents / contributors working in this repo. End-user docs
(features, wiring, calibration, install) live in [readme.md](readme.md) — don't duplicate
them here. Planned work lives in [ROADMAP.md](ROADMAP.md); bench validation in
[TESTING.md](TESTING.md).

## What this is

An **out-of-tree WLED community usermod** that drives WLED's global brightness (`bri`)
from ambient light: log10(lux)→brightness mapping, EMA smoothing, relative manual offset,
optional dark-off, MQTT/HA switch + illuminance sensor. Light sources: BH1750FVI
(I²C 0x23/0x5C), VEML7700 (I²C 0x10) — selectable or auto-probed — or an analog
photocell/LDR with two-point raw→lux calibration. **All code lives in one file:
`usermod_v2_auto_brightness.cpp`** (~650 lines). MIT licensed.

History: broken out of the sibling `AustinSaintAubin/wled-usermod-sensors-i2c` at its
v1.0.16 → v2.0.0 boundary (2026-07-12). The control logic (`updateAutoBrightness()`,
`onStateChange()`, dark-off semantics) was lifted **verbatim** from there and is
bench-proven in that repo's v1.0.16 TESTING.md; the sensor abstraction (source enum,
VEML7700, analog) is new and unproven until this repo's TESTING.md passes. Both usermods
can be installed together and can share one BH1750.

## Key identifiers — DO NOT rename

Renaming any of these breaks users' saved configs, HA entities, or the WLED loader:

| Identifier | Value |
|---|---|
| C++ class | `UsermodAutoBrightness` |
| Usermod id | `USERMOD_ID_AUTO_BRIGHTNESS` = 901 (sensors-i2c uses 900) |
| Config key (cfg.json `um`) | `Auto Brightness` (sub-groups: `Light Sensor`, `Brightness`, `Off When Dark`, `MQTT & Home Assistant`) |
| JSON state key | `AutoBri` (`on`, `resetOffset`, `read`) |
| MQTT topics | `<deviceTopic>/autobri` (state, retained), `/autobri/set` (command), `/autobri/lux` |
| HA discovery object ids | `auto_brightness` (switch), `ambient_light` (sensor) — **underscores, never spaces** (HA rejects spaced object ids; sensors-i2c ≤1.x had that bug) |
| HA unique_ids | `<mqttClientID>-autobri`, `<mqttClientID>-ambient-light` |
| `library.json` name | `wled-usermod-auto-brightness` — must keep the `wled-` prefix **and** match the repo name (WLED's `load_usermods.py` auto-recognition) |

`library.json` must keep `"build": { "libArchive": false }` or `REGISTER_USERMOD()` won't
link. Dependency gotcha: the VEML7700 registry package is
`adafruit/Adafruit VEML7700 Library` — **with the "Library" suffix**; without it PlatformIO
silently skips the install and the build dies on the missing header.

## How it's consumed / how to build

No standalone build — the mod compiles inside a WLED checkout, pulled via git URL:

```ini
custom_usermods = https://github.com/AustinSaintAubin/wled-usermod-auto-brightness.git#main
```

Building requires a **local WLED checkout**, assumed below at `../WLED` (a sibling of this
repo). Its `platformio_override.ini` (gitignored) holds the build envs; if missing, create
from [examples/platformio_override.sample.ini](examples/platformio_override.sample.ini)
(envs `esp32dev_auto_brightness` / `_ota`).

### Build & verify loop (do this for every code change)

`pio` is the PlatformIO CLI (VSCode install: `~/.platformio/penv/bin/pio`). Use `-d` to
point at the WLED checkout from any directory.

1. In the WLED override, point `custom_usermods` at the local checkout —
   `symlink://` needs an **absolute** path:
   `symlink:///absolute/path/to/wled-usermod-auto-brightness`
2. Build: `pio run -d ../WLED -e <env>`
   - Success = `[SUCCESS]` **and** the mod named in
     `INFO: Code from usermod libraries found in binary: … wled-usermod-auto-brightness`.
3. If the change touches anything inside `#ifndef WLED_DISABLE_MQTT`, also build a
   `no_mqtt` variant (env extending the first with `-D WLED_DISABLE_MQTT`) — CI builds
   both, and the conditional path otherwise never compiles locally.
4. A clean compile does **not** prove the settings UI works (see "Settings-UI quirks") —
   if you touched `appendConfigData()`, the user must load the usermod settings page in a
   browser.

Gotchas:
- PlatformIO **caches** the git-fetched mod. After pushing to `main`, force a re-pull with
  `rm -rf ../WLED/.pio/libdeps/*/wled-usermod-auto-brightness` before rebuilding. When
  changing `library.json` dependencies with a symlink, clear
  `../WLED/.pio/libdeps/<env>` too — deps are only resolved at install time.
- CI (`.github/workflows/build.yml`) additionally enforces `AUTO_BRIGHTNESS_VERSION` in
  the `.cpp` == `library.json` `version`; a mismatch fails the build.
- Flashing hardware (`…_ota -t upload`) — **only when the user asks**.

## Architecture map (`usermod_v2_auto_brightness.cpp`)

In file order:
- **Source enum** `SRC_AUTO/BH1750/VEML7700/ANALOG` (config) + `SRC_NONE` (runtime-only);
  `activeSrc` is what's actually delivering readings, `source` is what's configured.
- **`rawToLux()`** — analog calibration: linear interpolation in **log10(lux)** between
  (calRawDark→calLuxDark) and (calRawBright→calLuxBright), `t` clamped 0..1. Inverted
  wiring = calRawDark > calRawBright, handled by the sign of the denominator;
  `readFromConfig` guarantees the raw points differ and lux points are >0 and ordered.
- **`setAnalogPin()`** — WLED PinManager (`PinManager::` is a *namespace*):
  `isAnalogPin()` + `allocatePin(pin, false, PinOwner::UM_Unspecified)`; always
  deallocate the old pin on change (pattern from WLED's `LDR_Dusk_Dawn_v2`).
- **`probeBH()` / `probeVEML()` / `probeSensor()`** — probe order in Auto: BH1750 at the
  configured address, the other address, then VEML7700 (0x23/0x5C/0x10 never collide).
  `probeVEML()` **pings 0x10 before `veml.begin()`** — Adafruit's `begin()` news an
  `Adafruit_I2CDevice` every call (leak), so probes of an absent chip must never reach it.
  VEML config is fixed gain 1/8 + 100 ms integration; **never use `VEML_LUX_AUTO`** — its
  auto-ranging loop blocks for multiple integration periods and stalls WLED's loop.
  Re-probe cadence: every 30 s while `activeSrc == SRC_NONE` (driven from `loop()`).
- **`readLux()`** — single read path, 3-strike failure counter (`markSensorLost()`) →
  `SRC_NONE` → re-probe recovers. VEML dropout is detected by a `Wire` ping before the
  read (the Adafruit read has no error return). Analog averages 4 samples and can't
  "drop out".
- **`updateAutoBrightness()`** — the control law, **lifted verbatim from sensors-i2c**
  (only change: `loop()` refreshes `curLux` before calling, instead of reading inside).
  Invariants that must survive any edit: `nightlightActive` short-circuit; `bri == 0 &&
  !autoOffActive` = user power-off pause; the mapped path floors at 1 (**only dark-off may
  write `bri = 0`**, or the off would be misread as a user power-off); every self-write
  sets `applyingAuto` before `stateUpdated(CALL_MODE_NO_NOTIFY)`; dark-off "darkness wins"
  semantics with the `darkOverride` latch (see sensors-i2c v1.0.12 bench history for the
  bug this encodes).
- **`onStateChange()`** — manual-offset capture; mirror-image state machine of the above
  (`applyingAuto` reentrancy guard, `offPause`, `darkOverride` latch,
  `CALL_MODE_NIGHTLIGHT` ignored). Treat these two functions as one unit.
- **MQTT block** (`#ifndef WLED_DISABLE_MQTT`) — `publishLuxMqtt()` (changes-only + 5-min
  heartbeat so HA's 1800 s `expire_after` never lapses), discovery
  (`createMqttSensor`/`createMqttSwitch` + shared `addDiscoveryCommon` using WLED's
  `/status` LWT), `mqttInitialize()` publishes-or-clears retained configs, serviced via
  the `discoveryDirty`/`switchPubDirty` flags from `loop()` only while connected.
- **`appendConfigData()`** — entire settings UI as injected JS (see quirks below): Source
  + BH1750-address dropdowns, hints, Reset Offset instant button (POSTs
  `{"AutoBri":{"resetOffset":true}}`), three table IIFEs (calibration 2×2, Lux/Brightness
  2×2, Off When Dark), and the Live readout block (`id='abriRd'`, filters `/json/info` `u`
  by the explicit key list `Light Source` / `Ambient Light` / `Ambient Light Raw` /
  `Brightness Control`).
- **`addToConfig` / `readFromConfig`** — schema per the table above; clamps at the end of
  `readFromConfig` (source range, BH address whitelist, cal-point validity, lux/bri range
  ordering, smoothing ≤95, interval 1–600, dark-off never inverted). I²C-pin guard: every
  source except Analog requires `i2c_sda/i2c_scl ≥ 0`, enforced in both `setup()` and
  `readFromConfig` (WLED parses hw config before usermod config, so the pins are current).
  On re-save (`initDone`): release the pin if the source moved away from Analog, re-probe,
  reset `briSmoothed = NAN` and the dark-off latches, dirty both MQTT flags.

Info-page keys are deliberately **not** prefixed `"Sensor "` — sensors-i2c's Live Readings
table scoops every `Sensor `-prefixed key from `/json/info` when both mods are installed.

## Settings-UI quirks (hard-won — read before touching `appendConfigData`)

- All UI code is JS inside `oappend(F("…"))` C-strings. **One unbalanced quote silently
  breaks the entire settings page** (fields render raw). A successful compile does NOT
  prove the UI works — check in a browser.
- WLED renders each usermod field as `label <input><br>`; checkboxes carry a preceding
  hidden input that must move together with the visible one (the `hid()` helper in each
  IIFE).
- **Insert tables into the DOM *before* moving inputs into them.** `insertBefore` with a
  reference node that was already moved throws, and the `try{}catch(e){}` guard silently
  eats the whole table — this exact bug shipped an empty section in sensors-i2c up to
  v1.0.15 (fixed v1.0.16). Every table IIFE here follows insert-then-move; keep it that way.
- Every IIFE is guarded (`if(!ok)return`) so a WLED settings-DOM change degrades to plain
  fields instead of a broken page.
- Field names in JS must match config keys exactly: `'Auto Brightness:<group>:<field>'`.
  WLED strips punctuation from *displayed* group titles ("MQTT & Home Assistant" renders
  as "MQTT Home Assistant") — display-only, keys keep the `&`.

## JSON API

`{"AutoBri": {…}}` accepts: `"on": true/false` (ambient control on/off — the HA switch
state; runtime-only, not persisted), `"resetOffset": true` (clear the manual offset),
`"read": true` (fresh sensor reading on next `loop()` — I²C must never happen in the
async_tcp request context). State exposes `{"on": bool, "offset": int}`. Commands also
fire when a preset is applied → preset-bindable.

## Release process

Users consume `#main`, so every push is live on their next (cache-cleared) build; tags are
checkpoints gated on bench validation.

1. Bump the version in **2 CI-checked places**: `AUTO_BRIGHTNESS_VERSION` in the `.cpp`
   and `library.json` `version` (CI fails on mismatch). Update the sample-ini header
   comment when touching that file.
2. Commit style: Conventional Commits with the version in the subject —
   `feat: … (v1.0.1)` / `fix: … (v1.0.1)`; docs-only commits are `docs: …` with no bump.
   Trailer: `Co-Authored-By: Claude <model> <noreply@anthropic.com>`.
3. Build-verify (loop above) before committing; push `main`; confirm the GitHub Actions
   matrix (default + no_mqtt vs WLED main) is green.
4. Tag (`git tag vX.Y.Z && git push origin vX.Y.Z`) **only after** the relevant
   TESTING.md sections pass on hardware. TESTING.md convention: numbered checkbox
   sections + a dated Results/notes table; a title version bump per release.

### Maintainer environment notes (Austin's workstation — ignore on other machines)

- WLED checkout: `/home/austin.st.aubin/Documents/PlatformIO/WLED`; `pio` lives at
  `~/.platformio/penv/bin/pio`.
- That checkout's `platformio_override.ini` already contains verification envs for this
  repo: `auto_brightness_ci`, `auto_brightness_ci_no_mqtt`, `both_usermods_ci`,
  `both_usermods_ci_no_mqtt` (the `both_*` envs symlink this repo **and** sensors-i2c —
  use them whenever a change could affect coexistence).
- Sibling repos: `../wled-usermod-sensors-i2c` (companion mod; keep MQTT/HA naming and
  the shared-BH1750 story consistent with it), `../wled-usermod-word-clock-fx-16x16`
  (same repo conventions).
- A private NAS mirror is configured as git remote `nas` — after any push to GitHub,
  also run `git push nas main --tags`.
