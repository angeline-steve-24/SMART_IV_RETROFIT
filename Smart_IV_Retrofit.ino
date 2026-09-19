#include <WiFi.h>
#include <WebServer.h>
#include <LiquidCrystal_I2C.h>
#include <HX711.h>
#include <Preferences.h>

// -------------------- PINS --------------------
#define HX_DOUT 23
#define HX_SCK  19
#define BUZZER 4

// -------------------- SYSTEM --------------------
const float FULL_VOLUME_ML = 500.0f;
const unsigned long SAMPLE_INTERVAL_MS = 2000;
const unsigned long NO_FLOW_TIMEOUT_MS = 180000; // 3 minutes
const float MIN_FLOW_CHANGE_ML = 0.20f;

// Local Wi-Fi access point
const char* AP_SSID = "IV_MONITOR";
const char* AP_PASSWORD = "12345678";

// -------------------- OBJECTS --------------------
HX711 scale;
LiquidCrystal_I2C lcd(0x27, 16, 2);
WebServer server(80);
Preferences prefs;

// -------------------- CALIBRATION --------------------
// Calibrate using the actual empty IV bag and the actual full 500 mL bag.
// Values are stored in ESP32 non-volatile memory after calibration.
long emptyRaw = 0;
long fullRaw = 0;
bool calibrated = false;

// -------------------- MEASUREMENTS --------------------
float volumeML = 0.0f;
float levelPercent = 0.0f;
float flowRateMLMin = 0.0f;
float remainingTimeMin = 0.0f;

float previousVolumeML = -1.0f;
unsigned long previousSampleMs = 0;
unsigned long lastFlowChangeMs = 0;
unsigned long lastSampleMs = 0;

bool bagPresent = false;
bool possibleBlockage = false;

// Flow-rate smoothing
float flowFiltered = 0.0f;
const float FLOW_ALPHA = 0.25f;

// -------------------- BUZZER --------------------
unsigned long buzzerTimer = 0;
bool buzzerState = false;

// -------------------- HELPERS --------------------
long readStableRaw(uint8_t samples = 20) {
  long sum = 0;
  uint8_t count = 0;

  for (uint8_t i = 0; i < samples; i++) {
    if (scale.is_ready()) {
      sum += scale.read();
      count++;
    }
    delay(10);
  }

  return (count > 0) ? sum / count : 0;
}

void showLCD(const String& line1, const String& line2) {
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print(line1.substring(0, 16));
  lcd.setCursor(0, 1);
  lcd.print(line2.substring(0, 16));
}

// -------------------- CALIBRATION STORAGE --------------------
void saveCalibration() {
  prefs.begin("iv-cal", false);
  prefs.putLong("emptyRaw", emptyRaw);
  prefs.putLong("fullRaw", fullRaw);
  prefs.putBool("valid", true);
  prefs.end();
}

bool loadCalibration() {
  prefs.begin("iv-cal", true);

  bool valid = prefs.getBool("valid", false);
  emptyRaw = prefs.getLong("emptyRaw", 0);
  fullRaw = prefs.getLong("fullRaw", 0);

  prefs.end();

  if (!valid) return false;

  long range = fullRaw - emptyRaw;
  return labs(range) >= 1000;
}

void clearCalibration() {
  prefs.begin("iv-cal", false);
  prefs.clear();
  prefs.end();

  calibrated = false;
}

// -------------------- REAL CALIBRATION --------------------
void printCalibrationInstructions() {
  Serial.println();
  Serial.println("========== CALIBRATION ==========");
  Serial.println("Use the ACTUAL IV BAG for calibration.");
  Serial.println();
  Serial.println("STEP 1:");
  Serial.println("Hang the EMPTY IV bag on the hook.");
  Serial.println("Type E in Serial Monitor and press Enter.");
  Serial.println();
  Serial.println("STEP 2:");
  Serial.println("Fill/use the same bag at 500 mL.");
  Serial.println("Hang the FULL 500 mL bag on the hook.");
  Serial.println("Type F in Serial Monitor and press Enter.");
  Serial.println();
  Serial.println("Calibration values will be saved in ESP32 memory.");
  Serial.println("=================================");
}

