#pragma once

#include "wled.h"

#ifndef USERMOD_86BOX_RS485_UART
#define USERMOD_86BOX_RS485_UART 1
#endif

#ifndef USERMOD_86BOX_RS485_TX
#define USERMOD_86BOX_RS485_TX 17
#endif

#ifndef USERMOD_86BOX_RS485_RX
#define USERMOD_86BOX_RS485_RX 18
#endif

#ifndef USERMOD_86BOX_RS485_BAUD
#define USERMOD_86BOX_RS485_BAUD 115200
#endif

#ifndef USERMOD_86BOX_RS485_LINE_MAX
#define USERMOD_86BOX_RS485_LINE_MAX 1536
#endif

#ifndef USERMOD_86BOX_RS485_STATUS_INTERVAL_MS
#define USERMOD_86BOX_RS485_STATUS_INTERVAL_MS 15000UL
#endif

#ifndef USERMOD_86BOX_PSU_RELAY_PIN
#define USERMOD_86BOX_PSU_RELAY_PIN 47
#endif

#ifndef USERMOD_86BOX_PSU_RELAY_ENABLED
#define USERMOD_86BOX_PSU_RELAY_ENABLED 1
#endif

#ifndef USERMOD_86BOX_PSU_RELAY_ACTIVE_HIGH
#define USERMOD_86BOX_PSU_RELAY_ACTIVE_HIGH 1
#endif

#ifndef USERMOD_86BOX_PSU_RELAY_ON_LEAD_MS
#define USERMOD_86BOX_PSU_RELAY_ON_LEAD_MS 750UL
#endif

#ifndef USERMOD_86BOX_PSU_RELAY_OFF_HOLD_MS
#define USERMOD_86BOX_PSU_RELAY_OFF_HOLD_MS 10000UL
#endif

#ifndef USERMOD_86BOX_PSU_RELAY_MIN_CYCLE_MS
#define USERMOD_86BOX_PSU_RELAY_MIN_CYCLE_MS 1500UL
#endif

class Usermod86BoxRs485Bridge : public Usermod {
private:
  HardwareSerial bridgeSerial;
  String lineBuffer;
  unsigned long lastStatusMs = 0;
  bool serialReady = false;

  int8_t psuRelayPin = USERMOD_86BOX_PSU_RELAY_PIN;
  bool psuRelayEnabled = USERMOD_86BOX_PSU_RELAY_ENABLED;
  bool psuRelayActiveHigh = USERMOD_86BOX_PSU_RELAY_ACTIVE_HIGH;
  bool psuRelayReady = false;
  bool psuRelayOn = false;
  bool psuRelayFault = false;
  bool psuRelayOffPending = false;
  uint16_t psuRelayOnLeadMs = USERMOD_86BOX_PSU_RELAY_ON_LEAD_MS;
  uint16_t psuRelayOffHoldMs = USERMOD_86BOX_PSU_RELAY_OFF_HOLD_MS;
  uint16_t psuRelayMinCycleMs = USERMOD_86BOX_PSU_RELAY_MIN_CYCLE_MS;
  unsigned long psuRelayChangedMs = 0;
  unsigned long psuRelayOnSinceMs = 0;
  unsigned long psuRelayOffRequestedMs = 0;

  static bool elapsed(unsigned long since, unsigned long interval)
  {
    return millis() - since >= interval;
  }

  static bool jsonTruthy(JsonVariant value, bool fallback)
  {
    if (value.isNull()) return fallback;
    if (value.is<bool>()) return value.as<bool>();
    if (value.is<int>()) return value.as<int>() != 0;
    if (value.is<const char*>()) {
      const char *text = value.as<const char*>();
      if (!text) return fallback;
      return text[0] == '1' || text[0] == 't' || text[0] == 'T' || text[0] == 'o' || text[0] == 'O';
    }
    return fallback;
  }

  bool incomingStateMayNeedPsuPower(JsonObject root) const
  {
    if (!root[F("ps")].isNull()) return true;

    JsonVariant onValue = root[F("on")];
    JsonVariant briValue = root[F("bri")];

    if (!onValue.isNull() && !jsonTruthy(onValue, bri > 0)) return false;
    if (!briValue.isNull() && briValue.as<int>() > 0) return true;
    if (!onValue.isNull() && jsonTruthy(onValue, bri > 0)) return true;
    return false;
  }

  bool wledCurrentlyNeedsPsuPower() const
  {
    return bri > 0 || strip.getBrightness() > 0;
  }

  void clampPsuRelayConfig()
  {
    if (psuRelayOnLeadMs > 30000U) psuRelayOnLeadMs = 30000U;
    if (psuRelayOffHoldMs > 60000U) psuRelayOffHoldMs = 60000U;
    if (psuRelayMinCycleMs > 30000U) psuRelayMinCycleMs = 30000U;
  }

