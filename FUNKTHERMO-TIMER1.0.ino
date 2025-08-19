/************************************************************
 * ESP8266 Empfänger + TM1637 + Web-Settings + Akustik-Alarm
 * - mDNS: http://envrx.local/
 * - Endpunkte:
 *    GET /              -> Status + Link zu /settings
 *    GET /api/last      -> JSON {"t":..., "h":..., "age_ms":..., "from":"ip"}
 *    GET /update        -> Messwerte annehmen (Query: token, t, h)
 *    GET /settings      -> HTML-Form (Einstellungen)
 *    POST /save         -> speichert Einstellungen (EEPROM)
 * - Anzeige: Temp/Feuchte im Wechsel, Temp-Format "25:50" == 25,5°C
 * - Display-Policy: Immer an ODER periodisch für X Sekunden alle N Minuten
 * - Alarm: Buzzer bei Grenzwert-Verletzung, Cooldown & Hysterese
 * - Robust: Platzhalter bei NaN, saubere Re-Init nach Speichern/Boot
 ************************************************************/
#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <ESP8266mDNS.h>
#include <TM1637Display.h>
#include <EEPROM.h>

// ===== WLAN / Server =====
const char* WIFI_SSID = "Wilma2001_Ext";
const char* WIFI_PASS = "14D12k82";              // <-- HIER dein Passwort eintragen
const char* HOSTNAME  = "envrx";         // -> envrx.local
const char* TOKEN     = "secret123";     // muss zum Sender passen
ESP8266WebServer server(80);

// ===== TM1637 Display =====
#define TM_CLK   D6
#define TM_DIO   D7
TM1637Display display(TM_CLK, TM_DIO);
uint8_t segBuf[4]    = {0,0,0,0};
uint8_t targetBuf[4] = {0,0,0,0};

// ===== Letzte Messwerte =====
float lastT = NAN;
float lastH = NAN;
String lastFrom = "";
unsigned long lastAt = 0;

// ===== Anzeige / Animation =====
uint8_t brightness = 4;   // 0..7 (Default)
bool pulseUp = true;

enum ShowMode { SHOW_TEMP=0, SHOW_HUMI=1 };
ShowMode mode = SHOW_TEMP;
unsigned long modeChangedAt = 0;
const unsigned long MODE_DUR_MS = 6000;

enum Anim { ANIM_SLIDE=0, ANIM_PULSE=1, ANIM_WIPE=2 };
uint8_t animIndex = 0;
unsigned long animTickAt = 0;
const unsigned long ANIM_STEP_MS = 60;

bool animSlideInit = true; int slidePos = 4;
bool animPulseInit = true;
bool animWipeInit  = true; int wipeIndex = 0;

// ===== Display-Policy =====
enum DisplayMode : uint8_t { DISP_ALWAYS=0, DISP_PERIODIC=1 };
bool displayIsOn = true;
unsigned long displayWakeStart = 0;
unsigned long lastWakeAt = 0;

// ===== Alarm / Buzzer + Settings =====
struct Settings {
  uint32_t magic;
  bool     alarmEnabled;
  float    tempLow;              // ≤ Frost
  float    tempHigh;             // ≥ Hitze
  float    humiLow;              // ≤ trocken
  float    humiHigh;             // ≥ Schimmelgefahr
  uint8_t  buzzerPin;            // Standard D8
  uint16_t alarmMinIntervalS;    // Cooldown
  DisplayMode dispMode;          // Always/Periodic
  uint16_t dispWakeIntervalMin;  // alle N Minuten
  uint16_t dispOnSeconds;        // für S Sekunden
  uint8_t  dispBrightness;       // 0..7
  uint8_t  reserved[8];
} cfg;

const uint32_t CFG_MAGIC = 0x42A1D0C5;
unsigned long lastAlarmAt = 0;