void performCalibration() {
  showLCD("CALIBRATION", "Empty bag -> E");

  Serial.println();
  Serial.println("Hang EMPTY IV bag on the hook.");
  Serial.println("Type E and press Enter.");

  while (true) {
    if (Serial.available()) {
      char c = Serial.read();
      if (c == 'E' || c == 'e') break;
    }
    delay(20);
  }

  delay(1000);
  emptyRaw = readStableRaw(40);

  Serial.print("EMPTY RAW = ");
  Serial.println(emptyRaw);

  showLCD("EMPTY SAVED", "Full 500mL -> F");

  Serial.println();
  Serial.println("Now hang the FULL 500 mL IV bag.");
  Serial.println("Type F and press Enter.");

  while (true) {
    if (Serial.available()) {
      char c = Serial.read();
      if (c == 'F' || c == 'f') break;
    }
    delay(20);
  }

  delay(1000);
  fullRaw = readStableRaw(40);

  Serial.print("FULL RAW = ");
  Serial.println(fullRaw);

  long range = fullRaw - emptyRaw;

  if (labs(range) < 1000) {
    showLCD("CALIB FAILED", "Check HX711");

    Serial.println();
    Serial.println("ERROR: Calibration range is too small.");
    Serial.println("Check the load-cell wiring and mounting.");

    calibrated = false;
    delay(3000);
    printCalibrationInstructions();
    return;
  }

  saveCalibration();
  calibrated = true;

  Serial.println();
  Serial.println("CALIBRATION COMPLETE");
  Serial.print("EMPTY RAW: ");
  Serial.println(emptyRaw);
  Serial.print("FULL RAW : ");
  Serial.println(fullRaw);
  Serial.print("RAW RANGE: ");
  Serial.println(range);

  showLCD("CALIBRATION OK", "Monitoring...");
  delay(2000);
}

// -------------------- WEIGHT TO VOLUME --------------------
float rawToVolume(long raw) {
  if (!calibrated) return 0.0f;

  float fraction =
      (float)(raw - emptyRaw) /
      (float)(fullRaw - emptyRaw);

  fraction = constrain(fraction, 0.0f, 1.0f);

  return fraction * FULL_VOLUME_ML;
}

// -------------------- FLOW RATE --------------------
void updateFlow(float newVolume, unsigned long now) {

  if (previousVolumeML < 0.0f) {
    previousVolumeML = newVolume;
    previousSampleMs = now;
    lastFlowChangeMs = now;
    flowFiltered = 0.0f;
    flowRateMLMin = 0.0f;
    remainingTimeMin = 0.0f;
    possibleBlockage = false;
    return;
  }

  float dtMin = (now - previousSampleMs) / 60000.0f;

  if (dtMin <= 0.0f) return;

  // Positive value means IV fluid has been consumed.
  float consumed = previousVolumeML - newVolume;

  // Ignore very small changes caused by sensor noise.
  if (consumed >= MIN_FLOW_CHANGE_ML) {

    float instantFlow = consumed / dtMin;

    // Reject an unrealistic measurement spike.
    if (instantFlow >= 0.0f && instantFlow <= 1000.0f) {

      if (flowFiltered <= 0.0f) {
        flowFiltered = instantFlow;
      } else {
        flowFiltered =
            FLOW_ALPHA * instantFlow +
            (1.0f - FLOW_ALPHA) * flowFiltered;
      }

      flowRateMLMin = flowFiltered;

      lastFlowChangeMs = now;
      possibleBlockage = false;
    }
  }

  // Remaining time = remaining volume / flow rate.
  if (flowRateMLMin > 0.1f) {
    remainingTimeMin = newVolume / flowRateMLMin;
  } else {
    remainingTimeMin = 0.0f;
  }

  // Possible interruption if the volume does not decrease
  // for the predefined period while fluid remains.
  if (newVolume > 5.0f &&
      (now - lastFlowChangeMs >= NO_FLOW_TIMEOUT_MS)) {
    possibleBlockage = true;
  }

  previousVolumeML = newVolume;
  previousSampleMs = now;
}

// -------------------- TIME FORMAT --------------------
String formatTime(float minutes) {

  if (minutes <= 0.0f || !isfinite(minutes))
    return "--";

  int totalMinutes = (int)round(minutes);
  int hours = totalMinutes / 60;
  int mins = totalMinutes % 60;

  if (hours > 0)
    return String(hours) + "h " + String(mins) + "m";

  return String(mins) + " min";
}

// -------------------- STATUS --------------------
String statusText() {

  if (!bagPresent)
    return "NO BAG";

  if (levelPercent <= 10)
    return "CRITICAL";

  if (levelPercent <= 30)
    return "VERY LOW";

  if (levelPercent <= 50)
    return "LOW";

  if (possibleBlockage)
    return "POSSIBLE BLOCK";

  if (flowRateMLMin > 0.1f)
    return "FLOWING";

  return "FLOW NOT DETECTED";
}

