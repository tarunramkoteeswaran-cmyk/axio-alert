// Axio Alert demo firmware (prototype, not a medical device).
// Reads HR (MAX30102), GSR, skin temp (DS18B20), motion (MPU6050), sends JSON to the ward dashboard once a second.
// Button: N short presses = self-report N (1-5). Hold 2 s = nurse call. Dashboard can start the breathing guide.
// Board: ESP32C3 Dev Module | ESP32 Arduino core 3.x | USB CDC On Boot: Enabled | Libraries: OneWire, DallasTemperature.
// Copy secrets.h.example to secrets.h and edit it before uploading.
#include <Wire.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include "secrets.h"

const int PIN_SDA = 6, PIN_SCL = 7, PIN_GSR = 3, PIN_DS = 4, PIN_MOTOR = 5, PIN_BTN = 10, PIN_LED = 1;
const uint8_t MAX_ADDR = 0x57, MPU_ADDR = 0x68;
const int MOTOR_MAX = 200;                 // 0-255, breathing guide strength
const uint32_t IR_MIN = 50000;             // below this we assume no skin contact
const uint32_t BREATHE_MS = 30000;         // how long one breathing session lasts

OneWire ow(PIN_DS);
DallasTemperature ds(&ow);
bool contact = false, tempOK = false, callPending = false;
float bpm = 0, tempC = 0, motionMax = 0;
int gsr = 0, reportPending = 0, lastHttp = 0;
uint32_t breatheStart = 0, breatheUntil = 0, ledEvent = 0;

// ---------- I2C helpers ----------
void wr(uint8_t a, uint8_t r, uint8_t v) { Wire.beginTransmission(a); Wire.write(r); Wire.write(v); Wire.endTransmission(); }
int rd(uint8_t a, uint8_t r, uint8_t *b, int n) {
  Wire.beginTransmission(a); Wire.write(r);
  if (Wire.endTransmission(false) != 0) return 0;
  Wire.requestFrom((int)a, n);
  int i = 0; while (Wire.available() && i < n) b[i++] = Wire.read();
  return i;
}

// ---------- MAX30102 heart rate (crude peak timing, demo only) ----------
bool maxInit() {
  uint8_t id = 0;
  if (!rd(MAX_ADDR, 0xFF, &id, 1) || id != 0x15) return false;
  wr(MAX_ADDR, 0x09, 0x40); delay(100);
  wr(MAX_ADDR, 0x08, 0x10);                      // no averaging, FIFO rollover
  wr(MAX_ADDR, 0x09, 0x03);                      // RED + IR
  wr(MAX_ADDR, 0x0A, 0x27);                      // 4096 nA range, 100 sps, 411 us
  wr(MAX_ADDR, 0x0C, 0x24); wr(MAX_ADDR, 0x0D, 0x24);
  wr(MAX_ADDR, 0x04, 0); wr(MAX_ADDR, 0x05, 0); wr(MAX_ADDR, 0x06, 0);
  return true;
}
float fastF = 0, slowF = 0, ampMax = 0, ampMin = 0, ibi[5]; int nIbi = 0, ibiIdx = 0; bool prevPos = false, filt = false; uint32_t lastBeat = 0;
void processIR(uint32_t ir) {
  uint32_t now = millis();
  if (ir < IR_MIN) { contact = false; filt = false; nIbi = 0; bpm = 0; lastBeat = 0; return; }
  contact = true;
  if (!filt) { fastF = slowF = ir; ampMax = ampMin = 0; filt = true; }
  fastF += 0.30f * (ir - fastF); slowF += 0.04f * (ir - slowF);
  float ac = fastF - slowF;
  ampMax = fmaxf(ac, ampMax * 0.995f); ampMin = fminf(ac, ampMin * 0.995f);
  bool pos = ac > 0;
  if (pos && !prevPos && now - lastBeat > 300 && (ampMax - ampMin) > 150) {       // one upward crossing per beat
    if (lastBeat) {
      float t = now - lastBeat, avg = 0;
      for (int i = 0; i < nIbi; i++) avg += ibi[i];
      if (nIbi) avg /= nIbi;
      if (t < 1500 && (nIbi < 3 || fabsf(t - avg) < 0.3f * avg)) {                // reject outliers
        ibi[ibiIdx] = t; ibiIdx = (ibiIdx + 1) % 5; if (nIbi < 5) nIbi++;
        if (nIbi >= 4) { avg = 0; for (int i = 0; i < nIbi; i++) avg += ibi[i]; bpm = 60000.0f / (avg / nIbi); }
      }
    }
    lastBeat = now;
  }
  prevPos = pos;
  if (lastBeat && now - lastBeat > 3000) { bpm = 0; nIbi = 0; }
}
void drainMax() {
  uint8_t p[3]; if (rd(MAX_ADDR, 0x04, p, 3) < 3) return;
  int n = (p[0] - p[2]) & 0x1F;
  for (int i = 0; i < n; i++) {
    uint8_t b[6]; if (rd(MAX_ADDR, 0x07, b, 6) < 6) return;
    processIR((((uint32_t)b[3] << 16) | (b[4] << 8) | b[5]) & 0x3FFFF);
  }
}

// ---------- MPU6050 motion ----------
void readMotion() {
  uint8_t b[14]; if (rd(MPU_ADDR, 0x3B, b, 14) < 14) return;
  float ax = (int16_t)((b[0] << 8) | b[1]) / 16384.0f, ay = (int16_t)((b[2] << 8) | b[3]) / 16384.0f, az = (int16_t)((b[4] << 8) | b[5]) / 16384.0f;
  motionMax = fmaxf(motionMax, fabsf(sqrtf(ax * ax + ay * ay + az * az) - 1.0f));
}