// ---------- EEPROM / Config ----------
void loadDefaults() {
  cfg.magic = CFG_MAGIC;
  cfg.alarmEnabled = true;
  cfg.tempLow  = 2.0f;
  cfg.tempHigh = 35.0f;
  cfg.humiLow  = 30.0f;
  cfg.humiHigh = 70.0f;
  cfg.buzzerPin = D8;
  cfg.alarmMinIntervalS = 60;
  cfg.dispMode = DISP_ALWAYS;
  cfg.dispWakeIntervalMin = 5;
  cfg.dispOnSeconds = 10;
  cfg.dispBrightness = 4;
}
void saveConfig() { EEPROM.put(0, cfg); EEPROM.commit(); }
void loadConfig() {
  EEPROM.get(0, cfg);
  if (cfg.magic != CFG_MAGIC) { loadDefaults(); saveConfig(); }
}

// ---------- Utils ----------
uint8_t parsePinString(const String& s) {
  String x = s; x.trim();
  if (x.length()==0) return D8;
  if (x[0]=='D' || x[0]=='d') {
    int n = x.substring(1).toInt();
    switch(n){ case 0:return D0; case 1:return D1; case 2:return D2; case 3:return D3;
               case 4:return D4; case 5:return D5; case 6:return D6; case 7:return D7; case 8:return D8; }
    return D8;
  }
  int v = x.toInt();
  if (v<0 || v>16) return D8;
  return (uint8_t)v;
}

// ---------- Display-Helfer ----------
void blitBuf() { display.setSegments(segBuf); }

void updateColon(bool on) {           // DP-Bit an Digit[1] = 0x80
  if (on)  segBuf[1] |= 0x80;
  else     segBuf[1] &= ~0x80;
  blitBuf();
}

// Platzhalter, solange keine Messwerte empfangen wurden
void makePlaceholderTemp() {
  for (int i=0;i<4;i++) targetBuf[i] = 0;
  targetBuf[0] = 0x40;          // '-'
  targetBuf[1] = 0x40 | 0x80;   // '-' + Colon
  targetBuf[2] = 0x40;          // '-'
  targetBuf[3] = 0x40;          // '-'
}
void makePlaceholderHumi() {
  for (int i=0;i<4;i++) targetBuf[i] = 0;
  targetBuf[2] = 0x40;          // '-'
  targetBuf[3] = 0x40;          // '-'
}

// Temperatur im Format "25:50" (25,5°C), NaN-safe
void makeTargetTempFancy(float tempC) {
  if (isnan(tempC)) { makePlaceholderTemp(); return; }

  int t10 = (int)round(tempC * 10.0f); // 25.5 -> 255
  int t_whole = t10 / 10;
  int t_dec   = abs(t10 % 10);

  for (int i=0;i<4;i++) targetBuf[i] = 0;
  int absWhole = abs(t_whole);
  targetBuf[0] = display.encodeDigit((absWhole/10)%10);
  targetBuf[1] = display.encodeDigit(absWhole%10) | 0x80; // Colon an
  targetBuf[2] = display.encodeDigit(t_dec);
  targetBuf[3] = 0; // frei

  if (t_whole < 0) {                 // Minus statt führender 0
    targetBuf[0] = 0x40;             // '-'
    targetBuf[1] = display.encodeDigit(absWhole%10) | 0x80;
  }
}

// Feuchteanzeige, NaN-safe
void makeTargetHumi(float h) {
  if (isnan(h)) { makePlaceholderHumi(); return; }

  int hv = (int)round(h);
  hv = constrain(hv, 0, 100);
  for (int i=0;i<4;i++) targetBuf[i]=0;
  if (hv >= 100) {
    targetBuf[1] = display.encodeDigit(1);
    targetBuf[2] = display.encodeDigit(0);
    targetBuf[3] = display.encodeDigit(0);
  } else {
    targetBuf[2] = display.encodeDigit((hv/10)%10);
    targetBuf[3] = display.encodeDigit(hv%10);
  }
}

void rebuildTargetForMode() {
  if (mode == SHOW_TEMP) makeTargetTempFancy(lastT);
  else                   makeTargetHumi(lastH);

  // Animationen sauber resetten + alte Reste löschen
  animSlideInit = animPulseInit = animWipeInit = true;
  slidePos = 4; wipeIndex = 0;
  for (int i=0;i<4;i++) segBuf[i]=0;
  blitBuf();
}

