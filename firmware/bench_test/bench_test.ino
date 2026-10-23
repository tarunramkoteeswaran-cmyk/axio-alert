// Axio Alert bench test: one sketch, test each part from the Serial Monitor (115200 baud).
// Board: ESP32C3 Dev Module | ESP32 Arduino core 3.x | Tools > USB CDC On Boot: Enabled
// Libraries (Library Manager): OneWire, DallasTemperature (DS18B20 test only).
// Power from USB only. Skip the battery and TP4056 until every test here passes.
// Commands: s scan I2C | h MAX30102 | m MPU6050 | g GSR | t DS18B20 | b button | v motor breathing | l LED | x stop | ? help
#include <Wire.h>
#include <OneWire.h>
#include <DallasTemperature.h>

const int PIN_SDA = 6, PIN_SCL = 7, PIN_GSR = 3, PIN_DS = 4, PIN_MOTOR = 5, PIN_BTN = 10, PIN_LED = 1;
const uint8_t MAX_ADDR = 0x57, MPU_ADDR = 0x68;
const int MOTOR_MAX = 255;               // lower this (0-255) if the motor feels too strong
OneWire ow(PIN_DS);
DallasTemperature ds(&ow);
char mode = 'x';
uint32_t tNext = 0, tPrint = 0;

void wr(uint8_t a, uint8_t r, uint8_t v) { Wire.beginTransmission(a); Wire.write(r); Wire.write(v); Wire.endTransmission(); }
int rd(uint8_t a, uint8_t r, uint8_t *buf, int n) {
  Wire.beginTransmission(a); Wire.write(r);
  if (Wire.endTransmission(false) != 0) return 0;
  Wire.requestFrom((int)a, n);
  int i = 0; while (Wire.available() && i < n) buf[i++] = Wire.read();
  return i;
}

void scan() {
  int n = 0;
  for (uint8_t a = 1; a < 127; a++) {
    Wire.beginTransmission(a);
    if (Wire.endTransmission() == 0) { Serial.printf("  found 0x%02X\n", a); n++; }
  }
  Serial.printf("%d device(s). Expect 0x57 (MAX30102) and 0x68 (MPU6050).\n", n);
}

bool maxInit() {
  uint8_t id = 0;
  if (!rd(MAX_ADDR, 0xFF, &id, 1) || id != 0x15) { Serial.printf("MAX30102 not found (part id 0x%02X, expected 0x15)\n", id); return false; }
  wr(MAX_ADDR, 0x09, 0x40); delay(100);          // reset
  wr(MAX_ADDR, 0x08, 0x50);                       // average 4 samples, FIFO rollover
  wr(MAX_ADDR, 0x09, 0x03);                       // SpO2 mode (RED + IR)
  wr(MAX_ADDR, 0x0A, 0x27);                       // 4096 nA range, 100 sps, 411 us pulse
  wr(MAX_ADDR, 0x0C, 0x24); wr(MAX_ADDR, 0x0D, 0x24);   // LED currents (about 7 mA)
  wr(MAX_ADDR, 0x04, 0); wr(MAX_ADDR, 0x05, 0); wr(MAX_ADDR, 0x06, 0);
  Serial.println("MAX30102 ready. Rest a fingertip on the sensor, IR should jump well above 50000.");
  return true;
}
void maxRead() {
  uint8_t p[3];
  if (rd(MAX_ADDR, 0x04, p, 3) < 3) return;
  int n = (p[0] - p[2]) & 0x1F;
  if (n == 0) return;
  uint32_t red = 0, ir = 0;
  for (int i = 0; i < n; i++) {
    uint8_t b[6]; if (rd(MAX_ADDR, 0x07, b, 6) < 6) return;
    red = (((uint32_t)b[0] << 16) | (b[1] << 8) | b[2]) & 0x3FFFF;
    ir  = (((uint32_t)b[3] << 16) | (b[4] << 8) | b[5]) & 0x3FFFF;
  }
  Serial.printf("RED %lu  IR %lu  %s\n", (unsigned long)red, (unsigned long)ir, ir > 50000 ? "finger detected" : "no finger");
}

