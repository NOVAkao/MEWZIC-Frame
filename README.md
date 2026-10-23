# ESP32 Spotify Controller

An ESP32-based Bluetooth media controller with a 2.9-inch e-paper display. It
shows an RTC clock while disconnected, a cassette graphic while connected but
idle, and track information with album art during playback. A rotary encoder
controls volume and playback, while two buttons skip tracks.

The firmware is built with ESP-IDF and BTstack. It uses Bluetooth Classic
AVRCP/A2DP controls to interact with a paired media device; it does not require
the Spotify app to run on the ESP32.

## Features

- Clock and date from a DS3231-compatible RTC
- E-paper views for the clock, idle state, and now-playing information
- Album art decoding and display
- Rotary encoder for volume and play/pause
- Dedicated next-track and previous-track buttons
- Persistent storage of a previously connected Bluetooth peer for reconnect

## Hardware

- ESP32 DevKit (original ESP32 target)
- 2.9-inch, 296 x 128 e-paper display
- DS3231 RTC module
- KY-040-compatible rotary encoder
- Two momentary pushbuttons

### Wiring

| Component | Signal | ESP32 GPIO |
| --- | --- | ---: |
| E-paper | DIN / MOSI | 23 |
| E-paper | CLK / SCLK | 18 |
| E-paper | CS | 5 |
| E-paper | DC | 17 |
| E-paper | RST | 16 |
| E-paper | BUSY | 4 |
| RTC | SDA | 21 |
| RTC | SCL | 22 |
| Encoder | CLK | 32 |
| Encoder | DT | 33 |
| Encoder | SW (pushbutton) | 27 |
| Forward button | Switch to GND | 26 |
| Backward button | Switch to GND | 25 |

Connect module grounds to ESP32 ground and power the modules according to their
specifications. The button inputs use internal pull-ups and are active-low.
The RTC is accessed over I2C at address `0x68`.

The Wokwi wiring diagram is in [`diagram.json`](./diagram.json). It models the
RTC as a DS1307; the firmware uses the compatible register interface and the
same I2C address.

## Build requirements

- ESP-IDF **5.4.4** (the version recorded in `dependencies.lock`)
- BTstack checkout containing `port/esp32/components/btstack`
- ESP-IDF's configured Python and build tools

The root [`CMakeLists.txt`](./CMakeLists.txt) uses `IDF_PATH` and `BTSTACK_ROOT`.
If `BTSTACK_ROOT` is not set, it defaults to `C:/btstack` on Windows. Set it to
the location of your BTstack checkout if it is elsewhere.

From an ESP-IDF-enabled terminal, for example PowerShell:

```powershell
$env:BTSTACK_ROOT = "C:\btstack"
idf.py set-target esp32
idf.py build
```

To flash and open the serial monitor, replace `COMx` with the ESP32's serial
port:

```powershell
idf.py -p COMx flash monitor
```

The build produces `build/esp32_spotify_controller.bin` and
`build/esp32_spotify_controller.elf`.

## Use

1. Flash the firmware and power the ESP32 with the display, RTC, encoder, and
   buttons connected.
2. Pair/connect a Bluetooth media device to the controller. The firmware
   stores a connected peer in NVS and attempts to use that peer on a later boot.
3. Use the encoder to adjust volume; press its shaft for play/pause.
4. Press the forward or backward button to skip tracks.

The display shows the clock while Bluetooth is disconnected, the cassette
graphic when connected but not actively playing, and media details during
playback. The clock is read from the RTC; set the RTC time separately if it is
not already correct.

## Wokwi simulation

Wokwi does not emulate the ESP32 Bluetooth radio. The project therefore
defaults to hardware mode (`WOKWI_SIMULATION` is `0` in
[`main/config.h`](./main/config.h)). To simulate the display, clock, encoder,
and buttons, set it to `1` and rebuild. In simulation, Bluetooth initialization
is skipped and only the clock view is available.

The Wokwi project configuration uses the firmware and ELF files under `build/`;
see [`wokwi.toml`](./wokwi.toml).
