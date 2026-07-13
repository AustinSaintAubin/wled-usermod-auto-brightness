// SPDX-License-Identifier: MIT
// usermod_v2_auto_brightness — MIT © Austin St. Aubin
#include "wled.h"
#include <Wire.h>
#include <BH1750.h>
#include <Adafruit_VEML7700.h>

// Ambient-light auto-brightness usermod: drives WLED's overall brightness from a
// light sensor using a perceptual (logarithmic) lux→brightness map with smoothing,
// a relative manual offset, and an optional dark-off ("Off When Dark") function.
//
// Supported light sources (Source dropdown on the Usermods settings page):
//   - Auto      : probes the I2C sensors below and uses the first one found
//   - BH1750FVI : I2C 0x23 (or 0x5C with ADDR high)
//   - VEML7700  : I2C 0x10
//   - Analog    : photocell/LDR divider or analog lux sensor on an ADC pin,
//                 calibrated to lux via two raw→lux points
//
// I2C sources use WLED's globally configured pins (i2c_sda / i2c_scl); Wire.begin()
// is already called by the WLED core, so it is NOT called here.
// MQTT / Home Assistant publishing is optional and only compiled when MQTT is enabled.

// Local usermod id so this mod stays fully self-contained (no edit to const.h).
// 900 is used by the companion wled-usermod-sensors-i2c.
#ifndef USERMOD_ID_AUTO_BRIGHTNESS
#define USERMOD_ID_AUTO_BRIGHTNESS 901
#endif

#define AUTO_BRIGHTNESS_VERSION "1.0.2"  // keep in sync with library.json (CI-checked)

#define AUTOBRI_PROBE_INTERVAL_MS 30000UL   // re-probe cadence while no sensor is found
#define AUTOBRI_MQTT_HEARTBEAT_MS 300000UL  // forced lux republish (keeps HA alive)
#define AUTOBRI_HA_EXPIRE_AFTER_S 1800      // HA marks the lux entity unavailable after this

class UsermodAutoBrightness : public Usermod
{
private:
  // light-source selection (config value; SRC_NONE is runtime-only)
  enum : uint8_t { SRC_AUTO = 0, SRC_BH1750 = 1, SRC_VEML7700 = 2, SRC_ANALOG = 3, SRC_NONE = 255 };

  // ------- master -------
  bool enabled = true;
  bool initDone = false;

  // ------- light-sensor settings -------
  uint8_t  source = SRC_AUTO;
  uint8_t  bhAddress = 0x23;     // BH1750 I2C address (0x23 default, 0x5C if ADDR high)
  int8_t   analogPin = -1;       // ADC-capable GPIO for the Analog source
  // analog calibration: two raw→lux points, interpolated linearly in log10(lux)
  // (an LDR divider is ~linear in log-lux, and the brightness map is log10 anyway).
  // Inverted wiring (dark reads a HIGHER raw value) just means calRawDark > calRawBright.
  uint16_t calRawDark = 200;     // ADC reading in the dark
  uint32_t calLuxDark = 1;       // lux that reading represents
  uint16_t calRawBright = 3800;  // ADC reading in bright light
  uint32_t calLuxBright = 1000;  // lux that reading represents

  // ------- brightness-control settings -------
  bool     controlEnabled = false; // ambient control on/off (the HA switch state)
  uint16_t luxMin = 1;           // lux mapped to Min Brightness
  uint16_t luxMax = 1000;        // lux mapped to Max Brightness
  uint8_t  briMin = 5;           // brightness at/below luxMin
  uint8_t  briMax = 255;         // brightness at/above luxMax
  uint8_t  smoothing = 70;       // EMA smoothing percent (0 = off, higher = smoother)
  uint16_t updateInterval = 2;   // seconds between sensor reads / brightness updates
  bool     darkOffEnabled = false; // master switch for the dark-off (Off When Dark) function
  uint16_t offBelowLux = 5;      // turn strip fully off below this lux
  uint16_t onAboveLux  = 20;     // turn back on at/above this lux (clamped >= offBelowLux)
  bool     allowManualOffset = true;

  // ------- MQTT settings -------
  bool publishLux = true;        // publish the lux value (off silences a duplicate
                                 // illuminance entity when wled-usermod-sensors-i2c
                                 // shares the same BH1750)
  bool publishChangesOnly = true;
  bool haDiscovery = false;      // publish Home Assistant MQTT discovery

  // ------- runtime sensor state -------
  BH1750            lightMeter;
  Adafruit_VEML7700 veml;
  uint8_t activeSrc = SRC_NONE;  // which source is currently delivering readings
  uint8_t bhAddrFound = 0;       // BH1750 address that answered (Auto may pick either)
  int8_t  allocatedPin = -1;     // ADC pin currently held via PinManager
  uint8_t luxFails = 0;          // consecutive read failures -> mark lost + re-probe
  unsigned long lastProbeTime = 0;

  float    curLux = -1;          // lx (-1 = no reading yet)
  uint16_t curRaw = 0;           // last averaged ADC value (Analog source)
  float    lastLux = NAN;        // change tracking for "publish only on change"

  unsigned long lastReadTime = 0;
  bool readRequested = false;    // JSON "read" command -> fresh sample on next loop()

  // ------- brightness-control runtime -------
  float   briSmoothed   = NAN;   // EMA state (mapped brightness, before offset)
  int     lastTargetNoOffset = -1;
  int     userBriOffset = 0;     // relative offset captured from manual changes
  uint8_t lastAutoBri   = 0;     // last brightness value we applied
  bool    briBaselineSet = false;// have we applied auto brightness at least once
  bool    applyingAuto  = false; // guards onStateChange against our own writes
  bool    offPause      = false; // strip turned off by user -> pause control until back on
  bool    autoOffActive = false; // dark-off engaged (Off When Dark); keeps monitoring lux
  bool    darkOverride  = false; // user powered ON during dark-off: stay lit until On Above Lux

  bool discoveryDirty = true;    // (re)publish/clear HA discovery on next connected loop
  bool switchPubDirty = true;    // publish control-switch state on next connected loop
#ifndef WLED_DISABLE_MQTT
  unsigned long lastForcePublish = 0; // heartbeat timer (AUTOBRI_MQTT_HEARTBEAT_MS)
#endif

