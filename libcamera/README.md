# libcamera SoftISP tuning for GC2607

## Contents

- [Purpose](#purpose)
- [Install](#install)
- [Notes](#notes)

## Purpose

[`gc2607.yaml`](gc2607.yaml) is the libcamera SoftISP (`simple` pipeline) tuning
file for the GC2607. It sets the black-level pedestal and the colour-correction
matrices so that applications using the stock libcamera SoftISP path (for
example a PipeWire camera) get a correctly black-corrected, colour-corrected
image.

It is independent of the standalone Rust ISP (`gc2607-isp`), which carries its
own tuning and does not read this file.

## Install

libcamera loads SoftISP tuning from `/usr/share/libcamera/ipa/simple/` by the
sensor model name (`gc2607`):

```sh
sudo install -m 0644 gc2607.yaml /usr/share/libcamera/ipa/simple/gc2607.yaml
```

Restart any consumer (the capture daemon, the PipeWire camera) for the change to
take effect.

## Notes

- **No `Agc` algorithm.** Exposure and gain are owned by the external AE that
  writes the sensor sub-device directly. With libcamera's SoftISP `Agc` enabled,
  two controllers drive the sensor at once and the frame periodically
  over-exposes. The `Agc` line is therefore omitted on purpose.
- The black level (`4096` on the 16-bit scale) corresponds to the sensor's
  optical-black pedestal of 64 in 10-bit raw.
- The colour matrices are five reference correlated colour temperatures;
  libcamera interpolates between them by its AWB-estimated temperature.