  void writePsuRelayOutput(bool on)
  {
    digitalWrite(psuRelayPin, psuRelayActiveHigh == on ? HIGH : LOW);
    psuRelayOn = on;
    psuRelayChangedMs = millis();
    if (on) psuRelayOnSinceMs = psuRelayChangedMs;
  }

  bool setPsuRelay(bool on, bool force = false)
  {
    if (!psuRelayReady) return false;
    if (psuRelayOn == on) return true;
    if (!force && psuRelayChangedMs != 0 && !elapsed(psuRelayChangedMs, psuRelayMinCycleMs)) return false;

    writePsuRelayOutput(on);
    return true;
  }

  void releasePsuRelay()
  {
    if (!psuRelayReady) return;
    writePsuRelayOutput(false);
    pinManager.deallocatePin(psuRelayPin, PinOwner::UM_Unspecified);
    psuRelayReady = false;
    psuRelayOn = false;
    psuRelayOffPending = false;
  }

  void configurePsuRelay()
  {
    releasePsuRelay();
    clampPsuRelayConfig();
    psuRelayFault = false;

    if (!psuRelayEnabled || psuRelayPin < 0) return;

    if (!pinManager.allocatePin(psuRelayPin, true, PinOwner::UM_Unspecified)) {
      psuRelayFault = true;
      psuRelayReady = false;
      return;
    }

    pinMode(psuRelayPin, OUTPUT);
    psuRelayReady = true;
    psuRelayOn = false;
    psuRelayChangedMs = 0;
    psuRelayOffPending = false;
    digitalWrite(psuRelayPin, psuRelayActiveHigh ? LOW : HIGH);
  }

  void servicePsuRelay()
  {
    if (!psuRelayReady) return;

    if (wledCurrentlyNeedsPsuPower()) {
      psuRelayOffPending = false;
      setPsuRelay(true);
      return;
    }

    if (!psuRelayOn) {
      psuRelayOffPending = false;
      return;
    }

    if (!psuRelayOffPending) {
      psuRelayOffPending = true;
      psuRelayOffRequestedMs = millis();
      return;
    }

    if (elapsed(psuRelayOffRequestedMs, psuRelayOffHoldMs)) {
      setPsuRelay(false);
    }
  }

  void waitForPsuRelayOnLead()
  {
    if (!psuRelayReady) return;

    unsigned long waitStartedMs = millis();
    unsigned long maxWaitMs = (unsigned long)psuRelayMinCycleMs + psuRelayOnLeadMs + 100UL;
    while (!setPsuRelay(true) && !elapsed(waitStartedMs, maxWaitMs)) {
      delay(10);
      yield();
    }

    while (psuRelayOn && !elapsed(psuRelayOnSinceMs, psuRelayOnLeadMs) && !elapsed(waitStartedMs, maxWaitMs)) {
      delay(10);
      yield();
    }
    psuRelayOffPending = false;
  }

  static bool hasStateKeys(JsonObject root)
  {
    return root.containsKey(F("on")) ||
           root.containsKey(F("bri")) ||
           root.containsKey(F("transition")) ||
          root.containsKey(F("ps")) ||
          root.containsKey(F("rb")) ||
           root.containsKey(F("seg"));
  }

  static bool hasConfigKeys(JsonObject root)
  {
    return root.containsKey(F("nw")) ||
           root.containsKey(F("id")) ||
           root.containsKey(F("if"));
  }

  void setTransmitMode(bool transmitting)
  {
#ifdef USERMOD_86BOX_RS485_DE
    digitalWrite(USERMOD_86BOX_RS485_DE, transmitting ? HIGH : LOW);
#else
    (void)transmitting;
#endif
  }

  void writeJsonDocument(JsonDocument &doc)
  {
    if (!serialReady) return;
    setTransmitMode(true);
    serializeJson(doc, bridgeSerial);
    bridgeSerial.print('\n');
    bridgeSerial.flush();
    setTransmitMode(false);
  }

  void sendError(const __FlashStringHelper *detail)
  {
    StaticJsonDocument<128> doc;
    doc[F("error")] = detail;
    writeJsonDocument(doc);
  }

  static void copyJsonValue(JsonObject target, JsonObject source, const char *key)
  {
    JsonVariant value = source[key];
    if (!value.isNull()) target[key] = value;
  }

