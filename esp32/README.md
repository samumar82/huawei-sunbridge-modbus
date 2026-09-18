# ESP32 deployment — WT32-ETH01 v1.4

Huawei SunBridge can run directly on a WT32-ETH01 v1.4 (ESP32 + LAN8720) as an embedded alternative to the Debian/Proxmox deployment.

## Network behaviour

Ethernet is the preferred interface. At boot SunBridge first attempts Ethernet. If Ethernet is unavailable, it stores a fallback flag and automatically reboots into Wi-Fi mode without initializing Ethernet.

Ethernet and Wi-Fi use the same configured static IP. After a successful Wi-Fi fallback connection the flag is cleared; the device stays on Wi-Fi until its next restart, when Ethernet is tried again. This avoids running both interfaces with the same IP at the same time.

## Configuration

Installation-specific settings live in `include/secrets.h`. This file is ignored by Git and must not be committed.

Create it from the public template:

```powershell
Copy-Item .\include\secrets.example.h .\include\secrets.h
```

Then edit `include/secrets.h`. It contains Wi-Fi SSID/password, SunBridge static IP, gateway, subnet, DNS, Huawei SDongle IP/port, SunBridge listener port, and Modbus Unit ID. Values in `secrets.example.h` are examples only.

## Important IP rule

Never run two devices with the configured SunBridge IP simultaneously. If a Debian/Proxmox SunBridge instance and the ESP32 use the same address, stop one before starting the other.

## Features

- WT32-ETH01 v1.4 / ESP32 / LAN8720.
- Ethernet preferred with automatic Wi-Fi fallback at boot.
- Same configurable static IP on Ethernet and Wi-Fi.
- 14 tested Huawei register batches.
- 3 s polling interval, 2 s post-connect settling delay, 500 ms inter-batch pacing.
- Complete MBAP + PDU reads and serialized upstream access to the SDongle.
- FC3 cache reads and discovery passthrough; FC6 write-through.
- Web dashboard with active interface, network state, polling/cache state, active clients and recent disconnections.
- `/health` JSON endpoint with Ethernet/Wi-Fi state.
- OTA firmware update page at `/update`.
- NTP time using `Europe/Rome`; proxy operation does not depend on NTP.

## Validation

The firmware was tested with both Ethernet and Wi-Fi fallback against a Huawei SDongle. Both paths repeatedly completed all configured batches:

```text
Poll: 14/14 batches OK
```

One local 20-request connectivity sample produced:

| Interface | Ping success | Average ping | TCP listener | Average TCP connect |
|---|---:|---:|---:|---:|
| Ethernet | 20/20 | 9 ms | 20/20 | 5.97 ms |
| Wi-Fi | 20/20 | 72 ms | 20/20 | 15.91 ms |

The first Ethernet ping was a 138 ms startup outlier; the remaining 19 were approximately 1–4 ms. These figures describe one local test environment and are not universal performance guarantees.

## Build on Windows PowerShell

From the repository root:

```powershell
cd esp32
Copy-Item .\include\secrets.example.h .\include\secrets.h
notepad .\include\secrets.h
py -m platformio run
```

A successful build creates the OTA application image at `.pio\build\wt32-eth01\firmware.bin` and the merged first-flash image at `dist\huawei-sunbridge-wt32-eth01-webflash.bin`.

## First flash

Use a USB-to-TTL adapter with 3.3 V UART logic. Power the WT32-ETH01 through its appropriate 5 V input; do not power its 3.3 V rail from the adapter. Connect TXD to RX0/GPIO3, RXD to TX0/GPIO1, GND to GND, and hold IO0 to GND only while entering download mode.

Example:

```powershell
py "$env:USERPROFILE\.platformio\packages\tool-esptoolpy\esptool.py" --chip esp32 --port COM17 --baud 460800 write_flash 0x0 ".\dist\huawei-sunbridge-wt32-eth01-webflash.bin"
```

Replace `COM17` with your serial port. Disconnect IO0 from GND and power-cycle after flashing.

## OTA updates

Open `http://SUNBRIDGE_IP/update` and upload the normal `.pio\build\wt32-eth01\firmware.bin`. Do not use the merged web-flash image for OTA.

The web interface is intended for a trusted LAN and has no TLS. Do not expose it directly to the Internet.

## Serial diagnostics

```powershell
py -m platformio device monitor -b 115200 -p COM17
```

Typical Ethernet startup:

```text
Ethernet priority boot
Ethernet UP: <configured IP>
SunBridge <IP>:<port> -> SDongle <IP>:<port>
```

Typical fallback:

```text
Ethernet priority boot
Ethernet unavailable - next boot will use WiFi
[automatic reboot]
WiFi fallback boot - Ethernet not initialized
WiFi fallback UP: <configured IP>
SunBridge <IP>:<port> -> SDongle <IP>:<port>
```

The SDongle may need a short settling period before accepting a new Modbus polling session; during validation an initial unsuccessful poll was followed automatically by repeated `14/14` cycles.

## Rollback to Debian/Proxmox

Power off the ESP32 before starting another SunBridge instance configured with the same IP. If both implementations use the same address and listener port, Home Assistant does not require an endpoint change.
