/*
  tensionLEDMQTT - push-based LED patterns for the home tension wall

  Replaces the 5-second HTTP polling loop in ../tensionLEDHTTP. Instead of
  downloading a JSON file from Firebase Storage over and over, this board holds
  one long-lived MQTT connection open to a broker and the broker pushes a new
  pattern the instant the web app publishes one.

  The board only ever connects OUT, so there is no port forwarding, no dynamic
  DNS, and no inbound hole in the home firewall - and it still works from
  outside the house.

  Hardware: Adafruit Feather M0 + ATWINC1500 (WiFi101), 250 NeoPixels on A0.
  Libraries (Library Manager): WiFi101, ArduinoMqttClient, Adafruit NeoPixel.

  ---------------------------------------------------------------------------
  Wire protocol, topic "wall/pattern": one ASCII character per LED, index 0
  first. 250 bytes total, no JSON parser needed.

      '.'  off        'm'  magenta      'b'  blue
      'g'  green      'r'  red

  Any other character is treated as off. A short payload leaves the remaining
  LEDs off; a long one is truncated.

  Publish with retain = true. The broker then holds the current pattern, so a
  board that reboots gets the pattern already on the wall the moment it
  subscribes - which is the only thing the old polling loop was really for.

  See README.md for broker setup and the root-certificate step, which the
  WINC1500 requires before it will do TLS to a new host.
 */

#include <WiFi101.h>
#include <ArduinoMqttClient.h>
#include <Adafruit_NeoPixel.h>

#include "arduino_secrets.h"

/////// LED strip ///////
#define LED_PIN     A0
#define LED_COUNT   250
#define BRIGHTNESS  150          // max 255

/////// WINC1500 wiring on the Adafruit ATWINC1500 Feather ///////
#define WINC_CS   8
#define WINC_IRQ  7
#define WINC_RST  4
#define WINC_EN   2

/////// MQTT ///////
const char MQTT_CLIENT_ID[]   = "tension-wall";
const char TOPIC_PATTERN[]    = "wall/pattern";
const char TOPIC_STATUS[]     = "wall/status";
const unsigned long KEEPALIVE_MS = 60000;

/////// Reconnect behaviour ///////
const unsigned long RETRY_MS   = 5000;                  // between attempts
const unsigned long GIVE_UP_MS = 10UL * 60UL * 1000UL;  // reboot after 10 min offline

Adafruit_NeoPixel strip(LED_COUNT, LED_PIN, NEO_GRB + NEO_KHZ800);

WiFiSSLClient net;
MqttClient    mqtt(net);

// Our own copy of what the wall should be showing. The old sketch read state
// back out of the strip with getPixelColor() and compared it against hardcoded
// packed values, which only matched because of how brightness scaling rounds
// at BRIGHTNESS 150. Keeping the intended state here instead means changing
// the brightness can't break the colour logic.
uint8_t pattern[LED_COUNT];

unsigned long lastAttempt  = 0;
unsigned long offlineSince = 0;

void setup() {
  pinMode(LED_BUILTIN, OUTPUT);

  memset(pattern, '.', sizeof(pattern));
  strip.begin();
  strip.setBrightness(BRIGHTNESS);
  strip.show();                       // all pixels off

  WiFi.setPins(WINC_CS, WINC_IRQ, WINC_RST, WINC_EN);

  Serial.begin(9600);                 // no while(!Serial) - must run headless
  Serial.println(F("\ntensionLEDMQTT"));

  connectWiFi();
  if (WiFi.status() == WL_CONNECTED) connectBroker();
}

void loop() {
  if (WiFi.status() != WL_CONNECTED) {
    markOffline();
    if (retryDue()) {
      Serial.println(F("wifi: down, reconnecting"));
      WiFi.end();                     // hard reset the WINC before retrying
      connectWiFi();
    }
  } else if (!mqtt.connected()) {
    markOffline();
    if (retryDue()) connectBroker();
  } else {
    offlineSince = 0;
    mqtt.poll();                      // keepalive + dispatch incoming patterns
  }

  heartbeat();
  rebootIfStuckOffline();
}

/////// Pattern handling ///////

