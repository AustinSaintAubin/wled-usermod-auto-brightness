# Roadmap — wled-usermod-auto-brightness

Ordered plan of record. Items move top-to-bottom: **Release gate** blocks the v1.0.0 tag,
**Next up** is committed work, **Backlog** items are promoted only when their stated
trigger fires (a bench failure, a user request, or an upstream change). Keep this file
honest: when something ships, move its line to the Done log with the version.

## Release gate — v1.0.0 tag (do these first, in order)

1. **Bench pass of [TESTING.md](TESTING.md) §1–§4** on real hardware (ESP32 + BH1750;
   VEML7700 and a photocell for §2). Highest-risk areas, in order:
   - §1 settings page: three table IIFEs + Live readout are new JS — a browser check is
     the only real test (compile proves nothing here).
   - §2 light sources: VEML7700 read path (`VEML_LUX_CORRECTED_NOWAIT` values sane vs the
     BH1750), Auto probe order incl. 0x5C, analog calibration flow end-to-end (read raw
     off the Live table → enter cal points → lux tracks), analog pin re-allocation on
     settings change.
   - §3/§4 control + dark-off: regression only (logic lifted verbatim from sensors-i2c
     v1.0.16 where it passed) — a quick sweep, not a full re-qualification.
2. **Bench pass of TESTING.md §5–§6** (MQTT/HA + dropout/coexistence) once a broker/HA
   are reachable: discovery entities appear with underscore object ids, switch works both
   directions, lux heartbeat survives a 30-min soak, legacy sensors-i2c switch entity is
   cleared, both mods share one BH1750 without bus errors.
3. **Bench sensors-i2c v2.0.0** TESTING.md §0 (strip-down + legacy retained-config
   clear) — `27a38c9` is pushed to GitHub; only the bench remains. The two changes
   are designed to ship together — the new repo's readme already points at the split.
4. **Tag** `v1.0.0` here and `v2.0.0` there; `git push origin <tag>` and mirror to `nas`.
5. **Fix any bench fallout** as `fix: … (v1.0.x)` patches before tagging — the tag marks
   the first bench-proven state.

## Next up (after the tag)

6. **List in the WLED community-usermods index** (PR to `wled/WLED-Docs`, like the
   word-clock's WLED-Docs#336) — both this repo and the updated sensors-i2c description.

## Backlog (promotion trigger stated per item)

- **VEML7700 extended range** — non-blocking auto-gain state machine (step gain/IT
  between reads instead of the blocking `VEML_LUX_AUTO`), raising the ~15 klx ceiling for
  daylight-facing sensors. *Promote when*: a user needs meaningful resolution above
  ~10 klx (indoor use is unaffected — the mapping clamps at Lux Max anyway).
- **ADC tuning for analog** — configurable attenuation (`analogSetPinAttenuation`, 11 dB
  default limits usable range) and a larger/median sample window for noisy dividers.
  *Promote when*: bench §2 shows raw-value jitter > ~2 % or a divider that can't reach
  the ADC rails.
- **ESP8266 support** — fixed `A0` analog path (10-bit, no PinManager), I²C guard
  differences, flash-size audit. *Promote when*: someone actually asks; keep the
  "ESP32 only" readme note until then.
- **HA offset entity** — expose `userBriOffset` as an HA `number` via discovery
  (read/set over MQTT), complementing the switch. *Promote when*: HA-side automation of
  the offset is requested; the JSON API already covers presets.
- **Shared-sensor arbitration** — if both usermods sharing a BH1750 ever show real
  interference on bench (beyond the known one-stale-read-after-reprobe), add a guard
  (e.g. skip re-`begin()` when the chip answers). *Promote when*: TESTING.md §6 fails.
- **sensors-i2c discovery-topic fix** (other repo, tracked here for visibility): its own
  HA discovery object ids still contain spaces (`Absolute Humidity` etc.), so those
  entities likely never register; fix mirrors this repo's underscore scheme + one-time
  retained clears of the spaced topics. *Promote when*: sensors-i2c §4 bench confirms
  the failure (expected) — it's a `feat!`/2.1.0 there.
- **Lux-driven effect speed/intensity** (idea parking lot): map lux onto segment
  speed/intensity as an optional secondary output. *Promote when*: there's a concrete
  use case; out of scope for brightness-control v1.x.

## Done log

- 2026-07-13 — v1.0.4 Live-card placement fix (unbenched — TESTING.md §1): the card was
  landing **under** the "Light Sensor" header on-device — the `querySelector('.abrih')`
  insertion anchor resolved differently on real WLED than in the jsdom render (which
  showed it correctly as a standalone top panel). Now anchored deterministically to the
  master Enable row's own `<br>`, so it sits alone right below Enable in every
  environment. `tools/settings-ui.test.js` grew two DOM-order guards.
- 2026-07-13 — v1.0.3 Live-card visual balance (unbenched — TESTING.md §1): the Live
  readout was edge-justified (values right-aligned to the far edge) while the title and
  Refresh button were centered, leaving short rows with a disconnected gap. Reworked into
  a centered key→value list — muted labels, values left-aligned a fixed distance away
  with tabular figures, redundant "Reading/Value" header dropped. JS-only; both CI builds
  link; `tools/settings-ui.test.js` still green.
- 2026-07-12 — v1.0.2 word-clock-style settings UI (unbenched — TESTING.md §1): whole
  page rebuilt on the sibling word-clock's look & feel (injected `abri*` stylesheet,
  generic `abritbl` field-into-table mover, `abrilbl` relabel). Fixes the v1.0.1
  row-hiding bug (orphaned "Analog Pin" label / stacked "BH1750 Address" — WLED emits a
  hidden marker before *every* field, not just checkboxes; moving the field as a whole
  `getElementsByName` set fixes it). Analog Pin is now a chip-aware GPIO dropdown
  (`PinManager::isAnalogPin`); master row reads "Enable" with the hint below. Injected JS
  regression-tested via `tools/settings-ui.test.js` (jsdom against a faithful WLED DOM);
  both CI builds link.
- 2026-07-12 — v1.0.1 conditional settings UI (unbenched — new TESTING.md §1 item):
  Source dropdown live-hides inapplicable fields (BH1750 Address on VEML/Analog;
  Analog Pin + calibration table on I²C sources) via display:none so hidden values
  still persist. CI matrix now also builds against the WLED v16.0.1 release tag
  alongside main.
- 2026-07-12 — v1.0.0 initial release (unbenched): BH1750/VEML7700/analog sources,
  control logic lifted from sensors-i2c v1.0.16, MQTT/HA with underscore object ids,
  CI green (default + no_mqtt vs WLED main). Repo pushed to GitHub.