  static void copyFirstSegment(JsonObject targetState, JsonObject sourceState)
  {
    JsonArray sourceSegments = sourceState[F("seg")].as<JsonArray>();
    if (sourceSegments.isNull() || sourceSegments.size() == 0) return;

    JsonObject sourceSegment = sourceSegments[0].as<JsonObject>();
    if (sourceSegment.isNull()) return;

    JsonArray targetSegments = targetState.createNestedArray(F("seg"));
    JsonObject targetSegment = targetSegments.createNestedObject();
    copyJsonValue(targetSegment, sourceSegment, "fx");
    copyJsonValue(targetSegment, sourceSegment, "pal");
    copyJsonValue(targetSegment, sourceSegment, "sx");
    copyJsonValue(targetSegment, sourceSegment, "ix");
    copyJsonValue(targetSegment, sourceSegment, "cct");

    JsonArray sourceColors = sourceSegment[F("col")].as<JsonArray>();
    if (sourceColors.isNull()) return;

    JsonArray targetColors = targetSegment.createNestedArray(F("col"));
    for (uint8_t colorIndex = 0; colorIndex < sourceColors.size() && colorIndex < 3; colorIndex++) {
      JsonArray sourceColor = sourceColors[colorIndex].as<JsonArray>();
      if (sourceColor.isNull()) continue;

      JsonArray targetColor = targetColors.createNestedArray();
      for (uint8_t channelIndex = 0; channelIndex < sourceColor.size() && channelIndex < 3; channelIndex++) {
        JsonVariant channelValue = sourceColor[channelIndex];
        if (channelValue.is<int>()) targetColor.add(channelValue.as<int>());
      }
    }
  }

  void buildCompactSnapshot(JsonDocument &target, JsonObject sourceState, JsonObject sourceInfo)
  {
    JsonObject targetState = target.createNestedObject(F("state"));
    copyJsonValue(targetState, sourceState, "on");
    copyJsonValue(targetState, sourceState, "bri");
    copyJsonValue(targetState, sourceState, "transition");
    copyFirstSegment(targetState, sourceState);

    JsonObject targetInfo = target.createNestedObject(F("info"));
    copyJsonValue(targetInfo, sourceInfo, "ver");
    copyJsonValue(targetInfo, sourceInfo, "uptime");

    JsonObject sourceLeds = sourceInfo[F("leds")].as<JsonObject>();
    if (!sourceLeds.isNull()) {
      JsonObject targetLeds = targetInfo.createNestedObject(F("leds"));
      copyJsonValue(targetLeds, sourceLeds, "count");
    }

    JsonObject targetRelay = targetInfo.createNestedObject(F("psu"));
    targetRelay[F("pin")] = psuRelayPin;
    targetRelay[F("on")] = psuRelayOn;
    targetRelay[F("ready")] = psuRelayReady;
    targetRelay[F("pendingOff")] = psuRelayOffPending;
    targetRelay[F("fault")] = psuRelayFault;
  }

  void sendStateSnapshot()
  {
    DynamicJsonDocument full(4096);
    JsonObject fullState = full.createNestedObject(F("state"));
    JsonObject fullInfo = full.createNestedObject(F("info"));
    serializeState(fullState);
    serializeInfo(fullInfo);

    StaticJsonDocument<1024> compact;
    buildCompactSnapshot(compact, fullState, fullInfo);
    writeJsonDocument(compact);
    lastStatusMs = millis();
  }

  void applyConfig(JsonObject root)
  {
#ifndef USERMOD_86BOX_RS485_DISABLE_CONFIG_APPLY
    deserializeConfig(root, false);
    serializeConfig();
#else
    (void)root;
#endif
  }

  void applyState(JsonObject root)
  {
    if (incomingStateMayNeedPsuPower(root)) waitForPsuRelayOnLead();
    deserializeState(root, CALL_MODE_DIRECT_CHANGE);
    servicePsuRelay();
  }

  void handleLine(const String &line)
  {
    DynamicJsonDocument doc(3072);
    DeserializationError error = deserializeJson(doc, line);
    if (error) {
      sendError(F("json_parse_failed"));
      return;
    }

    JsonObject root = doc.as<JsonObject>();
    if (root.isNull()) {
      sendError(F("json_object_required"));
      return;
    }

    if (root[F("v")] == true) {
      sendStateSnapshot();
      return;
    }

    bool changed = false;
    if (hasConfigKeys(root)) {
      applyConfig(root);
      changed = true;
    }
    if (hasStateKeys(root)) {
      applyState(root);
      changed = true;
    }

    if (changed) {
      sendStateSnapshot();
    } else {
      sendError(F("unsupported_command"));
    }
  }

  void consumeByte(char receivedByte)
  {
    if (receivedByte == '\n' || receivedByte == '\r') {
      if (lineBuffer.length() > 0) {
        handleLine(lineBuffer);
        lineBuffer = "";
      }
      return;
    }

    if (lineBuffer.length() >= USERMOD_86BOX_RS485_LINE_MAX) {
      lineBuffer = "";
      sendError(F("line_too_long"));
      return;
    }
    lineBuffer += receivedByte;
  }

public:
  Usermod86BoxRs485Bridge() : Usermod("86Box RS485 Bridge", true), bridgeSerial(USERMOD_86BOX_RS485_UART) {}

