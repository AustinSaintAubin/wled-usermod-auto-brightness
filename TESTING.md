# Bench Test Checklist — v1.0.4

Hardware validation for the initial release (auto-brightness broken out of
wled-usermod-sensors-i2c v1.0.16 + new VEML7700 / analog sources), plus the
v1.0.2 word-clock-style settings UI (tables, Analog Pin dropdown, conditional rows)
and the v1.0.3/v1.0.4 Live-card rebalance + placement fix.
After a clean pass: `git tag v1.0.4 && git push origin v1.0.4`.

> The injected settings JS is regression-tested off-device with a jsdom replica of WLED's
> settings DOM: `cd tools && npm i jsdom && node settings-ui.test.js`. A clean compile +
> green jsdom run still does NOT replace the §1 browser check.

> ℹ️ **Migration note:** auto-brightness settings from wled-usermod-sensors-i2c are
> **not** migrated (different config key) — re-enter them once under
> *Config → Usermods → Auto Brightness*. Presets/automations change from
> `{"SensorsI2C":{"autoBri":…}}` to `{"AutoBri":{"on":…}}`; the MQTT
> `<deviceTopic>/autobri` topics carry over unchanged.

---

## 1. Flash & settings page — ⏳ not tested yet

- [ ] Word-clock style renders (v1.0.2): blue group headers with underline rule, the
      Live card, and all settings laid out as tables — no orphaned labels, no field
      stacked above its control
- [ ] Live card (v1.0.3) is a tidy centered key→value list: muted labels, values aligned
      a consistent distance to the right (not flung to the card edge), no "Reading/Value"
      header row; Refresh button centered below
- [ ] Live card (v1.0.4) sits as a standalone panel directly under the Enable row —
      ABOVE the "Light Sensor" header, not nested inside that group
- [ ] Master row reads "Enable ☑" with the "master switch — I2C sources…" hint on the
      line **below** it
- [ ] Tables render with inputs inside: Light Sensor (Source / BH1750 address / Analog
      pin), Calibration (Dark/Bright × ADC raw|Lux), Lux/Brightness 2×2, Brightness
      settings, Off When Dark, MQTT & Home Assistant
- [ ] Source dropdown shows Auto / BH1750FVI / VEML7700 / Analog; value saves/reloads
- [ ] Analog Pin is a **dropdown** (v1.0.2) listing only ADC-capable GPIOs + "unused"
      (classic ESP32: 32–39); a previously-saved pin stays selected
- [ ] Conditional fields: switching Source shows/hides whole rows live — BH1750 address
      only on Auto/BH1750; Analog pin + Calibration table only on Analog; hidden fields
      keep their values across a Save (set Analog Pin, switch to BH1750, Save, switch
      back → pin value still there)
- [ ] ↻ Refresh returns a genuinely fresh reading; with no sensor wired the table
      shows "(no reading — check sensor)"; unticked Enabled shows "(usermod disabled)"
- [ ] Threshold clamps: On Above < Off Below auto-corrects; Lux Max ≤ Lux Min corrects;
      equal calibration raw values are auto-separated
- [ ] Reset Offset button: with an offset active, click → "Offset Cleared ✓" and the
      info line offset returns to 0 instantly, no Save

## 2. Light sources — ⏳ not tested yet

- [ ] **Auto + BH1750** at 0x23 → info shows `BH1750 @0x23`, plausible lux
- [ ] **Auto + BH1750 @0x5C** (ADDR high) → found on the second probe address
- [ ] **Auto + VEML7700** (no BH1750 on the bus) → info shows `VEML7700`, plausible lux;
      compare readings against the BH1750 for sanity
- [ ] **Forced source** set to a sensor that isn't wired → `Not Found`, recovers ≤ 30 s
      after wiring it
- [ ] **Analog**: pin accepted (ADC1 GPIO), `Ambient Light Raw` moves with light,
      lux follows the calibration points; cover/uncover spans the mapped range
- [ ] **Analog inverted wiring** (dark raw > bright raw): mapping still monotonic
- [ ] Analog pin change on the settings page: old pin released, new pin allocated
      (no "pin allocation" errors in the log / no conflict with LED pins)

## 3. Auto-brightness core — ⏳ not tested yet

(behaviour lifted verbatim from sensors-i2c v1.0.16, where it passed on bench —
this is a regression check in the new home)

- [ ] Brightness tracks ambient light (log map + smoothing), floor of 1, never off
- [ ] Power off from app/UI → lights stay off; power on → clean resume, no phantom offset
- [ ] Manual nudge → offset captured and tracked
- [ ] Nightlight fade untouched, not captured as offset; lights stay off after fading to 0

## 4. Dark-off — ⏳ not tested yet

- [ ] Turns off below Off Below Lux; no flapping inside the hysteresis band; back on
      at On Above Lux
- [ ] Slider adjustment then cover sensor → strip switches off (darkness wins)
- [ ] Power ON while dark-off → stays lit, info shows `dark-off (overridden)`;
      releases at On Above Lux; power OFF re-arms dark-off immediately

## 5. MQTT / Home Assistant — ⏳ not tested yet

- [ ] Enable *HA Discovery* while MQTT connected → **Ambient Light** sensor +
      **Auto Brightness** switch appear without reboot/reconnect (underscore object
      ids — check `homeassistant/sensor/<clientID>/ambient_light/config` on the broker)
- [ ] Switch works both directions (HA→device, device→HA)
- [ ] Lux publishes to `<deviceTopic>/autobri/lux`; *Publish Changes Only* honored;
      heartbeat keeps the entity alive past 30 min (overnight soak)
- [ ] *Publish Illuminance* off → lux entity cleared from HA, switch remains
- [ ] Pull device power → entities *unavailable* (LWT); repower → back
- [ ] `no_mqtt` build variant compiles and runs (CI covers compile)

## 6. Dropout recovery / coexistence — ⏳ not tested yet

- [ ] Unplug the I2C sensor → `Not Found` within ~3 reads, no garbage lux published;
      replug → recovers ≤ 30 s, HA entities reappear without reboot
- [ ] **Both usermods installed** sharing one BH1750: both read plausible lux, no bus
      errors; only intended illuminance entities in HA (turn off *Publish Illuminance*
      here or the *Illuminance* reading there to deduplicate)

---

## Results / notes

| Date       | Item                       | Result | Notes |
|------------|----------------------------|--------|-------|
|            |                            |        |       |
