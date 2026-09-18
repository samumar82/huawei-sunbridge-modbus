# Huawei SunBridge Modbus

A lightweight LAN-only Modbus TCP cache/proxy for Huawei SUN2000 installations using an SDongle.

> **Goal:** work around the SDongle's very limited Modbus TCP connection capacity without adding Huawei EMMA. Everything stays on your LAN.

## Two deployment options

SunBridge can now run in either of these forms while keeping the same Home Assistant endpoint and the same SDongle-safe polling behavior:

| Deployment | Status | Best for |
|---|---|---|
| **Debian 12 LXC / Proxmox** | Reference implementation, validated on the test installation | Maximum observability and easy Linux maintenance |
| **WT32-ETH01 v1.4 / ESP32** | Validated embedded implementation with Ethernet-first + Wi-Fi fallback | Dedicated low-power appliance, no VM/container required |

The Debian implementation is `sunbridge_modbus.py` plus the `systemd/` service. The ESP32 implementation is under **[`esp32/`](esp32/README.md)** and is built with PlatformIO CLI. **The ESP32 folder contains the complete walkthrough**: user configuration, network settings, Wi-Fi fallback, BOM, wiring, first flash, PlatformIO build, serial diagnostics, OTA updates, validation and rollback. Start there if you want to build the hardware version.

The ESP32 dashboard reports the active network interface, SDongle/proxy state, polling/cache state and Modbus clients. Installation-specific IP addresses, gateway, DNS, SDongle endpoint, ports, Unit ID and Wi-Fi credentials are configured locally in `esp32/include/secrets.h` using the safe public `secrets.example.h` template. They are **not fixed to the reference installation** and the real secrets file is ignored by Git.

**Do not run two SunBridge instances with the same configured IP at the same time.** This includes running the Proxmox LXC and ESP32 simultaneously if they are configured to reuse the same address.

## Architecture

```text
Home Assistant / Huawei Solar
            |
       Modbus TCP :5502
            |
      Huawei SunBridge
       /            \
Debian LXC       WT32-ETH01
(Proxmox)        (ESP32/LAN8720)
       \            /
        single serialized
        Modbus TCP :502
              |
       Huawei SDongleA-05
              |
          SUN2000-L1
       /      |       \
   Battery  Meter      PV
```

The Huawei Wallbox remains on its normal Huawei path; optional local OCPP to Home Assistant is separate from SunBridge. Huawei EMMA is not required for this tested architecture.

## Tested reference installation

| Component | Tested model / version |
|---|---|
| Inverter | **Huawei SUN2000-4.6KTL-L1** |
| Inverter firmware | **V200R001C00SPC155** |
| Inverter Modbus Unit ID | **1** |
| SDongle | **Huawei SDongleA-05** |
| SDongle firmware | **V200R025C00SPC130** |
| SDongle protocol | **P1.15-D50.0** |
| SDongle monitoring hardware revision | **B** |
| Huawei Wallbox | **Huawei SCharger 7.4 kW single-phase** |
| Upstream Modbus TCP | **port 502** |
| Home Assistant integration | **Huawei Solar 2.1.5** |
| Optional Wallbox integration | **lbbrhzn/ocpp via local OCPP WebSocket** |
| Downstream SunBridge port | **5502** |
| Validated host | **Debian 12 LXC on Proxmox** |
| Embedded target | **WT32-ETH01 v1.4 / LAN8720** |

## SDongle-safe behavior

The tested SDongleA-05 is substantially more reliable when SunBridge opens one controlled upstream connection, waits **2 seconds** before the first request, reads complete Modbus TCP frames, waits about **500 ms** between register batches, and serializes all upstream traffic. Both implementations preserve this behavior.

## Register batches

```text
30000/15  30015/50  30071/1  30075/2
32000/20  32064/52  37100/38 37200/2
37760/28  40126/2   42900/10 43006/2
47000/1   47089/1
```

These ranges were required for the tested Huawei Solar setup to discover inverter, grid/power-meter and battery data correctly. Setup reads in the `30000–30071` area that are not already cached can be passed through upstream; FC6 writes are write-through.

## Option A — Debian 12 / Proxmox LXC

```bash
git clone https://github.com/samumar82/huawei-sunbridge-modbus.git
cd huawei-sunbridge-modbus
sudo mkdir -p /opt/huawei-sunbridge-modbus
sudo cp sunbridge_modbus.py /opt/huawei-sunbridge-modbus/
sudo cp systemd/huawei-sunbridge-modbus.service /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable --now huawei-sunbridge-modbus
```

Configure the service environment for the actual SDongle address. The reference LXC used Debian 12, 1 vCPU and 512 MB RAM.

## Option B — WT32-ETH01 v1.4

See **[`esp32/README.md`](esp32/README.md)** for the **complete ESP32 walkthrough**, including configuration through `secrets.h`, Ethernet-first/Wi-Fi-fallback behavior, BOM, CH340G USB-to-TTL wiring, IO0 download-mode procedure, PowerShell/esptool flashing, PlatformIO build, merged first-flash image, serial diagnostics, OTA updates, validation and rollback.

Preferred Windows PowerShell build from the repository root:

```powershell
py -m pip install -U platformio; cd esp32; py -m platformio run
```

The build creates `esp32/dist/huawei-sunbridge-wt32-eth01-webflash.bin`, a merged image intended for flashing from address `0x0` with an ESP Web Tools-compatible browser flasher.

## Home Assistant

Point Huawei Solar to the SunBridge host rather than directly to the SDongle:

```text
Host: <SunBridge LAN IP>
Port: 5502
Slave / Unit ID: 1
```

The host, listener port and Unit ID are configurable for the ESP32 in `esp32/include/secrets.h`. In the validated reference setup the LXC and ESP32 intentionally reused the same endpoint for A/B testing, with only one instance running at a time.

## Safety and limitations

- Keep the proxy and SDongle Modbus ports on a trusted LAN; Modbus TCP has no authentication layer.
- Do not expose the proxy, SDongle, or non-TLS local OCPP listener to the public Internet.
- FC6 write requests can change inverter settings; use write support carefully.
- The Debian implementation is the long-running reference. The WT32-ETH01 implementation has been validated over both Ethernet and Wi-Fi fallback with repeated full `14/14` register-batch polls.
- Firmware updates can change Huawei timing/register behavior.
- This project is not affiliated with or endorsed by Huawei.

## Credits

Derived from/inspired by `cloudapp-dev/sun2000-modbus-cache` (MIT), with technical references from `olivergregorius/sun2000_modbus`, compatibility testing against `wlcrs/huawei_solar`, and optional Wallbox integration using `lbbrhzn/ocpp`. The original MIT copyright notice is retained in the project license.

## License

MIT. See `LICENSE`.
