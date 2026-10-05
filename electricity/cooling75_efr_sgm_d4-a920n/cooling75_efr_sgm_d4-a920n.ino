
/*
    Application note: Read a EFR SGM D4 A920N electricity meter via
    phototransistor + 1k resistor interface and SML protocol
    Version 1.0 - 05.10.2026
    Copyright (C) 2026  Jan Laudahn https://laudart.de

    credits:
    - Hartmut Wendt  www.zihatec.de for the code base
    - user "rollercontainer"
             at https://www.photovoltaikforum.com/volkszaehler-org-f131/sml-protokoll-hilfe-gesucht-sml-gt-esp8266-gt-mqtt-t112216-s10.html

    more information about SML protocol:
             http://www.schatenseite.de/2016/05/30/smart-message-language-stromzahler-auslesen/
             https://wiki.volkszaehler.org/software/sml
             https://www.stefan-weigert.de/php_loader/sml.php

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "sml_efr_parser.h"
#include <ESP8266WiFi.h>
#include <SoftwareSerial.h>
#include <MQTTPubSubClient.h>
#include <ArduinoJson.h>
#include <ArduinoOTA.h>

// WiFi and MQTT
const char* SSID = "";
const char* PSK = "";
const char* MQTT_BROKER = "";
const short MQTT_PORT = 1883;
const char* mqtt_topic = "";
const char* mqtt_debug_topic = "";
const char* mqtt_user = "";
const char* mqtt_pw = "";

// transmission
StaticJsonDocument<384> doc;
char mqttjson[384];

// DATA
byte inByte; //byte to store the serial buffer
byte smlMessage[1000]; //byte to store the parsed message
const byte startSequence[] = { 0x1B, 0x1B, 0x1B, 0x1B, 0x01, 0x01, 0x01, 0x01 }; //start sequence of SML protocol
const byte stopSequence[]  = { 0x1B, 0x1B, 0x1B, 0x1B, 0x1A }; //end sequence of SML protocol
bool firstRun = true;
int smlIndex; //index counter within smlMessage array
int smlLength; //number of valid bytes in smlMessage after a complete telegram
int startIndex; //start index for start sequence search
int stopIndex; //start index for stop sequence search
int stage; //index to maneuver through cases
uint64_t deliveredTotal;
// in case of deliver to grid use signed
signed long currentpower; //variable to hold translated "Wirkleistung" value
uint64_t currentconsumption; //variable to hold translated "Gesamtverbrauch" value
uint64_t oldconsumption;

int pin_d2 = 4;

SoftwareSerial MeterSerial(pin_d2, -1, true); // RX only (GPIO3 = HW-Serial-RX!), inverted mode(!)
// MQTTPubSubClient's read loop (lwmqtt_arduino_network_read) calls delay(0)
// between read attempts. On core 3.x delay(0) does NOT yield, so the SDK never
// gets scheduled and lwIP can never hand the incoming TCP data to the client -
// the CONNACK is physically unable to arrive while that loop spins.
// Yielding inside read() fixes it here instead of patching the library.
class YieldingWiFiClient : public WiFiClient {
  public:
    int read(uint8_t* buf, size_t size) override {
      yield(); // let the SDK deliver pending packets
      return WiFiClient::read(buf, size);
    }
};

YieldingWiFiClient espClient;
//PubSubClient client(espClient);
MQTTPubSub::PubSubClient<256> client;

// #define _debug_msg
// #define _debug_sml

// Diagnose: zeigt die Rohbytes, die der Broker auf ein CONNECT antwortet.
// Nach der Fehlersuche wieder auskommentieren.
// #define _mqtt_probe

void setup() {
  // OTA setings
  ArduinoOTA.setHostname("ESP-Strom");
  ArduinoOTA.setPassword("yourPassword");

  // bring up serial ports
  Serial.begin(115200);   // debug via USB
  WiFi.setAutoConnect(true);
  WiFi.setSleepMode(WIFI_NONE_SLEEP);
  WiFi.mode(WIFI_STA);

  // static ip configuration
  IPAddress ip(192, 168, 0, 0);
  IPAddress dns(192, 168, 0, 1);
  IPAddress gateway(192, 168, 0, 1);
  IPAddress subnet(255, 255, 255, 0);
  WiFi.config(ip, dns, gateway, subnet);

  setup_wifi();
  // OTA/mDNS erst starten, wenn die Station wirklich eine IP hat
  ArduinoOTA.begin();
  delay(300); // let the WiFi/TCP stack settle before the first blocking connect
#ifdef _mqtt_probe
  probe_broker();
#endif
  espClient.connect(MQTT_BROKER, MQTT_PORT);
  client.begin(espClient);
  client.setCleanSession(true);
  // MUST stay below the ~3.2 s software watchdog: MQTTPubSubClient's
  // lwmqtt_arduino_network_read() busy-waits on millis() and only calls
  // delay(0), which does NOT yield on core 3.x - the default 5000 ms
  // therefore guarantees a "Soft WDT reset" whenever the broker is slow.
  client.setTimeout(1000);
  mqtt_reconnect();
  // start the meter's SoftwareSerial (interrupt-driven bit-banging) only after
  // WiFi/MQTT bring-up, so its ISR load can't starve the SDK during that window
  MeterSerial.begin(9600);  // meter via RS485
  resetParser();
  Serial.println("Starting loop");
}

#ifdef _mqtt_probe
// Open a raw TCP connection, send a hand-built minimal MQTT 3.1.1 CONNECT and
// hexdump whatever comes back. lwmqtt only reports "wrong packet type", this
// shows the actual bytes.
void probe_broker() {
  Serial.println();
  Serial.println("=== Broker probe ===");
  WiFiClient probe;
  if (!probe.connect(MQTT_BROKER, MQTT_PORT)) {
    Serial.println("TCP connect failed - nothing listening or firewalled");
    return;
  }
  Serial.println("TCP connected, sending CONNECT (client id 'ESP-Probe')");

  static const uint8_t connectPacket[] = {
    0x10, 0x15,                     // CONNECT, remaining length 21
    0x00, 0x04, 'M', 'Q', 'T', 'T', // protocol name
    0x04,                           // protocol level 4 = MQTT 3.1.1
    0x02,                           // flags: clean session, no user/pass
    0x00, 0x3C,                     // keep alive 60 s
    0x00, 0x09, 'E', 'S', 'P', '-', 'P', 'r', 'o', 'b', 'e'
  };
  probe.write(connectPacket, sizeof(connectPacket));
  probe.flush();

  // yielding wait - unlike the library's non-yielding delay(0)
  const uint32_t deadline = millis() + 3000;
  int n = 0;
  Serial.print("Reply hex: ");
  while ((int32_t)(millis() - deadline) < 0 && n < 32) {
    if (probe.available()) {
      uint8_t b = (uint8_t)probe.read();
      if (b < 0x10) Serial.print('0');
      Serial.print(b, HEX);
      Serial.print(' ');
      n++;
      continue;
    }
    if (!probe.connected()) break;
    delay(10);
  }
  if (n == 0) Serial.print("(nothing received)");
  Serial.println();
  Serial.print("Bytes received: ");
  Serial.println(n);
  Serial.print("Still connected: ");
  Serial.println(probe.connected());
  probe.stop();
  Serial.println("20 02 00 00 = CONNACK ok | 20 02 00 05 = not authorized");
  Serial.println("15 03 ..    = TLS alert -> port expects TLS");
  Serial.println("48 54 54 50 = 'HTTP' -> web server, not a broker");
  Serial.println("====================");
  Serial.println();
}
#endif

// bring the SML state machine back to "waiting for a start sequence"
void resetParser() {
  stage = 0;
  startIndex = 0;
  stopIndex = 0;
  smlIndex = 0;
  smlLength = 0;
}

// everything that arrived while we were blocked (reconnect, WiFi) is a torn
// telegram - drop it instead of parsing garbage
void flushMeter() {
  while (MeterSerial.available()) {
    MeterSerial.read();
  }
  resetParser();
}

void setup_wifi() {
  // connect to Wifi
  Serial.println("Connecting to WiFi...");
  WiFi.begin(SSID, PSK);

  while (WiFi.status() != WL_CONNECTED) {
    Serial.print(".");
    delay(100);
  }
  Serial.println(" connected!");
  Serial.println(WiFi.localIP());
}

void mqtt_reconnect() {

  // connect to broker again and connect start reconnecting mqtt client

  while (!client.isConnected()) {
    Serial.println("MQTT disconnected, trigger reconnect.");
    espClient.stop(); // release any previous socket/PCB before opening a new one

    // Never hand the MQTT layer a dead socket: its read loop busy-waits on
    // millis() and would spin until the timeout instead of failing fast.
    if (!espClient.connect(MQTT_BROKER, MQTT_PORT)) {
      Serial.print("TCP connect to broker failed, port ");
      Serial.println(MQTT_PORT);
      mqtt_backoff();
      continue;
    }
    client.begin(espClient);

    // generate new client ID
    String clientId = "ESP-Strom-";
    clientId += String(random(0xffff), HEX);
    if (client.connect(clientId.c_str(), mqtt_user, mqtt_pw)) {
      Serial.println("MQTT connected");
      client.publish(mqtt_debug_topic, "ESP-Strom: reconnected!");
    } else {
      Serial.println("MQTT connection failed");
      Serial.print("Return code: ");
      Serial.println(client.getReturnCode());
      Serial.print("Last error: ");
      Serial.println(client.getLastError()); // 0=OK -4=NET_TIMEOUT -5=FAILED_READ -9=WRONG_PACKET -10=DENIED
      Serial.print("TCP still connected: ");
      Serial.println(espClient.connected());
      Serial.print("Free heap: ");
      Serial.println(ESP.getFreeHeap());
      mqtt_backoff();
    }
  }
}

// wait ~5s but keep OTA responsive and the watchdog fed instead of
// blocking in one long call
void mqtt_backoff() {
  Serial.println("Try again in 5s");
  for (int i = 0; i < 50; i++) {
    ArduinoOTA.handle();
    delay(100);
  }
}

void loop() {
  client.update();
  ArduinoOTA.handle();

  switch (stage) {
    case 0:
      findStartSequence();
      break;
    case 1:
      findStopSequence();
      break;
    case 2:
      if (parseEfrTelegram()) {
        stage = 9;
      } else {
#ifdef _debug_msg
        Serial.println("Required EFR readings missing or have unexpected unit/scaler");
#endif
        resetParser();
      }
      break;
    case 9:
      publishMessage();
      break;
    default:
      resetParser();
      break;
  }
}

void debug_sml() {
  for (int x = 0; x < smlLength; ++x) {
    if (smlMessage[x] < 0x10) Serial.print('0');
    Serial.print(smlMessage[x], HEX);
    Serial.print(' ');
  }
  Serial.println();
  // Do not block for 30 seconds: SoftwareSerial would overrun meanwhile.
}

size_t advanceSequenceMatch(const byte* sequence, size_t sequenceLength,
                            size_t matched, byte nextByte) {
  const size_t observedLength = matched + 1;
  size_t candidate = observedLength < sequenceLength ? observedLength : sequenceLength;

  while (candidate > 0) {
    bool matches = true;
    const size_t observedStart = observedLength - candidate;
    for (size_t i = 0; i < candidate; ++i) {
      const size_t observedPosition = observedStart + i;
      const byte observed = observedPosition < matched ? sequence[observedPosition] : nextByte;
      if (observed != sequence[i]) {
        matches = false;
        break;
      }
    }
    if (matches) return candidate;
    --candidate;
  }
  return 0;
}

void findStartSequence() {
  while (MeterSerial.available()) {
    inByte = MeterSerial.read();

    startIndex = advanceSequenceMatch(startSequence, sizeof(startSequence), startIndex, inByte);
    if (startIndex == sizeof(startSequence)) {
        memcpy(smlMessage, startSequence, sizeof(startSequence));
#ifdef _debug_msg
        Serial.println("Match found - Start Sequence");
#endif
        stage = 1;
        smlIndex = sizeof(startSequence);
        startIndex = 0;
        stopIndex = 0;
        return;  // Do not discard payload bytes already waiting in the UART.
    }
  }
}

void findStopSequence() {
  while (MeterSerial.available()) {
    inByte = MeterSerial.read();

    if (smlIndex >= (int)sizeof(smlMessage)) {
#ifdef _debug_msg
      Serial.println("SML buffer overflow - resync");
#endif
      resetParser();
      return;
    }

    smlMessage[smlIndex++] = inByte;

    stopIndex = advanceSequenceMatch(stopSequence, sizeof(stopSequence), stopIndex, inByte);
    if (stopIndex == sizeof(stopSequence)) {
        smlLength = smlIndex;
        stage = 2;
        stopIndex = 0;
#ifdef _debug_sml
        debug_sml();
#endif
        return;
    }
  }
}

bool parseEfrTelegram() {
  SmlReading imported{};
  SmlReading exported{};
  SmlReading powerNow{};

  if (!smlFindReading(smlMessage, smlLength, 1, 8, 0, &imported) ||
      !smlFindReading(smlMessage, smlLength, 2, 8, 0, &exported) ||
      !smlFindReading(smlMessage, smlLength, 16, 7, 0, &powerNow)) {
    return false;
  }

  // EFR SGM-D4-A920N observations after InF On:
  // energy: unsigned, unit Wh (0x1E), scaler -1
  // power: signed, unit W (0x1B), scaler 0
  if (imported.isSigned || imported.unit != 0x1e || imported.scaler != -1 || imported.raw < 0 ||
      exported.isSigned || exported.unit != 0x1e || exported.scaler != -1 || exported.raw < 0 ||
      !powerNow.isSigned || powerNow.unit != 0x1b || powerNow.scaler != 0) {
    return false;
  }

  currentconsumption = static_cast<uint64_t>(imported.raw);  // raw unit: 0.1 Wh
  deliveredTotal = static_cast<uint64_t>(exported.raw);      // raw unit: 0.1 Wh
  currentpower = powerNow.raw;                               // unit: W; negative can mean export

  if (!firstRun && currentconsumption > oldconsumption + 10000ULL) {
    return false;
  }
  oldconsumption = currentconsumption;
  firstRun = false;
  return true;
}

void publishMessage() {
  client.update();
  doc.clear();

  // Keep legacy fields so existing Home Assistant consumers do not break.
  // Both energy legacy fields contain the raw meter count in 0.1 Wh.
  doc["consumedDeciWatt"] = currentconsumption;
  doc["deliveredkwh"] = deliveredTotal;
  doc["currentWatt"] = currentpower;  // no int16_t truncation

  // Correctly named/scaled fields for migration in Home Assistant.
  doc["energyImportKWh"] = static_cast<double>(currentconsumption) / 10000.0;
  doc["energyExportKWh"] = static_cast<double>(deliveredTotal) / 10000.0;

  doc["freeheap"] = ESP.getFreeHeap();
  doc["freeblocksize"] = ESP.getMaxFreeBlockSize();
  doc["heapfragmentation"] = ESP.getHeapFragmentation();

  if (doc.overflowed()) {
    Serial.println("MQTT JSON document overflow");
    resetParser();
    return;
  }

#ifdef _debug_msg
  serializeJsonPretty(doc, Serial);
  Serial.println();
#endif

  const size_t jsonLength = serializeJson(doc, mqttjson, sizeof(mqttjson));
  if (jsonLength == 0 || jsonLength >= sizeof(mqttjson) - 1) {
    Serial.println("MQTT JSON buffer too small");
    resetParser();
    return;
  }

  bool wasBlocked = false;
  if (WiFi.status() != WL_CONNECTED) {
    setup_wifi();
    wasBlocked = true;
  }

  bool published = client.publish(mqtt_topic, mqttjson);
  if (!published) {
    Serial.println("MQTT publish failed; reconnecting and retrying once...");
    mqtt_reconnect();
    wasBlocked = true;
    published = client.publish(mqtt_topic, mqttjson);
  }
  if (!published) {
    Serial.println("MQTT retry failed; dropping this sample and resynchronizing...");
    flushMeter();
    return;
  }

  memset(smlMessage, 0, sizeof(smlMessage));
  currentconsumption = 0;

  if (wasBlocked) {
    flushMeter();
  } else {
    resetParser();
  }
}