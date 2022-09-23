
/*
    Read a Logarex electricity meter via
    phototransistor + 1k resistor
    Version 1.0.0 - 23.09.2022
    2022  Jan Laudahn https://laudart.de

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
const short MQTT_PORT = 18883;
const char* mqtt_topic = "";
const char* mqtt_debug_topic = "";

/*
 * Scroll down and add IP values that re suitable to you
 */

// transmission
String tmpStr;
StaticJsonDocument<512> doc;
char mqttjson[512];

static uint16_t outputInterval = 2000;
static uint16_t uploadInterval = 2000;

uint32_t nextOutput = 0;
uint32_t nextUpload = 0;
uint8_t charsRead = 0;
char rxBuffer[20];
String rxID;
String mtrID = "UNKNOWN";
String mtrModel = "UNKNOWN";
double consSum = 0;
double delSum = 0;
float voltageL1 = 0;
float voltageL2 = 0;
float voltageL3 = 0;
float ampereL1 = 0;
float ampereL2 = 0;
float ampereL3 = 0;
float powerSum = 0;
float hertz = 0;
String mtrState = "UNK";

int pin_d2 = 4;

SoftwareSerial MeterSerial(pin_d2, 3, true); // RX, TX, inverted mode(!)
WiFiClient espClient;
MQTTPubSub::PubSubClient<512> client;

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
  /*
   * Add IP values that are suitable to your network
   */
  IPAddress ip(0, 0, 0, 0);
  IPAddress dns(0, 0, 0, 0);
  IPAddress gateway(0, 0, 0, 0);
  IPAddress subnet(0, 0, 0, 0);
  WiFi.config(ip, dns, gateway, subnet);

  setup_wifi();
  espClient.connect(MQTT_BROKER, MQTT_PORT);
  client.begin(espClient);
  client.setCleanSession(true);
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
  while (!client.isConnected()) {
    Serial.println("MQTT disconnected, trigger reconnect.");
    // generate new client ID
    String clientId = "ESP-Strom-";
    clientId += String(random(0xffff), HEX);
    if (client.connect(clientId.c_str(), "jan", "Start123!")) {
      Serial.println("MQTT connected");
      client.publish(mqtt_debug_topic, "ESP-Strom: reconnected!");
    } else {
      Serial.println("MQTT connection failed");
      Serial.print("Status: ");
      Serial.println(client.getReturnCode());
      Serial.println("Try again in 5s");
      delay(5000);
    }
  }
}

void parseValues() {
  if (rxID == "1-0:96.1.0*255") {
    mtrID = String(rxBuffer);
  }
  if (rxID == "1-0:1.8.0*255") {
    consSum = String(rxBuffer).toDouble();
  }
  if (rxID == "1-0:2.8.0*255") {
    delSum = String(rxBuffer).toDouble();
  }
  if (rxID == "1-0:32.7.0*255") {
    voltageL1 = String(rxBuffer).toFloat();
  }
  if (rxID == "1-0:52.7.0*255") {
    voltageL2 = String(rxBuffer).toFloat();
  }
  if (rxID == "1-0:72.7.0*255") {
    voltageL3 = String(rxBuffer).toFloat();
  }
  if (rxID == "1-0:31.7.0*255") {
    ampereL1 = String(rxBuffer).toFloat();
  }
  if (rxID == "1-0:51.7.0*255") {
    ampereL2 = String(rxBuffer).toFloat();
  }
  if (rxID == "1-0:71.7.0*255") {
    ampereL3 = String(rxBuffer).toFloat();
  }
  if (rxID == "1-0:16.7.0*255") {
    powerSum = String(rxBuffer).toFloat();
  }
  if (rxID == "1-0:96.90.2*255") {
    mtrState = String(rxBuffer);
  }
  if (rxID == "0-0:96.1.255*255") {
    mtrModel = String(rxBuffer);
  }
  if (rxID == "1-0:14.7.0*255") {
    hertz = String(rxBuffer).toFloat();
  }
}

void outputValues() {
  String output = "ID=";
  output += mtrID + ";Consumed: " + consSum + "Delivered: " + delSum + ";L1: " + voltageL1 + ampereL1 + " ;L2: " + voltageL2 + ampereL2 + " ;L3: " + voltageL3 + ampereL3 + " ;P: " + powerSum + "\n";
  Serial.println(output);
}

void loop() {
  client.update();

  if (MeterSerial.available()) {
    char c = MeterSerial.read();
    if (c == '(') {
      rxBuffer[charsRead] = '\0';
      rxID = String(rxBuffer);
      charsRead = 0;
    } else if (c == ')') {
      rxBuffer[charsRead] = '\0';
      if (charsRead > 1 && sizeof(rxID) > 1) {
        parseValues();
      }
      charsRead = 0;
    } else if (c == 0x0D || c == 0x0A) {
      // on CR or LF
      charsRead = 0;
    } else {
      rxBuffer[charsRead++] = c;
    }
    Serial.write(c);
  }

  if (nextOutput < millis()) {
    outputValues();
    nextOutput = ( millis() + outputInterval);
  }
  if (nextUpload < millis()) {
    publishMessage();
    nextUpload = ( millis() + uploadInterval);
  }
}

void publishMessage() {

  client.update();

  doc["consumedkwh"] = consSum;
  doc["deliveredkwh"] = delSum;
  doc["currentpower"] = powerSum;
  doc["voltageL1"] = voltageL1;
  doc["voltageL2"] = voltageL2;
  doc["voltageL3"] = voltageL3;
  doc["ampereL1"] = ampereL1;
  doc["ampereL2"] = ampereL2;
  doc["ampereL3"] = ampereL3;
  doc["hertz"] = hertz;
  doc["mtrID"] = mtrID;

#ifdef _debug_msg
  serializeJsonPretty(doc, Serial);
  Serial.println("");
#endif

  serializeJson(doc, mqttjson);

  // if publish wasn't successful, try to reconnect
  if (!client.publish(mqtt_topic, mqttjson )) {
    Serial.println("Problems with publish occured...");
    mqtt_reconnect();
  }

  // clear the buffers
  consSum = 0;
  delSum = 0;
  powerSum = 0;
  voltageL1 = 0;
  voltageL2 = 0;
  voltageL3 = 0;
  ampereL1 = 0;
  ampereL2 = 0;
  ampereL3 = 0;
  hertz = 0;
}