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
SPI_R: off=0x01 rx=00 00
PAGE -> 0x0 (write 0x00 to 0x7F)
SPI_W: off=0x7F,  data=00
SPI_R: off=0x03 rx=00 00
PAGE -> 0x0 (write 0x00 to 0x7F)
SPI_W: off=0x7F,  data=00
SPI_R: off=0x05 rx=00 00
PAGE -> 0x0 (write 0x00 to 0x7F)
SPI_W: off=0x7F,  data=00
SPI_R: off=0x07 rx=00 00 00 00
ZL3073x identity via /dev/spidev0.0
  Chip ID              : 0x0000  (Unknown)
  Revision             : 0x0000  (major=0 minor=0)
  Firmware Version     : 0x0000
  Custom Config Version: 0x00000000
```
