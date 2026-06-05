/**
 * GPS Tracker — ESP32 + NEO-6M + A7670E (LTE, UDP)
 *
 * Flow (every wake cycle):
 *  1. Wake from deep sleep
 *  2. Get GPS fix (NEO-6M via UART1)
 *  3. Store position in NVS
 *  4. Send position via A7670E UDP packet (UART2)
 *  5. Power down A7670E, deep sleep for 1 hour
 *
 * Wiring:
 *  NEO-6M
 *    TX   →  GPIO 16 (UART1 RX)
 *    VCC  →  GPIO 25 (via NPN/MOSFET) or 3.3 V direct
 *    GND  →  GND
 *
 *  A7670E
 *    TX      →  GPIO 22 (UART2 RX)
 *    RX      →  GPIO 21 (UART2 TX)
 *    PWRKEY  →  GPIO 26
 *    VBAT    →  LiPo (+) directly — NOT the ESP 3.3 V rail
 *    GND     →  common GND with ESP
 *
 * Library required:
 *  - TinyGPS++  by Mikal Hart
 */

#include <Arduino.h>
#include <WiFi.h>
#include <Preferences.h>
#include <TinyGPS++.h>

// ─── Dry-run flag ─────────────────────────────────────────────────────────────
// true  = skip all modem code, just print what would be sent (test GPS only)
// false = real transmission
#define DRY_RUN false

// ─── Pin config ───────────────────────────────────────────────────────────────

#define GPS_PWR_PIN     25   // HIGH = NEO-6M on. -1 = always powered.
#define LTE_PWRKEY_PIN  26

#define GPS_RX_PIN  16
#define GPS_TX_PIN  17
#define GPS_BAUD    9600

#define LTE_RX_PIN  22
#define LTE_TX_PIN  21
#define LTE_BAUD    115200

// ─── Timeouts ─────────────────────────────────────────────────────────────────

#define GPS_TIMEOUT_MS   90000UL
#define LTE_BOOT_MS      12000UL
#define LTE_CMD_TIMEOUT   5000UL
#define LTE_UDP_TIMEOUT  15000UL

// ─── Retries ──────────────────────────────────────────────────────────────────

#define LTE_CREG_RETRIES   20   // x 2 s = up to 40 s to register
#define LTE_PDP_RETRIES     3

// ─── Sleep ────────────────────────────────────────────────────────────────────

#define SLEEP_DURATION_SEC  3600UL   // 1 hour

// ─── Network config ───────────────────────────────────────────────────────────

#define SERVER_HOST  "YOUR_SERVER_IP"
#define SERVER_PORT  "4210"
#define SERVER_APN   ""       // e.g. "internet" — ask your carrier
#define SIM_PIN      ""               // dont put anything if your SIM doesn't have any code

// ─── NVS ──────────────────────────────────────────────────────────────────────

#define NVS_NS   "gps"
#define NVS_LAT  "lat"
#define NVS_LNG  "lng"

// ─── Globals ──────────────────────────────────────────────────────────────────

HardwareSerial gpsSerial(1);
HardwareSerial lteSerial(2);
TinyGPSPlus    gps;
Preferences    prefs;

// ─── Sleep ────────────────────────────────────────────────────────────────────

void goToSleep() {
  Serial.printf("\nSleeping for %lu s. Goodnight.\n", SLEEP_DURATION_SEC);
  Serial.flush();
  esp_sleep_enable_timer_wakeup((uint64_t)SLEEP_DURATION_SEC * 1000000ULL);
  esp_deep_sleep_start();
}

// ─── GPS ──────────────────────────────────────────────────────────────────────

void gpsPowerOn() {
  if (GPS_PWR_PIN >= 0) {
    pinMode(GPS_PWR_PIN, OUTPUT);
    digitalWrite(GPS_PWR_PIN, HIGH);
    delay(100);
  }
  gpsSerial.begin(GPS_BAUD, SERIAL_8N1, GPS_RX_PIN, GPS_TX_PIN);
}

void gpsPowerOff() {
  gpsSerial.end();
  if (GPS_PWR_PIN >= 0) digitalWrite(GPS_PWR_PIN, LOW);
}

