
/*
    Application note: Read a KAIFA MB310H4BDE2 electricity meter via
    phototransistor + 1k resistor interface and SML protocol
    Version 2.0 - 03.09.2026
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

#include <ESP8266WiFi.h>
#include <SoftwareSerial.h>
#include <MQTTPubSubClient.h>
#include <ArduinoJson.h>
#include <ArduinoOTA.h>
#include "secrets.h"

// WiFi and MQTT
const char* SSID = SECRET_SSID;
const char* PSK = SECRET_PSK;
const char* MQTT_BROKER = SECRET_MQTT_BROKER;
const short MQTT_PORT = 1883;
const char* mqtt_topic = SECRET_MQTT_TOPIC;
const char* mqtt_debug_topic = SECRET_MQTT_DEBUG_TOPIC;
const char* mqtt_user = SECRET_MQTT_USER;
const char* mqtt_pw = SECRET_MQTT_PW;

// transmission
String tmpStr;
StaticJsonDocument<256> doc;
char mqttjson[256];

// DATA
byte inByte; //byte to store the serial buffer
byte smlMessage[1000]; //byte to store the parsed message
const byte startSequence[] = { 0x1B, 0x1B, 0x1B, 0x1B, 0x01, 0x01, 0x01, 0x01 }; //start sequence of SML protocol
const byte stopSequence[]  = { 0x1B, 0x1B, 0x1B, 0x1B, 0x1A }; //end sequence of SML protocol
const byte powerSequence[] =  { 0x77, 0x07, 0x01, 0x00, 0x10, 0x07, 0x00, 0xFF }; //sequence preceeding the current "Real Power" value (2 Bytes)
// Voltage
const byte voltagePhase1[] = {0x77, 0x07, 0x01, 0x00, 0x20, 0x07, 0x00, 0xFF }; // 7 bytes to length nibble 8 to first value byte
const byte voltagePhase2[] = {0x77, 0x07, 0x01, 0x00, 0x34, 0x07, 0x00, 0xFF };
const byte voltagePhase3[] = {0x77, 0x07, 0x01, 0x00, 0x48, 0x07, 0x00, 0xFF };
// Current
const byte currentPhase1[] = {0x77, 0x07, 0x01, 0x00, 0x1F, 0x07, 0x00, 0xFF }; // 7 bytes to length nibble 8 to first value byte
const byte currentPhase2[] = {0x77, 0x07, 0x01, 0x00, 0x33, 0x07, 0x00, 0xFF };
const byte currentPhase3[] = {0x77, 0x07, 0x01, 0x00, 0x47, 0x07, 0x00, 0xFF };
const byte consumptionSequence[] = { 0x77, 0x07, 0x01, 0x00, 0x01, 0x08, 0x00, 0xFF }; //sequence predeecing the current "Total power consumption" value (4 Bytes)
const byte deliveredSequence[] = { 0x77, 0x07, 0x01, 0x00, 0x02, 0x08, 0x00, 0xFF }; // sequence preceeding the delivered to grid power 15 byte to 1 byte (?)
const byte vendorSequence[] = { 0x77, 0x07, 0x01, 0x00, 0x60, 0x32, 0x01, 0x01 }; // sequence preceeding the vendor shortname, 6 bytes to 3 byte in ASCII, here "HLY"
const byte uptimeSequence[] = {0x77, 0x01, 0x0B, 0x09, 0x01, 0x45, 0x53, 0x59, 0x11, 0x03, 0x9C, 0x7B, 0xB6 }; // 12 bytes to 4 byte uptime
bool foundSequence;
bool firstRun = true;
int smlIndex; //index counter within smlMessage array
int smlLength; //number of valid bytes in smlMessage after a complete telegram
int startIndex; //start index for start sequence search
int stopIndex; //start index for stop sequence search
int stage; //index to maneuver through cases
short powerbytes; //number of bytes containing power sequence
short phase1bytes;
short phase2bytes;
short phase3bytes;
int deliverbytes; //number of bytes containing delivered sequence
int consumedbytes;
byte power[8]; //array that holds the extracted 4 byte "Wirkleistung" value
byte consumption[8]; //array that holds the extracted 4 byte "Gesamtverbrauch" value
byte delivered[8];
byte uptime[8];
unsigned long uptimeTotal;
uint64_t deliveredTotal;
// in case of deliver to grid use signed
signed long currentpower; //variable to hold translated "Wirkleistung" value
int64_t phase1power;
int64_t phase2power;
int64_t phase3power;
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
  ArduinoOTA.setPassword(SECRET_OTA_PASSWORD);

  tmpStr.reserve(20);
  // bring up serial ports
  Serial.begin(115200);   // debug via USB
  WiFi.setAutoConnect(true);
  WiFi.setSleepMode(WIFI_NONE_SLEEP);
  WiFi.mode(WIFI_STA);

  // static ip configuration
  IPAddress ip(192, 168, 178, 231);
  IPAddress dns(192, 168, 178, 1);
  IPAddress gateway(192, 168, 178, 1);
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
      findStartSequence(); // look for start sequence
      break;
    case 1:
      findStopSequence(); // look for stop sequence
      break;
    case 2:
      findPowerSequence(); //look for power sequence and extract
      break;
    case 3:
      findConsumptionSequence(); //look for consumption sequence and exctract
      break;
    case 4:
      findDeliveredSequence(); // look for power send to grid
      break;
    //    case 5:
    //      findUptime(); // look for uptime of power meter
    //      break;
    //    case 6:
    //      findPhase1Power();
    //      break;
    //    case 7:
    //      findPhase2Power();
    //      break;
    //    case 8:
    //      findPhase3Power();
    //      break;
    case 9:
      publishMessage(); // send out via MQTT
      break;
  }
}

void debug_sml() {
  for (int x = 0; x < smlLength; x++) {
    Serial.print(smlMessage[x], HEX);
    Serial.print(" ");
  }
  Serial.println("");
  // show complete sml message every 20 seconds
  delay(20000);
}

void findStartSequence() {
  while (MeterSerial.available())
  {
    // yield();

    inByte = MeterSerial.read(); //read serial buffer into array

    if (inByte == startSequence[startIndex]) //in case byte in array matches the start sequence at position 0,1,2...
    {
      smlMessage[startIndex] = inByte; //set smlMessage element at position 0,1,2 to inByte value
      startIndex++;
      if (startIndex == sizeof(startSequence)) //all start sequence values have been identified
      {
#ifdef _debug_msg
        Serial.println("Match found - Start Sequence");
#endif
        stage = 1; //go to next case
        smlIndex = startIndex; //set start index to last position to avoid rerunning the first numbers in end sequence search
        startIndex = 0;
      }
    }
    else {
      startIndex = 0; // set index  back to 0 for startover
    }
    // client.update();
  }
}

void findStopSequence() {
  while (MeterSerial.available())
  {
    inByte = MeterSerial.read();

    // no stop sequence within one buffer length: lost bytes or line noise.
    // Without this check smlIndex runs past smlMessage[] and corrupts the
    // globals/heap behind it - the crash then happens anywhere, often inside
    // the WiFi/MQTT stack.
    if (smlIndex >= (int)sizeof(smlMessage)) {
#ifdef _debug_msg
      Serial.println("SML buffer overflow - resync");
#endif
      resetParser();
      return;
    }

    smlMessage[smlIndex] = inByte;
    smlIndex++;

    if (inByte == stopSequence[stopIndex])
    {
      stopIndex++;
      if (stopIndex == sizeof(stopSequence))
      {
#ifdef _debug_msg
        Serial.println("Match found - Stop Sequence");
#endif
        smlLength = smlIndex; // only these bytes are valid telegram data
        stage = 2;
        stopIndex = 0;
#ifdef _debug_sml
        stage = 0;
        debug_sml();
#endif
        return; // leave the rest in the UART buffer, extract first
      }
    }
    else {
      stopIndex = 0;
    }
    // client.update();
  }
}

void findPowerSequence() {
  foundSequence = false;
  currentpower = 0; //reset accumulator, otherwise it keeps building on the previous cycle's value
  byte temp; //temp variable to store loop search data
  startIndex = 0; //start at position 0 of exctracted SML message
  for (int x = 0; x < smlLength; x++) { //only search the bytes of the current telegram
    temp = smlMessage[x]; //set temp variable to 0,1,2 element in extracted SML message
    if (temp == powerSequence[startIndex]) //compare with power sequence
    {
      startIndex++;
      if (startIndex == sizeof(powerSequence)) //in complete sequence is found
      {
        startIndex = 0;
        // find number of bytes for power sequence since this is dynamically.
        // The TL byte counts itself, so len-1 value bytes follow it.
        const int tl = x + 7;
        if (tl >= smlLength) {
          continue;
        }
        const int len = (smlMessage[tl] & 0x0F);
        if (len < 2 || (len - 1) > (int)sizeof(power) || (tl + len) > smlLength) {
          continue; // implausible length - keep searching instead of overflowing power[]
        }
        for (int y = 0; y < len - 1; y++) { //read the next byte(s) (the actual power value)
          power[y] = smlMessage[tl + 1 + y]; //store into power array
        }
        powerbytes = len;
        foundSequence = true;
        break;
      }
    }
    else {
      startIndex = 0;
    }
  }

  // write to final variable
  if (foundSequence) {
    for (int j = 0; j < powerbytes - 1; j++) {
      currentpower += power[j];
      if (j < powerbytes - 2) {
        currentpower <<= 8;
      }
    }
    stage = 3; // go to stage 3
  } else {
    resetParser(); // start over when sequence not found
  }
  memset(power, 0, sizeof(power));
}

void findConsumptionSequence() {
  foundSequence = false;
  currentconsumption = 0; //reset accumulator, it is only cleared on the happy path otherwise
  byte temp;
  startIndex = 0;
  for (int x = 0; x < smlLength; x++) {
    temp = smlMessage[x];
    if (temp == consumptionSequence[startIndex])
    {
      startIndex++;
      if (startIndex == sizeof(consumptionSequence)) {
        startIndex = 0;
        //consumedbytes contains no of bytes for the value, e.g. SML value: x69 => 9 will be extracted
        const int tl = x + 11;
        if (tl >= smlLength) {
          continue;
        }
        const int len = (smlMessage[tl] & 0x0F);
        if (len < 2 || (len - 1) > (int)sizeof(consumption) || (tl + len) > smlLength) {
          continue; // implausible length - would overflow consumption[]
        }
        consumedbytes = len;
#ifdef _debug_msg
        Serial.println("Match found - Consumption Sequence:");
#endif
        for (int y = 0; y < len - 1; y++) {
          consumption[y] = smlMessage[tl + 1 + y];
#ifdef _debug_msg
          Serial.print(String(consumption[y], HEX));
          Serial.print(" ");
#endif
        }
#ifdef _debug_msg
        Serial.println();
#endif
        foundSequence = true;
        break;
      }
    }
    else {
      startIndex = 0;
    }
  }

  // write to final variable
  if (!foundSequence) {
    resetParser(); // start over when sequence not found
    return;
  }
  for (int j = 0; j < consumedbytes - 1; j++) {
    currentconsumption += consumption[j];
    if (j < consumedbytes - 2) {
      currentconsumption <<= 8;
    }
  }

  // add check for feasible value and reset if true
  // During first run the value for comparing will be set, afterwards smaller values are allowed, but not extensively higher values
  if (!firstRun && currentconsumption > oldconsumption + 10000) {
    resetParser(); // implausible jump - discard this telegram
    return;
  }
  // remember the accepted reading, otherwise the check above locks up forever
  oldconsumption = currentconsumption;
  firstRun = false;
  stage = 4;
}

void findDeliveredSequence() {
  foundSequence = false;
  byte temp;
  deliveredTotal = 0;
  startIndex = 0;
  for (int x = 0; x < smlLength; x++) {
    temp = smlMessage[x];
    if (temp == deliveredSequence[startIndex]) {
      startIndex++;
      if (startIndex == sizeof(deliveredSequence)) {
        startIndex = 0;
        const int tl = x + 7;
        if (tl >= smlLength) {
          continue;
        }
        const int len = (smlMessage[tl] & 0x0F);
        if (len < 2 || (len - 1) > (int)sizeof(delivered) || (tl + len) > smlLength) {
          continue; // implausible length - would overflow delivered[]
        }
        deliverbytes = len;
#ifdef _debug_msg
        Serial.println("Match found - Delivered Sequence:");
#endif
        for (int y = 0; y < len - 1; y++) {
          delivered[y] = smlMessage[tl + 1 + y];
#ifdef _debug_msg
          Serial.print(String(delivered[y], HEX));
          Serial.print(" ");
#endif
        }
#ifdef _debug_msg
        Serial.println();
#endif
        foundSequence = true;
        break;
      }
    } else {
      startIndex = 0;
    }
  }
  // value deliverbytes extends when power is delivered to grid!
  if (foundSequence) {
    for (int i = 0; i < deliverbytes - 1; i++) {
      deliveredTotal += delivered[i];
      if (i < deliverbytes - 2) {
        deliveredTotal <<= 8;
      }
    }
    stage = 9;
  } else {
    resetParser(); // start over when sequence not found
  }
}

void findUptime() {
  foundSequence = false;
  byte temp;
  startIndex = 0;
  for (int x = 0; x < smlLength; x++) {
    temp = smlMessage[x];
    if (temp == uptimeSequence[startIndex]) {
      startIndex++;
      if (startIndex == sizeof(uptimeSequence)) {
        if (x + 15 >= smlLength) {
          startIndex = 0;
          continue;
        }
#ifdef _debug_msg
        Serial.println("Match found - Uptime Sequence:");
#endif
        for (int y = 0; y < 4; y++) {
          uptime[y] = smlMessage[x + y + 12];
#ifdef _debug_msg
          Serial.print(String(uptime[y], HEX));
          Serial.print(" ");
#endif
        }
#ifdef _debug_msg
        Serial.println();
#endif
        startIndex = 0;
        stage = 6;
        foundSequence = true;
      }
    } else {
      startIndex = 0;
    }
  }
  // combine to uptimeTotal
  if (foundSequence) {
    uptimeTotal = uptime[0];
    uptimeTotal <<= 8;
    uptimeTotal += uptime[1];
    uptimeTotal <<= 8;
    uptimeTotal += uptime[2];
    uptimeTotal <<= 8;
    uptimeTotal += uptime[3];
  } else {
    stage = 0; // start over when sequence not found
  }
}

//void findPhase1Power() {
//  foundSequence = false;
//  byte temp; //temp variable to store loop search data
//  startIndex = 0; //start at position 0 of exctracted SML message
//  for (int x = 0; x < sizeof(smlMessage); x++) { //for as long there are element in the exctracted SML message
//    temp = smlMessage[x]; //set temp variable to 0,1,2 element in extracted SML message
//    if (temp == phase1[startIndex]) //compare with power sequence
//    {
//      startIndex++;
//      if (startIndex == sizeof(phase1)) //in complete sequence is found
//      {
//        // find number of bytes for power sequence since this is dynamically
//        phase1bytes = (smlMessage[x + 7] & 0x0F);
//        for (int y = 0; y < phase1bytes; y++) { //read the next byte(s) (the actual power value)
//          power[y] = smlMessage[x + y + 8]; //store into power array
//        }
//        stage = 7; // go to stage 7
//        foundSequence = true;
//        startIndex = 0;
//      }
//    }
//    else {
//      startIndex = 0;
//    }
//  }
//  // write to final variable
//  if (foundSequence) {
//    for (int j = 0; j < phase1bytes - 1; j++) {
//      phase1power += power[j];
//      if (j < phase1bytes - 2) {
//        phase1power <<= 8;
//      }
//    }
//  } else {
//    stage = 0; // start over when sequence not found
//  }
//  memset(power, 0, sizeof(power));
//}

void publishMessage() {

  client.update();

  // check if values are feasible
  //  if (phase1power > 1000000 || phase2power > 1000000 || phase3power > 1000000) {
  //    Serial.println("Invalid values... skipping this publish.");
  //  } else {

  //    doc["uptime"] = uptimeTotal;
  //    doc["phase1"] = (signed int64_t)phase1power;
  //    doc["phase2"] = (signed int64_t)phase2power;
  //    doc["phase3"] = (signed int64_t)phase3power;
  doc["consumedDeciWatt"] = currentconsumption;
  doc["deliveredkwh"] = deliveredTotal;
  doc["currentWatt"] = (signed short)currentpower;
  doc["freeheap"] = ESP.getFreeHeap();
  doc["freeblocksize"] = ESP.getMaxFreeBlockSize();
  doc["heapfragmentation"] = ESP.getHeapFragmentation();

#ifdef _debug_msg
  serializeJsonPretty(doc, Serial);
  Serial.println("");
#endif

  serializeJson(doc, mqttjson);

  bool wasBlocked = false;

  // check WiFi connection before sending
  if (WiFi.status() != WL_CONNECTED) {
    setup_wifi();
    wasBlocked = true;
  }

  // if publish wasn't successful, try to reconnect
  if (!client.publish(mqtt_topic, mqttjson )) {
    Serial.println("Problems with publish occured...");
    mqtt_reconnect();
    wasBlocked = true;
  }
  //}
  // clear the buffers
  memset(smlMessage, 0, sizeof(smlMessage));
  memset(power, 0, sizeof(power));
  memset(consumption, 0, sizeof(consumption));
  memset(delivered, 0, sizeof(delivered));
  memset(uptime, 0, sizeof(uptime));
  phase1power = 0;
  phase2power = 0;
  phase3power = 0;
  currentconsumption = 0;
  //reset case
  if (wasBlocked) {
    // reconnecting takes seconds - the 64 byte UART buffer overflowed meanwhile,
    // so the pending bytes are a torn telegram
    flushMeter();
  } else {
    resetParser();
  }
}
