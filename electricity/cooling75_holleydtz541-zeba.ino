
/*
    Application note: Read a Holley DTZ541-ZEBA (2021) electricity meter via
    phototransistor + 1k resistor interface and SML protocol
    Version 1.13 - 18.05.2022
    Copyright (C) 2022  Jan Laudahn https://laudart.de

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

// WiFi and MQTT
const char* SSID = "";
const char* PSK = "";
const char* MQTT_BROKER = "";
const short MQTT_PORT = ;

// transmission
String tmpStr;
//char publishValue[20];
//bool pubsuccess;
StaticJsonDocument<200> doc;
char mqttjson[256];


// DATA
byte inByte; //byte to store the serial buffer
byte smlMessage[1000]; //byte to store the parsed message
const byte startSequence[] = { 0x1B, 0x1B, 0x1B, 0x1B, 0x01, 0x01, 0x01, 0x01 }; //start sequence of SML protocol
const byte stopSequence[]  = { 0x1B, 0x1B, 0x1B, 0x1B, 0x1A }; //end sequence of SML protocol
const byte powerSequence[] =       { 0x77, 0x07, 0x01, 0x00, 0x10, 0x07, 0x00, 0xFF }; //sequence preceeding the current "Real Power" value (2 Bytes)
const byte consumptionSequence[] = { 0x77, 0x07, 0x01, 0x00, 0x01, 0x08, 0x00, 0xFF }; //sequence predeecing the current "Total power consumption" value (4 Bytes)
const byte deliveredSequence[] = { 0x77, 0x07, 0x01, 0x00, 0x02, 0x08, 0x00, 0xFF }; // sequence preceeding the delivered to grid power 15 byte to 1 byte (?)
const byte vendorSequence[] = { 0x77, 0x07, 0x01, 0x00, 0x60, 0x32, 0x01, 0x01 }; // sequence preceeding the vendor shortname, 6 bytes to 3 byte in ASCII, here "HLY"
const byte uptimeSequence[] = { 0x07, 0x01, 0x00, 0x62, 0x0A, 0xFF, 0xFF }; // 5 bytes to 4 byte uptime
bool foundSequence;
int smlIndex; //index counter within smlMessage array
int startIndex; //start index for start sequence search
int stopIndex; //start index for stop sequence search
int stage; //index to maneuver through cases
int powerbytes; //number of bytes containing power sequence
int deliverbytes; //number of bytes containing delivered sequence
byte power[8]; //array that holds the extracted 4 byte "Wirkleistung" value
byte consumption[8]; //array that holds the extracted 4 byte "Gesamtverbrauch" value
byte delivered[8];
byte uptime[8];
unsigned long uptimeTotal;
unsigned long deliveredTotal;
signed short currentpower; //variable to hold translated "Wirkleistung" value
unsigned long currentconsumption; //variable to hold translated "Gesamtverbrauch" value

// pins and logic
int pin_d2 = 4;
short failcount;

SoftwareSerial MeterSerial(pin_d2, 3, true); // RX, TX, inverted mode(!)
WiFiClient espClient;
MQTTPubSub::PubSubClient<256> client;

// #define _debug_msg
// #define _debug_sml

void setup() {

  tmpStr.reserve(20);
  // bring up serial ports
  Serial.begin(115200);   // debug via USB
  MeterSerial.begin(9600);  // meter via RS485
  WiFi.setAutoConnect(true);
  WiFi.setSleepMode(WIFI_NONE_SLEEP);
  WiFi.mode(WIFI_STA);

  // static ip configuration
  IPAddress ip(192, 168, 178, 231);
  IPAddress dns(192, 168, 178, 151);
  IPAddress gateway(192, 168, 178, 1);
  IPAddress subnet(255, 255, 255, 0);
  WiFi.config(ip, dns, gateway, subnet);

  setup_wifi();
  espClient.connect(MQTT_BROKER, MQTT_PORT);
  client.begin(espClient);
  client.setCleanSession(true);
  client.setKeepAliveTimeout(10);
  client.setTimeout(1000);
  mqtt_reconnect();
  Serial.println("Starting loop");
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
  client.disconnect();
  // connect to broker again and connect start reconnecting mqtt client
  espClient.connect(MQTT_BROKER, MQTT_PORT);
  while (!client.isConnected() && WiFi.status() == WL_CONNECTED) {
    Serial.println("MQTT disconnected, trigger reconnect.");
    // generate new client ID
    String clientId = "ESP-Strom-";
    clientId += String(random(0xffff), HEX);
    if (client.connect(clientId.c_str())) {
      Serial.println("MQTT connected");
      client.publish("/home/debug/electricity", "ESP-Strom: reconnected!");
    } else {
      Serial.println("MQTT connection failed");
      Serial.print("Status: ");
      Serial.println(client.getReturnCode());
      Serial.println("Try again in 5s");
      failcount++;
      if (failcount > 12) {
        ESP.restart();
      }
      delay(5000);
    }
  }
}

void loop() {
  client.update();
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
    case 5:
      findUptime(); // look for uptime of power meter
      break;
    case 6:
      publishMessage(); // send out via MQTT
      break;
  }
}

void debug_sml() {
  for (int x = 0; x < sizeof(smlMessage); x++) {
    Serial.print(smlMessage[x], HEX);
    Serial.print(" ");
  }
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
    // yield();

    inByte = MeterSerial.read();
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
        stage = 2;
        stopIndex = 0;
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
  byte temp; //temp variable to store loop search data
  startIndex = 0; //start at position 0 of exctracted SML message
  for (int x = 0; x < sizeof(smlMessage); x++) { //for as long there are element in the exctracted SML message
    temp = smlMessage[x]; //set temp variable to 0,1,2 element in extracted SML message
    if (temp == powerSequence[startIndex]) //compare with power sequence
    {
      startIndex++;
      if (startIndex == sizeof(powerSequence)) //in complete sequence is found
      {
        // find number of bytes for power sequence since this is dynamically
        powerbytes = (smlMessage[x + 7] & 0x0F);
        for (int y = 0; y < powerbytes; y++) { //read the next byte(s) (the actual power value)
          power[y] = smlMessage[x + y + 8]; //store into power array
        }
        stage = 3; // go to stage 3
        startIndex = 0;
        foundSequence = true;
      }
    }
    else {
      startIndex = 0;
    }
  }
  // write var only when powerSequence has been found
  if (foundSequence) {
    if (powerbytes == 3) {
      currentpower = (power[0] << 8 | power[1] << 0); //merge 2 bytes into single variable to calculate power value
    } else if (powerbytes == 2 && power[0] < 128) {
      currentpower = power[0];
    } else {
      currentpower = (power[0] << 8) | ((byte) 00000000);
      currentpower >>= 8;
    }
  } else {
    stage = 0; // start over when sequence not found
  }
}

void findConsumptionSequence() {
  foundSequence = false;
  byte temp;
  startIndex = 0;
  for (int x = 0; x < sizeof(smlMessage); x++) {
    temp = smlMessage[x];
    if (temp == consumptionSequence[startIndex])
    {
      startIndex++;
      if (startIndex == sizeof(consumptionSequence))
      {
        for (int y = 0; y < 4; y++) {
          // get 4 bytes containing the total consumption
          consumption[y] = smlMessage[x + y + 19];
#ifdef _debug_msg
          Serial.print(String(consumption[y], HEX));
          Serial.print(" ");
#endif
        }
#ifdef _debug_msg
        Serial.println();
#endif
        stage = 4;
        startIndex = 0;
        foundSequence = true;
      }
    }
    else {
      startIndex = 0;
    }
  }

  if (foundSequence) {
    currentconsumption = consumption[0];
    currentconsumption <<= 8;
    currentconsumption += consumption[1];
    currentconsumption <<= 8;
    currentconsumption += consumption[2];
    currentconsumption <<= 8;
    currentconsumption += consumption[3];
  } else {
    stage = 0; // start over when sequence not found
  }
}

void findDeliveredSequence() {
  foundSequence = false;
  byte temp;
  deliveredTotal = 0;
  startIndex = 0;
  for (int x = 0; x < sizeof(smlMessage); x++) {
    temp = smlMessage[x];
    if (temp == deliveredSequence[startIndex]) {
      startIndex++;
      if (startIndex == sizeof(deliveredSequence)) {
        deliverbytes = (smlMessage[x + 14] & 0x0F);
        for (int y = 0; y < deliverbytes - 1; y++) {
          delivered[y] = smlMessage[x + y + 15];
        }
        startIndex = 0;
        stage = 5;
        foundSequence = true;
      }
    } else {
      startIndex = 0;
    }
  }

  if (foundSequence) {
    // value deliverbytes extends when power is delivered to grid!
    for (int i = 0; i < deliverbytes - 1; i++) {
      deliveredTotal += delivered[i];
      if (i < deliverbytes - 2) {
        deliveredTotal <<= 8;
      }
    }
  } else {
    stage = 0; // start over when sequence not found
  }
}

void findUptime() {
  byte temp;
  foundSequence = false;
  startIndex = 0;
  for (int x = 0; x < sizeof(smlMessage); x++) {
    temp = smlMessage[x];
    if (temp == uptimeSequence[startIndex]) {
      startIndex++;
      if (startIndex == sizeof(uptimeSequence)) {
#ifdef _debug_msg
        Serial.print("Uptime (bytes): ");
#endif
        for (int y = 0; y < 4; y++) {
          uptime[y] = smlMessage[x + y + 5];
#ifdef _debug_msg
          Serial.print(String(uptime[y], HEX));
          Serial.print(" ");
#endif
        }
        startIndex = 0;
        stage = 6;
        foundSequence = true;
      }
    } else {
      startIndex = 0;
    }
  }
  if (foundSequence) {
    // combine to uptimeTotal
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

void publishMessage() {
#ifdef _debug_sml
  debug_sml();
#endif


  // check WiFi connection
  //  if (WiFi.status() != WL_CONNECTED || !client.isConnected()) {
  //    Serial.println("WLAN disconnected, trigger reconnect.");
  //    WiFi.disconnect();
  //    setup_wifi();
  //    // when WiFi was reconnected, reconnect MQTT too
  //    mqtt_reconnect();
  //  }

  // client.update();

  // debug output
  //  Serial.print("getFreeHeap: ");
  //  Serial.println(ESP.getFreeHeap());
  //  Serial.print("getHeapFragmentation: ");
  //  Serial.println(ESP.getHeapFragmentation());
  //  Serial.print("getMaxFreeBlockSize: ");
  //  Serial.println(ESP.getMaxFreeBlockSize());
  //  Serial.print("Total consumed: ");
  //  Serial.println(currentconsumption);
  //  Serial.print("Total delivered: ");
  //  Serial.println(deliveredTotal);
  //  Serial.print("Current power (short): ");
  //  Serial.println((signed short)currentpower);

  doc["consumedDeciWatt"] = currentconsumption;
  doc["deliveredkwh"] = deliveredTotal;
  doc["currentWatt"] = (signed short)currentpower;
  doc["uptime"] = uptimeTotal;
  doc["freeheap"] = ESP.getFreeHeap();
  doc["freeblocksize"] = ESP.getMaxFreeBlockSize();
  doc["heapfragmentation"] = ESP.getHeapFragmentation();

  serializeJson(doc, mqttjson);

  // if publish wasn't successful, try to reconnect
  if (!client.publish("/home/commodity/electricity", mqttjson )) {
    Serial.println("Problems with publish occured...");
    mqtt_reconnect();
  }

  // clear the buffers
  memset(smlMessage, 0, sizeof(smlMessage));
  memset(power, 0, sizeof(power));
  memset(consumption, 0, sizeof(consumption));
  memset(delivered, 0, sizeof(delivered));
  memset(uptime, 0, sizeof(uptime));
  //reset case
  smlIndex = 0;
  stage = 0; // start over
}