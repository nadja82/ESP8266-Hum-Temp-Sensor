/************************************************************
 * ESP8266 Sender: DHT22 -> HTTP-Update an Empfänger
 * - Findet Empfänger per DNS/mDNS:
 *     1) WiFi.hostByName("envrx.local", ip)
 *     2) MDNS.queryService("http","tcp") Fallback
 ************************************************************/
#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <ESP8266HTTPClient.h>
#include <ESP8266mDNS.h>
#include <WiFiClient.h>
#include <DHT.h>

// --------- Einstellungen ---------
const char* WIFI_SSID = "Wilma2000";
const char* WIFI_PASS = "14D12k82";
const char* HOSTNAME  = "envtx";        // Sender-Hostname
const char* RX_HOST   = "envrx";        // Empfänger-Hostname (ohne ".local")
const char* TOKEN     = "secret123";    // muss zum Empfänger passen

// DHT-Sensor
#define DHTPIN D5
#define DHTTYPE DHT22
DHT dht(DHTPIN, DHTTYPE);

// Sendeintervall
const unsigned long SEND_MS = 10000;

// LED
#ifndef LED_BUILTIN
#define LED_BUILTIN 2
#endif

WiFiClient client;
unsigned long lastSend = 0;
IPAddress rxIP;
bool haveRxIP = false;

bool resolveReceiver() {
  String fqdn = String(RX_HOST) + ".local";
  Serial.printf("Suche Empfänger: %s ...\n", fqdn.c_str());

  // 1) Versuch: DNS/mDNS via hostByName
  if (WiFi.hostByName(fqdn.c_str(), rxIP) == 1) {
    haveRxIP = true;
    Serial.printf("Gefunden via hostByName: %s\n", rxIP.toString().c_str());
    return true;
  }

  // 2) Fallback: mDNS-Service-Browse (Empfänger announced _http._tcp)
  int n = MDNS.queryService("http", "tcp"); // blockiert bis ~2s
  Serial.printf("mDNS Services gefunden: %d\n", n);
  for (int i = 0; i < n; i++) {
    // MDNS.hostname(i) ist der Host ohne ".local"
    if (MDNS.hostname(i) == String(RX_HOST)) {
      rxIP = MDNS.IP(i);
      haveRxIP = true;
      Serial.printf("Gefunden via mDNS queryService: %s\n", rxIP.toString().c_str());
      return true;
    }
  }

  haveRxIP = false;
  Serial.println("Empfänger nicht gefunden.");
  return false;
}

void blinkOK() {
  digitalWrite(LED_BUILTIN, LOW);
  delay(60);
  digitalWrite(LED_BUILTIN, HIGH);
}

void setup() {
  pinMode(LED_BUILTIN, OUTPUT);
  digitalWrite(LED_BUILTIN, HIGH);

  Serial.begin(115200);
  delay(50);
  Serial.println("\nESP8266 Sender startet ...");

  dht.begin();

  WiFi.mode(WIFI_STA);
  WiFi.hostname(HOSTNAME);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  Serial.printf("WLAN verbinden zu %s ...\n", WIFI_SSID);
  while (WiFi.status() != WL_CONNECTED) { delay(400); Serial.print("."); }
  Serial.printf("\nVerbunden: %s  IP: %s\n", WiFi.SSID().c_str(), WiFi.localIP().toString().c_str());

  // mDNS initialisieren (für queryService nötig)
  if (!MDNS.begin(HOSTNAME)) {
    Serial.println("mDNS Start fehlgeschlagen (Sender), fahre trotzdem fort.");
  }

  // gleich zu Beginn versuchen, den Empfänger zu finden
  resolveReceiver();
}

void loop() {
  if (WiFi.status() != WL_CONNECTED) {
    WiFi.disconnect();
    WiFi.begin(WIFI_SSID, WIFI_PASS);
    delay(1000);
    return;
  }

  unsigned long now = millis();
  if (now - lastSend >= SEND_MS) {
    lastSend = now;

    // ggf. Empfänger-IP frisch auflösen
    if (!haveRxIP && !resolveReceiver()) return;

    float h = dht.readHumidity();
    float t = dht.readTemperature(); // °C
    if (isnan(h) || isnan(t)) {
      Serial.println("DHT-Lesefehler, versuche später erneut.");
      haveRxIP = false; // nächste Runde erneut suchen
      return;
    }

    String url = "http://" + rxIP.toString() + "/update?token=" + TOKEN +
                 "&t=" + String(t, 2) + "&h=" + String(h, 2);

    HTTPClient http;
    Serial.printf("GET -> %s\n", url.c_str());
    if (http.begin(client, url)) {
      int code = http.GET();
      if (code > 0) {
        String payload = http.getString();
        Serial.printf("Antwort %d: %s\n", code, payload.c_str());
        if (code == 200) blinkOK();
      } else {
        Serial.printf("HTTP Fehler: %s\n", http.errorToString(code).c_str());
        haveRxIP = false;
      }
      http.end();
    } else {
      Serial.println("http.begin() fehlgeschlagen");
      haveRxIP = false;
    }
  }

  MDNS.update();
}