  void setup() override
  {
#ifdef USERMOD_86BOX_RS485_DE
    pinMode(USERMOD_86BOX_RS485_DE, OUTPUT);
    setTransmitMode(false);
#endif

    lineBuffer.reserve(USERMOD_86BOX_RS485_LINE_MAX);
    bridgeSerial.begin(USERMOD_86BOX_RS485_BAUD, SERIAL_8N1,
                       USERMOD_86BOX_RS485_RX, USERMOD_86BOX_RS485_TX);
    serialReady = true;
    configurePsuRelay();
    initDone = true;
  }

  void loop() override
  {
    servicePsuRelay();

    while (serialReady && bridgeSerial.available() > 0) {
      int received = bridgeSerial.read();
      if (received >= 0) consumeByte((char)received);
    }

    if (serialReady && millis() - lastStatusMs > USERMOD_86BOX_RS485_STATUS_INTERVAL_MS) {
      sendStateSnapshot();
    }
  }

  void connected() override
  {
    sendStateSnapshot();
  }

  void addToJsonInfo(JsonObject &root) override
  {
    JsonObject usermodsInfo = root[F("u")].as<JsonObject>();
    if (usermodsInfo.isNull()) usermodsInfo = root.createNestedObject(F("u"));

    char detail[64];
    snprintf(detail, sizeof(detail), "UART%d TX%d RX%d @ %lu",
             USERMOD_86BOX_RS485_UART,
             USERMOD_86BOX_RS485_TX,
             USERMOD_86BOX_RS485_RX,
             (unsigned long)USERMOD_86BOX_RS485_BAUD);

    JsonArray bridgeInfo = usermodsInfo.createNestedArray(F("86Box RS485"));
    bridgeInfo.add(detail);

    char relayDetail[96];
    if (!psuRelayEnabled || psuRelayPin < 0) {
      snprintf(relayDetail, sizeof(relayDetail), "disabled");
    } else if (psuRelayFault) {
      snprintf(relayDetail, sizeof(relayDetail), "GPIO%d allocation failed", psuRelayPin);
    } else {
      snprintf(relayDetail, sizeof(relayDetail), "GPIO%d %s, lead %ums, hold %ums",
               psuRelayPin,
               psuRelayOn ? "on" : "off",
               psuRelayOnLeadMs,
               psuRelayOffHoldMs);
    }

    JsonArray relayInfo = usermodsInfo.createNestedArray(F("86Box PSU Relay"));
    relayInfo.add(relayDetail);
  }

  void addToConfig(JsonObject &root) override
  {
    JsonObject top = root.createNestedObject(_name);
    top[F("enabled")] = enabled;
    top[F("psuRelayEnabled")] = psuRelayEnabled;
    top[F("psuRelayPin")] = psuRelayPin;
    top[F("psuRelayActiveHigh")] = psuRelayActiveHigh;
    top[F("psuRelayOnLeadMs")] = psuRelayOnLeadMs;
    top[F("psuRelayOffHoldMs")] = psuRelayOffHoldMs;
    top[F("psuRelayMinCycleMs")] = psuRelayMinCycleMs;
  }

  bool readFromConfig(JsonObject &root) override
  {
    JsonObject top = root[_name];
    if (top.isNull()) return false;

    bool oldRelayEnabled = psuRelayEnabled;
    int8_t oldRelayPin = psuRelayPin;
    bool oldRelayActiveHigh = psuRelayActiveHigh;

    bool configComplete = true;
    configComplete &= getJsonValue(top[F("enabled")], enabled);
    configComplete &= getJsonValue(top[F("psuRelayEnabled")], psuRelayEnabled);
    configComplete &= getJsonValue(top[F("psuRelayPin")], psuRelayPin);
    configComplete &= getJsonValue(top[F("psuRelayActiveHigh")], psuRelayActiveHigh);
    configComplete &= getJsonValue(top[F("psuRelayOnLeadMs")], psuRelayOnLeadMs);
    configComplete &= getJsonValue(top[F("psuRelayOffHoldMs")], psuRelayOffHoldMs);
    configComplete &= getJsonValue(top[F("psuRelayMinCycleMs")], psuRelayMinCycleMs);
    clampPsuRelayConfig();

    if (initDone && (oldRelayEnabled != psuRelayEnabled || oldRelayPin != psuRelayPin || oldRelayActiveHigh != psuRelayActiveHigh)) {
      configurePsuRelay();
    }

    return configComplete;
  }
};