bool mpuInit() {
  uint8_t id = 0;
  if (!rd(MPU_ADDR, 0x75, &id, 1)) { Serial.println("MPU6050 not found at 0x68 (check AD0 to GND)"); return false; }
  Serial.printf("MPU WHO_AM_I 0x%02X (0x68 expected; some clones report other values)\n", id);
  wr(MPU_ADDR, 0x6B, 0);                          // wake up
  Serial.println("MPU ready. At rest motion should be near 0; shake it and it should rise.");
  return true;
}
void mpuRead() {
  uint8_t b[14]; if (rd(MPU_ADDR, 0x3B, b, 14) < 14) return;
  float ax = (int16_t)((b[0] << 8) | b[1]) / 16384.0f, ay = (int16_t)((b[2] << 8) | b[3]) / 16384.0f, az = (int16_t)((b[4] << 8) | b[5]) / 16384.0f;
  float mag = sqrtf(ax * ax + ay * ay + az * az);
  Serial.printf("ax %.2f ay %.2f az %.2f g | motion %.2f\n", ax, ay, az, fabsf(mag - 1.0f));
}

void gsrRead() {
  long s = 0; for (int i = 0; i < 16; i++) s += analogRead(PIN_GSR);
  Serial.printf("GSR raw %ld / 4095\n", s / 16);
}
void tempRead() {
  ds.requestTemperatures();
  float t = ds.getTempCByIndex(0);
  if (t == DEVICE_DISCONNECTED_C) Serial.println("DS18B20 not found: check wiring and the 4.7k pull-up (DATA to 3V3)");
  else Serial.printf("Skin temp probe: %.2f C\n", t);
}

void motorBreath() {                              // 4 s ramp up, 6 s fade = 6 breaths per minute
  uint32_t t = millis() % 10000;
  float d = t < 4000 ? t / 4000.0f : 1.0f - (t - 4000) / 6000.0f;
  ledcWrite(PIN_MOTOR, (int)(d * MOTOR_MAX));
  if (millis() - tPrint > 500) { tPrint = millis(); Serial.printf("%s duty %d%%\n", t < 4000 ? "inhale" : "exhale", (int)(d * 100)); }
}

void buttonTest() {
  static bool down = false, flagged = false; static uint32_t t0 = 0, tLast = 0;
  bool pressed = digitalRead(PIN_BTN) == LOW;
  if (millis() - tLast < 30) return;               // simple debounce
  if (pressed && !down) { down = true; flagged = false; t0 = millis(); tLast = millis(); }
  if (pressed && down && !flagged && millis() - t0 >= 2000) { flagged = true; Serial.println("HOLD 2 s -> nurse call"); }
  if (!pressed && down) { down = false; tLast = millis(); if (!flagged) Serial.printf("short press (%lu ms) -> self-report\n", (unsigned long)(millis() - t0)); }
}

void help() { Serial.println("s scan | h MAX30102 | m MPU6050 | g GSR | t DS18B20 | b button | v motor | l LED | x stop | ? help"); }

void setMode(char c) {
  ledcWrite(PIN_MOTOR, 0); digitalWrite(PIN_LED, LOW);
  mode = c; tNext = 0; Serial.printf("--- mode %c ---\n", c);
  if (c == 's') { scan(); mode = 'x'; }
  else if (c == 'h' && !maxInit()) mode = 'x';
  else if (c == 'm' && !mpuInit()) mode = 'x';
  else if (c == '?') { help(); mode = 'x'; }
}

void setup() {
  Serial.begin(115200); delay(1500);
  Wire.begin(PIN_SDA, PIN_SCL); Wire.setClock(100000);
  analogReadResolution(12); analogSetAttenuation(ADC_11db);
  pinMode(PIN_BTN, INPUT_PULLUP); pinMode(PIN_LED, OUTPUT);
  ledcAttach(PIN_MOTOR, 2000, 8); ledcWrite(PIN_MOTOR, 0);
  ds.begin();
  Serial.println("Axio Alert bench test"); help();
}

void loop() {
  while (Serial.available()) { char c = Serial.read(); if (strchr("smhgtbvlx?", c)) setMode(c); }
  uint32_t now = millis();
  switch (mode) {
    case 'b': buttonTest(); break;
    case 'v': if (now >= tNext) { tNext = now + 20; motorBreath(); } break;
    case 'l': if (now >= tNext) { tNext = now + 250; digitalWrite(PIN_LED, !digitalRead(PIN_LED)); } break;
    case 'h': if (now >= tNext) { tNext = now + 200; maxRead(); } break;
    case 'm': if (now >= tNext) { tNext = now + 200; mpuRead(); } break;
    case 'g': if (now >= tNext) { tNext = now + 200; gsrRead(); } break;
    case 't': if (now >= tNext) { tNext = now + 1000; tempRead(); } break;
  }
}
