// ============================================================
// AgroSense - Urban Smart Irrigation System
// ESP32 WROOM-32 (38-pin)
// ============================================================
// Pin Summary:
//   GPIO32 - Pump ON/OFF (F5305S Signal+)
//   GPIO33 - DHT11 DATA
//   GPIO34 - Soil Sensor 1 AOUT (Tomato)
//   GPIO36 - Soil Sensor 2 AOUT (Pechay)
//   GPIO26 - HC-SR04 TRIG (Well level)
//   GPIO27 - HC-SR04 ECHO (Well level)
//   GPIO25 - Servo PWM (SG90)
//   GPIO21 - LCD SDA (I2C)
//   GPIO22 - LCD SCL (I2C)
//   GPIO18 - AUTO/MANUAL momentary push button (active LOW, toggles mode)
//   GPIO4  - NEXT button (active LOW)
//   GPIO5  - SELECT button (active LOW)
//   GPIO23 - BACK button (active LOW)
//   GPIO19 - Status LED (manual override indicator)
// ============================================================

#include <WiFi.h>
#include <WiFiManager.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <DHT.h>
#include <ESP32Servo.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <math.h>
#include "secrets.h"

// ============================================================
// Network and backend config
// ============================================================
const char* SERVER_BASE_URL = SECRET_SERVER_BASE_URL;
const char* API_KEY         = SECRET_API_KEY;
const char* DEVICE_ID       = SECRET_DEVICE_ID;

// ============================================================
// Pin assignments
// ============================================================
const int PUMP_PIN   = 32;
const int DHT_PIN    = 33;
const int SOIL1_PIN  = 34;  // Tomato
const int SOIL2_PIN  = 36;  // Pechay
const int SERVO_PIN  = 25;
const int WATER_TRIG_PIN = 26;
const int WATER_ECHO_PIN = 27;

const int LCD_SDA    = 21;
const int LCD_SCL    = 22;

const int BTN_MODE   = 18;  // Momentary push button: press to toggle AUTO/MANUAL
const int BTN_NEXT   = 4;   // Momentary: scroll / navigate
const int BTN_SELECT = 5;   // Momentary: confirm
const int BTN_BACK   = 23;  // Momentary: back / cancel

const int LED_PIN    = 19;  // Manual override indicator

// ============================================================
// Hardware config
// ============================================================
const bool HAS_SENSOR2         = true;
const int  SERVO_ANGLE_TOMATO  = 90;
const int  SERVO_ANGLE_PECHAY  = 0;
const int  LCD_ADDR            = 0x3F; // Change to 0x27 if display is blank
const int  LCD_COLS            = 16;
const int  LCD_ROWS            = 2;
const float WELL_DEPTH_CM      = 25.0f;
const float WELL_CAPACITY_ML   = 1000.0f;
const float WELL_LOW_WARNING_ML = 200.0f;
const float WELL_CRITICAL_ML    = 50.0f;
const unsigned long WATER_ECHO_TIMEOUT_US = 30000;
const int WATER_ECHO_RETRIES = 3;
const unsigned long WATER_RETRY_DELAY_MS = 5;
const bool WATER_LEVEL_DEBUG_LOG = true;

// Offline-fallback thresholds on the 0-100% calibrated sensor scale.
// Sensor 0% = PWP (12% VWC), Sensor 100% = FC (29% VWC).
// Tomato: MAD 40% of available water -> ON at 60% (approx 22.2% VWC), OFF at 95% (approx 28.2% VWC).
// Pechay: MAD 15% of available water -> ON at 80% (approx 25.6% VWC), OFF at 95% (approx 28.2% VWC).
// These only apply when the ESP32 cannot reach the server (Wi-Fi offline).
const float TOMATO_MOISTURE_ON_DEFAULT     = 60.0f;
const float TOMATO_MOISTURE_OFF_DEFAULT    = 95.0f;
const float PECHAY_MOISTURE_ON_DEFAULT     = 80.0f;
const float PECHAY_MOISTURE_OFF_DEFAULT    = 95.0f;
const unsigned long TOMATO_PWM_ON_MS_DEFAULT  = 60000UL;
const unsigned long TOMATO_PWM_OFF_MS_DEFAULT = 60000UL;
const unsigned long PECHAY_PWM_ON_MS_DEFAULT  = 15000UL;
const unsigned long PECHAY_PWM_OFF_MS_DEFAULT = 45000UL;
const unsigned long MANUAL_WATER_TIMEOUT_TOMATO_MS = 60000UL;
const unsigned long MANUAL_WATER_TIMEOUT_PECHAY_MS = 20000UL;
const unsigned long PECHAY_ANOXIA_GUARD_MS = 1800000UL;
const float MOISTURE_DEBOUNCE_PCT = 1.0f;

// ============================================================
// Pump polarity config
// ------------------------------------------------------------
// F5305S is a P-channel MOSFET.
//
// If wired as a bare high-side switch (gate tied directly to GPIO):
//   HIGH = gate pulled up → MOSFET OFF → pump OFF
//   LOW  = gate pulled low → MOSFET ON  → pump ON
//   → Set PUMP_ACTIVE_HIGH to false
//
// If using a MOSFET driver module (e.g. with an N-channel input stage
// or an inverting gate driver, which inverts the logic):
//   HIGH = pump ON, LOW = pump OFF
//   → Set PUMP_ACTIVE_HIGH to true  (default)
//
// If pump is unresponsive after upload, flip this value.
// ============================================================
#define PUMP_ACTIVE_HIGH true

// ============================================================
// Objects
// ============================================================
DHT              dht(DHT_PIN, DHT11);
Servo            valveServo;
LiquidCrystal_I2C lcd(LCD_ADDR, LCD_COLS, LCD_ROWS);

// ============================================================
// Sensor state
// ============================================================
float moisture1Pct   = 0.0f;
float moisture2Pct   = 0.0f;
float moistureAvgPct = 0.0f;
float temperatureC   = NAN;
float humidityPct    = NAN;
float flowMl         = 0.0f;
float wellWaterMl    = 0.0f;