// ---------- Animationen ----------
void animSlideStep() {
  if (animSlideInit) { slidePos = 4; animSlideInit=false; }
  for (int i=0;i<4;i++) segBuf[i]=0;
  for (int i=0;i<4;i++) {
    int dst = i + slidePos - 4;
    if (dst>=0 && dst<4) segBuf[dst] = targetBuf[i];
  }
  blitBuf();
  if (slidePos>0) slidePos--;
}
void animPulseStep() {
  if (animPulseInit) { memcpy(segBuf, targetBuf, 4); animPulseInit=false; }
  blitBuf();
  if (pulseUp) { if (brightness < 7) brightness++; else pulseUp=false; }
  else         { if (brightness > 1) brightness--; else pulseUp=true; }
  display.setBrightness(brightness);
}
void animWipeStep() {
  if (animWipeInit) { for (int i=0;i<4;i++) segBuf[i]=0; wipeIndex=0; animWipeInit=false; }
  if (wipeIndex < 4) { segBuf[wipeIndex] = targetBuf[wipeIndex]; wipeIndex++; }
  blitBuf();
}

// ---------- Display-Policy ----------
void displayPower(bool on) {
  displayIsOn = on;
  display.setBrightness(cfg.dispBrightness, on); // off: Display aus
  if (on) blitBuf();
}
void applyDisplayPolicy(bool immediate) {
  if (cfg.dispMode == DISP_ALWAYS) {
    displayPower(true);
    return;
  }
  if (immediate) {
    unsigned long now = millis();
    lastWakeAt = now;
    displayWakeStart = now;
    displayPower(true);
    return;
  }
  // Periodik steuert loop()
}

// ---------- Alarm-Logik ----------
bool isOutOfRange(float t, float h) {
  if (!cfg.alarmEnabled) return false;
  if (!isnan(t) && (t <= cfg.tempLow || t >= cfg.tempHigh)) return true;
  if (!isnan(h) && (h <= cfg.humiLow || h >= cfg.humiHigh)) return true;
  return false;
}
void beepPatternWarning() {
  uint8_t pin = cfg.buzzerPin;
  tone(pin, 1200, 120); delay(160);
  tone(pin, 1600, 120); delay(160);
  tone(pin, 900,  160); delay(220);
  noTone(pin);
}