// Called by mqtt.poll() when a pattern arrives.
void onPattern(int messageSize) {
  memset(pattern, '.', sizeof(pattern));

  int n = 0;
  while (mqtt.available()) {          // drain fully, even if over-long
    int c = mqtt.read();
    if (c < 0) break;
    if (n < LED_COUNT) pattern[n] = (uint8_t)c;
    n++;
  }

  Serial.print(F("pattern: "));
  Serial.print(n);
  Serial.println(F(" bytes"));

  render();
}

uint32_t colorFor(uint8_t code) {
  // These r,g,b arguments are carried over verbatim from tensionLEDHTTP so the
  // wall keeps showing exactly the same colours. Note the channel order looks
  // swapped relative to the names - "green" is Color(255,0,0) and "red" is
  // Color(0,255,0). If you ever rewire or swap strips, fix it here, in one
  // place, rather than in four.
  switch (code) {
    case 'm': return strip.Color(  0, 255, 255);   // magenta
    case 'b': return strip.Color(  0,   0, 255);   // blue
    case 'g': return strip.Color(255,   0,   0);   // green
    case 'r': return strip.Color(  0, 255,   0);   // red
    default:  return strip.Color(  0,   0,   0);   // off
  }
}

void render() {
  for (int i = 0; i < LED_COUNT; i++) {
    strip.setPixelColor(i, colorFor(pattern[i]));
  }
  strip.show();   // one transmission for the whole strip, not one per pixel
}

/////// Connections ///////

void connectWiFi() {
  Serial.print(F("wifi: connecting to "));
  Serial.println(SECRET_SSID);

  WiFi.begin(SECRET_SSID, SECRET_PASS);   // blocks until connected or timeout

  if (WiFi.status() == WL_CONNECTED) {
    Serial.print(F("wifi: ok, ip "));
    Serial.println(WiFi.localIP());
  } else {
    Serial.println(F("wifi: failed"));
  }
}

void connectBroker() {
  Serial.print(F("mqtt: connecting to "));
  Serial.println(SECRET_MQTT_BROKER);

  mqtt.setId(MQTT_CLIENT_ID);
  mqtt.setUsernamePassword(SECRET_MQTT_USER, SECRET_MQTT_PASS);
  mqtt.setKeepAliveInterval(KEEPALIVE_MS);
  mqtt.setCleanSession(true);

  // Last will: if this board drops off the network without saying goodbye, the
  // broker publishes "offline" for us so the web app can show it. Delete these
  // three lines if your ArduinoMqttClient version lacks beginWill().
  mqtt.beginWill(TOPIC_STATUS, true, 1);
  mqtt.print("offline");
  mqtt.endWill();

  if (!mqtt.connect(SECRET_MQTT_BROKER, SECRET_MQTT_PORT)) {
    Serial.print(F("mqtt: failed, error "));
    Serial.println(mqtt.connectError());
    return;                           // loop() will pace the next attempt
  }

  Serial.println(F("mqtt: connected"));

  mqtt.onMessage(onPattern);
  mqtt.subscribe(TOPIC_PATTERN, 1);   // the retained pattern arrives right after

  mqtt.beginMessage(TOPIC_STATUS, true, 1);
  mqtt.print("online");
  mqtt.endMessage();
}

/////// Health ///////

bool retryDue() {
  if (millis() - lastAttempt < RETRY_MS) return false;
  lastAttempt = millis();
  return true;
}

void markOffline() {
  if (offlineSince == 0) offlineSince = millis();
}

// The WINC1500's TLS stack can wedge during a long outage. If we've been
// offline for GIVE_UP_MS, start over from scratch. The wall keeps displaying
// its last pattern until then, which is what you want mid-climb.
void rebootIfStuckOffline() {
  if (offlineSince == 0) return;
  if (millis() - offlineSince < GIVE_UP_MS) return;

  Serial.println(F("offline too long, rebooting"));
  Serial.flush();
#if defined(ARDUINO_ARCH_SAMD)
  NVIC_SystemReset();
#else
  while (true) {}                     // let the watchdog take it
#endif
}

void heartbeat() {
  // Onboard LED: a short blip every 2 s when connected, fast blink when not.
  bool online = (WiFi.status() == WL_CONNECTED) && mqtt.connected();
  unsigned long period = online ? 2000 : 250;
  unsigned long onTime = online ?   60 : 125;
  digitalWrite(LED_BUILTIN, (millis() % period) < onTime ? HIGH : LOW);
}
