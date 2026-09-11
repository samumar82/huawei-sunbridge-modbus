# Proxmox LXC installation

This is the reference deployment used while developing Huawei SunBridge Modbus.

## Recommended container

- Debian 12
- unprivileged LXC
- 1 vCPU
- 512 MB RAM
- 4 GB disk
- static/reserved LAN address
- Ethernet preferred

No Docker is required.

## 1. Prepare Debian

```bash
apt update
apt install -y python3 git
```

## 2. Install SunBridge

```bash
git clone https://github.com/samumar82/huawei-sunbridge-modbus.git /tmp/huawei-sunbridge-modbus
mkdir -p /opt/huawei-sunbridge-modbus
cp /tmp/huawei-sunbridge-modbus/sunbridge_modbus.py /opt/huawei-sunbridge-modbus/
cp /tmp/huawei-sunbridge-modbus/systemd/huawei-sunbridge-modbus.service /etc/systemd/system/
```

## 3. Configure your SDongle address

Edit:

```bash
nano /etc/systemd/system/huawei-sunbridge-modbus.service
```

Change only the example SDongle IP first:

```ini
Environment=SUN2000_HOST=192.168.1.40
```

Keep the tested defaults unless you have a reason to change them:

```ini
Environment=SUN2000_PORT=502
Environment=SUN2000_UNIT_ID=1
Environment=LISTEN_PORT=5502
Environment=CONNECT_WAIT=2.0
Environment=BATCH_DELAY=0.5
```

## 4. Start

```bash
systemctl daemon-reload
systemctl enable --now huawei-sunbridge-modbus
```

## 5. Check status

```bash
systemctl status huawei-sunbridge-modbus
```

Follow logs:

```bash
journalctl -u huawei-sunbridge-modbus -f
```

A healthy tested installation should repeatedly reach messages similar to:

```text
Poll: 14/14 batches OK
```

## 6. Home Assistant

Configure Huawei Solar to use the **LXC/SunBridge IP**, port **5502**, Unit ID **1**.

Do not change the Wallbox to point to SunBridge. The Wallbox remains on the Huawei-native path.