// ---------- HTML ----------
String htmlIndex() {
  String t = isnan(lastT) ? "–" : String(lastT, 1) + " °C";
  String h = isnan(lastH) ? "–" : String(lastH, 0) + " %";
  String s;
  s.reserve(2000);
  s += F("<!doctype html><html><head><meta charset='utf-8'>"
         "<meta name='viewport' content='width=device-width,initial-scale=1'>"
         "<title>ESP8266 Empfänger</title>"
         "<style>body{font-family:system-ui;max-width:720px;margin:24px auto;padding:0 12px}"
         "a.btn{display:inline-block;padding:8px 12px;border:1px solid #ccc;border-radius:8px;text-decoration:none}"
         "table{border-collapse:collapse}td{padding:4px 8px}</style>"
         "</head><body>");
  s += "<h1>ESP8266 Empfänger</h1>";
  s += "<p><b>IP:</b> " + WiFi.localIP().toString() + "</p>";
  s += "<table><tr><td>Temperatur:</td><td><b>" + t + "</b></td></tr>";
  s += "<tr><td>Luftfeuchte:</td><td><b>" + h + "</b></td></tr>";
  s += "<tr><td>Letztes Update:</td><td>" + (lastAt ? (String((millis()-lastAt)/1000) + " s her") : "–") + "</td></tr>";
  s += "<tr><td>Von:</td><td>" + (lastFrom.length()? lastFrom : "–") + "</td></tr></table>";
  s += "<p><a class='btn' href='/settings'>Einstellungen</a> &nbsp; <a class='btn' href='/api/last'>API</a></p>";
  s += "</body></html>";
  return s;
}
String htmlSettings() {
  String s;
  s.reserve(4000);
  s += F("<!doctype html><html><head><meta charset='utf-8'>"
         "<meta name='viewport' content='width=device-width,initial-scale=1'>"
         "<title>Einstellungen</title>"
         "<style>body{font-family:system-ui;max-width:760px;margin:24px auto;padding:0 12px}"
         "fieldset{margin:12px 0;padding:12px;border-radius:8px}"
         "label{display:block;margin:6px 0 4px}input,select{padding:6px 8px;width:100%;box-sizing:border-box}"
         "button{padding:10px 14px;border-radius:8px;border:1px solid #888;background:#eee;cursor:pointer}"
         "</style></head><body>");
  s += "<h1>Einstellungen</h1><form method='POST' action='/save'>";
  // Alarm
  s += "<fieldset><legend><b>Akustik-Alarm</b></legend>";
  s += "<label><input type='checkbox' name='alarmEnabled' value='1' " + String(cfg.alarmEnabled ? "checked" : "") + "> aktiv</label>";
  s += "<label>Frost-Grenze (Temp ≤):</label><input type='number' step='0.1' name='tempLow' value='" + String(cfg.tempLow,1) + "'>";
  s += "<label>Temp-Obergrenze (Temp ≥):</label><input type='number' step='0.1' name='tempHigh' value='" + String(cfg.tempHigh,1) + "'>";
  s += "<label>Feuchte-Untergrenze (Hum ≤):</label><input type='number' step='0.1' name='humiLow' value='" + String(cfg.humiLow,1) + "'>";
  s += "<label>Feuchte-Obergrenze (Hum ≥):</label><input type='number' step='0.1' name='humiHigh' value='" + String(cfg.humiHigh,1) + "'>";
  s += "<label>Alarm-Cooldown (Sekunden):</label><input type='number' name='alarmMinIntervalS' value='" + String(cfg.alarmMinIntervalS) + "'>";
  s += "<label>Buzzer-Pin:</label><input type='text' name='buzzerPin' value='" + String(cfg.buzzerPin) + "'>";
  s += "<p style='font-size:12px;color:#555'>Aktiver 1-Pin Mini-Speaker (Standard: D8). ESP8266 kann <code>tone()</code>.</p>";
  s += "</fieldset>";
  // Display
  s += "<fieldset><legend><b>LED-Anzeige</b></legend>";
  s += "<label>Modus:</label><select name='dispMode'>";
  s += "<option value='0'" + String(cfg.dispMode==DISP_ALWAYS?" selected":"") + ">Immer an</option>";
  s += "<option value='1'" + String(cfg.dispMode==DISP_PERIODIC?" selected":"") + ">Periodisch aufwecken</option></select>";
  s += "<label>Wake-Intervall (Minuten):</label><input type='number' name='dispWakeIntervalMin' value='" + String(cfg.dispWakeIntervalMin) + "'>";
  s += "<label>Anzeigedauer (Sekunden):</label><input type='number' name='dispOnSeconds' value='" + String(cfg.dispOnSeconds) + "'>";
  s += "<label>Helligkeit (0..7):</label><input type='number' min='0' max='7' name='dispBrightness' value='" + String(cfg.dispBrightness) + "'>";
  s += "</fieldset>";
  s += "<button type='submit'>Speichern</button> &nbsp; <a href='/'><button type='button'>Zurück</button></a>";
  s += "</form></body></html>";
  return s;
}