  // strings to reduce flash usage
  static const char _name[];
  static const char _enabled[];
  static const char _grpSensor[];
  static const char _grpBri[];
  static const char _grpDarkOff[];
  static const char _grpMqtt[];
  static const char _stateKey[];

  // ---- light-sensor layer ----

  // interpolate the ADC value between the two calibration points, linearly in
  // log10(lux). calRawDark > calRawBright (inverted divider) works unchanged;
  // readFromConfig guarantees the two raw values differ.
  float rawToLux(uint16_t raw) {
    float t = ((float)raw - (float)calRawDark) / ((float)calRawBright - (float)calRawDark);
    t = constrain(t, 0.0f, 1.0f);
    float lg = log10f((float)calLuxDark) + t * (log10f((float)calLuxBright) - log10f((float)calLuxDark));
    return powf(10.0f, lg);
  }

  // (de)allocate the analog pin through WLED's pin manager. newPin < 0 releases.
  void setAnalogPin(int8_t newPin) {
    if (allocatedPin >= 0 && allocatedPin != newPin) {
      PinManager::deallocatePin(allocatedPin, PinOwner::UM_Unspecified);
      allocatedPin = -1;
    }
    if (newPin >= 0 && allocatedPin < 0) {
      if (PinManager::isAnalogPin(newPin) &&
          PinManager::allocatePin(newPin, false, PinOwner::UM_Unspecified)) {
        allocatedPin = newPin;
      }
    }
  }

  bool probeBH(uint8_t addr) {
    return lightMeter.begin(BH1750::CONTINUOUS_HIGH_RES_MODE, addr);
  }

  bool probeVEML() {
    // ping first: nothing at 0x10 is the common case while probing, and each
    // Adafruit begin() news an I2CDevice — the ping keeps re-probes leak-free
    Wire.beginTransmission(VEML7700_I2CADDR_DEFAULT);
    if (Wire.endTransmission() != 0) return false;
    if (!veml.begin()) return false;
    // fixed gain/integration instead of VEML_LUX_AUTO: auto-ranging blocks for
    // multiple integration periods and would stall WLED's loop. Gain 1/8 at
    // 100 ms covers ~0.23 lx .. ~15 klx (direct sunlight clips; the brightness
    // mapping clamps at Lux Max anyway).
    veml.setGain(VEML7700_GAIN_1_8);
    veml.setIntegrationTime(VEML7700_IT_100MS);
    return true;
  }

  // Find/recover the light source per the configured Source. Called from setup(),
  // settings save, and every 30 s while nothing is active. Auto probes BH1750 at
  // the configured address, then the other one, then VEML7700 (0x23/0x5C/0x10
  // never collide, so the order is just preference).
  void probeSensor() {
    uint8_t prev = activeSrc;
    activeSrc = SRC_NONE;
    switch (source) {
      case SRC_ANALOG:
        setAnalogPin(analogPin);
        if (allocatedPin >= 0) activeSrc = SRC_ANALOG;
        break;
      case SRC_BH1750:
        if (probeBH(bhAddress)) { activeSrc = SRC_BH1750; bhAddrFound = bhAddress; }
        break;
      case SRC_VEML7700:
        if (probeVEML()) activeSrc = SRC_VEML7700;
        break;
      default: { // SRC_AUTO
        uint8_t alt = (bhAddress == 0x5C) ? 0x23 : 0x5C;
        if      (probeBH(bhAddress)) { activeSrc = SRC_BH1750; bhAddrFound = bhAddress; }
        else if (probeBH(alt))       { activeSrc = SRC_BH1750; bhAddrFound = alt; }
        else if (probeVEML())        { activeSrc = SRC_VEML7700; }
        break;
      }
    }
    luxFails = 0;
    if (activeSrc != prev) discoveryDirty = true; // sensor appeared/changed -> refresh HA
    DEBUG_PRINTF("[%s] source=%u active=%u\n", _name, source, activeSrc);
  }

  void markSensorLost() {
    if (++luxFails < 3) return;   // transient glitch: keep the source
    activeSrc = SRC_NONE;         // lost -> periodic re-probe recovers it
    discoveryDirty = true;
  }

  // Single lux read path for all sources. True when curLux is fresh.
  bool readLux() {
    switch (activeSrc) {
      case SRC_BH1750: {
        float lux = lightMeter.readLightLevel();
        if (lux < 0) { markSensorLost(); return false; } // read error / not ready
        luxFails = 0;
        curLux = lux;
        return true;
      }
      case SRC_VEML7700: {
        // the Adafruit read has no error return, so detect a dropout with a bus ping
        Wire.beginTransmission(VEML7700_I2CADDR_DEFAULT);
        if (Wire.endTransmission() != 0) { markSensorLost(); return false; }
        float lux = veml.readLux(VEML_LUX_CORRECTED_NOWAIT);
        if (isnan(lux) || lux < 0) { markSensorLost(); return false; }
        luxFails = 0;
        curLux = lux;
        return true;
      }
      case SRC_ANALOG: {
        if (allocatedPin < 0) return false;
        uint32_t sum = 0;
        for (uint8_t i = 0; i < 4; i++) sum += analogRead(allocatedPin);
        curRaw = sum / 4;          // small average against ADC noise
        curLux = rawToLux(curRaw);
        return true;
      }
      default:
        return false;
    }
  }

  // ---- brightness control ----

