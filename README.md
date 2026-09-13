# WirelessPrinting (ESP32)

Fork of [probonopd/WirelessPrinting](https://github.com/probonopd/WirelessPrinting), reworked for the ESP32.

Send gcode to a Marlin printer over WiFi from any slicer that speaks the OctoPrint API (OrcaSlicer, Cura, PrusaSlicer). The module stores the file and streams it to the printer over serial, so the printer needs no SD card and no computer has to stay on for the whole print.

## What is different from upstream

- **ESP32 only.** The ESP8266 environments are gone.
- **microSD through SD_MMC in 1-bit mode**, which works with the onboard slot of ESP32-CAM boards and keeps most GPIOs free. Falls back to SPIFFS when no card is present.
- **Line numbers and checksums.** Every command is sent as `N<n> <command>*<checksum>` and lost bytes are recovered through Marlin's resend mechanism. Upstream sends plain text, so a single dropped byte silently turns into a wrong move. On a hand-wired serial link at 250000 baud this happens often enough to ruin prints.
- **Transmission errors no longer stop the print.** Upstream treats every `Error:` from Marlin as fatal and fires `M112`, which halts the printer. Only genuine faults do that now.
- **A new telnet client takes over** instead of being locked out by a half-open socket.
- NeoPixel code removed, `platformio.ini` updated to a platform version the registry still serves.

## Serial pins

| Function | GPIO |
| --- | --- |
| Printer RX (module transmits) | 12 |
| Printer TX (module receives) | 13 |
| SD_MMC D0 / CLK / CMD | 2 / 14 / 15 |

GPIO1 and GPIO3 stay free, so the USB-TTL console keeps working for debugging while the printer is connected.

The baud rate is detected automatically from 115200, 250000 and 57600.

## Build and flash

```
pio run -e esp32dev
pio run -e esp32dev -t upload
```

After the first flash the module can be updated over the air, either from `http://<ip>/update` or with `espota.py`.

## Use

On first boot the module opens an access point named `AutoConnectAP`. Connect to it and enter your WiFi credentials.

- Web interface: `http://<ip>/`
- Raw gcode console: `nc <ip> 23`
- Slicer: host type **OctoPrint**, the module's IP, any API key

There is no authentication. Keep it on your local network.

## Credit

All of the original work is by [probonopd](https://github.com/probonopd) and the contributors to the upstream project. Upstream ships no license file.
