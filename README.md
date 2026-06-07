# gc2607-driver

A Linux V4L2 sub-device driver for the GalaxyCore **GC2607** colour image
sensor as fitted to the Huawei MateBook X Pro 2024 (`VGHH-XX`, Intel Meteor
Lake / IPU6). It brings up the sensor over I2C, exposes the standard camera
controls, and registers as an async sub-device so the IPU6 ISYS can stream raw
Bayer frames.

## Contents

- [Purpose](#purpose)
- [Register source](#register-source)
- [Sensor parameters](#sensor-parameters)
- [Build](#build)
- [Install](#install)
- [Dependencies in the stack](#dependencies-in-the-stack)
- [Related projects](#related-projects)

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
sensor (see [ipu-bridge-gc2607](../ipu-bridge-gc2607)); otherwise the IPU6 will
not establish the CSI-2 link.

## Build

Out-of-tree against the running kernel:

```sh
make                      # builds gc2607.ko against /lib/modules/$(uname -r)/build
make KDIR=/path/to/ksrc   # build against a specific kernel tree
make clean
```

Requires the kernel headers/build tree for the target kernel.

## Install

```sh
sudo cp gc2607.ko /lib/modules/$(uname -r)/updates/
sudo depmod -a
sudo modprobe gc2607      # or it binds automatically on boot via acpi:GCTI2607
```

The module binds to the ACPI device `GCTI2607`. Installing under `updates/`
survives a reboot but **not** a kernel upgrade; packaging via DKMS is the
intended long-term form.

## Dependencies in the stack

For an end-to-end colour camera the driver alone is not enough; it depends on:

1. The IPU-bridge knowing the sensor HID — see [ipu-bridge-gc2607](../ipu-bridge-gc2607).
2. The physical camera switch being on (otherwise the frame saturates).
3. A consumer of the raw stream: libcamera SoftISP, or the project's own
   [gc2607-isp](../gc2607-isp).

## Related projects

- [gc2607-isp](../gc2607-isp) — software ISP that turns the raw Bayer stream into a colour webcam.
- [ipu-bridge-gc2607](../ipu-bridge-gc2607) — the IPU-bridge patch that registers this sensor with the IPU6.
