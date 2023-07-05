#include <SDM_Config_User.h>
#include <SDM.h>
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
const char* mqtt_topic = "/home/commodity/electricity";
const char* mqtt_debug_topic = "/home/debug/electricity";
const char* mqtt_user = "";
const char* mqtt_pw = "";

// variables
float currentPower;
float frequency;
float phase1voltage;
float phase2voltage;
float phase3voltage;
float phase1current;
float phase2current;
float phase3current;
float phase1power;
float phase2power;
float phase3power;
float importedkwh;
float exportedkwh;

// transmission
StaticJsonDocument<384> doc;
char mqttjson[384];

// SoftwareSerial
SoftwareSerial MeterSerial; //(4, 0, false); // RX, TX, inverted mode(!)

SDM sdm(MeterSerial, 9600, NOT_A_PIN, SWSERIAL_8N1, 0, 4); // order: (RX, TX)
WiFiClient espClient;
MQTTPubSub::PubSubClient<384> client;

// #define _debug

void setup() {
  // OTA setings
  ArduinoOTA.setHostname("");
  ArduinoOTA.setPassword("");
  ArduinoOTA.begin();

  // debug via USB
  Serial.begin(115200);
  sdm.begin();

  WiFi.setAutoConnect(true);
  WiFi.setSleepMode(WIFI_NONE_SLEEP);
  WiFi.mode(WIFI_STA);

  // static ip configuration
  IPAddress ip(192, 168, 0, 100);
  IPAddress dns(192, 168, 0, 1);
  IPAddress gateway(192, 168, 0, 1);
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

void loop() {
  ArduinoOTA.handle();
  currentPower = sdm.readVal(SDM_TOTAL_SYSTEM_POWER, 0x01);
  delay(50);
  frequency = sdm.readVal(SDM_FREQUENCY, 0x01);
  delay(50);
  phase1voltage = sdm.readVal(SDM_PHASE_1_VOLTAGE, 0x01);
  delay(50);
  phase2voltage = sdm.readVal(SDM_PHASE_2_VOLTAGE, 0x01);
  delay(50);
  phase3voltage = sdm.readVal(SDM_PHASE_3_VOLTAGE, 0x01);
  delay(50);
  phase1current = sdm.readVal(SDM_PHASE_1_CURRENT, 0x01);
  delay(50);
  phase2current = sdm.readVal(SDM_PHASE_2_CURRENT, 0x01);
  delay(50);
  phase3current = sdm.readVal(SDM_PHASE_3_CURRENT, 0x01);
  delay(50);
  phase1power = sdm.readVal(SDM_PHASE_1_POWER, 0x01);
  delay(50);
  phase2power = sdm.readVal(SDM_PHASE_2_POWER, 0x01);
  delay(50);
  phase3power = sdm.readVal(SDM_PHASE_3_POWER, 0x01);
  delay(50);
  importedkwh = sdm.readVal(SDM_IMPORT_ACTIVE_ENERGY, 0x01);
  delay(50);
  exportedkwh = sdm.readVal(SDM_EXPORT_ACTIVE_ENERGY, 0x01);
  delay(50);


  publishMessage();

#ifdef _debug
  Serial.println("---------------------------");
  Serial.print("Current power: ");
  Serial.println(currentPower);
  Serial.print("Frequency: ");
  Serial.println(frequency);
  Serial.println("Voltages:");
  Serial.println(phase1voltage);
  Serial.println(phase2voltage);
  Serial.println(phase3voltage);
  Serial.println("Currents:");
  Serial.println(phase1current);
  Serial.println(phase2current);
  Serial.println(phase3current);
  Serial.print("Imported kWh: ");
  Serial.println(importedkwh);
  Serial.print("Exported kWh: ");
  Serial.println(exportedkwh);
#endif

  delay(1000);
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

  while (!client.isConnected() && WiFi.status() == WL_CONNECTED) {
    Serial.println("MQTT disconnected, trigger reconnect.");
    espClient.connect(MQTT_BROKER, MQTT_PORT);
    client.begin(espClient);

    // generate new client ID
    String clientId = "ESP-Strom-";
    clientId += String(random(0xffff), HEX);
    if (client.connect(clientId.c_str())) {
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

void publishMessage() {
  // unrealistic values for currentpower
  //  if (currentpower < -15000) {
  //    clearbuffers();
  //    stage = 0;
  //    return;
  //  }

  doc["consumedDeciWatt"] = round2(importedkwh);
  doc["deliveredkwh"] = round2(exportedkwh);
  doc["currentWatt"] = round2(currentPower);
  doc["p1voltage"] = round2(phase1voltage);
  doc["p2voltage"] = round2(phase2voltage);
  doc["p3voltage"] = round2(phase3voltage);
  doc["p1current"] = round2(phase1current);
  doc["p2current"] = round2(phase2current);
  doc["p3current"] = round2(phase3current);
  doc["p1power"] = round2(phase1power);
  doc["p2power"] = round2(phase2power);
  doc["p3power"] = round2(phase3power);
  doc["frequency"] = round2(frequency);
  doc["freeheap"] = ESP.getFreeHeap();
  doc["freeblocksize"] = ESP.getMaxFreeBlockSize();
  doc["heapfragmentation"] = ESP.getHeapFragmentation();

  serializeJson(doc, mqttjson);

  Serial.println("###");
  Serial.println(mqttjson);

  // check WiFi connection before sending
  if (WiFi.status() != WL_CONNECTED) {
    setup_wifi();
  }

  // if publish wasn't successful, try to reconnect
  if (!client.publish( mqtt_topic, mqttjson )) {
    Serial.println("Problems with publish occured...");
    mqtt_reconnect();
  }
  //  clearbuffers();
}

double round2(double value) {
  return (int)(value * 100 + 0.5) / 100.0;
}