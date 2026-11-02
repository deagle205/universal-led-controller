# universal-led-controller notes

## Current state
Controller firmware (LED_Controller) is written and flashed to the controller on COM13. It boots ("LED controller ready"), but the screen and encoder have not been confirmed by the user yet. LED_Node is flashed on the node board (MAC 48:9D:31:E3:53:5C, name LED-535C) and boots. Controller and node have not been tested together yet.

## Next steps
- User checks the controller screen and encoder: dial direction (ENC_REVERSE), one row per click (STEPS_PER_DETENT), screen orientation (setRotation).
- Power both boards and check that LED-535C shows up under "Light" and follows the controller.
- Find out whether the RGB module is common-cathode ("-" pin) or common-anode ("+" pin). Set COMMON_ANODE in LED_Node to match.
- Design review deck: fill in Neldinson's and Asma's roles (slide 4), add real research numbers (slide 6), swap the slide 15 mockup for a photo of the controller.
- Decide the real strip type (addressable WS2812 or 12V analog RGB). This changes the node wiring.

## Key facts
- Code: Documents\Arduino\UniversalLED (LED_Controller, LED_Node). led_protocol.h must be identical in both folders. FQBN esp32:esp32:esp32, upload with UploadSpeed=115200.
- GitHub: https://github.com/deagle205/universal-led-controller (private). The repo root is Documents\Arduino\UniversalLED. docs/NOTES.md and the deck in docs/ are copies, so recopy them before committing.\n- Controller = COM13, MAC 48:9D:31:E3:53:D0.\n- Link: ESP-NOW on channel 1. The controller broadcasts DISCOVER every 2 s, nodes answer HELLO (name LED-XXXX from the MAC + current color), and the controller sends SET (rgb, bri, on). Up to 10 nodes; "All lights" sends to every node.
- Controller UI: Home (Light, Presets, Custom color HSV, Saved colors, Brightness, Power). Press = select, hold 600 ms = back/cancel. Presets and saved colors preview live. There are 12 saved slots, stored in NVS ("ledctl"/"slots").
- Node: R/G/B PWM with gamma 2.2. At boot it flashes R, G, B, then restores its last color from NVS.
- Gotcha: in core 3.3.12, WiFi.macAddress() read all zeros right after WiFi.mode() on the node. Use esp_read_mac(mac, ESP_MAC_WIFI_STA).
- Both boards enumerate as COM13 when plugged in one at a time. Tell them apart by MAC (controller ...53:D0, node ...53:5C).
- Gotcha: types used in function signatures must go in a .h (ui_types.h). If they are defined in the .ino, the Arduino prototype generator breaks the build.
- Serial monitor: use pyserial with dtr/rts False and a read timeout, or readline blocks forever.
- Boards: ESP32-DevKit (ESP32-D0WD), same kind as the esp32-display project.
- Screen: GMT020-02 2.0" ST7789 240x320, the same one as the BAT/esp32-display project (Adafruit_ST7789, tft.init(240, 320)).
- Controller pins: TFT VCC=3V3, GND, SCL/SCK=18, SDA/MOSI=23, CS=5, DC=2, RST=4, BL=3V3. Encoder (KY-040 style): CLK=32, DT=33, SW=25, +=3V3 (not 5V). Use INPUT_PULLUP.
- LED node: RGB module R=25, G=26, B=27, "-"=GND (common-cathode). If it is common-anode, wire "+" to 3V3 and set COMMON_ANODE 1.
- Mid-semester design review deck: Downloads\Universal Light Controller - Design Review.pptx. It is built from "Light by Slidesgo.pptx" (the original is untouched) and follows "Mid-semester design review rubric.docx": problem, team, research, ideation, concept selection, prototype, full-sentence titles, 7-10 min. Team: Darrick, Neldinson, Asma. Previews render through PowerPoint COM (no LibreOffice on this PC).
- Avoid: GPIO 6-11 (flash), 34-39 (input only, no pull-ups), 1/3 (USB serial), 12 (strap; HIGH at boot breaks flash voltage).

## Session log
- 2026-11-02: Put the firmware, deck, notes and a new README on GitHub in a private repo.
- 2026-10-24: Made the 18-slide design review deck. Firmware: the Light row now shows 10 characters ("All lights" no longer cut off); the controller has not been reflashed yet.
- 2026-10-24: Flashed LED_Node to the node board. Fixed its name: the MAC read as zeros, so it now uses esp_read_mac.
- 2026-10-24: Wrote the controller firmware (presets, HSV custom picker, 12 saved slots, multi-node ESP-NOW) and the node firmware. Flashed the controller.
- 2026-10-24: Made the pin plan for the controller (ST7789 + encoder) and the RGB-module node.
- 2026-10-24: created
