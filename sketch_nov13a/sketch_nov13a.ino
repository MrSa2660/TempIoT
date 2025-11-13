#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <OneWire.h>
#include <DallasTemperature.h>

// ==== Pin configuration ====
#define OLED_SDA 21
#define OLED_SCL 22
#define ONE_WIRE_BUS 17  // DS18B20 on GPIO17
#define BTN_UP 33
#define BTN_DOWN 25
#define LED_RED 13
#define LED_GREEN 12
#define LED_BLUE 27

// ==== Constants ====
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 32
#define OLED_RESET -1
#define TEMP_INTERVAL 1000
#define DEBOUNCE_MS 150
#define HYST 0.5

// ==== Global objects ====
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);
OneWire oneWire(ONE_WIRE_BUS);
DallasTemperature sensors(&oneWire);

// ==== Variables ====
double setpoint = 21.0;
unsigned long lastTempRead = 0;
float currentTemp = NAN;
bool upPressed = false, downPressed = false;
unsigned long lastUpPress = 0, lastDownPress = 0;

enum HeatState { COOLING, ONTARGET, HEATING };
HeatState state = ONTARGET;

// ==== Helper ====
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

// ==== Setup ====
void setup() {
  Serial.begin(115200);
  Wire.begin(OLED_SDA, OLED_SCL);

  // Initialize OLED
  if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
    Serial.println("SSD1306 init failed!");
    for (;;);
  }
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 0);
  display.println("Thermostat");
  display.setCursor(0, 16);
  display.println("Booting...");
  display.display();

  // Initialize pins
  pinMode(LED_RED, OUTPUT);
  pinMode(LED_GREEN, OUTPUT);
  pinMode(LED_BLUE, OUTPUT);
  pinMode(BTN_UP, INPUT_PULLUP);
  pinMode(BTN_DOWN, INPUT_PULLUP);

  // Init sensor
  sensors.begin();
  delay(500);
}

// ==== Loop ====
void loop() {
  unsigned long now = millis();

  // ---- Debounced buttons ----
  bool upNow = digitalRead(BTN_UP) == LOW;
  bool downNow = digitalRead(BTN_DOWN) == LOW;

  if (upNow && !upPressed && (now - lastUpPress > DEBOUNCE_MS)) {
    setpoint += 0.5;
    upPressed = true;
    lastUpPress = now;
  } else if (!upNow) {
    upPressed = false;
  }

  if (downNow && !downPressed && (now - lastDownPress > DEBOUNCE_MS)) {
    setpoint -= 0.5;
    downPressed = true;
    lastDownPress = now;
  } else if (!downNow) {
    downPressed = false;
  }

  // ---- Temperature read ----
  if (now - lastTempRead >= TEMP_INTERVAL) {
    lastTempRead = now;
    sensors.requestTemperatures();
    currentTemp = sensors.getTempCByIndex(0);
    Serial.print("Temp: "); Serial.println(currentTemp);
  }

  // ---- Logic ----
  state = decideState(currentTemp, setpoint, HYST);
  setLEDs(state);

  // ---- Display ----
  drawDisplay(currentTemp, setpoint, state);

  delay(35);
}
