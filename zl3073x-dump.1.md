% ZL3073X-DUMP(1) zl3073x-dump 1.1.0 | Free Mobile
% Vincent Jardin <vjardin@free.fr>
% October 2026

<!--
SPDX-License-Identifier: AGPL-3.0-or-later
Copyright 2026 Free Mobile - Vincent Jardin

Build the man page:
    pandoc -s -t man zl3073x-dump.1.md -o zl3073x-dump.1
Preview it:
    pandoc -s -t man zl3073x-dump.1.md | man -l -
-->

# NAME

zl3073x-dump — dump, decode and integrity-check a Microchip ZL3073x DPLL / network synchronizer over SPI from Linux

# SYNOPSIS

**zl3073x-dump** \[**-o** _text_] \[**-j** _json_] \[**-d** _spiB.C_] \[**-D** _driver_]
\[**-s** _hz_] \[**-p** _pages_] \[**-r** _passes_] \[**-w** _ms_] \[**-n**] \[**-k**] \[**-F**] \[**-h**]

# DESCRIPTION

**zl3073x-dump** reads a Microchip / Microsemi **ZL3073x** DPLL (a SyncE/PTP
*network synchronizer*.

The intended use is **field verification**: confirm that the DPLL booted the
configuration you expect — the right reference frequencies, synthesizers and
outputs for your PTP/SyncE plan — and that the flash image has not been
corrupted, *without* a Microchip GUI or any vendor tooling.

The tool reads three things:

*Identity and integrity.*
: `info.READY`, `id`, `revision`, `fw_ver`, `custom_config_ver`, and
  `master_clk_status.cfg_invalid` — the on-chip boot loader's own verdict on
  the flash configuration it auto-loaded at power-up.

*The full per-instance configuration.*
: By **walking the device mailboxes**, every reference (10), DPLL, synthesizer
  (5) and output (10) is read and decoded, including the computed output
  frequency in Hz for references and synths. The DPLLs are the part's channels
  (1 to 5 from the chip ID: ZL30731 to ZL30735; DPLL0, DPLL1 and DPLL2 on a
  ZL30733) and DPLL5, the NCO-hybrid assist.

*The raw register map.*
: The live register pages are emitted as a hex `register_map` so a dump can be
  byte-diffed against a known-good ("golden") capture, plus a repeatable
  **stable-cfg CRC32** that excludes free-running status bytes, and a
  **mailbox-cfg CRC32** over the per-instance configuration that the
  mailboxes hold.

## What it can and cannot read

The ZL3073x has **no host command to read raw flash**. At power-up the device
auto-loads **one** of its stored configurations (selected by the `GPIO_AC[2:0]`
strap pins) into its register map; the other stored banks are invisible to the
host. **zl3073x-dump therefore reflects the single configuration currently
loaded** (its slot number is reported as `config_slot`), not the other banks.
To inspect a different bank you must make the device boot it (change the
`GPIO_AC` strap and reset) and dump again.

## Driver handling

On a running system the ZL3073x is normally bound to the in-kernel
**zl3073x-spi** driver, which owns the SPI device and exposes the DPLL through
the kernel **dpll**(_netlink_) subsystem. To read raw registers, **zl3073x-dump**
temporarily unbinds that driver, binds **spidev**, reads
`/dev/spidevB.C`, then **always** rebinds the DPLL driver — on normal exit, on
error, and on **SIGINT**/**SIGTERM**/**SIGHUP**. The DPLL output clocks keep
running throughout (the configuration lives in the device, not the driver), but
the kernel dpll/devlink objects briefly disappear and reappear.

When the device is bound to **spidev** already (a device tree that gives the
ZL3073x to a userspace PTP daemon), the tool reads it as it is and changes no
binding. In every case it puts back only what it changed. The lock file (see
**FILES**) is taken before any of this: a run refused for the lock changes
nothing.

> **Note.** The kernel driver's *probe* writes a few registers, so each rebind
> perturbs the device state slightly. For a byte- or CRC-stable golden compare,
> read back-to-back with **-k** then **-n** (see **INTEGRITY CHECKING**).

# OPTIONS

`-o` _file_
:   Write the decoded **text** report to _file_ (`/dev/stdout` is the default
    when neither **-o** nor **-j** is given).

`-j` _file_
:   Write the structured **JSON** dump to _file_. Both outputs carry the raw hex
    `register_map`.

`-d` _spiB.C_
:   sysfs SPI device name, _busN.csN_ form (default **spi0.0**).

`-D` _driver_
:   Kernel DPLL driver to unbind/rebind (default **zl3073x-spi**).

`-s` _hz_
:   SPI clock in Hz (default **5000000**; keep at or below ~10 MHz on the
    1.8 V bus).

`-p` _pages_
:   Number of 127-byte register pages (1–15) to capture for the CRC and
    `register_map` (default **15**, the whole map).

`-r` _passes_
:   Number of whole-map sampling passes used to detect free-running ("volatile")
    status bytes, which are excluded from the stable CRC (default **8**).

`-w` _ms_
:   Delay between sampling passes, in milliseconds (default **300**). The total
    sampling window is roughly _passes_ x _ms_; widen it to catch slow-ticking
    status registers.

`-n`
:   Do **not** touch the kernel driver — assume `/dev/spidevB.C` is already
    bound. Use after a **-k** run.

`-k`
:   Leave **spidev** bound on exit instead of rebinding the DPLL driver. Used to
    take a golden capture that a following **-n** run can compare against without
    an intervening driver re-probe.

`-F`
:   Run even while another process holds the chip's lock file (see **FILES**).
    With **-F** each register access (page select + data transfer) is serialized,
    but 3rd parties multi-access mailbox sequences can interleave with
    ours: use it only for a read-only look on a live system.

`-h`
:   Print a short usage summary.

# OUTPUT

## Text report

```
ZL3073x DPLL config dump (spi0.0)
  id 0x0e95 (ZL30733)  rev 0x03  fw 178a  custom_config_ver 0xffffffff (BLANK)
  ready=1  cfg_invalid=0  booted config slot AC=0
  stable-cfg CRC32 0x18e9ff57 (1902/1905 static)   whole-map CRC32 0x25666f35 (3 volatile)
  mailbox-cfg CRC32 0x5d0c41e7 (29 instances, 1481 bytes)

ref (10 instances):
  [ 2] freq_base=10000 freq_mult=12500 ... => 125000000 Hz
  [ 6] freq_base=6400  freq_mult=25177 ... => 161132800 Hz
  ...
dpll (4 instances):
  [ 0] ...
  [ 1] ref_prio0=255 ref_prio1=16 ... bw_fixed=7 bw_var=129  => loop bandwidth 1.07 Hz (variable)
  [ 2] ref_prio0=15 ref_prio1=255 ... bw_fixed=7 bw_var=79  => loop bandwidth 0.0294 Hz (variable)
  [ 5] ref_prio0=255 ref_prio1=16 ... bw_fixed=7 bw_var=129  => loop bandwidth 1.07 Hz (variable)
synth (5 instances):
  [ 0] ... => 312500000 Hz
  ...
register_map (raw hex, 15 pages x 0x7F):
  0000: a1 0e 95 03 00 17 8a ff ...
```

## JSON object

The JSON has the keys `device`, `identity`, `integrity`, `refs`, `dplls`,
`synths`, `outputs`, and `register_map`. `refs` and `synths` carry a computed
`freq_hz`; `refs` also carry `enabled`. The `index` of a `dplls` entry is the
DPLL's number: 0, 1, 2 and 5 on a ZL30733. `dplls` carry the raw `bw_fixed`
and `bw_var` and the decoded `loop_bw_hz` (`null` for the reserved code) and
`loop_bw_mode` (`fixed`, `variable` or `reserved`). The `integrity` object holds
`chip_id_ok`, `ready`, `cfg_invalid`, `config_slot`, `stable_cfg_crc32`,
`whole_map_crc32`, the byte counts, `mailbox_cfg_crc32`, `mailbox_instances`
and `mailbox_bytes`. `register_map` is `{pages, page_bytes,
hex[]}` with one lowercase-hex string per page (254 chars = 127 bytes).

# EXIT STATUS

**0**
:   Success and the device passed the integrity verdict
    (`chip_id_ok` && `ready` && !`cfg_invalid`).

**1**
:   A read failed, or the device is present but the integrity verdict failed
    (wrong chip ID, not ready, or `cfg_invalid` set), or another process holds
    the chip's lock and **-F** was not given.

**2**
:   Command-line / usage error.

# BACKGROUND: THE DPLL IN A PTP / SYNCE SYSTEM

A ZL3073x is the **timing heart** of a PTP (IEEE 1588) and Synchronous Ethernet
node. It does three jobs that map directly onto the three instance types this
tool dumps:

**References** (`refs`) — the *input* clocks the DPLL can lock to: a SyncE clock
recovered from an Ethernet PHY/SerDes, a 1 PPS from a GNSS receiver, a 10 MHz
house reference, or a clock disciplined by a PTP servo. Each ref has a nominal
frequency (`freq_hz`) and an `enabled` flag; the **`dpll`** subsystem then
chooses among the enabled refs at run time by priority.

**DPLL channels** (`dplls`) — the loop filters. Each channel locks to one ref
(chosen by the per-ref priority table `ref_prio0..4`), filters its jitter and
wander, and on reference loss enters **holdover** on the last good frequency.
Its **loop bandwidth** sets that filter: the channel follows the reference's
variations below it and filters out the faster ones. It is read from the
mailbox registers `dpll_bw_fixed` (one of six fixed values, or *variable*) and
`dpll_bw_var` (*variable*: bandwidth = 10^(bw_var/32) × 10^-4 Hz, so 0x80 is
1 Hz). ITU-T G.8262.1 requires 1 to 3 Hz for an eEEC; a 1 PPS reference wants
30 mHz or less.
A node typically uses one channel for the PTP/SyncE frequency and another for a
1 PPS/phase plane. **DPLL5**, the NCO-hybrid assist, drives no synthesizer: a
channel in NCO-hybrid mode adds the assist's frequency offset to its own.

This tool reports the **static configuration** — *what the DPLL is set up to do*.
The **live lock state** — which reference is currently selected, whether the
channel is *locked* or in *holdover*, and the measured phase offset — lives in
the kernel and is read with the **dpll**(1) iproute2 tool, **ethtool**(8), or
**linuxptp** (`ptp4l`, `phc2sys`). The two are complementary: use
**zl3073x-dump** to prove the *right config is loaded and intact*, and **dpll** /
**linuxptp** to prove it is *locked correctly right now*.

# EXAMPLES

## Quick health check before starting PTP

Confirm the DPLL booted a valid configuration, then start the PTP stack only on
success:

```sh
zl3073x-dump >/run/dpll-config.txt && ptp4l -f /etc/ptp4l.conf -i eth0 &
```

The exit status gates the launch: a corrupted or unprogrammed flash
(`cfg_invalid` set, or wrong chip ID) returns non-zero and PTP is not started.

## Capture a golden reference on a known-good unit

On a board you trust, capture the configuration fingerprint and full JSON. The
**-k** keeps **spidev** bound so the kernel driver does not re-probe and shift
the state:

```sh
zl3073x-dump -k -j /etc/dpll/golden.json
```

Record the `stable_cfg_crc32` and the `mailbox_cfg_crc32`, and archive
`golden.json` as the site reference.

## Verify a field unit against the golden capture

Immediately after the **-k** capture (no driver re-probe in between), read again
with **-n** and compare. To restore the DPLL driver afterwards, unbind spidev:

```sh
zl3073x-dump -n -j /tmp/now.json

# compare the integrity fingerprint: both CRCs
jq -r '.integrity | .stable_cfg_crc32, .mailbox_cfg_crc32' /etc/dpll/golden.json /tmp/now.json

# or a full field-level diff of the decoded configuration
diff <(jq -S 'del(.integrity.whole_map_crc32)' /etc/dpll/golden.json) \
     <(jq -S 'del(.integrity.whole_map_crc32)' /tmp/now.json)

# restore the kernel DPLL driver
echo spi0.0 > /sys/bus/spi/drivers/spidev/unbind
echo ''     > /sys/bus/spi/devices/spi0.0/driver_override
echo spi0.0 > /sys/bus/spi/drivers/zl3073x-spi/bind
```

A matching pair, `stable_cfg_crc32` and `mailbox_cfg_crc32`, is strong
evidence the loaded configuration is byte-for-byte the golden one. Exclude `whole_map_crc32` from a strict diff — it
includes free-running status registers and is expected to vary.

## List the PTP/SyncE output frequencies

Pull just the synthesizer and reference frequencies the node is configured to
produce and consume:

```sh
zl3073x-dump -j /tmp/d.json
jq -r '.synths[] | "synth \(.index): \(.freq_hz) Hz"'                /tmp/d.json
jq -r '.refs[]   | select(.enabled) | "ref \(.index): \(.freq_hz) Hz"' /tmp/d.json
```

Use this to confirm, for example, that ref 2 is the **125 MHz** SyncE lane clock
and ref 6 is the **161.1328 MHz** 10G recovered clock before wiring them into a
`ptp4l` / SyncE (`esmc`) configuration.

## Inspect the DPLL reference-priority plan

The per-channel reference priorities decide which input each DPLL channel
prefers — the static half of a PTP/SyncE source-selection policy:

```sh
zl3073x-dump -j /tmp/d.json
jq -r '.dplls[] | "dpll \(.index): prio \(.ref_prio0),\(.ref_prio1),\(.ref_prio2)"' /tmp/d.json
```

A priority byte of `0x0F` (15) means *not selectable*; lower values are higher
priority. Cross-check the live selection with `dpll pin show` or
`dpll device show`.

## Cross-check against the live kernel state

After confirming the static config, verify the run-time lock with the kernel
tooling (the DPLL must be bound to **zl3073x-spi** for these to work):

```sh
dpll device show                 # lock-status: locked / holdover / unlocked
dpll pin show                    # which reference is currently selected
ptp4l -f /etc/ptp4l.conf -i eth0 # PTP servo
phc2sys -a -r                    # discipline the system clock
```

# INTEGRITY CHECKING

There are two integrity signals, of different strength:

**Easy / one-number — `custom_config_ver`.**
: A 32-bit version tag stamped into the flash image when the configuration was
  built. If present it is the cheapest "which config, which revision" check. If
  it reads `0xFFFFFFFF` the image is **untagged** (the version field was never
  programmed); this does *not* mean the config is empty or bad — only that you
  cannot use the tag and must fall back on the content checks below.

**Strong / content — `stable_cfg_crc32`, `mailbox_cfg_crc32` and `register_map`.**
: `stable_cfg_crc32` is a CRC over the directly addressed configuration
  registers (free-running status bytes excluded). It leaves out the four
  mailbox pages (reference, DPLL, synthesizer and output windows), because a
  window shows whichever instance was read last. So we are missing the
  per-instance configuration: reference frequencies and monitors, DPLL
  priorities, loop bandwidths and phase-slope limits, synthesizer frequencies,
  output dividers.
  `mailbox_cfg_crc32` covers those: each walked instance's window, from its first
  configuration byte (offset 0x05, after the mask and the semaphore) to the
  window's last register, in walk order and keyed by type and instance.
  Use both together. Take them before any process changes the
  configuration at run time (a PTP daemon steering a DPLL, for instance), or
  compare captures taken in the same state. `register_map` is the raw hex map
  for a byte-level diff. Capture all three on a golden unit and compare. Because
  the kernel probe writes registers on each rebind, always capture-then-compare
  with **-k**/**-n** so no re-probe falls between the two reads.

**Device self-verdict — `cfg_invalid`.**
: The on-chip boot loader sets `master_clk_status.cfg_invalid` if the flash
  configuration it loaded failed its own validation. `cfg_invalid = 0` is the
  device asserting the loaded image is structurally sound.

# FILES

`/dev/spidevB.C`
:   The spidev character device the tool reads while the DPLL driver is unbound.

`/sys/bus/spi/drivers/zl3073x-spi/{bind,unbind}`, `/sys/bus/spi/drivers/spidev/{bind,unbind}`, `/sys/bus/spi/devices/spiB.C/driver_override`
:   The sysfs entries used to swap the SPI device between the DPLL driver and
    spidev.

`/var/lock/zl3073x-spidev.lock` (fallback `/tmp/zl3073x-spidev.lock`)
:   Lock file to be shared with any PTP daemon's SPI dev layer.
    A ZL3073x access is a page select followed by a data transfer, so another
    process moving the page in between lands the data in another register.
    The tool takes it first, before any driver change.

# NOTES

- Multi-byte registers are **big-endian**, matching the device and the in-kernel
  driver.
- The mailbox poll times out after roughly two seconds per instance; a timeout
  is reported and aborts the run.
- The tool needs write access to the spidev/driver sysfs nodes — run as root (or
  with the matching capabilities) unless **-n** is used against an
  already-bound spidev.

# SEE ALSO

**dpll**(1), **ptp4l**(8), **phc2sys**(8), **ethtool**(8), **spidev**(4)

Microchip ZL3073x datasheet **DS20006552**; the Linux kernel driver
`drivers/dpll/zl3073x/`.

# COPYRIGHT

Copyright 2026 Free Mobile — Vincent Jardin. License AGPLv3+: GNU Affero GPL
version 3 or later <https://gnu.org/licenses/agpl.html>. This is free software:
you are free to change and redistribute it. There is NO WARRANTY, to the extent
permitted by law.