// -------------------- BUZZER --------------------
void updateBuzzer() {

  unsigned long now = millis();

  if (!bagPresent) {
    noTone(BUZZER);
    buzzerState = false;
    buzzerTimer = now;
    return;
  }

  unsigned long onTime = 0;
  unsigned long offTime = 0;
  int frequency = 0;

  if (levelPercent > 50.0f) {

    noTone(BUZZER);
    buzzerState = false;
    buzzerTimer = now;
    return;

  } else if (levelPercent > 30.0f) {

    frequency = 1500;
    onTime = 1500;
    offTime = 1200;

  } else if (levelPercent > 10.0f) {

    frequency = 600;
    onTime = 600;
    offTime = 300;

  } else {

    frequency = 400;
    onTime = 400;
    offTime = 150;
  }

  if (!buzzerState) {

    if (now - buzzerTimer >= offTime) {
      tone(BUZZER, frequency);
      buzzerState = true;
      buzzerTimer = now;
    }

  } else {

    if (now - buzzerTimer >= onTime) {
      noTone(BUZZER);
      buzzerState = false;
      buzzerTimer = now;
    }
  }
}

// -------------------- LCD --------------------
void updateLCD() {

  static unsigned long lastLCD = 0;
  static uint8_t page = 0;

  if (millis() - lastLCD < 2500)
    return;

  lastLCD = millis();

  if (!bagPresent) {
    showLCD("IV: EMPTY", "PLACE BAG");
    return;
  }

  if (page == 0) {

    String line1 =
        "IV:" +
        String((int)round(volumeML)) +
        "mL " +
        String((int)round(levelPercent)) +
        "%";

    showLCD(line1, statusText());

  } else {

    String line1 =
        "Flow:" +
        String(flowRateMLMin, 1) +
        "mL/m";

    String line2 =
        "Time:" +
        formatTime(remainingTimeMin);

    showLCD(line1, line2);
  }

  page = (page + 1) % 2;
}

// -------------------- REAL WEIGHT MEASUREMENT --------------------
void measureWeight() {

  if (!scale.is_ready()) {

    Serial.println("HX711 NOT READY");

    showLCD("HX711 ERROR", "CHECK WIRING");

    noTone(BUZZER);
    return;
  }

  long raw = readStableRaw(10);

  float newVolume = rawToVolume(raw);

  volumeML = newVolume;

  levelPercent =
      (volumeML / FULL_VOLUME_ML) * 100.0f;

  levelPercent =
      constrain(levelPercent, 0.0f, 100.0f);

  // No extra sensor is used for bottle presence.
  // The empty calibration point represents an empty bag.
  bagPresent = (volumeML > 5.0f);

  unsigned long now = millis();

  updateFlow(volumeML, now);

  Serial.print("RAW: ");
  Serial.print(raw);

  Serial.print(" | Volume: ");
  Serial.print(volumeML, 1);
  Serial.print(" mL");

  Serial.print(" | Level: ");
  Serial.print(levelPercent, 1);
  Serial.print("%");

  Serial.print(" | Flow: ");
  Serial.print(flowRateMLMin, 1);
  Serial.print(" mL/min");

  Serial.print(" | Remaining: ");
  Serial.print(formatTime(remainingTimeMin));

  Serial.print(" | Status: ");
  Serial.println(statusText());

  updateLCD();
  updateBuzzer();
}