// ============================================================
// Irrigation config (can be overridden by server)
// ============================================================
float         moistureOnThreshold  = TOMATO_MOISTURE_ON_DEFAULT;
float         moistureOffThreshold = TOMATO_MOISTURE_OFF_DEFAULT;
unsigned long slowPwmOnMs          = TOMATO_PWM_ON_MS_DEFAULT;
unsigned long slowPwmOffMs         = TOMATO_PWM_OFF_MS_DEFAULT;

float         moistureOnTomato      = TOMATO_MOISTURE_ON_DEFAULT;
float         moistureOffTomato     = TOMATO_MOISTURE_OFF_DEFAULT;
float         moistureOnPechay      = PECHAY_MOISTURE_ON_DEFAULT;
float         moistureOffPechay     = PECHAY_MOISTURE_OFF_DEFAULT;
unsigned long slowPwmOnMsTomato     = TOMATO_PWM_ON_MS_DEFAULT;
unsigned long slowPwmOffMsTomato    = TOMATO_PWM_OFF_MS_DEFAULT;
unsigned long slowPwmOnMsPechay     = PECHAY_PWM_ON_MS_DEFAULT;
unsigned long slowPwmOffMsPechay    = PECHAY_PWM_OFF_MS_DEFAULT;
unsigned long manualWaterTimeoutMsTomato = MANUAL_WATER_TIMEOUT_TOMATO_MS;
unsigned long manualWaterTimeoutMsPechay = MANUAL_WATER_TIMEOUT_PECHAY_MS;
String        plantStageTomato      = "--";
String        plantStagePechay      = "--";
unsigned long pechayHighMoistureStartMs = 0;
bool          pechayAnoxiaGuardActive   = false;
bool          pwmPhaseIsOn              = true;

float sensorCalA = -0.0133f;
float sensorCalB = -1.986f;
float sensorCalC = 75.441f;

// Calibration voltages pre-loaded from measured sensor data.
// Back-calculated from dry/wet VWC% readings via the quadratic model.
// dryV > wetV + 0.001 activates the linear 2-point calibration in voltageToVwcPct().
// Values are overwritten by the server on first successful sync.
float sensorDryVoltTomato = 2.394f;
float sensorWetVoltTomato = 1.900f;
float sensorDryVoltPechay = 2.456f;
float sensorWetVoltPechay = 1.277f;

// ============================================================
// Pump and crop state
// ============================================================
bool          desiredPumpOn     = false;
bool          prevDesiredPumpOn = false;  // Used to detect rising edge in slowPwmTick
bool          pumpOutputOn      = false;
unsigned long pwmPhaseStartMs   = 0;
String        activeCrop        = "Tomato";
int           lastServoCommandVersion = 0;  // Tracks last ACK'd servo command version
int           lastPumpCommandVersion  = 0;  // Tracks last ACK'd pump command version

// ============================================================
// Timing
// ============================================================
unsigned long lastSensorReadMs = 0;
unsigned long lastSyncMs       = 0;
unsigned long lastLcdUpdateMs  = 0;
const unsigned long SENSOR_INTERVAL_MS  = 1000;
const unsigned long SYNC_INTERVAL_MS    = 5000;
const unsigned long LCD_UPDATE_MS       = 3000; // Home screen rotation

// ============================================================
// UI state
// ============================================================

// Modes
enum SystemMode { MODE_AUTO, MODE_MANUAL };
SystemMode currentMode = MODE_AUTO;

void applySystemMode(SystemMode nextMode, const char* source);

// Menu states
enum MenuState {
  SCREEN_HOME,
  SCREEN_MENU,
  SCREEN_VIEW_SENSORS,
  SCREEN_SYSTEM_STATUS,
  SCREEN_MANUAL_SELECT_PLOT,
  SCREEN_MANUAL_CONFIRM_WET,
  SCREEN_MANUAL_WATERING,
  SCREEN_MANUAL_DONE
};
MenuState currentScreen = SCREEN_HOME;

// Sub-screen indices for scrollable screens
int  homeScreenIdx         = 0; // 0=moisture, 1=status, 2=pump, 3=well
int  menuIdx               = 0; // 0=View Sensors, 1=Manual Water, 2=System Status
int  viewSensorIdx         = 0; // 0=Tomato, 1=Pechay, 2=Environment, 3=Well
int  systemStatusIdx       = 0; // 0=pump, 1=servo
int  manualPlotIdx         = 0; // 0=Tomato, 1=Pechay
int  manualConfirmIdx      = 0; // 0=YES, 1=NO

// Manual watering state
String        manualTargetCrop     = "Tomato";
bool          manualWateringActive = false;
unsigned long manualWaterStartMs   = 0;
unsigned long manualWaterTimeoutMs = MANUAL_WATER_TIMEOUT_TOMATO_MS;

// LCD dirty flag — only redraw when needed
bool lcdNeedsRedraw = true;

// Button debounce
unsigned long lastNextPressMs   = 0;
unsigned long lastSelectPressMs = 0;
unsigned long lastBackPressMs   = 0;
unsigned long lastModePressMs   = 0;
const unsigned long DEBOUNCE_MS = 200;

// Button edge detection — tracks previous state to fire only on falling edge
bool prevBtnMode   = HIGH;
bool prevBtnNext   = HIGH;
bool prevBtnSelect = HIGH;
bool prevBtnBack   = HIGH;

// ============================================================
// Utility
// ============================================================

float clampFloat(float value, float minVal, float maxVal) {
  return fminf(maxVal, fmaxf(minVal, value));
}

float adcToVoltage(int raw) {
  return (raw * 3.0f) / 4095.0f;
}

// Read ADC multiple times, drop min/max outliers, then average.
int readAdcAveraged(int pin, int samples = 8) {
  if (samples < 3) {
    samples = 3;
  }

  int minVal = 4095;
  int maxVal = 0;
  long sum = 0;

  for (int i = 0; i < samples; i++) {
    const int value = analogRead(pin);
    if (value < minVal) minVal = value;
    if (value > maxVal) maxVal = value;
    sum += value;
    delayMicroseconds(50);
  }

  return (int)((sum - minVal - maxVal) / (samples - 2));
}

