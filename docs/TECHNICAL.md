# Technical notes

Protocol details and reverse-engineering notes for Owie developers. Back to the [README](../README.md).

## Things found during development

The BMS (Battery Management System) board, located in the battery side of the OneWheel, communicates with the main board via [RS485](https://en.wikipedia.org/wiki/RS-485) protocol. Details that I've managed to discover so far:

- 115200 baud, standard 8N1 framing. In normal operation only the BMS talks, but the link is half duplex: the BMS does listen for a small command set (an unlock word followed by threshold-setting commands; Pint BMSes also accept a command that raises their charge-current limit). Owie's wiring does not pass the controller's transmissions on to the BMS, so none of those commands can reach the BMS while Owie is installed - see "Known limitations" below.
- The RS485 bus signaling voltage seems to be 3.3v. This makes it possible to read the bus voltages directly via ESP8266 and doesn't require 5v -> 3.3v level shifting.
- Both ends of the RS485 bus are terminated properly - 120-ohm resistors across the A and B lines and pullup / pulldown resistors A and B lines correspondingly.

## Doing away with MAX485 drivers

Technically, we'd need to use RS485 drivers such as MAX485 to intercept and retransmit bits on the line, however so far it seems like we can do away with them:

### Receiving RS485 directly via hardware UART:

The A (high) line of the RS485 bus coming from the BMS can be read directly via hardware UART with a little care.

When the line isn't driven by the BMS transmitter, it hovers around 3.3v/2 = 1.6v due to the terminating resistors on the BMS side. 1.6v isn't a defined logic level for a GPIO pin so chances are we'll read spurious data. However, if we turn on the pullup resistor of the UART RX pin to which the A line is connected, the bus idle voltage gets pulled up to right above 3 volts, which is more than 0.75\*VDD necessary for a logic 1 input, mentioned in ESP32 datasheet. This way the A wire from the BMS can be fed directly into the UART Rx pin and its logical state will read 1 at line idle, just as we want for UART communication.

## Transmitting RS485 without the driver

Transmitting the data to the MB requires us to signal on two wires. The A wire of the RS485 can be connected directly to the output of a hardware UART. The B line must be inverse of the A line as RS485 uses differential signaling.
I've achieved this by simply attaching a pin change interrupt to the UART TX pin and bitbanging the inverse value of this pin to the B line of the RS485.

# Communication protocol

The data frames sent by BMS are of the following general format:

1. Preamble - 3 bytes, fixed: `FF 55 AA`
1. Message type - 1 byte. Types `0x00`-`0x0D` and `0x0F`-`0x11` have been observed; there is no `0x0A`. `0x01` only appears while the BMS counts down to a protective power-off, `0x0E` only as a reply to a command (never with Owie, see below), `0x0F`-`0x11` only on newer Pint firmware.
1. Message body - variable length but fixed based on the message type above (`PACKET_LENGTHS_BY_TYPE` in `lib/bms/packet.h`).
1. Checksum - last two bytes of the frame, big-endian - simply the sum of all of the bytes in the frame, including the preamble.

## Message types

Reverse engineering of the BMS firmware (a PIC16F1788) explains the messages and their timing. The BMS runs a fast loop that samples the pack current once per millisecond, a measurement block once per second (cell voltages, temperatures and most of the messages below) and a 100 ms task (status byte, current, countdown warning). Parsing lives in `lib/bms/packet_parsers.cpp`.

| type | payload | rate | content |
|---|---|---|---|
| `0x00` | 1 byte | 100 ms | status flags: bits 1:0 temperature (01 hot, 10 cold, 11 sensor error), bits 3:2 voltage (01 under-voltage, 10 over-voltage), bit 4 balancing active, bit 5 charger detected, bits 7:6 the BMS's own SOC bucket (00 above 80 %, 01 50-80 %, 10 20-50 %, 11 20 % or below) |
| `0x01` | 2 bytes | 100 ms, only while a countdown is below 29 s | shutdown countdown: seconds left, reason mask (1 over-voltage, 2 under-voltage, 4 over-temperature, 8 idle timer) |
| `0x02` | 16 x uint16 | 1 s | 15 cell voltages in mV, then one word that is not a cell: the BMS's millisecond counter since its one-second tick (typically 42) |
| `0x03` | 1 byte | 1 s | state of charge in %. Owie rewrites this one |
| `0x04` | 5 bytes | 1 s | temperatures in C, unsigned, 0xFF = sensor invalid; the first four are pack thermistors, the fifth sits on the BMS board |
| `0x05` | int16 | 1 s and 100 ms | pack current in raw ADC counts, positive = discharge, about 55 mA per count (measured; consistent with a gain-20 amplifier on the BMS's 1 mOhm shunt) |
| `0x06` | 4 bytes | 1 s (XR 4210, Pint 5314 and newer); once at boot (Pint 5300) | BMS serial number = low bytes of the PIC's four user-ID words. XR boards with firmware 2006 (hardware 4206-4209) don't send it. Owie rewrites this one |
| `0x07` | 7 bytes | 1 s | thresholds: over-voltage mV, charge-complete mV, under-voltage mV (uint16 each), over-temperature C |
| `0x08` | 1 byte | 1 s | hardware/variant byte: 02 XR 4208-4210, 04 XR 4206, 06 Pint |
| `0x09` | 1 byte | 1 s | reason for the last shutdown (codes below) |
| `0x0B` | uint16 | once at boot | firmware version: 2006 (XR 4206-4209), 2007 (XR 4210), 3004 / 3005 / 3008 (Pint) |
| `0x0C` | uint16 | 1 s | cell balancing bit mask |
| `0x0D` | 3 bytes | 1 s, XR 4210 and Pint only | cycle counter (uint16), state of health in % |
| `0x0E` | 1 byte | reply to a command (Pint) | acknowledgement; never seen with Owie because commands cannot reach the BMS |
| `0x0F`, `0x10`, `0x11` | 5, 10, 4 bytes | newer Pint firmware (3008); rate not established | unknown - known only from captured frames |

## Power-off

Right before the BMS cuts power it sends a bare ASCII code with no preamble or checksum: `BU` power button, `DC` done charging, `CD` charger disconnected, `ZC` idle timeout (20 min without current), `UV` under-voltage, `CUV` critical under-voltage, `OV` over-voltage, `OT` over-temperature, `OC` over-current, `QC` hardware fault input, `PR` pre-charge failed, `CS` current-sensor zero out of range, `CC` charge over-current (Pint). It then drains its transmit buffer and waits about one second before releasing its power latch - that second is what gives Owie time to store its battery state. The same reason is reported as a numeric code in message `0x09` after the next boot: DC 1, CD 2, BU 3, QC 4, OC 5, UV 6, CUV 7, OT 8, OV 9, PR 10, ZC 11, CS 12, CC 13. Message `0x0B` is the *boot* message, not a shutdown signal.

## Known limitations

- Because Owie does not relay the controller's transmissions, the BMS never receives the unlock word or any command. On Pints the command that raises the BMS charge-current limit (from roughly 4 A to roughly 10 A) cannot get through either, so a fast charger might trip the BMS's charge over-current protection (`CC`) with Owie installed. This follows from the BMS firmware and has not been confirmed on a board - reports welcome.
