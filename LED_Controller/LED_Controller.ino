// Universal LED controller: ST7789 screen + rotary encoder.
// Finds LED nodes over ESP-NOW and sets their color.
//   turn  = move / adjust
//   press = select
//   hold  = back (cancels a color preview)
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <Preferences.h>
#include <SPI.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>
#include "led_protocol.h"
#include "ui_types.h"

// ---- Pins ----
#define TFT_CS   5
#define TFT_DC   2
#define TFT_RST  4   // SCK = 18, MOSI = 23 (hardware SPI)
#define ENC_CLK  32
#define ENC_DT   33
#define ENC_SW   25

#define STEPS_PER_DETENT 4   // set to 2 if one click moves two rows
#define ENC_REVERSE      0   // set to 1 if turning right moves up
#define HOLD_MS          600

// ---- Layout (landscape 320x240) ----
#define SCR_W     320
#define SCR_H     240
#define HDR_H     40
#define LIST_Y    46
#define ROW_H     28
#define LIST_ROWS 6
#define FOOT_Y    222

#define C_BG     0x0000
#define C_HDR    0x10A2
#define C_TEXT   0xFFFF
#define C_DIM    0x8410
#define C_ACCENT 0x2D7F
#define C_SELBG  0x18E3
#define C_EDITBG 0x3186
#define C_GOOD   0x37E6
#define C_BAD    0xF8A2
#define C_WARN   0xFE60

Adafruit_ST7789 tft(TFT_CS, TFT_DC, TFT_RST);
Preferences prefs;

// ---- Lights ----
#define MAX_NODES       10
#define NODE_TIMEOUT_MS 7000
struct Node { uint8_t mac[6]; char name[16]; LedState st; uint32_t lastSeen; bool online; };
Node nodes[MAX_NODES];
int nodeCount = 0;
bool nodesChanged = false;

LedState allState = {255, 255, 255, 180, true};
int target = -1;  // -1 = all lights, otherwise an index into nodes[]
LedState &cur() { return target < 0 ? allState : nodes[target].st; }
const char *targetName(int t) { return t < 0 ? "All lights" : nodes[t].name; }

struct Preset { const char *name; uint8_t r, g, b; };
const Preset PRESETS[] = {
  {"Red", 255, 0, 0},       {"Orange", 255, 80, 0},     {"Yellow", 255, 200, 0},
  {"Lime", 140, 255, 0},    {"Green", 0, 255, 0},       {"Teal", 0, 255, 140},
  {"Cyan", 0, 255, 255},    {"Blue", 0, 0, 255},        {"Purple", 140, 0, 255},
  {"Magenta", 255, 0, 255}, {"Pink", 255, 60, 120},     {"Warm white", 255, 150, 60},
  {"Cool white", 255, 255, 255},
};
const int NUM_PRESETS = sizeof(PRESETS) / sizeof(PRESETS[0]);

#define NUM_SLOTS  12
#define SLOT_EMPTY 0xFFFFFFFF
uint32_t slots[NUM_SLOTS];  // 0xRRGGBB, kept in flash

void loadSlots() {
  for (int i = 0; i < NUM_SLOTS; i++) slots[i] = SLOT_EMPTY;
  if (prefs.isKey("slots")) prefs.getBytes("slots", slots, sizeof(slots));
}
void saveSlots() { prefs.putBytes("slots", slots, sizeof(slots)); }
int firstEmptySlot() {
  for (int i = 0; i < NUM_SLOTS; i++) if (slots[i] == SLOT_EMPTY) return i;
  return 0;
}

// ---- Color math ----
void hsv2rgb(int h, int s, int v, uint8_t &r, uint8_t &g, uint8_t &b) {
  float H = h / 60.0f, S = s / 100.0f, V = v / 100.0f;
  int i = (int)H % 6;
  float f = H - (int)H;
  float p = V * (1 - S), q = V * (1 - S * f), t = V * (1 - S * (1 - f));
  float R, G, B;
  switch (i) {
    case 0:  R = V; G = t; B = p; break;
    case 1:  R = q; G = V; B = p; break;
    case 2:  R = p; G = V; B = t; break;
    case 3:  R = p; G = q; B = V; break;
    case 4:  R = t; G = p; B = V; break;
    default: R = V; G = p; B = q; break;
  }
  r = R * 255 + 0.5f; g = G * 255 + 0.5f; b = B * 255 + 0.5f;
}