// ---------- HTTP Handler ----------
void handleRoot()       { server.send(200, "text/html; charset=utf-8", htmlIndex()); }
void handleSettings()   { server.send(200, "text/html; charset=utf-8", htmlSettings()); }
void handleApiLast() {
  String json = "{";
  json += "\"t\":" + (isnan(lastT) ? String("null") : String(lastT, 2)) + ",";
  json += "\"h\":" + (isnan(lastH) ? String("null") : String(lastH, 2)) + ",";
  json += "\"age_ms\":" + String(lastAt ? (millis() - lastAt) : 0) + ",";
  json += "\"from\":\"" + (lastFrom.length() ? lastFrom : String("")) + "\"";
  json += "}";
  server.send(200, "application/json", json);
}
void handleUpdate() {
  if (!server.hasArg("token") || server.arg("token") != TOKEN) {
    server.send(401, "text/plain", "Unauthorized");
    return;
  }
  if (!server.hasArg("t") || !server.hasArg("h")) {
    server.send(400, "text/plain", "Missing t or h");
    return;
  }
  lastT = server.arg("t").toFloat();
  lastH = server.arg("h").toFloat();
  lastAt = millis();
  lastFrom = server.client().remoteIP().toString();
  server.send(200, "application/json", "{\"ok\":true}");
  rebuildTargetForMode();
}
void handleSave() {
  // --- Alarm ---
  cfg.alarmEnabled = server.hasArg("alarmEnabled");
  if (server.hasArg("tempLow"))  cfg.tempLow  = server.arg("tempLow").toFloat();
  if (server.hasArg("tempHigh")) cfg.tempHigh = server.arg("tempHigh").toFloat();
  if (server.hasArg("humiLow"))  cfg.humiLow  = server.arg("humiLow").toFloat();
  if (server.hasArg("humiHigh")) cfg.humiHigh = server.arg("humiHigh").toFloat();
  if (server.hasArg("alarmMinIntervalS")) {
    int s = server.arg("alarmMinIntervalS").toInt();
    cfg.alarmMinIntervalS = (uint16_t)max(5, s); // min 5 s
  }
  if (server.hasArg("buzzerPin")) {
    uint8_t newPin = parsePinString(server.arg("buzzerPin"));
    if (newPin != cfg.buzzerPin) {
      noTone(cfg.buzzerPin);
      pinMode(cfg.buzzerPin, INPUT);
      cfg.buzzerPin = newPin;
      pinMode(cfg.buzzerPin, OUTPUT);
      noTone(cfg.buzzerPin);
    }
  }

  // --- Display ---
  if (server.hasArg("dispMode")) cfg.dispMode = (DisplayMode)server.arg("dispMode").toInt();
  if (server.hasArg("dispWakeIntervalMin")) {
    int m = server.arg("dispWakeIntervalMin").toInt();
    cfg.dispWakeIntervalMin = (uint16_t)max(1, m);  // min 1 min
  }
  if (server.hasArg("dispOnSeconds")) {
    int s = server.arg("dispOnSeconds").toInt();
    cfg.dispOnSeconds = (uint16_t)max(2, s);        // min 2 s
  }
  if (server.hasArg("dispBrightness")) {
    int b = server.arg("dispBrightness").toInt();
    cfg.dispBrightness = (uint8_t)constrain(b, 0, 7);
    brightness = cfg.dispBrightness;
  }

  saveConfig();

  // --- Anzeige & Policy sauber neu starten ---
  modeChangedAt = millis();
  animIndex = 0;
  brightness = cfg.dispBrightness; pulseUp = true;
  display.setBrightness(brightness, true);

  rebuildTargetForMode();
  for (int i=0;i<4;i++) segBuf[i]=0;
  blitBuf();
  updateColon(false);

  if (cfg.dispMode == DISP_ALWAYS) {
    displayPower(true);
  } else {
    unsigned long now = millis();
    lastWakeAt = now;
    displayWakeStart = now;
    displayPower(true);
  }

  server.send(200, "text/html; charset=utf-8",
              F("<!doctype html><meta charset='utf-8'><body style='font-family:system-ui'><h2>Gespeichert.</h2><a href='/'>Zurück</a></body>"));
}