  void updateAutoBrightness() {
    if (nightlightActive) return;             // never fight the nightlight
    if (bri == 0 && !autoOffActive) return;   // user powered off: paused until back on
    // curLux was refreshed by readLux() in loop() right before this call

    // optional dark-off ("Off When Dark"): below "Off Below Lux" switch the strip
    // off entirely; normal control resumes at/above "On Above Lux" (user-set
    // hysteresis; clamped >= off threshold so the range can't invert).
    // Darkness wins: a brightness adjustment only updates the manual offset and
    // the strip switches back off. The one exception is an explicit power-ON from
    // the dark-off state (darkOverride, latched in onStateChange), which keeps
    // the light on until the room reaches On Above Lux.
    if (darkOffEnabled) {
      if (autoOffActive) {
        if (curLux >= (float)onAboveLux) {  // bright again: release dark-off (and any
          autoOffActive = false;            // manual override) and resume control
          darkOverride = false;
          briSmoothed = NAN;                // jump straight to the mapped target
        } else if (darkOverride) {
          return;                           // lit on purpose while dark: hands off
        } else {
          if (bri > 0) {                    // darkness wins: (re)assert off, even
            applyingAuto = true;            // right after a brightness adjustment
            bri = 0;
            lastAutoBri = 0;
            stateUpdated(CALL_MODE_NO_NOTIFY);
          }
          return;
        }
      } else if (curLux < (float)offBelowLux) {
        autoOffActive = true;
        if (bri > 0) {
          applyingAuto = true;
          bri = 0;
          lastAutoBri = 0;
          stateUpdated(CALL_MODE_NO_NOTIFY);
        }
        return;
      }
    }

    uint16_t lMin = max((uint16_t)1, luxMin);          // keep log valid
    uint16_t lMax = max((uint16_t)(lMin + 1), luxMax); // ensure range
    float lx = constrain(curLux, (float)lMin, (float)lMax);

    float target = mapf(log10f(lx), log10f((float)lMin), log10f((float)lMax),
                        (float)briMin, (float)briMax);

    // exponential moving average to avoid flicker
    if (isnan(briSmoothed)) {
      briSmoothed = target;
    } else {
      float alpha = 1.0f - (constrain(smoothing, 0, 95) / 100.0f);
      briSmoothed += alpha * (target - briSmoothed);
    }

    lastTargetNoOffset = (int)roundf(briSmoothed);
    // floor at 1: the mapping/offset path must never power the strip off, or the
    // off would be misread as a user power-off; only dark-off may write bri = 0
    int finalBri = constrain(lastTargetNoOffset + userBriOffset, 1, 255);

    if (!briBaselineSet || abs(finalBri - (int)bri) >= 2) {
      applyingAuto = true;
      bri = (uint8_t)finalBri;
      lastAutoBri = bri;
      briBaselineSet = true;
      stateUpdated(CALL_MODE_NO_NOTIFY); // keep preset/effect/color; only change brightness
    }
  }

#ifndef WLED_DISABLE_MQTT
  // publish the lux reading to <deviceTopic>/autobri/lux (changes-only with a
  // periodic heartbeat so HA's expire_after window never lapses)
  void publishLuxMqtt() {
    if (!publishLux || !WLED_MQTT_CONNECTED || curLux < 0) return;
    bool force = !publishChangesOnly;
    if (millis() - lastForcePublish >= AUTOBRI_MQTT_HEARTBEAT_MS) force = true;
    float l = roundf(curLux * 10.0f) / 10.0f;
    if (!force && l == lastLux) return;
    lastForcePublish = millis();
    lastLux = l;
    char buf[128];
    snprintf_P(buf, sizeof(buf), PSTR("%s/autobri/lux"), mqttDeviceTopic);
    mqtt->publish(buf, 0, false, String(curLux, 1).c_str());
  }

  // shared discovery fragments: availability via WLED's MQTT LWT (<topic>/status
  // carries retained "online"/"offline") + the common device info block
  void addDiscoveryCommon(StaticJsonDocument<768> &doc) {
    doc[F("availability_topic")] = String(mqttDeviceTopic) + F("/status");
    doc[F("payload_available")] = F("online");
    doc[F("payload_not_available")] = F("offline");

    JsonObject device = doc.createNestedObject(F("device"));
    device[F("name")] = serverDescription;
    device[F("identifiers")] = "wled-sensor-" + String(mqttClientID);
    device[F("manufacturer")] = F(WLED_BRAND);
    device[F("model")] = F(WLED_PRODUCT_NAME);
    device[F("sw_version")] = versionString;
  }

  // HA illuminance sensor on <deviceTopic>/autobri/lux. Publishes the discovery
  // config (retained), or clears it (empty retained payload) when inactive so the
  // stale HA entity actually goes away. The "ambient_light" object id / unique_id
  // are distinct from wled-usermod-sensors-i2c's Illuminance entity, so running
  // both usermods never collides.
  void createMqttSensor(bool active) {
    String t = String(F("homeassistant/sensor/")) + mqttClientID + F("/ambient_light/config");
    if (!active) { mqtt->publish(t.c_str(), 0, true, ""); return; }

    StaticJsonDocument<768> doc;
    doc[F("name")] = String(serverDescription) + F(" Ambient Light");
    doc[F("state_topic")] = String(mqttDeviceTopic) + F("/autobri/lux");
    doc[F("unique_id")] = String(mqttClientID) + F("-ambient-light");
    doc[F("unit_of_measurement")] = F("lx");
    doc[F("device_class")] = F("illuminance");
    doc[F("expire_after")] = AUTOBRI_HA_EXPIRE_AFTER_S;
    addDiscoveryCommon(doc);

    String out;
    serializeJson(doc, out);
    mqtt->publish(t.c_str(), 0, true, out.c_str());
  }

  // HA switch for the ambient control: state on <deviceTopic>/autobri, commands
  // (ON/OFF) on <deviceTopic>/autobri/set — lets HA toggle ambient control
  // without hand-written JSON API presets.
  void createMqttSwitch(bool active) {
    String t = String(F("homeassistant/switch/")) + mqttClientID + F("/auto_brightness/config");
    if (!active) { mqtt->publish(t.c_str(), 0, true, ""); return; }

    StaticJsonDocument<768> doc;
    doc[F("name")] = String(serverDescription) + F(" Auto Brightness");
    String st = String(mqttDeviceTopic) + F("/autobri");
    doc[F("state_topic")] = st;
    doc[F("command_topic")] = st + F("/set");
    doc[F("payload_on")] = F("ON");
    doc[F("payload_off")] = F("OFF");
    doc[F("unique_id")] = String(mqttClientID) + F("-autobri");
    addDiscoveryCommon(doc);

    String out;
    serializeJson(doc, out);
    mqtt->publish(t.c_str(), 0, true, out.c_str());
  }

