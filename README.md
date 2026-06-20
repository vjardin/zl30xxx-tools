# ZL30xxx-tools

Tools for testing and inspecting a Microchip (ex-Microsemi) ZL3073x family DPLL
(network synchronizer) over the Linux `spidev` bus.

| tool | purpose |
|------|---------|
| `zl30733_id`   | quick identity / revision / firmware / config-version read over a raw `/dev/spidevX.Y` |
| `zl3073x-dump` | full configuration dump + decode + integrity check (mailbox walk over every reference / DPLL / synth / output), text and JSON output; manages the in-kernel `zl3073x-spi` driver bind itself. See **zl3073x-dump(1)**. |

## Compile

Either cross compile with Buildroot, or locally / standalone-cross with:
```
$ make                                   # native build
$ make CROSS_COMPILE=aarch64-linux-gnu-  # standalone cross build
$ make STATIC=1                          # static link (e.g. for an initramfs)
$ make man                               # build the man page(s) (needs pandoc)
$ make install install-man PREFIX=/usr/local
```

## Usage

### zl3073x-dump

```
zl3073x-dump                         # decoded text report to stdout
zl3073x-dump -j /tmp/dpll.json       # structured JSON (refs/dplls/synths/outputs + register_map)
zl3073x-dump -k -j golden.json       # capture a golden config (keeps spidev bound)
man ./zl3073x-dump.1                  # full manual, incl. PTP/SyncE examples
```

The man page documents the integrity-check workflow (golden capture/compare via
`stable_cfg_crc32`) and how the dumped references/synths/outputs map onto a
PTP (IEEE 1588) / Synchronous Ethernet deployment.

### zl30733_id

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
