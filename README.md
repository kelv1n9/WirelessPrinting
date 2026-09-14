# WirelessPrinting (ESP32)

Fork of [probonopd/WirelessPrinting](https://github.com/probonopd/WirelessPrinting), reworked for the ESP32.

Send gcode to a Marlin printer over WiFi from any slicer that speaks the OctoPrint API (OrcaSlicer, Cura, PrusaSlicer). The module stores the file and streams it to the printer over serial, so the printer needs no SD card and no computer has to stay on for the whole print.

## What is different from upstream

- **ESP32 only.** The ESP8266 environments are gone.
- **microSD through SD_MMC in 1-bit mode**, which works with the onboard slot of ESP32-CAM boards and keeps most GPIOs free. A card is required, the SPIFFS fallback is gone.
- **The card is a library, not a single slot.** Upstream deletes the previous file on every upload and answers the OctoPrint file listing with an empty stub. Files now accumulate, `/api/files` returns them all with sizes and free space, and one of them is selected for printing. The selection survives a reboot.
- **Upload and print are separate.** Upstream ignores the `print` field and always starts a print, so both of a slicer's buttons behave the same. The field is now honoured.
- **Line numbers and checksums.** Every command is sent as `N<n> <command>*<checksum>` and lost bytes are recovered through Marlin's resend mechanism. Upstream sends plain text, so a single dropped byte silently turns into a wrong move. On a hand-wired serial link at 250000 baud this happens often enough to ruin prints.
- **Transmission errors no longer stop the print.** Upstream treats every `Error:` from Marlin as fatal and fires `M112`, which halts the printer. Only genuine faults do that now.
- **A new telnet client takes over** instead of being locked out by a half-open socket.
- **Smaller image.** 72% of the app partition instead of 80%, which leaves room to grow. AsyncElegantOTA was replaced by a plain upload form of its own (its embedded web page alone was 53 KB), and the SPIFFS filesystem, the SPIFFS web editor, the NeoPixel code and the unused SdFat and ESPAsyncTCP dependencies were dropped.
- `platformio.ini` updated to a platform version the registry still serves.

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

After the first flash the module can be updated over the air, either from `http://<ip>/update` or with `espota.py` on port 3232. Both paths are kept on purpose, so a module that is hard to reach physically still has a second way in.

## Use

On first boot the module opens an access point named `AutoConnectAP`. Connect to it and enter your WiFi credentials.

- Web interface: `http://<ip>/`
- Raw gcode console: `nc <ip> 23`
- Slicer: host type **OctoPrint**, the module's IP, any API key

Beyond the OctoPrint subset, the web interface uses two endpoints of its own:

```
POST /select?name=<file>
POST /delete?name=<file>
```

A file that is currently being printed cannot be deleted or overwritten.

There is no authentication. Keep it on your local network.

## Credit

All of the original work is by [probonopd](https://github.com/probonopd) and the contributors to the upstream project. Upstream ships no license file.
