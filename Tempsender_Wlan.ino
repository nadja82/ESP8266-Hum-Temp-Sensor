/************************************************************
ONLY Recive without Display !!!!


 * ESP8266 Receiver: Temperatur & Humid über HTTP
 * - mDNS: http://envrx.local/
 * - Endpunkte:
 *    GET /              -> HTML Statusseite
 *    GET /api/last      -> JSON {"t":..., "h":..., "age_ms":..., "from":"ip"}
 *    GET /update        -> Messwerte annehmen (Query: token, t, h)
 * - Sicherheit: einfacher Token (unten TOKEN)
 ************************************************************/
#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <ESP8266mDNS.h>

const char* WIFI_SSID = "SSID";
const char* WIFI_PASS = "PWD";
const char* HOSTNAME  = "envrx";          // ergibt envrx.local
const char* TOKEN     = "secret123";      // einfacher Schreibschutz

ESP8266WebServer server(80);

float lastT = NAN;
float lastH = NAN;
String lastFrom = "";
unsigned long lastAt = 0;

String htmlPage() {
  String age = (lastAt == 0) ? "–" : String(millis() - lastAt);
  String t = isnan(lastT) ? "–" : String(lastT, 1) + " °C";
  String h = isnan(lastH) ? "–" : String(lastH, 1) + " %";
  String ip = WiFi.localIP().toString();

  String s = F(
    "<!doctype html><html><head><meta charset='utf-8'>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>ESP8266 Empfänger</title>"
    "<style>"
    "body{font-family:system-ui,-apple-system,Segoe UI,Roboto,Arial;margin:24px;background:#0f172a;color:#e5e7eb}"
    ".card{max-width:520px;padding:20px;border-radius:16px;background:#111827;box-shadow:0 8px 30px rgba(0,0,0,.35)}"
    "h1{margin:0 0 8px;font-size:22px}"
    ".kv{display:grid;grid-template-columns:140px 1fr;gap:8px;margin-top:12px}"
    ".muted{color:#9CA3AF}"
    "code{background:#0b1220;padding:2px 6px;border-radius:6px}"
    "a{color:#93c5fd;text-decoration:none}"
    "</style></head><body>"
    "<div class='card'>"
    "<h1>ESP8266 Empfänger</h1>"
  );
  s += "<div class='kv'><div>IP:</div><div><code>" + ip + "</code></div>";
  s += "<div>Temperatur:</div><div><strong>" + t + "</strong></div>";
  s += "<div>Luftfeuchte:</div><div><strong>" + h + "</strong></div>";
  s += "<div>Letztes Update:</div><div>" + (lastAt ? (String((millis() - lastAt)/1000) + " s her") : "–") + "</div>";
  s += "<div>Von:</div><div>" + (lastFrom.length() ? lastFrom : "–") + "</div></div>";
  s += "<p class='muted' style='margin-top:14px'>API: <a href='/api/last'>/api/last</a></p>";
  s += "</div></body></html>";
  return s;
}

void handleRoot() {
  server.send(200, "text/html; charset=utf-8", htmlPage());
}

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
  Serial.printf("[UPDATE] T=%.2f H=%.2f from %s\n", lastT, lastH, lastFrom.c_str());
}

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
  Serial.println("HTTP-Server gestartet auf Port 80");

  if (MDNS.begin(HOSTNAME)) {
    MDNS.addService("http", "tcp", 80);
    Serial.printf("mDNS aktiv: http://%s.local/\n", HOSTNAME);
  } else {
    Serial.println("mDNS Start fehlgeschlagen");
  }
}

void loop() {
  server.handleClient();
  MDNS.update();
}
