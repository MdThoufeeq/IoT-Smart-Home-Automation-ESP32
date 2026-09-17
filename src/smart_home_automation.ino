#include <WiFi.h>
#include "Adafruit_MQTT.h"
#include "Adafruit_MQTT_Client.h"

// ---------- WiFi ----------
#define WIFI_SSID    "YOUR_WIFI_SSID"
#define WIFI_PASS    "YOUR_WIFI_PASSWORD"

// ---------- Adafruit IO ----------
#define AIO_SERVER      "io.adafruit.com"
#define AIO_SERVERPORT  1883
#define IO_USERNAME  "YOUR_ADAFRUIT_IO_USERNAME"
#define IO_KEY       "YOUR_ADAFRUIT_IO_KEY"

// Create WiFi + MQTT client
WiFiClient client;
Adafruit_MQTT_Client mqtt(&client, AIO_SERVER, AIO_SERVERPORT, IO_USERNAME, IO_KEY);

// Feeds
Adafruit_MQTT_Subscribe ledControl = Adafruit_MQTT_Subscribe(&mqtt, IO_USERNAME "/feeds/led-control");
Adafruit_MQTT_Publish   ldrStatus  = Adafruit_MQTT_Publish(&mqtt, IO_USERNAME "/feeds/ldr-status");
Adafruit_MQTT_Subscribe ldrControl = Adafruit_MQTT_Subscribe(&mqtt, IO_USERNAME "/feeds/ldr-control"); 

// Publisher for syncing the dashboard toggle
Adafruit_MQTT_Publish ledControlPub = Adafruit_MQTT_Publish(&mqtt, IO_USERNAME "/feeds/led-control");

// ---------- Pins ----------
#define LED_PIN     13
#define BUTTON_PIN  14
#define LDR_PIN     2

// ---------- Behaviour control ----------
#define HOLD_OFF_MS             8000
#define AIO_MIN_INTERVAL_MS     1000   // <<< FIX: minimum 1 second between publishes
#define LDR_MIN_ACTION_GAP_MS   2000
#define CLOUD_ECHO_SUPPRESS_MS  1200  

// ---------- State ----------
bool ledState = false;
bool buttonLast = HIGH;
bool ldrEnabled = false;   
unsigned long holdOffUntil = 0;
int lastLdrSent = -1;

unsigned long lastLedControlAt  = 0;
unsigned long lastLdrStatusAt   = 0;
unsigned long lastAutoActionAt  = 0;

bool lastCloudState = false;
unsigned long lastCloudAt = 0;

// Change origin
enum ChangeOrigin { ORIGIN_CLOUD, ORIGIN_BUTTON, ORIGIN_LDR };

// ---------- Helpers ----------
void publishLED(ChangeOrigin origin) {
  unsigned long now = millis();

  // Publish to Adafruit IO only if >1s since last publish
  if (now - lastLedControlAt >= AIO_MIN_INTERVAL_MS) {
    if (ledControlPub.publish(ledState ? "ON" : "OFF")) {
      Serial.print("LED control sync published: ");
      Serial.println(ledState ? "ON" : "OFF");
    }
    lastLedControlAt = now;
  } else {
    Serial.println("⚠️ Skipped publish (rate limit 1s)");
  }
}

void setLED(bool state, ChangeOrigin origin) {
  if (state == ledState) return;

  ledState = state;
  digitalWrite(LED_PIN, ledState ? HIGH : LOW);

  Serial.print("LED set to: ");
  Serial.print(ledState ? "ON" : "OFF");
  if (origin == ORIGIN_BUTTON) Serial.println(" (button)");
  else if (origin == ORIGIN_CLOUD) Serial.println(" (cloud)");
  else Serial.println(" (auto-LDR)");

  publishLED(origin);

  if (origin == ORIGIN_BUTTON || origin == ORIGIN_CLOUD) {
    holdOffUntil = millis() + HOLD_OFF_MS;
  }
}

void MQTT_connect() {
  int8_t ret;
  if (mqtt.connected()) return;

  Serial.print("Connecting to MQTT...");
  while ((ret = mqtt.connect()) != 0) {
    Serial.println(mqtt.connectErrorString(ret));
    mqtt.disconnect();
    delay(5000);
  }
  Serial.println("Connected to MQTT!");
}

void setup() {
  pinMode(LED_PIN, OUTPUT);
  pinMode(BUTTON_PIN, INPUT_PULLUP);
  pinMode(LDR_PIN, INPUT);

  Serial.begin(115200);
  delay(50);

  WiFi.begin(WIFI_SSID, WIFI_PASS);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\nWiFi Connected!");
  Serial.print("IP Address: ");
  Serial.println(WiFi.localIP());

  mqtt.subscribe(&ledControl);
  mqtt.subscribe(&ldrControl);

  publishLED(ORIGIN_BUTTON);
}

void loop() {
  MQTT_connect();

  Adafruit_MQTT_Subscribe *sub;
  while ((sub = mqtt.readSubscription(10))) {
    if (sub == &ledControl) {
      String cmd = (char *)ledControl.lastread;
      bool desired = (cmd == "ON");
      Serial.print("📩 Cloud says: ");
      Serial.println(cmd);

      unsigned long now = millis();
      if (desired == lastCloudState && (now - lastCloudAt < CLOUD_ECHO_SUPPRESS_MS)) {
        Serial.println("⏩ Ignored duplicate cloud echo");
        continue;
      }
      lastCloudState = desired;
      lastCloudAt = now;

      setLED(desired, ORIGIN_CLOUD);
    }

    if (sub == &ldrControl) {
      String cmd = (char *)ldrControl.lastread;
      ldrEnabled = (cmd == "ON");
      Serial.print("📩 LDR Control set to: ");
      Serial.println(ldrEnabled ? "ENABLED" : "DISABLED");
    }
  }

  // Button
  bool buttonNow = digitalRead(BUTTON_PIN);
  if (buttonNow == LOW && buttonLast == HIGH) {
    Serial.println("🔘 Button pressed -> Toggle LED");
    setLED(!ledState, ORIGIN_BUTTON);
  }
  buttonLast = buttonNow;

  // LDR
  if (ldrEnabled) {
    unsigned long now = millis();
    int isDark = (digitalRead(LDR_PIN) == LOW) ? 1 : 0;

    if (isDark != lastLdrSent) {
      if (now - lastLdrStatusAt >= AIO_MIN_INTERVAL_MS) {
        lastLdrSent = isDark;
        if (ldrStatus.publish(isDark ? "1" : "0")) {
          Serial.print("☀️ LDR published: ");
          Serial.println(isDark ? "DARK (1)" : "BRIGHT (0)");
        }
        lastLdrStatusAt = now;
      }
    }

    if (now >= holdOffUntil && (now - lastAutoActionAt >= LDR_MIN_ACTION_GAP_MS)) {
      if (isDark && !ledState) {
        Serial.println("🌙 Auto-LDR: Dark detected -> Turn LED ON");
        setLED(true, ORIGIN_LDR);
        lastAutoActionAt = now;
      } else if (!isDark && ledState) {
        Serial.println("☀️ Auto-LDR: Bright detected -> Turn LED OFF");
        setLED(false, ORIGIN_LDR);
        lastAutoActionAt = now;
      }
    }
  }

  delay(50);
}