void rgb2hsv(uint8_t r, uint8_t g, uint8_t b, int &h, int &s, int &v) {
  float R = r / 255.0f, G = g / 255.0f, B = b / 255.0f;
  float mx = max(R, max(G, B)), mn = min(R, min(G, B)), d = mx - mn;
  v = lroundf(mx * 100);
  s = mx <= 0 ? 0 : lroundf(d / mx * 100);
  float hf = 0;
  if (d > 0) {
    if (mx == R)      hf = 60 * fmodf((G - B) / d, 6);
    else if (mx == G) hf = 60 * ((B - R) / d + 2);
    else              hf = 60 * ((R - G) / d + 4);
  }
  if (hf < 0) hf += 360;
  h = lroundf(hf) % 360;
}

uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b) { return tft.color565(r, g, b); }
uint16_t slotColor(uint32_t c) { return rgb565(c >> 16, c >> 8, c); }

// ---- Radio (ESP-NOW) ----
const uint8_t BCAST[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

// Messages arrive on the Wi-Fi task; queue them and handle them in loop().
#define RXQ 8
struct Rx { uint8_t mac[6]; LedMsg msg; };
Rx rxq[RXQ];
volatile uint8_t rxHead = 0, rxTail = 0;
portMUX_TYPE rxMux = portMUX_INITIALIZER_UNLOCKED;

void onRecv(const esp_now_recv_info_t *info, const uint8_t *data, int len) {
  if (len != sizeof(LedMsg) || data[0] != LED_MAGIC) return;
  portENTER_CRITICAL(&rxMux);
  uint8_t next = (rxHead + 1) % RXQ;
  if (next != rxTail) {
    memcpy(rxq[rxHead].mac, info->src_addr, 6);
    memcpy(&rxq[rxHead].msg, data, sizeof(LedMsg));
    rxHead = next;
  }
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

void startRadio() {
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  esp_wifi_set_channel(LED_CHANNEL, WIFI_SECOND_CHAN_NONE);
  if (esp_now_init() != ESP_OK) Serial.println("ESP-NOW init failed");
  esp_now_register_recv_cb(onRecv);
  addPeer(BCAST);
}

void sendMsg(const uint8_t *mac, uint8_t type, const LedState &s) {
  LedMsg m = {};
  m.magic = LED_MAGIC;
  m.type = type;
  m.r = s.r; m.g = s.g; m.b = s.b; m.bri = s.bri; m.on = s.on;
  esp_now_send(mac, (const uint8_t *)&m, sizeof(m));
}

void applyToTarget() {
  const LedState &s = cur();
  Serial.printf("SET %s -> #%02X%02X%02X bri %d %s\n", targetName(target), s.r, s.g, s.b, s.bri, s.on ? "on" : "off");
  if (target >= 0) { sendMsg(nodes[target].mac, MSG_SET, s); return; }
  for (int i = 0; i < nodeCount; i++) {
    nodes[i].st = allState;
    sendMsg(nodes[i].mac, MSG_SET, allState);
  }
  if (nodeCount == 0) sendMsg(BCAST, MSG_SET, allState);
}

// Sends are rate-limited so spinning the dial doesn't flood the radio.
bool sendPending = false;
uint32_t lastSendMs = 0;
void markDirty() { sendPending = true; }

int findNode(const uint8_t *mac) {
  for (int i = 0; i < nodeCount; i++) if (memcmp(nodes[i].mac, mac, 6) == 0) return i;
  return -1;
}

void handleRadio() {
  while (true) {
    Rx r;
    bool got = false;
    portENTER_CRITICAL(&rxMux);
    if (rxTail != rxHead) { r = rxq[rxTail]; rxTail = (rxTail + 1) % RXQ; got = true; }
    portEXIT_CRITICAL(&rxMux);
    if (!got) break;
    if (r.msg.type != MSG_HELLO) continue;

    int idx = findNode(r.mac);
    if (idx < 0) {
      if (nodeCount >= MAX_NODES) continue;
      idx = nodeCount++;
      memcpy(nodes[idx].mac, r.mac, 6);
      nodes[idx].st = {r.msg.r, r.msg.g, r.msg.b, r.msg.bri, (bool)r.msg.on};
      nodes[idx].online = false;
      addPeer(r.mac);
      nodesChanged = true;
    }
    Node &n = nodes[idx];
    memcpy(n.name, r.msg.name, 15);
    n.name[15] = 0;
    n.lastSeen = millis();
    if (!n.online) {
      n.online = true;
      nodesChanged = true;
      Serial.printf("Node online: %s\n", n.name);
    }
  }
}

// ---- Encoder + button ----
volatile int32_t encCount = 0;
volatile uint8_t encPrev = 0;
const int8_t QDEC[16] = {0, -1, 1, 0, 1, 0, 0, -1, -1, 0, 0, 1, 0, 1, -1, 0};

void IRAM_ATTR onEncoder() {
  uint8_t s = (digitalRead(ENC_CLK) << 1) | digitalRead(ENC_DT);
  encPrev = ((encPrev << 2) | s) & 0x0F;
  encCount += QDEC[encPrev];
}

int readDetents() {
  static int32_t used = 0;
  int32_t c = encCount;
  int d = (c - used) / STEPS_PER_DETENT;
  used += d * STEPS_PER_DETENT;
  return ENC_REVERSE ? -d : d;
}

BtnEvt readButton() {
  static bool stable = HIGH, lastRaw = HIGH, holdFired = false;
  static uint32_t changeMs = 0, downMs = 0;
  bool raw = digitalRead(ENC_SW);
  uint32_t now = millis();
  if (raw != lastRaw) { lastRaw = raw; changeMs = now; }
  if (now - changeMs > 25 && raw != stable) {
    stable = raw;
    if (stable == LOW) { downMs = now; holdFired = false; }
    else if (!holdFired) return BTN_PRESS;
  }
  if (stable == LOW && !holdFired && now - downMs > HOLD_MS) { holdFired = true; return BTN_HOLD; }
  return BTN_NONE;
}

// ---- UI ----
Screen screen = SCR_HOME;
int cursor = 0, scrollTop = 0;
LedState undo;          // restored when backing out of a preview
int hue = 0, sat = 0, val = 100;
bool editing = false;   // custom screen: dial changes the value instead of moving

void drawRight(const char *s, int xRight, int y, int size) {
  tft.setTextSize(size);
  tft.setCursor(xRight - strlen(s) * 6 * size, y);
  tft.print(s);
}

const char *screenTitle() {
  switch (screen) {
    case SCR_HOME:      return "LED Controller";
    case SCR_TARGET:    return "Choose light";
    case SCR_PRESETS:   return "Presets";
    case SCR_CUSTOM:    return "Custom color";
    case SCR_SAVED:     return "Saved colors";
    case SCR_SAVE_SLOT: return "Save to slot";
    default:            return "Brightness";
  }
}

void drawHeaderSwatch() {
  const LedState &s = cur();
  int x = SCR_W - 58, y = 6;
  tft.fillRoundRect(x, y, 50, 28, 5, s.on ? rgb565(s.r, s.g, s.b) : C_HDR);
  tft.drawRoundRect(x, y, 50, 28, 5, C_DIM);
  if (!s.on) {
    tft.setTextSize(1);
    tft.setTextColor(C_DIM);
    tft.setCursor(x + 16, y + 10);
    tft.print("OFF");
  }
}

void drawHeader() {
  tft.fillRect(0, 0, SCR_W, HDR_H, C_HDR);
  tft.setTextSize(2);
  tft.setTextColor(C_TEXT);
  tft.setCursor(12, 6);
  tft.print(screenTitle());
  tft.setTextSize(1);
  tft.setTextColor(C_ACCENT);
  tft.setCursor(12, 27);
  tft.print("> ");
  tft.print(targetName(target));
  drawHeaderSwatch();
}

int listCount() {
  switch (screen) {
    case SCR_HOME:      return 6;
    case SCR_TARGET:    return nodeCount + 1;
    case SCR_PRESETS:   return NUM_PRESETS;
    case SCR_SAVED:
    case SCR_SAVE_SLOT: return NUM_SLOTS;
    default:            return 0;
  }
}

void drawFooter() {
  tft.fillRect(0, FOOT_Y - 2, SCR_W, SCR_H - FOOT_Y + 2, C_BG);
  tft.setTextSize(1);
  tft.setTextColor(C_DIM);
  tft.setCursor(12, FOOT_Y + 4);
  switch (screen) {
    case SCR_HOME:      tft.print("turn: move   press: select"); break;
    case SCR_TARGET:    tft.print("press: pick light   hold: back"); break;
    case SCR_PRESETS:
    case SCR_SAVED:     tft.print("press: keep color   hold: cancel"); break;
    case SCR_SAVE_SLOT: tft.print("press: save here   hold: back"); break;
    case SCR_CUSTOM:    tft.print(editing ? "turn: adjust   press: done" : "press: edit   hold: cancel"); break;
    case SCR_BRIGHT:    tft.print("turn: adjust   press: done"); break;
  }
  int n = listCount();
  if (n > LIST_ROWS) {
    char buf[12];
    snprintf(buf, sizeof(buf), "%d/%d", cursor + 1, n);
    drawRight(buf, SCR_W - 12, FOOT_Y + 4, 1);
  }
}

void getItem(int i, Item &it) {
  static char lbl[16];
  it.label = "";
  it.right[0] = 0;
  it.rightColor = C_DIM;
  it.hasSwatch = false;
  switch (screen) {
    case SCR_HOME: {
      static const char *L[] = {"Light", "Presets", "Custom color", "Saved colors", "Brightness", "Power"};
      it.label = L[i];
      if (i == 0) {
        strncpy(it.right, targetName(target), 10);
        it.right[10] = 0;
        it.rightColor = C_ACCENT;
      }
      if (i == 4) snprintf(it.right, sizeof(it.right), "%d%%", (cur().bri * 100 + 127) / 255);
      if (i == 5) {
        strcpy(it.right, cur().on ? "ON" : "OFF");
        it.rightColor = cur().on ? C_GOOD : C_BAD;
      }
      break;
    }
    case SCR_TARGET:
      if (i == 0) {
        it.label = "All lights";
        snprintf(it.right, sizeof(it.right), "%d found", nodeCount);
      } else {
        Node &n = nodes[i - 1];
        it.label = n.name;
        strcpy(it.right, n.online ? "online" : "offline");
        it.rightColor = n.online ? C_GOOD : C_BAD;
        it.hasSwatch = true;
        it.swatch = rgb565(n.st.r, n.st.g, n.st.b);
      }
      break;
    case SCR_PRESETS:
      it.label = PRESETS[i].name;
      it.hasSwatch = true;
      it.swatch = rgb565(PRESETS[i].r, PRESETS[i].g, PRESETS[i].b);
      break;
    case SCR_SAVED:
    case SCR_SAVE_SLOT:
      snprintf(lbl, sizeof(lbl), "Slot %d", i + 1);
      it.label = lbl;
      if (slots[i] == SLOT_EMPTY) strcpy(it.right, "empty");
      else { it.hasSwatch = true; it.swatch = slotColor(slots[i]); }
      break;
    default: break;
  }
}

void drawRow(int i) {
  if (i < scrollTop || i >= scrollTop + LIST_ROWS || i >= listCount()) return;
  int y = LIST_Y + (i - scrollTop) * ROW_H;
  bool sel = (i == cursor);
  tft.fillRect(0, y, SCR_W, ROW_H, sel ? C_SELBG : C_BG);
  if (sel) tft.fillRect(0, y, 4, ROW_H, C_ACCENT);
  Item it;
  getItem(i, it);
  if (it.hasSwatch) {
    tft.fillRoundRect(12, y + 5, 22, 18, 3, it.swatch);
    tft.drawRoundRect(12, y + 5, 22, 18, 3, C_DIM);
  }
  tft.setTextSize(2);
  tft.setTextColor(C_TEXT);
  tft.setCursor(screen == SCR_HOME ? 16 : 44, y + 6);
  tft.print(it.label);
  if (it.right[0]) {
    tft.setTextColor(it.rightColor);
    drawRight(it.right, SCR_W - 12, y + 6, 2);
  }
}

void drawList() {
  tft.fillRect(0, LIST_Y, SCR_W, LIST_ROWS * ROW_H, C_BG);
  for (int i = scrollTop; i < scrollTop + LIST_ROWS; i++) drawRow(i);
}

void moveCursor(int d) {
  int n = listCount();
  if (n == 0) return;
  int old = cursor;
  cursor = constrain(cursor + d, 0, n - 1);
  if (cursor == old) return;
  if (cursor < scrollTop) { scrollTop = cursor; drawList(); }
  else if (cursor >= scrollTop + LIST_ROWS) { scrollTop = cursor - LIST_ROWS + 1; drawList(); }
  else { drawRow(old); drawRow(cursor); }
  drawFooter();
}

// ---- Custom color screen ----
#define CX     136
#define CW     176
#define CROW_Y 46
#define CROW_H 34
const char *CUSTOM_ROWS[] = {"Hue", "Sat", "Value", "Save to slot", "Done"};

void drawSwatch() {
  const LedState &s = cur();
  tft.fillRoundRect(12, CROW_Y, 112, 124, 8, rgb565(s.r, s.g, s.b));
  tft.drawRoundRect(12, CROW_Y, 112, 124, 8, C_DIM);
  char buf[12];
  snprintf(buf, sizeof(buf), "#%02X%02X%02X", s.r, s.g, s.b);
  tft.fillRect(12, CROW_Y + 132, 112, 16, C_BG);
  tft.setTextSize(2);
  tft.setTextColor(C_TEXT);
  tft.setCursor(12, CROW_Y + 132);
  tft.print(buf);
}

void drawCustomRow(int i) {
  int y = CROW_Y + i * CROW_H;
  bool sel = (i == cursor);
  bool ed = sel && editing;
  tft.fillRect(CX, y, CW, CROW_H - 2, sel ? (ed ? C_EDITBG : C_SELBG) : C_BG);
  if (sel) tft.fillRect(CX, y, 3, CROW_H - 2, ed ? C_WARN : C_ACCENT);
  tft.setTextSize(2);
  tft.setTextColor(ed ? C_WARN : C_TEXT);
  if (i >= 3) {
    tft.setCursor(CX + 10, y + 9);
    tft.print(CUSTOM_ROWS[i]);
    return;
  }
  tft.setCursor(CX + 10, y + 2);
  tft.print(CUSTOM_ROWS[i]);
  int v = i == 0 ? hue : i == 1 ? sat : val;
  char buf[8];
  snprintf(buf, sizeof(buf), "%d", v);
  drawRight(buf, CX + CW - 8, y + 2, 2);

  // gradient bar with a marker at the current value
  int bx = CX + 10, bw = CW - 20, by = y + 21, bh = 7;
  for (int x = 0; x < bw; x++) {
    uint8_t r, g, b;
    if (i == 0)      hsv2rgb(x * 359 / (bw - 1), 100, 100, r, g, b);
    else if (i == 1) hsv2rgb(hue, x * 100 / (bw - 1), 100, r, g, b);
    else             hsv2rgb(hue, sat, x * 100 / (bw - 1), r, g, b);
    tft.drawFastVLine(bx + x, by, bh, rgb565(r, g, b));
  }
  int pos = i == 0 ? v * (bw - 1) / 359 : v * (bw - 1) / 100;
  tft.fillRect(bx + pos - 1, by - 2, 3, bh + 4, C_TEXT);
}

void drawCustom() {
  drawSwatch();
  for (int i = 0; i < 5; i++) drawCustomRow(i);
}

void customTurn(int d, bool fast) {
  if (!editing) {
    int old = cursor;
    cursor = constrain(cursor + d, 0, 4);
    if (cursor != old) { drawCustomRow(old); drawCustomRow(cursor); }
    return;
  }
  int step = fast ? 5 : 1;
  if (cursor == 0)      hue = ((hue + d * step * 2) % 360 + 360) % 360;
  else if (cursor == 1) sat = constrain(sat + d * step, 0, 100);
  else                  val = constrain(val + d * step, 0, 100);

  LedState &s = cur();
  hsv2rgb(hue, sat, val, s.r, s.g, s.b);
  s.on = true;
  markDirty();
  drawSwatch();
  drawHeaderSwatch();
  for (int i = cursor; i <= 2; i++) drawCustomRow(i);  // later bars depend on earlier values
}

// ---- Brightness screen ----
int briPct() { return (cur().bri * 100 + 127) / 255; }

void drawBright() {
  int pct = briPct();
  char buf[8];
  snprintf(buf, sizeof(buf), "%d%%", pct);
  tft.fillRect(0, 70, SCR_W, 50, C_BG);
  tft.setTextSize(6);
  tft.setTextColor(C_TEXT);
  tft.setCursor((SCR_W - (int)strlen(buf) * 36) / 2, 72);
  tft.print(buf);

  const LedState &s = cur();
  int bx = 20, by = 150, bw = 280, bh = 24;
  int fw = (bw - 4) * pct / 100;
  tft.drawRoundRect(bx, by, bw, bh, 6, C_DIM);
  tft.fillRect(bx + 2, by + 2, fw, bh - 4, rgb565(s.r, s.g, s.b));
  tft.fillRect(bx + 2 + fw, by + 2, bw - 4 - fw, bh - 4, C_BG);
}

void brightTurn(int d, bool fast) {
  int pct = constrain(briPct() + d * (fast ? 10 : 2), 0, 100);
  LedState &s = cur();
  s.bri = (pct * 255 + 50) / 100;
  if (pct > 0) s.on = true;
  markDirty();
  drawBright();
  drawHeaderSwatch();
}

// ---- Navigation ----
void openScreen(Screen s, int c = 0) {
  screen = s;
  cursor = c;
  editing = false;
  scrollTop = max(0, cursor - LIST_ROWS + 1);
  tft.fillScreen(C_BG);
  drawHeader();
  if (s == SCR_CUSTOM) drawCustom();
  else if (s == SCR_BRIGHT) drawBright();
  else drawList();
  drawFooter();
}

void previewColor(uint8_t r, uint8_t g, uint8_t b) {
  LedState &s = cur();
  s.r = r; s.g = g; s.b = b;
  s.on = true;
  markDirty();
  drawHeaderSwatch();
}

void previewAtCursor() {
  if (screen == SCR_PRESETS) previewColor(PRESETS[cursor].r, PRESETS[cursor].g, PRESETS[cursor].b);
  if (screen == SCR_SAVED && slots[cursor] != SLOT_EMPTY) {
    uint32_t c = slots[cursor];
    previewColor(c >> 16, c >> 8, c);
  }
}

void onTurn(int d, bool fast) {
  switch (screen) {
    case SCR_CUSTOM: customTurn(d, fast); break;
    case SCR_BRIGHT: brightTurn(d, fast); break;
    default:
      moveCursor(d);
      previewAtCursor();
      break;
  }
}

void onPress() {
  switch (screen) {
    case SCR_HOME:
      switch (cursor) {
        case 0: openScreen(SCR_TARGET, target + 1); break;
        case 1: undo = cur(); openScreen(SCR_PRESETS); break;
        case 2:
          undo = cur();
          rgb2hsv(cur().r, cur().g, cur().b, hue, sat, val);
          openScreen(SCR_CUSTOM);
          break;
        case 3: undo = cur(); openScreen(SCR_SAVED); break;
        case 4: openScreen(SCR_BRIGHT); break;
        case 5:
          cur().on = !cur().on;
          markDirty();
          drawRow(5);
          drawHeaderSwatch();
          break;
      }
      break;
    case SCR_TARGET:
      target = cursor - 1;
      openScreen(SCR_HOME, 0);
      break;
    case SCR_PRESETS:
      previewAtCursor();
      openScreen(SCR_HOME, 1);
      break;
    case SCR_SAVED:
      previewAtCursor();
      openScreen(SCR_HOME, 3);
      break;
    case SCR_SAVE_SLOT: {
      const LedState &s = cur();
      slots[cursor] = ((uint32_t)s.r << 16) | ((uint32_t)s.g << 8) | s.b;
      saveSlots();
      drawRow(cursor);
      tft.fillRect(0, FOOT_Y - 2, SCR_W, SCR_H - FOOT_Y + 2, C_BG);
      tft.setTextSize(1);
      tft.setTextColor(C_GOOD);
      tft.setCursor(12, FOOT_Y + 4);
      tft.printf("Saved to slot %d", cursor + 1);
      delay(700);
      openScreen(SCR_CUSTOM, 3);
      break;
    }
    case SCR_CUSTOM:
      if (cursor <= 2) {
        editing = !editing;
        drawCustomRow(cursor);
        drawFooter();
      } else if (cursor == 3) {
        openScreen(SCR_SAVE_SLOT, firstEmptySlot());
      } else {
        openScreen(SCR_HOME, 2);
      }
      break;
    case SCR_BRIGHT:
      openScreen(SCR_HOME, 4);
      break;
  }
}

void onHold() {
  switch (screen) {
    case SCR_HOME: break;
    case SCR_TARGET: openScreen(SCR_HOME, 0); break;
    case SCR_PRESETS:
    case SCR_SAVED: {
      int back = screen == SCR_PRESETS ? 1 : 3;
      cur() = undo;
      markDirty();
      openScreen(SCR_HOME, back);
      break;
    }
    case SCR_SAVE_SLOT: openScreen(SCR_CUSTOM, 3); break;
    case SCR_CUSTOM:
      if (editing) {
        editing = false;
        drawCustomRow(cursor);
        drawFooter();
      } else {
        cur() = undo;
        markDirty();
        openScreen(SCR_HOME, 2);
      }
      break;
    case SCR_BRIGHT: openScreen(SCR_HOME, 4); break;
  }
}

void setup() {
  Serial.begin(115200);
  pinMode(ENC_CLK, INPUT_PULLUP);
  pinMode(ENC_DT, INPUT_PULLUP);
  pinMode(ENC_SW, INPUT_PULLUP);
  encPrev = (digitalRead(ENC_CLK) << 1) | digitalRead(ENC_DT);
  attachInterrupt(ENC_CLK, onEncoder, CHANGE);
  attachInterrupt(ENC_DT, onEncoder, CHANGE);

  tft.init(240, 320);
  tft.setSPISpeed(40000000);
  tft.setRotation(1);

  prefs.begin("ledctl", false);
  loadSlots();
  startRadio();
  Serial.printf("LED controller ready, MAC %s\n", WiFi.macAddress().c_str());
  openScreen(SCR_HOME);
}

void loop() {
  uint32_t now = millis();

  int d = readDetents();
  if (d) {
    static uint32_t lastTurnMs = 0;
    bool fast = now - lastTurnMs < 60;
    lastTurnMs = now;
    onTurn(d, fast);
  }

  BtnEvt e = readButton();
  if (e == BTN_PRESS) onPress();
  if (e == BTN_HOLD) onHold();

  handleRadio();

  static uint32_t lastDiscoverMs = 0;
  if (now - lastDiscoverMs > 2000) {
    lastDiscoverMs = now;
    sendMsg(BCAST, MSG_DISCOVER, allState);
    for (int i = 0; i < nodeCount; i++) {
      bool on = now - nodes[i].lastSeen < NODE_TIMEOUT_MS;
      if (on != nodes[i].online) { nodes[i].online = on; nodesChanged = true; }
    }
  }
  if (nodesChanged) {
    nodesChanged = false;
    if (screen == SCR_TARGET) { drawList(); drawFooter(); }
  }

  if (sendPending && now - lastSendMs >= 40) {
    sendPending = false;
    lastSendMs = now;
    applyToTarget();
  }
}
