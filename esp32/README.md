# ESP32 deployment — WT32-ETH01 v1.4

This is the embedded alternative to the Debian/Proxmox deployment. It implements the same Huawei SunBridge cache/proxy behavior directly on a WT32-ETH01 v1.4 (ESP32 + LAN8720 Ethernet).

## Fixed network settings for the reference installation

| Item | Value |
|---|---|
| WT32-ETH01 | `192.168.10.27/24` |
| Gateway / DNS | `192.168.10.1` |
| SunBridge listener | TCP `5502` |
| Web dashboard | HTTP `80` |
| Huawei SDongle | `192.168.10.2:502` |
| Modbus Unit ID | `1` |

**Important:** the reference Proxmox LXC also uses `192.168.10.27`. Never run the LXC and ESP32 simultaneously. Stop one before starting the other. This deliberate IP reuse lets Home Assistant switch implementations without changing its Huawei Solar configuration.

## Features

- Ethernet-only proxy on WT32-ETH01 v1.4 / LAN8720.
- Same 14 tested Huawei register batches as the Debian implementation.
- 2 s post-connect settling delay and 500 ms inter-batch pacing.
- Complete MBAP + PDU reads (no assumption that one TCP read contains a full frame).
- Serialized single upstream access to the SDongle.
- FC3 cache reads and discovery passthrough; FC6 write-through.
- Web dashboard at `http://192.168.10.27/`.
- Active Modbus clients: IP, source port, connection date/time, duration, last request and request count.
- Last 10 disconnected clients.
- NTP time in `Europe/Rome`; proxy operation does not depend on NTP.
- `/health` JSON endpoint.

## Build on Windows PowerShell

Only PlatformIO CLI is required. From the repository root, the preferred single command is:

```powershell
py -m pip install -U platformio; cd esp32; py -m platformio run
```

After a successful build the normal application image is under `.pio\build\wt32-eth01\firmware.bin` and the post-build script creates the merged browser-flash image:

```text
dist\huawei-sunbridge-wt32-eth01-webflash.bin
```

The merged image contains bootloader, partition table, boot_app0 and application at their correct offsets, so it can be flashed as one image starting at address `0x0` by a compatible ESP Web Tools flasher such as web.esphome.io.

## Flash from web.esphome.io

1. Stop LXC 126 first, because it owns `192.168.10.27` in the reference setup.
2. Connect the WT32-ETH01 to the Windows PC through a USB-to-TTL adapter using **3.3 V logic**.
3. Put the ESP32 into download mode (GPIO0 low while resetting/powering the board, according to your adapter/wiring).
4. Open web.esphome.io in a Chromium-based browser, connect the serial port and install the merged file `dist\huawei-sunbridge-wt32-eth01-webflash.bin` at offset `0x0` when prompted for a custom binary.
5. Remove GPIO0 from ground and reset/power-cycle.
6. Connect Ethernet. The board should answer at `192.168.10.27`, dashboard port 80, Modbus TCP port 5502.

## Direct PlatformIO upload alternative

If you prefer to compile and flash from PowerShell in one command, replace `COM5` with the actual serial port:

```powershell
cd esp32; py -m platformio run -t upload --upload-port COM5
```

For serial diagnostics:

```powershell
cd esp32; py -m platformio device monitor -b 115200 -p COM5
```

## Test / rollback

Keep the existing Debian LXC untouched during validation. Stop LXC 126, boot the ESP32 and verify Home Assistant plus the web dashboard. To roll back, power off/disconnect the ESP32 and start LXC 126 again. Because both implementations use `192.168.10.27:5502`, Home Assistant needs no endpoint change.
