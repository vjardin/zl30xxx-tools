# ZL70XXX-tools

This tool is aimed at testing the DPLL over SPI bus with some basic readings.

## Compile

Either you cross compile it with Buildroot or locally using:
```
$ make
```

## Usage

Example:
```
zl30733_id  -D 1 -s 100000 -m 0 -d /dev/spidev0.0
PAGE -> 0x0 (write 0x00 to 0x7F)
SPI_W: off=0x7F,  data=00
SPI_R: off=0x01 rx=0E 95
PAGE -> 0x0 (write 0x00 to 0x7F)
SPI_W: off=0x7F,  data=00
SPI_R: off=0x03 rx=03
PAGE -> 0x0 (write 0x00 to 0x7F)
SPI_W: off=0x7F,  data=00
SPI_R: off=0x05 rx=17 8A
PAGE -> 0x0 (write 0x00 to 0x7F)
SPI_W: off=0x7F,  data=00
SPI_R: off=0x07 rx=FF FF FF FF
ZL3073x identity via /dev/spidev0.0
  Chip ID              : 0x0E95  (ZL3073x (A))
  Revision             : 0x03  (major=0 minor=3)
  Firmware Version     : 0x178A
  Custom Config Version: 0xFFFFFFFF
```
