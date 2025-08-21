/************************************************************
 * ESP8266 Empfänger + TM1637 4-Digit Display
 * - mDNS: http://envrx.local/
 * - Endpunkte:
 *    GET /              -> HTML Statusseite
 *    GET /api/last      -> JSON {"t":..., "h":..., "age_ms":..., "from":"ip"}
 *    GET /update        -> Messwerte annehmen (Query: token, t, h)
 * - Anzeige (TM1637): wechselt alle 6 s zwischen Temp und Feuchte.
 *   Animationen: Slide-In, Pulse (Helligkeit), Wipe
 * - Colon-Blink: Wird über DP-Bit (0x80) von Digit[1] realisiert (ohne setColon()).
 ************************************************************/
#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <ESP8266mDNS.h>
#include <TM1637Display.h>

// ===== WLAN / Server =====
const char* WIFI_SSID = "SSID";
const char* WIFI_PASS = "PWD";
const char* HOSTNAME  = "envrx";          // -> envrx.local
const char* TOKEN     = "secret123";      // Schreibschutz für /update
ESP8266WebServer server(80);

// ===== Letzte Messwerte =====
float lastT = NAN;
float lastH = NAN;
String lastFrom = "";
unsigned long lastAt = 0;

// ===== TM1637 Display =====
#define TM_CLK   D6
#define TM_DIO   D7
TM1637Display display(TM_CLK, TM_DIO);

uint8_t segBuf[4]    = {0,0,0,0};   // aktuell angezeigte Segmente
uint8_t targetBuf[4] = {0,0,0,0};   // Zielbild für Animationen

uint8_t brightness = 4;   // 0..7
bool pulseUp = true;

// Anzeige-Modus
enum ShowMode { SHOW_TEMP=0, SHOW_HUMI=1 };
ShowMode mode = SHOW_TEMP;
unsigned long modeChangedAt = 0;
const unsigned long MODE_DUR_MS = 6000;  // alle 6 s wechseln

// Animationen
enum Anim { ANIM_SLIDE=0, ANIM_PULSE=1, ANIM_WIPE=2 };
uint8_t animIndex = 0;
unsigned long animTickAt = 0;
const unsigned long ANIM_STEP_MS = 60;

// ===== HTML =====
String htmlPage() {
  String t = isnan(lastT) ? "–" : String(lastT, 1) + " °C";
  String h = isnan(lastH) ? "–" : String(lastH, 0) + " %";
  String s;
  s.reserve(1000);
  s += F("<!doctype html><html><head><meta charset='utf-8'>"
         "<meta name='viewport' content='width=device-width,initial-scale=1'>"
         "<title>ESP8266 Empfänger</title></head><body>");
  s += "<h1>ESP8266 Empfänger</h1>";
  s += "IP: " + WiFi.localIP().toString() + "<br>";
  s += "Temperatur: " + t + "<br>";
  s += "Luftfeuchte: " + h + "<br>";
  s += "Letztes Update: " + (lastAt ? (String((millis()-lastAt)/1000) + " s her") : String("–")) + "<br>";
  s += "Von: " + (lastFrom.length() ? lastFrom : String("–"));
  s += "</body></html>";
  return s;
}

void handleRoot() { server.send(200, "text/html; charset=utf-8", htmlPage()); }
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
  // Neues Zielbild für aktuelle Anzeige vorbereiten
  rebuildTargetForMode();
}

// ===== Display-Helfer =====
void toDigitsSigned(int value, uint8_t out[4]) {
  for (int i=0;i<4;i++) out[i]=0;
  if (value < 0) {
    int v = -value;
    out[0] = 0x40; // '-' (Minus)
    out[1] = display.encodeDigit((v/100)%10);
    out[2] = display.encodeDigit((v/10)%10);
    out[3] = display.encodeDigit(v%10);
  } else {
    out[0] = display.encodeDigit((value/1000)%10);
    out[1] = display.encodeDigit((value/100)%10);
    out[2] = display.encodeDigit((value/10)%10);
    out[3] = display.encodeDigit(value%10);
  }
}
void setDecimalAt(uint8_t buf[4], int digitIndex) {
  if (digitIndex >=0 && digitIndex < 4) buf[digitIndex] |= 0x80;
}
void makeTargetHumi(float h) {
  int hv = (int)round(h);
  hv = constrain(hv, 0, 100);
  for (int i=0;i<4;i++) targetBuf[i]=0;
  // rechtsbündig
  if (hv >= 100) {
    targetBuf[1] = display.encodeDigit(1);
    targetBuf[2] = display.encodeDigit(0);
    targetBuf[3] = display.encodeDigit(0);
  } else {
    targetBuf[2] = display.encodeDigit((hv/10)%10);
    targetBuf[3] = display.encodeDigit(hv%10);
  }
}
void blitBuf() { display.setSegments(segBuf); }