// ---------- button: N short presses = report N, hold 2 s = call ----------
void buttons() {
  static bool down = false, held = false; static uint32_t tDown = 0, tEdge = 0, tRel = 0; static int presses = 0;
  bool p = digitalRead(PIN_BTN) == LOW; uint32_t now = millis();
  if (now - tEdge > 30) {
    if (p && !down) { down = true; held = false; tDown = now; tEdge = now; }
    else if (!p && down) { down = false; tEdge = now; if (!held) { presses++; tRel = now; } }
  }
  if (down && !held && now - tDown >= 2000) { held = true; presses = 0; callPending = true; ledEvent = now + 1500; }
  if (!down && presses > 0 && now - tRel > 1500) { reportPending = presses > 5 ? 5 : presses; presses = 0; ledEvent = now + 600; }
}

// ---------- breathing guide: 4 s ramp up, 6 s fade (6 breaths/min) ----------
void breathing() {
  uint32_t now = millis();
  if (now >= breatheUntil) { ledcWrite(PIN_MOTOR, 0); return; }
  uint32_t t = (now - breatheStart) % 10000;
  float d = t < 4000 ? t / 4000.0f : 1.0f - (t - 4000) / 6000.0f;
  ledcWrite(PIN_MOTOR, (int)(d * MOTOR_MAX));
}
void startBreathing() { uint32_t now = millis(); if (now >= breatheUntil) breatheStart = now; breatheUntil = now + BREATHE_MS; }

void ledUpdate() {
  uint32_t now = millis();
  if (now < ledEvent) digitalWrite(PIN_LED, (now / 100) % 2);
  else if (WiFi.status() == WL_CONNECTED) digitalWrite(PIN_LED, HIGH);
  else digitalWrite(PIN_LED, (now / 500) % 2);
}

// ---------- once a second: read slow sensors and POST ----------
void sendReading() {
  long s = 0; for (int i = 0; i < 16; i++) s += analogRead(PIN_GSR);
  gsr = s / 16;
  float t = ds.getTempCByIndex(0); ds.requestTemperatures();          // reads last second's conversion
  tempOK = t > -50 && t < 85; if (tempOK) tempC = t;
  char b[256]; int n = snprintf(b, sizeof b, "{\"bed\":%d,\"name\":\"%s\",\"contact\":%s,\"motion\":%.2f,\"gsr\":%d", BED_ID, BED_NAME, contact ? "true" : "false", motionMax, gsr);
  if (bpm > 0) n += snprintf(b + n, sizeof b - n, ",\"hr\":%.0f", bpm);
  if (tempOK) n += snprintf(b + n, sizeof b - n, ",\"temp\":%.2f", tempC);
  if (callPending) n += snprintf(b + n, sizeof b - n, ",\"call\":true");
  if (reportPending) n += snprintf(b + n, sizeof b - n, ",\"report\":%d", reportPending);
  snprintf(b + n, sizeof b - n, "}");
  Serial.printf("HR %.0f GSR %d T %.1f motion %.2f contact %d wifi %d http %d\n", bpm, gsr, tempC, motionMax, contact, WiFi.status() == WL_CONNECTED, lastHttp);
  motionMax = 0;
  if (WiFi.status() != WL_CONNECTED) { WiFi.reconnect(); return; }
  HTTPClient http; http.begin(SERVER_URL); http.setTimeout(800); http.addHeader("Content-Type", "application/json");
  lastHttp = http.POST(b);
  if (lastHttp == 200) {
    callPending = false; reportPending = 0;                            // keep them queued if the post failed
    String body = http.getString(); int i = body.indexOf("breathe");
    if (i >= 0) { int j = body.indexOf("true", i); if (j >= 0 && j - i < 14) startBreathing(); }
  }
  http.end();
}

void setup() {
  Serial.begin(115200); delay(1500);
  Wire.begin(PIN_SDA, PIN_SCL); Wire.setClock(100000);
  analogReadResolution(12); analogSetAttenuation(ADC_11db);
  pinMode(PIN_BTN, INPUT_PULLUP); pinMode(PIN_LED, OUTPUT);
  ledcAttach(PIN_MOTOR, 2000, 8); ledcWrite(PIN_MOTOR, 0);
  ds.begin(); ds.setWaitForConversion(false); ds.requestTemperatures();
  Serial.println(maxInit() ? "MAX30102 ok" : "MAX30102 NOT FOUND");
  uint8_t id; Serial.println(rd(MPU_ADDR, 0x75, &id, 1) ? "MPU6050 ok" : "MPU6050 NOT FOUND"); wr(MPU_ADDR, 0x6B, 0);
  WiFi.mode(WIFI_STA); WiFi.setTxPower(WIFI_POWER_8_5dBm);             // helps some SuperMini boards connect reliably
  WiFi.begin(WIFI_SSID, WIFI_PASS);
}

void loop() {
  static uint32_t tMpu = 0, tSend = 0;
  uint32_t now = millis();
  drainMax(); buttons(); breathing(); ledUpdate();
  if (now - tMpu >= 100) { tMpu = now; readMotion(); }
  if (now - tSend >= 1000) { tSend = now; sendReading(); }
  if (Serial.available() && Serial.read() == 'v') startBreathing();    // 'v' in Serial Monitor = test the breathing guide
  delay(5);
}
