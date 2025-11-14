#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <Preferences.h>

// ==== Pin configuration ====
#define OLED_SDA    21
#define OLED_SCL    22
#define ONE_WIRE_BUS 17   // DS18B20 on GPIO17
#define BTN_UP      33
#define BTN_DOWN    25
#define LED_RED     13
#define LED_GREEN   12
#define LED_BLUE    27
#define BTN_WAKE    4    // TREDJE KNAP: reset / wake / AP-setup (til GND, INPUT_PULLUP)

// ==== Constants ====
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 32
#define OLED_RESET   -1
#define TEMP_INTERVAL 1000
#define DEBOUNCE_MS   150

const unsigned long LONG_PRESS_MS     = 10000;        // 10 sek for AP-setup
const unsigned long AWAKE_DURATION_MS = 1UL * 60UL * 1000UL; // 1 minutter vågen-tid

// ==== Global objects ====
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);
OneWire oneWire(ONE_WIRE_BUS);
DallasTemperature sensors(&oneWire);
WebServer server(80);
Preferences prefs;

// ==== WiFi / modes ====
String wifiSsid;
String wifiPass;
bool apMode = false;

// ==== Termostat-variabler ====
double setpoint    = 21.0;
double hysteresis  = 0.5;
unsigned long lastTempRead = 0;
float currentTemp  = NAN;
bool upPressed     = false;
bool downPressed   = false;
unsigned long lastUpPress   = 0;
unsigned long lastDownPress = 0;

enum HeatState { COOLING, ONTARGET, HEATING };
HeatState state = ONTARGET;

// BTN_WAKE state
bool wakeBtnLast = false;
unsigned long wakePressStart = 0;

// Awake timer
unsigned long awakeStart = 0;

// ==== Temperatur-historik til web-graf ====
const int NUM_POINTS = 120;
float tempHistory[NUM_POINTS];
int tempIndex = 0;
bool bufferFilled = false;

// ==== Helper-funktioner ====
String formatTemp(float t) {
  if (isnan(t)) return "--.-";
  char buf[8];
  snprintf(buf, sizeof(buf), "%.1f", t);
  return String(buf);
}

HeatState decideState(float t, double sp, double hyst) {
  if (isnan(t)) return ONTARGET;
  double half = hyst / 2.0;
  if (t < sp - half) return HEATING;
  if (t > sp + half) return COOLING;
  return ONTARGET;
}

void setLEDs(HeatState s) {
  digitalWrite(LED_RED,   s == HEATING  ? HIGH : LOW);
  digitalWrite(LED_GREEN, s == ONTARGET ? HIGH : LOW);
  digitalWrite(LED_BLUE,  s == COOLING  ? HIGH : LOW);
}

void drawDisplay(float temp, double sp, HeatState s) {
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);

  display.setCursor(0, 0);
  display.print("T:"); display.print(formatTemp(temp)); display.print("C");

  display.setCursor(0, 16);
  display.print("SP:"); display.print(formatTemp(sp)); display.print("C ");
  if (s == HEATING) display.print("UP");
  else if (s == COOLING) display.print("DOWN");
  else display.print("OK");

  display.display();
}

// ==== Historik persistens ====
void saveHistoryToPrefs() {
  // Gem hele buffer + index + flag
  prefs.putBytes("temps", tempHistory, sizeof(tempHistory));
  prefs.putInt("tIndex", tempIndex);
  prefs.putBool("tFull", bufferFilled);
}

void loadHistoryFromPrefs() {
  size_t len = prefs.getBytesLength("temps");
  if (len == sizeof(tempHistory)) {
    prefs.getBytes("temps", tempHistory, sizeof(tempHistory));
    tempIndex    = prefs.getInt("tIndex", 0);
    bufferFilled = prefs.getBool("tFull", false);

    // sanity check
    if (tempIndex < 0 || tempIndex > NUM_POINTS) {
      tempIndex = 0;
      bufferFilled = false;
    }
  } else {
    // ingen gemt historik -> init til NAN
    for (int i = 0; i < NUM_POINTS; i++) {
      tempHistory[i] = NAN;
    }
    tempIndex = 0;
    bufferFilled = false;
  }
}