bool getGPSFix(double &lat, double &lng) {
  Serial.println("[GPS] Waiting for fix...");
  unsigned long start   = millis();
  unsigned long lastDot = 0;

  while (millis() - start < GPS_TIMEOUT_MS) {
    while (gpsSerial.available()) gps.encode(gpsSerial.read());

    if (gps.location.isValid()  && gps.location.isUpdated() &&
        gps.hdop.isValid()      && gps.hdop.value() < 300) {
      lat = gps.location.lat();
      lng = gps.location.lng();
      Serial.printf("\n[GPS] Fix: %.6f, %.6f  HDOP: %.1f  Sats: %d\n",
                    lat, lng, gps.hdop.hdop(), gps.satellites.value());
      return true;
    }

    if (millis() - lastDot > 5000) { Serial.print("."); lastDot = millis(); }
  }

  Serial.println("\n[GPS] Timeout — no fix.");
  return false;
}

// ─── NVS ──────────────────────────────────────────────────────────────────────

void storePosition(double lat, double lng) {
  prefs.begin(NVS_NS, false);
  prefs.putDouble(NVS_LAT, lat);
  prefs.putDouble(NVS_LNG, lng);
  prefs.end();
  Serial.println("[NVS] Saved.");
}

bool loadLastPosition(double &lat, double &lng) {
  prefs.begin(NVS_NS, true);
  lat = prefs.getDouble(NVS_LAT, 0.0);
  lng = prefs.getDouble(NVS_LNG, 0.0);
  prefs.end();
  return (lat != 0.0 || lng != 0.0);
}

// ─── AT helpers ───────────────────────────────────────────────────────────────

bool atCmd(const char *cmd, const char *expected,
           unsigned long timeoutMs = LTE_CMD_TIMEOUT) {
  while (lteSerial.available()) lteSerial.read();
  Serial.printf("[LTE] >> %s\n", cmd);
  lteSerial.println(cmd);

  String buf;
  buf.reserve(256);
  unsigned long t = millis();
  while (millis() - t < timeoutMs) {
    while (lteSerial.available()) buf += (char)lteSerial.read();
    if (buf.indexOf(expected) >= 0) {
      Serial.printf("[LTE] << %s\n", buf.c_str());
      return true;
    }
  }
  Serial.printf("[LTE] TIMEOUT — got: %s\n", buf.c_str());
  return false;
}

String atResp(const char *cmd, unsigned long timeoutMs = LTE_CMD_TIMEOUT) {
  while (lteSerial.available()) lteSerial.read();
  Serial.printf("[LTE] >> %s\n", cmd);
  lteSerial.println(cmd);
  String buf;
  buf.reserve(256);
  unsigned long t = millis();
  while (millis() - t < timeoutMs) {
    while (lteSerial.available()) buf += (char)lteSerial.read();
    if (buf.indexOf("OK") >= 0 || buf.indexOf("ERROR") >= 0) break;
  }
  Serial.printf("[LTE] << %s\n", buf.c_str());
  return buf;
}

// ─── A7670E power ─────────────────────────────────────────────────────────────

void ltePowerOn() {
  pinMode(LTE_PWRKEY_PIN, OUTPUT);
  digitalWrite(LTE_PWRKEY_PIN, LOW);

  lteSerial.begin(LTE_BAUD, SERIAL_8N1, LTE_RX_PIN, LTE_TX_PIN);

  Serial.println("[LTE] PWRKEY pulse...");
  digitalWrite(LTE_PWRKEY_PIN, HIGH);
  delay(2000);
  digitalWrite(LTE_PWRKEY_PIN, LOW);

  unsigned long t = millis();
  while (millis() - t < LTE_BOOT_MS) {
    if (atCmd("AT", "OK", 1000)) {
      atCmd("ATE0", "OK");
      if (strlen(SIM_PIN) > 0) {
        atCmd("AT+CPIN=\"" SIM_PIN "\"", "OK");
      }
      Serial.println("[LTE] Ready.");
      return;
    }
  }
  Serial.println("[LTE] No response after boot.");
}

void ltePowerOff() {
  Serial.println("[LTE] Shutting down...");
  lteSerial.println("AT+CPOF");
  delay(3000);
  lteSerial.end();
}

// ─── Network bring-up ─────────────────────────────────────────────────────────