void readWaterLevel() {
  unsigned long duration = 0;

  for (int attempt = 0; attempt < WATER_ECHO_RETRIES && duration == 0; attempt++) {
    digitalWrite(WATER_TRIG_PIN, LOW);
    delayMicroseconds(2);
    digitalWrite(WATER_TRIG_PIN, HIGH);
    delayMicroseconds(10);
    digitalWrite(WATER_TRIG_PIN, LOW);

    duration = pulseIn(WATER_ECHO_PIN, HIGH, WATER_ECHO_TIMEOUT_US);
    if (duration == 0 && attempt < (WATER_ECHO_RETRIES - 1)) {
      delay(WATER_RETRY_DELAY_MS);
    }
  }

  if (duration == 0) {
    if (WATER_LEVEL_DEBUG_LOG) {
      Serial.println("[water] echo timeout");
    }
    return; // Keep last valid reading when sensor misses an echo
  }

  const float distanceCm   = (duration * 0.0343f) * 0.5f;
  const float waterDepthCm = clampFloat(WELL_DEPTH_CM - distanceCm, 0.0f, WELL_DEPTH_CM);
  const float levelRatio   = waterDepthCm / WELL_DEPTH_CM;
  wellWaterMl = clampFloat(levelRatio * WELL_CAPACITY_ML, 0.0f, WELL_CAPACITY_ML);

  if (WATER_LEVEL_DEBUG_LOG) {
    Serial.print("[water] us=");
    Serial.print(duration);
    Serial.print(" dist_cm=");
    Serial.print(distanceCm, 1);
    Serial.print(" depth_cm=");
    Serial.print(waterDepthCm, 1);
    Serial.print(" ml=");
    Serial.println((int)wellWaterMl);
  }
}

// Calibration model: Vs = -0.0133*theta^2 - 1.986*theta + 75.441
float voltageToVwcPct(float vs, const String& crop) {
  float dryV = 0.0f;
  float wetV = 0.0f;
  if (crop == "Pechay") {
    dryV = sensorDryVoltPechay;
    wetV = sensorWetVoltPechay;
  } else {
    dryV = sensorDryVoltTomato;
    wetV = sensorWetVoltTomato;
  }

  if (dryV > wetV + 0.001f) {
    const float pct = ((dryV - vs) / (dryV - wetV)) * 100.0f;
    return clampFloat(pct, 0.0f, 100.0f);
  }

  const float a            = sensorCalA;
  const float b            = sensorCalB;
  const float c            = sensorCalC - vs;
  const float discriminant = (b * b) - (4.0f * a * c);
  if (discriminant < 0) return 0.0f;
  const float theta = (-b - sqrtf(discriminant)) / (2.0f * a);
  return clampFloat(theta, 0.0f, 100.0f);
}

// Pad string to exactly 16 chars — clears leftover LCD chars
String padTo16(String s) {
  while (s.length() < 16) s += ' ';
  if (s.length() > 16) s = s.substring(0, 16);
  return s;
}

void getCropThresholds(const String& crop, float& onThresh, float& offThresh) {
  if (crop == "Pechay") {
    onThresh = moistureOnPechay;
    offThresh = moistureOffPechay;
    return;
  }
  onThresh = moistureOnTomato;
  offThresh = moistureOffTomato;
}

void getCropPwmTiming(const String& crop, unsigned long& onMs, unsigned long& offMs) {
  if (crop == "Pechay") {
    onMs = slowPwmOnMsPechay;
    offMs = slowPwmOffMsPechay;
    return;
  }
  onMs = slowPwmOnMsTomato;
  offMs = slowPwmOffMsTomato;
}

String moistureLabel(float pct, const String& crop = "") {
  const String cropToUse = crop.length() > 0 ? crop : activeCrop;
  float onThresh = moistureOnThreshold;
  float offThresh = moistureOffThreshold;
  getCropThresholds(cropToUse, onThresh, offThresh);

  if (isnan(pct))                          return "N/A  ";
  if (pct < onThresh)                      return "DRY  ";
  if (pct > offThresh)                     return "WET  ";
  return "OK   ";
}

// ============================================================
// LCD helpers
// ============================================================

void lcdPrint(int row, String text) {
  lcd.setCursor(0, row);
  lcd.print(padTo16(text));
}

void lcdClear() {
  lcd.clear();
}

// ============================================================
// Hardware control
// ============================================================

void setPumpOutput(bool on) {
  pumpOutputOn = on;
#if PUMP_ACTIVE_HIGH
  digitalWrite(PUMP_PIN, on ? HIGH : LOW);
#else
  // P-channel bare high-side: logic inverted
  digitalWrite(PUMP_PIN, on ? LOW : HIGH);
#endif
}

void updateServoForCrop(const String& crop) {
  if (crop == "Pechay") {
    valveServo.write(SERVO_ANGLE_PECHAY);
    return;
  }
  valveServo.write(SERVO_ANGLE_TOMATO);
}

void setLed(bool on) {
  digitalWrite(LED_PIN, on ? HIGH : LOW);
}

// ============================================================
// WiFi
// ============================================================

void connectWiFi() {
  if (WiFi.status() == WL_CONNECTED) return;

  WiFi.mode(WIFI_STA);
  WiFiManager wm;
  wm.setAPCallback([](WiFiManager*) {
    lcdPrint(0, "WiFi Setup Mode ");
    lcdPrint(1, "Go 192.168.4.1 ");
  });
  wm.setConfigPortalTimeout(180);

  Serial.println("[wifi] attempting auto-connect");
  const bool connected = wm.autoConnect("AgroSense-Setup");

  if (connected && WiFi.status() == WL_CONNECTED) {
    Serial.println();
    Serial.print("[wifi] connected, IP: ");
    Serial.println(WiFi.localIP());
  } else {
    Serial.println();
    Serial.println("[wifi] config portal timeout, running offline");
  }
}

// ============================================================
// Sensor reading
// ============================================================