  void publishSwitchState() {
    if (!WLED_MQTT_CONNECTED) return;
    char buf[128];
    snprintf_P(buf, sizeof(buf), PSTR("%s/autobri"), mqttDeviceTopic);
    mqtt->publish(buf, 0, true, controlEnabled ? "ON" : "OFF"); // retained so HA restarts see current state
  }

  // (Re)publish HA discovery for the active entities and clear the retained config
  // otherwise (so disabling HA discovery, losing the sensor, or turning off lux
  // publishing removes the entity). Serviced from loop() via discoveryDirty: MQTT
  // connect, settings save, or the sensor appearing late / recovering.
  void mqttInitialize() {
    bool found = (activeSrc != SRC_NONE);
    createMqttSensor(haDiscovery && publishLux && found);
    createMqttSwitch(haDiscovery && found);
  }
#endif

public:
  void setup() {
    // I2C is required for every source except Analog; never start without pins
    if (source != SRC_ANALOG && (i2c_sda < 0 || i2c_scl < 0)) { enabled = false; return; }
    if (source != SRC_ANALOG) setAnalogPin(-1); // release a pin left from a config change
    probeSensor();
    initDone = true;
  }

  void loop() {
    if (!enabled || strip.isUpdating()) return;
    unsigned long now = millis();

    if (readRequested || now - lastReadTime >= (unsigned long)updateInterval * 1000) {
      readRequested = false;
      lastReadTime = now;
      if (readLux()) {
#ifndef WLED_DISABLE_MQTT
        publishLuxMqtt();
#endif
        if (controlEnabled) updateAutoBrightness();
      }
    }

    // recover the light source if it isn't currently present
    if (activeSrc == SRC_NONE && now - lastProbeTime >= AUTOBRI_PROBE_INTERVAL_MS) {
      lastProbeTime = now;
      probeSensor();
    }

#ifndef WLED_DISABLE_MQTT
    // (re)publish/clear HA discovery after connect, settings save, or sensor recovery
    if (discoveryDirty && WLED_MQTT_CONNECTED) {
      discoveryDirty = false;
      switchPubDirty = true;
      mqttInitialize();
    }
    // switch state after connect / MQTT command / JSON command / settings save
    if (switchPubDirty && WLED_MQTT_CONNECTED) {
      switchPubDirty = false;
      publishSwitchState();
    }
#endif
  }

  // capture manual brightness changes as a relative offset
  void onStateChange(uint8_t mode) {
    if (!enabled || !controlEnabled) return;
    if (applyingAuto) { applyingAuto = false; return; } // our own write
    if (bri == 0) {                                     // powered off: not a manual offset
      if (autoOffActive) darkOverride = false;          // off again in darkness: re-arm dark-off
      else offPause = true;                             // normal power-off: pause until back on
      return;
    }
    if (offPause) {                                     // powered back on: resume auto control;
      offPause = false;                                 // the restored brightness isn't manual
      lastAutoBri = bri;
      return;
    }
    if (autoOffActive && lastAutoBri == 0) {            // powered ON while dark-off holds the strip
      darkOverride = true;                              // off: deliberate "light despite darkness",
      lastAutoBri = bri;                                // honored until the room reaches On Above Lux
      return;
    }
    if (mode == CALL_MODE_NIGHTLIGHT) return;           // nightlight fade is not a manual change
    if (!briBaselineSet || bri == lastAutoBri) return;  // not a brightness change we care about
    if (allowManualOffset && lastTargetNoOffset >= 0) {
      userBriOffset = constrain((int)bri - lastTargetNoOffset, -255, 255);
    }
    lastAutoBri = bri;
  }

  void addToJsonInfo(JsonObject &root) {
    JsonObject user = root[F("u")];
    if (user.isNull()) user = root.createNestedObject(F("u"));

    // Mod identity + version
    JsonArray ver = user.createNestedArray(FPSTR(_name));
    ver.add(F("v" AUTO_BRIGHTNESS_VERSION));

    if (!enabled) {
      ver.add(F(" (disabled)"));
      return;
    }

    // These keys are read by the Live readout on the settings page; they are
    // deliberately NOT prefixed "Sensor " so wled-usermod-sensors-i2c's own live
    // table doesn't scoop them up when both usermods are installed.
    {
      JsonArray j = user.createNestedArray(F("Light Source"));
      switch (activeSrc) {
        case SRC_BH1750:   j.add(String(F("BH1750 @0x")) + String(bhAddrFound, HEX)); break;
        case SRC_VEML7700: j.add(F("VEML7700")); break;
        case SRC_ANALOG:   j.add(String(F("Analog GPIO")) + allocatedPin); break;
        default:           j.add(F("Not Found")); break;
      }
    }
    {
      JsonArray j = user.createNestedArray(F("Ambient Light"));
      if (activeSrc == SRC_NONE) j.add(F("-"));
      else if (curLux < 0) j.add(F("-"));
      else { j.add(roundf(curLux * 10.0f) / 10.0f); j.add(F("lx")); }
    }
    if (activeSrc == SRC_ANALOG) {
      // raw ADC value: this is what the user reads off to fill the calibration points
      JsonArray j = user.createNestedArray(F("Ambient Light Raw"));
      j.add(curRaw);
      j.add(F(" (ADC)"));
    }
    // Control status: applied brightness (/255), the lux-mapped target, and the
    // current manual offset. The second array element is always non-empty so WLED
    // renders "value + suffix" (an empty suffix would stringify the whole array
    // and show a stray trailing comma).
    if (controlEnabled && activeSrc != SRC_NONE) {
      JsonArray j = user.createNestedArray(F("Brightness Control"));
      j.add(bri);
      String suffix = F(" / 255");
      if (lastTargetNoOffset >= 0) suffix += String(F(", target ")) + lastTargetNoOffset;
      suffix += String(F(", offset ")) + (userBriOffset > 0 ? "+" : "") + userBriOffset;
      if (autoOffActive) suffix += darkOverride ? F(", dark-off (overridden)") : F(", dark-off");
      j.add(suffix);
    }
  }

