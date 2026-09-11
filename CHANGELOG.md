# Changelog

## 0.1.0 - Initial public release

- Created Huawei SunBridge Modbus from the MIT-licensed `cloudapp-dev/sun2000-modbus-cache` foundation.
- Added SDongleA-05 connection settling delay (tested default: 2 seconds).
- Increased inter-batch pacing to a tested default of 500 ms.
- Switched response handling to complete Modbus TCP frame reads.
- Expanded register cache for Huawei Solar inverter, meter and battery discovery/use.
- Added serialized upstream access to avoid SunBridge itself creating overlapping SDongle sessions.
- Avoided silently returning zero for unknown setup/discovery addresses.
- Added FC6 write-through with the same SDongle connection settling behavior.
- Added Debian 12 / Proxmox LXC systemd deployment example.
- Added visual LAN architecture documentation.
- Documented tested Huawei hardware and firmware revisions.
- Documented Wallbox coexistence and that Huawei EMMA is not required.
