// Types used in function signatures. They live here because the Arduino builder
// puts its auto-generated prototypes above any types defined in the .ino.
#pragma once
#include <stdint.h>

struct LedState { uint8_t r, g, b, bri; bool on; };

enum BtnEvt { BTN_NONE, BTN_PRESS, BTN_HOLD };

enum Screen { SCR_HOME, SCR_TARGET, SCR_PRESETS, SCR_CUSTOM, SCR_SAVED, SCR_SAVE_SLOT, SCR_BRIGHT };

struct Item { const char *label; char right[16]; uint16_t rightColor; bool hasSwatch; uint16_t swatch; };