// ---------- Setup / Loop ----------
void setup() {
  Serial.begin(115200);
  delay(30);

  EEPROM.begin(512);
  loadConfig();

  pinMode(cfg.buzzerPin, OUTPUT);
  noTone(cfg.buzzerPin);

  WiFi.mode(WIFI_STA);
  WiFi.hostname(HOSTNAME);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  Serial.printf("WLAN verbinden zu %s ...\n", WIFI_SSID);
  while (WiFi.status() != WL_CONNECTED) { delay(400); Serial.print("."); }
  Serial.printf("\nVerbunden: %s  IP: %s\n", WiFi.SSID().c_str(), WiFi.localIP().toString().c_str());

  server.on("/", handleRoot);
  server.on("/api/last", handleApiLast);
  server.on("/update", handleUpdate);
  server.on("/settings", handleSettings);
  server.on("/save", HTTPMethod::HTTP_POST, handleSave);
  server.begin();
  Serial.println("HTTP-Server gestartet (80)");

  if (MDNS.begin(HOSTNAME)) {
    MDNS.addService("http", "tcp", 80);
    Serial.printf("mDNS aktiv: http://%s.local/\n", HOSTNAME);
  } else {
    Serial.println("mDNS Start fehlgeschlagen");
  }

  brightness = cfg.dispBrightness;
  display.setBrightness(brightness, true);
  for (int i=0;i<4;i++) segBuf[i]=0;
  blitBuf();

  mode = SHOW_TEMP;
  modeChangedAt = millis();
  animIndex = 0;
  rebuildTargetForMode();
  display.setBrightness(cfg.dispBrightness, true);
  updateColon(false);

  applyDisplayPolicy(true);
}

void loop() {
  server.handleClient();
  MDNS.update();

  unsigned long now = millis();

  // Display-Policy Periodik
  if (cfg.dispMode == DISP_PERIODIC) {
    unsigned long intervalMs = max(1UL, (unsigned long)cfg.dispWakeIntervalMin) * 60UL * 1000UL;
    unsigned long onMs       = max(1UL, (unsigned long)cfg.dispOnSeconds)       * 1000UL;

    if (!displayIsOn) {
      if (now - lastWakeAt >= intervalMs) {
        lastWakeAt = now;
        displayWakeStart = now;
        displayPower(true);
        rebuildTargetForMode();
      }
    } else {
      if (now - displayWakeStart >= onMs) {
        displayPower(false);
      }
    }
  }

  // Anzeige nur animieren, wenn an
  if (displayIsOn) {
    if (now - modeChangedAt >= MODE_DUR_MS) {
      mode = (mode == SHOW_TEMP) ? SHOW_HUMI : SHOW_TEMP;
      modeChangedAt = now;
      animIndex = (animIndex + 1) % 3;
      brightness = cfg.dispBrightness; pulseUp = true;
      display.setBrightness(brightness);
      rebuildTargetForMode();
    }
    if (now - animTickAt >= ANIM_STEP_MS) {
      animTickAt = now;
      switch (animIndex) {
        case ANIM_SLIDE: animSlideStep(); break;
        case ANIM_PULSE: animPulseStep(); break;
        case ANIM_WIPE:  animWipeStep();  break;
      }
    }
    if (mode == SHOW_TEMP) updateColon(((now / 500) % 2) != 0);
    else                   updateColon(false);
  }

  // Alarm prüfen (Cooldown + Hysterese)
  static bool latched = false;
  bool out = isOutOfRange(lastT, lastH);
  if (out && cfg.alarmEnabled) {
    if (!latched && (now - lastAlarmAt >= (unsigned long)cfg.alarmMinIntervalS * 1000UL)) {
      beepPatternWarning();
      lastAlarmAt = now;
      latched = true;
    }
  } else {
    bool safeT = true, safeH = true;
    if (!isnan(lastT)) {
      if (lastT <= cfg.tempLow || lastT >= cfg.tempHigh) safeT = false;
      if (lastT > (cfg.tempLow + 0.5f) && lastT < (cfg.tempHigh - 0.5f)) safeT = true;
    }
    if (!isnan(lastH)) {
      if (lastH <= cfg.humiLow || lastH >= cfg.humiHigh) safeH = false;
      if (lastH > (cfg.humiLow + 2.0f) && lastH < (cfg.humiHigh - 2.0f)) safeH = true;
    }
    if (safeT && safeH) latched = false;
  }
}
