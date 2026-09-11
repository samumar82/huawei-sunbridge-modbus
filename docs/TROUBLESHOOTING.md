# Troubleshooting

## Home Assistant connects but devices are missing

Check the SunBridge log first:

```bash
journalctl -u huawei-sunbridge-modbus -f
```

The tested setup reaches:

```text
Poll: 14/14 batches OK
```

Missing meter/battery discovery can be caused by incomplete register coverage or failed upstream batches.

## The first request times out

On the tested SDongleA-05, sending Modbus immediately after TCP connect was unreliable. SunBridge therefore waits **2 seconds** before the first request.

Default:

```ini
Environment=CONNECT_WAIT=2.0
```

## Some batches randomly fail

The tested SDongle was unreliable with approximately 50 ms between requests. A **500 ms** pause produced repeated complete polling cycles.

Default:

```ini
Environment=BATCH_DELAY=0.5
```

## Port 6607

Do not assume port 6607 is available. The tested SDongleA-05 firmware used **TCP 502** for Modbus and refused TCP 6607.

## Wallbox

The Wallbox does not need to connect to port 5502. In the tested installation it stays on the normal Huawei path while Home Assistant uses SunBridge.

## EMMA

Huawei EMMA is **not required** for this architecture. SunBridge is a LAN software proxy/cache; it does not emulate EMMA and does not replace Huawei safety/control hardware.

## Security

Never forward TCP 502 or 5502 from your Internet router. Keep Modbus and SunBridge on trusted LAN/VLAN networks only.
