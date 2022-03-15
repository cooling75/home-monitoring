/*
  Read general gas meter consumption with a reed switch and pulldown ressitor
  based on Debounce example from Arduino IDE

  Jan Laudahn
  Version 1.1

  Each time the input pin goes from LOW to HIGH (e.g. because of a push-button
  press), the output pin is toggled from LOW to HIGH or HIGH to LOW. There's a
  minimum delay between toggles to debounce the circuit (i.e. to ignore noise).

  The circuit:
  - reed switch attached from pin 2 to +5V
  - 10 kilohm resistor attached from pin 2 to ground

  Part of this example code is in the public domain.

  https://www.arduino.cc/en/Tutorial/BuiltInExamples/Debounce
*/

#include <ESP8266WiFi.h>
#include <MQTTPubSubClient.h>
#include <WiFiManager.h>          //https://github.com/tzapu/WiFiManager WiFi Configuration Magic

// WiFi and MQTT
char mqtt_server[40];
char mqtt_port[6];
int mqtt_port_int;
char gas_value[11];
int gas_value_int;

String tmpStr;
char publishValue[20];

// constants won't change. They're used here to set pin numbers:
const int buttonPin = 4;    // reed switch

// Variables will change:
int ledState = HIGH;         // the current state of the output pin
int buttonState;             // the current reading from the input pin
int lastButtonState = LOW;   // the previous reading from the input pin

// the following variables are unsigned longs because the time, measured in
// milliseconds, will quickly become a bigger number than can be stored in an int.
unsigned long lastDebounceTime = 0;  // the last time the output pin was toggled
unsigned long debounceDelay = 50;    // the debounce time; increase if the output flickers
unsigned long gascount;

WiFiClient espClient;
WiFiManager wifiManager;
MQTTPubSubClient client;

// id/name, placeholder/prompt, default, length
WiFiManagerParameter custom_mqtt_server("server", "mqtt server", mqtt_server, 40);
WiFiManagerParameter custom_mqtt_port("port", "mqtt port", mqtt_port, 6);
WiFiManagerParameter custom_gas_value("gasvalue", "gas value", gas_value, 11);

void setup() {
  tmpStr.reserve(20);
  pinMode(buttonPin, INPUT);
  Serial.begin(115200);
  WiFi.setSleepMode(WIFI_NONE_SLEEP);
  WiFi.mode(WIFI_STA);
  wifiManager.setConfigPortalBlocking(true);
  wifiManager.setWiFiAutoReconnect(true);
  // keep wifi config portal online for 10 minutes
  wifiManager.setConfigPortalTimeout(300);

  wifiManager.addParameter(&custom_mqtt_server);
  wifiManager.addParameter(&custom_mqtt_port);
  wifiManager.addParameter(&custom_gas_value);

  //first parameter is name of access point, second is the password
  // wifiManager.autoConnect("ESP-AP-GAS");

  wifiManager.startConfigPortal("ESP-AP-GAS");

  // mqtt start
  strcpy(mqtt_server, custom_mqtt_server.getValue());
  strcpy(mqtt_port, custom_mqtt_port.getValue());
  strcpy(gas_value, custom_gas_value.getValue());


  // set MQTT and initial gas value
  mqtt_port_int = atoi(mqtt_port);
  gascount = atoi(gas_value);
  espClient.connect(mqtt_server, mqtt_port_int);
  client.begin(espClient);
  client.setCleanSession(true);
  client.connect("ESP-Gas");

  Serial.println("MQTT server: ");
  Serial.print(mqtt_server);
  Serial.println("");
  Serial.println("MQTT Port: ");
  Serial.print(mqtt_port);
  Serial.println("");
  Serial.println("Gas meter value: ");
  Serial.print(gas_value);
  Serial.println("");
}

void loop() {
  // update MQTT client connection
  client.update();
  delay(10);
  // read the state of the switch into a local variable:
  int reading = digitalRead(buttonPin);

  // check to see if you just pressed the button
  // (i.e. the input went from LOW to HIGH), and you've waited long enough
  // since the last press to ignore any noise:

  // If the switch changed, due to noise or pressing:
  if (reading != lastButtonState) {
    // reset the debouncing timer
    lastDebounceTime = millis();
  }

  if ((millis() - lastDebounceTime) > debounceDelay) {
    // whatever the reading is at, it's been there for longer than the debounce
    // delay, so take it as the actual current state:

    // if the button state has changed:
    if (reading != buttonState) {
      buttonState = reading;

      // only toggle the LED if the new button state is HIGH
      if (buttonState == HIGH) {
        //ledState = !ledState;
        gascount += 1;
        //MQTT publish
        Serial.println("Gas meter count: ");
        Serial.print(gascount);
        Serial.println("");
        if (client.publish("/home/commodity/gas/consumedcubicmhundredths", int2charArray(gascount))) {
          // if (client.publish("/home/commodity/gas/test", int2charArray(gascount))) {
          Serial.println("MQTT message published");
        } else {
          Serial.println("MQTT publish not successful!");
          mqtt_reconnect();
        }
      }
    }
  }

  // save the reading. Next time through the loop, it'll be the lastButtonState:
  lastButtonState = reading;
}

char* int2charArray(const unsigned long value) {
  tmpStr = "";
  tmpStr += value;
  tmpStr.toCharArray(publishValue, tmpStr.length() + 1);
  tmpStr = "";
  return publishValue;
}

void mqtt_reconnect() {
  client.disconnect();
  espClient.connect(mqtt_server, mqtt_port_int);
  while (!client.isConnected() && WiFi.status() == WL_CONNECTED) {
    Serial.println("MQTT disconnected, trigger reconnect.");
    // generate new client ID
    String clientId = "ESP-Gas-";
    clientId += String(random(0xffff), HEX);
    if (client.connect(clientId.c_str())) {
      Serial.println("Connected");
      client.publish("/home/debug/strom", "ESP-Gas: reconnected!");
    } else {
      Serial.println("MQTT connection failed");
      Serial.print("Status: ");
      Serial.println(client.getReturnCode());
      Serial.println("Try again in 1s");
      client.disconnect();
    }
    delay(1000);
  }
}