void readSensors() {
  const int   raw1 = readAdcAveraged(SOIL1_PIN);
  const float v1   = adcToVoltage(raw1);
  moisture1Pct     = voltageToVwcPct(v1, "Tomato");

  if (HAS_SENSOR2) {
    const int   raw2 = readAdcAveraged(SOIL2_PIN);
    const float v2   = adcToVoltage(raw2);
    moisture2Pct     = voltageToVwcPct(v2, "Pechay");
    moistureAvgPct   = (moisture1Pct + moisture2Pct) * 0.5f;
  } else {
    moisture2Pct   = NAN;
    moistureAvgPct = moisture1Pct;
  }

  const float dhtTemp = dht.readTemperature();
  const float dhtHum  = dht.readHumidity();
  if (!isnan(dhtTemp)) temperatureC = dhtTemp;
  if (!isnan(dhtHum))  humidityPct  = dhtHum;

  readWaterLevel();
}

// ============================================================
// Irrigation logic (AUTO mode only)
// ============================================================

void evaluateRuleBasedPump() {
  if (currentMode == MODE_MANUAL) return; // Manual mode bypasses this

  if (wellWaterMl < WELL_CRITICAL_ML) {
    desiredPumpOn = false;
    return;
  }

  float moistureForCrop = moisture1Pct;
  float onThresh = moistureOnTomato;
  float offThresh = moistureOffTomato;

  getCropThresholds(activeCrop, onThresh, offThresh);

  if (activeCrop == "Pechay") {
    if (!HAS_SENSOR2 || isnan(moisture2Pct)) {
      desiredPumpOn = false;
      return;
    }
    moistureForCrop = moisture2Pct;

    if (moistureForCrop > offThresh) {
      if (pechayHighMoistureStartMs == 0) {
        pechayHighMoistureStartMs = millis();
      } else if ((millis() - pechayHighMoistureStartMs) >= PECHAY_ANOXIA_GUARD_MS) {
        pechayAnoxiaGuardActive = true;
      }
    } else {
      pechayHighMoistureStartMs = 0;
      if (moistureForCrop < onThresh) {
        pechayAnoxiaGuardActive = false;
      }
    }

    if (pechayAnoxiaGuardActive) {
      desiredPumpOn = false;
      return;
    }
  } else {
    pechayHighMoistureStartMs = 0;
    pechayAnoxiaGuardActive = false;
  }

  if (moistureForCrop < (onThresh - MOISTURE_DEBOUNCE_PCT)) {
    desiredPumpOn = true;
    return;
  }
  if (moistureForCrop > (offThresh + MOISTURE_DEBOUNCE_PCT)) {
    desiredPumpOn = false;
  }
}

void slowPwmTick() {
  if (currentMode == MODE_MANUAL) return; // Manual mode controls pump directly

  const unsigned long now = millis();
  unsigned long activePwmOnMs = slowPwmOnMs;
  unsigned long activePwmOffMs = slowPwmOffMs;

  getCropPwmTiming(activeCrop, activePwmOnMs, activePwmOffMs);

  if (!desiredPumpOn) {
    if (pumpOutputOn) setPumpOutput(false);
    pwmPhaseStartMs  = now;
    prevDesiredPumpOn = false;
    pwmPhaseIsOn = true;
    return;
  }

  // Rising edge: desiredPumpOn just became true → fire pump immediately,
  // skipping the off-phase wait that would otherwise stall activation.
  if (!prevDesiredPumpOn) {
    setPumpOutput(true);
    pwmPhaseIsOn = true;
    pwmPhaseStartMs   = now;
    prevDesiredPumpOn = true;
    return;
  }

  if (pwmPhaseIsOn) {
    if (now - pwmPhaseStartMs >= activePwmOnMs) {
      if (activePwmOffMs == 0) {
        setPumpOutput(true);
        pwmPhaseStartMs = now;
      } else {
        setPumpOutput(false);
        pwmPhaseIsOn = false;
        pwmPhaseStartMs = now;
      }
    }
    return;
  }

  if (now - pwmPhaseStartMs >= activePwmOffMs) {
    setPumpOutput(true);
    pwmPhaseIsOn = true;
    pwmPhaseStartMs = now;
  }
}

// ============================================================
// Server sync
// ============================================================