  // expose current control state for external automation
  void addToJsonState(JsonObject &root) {
    JsonObject um = root[FPSTR(_stateKey)];
    if (um.isNull()) um = root.createNestedObject(FPSTR(_stateKey));
    um[F("on")] = controlEnabled;
    um[F("offset")] = userBriOffset;
  }

  // accept commands (also fires when a preset is applied) -> preset-triggerable
  void readFromJsonState(JsonObject &root) {
    JsonObject um = root[FPSTR(_stateKey)];
    if (um.isNull()) return;
    bool b;
    if (getJsonValue(um[F("on")], b)) { controlEnabled = b; switchPubDirty = true; }
    if (getJsonValue(um[F("resetOffset")], b) && b) userBriOffset = 0;
    if (getJsonValue(um[F("read")], b) && b) readRequested = true; // I2C happens in loop(), not here
  }

  void appendConfigData() {
    // The settings UI adopts the sibling word-clock usermod's look & feel: a small
    // injected stylesheet (abri* classes), a generic field-into-table mover (abritbl),
    // and a relabel helper (abrilbl). Everything is guarded so a WLED settings-DOM
    // change degrades to plain fields. See AGENTS.md "Settings-UI quirks" before editing
    // — the JS is verified out-of-band by scratch jsdom tests against a faithful DOM.

    // ---- style ----
    oappend(F("(function(){var s=document.createElement('style');s.innerHTML="));
    oappend(F("'.abrih{margin:16px 14px 6px;padding-bottom:2px;font-weight:600;color:#4aa3ff;border-bottom:1px solid #2c2c2c;letter-spacing:.3px}'"));
    oappend(F("+'.abricard{background:#101010;border:1px solid #2c2c2c;border-radius:8px;padding:6px 10px;margin:6px 14px;display:block}'"));
    oappend(F("+'.abritbl{border-collapse:collapse;margin:4px 14px 8px}'"));
    oappend(F("+'.abricard .abritbl{margin:2px 0}'"));
    oappend(F("+'.abritbl th,.abritbl td{padding:3px 12px 3px 0;text-align:left;vertical-align:middle}'"));
    oappend(F("+'.abritbl th{color:#4aa3ff;font-weight:600;border-bottom:1px solid #2c2c2c}'"));
    oappend(F("+'.abritbl select,.abritbl input:not([type=checkbox]){margin:0;vertical-align:middle;height:26px;box-sizing:border-box}'"));
    oappend(F("+'.abritbl input:not([type=checkbox]){width:84px}'"));
    oappend(F("+'.abritbl input[type=checkbox]{margin:0;vertical-align:middle}'"));
    oappend(F("+'.abritbl button{margin:0 0 0 6px;vertical-align:middle;cursor:pointer;border-radius:6px;padding:2px 9px}'"));
    oappend(F("+'.abrii{font-size:11px;opacity:.6;font-style:normal;margin-left:6px}'"));
    oappend(F(";document.head.appendChild(s);})();"));

    // ---- helpers ----
    // abritbl(hdr, rows): rows = [rowLabel, [fieldSuffix...], extra(tr,cells)|null].
    // Moves ALL elements sharing each name — the hidden type-marker input WLED emits
    // before every field AND the visible input/select — into the row's cells, so the
    // markers travel with their field (moving a single element orphaned the label; see
    // AGENTS.md). Cleans the stray label text + trailing <br>, inserts the table where
    // the first field was, returns it (for id-tagging).
    oappend(F("abritbl=function(hdr,rows){var ok=0;for(var r=0;r<rows.length;r++)if(d.getElementsByName('Auto Brightness:'+rows[r][1][0]).length){ok=1;break;}if(!ok)return null;"));
    oappend(F("var t=document.createElement('table');t.className='abritbl';"));
    oappend(F("var h=document.createElement('tr');for(var i=0;i<hdr.length;i++){var th=document.createElement('th');th.textContent=hdr[i];h.appendChild(th);}t.appendChild(h);"));
    oappend(F("var anchor=null,kill=[];"));
    oappend(F("function mv(td,nm){var e=d.getElementsByName('Auto Brightness:'+nm);if(!e.length)return;var lbl=e[0].previousSibling,af=e[e.length-1].nextSibling;if(!anchor)anchor=(lbl&&lbl.nodeType===3)?lbl:e[0];var a=[];for(var k=0;k<e.length;k++)a.push(e[k]);for(k=0;k<a.length;k++)td.appendChild(a[k]);if(lbl&&lbl.nodeType===3)kill.push(lbl);if(af&&af.nodeName==='BR')kill.push(af);}"));
    oappend(F("for(var ri=0;ri<rows.length;ri++){var row=rows[ri];var tr=document.createElement('tr');var c0=document.createElement('td');c0.textContent=row[0];tr.appendChild(c0);var cells=[];for(var fi=0;fi<row[1].length;fi++){var td=document.createElement('td');mv(td,row[1][fi]);tr.appendChild(td);cells.push(td);}if(row[2])row[2](tr,cells);t.appendChild(tr);}"));
    oappend(F("if(anchor&&anchor.parentNode)anchor.parentNode.insertBefore(t,anchor);"));
    oappend(F("for(ri=0;ri<kill.length;ri++)if(kill[ri].parentNode)kill[ri].parentNode.removeChild(kill[ri]);return t;};"));
    oappend(F("abrilbl=function(fld,t){var a=d.getElementsByName('Auto Brightness:'+fld);if(!a.length)return;var r=a[0].previousSibling;if(r&&r.nodeType===3)r.textContent=' '+t+' ';};"));

    // ---- restyle WLED's auto group titles (<p><u>Group</u></p>) into .abrih ----
    oappend(F("(function(){var secs=d.getElementsByClassName('sec'),sec=null;for(var i=0;i<secs.length;i++){var h=secs[i].querySelector('h3');if(h&&h.textContent==='Auto Brightness'){sec=secs[i];break;}}if(!sec)return;var ps=sec.querySelectorAll('p');for(var j=0;j<ps.length;j++){var u=ps[j].querySelector('u');if(!u)continue;ps[j].className='abrih';ps[j].textContent=u.textContent;var pv=ps[j].previousElementSibling;if(pv&&pv.tagName==='HR')pv.style.display='none';}})();"));

    // ---- dropdowns (before tables so the <select> carries the field name) ----
    oappend(F("dd=addDropdown('Auto Brightness:Light Sensor','Source');"));
    oappend(F("addOption(dd,'Auto (I2C: BH1750, then VEML7700)','0');"));
    oappend(F("addOption(dd,'BH1750FVI (I2C)','1');"));
    oappend(F("addOption(dd,'VEML7700 (I2C)','2');"));
    oappend(F("addOption(dd,'Analog (photocell / LDR)','3');"));
    oappend(F("dd=addDropdown('Auto Brightness:Light Sensor','BH1750 Address');"));
    oappend(F("addOption(dd,'0x23 (default)','35');"));
    oappend(F("addOption(dd,'0x5C','92');"));
    // Analog Pin dropdown: only the ADC1-capable GPIOs this chip actually accepts
    // (PinManager::isAnalogPin is chip-aware and matches what setAnalogPin() allows),
    // plus "unused". A saved pin that's no longer valid falls back to the unused option.
    oappend(F("dd=addDropdown('Auto Brightness:Light Sensor','Analog Pin');"));
    oappend(F("addOption(dd,'unused','-1');"));
    {
      char buf[40];
      for (int g = 0; g <= 48; g++) {
        if (PinManager::isAnalogPin((byte)g)) {
          snprintf_P(buf, sizeof(buf), PSTR("addOption(dd,'%d','%d');"), g, g);
          oappend(buf);
        }
      }
    }

    // ---- tables ----
    oappend(F("abritbl(['Sensor','Value'],[['Source',['Light Sensor:Source']],['BH1750 address',['Light Sensor:BH1750 Address']],['Analog pin',['Light Sensor:Analog Pin']]]);"));
    oappend(F("(function(){var t=abritbl(['Calibration','ADC raw','Lux'],[['Dark',['Light Sensor:Cal Dark Raw','Light Sensor:Cal Dark Lux']],['Bright',['Light Sensor:Cal Bright Raw','Light Sensor:Cal Bright Lux']]]);if(t)t.id='abriCal';})();"));
    oappend(F("abritbl(['Range','Lux','Brightness'],[['Min',['Brightness:Lux Min','Brightness:Brightness Min']],['Max',['Brightness:Lux Max','Brightness:Brightness Max']]]);"));
    oappend(F("abritbl(['Setting','Value'],[['Ambient control',['Brightness:Control Enabled']],['Smoothing',['Brightness:Smoothing']],['Update interval',['Brightness:Update Interval']],['Allow manual offset',['Brightness:Allow Manual Offset']]]);"));
    oappend(F("abritbl(['Setting','Value'],[['Enabled',['Off When Dark:Enabled']],['Off below (lux)',['Off When Dark:Off Below Lux']],['On above (lux)',['Off When Dark:On Above Lux']]]);"));
    oappend(F("abritbl(['Setting','Value'],[['Publish illuminance',['MQTT & Home Assistant:Publish Illuminance']],['Publish changes only',['MQTT & Home Assistant:Publish Changes Only']],['Home Assistant discovery',['MQTT & Home Assistant:Home Assistant Discovery']]]);"));

    // ---- master row relabel ----
    oappend(F("abrilbl('Enabled','Enable');"));

    // ---- "Live" readout card (source/lux/raw/control from /json/info; Refresh takes a
    // genuinely fresh reading — the raw ADC value is what you read off to fill the
    // calibration table). Inserted before the first group header. ----
    oappend(F("(function(){try{if(d.getElementById('abriRd'))return;var en=d.getElementsByName('Auto Brightness:Enabled');if(!en.length)return;var cb=en[en.length-1],sec=cb;while(sec&&!(sec.nodeType==1&&sec.tagName=='DIV'&&sec.className=='sec'))sec=sec.parentNode;if(!sec)return;"));
    oappend(F("var card=d.createElement('div');card.className='abricard';var p=d.createElement('p');p.className='abrih';p.textContent='Live';card.appendChild(p);"));
    oappend(F("var T=d.createElement('table');T.id='abriRd';T.className='abritbl';card.appendChild(T);"));
    oappend(F("function row(k,v,hd){var tr=d.createElement('tr');var a=d.createElement(hd?'th':'td');a.textContent=k;var b=d.createElement(hd?'th':'td');b.textContent=v;b.style.textAlign='right';tr.appendChild(a);tr.appendChild(b);T.appendChild(tr);}"));
    oappend(F("function refresh(){T.innerHTML='';row('Reading','Value',1);fetch('/json/info').then(function(r){return r.json();}).then(function(j){var u=(j&&j.u)||{},K=['Light Source','Ambient Light','Ambient Light Raw','Brightness Control'],any=0;K.forEach(function(k){if(!(k in u))return;any=1;var v=u[k];row(k,Array.isArray(v)?v.filter(function(x){return x!==''&&x!=null;}).join(' '):(''+v));});if(!any)row(cb.checked?'(no reading \\u2014 check sensor)':'(usermod disabled)','');}).catch(function(){row('(fetch failed)','');});}"));
    oappend(F("function reread(){fetch('/json/state',{method:'POST',headers:{'Content-Type':'application/json'},body:'{\"AutoBri\":{\"read\":true}}'}).then(function(){setTimeout(refresh,400);}).catch(function(){refresh();});}"));
    oappend(F("var btn=d.createElement('button');btn.type='button';btn.textContent='\\u21bb Refresh';btn.addEventListener('click',reread);var bw=d.createElement('div');bw.style.marginTop='4px';bw.appendChild(btn);card.appendChild(bw);"));
    oappend(F("var anchor=sec.querySelector('.abrih')||sec.querySelector('.abritbl');sec.insertBefore(card,anchor||null);refresh();}catch(e){}})();"));

    // ---- hints (master hint on its own line below the checkbox; field hints land
    // inside their value cells because the tables have already been built) ----
    oappend(F("addInfo('Auto Brightness:Enabled',1,\"<br><i class='abrii'>master switch \\u2014 I2C sources need the global I2C pins (top of this page)</i>\");"));
    oappend(F("addInfo('Auto Brightness:Brightness:Smoothing',1,\"<i class='abrii'>% (0=off)</i>\");"));
    oappend(F("addInfo('Auto Brightness:Brightness:Update Interval',1,\"<i class='abrii'>sec</i>\");"));
    oappend(F("addInfo('Auto Brightness:Off When Dark:Enabled',1,\"<i class='abrii'>turn strip fully off in darkness</i>\");"));

    // ---- Reset Offset button in the Allow Manual Offset value cell (POSTs the
    // resetOffset command directly — no Save needed) ----
    oappend(F("(function(){var a=d.getElementsByName('Auto Brightness:Brightness:Allow Manual Offset');if(!a.length)return;var cb=a[a.length-1],td=cb.parentNode;if(!td)return;var btn=d.createElement('button');btn.type='button';btn.textContent='Reset Offset';btn.addEventListener('click',function(){fetch('/json/state',{method:'POST',headers:{'Content-Type':'application/json'},body:'{\"AutoBri\":{\"resetOffset\":true}}'}).then(function(){btn.textContent='Offset Cleared \\u2713';setTimeout(function(){btn.textContent='Reset Offset';},1500);}).catch(function(){});});td.appendChild(btn);})();"));

    // ---- Source-conditional visibility: hide the whole <tr> for BH1750 Address
    // (VEML/Analog) and Analog Pin + calibration table (I2C sources). display:none,
    // never removed, so the hidden fields still submit and their values persist. ----
    oappend(F("(function(){try{var sel=d.getElementsByName('Auto Brightness:Light Sensor:Source');sel=sel.length?sel[sel.length-1]:null;var bh=d.getElementsByName('Auto Brightness:Light Sensor:BH1750 Address');bh=bh.length?bh[bh.length-1]:null;var ap=d.getElementsByName('Auto Brightness:Light Sensor:Analog Pin');ap=ap.length?ap[ap.length-1]:null;var ct=d.getElementById('abriCal');if(!sel||!bh||!ap||!ct)return;var bhr=bh.closest?bh.closest('tr'):null,apr=ap.closest?ap.closest('tr'):null;if(!bhr||!apr)return;function upd(){var v=sel.value;bhr.style.display=(v=='0'||v=='1')?'':'none';var an=(v=='3');apr.style.display=an?'':'none';ct.style.display=an?'':'none';}sel.addEventListener('change',upd);upd();}catch(e){}})();"));

    // Source-conditional visibility: hide fields that don't apply to the
    // selected Source (BH1750 Address for VEML/Analog; Analog Pin + the
    // calibration table for I2C sources). Rows are wrapped in a div and
    // toggled via display:none — never `disabled`, and never removed — so the
    // hidden inputs still submit on Save and their values persist. Must run
    // AFTER the calibration-table IIFE (targets #abriCal); same insert-then-
    // move discipline as the tables (wrapper enters the DOM before nodes move).
    oappend(F("(function(){try{var P='Auto Brightness:Light Sensor:';"));
    oappend(F("function vis(n){var a=d.getElementsByName(P+n);return a.length?a[a.length-1]:null;}"));
    oappend(F("var sel=vis('Source'),bh=vis('BH1750 Address'),ap=vis('Analog Pin'),ct=d.getElementById('abriCal');"));
    oappend(F("if(!sel||!bh||!ap||!ct)return;"));
    oappend(F("function wrap(e){var s=e;while(s.previousSibling&&s.previousSibling.nodeType==3)s=s.previousSibling;"));
    oappend(F("var w=d.createElement('div');e.parentNode.insertBefore(w,s);"));
    oappend(F("var n=s;while(n){var q=n.nextSibling,b=(n.nodeType==1&&n.tagName=='BR');w.appendChild(n);if(b)break;n=q;}return w;}"));
    oappend(F("var wb=wrap(bh),wa=wrap(ap);"));
    oappend(F("function upd(){var v=sel.value;wb.style.display=(v=='0'||v=='1')?'':'none';"));
    oappend(F("var an=(v=='3');wa.style.display=an?'':'none';ct.style.display=an?'':'none';}"));
    oappend(F("sel.addEventListener('change',upd);upd();"));
    oappend(F("}catch(e){}})();"));
  }

