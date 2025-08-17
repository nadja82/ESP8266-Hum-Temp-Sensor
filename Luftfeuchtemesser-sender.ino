/*
 * ESP8266 + WE-DA-104-1 (DHTxx) + FS1000A (ASK/OOK) Sender
 * - Misst Temp/Feuchte alle 15 Minuten und sendet per RadioHead RH_ASK
 * Abhängigkeiten:
 *  - DHT sensor library (Adafruit)
 *  - RadioHead (RH_ASK)
 */

#include <Arduino.h>
#include <DHT.h>
#include <RH_ASK.h>
#include <SPI.h>   // von RH_ASK benötigt

// ======== Benutzer-Einstellungen ========
#define SENSOR_ID        1         // Kennung dieses Senders
#define DHTPIN           D5        // NodeMCU: D5 = GPIO14
#define TX_PIN           D2        // NodeMCU: D2 = GPIO4 (FS1000A DATA)
#define DHTTYPE          DHT11     // DHT11 oder DHT22 (AM2302)
const unsigned long SEND_INTERVAL_MS = 10UL * 1000UL; // TEST: alle 10s
const uint8_t SEND_REPEATS = 8;                       // öfter wiederholen

// =======================================

DHT dht(DHTPIN, DHTTYPE);
// RH_ASK(bitrate, rxPin, txPin, pttPin, pttInverted)
RH_ASK driver(2000, 0xFF, TX_PIN, 0xFF, false);

unsigned long lastSend = 0;

bool readDHT(float &t, float &h) {
  for (uint8_t i = 0; i < 5; i++) {
    h = dht.readHumidity();
    t = dht.readTemperature(); // °C
    if (!isnan(h) && !isnan(t)) return true;
    delay(1000);
  }
  return false;
}

void sendMessage(const char* msg) {
  driver.send((uint8_t*)msg, strlen(msg));
  driver.waitPacketSent();
}

void sendReadings(float t, float h) {
  char payload[48];
  snprintf(payload, sizeof(payload), "ID:%d;T:%.2f;H:%.2f", SENSOR_ID, t, h);
  for (uint8_t i = 0; i < SEND_REPEATS; i++) {
    sendMessage(payload);
    delay(20);
  }
}

void sendError(const char* errCode) {
  char payload[32];
  snprintf(payload, sizeof(payload), "ID:%d;ERR:%s", SENSOR_ID, errCode);
  for (uint8_t i = 0; i < SEND_REPEATS; i++) {
    sendMessage(payload);
    delay(20);
  }
}

void setup() {
  Serial.begin(115200);
  delay(200);
  dht.begin();

  if (!driver.init()) {
    Serial.println(F("RH_ASK init fehlgeschlagen!"));
  } else {
    Serial.println(F("RH_ASK init OK."));
  }

  // Erste Messung direkt nach Start
  lastSend = millis() - SEND_INTERVAL_MS;
}

void loop() {
  unsigned long now = millis();

  if (now - lastSend >= SEND_INTERVAL_MS) {
    lastSend = now;

    float t = NAN, h = NAN;
    if (readDHT(t, h)) {
      Serial.print(F("Messung: T=")); Serial.print(t, 2);
      Serial.print(F(" °C, H=")); Serial.print(h, 2);
      Serial.println(F(" %"));
      sendReadings(t, h);
      Serial.println(F("Gesendet."));
    } else {
      Serial.println(F("DHT-Lesefehler nach mehreren Versuchen."));
      sendError("DHT");
    }
  }

  delay(5);
}
