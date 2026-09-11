# X3 route progress and overview background — 2026-09-11

The personal X3 (`ESP32-C3`, MAC `d4:05:92:90:13:74`) was updated with the
`navigator-x3` application built from commit `570cbf04`. The update includes a
continuous black walked route, a white-core/black-contour route ahead, and the
calm grayscale background in the whole-route Overview as well as GPS zoom.

## Installation evidence

- Serial device: `/dev/cu.usbmodem31401`.
- Detected flash: 16 MB.
- Partition layout confirmed from the device: app0 at `0x10000`, app1 at
  `0x650000`; OTA metadata selected app0 before the write.
- Only app0 at `0x10000` was written. Reader/dashboard app1, partition table,
  NVS, OTA metadata and the SD card were not written.
- Installed image: 816,912 bytes; SHA-256
  `bdb7a24aff06e36d81d0f11be8725df5d205c163caff251d46f02e40b8f68e21`.
- The exact installed byte range was read back and matched the build
  byte-for-byte with the same SHA-256.
- The 32 KiB `0x8000..0xffff` metadata snapshot was read before and after the
  update and remained byte-identical; SHA-256
  `0f593ef62e0da142b0a3d20c3a8e5860fd35a8415b74a2df2ae7d6e887b9f050`.
- The full pre-flash app0 partition was retained at
  `/Volumes/2TB/x3-flash-backups/2026-09-11-route-background/preflash-app0-full.bin`;
  SHA-256
  `3dba1ac313d30b87fa4353bcc4e2bfccc622005b8cde2d3b2f86a5317fb94f73`.

## Physical acceptance still required

Open the Fort Weiland route in Overview and confirm that the subtle grayscale
context appears behind the route, the walked portion is a continuous black
line, and the route ahead has a continuous white core inside a black contour.
If the route extent exceeds the existing 3x3 tile budget, the intentional safe
fallback is a clean white route-only frame. GPS zoom should continue to show
the background once a fresh fix is available.