void syncWithServer() {
  if (WiFi.status() != WL_CONNECTED) {
    WiFi.reconnect();
    return;
  }

  HTTPClient http;
  const String url = String(SERVER_BASE_URL) + "/api/ingest?key=" + API_KEY;
  http.begin(url);
  http.addHeader("Content-Type", "application/json");
  http.addHeader("X-API-Key", API_KEY);

  StaticJsonDocument<800> payload;
  payload["device_id"]         = DEVICE_ID;
  payload["moisture1"]         = moisture1Pct;
  if (HAS_SENSOR2) payload["moisture2"] = moisture2Pct;
  payload["moisture"]          = moistureAvgPct;
  payload["temperature"]       = temperatureC;
  payload["humidity"]          = humidityPct;
  payload["flow_ml"]           = flowMl;
  payload["well_water_ml"]     = (int)wellWaterMl;
  payload["well_capacity_ml"]  = (int)WELL_CAPACITY_ML;
  payload["pump_on"]           = pumpOutputOn;
  payload["mode"]              = (currentMode == MODE_AUTO) ? "AUTO" : "MANUAL";
  payload["active_crop"]       = activeCrop;
  payload["servo_ack_version"] = lastServoCommandVersion;
  payload["pump_ack_version"]  = lastPumpCommandVersion;

  String body;
  serializeJson(payload, body);

  const int statusCode = http.POST(body);
  if (statusCode > 0) {
    const String             responseBody = http.getString();
    StaticJsonDocument<1024> response;
    const DeserializationError err = deserializeJson(response, responseBody);

    if (!err) {
      // ---- Servo / crop: apply in both AUTO and MANUAL (when not actively watering) ----
      if (!manualWateringActive && response.containsKey("active_crop")) {
        const char* nextCropRaw = response["active_crop"].as<const char*>();
        if (nextCropRaw != nullptr) {
          const String nextCrop(nextCropRaw);
          if (nextCrop.length() > 0 && nextCrop != activeCrop) {
            Serial.print("[servo] crop ");
            Serial.print(activeCrop);
            Serial.print(" -> ");
            Serial.println(nextCrop);
            activeCrop = nextCrop;
            updateServoForCrop(activeCrop);
            lcdNeedsRedraw = true;
          }
        }
      }

      if (!manualWateringActive && response.containsKey("mode_command")) {
        const char* modeCommandRaw = response["mode_command"].as<const char*>();
        if (modeCommandRaw != nullptr) {
          const String modeCommand(modeCommandRaw);
          if (modeCommand == "MANUAL") {
            applySystemMode(MODE_MANUAL, "server");
          } else if (modeCommand == "AUTO") {
            applySystemMode(MODE_AUTO, "server");
          }
        }
      }

      if (response.containsKey("manual_water_ms_tomato")) {
        const unsigned long timeoutMs = response["manual_water_ms_tomato"].as<unsigned long>();
        if (timeoutMs > 0) manualWaterTimeoutMsTomato = timeoutMs;
      }

      if (response.containsKey("manual_water_ms_pechay")) {
        const unsigned long timeoutMs = response["manual_water_ms_pechay"].as<unsigned long>();
        if (timeoutMs > 0) manualWaterTimeoutMsPechay = timeoutMs;
      }

      if (response.containsKey("plant_stage_tomato")) {
        const char* stageRaw = response["plant_stage_tomato"].as<const char*>();
        if (stageRaw != nullptr && stageRaw[0] != '\0') {
          plantStageTomato = String(stageRaw);
        } else {
          plantStageTomato = "--";
        }
      }

      if (response.containsKey("plant_stage_pechay")) {
        const char* stageRaw = response["plant_stage_pechay"].as<const char*>();
        if (stageRaw != nullptr && stageRaw[0] != '\0') {
          plantStagePechay = String(stageRaw);
        } else {
          plantStagePechay = "--";
        }
      }

      // ACK the servo command version the server sent
      if (response.containsKey("servo_command_version"))
        lastServoCommandVersion = response["servo_command_version"].as<int>();

      // ---- Pump / thresholds: AUTO mode only ----
      if (currentMode == MODE_AUTO) {
        if (response.containsKey("pump_command"))
          desiredPumpOn = response["pump_command"].as<bool>();

        if (response.containsKey("moisture_on_tomato"))
          moistureOnTomato = response["moisture_on_tomato"].as<float>();

        if (response.containsKey("moisture_off_tomato"))
          moistureOffTomato = response["moisture_off_tomato"].as<float>();

        if (response.containsKey("moisture_on_pechay"))
          moistureOnPechay = response["moisture_on_pechay"].as<float>();

        if (response.containsKey("moisture_off_pechay"))
          moistureOffPechay = response["moisture_off_pechay"].as<float>();

        if (response.containsKey("pwm_on_ms_tomato"))
          slowPwmOnMsTomato = response["pwm_on_ms_tomato"].as<unsigned long>();

        if (response.containsKey("pwm_off_ms_tomato"))
          slowPwmOffMsTomato = response["pwm_off_ms_tomato"].as<unsigned long>();

        if (response.containsKey("pwm_on_ms_pechay"))
          slowPwmOnMsPechay = response["pwm_on_ms_pechay"].as<unsigned long>();

        if (response.containsKey("pwm_off_ms_pechay"))
          slowPwmOffMsPechay = response["pwm_off_ms_pechay"].as<unsigned long>();

        if (response.containsKey("sensor_cal_a"))
          sensorCalA = response["sensor_cal_a"].as<float>();

        if (response.containsKey("sensor_cal_b"))
          sensorCalB = response["sensor_cal_b"].as<float>();

        if (response.containsKey("sensor_cal_c"))
          sensorCalC = response["sensor_cal_c"].as<float>();

        if (response.containsKey("sensor_dry_voltage_tomato"))
          sensorDryVoltTomato = response["sensor_dry_voltage_tomato"].as<float>();

        if (response.containsKey("sensor_wet_voltage_tomato"))
          sensorWetVoltTomato = response["sensor_wet_voltage_tomato"].as<float>();

        if (response.containsKey("sensor_dry_voltage_pechay"))
          sensorDryVoltPechay = response["sensor_dry_voltage_pechay"].as<float>();

        if (response.containsKey("sensor_wet_voltage_pechay"))
          sensorWetVoltPechay = response["sensor_wet_voltage_pechay"].as<float>();

        if (response.containsKey("moisture_on_threshold"))
          moistureOnThreshold = response["moisture_on_threshold"].as<float>();

        if (response.containsKey("moisture_off_threshold"))
          moistureOffThreshold = response["moisture_off_threshold"].as<float>();

        if (response.containsKey("pwm_on_ms"))
          slowPwmOnMs = response["pwm_on_ms"].as<unsigned long>();

        if (response.containsKey("pwm_off_ms"))
          slowPwmOffMs = response["pwm_off_ms"].as<unsigned long>();

        // Keep legacy globals in sync for any remaining code paths.
        if (activeCrop == "Pechay") {
          moistureOnThreshold = moistureOnPechay;
          moistureOffThreshold = moistureOffPechay;
          slowPwmOnMs = slowPwmOnMsPechay;
          slowPwmOffMs = slowPwmOffMsPechay;
        } else {
          moistureOnThreshold = moistureOnTomato;
          moistureOffThreshold = moistureOffTomato;
          slowPwmOnMs = slowPwmOnMsTomato;
          slowPwmOffMs = slowPwmOffMsTomato;
        }

        // ACK the pump command version the server sent
        if (response.containsKey("pump_command_version"))
          lastPumpCommandVersion = response["pump_command_version"].as<int>();
      }
    }

    Serial.print("[sync] ");
    Serial.print(statusCode);
    Serial.print(" m1=");
    Serial.print(moisture1Pct, 1);
    Serial.print(" m2=");
    Serial.print(moisture2Pct, 1);
    Serial.print(" pump=");
    Serial.print(pumpOutputOn ? "ON" : "OFF");
    Serial.print(" mode=");
    Serial.println(currentMode == MODE_AUTO ? "AUTO" : "MANUAL");
  } else {
    Serial.print("[sync] POST failed: ");
    Serial.println(statusCode);
  }

  http.end();
}

// ============================================================
// LCD display rendering
// ============================================================

