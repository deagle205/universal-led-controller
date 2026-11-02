// Shared ESP-NOW message format. Keep this file identical in LED_Controller and LED_Node.
#pragma once
#include <stdint.h>

#define LED_MAGIC   0x4C  // 'L'
#define LED_CHANNEL 1     // Wi-Fi channel every board uses

enum : uint8_t {
  MSG_DISCOVER = 1,  // controller -> broadcast: "who is out there?"
  MSG_HELLO    = 2,  // node -> controller: name + current color
  MSG_SET      = 3,  // controller -> node: new color
};

struct __attribute__((packed)) LedMsg {
  uint8_t magic;
  uint8_t type;
  uint8_t r, g, b;
  uint8_t bri;   // 0-255 master brightness
  uint8_t on;    // 0 = off, 1 = on
  char    name[16];
};
