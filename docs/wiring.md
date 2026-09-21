# Case wiring

Waveshare ESP32-S3-Touch-LCD-2.1. The case covers both onboard USB-C sockets. Power comes out on the 12-pin header. Flashing still uses the UART Type-C, with the case open.

Pin numbers follow the [Waveshare 12-pin table](https://docs.waveshare.com/ESP32-S3-Touch-LCD-2.1). Pin 1 is GND.

## 12-pin header

| Pin | Name | Case use |
|-----|------|----------|
| 1 | GND | Switch common, and power ground |
| 2 | VBUS | +5 V from the panel USB-C jack |
| 3 | D− / GPIO19 | Mute / unmute |
| 4 | D+ / GPIO20 | Dismiss |
| 5 | GND | Same ground as pin 1 |
| 6 | 3V3 | Do not connect |
| 7 | SCL / GPIO7 | Do not connect (touch, IMU, RTC) |
| 8 | SDA / GPIO15 | Do not connect (touch, IMU, RTC) |
| 9 | TXD / GPIO43 | Previous conversation |
| 10 | RXD / GPIO44 | Next conversation |
| 11 | NC | No connection on the board |
| 12 | IO0 / GPIO0 | Do not connect (BOOT strap) |

The native USB-C and the UART Type-C stay inside the case and are left unplugged in normal use.

## Power

The panel jack only needs 5 V and ground. VBUS on pin 2 is the same rail as the onboard USB sockets, so the board runs and the LiPo charges.

| Panel USB-C | 12-pin |
|-------------|--------|
| VBUS | Pin 2 |
| GND | Pin 1 or pin 5 |
| D+ / D− | Leave open |
| CC1, CC2 | 5.1 kΩ each to GND, on the jack |

Those CC resistors make the socket a power inlet. A USB-C charger will not turn 5 V on without them. A jack sold with 5.1 kΩ pull-downs already fitted is the right part. Pull-ups are the wrong direction. A USB-A pigtail does not need extra resistors; the resistor is in the cable plug.

Do not feed the header’s 3.3 V pin. Do not put more than 5 V on pin 2.

## Battery

One 3.7 V LiPo (4.2 V full) on the 2-pin 1.25 mm system-battery socket. Waveshare calls it MX1.25; the schematic labels it PH1.25. That is not the smaller RTC battery header. Match the polarity printed on the board.

The onboard ETA6098 charger is set for about 2 A and stops at 4.2 V. A 2500 mAh pack is fine when the cell is rated for at least a 2 A charge. Use a protected pack. The battery switch selects whether the board runs from the cell. Charging uses the charger’s connection to the cell, and 5 V on pin 2 charges the same way as either onboard USB-C.

## Switches

Four normally-open momentary switches, each from the GPIO to ground (pin 1 or pin 5). The firmware enables internal pull-ups. A press is a low that has been steady for 30 ms. Open at rest, so a loose wire does nothing.

| Switch | 12-pin | GPIO | Action |
|--------|--------|------|--------|
| Previous | 9 TXD | GPIO43 | Previous conversation |
| Next | 10 RXD | GPIO44 | Next conversation |
| Mute | 3 D− | GPIO19 | Mute / unmute |
| Dismiss | 4 D+ | GPIO20 | Dismiss the conversation on screen |

Previous and next do nothing when only one conversation is active. Mute and dismiss do not send Cancel or Run. Leave the switches open while flashing. GPIO43 and GPIO44 are the UART used by the CH343, and a closed switch shorts that link.

The 12-pin cable Waveshare ships is about 100 mm, which is short enough for the internal pull-ups. If the wires are longer and presses show up on their own, add 10 kΩ from that GPIO to pin 6 (3.3 V).

## Flashing

Open the case and use the UART Type-C (the CH343 port), with the switches released. `pio run -t upload` from `firmware/`. The panel power jack can stay disconnected while you do that. The native USB-C is unused; GPIO19 and GPIO20 are the mute and dismiss switches.