void addTemperature(float t) {
  tempHistory[tempIndex] = t;
  tempIndex++;
  if (tempIndex >= NUM_POINTS) {
    tempIndex = 0;
    bufferFilled = true;
  }
  saveHistoryToPrefs();  // gem efter hver måling (enkelt; evt. kan man lave hver N'te for mindre flash-slid)
}

// ==== HTML: normal webinterface (STA-mode) ====
const char main_html[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
  <meta charset="utf-8">
  <title>ESP32 Termostat</title>
  <style>
    body { font-family: Arial, sans-serif; max-width: 800px; margin: 0 auto; padding: 10px; }
    h1 { text-align: center; }
    #status { margin-bottom: 10px; padding: 10px; border: 1px solid #ccc; }
    canvas { border: 1px solid #ccc; width: 100%; max-width: 800px; height: 300px; }
    form { margin-top: 10px; padding: 10px; border: 1px solid #ccc; }
    label { display: inline-block; width: 100px; }
    input[type="number"] { width: 80px; }
    button { margin-top: 10px; padding: 5px 10px; }
    .heater-on { color: green; font-weight: bold; }
    .heater-off { color: red; font-weight: bold; }
  </style>
</head>
<body>
  <h1>ESP32 Termostat</h1>

  <div id="status">
    <div>Aktuel temperatur: <span id="currentTemp">-</span> &deg;C</div>
    <div>Setpoint: <span id="currentSetpoint">-</span> &deg;C</div>
    <div>Hysterese: <span id="currentHyst">-</span> &deg;C</div>
    <div>Varme: <span id="heaterStatus" class="heater-off">-</span></div>
    <div>State: <span id="stateText">-</span></div>
  </div>

  <canvas id="graph" width="800" height="300"></canvas>

  <form id="configForm">
    <h3>Konfiguration</h3>
    <div>
      <label for="setpoint">Setpoint:</label>
      <input type="number" step="0.1" id="setpoint" name="setpoint"> &deg;C
    </div>
    <div>
      <label for="hysteresis">Hysterese:</label>
      <input type="number" step="0.1" id="hysteresis" name="hysteresis"> &deg;C
    </div>
    <button type="submit">Gem</button>
  </form>

  <script>
    let timerId = null;

    async function fetchData() {
      try {
        const res = await fetch('/data');
        const data = await res.json();

        const count = data.count || 0;
        let currentTemp = "-";
        if (count > 0) currentTemp = data.temps[count - 1].toFixed(1);

        document.getElementById('currentTemp').textContent     = currentTemp;
        document.getElementById('currentSetpoint').textContent = data.setpoint.toFixed(1);
        document.getElementById('currentHyst').textContent     = data.hysteresis.toFixed(1);

        const heaterStatus = document.getElementById('heaterStatus');
        if (data.heaterOn) { heaterStatus.textContent = "TÆNDT (HEATING)"; heaterStatus.className = "heater-on"; }
        else { heaterStatus.textContent = "SLUKKET"; heaterStatus.className = "heater-off"; }

        const stateText = document.getElementById('stateText');
        if (data.state == 0) stateText.textContent = "COOLING";
        else if (data.state == 1) stateText.textContent = "ONTARGET";
        else if (data.state == 2) stateText.textContent = "HEATING";

        document.getElementById('setpoint').value   = data.setpoint.toFixed(1);
        document.getElementById('hysteresis').value = data.hysteresis.toFixed(1);

        drawGraph(data);
      } catch (e) { console.error(e); }
    }

    function drawGraph(data) {
      const canvas = document.getElementById('graph');
      const ctx = canvas.getContext('2d');
      ctx.clearRect(0, 0, canvas.width, canvas.height);

      const temps = data.temps || [];
      const count = data.count || 0;
      if (count < 2) return;

      let minT = temps[0], maxT = temps[0];
      for (let i = 1; i < count; i++) {
        if (temps[i] < minT) minT = temps[i];
        if (temps[i] > maxT) maxT = temps[i];
      }
      if (maxT === minT) { maxT += 0.5; minT -= 0.5; }

      const padding = 20;
      const w = canvas.width  - 2 * padding;
      const h = canvas.height - 2 * padding;

      ctx.beginPath();
      ctx.moveTo(padding, padding);
      ctx.lineTo(padding, padding + h);
      ctx.lineTo(padding + w, padding + h);
      ctx.stroke();

      ctx.beginPath();
      for (let i = 0; i < count; i++) {
        const x = padding + (i * w) / (count - 1);
        const y = padding + h - ((temps[i] - minT) / (maxT - minT)) * h;
        if (i === 0) ctx.moveTo(x, y);
        else ctx.lineTo(x, y);
      }
      ctx.stroke();

      const sp  = data.setpoint;
      const spY = padding + h - ((sp - minT) / (maxT - minT)) * h;
      ctx.setLineDash([5,3]);
      ctx.beginPath();
      ctx.moveTo(padding, spY);
      ctx.lineTo(padding + w, spY);
      ctx.stroke();
      ctx.setLineDash([]);

      ctx.fillText(maxT.toFixed(1) + "°C", padding + 5, padding + 10);
      ctx.fillText(minT.toFixed(1) + "°C", padding + 5, padding + h - 5);
    }

    async function sendConfig(event) {
      event.preventDefault();
      const sp = document.getElementById('setpoint').value;
      const h  = document.getElementById('hysteresis').value;
      try {
        await fetch('/set?sp=' + encodeURIComponent(sp) + '&h=' + encodeURIComponent(h));
        fetchData();
      } catch (e) { console.error(e); }
    }

    window.onload = function() {
      document.getElementById('configForm').addEventListener('submit', sendConfig);
      fetchData();
      timerId = setInterval(fetchData, 5000);
    };
  </script>
</body>
</html>
)rawliteral";

// ==== HTML: AP-setup-mode (WiFi konfiguration) ====
const char ap_html[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
  <meta charset="utf-8">
  <title>Thermostat WiFi Setup</title>
  <style>
    body { font-family: Arial, sans-serif; max-width: 400px; margin: 0 auto; padding: 10px; }
    h1 { text-align: center; }
    form { border: 1px solid #ccc; padding: 10px; margin-top: 10px; }
    label { display: block; margin-top: 5px; }
    input[type="text"], input[type="password"] { width: 100%; }
    button { margin-top: 10px; padding: 5px 10px; }
  </style>
</head>
<body>
  <h1>WiFi Setup</h1>
  <p>Indtast dit WiFi-netværk og password.</p>
  <form method="POST" action="/save">
    <label>SSID:
      <input type="text" name="ssid" required>
    </label>
    <label>Password:
      <input type="password" name="pass" required>
    </label>
    <button type="submit">Gem &amp; genstart</button>
  </form>
</body>
</html>
)rawliteral";

// ==== Web handlers (STA-mode) ====
void handleRoot() {
  if (apMode) {
    server.send_P(200, "text/html", ap_html);
  } else {
    server.send_P(200, "text/html", main_html);
  }
}

void handleData() {
  if (apMode) {
    server.send(400, "text/plain", "AP setup mode, no data");
    return;
  }

  int count = bufferFilled ? NUM_POINTS : tempIndex;

  String json = "{";
  json += "\"setpoint\":"  + String(setpoint, 1)   + ",";
  json += "\"hysteresis\":"+ String(hysteresis, 1) + ",";
  bool heaterOn = (state == HEATING);
  json += "\"heaterOn\":"  + String(heaterOn ? "true" : "false") + ",";
  json += "\"state\":"     + String((int)state) + ",";
  json += "\"count\":"     + String(count) + ",";
  json += "\"temps\":[";

  for (int i = 0; i < count; i++) {
    int index;
    if (bufferFilled) index = (tempIndex + i) % NUM_POINTS;
    else              index = i;
    json += String(tempHistory[index], 1);
    if (i < count - 1) json += ",";
  }
  json += "]}";

  server.send(200, "application/json", json);
}

void handleSet() {
  if (apMode) {
    server.send(400, "text/plain", "AP setup mode");
    return;
  }

  bool changed = false;

  if (server.hasArg("sp")) {
    setpoint = server.arg("sp").toFloat();
    changed = true;
  }
  if (server.hasArg("h")) {
    hysteresis = server.arg("h").toFloat();
    if (hysteresis < 0.1) hysteresis = 0.1;
    changed = true;
  }

  if (changed) {
    prefs.putDouble("setpoint", setpoint);       // NEW
    prefs.putDouble("hysteresis", hysteresis);   // NEW (optional but nice)
  }

  server.send(200, "text/plain", "OK");
}

// ==== AP-setup handlers ====
void handleApRoot() {
  server.send_P(200, "text/html", ap_html);
}

void handleApSave() {
  if (server.method() == HTTP_POST) {
    String ssidNew = server.arg("ssid");
    String passNew = server.arg("pass");

    prefs.putString("ssid", ssidNew);
    prefs.putString("pass", passNew);

    server.send(200, "text/html",
                "<html><body><h1>Saved!</h1><p>Rebooting...</p></body></html>");
    delay(1000);
    ESP.restart();
  } else {
    server.send(405, "text/plain", "Use POST");
  }
}

void handleNotFound() {
  server.send(404, "text/plain", "Not found");
}

// ==== WiFi / mode-funktioner ====
void startApSetupMode() {
  apMode = true;

  WiFi.disconnect(true);
  WiFi.mode(WIFI_AP);
  WiFi.softAP("ThermoSetup", "12345678");
  IPAddress ip = WiFi.softAPIP();

  Serial.println("AP setup mode started.");
  Serial.print("Connect to WiFi 'ThermoSetup', password '12345678', IP: ");
  Serial.println(ip);

  display.clearDisplay();
  display.setTextSize(1);
  display.setCursor(0, 0);
  display.println("AP Setup mode");
  display.setCursor(0, 16);
  display.println("SSID: ThermoSetup");
  display.display();

  server.stop();
  server.on("/", handleApRoot);
  server.on("/save", HTTP_POST, handleApSave);
  server.onNotFound(handleNotFound);
  server.begin();
}

bool startStaMode() {
  apMode = false;

  Serial.print("Connecting to WiFi SSID: ");
  Serial.println(wifiSsid);

  WiFi.mode(WIFI_STA);
  WiFi.begin(wifiSsid.c_str(), wifiPass.c_str());

  unsigned long start = millis();
  const unsigned long TIMEOUT_MS = 20000;
  while (WiFi.status() != WL_CONNECTED && millis() - start < TIMEOUT_MS) {
    delay(500);
    Serial.print(".");
  }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("WiFi connected, IP: ");
    Serial.println(WiFi.localIP());

    display.clearDisplay();
    display.setCursor(0, 0);
    display.println("WiFi OK");
    display.setCursor(0, 16);
    display.print(WiFi.localIP());
    display.display();

    server.stop();
    server.on("/", handleRoot);
    server.on("/data", handleData);
    server.on("/set", handleSet);
    server.onNotFound(handleNotFound);
    server.begin();

    return true;
  } else {
    Serial.println("WiFi connect failed.");
    return false;
  }
}

// ==== Deep sleep ====
void goToSleep() {
  Serial.println("Going to deep sleep...");
  display.clearDisplay();
  display.setCursor(0, 0);
  display.println("Sleeping...");
  display.display();

  server.stop();
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);

  // Wake on BTN_WAKE going LOW (knap til GND, INPUT_PULLUP)
  esp_sleep_enable_ext0_wakeup((gpio_num_t)BTN_WAKE, 0);
  delay(200);
  esp_deep_sleep_start();
}

// ==== Setup ====
void setup() {
  Serial.begin(115200);
  delay(200);

  Wire.begin(OLED_SDA, OLED_SCL);

  if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
    Serial.println("SSD1306 init failed!");
    for (;;);
  }

  display.clearDisplay();
  display.setTextSize(1);
  display.setCursor(0, 0);
  display.println("Thermostat");
  display.setCursor(0, 16);
  display.println("Booting...");
  display.display();

  pinMode(LED_RED,   OUTPUT);
  pinMode(LED_GREEN, OUTPUT);
  pinMode(LED_BLUE,  OUTPUT);
  pinMode(BTN_UP,    INPUT_PULLUP);
  pinMode(BTN_DOWN,  INPUT_PULLUP);
  pinMode(BTN_WAKE,  INPUT_PULLUP);

  sensors.begin();
  prefs.begin("thermo", false);

  setpoint   = prefs.getDouble("setpoint", setpoint);       // defaults to current (21.0) if none
  hysteresis = prefs.getDouble("hysteresis", hysteresis);   // defaults to 0.5 if none

  // Load historik fra NVS
  loadHistoryFromPrefs();

  // Hent gemt WiFi
  wifiSsid = prefs.getString("ssid", "");
  wifiPass = prefs.getString("pass", "");

  if (wifiSsid.length() == 0) {
    Serial.println("No WiFi config, starting AP setup mode");
    startApSetupMode();
  } else {
    if (!startStaMode()) {
      startApSetupMode();
    }
  }

  awakeStart = millis();
}

