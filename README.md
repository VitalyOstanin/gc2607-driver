# gc2607-driver

A Linux V4L2 sub-device driver for the GalaxyCore **GC2607** colour image
sensor as fitted to the Huawei MateBook X Pro 2024 (`VGHH-XX`, Intel Meteor
Lake / IPU6). It brings up the sensor over I2C, exposes the standard camera
controls, and registers as an async sub-device so the IPU6 ISYS can stream raw
Bayer frames.

## Contents

- [Purpose](#purpose)
- [Register sequences](#register-sequences)
- [Sensor parameters](#sensor-parameters)
- [Kernel compatibility](#kernel-compatibility)
- [Build](#build)
- [Install](#install)
- [Dependencies in the stack](#dependencies-in-the-stack)
- [Related projects](#related-projects)
- [Acknowledgements](#acknowledgements)

## Purpose

There is no mainline driver for the GalaxyCore GC2607. This driver is written
to the kernel camera-sensor guidelines (V4L2 CCI register access, runtime PM,
`V4L2_CID_VBLANK` frame-rate control, exposure/gain controls, async sub-device
registration). The register sequences match the factory configuration rather
than a third-party port, so the mode timing is correct.

## Register sequences

The initialisation sequences in [gc2607-regs.h](gc2607-regs.h) configure the
native **1928x1088 mode at 30 fps**, 10-bit GRBG Bayer, over 2 MIPI CSI-2 data
lanes, with a 19.2 MHz external clock.

## Sensor parameters

| Parameter | Value |
|-----------|-------|
| Native resolution | 1928x1088 |
| Bayer order | GRBG, 10-bit |
| MIPI lanes | 2 |
| Link frequency | 257343750 Hz (257.34 MHz) |
| External clock | 19.2 MHz |
| Black level (pedestal) | 64 |
| Controls | `EXPOSURE`, `ANALOGUE_GAIN` (LUT index 0..16), `VBLANK`, `HBLANK`, `PIXEL_RATE`, `LINK_FREQ` |

The link frequency must match the value the IPU-bridge advertises for this
sensor (see [gc2607-ipu-bridge](https://github.com/VitalyOstanin/gc2607-ipu-bridge));
otherwise the IPU6 will not establish the CSI-2 link.

## Kernel compatibility

| Kernel  | Status                  | Reason |
|---------|-------------------------|--------|
| < 6.8   | does not build          | uses `v4l2_subdev_state_get_format()`, which is absent in 6.7 and earlier (verified against the v6.6/v6.7/v6.8 source trees) |
| 6.8     | expected to build       | every kernel API the driver calls is present from 6.8; not compiled on 6.8 here |
| 7.0     | verified                | built, loaded and streaming on the development machine |
| > 7.0   | not guaranteed          | the V4L2 sub-device state API is renamed across releases; a future change may require a one-line update |

The hard lower bound is set by the newest in-kernel API the driver uses. The
other dependencies are older: the CCI register helpers (`cci_*`,
`devm_cci_regmap_init_i2c`) date to ~6.5-6.6, and the fwnode/control/runtime-PM
helpers are older still. There is no `BUILD_EXCLUSIVE_KERNEL` guard in
`dkms.conf`: on an unsupported kernel the build fails with a clear
missing-symbol error, and because the driver is a separate DKMS package its
failure does not affect the rest of the camera stack.

## Build

Out-of-tree against the running kernel:

```sh
make                      # builds gc2607.ko against /lib/modules/$(uname -r)/build
make KDIR=/path/to/ksrc   # build against a specific kernel tree
make clean
```

Requires the kernel headers/build tree for the target kernel.

## Install

### DKMS (recommended)

DKMS rebuilds the module automatically on every kernel upgrade. The package is
defined by [dkms.conf](dkms.conf).

```sh
sudo dkms add .
sudo dkms install gc2607-driver/1.0
sudo modprobe gc2607      # or it binds automatically on boot via acpi:GCTI2607
```

DKMS installs the module into the distribution's updates directory, which
`depmod` searches ahead of the stock kernel tree. `dkms status` shows the build
state per kernel; `sudo dkms remove gc2607-driver/1.0 --all` reverts it.

If a copy was previously installed by hand (see below), remove it first so the
manual file does not shadow the DKMS one:

```sh
sudo rm -f /lib/modules/$(uname -r)/updates/gc2607.ko
sudo depmod -a
```

### Manual

```sh
sudo cp gc2607.ko /lib/modules/$(uname -r)/updates/
sudo depmod -a
sudo modprobe gc2607
```

The module binds to the ACPI device `GCTI2607`. Installing under `updates/` by
hand survives a reboot but **not** a kernel upgrade — use DKMS for that.

## Dependencies in the stack

For an end-to-end colour camera the driver alone is not enough; it depends on:

1. The IPU-bridge knowing the sensor HID — see [gc2607-ipu-bridge](https://github.com/VitalyOstanin/gc2607-ipu-bridge).
2. The physical camera switch being on (otherwise the frame saturates).
3. A consumer of the raw stream: libcamera SoftISP, or the project's own
   [gc2607-isp](https://github.com/VitalyOstanin/gc2607-isp).

## Related projects

- [gc2607-isp](https://github.com/VitalyOstanin/gc2607-isp) — software ISP that turns the raw Bayer stream into a colour webcam.
- [gc2607-ipu-bridge](https://github.com/VitalyOstanin/gc2607-ipu-bridge) — the IPU-bridge patch that registers this sensor with the IPU6.

## Acknowledgements

Two earlier community drivers for the MateBook GC2607 were the starting
reference for the Linux bring-up — the INT3472 power sequencing, the
`ipu_bridge` registration, and the overall approach — and gave this effort its
initial push. Thanks to their authors:

- [abbood/gc2607-v4l2-driver](https://github.com/abbood/gc2607-v4l2-driver) — GC2607 V4L2 sub-device driver with INT3472 power and an `ipu_bridge` patch (Arch).
- [antonbiluta/gc2607-driver](https://github.com/antonbiluta/gc2607-driver) — GC2607 driver and `ipu_bridge` patch (Fedora).

This driver is an independent rewrite to the kernel camera-sensor guidelines
(V4L2 CCI register access, runtime PM, `V4L2_CID_VBLANK` framerate control,
async sub-device registration), with register sequences matching the factory
configuration so the mode timing is correct.
