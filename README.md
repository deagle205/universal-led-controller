# Universal LED Controller

A handheld controller that sets the color of wireless LED nodes. Both ends are ESP32 boards that talk over ESP-NOW. The controller has a 2.0" ST7789 screen and a rotary encoder.

**Status:** both firmwares are written and flashed, and both boards boot. The controller's screen and encoder are not confirmed yet, and the controller and node have not been tested together.

## What's here

| Path | Contents |
| --- | --- |
| `LED_Controller/` | Controller firmware (screen, encoder, menus, saved colors) |
| `LED_Node/` | Node firmware (RGB PWM output, answers the controller) |
| `docs/Universal Light Controller - Design Review.pptx` | Mid-semester design review deck |
| `docs/NOTES.md` | Working notes: current state, next steps, gotchas |

`led_protocol.h` is in both firmware folders and must be identical in both.

## Build and flash

Install the ESP32 board package. The controller also needs the **Adafruit GFX** and **Adafruit ST7735 and ST7789** libraries.

```
arduino-cli compile --fqbn esp32:esp32:esp32 LED_Controller
arduino-cli upload --fqbn esp32:esp32:esp32:UploadSpeed=115200 -p COM13 LED_Controller
```

Use the same commands with `LED_Node` for a node. Change the port to match your board.

## Using the controller

Turn the dial to move or adjust, press to select, and hold for 600 ms to go back or cancel.

- **Light:** pick one node, or "All lights"
- **Presets** and **Saved colors:** preview live on the light (12 saved slots)
- **Custom color:** HSV picker
- **Brightness** and **Power**

## How the link works

ESP-NOW on channel 1. The controller broadcasts `DISCOVER` every 2 s. Each node answers `HELLO` with its name (`LED-XXXX`, from its MAC) and current color. The controller then sends `SET` (RGB, brightness, on/off). It supports up to 10 nodes.

At boot, a node flashes red, green, then blue, and restores its last color.

## Wiring

**Controller**

| Part | Pin | ESP32 |
| --- | --- | --- |
| ST7789 | VCC, BL | 3V3 |
| ST7789 | GND | GND |
| ST7789 | SCL/SCK | 18 |
| ST7789 | SDA/MOSI | 23 |
| ST7789 | CS | 5 |
| ST7789 | DC | 2 |
| ST7789 | RST | 4 |
| Encoder | CLK | 32 |
| Encoder | DT | 33 |
| Encoder | SW | 25 |
| Encoder | + | 3V3 (not 5V) |

**Node**

| RGB module | ESP32 |
| --- | --- |
| R | 25 |
| G | 26 |
| B | 27 |
| - | GND |

This is for a common-cathode module. For common-anode, wire `+` to 3V3 and set `COMMON_ANODE 1` in `LED_Node`.

## Settings to adjust

In `LED_Controller.ino`:

- `ENC_REVERSE`: set to 1 if turning right moves up
- `STEPS_PER_DETENT`: set to 2 if one click moves two rows

## Team

Darrick, Neldinson, Asma