// -------------------- WEB DASHBOARD --------------------
const char MAIN_PAGE[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Smart IV Retrofit</title>

<style>
body{
  font-family:Arial;
  text-align:center;
  background:#f4f6f8;
  margin:0;
  padding:20px;
}

.card{
  max-width:500px;
  margin:auto;
  background:white;
  padding:20px;
  border-radius:16px;
}

.value{
  font-size:34px;
  font-weight:bold;
  margin:10px;
}

.label{
  color:#555;
}

.bar{
  height:25px;
  background:#ddd;
  border-radius:15px;
  overflow:hidden;
}

.fill{
  height:100%;
  width:0%;
  background:#2e8b57;
}

.status{
  font-size:22px;
  font-weight:bold;
  margin:15px;
}

.grid{
  display:grid;
  grid-template-columns:1fr 1fr;
  gap:12px;
}

.box{
  padding:15px;
  background:#eef1f4;
  border-radius:10px;
}
</style>
</head>

<body>

<div class="card">

<h2>Smart IV Retrofit</h2>

<div class="label">Remaining Fluid</div>

<div class="value">
<span id="vol">--</span> mL
</div>

<div class="bar">
<div class="fill" id="bar"></div>
</div>

<div class="value">
<span id="pct">--</span>%
</div>

<div class="grid">

<div class="box">
<b>Flow Rate</b><br>
<span id="flow">--</span> mL/min
</div>

<div class="box">
<b>Remaining Time</b><br>
<span id="time">--</span>
</div>

</div>

<div class="status" id="status">
Loading...
</div>

<div id="block"></div>

<p>ESP32 Local Monitoring</p>

</div>

<script>

async function update(){

  try{

    const response =
      await fetch('/data');

    const data =
      await response.json();

    document.getElementById('vol')
      .textContent =
      data.volume.toFixed(1);

    document.getElementById('pct')
      .textContent =
      data.level.toFixed(1);

    document.getElementById('bar')
      .style.width =
      data.level + '%';

    document.getElementById('flow')
      .textContent =
      data.flow.toFixed(1);

    document.getElementById('time')
      .textContent =
      data.remaining;

    document.getElementById('status')
      .textContent =
      data.status;

    document.getElementById('block')
      .textContent =
      data.blockage
      ? 'Possible flow interruption detected'
      : '';

  }catch(error){}
}

setInterval(update,2000);

update();

</script>

</body>
</html>
)rawliteral";

void handleRoot() {
  server.send_P(200, "text/html", MAIN_PAGE);
}

void handleData() {

  String json = "{";

  json += "\"volume\":";
  json += String(volumeML, 1);
  json += ",";

  json += "\"level\":";
  json += String(levelPercent, 1);
  json += ",";

  json += "\"flow\":";
  json += String(flowRateMLMin, 1);
  json += ",";

  json += "\"remaining\":\"";
  json += formatTime(remainingTimeMin);
  json += "\",";

  json += "\"status\":\"";
  json += statusText();
  json += "\",";

  json += "\"blockage\":";
  json += possibleBlockage ? "true" : "false";

  json += "}";

  server.send(200, "application/json", json);
}

void startWebServer() {

  server.on("/", handleRoot);
  server.on("/data", handleData);

  server.begin();

  Serial.println("Web server started.");

  Serial.print("Open in browser: http://");
  Serial.println(WiFi.softAPIP());
}

// -------------------- SETUP --------------------
void setup() {

  Serial.begin(115200);
  delay(500);

  lcd.init();
  lcd.backlight();

  pinMode(BUZZER, OUTPUT);
  noTone(BUZZER);

  showLCD("SMART IV", "INITIALIZING");

  // HX711
  scale.begin(HX_DOUT, HX_SCK);

  if (!scale.is_ready()) {

    showLCD("HX711 ERROR", "CHECK WIRING");

    Serial.println("HX711 not detected.");

    while (!scale.is_ready()) {
      delay(1000);
    }
  }

  Serial.println("HX711 connected.");

  // Load saved real calibration.
  calibrated = loadCalibration();

  if (!calibrated) {

    printCalibrationInstructions();
    performCalibration();

  } else {

    Serial.println("Stored calibration loaded.");

    Serial.print("Empty RAW: ");
    Serial.println(emptyRaw);

    Serial.print("Full RAW: ");
    Serial.println(fullRaw);
  }

  // ESP32 creates its own local Wi-Fi network.
  WiFi.mode(WIFI_AP);
  WiFi.softAP(AP_SSID, AP_PASSWORD);

  Serial.print("AP IP: ");
  Serial.println(WiFi.softAPIP());

  showLCD("WiFi: IV_MONITOR", "192.168.4.1");
  delay(2000);

  startWebServer();

  previousVolumeML = -1.0f;
  previousSampleMs = millis();
  lastFlowChangeMs = millis();
}

// -------------------- LOOP --------------------
void loop() {

  server.handleClient();

  // Press R in Serial Monitor to erase calibration
  // and perform calibration again.
  if (Serial.available()) {

    char c = Serial.read();

    if (c == 'R' || c == 'r') {

      clearCalibration();

      Serial.println(
          "Calibration cleared. Restarting calibration..."
      );

      performCalibration();

      previousVolumeML = -1.0f;
      flowRateMLMin = 0.0f;
      flowFiltered = 0.0f;
      remainingTimeMin = 0.0f;
      possibleBlockage = false;
      lastFlowChangeMs = millis();
    }
  }

  if (millis() - lastSampleMs >= SAMPLE_INTERVAL_MS) {

    lastSampleMs = millis();

    // REAL sensor measurement.
    measureWeight();
  }
}
