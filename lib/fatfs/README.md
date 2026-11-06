FatFs R0.13a as shipped in esp-idf v3.3.5 (`components/fatfs/src`, commit 03810c4), the
version the Arduino core 1.0.6 was built against: its eight headers are byte for byte
those in the core's `tools/sdk/include/fatfs`.

Built from source here instead of taken from the core's `libfatfs.a` for one line in
`ffconf.h`: `FF_SS_WL` is 512 and not 4096. See the comment there.

Left out: wear levelling, raw flash and SPI flash mounting, which this firmware does not use.
