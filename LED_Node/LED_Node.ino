// Universal LED node: drives one RGB LED (module for now) and takes orders from the controller over ESP-NOW.
// On power-up it flashes red, green, blue so you can check the wiring, then restores its last color.
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <esp_mac.h>
#include <Preferences.h>
#include "led_protocol.h"

#define PIN_R 25
#define PIN_G 26
#define PIN_B 27
#define COMMON_ANODE 0   // set to 1 if the module's common pin is "+" instead of "-"
#define PWM_FREQ 5000
#define PWM_BITS 8

Preferences prefs;
char nodeName[16];
uint8_t r = 255, g = 255, b = 255, bri = 180, on = 1;

volatile bool needHello = false, gotSet = false;
uint8_t helloTo[6];
LedMsg pending;
portMUX_TYPE rxMux = portMUX_INITIALIZER_UNLOCKED;
uint32_t saveAtMs = 0;  // flash write is delayed until the color stops changing

uint8_t gamma8(uint32_t v) { return powf(v / 255.0f, 2.2f) * 255 + 0.5f; }

void writeChannel(int pin, uint8_t c) {
  uint8_t v = on ? gamma8(c * bri / 255) : 0;
  ledcWrite(pin, COMMON_ANODE ? 255 - v : v);
}

void applyOutput() {
  writeChannel(PIN_R, r);
  writeChannel(PIN_G, g);
  writeChannel(PIN_B, b);
}

void onRecv(const esp_now_recv_info_t *info, const uint8_t *data, int len) {
  if (len != sizeof(LedMsg) || data[0] != LED_MAGIC) return;
  const LedMsg *m = (const LedMsg *)data;
  portENTER_CRITICAL(&rxMux);
  if (m->type == MSG_DISCOVER) { memcpy(helloTo, info->src_addr, 6); needHello = true; }
  if (m->type == MSG_SET) { memcpy(&pending, m, sizeof(LedMsg)); gotSet = true; }
  portEXIT_CRITICAL(&rxMux);
}

void addPeer(const uint8_t *mac) {
  if (esp_now_is_peer_exist(mac)) return;
  esp_now_peer_info_t p = {};
  memcpy(p.peer_addr, mac, 6);
  p.channel = LED_CHANNEL;
  p.ifidx = WIFI_IF_STA;
  p.encrypt = false;
  esp_now_add_peer(&p);
}

void setup() {
  Serial.begin(115200);
  ledcAttach(PIN_R, PWM_FREQ, PWM_BITS);
  ledcAttach(PIN_G, PWM_FREQ, PWM_BITS);
  ledcAttach(PIN_B, PWM_FREQ, PWM_BITS);

  // wiring check: red, green, blue
  uint8_t savedOn = on;
  on = 1; bri = 255;
  r = 255; g = 0;   b = 0;   applyOutput(); delay(300);
  r = 0;   g = 255; b = 0;   applyOutput(); delay(300);
  r = 0;   g = 0;   b = 255; applyOutput(); delay(300);
  on = savedOn;

  prefs.begin("lednode", false);
  r = prefs.getUChar("r", 255);
  g = prefs.getUChar("g", 255);
  b = prefs.getUChar("b", 255);
  bri = prefs.getUChar("bri", 180);
  on = prefs.getUChar("on", 1);
  applyOutput();

  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  esp_wifi_set_channel(LED_CHANNEL, WIFI_SECOND_CHAN_NONE);
  uint8_t mac[6];
  esp_read_mac(mac, ESP_MAC_WIFI_STA);  // WiFi.macAddress() can read zeros this early
  snprintf(nodeName, sizeof(nodeName), "LED-%02X%02X", mac[4], mac[5]);
  if (esp_now_init() != ESP_OK) Serial.println("ESP-NOW init failed");
  esp_now_register_recv_cb(onRecv);
  Serial.printf("%s ready, MAC %02X:%02X:%02X:%02X:%02X:%02X\n", nodeName, mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

void loop() {
  if (needHello) {
    uint8_t to[6];
    portENTER_CRITICAL(&rxMux);
    memcpy(to, helloTo, 6);
    needHello = false;
    portEXIT_CRITICAL(&rxMux);
    addPeer(to);
    LedMsg m = {};
    m.magic = LED_MAGIC;
    m.type = MSG_HELLO;
    m.r = r; m.g = g; m.b = b; m.bri = bri; m.on = on;
    strncpy(m.name, nodeName, sizeof(m.name) - 1);
    esp_now_send(to, (const uint8_t *)&m, sizeof(m));
  }

  if (gotSet) {
    portENTER_CRITICAL(&rxMux);
    r = pending.r; g = pending.g; b = pending.b; bri = pending.bri; on = pending.on;
    gotSet = false;
    portEXIT_CRITICAL(&rxMux);
    applyOutput();
    Serial.printf("SET #%02X%02X%02X bri %d %s\n", r, g, b, bri, on ? "on" : "off");
    saveAtMs = millis() + 2000;
  }

  if (saveAtMs && (int32_t)(millis() - saveAtMs) >= 0) {
    saveAtMs = 0;
    prefs.putUChar("r", r);
    prefs.putUChar("g", g);
    prefs.putUChar("b", b);
    prefs.putUChar("bri", bri);
    prefs.putUChar("on", on);
  }
}
