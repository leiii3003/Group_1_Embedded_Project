// ============================================================
// AgroSense - Urban Smart Irrigation System
// ESP32 WROOM-32 (38-pin)
// ============================================================
// Pin Summary:
//   GPIO32 - Pump ON/OFF (F5305S Signal+)
//   GPIO33 - DHT11 DATA
//   GPIO34 - Soil Sensor 1 AOUT (Tomato)
//   GPIO36 - Soil Sensor 2 AOUT (Pechay)
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
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <DHT.h>
#include <ESP32Servo.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <math.h>

// ============================================================
// Network and backend config
// ============================================================
const char* WIFI_SSID       = "iyang";
const char* WIFI_PASS       = "alms9121";
const char* SERVER_BASE_URL = "https://agrosense-backend-k5zp.onrender.com";
const char* API_KEY         = "f3125eac-b6c5-403c-b6de-5879a5bd1a08";
const char* DEVICE_ID       = "esp32-agrosense-01";

// ============================================================
// Pin assignments
// ============================================================
const int PUMP_PIN   = 32;
const int DHT_PIN    = 33;
const int SOIL1_PIN  = 34;  // Tomato
const int SOIL2_PIN  = 36;  // Pechay
const int SERVO_PIN  = 25;

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

// ============================================================
// Irrigation config (can be overridden by server)
// ============================================================
float         moistureOnThreshold  = 30.0f;
float         moistureOffThreshold = 65.0f;
unsigned long slowPwmOnMs          = 30000;
unsigned long slowPwmOffMs         = 30000;

// ============================================================
// Pump and crop state
// ============================================================
bool          desiredPumpOn     = false;
bool          prevDesiredPumpOn = false;  // Used to detect rising edge in slowPwmTick
bool          pumpOutputOn      = false;
unsigned long pwmPhaseStartMs   = 0;
String        activeCrop        = "Tomato";
String        previousCrop      = "Tomato"; // Saved before manual watering to restore after
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
int  homeScreenIdx         = 0; // 0=moisture, 1=env, 2=status
int  menuIdx               = 0; // 0=View Sensors, 1=Manual Water, 2=System Status
int  viewSensorIdx         = 0; // 0=Tomato, 1=Pechay, 2=Environment
int  systemStatusIdx       = 0; // 0=pump, 1=servo
int  manualPlotIdx         = 0; // 0=Tomato, 1=Pechay
int  manualConfirmIdx      = 0; // 0=YES, 1=NO

// Manual watering state
String        manualTargetCrop     = "Tomato";
bool          manualWateringActive = false;
unsigned long manualWaterStartMs   = 0;
const unsigned long MANUAL_WATER_TIMEOUT_MS = 30000; // 30s safety cutoff

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

