#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <EEPROM.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>

// --- Network & MQTT Settings ---
const char* ssid = "YOUR_WIFI_SSID";
const char* password = "YOUR_WIFI_PASSWORD";
const char* mqtt_server = "broker.emqx.io"; // Replace with your EMQX broker IP/URL
const int mqtt_port = 1883;

WiFiClient espClient;
PubSubClient client(espClient);

// --- Pin Definitions ---
#define FLOW1_PIN 27
#define FLOW2_PIN 32
#define TRIG1_PIN 18
#define ECHO1_PIN 34
#define TRIG2_PIN 19
#define ECHO2_PIN 35
#define VALVE1_PIN 25
#define VALVE2_PIN 26
#define PUMP_PIN 23
#define BUZZER_PIN 4

#define EEPROM_SIZE 8 
#define FULL_DISTANCE 15.0 

LiquidCrystal_I2C lcd(0x27, 20, 4);

// --- Variables ---
volatile int flow1Pulses = 0;
volatile int flow2Pulses = 0;
float flowRate1 = 0.0, flowRate2 = 0.0;
unsigned long lastTime = 0;
unsigned long lastMqttTime = 0;
float emptyDist1 = 100.0, emptyDist2 = 100.0; 
int percent1 = 0, percent2 = 0;
bool isPumping = false;

// --- Interrupts ---
void IRAM_ATTR pulseCounter1() { flow1Pulses++; }
void IRAM_ATTR pulseCounter2() { flow2Pulses++; }

void setup() {
  Serial.begin(115200);
  EEPROM.begin(EEPROM_SIZE);
  
  pinMode(TRIG1_PIN, OUTPUT); pinMode(ECHO1_PIN, INPUT);
  pinMode(TRIG2_PIN, OUTPUT); pinMode(ECHO2_PIN, INPUT);
  pinMode(VALVE1_PIN, OUTPUT); pinMode(VALVE2_PIN, OUTPUT);
  pinMode(PUMP_PIN, OUTPUT); pinMode(BUZZER_PIN, OUTPUT);
  pinMode(FLOW1_PIN, INPUT_PULLUP); pinMode(FLOW2_PIN, INPUT_PULLUP);
  
  attachInterrupt(digitalPinToInterrupt(FLOW1_PIN), pulseCounter1, FALLING);
  attachInterrupt(digitalPinToInterrupt(FLOW2_PIN), pulseCounter2, FALLING);

  lcd.init(); lcd.backlight();
  lcd.print("Connecting WiFi...");
  
  setup_wifi();
  client.setServer(mqtt_server, mqtt_port);
  client.setCallback(mqttCallback);

  EEPROM.get(0, emptyDist1);
  EEPROM.get(4, emptyDist2);
}

void setup_wifi() {
  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\nWiFi connected");
}

void reconnect() {
  while (!client.connected()) {
    if (client.connect("ESP32_WaterSystem")) {
      client.subscribe("water_system/commands");
    } else {
      delay(5000);
    }
  }
}

void mqttCallback(char* topic, byte* payload, unsigned int length) {
  String message;
  for (int i = 0; i < length; i++) {
    message += (char)payload[i];
  }
  
  // Parse incoming JSON command
  StaticJsonDocument<200> doc;
  DeserializationError error = deserializeJson(doc, message);
  if (!error) {
    const char* action = doc["action"];
    if (String(action) == "calibrate") {
      calibrateEmptyTanks();
    }
  }
}

