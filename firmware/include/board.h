#pragma once

// Waveshare ESP32-S3-Touch-LCD-2.1 pin map (wiki).

#define I2C_SDA_PIN 15
#define I2C_SCL_PIN 7

#define LCD_BL_PIN 6
#define LCD_MOSI_PIN 1
#define LCD_SCLK_PIN 2

#define RGB_HSYNC 38
#define RGB_VSYNC 39
#define RGB_DE 40
#define RGB_PCLK 41

#define RGB_R1 46
#define RGB_R2 3
#define RGB_R3 8
#define RGB_R4 18
#define RGB_R5 17

#define RGB_G0 14
#define RGB_G1 13
#define RGB_G2 12
#define RGB_G3 11
#define RGB_G4 10
#define RGB_G5 9

#define RGB_B1 5
#define RGB_B2 45
#define RGB_B3 48
#define RGB_B4 47
#define RGB_B5 21

#define CST820_ADDR 0x15
#define CST820_INT_PIN 16

#define TCA9554_ADDR 0x20
#define EXIO_LCD_RST 1
#define EXIO_TP_RST 2
#define EXIO_LCD_CS 3
#define EXIO_BUZZER 8

#define PANEL_W 480
#define PANEL_H 480

// Battery divider on GPIO4: 200k (BAT) / 100k (GND), so pack voltage is 3× the ADC pin.
#define BAT_ADC_PIN 4

// 12-pin header switches: normally open to GND, active low, internal pull-up.
// Pin 3 D- = mute/unmute, pin 4 D+ = dismiss, pin 9 TXD = previous, pin 10 RXD = next.
#define SW_MUTE_PIN 19
#define SW_DISMISS_PIN 20
#define SW_PREV_PIN 43
#define SW_NEXT_PIN 44