void renderLcd() {
  if (!lcdNeedsRedraw) return;
  lcdNeedsRedraw = false;

  switch (currentScreen) {

    // ----------------------------------------------------------
    case SCREEN_HOME:
      switch (homeScreenIdx) {
        case 0:
          // Row 0: Tom:45% Pec:72%
          // Row 1: 28C 65% AUTO
          {
            String r0 = "Tom:";
            if (isnan(moisture1Pct)) r0 += "N/A  ";
            else { r0 += String((int)moisture1Pct); r0 += "% "; }
            r0 += "Pec:";
            if (isnan(moisture2Pct)) r0 += "N/A ";
            else { r0 += String((int)moisture2Pct); r0 += "%"; }

            String r1 = "";
            if (!isnan(temperatureC)) { r1 += String((int)temperatureC); r1 += "C "; }
            else r1 += "?C ";
            if (!isnan(humidityPct)) { r1 += String((int)humidityPct); r1 += "% "; }
            else r1 += "?% ";
            r1 += (currentMode == MODE_AUTO) ? "AUTO" : "MAN";

            lcdPrint(0, r0);
            lcdPrint(1, r1);
          }
          break;

        case 1:
          // Row 0: Tomato status
          // Row 1: Pechay status
          {
            String r0 = "Tom: ";
            r0 += moistureLabel(moisture1Pct, "Tomato");
            r0 += String((int)moisture1Pct);
            r0 += "%";
            String r1 = "Pec: ";
            r1 += moistureLabel(moisture2Pct, "Pechay");
            r1 += String((int)moisture2Pct);
            r1 += "%";
            lcdPrint(0, r0);
            lcdPrint(1, r1);
          }
          break;

        case 2:
          // Row 0: Pump and crop
          // Row 1: NEXT=more
          {
            String r0 = "Pump:";
            r0 += pumpOutputOn ? "ON " : "OFF";
            r0 += " ";
            r0 += activeCrop;
            lcdPrint(0, r0);
            lcdPrint(1, "NEXT:more SEL:--");
          }
          break;

        case 3:
          // Row 0: Well level in mL
          // Row 1: Well level in percent + menu hint
          {
            const int pct = (int)clampFloat((wellWaterMl / WELL_CAPACITY_ML) * 100.0f, 0.0f, 100.0f);
            String r0 = "Well:";
            r0 += String((int)wellWaterMl);
            r0 += "mL";
            String r1 = "Lvl:";
            r1 += String(pct);
            r1 += "% NEXT:menu";
            lcdPrint(0, r0);
            lcdPrint(1, r1);
          }
          break;
      }
      break;

    // ----------------------------------------------------------
    case SCREEN_MENU:
      switch (menuIdx) {
        case 0:
          lcdPrint(0, ">1.View Sensors ");
          lcdPrint(1, "SEL=OK NEXT=next");
          break;
        case 1:
          lcdPrint(0, ">2.Manual Water ");
          lcdPrint(1, "SEL=OK NEXT=next");
          break;
        case 2:
          lcdPrint(0, ">3.System Status");
          lcdPrint(1, "SEL=OK BACK=home");
          break;
      }
      break;

    // ----------------------------------------------------------
    case SCREEN_VIEW_SENSORS:
      switch (viewSensorIdx) {
        case 0:
          lcdPrint(0, "TOMATO PLOT     ");
          {
            String r1 = "Moist:";
            r1 += String((int)moisture1Pct);
            r1 += "% ";
            r1 += moistureLabel(moisture1Pct, "Tomato");
            lcdPrint(1, r1);
          }
          break;
        case 1:
          lcdPrint(0, "PECHAY PLOT     ");
          {
            String r1 = "Moist:";
            if (isnan(moisture2Pct)) r1 += "N/A     ";
            else {
              r1 += String((int)moisture2Pct);
              r1 += "% ";
              r1 += moistureLabel(moisture2Pct, "Pechay");
            }
            lcdPrint(1, r1);
          }
          break;
        case 2:
          lcdPrint(0, "ENVIRONMENT     ");
          {
            String r1 = "";
            if (!isnan(temperatureC)) { r1 += String((int)temperatureC); r1 += "C "; }
            else r1 += "?C ";
            r1 += "Hum:";
            if (!isnan(humidityPct)) { r1 += String((int)humidityPct); r1 += "%"; }
            else r1 += "?%";
            lcdPrint(1, r1);
          }
          break;
        case 3:
          lcdPrint(0, "WELL LEVEL      ");
          {
            const int pct = (int)clampFloat((wellWaterMl / WELL_CAPACITY_ML) * 100.0f, 0.0f, 100.0f);
            String r1 = String((int)wellWaterMl);
            r1 += "mL ";
            r1 += String(pct);
            r1 += "%";
            lcdPrint(1, r1);
          }
          break;
      }
      break;

    // ----------------------------------------------------------
    case SCREEN_SYSTEM_STATUS:
      switch (systemStatusIdx) {
        case 0:
          {
            String r0 = "Pump:";
            r0 += pumpOutputOn ? "ON  " : "OFF ";
            r0 += (currentMode == MODE_AUTO) ? "AUTO" : "MAN ";
            String r1 = "Crop:";
            r1 += activeCrop;
            lcdPrint(0, r0);
            lcdPrint(1, r1);
          }
          break;
        case 1:
          {
            String r0 = "Servo:";
            r0 += activeCrop;
            String r1 = "LED:";
            r1 += (currentMode == MODE_MANUAL) ? "ON " : "OFF";
            r1 += " BACK=menu";
            lcdPrint(0, r0);
            lcdPrint(1, r1);
          }
          break;
      }
      break;

    // ----------------------------------------------------------
    case SCREEN_MANUAL_SELECT_PLOT:
      lcdPrint(0, "Select Plot:    ");
      if (manualPlotIdx == 0) {
        lcdPrint(1, ">Tomato  Pechay ");
      } else {
        lcdPrint(1, " Tomato >Pechay ");
      }
      break;

    // ----------------------------------------------------------
    case SCREEN_MANUAL_CONFIRM_WET:
      {
        const String stageLabel = (manualTargetCrop == "Pechay") ? plantStagePechay : plantStageTomato;
        const unsigned long previewMs = (manualTargetCrop == "Pechay") ? manualWaterTimeoutMsPechay : manualWaterTimeoutMsTomato;
        const unsigned long previewSecs = previewMs / 1000;

        String r0 = manualTargetCrop;
        r0 += ":";
        r0 += (stageLabel.length() > 0) ? stageLabel : "--";

        String r1 = (manualConfirmIdx == 0) ? ">YES " : " YES ";
        r1 += String(previewSecs);
        r1 += "s ";
        r1 += (manualConfirmIdx == 1) ? ">NO" : " NO";

        lcdPrint(0, r0);
        lcdPrint(1, r1);
      }
      break;

    // ----------------------------------------------------------
    case SCREEN_MANUAL_WATERING:
      {
        String r0 = "Watering ";
        r0 += manualTargetCrop;
        lcdPrint(0, r0);
        unsigned long elapsed   = millis() - manualWaterStartMs;
        unsigned long remaining = (manualWaterTimeoutMs > elapsed)
                ? (manualWaterTimeoutMs - elapsed) / 1000
                                  : 0;
        String r1 = "Stop:BACK ";
        r1 += String(remaining);
        r1 += "s left";
        lcdPrint(1, r1);
      }
      break;

    // ----------------------------------------------------------
    case SCREEN_MANUAL_DONE:
      {
        String r0 = "Done! ";
        r0 += manualTargetCrop;
        lcdPrint(0, r0);
        lcdPrint(1, "BACK=menu       ");
      }
      break;
  }
}