bool lteNetworkUp() {

  if (!atCmd("AT+CPIN?", "READY")) {
    Serial.println("[LTE] SIM not ready");
    return false;
  }

  Serial.println("[LTE] Waiting for LTE registration...");
  bool reg = false;
  for (int i = 0; i < LTE_CREG_RETRIES && !reg; i++) {
    String r = atResp("AT+CEREG?", 2000);
    if (r.indexOf(",1") >= 0 || r.indexOf(",5") >= 0) {
      reg = true;
    } else {
      r = atResp("AT+CGREG?", 2000);
      if (r.indexOf(",1") >= 0 || r.indexOf(",5") >= 0) reg = true;
    }
    if (!reg) delay(2000);
  }
  if (!reg) { Serial.println("[LTE] Registration failed"); return false; }
  Serial.println("[LTE] Registered.");

  atCmd("AT+CIPSHUT", "SHUT OK", 5000);

  { String cmd = String("AT+CGDCONT=1,\"IP\",\"") + SERVER_APN + "\"";
    atCmd(cmd.c_str(), "OK"); }

  bool pdp = false;
  for (int i = 0; i < LTE_PDP_RETRIES && !pdp; i++) {
    if (atCmd("AT+CGACT=1,1", "OK", 10000)) {
      pdp = true;
    } else {
      Serial.printf("[LTE] PDP activate failed, retry %d/%d\n", i + 1, LTE_PDP_RETRIES);
      delay(2000);
    }
  }
  if (!pdp) { Serial.println("[LTE] PDP context failed"); return false; }
  Serial.println("[LTE] PDP active.");

  return true;
}

// ─── UDP send ─────────────────────────────────────────────────────────────────

bool lteSendPosition(double lat, double lng) {

  if (!atCmd("AT", "OK")) return false;
  if (!lteNetworkUp())    return false;

  { String cmd = String("AT+CIPSTART=0,\"UDP\",\"") + SERVER_HOST + "\"," + SERVER_PORT;
    if (!atCmd(cmd.c_str(), "OK", LTE_UDP_TIMEOUT)) {
      if (!atCmd(cmd.c_str(), "CONNECT", LTE_UDP_TIMEOUT)) {
        Serial.println("[LTE] CIPSTART failed");
        return false;
      }
    }
  }
  delay(500);

  char body[32];
  snprintf(body, sizeof(body), "%.6f,%.6f", lat, lng);
  int len = strlen(body);

  { String cmd = String("AT+CIPSEND=0,") + len;
    if (!atCmd(cmd.c_str(), ">", 5000)) {
      atCmd("AT+CIPCLOSE=0", "OK");
      return false;
    }
  }

  lteSerial.print(body);
  delay(200);
  lteSerial.write(0x1A);   // Ctrl+Z

  if (!atCmd("", "SEND OK", 5000)) {
    Serial.println("[LTE] SEND OK not received — packet may still have gone out");
  }

  atCmd("AT+CIPCLOSE=0", "OK", 3000);
  Serial.printf("[LTE] Sent: %s\n", body);
  return true;
}

// ─── Entry point ──────────────────────────────────────────────────────────────

void setup() {
  Serial.begin(115200);
  delay(100);

  WiFi.mode(WIFI_OFF);
  btStop();

  Serial.println("\n=== GPS Tracker — awake ===");

  // 1. GPS fix
  gpsPowerOn();
  double lat = 0.0, lng = 0.0;
  bool fresh = getGPSFix(lat, lng);
  gpsPowerOff();

  if (!fresh) {
    if (!loadLastPosition(lat, lng)) {
      Serial.println("No fix, no cache. Sleeping.");
      goToSleep();
      return;
    }
    Serial.printf("[NVS] Using cached: %.6f, %.6f\n", lat, lng);
  } else {
    storePosition(lat, lng);
  }

  // 2. Send
#if DRY_RUN
  Serial.printf("[DRY RUN] Would send UDP to %s:%s  →  %.6f,%.6f\n",
                SERVER_HOST, SERVER_PORT, lat, lng);
#else
  ltePowerOn();
  lteSendPosition(lat, lng);
  ltePowerOff();
#endif

  // 3. Sleep
  goToSleep();
}

void loop() {}