void loop() {
  if (!client.connected()) reconnect();
  client.loop();

  unsigned long currentTime = millis();
  
  // 1. Read Sensors & Flow Rate (Every 1 Second)
  if (currentTime - lastTime >= 1000) {
    flowRate1 = (flow1Pulses / 7.5);
    flowRate2 = (flow2Pulses / 7.5);
    flow1Pulses = 0; flow2Pulses = 0;
    lastTime = currentTime;
    
    float dist1 = getDistance(TRIG1_PIN, ECHO1_PIN);
    float dist2 = getDistance(TRIG2_PIN, ECHO2_PIN);
    percent1 = mapDistanceToPercent(dist1, emptyDist1, FULL_DISTANCE);
    percent2 = mapDistanceToPercent(dist2, emptyDist2, FULL_DISTANCE);
    
    controlPumpAndValves(percent1, percent2);
    updateLCD(percent1, percent2, flowRate1, flowRate2);
  }

  // 2. Publish MQTT Telemetry (Every 2 Seconds to avoid spamming)
  if (currentTime - lastMqttTime >= 2000) {
    publishTelemetry();
    lastMqttTime = currentTime;
  }
}

void publishTelemetry() {
  StaticJsonDocument<256> doc;
  doc["tank1_percent"] = percent1;
  doc["tank2_percent"] = percent2;
  doc["flowrate1_Lmin"] = flowRate1;
  doc["flowrate2_Lmin"] = flowRate2;
  doc["pump_active"] = isPumping;

  char jsonBuffer[256];
  serializeJson(doc, jsonBuffer);
  client.publish("water_system/sensors", jsonBuffer);
}

// --- Helper Functions (Same logic as previous implementation) ---
float getDistance(int trigPin, int echoPin) {
  digitalWrite(trigPin, LOW); delayMicroseconds(2);
  digitalWrite(trigPin, HIGH); delayMicroseconds(10);
  digitalWrite(trigPin, LOW);
  return (pulseIn(echoPin, HIGH) * 0.0343) / 2.0; 
}

int mapDistanceToPercent(float currentDist, float emptyDist, float fullDist) {
  if (currentDist >= emptyDist) return 0;
  if (currentDist <= fullDist) return 100;
  return constrain(map(currentDist, emptyDist, fullDist, 0, 100), 0, 100);
}

void controlPumpAndValves(int p1, int p2) {
  digitalWrite(PUMP_PIN, LOW); digitalWrite(VALVE1_PIN, LOW); digitalWrite(VALVE2_PIN, LOW); digitalWrite(BUZZER_PIN, LOW);
  isPumping = false;

  if (p1 < 100 && p2 == 100) {
    digitalWrite(VALVE1_PIN, HIGH); delay(500); digitalWrite(PUMP_PIN, HIGH); isPumping = true;
  } else if (p2 < 100 && p1 == 100) {
    digitalWrite(VALVE2_PIN, HIGH); delay(500); digitalWrite(PUMP_PIN, HIGH); isPumping = true;
  } else if (p1 < 100 && p2 < 100) {
    digitalWrite(VALVE1_PIN, HIGH); delay(500); digitalWrite(PUMP_PIN, HIGH); isPumping = true;
  } else {
    digitalWrite(BUZZER_PIN, HIGH); delay(200); digitalWrite(BUZZER_PIN, LOW);
  }
}

void updateLCD(int p1, int p2, float f1, float f2) {
  lcd.setCursor(0, 0); lcd.print("T1: "); lcd.print(p1); lcd.print("%  ");
  lcd.setCursor(10, 0); lcd.print("F1: "); lcd.print(f1, 1); lcd.print("L/m");
  lcd.setCursor(0, 1); lcd.print("T2: "); lcd.print(p2); lcd.print("%  ");
  lcd.setCursor(10, 1); lcd.print("F2: "); lcd.print(f2, 1); lcd.print("L/m");
  lcd.setCursor(0, 2);
  if (isPumping) lcd.print("Status: PUMPING ");
  else lcd.print("Status: IDLE    ");
}

void calibrateEmptyTanks() {
  lcd.clear(); lcd.print("Calibrating...");
  emptyDist1 = getDistance(TRIG1_PIN, ECHO1_PIN);
  emptyDist2 = getDistance(TRIG2_PIN, ECHO2_PIN);
  EEPROM.put(0, emptyDist1); EEPROM.put(4, emptyDist2); EEPROM.commit();
  lcd.clear(); lcd.print("Saved!"); delay(1000); lcd.clear();
}