  void addToConfig(JsonObject &root) {
    JsonObject top = root.createNestedObject(FPSTR(_name));
    top[FPSTR(_enabled)] = enabled;

    JsonObject s = top.createNestedObject(FPSTR(_grpSensor));
    s[F("Source")] = source;
    s[F("BH1750 Address")] = bhAddress;
    s[F("Analog Pin")] = analogPin;
    s[F("Cal Dark Raw")] = calRawDark;
    s[F("Cal Dark Lux")] = calLuxDark;
    s[F("Cal Bright Raw")] = calRawBright;
    s[F("Cal Bright Lux")] = calLuxBright;

    JsonObject b = top.createNestedObject(FPSTR(_grpBri));
    b[F("Control Enabled")] = controlEnabled;
    b[F("Lux Min")] = luxMin;
    b[F("Lux Max")] = luxMax;
    b[F("Brightness Min")] = briMin;
    b[F("Brightness Max")] = briMax;
    b[F("Smoothing")] = smoothing;
    b[F("Update Interval")] = updateInterval;
    b[F("Allow Manual Offset")] = allowManualOffset;

    JsonObject o = top.createNestedObject(FPSTR(_grpDarkOff));
    o[FPSTR(_enabled)] = darkOffEnabled;
    o[F("Off Below Lux")] = offBelowLux;
    o[F("On Above Lux")] = onAboveLux;

    JsonObject m = top.createNestedObject(FPSTR(_grpMqtt));
    m[F("Publish Illuminance")] = publishLux;
    m[F("Publish Changes Only")] = publishChangesOnly;
    m[F("Home Assistant Discovery")] = haDiscovery;
  }