// ===== Animationen =====
bool animSlideInit = true;
int  slidePos = 4;
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
bool animPulseInit = true;
void animPulseStep() {
  if (animPulseInit) { memcpy(segBuf, targetBuf, 4); animPulseInit=false; }
  blitBuf();
  if (pulseUp) {
    if (brightness < 7) brightness++;
    else pulseUp = false;
  } else {
    if (brightness > 1) brightness--;
    else pulseUp = true;
  }
  display.setBrightness(brightness);
}
bool animWipeInit = true;
int  wipeIndex = 0;
void animWipeStep() {
  if (animWipeInit) { for (int i=0;i<4;i++) segBuf[i]=0; wipeIndex=0; animWipeInit=false; }
  if (wipeIndex < 4) {
    segBuf[wipeIndex] = targetBuf[wipeIndex];
    wipeIndex++;
  }
  blitBuf();
}

// ===== Colon (Doppelpunkt) ohne setColon(): DP-Bit in segBuf[1] =====
void updateColon(bool on) {
  if (on)  segBuf[1] |= 0x80;   // Bit setzen
  else     segBuf[1] &= ~0x80;  // Bit löschen
  blitBuf(); // nach Änderung sofort anzeigen
}

// Baut targetBuf abhängig vom Modus
void rebuildTargetForMode() {
  if (mode == SHOW_TEMP) {
    int t10 = (int)round(lastT * 10.0f); // z.B. 25.5°C -> 255
    int t_whole = t10 / 10;              // Ganzzahl 25
    int t_dec   = abs(t10 % 10);         // Zehntel 5

    for (int i=0;i<4;i++) targetBuf[i] = 0;
    // z.B. 25:50 -> Digit0=2, Digit1=5, Colon an, Digit2=5, Digit3=0
    targetBuf[0] = display.encodeDigit((t_whole/10)%10);
    targetBuf[1] = display.encodeDigit(t_whole%10) | 0x80; // 0x80 = Colon an
    targetBuf[2] = display.encodeDigit(t_dec);
    targetBuf[3] = display.encodeDigit(0); // fix "0" für °C/° oder leer lassen

  } else {
    makeTargetHumi(lastH);
  }

  animSlideInit = animPulseInit = animWipeInit = true;
  slidePos = 4; wipeIndex = 0;
}


// ===== Setup / Loop =====
void setup() {
  Serial.begin(115200);
  delay(50);

  WiFi.mode(WIFI_STA);
  WiFi.hostname(HOSTNAME);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  Serial.printf("WLAN verbinden zu %s ...\n", WIFI_SSID);
  while (WiFi.status() != WL_CONNECTED) { delay(400); Serial.print("."); }
  Serial.printf("\nVerbunden: %s  IP: %s\n", WiFi.SSID().c_str(), WiFi.localIP().toString().c_str());

  server.on("/", handleRoot);
  server.on("/api/last", handleApiLast);
  server.on("/update", handleUpdate);
  server.begin();
  Serial.println("HTTP-Server gestartet (80)");

  if (MDNS.begin(HOSTNAME)) {
    MDNS.addService("http", "tcp", 80);
    Serial.printf("mDNS aktiv: http://%s.local/\n", HOSTNAME);
  } else {
    Serial.println("mDNS Start fehlgeschlagen");
  }

  display.setBrightness(brightness, true); // Display an
  for (int i=0;i<4;i++) segBuf[i]=0;
  blitBuf();

  mode = SHOW_TEMP;
  modeChangedAt = millis();
  animIndex = 0;
  rebuildTargetForMode();
}

void loop() {
  server.handleClient();
  MDNS.update();

  unsigned long now = millis();

  // Moduswechsel
  if (now - modeChangedAt >= MODE_DUR_MS) {
    mode = (mode == SHOW_TEMP) ? SHOW_HUMI : SHOW_TEMP;
    modeChangedAt = now;
    animIndex = (animIndex + 1) % 3; // nächster Animationstyp
    brightness = 4; pulseUp = true;
    display.setBrightness(brightness);
    rebuildTargetForMode();
  }

  // Animation abspielen (alle ANIM_STEP_MS)
  if (now - animTickAt >= ANIM_STEP_MS) {
    animTickAt = now;
    switch (animIndex) {
      case ANIM_SLIDE: animSlideStep(); break;
      case ANIM_PULSE: animPulseStep(); break;
      case ANIM_WIPE:  animWipeStep();  break;
    }
  }

  // Colon-Blink NACH der Animation anwenden, damit es nicht überschrieben wird.
  // Bei Temperatur blinkt die Colon alle 500 ms, bei Feuchte aus.
  if (mode == SHOW_TEMP) {
    updateColon(((now / 500) % 2) != 0);
  } else {
    updateColon(false);
  }
}