// ==== Loop ====
void loop() {
  unsigned long now = millis();
  server.handleClient();

  // --- Wake/AP-setup-knap ---
  bool wakeNow = (digitalRead(BTN_WAKE) == LOW);  // LOW = tryk (INPUT_PULLUP)

  if (wakeNow && !wakeBtnLast) {
    // knap lige trykket ned
    wakePressStart = now;
  }

  if (wakeNow && (now - wakePressStart >= LONG_PRESS_MS) && !apMode) {
    // Langt tryk -> AP setup mode
    Serial.println("Long press on WAKE -> AP setup mode");
    startApSetupMode();
  }

  wakeBtnLast = wakeNow;

  // --- Knapper til setpoint ---
  bool upNow   = (digitalRead(BTN_UP) == LOW);
  bool downNow = (digitalRead(BTN_DOWN) == LOW);

  if (upNow && !upPressed && (now - lastUpPress > DEBOUNCE_MS)) {
    setpoint += 0.5;
    prefs.putDouble("setpoint", setpoint);   // NEW
    upPressed = true;
    lastUpPress = now;
  } else if (!upNow) {
    upPressed = false;
  }

  if (downNow && !downPressed && (now - lastDownPress > DEBOUNCE_MS)) {
    setpoint -= 0.5;
    prefs.putDouble("setpoint", setpoint);   // NEW
    downPressed = true;
    lastDownPress = now;
  } else if (!downNow) {
    downPressed = false;
  }

  // --- Temperatur læsning ---
  if (!apMode && (now - lastTempRead >= TEMP_INTERVAL)) {
    lastTempRead = now;
    sensors.requestTemperatures();
    currentTemp = sensors.getTempCByIndex(0);
    addTemperature(currentTemp);

    Serial.print("Temp: ");
    Serial.println(currentTemp);
  }

  // --- Termostat logik (kun i normal mode) ---
  if (!apMode) {
    state = decideState(currentTemp, setpoint, hysteresis);
    setLEDs(state);
    drawDisplay(currentTemp, setpoint, state);
  }

  // --- Sleep timeout (kun i normal STA-mode) ---
  if (!apMode && (now - awakeStart > AWAKE_DURATION_MS)) {
    goToSleep();
  }

  delay(20);
}