  bool readFromConfig(JsonObject &root) {
    JsonObject top = root[FPSTR(_name)];
    if (top.isNull()) {
      DEBUG_PRINTF("[%s] No config found. (Using defaults.)\n", _name);
      return false;
    }
    bool configComplete = true;

    configComplete &= getJsonValue(top[FPSTR(_enabled)], enabled, true);

    JsonObject s = top[FPSTR(_grpSensor)];
    configComplete &= getJsonValue(s[F("Source")], source, SRC_AUTO);
    configComplete &= getJsonValue(s[F("BH1750 Address")], bhAddress, 0x23);
    configComplete &= getJsonValue(s[F("Analog Pin")], analogPin, -1);
    configComplete &= getJsonValue(s[F("Cal Dark Raw")], calRawDark, 200);
    configComplete &= getJsonValue(s[F("Cal Dark Lux")], calLuxDark, 1);
    configComplete &= getJsonValue(s[F("Cal Bright Raw")], calRawBright, 3800);
    configComplete &= getJsonValue(s[F("Cal Bright Lux")], calLuxBright, 1000);

    JsonObject b = top[FPSTR(_grpBri)];
    configComplete &= getJsonValue(b[F("Control Enabled")], controlEnabled, false);
    configComplete &= getJsonValue(b[F("Lux Min")], luxMin, 1);
    configComplete &= getJsonValue(b[F("Lux Max")], luxMax, 1000);
    configComplete &= getJsonValue(b[F("Brightness Min")], briMin, 5);
    configComplete &= getJsonValue(b[F("Brightness Max")], briMax, 255);
    configComplete &= getJsonValue(b[F("Smoothing")], smoothing, 70);
    configComplete &= getJsonValue(b[F("Update Interval")], updateInterval, 2);
    configComplete &= getJsonValue(b[F("Allow Manual Offset")], allowManualOffset, true);

    JsonObject o = top[FPSTR(_grpDarkOff)];
    configComplete &= getJsonValue(o[FPSTR(_enabled)], darkOffEnabled, false);
    configComplete &= getJsonValue(o[F("Off Below Lux")], offBelowLux, 5);
    configComplete &= getJsonValue(o[F("On Above Lux")], onAboveLux, 20);

    JsonObject m = top[FPSTR(_grpMqtt)];
    configComplete &= getJsonValue(m[F("Publish Illuminance")], publishLux, true);
    configComplete &= getJsonValue(m[F("Publish Changes Only")], publishChangesOnly, true);
    configComplete &= getJsonValue(m[F("Home Assistant Discovery")], haDiscovery, false);

    // sanity / clamping
    if (source > SRC_ANALOG) source = SRC_AUTO;
    if (bhAddress != 0x23 && bhAddress != 0x5C) bhAddress = 0x23;
    if (analogPin < -1) analogPin = -1;
    // the analog calibration needs two distinct raw points and valid (log-able) lux
    if (calRawBright == calRawDark) calRawBright = calRawDark + 1;
    calLuxDark   = constrain(calLuxDark,   (uint32_t)1, (uint32_t)100000);
    calLuxBright = constrain(calLuxBright, (uint32_t)1, (uint32_t)100000);
    if (calLuxBright <= calLuxDark) calLuxBright = calLuxDark + 1;
    if (luxMax <= luxMin) luxMax = luxMin + 1;
    if (briMax < briMin) { uint8_t t = briMin; briMin = briMax; briMax = t; }
    smoothing = constrain(smoothing, 0, 95);
    updateInterval = constrain(updateInterval, 1, 600);
    if (onAboveLux < offBelowLux) onAboveLux = offBelowLux; // never invert the dark-off range

    // I2C is a hard requirement for every source except Analog: never let a saved
    // "Enabled" re-arm the mod while the global I2C pins are unconfigured (setup()
    // refuses for the same reason; WLED parses the hw section before usermod
    // config, so the pins are current here)
    if (source != SRC_ANALOG && (i2c_sda < 0 || i2c_scl < 0)) enabled = false;

    if (initDone) {
      if (source != SRC_ANALOG) setAnalogPin(-1); // source changed away: release the pin
      probeSensor();                              // re-probe with the new settings
      briSmoothed = NAN;                          // reset smoothing state
      autoOffActive = darkOverride = false;       // re-evaluate dark-off against new settings
      discoveryDirty = true;                      // re-publish/clear HA discovery
      switchPubDirty = true;
    }
    return configComplete;
  }

#ifndef WLED_DISABLE_MQTT
  void onMqttConnect(bool sessionPresent) {
    if (!enabled) return;
    char buf[128];
    snprintf_P(buf, sizeof(buf), PSTR("%s/autobri/set"), mqttDeviceTopic);
    mqtt->subscribe(buf, 0);
    discoveryDirty = true; // serviced from loop() while connected
    switchPubDirty = true;
  }