// ============================================================
// Manual watering logic
// ============================================================

void startManualWatering(const String& crop) {
  manualTargetCrop     = crop;
  activeCrop           = crop;
  manualWateringActive = true;
  manualWaterStartMs   = millis();
  manualWaterTimeoutMs = (crop == "Pechay") ? manualWaterTimeoutMsPechay : manualWaterTimeoutMsTomato;
  if (manualWaterTimeoutMs == 0) {
    manualWaterTimeoutMs = (crop == "Pechay") ? MANUAL_WATER_TIMEOUT_PECHAY_MS : MANUAL_WATER_TIMEOUT_TOMATO_MS;
  }
  updateServoForCrop(activeCrop);
  delay(500); // Let servo settle before pump
  setPumpOutput(true);
  currentScreen  = SCREEN_MANUAL_WATERING;
  lcdNeedsRedraw = true;
}

void stopManualWatering() {
  setPumpOutput(false);
  manualWateringActive = false;
  currentScreen  = SCREEN_MANUAL_DONE;
  lcdNeedsRedraw = true;
}

void tickManualWatering() {
  if (!manualWateringActive) return;

  // Safety timeout
  if (millis() - manualWaterStartMs >= manualWaterTimeoutMs) {
    stopManualWatering();
    return;
  }

  // Refresh remaining time on LCD every second
  lcdNeedsRedraw = true;
}

void applySystemMode(SystemMode nextMode, const char* source) {
  if (nextMode == currentMode) return;

  if (nextMode == MODE_MANUAL) {
    currentMode = MODE_MANUAL;
    setLed(true);
    currentScreen = SCREEN_HOME;
    homeScreenIdx = 0;
    Serial.print("[mode] switched to MANUAL");
  } else {
    currentMode = MODE_AUTO;
    setLed(false);
    if (manualWateringActive) stopManualWatering();
    setPumpOutput(false);
    desiredPumpOn = false;
    currentScreen = SCREEN_HOME;
    homeScreenIdx = 0;
    Serial.print("[mode] switched to AUTO");
  }

  if (source != nullptr && source[0] != '\0') {
    Serial.print(" via ");
    Serial.print(source);
  }
  Serial.println();
  lcdNeedsRedraw = true;
}

// ============================================================
// Button handlers
// ============================================================

// MODE button is a momentary push button.
// Each press toggles between AUTO and MANUAL mode.
void handleModeButton() {
  unsigned long now = millis();
  if (now - lastModePressMs < DEBOUNCE_MS) return;
  lastModePressMs = now;

  applySystemMode(currentMode == MODE_AUTO ? MODE_MANUAL : MODE_AUTO, "button");
}

void handleNextButton() {
  unsigned long now = millis();
  if (now - lastNextPressMs < DEBOUNCE_MS) return;
  lastNextPressMs = now;

  switch (currentScreen) {
    case SCREEN_HOME:
      homeScreenIdx = (homeScreenIdx + 1) % 4;
      if (homeScreenIdx == 0) {
        currentScreen = SCREEN_MENU;
        menuIdx = 0;
      }
      break;

    case SCREEN_MENU:
      menuIdx = (menuIdx + 1) % 3;
      break;

    case SCREEN_VIEW_SENSORS:
      viewSensorIdx = (viewSensorIdx + 1) % 4;
      break;

    case SCREEN_SYSTEM_STATUS:
      systemStatusIdx = (systemStatusIdx + 1) % 2;
      break;

    case SCREEN_MANUAL_SELECT_PLOT:
      manualPlotIdx = (manualPlotIdx + 1) % 2;
      break;

    case SCREEN_MANUAL_CONFIRM_WET:
      manualConfirmIdx = (manualConfirmIdx + 1) % 2;
      break;

    default:
      break;
  }

  lcdNeedsRedraw = true;
}

void handleSelectButton() {
  unsigned long now = millis();
  if (now - lastSelectPressMs < DEBOUNCE_MS) return;
  lastSelectPressMs = now;

  switch (currentScreen) {

    case SCREEN_HOME:
      currentScreen = SCREEN_MENU;
      menuIdx       = 0;
      break;

    case SCREEN_MENU:
      switch (menuIdx) {
        case 0: // View Sensors
          currentScreen = SCREEN_VIEW_SENSORS;
          viewSensorIdx = 0;
          break;
        case 1: // Manual Water
          if (currentMode == MODE_MANUAL) {
            currentScreen = SCREEN_MANUAL_SELECT_PLOT;
            manualPlotIdx = 0;
          } else {
            lcdPrint(0, "Switch to MANUAL");
            lcdPrint(1, "mode first!     ");
            delay(2000);
            lcdNeedsRedraw = true;
            return;
          }
          break;
        case 2: // System Status
          currentScreen   = SCREEN_SYSTEM_STATUS;
          systemStatusIdx = 0;
          break;
      }
      break;

    case SCREEN_MANUAL_SELECT_PLOT:
      {
        manualTargetCrop = (manualPlotIdx == 0) ? "Tomato" : "Pechay";
        manualConfirmIdx = 1; // Default to NO for safety
        currentScreen    = SCREEN_MANUAL_CONFIRM_WET;
      }
      break;

    case SCREEN_MANUAL_CONFIRM_WET:
      if (manualConfirmIdx == 0) {
        startManualWatering(manualTargetCrop);
      } else {
        currentScreen = SCREEN_MANUAL_SELECT_PLOT;
      }
      break;

    case SCREEN_MANUAL_DONE:
      currentScreen = SCREEN_MENU;
      menuIdx       = 1;
      break;

    default:
      break;
  }

  lcdNeedsRedraw = true;
}