// Calibration model: Vs = -0.0133*theta^2 - 1.986*theta + 75.441
float voltageToVwcPct(float vs) {
  const float a            = -0.0133f;
  const float b            = -1.986f;
  const float c            = 75.441f - vs;
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

String moistureLabel(float pct) {
  if (isnan(pct))                          return "N/A  ";
  if (pct < moistureOnThreshold)           return "DRY  ";
  if (pct > moistureOffThreshold)          return "WET  ";
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
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  Serial.print("[wifi] connecting");

  int retries = 0;
  while (WiFi.status() != WL_CONNECTED && retries < 30) {
    delay(500);
    Serial.print(".");
    retries++;
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println();
    Serial.print("[wifi] connected, IP: ");
    Serial.println(WiFi.localIP());
  } else {
    Serial.println();
    Serial.println("[wifi] connection timeout");
  }
}

// ============================================================
// Sensor reading
// ============================================================

void readSensors() {
  const int   raw1 = analogRead(SOIL1_PIN);
  const float v1   = adcToVoltage(raw1);
  moisture1Pct     = voltageToVwcPct(v1);

  if (HAS_SENSOR2) {
    const int   raw2 = analogRead(SOIL2_PIN);
    const float v2   = adcToVoltage(raw2);
    moisture2Pct     = voltageToVwcPct(v2);
    moistureAvgPct   = (moisture1Pct + moisture2Pct) * 0.5f;
  } else {
    moisture2Pct   = NAN;
    moistureAvgPct = moisture1Pct;
  }

  const float dhtTemp = dht.readTemperature();
  const float dhtHum  = dht.readHumidity();
  if (!isnan(dhtTemp)) temperatureC = dhtTemp;
  if (!isnan(dhtHum))  humidityPct  = dhtHum;
}

// ============================================================
// Irrigation logic (AUTO mode only)
// ============================================================

void evaluateRuleBasedPump() {
  if (currentMode == MODE_MANUAL) return; // Manual mode bypasses this

  float moistureForCrop = moisture1Pct;

  if (activeCrop == "Pechay") {
    if (!HAS_SENSOR2 || isnan(moisture2Pct)) {
      desiredPumpOn = false;
      return;
    }
    moistureForCrop = moisture2Pct;
  }

  if (moistureForCrop < moistureOnThreshold) {
    desiredPumpOn = true;
    return;
  }
  if (moistureForCrop > moistureOffThreshold) {
    desiredPumpOn = false;
  }
}

void slowPwmTick() {
  if (currentMode == MODE_MANUAL) return; // Manual mode controls pump directly

  const unsigned long now = millis();

  if (!desiredPumpOn) {
    if (pumpOutputOn) setPumpOutput(false);
    pwmPhaseStartMs  = now;
    prevDesiredPumpOn = false;
    return;
  }

  // Rising edge: desiredPumpOn just became true → fire pump immediately,
  // skipping the off-phase wait that would otherwise stall activation.
  if (!prevDesiredPumpOn) {
    setPumpOutput(true);
    pwmPhaseStartMs   = now;
    prevDesiredPumpOn = true;
    return;
  }

  if (pumpOutputOn) {
    if (now - pwmPhaseStartMs >= slowPwmOnMs) {
      setPumpOutput(false);
      pwmPhaseStartMs = now;
    }
    return;
  }

  if (now - pwmPhaseStartMs >= slowPwmOffMs) {
    setPumpOutput(true);
    pwmPhaseStartMs = now;
  }
}

// ============================================================
// Server sync
// ============================================================

void syncWithServer() {
  if (WiFi.status() != WL_CONNECTED) {
    connectWiFi();
    return;
  }

  HTTPClient http;
  const String url = String(SERVER_BASE_URL) + "/api/ingest?key=" + API_KEY;
  http.begin(url);
  http.addHeader("Content-Type", "application/json");
  http.addHeader("X-API-Key", API_KEY);

  StaticJsonDocument<640> payload;
  payload["device_id"]         = DEVICE_ID;
  payload["moisture1"]         = moisture1Pct;
  if (HAS_SENSOR2) payload["moisture2"] = moisture2Pct;
  payload["moisture"]          = moistureAvgPct;
  payload["temperature"]       = temperatureC;
  payload["humidity"]          = humidityPct;
  payload["flow_ml"]           = flowMl;
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
      // ACK the servo command version the server sent
      if (response.containsKey("servo_command_version"))
        lastServoCommandVersion = response["servo_command_version"].as<int>();

      // ---- Pump / thresholds: AUTO mode only ----
      if (currentMode == MODE_AUTO) {
        if (response.containsKey("pump_command"))
          desiredPumpOn = response["pump_command"].as<bool>();

        if (response.containsKey("moisture_on_threshold"))
          moistureOnThreshold = response["moisture_on_threshold"].as<float>();

        if (response.containsKey("moisture_off_threshold"))
          moistureOffThreshold = response["moisture_off_threshold"].as<float>();

        if (response.containsKey("pwm_on_ms"))
          slowPwmOnMs = response["pwm_on_ms"].as<unsigned long>();

        if (response.containsKey("pwm_off_ms"))
          slowPwmOffMs = response["pwm_off_ms"].as<unsigned long>();

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
            r0 += moistureLabel(moisture1Pct);
            r0 += String((int)moisture1Pct);
            r0 += "%";
            String r1 = "Pec: ";
            r1 += moistureLabel(moisture2Pct);
            r1 += String((int)moisture2Pct);
            r1 += "%";
            lcdPrint(0, r0);
            lcdPrint(1, r1);
          }
          break;

        case 2:
          // Row 0: Pump and crop
          // Row 1: Press NEXT=menu
          {
            String r0 = "Pump:";
            r0 += pumpOutputOn ? "ON " : "OFF";
            r0 += " ";
            r0 += activeCrop;
            lcdPrint(0, r0);
            lcdPrint(1, "NEXT:menu SEL:--");
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
            r1 += moistureLabel(moisture1Pct);
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
              r1 += moistureLabel(moisture2Pct);
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
        float targetMoisture = (manualTargetCrop == "Tomato") ? moisture1Pct : moisture2Pct;
        bool  soilWet        = targetMoisture > moistureOffThreshold;
        bool  soilOk         = targetMoisture >= moistureOnThreshold && targetMoisture <= moistureOffThreshold;

        if (soilWet) {
          lcdPrint(0, "WARNING:Soil WET");
        } else if (soilOk) {
          lcdPrint(0, "Soil OK! Water? ");
        } else {
          lcdPrint(0, "Plant needs H2O ");
        }

        if (manualConfirmIdx == 0) {
          lcdPrint(1, ">YES        NO  ");
        } else {
          lcdPrint(1, " YES       >NO  ");
        }
      }
      break;

    // ----------------------------------------------------------
    case SCREEN_MANUAL_WATERING:
      {
        String r0 = "Watering ";
        r0 += manualTargetCrop;
        lcdPrint(0, r0);
        unsigned long elapsed   = millis() - manualWaterStartMs;
        unsigned long remaining = (MANUAL_WATER_TIMEOUT_MS > elapsed)
                                  ? (MANUAL_WATER_TIMEOUT_MS - elapsed) / 1000
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
  previousCrop         = activeCrop; // Save so servo can return after watering
  manualTargetCrop     = crop;
  manualWateringActive = true;
  manualWaterStartMs   = millis();
  updateServoForCrop(crop);
  delay(500); // Let servo settle before pump
  setPumpOutput(true);
  currentScreen  = SCREEN_MANUAL_WATERING;
  lcdNeedsRedraw = true;
}

void stopManualWatering() {
  setPumpOutput(false);
  // Restore servo to the crop that was active before manual watering started
  if (previousCrop != manualTargetCrop) {
    Serial.print("[servo] restore ");
    Serial.print(manualTargetCrop);
    Serial.print(" -> ");
    Serial.println(previousCrop);
    activeCrop = previousCrop;
    updateServoForCrop(activeCrop);
  }
  manualWateringActive = false;
  currentScreen  = SCREEN_MANUAL_DONE;
  lcdNeedsRedraw = true;
}

void tickManualWatering() {
  if (!manualWateringActive) return;

  // Safety timeout
  if (millis() - manualWaterStartMs >= MANUAL_WATER_TIMEOUT_MS) {
    stopManualWatering();
    return;
  }

  // Stop if soil saturated during manual watering
  float targetMoisture = (manualTargetCrop == "Tomato") ? moisture1Pct : moisture2Pct;
  if (!isnan(targetMoisture) && targetMoisture > moistureOffThreshold) {
    stopManualWatering();
    return;
  }

  // Refresh remaining time on LCD every second
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

  // Toggle mode on each press
  if (currentMode == MODE_AUTO) {
    currentMode = MODE_MANUAL;
    setLed(true);
    currentScreen = SCREEN_HOME;
    homeScreenIdx = 0;
    Serial.println("[mode] switched to MANUAL");
  } else {
    currentMode = MODE_AUTO;
    setLed(false);
    // Switching back to auto — stop any in-progress manual watering
    if (manualWateringActive) stopManualWatering();
    setPumpOutput(false);
    desiredPumpOn = false;
    currentScreen = SCREEN_HOME;
    homeScreenIdx = 0;
    Serial.println("[mode] switched to AUTO");
  }

  lcdNeedsRedraw = true;
}

void handleNextButton() {
  unsigned long now = millis();
  if (now - lastNextPressMs < DEBOUNCE_MS) return;
  lastNextPressMs = now;

  switch (currentScreen) {
    case SCREEN_HOME:
      homeScreenIdx = (homeScreenIdx + 1) % 3;
      if (homeScreenIdx == 0) {
        currentScreen = SCREEN_MENU;
        menuIdx = 0;
      }
      break;

    case SCREEN_MENU:
      menuIdx = (menuIdx + 1) % 3;
      break;

    case SCREEN_VIEW_SENSORS:
      viewSensorIdx = (viewSensorIdx + 1) % 3;
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
        float targetMoisture = (manualTargetCrop == "Tomato") ? moisture1Pct : moisture2Pct;

        if (isnan(targetMoisture) || targetMoisture < moistureOnThreshold) {
          // Soil is dry — start watering directly, no confirm needed
          startManualWatering(manualTargetCrop);
        } else {
          // Soil is OK or WET — ask for confirmation
          manualConfirmIdx = 1; // Default to NO for safety
          currentScreen    = SCREEN_MANUAL_CONFIRM_WET;
        }
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
    homeScreenIdx   = (homeScreenIdx + 1) % 3;
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