  // handles <deviceTopic>/autobri/set (WLED strips the device-topic prefix)
  bool onMqttMessage(char* topic, char* payload) {
    if (strcmp_P(topic, PSTR("/autobri/set")) != 0) return false;
    if      (!strcasecmp(payload, "ON")  || !strcmp(payload, "1")) controlEnabled = true;
    else if (!strcasecmp(payload, "OFF") || !strcmp(payload, "0")) controlEnabled = false;
    switchPubDirty = true; // echo state back (unknown payloads re-assert the truth)
    return true;
  }
#endif

  // API for inter-usermod data exchange
  inline float getLux() { return (curLux < 0) ? NAN : curLux; } // NAN = no reading

  uint16_t getId() { return USERMOD_ID_AUTO_BRIGHTNESS; }
};

const char UsermodAutoBrightness::_name[]       PROGMEM = "Auto Brightness";
const char UsermodAutoBrightness::_enabled[]    PROGMEM = "Enabled";
const char UsermodAutoBrightness::_grpSensor[]  PROGMEM = "Light Sensor";
const char UsermodAutoBrightness::_grpBri[]     PROGMEM = "Brightness";
const char UsermodAutoBrightness::_grpDarkOff[] PROGMEM = "Off When Dark";
const char UsermodAutoBrightness::_grpMqtt[]    PROGMEM = "MQTT & Home Assistant";
const char UsermodAutoBrightness::_stateKey[]   PROGMEM = "AutoBri";

static UsermodAutoBrightness auto_brightness;
REGISTER_USERMOD(auto_brightness);