void handleBackButton() {
  unsigned long now = millis();
  if (now - lastBackPressMs < DEBOUNCE_MS) return;
  lastBackPressMs = now;

  switch (currentScreen) {

    case SCREEN_MANUAL_WATERING:
      stopManualWatering();
      break;

    case SCREEN_MANUAL_DONE:
    case SCREEN_MANUAL_SELECT_PLOT:
    case SCREEN_MANUAL_CONFIRM_WET:
    case SCREEN_VIEW_SENSORS:
    case SCREEN_SYSTEM_STATUS:
      currentScreen = SCREEN_MENU;
      break;

    case SCREEN_MENU:
      currentScreen = SCREEN_HOME;
      homeScreenIdx = 0;
      break;

    case SCREEN_HOME:
    default:
      break;
  }

  lcdNeedsRedraw = true;
}

// ============================================================
// Setup
// ============================================================

void setup() {
  Serial.begin(115200);

  // Pump — ensure OFF at boot (respects PUMP_ACTIVE_HIGH polarity)
  pinMode(PUMP_PIN, OUTPUT);
  setPumpOutput(false);

  // LED
  pinMode(LED_PIN, OUTPUT);
  setLed(false);

  // Buttons
  pinMode(BTN_MODE,   INPUT_PULLUP);
  pinMode(BTN_NEXT,   INPUT_PULLUP);
  pinMode(BTN_SELECT, INPUT_PULLUP);
  pinMode(BTN_BACK,   INPUT_PULLUP);

  // ADC
  analogReadResolution(12);
  analogSetPinAttenuation(SOIL1_PIN, ADC_11db);
  analogSetPinAttenuation(SOIL2_PIN, ADC_11db);
  delay(100); // Guard for GPIO36 startup glitch

  // DHT11
  dht.begin();

  // Well level (HC-SR04)
  pinMode(WATER_TRIG_PIN, OUTPUT);
  pinMode(WATER_ECHO_PIN, INPUT);
  digitalWrite(WATER_TRIG_PIN, LOW);

  // Servo
  valveServo.attach(SERVO_PIN, 500, 2400);
  updateServoForCrop(activeCrop);

  // LCD
  Wire.begin(LCD_SDA, LCD_SCL);
  lcd.init();
  lcd.backlight();
  lcdPrint(0, "AgroSense v1.0  ");
  lcdPrint(1, "Initializing... ");
  delay(1500);

  bool resetWiFiRequested = false;
  if (digitalRead(BTN_BACK) == LOW) {
    delay(800); // Require a short hold to avoid accidental resets.
    resetWiFiRequested = (digitalRead(BTN_BACK) == LOW);
  }

  if (resetWiFiRequested) {
    WiFiManager wm;
    wm.resetSettings();
    WiFi.disconnect(true, true);
    Serial.println("[wifi] saved credentials cleared (BACK held on boot)");
    lcdPrint(0, "WiFi creds reset ");
    lcdPrint(1, "Setup mode next ");
    delay(1500);
  }

  // WiFi
  lcdPrint(0, "Connecting WiFi ");
  lcdPrint(1, "Please wait...  ");
  connectWiFi();

  if (WiFi.status() == WL_CONNECTED) {
    lcdPrint(0, "WiFi Connected! ");
    lcdPrint(1, WiFi.localIP().toString());
  } else {
    lcdPrint(0, "WiFi Failed     ");
    lcdPrint(1, "Running offline ");
  }
  delay(2000);

  pwmPhaseStartMs = millis();
  lcdNeedsRedraw  = true;
}

// ============================================================
// Loop
// ============================================================

void loop() {
  const unsigned long now = millis();

  // Read sensors every 1s
  if (now - lastSensorReadMs >= SENSOR_INTERVAL_MS) {
    lastSensorReadMs = now;
    readSensors();
    if (currentMode == MODE_AUTO) evaluateRuleBasedPump();
  }

  // Auto pump tick
  if (currentMode == MODE_AUTO) slowPwmTick();

  // Manual watering watchdog
  if (currentMode == MODE_MANUAL && manualWateringActive) tickManualWatering();

  // Server sync every 5s
  if (now - lastSyncMs >= SYNC_INTERVAL_MS) {
    lastSyncMs = now;
    syncWithServer();
  }

  // Home screen auto-rotate every 3s when on home
  if (currentScreen == SCREEN_HOME && now - lastLcdUpdateMs >= LCD_UPDATE_MS) {
    lastLcdUpdateMs = now;
    homeScreenIdx   = (homeScreenIdx + 1) % 4;
    lcdNeedsRedraw  = true;
  }

  // Edge-detected button reads — fire handler only on falling edge (HIGH → LOW)
  bool currBtnMode   = digitalRead(BTN_MODE);
  bool currBtnNext   = digitalRead(BTN_NEXT);
  bool currBtnSelect = digitalRead(BTN_SELECT);
  bool currBtnBack   = digitalRead(BTN_BACK);

  if (currBtnMode   == LOW && prevBtnMode   == HIGH) handleModeButton();
  if (currBtnNext   == LOW && prevBtnNext   == HIGH) handleNextButton();
  if (currBtnSelect == LOW && prevBtnSelect == HIGH) handleSelectButton();
  if (currBtnBack   == LOW && prevBtnBack   == HIGH) handleBackButton();

  prevBtnMode   = currBtnMode;
  prevBtnNext   = currBtnNext;
  prevBtnSelect = currBtnSelect;
  prevBtnBack   = currBtnBack;

  // Render LCD only when needed
  renderLcd();
}